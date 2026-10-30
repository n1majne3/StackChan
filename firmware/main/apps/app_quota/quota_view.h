/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_model.h"
#include <lvgl.h>
#include <string>

namespace quota {

/**
 * @brief Dark quota card list rendered on the active LVGL screen.
 *        Call all methods while holding the lvgl lock.
 */
class QuotaView {
public:
    QuotaView();
    ~QuotaView();

    void showLoading(const std::string& msg);
    void showError(const std::string& msg);
    void showSnapshot(const Snapshot& snap);
    void setStatusText(const std::string& msg);

    /** Full-screen detail overlay for one card; tap anywhere to dismiss. */
    void showCardDetail(const Card& card);
    void closeDetail();

private:
    lv_obj_t* _root = nullptr;
    lv_obj_t* _list = nullptr;
    lv_obj_t* _status = nullptr;
    lv_obj_t* _detail = nullptr;
    lv_timer_t* _hide_timer = nullptr;
    Snapshot _snap;

    static void _onHideTimer(lv_timer_t* t);
    static void _onCardClicked(lv_event_t* e);
    static void _onDetailClicked(lv_event_t* e);
    void _clear();
    lv_obj_t* _makeCard(const Card& card, size_t idx);
    lv_obj_t* _makeWindowRow(lv_obj_t* parent, const Window& win,
                             const std::string& right, int bar_w = 48);
};

}  // namespace quota
