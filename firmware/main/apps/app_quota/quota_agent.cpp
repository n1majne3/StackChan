/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "quota_agent.h"
#include "quota_fetch.h"
#include <application.h>
#include <wifi_manager.h>
#include <mooncake_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>

#ifndef CONFIG_QUOTA_AGENT_REFRESH_MIN
#define CONFIG_QUOTA_AGENT_REFRESH_MIN 30
#endif
#ifndef CONFIG_QUOTA_AGENT_MIN_FREE_INTERNAL
#define CONFIG_QUOTA_AGENT_MIN_FREE_INTERNAL 26624
#endif

namespace quota::agent_refresh {
namespace {

constexpr std::string_view _tag = "QuotaAgent";

constexpr uint32_t kStartupDelayMs = 90 * 1000;      // let boot-time OTA/activation finish first
constexpr uint32_t kRecheckMs      = 60 * 1000;      // conditions not met: look again later
constexpr uint32_t kMaxWaitMs      = 5 * 60 * 1000;  // re-evaluate staleness at least this often
constexpr uint32_t kRetryMs        = 10 * 60 * 1000; // after a failed fetch
constexpr uint32_t kRelogMs        = 15 * 60 * 1000; // repeat an unchanged "waiting" log this often
constexpr uint32_t kStackBytes     = 12 * 1024;      // in PSRAM; the TLS handshake runs on it
constexpr size_t kMinLargestBlock  = 4096;
constexpr int64_t kClockValidEpoch = 1700000000;

constexpr uint32_t kIntervalMs = (uint32_t)CONFIG_QUOTA_AGENT_REFRESH_MIN * 60u * 1000u;

// poll() runs on one thread (the agent UI task); the fetch task touches only
// the atomics and the mutex-guarded block.
bool s_started            = false;
const char* s_last_why    = nullptr;  // why the due refresh is waiting, nullptr = not waiting
uint32_t s_last_why_log_ms = 0;
std::atomic<bool> s_busy{false};
std::atomic<uint32_t> s_next_check_ms{0};

std::mutex s_mtx;
int64_t s_last_epoch  = 0;  // fetchedAt of the newest summary, 0 = unknown
bool s_have_ok        = false;
uint32_t s_last_ok_ms = 0;  // monotonic time of the last success this boot
CachedSummary s_latest;
uint32_t s_revision = 0;

uint32_t nowMs()
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

bool reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

bool agentIdle()
{
    return Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
}

// Milliseconds until the summary is due for a refresh; 0 when due now.
uint32_t msUntilDue(uint32_t now_ms)
{
    std::lock_guard<std::mutex> g(s_mtx);
    const time_t now = time(nullptr);
    if (now >= kClockValidEpoch && s_last_epoch > 0 && now >= s_last_epoch) {
        // Wall clock survives reboots, so a fresh NVS summary isn't refetched.
        const int64_t age_ms = (int64_t)(now - s_last_epoch) * 1000;
        return age_ms >= kIntervalMs ? 0 : (uint32_t)(kIntervalMs - age_ms);
    }
    if (s_have_ok) {
        const uint32_t age_ms = now_ms - s_last_ok_ms;
        return age_ms >= kIntervalMs ? 0 : kIntervalMs - age_ms;
    }
    return 0;
}

// Why a fetch can't start right now, or nullptr when it can.
const char* blocker()
{
    if (!WifiManager::GetInstance().IsConnected()) return "wifi down";
    if (!agentIdle()) return "agent busy";
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < CONFIG_QUOTA_AGENT_MIN_FREE_INTERNAL) {
        return "low internal heap";
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < kMinLargestBlock) return "fragmented internal heap";
    return nullptr;
}

void runFetch()
{
    const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // Voice wins: stop reading as soon as a conversation starts.
    bool aborted = false;
    Snapshot snap = fetchAll(nullptr, [&]() {
        if (!aborted && !agentIdle()) aborted = true;
        return aborted;
    });
    const uint32_t done_ms = nowMs();

    if (aborted || snap.cards.empty()) {
        if (aborted) {
            mclog::tagInfo(_tag, "refresh aborted: agent left idle");
        } else {
            mclog::tagWarn(_tag, "refresh failed: {}", snap.error.empty() ? "no cards" : snap.error);
        }
        s_next_check_ms = done_ms + (aborted ? kRecheckMs : kRetryMs);
        return;
    }

    const time_t now       = time(nullptr);
    CachedSummary summary  = summarize(snap, now >= kClockValidEpoch ? now : 0);
    const size_t num_cards = summary.cards.size();
    snap                   = Snapshot();

    {
        std::lock_guard<std::mutex> g(s_mtx);
        s_last_epoch  = summary.fetchedAt;
        s_have_ok     = true;
        s_last_ok_ms  = done_ms;
        s_latest      = summary;
        s_revision++;
    }
    // NVS writes disable the flash cache, which asserts on a PSRAM stack —
    // hand the save to the xiaozhi main task (internal stack).
    Application::GetInstance().Schedule([summary = std::move(summary)]() { saveCache(summary); });

    s_next_check_ms = done_ms + kIntervalMs;
    mclog::tagInfo(_tag, "refreshed {} cards, internal heap free {} before, {} after, lifetime min {}", num_cards,
                   heap_before, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void taskMain(void*)
{
    runFetch();  // all locals are gone before the task deletes itself
    s_busy = false;
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

void poll()
{
    if (CONFIG_QUOTA_AGENT_REFRESH_MIN <= 0 || s_busy) return;

    const uint32_t now = nowMs();
    if (!s_started) {
        s_started = true;
        {
            std::lock_guard<std::mutex> g(s_mtx);
            s_last_epoch = loadCache().fetchedAt;
        }
        s_next_check_ms = now + kStartupDelayMs;
        return;
    }
    if (!reached(now, s_next_check_ms)) return;

    if (const uint32_t wait = msUntilDue(now)) {
        s_next_check_ms = now + std::min(wait, kMaxWaitMs);
        return;
    }

    if (const char* why = blocker()) {
        // Re-log periodically: a refresh can stay blocked for hours, and a
        // single line at the start of that is easy to miss.
        if (why != s_last_why || reached(now, s_last_why_log_ms + kRelogMs)) {
            mclog::tagInfo(_tag, "refresh due, waiting: {} (internal heap free {}, largest block {}, need {})", why,
                           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                           CONFIG_QUOTA_AGENT_MIN_FREE_INTERNAL);
            s_last_why        = why;
            s_last_why_log_ms = now;
        }
        s_next_check_ms = now + kRecheckMs;
        return;
    }
    s_last_why = nullptr;

    s_busy = true;
    // Stack and TCB in PSRAM: the task costs no internal RAM, and only exists
    // for the length of one fetch. Lowest useful priority.
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "quota_agent", kStackBytes, nullptr, 2, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        mclog::tagError(_tag, "fetch task create failed");
        s_busy          = false;
        s_next_check_ms = now + kRetryMs;
    }
}

const char* waitingReason()
{
    return s_last_why;
}

bool takeUpdate(uint32_t& seen_revision, CachedSummary& out)
{
    std::lock_guard<std::mutex> g(s_mtx);
    if (s_revision == seen_revision) return false;
    seen_revision = s_revision;
    // Move, don't copy: the card vector is small enough to sit in internal RAM.
    out      = std::move(s_latest);
    s_latest = CachedSummary();
    return true;
}

}  // namespace quota::agent_refresh
