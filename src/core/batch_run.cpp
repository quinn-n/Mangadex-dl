// SPDX-License-Identifier: GPL-3.0-or-later
//
// `batch`, phase 2: the row loop over an injected TitleRunner, its stop rules, the aggregate exit
// code, the closing message and cross-row collision detection (batch plan T3b; batch-spec.md
// "Exit codes" and "Input & output"). The list plan's T5 adds the source-aware wording (`line` for
// csv, `item` for mdlist) and the runner's effective dir (RowOutcome::dir).
//
// Threads: everything here runs on the calling thread. The runner's own threads (run_download's
// pool, bridge and reporter) start and are joined inside one runner.run() call.
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/interrupt.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// At most this many line (or item) numbers are listed in the closing message, then
/// " and <k> more".
constexpr std::size_t max_listed_lines = 10;

/// What a row is called in the closing message: one CSV line, or one list item.
struct RowNoun {
    std::string_view one{};
    std::string_view many{};
};

/// "line"/"lines" for csv, "item"/"items" for mdlist. Points at static storage.
[[nodiscard]] constexpr RowNoun row_noun(BatchSource source) noexcept {
    switch (source) {
    case BatchSource::csv:
        return RowNoun{.one = "line", .many = "lines"};
    case BatchSource::mdlist:
        return RowNoun{.one = "item", .many = "items"};
    }
    return RowNoun{.one = "line", .many = "lines"}; // only reachable through an out-of-range value
}

[[nodiscard]] bool is_duplicate(const BatchJob& job) noexcept {
    return job.duplicate_of.has_value();
}

/// True when a row the loop would still run (a non-duplicate) follows index `index`.
[[nodiscard]] bool runnable_row_after(std::span<const BatchJob> jobs, std::size_t index) noexcept {
    return std::ranges::any_of(jobs.subspan(index + 1),
                               [](const BatchJob& job) { return !is_duplicate(job); });
}

/// The result row for a job that does not run: `duplicate` (keeping its duplicate_of) or
/// `not_run`. Strings are copied from the job.
[[nodiscard]] TitleResult unrun_result(const BatchJob& job) {
    const bool duplicate = is_duplicate(job);
    return TitleResult{
        .line = job.line,
        .manga_id = std::string{job.options.manga.str()},
        .dir = job.dir,
        .status = duplicate ? TitleStatus::duplicate : TitleStatus::not_run,
        .duplicate_of = job.duplicate_of,
    };
}

/// The result row for a job the runner ran. `dir` is the runner's effective directory when it
/// reported one (--dir-per-title: DIR/<name>), else the job's.
[[nodiscard]] TitleResult run_result(const BatchJob& job, RowOutcome outcome) {
    TitleResult row{
        .line = job.line,
        .manga_id = std::string{job.options.manga.str()},
        .dir = std::move(outcome.dir),
        .status = title_status_for(outcome.exit_code),
        .exit_code = outcome.exit_code,
        .error = outcome.error.empty() ? std::nullopt
                                       : std::optional<std::string>{std::move(outcome.error)},
        .files = outcome.files,
        .report = std::move(outcome.report),
    };
    if (row.dir.empty()) {
        row.dir = job.dir;
    }
    return row;
}

/// Did not complete: every status that feeds the exit code (not ok, duplicate or not_run).
[[nodiscard]] constexpr bool is_failure(TitleStatus status) noexcept {
    switch (status) {
    case TitleStatus::not_found:
    case TitleStatus::no_chapters:
    case TitleStatus::incomplete:
    case TitleStatus::network:
    case TitleStatus::failed:
        return true;
    case TitleStatus::ok:
    case TitleStatus::duplicate:
    case TitleStatus::not_run:
        return false;
    }
    return false; // only reachable through an out-of-range enum value
}

/// The exit code a failed status stands for in the uniform/mixed rule; 0 for the rest.
[[nodiscard]] constexpr int code_for(TitleStatus status) noexcept {
    switch (status) {
    case TitleStatus::network:
        return exit_code::network;
    case TitleStatus::not_found:
        return exit_code::not_found;
    case TitleStatus::no_chapters:
        return exit_code::no_chapters;
    case TitleStatus::incomplete:
        return exit_code::incomplete;
    case TitleStatus::failed:
        return exit_code::failure;
    case TitleStatus::ok:
    case TitleStatus::duplicate:
    case TitleStatus::not_run:
        return exit_code::success;
    }
    return exit_code::failure; // only reachable through an out-of-range enum value
}

/// Would running the same command again plausibly fix this row? Only a network error or an
/// incomplete title (some files failed) is worth a retry; a missing title, a list with nothing
/// to download or a local failure fails the same way the next time.
[[nodiscard]] constexpr bool is_retryable(TitleStatus status) noexcept {
    switch (status) {
    case TitleStatus::network:
    case TitleStatus::incomplete:
        return true;
    case TitleStatus::ok:
    case TitleStatus::not_found:
    case TitleStatus::no_chapters:
    case TitleStatus::failed:
    case TitleStatus::duplicate:
    case TitleStatus::not_run:
        return false;
    }
    return false; // only reachable through an out-of-range enum value
}

/// "<n> title" / "<n> titles".
[[nodiscard]] std::string titles_phrase(std::size_t n) {
    return std::format("{} {}", n, n == 1 ? "title" : "titles");
}

/// The early-stop reason, or nullopt for none/interrupted (which have no "stopped at" line).
[[nodiscard]] std::optional<std::string> stop_reason(BatchStop stop) {
    switch (stop) {
    case BatchStop::fail_fast:
        return std::string{"--fail-fast"};
    case BatchStop::local_failure:
        return std::string{"the error above affects every title"};
    case BatchStop::network_streak:
        return std::format("{} titles in a row failed with network errors", network_streak_limit);
    case BatchStop::output_error:
        return std::string{"cannot write to stdout"};
    case BatchStop::none:
    case BatchStop::interrupted:
        return std::nullopt;
    }
    return std::nullopt; // only reachable through an out-of-range enum value
}

/// "line 4" / "lines 4, 9, 12" / "lines 1, ..., 10 and 3 more" (or "item"/"items").
[[nodiscard]] std::string lines_phrase(const std::vector<std::size_t>& lines, RowNoun noun) {
    std::string text{lines.size() == 1 ? noun.one : noun.many};
    text += ' ';
    const std::size_t listed = std::min(lines.size(), max_listed_lines);
    for (std::size_t i = 0; i < listed; ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += std::to_string(lines[i]);
    }
    if (lines.size() > listed) {
        text += std::format(" and {} more", lines.size() - listed);
    }
    return text;
}

} // namespace

TitleStatus title_status_for(int exit_code) noexcept {
    switch (exit_code) {
    case exit_code::success:
        return TitleStatus::ok;
    case exit_code::network:
        return TitleStatus::network;
    case exit_code::not_found:
        return TitleStatus::not_found;
    case exit_code::no_chapters:
        return TitleStatus::no_chapters;
    case exit_code::incomplete:
        return TitleStatus::incomplete;
    default:
        return TitleStatus::failed;
    }
}

BatchResult run_batch(std::span<const BatchJob> jobs, TitleRunner& runner, bool fail_fast) {
    BatchResult result{};
    result.titles.reserve(jobs.size());
    const auto total = static_cast<std::size_t>(
        std::ranges::count_if(jobs, [](const BatchJob& job) { return !is_duplicate(job); }));

    std::size_t position = 0;
    std::size_t network_streak = 0;
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        const BatchJob& job = jobs[index];

        // A stop already recorded (or an interrupt between rows): the rest does not run. A
        // duplicate after a stop keeps its status, but its "skipped" warning is not printed.
        if (result.stop == BatchStop::none && interrupt_requested()) {
            result.stop = BatchStop::interrupted;
            result.stopped_at_line = job.line;
        }
        if (result.stop != BatchStop::none) {
            result.titles.push_back(unrun_result(job));
            continue;
        }

        if (is_duplicate(job)) {
            runner.on_duplicate(job);
            result.titles.push_back(unrun_result(job));
            continue; // neither counts toward nor resets the network streak
        }

        ++position;
        RowOutcome outcome = runner.run(job, position, total);
        const bool interrupted = outcome.interrupted;
        const bool fatal = outcome.fatal;
        TitleResult row = run_result(job, std::move(outcome));
        const TitleStatus status = row.status;

        if (interrupted) {
            // The row did not finish: it counts as not run (its files and report are kept).
            row.status = TitleStatus::not_run;
            row.exit_code = std::nullopt;
            row.error = std::nullopt;
            result.titles.push_back(std::move(row));
            result.stop = BatchStop::interrupted;
            result.stopped_at_line = job.line;
            continue;
        }
        result.titles.push_back(std::move(row));

        network_streak = status == TitleStatus::network ? network_streak + 1 : 0;

        // The first reason that applies, in the plan's order.
        BatchStop stop = BatchStop::none;
        if (fatal) {
            stop = BatchStop::local_failure;
        } else if (runner.output_failed()) {
            stop = BatchStop::output_error;
        } else if (fail_fast && status != TitleStatus::ok) {
            stop = BatchStop::fail_fast;
        } else if (network_streak >= network_streak_limit) {
            stop = BatchStop::network_streak;
        }
        // A local failure is always recorded, so the batch exits 1 wherever it happened. Any
        // other reason is a stop only when it leaves a runnable row behind; on the last row it
        // changes nothing.
        if (stop == BatchStop::local_failure ||
            (stop != BatchStop::none && runnable_row_after(jobs, index))) {
            result.stop = stop;
            result.stopped_at_line = job.line;
        }
    }
    return result;
}

int batch_exit_code(const BatchResult& result) noexcept {
    if (result.stop == BatchStop::interrupted || result.stop == BatchStop::local_failure) {
        // interrupted: a fallback only; main() re-raises the signal (130/143).
        return exit_code::failure;
    }
    // Indexed by exit code: 1 (failed), 3, 4, 5, 6.
    std::array<bool, exit_code::incomplete + 1> seen{};
    for (const TitleResult& row : result.titles) {
        if (is_failure(row.status)) {
            seen.at(static_cast<std::size_t>(code_for(row.status))) = true;
        }
    }
    int only = exit_code::success;
    std::size_t distinct = 0;
    for (std::size_t code = 0; code < seen.size(); ++code) {
        if (seen.at(code)) {
            ++distinct;
            only = static_cast<int>(code);
        }
    }
    return distinct > 1 ? exit_code::mixed_failures : only;
}

std::optional<std::string> batch_closing_message(const BatchResult& result, bool dry_run) {
    return batch_closing_message(result, dry_run, BatchSource::csv);
}

std::optional<std::string> batch_closing_message(const BatchResult& result, bool dry_run,
                                                 BatchSource source) {
    const RowNoun noun = row_noun(source);
    if (result.stop == BatchStop::interrupted) {
        return std::nullopt;
    }
    if (const std::optional<std::string> reason = stop_reason(result.stop)) {
        const auto not_run = static_cast<std::size_t>(std::ranges::count_if(
            result.titles, [](const auto& row) { return row.status == TitleStatus::not_run; }));
        if (not_run == 0) {
            // Only a local failure on the last runnable row stops with nothing left to run.
            return std::format("stopped at {} {}: {}", noun.one, result.stopped_at_line, *reason);
        }
        return std::format("stopped at {} {}: {}; {} not run", noun.one, result.stopped_at_line,
                           *reason, titles_phrase(not_run));
    }

    std::vector<std::size_t> failed_lines{};
    std::size_t considered = 0;
    bool retryable = false;
    for (const TitleResult& row : result.titles) {
        if (row.status == TitleStatus::duplicate) {
            continue;
        }
        ++considered;
        if (is_failure(row.status)) {
            failed_lines.push_back(row.line);
            retryable = retryable || is_retryable(row.status);
        }
    }
    if (failed_lines.empty()) {
        return std::nullopt;
    }
    const std::string of = std::format("{} of {}", failed_lines.size(), titles_phrase(considered));
    if (dry_run) {
        return std::format("{} would fail ({})", of, lines_phrase(failed_lines, noun));
    }
    // Suggest a retry only when one could help; otherwise point at the per-row errors.
    const std::string_view advice =
        retryable ? "run the same command again to retry them" : "see the errors above";
    return std::format("{} did not complete ({}); {}", of, lines_phrase(failed_lines, noun),
                       advice);
}

std::optional<std::size_t> OutputRegistry::claim(std::string path, std::size_t line) {
    std::string key = std::filesystem::path{std::move(path)}.lexically_normal().generic_string();
    const auto [it, inserted] = by_path_.try_emplace(std::move(key), line);
    if (inserted || it->second == line) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace mangadex_dl
