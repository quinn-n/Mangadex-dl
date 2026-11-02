// SPDX-License-Identifier: GPL-3.0-or-later
//
// `login` and `logout` (follows plan T17). See cmd_login.hpp for the contract; the notes here are
// about order and lifetime.
//
// Order: the pure checks (the raw auth arguments, then the API base) come first, so a custom
// --api-base never sees a file descriptor, a credential or a request. Then the interrupt handlers
// (restored when the command returns; main() re-raises a pending signal after finish_stdout()),
// guard_secrets() (before any secret is read: the login phase also calls it, logout has no login
// phase), the agent, the backend and the trace.
//
// Lifetime: everything a LoginSession borrows (the Io, the Backend's transport and clock, the
// StderrTrace) is declared before the LoginPhase, so it outlives the phase; io.prompter, when
// set, is the caller's and outlives the whole run. Every path after a successful login phase
// closes it (close_login_phase) before returning. logout's InterruptBridge lives only around the
// logout() call and is joined before its outcome is printed.
#include "cmd_login.hpp"

#include "cmd_auth.hpp"
#include "cmd_batch_jobs.hpp"
#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "cmd_trace.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/session_store.hpp"

#include <CLI/CLI.hpp>

#include <expected>
#include <format>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl::app {
namespace {

constexpr std::string_view login_name = "login";
constexpr std::string_view logout_name = "logout";

/// The `login --help` footer, before the exit-status table. Kept aligned (and within 80 columns):
/// the root formatter has footer formatting off.
constexpr std::string_view login_footer{
    "Logs in to MangaDex with your password and saves the login (never the\n"
    "password) for 'mangadex-dl follows', readable only by you, in\n"
    "$XDG_STATE_HOME/mangadex-dl/session.json (~/.local/state/mangadex-dl/ by\n"
    "default). It always logs in afresh, so it also renews a login that ended.\n"
    "\n"
    "Logging in needs a MangaDex personal API client: create one at\n"
    "https://mangadex.org/settings (API Clients); a new client may need approval\n"
    "by MangaDex staff before it works. In a terminal, login asks for what it\n"
    "does not know yet: the client id and secret, your username and your\n"
    "password. The client and the username of the saved login are reused, so\n"
    "usually only the password is asked.\n"
    "\n"
    "The details can also come from a private file (chmod 600):\n"
    "$XDG_CONFIG_HOME/mangadex-dl/credentials (~/.config/mangadex-dl/ by\n"
    "default) or --credentials FILE, one 'key = value' per line with the keys\n"
    "client_id, client_secret, username and, optionally, password.\n"
    "Never put a password on the command line.\n"
    "\n"
    "To switch accounts, run 'mangadex-dl logout' first, or use --credentials\n"
    "with the other account.\n"
    "\n"
    "Nothing is written to stdout, also with --json.\n"
    "\n"
    "Examples:\n"
    "  Log in, typing the details at the prompts:\n"
    "    mangadex-dl login\n"
    "  In a script, with the password from a password manager:\n"
    "    pass show mangadex | mangadex-dl login --password-stdin\n"
    "\n"};

/// The `logout --help` footer, before the exit-status table (aligned, within 80 columns).
constexpr std::string_view logout_footer{
    "Ends the saved MangaDex login: asks MangaDex to end the session (best\n"
    "effort: the saved login is removed even if MangaDex can't be reached), then\n"
    "deletes $XDG_STATE_HOME/mangadex-dl/session.json\n"
    "(~/.local/state/mangadex-dl/ by default). Running it when you are not\n"
    "logged in is fine. It never reads or changes the credentials file: delete\n"
    "that yourself if it holds your password.\n"
    "\n"
    "Nothing is written to stdout, also with --json.\n"
    "\n"
    "Examples:\n"
    "  Log out of MangaDex:\n"
    "    mangadex-dl logout\n"
    "\n"};

/// One usage error plus the hint: exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error, std::string_view command) {
    print_usage_line(io, error.message);
    return usage_hint(io, command);
}

/// --api-base for an auth command (pure): ApiBase::parse, then check_auth_api_base (the official
/// API or a fixture tree only).
[[nodiscard]] Result<ApiBase> auth_api_base(std::string_view text, std::string_view command) {
    Result<ApiBase> base = ApiBase::parse(text);
    if (!base) {
        return base;
    }
    if (Result<void> allowed = check_auth_api_base(*base, command); !allowed) {
        return std::unexpected(std::move(allowed).error());
    }
    return base;
}

/// login's phase 1 (pure): check_raw_auth_args, then the API base.
[[nodiscard]] Result<ApiBase> validate_login(const RawLoginArgs& args) {
    if (Result<void> raw = check_raw_auth_args(args.auth); !raw) {
        return std::unexpected(std::move(raw).error());
    }
    return auth_api_base(args.api_base, login_name);
}

// ---- login --------------------------------------------------------------------------------------

/// The login phase and its report over `backend`, after the handlers and the guard.
[[nodiscard]] int login_over(const RawLoginArgs& args, const ApiBase& base,
                             const OutputPolicy& policy, Io& io, const Backend& backend) {
    // Declared before the phase: the session borrows it (and `io`, `backend`) until it dies.
    StderrTrace trace{io, policy};
    std::expected<LoginPhase, int> phase = run_login_phase(args.auth, base, LoginMode::password, io,
                                                           policy, backend, trace, login_name);
    if (!phase) {
        return phase.error(); // already printed (an interrupt prints nothing)
    }
    // Closed before anything is printed: the secrets are wiped and the lock released. The notes
    // stay valid (they live in the phase, which is destroyed only when this returns).
    close_login_phase(*phase, io, policy);
    return report_login(io, policy, phase->notes);
}

/// run_login_command over `injected`, or (null) over a fresh make_backend.
[[nodiscard]] int run_login(const RawLoginArgs& args, const GlobalOptions& globals, Io& io,
                            const Backend* injected) {
    // Phase 1 (pure): no file descriptor, no stdin and no request before the arguments are valid.
    const OutputPolicy policy = output_policy(globals, io);
    const Result<ApiBase> base = validate_login(args);
    if (!base) {
        return usage_failure(io, base.error(), login_name);
    }

    // Phase 2: restores the previous SIGINT/SIGTERM dispositions when this returns.
    const Result<ScopedInterruptHandlers> handlers = ScopedInterruptHandlers::install();
    if (!handlers) {
        print_error(io, policy, with_remedy(handlers.error()));
        return handlers.error().exit_code();
    }
    guard_secrets(); // run_login_phase calls it too; harmless

    if (injected != nullptr) {
        return login_over(args, *base, policy, io, *injected);
    }
    // Declared in borrow order (agent -> backend), so the backend outlives the login below.
    const std::string agent = user_agent();
    const Result<Backend> backend = make_backend(*base, agent);
    if (!backend) {
        print_error(io, policy, with_remedy(backend.error()));
        return backend.error().exit_code();
    }
    return login_over(args, *base, policy, io, *backend);
}

// ---- logout -------------------------------------------------------------------------------------

/// A failed logout(): an interrupt prints nothing (main() re-raises the signal; the file is left
/// in place); a usage error gets the hint; anything else (the file could not be removed, the lock
/// is held elsewhere) is one error line.
[[nodiscard]] int logout_failure(Io& io, const OutputPolicy& policy, const Error& error) {
    if (error.code == Errc::interrupted) {
        return error.exit_code();
    }
    if (error.code == Errc::usage) {
        return usage_failure(io, error, logout_name);
    }
    print_error(io, policy, error.message);
    return error.exit_code();
}

/// What logout prints when it finished: its warning first (a failed revoke, an unreadable
/// session; it says the login was removed anyway), then what happened. Always exit 0.
[[nodiscard]] int report_logout(Io& io, const OutputPolicy& policy, const LogoutOutcome& outcome) {
    if (outcome.warning.has_value()) {
        print_warning(io, policy, *outcome.warning);
    }
    if (outcome.removed) {
        print_note(io, policy, std::format("logged out; removed {}", outcome.path.string()));
    } else if (outcome.path.empty()) {
        // No usable HOME: there is no state directory, so there can be no saved login.
        print_note(io, policy,
                   "not logged in (no saved login: HOME is not set to an absolute path)");
    } else {
        print_note(io, policy,
                   std::format("not logged in (no saved login at {})", outcome.path.string()));
    }
    return exit_code::success;
}

/// logout() and its report over `backend`, after the handlers and the guard.
[[nodiscard]] int logout_over(const ApiBase& base, const OutputPolicy& policy, Io& io,
                              const Backend& backend) {
    // Every pointer in `services` is borrowed for the call: `backend` is the caller's, `trace`
    // is declared here and outlives logout(). Assigned member by member, not with designated
    // initializers: the clang-analyzer leak false positive explained in run_title
    // (cmd_download_run.cpp).
    StderrTrace trace{io, policy};
    AuthServices services{};
    services.transport = backend.transport.get();
    services.clock = backend.clock;
    services.jitter = make_jitter();
    services.trace = &trace;
    Result<LogoutOutcome> outcome{};
    {
        // The bridge turns SIGINT/SIGTERM into a stop request while logout() runs (the lock
        // wait, the revoke); it is joined at the end of this block.
        const std::stop_source stop{};
        const InterruptBridge bridge{stop};
        outcome = logout(base, user_dirs(io.env), services, LockTiming{}, stop.get_token());
    }
    if (!outcome) {
        return logout_failure(io, policy, outcome.error());
    }
    return report_logout(io, policy, *outcome);
}

/// run_logout_command over `injected`, or (null) over a fresh make_backend.
[[nodiscard]] int run_logout(const RawLogoutArgs& args, const GlobalOptions& globals, Io& io,
                             const Backend* injected) {
    // Phase 1 (pure): nothing is opened before the API base is accepted.
    const OutputPolicy policy = output_policy(globals, io);
    const Result<ApiBase> base = auth_api_base(args.api_base, logout_name);
    if (!base) {
        return usage_failure(io, base.error(), logout_name);
    }

    const Result<ScopedInterruptHandlers> handlers = ScopedInterruptHandlers::install();
    if (!handlers) {
        print_error(io, policy, with_remedy(handlers.error()));
        return handlers.error().exit_code();
    }
    // Before the saved session (which holds the client secret and the refresh token) is read.
    guard_secrets();

    if (injected != nullptr) {
        return logout_over(*base, policy, io, *injected);
    }
    const std::string agent = user_agent();
    const Result<Backend> backend = make_backend(*base, agent);
    if (!backend) {
        print_error(io, policy, with_remedy(backend.error()));
        return backend.error().exit_code();
    }
    return logout_over(*base, policy, io, *backend);
}

// ---- the Commands -------------------------------------------------------------------------------

/// The hidden --api-base of login and logout (a test seam, as on download).
void bind_api_base(CLI::App& sub, std::string& api_base) {
    sub.add_option("--api-base", api_base, "API base URL")->group("");
}

class LoginCommand final : public Command {
public:
    /// Binds options to members of this object (never to locals of register_login). Keeps the
    /// --credentials option, borrowed from `sub`, which outlives dispatch.
    void bind(CLI::App& sub) {
        credentials_ = bind_auth_options(sub, args_.auth);
        bind_api_base(sub, args_.api_base);
        sub.footer(login_help_footer());
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        // After parse(), before anything reads the args (the CLI::App is still alive here).
        if (credentials_ != nullptr) {
            record_auth_options(args_.auth, *credentials_);
        }
        return run_login_command(args_, globals, io);
    }

private:
    RawLoginArgs args_{};
    /// Borrowed from the CLI::App (valid until it is destroyed, after dispatch); never owned.
    const CLI::Option* credentials_ = nullptr;
};

class LogoutCommand final : public Command {
public:
    void bind(CLI::App& sub) {
        bind_api_base(sub, args_.api_base);
        sub.footer(logout_help_footer());
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        return run_logout_command(args_, globals, io);
    }

private:
    RawLogoutArgs args_{};
};

} // namespace

int report_login(Io& io, const OutputPolicy& policy, const LoginNotes& notes) {
    const std::string user = username_for_display(notes.username);
    const std::string path =
        notes.session_path.has_value() ? notes.session_path->string() : std::string{};
    const std::string saved_line =
        path.empty()
            ? std::format("logged in to MangaDex as {}; the login is saved", user)
            : std::format("logged in to MangaDex as {}; the login is saved in {}", user, path);
    switch (save_outcome(notes)) {
    case SaveOutcome::saved:
        print_note(io, policy, saved_line);
        return exit_code::success;
    case SaveOutcome::unsynced:
        // The file is in place (later runs will find it), but a crash could still lose it.
        print_note(io, policy, saved_line);
        print_warning(io, policy,
                      std::format("the login was saved in {}, but the directory could not be "
                                  "synced; it may not survive a crash",
                                  path));
        return exit_code::success;
    case SaveOutcome::failed:
        break;
    }
    // SaveOutcome::failed (or an out-of-range value): the one message for it (the login phase
    // leaves the core's save warning out in LoginMode::password). Without a usable HOME there is
    // no session path, and the reason says so.
    const std::string why = notes.save_error.has_value() ? *notes.save_error : std::string{};
    if (path.empty()) {
        print_error(io, policy, std::format("could not save the MangaDex login: {}", why));
    } else {
        print_error(io, policy,
                    std::format("could not save the MangaDex login to {}: {}", path, why));
    }
    return exit_code::failure;
}

int run_login_command(const RawLoginArgs& args, const GlobalOptions& globals, Io& io) {
    return run_login(args, globals, io, nullptr);
}

int run_login_command(const RawLoginArgs& args, const GlobalOptions& globals, Io& io,
                      const Backend& backend) {
    return run_login(args, globals, io, &backend);
}

int run_logout_command(const RawLogoutArgs& args, const GlobalOptions& globals, Io& io) {
    return run_logout(args, globals, io, nullptr);
}

int run_logout_command(const RawLogoutArgs& args, const GlobalOptions& globals, Io& io,
                       const Backend& backend) {
    return run_logout(args, globals, io, &backend);
}

std::string login_help_footer() { return std::string{login_footer} + exit_status_footer(); }

std::string logout_help_footer() { return std::string{logout_footer} + exit_status_footer(); }

std::unique_ptr<Command> register_login(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<LoginCommand>();
    CLI::App* sub = parent.add_subcommand(std::string{login_name},
                                          "Log in to MangaDex and save the login for follows");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in the caller's
    // invocation and `self` is owned by the caller's command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

std::unique_ptr<Command> register_logout(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<LogoutCommand>();
    CLI::App* sub =
        parent.add_subcommand(std::string{logout_name}, "Remove the saved MangaDex login");
    command->bind(*sub);
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace mangadex_dl::app
