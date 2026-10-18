// SPDX-License-Identifier: GPL-3.0-or-later
//
// The outcome of a download run: per-file statuses, the exit-code rule, and the --json document.
// Frozen interface (download plan T1); implemented in src/core/report.cpp.
#pragma once

#include "mangadex_dl/model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mangadex_dl {

enum class EpubStatus {
    planned, ///< dry-run: would be written
    written, ///< published to DIR
    exists,  ///< a file of that name already existed and --overwrite was not given
    failed,  ///< a network or media failure; nothing was published
};

struct EpubOutcome {
    PlannedEpub epub{};
    std::string path{}; ///< DIR/<file_name> with DIR as typed
    EpubStatus status = EpubStatus::planned;
    bool replaced = false; ///< --overwrite: written over an existing file (dry run: would be)
    std::size_t pages = 0;
    std::uint64_t bytes = 0;
    std::string error{}; ///< non-empty only for failed

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

/// The --json document: `format_version: 1` and the design's schema, invalid UTF-8 replaced by
/// U+FFFD, never ANSI, terminated by '\n'.
[[nodiscard]] std::string report_to_json(const RunReport& report);

} // namespace mangadex_dl
