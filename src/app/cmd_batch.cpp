// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_batch.hpp"

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
#include "mangadex_dl/fs/row_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>
#include <string_view>
#include <utility>
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

/// `mangadex-dl: error: <message>` for a usage error: uncolored, like CLI11's parse errors and
/// `download`'s semantic usage errors, so every exit-2 diagnostic looks the same.
void print_usage_line(Io& io, std::string_view message) {
    std::print(io.err, "{}: error: {}\n", tool_name, terminal_safe(message));
}

/// The hint line every usage error ends with; returns exit 2.
[[nodiscard]] int usage_hint(Io& io) {
    std::print(io.err, "Run '{} batch --help' for more information.\n", tool_name);
    return exit_code::usage;
}

/// One usage error (a single message) plus the hint: exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error) {
    print_usage_line(io, error.message);
    return usage_hint(io);
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
    return usage_hint(io);
}

/// `mangadex-dl: warning: <message>` (not with -q), the same line ConsoleObserver prints.
void print_warning(Io& io, const OutputPolicy& policy, std::string_view message) {
    if (policy.quiet) {
        return;
    }
    std::print(io.err, "{}: {} {}\n", tool_name,
               paint("warning:", Style::yellow, policy.color_stderr), terminal_safe(message));
}

/// `mangadex-dl: <message>` (not with -q, never colored), the same note line ConsoleObserver
/// prints.
void print_note(Io& io, const OutputPolicy& policy, std::string_view message) {
    if (policy.quiet) {
        return;
    }
    std::print(io.err, "{}: {}\n", tool_name, terminal_safe(message));
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

/// Is a local error from run_download global (the batch stops)? The pre-flight has already
/// turned the row-scoped directory cases into `failed` rows, so whatever is left of failure, io
/// and usage is treated as global: the conservative choice (batch plan rev1).
[[nodiscard]] constexpr bool is_fatal(Errc code) noexcept {
    switch (code) {
    case Errc::failure:
    case Errc::io:
    case Errc::usage:
        return true;
    case Errc::network:
    case Errc::not_found:
    case Errc::no_chapters:
    case Errc::incomplete:
    case Errc::interrupted:
    case Errc::mixed_failures:
        return false;
    }
    return true; // only reachable through an out-of-range enum value
}

/// The app side of the run_batch seam: one row = the read-only directory pre-flight, then
/// run_title over the shared Backend with a row-prefixed observer, then the cross-row collision
/// check. Prints on io's streams; never prints JSON.
///
/// Lifetime: every pointer is borrowed from run_batch_command's frame, where each object is
/// declared before this runner, so all of them outlive it. Neither copyable nor movable (the
/// base deletes both).
class ConsoleTitleRunner final : public TitleRunner {
public:
    ConsoleTitleRunner(const BatchSettings& settings, const Backend& backend,
                       const std::string& agent, Io& io, const OutputPolicy& policy,
                       OutputRegistry& registry) noexcept
        : settings_(&settings), backend_(&backend), agent_(&agent), io_(&io), policy_(&policy),
          registry_(&registry) {}

    [[nodiscard]] RowOutcome run(const BatchJob& job, std::size_t position,
                                 std::size_t total) override {
        const std::string prefix = row_prefix(job);
        if (policy_->verbose >= 1 && !policy_->quiet) {
            std::print(io_->err, "{}: {}\n", tool_name,
                       terminal_safe(std::format("{}{}", prefix, job.options.manga.str())));
        }

        // Read-only: decides whether a directory problem fails this row or stops the batch,
        // before the row downloads (or creates) anything.
        const RowDirVerdict dir =
            check_row_dir(std::filesystem::path{settings_->defaults.dir}, job.sub_dir);
        switch (dir.kind) {
        case RowDirKind::ok:
            break;
        case RowDirKind::row_error:
        case RowDirKind::global_error:
            print_error(*io_, *policy_, prefix + dir.message);
            return RowOutcome{.exit_code = exit_code::failure,
                              .error = dir.message,
                              .fatal = dir.kind == RowDirKind::global_error};
        }

        // The observer copies the RowContext, prefix included (nothing borrows `prefix`).
        TitleRun run = run_title(
            job.options, job.raw, *backend_, *agent_, *io_, *policy_,
            RowContext{
                .prefix = prefix, .position = position, .total = total, .exists_notice = false});

        RowOutcome outcome{.exit_code = run.exit_code, .error = std::move(run.closing_error)};
        // From the observer, not the report: a row that ends with an Error after leaving files
        // alone has no report, and download's own note would still count them.
        up_to_date_ += run.up_to_date;
        if (run.error) {
            outcome.interrupted = run.error->code == Errc::interrupted;
            outcome.fatal = is_fatal(run.error->code);
        }
        if (run.report) {
            warn_collisions(job, prefix, *run.report);
            outcome.files = summarize(*run.report);
            if (policy_->json) { // only --json needs the per-row document
                outcome.report = std::move(run.report);
            }
        }
        return outcome;
    }

    void on_duplicate(const BatchJob& job) override {
        print_warning(*io_, *policy_,
                      std::format("{}same as line {}; skipped", row_prefix(job),
                                  job.duplicate_of.value_or(0)));
    }

    /// Files left alone across every row so far because they hold every chapter: the count for
    /// the single end-of-batch note.
    [[nodiscard]] std::size_t up_to_date() const noexcept { return up_to_date_; }

    [[nodiscard]] bool output_failed() override {
        if (!io_->out) {
            return true; // an earlier write failed; its errno is long gone
        }
        errno = 0; // only report what this flush sets
        io_->out.flush();
        if (io_->out) {
            return false;
        }
        // The stream drops what it could not write, so finish_stdout's own flush at exit succeeds
        // and could only say "output error": hand it this flush's reason instead.
        note_stdout_errno(errno);
        return true;
    }

private:
    /// "<label>:<line>: " (raw: print_error/print_warning sanitize it with the message).
    [[nodiscard]] std::string row_prefix(const BatchJob& job) const {
        return std::format("{}:{}: ", settings_->label, job.line);
    }

    /// Every file this row produced (written, planned or already there) is claimed for its
    /// line; a path an earlier row claimed gets one warning. Failed files produced nothing.
    void warn_collisions(const BatchJob& job, const std::string& prefix, const RunReport& report) {
        for (const EpubOutcome& file : report.epubs) {
            switch (file.status) {
            case EpubStatus::planned:
            case EpubStatus::written:
            case EpubStatus::exists:
                if (const std::optional<std::size_t> earlier =
                        registry_->claim(file.path, job.line)) {
                    print_warning(*io_, *policy_,
                                  std::format("{}{} was already written by line {}; give one of "
                                              "them a 'title' column",
                                              prefix, file.path, *earlier));
                }
                break;
            case EpubStatus::failed:
                break;
            }
        }
    }

    const BatchSettings* settings_ = nullptr; ///< borrowed, never null
    const Backend* backend_ = nullptr;        ///< borrowed, never null; shared by every row
    const std::string* agent_ = nullptr;      ///< borrowed, never null
    Io* io_ = nullptr;                        ///< borrowed, never null
    const OutputPolicy* policy_ = nullptr;    ///< borrowed, never null
    OutputRegistry* registry_ = nullptr;      ///< borrowed, never null
    std::size_t up_to_date_ = 0;
};

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
    // Lifetime: declared in borrow order (agent -> backend -> registry -> runner), so each lives
    // until run_batch below has returned. One Backend for every row; each row's DownloadServices,
    // reporter, pool and bridge are created and joined inside run_title.
    const std::string agent = user_agent();
    const Result<Backend> backend = make_backend(settings->base.api_base, agent);
    if (!backend) {
        print_error(io, policy, with_remedy(backend.error()));
        return backend.error().exit_code();
    }
    OutputRegistry registry{};
    ConsoleTitleRunner runner{*settings, *backend, agent, io, policy, registry};
    const BatchResult result = run_batch(*jobs, runner, settings->fail_fast);

    if (result.stop == BatchStop::interrupted) {
        // No JSON, no notice, no closing line: main() re-raises the signal (130/143).
        return batch_exit_code(result);
    }
    if (const std::optional<std::string> note = up_to_date_notice(runner.up_to_date())) {
        print_note(io, policy, *note); // a note, like download's: no `warning:`, never colored
    }
    if (const std::optional<std::string> closing = batch_closing_message(result, dry_run)) {
        print_error(io, policy, *closing); // even with -q and --json
    }
    if (policy.json) {
        // Built whole, written once, never colored.
        std::print(io.out, "{}", batch_to_json(result, settings->file, dry_run));
    }
    return batch_exit_code(result);
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
