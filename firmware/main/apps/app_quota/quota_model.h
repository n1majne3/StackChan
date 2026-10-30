/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <string>
#include <vector>

namespace quota {

/**
 * @brief One quota window on a provider card (e.g. "5 小时", "7 天", "本月")
 */
struct Window {
    std::string name;      // window label
    std::string text;      // human text: "剩余 95%" / "1.2k / 3k" / error msg
    float percent = -1;    // 0..100, -1 when unknown
    bool unlimited = false;
    std::string resetsAt;  // reset time, short local format "MM-DD HH:MM"
};

/**
 * @brief A provider/subscription card shown on screen
 */
struct Card {
    std::string source;    // "magpie" / "cpamgr"
    std::string name;      // display name, e.g. "OpenCode Go"
    std::string user;      // account / seat label
    std::string error;     // non-empty => card failed
    std::vector<Window> windows;
};

struct Snapshot {
    std::vector<Card> cards;
    std::string error;     // whole-fetch error
    uint32_t fetchedAtMs = 0;
};

}  // namespace quota
