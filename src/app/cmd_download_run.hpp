// SPDX-License-Identifier: GPL-3.0-or-later
//
// One title, run exactly as `download` runs it (batch plan T4): the backend (transport + clock),
// the User-Agent and retry jitter, the per-title DownloadServices and ConsoleObserver, and the
// closing lines `download` prints on stderr. `download` runs it once; `batch` and `mdlist` once per
// row, over one shared Backend.
#pragma once

#include "cmd_download_output.hpp"
#include "io.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/title_dir.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mangadex_dl::app {

/// The User-Agent MangaDex asks for: identifies the tool, never spoofed, no Via header.
[[nodiscard]] std::string user_agent();

/// `jitter(max)` returns a value in [0, max]. Seeded once per call (getrandom, else the steady
/// clock). The engine lives inside the callable (captured by value), and ApiClient serializes its
/// calls, so it needs no lock.
[[nodiscard]] std::function<std::uint32_t(std::uint32_t)> make_jitter();

/// The transport and clock behind one or more runs. Declared before the DownloadServices that
/// borrow them, so it outlives every run_download that uses it. Runs over one Backend must be
/// sequential.
struct Backend {
    std::unique_ptr<Transport> transport{};
    std::unique_ptr<Clock> owned_clock{}; ///< the instant clock in fixture mode; null otherwise
    Clock* clock = nullptr;               ///< owned_clock.get() or the process-wide system_clock()
};

/// Fixture mode (`--api-base file://...`): the hermetic file transport and an instant clock, so no
/// retry ever sleeps and libcurl is never loaded. Otherwise: libcurl and the real clock. The
/// fixture root is copied; `agent` is copied into the curl configuration.
[[nodiscard]] Result<Backend> make_backend(const ApiBase& api_base, const std::string& agent);

/// An Error's message, plus what to do about it where the user can act (exit 3: the network).
[[nodiscard]] std::string with_remedy(const Error& error);

/// The exit-5 diagnostic: what was asked for (as typed; print_error makes it terminal-safe) and
/// how many chapters were skipped for a reason other than dedupe.
[[nodiscard]] std::string no_chapters_message(const RawDownloadArgs& args, const RunReport& report);

/// The exit-6 closing line: how many files failed, and that a rerun retries only those.
/// A dry run only plans, so its failures (a file that can't be planned) won't clear on a rerun.
[[nodiscard]] std::string incomplete_message(const RunSummary& summary, std::size_t total,
                                             bool dry_run);

/// What one run_title call ended with.
struct TitleRun {
    std::optional<RunReport> report{}; ///< set when run_download returned a report
    std::optional<Error> error{};      ///< set when it returned an Error (incl. interrupted)
    int exit_code = 0;
    /// The closing message printed for this run, without the row prefix and without the remedy:
    /// the Error's message, the exit-5 or the exit-6 line. Empty when none was printed
    /// (success, or interrupted).
    std::string closing_error{};
    /// Files left alone as up to date, counted by the observer on every path (also when the run
    /// ended with an Error and so has no report): what the "already up to date" note counts.
    std::size_t up_to_date = 0;
};

/// One title, exactly as `download` runs it: fresh DownloadServices over `backend` (transport and
/// clock borrowed, make_jitter(), a default stop source, a copy of `agent`), a ConsoleObserver
/// with `row`, run_download, observer.finish(), then the closing lines `download` prints, each as
/// print_error(io, policy, row.prefix + message) (an Error gets with_remedy; an interruption
/// prints nothing). Never prints JSON. `backend` and `io` must outlive the call; nothing is kept
/// after it returns.
///
/// `title_dir` (mdlist --dir-per-title only; null for `download` and `batch`) becomes
/// DownloadServices::title_dir: borrowed for the call, called at most once by run_download on
/// this thread, never stored past it. A non-null `row.claimed` (mdlist only) becomes
/// DownloadServices::claimed (OutputRegistry::claimed_by), so --overwrite never replaces a path
/// an earlier row claimed. A non-null `row.planned` (batch and mdlist) becomes
/// DownloadServices::planned_in_run (OutputRegistry::planned_at), so a dry run without
/// --overwrite decides a path an earlier row planned as the real run will find it.
[[nodiscard]] TitleRun run_title(const DownloadOptions& options, const RawDownloadArgs& raw,
                                 const Backend& backend, const std::string& agent, Io& io,
                                 const OutputPolicy& policy, const RowContext& row,
                                 TitleDirResolver* title_dir = nullptr);

} // namespace mangadex_dl::app
