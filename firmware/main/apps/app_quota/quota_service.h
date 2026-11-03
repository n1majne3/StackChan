/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_model.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace quota {

/**
 * @brief Quota fetch service for the Quota app (mooncake mode only). Owns a
 *        FreeRTOS task that brings WiFi up and refreshes the snapshot on a
 *        fixed interval, saving a summary to NVS for the agent-face pill.
 *        Never runs in the xiaozhi agent, which has little internal heap and
 *        owns WiFi itself — quota::agent_refresh covers that mode.
 */
class QuotaService {
public:
    QuotaService(const QuotaService&)            = delete;
    QuotaService& operator=(const QuotaService&) = delete;

    static QuotaService& Get()
    {
        static QuotaService instance;
        return instance;
    }

    /// Idempotent — spawns the refresh task on first call.
    void start();

    /// Ask the task to fetch again as soon as possible.
    void requestRefresh();

    /// Monotonic snapshot version — bump on every completed fetch round.
    uint32_t revision() const
    {
        return _revision.load();
    }

    /// Copy out the latest snapshot (thread-safe).
    Snapshot snapshot();

    /// Current service status for UI display ("fetching", "waiting network", ...).
    std::string statusText();

    bool isFetching() const
    {
        return _fetching.load();
    }

private:
    QuotaService() = default;

    TaskHandle_t _task = nullptr;

    std::mutex _mtx;
    Snapshot _snapshot;
    std::string _status = "idle";

    std::atomic<uint32_t> _revision{0};
    std::atomic<bool> _fetching{false};

    void _taskMain();
    void _fetchOnce();
};

inline QuotaService& GetQuotaService()
{
    return QuotaService::Get();
}

}  // namespace quota
