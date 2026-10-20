// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns the fetched title, feed and covers into a Plan: filtering, skips, dedupe, ordering and
// grouping per layout. Pure: no I/O, no clocks. Frozen interface (download plan T1); implemented
// in src/core/planner.cpp.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/selection.hpp"
#include "mangadex_dl/zip.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// An EPUB holds at most this many zip entries (no ZIP64; 0xFFFF is the ZIP64 sentinel). A
/// planned file whose entry count (fixed entries + 2 per page + the cover) is larger is reported
/// as failed instead of being downloaded.
inline constexpr std::size_t max_zip_entries_per_epub = zip_max_entries;

/// Everything the planner reads. Owned copies; the planner never keeps references into it.
struct PlanInput {
    MangaInfo manga{};
    std::vector<Chapter> chapters{}; ///< any language and order; the planner re-filters and sorts
    std::vector<Cover> covers{};
};

struct PlanOptions {
    Selection volumes = Selection::all();
    Selection loose_chapters = Selection::all();
    Layout layout = Layout::volumes;
    std::string lang = "en";
    std::vector<std::string> preferred_groups{}; ///< --group names or UUIDs, in priority order
    DirectionChoice direction = DirectionChoice::automatic;
    std::optional<std::string> title_override{};
};

/// Builds the plan. Deterministic: shuffled input yields an identical plan. An empty `epubs` list
/// is a valid result (the caller maps it to exit 5).
[[nodiscard]] Result<Plan> build_plan(const PlanInput& input, const PlanOptions& options);

/// `title.en`, else the first alt title in `lang`, else the first title value.
[[nodiscard]] std::string resolve_title(const MangaInfo& manga, std::string_view lang);

/// The index of the first entry of `preferred` (--group names or UUIDs, in priority order) that
/// matches one of `groups` by ASCII case-insensitive name or id, or `preferred.size()` when none
/// matches (the worst rank). The dedupe and the update re-pick both rank releases with it.
[[nodiscard]] std::size_t group_preference_rank(std::span<const ScanGroup> groups,
                                                std::span<const std::string> preferred);

/// `automatic` is rtl for a `ja` or `ja-ro` original and ltr otherwise.
[[nodiscard]] Direction resolve_direction(DirectionChoice choice,
                                          std::string_view original_language) noexcept;

} // namespace mangadex_dl
