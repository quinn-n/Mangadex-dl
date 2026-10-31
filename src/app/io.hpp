// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process I/O seam. main() builds the real Io once (process_io); tests build one around
// std::ostringstream sinks and call run() in-process. Nothing below src/app/ touches std::cout,
// std::cerr or getenv directly.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl {
class Prompter; // include/mangadex_dl/prompt.hpp
} // namespace mangadex_dl

namespace mangadex_dl::app {

/// Environment read once at startup. getenv results are untrusted input: keep only what is needed,
/// already interpreted.
struct EnvSnapshot {
    bool no_color = false;       ///< NO_COLOR is set and non-empty
    bool clicolor_force = false; ///< CLICOLOR_FORCE is set, non-empty and not "0"
    bool term_dumb = false;      ///< TERM is exactly "dumb"
    /// HOME, XDG_CONFIG_HOME and XDG_STATE_HOME: owned copies of the raw values, nullopt when unset
    /// or empty. Not yet checked for being absolute (make_user_dirs does that).
    std::optional<std::string> home{};
    std::optional<std::string> xdg_config_home{};
    std::optional<std::string> xdg_state_home{};
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
    /// Borrowed (it must outlive every run() that uses this Io); null = the real terminal prompter
    /// (src/app/cmd_auth_prompt.cpp) over stdin_fd and err. Tests inject a scripted one.
    mangadex_dl::Prompter* prompter = nullptr;
};

/// The real process: std::cout/std::cerr, isatty(0/1/2), environment snapshot, stdin_fd 0, no
/// injected prompter. On the first call it also installs a RecordingStdioBuf
/// (recording_stdio_buf.hpp) over `stdout` as std::cout's buffer, for finish_stdout(). That buffer
/// is a function-local static, so it is destroyed only during static destruction, and its owner
/// puts std::cout's original buffer back first. libstdc++'s final flush of std::cout (its
/// ios_base::Init, constructed earlier and therefore destroyed later) never reaches a destroyed
/// buffer, and std::cerr's tie points at the std::cout object, which is never destroyed.
[[nodiscard]] Io process_io();

/// SIGPIPE policy A: default disposition and unblocked, even if the parent ignored or blocked it,
/// so `mangadex-dl ... | head -1` dies quietly with 141 instead of reporting write errors.
void restore_default_sigpipe() noexcept;

/// "mangadex-dl: <message>\n" on stderr with stdio only (never std::print: it can throw). For
/// main()'s catch ladder.
void last_resort(const char* message) noexcept;

/// Flush stdout and check it on every exit path (gnulib close_stdout analog). On a write error,
/// prints exactly one "mangadex-dl: write error: <reason>" line and turns rc 0 into 1 (a nonzero rc
/// passes through). The reason is the strerror_r text of the FIRST failed stdout write or flush,
/// wherever it happened: a flush forced by a write to the tied std::cerr, an overflow of the stdio
/// buffer in the middle of the run, or this final flush. With std::cout's buffer from process_io()
/// that errno is the one RecordingStdioBuf recorded; without it, only this flush's errno is known.
/// "output error" is the fallback when no errno was ever observed. Never closes stdout or stderr,
/// never allocates.
[[nodiscard]] int finish_stdout(int rc) noexcept;

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
