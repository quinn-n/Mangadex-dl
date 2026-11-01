// SPDX-License-Identifier: GPL-3.0-or-later
//
// `follows` (follows plan T18). See cmd_follows.hpp for the contract; the notes here are about
// order and lifetime.
//
// Order: the pure checks (validate_follows_args, then the API base) come before the interrupt
// handlers, any file descriptor or any request, so a usage error never opens anything and a custom
// --api-base never sees a credential (the login phase checks the base again). The login phase
// owns the first secret read; the collection fetch is the only user of the session; then
// close_login_phase wipes the secrets and releases the session lock on every path out of the
// fetch (success, failure and interrupt alike) before anything else happens, so no token is alive
// while a single title downloads.
//
// Lifetime: declared in borrow order (handlers -> agent -> backend -> trace -> the LoginPhase ->
// the collection -> the jobs), so each lives until run_batch_jobs has returned, and the session
// (which borrows the backend's transport and clock and the trace) is destroyed before them.
//
// Output: every stderr line goes through print_error / print_warning / print_note /
// print_usage_line (terminal_safe on the whole line). DIR, a credential and a token are never in
// a message printed here; the username is printed only by the login phase (username_for_display)
// and goes raw only into the --json document.
#include "cmd_follows.hpp"

#include "cmd_auth.hpp"
#include "cmd_batch_jobs.hpp"
#include "cmd_download_options.hpp"
#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "cmd_trace.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::app {
namespace {

/// The subcommand word, for the usage hint and the API-base message.
constexpr std::string_view follows_command = "follows";

/// The most unknown reading-status values the -v note names; the rest are counted.
constexpr std::size_t max_unknown_statuses_shown = 5;

/// The `follows --help` footer, before the exit-status table. Kept aligned (and within 80
/// columns): the root formatter has footer formatting off.
constexpr std::string_view follows_footer{
    "LOGIN:\n"
    "  Reading your follows needs a MangaDex login through a personal API client.\n"
    "  Create one at https://mangadex.org/settings (API Clients); a new client\n"
    "  may need approval by MangaDex staff before it works. The first run in a\n"
    "  terminal asks for the client id and secret, your username and password,\n"
    "  logs in, and saves the login (never the password) to\n"
    "  $XDG_STATE_HOME/mangadex-dl/session.json (~/.local/state/mangadex-dl/ by\n"
    "  default), readable only by you. Later runs, cron jobs included, reuse it\n"
    "  until MangaDex ends the session; then run 'mangadex-dl login' again.\n"
    "\n"
    "  The details can also come from a private file (chmod 600):\n"
    "  $XDG_CONFIG_HOME/mangadex-dl/credentials (~/.config/mangadex-dl/ by\n"
    "  default) or --credentials FILE, one 'key = value' per line with the keys\n"
    "  client_id, client_secret, username and, optionally, password.\n"
    "  Never put a password on the command line.\n"
    "\n"
    "READING STATUS:\n"
    "  --status narrows the titles you follow to those whose reading status is\n"
    "  one of: reading, on_hold, plan_to_read, dropped, re_reading, completed.\n"
    "  Give several separated by commas, or repeat --status. Followed titles\n"
    "  without a reading status are left out. The titles keep the order of your\n"
    "  follows.\n"
    "\n"
    "  Only the lists of titles are fetched with your login; the titles\n"
    "  themselves are downloaded anonymously, one after another, as with mdlist.\n"
    "\n"
    "Examples:\n"
    "  Log in once, then download everything you follow into ~/Manga:\n"
    "    mangadex-dl login\n"
    "    mangadex-dl follows ~/Manga\n"
    "  Only what you are reading or plan to read, one folder per title:\n"
    "    mangadex-dl follows ~/Manga --status reading,plan_to_read --dir-per-title\n"
    "  In a script, with the password from a password manager:\n"
    "    pass show mangadex | mangadex-dl follows ~/Manga --password-stdin\n"
    "  Show what would be written, without downloading anything:\n"
    "    mangadex-dl follows ~/Manga --dry-run\n"
    "\n"
    "Running the same command again skips files that already hold every chapter,\n"
    "rebuilds files MangaDex has added chapters to, and downloads titles you have\n"
    "followed since the last run.\n"
    "\n"
    "Symlinks inside DIR are followed: don't point DIR at a directory that others\n"
    "can write to. Each title costs several API requests: many follows make a long\n"
    "run.\n"
    "\n"};

/// One usage error plus the hint: exit 2, nothing on stdout. print_usage_line makes the message
/// terminal_safe (an unknown --status word is echoed, cut to 32 bytes; DIR never is).
[[nodiscard]] int usage_failure(Io& io, const Error& error) {
    print_usage_line(io, error.message);
    return usage_hint(io, follows_command);
}

/// Binds every `follows` option to `args` (a Command member, or a test's local that outlives
/// parse()), in help order: DIR, the shared selection and run options (byte-identical to
/// mdlist's), --fail-fast, --dir-per-title, --status, then the auth options. Returns the
/// `--credentials` option (borrowed from `sub`).
[[nodiscard]] CLI::Option* bind_follows_options(CLI::App& sub, RawFollowsArgs& args) {
    // Help text: each description fits the 44-column right column on one line.
    sub.add_option("DIR", args.defaults.dir, "Output directory (created if missing)")
        ->type_name("")
        ->capture_default_str();
    bind_selection_options(sub, args.defaults);
    bind_run_options(sub, args.defaults);
    sub.add_flag("--fail-fast", args.fail_fast, "Stop at the first title that fails");
    sub.add_flag("--dir-per-title", args.dir_per_title, "Put each title in a subdirectory of DIR");
    // The --group pattern: one value per occurrence (expected(1) alone would reject a repeated
    // --status; TakeAll alone would swallow the next positional). StatusFilter::parse splits the
    // commas, so `--status reading,on_hold` equals `--status reading --status on_hold`.
    sub.add_option("--status", args.status, "Only follows with these reading statuses")
        ->type_name("WORD,...")
        ->expected(1)
        ->allow_extra_args(false)
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    return bind_auth_options(sub, args.auth);
}

/// The collection fetch over the shared backend, with the session as the token provider: its
/// own ApiClient and InterruptBridge, both created and joined inside fetch_follows_collection.
[[nodiscard]] AuthResult<FollowsCollection> fetch_collection(const FollowsSettings& settings,
                                                             const Backend& backend,
                                                             StderrTrace& trace,
                                                             TokenProvider& provider) {
    // Assigned member by member, not with designated initializers: the clang-analyzer leak false
    // positive with std::function members explained in run_title (cmd_download_run.cpp).
    // Every pointer is borrowed for the call: `backend` and `trace` are the caller's.
    FollowsFetchServices services{};
    services.transport = backend.transport.get();
    services.clock = backend.clock;
    services.jitter = make_jitter();
    services.trace = &trace;
    return fetch_follows_collection(settings.batch.base.api_base, services, provider,
                                    settings.filter.has_value());
}

/// `names` joined with ", " (the --status words, API spelling, in the order given).
[[nodiscard]] std::string join_names(const std::vector<std::string>& names) {
    std::string joined{};
    for (const std::string& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

/// -v: `follows: <j> titles have a reading status mangadex-dl does not know ('<a>', '<b>' and K
/// more) and are left out`. At most max_unknown_statuses_shown values (in the map's order), so a
/// hostile status map can't flood stderr; the values are the parser's (validated to
/// `[a-z_]{1,32}`), and print_note makes the line terminal_safe anyway.
[[nodiscard]] std::string unknown_statuses_note(const FilteredFollows& filtered) {
    std::size_t titles = 0;
    std::string values{};
    std::size_t shown = 0;
    for (const auto& [value, count] : filtered.unknown) {
        titles += count;
        if (shown == max_unknown_statuses_shown) {
            continue;
        }
        if (shown > 0) {
            values += ", ";
        }
        values += std::format("'{}'", value);
        ++shown;
    }
    if (filtered.unknown.size() > shown) {
        values += std::format(" and {} more", filtered.unknown.size() - shown);
    }
    const bool one = titles == 1;
    return std::format("follows: {} {} a reading status mangadex-dl does not know ({}) and {} left "
                       "out",
                       titles, one ? "title has" : "titles have", values, one ? "is" : "are");
}

/// -v: `follows: <n> titles`, then with --status `follows: <k> of <n> have the reading status
/// <statuses>` and the unknown-status note.
void print_follows_lines(Io& io, const OutputPolicy& policy, std::size_t follows,
                         const std::optional<FilteredFollows>& filtered,
                         const std::optional<StatusFilter>& filter) {
    if (policy.verbose < 1) {
        return;
    }
    print_note(io, policy,
               std::format("follows: {} {}", follows, follows == 1 ? "title" : "titles"));
    if (!filtered.has_value() || !filter.has_value() || follows == 0) {
        return; // without a follow there was nothing to filter (and no status request)
    }
    print_note(io, policy,
               std::format("follows: {} of {} have the reading status {}", filtered->titles.size(),
                           follows, join_names(filter->names())));
    if (!filtered->unknown.empty()) {
        print_note(io, policy, unknown_statuses_note(*filtered));
    }
}

/// Everything after the backend exists. `backend` and `agent` are the caller's and outlive the
/// call.
[[nodiscard]] int run_with_backend(const FollowsSettings& settings, const OutputPolicy& policy,
                                   Io& io, const Backend& backend, const std::string& agent) {
    // Borrowed by the session (declared before it, so it outlives it) and by the fetch.
    StderrTrace trace{io, policy};

    // The login phase: it prints its own warnings and failures (8, 9, 1, 2, 3, or nothing on an
    // interrupt) and never writes stdout.
    std::expected<LoginPhase, int> login =
        run_login_phase(settings.auth, settings.batch.base.api_base, LoginMode::reuse, io, policy,
                        backend, trace, follows_command);
    if (!login) {
        return login.error();
    }
    LoginPhase& phase = *login;
    if (phase.session == nullptr) { // run_login_phase's contract: never null on success
        print_error(io, policy, "internal error: the login phase returned no session");
        return exit_code::failure;
    }

    // The only authorized requests. Then the session is closed at once, on every path (success,
    // failure, interrupt): the late warnings first, then the secrets wiped and the lock released.
    const AuthResult<FollowsCollection> collection =
        fetch_collection(settings, backend, trace, *phase.session);
    close_login_phase(phase, io, policy);
    if (!collection) {
        // One line, no JSON, DIR untouched: an interrupt prints nothing; a request failure gets
        // the network remedy; the documents' own problems (malformed, over a cap) and the
        // >3-grants refusal are self-contained; the API's second 401 -> 9; a session that can't
        // refresh and can't ask -> 8.
        return report_auth_failure(io, policy, collection.error(), follows_command);
    }

    // The --status filter (pure). Without a follow there is no status map: nothing is kept.
    const std::vector<MangaId>& follows = collection->follows;
    std::optional<FilteredFollows> filtered{};
    if (settings.filter.has_value()) {
        const StatusMap no_statuses{};
        const StatusMap& statuses =
            collection->statuses.has_value() ? *collection->statuses : no_statuses;
        filtered = filter_follows(follows, statuses, *settings.filter);
    }
    const std::span<const MangaId> selected = filtered.has_value()
                                                  ? std::span<const MangaId>{filtered->titles}
                                                  : std::span<const MangaId>{follows};
    print_follows_lines(io, policy, follows.size(), filtered, settings.filter);

    // `jobs` is not changed while run_batch_jobs borrows it. `selected` counts the kept follows,
    // duplicates included (each is an item).
    const std::vector<BatchJob> jobs = plan_follows(selected, settings);
    const FollowsSummary summary{
        .username = phase.notes.username,
        .titles = follows.size(),
        .statuses = settings.filter.has_value()
                        ? std::optional<std::vector<std::string>>{settings.filter->names()}
                        : std::nullopt,
        .selected = jobs.size(),
    };

    // Nothing to run: a warning (not with -q), the document, exit 0. DIR is not touched.
    if (jobs.empty()) {
        if (follows.empty()) {
            print_warning(io, policy, "you follow no titles on MangaDex");
        } else if (settings.filter.has_value()) {
            print_warning(io, policy,
                          std::format("none of the titles you follow have the reading status {}",
                                      join_names(settings.filter->names())));
        }
        if (policy.json) {
            std::print(io.out, "{}",
                       batch_to_json(BatchResult{}, settings.batch.file,
                                     settings.batch.base.dry_run, summary));
        }
        return exit_code::success;
    }

    // Every item, anonymously, through the runner mdlist uses, with follows' wording and the
    // `follows` object in the document.
    return run_batch_jobs(jobs, settings.batch, backend, agent, io, policy, summary);
}

/// run_follows_command with or without an injected backend (`injected` null: make_backend).
[[nodiscard]] int run_follows(const RawFollowsArgs& args, const GlobalOptions& globals, Io& io,
                              const Backend* injected) {
    // Phase 1 (pure): no file descriptor, no request and no directory before the arguments are
    // valid. `settings` owns copies of everything it keeps from `args`. The API base is checked
    // here too (the login phase checks it again), so a custom one never gets a backend.
    const OutputPolicy policy = output_policy(globals, io);
    const Result<FollowsSettings> settings = validate_follows_args(args);
    if (!settings) {
        return usage_failure(io, settings.error());
    }
    if (const Result<void> base =
            check_auth_api_base(settings->batch.base.api_base, follows_command);
        !base) {
        return usage_failure(io, base.error());
    }

    // Phase 2. Restores the previous SIGINT/SIGTERM dispositions when this returns; main() then
    // re-raises a pending signal after finish_stdout(). One install covers the login, the
    // collection fetch and every item.
    const Result<ScopedInterruptHandlers> handlers = ScopedInterruptHandlers::install();
    if (!handlers) {
        print_error(io, policy, with_remedy(handlers.error()));
        return handlers.error().exit_code();
    }
    // Before any secret is read (the login phase calls it again; harmless).
    guard_secrets();

    // One Backend for the login, the collection and every item.
    const std::string agent = user_agent();
    std::optional<Backend> owned{};
    const Backend* backend = injected;
    if (backend == nullptr) {
        Result<Backend> made = make_backend(settings->batch.base.api_base, agent);
        if (!made) {
            print_error(io, policy, with_remedy(made.error()));
            return made.error().exit_code();
        }
        backend = &owned.emplace(std::move(*made));
    }
    return run_with_backend(*settings, policy, io, *backend, agent);
}

class FollowsCommand final : public Command {
public:
    /// Binds options to members of this object (never to locals of register_follows).
    void bind(CLI::App& sub) {
        credentials_ = bind_follows_options(sub, args_);
        sub.footer(follows_help_footer());
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        // After parse(), before anything reads the arguments; the CLI::App is still alive.
        if (credentials_ != nullptr) {
            record_auth_options(args_.auth, *credentials_);
        }
        return run_follows_command(args_, globals, io);
    }

private:
    RawFollowsArgs args_{};
    /// Borrowed from the CLI::App bind() was given. cli.cpp's run() destroys that app only after
    /// dispatch has returned, and this pointer is read only inside run() above.
    const CLI::Option* credentials_ = nullptr;
};

} // namespace

int run_follows_command(const RawFollowsArgs& args, const GlobalOptions& globals, Io& io) {
    return run_follows(args, globals, io, nullptr);
}

int run_follows_command(const RawFollowsArgs& args, const GlobalOptions& globals, Io& io,
                        const Backend& backend) {
    return run_follows(args, globals, io, &backend);
}

std::string follows_help_footer() { return std::string{follows_footer} + exit_status_footer(); }

Result<RawFollowsArgs> parse_follows_args_for_testing(std::span<const std::string> args) {
    std::vector<const char*> argv{};
    argv.reserve(args.size() + 1);
    argv.push_back("follows");
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    if (!std::in_range<int>(argv.size())) {
        return std::unexpected(Error{.code = Errc::usage, .message = "too many arguments"});
    }
    // `raw` and `app` are locals that outlive parse(); the Option* is borrowed from `app`.
    RawFollowsArgs raw{};
    CLI::App app{"follows options (tests only)"};
    const CLI::Option* const credentials = bind_follows_options(app, raw);
    if (credentials == nullptr) { // CLI11 throws rather than return null; never dereference one
        return std::unexpected(
            Error{.code = Errc::failure, .message = "internal error: no --credentials option"});
    }
    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& error) {
        return std::unexpected(Error{.code = Errc::usage, .message = error.what()});
    }
    record_auth_options(raw.auth, *credentials);
    return raw;
}

std::unique_ptr<Command> register_follows(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<FollowsCommand>();
    CLI::App* sub = parent.add_subcommand(
        "follows", "Download every title you follow on MangaDex (needs a login)");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in the caller's
    // invocation and `self` is owned by the caller's command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace mangadex_dl::app
