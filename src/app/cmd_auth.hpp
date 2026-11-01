// SPDX-License-Identifier: GPL-3.0-or-later
//
// The login glue `follows` and `login` share (follows plan T16): the auth options, the user's
// directories from the environment snapshot, the core-dump guard, and the login phase (the
// --password-stdin read, the credentials file, the prompter choice, LoginSession::open and what
// it prints), plus the end of that phase (the late warnings, then close()).
//
// Everything this file prints goes through print_error / print_warning / print_note /
// print_usage_line, which make the whole line terminal_safe: the saved-login warnings, every
// failure message and the username are untrusted text. No secret is ever printed or formatted.
#pragma once

#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/session_store.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace CLI {
class App;
class Option;
} // namespace CLI

namespace mangadex_dl::app {

class StderrTrace; // cmd_trace.hpp

/// The longest username shown in a message, in bytes (cut on a UTF-8 boundary).
inline constexpr std::size_t max_username_display_bytes = 64;

/// Adds `--credentials FILE`, `--password-stdin` and `--no-input` to `sub`, bound to `args`
/// (which must outlive parse() and dispatch: a Command member). `--credentials` is bound to the
/// std::string verbatim (no default, not ExistingFile, no envname), so `--credentials ''` and
/// `--credentials --no-input` keep their value. Returns the `--credentials` option: borrowed
/// from `sub`, so valid until the CLI::App is destroyed. After parse(), and before anything reads
/// `args`, the Command sets `args.credentials_given = option->count() > 0` (record_auth_options).
[[nodiscard]] CLI::Option* bind_auth_options(CLI::App& sub, RawAuthArgs& args);

/// `args.credentials_given = credentials.count() > 0`. Call it after parse(), in run(), with the
/// option bind_auth_options returned (the CLI::App is still alive there).
void record_auth_options(RawAuthArgs& args, const CLI::Option& credentials);

/// Tests only: a throwaway CLI::App with only the auth options, parsing `args` (no argv[0]) and
/// recording credentials_given as a Command does. A CLI11 parse error -> Errc::usage with
/// CLI11's what() verbatim, which may echo an argument's value: never show that message to a
/// user (the value-free mapping of parse errors for follows/login/logout is cli.cpp's, T19).
[[nodiscard]] Result<RawAuthArgs> parse_auth_args_for_testing(std::span<const std::string> args);

/// make_user_dirs over the snapshot's HOME, XDG_CONFIG_HOME and XDG_STATE_HOME.
[[nodiscard]] UserDirs user_dirs(const EnvSnapshot& env);

/// disable_core_dumps(), best effort (the outcome is not reported). From the main thread, before
/// any secret is read. run_login_phase calls it itself (after its pure checks); a command may also
/// call it earlier, right after the interrupt handlers (logout must: it has no login phase).
/// Calling it more than once is harmless.
void guard_secrets() noexcept;

/// `username` for display: at most max_username_display_bytes, cut on a UTF-8 boundary. Not yet
/// terminal_safe: print it only through the print_* helpers (they sanitize the whole line).
[[nodiscard]] std::string username_for_display(std::string_view username);

/// What a successful login phase hands to its command.
///
/// Declaration order is destruction order in reverse: `session` is destroyed before `prompter`,
/// so the session never outlives the terminal prompter it may still ask (a password fallback
/// after a 401 during the collection fetch). Move-constructible (to be returned), but neither
/// copyable nor assignable: an implicit move assignment would replace `prompter` before
/// `session`, leaving the old session pointing at a destroyed prompter for a moment.
struct LoginPhase {
    LoginPhase() = default;
    LoginPhase(LoginPhase&&) noexcept = default;
    LoginPhase(const LoginPhase&) = delete;
    LoginPhase& operator=(const LoginPhase&) = delete;
    LoginPhase& operator=(LoginPhase&&) = delete;
    ~LoginPhase() = default;

    /// The real terminal prompter, when the login phase chose it and Io injects none; null
    /// otherwise. Owned here because the session borrows it.
    std::unique_ptr<Prompter> prompter{};
    /// Never null after a successful run_login_phase; close_login_phase closes it (the object
    /// stays, closed, until the LoginPhase is destroyed).
    std::unique_ptr<LoginSession> session{};
    LoginNotes notes{};
};

/// Whether the session file was written (for `login`, which reports this itself).
enum class SaveOutcome {
    saved,    ///< no save_error
    unsynced, ///< written and in place, but the state directory could not be fsynced
    failed,   ///< not saved (no usable state directory, or the write failed)
};

/// Classifies notes.save_error.
[[nodiscard]] SaveOutcome save_outcome(const LoginNotes& notes);

/// The login phase of `follows` (LoginMode::reuse) and `login` (LoginMode::password), after the
/// interrupt handlers, the backend and the trace exist. In order:
///  1. check_raw_auth_args, then check_auth_api_base(api_base, command): exit 2 (the message and
///     `Run 'mangadex-dl <command> --help' ...`) before any file, stdin or request is touched;
///     then guard_secrets(), before any secret is read;
///  2. --password-stdin, if given, is read from io.stdin_fd (always, even if a session would make
///     it unnecessary): an empty one is exit 2, a read error exit 1;
///  3. the credentials file: `args.credentials` when credentials_given (a missing file is exit 1),
///     else default_credentials_path(user_dirs(io.env)) (a missing default file is fine): exit 1
///     on any problem;
///  4. the prompter: only when io.stdin_tty && io.stderr_tty && !no_input && !password_stdin, then
///     io.prompter, or a TerminalPrompter over io.stdin_fd and io.err; otherwise none (a missing
///     detail is then exit 8);
///  5. LoginSession::open, with its own stop source driven by an InterruptBridge (joined before
///     this returns).
///
/// On success it prints notes.warnings (print_warning) and, with -v, `logged in to MangaDex as
/// <username> (saved login|refreshed|password)`, then returns the phase. With LoginMode::password
/// (`login`) the warning of a failed save is left out: the command reports save_outcome itself
/// (exit 1, or a warning when the file is in place but unsynced), so it is never said twice.
///
/// On failure it prints the warnings, then the error, and returns the exit code: an interrupt
/// prints nothing (main() re-raises the signal); a usage error gets the help hint; a request
/// failure gets with_remedy; a refusal its message, plus with -v what the server answered (the
/// HTTP status, the error code and its description). stdout is never written.
///
/// Lifetime: `io`, `backend` and `trace` are borrowed by the returned session (and its prompter):
/// declare them before the LoginPhase, so they outlive it. `io.prompter`, when set, must outlive
/// the LoginPhase too. `args`, `api_base` and `policy` are only read during the call.
[[nodiscard]] std::expected<LoginPhase, int>
run_login_phase(const RawAuthArgs& args, const ApiBase& api_base, LoginMode mode, Io& io,
                const OutputPolicy& policy, const Backend& backend, StderrTrace& trace,
                std::string_view command);

/// The end of the login phase, after the last request that uses the session (the collection
/// fetch): drains LoginSession::take_late_warnings() through print_warning, then close() (the
/// secrets are wiped and the lock is released). Call it before any download. Idempotent; a phase
/// without a session is left alone. The session object itself stays alive (closed) until the
/// LoginPhase is destroyed.
void close_login_phase(LoginPhase& phase, Io& io, const OutputPolicy& policy);

} // namespace mangadex_dl::app
