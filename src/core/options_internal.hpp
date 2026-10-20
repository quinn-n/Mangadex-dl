// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): the option validators src/core/options.cpp shares with the
// batch planner, src/core/batch_plan.cpp (batch plan T3a). Implemented in src/core/options.cpp.
// Every message here starts with the long option name ("--layout must be ...", "--volumes: ..."),
// exactly as `download` prints it; the batch planner strips that prefix for its column messages.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/selection.hpp"

#include <string_view>

namespace mangadex_dl::detail {

/// `volumes`, `chapters` or `single`; Errc::usage "--layout must be ..." otherwise.
[[nodiscard]] Result<Layout> parse_layout(std::string_view word);

/// `auto`, `ltr` or `rtl`; Errc::usage "--direction must be ..." otherwise.
[[nodiscard]] Result<DirectionChoice> parse_direction(std::string_view word);

/// Selection::parse with `flag` in front of its message: "<flag>: <message>".
[[nodiscard]] Result<Selection> parse_selection(std::string_view flag, std::string_view spec);

/// Everything validate_download_args checks after MANGA and the DIR checks, in the same order:
/// the two selections, both `none`, --layout, --lang, --direction, --title, --jobs, --api-base;
/// then the `report` derivation. `raw.manga` is ignored: `manga` is used instead. `raw.dir` is
/// taken as is (the callers check it).
[[nodiscard]] Result<DownloadOptions> validate_common(const RawDownloadArgs& raw, MangaId manga);

} // namespace mangadex_dl::detail
