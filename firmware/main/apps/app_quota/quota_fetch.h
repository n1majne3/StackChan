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
 */
Snapshot fetchAll(std::function<void(const Snapshot&)> on_progress = nullptr);

}  // namespace quota
