/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_view.h"
#include <mooncake.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/**
 * @brief AI quota app — shows subscription quota remaining fetched
 *        from configured HTTP endpoints (magpie / cpamgr).
 */
class AppQuota : public mooncake::AppAbility {
public:
    AppQuota();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<quota::QuotaView> _view;

    // fetch worker
    std::atomic<bool> _stop_fetch{false};
    std::atomic<bool> _fetch_done{false};
    TaskHandle_t _fetch_task = nullptr;
    quota::Snapshot _pending;
    std::mutex _pending_mtx;
    bool _fetch_running = false;
    uint32_t _next_refresh_ms = 0;

    void _startFetch();
    void _joinFetch();
    void _applyPending();
};
