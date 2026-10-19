// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update decision for one planned file that already exists and is ours: match planned chapters
// against held ones, classify the file (complete, incomplete, diverged), build the final chapter
// list (what to reuse, what to download), and reconcile the report. Pure: no I/O, no clocks.
// Frozen interface (skip-existing plan T1); implemented in src/core/update_plan.cpp.
#pragma once

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/model.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mangadex_dl {

enum class UpdateState {
    complete,   ///< every planned chapter is held: leave the file alone
    incomplete, ///< some planned chapters are missing (or re-picked), none held is extra: rebuild
    diverged,   ///< missing and extra chapters: leave the file alone, with a warning
};

enum class ChapterSource {
    reuse,    ///< copy the held pages
    download, ///< fetch the planned release
};

/// One chapter of the rebuilt file, in plan reading order. `held` and `planned` are indices
/// (never iterators) into BookManifest::chapters and PlannedEpub::chapters of the inputs the
/// decision was made from; those containers must stay unmodified while the decision is used.
struct FinalChapter {
    Chapter chapter{}; ///< what the file will hold (the held release keeps its id and metadata)
    ChapterSource source = ChapterSource::download;
    ImageQuality quality = ImageQuality::data; ///< the held quality when reused, else the run's
    std::optional<std::size_t> held{};         ///< the matched held chapter, if any
    std::size_t planned = 0;                   ///< the planned chapter this entry stands for

    friend bool operator==(const FinalChapter&, const FinalChapter&) = default;
};

/// A held release kept instead of a planned one with the same number (different id).
struct KeptRelease {
    std::string held_id{};
    std::string planned_id{};
    std::string key_label{}; ///< "Vol. 1 Ch. 1" or "Ch. 10"

    friend bool operator==(const KeptRelease&, const KeptRelease&) = default;
};

/// A held release replaced by a planned one that --group ranks strictly better (decision 5).
struct Repick {
    std::string held_id{};
    std::string planned_id{};
    std::string held_group{};    ///< the held release's group names, for the -v line
    std::string planned_group{}; ///< the planned release's group names
    std::string key_label{};

    friend bool operator==(const Repick&, const Repick&) = default;
};

struct UpdateDecision {
    UpdateState state = UpdateState::complete;
    std::vector<FinalChapter> chapters{}; ///< the final list, in plan reading order
    std::vector<std::size_t> missing{};   ///< planned indices to download (re-picked included)
    std::vector<std::size_t> extra{};     ///< held indices no planned chapter matched
    std::vector<KeptRelease> kept{};
    std::vector<Repick> repicked{};

    friend bool operator==(const UpdateDecision&, const UpdateDecision&) = default;
};

/// Matches `planned` against `held` (same id, then same normalized key, then a loose held chapter
/// that the plan now puts in a volume; each held chapter at most once) and decides. With a
/// non-empty `preferred_groups`, a release matched by number is re-picked when the planned one
/// ranks strictly better (group_preference_rank). `quality` is the run's image quality.
[[nodiscard]] UpdateDecision decide_update(const PlannedEpub& planned, const BookManifest& held,
                                           std::span<const std::string> preferred_groups,
                                           ImageQuality quality);

/// `decision` with the entry at `final_index` switched to downloading the planned release for its
/// key (a held page failed validation). Any KeptRelease for it is removed.
///
/// Precondition: `final_index < decision.chapters.size()`. An out-of-range index is checked, never
/// undefined behaviour: `decision` is returned unchanged.
[[nodiscard]] UpdateDecision fall_back_to_download(UpdateDecision decision, std::size_t final_index,
                                                   const PlannedEpub& planned,
                                                   ImageQuality quality);

/// `planned` with `chapters` replaced by the final list and `planned_pages` = held images +
/// planned pages. File name, title, identifier, cover and series position come from the plan.
[[nodiscard]] PlannedEpub final_epub(const PlannedEpub& planned, const UpdateDecision& decision);

/// Ids of the chapters the rebuild downloads, in final-list order.
[[nodiscard]] std::vector<std::string> new_chapter_ids(const UpdateDecision& decision);

/// Ids of the chapters the rebuild reuses, in final-list order.
[[nodiscard]] std::vector<std::string> reused_chapter_ids(const UpdateDecision& decision);

/// The plan's skipped list after substitutions: each kept held release loses its `duplicate`
/// entry, and the planned release it stands in for is appended as a duplicate of it.
[[nodiscard]] std::vector<SkippedChapter> reconcile_skipped(std::vector<SkippedChapter> skipped,
                                                            std::span<const KeptRelease> kept,
                                                            const PlannedEpub& planned);

/// The chapters' display labels (for example "Ch. 4") for the -v lines: at most `max_shown`
/// labels, then "and K more".
[[nodiscard]] std::string chapter_labels(std::span<const Chapter> chapters,
                                         std::size_t max_shown = 5);

} // namespace mangadex_dl
