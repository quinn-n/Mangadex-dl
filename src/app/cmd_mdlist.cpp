// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_mdlist.hpp"

#include "cmd_batch_jobs.hpp"
#include "cmd_download_options.hpp"
#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl::app {
namespace {

/// The `mdlist --help` footer, before the exit-status table. Kept aligned (and within 80
/// columns): the root formatter has footer formatting off.
constexpr std::string_view mdlist_footer{
    "LIST:\n"
    "  A public MangaDex custom list (MDList): its UUID, or its address on the\n"
    "  website, https://mangadex.org/list/<uuid>/<name> (http://, www., no\n"
    "  scheme or a trailing / are fine). No login is used, so the list must be\n"
    "  public. Every title in it is downloaded into DIR, one after another in\n"
    "  the list's order, with the options above. With --dir-per-title, each\n"
    "  title goes into its own subdirectory, named like its files:\n"
    "  DIR/<title>/<title> - Vol. 01.epub.\n"
    "\n"
    "  To download the titles you follow, add them to a public MDList.\n"
    "\n"
    "Examples:\n"
    "  Every title in a list, into ~/Manga:\n"
    "    mangadex-dl mdlist https://mangadex.org/list/<uuid> ~/Manga\n"
    "  The same, one folder per title:\n"
    "    mangadex-dl mdlist <uuid> ~/Manga --dir-per-title\n"
    "  Show what would be written, without downloading anything:\n"
    "    mangadex-dl mdlist <uuid> ~/Manga --dry-run\n"
    "  Volumes 1 to 3 of each title, in French, as one JSON document:\n"
    "    mangadex-dl mdlist <uuid> ~/Manga --volumes 1-3 --lang fr --json\n"
    "\n"
    "Running the same command again skips files that already hold every chapter\n"
    "and rebuilds a file when MangaDex has added chapters to it, downloading only\n"
    "the new ones; titles added to the list since the last run are downloaded too.\n"
    "\n"
    "Symlinks inside DIR are followed: don't point DIR at a directory that others\n"
    "can write to. Each title costs several API requests: a long list is a long run.\n"
    "\n"};

/// The -vv request trace of the list fetch: `mangadex-dl: <line>` on stderr, terminal_safe and
/// uncolored, exactly like the engine's own trace lines. Silent unless -vv without -q.
///
/// Threads: TraceSink requires trace() to be thread-safe, so every line is written under the
/// mutex (the list fetch itself calls it on the calling thread only: its client has no reporter).
/// Lifetime: `io` is borrowed (the caller's, it outlives the sink); fetch_custom_list keeps no
/// pointer to the sink past its return.
class StderrTrace final : public TraceSink {
public:
    StderrTrace(Io& io, const OutputPolicy& policy) noexcept
        : io_(&io), enabled_(policy.verbose >= 2 && !policy.quiet) {}

    void trace(std::string line) override {
        if (!enabled_) {
            return;
        }
        const std::scoped_lock lock{mutex_};
        std::print(io_->err, "{}: {}\n", tool_name, terminal_safe(line));
    }

private:
    Io* io_ = nullptr; ///< borrowed, never null
    bool enabled_ = false;
    std::mutex mutex_{};
};

/// One usage error plus the hint: exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error) {
    print_usage_line(io, error.message);
    return usage_hint(io, "mdlist");
}

/// The list fetch over the shared backend: its own ApiClient and InterruptBridge, both created and
/// joined inside fetch_custom_list, before the first item starts.
[[nodiscard]] ListFetchResult<CustomList> fetch_list(const MdlistSettings& settings,
                                                     const Backend& backend, StderrTrace& trace) {
    // Assigned member by member, not with designated initializers: the clang-analyzer leak false
    // positive with std::function members explained in run_title (cmd_download_run.cpp).
    // Every pointer is borrowed for the call: `backend` and `trace` are the caller's.
    ListFetchServices services{};
    services.transport = backend.transport.get();
    services.clock = backend.clock;
    services.jitter = make_jitter();
    services.trace = &trace;
    return fetch_custom_list(settings.list, settings.batch.base.api_base, services);
}

/// A failure of the list itself: one line, no JSON. A request failure (the transport, an HTTP
/// status) gets the network remedy on exit 3; the document's own problems (malformed, over the
/// title cap) are self-contained and get none (list plan, Spec issue 1). An interrupt prints
/// nothing: main() re-raises the signal.
[[nodiscard]] int list_failure(Io& io, const OutputPolicy& policy, const ListFetchError& failure) {
    const Error& error = failure.error;
    if (error.code == Errc::interrupted) {
        return error.exit_code();
    }
    switch (failure.kind) {
    case ListFailureKind::request:
        print_error(io, policy, with_remedy(error));
        return error.exit_code();
    case ListFailureKind::content:
        print_error(io, policy, error.message);
        return error.exit_code();
    }
    print_error(io, policy, error.message); // only reachable through an out-of-range enum value
    return error.exit_code();
}

/// -v: `list <id> "<name>": <n> titles`, or `list <id>: <n> titles` for a list without a name.
/// The name is display-only and untrusted: it is cut for display, is a format argument (never the
/// format string), and print_note makes the whole line terminal_safe.
void print_list_line(Io& io, const OutputPolicy& policy, const CustomList& list) {
    if (policy.verbose < 1) {
        return;
    }
    const std::size_t count = list.titles.size();
    const std::string_view noun = count == 1 ? "title" : "titles";
    if (list.name) {
        print_note(io, policy,
                   std::format("list {} \"{}\": {} {}", list.id.str(),
                               list_name_for_display(*list.name), count, noun));
        return;
    }
    print_note(io, policy, std::format("list {}: {} {}", list.id.str(), count, noun));
}

class MdlistCommand final : public Command {
public:
    /// Binds options to members of this object (never to locals of register_mdlist). The shared
    /// binders bind into `args_.defaults`, the member, so every option `batch` offers has the
    /// same name, validator, default and help string here.
    void bind(CLI::App& sub) {
        // Help text: each description fits the 44-column right column on one line. LIST is not
        // CLI::ExistingFile: it is validated after parsing (ListId::parse) and never echoed.
        sub.add_option("LIST", args_.list, "Public MDList: its UUID or list URL")
            ->type_name("")
            ->required();
        sub.add_option("DIR", args_.defaults.dir, "Output directory (created if missing)")
            ->type_name("")
            ->capture_default_str();
        bind_selection_options(sub, args_.defaults);
        bind_run_options(sub, args_.defaults);
        sub.add_flag("--fail-fast", args_.fail_fast, "Stop at the first title that fails");
        sub.add_flag("--dir-per-title", args_.dir_per_title,
                     "Put each title in a subdirectory of DIR");
        sub.footer(mdlist_help_footer());
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        return run_mdlist_command(args_, globals, io);
    }

private:
    RawMdlistArgs args_{};
};

} // namespace

int run_mdlist_command(const RawMdlistArgs& args, const GlobalOptions& globals, Io& io) {
    // Phase 1 (pure): no file descriptor, no request and no directory before the arguments are
    // valid. `settings` owns copies of everything it keeps from `args`.
    const OutputPolicy policy = output_policy(globals, io);
    const Result<MdlistSettings> settings = validate_mdlist_args(args);
    if (!settings) {
        return usage_failure(io, settings.error());
    }

    // Phase 2. Restores the previous SIGINT/SIGTERM dispositions when this returns; main() then
    // re-raises a pending signal after finish_stdout(). One install covers the list fetch and
    // every item.
    const Result<ScopedInterruptHandlers> handlers = ScopedInterruptHandlers::install();
    if (!handlers) {
        print_error(io, policy, with_remedy(handlers.error()));
        return handlers.error().exit_code();
    }
    // Lifetime: declared in borrow order (agent -> backend -> trace), after the handlers, so each
    // lives until run_batch_jobs below has returned. One Backend for the list fetch and every item.
    const std::string agent = user_agent();
    const Result<Backend> backend = make_backend(settings->batch.base.api_base, agent);
    if (!backend) {
        print_error(io, policy, with_remedy(backend.error()));
        return backend.error().exit_code();
    }
    StderrTrace trace{io, policy};

    // Phase 3: one request. The fetch's client and interrupt bridge are joined inside the call, so
    // its poller is gone before any item's own bridge starts.
    const ListFetchResult<CustomList> list = fetch_list(*settings, *backend, trace);
    if (!list) {
        return list_failure(io, policy, list.error());
    }
    print_list_line(io, policy, *list);

    // Phase 4 (pure). `jobs` is not changed while run_batch_jobs borrows it.
    const std::vector<BatchJob> jobs = plan_list(*list, *settings);
    const std::optional<ListSummary> summary{list_summary(*list)};
    const bool dry_run = settings->batch.base.dry_run;

    // Phase 5: nothing to run. DIR is not touched.
    if (jobs.empty()) {
        print_warning(io, policy, std::format("list {} has no titles", list->id.str()));
        if (policy.json) {
            std::print(io.out, "{}",
                       batch_to_json(BatchResult{}, settings->batch.file, dry_run, summary));
        }
        return exit_code::success;
    }

    // Phases 6 and 7: the runner `batch` uses, with mdlist's wording (settings.batch.source) and
    // the `list` object in the document.
    return run_batch_jobs(jobs, settings->batch, *backend, agent, io, policy, summary);
}

std::string mdlist_help_footer() { return std::string{mdlist_footer} + exit_status_footer(); }

std::unique_ptr<Command> register_mdlist(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<MdlistCommand>();
    CLI::App* sub =
        parent.add_subcommand("mdlist", "Download every title in a public MangaDex list (MDList)");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in run()'s
    // Invocation and `self` is owned by run()'s command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace mangadex_dl::app
