// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process I/O seam. main() builds the real Io once (process_io); tests build one around
// std::ostringstream sinks and call run() in-process. Nothing below src/app/ touches std::cout,
// std::cerr or getenv directly.
#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

namespace mangadex_dl::app {

/// Environment read once at startup. getenv results are untrusted input: keep only what is needed,
/// already interpreted.
struct EnvSnapshot {
    bool no_color = false;       ///< NO_COLOR is set and non-empty
    bool clicolor_force = false; ///< CLICOLOR_FORCE is set, non-empty and not "0"
    bool term_dumb = false;      ///< TERM is exactly "dumb"
};

struct Io {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): non-owning view
    std::ostream& out; ///< data only (stdout)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): non-owning view
    std::ostream& err; ///< diagnostics, progress, prompts (stderr)
    bool stdin_tty = false;
    bool stdout_tty = false;
    bool stderr_tty = false;
    EnvSnapshot env{};
    int stdin_fd = -1; ///< fd to read stdin from; borrowed, never closed; -1 = closed (the default)
};

/// The real process: std::cout/std::cerr, isatty(0/1/2), environment snapshot, stdin_fd 0.
[[nodiscard]] Io process_io();

/// SIGPIPE policy A: default disposition and unblocked, even if the parent ignored or blocked it,
/// so `mangadex-dl ... | head -1` dies quietly with 141 instead of reporting write errors.
void restore_default_sigpipe() noexcept;

/// "mangadex-dl: <message>\n" on stderr with stdio only (never std::print: it can throw). For
/// main()'s catch ladder.
void last_resort(const char* message) noexcept;

/// Flush stdout and check it on every exit path (gnulib close_stdout analog). On a write error,
/// prints "mangadex-dl: write error: <reason>" and turns rc 0 into 1. The reason is best-effort
/// (strerror_r, or "output error" when it is unknown). Never closes stdout or stderr.
[[nodiscard]] int finish_stdout(int rc) noexcept;

/// Records the errno of a failed mid-run flush of stdout (io.out) for finish_stdout. After a
/// failed write the buffered bytes are dropped, so the flush at exit succeeds and the reason would
/// otherwise be lost. The first nonzero value wins; 0 is ignored; finish_stdout uses it only when
/// its own flush reports no errno, and clears it.
void note_stdout_errno(int err) noexcept;

enum class ColorMode {
    automatic,
    always,
    never,
};

/// Precedence: --color=always|never > NO_COLOR > CLICOLOR_FORCE > TERM=dumb > isatty. Decide per
/// stream (pass io.stdout_tty or io.stderr_tty). --json output never calls this: it has no ANSI.
[[nodiscard]] bool resolve_color(ColorMode mode, const EnvSnapshot& env, bool is_tty) noexcept;

enum class Style {
    bold,
    dim,
    red,
    green,
    yellow,
};

/// Wraps text in an SGR sequence when enabled; returns it unchanged otherwise.
[[nodiscard]] std::string paint(std::string_view text, Style style, bool enabled);

} // namespace mangadex_dl::app
