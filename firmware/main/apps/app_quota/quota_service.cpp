/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "quota_service.h"
#include "quota_fetch.h"
#include "quota_cache.h"
#include <ctime>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <wifi_manager.h>
#include <freertos/task.h>

#ifndef CONFIG_QUOTA_REFRESH_SEC
#define CONFIG_QUOTA_REFRESH_SEC 60
#endif

static const std::string_view _tag = "QuotaService";
// ESP-IDF xTaskCreate stack depth is in BYTES (unlike vanilla FreeRTOS words)
static constexpr uint32_t SVC_STACK = 16 * 1024;

namespace quota {

void QuotaService::start()
{
    if (_task) return;

    auto code = [](void* arg) {
        static_cast<QuotaService*>(arg)->_taskMain();
        vTaskDelete(nullptr);
    };
    // Priority kept low — fetching must never starve audio/xiaozhi work.
    if (xTaskCreatePinnedToCore(code, "quota_svc", SVC_STACK, this, 3, &_task, 0) != pdPASS) {
        mclog::tagError(_tag, "service task create failed");
        _task = nullptr;
    }
}

void QuotaService::requestRefresh()
{
    if (_task) {
        xTaskNotifyGive(_task);
    }
}

Snapshot QuotaService::snapshot()
{
    std::lock_guard<std::mutex> g(_mtx);
    return _snapshot;
}

std::string QuotaService::statusText()
{
    std::lock_guard<std::mutex> g(_mtx);
    return _status;
}

void QuotaService::_fetchOnce()
{
    _fetching = true;
    {
        std::lock_guard<std::mutex> g(_mtx);
        _status = "fetching";
    }

    bool have_previous;
    {
        std::lock_guard<std::mutex> g(_mtx);
        have_previous = !_snapshot.cards.empty();
    }

    // Progressive publish only on the first load (empty screen benefits).
    // On refresh, partial snapshots would make cards from not-yet-refetched
    // sources vanish until their source completes — keep the old ones up.
    auto publish = [this](const Snapshot& s) {
        {
            std::lock_guard<std::mutex> g(_mtx);
            _snapshot = s;
        }
        _revision.fetch_add(1);
    };

    Snapshot snap = fetchAll(have_previous ? nullptr : std::function<void(const Snapshot&)>(publish), nullptr);
    snap.fetchedAtMs = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // A refresh that failed outright shouldn't wipe good data off the screen.
    if (snap.cards.empty() && have_previous) {
        std::lock_guard<std::mutex> g(_mtx);
        _status   = snap.error.empty() ? "refresh failed" : snap.error;
        _fetching = false;
        return;
    }

    // Persist a summary for the agent-face pill (which never fetches itself).
    if (!snap.cards.empty()) {
        const time_t now = time(nullptr);
        saveCache(summarize(snap, now > 1700000000 ? now : 0));
    }

    {
        std::lock_guard<std::mutex> g(_mtx);
        _snapshot = std::move(snap);
        _status   = "ok";
    }
    _revision.fetch_add(1);
    _fetching = false;
}

void QuotaService::_taskMain()
{
    mclog::tagInfo(_tag, "quota service started");

    uint32_t last_fetch_ms = 0;
    for (;;) {
        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (!WifiManager::GetInstance().IsConnected()) {
            // Mooncake leaves WiFi down until an app asks — the Quota app
            // started us, and its close warm-reboots, so this never leaks
            // into the xiaozhi agent (which must own WiFi itself).
            {
                std::lock_guard<std::mutex> g(_mtx);
                _status = "connecting";
            }
            GetHAL().startNetwork([this](std::string_view msg) {
                std::lock_guard<std::mutex> g(_mtx);
                _status = std::string(msg);
            });
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
            continue;
        }

        // Debounce manual refresh requests that arrive during a fetch.
        if (!_fetching && (last_fetch_ms == 0 || now - last_fetch_ms > 3000)) {
            _fetchOnce();
            last_fetch_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_QUOTA_REFRESH_SEC * 1000));
    }
}

}  // namespace quota
