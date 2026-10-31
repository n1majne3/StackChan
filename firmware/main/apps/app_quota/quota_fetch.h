/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "quota_model.h"
#include <functional>

namespace quota {

/**
 * @brief Fetch and parse all configured quota sources.
 *        Blocking — call from a worker task, not the lvgl/mooncake thread.
 * @param on_progress invoked after each source finishes with the
 *        accumulating snapshot, so the UI can render progressively.
 * @param should_abort checked between sources and between JSON items —
 *        returning true stops the rest of the fetch and keeps partial data.
 */
Snapshot fetchAll(std::function<void(const Snapshot&)> on_progress = nullptr,
                  std::function<bool()> should_abort = nullptr);

}  // namespace quota
