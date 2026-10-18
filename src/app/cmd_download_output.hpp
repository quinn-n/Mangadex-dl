// SPDX-License-Identifier: GPL-3.0-or-later
//
// What `download` prints (download plan T13): the stdout path stream, warnings and errors on
// stderr, -v/-vv detail, the TTY progress line, and the --json report. The download itself only
// talks to the DownloadObserver interface; this file turns its events into terminal output.
#pragma once

#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"

#include <cstddef>
#include <string>
#include <string_view>

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

/// Turns DownloadObserver events into console output. Not thread-safe: run_download calls every
/// event on its calling thread. Holds `io` by pointer: the Io must outlive this observer.
///
/// - stdout: `<path>\n` for each written file, and each planned file in a dry run; nothing with
///   --json. Existing (skipped) files are never listed.
/// - stderr: `mangadex-dl: warning: ...` (not with -q), `mangadex-dl: error: ...` for a failed
///   file (always), -v per-file detail ("would write"/"would replace" in a dry run, "wrote") and
///   dedupe detail, -vv per-request trace lines.
/// - progress (policy.progress only): one line rewritten with '\r' and padded with spaces, cleared
///   before any other stderr line and at finish(). No cursor escapes.
class ConsoleObserver final : public DownloadObserver {
public:
    ConsoleObserver(Io& io, const OutputPolicy& policy) noexcept : io_(&io), policy_(policy) {}
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
    /// "N files already exist" warning (not with -q). Idempotent.
    void finish();

private:
    void warning(std::string_view message);
    void verbose_line(std::string_view message);
    void draw_progress();
    void clear_progress();

    Io* io_ = nullptr; ///< borrowed, never null
    OutputPolicy policy_{};

    // Progress state for the file being built.
    bool in_epub_ = false;
    std::string label_{}; ///< "Vol. 03", already terminal_safe and bounded
    std::size_t index_ = 0;
    std::size_t total_epubs_ = 0;
    std::size_t pages_done_ = 0;
    std::size_t pages_total_ = 0;
    std::size_t drawn_width_ = 0; ///< bytes of the progress line on screen; 0 = nothing drawn

    std::size_t exists_ = 0;
    bool exists_reported_ = false;
};

} // namespace mangadex_dl::app
