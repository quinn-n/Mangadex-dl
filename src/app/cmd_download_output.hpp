// SPDX-License-Identifier: GPL-3.0-or-later
//
// What `download` prints (download plan T13, skip-existing plan T11): the stdout path stream,
// warnings and errors on stderr, -v/-vv detail, the TTY progress line, and the --json report. The
// download itself only talks to the DownloadObserver interface; this file turns its events into
// terminal output.
#pragma once

#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
class OutputRegistry;
} // namespace mangadex_dl

namespace mangadex_dl::app {

/// How much to print, decided once per run from the global flags and the process I/O.
struct OutputPolicy {
    bool json = false;         ///< --json: stdout carries only the final document (no path stream)
    bool quiet = false;        ///< -q: errors only on stderr
    int verbose = 0;           ///< -v = 1, -vv = 2
    bool progress = false;     ///< draw the \r progress line on stderr
    bool color_stderr = false; ///< color `warning:` / `error:` on stderr

    friend bool operator==(const OutputPolicy&, const OutputPolicy&) = default;
};

/// Progress only when stderr is a TTY, without -q, without --json and with TERM other than "dumb".
/// Color via resolve_color for stderr (stdout is never colored).
[[nodiscard]] OutputPolicy output_policy(const GlobalOptions& globals, const Io& io) noexcept;

/// A copy of `text` that is safe to put on a terminal inside a one-line diagnostic: C0 controls
/// (ESC, CR, LF, ...), DEL, C1 controls, bidi marks/embeddings/overrides/isolates (U+061C,
/// U+200E-F, U+202A-E, U+2066-9), invisible characters (U+200B-D, U+2060-6F, U+FEFF), U+2028/9
/// and invalid UTF-8 bytes each become '?'. Other valid UTF-8 is kept as is.
[[nodiscard]] std::string terminal_safe(std::string_view text);

/// "mangadex-dl: error: <message>\n" on stderr (`error:` red when policy.color_stderr). Printed
/// even with -q. The message passes through terminal_safe. Clear a ConsoleObserver's progress
/// line first (ConsoleObserver::finish) when one may be on screen.
void print_error(Io& io, const OutputPolicy& policy, std::string_view message);

/// The --json document (report_to_json) on stdout, written once. Never ANSI.
void print_report_json(Io& io, const RunReport& report);

/// Which row of a multi-title run the observer reports for. A default RowContext is `download`'s
/// own output: no prefix, no counter, and finish() prints the "already up to date" note.
struct RowContext {
    std::string prefix{};      ///< "<file>:<line>: " (raw; terminal_safe'd with each message)
    std::size_t position = 0;  ///< 1-based; 0 = no "(i/n) " progress counter
    std::size_t total = 0;     ///< the n of "(i/n) "
    bool exists_notice = true; ///< false: finish() leaves the "up to date" note to the caller
    /// mdlist only (null for `download` and `batch`): the run's OutputRegistry, holding the paths
    /// of the earlier rows (a row claims its own only after its run). The engine's "<path>: left
    /// alone: holds a different book; ..." warning is dropped for a path in it, because the
    /// runner's collision line for that path says it instead. run_title also hands it to the
    /// engine as DownloadServices::claimed, so --overwrite leaves such a path alone instead of
    /// replacing it. Borrowed and only read: the registry is declared before the runner and
    /// outlives every observer and every run_title call.
    const OutputRegistry* claimed = nullptr;
    /// batch and mdlist (null for `download`): the run's OutputRegistry, holding the files the
    /// earlier rows of a dry run planned. run_title hands it to the engine as
    /// DownloadServices::planned_in_run (OutputRegistry::planned_at), so a dry run without
    /// --overwrite decides such a path as the real run will find it. The observer never reads
    /// it; `claimed` stays mdlist-only. Borrowed and only read, like `claimed`.
    const OutputRegistry* planned = nullptr;

    friend bool operator==(const RowContext&, const RowContext&) = default;
};

/// Turns DownloadObserver events into console output. Not thread-safe: run_download calls every
/// event on its calling thread. Holds `io` by pointer: the Io must outlive this observer.
///
/// - stdout: `<path>\n` for each written file (updates included), and each planned file in a dry
///   run; nothing with --json. Files left alone (status exists) are never listed.
/// - stderr: `mangadex-dl: warning: ...` (not with -q), `mangadex-dl: error: ...` for a failed
///   file (always; a failed update of an incomplete file ends " (the existing file was kept)"),
///   -v per-file detail ("would write"/"would replace"/"would update" in a dry run, "wrote",
///   "<path>: up to date (N chapters)") and dedupe detail, -vv per-request trace lines. A file
///   that appeared under the name while ours was being written gets its own warning. Every
///   message, the run's own warnings and -v lines included, passes through terminal_safe.
/// - progress (policy.progress only): one line rewritten with '\r' and padded with spaces, cleared
///   before any other stderr line and at finish(). No cursor escapes.
/// - `row` (owned copy): its prefix goes after `warning:`/`error:` (verbose and trace lines stay
///   unprefixed), its position/total add a `(i/n) ` counter in front of the progress line,
///   exists_notice=false leaves the "already up to date" note to the caller, and a non-null
///   `claimed` drops the "holds a different book" warning for a path an earlier row claimed.
class ConsoleObserver final : public DownloadObserver {
public:
    ConsoleObserver(Io& io, const OutputPolicy& policy, RowContext row = {})
        : io_(&io), policy_(policy), row_(std::move(row)) {}
    ConsoleObserver(const ConsoleObserver&) = delete;
    ConsoleObserver& operator=(const ConsoleObserver&) = delete;
    ConsoleObserver(ConsoleObserver&&) = delete;
    ConsoleObserver& operator=(ConsoleObserver&&) = delete;
    ~ConsoleObserver() override = default;

    void on_plan(const Plan& plan) override;
    void on_epub_begin(const PlannedEpub& epub, std::size_t index, std::size_t total) override;
    void on_pages(std::size_t done, std::size_t total) override;
    void on_epub_end(const EpubOutcome& outcome) override;
    void on_warning(std::string_view message) override;
    void on_verbose(int level, std::string_view message) override;

    /// Call once run_download has returned, on every path (success, Error, interruption) and
    /// before print_error / print_report_json: clears the progress line and prints the single
    /// "N files are already up to date" note (no `warning:` prefix, never colored; not with -q,
    /// nor when row.exists_notice is false). It counts files left alone as up to date
    /// (existing == complete) only. Idempotent.
    void finish();

    /// Files left alone so far because they hold every chapter: what the note counts. Kept on
    /// every path, so a caller that prints the note itself (row.exists_notice false) can count
    /// the files of a run that then ended with an Error.
    [[nodiscard]] std::size_t up_to_date() const noexcept { return up_to_date_; }

private:
    /// The stderr detail for a file left alone (status exists): counts it toward the note when
    /// it is up to date, warns when it appeared during the run.
    void left_alone(const EpubOutcome& outcome);
    /// Is `message` the engine's "<path>: left alone: holds a different book; ..." warning for a
    /// path that row_.claimed holds (so the runner's collision line reports it)?
    [[nodiscard]] bool left_to_collision_line(std::string_view message) const;
    void warning(std::string_view message);
    /// "mangadex-dl: <message>" on stderr, terminal_safe, uncolored (-v lines and the note).
    void plain_line(std::string_view message);
    void draw_progress();
    void clear_progress();

    Io* io_ = nullptr; ///< borrowed, never null
    OutputPolicy policy_{};
    RowContext row_{};

    // Progress state for the file being built.
    bool in_epub_ = false;
    std::string label_{}; ///< "Vol. 03", already terminal_safe and bounded
    std::size_t index_ = 0;
    std::size_t total_epubs_ = 0;
    std::size_t pages_done_ = 0;
    std::size_t pages_total_ = 0;
    std::size_t drawn_width_ = 0; ///< bytes of the progress line on screen; 0 = nothing drawn

    std::size_t up_to_date_ = 0; ///< files left alone because they hold every chapter
    bool up_to_date_reported_ = false;
};

} // namespace mangadex_dl::app
