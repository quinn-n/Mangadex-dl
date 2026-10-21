// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): the `download --json` document as a JSON value, so the batch
// document (src/core/batch_json.cpp) can embed one per row (batch plan T3b). Implemented in
// src/core/report.cpp; report_to_json() is exactly report_json(r) dumped with indent 2 and the
// replace handler, plus '\n'.
#pragma once

#include "mangadex_dl/report.hpp"

#include <nlohmann/json.hpp>

namespace mangadex_dl::detail {

/// The `download --json` document for `report`, keys in the documented order. The value owns all
/// of its data (strings are copied out of `report`), so it may outlive the report.
[[nodiscard]] nlohmann::ordered_json report_json(const RunReport& report);

} // namespace mangadex_dl::detail
