// SPDX-License-Identifier: GPL-3.0-or-later
//
// The download run: plan, then fetch, build and publish each file. Frozen interface (download
// plan T1); implemented in src/core/download.cpp (a stub until T14).
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace mangadex_dl {

class TitleDirResolver; // title_dir.hpp

/// Progress and diagnostics from run_download. Every call happens on the thread that called
/// run_download (never on a worker), so implementations need no locking. Views and references
/// are valid only during the call.
class DownloadObserver {
public:
    DownloadObserver(const DownloadObserver&) = delete;
    DownloadObserver& operator=(const DownloadObserver&) = delete;
    DownloadObserver(DownloadObserver&&) = delete;
    DownloadObserver& operator=(DownloadObserver&&) = delete;
    virtual ~DownloadObserver() = default;

    virtual void on_plan(const Plan& plan) = 0;
    /// `index` is 0-based; `total` is the number of files in the plan. Fires only for files that
    /// are actually downloaded.
    virtual void on_epub_begin(const PlannedEpub& epub, std::size_t index, std::size_t total) = 0;
    virtual void on_pages(std::size_t done, std::size_t total) = 0;
    /// Fires for every file in the plan, including ones reported `exists`, `planned` (dry run) or
    /// `failed` by a plan error, which get no preceding on_epub_begin.
    virtual void on_epub_end(const EpubOutcome& outcome) = 0;
    /// One line, without the "mangadex-dl: warning: " prefix.
    virtual void on_warning(std::string_view message) = 0;
    /// `level` 1 (-v) or 2 (-vv).
    virtual void on_verbose(int level, std::string_view message) = 0;

protected:
    DownloadObserver() = default;
};

/// What run_download talks to. `transport` and `clock` must be non-null; `title_dir` is nullable
/// (null = the plain stem). Every pointer is borrowed for the run_download call only and never
/// stored past it.
struct DownloadServices {
    Transport* transport = nullptr;
    Clock* clock = nullptr;
    std::function<std::uint32_t(std::uint32_t)> jitter{}; ///< see ApiClient
    std::stop_source stop{}; ///< shares its stop state with the caller's copy
    std::string user_agent{};
    /// --dir-per-title only; borrowed for the run_download call, nullable (null = the plain
    /// stem); called only on the calling thread.
    TitleDirResolver* title_dir = nullptr;
};

/// Runs the whole download. An Error is fatal for the run: Errc::failure (exit 1, a local
/// problem), Errc::network (3), Errc::not_found (4) or Errc::interrupted. With
/// options.dir_per_title, also Errc::row_dir (exit 1): this title's own directory below DIR is
/// unusable, nothing was written, and a multi-title run continues with the next title. Otherwise
/// the report says what happened per file; exit_code_for(report) gives 0, 5 or 6.
[[nodiscard]] Result<RunReport> run_download(const DownloadOptions& options,
                                             const DownloadServices& services,
                                             DownloadObserver& observer);

} // namespace mangadex_dl
