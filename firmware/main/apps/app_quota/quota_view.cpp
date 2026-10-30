/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "quota_view.h"
#include <cstdio>

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_16_4);
LV_FONT_DECLARE(font_puhui_basic_14_1);
#define QUOTA_FONT    (&font_puhui_basic_14_1)
#define QUOTA_FONT_M  (&lv_font_montserrat_14)

namespace {
const lv_color_t COL_BG      = lv_color_hex(0x0A0A0A);
const lv_color_t COL_CARD    = lv_color_hex(0x1B1F2A);
const lv_color_t COL_TEXT    = lv_color_hex(0xE5E5E5);
const lv_color_t COL_MUTED   = lv_color_hex(0x8B95A6);
const lv_color_t COL_TRACK   = lv_color_hex(0x2A2F3D);
const lv_color_t COL_OK      = lv_color_hex(0x34D399);
const lv_color_t COL_WARN    = lv_color_hex(0xFBBF24);
const lv_color_t COL_DANGER  = lv_color_hex(0xEF4444);
}  // namespace

quota::QuotaView::QuotaView()
{
    _root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_root, 320, 240);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    // Full-bleed list — the status line floats over it and auto-hides
    _list = lv_obj_create(_root);
    lv_obj_set_pos(_list, 0, 0);
    lv_obj_set_size(_list, 320, 240);
    lv_obj_set_style_bg_color(_list, COL_BG, 0);
    lv_obj_set_style_bg_opa(_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 2, 0);
    // keep right-aligned text clear of the scrollbar
    lv_obj_set_style_pad_right(_list, 8, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(_list, 2, 0);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);

    // Status line: floats at bottom, shown briefly then auto-hidden
    _status = lv_label_create(_root);
    lv_label_set_text(_status, "");
    lv_obj_set_style_text_font(_status, QUOTA_FONT_M, 0);
    lv_obj_set_style_text_color(_status, COL_MUTED, 0);
    lv_obj_set_style_bg_color(_status, COL_BG, 0);
    lv_obj_set_style_bg_opa(_status, LV_OPA_80, 0);
    lv_obj_set_style_pad_ver(_status, 1, 0);
    lv_obj_set_style_pad_hor(_status, 6, 0);
    lv_obj_set_style_radius(_status, 3, 0);
    lv_obj_set_pos(_status, 8, 240 - 15);
    lv_obj_set_width(_status, 300);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_add_flag(_status, LV_OBJ_FLAG_HIDDEN);

    _hide_timer = lv_timer_create(_onHideTimer, 3000, this);
    lv_timer_pause(_hide_timer);
}

quota::QuotaView::~QuotaView()
{
    if (_hide_timer) {
        lv_timer_delete(_hide_timer);
        _hide_timer = nullptr;
    }
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}

void quota::QuotaView::_onHideTimer(lv_timer_t* t)
{
    auto* self = static_cast<QuotaView*>(lv_timer_get_user_data(t));
    lv_timer_pause(t);
    if (self->_status) lv_obj_add_flag(self->_status, LV_OBJ_FLAG_HIDDEN);
}

void quota::QuotaView::_clear()
{
    lv_obj_clean(_list);
}

void quota::QuotaView::setStatusText(const std::string& msg)
{
    if (!_status) return;
    if (msg.empty()) {
        lv_obj_add_flag(_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(_status, msg.c_str());
    lv_obj_remove_flag(_status, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(_hide_timer);
    lv_timer_resume(_hide_timer);
}

void quota::QuotaView::showLoading(const std::string& msg)
{
    _clear();
    lv_obj_t* label = lv_label_create(_list);
    lv_label_set_text(label, msg.c_str());
    lv_obj_set_style_text_color(label, COL_MUTED, 0);
    lv_obj_set_style_text_font(label, QUOTA_FONT, 0);
}

void quota::QuotaView::showError(const std::string& msg)
{
    _clear();
    lv_obj_t* label = lv_label_create(_list);
    lv_label_set_text(label, msg.c_str());
    lv_obj_set_style_text_color(label, COL_DANGER, 0);
    lv_obj_set_style_text_font(label, QUOTA_FONT, 0);
    lv_obj_set_width(label, 290);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

lv_obj_t* quota::QuotaView::_makeWindowRow(lv_obj_t* parent, const Window& win,
                                          const std::string& right)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(row, 5, 0);

    // window name ("5 小时" / "7d" / "credits") - natural width, muted
    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, win.name.c_str());
    lv_obj_set_style_text_font(name, QUOTA_FONT, 0);
    lv_obj_set_style_text_color(name, COL_MUTED, 0);

    if (win.unlimited) {
        lv_obj_t* t = lv_label_create(row);
        lv_label_set_text(t, win.text.empty() ? "unlimited" : win.text.c_str());
        lv_obj_set_style_text_font(t, QUOTA_FONT, 0);
        lv_obj_set_style_text_color(t, COL_OK, 0);
        lv_obj_set_flex_grow(t, 1);
        return row;
    }

    if (win.percent >= 0) {
        // progress track
        lv_obj_t* bar = lv_bar_create(row);
        lv_obj_set_size(bar, 48, 4);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, (int32_t)win.percent, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, COL_TRACK, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_color_t c = win.percent > 50 ? COL_OK : (win.percent > 20 ? COL_WARN : COL_DANGER);
        lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    }

    // remaining text ("79%" / "62.5k") sized to content
    lv_obj_t* text = lv_label_create(row);
    lv_label_set_text(text, win.text.c_str());
    lv_obj_set_style_text_font(text, QUOTA_FONT, 0);
    lv_color_t tc = COL_TEXT;
    if (win.percent >= 0) tc = win.percent > 50 ? COL_OK : (win.percent > 20 ? COL_WARN : COL_DANGER);
    lv_obj_set_style_text_color(text, tc, 0);

    // right slot: provider name on row 0 (highlighted), else reset time (muted)
    if (!right.empty()) {
        lv_obj_t* rst = lv_label_create(row);
        lv_label_set_text(rst, right.c_str());
        lv_obj_set_style_text_font(rst, QUOTA_FONT_M, 0);
        bool is_name = (right != win.resetsAt);
        lv_obj_set_style_text_color(rst, is_name ? COL_TEXT : COL_MUTED, 0);
        lv_obj_set_flex_grow(rst, 1);
        lv_obj_set_style_text_align(rst, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(rst, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    return row;
}

lv_obj_t* quota::QuotaView::_makeCard(const Card& card)
{
    lv_obj_t* box = lv_obj_create(_list);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, COL_CARD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(box, 1, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    if (!card.windows.empty()) {
        // Provider name rides the first row's right slot — no separate title row.
        for (size_t i = 0; i < card.windows.size(); i++) {
            const std::string& right = (i == 0) ? card.name : card.windows[i].resetsAt;
            _makeWindowRow(box, card.windows[i], right);
        }
    } else {
        // compact single row: name left, error/state right
        lv_obj_t* row = lv_obj_create(box);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 5, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, card.name.c_str());
        lv_obj_set_style_text_font(name, QUOTA_FONT, 0);
        lv_obj_set_style_text_color(name, COL_TEXT, 0);

        lv_obj_t* e = lv_label_create(row);
        lv_label_set_text(e, card.error.empty() ? "no windows" : card.error.c_str());
        lv_obj_set_style_text_font(e, QUOTA_FONT_M, 0);
        lv_obj_set_style_text_color(e, card.error.empty() ? COL_MUTED : COL_DANGER, 0);
        lv_obj_set_flex_grow(e, 1);
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_RIGHT, 0);
    }

    return box;
}

void quota::QuotaView::showSnapshot(const Snapshot& snap)
{
    _clear();
    if (!snap.error.empty()) {
        showError(snap.error);
        if (snap.cards.empty()) return;
    }
    for (const auto& card : snap.cards) {
        _makeCard(card);
    }
    if (snap.cards.empty() && snap.error.empty()) {
        showLoading("no quota sources");
    }
}
