// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch/batch_source_test.cpp: the batch core reused for lists (list plan T5): the
// source-aware closing message (AC7: "item"/"items" for mdlist, today's "line"/"lines" for csv),
// an interrupt during item 1 of 2 under an mdlist-shaped job list (AC12, unit half), and the
// runner's effective dir (RowOutcome::dir) with a row-scoped --dir-per-title failure (AC39, core
// half).
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
using md::BatchSource;
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
    return std::format("5a1b2c3d-0000-4000-8000-{:012x}", n);
}

[[nodiscard]] md::MangaId manga(std::size_t n) {
    auto parsed = md::MangaId::parse(uuid(n));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

/// An mdlist-shaped job list: line = the 1-based list position, dir = DIR, no sub_dir, the
/// --dir-per-title flag on every job.
[[nodiscard]] std::vector<md::BatchJob> list_jobs(std::size_t count) {
    std::vector<md::BatchJob> jobs{};
    jobs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        md::BatchJob job{};
        job.line = i + 1;
        job.options.manga = manga(i + 1);
        job.options.dir = "out";
        job.options.dir_per_title = true;
        job.dir = "out";
        jobs.push_back(std::move(job));
    }
    return jobs;
}

/// Returns the scripted outcomes in order, records the lines it ran, and simulates SIGINT
/// during the row on `interrupt_on_line`.
class FakeRunner final : public md::TitleRunner {
public:
    FakeRunner(std::vector<md::RowOutcome> outcomes, std::optional<std::size_t> interrupt_on_line)
        : outcomes_(std::move(outcomes)), interrupt_on_line_(interrupt_on_line) {}

    md::RowOutcome run(const md::BatchJob& job, std::size_t /*position*/,
                       std::size_t /*total*/) override {
        ran_.push_back(job.line);
        if (interrupt_on_line_ == job.line) {
            md::simulate_interrupt_for_testing(SIGINT);
        }
        REQUIRE(next_ < outcomes_.size());
        return outcomes_[next_++];
    }

    void on_duplicate(const md::BatchJob& /*job*/) override {}

    bool output_failed() override { return false; }

    [[nodiscard]] const std::vector<std::size_t>& ran() const noexcept { return ran_; }

private:
    std::vector<md::RowOutcome> outcomes_{};
    std::optional<std::size_t> interrupt_on_line_{};
    std::vector<std::size_t> ran_{};
    std::size_t next_ = 0;
};

/// A finished row with `status`, for the message tests.
[[nodiscard]] md::TitleResult row(std::size_t line, TitleStatus status) {
    return md::TitleResult{.line = line, .manga_id = uuid(line), .status = status};
}

/// 15 rows, all ok except `failed` at the given lines (1-based).
[[nodiscard]] md::BatchResult fifteen_with(const std::vector<std::size_t>& failed_lines,
                                           TitleStatus failed_status) {
    md::BatchResult result{};
    for (std::size_t line = 1; line <= 15; ++line) {
        const bool failed = std::ranges::find(failed_lines, line) != failed_lines.end();
        result.titles.push_back(row(line, failed ? failed_status : TitleStatus::ok));
    }
    return result;
}

/// The same message for csv and mdlist differs only in the row noun.
struct Worded {
    std::string csv{};
    std::string mdlist{};
};

} // namespace

// ---- AC7: the closing message ------------------------------------------------------------------

TEST_CASE("batch source: did not complete / would fail name items for mdlist", "[batch][source]") {
    const md::BatchResult not_found = fifteen_with({4, 9}, TitleStatus::not_found);
    const md::BatchResult network = fifteen_with({4, 9}, TitleStatus::network);
    const Worded advice_errors{
        .csv = "2 of 15 titles did not complete (lines 4, 9); see the errors above",
        .mdlist = "2 of 15 titles did not complete (items 4, 9); see the errors above"};
    const Worded advice_retry{.csv = "2 of 15 titles did not complete (lines 4, 9); run the same "
                                     "command again to retry them",
                              .mdlist = "2 of 15 titles did not complete (items 4, 9); run the "
                                        "same command again to retry them"};
    const Worded dry{.csv = "2 of 15 titles would fail (lines 4, 9)",
                     .mdlist = "2 of 15 titles would fail (items 4, 9)"};

    CHECK(md::batch_closing_message(not_found, false, BatchSource::mdlist) ==
          std::optional{advice_errors.mdlist});
    CHECK(md::batch_closing_message(network, false, BatchSource::mdlist) ==
          std::optional{advice_retry.mdlist});
    CHECK(md::batch_closing_message(not_found, true, BatchSource::mdlist) ==
          std::optional{dry.mdlist});
    CHECK(md::batch_closing_message(network, true, BatchSource::mdlist) ==
          std::optional{dry.mdlist});

    // csv: today's strings byte for byte, and the same as the two-argument overload.
    CHECK(md::batch_closing_message(not_found, false, BatchSource::csv) ==
          std::optional{advice_errors.csv});
    CHECK(md::batch_closing_message(network, false, BatchSource::csv) ==
          std::optional{advice_retry.csv});
    CHECK(md::batch_closing_message(not_found, true, BatchSource::csv) == std::optional{dry.csv});
    for (const bool dry_run : {false, true}) {
        CAPTURE(dry_run);
        CHECK(md::batch_closing_message(not_found, dry_run) ==
              md::batch_closing_message(not_found, dry_run, BatchSource::csv));
        CHECK(md::batch_closing_message(network, dry_run) ==
              md::batch_closing_message(network, dry_run, BatchSource::csv));
    }
}

TEST_CASE("batch source: singular item", "[batch][source]") {
    md::BatchResult result{};
    result.titles = {row(4, TitleStatus::failed), row(5, TitleStatus::ok)};
    CHECK(md::batch_closing_message(result, false, BatchSource::mdlist) ==
          std::optional<std::string>{
              "1 of 2 titles did not complete (item 4); see the errors above"});
    CHECK(md::batch_closing_message(result, true, BatchSource::mdlist) ==
          std::optional<std::string>{"1 of 2 titles would fail (item 4)"});
    CHECK(md::batch_closing_message(result, false, BatchSource::csv) ==
          std::optional<std::string>{
              "1 of 2 titles did not complete (line 4); see the errors above"});
    CHECK(md::batch_closing_message(result, true) ==
          std::optional<std::string>{"1 of 2 titles would fail (line 4)"});
}

TEST_CASE("batch source: at most 10 item numbers", "[batch][source]") {
    md::BatchResult result{};
    for (std::size_t line = 1; line <= 10; ++line) {
        result.titles.push_back(row(line, TitleStatus::not_found));
    }
    CHECK(md::batch_closing_message(result, true, BatchSource::mdlist) ==
          std::optional<std::string>{"10 of 10 titles would fail (items 1, 2, 3, 4, 5, 6, 7, 8, 9, "
                                     "10)"});
    result.titles.push_back(row(11, TitleStatus::not_found));
    result.titles.push_back(row(12, TitleStatus::incomplete));
    result.titles.push_back(row(13, TitleStatus::duplicate));
    CHECK(md::batch_closing_message(result, true, BatchSource::mdlist) ==
          std::optional<std::string>{"12 of 12 titles would fail (items 1, 2, 3, 4, 5, 6, 7, 8, 9, "
                                     "10 and 2 more)"});
    CHECK(md::batch_closing_message(result, false, BatchSource::mdlist) ==
          std::optional<std::string>{"12 of 12 titles did not complete (items 1, 2, 3, 4, 5, 6, 7, "
                                     "8, 9, 10 and 2 more); run the same command again to retry "
                                     "them"});
    CHECK(md::batch_closing_message(result, false, BatchSource::csv) ==
          std::optional<std::string>{"12 of 12 titles did not complete (lines 1, 2, 3, 4, 5, 6, 7, "
                                     "8, 9, 10 and 2 more); run the same command again to retry "
                                     "them"});
    CHECK(md::batch_closing_message(result, false) ==
          md::batch_closing_message(result, false, BatchSource::csv));
}

TEST_CASE("batch source: stopped at item <k> with every reason", "[batch][source]") {
    md::BatchResult result{};
    for (std::size_t line = 1; line <= 15; ++line) {
        TitleStatus status = TitleStatus::ok;
        if (line == 9) {
            status = TitleStatus::not_found;
        } else if (line > 9) {
            status = TitleStatus::not_run;
        }
        result.titles.push_back(row(line, status));
    }
    result.stopped_at_line = 9;

    struct Reason {
        md::BatchStop stop = md::BatchStop::none;
        std::string text{};
    };
    const Reason reason = GENERATE(
        Reason{.stop = md::BatchStop::fail_fast, .text = "--fail-fast"},
        Reason{.stop = md::BatchStop::local_failure, .text = "the error above affects every title"},
        Reason{.stop = md::BatchStop::network_streak,
               .text = "3 titles in a row failed with network errors"},
        Reason{.stop = md::BatchStop::output_error, .text = "cannot write to stdout"});
    CAPTURE(reason.text);
    result.stop = reason.stop;
    for (const bool dry_run : {false, true}) {
        CAPTURE(dry_run);
        CHECK(md::batch_closing_message(result, dry_run, BatchSource::mdlist) ==
              std::optional{std::format("stopped at item 9: {}; 6 titles not run", reason.text)});
        CHECK(md::batch_closing_message(result, dry_run, BatchSource::csv) ==
              std::optional{std::format("stopped at line 9: {}; 6 titles not run", reason.text)});
        CHECK(md::batch_closing_message(result, dry_run) ==
              md::batch_closing_message(result, dry_run, BatchSource::csv));
    }

    SECTION("one title not run") {
        result.titles.resize(10);
        CHECK(md::batch_closing_message(result, false, BatchSource::mdlist) ==
              std::optional{std::format("stopped at item 9: {}; 1 title not run", reason.text)});
    }
    SECTION("nothing left to run") {
        result.titles.resize(9);
        CHECK(md::batch_closing_message(result, false, BatchSource::mdlist) ==
              std::optional{std::format("stopped at item 9: {}", reason.text)});
        CHECK(md::batch_closing_message(result, false, BatchSource::csv) ==
              std::optional{std::format("stopped at line 9: {}", reason.text)});
    }
}

TEST_CASE("batch source: nothing when every item is ok or a duplicate, or on an interrupt",
          "[batch][source]") {
    md::BatchResult result{};
    result.titles = {row(1, TitleStatus::ok), row(2, TitleStatus::duplicate)};
    CHECK(!md::batch_closing_message(result, false, BatchSource::mdlist).has_value());
    CHECK(!md::batch_closing_message(result, true, BatchSource::mdlist).has_value());
    result.titles.push_back(row(3, TitleStatus::not_run));
    result.stop = md::BatchStop::interrupted;
    result.stopped_at_line = 3;
    CHECK(!md::batch_closing_message(result, false, BatchSource::mdlist).has_value());
}

// ---- AC12, unit half: an interrupt during item 1 of 2 ------------------------------------------

TEST_CASE("batch source: an interrupt during item 1 of 2 never runs item 2",
          "[batch][source][interrupt]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = list_jobs(2);
    FakeRunner runner{{md::RowOutcome{}, md::RowOutcome{}}, 1};
    const md::BatchResult result = md::run_batch(jobs, runner, false);

    CHECK(runner.ran() == std::vector<std::size_t>{1});
    CHECK(result.stop == md::BatchStop::interrupted);
    CHECK(result.stopped_at_line == 2);
    REQUIRE(result.titles.size() == 2);
    CHECK(result.titles[1].status == TitleStatus::not_run);
    CHECK(result.titles[1].line == 2);
    CHECK(!md::batch_closing_message(result, false, BatchSource::mdlist).has_value());
    CHECK(!md::batch_closing_message(result, true, BatchSource::mdlist).has_value());
    CHECK(md::batch_exit_code(result) == md::exit_code::failure);
}

// ---- AC39, core half: the effective dir and a row-scoped --dir-per-title failure ---------------

TEST_CASE("batch source: a non-fatal row_dir failure keeps its effective dir; the next item runs",
          "[batch][source]") {
    const InterruptFlagReset reset;
    const std::vector<md::BatchJob> jobs = list_jobs(2);
    const md::RowOutcome row_dir{
        .exit_code = 1,
        .error = "cannot open directory 'out/X': Not a directory",
        .dir = "out/X",
    };

    SECTION("{failed, ok}: the run continues, exit 1") {
        FakeRunner runner{{row_dir, md::RowOutcome{.dir = "out/Clean Title"}}, std::nullopt};
        const md::BatchResult result = md::run_batch(jobs, runner, false);
        CHECK(runner.ran() == std::vector<std::size_t>{1, 2});
        CHECK(result.stop == md::BatchStop::none);
        REQUIRE(result.titles.size() == 2);
        CHECK(result.titles[0].status == TitleStatus::failed);
        CHECK(result.titles[0].exit_code == std::optional<int>{1});
        CHECK(result.titles[0].dir == "out/X");
        CHECK(result.titles[0].error ==
              std::optional<std::string>{"cannot open directory 'out/X': Not a directory"});
        CHECK(result.titles[1].status == TitleStatus::ok);
        CHECK(result.titles[1].dir == "out/Clean Title");
        CHECK(md::batch_exit_code(result) == md::exit_code::failure);
        CHECK(md::batch_closing_message(result, false, BatchSource::mdlist) ==
              std::optional<std::string>{
                  "1 of 2 titles did not complete (item 1); see the errors above"});
    }
    SECTION("{failed, 4} -> 7") {
        FakeRunner runner{{row_dir, md::RowOutcome{.exit_code = 4}}, std::nullopt};
        const md::BatchResult result = md::run_batch(jobs, runner, false);
        CHECK(runner.ran() == std::vector<std::size_t>{1, 2});
        REQUIRE(result.titles.size() == 2);
        CHECK(result.titles[0].dir == "out/X");
        // No effective dir reported (exit 4 before the plan): the job's dir.
        CHECK(result.titles[1].status == TitleStatus::not_found);
        CHECK(result.titles[1].dir == "out");
        CHECK(md::batch_exit_code(result) == md::exit_code::mixed_failures);
    }
}

TEST_CASE("batch source: without an effective dir every row reports the job's dir",
          "[batch][source]") {
    const InterruptFlagReset reset;
    std::vector<md::BatchJob> jobs = list_jobs(3);
    jobs[2].duplicate_of = 1;
    jobs[2].options.manga = jobs[0].options.manga;
    FakeRunner runner{{md::RowOutcome{}, md::RowOutcome{.exit_code = 5}}, std::nullopt};
    const md::BatchResult result = md::run_batch(jobs, runner, false);
    REQUIRE(result.titles.size() == 3);
    for (const md::TitleResult& title : result.titles) {
        CAPTURE(title.line);
        CHECK(title.dir == "out");
    }
    CHECK(result.titles[2].status == TitleStatus::duplicate);
}
