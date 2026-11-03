/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_cache.h"
#include <cstdint>

namespace quota {

/**
 * @brief Slow background refresh of the quota summary while the xiaozhi
 *        agent runs (every CONFIG_QUOTA_AGENT_REFRESH_MIN minutes).
 *
 *        The agent has little internal RAM to spare, so nothing stays
 *        resident: each refresh is a one-shot task with its stack in PSRAM
 *        that exists only for the fetch. It starts only while the agent is
 *        idle with WiFi up and enough free internal heap, and aborts if a
 *        conversation starts. The NVS write runs on the xiaozhi main task
 *        (flash writes are not allowed from a PSRAM stack).
 *
 *        Call poll() about once a second from the agent UI; it does nothing
 *        but compare timestamps until a refresh is due.
 */
namespace agent_refresh {

void poll();

/// If a refresh finished since `seen_revision`, copy it out and return true.
bool takeUpdate(uint32_t& seen_revision, CachedSummary& out);

}  // namespace agent_refresh

}  // namespace quota
