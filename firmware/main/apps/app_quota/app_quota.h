/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_view.h"
#include <mooncake.h>
#include <cstdint>
#include <memory>
#include <string>

/**
 * @brief AI quota app — full-screen view of the shared QuotaService snapshot.
 *        Fetching itself lives in quota::QuotaService so data keeps refreshing
 *        while this app is closed or the XiaoZhi agent is running.
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
    uint32_t _seen_revision = 0;
    std::string _seen_status;

    void _renderSnapshot(const quota::Snapshot& snap);
};
