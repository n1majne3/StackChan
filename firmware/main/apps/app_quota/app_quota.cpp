/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_quota.h"
#include "quota_service.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <apps/common/common.h>
#include <ctime>
#include <smooth_lvgl.hpp>

using namespace mooncake;

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

void AppQuota::_renderSnapshot(const quota::Snapshot& snap)
{
    if (!snap.error.empty() && snap.cards.empty()) {
        _view->showError(snap.error);
    } else {
        _view->showSnapshot(snap);
    }

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

    auto& svc = quota::GetQuotaService();
    svc.start();
    svc.requestRefresh();

    LvglLockGuard lock;
    _view = std::make_unique<quota::QuotaView>();
    view::create_home_indicator([&]() { close(); }, 0x6EE7B7, 0x06281D);
    view::create_status_bar(0x6EE7B7, 0x06281D);

    auto snap = svc.snapshot();
    if (!snap.cards.empty() || !snap.error.empty()) {
        _seen_revision = svc.revision();
        _renderSnapshot(snap);
    } else {
        _view->showLoading("loading quotas");
    }
}

void AppQuota::onRunning()
{
    auto& svc = quota::GetQuotaService();

    const uint32_t rev = svc.revision();
    if (rev != _seen_revision) {
        _seen_revision = rev;
        LvglLockGuard lock;
        _renderSnapshot(svc.snapshot());
    }

    auto status = svc.statusText();
    if (status != _seen_status) {
        _seen_status = status;
        LvglLockGuard lock;
        // While nothing has ever been fetched, surface service state in the
        // loading area; after that it goes to the transient status pill.
        auto snap = svc.snapshot();
        if (snap.cards.empty() && snap.error.empty()) {
            _view->showLoading(status == "ok" ? "loading quotas" : status);
        } else if (status != "ok") {
            _view->setStatusText(status);
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

    {
        LvglLockGuard lock;
        _view.reset();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(4);
}
