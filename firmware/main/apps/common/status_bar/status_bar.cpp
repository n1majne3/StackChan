/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <mooncake.h>
#include <mooncake_log.h>
#include <cstdint>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <assets/assets.h>
#include <fmt/chrono.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <memory>
#include <vector>
#include <lvgl.h>
#include <src/draw/lv_image_dsc.h>
#include "apps/app_quota/quota_cache.h"
#include "apps/app_quota/quota_agent.h"
#include <ctime>

using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

/**
 * @brief
 *
 */
class StatuBarGesture {
public:
    std::function<void(void)> onGesture;

    StatuBarGesture() : _is_tracking(false), _last_state(LV_INDEV_STATE_REL)
    {
    }

    void init()
    {
        _is_tracking    = false;
        _last_state     = LV_INDEV_STATE_REL;
        _screen_height  = 240;
        _top_threshold  = 20;  // 距离顶部 20 像素内触发
        _swipe_min_dist = 50;  // 向下滑动至少 50 像素才触发
    }

    void update()
    {
        lv_indev_t* indev = GetHAL().lvTouchpad;
        if (!indev) {
            return;
        }

        lv_indev_state_t state = lv_indev_get_state(indev);
        lv_point_t curr_point;
        lv_indev_get_point(indev, &curr_point);

        // 1. 按下瞬间 (Transition: Released -> Pressed)
        if (state == LV_INDEV_STATE_PR && _last_state == LV_INDEV_STATE_REL) {
            // 只有在按下那一刻就在顶部，才标记为追踪开始
            if (curr_point.y <= _top_threshold && curr_point.y >= 0) {
                _start_point = curr_point;
                _is_tracking = true;
            } else {
                _is_tracking = false;  // 按下位置不对，此次滑动全程忽略
            }
        }
        // 2. 抬起瞬间 (Transition: Pressed -> Released)
        else if (state == LV_INDEV_STATE_REL && _last_state == LV_INDEV_STATE_PR) {
            if (_is_tracking) {
                int delta_y = curr_point.y - _start_point.y;  // 向下滑为正
                int delta_x = abs(curr_point.x - _start_point.x);

                // 判断标准：向下位移足够，且角度偏垂直
                if (delta_y > _swipe_min_dist && delta_y > delta_x) {
                    if (onGesture) {
                        onGesture();
                    }
                }
                _is_tracking = false;  // 重置追踪状态
            }
        }

        _last_state = state;  // 更新状态机
    }

private:
    bool _is_tracking;
    lv_indev_state_t _last_state;  // 记录上一帧的状态
    lv_point_t _start_point;
    int _screen_height;
    int _top_threshold;
    int _swipe_min_dist;
};

namespace status_bar_view {

class Widget {
public:
    virtual ~Widget()              = default;
    virtual void update()          = 0;
    virtual void onParentHide()    {}
    virtual void onParentShow()    {}
    // Called whenever the panel's bottom edge moves (0 when fully hidden).
    virtual void onParentMoved(int panelBottomY) {}
};

class TimeLabel : public Widget {
public:
    TimeLabel(lv_obj_t* parent, uint32_t colorText)
    {
        _label = std::make_unique<Label>(parent);
        _label->setText("");
        _label->setTextColor(lv_color_hex(colorText));
        _label->setTextFont(&lv_font_montserrat_16);
        _label->align(LV_ALIGN_CENTER, 0, 0);

        update();
    }

    void update() override
    {
        auto now   = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);

        struct tm local_tm;
        localtime_r(&now_t, &local_tm);

        int hour12 = local_tm.tm_hour % 12;
        if (hour12 == 0) {
            hour12 = 12;
        }

        _label->setText(fmt::format("{}:{:02d} {}", hour12, local_tm.tm_min, local_tm.tm_hour >= 12 ? "PM" : "AM"));
    }

private:
    std::unique_ptr<Label> _label;
};

class BatteryIcon {
public:
    BatteryIcon(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _color_primary = colorPrimary;

        _bat_top = std::make_unique<uitk::lvgl_cpp::Container>(parent);
        _bat_top->setBgColor(lv_color_hex(colorSecondary));
        _bat_top->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _bat_top->setBorderWidth(0);
        _bat_top->setSize(4, 4);
        _bat_top->setRadius(4);
        _bat_top->setOutlineWidth(1);
        _bat_top->setOutlineColor(lv_color_hex(colorPrimary));

        _bar = std::make_unique<uitk::lvgl_cpp::Bar>(parent);
        _bar->setSize(30, 12);
        _bar->setRadius(4);
        _bar->setRadius(0, LV_PART_INDICATOR);
        _bar->setBgColor(lv_color_hex(colorSecondary));
        _bar->setBgColor(lv_color_hex(colorPrimary), LV_PART_INDICATOR);
        _bar->setBgOpa(LV_OPA_COVER);
        _bar->setRange(0, 100);
        _bar->setValue(0);
        _bar->setPadding(1, 1, 1, 1);
        _bar->setOutlineWidth(1);
        _bar->setOutlineColor(lv_color_hex(colorPrimary));
        _bar->setRadius(3, LV_PART_INDICATOR);

        _lightning_icon     = std::make_unique<uitk::lvgl_cpp::Image>(parent);
        _icon_bat_lightning = assets::get_image("icon_bat_lightning.bin");
        _lightning_icon->setSrc(&_icon_bat_lightning);
        _lightning_icon->setImageRecolor(lv_color_hex(colorPrimary));
        _lightning_icon->setImageRecolorOpa(LV_OPA_COVER);
        _lightning_icon->setHidden(true);
    }

    void align(lv_align_t align, int32_t x_ofs, int32_t y_ofs)
    {
        _bar->align(align, x_ofs, y_ofs);
        lv_obj_align_to(_bat_top->get(), _bar->get(), LV_ALIGN_CENTER, 15, 0);
        lv_obj_align_to(_lightning_icon->get(), _bar->get(), LV_ALIGN_CENTER, 0, 0);
    }

    void setLevel(uint8_t level)
    {
        _bar->setValue(level);
    }

    void setCharging(bool charging)
    {
        if (charging) {
            _bar->setBgColor(lv_color_hex(0x19C25F), LV_PART_INDICATOR);
            _lightning_icon->setHidden(false);
        } else {
            _bar->setBgColor(lv_color_hex(_color_primary), LV_PART_INDICATOR);
            _lightning_icon->setHidden(true);
        }
    }

private:
    std::unique_ptr<Bar> _bar;
    std::unique_ptr<Container> _bat_top;
    std::unique_ptr<Image> _lightning_icon;
    lv_image_dsc_t _icon_bat_lightning;

    uint32_t _color_primary = 0;
};

class Battery : public Widget {
public:
    Battery(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _label_level = std::make_unique<Label>(parent);
        _label_level->setText("");
        _label_level->setTextColor(lv_color_hex(colorPrimary));
        _label_level->setTextFont(&lv_font_montserrat_16);
        _label_level->align(LV_ALIGN_RIGHT_MID, -41, 0);

        _battery_icon = std::make_unique<BatteryIcon>(parent, colorSecondary, colorPrimary);
        _battery_icon->align(LV_ALIGN_RIGHT_MID, -7, 0);

        update();
    }

    void update() override
    {
        auto level = GetHAL().getBatteryLevel();
        _label_level->setText(fmt::format("{}%", level));
        _battery_icon->setLevel(level);
        _battery_icon->setCharging(GetHAL().isBatteryCharging());
    }

private:
    std::unique_ptr<Label> _label_level;
    std::unique_ptr<BatteryIcon> _battery_icon;
};

class Wifi : public Widget {
public:
    Wifi(lv_obj_t* parent, uint32_t colorPrimary)
    {
        _wifi_icon = std::make_unique<Image>(parent);
        _wifi_icon->align(LV_ALIGN_LEFT_MID, 11, 0);
        _wifi_icon->setImageRecolor(lv_color_hex(colorPrimary));
        _wifi_icon->setImageRecolorOpa(LV_OPA_COVER);

        _icon_wifi_low    = assets::get_image("icon_wifi_low.bin");
        _icon_wifi_medium = assets::get_image("icon_wifi_medium.bin");
        _icon_wifi_high   = assets::get_image("icon_wifi_high.bin");
        _icon_wifi_slash  = assets::get_image("icon_wifi_slash.bin");

        update();
    }

    void update() override
    {
        auto status = GetHAL().getWifiStatus();
        switch (status) {
            case WifiStatus::None:
                _wifi_icon->setSrc(NULL);
                break;
            case WifiStatus::Low:
                _wifi_icon->setSrc(&_icon_wifi_low);
                break;
            case WifiStatus::Medium:
                _wifi_icon->setSrc(&_icon_wifi_medium);
                break;
            case WifiStatus::High:
                _wifi_icon->setSrc(&_icon_wifi_high);
                break;
            default:
                _wifi_icon->setSrc(&_icon_wifi_slash);
                break;
        }
    }

private:
    std::unique_ptr<Image> _wifi_icon;
    lv_image_dsc_t _icon_wifi_low;
    lv_image_dsc_t _icon_wifi_medium;
    lv_image_dsc_t _icon_wifi_high;
    lv_image_dsc_t _icon_wifi_slash;
};

/**
 * @brief Quota summary widget — a small persistent pill at the screen's
 *        top-right corner (xiaozhi mode only), clear of the bottom subtitle
 *        bubble and sliding down with the status bar when it's pulled down.
 *        It cycles through providers' worst remaining window every few
 *        seconds; tap it to expand a per-provider detail list anchored just
 *        below it. Data starts as the NVS summary from the last fetch and is
 *        refreshed in the background every CONFIG_QUOTA_AGENT_REFRESH_MIN
 *        minutes by quota::agent_refresh, which only fetches while the agent
 *        is idle and RAM allows. In mooncake apps the widget stays hidden and
 *        the status bar looks stock.
 */
class QuotaWidget : public Widget {
public:
    QuotaWidget(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
        : _color_secondary(colorSecondary), _color_primary(colorPrimary)
    {
        _pill = std::make_unique<Container>(lv_screen_active());
        _pill->setSize(LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        _pill->setRadius(10);
        _pill->setBgColor(lv_color_hex(colorPrimary));
        _pill->setBgOpa(LV_OPA_60);
        _pill->setBorderWidth(0);
        _pill->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _pill->setPadding(2, 2, 8, 8);
        _pill->align(LV_ALIGN_TOP_RIGHT, -4, _pill_margin);
        _pill->setHidden(true);
        _pill->onClick().connect([this]() { toggleDetail(); });

        _pill_label = std::make_unique<Label>(_pill->get());
        _pill_label->setText("");
        _pill_label->setTextColor(lv_color_hex(colorSecondary));
        _pill_label->setTextFont(&lv_font_montserrat_14);
        _pill_label->align(LV_ALIGN_CENTER, 0, 0);

        _detail = std::make_unique<Container>(lv_screen_active());
        _detail->setSize(320, 146);
        _detail->setPos(0, _pill_margin + _pill_h + 4);
        _detail->setBgColor(lv_color_hex(colorSecondary));
        _detail->setBgOpa(LV_OPA_COVER);
        _detail->setBorderWidth(0);
        _detail->setRadius(0);
        _detail->setPadding(6, 10, 6, 6);
        lv_obj_set_flex_flow(_detail->get(), LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(_detail->get(), LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        _detail->setHidden(true);
        _detail->onClick().connect([this]() { _hideDetail(); });

        update();
    }

    void toggleDetail()
    {
        if (lv_obj_has_flag(_detail->get(), LV_OBJ_FLAG_HIDDEN)) {
            _showDetail();
        } else {
            _hideDetail();
        }
    }

    void onParentHide() override
    {
        _hideDetail();
    }

    // Ride just below the status bar while it slides in/out, and sit at the
    // top-right corner when it's hidden.
    void onParentMoved(int panelBottomY) override
    {
        const int y = (panelBottomY > 0 ? panelBottomY : 0) + _pill_margin;
        _pill->align(LV_ALIGN_TOP_RIGHT, -4, y);
        _detail->setPos(0, y + _pill_h + 4);
    }

    void update() override
    {
        // Persistent pill only makes sense on the xiaozhi face — inside
        // mooncake apps the widget would float over unrelated content.
        bool in_agent = hal_bridge::is_xiaozhi_mode();
        if (in_agent != _pill_visible) {
            _pill_visible = in_agent;
            _pill->setHidden(!in_agent);
            if (!in_agent) _hideDetail();
        }
        if (!in_agent) return;

        // One NVS read per agent session; after that, background refreshes
        // hand their summary over in memory.
        if (!_loaded) {
            _cache  = quota::loadCache();
            _loaded = true;
        }
        quota::agent_refresh::poll();
        if (quota::agent_refresh::takeUpdate(_seen_revision, _cache)) {
            _last_minute = -1;  // rebuild pill texts below
            if (!lv_obj_has_flag(_detail->get(), LV_OBJ_FLAG_HIDDEN)) _rebuildDetail();
        }

        // Texts (with age suffix) are rebuilt at most once a minute; between
        // rebuilds the pill only steps through them — no heap churn on the
        // 1Hz widget tick beyond LVGL's copy when the shown text changes.
        const time_t now     = time(nullptr);
        const int64_t minute = now / 60;
        if (minute != _last_minute) {
            _last_minute = minute;
            _rebuildPillTexts();
        }

        // Rotate through providers so every one gets screen time.
        const size_t idx = _pill_texts.size() > 1 ? (size_t)(now / _rotate_sec) % _pill_texts.size() : 0;
        const std::string& text = _pill_texts[idx];
        if (text != _pill_text) {
            _pill_text = text;
            _pill_label->setText(text);
        }
    }

private:
    static constexpr int _rotate_sec  = 5;
    static constexpr int _pill_margin = 4;   // gap above the pill (screen top or status bar)
    static constexpr int _pill_h      = 20;  // montserrat_14 line + 2px top/bottom padding

    void _rebuildPillTexts()
    {
        _pill_texts.clear();
        const std::string age = _ageText();
        for (const auto& c : _cache.cards) {
            std::string t = _shortName(c.name, 8);
            if (c.error) {
                t += " err";
            } else if (c.percent >= 0) {
                t += fmt::format(" {}%", c.percent);
            } else {
                t += " --";
            }
            if (!age.empty()) t += " " + age;
            _pill_texts.push_back(std::move(t));
        }
        if (_pill_texts.empty()) _pill_texts.push_back("quota --");
    }

    // "5m" / "3h" / "2d"; empty if the clock or fetch time is unknown.
    std::string _ageText() const
    {
        const time_t now = time(nullptr);
        if (_cache.fetchedAt <= 0 || now < 1700000000 || now < _cache.fetchedAt) return "";
        const int64_t mins = (now - _cache.fetchedAt) / 60;
        if (mins < 60) return fmt::format("{}m", mins);
        if (mins < 48 * 60) return fmt::format("{}h", mins / 60);
        return fmt::format("{}d", mins / (24 * 60));
    }

    void _showDetail()
    {
        _rebuildDetail();
        _detail->moveForeground();
        _detail->setHidden(false);
    }

    void _hideDetail()
    {
        if (!lv_obj_has_flag(_detail->get(), LV_OBJ_FLAG_HIDDEN)) {
            _detail->setHidden(true);
        }
    }

    // Provider names can be long — keep chunks compact so the centered clock
    // isn't touched (bar is only 320px wide); detail rows can afford more.
    static std::string _shortName(const std::string& name, size_t maxLen)
    {
        return name.size() <= maxLen ? name : name.substr(0, maxLen);
    }

    void _rebuildDetail()
    {
        _clearRows();

        if (_cache.empty()) {
            _rowText(_makeRow(), "No data - open Quota app", "");
            return;
        }

        for (const auto& c : _cache.cards) {
            auto* row = _makeRow();
            if (c.error) {
                _rowText(row, _shortName(c.name, 18), "err");
            } else if (c.percent >= 0) {
                _rowText(row, _shortName(c.name, 18), fmt::format("{} {}%", c.window, c.percent));
            } else {
                _rowText(row, _shortName(c.name, 18), "--");
            }
        }
        std::string age = _ageText();
        if (!age.empty()) {
            _rowText(_makeRow(), "updated " + age + " ago", "");
        }
    }

    lv_obj_t* _makeRow()
    {
        auto row = std::make_unique<Container>(_detail->get());
        row->setSize(300, LV_SIZE_CONTENT);
        row->setBgOpa(LV_OPA_TRANSP);
        row->setBorderWidth(0);
        row->setRadius(0);
        row->setPadding(0, 0, 0, 0);
        row->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(row->get(), LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row->get(), LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        _rows.push_back(std::move(row));
        return _rows.back()->get();
    }

    void _rowText(lv_obj_t* row, const std::string& left, const std::string& right)
    {
        auto l = std::make_unique<Label>(row);
        l->setText(left);
        l->setTextColor(lv_color_hex(_color_primary));
        l->setTextFont(&lv_font_montserrat_14);
        lv_obj_set_flex_grow(l->get(), 1);

        auto r = std::make_unique<Label>(row);
        r->setText(right);
        r->setTextColor(lv_color_hex(_color_primary));
        r->setTextFont(&lv_font_montserrat_14);
        r->align(LV_ALIGN_RIGHT_MID, 0, 0);

        _row_labels.push_back(std::move(l));
        _row_labels.push_back(std::move(r));
    }

    void _clearRows()
    {
        // Labels must die before their row parents — LVGL frees a container's
        // children with it, so clearing rows first would leave dangling labels.
        _row_labels.clear();
        _rows.clear();
    }

    std::unique_ptr<Container> _pill;
    std::unique_ptr<Label> _pill_label;
    std::unique_ptr<Container> _detail;
    std::vector<std::unique_ptr<Container>> _rows;
    std::vector<std::unique_ptr<Label>> _row_labels;
    bool _pill_visible   = false;
    bool _loaded            = false;
    uint32_t _seen_revision = 0;
    int64_t _last_minute    = -1;
    quota::CachedSummary _cache;
    std::string _pill_text;
    std::vector<std::string> _pill_texts;
    uint32_t _color_secondary;
    uint32_t _color_primary;
};

class StatusBarView {
public:
    StatusBarView(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
        _panel->setBgColor(lv_color_hex(colorSecondary));
        _panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _panel->align(LV_ALIGN_TOP_MID, 0, 0);
        _panel->setBorderWidth(0);
        _panel->setSize(320, 28);
        _panel->setRadius(0);
        _panel->setPadding(0, 0, 0, 0);
        _panel->onClick().connect([this]() { hide(); });

        _widgets.push_back(std::make_unique<TimeLabel>(_panel->get(), colorPrimary));
        _widgets.push_back(std::make_unique<Battery>(_panel->get(), colorSecondary, colorPrimary));
        _widgets.push_back(std::make_unique<Wifi>(_panel->get(), colorPrimary));
        _widgets.push_back(std::make_unique<QuotaWidget>(_panel->get(), colorSecondary, colorPrimary));

        _panel->setPos(0, _pos_y_hide);
        _pos_y_anim.springOptions().bounce         = 0.1;
        _pos_y_anim.springOptions().visualDuration = 0.3;
        // _pos_y_anim.easingOptions().duration = 0.3;
        _pos_y_anim.teleport(_pos_y_hide);
    }

    void update()
    {
        _pos_y_anim.update();
        if (!_pos_y_anim.done()) {
            _panel->setPos(0, _pos_y_anim.directValue());
        }
        const int panel_bottom = lv_obj_get_y(_panel->get()) + 28;
        if (panel_bottom != _last_panel_bottom) {
            _last_panel_bottom = panel_bottom;
            for (auto& widget : _widgets) {
                widget->onParentMoved(panel_bottom);
            }
        }
        if (_pos_y_anim.done()) {
            if (_hide_panel_flag) {
                _hide_panel_flag = false;
                _panel->setHidden(true);
            }
        }

        if (GetHAL().millis() - _last_update_tick > 1000) {
            _last_update_tick = GetHAL().millis();
            for (auto& widget : _widgets) {
                widget->update();
            }
        }
    }

    void show()
    {
        _panel->moveForeground();
        _panel->setHidden(false);
        _pos_y_anim = _pos_y_show;
        _is_hidden  = false;
    }

    void hide()
    {
        for (auto& widget : _widgets) {
            widget->onParentHide();
        }
        _pos_y_anim      = _pos_y_hide;
        _is_hidden       = true;
        _hide_panel_flag = true;
    }

    bool isHidden() const
    {
        return _is_hidden;
    }

private:
    const int _pos_y_show = 0;
    const int _pos_y_hide = -28;

    std::unique_ptr<Container> _panel;
    std::vector<std::unique_ptr<Widget>> _widgets;
    AnimateValue _pos_y_anim;
    bool _is_hidden            = false;
    bool _hide_panel_flag      = false;
    uint32_t _last_update_tick = 0;
    int _last_panel_bottom     = -1;
};

}  // namespace status_bar_view

/**
 * @brief
 *
 */
class StatusBar {
public:
    void init(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _status_bar_gesture            = std::make_unique<StatuBarGesture>();
        _status_bar_gesture->onGesture = [&]() { handle_gesture(); };
        _status_bar_gesture->init();

        _status_bar_view = std::make_unique<status_bar_view::StatusBarView>(parent, colorSecondary, colorPrimary);
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
        _is_first_show        = true;
    }

    void update()
    {
        _status_bar_gesture->update();
        _status_bar_view->update();
        update_visibility();
    }

private:
    std::unique_ptr<StatuBarGesture> _status_bar_gesture;
    std::unique_ptr<status_bar_view::StatusBarView> _status_bar_view;
    uint32_t _status_bar_show_tick = 0;
    bool _is_first_show            = true;

    void handle_gesture()
    {
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
    }

    void update_visibility()
    {
        if (!_status_bar_view->isHidden()) {
            if (GetHAL().millis() - _status_bar_show_tick > (_is_first_show ? 1800 : 6000)) {
                _is_first_show = false;
                _status_bar_view->hide();
            }
        }
    }
};

/**
 * @brief
 *
 */
namespace view {

static std::unique_ptr<StatusBar> _status_bar;

void create_status_bar(uint32_t colorSecondary, uint32_t colorPrimary, lv_obj_t* parent)
{
    _status_bar = std::make_unique<StatusBar>();
    _status_bar->init(parent, colorSecondary, colorPrimary);
}

void update_status_bar()
{
    if (_status_bar) {
        _status_bar->update();
    }
}

bool is_status_bar_created()
{
    return _status_bar != nullptr;
}

void destroy_status_bar()
{
    _status_bar.reset();
}

}  // namespace view
