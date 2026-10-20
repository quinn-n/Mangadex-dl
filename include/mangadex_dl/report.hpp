// SPDX-License-Identifier: GPL-3.0-or-later
//
// The outcome of a download run: per-file statuses, the exit-code rule, and the --json document.
// Frozen interface (download plan T1, extended by skip-existing plan T1); implemented in
// src/core/report.cpp.
#pragma once

#include "mangadex_dl/model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mangadex_dl {

enum class EpubStatus {
    planned, ///< dry-run: would be written (or, for an incomplete file, updated)
    written, ///< published to DIR
    exists,  ///< a file of that name already existed and was left alone (up to date, diverged or
             ///< foreign); --overwrite was not given
    failed,  ///< a network or media failure; nothing was published (an existing file is kept)
};

/// What was found under the file's name before the run decided what to do with it.
enum class ExistingKind {
    none,       ///< nothing (or a dry run without DIR)
    complete,   ///< ours and up to date: left alone
    incomplete, ///< ours with chapters missing: rebuilt with page reuse (dry run: would be)
    diverged,   ///< ours with chapters missing and extra ones held: left alone, with a warning
    foreign,    ///< not ours, not a regular file, unreadable or changed: left alone, warned
    unchecked,  ///< --overwrite: replaced without inspecting it
};

struct EpubOutcome {
    PlannedEpub epub{};
    std::string path{}; ///< DIR/<file_name> with DIR as typed
    EpubStatus status = EpubStatus::planned;
    /// Written over an existing file (dry run: would be), by --overwrite or by an update of an
    /// incomplete file.
    bool replaced = false;
    std::size_t pages = 0;
    std::uint64_t bytes = 0;
    std::string error{}; ///< non-empty only for failed
    ExistingKind existing = ExistingKind::none;
    std::vector<std::string> new_chapters{};    ///< incomplete only: ids the update downloads
    std::vector<std::string> reused_chapters{}; ///< incomplete only: ids copied from the old file

    friend bool operator==(const EpubOutcome&, const EpubOutcome&) = default;
};

struct RunSummary {
    std::size_t planned = 0;
    std::size_t written = 0;
    std::size_t exists = 0;
    std::size_t failed = 0;

    friend bool operator==(const RunSummary&, const RunSummary&) = default;
};

struct RunReport {
    bool dry_run = false;
    Layout layout = Layout::volumes;
    MangaSummary manga{};
    std::vector<EpubOutcome> epubs{};
    std::vector<SkippedChapter> skipped{};

    friend bool operator==(const RunReport&, const RunReport&) = default;
};

/// Counts the statuses in `report.epubs`.
[[nodiscard]] RunSummary summarize(const RunReport& report) noexcept;

/// exit_code::incomplete (6) if any file failed, else exit_code::no_chapters (5) if there are no
/// files, else exit_code::success. "Everything already existed" is success.
[[nodiscard]] int exit_code_for(const RunReport& report) noexcept;

/// The note printed once a run (`download`) or a whole list (`batch`) has left `up_to_date`
/// files alone because they already hold every chapter: "1 file is already up to date; ..." /
/// "N files are already up to date; ...", or nullopt for 0.
[[nodiscard]] std::optional<std::string> up_to_date_notice(std::size_t up_to_date);

/// The --json document: `format_version: 1` and the design's schema, invalid UTF-8 replaced by
/// U+FFFD, never ANSI, terminated by '\n'.
[[nodiscard]] std::string report_to_json(const RunReport& report);

} // namespace mangadex_dl
