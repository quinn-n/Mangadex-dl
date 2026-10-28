// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_batch.hpp"

#include "cmd_batch_jobs.hpp"
#include "cmd_download_options.hpp"
#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/fs/bounded_read.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl::app {
namespace {

/// The `batch --help` footer, before the exit-status table. Kept aligned (and within 80 columns):
/// the root formatter has footer formatting off.
constexpr std::string_view batch_footer{
    "CSV file:\n"
    "  UTF-8, comma-separated, \"double quotes\" around fields that contain commas.\n"
    "  Blank lines and lines starting with '#' are ignored. Without a header row,\n"
    "  each line is one title (UUID or https://mangadex.org/title/<uuid>). With a\n"
    "  header row, these columns are recognised (any order; an empty cell means\n"
    "  \"use the command-line value\"):\n"
    "    manga            the title, as MANGA in 'download' (required)\n"
    "    title            --title for this row\n"
    "    dir              subdirectory of DIR for this row\n"
    "    volumes, loose-chapters, layout, lang, group, direction\n"
    "                     as the options; group lists NAME|NAME in priority order\n"
    "    note             ignored\n"
    "  For example:\n"
    "    manga,title,dir\n"
    "    https://mangadex.org/title/<uuid>,,\n"
    "    <uuid>,My Title,series/my-title\n"
    "\n"
    "For a MangaDex list (MDList) instead of a file, see 'mangadex-dl mdlist --help'.\n"
    "\n"
    "Examples:\n"
    "  Everything in the list, into ~/Manga:\n"
    "    mangadex-dl batch titles.csv ~/Manga\n"
    "  Show what would be written, without downloading anything:\n"
    "    mangadex-dl batch titles.csv ~/Manga --dry-run\n"
    "  A list from another command:\n"
    "    grep -v '^#done' titles.csv | mangadex-dl batch - ~/Manga\n"
    "\n"
    "Running the same command again skips files that already hold every chapter\n"
    "and rebuilds a file when MangaDex has added chapters to it, downloading only\n"
    "the new ones, so an interrupted batch can be restarted and a list of series\n"
    "kept up to date.\n"
    "\n"
    "Symlinks inside DIR are followed: don't point DIR at a directory that others\n"
    "can write to. Each title costs several API requests: a long list is a long run.\n"
    "\n"};

/// One usage error (a single message) plus the hint: exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error) {
    print_usage_line(io, error.message);
    return usage_hint(io, "batch");
}

/// Every list diagnostic, then "... and <n> more errors" when some were not kept, then the hint.
[[nodiscard]] int list_failure(Io& io, const ListErrors& errors) {
    for (const std::string& message : errors.messages) {
        print_usage_line(io, message);
    }
    if (errors.more > 0) {
        print_usage_line(io, std::format("... and {} more {}", errors.more,
                                         errors.more == 1 ? "error" : "errors"));
    }
    return usage_hint(io, "batch");
}

/// The list's bytes: FILE through read_bounded_file, or "-" through read_bounded_fd on
/// io.stdin_fd (borrowed: never closed, its flags never changed). Opens nothing else.
[[nodiscard]] Result<std::string> read_list(const BatchSettings& settings, Io& io,
                                            const OutputPolicy& policy) {
    if (settings.file == "-") {
        if (io.stdin_tty && io.stderr_tty && !policy.quiet) {
            std::print(io.err, "{}: reading titles from the terminal; end with Ctrl-D\n",
                       tool_name);
        }
        return read_bounded_fd(io.stdin_fd, settings.label, max_list_bytes);
    }
    return read_bounded_file(std::filesystem::path{settings.file}, settings.label, max_list_bytes);
}

class BatchCommand final : public Command {
public:
    /// Binds options to members of this object (never to locals of register_batch).
    void bind(CLI::App& sub) {
        // Help text: each description fits the 44-column right column on one line, as in
        // `download`. Positionals carry no type name: FILE and DIR already say what they are.
        // FILE is not CLI::ExistingFile: "-" means stdin, and the read reports a missing file.
        sub.add_option("FILE", args_.file, "CSV file listing titles ('-' reads stdin)")
            ->type_name("")
            ->required();
        sub.add_option("DIR", args_.defaults.dir, "Output directory (created if missing)")
            ->type_name("")
            ->capture_default_str();
        bind_selection_options(sub, args_.defaults);
        bind_run_options(sub, args_.defaults);
        sub.add_flag("--fail-fast", args_.fail_fast, "Stop at the first title that fails");
        sub.footer(batch_help_footer());
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        return run_batch_command(args_, globals, io);
    }

private:
    RawBatchArgs args_{};
};

} // namespace

int run_batch_command(const RawBatchArgs& args, const GlobalOptions& globals, Io& io) {
    // Phases 1-5 open no file descriptor before the list is read (the fd-0 guard): no interrupt
    // handlers, no backend, no output directory, no fixture root and no row-dir check.
    const OutputPolicy policy = output_policy(globals, io);
    const Result<BatchSettings> settings = validate_batch_args(args);
    if (!settings) {
        return usage_failure(io, settings.error());
    }

    // `bytes` owns the list for the whole of phase 1; parse_title_list copies what it keeps.
    const Result<std::string> bytes = read_list(*settings, io, policy);
    if (!bytes) {
        if (bytes.error().code == Errc::usage) {
            return usage_failure(io, bytes.error()); // over the size cap
        }
        print_error(io, policy, bytes.error().message);
        return bytes.error().exit_code();
    }
    const ListResult<TitleList> list = parse_title_list(*bytes, settings->label);
    if (!list) {
        return list_failure(io, list.error());
    }
    const ListResult<std::vector<BatchJob>> jobs = plan_batch(*list, *settings);
    if (!jobs) {
        return list_failure(io, jobs.error());
    }

    const bool dry_run = settings->base.dry_run;
    if (jobs->empty()) {
        // Nothing to do: no handlers, no backend and no filesystem access.
        print_warning(io, policy, std::format("{}: lists no titles", settings->label));
        if (policy.json) {
            std::print(io.out, "{}", batch_to_json(BatchResult{}, settings->file, dry_run));
        }
        return exit_code::success;
    }

    // Restores the previous SIGINT/SIGTERM dispositions when this returns; main() then re-raises
    // a pending signal after finish_stdout().
    const Result<ScopedInterruptHandlers> handlers = ScopedInterruptHandlers::install();
    if (!handlers) {
        print_error(io, policy, with_remedy(handlers.error()));
        return handlers.error().exit_code();
    }
    // Lifetime: declared in borrow order (agent -> backend), after the handlers, so each lives
    // until run_batch_jobs below has returned. One Backend for every row.
    const std::string agent = user_agent();
    const Result<Backend> backend = make_backend(settings->base.api_base, agent);
    if (!backend) {
        print_error(io, policy, with_remedy(backend.error()));
        return backend.error().exit_code();
    }
    return run_batch_jobs(*jobs, *settings, *backend, agent, io, policy);
}

std::string batch_help_footer() { return std::string{batch_footer} + exit_status_footer(); }

std::unique_ptr<Command> register_batch(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<BatchCommand>();
    CLI::App* sub = parent.add_subcommand("batch", "Download every title listed in a CSV file");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in run()'s
    // Invocation and `self` is owned by run()'s command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace mangadex_dl::app
