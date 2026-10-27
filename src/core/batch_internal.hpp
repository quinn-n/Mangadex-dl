// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): the batch dedupe that both multi-title planners share,
// plan_batch (src/core/batch_plan.cpp) and plan_list (src/core/mdlist_plan.cpp). Implemented in
// src/core/batch_plan.cpp (list plan T8).
#pragma once

#include "mangadex_dl/batch.hpp"

#include <vector>

namespace mangadex_dl::detail {

/// Sets `duplicate_of` to the earlier job's `line` on each job whose `options` equal an earlier
/// job's (compared member-wise, grouped by `options.manga`). Leaves first occurrences untouched,
/// never reorders or resizes `jobs`.
void mark_duplicates(std::vector<BatchJob>& jobs);

} // namespace mangadex_dl::detail
