/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_model.h"
#include <cstdint>
#include <string>
#include <vector>

namespace quota {

/**
 * @brief Compact per-card summary persisted to NVS so the agent face can show
 *        the last known quota without running any fetch/network code itself
 *        (the xiaozhi agent has no internal-heap headroom to spare for that).
 */
struct CachedCard {
    std::string name;
    std::string window;   // worst window label, empty if none
    int percent = -1;     // worst remaining %, -1 unknown
    bool error  = false;
};

struct CachedSummary {
    std::vector<CachedCard> cards;
    int64_t fetchedAt = 0;  // unix epoch seconds, 0 = never

    bool empty() const
    {
        return cards.empty();
    }
};

/// Reduce a snapshot to its summary (worst remaining window per card).
CachedSummary summarize(const Snapshot& snap, int64_t fetchedAt);

/// Persist to NVS. Skips the flash write if nothing changed.
void saveCache(const CachedSummary& summary);

/// Load from NVS; empty summary if none stored.
CachedSummary loadCache();

}  // namespace quota
