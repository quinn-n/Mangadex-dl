// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch/batch_run_test.cpp: the batch row loop (batch plan T3b) against a scripted
// FakeRunner: the stop rules (fatal, stdout broken, --fail-fast, the network streak, interrupts),
// duplicates, the aggregate exit code (AC7, rev1 fatal vs row-scoped `failed`), the closing
// message and cross-row collision detection.
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/report.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <csignal>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
using md::TitleStatus;

/// Clears the process-wide interrupt flag before and after a test.
class InterruptFlagReset {
public:
    InterruptFlagReset() noexcept { md::reset_interrupt_for_testing(); }
    InterruptFlagReset(const InterruptFlagReset&) = delete;
    InterruptFlagReset& operator=(const InterruptFlagReset&) = delete;
    InterruptFlagReset(InterruptFlagReset&&) = delete;
    InterruptFlagReset& operator=(InterruptFlagReset&&) = delete;
    ~InterruptFlagReset() { md::reset_interrupt_for_testing(); }
};

/// A canonical id that differs per `n` (n < 4096).
[[nodiscard]] std::string uuid(std::size_t n) {
    return std::format("0a1b2c3d-0000-4000-8000-{:012x}", n);
}

[[nodiscard]] md::MangaId manga(std::size_t n) {
    auto parsed = md::MangaId::parse(uuid(n));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

/// One job per entry; line = index + 2 (a header on line 1), a distinct title per row.
/// `duplicates[i]` set: row i is a duplicate of that line (and keeps the same title).
[[nodiscard]] std::vector<md::BatchJob>
make_jobs(std::size_t count, const std::vector<std::optional<std::size_t>>& duplicates = {}) {
    std::vector<md::BatchJob> jobs{};
    jobs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        md::BatchJob job{};
        job.line = i + 2;
        job.options.manga = manga(i + 1);
        job.dir = std::format("out/row{}", i + 2);
        if (i < duplicates.size()) {
            job.duplicate_of = duplicates[i];
        }
        jobs.push_back(std::move(job));
    }
    return jobs;
}

[[nodiscard]] md::RowOutcome code(int exit_code) { return md::RowOutcome{.exit_code = exit_code}; }

/// A global local condition (DIR unusable, ENOSPC, ...): the batch stops.
[[nodiscard]] md::RowOutcome fatal() {
    return md::RowOutcome{
        .exit_code = 1, .error = "cannot open directory 'out': Not a directory", .fatal = true};
}

/// A row-scoped `dir` failure: the row fails, the batch continues.
[[nodiscard]] md::RowOutcome row_failed() {
    return md::RowOutcome{.exit_code = 1,
                          .error = "cannot open directory 'out/blocked': Not a directory"};
}

/// What a FakeRunner does: the outcomes it returns in order, the row after which stdout counts
/// as broken, and the row during which it simulates SIGINT.
struct Script {
    std::vector<md::RowOutcome> outcomes{};
    std::optional<std::size_t> output_fails_after_line{};
    std::optional<std::size_t> interrupt_on_line{};
};

/// What a FakeRunner was asked to do.
struct Calls {
    std::vector<std::size_t> ran{};        ///< lines passed to run()
    std::vector<std::size_t> positions{};  ///< run()'s position argument
    std::vector<std::size_t> totals{};     ///< run()'s total argument
    std::vector<std::size_t> duplicates{}; ///< lines passed to on_duplicate()
    std::size_t output_checks = 0;         ///< output_failed() calls
};

/// Returns the scripted outcomes in order and records every call.
class FakeRunner final : public md::TitleRunner {
public:
    explicit FakeRunner(Script script) : script_(std::move(script)) {}

    md::RowOutcome run(const md::BatchJob& job, std::size_t position, std::size_t total) override {
        calls_.ran.push_back(job.line);
        calls_.positions.push_back(position);
        calls_.totals.push_back(total);
        if (script_.interrupt_on_line == job.line) {
            md::simulate_interrupt_for_testing(SIGINT);
        }
        REQUIRE(next_ < script_.outcomes.size());
        return script_.outcomes[next_++];
    }

    void on_duplicate(const md::BatchJob& job) override { calls_.duplicates.push_back(job.line); }

    bool output_failed() override {
        ++calls_.output_checks;
        return script_.output_fails_after_line.has_value() && !calls_.ran.empty() &&
               calls_.ran.back() >= *script_.output_fails_after_line;
    }

    [[nodiscard]] const Calls& calls() const noexcept { return calls_; }

private:
    Script script_{};
    Calls calls_{};
    std::size_t next_ = 0;
};

[[nodiscard]] std::vector<TitleStatus> statuses(const md::BatchResult& result) {
    std::vector<TitleStatus> out{};
    out.reserve(result.titles.size());
    for (const md::TitleResult& row : result.titles) {
        out.push_back(row.status);
    }
    return out;
}

/// Runs one row per scripted outcome (no duplicates).
struct Ran {
    md::BatchResult result{};
    std::vector<std::size_t> lines_run{};
};

[[nodiscard]] Ran run_codes(std::vector<md::RowOutcome> script, bool fail_fast = false) {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(script.size());
    FakeRunner runner{Script{.outcomes = std::move(script)}};
    md::BatchResult result = md::run_batch(jobs, runner, fail_fast);
    return Ran{.result = std::move(result), .lines_run = runner.calls().ran};
}

/// A finished row with `status`, for the message and exit-code tests.
[[nodiscard]] md::TitleResult row(std::size_t line, TitleStatus status) {
    return md::TitleResult{.line = line, .manga_id = uuid(line), .status = status};
}

} // namespace

// ---- title_status_for ------------------------------------------------------------------------

TEST_CASE("title_status_for maps download's exit codes", "[batch][run]") {
    CHECK(md::title_status_for(0) == TitleStatus::ok);
    CHECK(md::title_status_for(3) == TitleStatus::network);
    CHECK(md::title_status_for(4) == TitleStatus::not_found);
    CHECK(md::title_status_for(5) == TitleStatus::no_chapters);
    CHECK(md::title_status_for(6) == TitleStatus::incomplete);
    CHECK(md::title_status_for(1) == TitleStatus::failed);
    CHECK(md::title_status_for(2) == TitleStatus::failed);
    CHECK(md::title_status_for(7) == TitleStatus::failed);
    CHECK(md::title_status_for(70) == TitleStatus::failed);
    CHECK(md::title_status_for(-1) == TitleStatus::failed);
}

// ---- run_batch + batch_exit_code (AC7) ---------------------------------------------------------

TEST_CASE("run_batch: every row runs in order with positions among non-duplicates",
          "[batch][run]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(4, {std::nullopt, std::nullopt, 2});
    FakeRunner runner{Script{.outcomes = {code(0), code(0), code(0)}}};
    const md::BatchResult result = md::run_batch(jobs, runner, false);

    CHECK(runner.calls().ran == std::vector<std::size_t>{2, 3, 5});
    CHECK(runner.calls().positions == std::vector<std::size_t>{1, 2, 3});
    CHECK(runner.calls().totals == std::vector<std::size_t>{3, 3, 3});
    CHECK(runner.calls().duplicates == std::vector<std::size_t>{4});
    CHECK(result.stop == md::BatchStop::none);
    CHECK(result.stopped_at_line == 0);
    CHECK(statuses(result) ==
          std::vector{TitleStatus::ok, TitleStatus::ok, TitleStatus::duplicate, TitleStatus::ok});
    CHECK(md::batch_exit_code(result) == md::exit_code::success);

    // The rows copy their strings from the jobs.
    REQUIRE(result.titles.size() == 4);
    const md::TitleResult& dup = result.titles[2];
    CHECK(dup.line == 4);
    CHECK(dup.manga_id == uuid(3));
    CHECK(dup.dir == "out/row4");
    CHECK(dup.duplicate_of == std::optional<std::size_t>{2});
    CHECK(!dup.exit_code.has_value());
    CHECK(!dup.error.has_value());
    const md::TitleResult& first = result.titles[0];
    CHECK(first.manga_id == uuid(1));
    CHECK(first.dir == "out/row2");
    CHECK(first.exit_code == std::optional<int>{0});
    CHECK(!first.error.has_value());
    CHECK(!first.duplicate_of.has_value());
}

TEST_CASE("run_batch: the outcome's error, files and report move into the row", "[batch][run]") {
    md::RunReport report{};
    report.manga.title = "Kept";
    const md::RunSummary files{.planned = 1, .written = 2, .exists = 3, .failed = 4};
    const Ran ran = run_codes({md::RowOutcome{
        .exit_code = 6, .error = "1 of 7 files failed", .files = files, .report = report}});
    REQUIRE(ran.result.titles.size() == 1);
    const md::TitleResult& only = ran.result.titles[0];
    CHECK(only.status == TitleStatus::incomplete);
    CHECK(only.exit_code == std::optional<int>{6});
    CHECK(only.error == std::optional<std::string>{"1 of 7 files failed"});
    CHECK(only.files == files);
    CHECK(only.report == std::optional<md::RunReport>{report});
}

TEST_CASE("run_batch: an empty job list runs nothing", "[batch][run]") {
    const Ran ran = run_codes({});
    // Field-wise rather than `ran.result == md::BatchResult{}`: at -O2, GCC inlines the defaulted
    // vector<TitleResult> == against the constant empty vector and reports an infeasible
    // -Wnull-dereference on the never-entered element loop.
    CHECK(ran.result.titles.empty());
    CHECK(ran.result.stop == md::BatchStop::none);
    CHECK(ran.result.stopped_at_line == 0);
    CHECK(md::batch_exit_code(ran.result) == md::exit_code::success);
    CHECK(!md::batch_closing_message(ran.result, false).has_value());
}

TEST_CASE("batch_exit_code: uniform failures keep their code, mixed ones are 7", "[batch][run]") {
    CHECK(md::batch_exit_code(run_codes({code(0), code(0)}).result) == 0);
    CHECK(md::batch_exit_code(run_codes({code(4)}).result) == 4);
    CHECK(md::batch_exit_code(run_codes({code(4), code(0), code(4)}).result) == 4);
    CHECK(md::batch_exit_code(run_codes({code(5), code(4)}).result) == 7);
    CHECK(md::batch_exit_code(run_codes({code(3), code(6)}).result) == 7);
    CHECK(md::batch_exit_code(run_codes({code(6), code(0)}).result) == 6);
    CHECK(md::batch_exit_code(run_codes({code(5)}).result) == 5);
    CHECK(md::batch_exit_code(run_codes({code(3)}).result) == 3);
}

TEST_CASE("run_batch: a fatal row stops the batch with exit 1", "[batch][run]") {
    SECTION("at row 2 of 4") {
        const Ran ran = run_codes({code(0), fatal(), code(0), code(0)});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3});
        CHECK(ran.result.stop == md::BatchStop::local_failure);
        CHECK(ran.result.stopped_at_line == 3);
        CHECK(statuses(ran.result) == std::vector{TitleStatus::ok, TitleStatus::failed,
                                                  TitleStatus::not_run, TitleStatus::not_run});
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
        REQUIRE(ran.result.titles.size() == 4);
        CHECK(!ran.result.titles[2].exit_code.has_value());
        CHECK(ran.result.titles[1].error ==
              std::optional<std::string>{"cannot open directory 'out': Not a directory"});
    }
    SECTION("after an earlier 4: still 1, never 7") {
        const Ran ran = run_codes({code(4), fatal(), code(0), code(0)});
        CHECK(ran.result.stop == md::BatchStop::local_failure);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
    }
    SECTION("on the last row: still a local_failure stop, exit 1, no 'not run' tail") {
        const Ran ran = run_codes({code(0), fatal()});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3});
        CHECK(ran.result.stop == md::BatchStop::local_failure);
        CHECK(ran.result.stopped_at_line == 3);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
        CHECK(md::batch_closing_message(ran.result, false) ==
              std::optional<std::string>{"stopped at line 3: the error above affects every title"});
    }
    SECTION("{4, fatal on the last row} -> 1, never 7") {
        const Ran ran = run_codes({code(4), fatal()});
        CHECK(ran.result.stop == md::BatchStop::local_failure);
        CHECK(ran.result.stopped_at_line == 3);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
    }
    SECTION("a single fatal row -> 1") {
        const Ran ran = run_codes({fatal()});
        CHECK(ran.lines_run == std::vector<std::size_t>{2});
        CHECK(ran.result.stop == md::BatchStop::local_failure);
        CHECK(ran.result.stopped_at_line == 2);
        CHECK(statuses(ran.result) == std::vector{TitleStatus::failed});
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
        CHECK(md::batch_closing_message(ran.result, true) ==
              std::optional<std::string>{"stopped at line 2: the error above affects every title"});
    }
    SECTION("followed only by duplicates: a local_failure stop, the duplicates keep their "
            "status and get no warning") {
        const InterruptFlagReset reset;
        const std::vector<md::BatchJob> jobs = make_jobs(3, {std::nullopt, std::nullopt, 2});
        FakeRunner runner{Script{.outcomes = {code(0), fatal()}}};
        const md::BatchResult result = md::run_batch(jobs, runner, false);
        CHECK(result.stop == md::BatchStop::local_failure);
        CHECK(result.stopped_at_line == 3);
        CHECK(statuses(result) ==
              std::vector{TitleStatus::ok, TitleStatus::failed, TitleStatus::duplicate});
        CHECK(runner.calls().duplicates.empty());
        CHECK(md::batch_exit_code(result) == md::exit_code::failure);
        CHECK(md::batch_closing_message(result, false) ==
              std::optional<std::string>{"stopped at line 3: the error above affects every title"});
    }
}

TEST_CASE("run_batch: a non-fatal failed row (row-scoped dir) does not stop", "[batch][run]") {
    SECTION("at row 1 of 3: rows 2 and 3 still run; {failed, ok, ok} -> 1") {
        const Ran ran = run_codes({row_failed(), code(0), code(0)});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3, 4});
        CHECK(ran.result.stop == md::BatchStop::none);
        CHECK(statuses(ran.result) ==
              std::vector{TitleStatus::failed, TitleStatus::ok, TitleStatus::ok});
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
        CHECK(md::batch_closing_message(ran.result, false) ==
              std::optional<std::string>{
                  "1 of 3 titles did not complete (line 2); see the errors above"});
    }
    SECTION("{failed, 4} -> 7") {
        const Ran ran = run_codes({row_failed(), code(4)});
        CHECK(ran.result.stop == md::BatchStop::none);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::mixed_failures);
    }
    SECTION("{failed, failed} -> 1") {
        const Ran ran = run_codes({row_failed(), row_failed()});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3});
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
    }
    SECTION("with --fail-fast: a fail_fast stop, exit 1") {
        const Ran ran = run_codes({row_failed(), code(0)}, true);
        CHECK(ran.lines_run == std::vector<std::size_t>{2});
        CHECK(ran.result.stop == md::BatchStop::fail_fast);
        CHECK(ran.result.stopped_at_line == 2);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::failure);
    }
}

TEST_CASE("run_batch: --fail-fast stops at the first non-ok row", "[batch][run]") {
    SECTION("a 5 at row 1 of 2 -> 5, row 2 not run") {
        const Ran ran = run_codes({code(5), code(0)}, true);
        CHECK(ran.lines_run == std::vector<std::size_t>{2});
        CHECK(ran.result.stop == md::BatchStop::fail_fast);
        CHECK(statuses(ran.result) == std::vector{TitleStatus::no_chapters, TitleStatus::not_run});
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::no_chapters);
        CHECK(md::batch_closing_message(ran.result, false) ==
              std::optional<std::string>{"stopped at line 2: --fail-fast; 1 title not run"});
    }
    SECTION("ok rows don't trip it") {
        const Ran ran = run_codes({code(0), code(0)}, true);
        CHECK(ran.result.stop == md::BatchStop::none);
    }
    SECTION("on the last row it is no stop") {
        const Ran ran = run_codes({code(0), code(4)}, true);
        CHECK(ran.result.stop == md::BatchStop::none);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::not_found);
    }
    SECTION("without --fail-fast every row runs") {
        const Ran ran = run_codes({code(5), code(0)});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3});
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::no_chapters);
    }
}

TEST_CASE("run_batch: three network rows in a row stop the batch", "[batch][run]") {
    SECTION("3,3,3,3 -> stop after row 3, row 4 not run, exit 3") {
        const Ran ran = run_codes({code(3), code(3), code(3), code(3)});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3, 4});
        CHECK(ran.result.stop == md::BatchStop::network_streak);
        CHECK(ran.result.stopped_at_line == 4);
        CHECK(statuses(ran.result).back() == TitleStatus::not_run);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::network);
        CHECK(md::batch_closing_message(ran.result, false) ==
              std::optional<std::string>{
                  "stopped at line 4: 3 titles in a row failed with network errors; 1 title "
                  "not run"});
    }
    SECTION("3,4,3,3,3 -> the streak ends on the last row: no stop, exit 7") {
        const Ran ran = run_codes({code(3), code(4), code(3), code(3), code(3)});
        CHECK(ran.lines_run.size() == 5);
        CHECK(ran.result.stop == md::BatchStop::none);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::mixed_failures);
    }
    SECTION("4,3,3,3,0 -> stop after row 4, row 5 not run, exit 7") {
        const Ran ran = run_codes({code(4), code(3), code(3), code(3), code(0)});
        CHECK(ran.lines_run == std::vector<std::size_t>{2, 3, 4, 5});
        CHECK(ran.result.stop == md::BatchStop::network_streak);
        CHECK(ran.result.stopped_at_line == 5);
        CHECK(statuses(ran.result).back() == TitleStatus::not_run);
        CHECK(md::batch_exit_code(ran.result) == md::exit_code::mixed_failures);
    }
    SECTION("an ok row resets the streak") {
        const Ran ran = run_codes({code(3), code(3), code(0), code(3), code(3), code(0)});
        CHECK(ran.lines_run.size() == 6);
        CHECK(ran.result.stop == md::BatchStop::none);
    }
    SECTION("a duplicate in the middle is not run and does not break the streak") {
        const InterruptFlagReset reset;
        const std::vector<md::BatchJob> jobs =
            make_jobs(5, {std::nullopt, std::nullopt, 2, std::nullopt, std::nullopt});
        FakeRunner runner{Script{.outcomes = {code(3), code(3), code(3), code(0)}}};
        const md::BatchResult result = md::run_batch(jobs, runner, false);
        CHECK(runner.calls().ran == std::vector<std::size_t>{2, 3, 5});
        CHECK(runner.calls().duplicates == std::vector<std::size_t>{4});
        CHECK(result.stop == md::BatchStop::network_streak);
        CHECK(result.stopped_at_line == 5);
        CHECK(statuses(result) == std::vector{TitleStatus::network, TitleStatus::network,
                                              TitleStatus::duplicate, TitleStatus::network,
                                              TitleStatus::not_run});
        CHECK(md::batch_exit_code(result) == md::exit_code::network);
    }
    SECTION("duplicates after a stop keep `duplicate` and get no warning") {
        const InterruptFlagReset reset;
        const std::vector<md::BatchJob> jobs =
            make_jobs(5, {std::nullopt, std::nullopt, std::nullopt, 2, std::nullopt});
        FakeRunner runner{Script{.outcomes = {code(3), code(3), code(3), code(0)}}};
        const md::BatchResult result = md::run_batch(jobs, runner, false);
        CHECK(runner.calls().ran == std::vector<std::size_t>{2, 3, 4});
        CHECK(runner.calls().duplicates.empty());
        CHECK(statuses(result) == std::vector{TitleStatus::network, TitleStatus::network,
                                              TitleStatus::network, TitleStatus::duplicate,
                                              TitleStatus::not_run});
        CHECK(md::batch_closing_message(result, false) ==
              std::optional<std::string>{
                  "stopped at line 4: 3 titles in a row failed with network errors; 1 title "
                  "not run"});
    }
}

TEST_CASE("run_batch: a broken stdout stops the batch", "[batch][run]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(3);
    FakeRunner runner{
        Script{.outcomes = {code(0), code(0), code(0)}, .output_fails_after_line = 2}};
    const md::BatchResult result = md::run_batch(jobs, runner, false);
    CHECK(runner.calls().ran == std::vector<std::size_t>{2});
    CHECK(result.stop == md::BatchStop::output_error);
    CHECK(result.stopped_at_line == 2);
    CHECK(statuses(result) ==
          std::vector{TitleStatus::ok, TitleStatus::not_run, TitleStatus::not_run});
    CHECK(
        md::batch_closing_message(result, false) ==
        std::optional<std::string>{"stopped at line 2: cannot write to stdout; 2 titles not run"});
    // A broken stdout is a write error that finish_stdout maps to 1; the aggregate is the rows'.
    CHECK(md::batch_exit_code(result) == md::exit_code::success);
}

TEST_CASE("run_batch: stdout is checked after every row", "[batch][run]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(3);
    FakeRunner runner{Script{.outcomes = {code(0), code(4), code(0)}}};
    const md::BatchResult result = md::run_batch(jobs, runner, false);
    CHECK(runner.calls().output_checks == 3);
    CHECK(result.stop == md::BatchStop::none);
}

// ---- interrupts (AC10, unit tier) -------------------------------------------------------------

TEST_CASE("run_batch: an interrupt during row 1 stops before row 2", "[batch][run][interrupt]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(3);
    FakeRunner runner{Script{.outcomes = {code(0), code(0), code(0)}, .interrupt_on_line = 2}};
    const md::BatchResult result = md::run_batch(jobs, runner, false);
    CHECK(runner.calls().ran == std::vector<std::size_t>{2});
    CHECK(runner.calls().output_checks == 1);
    CHECK(result.stop == md::BatchStop::interrupted);
    CHECK(result.stopped_at_line == 3);
    CHECK(statuses(result) ==
          std::vector{TitleStatus::ok, TitleStatus::not_run, TitleStatus::not_run});
    CHECK(!md::batch_closing_message(result, false).has_value());
    CHECK(md::batch_exit_code(result) == md::exit_code::failure);
}

TEST_CASE("run_batch: the runner's interrupted flag stops the batch", "[batch][run][interrupt]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(3);
    const md::RowOutcome interrupted{.exit_code = 1, .error = "interrupted", .interrupted = true};
    FakeRunner runner{Script{.outcomes = {code(4), interrupted, code(0)}}};
    const md::BatchResult result = md::run_batch(jobs, runner, false);
    CHECK(runner.calls().ran == std::vector<std::size_t>{2, 3});
    CHECK(runner.calls().output_checks == 1); // nothing is checked after an interrupted row
    CHECK(result.stop == md::BatchStop::interrupted);
    CHECK(result.stopped_at_line == 3);
    CHECK(statuses(result) ==
          std::vector{TitleStatus::not_found, TitleStatus::not_run, TitleStatus::not_run});
    REQUIRE(result.titles.size() == 3);
    CHECK(!result.titles[1].exit_code.has_value());
    CHECK(!result.titles[1].error.has_value());
    CHECK(!md::batch_closing_message(result, false).has_value());
    CHECK(md::batch_exit_code(result) == md::exit_code::failure);
}

TEST_CASE("run_batch: a flag set before the call runs no row", "[batch][run][interrupt]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = make_jobs(2);
    FakeRunner runner{Script{.outcomes = {code(0), code(0)}}};
    md::simulate_interrupt_for_testing(SIGTERM);
    const md::BatchResult result = md::run_batch(jobs, runner, false);
    CHECK(runner.calls().ran.empty());
    CHECK(runner.calls().output_checks == 0);
    CHECK(result.stop == md::BatchStop::interrupted);
    CHECK(result.stopped_at_line == 2);
    CHECK(statuses(result) == std::vector{TitleStatus::not_run, TitleStatus::not_run});
    CHECK(!md::batch_closing_message(result, false).has_value());
    CHECK(md::batch_exit_code(result) == md::exit_code::failure);
}

// ---- batch_closing_message ---------------------------------------------------------------------

TEST_CASE("batch_closing_message: nothing when every row is ok or a duplicate",
          "[batch][message]") {
    md::BatchResult result{};
    CHECK(!md::batch_closing_message(result, false).has_value());
    result.titles = {row(2, TitleStatus::ok), row(3, TitleStatus::duplicate)};
    CHECK(!md::batch_closing_message(result, false).has_value());
    CHECK(!md::batch_closing_message(result, true).has_value());
}

TEST_CASE("batch_closing_message: did not complete / would fail", "[batch][message]") {
    md::BatchResult result{};
    result.titles = {row(4, TitleStatus::not_found), row(5, TitleStatus::ok),
                     row(9, TitleStatus::no_chapters), row(10, TitleStatus::duplicate),
                     row(12, TitleStatus::failed)};
    // Nothing here goes away on a retry: point at the per-row errors instead.
    CHECK(md::batch_closing_message(result, false) ==
          std::optional<std::string>{
              "3 of 4 titles did not complete (lines 4, 9, 12); see the errors above"});
    CHECK(md::batch_closing_message(result, true) ==
          std::optional<std::string>{"3 of 4 titles would fail (lines 4, 9, 12)"});

    SECTION("one network or incomplete row is enough to suggest a retry") {
        const auto status = GENERATE(TitleStatus::network, TitleStatus::incomplete);
        CAPTURE(md::title_status_name(status));
        result.titles.push_back(row(13, status));
        CHECK(md::batch_closing_message(result, false) ==
              std::optional<std::string>{"4 of 5 titles did not complete (lines 4, 9, 12, 13); "
                                         "run the same command again to retry them"});
        // A dry run never suggests a retry.
        CHECK(md::batch_closing_message(result, true) ==
              std::optional<std::string>{"4 of 5 titles would fail (lines 4, 9, 12, 13)"});
    }
    SECTION("each non-retryable status alone points at the errors") {
        const auto status =
            GENERATE(TitleStatus::not_found, TitleStatus::no_chapters, TitleStatus::failed);
        CAPTURE(md::title_status_name(status));
        result.titles = {row(3, status), row(4, TitleStatus::ok)};
        CHECK(md::batch_closing_message(result, false) ==
              std::optional<std::string>{
                  "1 of 2 titles did not complete (line 3); see the errors above"});
    }

    SECTION("singular forms") {
        result.titles = {row(7, TitleStatus::incomplete)};
        CHECK(md::batch_closing_message(result, false) ==
              std::optional<std::string>{"1 of 1 title did not complete (line 7); run the same "
                                         "command again to retry them"});
        CHECK(md::batch_closing_message(result, true) ==
              std::optional<std::string>{"1 of 1 title would fail (line 7)"});
        result.titles.push_back(row(8, TitleStatus::ok));
        CHECK(md::batch_closing_message(result, true) ==
              std::optional<std::string>{"1 of 2 titles would fail (line 7)"});
    }
    SECTION("every network row counts") {
        result.titles = {row(2, TitleStatus::network), row(3, TitleStatus::network)};
        CHECK(md::batch_closing_message(result, true) ==
              std::optional<std::string>{"2 of 2 titles would fail (lines 2, 3)"});
    }
}

TEST_CASE("batch_closing_message: at most 10 line numbers", "[batch][message]") {
    md::BatchResult result{};
    for (std::size_t line = 1; line <= 10; ++line) {
        result.titles.push_back(row(line, TitleStatus::not_found));
    }
    CHECK(md::batch_closing_message(result, true) ==
          std::optional<std::string>{"10 of 10 titles would fail (lines 1, 2, 3, 4, 5, 6, 7, 8, 9, "
                                     "10)"});
    result.titles.push_back(row(11, TitleStatus::not_found));
    CHECK(md::batch_closing_message(result, true) ==
          std::optional<std::string>{"11 of 11 titles would fail (lines 1, 2, 3, 4, 5, 6, 7, 8, 9, "
                                     "10 and 1 more)"});
    result.titles.push_back(row(12, TitleStatus::network));
    result.titles.push_back(row(13, TitleStatus::ok));
    CHECK(md::batch_closing_message(result, false) ==
          std::optional<std::string>{"12 of 13 titles did not complete (lines 1, 2, 3, 4, 5, 6, 7, "
                                     "8, 9, 10 and 2 more); run the same command again to retry "
                                     "them"});
}

TEST_CASE("batch_closing_message: every stop reason", "[batch][message]") {
    md::BatchResult result{};
    result.titles = {row(2, TitleStatus::failed), row(3, TitleStatus::not_run),
                     row(4, TitleStatus::duplicate), row(5, TitleStatus::not_run)};
    result.stopped_at_line = 2;

    result.stop = md::BatchStop::local_failure;
    CHECK(md::batch_closing_message(result, false) ==
          std::optional<std::string>{
              "stopped at line 2: the error above affects every title; 2 titles not run"});
    // The stop wording does not change with --dry-run.
    CHECK(md::batch_closing_message(result, true) == md::batch_closing_message(result, false));

    result.stop = md::BatchStop::fail_fast;
    CHECK(md::batch_closing_message(result, false) ==
          std::optional<std::string>{"stopped at line 2: --fail-fast; 2 titles not run"});

    result.stop = md::BatchStop::output_error;
    CHECK(
        md::batch_closing_message(result, false) ==
        std::optional<std::string>{"stopped at line 2: cannot write to stdout; 2 titles not run"});

    result.stop = md::BatchStop::network_streak;
    result.titles.pop_back();
    CHECK(md::batch_closing_message(result, false) ==
          std::optional<std::string>{
              "stopped at line 2: 3 titles in a row failed with network errors; 1 title not run"});

    result.stop = md::BatchStop::interrupted;
    CHECK(!md::batch_closing_message(result, false).has_value());
}

// ---- batch_exit_code on hand-built results ------------------------------------------------------

TEST_CASE("batch_exit_code: duplicate and not_run never count", "[batch][run]") {
    md::BatchResult result{};
    result.titles = {row(2, TitleStatus::duplicate), row(3, TitleStatus::not_run)};
    CHECK(md::batch_exit_code(result) == md::exit_code::success);
    result.titles.push_back(row(4, TitleStatus::incomplete));
    CHECK(md::batch_exit_code(result) == md::exit_code::incomplete);
    result.titles.push_back(row(5, TitleStatus::failed));
    CHECK(md::batch_exit_code(result) == md::exit_code::mixed_failures);
    result.stop = md::BatchStop::interrupted;
    CHECK(md::batch_exit_code(result) == md::exit_code::failure);
    result.stop = md::BatchStop::local_failure;
    CHECK(md::batch_exit_code(result) == md::exit_code::failure);
    result.stop = md::BatchStop::fail_fast;
    CHECK(md::batch_exit_code(result) == md::exit_code::mixed_failures);
}

// ---- OutputRegistry ----------------------------------------------------------------------------

TEST_CASE("OutputRegistry: the first claim wins", "[batch][registry]") {
    md::OutputRegistry registry{};
    CHECK(!registry.claim("out/A - Vol. 01.epub", 2).has_value());
    CHECK(!registry.claim("out/B - Vol. 01.epub", 3).has_value());
    CHECK(registry.claim("out/A - Vol. 01.epub", 4) == std::optional<std::size_t>{2});
    // The earlier line keeps the claim.
    CHECK(registry.claim("out/A - Vol. 01.epub", 5) == std::optional<std::size_t>{2});
    CHECK(registry.claim("out/B - Vol. 01.epub", 5) == std::optional<std::size_t>{3});
}

TEST_CASE("OutputRegistry: the same line claiming again is no collision", "[batch][registry]") {
    md::OutputRegistry registry{};
    CHECK(!registry.claim("out/A.epub", 2).has_value());
    CHECK(!registry.claim("out/A.epub", 2).has_value());
    CHECK(registry.claim("out/A.epub", 3) == std::optional<std::size_t>{2});
}

TEST_CASE("OutputRegistry: keyed on the lexically normalized path", "[batch][registry]") {
    md::OutputRegistry registry{};
    CHECK(!registry.claim("out/A.epub", 2).has_value());
    CHECK(registry.claim("out//A.epub", 3) == std::optional<std::size_t>{2});
    CHECK(registry.claim("out/./A.epub", 4) == std::optional<std::size_t>{2});
    CHECK(registry.claim("./out/A.epub", 5) == std::optional<std::size_t>{2});
    CHECK(registry.claim("out/sub/../A.epub", 6) == std::optional<std::size_t>{2});
    // Different files stay different.
    CHECK(!registry.claim("out/a.epub", 7).has_value());
    CHECK(!registry.claim("out/sub/A.epub", 8).has_value());
}
