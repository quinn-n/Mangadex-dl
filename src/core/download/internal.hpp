// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): the pieces run_download (src/core/download.cpp) shares with
// its two phases, src/core/download/plan_phase.cpp and src/core/download/execute_phase.cpp
// (download plan T14; skip-existing plan T10 adds download/existing.{hpp,cpp}; list plan T6 adds
// the --dir-per-title step between them, src/core/download/dir_per_title.cpp).
//
// Threads: run_download, both phases and every DownloadObserver call run on the calling thread.
// Only FetchPool workers run elsewhere; they reach this module through LineQueue::trace/note and
// stop_now, both thread-safe.
#pragma once

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fetch_pool.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/title_dir.hpp"

#include <filesystem>
#include <format>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl::detail {

/// Collects diagnostic lines from any thread until the calling thread hands them to the
/// observer (drain). The ApiClient's per-request trace lines are level 2 (-vv); note() adds lines
/// of any level. Every line is an owned copy: nothing borrowed crosses threads.
class LineQueue final : public TraceSink {
public:
    LineQueue() = default;

    /// A level-2 line (TraceSink; thread-safe).
    void trace(std::string line) override;

    /// A line at `level` (1 = -v, 2 = -vv). Thread-safe.
    void note(int level, std::string line);

    /// Passes every queued line to observer.on_verbose, oldest first. Calling thread only.
    void drain(DownloadObserver& observer);

private:
    struct Line {
        int level = 2;
        std::string text{};
    };

    std::mutex mutex_{};
    std::vector<Line> lines_{}; ///< guarded by mutex_
};

/// What both phases share. Every pointer is borrowed from run_download's locals (or its
/// caller's DownloadServices) and valid for the whole run; all but `title_dir` are non-null.
struct RunContext {
    const DownloadOptions* options = nullptr;
    ApiClient* client = nullptr;
    FetchPool* pool = nullptr;
    DownloadObserver* observer = nullptr;
    LineQueue* lines = nullptr;
    std::stop_source stop{}; ///< shares its stop state with DownloadServices::stop
    /// DownloadServices::title_dir: nullable (null = the plain stem); used only by
    /// resolve_title_dir, on the calling thread. Never reached by a FetchPool worker.
    TitleDirResolver* title_dir = nullptr;
};

/// The plan phase's result: the plan and the title record (the execute phase needs its creators).
struct PlanResult {
    MangaInfo manga{};
    Plan plan{};
};

/// Turns a pending SIGINT/SIGTERM into a stop request at once (the InterruptBridge notices it
/// only within its poll interval), then says whether the run must stop. Thread-safe.
[[nodiscard]] bool stop_now(std::stop_source& stop) noexcept;

/// Errc::interrupted, the error every stop unwinds with.
[[nodiscard]] Error interrupted_error();

/// An invariant of the download module broke (never expected): Errc::failure with
/// "internal error: <what>", so the run ends with exit 1.
[[nodiscard]] inline Error internal_error(std::string_view what) {
    return Error{.code = Errc::failure, .message = std::format("internal error: {}", what)};
}

/// Fetches the title, the feed and the covers, builds the plan, and reports it to the observer
/// (on_plan, then one warning per skipped chapter except duplicates). Errors are fatal for the
/// run: Errc::not_found, Errc::network, Errc::interrupted. A cover listing that fails is only a
/// warning.
[[nodiscard]] Result<PlanResult> plan_phase(RunContext& ctx);

/// --dir-per-title, between the phases: options.dir_per_title and a non-empty plan only (an
/// empty plan claims no directory). Asks ctx.title_dir for the title's directory name (null:
/// plan.file_stem as is), re-checks that it is one safe path component, passes the resolver's
/// warning to observer.on_warning, prints the -v "output directory: DIR/<name>" line, and runs
/// the read-only check_row_dir(DIR, name) pre-flight. Creates nothing. Returns
/// options.dir / name. Errors: Errc::interrupted; Errc::row_dir (that directory is unusable:
/// check_row_dir's message plus "; rename or remove it, or run without --dir-per-title"; a
/// multi-title run continues); Errc::failure (DIR itself is unusable, or the resolver broke
/// its contract: "internal error: ...").
[[nodiscard]] Result<std::filesystem::path> resolve_title_dir(RunContext& ctx, const Plan& plan);

/// Dry-run: reports every file as planned, exists or failed, touching nothing (an existing file
/// is assessed from its central directory and manifest only). Otherwise, in order, each file is
/// assessed (unless --overwrite; download/existing.hpp) and then written when absent, left alone
/// (exists) when complete, diverged or foreign, or rebuilt with page reuse and replaced when
/// incomplete. A per-file network or media failure marks that file failed (an existing file is
/// kept) and the run continues; a local file-system failure (Errc::failure) or a stop
/// (Errc::interrupted) aborts the run, and every temporary is discarded on the way out. The
/// report's skipped list is reconciled with the held releases the files keep.
[[nodiscard]] Result<RunReport> execute_phase(RunContext& ctx, const PlanResult& planned);

} // namespace mangadex_dl::detail
