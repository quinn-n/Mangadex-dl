// SPDX-License-Identifier: GPL-3.0-or-later
//
// The login glue (follows plan T16). See cmd_auth.hpp for the contract; the notes here are about
// order and lifetime.
//
// Order: the pure checks (the raw arguments, then the API base) come before stdin, the
// credentials file or the state directory is touched, so a custom --api-base never sees a
// credential. --password-stdin is read before the credentials file, as the spec's phases say.
//
// Lifetime: the TerminalPrompter, when chosen, is created here but owned by the returned
// LoginPhase, declared before its session, so the session (which may still ask for a password
// during a later refresh) never outlives it. The session also borrows the Backend's transport and
// clock and the caller's StderrTrace, which the caller declares before the LoginPhase. The
// InterruptBridge lives only around LoginSession::open: it is joined before this returns, and the
// session keeps no stop token past open().
#include "cmd_auth.hpp"

#include "cmd_auth_prompt.hpp"
#include "cmd_batch_jobs.hpp"
#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "cmd_trace.hpp"
#include "io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "mangadex_dl/text_safety.hpp"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::app {
namespace {

/// What session_store.cpp's StateDir::write reports when the new file is in place but the
/// directory fsync failed (its errno text follows after ": "). A copy of the core wording in
/// src/core/auth/session_store.cpp (StateDir::write, "saved, but cannot sync the state
/// directory"): keep the two in step.
constexpr std::string_view unsynced_prefix = "saved, but cannot sync the state directory";

/// One usage error plus the hint: exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error, std::string_view command) {
    print_usage_line(io, error.message);
    return usage_hint(io, command);
}

/// The spec's prompt rule: stdin and stderr are terminals, --no-input is absent and stdin is not
/// spoken for by --password-stdin.
[[nodiscard]] bool may_prompt(const RawAuthArgs& args, const Io& io) noexcept {
    return io.stdin_tty && io.stderr_tty && !args.no_input && !args.password_stdin;
}

[[nodiscard]] std::string_view how_text(LoginHow how) noexcept {
    switch (how) {
    case LoginHow::saved:
        return "saved login";
    case LoginHow::refreshed:
        return "refreshed";
    case LoginHow::password:
        return "password";
    }
    return "login"; // out-of-range value
}

/// The warning LoginSession::open adds next to save_error: a copy of the wording of save_warning()
/// in src/core/auth/login_session.cpp; keep the two in step. A drift is fail-safe (the warning is
/// then printed, not dropped), and the auth phase tests pin the equality (login prints it zero
/// times, follows once).
[[nodiscard]] std::string save_warning_text(const LoginNotes& notes, std::string_view why) {
    if (notes.session_path.has_value()) {
        return std::format("could not save the MangaDex login to {}: {}; the next run will need "
                           "the password again",
                           notes.session_path->string(), why);
    }
    return std::format(
        "could not save the MangaDex login: {}; the next run will need the password again", why);
}

/// notes.warnings through print_warning. With LoginMode::password (`login`), the warning of a
/// failed save is skipped: the command reports save_outcome itself. A warning whose text does not
/// match is printed (never dropped by mistake).
void print_login_warnings(Io& io, const OutputPolicy& policy, const LoginNotes& notes,
                          LoginMode mode) {
    std::optional<std::string> skipped{};
    if (mode == LoginMode::password && notes.save_error.has_value()) {
        skipped = save_warning_text(notes, *notes.save_error);
    }
    for (const std::string& warning : notes.warnings) {
        if (skipped.has_value() && warning == *skipped) {
            continue;
        }
        print_warning(io, policy, warning);
    }
}

/// -v: what the login server answered to a refused request: the HTTP status, the `error` code
/// (validated by classify_token_error) and its untrusted `error_description` (already cut to 200
/// bytes; print_note makes it terminal_safe, and it is a format argument, never the format).
void print_refusal_detail(Io& io, const OutputPolicy& policy, const TokenErrorInfo& info) {
    // Only a refusal that carries an `error` code is the login server's answer: the API's own
    // second 401 on the collection has none.
    if (policy.verbose < 1 || !info.code.has_value()) {
        return;
    }
    std::string line{"the MangaDex login server answered"};
    if (info.status > 0) {
        line += std::format(" HTTP {}", info.status);
    }
    line += std::format(" with {}", *info.code);
    if (info.description.has_value()) {
        line += std::format(": {}", *info.description);
    }
    print_note(io, policy, line);
}

/// The credentials file and the path its messages name: `--credentials FILE` (explicit: a missing
/// file is an error), else the default path (a missing file is an empty CredentialsFile), else
/// none at all (no usable HOME or XDG_CONFIG_HOME).
struct LoadedCredentials {
    CredentialsFile file{};
    std::filesystem::path path{}; ///< empty: no file to name
};

[[nodiscard]] Result<LoadedCredentials> load_credentials(const RawAuthArgs& args,
                                                         const UserDirs& dirs) {
    if (args.credentials_given) {
        std::filesystem::path path{args.credentials};
        Result<CredentialsFile> file = load_credentials_file(path, /*explicit_path=*/true);
        if (!file) {
            return std::unexpected(std::move(file).error());
        }
        return LoadedCredentials{.file = std::move(*file), .path = std::move(path)};
    }
    std::optional<std::filesystem::path> path = default_credentials_path(dirs);
    if (!path.has_value()) {
        return LoadedCredentials{};
    }
    Result<CredentialsFile> file = load_credentials_file(*path, /*explicit_path=*/false);
    if (!file) {
        return std::unexpected(std::move(file).error());
    }
    return LoadedCredentials{.file = std::move(*file), .path = std::move(*path)};
}

} // namespace

CLI::Option* bind_auth_options(CLI::App& sub, RawAuthArgs& args) {
    // Help text: each description fits the 44-column right column on one line. --credentials is
    // a path, not a secret; it is not ExistingFile (a missing file is reported when it is opened)
    // and shows no default (it depends on the environment).
    CLI::Option* credentials =
        sub.add_option("--credentials", args.credentials, "Read login details from FILE")
            ->type_name("FILE");
    sub.add_flag("--password-stdin", args.password_stdin, "Read the password from stdin");
    sub.add_flag("--no-input", args.no_input, "Never prompt; fail if a login is needed");
    return credentials;
}

void record_auth_options(RawAuthArgs& args, const CLI::Option& credentials) {
    args.credentials_given = credentials.count() > 0;
}

Result<RawAuthArgs> parse_auth_args_for_testing(std::span<const std::string> args) {
    std::vector<const char*> argv{};
    argv.reserve(args.size() + 1);
    argv.push_back("mangadex-dl");
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    if (!std::in_range<int>(argv.size())) {
        return std::unexpected(Error{.code = Errc::usage, .message = "too many arguments"});
    }
    // `raw` and `app` are locals that outlive parse(); the Option* is borrowed from `app`.
    RawAuthArgs raw{};
    CLI::App app{"auth options (tests only)"};
    const CLI::Option* const credentials = bind_auth_options(app, raw);
    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& error) {
        return std::unexpected(Error{.code = Errc::usage, .message = error.what()});
    }
    record_auth_options(raw, *credentials);
    return raw;
}

int report_auth_failure(Io& io, const OutputPolicy& policy, const AuthFailure& failure,
                        std::string_view command) {
    const Error& error = failure.error;
    if (error.code == Errc::interrupted) {
        return error.exit_code(); // silent: main() re-raises the signal
    }
    if (error.code == Errc::usage) {
        return usage_failure(io, error, command);
    }
    switch (failure.kind) {
    case AuthFailureKind::request:
        print_error(io, policy, with_remedy(error));
        return error.exit_code();
    case AuthFailureKind::refused:
        print_refusal_detail(io, policy, failure.info);
        print_error(io, policy, error.message);
        return error.exit_code();
    case AuthFailureKind::content:
    case AuthFailureKind::local:
        print_error(io, policy, error.message);
        return error.exit_code();
    }
    print_error(io, policy, error.message); // only reachable through an out-of-range enum value
    return error.exit_code();
}

UserDirs user_dirs(const EnvSnapshot& env) {
    // The views borrow the snapshot's strings for this call only.
    const auto view = [](const std::optional<std::string>& value) {
        return value.has_value() ? std::optional<std::string_view>{*value} : std::nullopt;
    };
    return make_user_dirs(view(env.home), view(env.xdg_config_home), view(env.xdg_state_home));
}

void guard_secrets() noexcept {
    // Best effort by design (secret.hpp): a process that stays dumpable still works.
    static_cast<void>(disable_core_dumps());
}

std::string username_for_display(std::string_view username) {
    if (username.size() <= max_username_display_bytes) {
        return std::string{username};
    }
    std::size_t length = max_username_display_bytes;
    while (length > 0 &&
           detail::is_utf8_continuation(static_cast<unsigned char>(username[length]))) {
        --length;
    }
    return std::string{username.substr(0, length)};
}

SaveOutcome save_outcome(const LoginNotes& notes) {
    if (!notes.save_error.has_value()) {
        return SaveOutcome::saved;
    }
    if (notes.save_error->starts_with(unsynced_prefix)) {
        return SaveOutcome::unsynced;
    }
    return SaveOutcome::failed;
}

std::expected<LoginPhase, int> run_login_phase(const RawAuthArgs& args, const ApiBase& api_base,
                                               LoginMode mode, Io& io, const OutputPolicy& policy,
                                               const Backend& backend, StderrTrace& trace,
                                               std::string_view command) {
    // 1. Pure checks: no stdin, file or request before both pass.
    if (Result<void> raw = check_raw_auth_args(args); !raw) {
        return std::unexpected(usage_failure(io, raw.error(), command));
    }
    if (Result<void> base = check_auth_api_base(api_base, command); !base) {
        return std::unexpected(usage_failure(io, base.error(), command));
    }

    // Before any secret is read (the password, the credentials file, the saved session).
    guard_secrets();

    // 2. --password-stdin: read once whenever it is given.
    std::optional<Secret> stdin_password{};
    if (args.password_stdin) {
        Result<Secret> password = read_password_stdin(io.stdin_fd);
        if (!password) {
            if (password.error().code == Errc::usage) {
                return std::unexpected(usage_failure(io, password.error(), command));
            }
            print_error(io, policy, password.error().message);
            return std::unexpected(password.error().exit_code());
        }
        stdin_password = std::move(*password);
    }

    // 3. The credentials file.
    const UserDirs dirs = user_dirs(io.env);
    Result<LoadedCredentials> credentials = load_credentials(args, dirs);
    if (!credentials) {
        print_error(io, policy, credentials.error().message);
        return std::unexpected(credentials.error().exit_code());
    }

    // 4. The prompter. Owned by the LoginPhase (declared before its session) when it is ours.
    std::unique_ptr<Prompter> owned_prompter{};
    Prompter* prompter = nullptr;
    if (may_prompt(args, io)) {
        if (io.prompter != nullptr) {
            prompter = io.prompter;
        } else {
            owned_prompter = std::make_unique<TerminalPrompter>(io.stdin_fd, io.err);
            prompter = owned_prompter.get();
        }
    }

    // 5. The session. Every pointer in `services` is borrowed from the caller (backend, trace),
    // which outlives the session. Assigned member by member, not with designated initializers:
    // the clang-analyzer leak false positive explained in run_title (cmd_download_run.cpp).
    AuthServices services{};
    services.transport = backend.transport.get();
    services.clock = backend.clock;
    services.jitter = make_jitter();
    services.trace = &trace;
    LoginRequest request{
        .api_base = api_base,
        .dirs = dirs,
        .mode = mode,
        .credentials = std::move(credentials->file),
        .credentials_path = std::move(credentials->path),
        .password_stdin = std::move(stdin_password),
        .prompter = prompter,
        .lock = LockTiming{},
    };
    LoginNotes notes{};
    AuthResult<std::unique_ptr<LoginSession>> session{};
    {
        // The bridge turns SIGINT/SIGTERM into a stop request while open() runs (a token POST,
        // its retries, the lock wait); it is joined at the end of this block.
        const std::stop_source stop{};
        const InterruptBridge bridge{stop};
        session = LoginSession::open(std::move(request), services, stop.get_token(), notes);
    }
    if (!session) {
        if (session.error().error.code != Errc::interrupted) {
            print_login_warnings(io, policy, notes, mode);
        }
        return std::unexpected(report_auth_failure(io, policy, session.error(), command));
    }

    print_login_warnings(io, policy, notes, mode);
    if (policy.verbose >= 1) {
        print_note(io, policy,
                   std::format("logged in to MangaDex as {} ({})",
                               username_for_display(notes.username), how_text(notes.how)));
    }
    LoginPhase phase{};
    phase.prompter = std::move(owned_prompter);
    phase.session = std::move(*session);
    phase.notes = std::move(notes);
    return phase;
}

void close_login_phase(LoginPhase& phase, Io& io, const OutputPolicy& policy) {
    if (phase.session == nullptr) {
        return;
    }
    // Drained before close(): close() discards them.
    const std::vector<std::string> late = phase.session->take_late_warnings();
    for (const std::string& warning : late) {
        print_warning(io, policy, warning);
    }
    phase.session->close();
}

} // namespace mangadex_dl::app
