// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `login` and `logout` subcommands (follows plan T17).
//
// `mangadex-dl login` always does a fresh password grant (LoginMode::password) through the shared
// login phase (cmd_auth.hpp) and saves the session for `follows`; it reports where the login was
// saved, or exits 1 if it could not be saved. `mangadex-dl logout` revokes the saved refresh token
// (best effort) and removes the session file (core logout()). Neither ever writes to stdout, also
// with --json: every line goes to stderr through the print_* helpers, which make it terminal_safe.
//
// Phases of both: validate (pure, exit 2) -> the interrupt handlers -> guard_secrets() -> agent ->
// backend -> trace -> the login phase (login) or logout() (logout) -> what to print.
#pragma once

#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/auth.hpp"

#include <memory>
#include <span>
#include <string>

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// The whole `login` command after parsing, testable without CLI11: check_raw_auth_args and the
/// API base (parse, then check_auth_api_base) before anything is opened, then the interrupt
/// handlers, guard_secrets(), make_backend, and run_login_phase(LoginMode::password); then the
/// session is closed and the outcome printed (not with -q): `logged in to MangaDex as <username>;
/// the login is saved in <path>` (exit 0), plus a warning when the file is in place but its
/// directory could not be synced (exit 0), or an error when it could not be saved (exit 1).
/// stdout is never written. `args.auth.credentials_given` must already be recorded
/// (record_auth_options). `args`, `globals` and `io` are only borrowed for the call.
[[nodiscard]] int run_login_command(const RawLoginArgs& args, const GlobalOptions& globals, Io& io);

/// The same, over `backend` instead of a fresh make_backend (tests inject a recording fixture
/// backend). `backend` must serve args.api_base and is borrowed for the call only.
[[nodiscard]] int run_login_command(const RawLoginArgs& args, const GlobalOptions& globals, Io& io,
                                    const Backend& backend);

/// What `login` prints once its login phase succeeded and was closed, by save_outcome(notes);
/// returns the exit code. Not with -q (except the error):
///  - saved: `logged in to MangaDex as <username>; the login is saved in <path>`, exit 0;
///  - unsynced: the same line, then the warning `the login was saved in <path>, but the directory
///    could not be synced; it may not survive a crash`, exit 0;
///  - failed: the error `could not save the MangaDex login to <path>: <why>` (or, without a
///    session path, `could not save the MangaDex login: <why>`), exit 1.
/// The username is cut for display (username_for_display); every line goes through the print_*
/// helpers, which make it terminal_safe. Never a secret, never stdout.
[[nodiscard]] int report_login(Io& io, const OutputPolicy& policy, const LoginNotes& notes);

/// The whole `logout` command after parsing, testable without CLI11: the API base (parse, then
/// check_auth_api_base) before anything is opened, then the interrupt handlers, guard_secrets(),
/// make_backend and logout(). Prints (not with -q) `logged out; removed <path>` or `not logged in
/// (no saved login at <path>)`, after logout()'s warning if there is one (a failed revoke, an
/// unreadable session): exit 0. A file that could not be removed or a session lock held elsewhere
/// is exit 1. An interrupt (during the lock wait or the revoke) prints nothing and leaves the file
/// in place. stdout is never written. `args`, `globals` and `io` are only borrowed for the call.
[[nodiscard]] int run_logout_command(const RawLogoutArgs& args, const GlobalOptions& globals,
                                     Io& io);

/// The same, over `backend` instead of a fresh make_backend (tests inject a recording fixture
/// backend). `backend` must serve args.api_base and is borrowed for the call only.
[[nodiscard]] int run_logout_command(const RawLogoutArgs& args, const GlobalOptions& globals,
                                     Io& io, const Backend& backend);

/// The `login --help` footer: what login does and where it saves, the client and the credentials
/// file, account switching, the stdout note, examples, then the shared exit-status table.
/// Pre-aligned, at most 80 columns, no trailing newline.
[[nodiscard]] std::string login_help_footer();

/// The `logout --help` footer: what logout does, the stdout note, an example, then the shared
/// exit-status table. Pre-aligned, at most 80 columns, no trailing newline.
[[nodiscard]] std::string logout_help_footer();

/// Adds `login` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `login` is chosen.
[[nodiscard]] std::unique_ptr<Command> register_login(CLI::App& parent, Command*& selected);

/// Adds `logout` to `parent`, like register_login.
[[nodiscard]] std::unique_ptr<Command> register_logout(CLI::App& parent, Command*& selected);

/// Tests only (until cli.cpp registers the commands): a throwaway root CLI::App with only `login`
/// and `logout` registered, parsing `args` (no argv[0]; args[0] is the subcommand), then the
/// selected Command's run(io, globals). --help prints to io.out and is exit 0; any other CLI11
/// parse error is exit 2 with CLI11's stock text on io.err, which may echo an argument's value
/// (the value-free mapping for these commands is cli.cpp's, T19): never show it to a user.
[[nodiscard]] int run_login_logout_for_testing(std::span<const std::string> args,
                                               const GlobalOptions& globals, Io& io);

} // namespace mangadex_dl::app
