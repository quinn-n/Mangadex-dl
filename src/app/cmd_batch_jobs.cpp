// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_batch_jobs.hpp"

#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "io.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/fs/row_dir.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/title_dir.hpp"
#include "mangadex_dl/version.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl::app {
namespace {

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
    case Errc::row_dir: // mdlist --dir-per-title only (batch never produces it): the row fails
        return false;
    }
    return true; // only reachable through an out-of-range enum value
}

/// What one row is called in row-scoped messages: "line" (a CSV line) or "item" (a list
/// position). Points at static storage.
[[nodiscard]] constexpr std::string_view row_noun(BatchSource source) noexcept {
    switch (source) {
    case BatchSource::csv:
        return "line";
    case BatchSource::mdlist:
        return "item";
    }
    return "line"; // only reachable through an out-of-range enum value
}

/// The app side of the TitleDirResolver seam (mdlist --dir-per-title): claims each title's
/// directory in the run's TitleDirRegistry for the current item, and remembers the name it
/// claimed so the runner can report it as the row's `dir`.
///
/// Threads: run_download calls resolve() on its calling thread (the runner's), at most once per
/// row, so no lock is needed. Lifetime: `registry` is borrowed; it is declared before the runner
/// that owns this resolver in run_batch_jobs's frame, so it outlives it. Neither copyable nor
/// movable (the base deletes both).
class ConsoleTitleDirResolver final : public TitleDirResolver {
public:
    explicit ConsoleTitleDirResolver(TitleDirRegistry& registry) noexcept : registry_(&registry) {}

    /// Starts row `item`: forgets the previous row's claim, so a row whose run ends before its
    /// plan (exit 3 or 4: resolve() is never called) reports no directory of its own.
    void begin(std::size_t item) noexcept {
        item_ = item;
        claimed_.reset();
    }

    [[nodiscard]] TitleDirChoice resolve(std::string_view stem, const MangaId& manga) override {
        // The registry copies `stem` into owned keys; the claim and the choice own their strings.
        TitleDirClaim claim = registry_->claim(stem, manga, item_);
        TitleDirChoice choice{.name = claim.name};
        if (claim.taken_by) {
            // Raw: the observer's on_warning adds `warning: <prefix>` and sanitizes the line.
            choice.warning = std::format("directory '{}' already holds item {} (a title with the "
                                         "same name); using '{}'",
                                         stem, *claim.taken_by, claim.name);
        }
        claimed_ = std::move(claim.name);
        return choice;
    }

    /// The name resolve() claimed since the last begin(), if it was called.
    [[nodiscard]] const std::optional<std::string>&
    claimed() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return claimed_;
    }

private:
    TitleDirRegistry* registry_ = nullptr; ///< borrowed, never null
    std::size_t item_ = 0;                 ///< the current row's line (its list position)
    std::optional<std::string> claimed_{};
};

/// The app side of the run_batch seam: one row = the read-only directory pre-flight, then
/// run_title over the shared Backend with a row-prefixed observer (and, for a --dir-per-title
/// row, the directory resolver), then the cross-row collision check. Prints on io's streams;
/// never prints JSON.
///
/// Lifetime: every pointer is borrowed. `registry` and `dirs` are declared before this runner in
/// run_batch_jobs's frame; the rest come from run_batch_jobs's caller, which keeps them alive
/// for the whole call. So all of them outlive the runner. `resolver_` is a member: run_title
/// borrows it for one row only. Neither copyable nor movable (the base deletes both).
class ConsoleTitleRunner final : public TitleRunner {
public:
    ConsoleTitleRunner(const BatchSettings& settings, const Backend& backend,
                       const std::string& agent, Io& io, const OutputPolicy& policy,
                       OutputRegistry& registry, TitleDirRegistry& dirs) noexcept
        : settings_(&settings), backend_(&backend), agent_(&agent), io_(&io), policy_(&policy),
          registry_(&registry), resolver_(dirs) {}

    [[nodiscard]] RowOutcome run(const BatchJob& job, std::size_t position,
                                 std::size_t total) override {
        const std::string prefix = row_prefix(job);
        if (policy_->verbose >= 1 && !policy_->quiet) {
            std::print(io_->err, "{}: {}\n", tool_name,
                       terminal_safe(std::format("{}:{}: {}", settings_->label, job.line,
                                                 job.options.manga.str())));
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

        // --dir-per-title: the engine asks the resolver for this row's directory after the plan.
        const bool per_title = job.options.dir_per_title;
        TitleDirResolver* title_dir = nullptr;
        if (per_title) {
            resolver_.begin(job.line);
            title_dir = &resolver_;
        }

        // The observer copies the RowContext, prefix included (nothing borrows `prefix`).
        TitleRun run = run_title(
            job.options, job.raw, *backend_, *agent_, *io_, *policy_,
            RowContext{
                .prefix = prefix, .position = position, .total = total, .exists_notice = false},
            title_dir);

        RowOutcome outcome{.exit_code = run.exit_code, .error = std::move(run.closing_error)};
        if (per_title) {
            // Set once the engine claimed a name, also when the pre-flight then failed the row
            // (row_dir); empty (the job's DIR) for a row that ended before its plan.
            if (const std::optional<std::string>& name = resolver_.claimed()) {
                outcome.dir = join_list_dir(job.dir, *name);
            }
        }
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
                      std::format("{}same as {} {}; skipped", row_prefix(job),
                                  row_noun(settings_->source), job.duplicate_of.value_or(0)));
    }

    /// Files left alone across every row so far because they hold every chapter: the count for
    /// the single end-of-batch note.
    [[nodiscard]] std::size_t up_to_date() const noexcept { return up_to_date_; }

    [[nodiscard]] bool output_failed() override {
        // finish_stdout reports the reason: std::cout's RecordingStdioBuf keeps the errno of the
        // first failed write or flush, this one included.
        io_->out.flush();
        return !io_->out;
    }

private:
    /// csv "<label>:<line>: "; mdlist "<label>:<line>: <uuid>: ", because the user wrote no file
    /// to look a position up in (raw: print_error/print_warning sanitize it with the message).
    [[nodiscard]] std::string row_prefix(const BatchJob& job) const {
        switch (settings_->source) {
        case BatchSource::csv:
            break;
        case BatchSource::mdlist:
            return std::format("{}:{}: {}: ", settings_->label, job.line, job.options.manga.str());
        }
        return std::format("{}:{}: ", settings_->label, job.line);
    }

    /// The collision line for `path`, which line/item `earlier` already claimed.
    [[nodiscard]] std::string collision_message(const std::string& prefix, const std::string& path,
                                                std::size_t earlier) const {
        switch (settings_->source) {
        case BatchSource::csv:
            break;
        case BatchSource::mdlist:
            return std::format("{}{} was already written by item {}; download one of them on its "
                               "own with '{} download <title> --title ...'",
                               prefix, path, earlier, tool_name);
        }
        return std::format("{}{} was already written by line {}; give one of them a 'title' column",
                           prefix, path, earlier);
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
                    print_warning(*io_, *policy_, collision_message(prefix, file.path, *earlier));
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
    /// No default constructor (it needs the registry): initialized by the constructor.
    ConsoleTitleDirResolver resolver_;
    std::size_t up_to_date_ = 0;
};

} // namespace

void print_usage_line(Io& io, std::string_view message) {
    std::print(io.err, "{}: error: {}\n", tool_name, terminal_safe(message));
}

int usage_hint(Io& io, std::string_view command) {
    std::print(io.err, "Run '{} {} --help' for more information.\n", tool_name, command);
    return exit_code::usage;
}

void print_warning(Io& io, const OutputPolicy& policy, std::string_view message) {
    if (policy.quiet) {
        return;
    }
    std::print(io.err, "{}: {} {}\n", tool_name,
               paint("warning:", Style::yellow, policy.color_stderr), terminal_safe(message));
}

void print_note(Io& io, const OutputPolicy& policy, std::string_view message) {
    if (policy.quiet) {
        return;
    }
    std::print(io.err, "{}: {}\n", tool_name, terminal_safe(message));
}

int run_batch_jobs(std::span<const BatchJob> jobs, const BatchSettings& settings,
                   const Backend& backend, const std::string& agent, Io& io,
                   const OutputPolicy& policy, const std::optional<ListSummary>& list) {
    const bool dry_run = settings.base.dry_run;
    // Lifetime: declared in borrow order (registry -> dirs -> runner), after the caller's
    // handlers, agent and backend, so each lives until run_batch below has returned. Each row's
    // DownloadServices, reporter, pool and bridge are created and joined inside run_title.
    OutputRegistry registry{};
    TitleDirRegistry dirs{};
    ConsoleTitleRunner runner{settings, backend, agent, io, policy, registry, dirs};
    const BatchResult result = run_batch(jobs, runner, settings.fail_fast);

    if (result.stop == BatchStop::interrupted) {
        // No JSON, no notice, no closing line: main() re-raises the signal (130/143).
        return batch_exit_code(result);
    }
    if (const std::optional<std::string> note = up_to_date_notice(runner.up_to_date())) {
        print_note(io, policy, *note); // a note, like download's: no `warning:`, never colored
    }
    if (const std::optional<std::string> closing =
            batch_closing_message(result, dry_run, settings.source)) {
        print_error(io, policy, *closing); // even with -q and --json
    }
    if (policy.json) {
        // Built whole, written once, never colored.
        std::print(io.out, "{}", batch_to_json(result, settings.file, dry_run, list));
    }
    return batch_exit_code(result);
}

} // namespace mangadex_dl::app
