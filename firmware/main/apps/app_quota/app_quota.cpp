/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_quota.h"
#include "quota_fetch.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <apps/common/common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>
#include <ctime>
#include <smooth_lvgl.hpp>

using namespace mooncake;

#ifndef CONFIG_QUOTA_REFRESH_SEC
#define CONFIG_QUOTA_REFRESH_SEC 60
#endif

static const std::string_view _tag = "AppQuota";
// ESP-IDF xTaskCreate stack depth is in BYTES (unlike vanilla FreeRTOS words)
static constexpr uint32_t FETCH_STACK = 16 * 1024;

AppQuota::AppQuota()
{
    setAppInfo().name = "Quota";
    static auto icon  = assets::get_image("icon_sentinel.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x34D399;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppQuota::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppQuota::_startFetch()
{
    if (_fetch_running) return;
    _fetch_running = true;
    _fetch_done    = false;
    _stop_fetch    = false;

    auto self = this;
    auto code = [](void* arg) {
        auto* self = static_cast<AppQuota*>(arg);
        // lwip/tcpip thread only exists after network init — must bring the
        // network up (or at least confirm it's up) before any DNS/socket use.
        if (!WifiManager::GetInstance().IsConnected()) {
            GetHAL().startNetwork([self](std::string_view msg) {
                LvglLockGuard lock;
                if (self->_view) self->_view->setStatusText(std::string(msg));
            });
        }
        quota::Snapshot snap;
        if (!self->_stop_fetch) {
            // Each completed source pushes a partial snapshot so the UI
            // renders progressively instead of waiting for the slowest one.
            auto publish = [self](const quota::Snapshot& s) {
                std::lock_guard<std::mutex> g(self->_pending_mtx);
                self->_pending      = s;
                self->_fetch_done   = true;
            };
            snap = quota::fetchAll(publish);
            snap.fetchedAtMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
            {
                std::lock_guard<std::mutex> g(self->_pending_mtx);
                self->_pending = snap;
            }
        } else {
            std::lock_guard<std::mutex> g(self->_pending_mtx);
            self->_pending = std::move(snap);
        }
        self->_fetch_done    = true;
        self->_fetch_running = false;
        vTaskDelete(nullptr);
    };
    if (xTaskCreatePinnedToCore(code, "quota_fetch", FETCH_STACK, self, 5,
                                &_fetch_task, 0) != pdPASS) {
        mclog::tagError(_tag, "fetch task create failed");
        _fetch_running = false;
    }
}

void AppQuota::_joinFetch()
{
    _stop_fetch = true;
    // wait for the worker to finish (bounded; fetch has http timeouts)
    for (int i = 0; i < 50 && _fetch_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void AppQuota::_applyPending()
{
    quota::Snapshot snap;
    {
        std::lock_guard<std::mutex> g(_pending_mtx);
        snap = std::move(_pending);
    }
    if (!snap.error.empty() && snap.cards.empty()) {
        _view->showError(snap.error);
    } else {
        _view->showSnapshot(snap);
    }
    // refresh status line: show local time of last update
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[24];
    snprintf(buf, sizeof(buf), "updated %02d:%02d", tmv.tm_hour, tmv.tm_min);
    _view->setStatusText(buf);
}

void AppQuota::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    {
        LvglLockGuard lock;
        _view = std::make_unique<quota::QuotaView>();
        _view->showLoading("loading quotas");
        view::create_home_indicator([&]() { close(); }, 0x6EE7B7, 0x06281D);
        view::create_status_bar(0x6EE7B7, 0x06281D);
    }

    _next_refresh_ms = 0;  // fetch immediately
}

void AppQuota::onRunning()
{
    if (_fetch_done.exchange(false)) {
        LvglLockGuard lock;
        _applyPending();
    }

    if (GetHAL().millis() >= _next_refresh_ms) {
        _next_refresh_ms = GetHAL().millis() + CONFIG_QUOTA_REFRESH_SEC * 1000;
        if (!_fetch_running) {
            _startFetch();
        }
    }

    {
        LvglLockGuard lock;
        view::update_home_indicator();
        view::update_status_bar();
    }
}

void AppQuota::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _joinFetch();

    LvglLockGuard lock;
    _view.reset();
    view::destroy_home_indicator();
    view::destroy_status_bar();

    GetHAL().requestWarmReboot(4);
}
