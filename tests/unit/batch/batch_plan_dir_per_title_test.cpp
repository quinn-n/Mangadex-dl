// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch/batch_plan_dir_per_title_test.cpp: `batch --dir-per-title` in phase 1
// (batch-dir-per-title plan T1). validate_batch_args copies the flag and never sets it on the
// CLI-level base options; plan_batch sets DownloadOptions::dir_per_title only on rows without a
// `dir` cell, so an explicit cell replaces the per-title directory (spec QB1, AC5); with the flag
// off every job is what it was (AC1); and the dedupe sees the flag (AC5). Lists come from the CSV
// parser (the committed shapes) or are built by hand.
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/options.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;

constexpr std::string_view uuid_a = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view uuid_b = "77777777-7777-4777-8777-777777777777";

[[nodiscard]] md::RawBatchArgs batch_args(bool dir_per_title) {
    md::RawBatchArgs raw{.file = "titles.csv", .dir_per_title = dir_per_title};
    raw.defaults.dir = "out";
    return raw;
}

[[nodiscard]] md::BatchSettings settings_for(const md::RawBatchArgs& raw) {
    md::Result<md::BatchSettings> settings = md::validate_batch_args(raw);
    REQUIRE(settings.has_value());
    return std::move(*settings);
}

[[nodiscard]] md::TitleList parsed(std::string_view csv) {
    md::ListResult<md::TitleList> list = md::parse_title_list(csv, "titles.csv");
    REQUIRE(list.has_value());
    return std::move(*list);
}

[[nodiscard]] md::ListRecord record(std::size_t line, std::string_view manga = uuid_a) {
    return md::ListRecord{.line = line, .manga = std::string{manga}};
}

[[nodiscard]] md::TitleList list_of(std::vector<md::ListRecord> records) {
    return md::TitleList{.has_header = true, .records = std::move(records)};
}

[[nodiscard]] std::vector<md::BatchJob> plan_ok(const md::TitleList& list,
                                                const md::BatchSettings& settings) {
    md::ListResult<std::vector<md::BatchJob>> jobs = md::plan_batch(list, settings);
    if (!jobs) {
        for (const std::string& message : jobs.error().messages) {
            UNSCOPED_INFO(message);
        }
    }
    REQUIRE(jobs.has_value());
    return std::move(*jobs);
}

/// Every member of the merged strings (RawDownloadArgs has no operator==), as references.
[[nodiscard]] auto raw_fields(const md::RawDownloadArgs& raw) {
    return std::tie(raw.manga, raw.dir, raw.volumes, raw.loose_chapters, raw.layout, raw.lang,
                    raw.groups, raw.data_saver, raw.direction, raw.title, raw.overwrite,
                    raw.dry_run, raw.jobs, raw.api_base, raw.no_report);
}

/// Every member of two jobs (BatchJob has no operator==).
void check_same_job(const md::BatchJob& actual, const md::BatchJob& expected) {
    CAPTURE(expected.line);
    CHECK(actual.line == expected.line);
    CHECK(raw_fields(actual.raw) == raw_fields(expected.raw));
    CHECK(actual.options == expected.options);
    CHECK(actual.dir == expected.dir);
    CHECK(actual.duplicate_of == expected.duplicate_of);
    CHECK(actual.sub_dir == expected.sub_dir);
}

/// The committed CSV shapes: headerless, a `dir` column, a `title` column, ragged rows.
constexpr std::string_view headerless_csv = "11111111-1111-4111-8111-111111111111\n"
                                            "77777777-7777-4777-8777-777777777777\n";
constexpr std::string_view dir_csv = "manga,dir\n"
                                     "11111111-1111-4111-8111-111111111111,series/clean\n"
                                     "77777777-7777-4777-8777-777777777777,\n"
                                     "11111111-1111-4111-8111-111111111111,   \n";
constexpr std::string_view title_csv = "manga,title\n"
                                       "11111111-1111-4111-8111-111111111111,Other Title\n"
                                       "11111111-1111-4111-8111-111111111111,\n";
constexpr std::string_view ragged_csv = "manga,title,dir,lang\n"
                                        "11111111-1111-4111-8111-111111111111,T\n"
                                        "77777777-7777-4777-8777-777777777777,,,\n"
                                        "11111111-1111-4111-8111-111111111111,,a/b,fr\n"
                                        "11111111-1111-4111-8111-111111111111\n";

} // namespace

// ---- AC5: validate_batch_args ----------------------------------------------------------------

TEST_CASE("batch plan --dir-per-title: validate_batch_args copies the flag, never onto base",
          "[batch][plan][dir_per_title]") {
    const md::BatchSettings off = settings_for(batch_args(false));
    CHECK_FALSE(off.dir_per_title);
    CHECK_FALSE(off.base.dir_per_title);
    CHECK(off.source == md::BatchSource::csv);

    const md::BatchSettings on = settings_for(batch_args(true));
    CHECK(on.dir_per_title);
    CHECK_FALSE(on.base.dir_per_title);
    CHECK(on.source == md::BatchSource::csv);
    // Nothing else differs.
    CHECK(on.base == off.base);
    CHECK(raw_fields(on.defaults) == raw_fields(off.defaults));
    CHECK(on.label == off.label);
    CHECK(on.fail_fast == off.fail_fast);

    md::RawBatchArgs untouched{.file = "titles.csv"};
    untouched.defaults.dir = "out";
    CHECK_FALSE(untouched.dir_per_title);
    CHECK_FALSE(settings_for(untouched).dir_per_title);
}

// ---- AC1: the flag off changes nothing -------------------------------------------------------

TEST_CASE("batch plan --dir-per-title: with the flag off, every job is as without it",
          "[batch][plan][dir_per_title]") {
    const std::string_view csv = GENERATE(headerless_csv, dir_csv, title_csv, ragged_csv);
    CAPTURE(csv);
    const md::TitleList list = parsed(csv);
    md::RawBatchArgs defaulted{.file = "titles.csv"};
    defaulted.defaults.dir = "out";
    const std::vector<md::BatchJob> today = plan_ok(list, settings_for(defaulted));
    const std::vector<md::BatchJob> off = plan_ok(list, settings_for(batch_args(false)));
    const std::vector<md::BatchJob> on = plan_ok(list, settings_for(batch_args(true)));
    REQUIRE(off.size() == today.size());
    REQUIRE(on.size() == today.size());
    for (std::size_t i = 0; i < today.size(); ++i) {
        CHECK_FALSE(today[i].options.dir_per_title);
        check_same_job(off[i], today[i]);
        // The flag touches only options.dir_per_title, and only where the row has no cell.
        CHECK(on[i].options.dir_per_title == today[i].sub_dir.empty());
        md::BatchJob on_without_flag = on[i];
        on_without_flag.options.dir_per_title = false;
        check_same_job(on_without_flag, today[i]);
    }
}

// ---- AC5: which rows take the per-title step ---------------------------------------------------

TEST_CASE("batch plan --dir-per-title: a row without a dir cell takes the per-title step",
          "[batch][plan][dir_per_title]") {
    const md::ListRecord absent = record(2);
    md::ListRecord empty = record(3, uuid_b);
    empty.dir = std::string{};
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({absent, empty}), settings_for(batch_args(true)));
    REQUIRE(jobs.size() == 2);
    for (const md::BatchJob& job : jobs) {
        CAPTURE(job.line);
        CHECK(job.options.dir_per_title);
        CHECK(job.dir == "out");
        CHECK(job.options.dir == std::filesystem::path{"out"});
        CHECK(job.sub_dir.empty());
        CHECK_FALSE(job.duplicate_of.has_value());
    }
}

TEST_CASE("batch plan --dir-per-title: an all-spaces dir cell from the CSV is a derived row",
          "[batch][plan][dir_per_title]") {
    const std::vector<md::BatchJob> jobs =
        plan_ok(parsed("manga,dir\n11111111-1111-4111-8111-111111111111,   \n"),
                settings_for(batch_args(true)));
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].options.dir_per_title);
    CHECK(jobs[0].dir == "out");
    CHECK(jobs[0].sub_dir.empty());
}

TEST_CASE("batch plan --dir-per-title: a hand-built all-spaces dir cell is still a plan error",
          "[batch][plan][dir_per_title]") {
    md::ListRecord spaces = record(2);
    spaces.dir = "   ";
    const bool flag = GENERATE(false, true);
    CAPTURE(flag);
    const md::ListResult<std::vector<md::BatchJob>> jobs =
        md::plan_batch(list_of({spaces}), settings_for(batch_args(flag)));
    // REQUIRE(!x) rather than REQUIRE_FALSE, as in batch_plan_test.cpp (clang-analyzer).
    REQUIRE(!jobs.has_value());
    CHECK(jobs.error().messages ==
          std::vector<std::string>{"titles.csv:2: dir: must not be empty"});
    CHECK(jobs.error().more == 0);
}

TEST_CASE("batch plan --dir-per-title: an explicit dir cell replaces the per-title directory",
          "[batch][plan][dir_per_title]") {
    const std::string cell =
        GENERATE(std::string{"series/clean"}, std::string{"Clean Title"}, std::string{"a/b"});
    CAPTURE(cell);
    md::ListRecord row = record(4);
    row.dir = cell;
    const std::vector<md::BatchJob> jobs = plan_ok(list_of({row}), settings_for(batch_args(true)));
    REQUIRE(jobs.size() == 1);
    const md::BatchJob& job = jobs.front();
    CHECK_FALSE(job.options.dir_per_title);
    CHECK(job.dir == md::join_list_dir("out", cell));
    CHECK(job.options.dir == std::filesystem::path{md::join_list_dir("out", cell)});
    CHECK(job.sub_dir == cell);
}

TEST_CASE("batch plan --dir-per-title: a trimmed explicit cell is still explicit",
          "[batch][plan][dir_per_title]") {
    md::ListRecord row = record(2);
    row.dir = " a ";
    const std::vector<md::BatchJob> jobs = plan_ok(list_of({row}), settings_for(batch_args(true)));
    REQUIRE(jobs.size() == 1);
    CHECK_FALSE(jobs[0].options.dir_per_title);
    CHECK(jobs[0].sub_dir == "a");
    CHECK(jobs[0].dir == "out/a");
}

// ---- AC5: dedupe --------------------------------------------------------------------------------

TEST_CASE("batch plan --dir-per-title: two identical derived rows are duplicates",
          "[batch][plan][dir_per_title][dup]") {
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({record(2), record(3, uuid_b), record(4)}), settings_for(batch_args(true)));
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[0].options.dir_per_title);
    CHECK(jobs[2].options.dir_per_title);
    CHECK_FALSE(jobs[0].duplicate_of.has_value());
    CHECK_FALSE(jobs[1].duplicate_of.has_value());
    CHECK(jobs[2].duplicate_of == std::optional<std::size_t>{2});
}

TEST_CASE("batch plan --dir-per-title: a derived and an explicit row of one manga both run",
          "[batch][plan][dir_per_title][dup]") {
    md::ListRecord explicit_row = record(2);
    explicit_row.dir = "Clean Title";
    const md::ListRecord derived = record(3);
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({explicit_row, derived}), settings_for(batch_args(true)));
    REQUIRE(jobs.size() == 2);
    CHECK_FALSE(jobs[0].options.dir_per_title);
    CHECK(jobs[1].options.dir_per_title);
    CHECK_FALSE(jobs[0].duplicate_of.has_value());
    CHECK_FALSE(jobs[1].duplicate_of.has_value());
}

TEST_CASE("batch plan --dir-per-title: two identical explicit rows are duplicates, as without it",
          "[batch][plan][dir_per_title][dup]") {
    md::ListRecord first = record(2);
    first.dir = "series/clean";
    md::ListRecord second = record(5);
    second.dir = "series/clean";
    const bool flag = GENERATE(false, true);
    CAPTURE(flag);
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({first, second}), settings_for(batch_args(flag)));
    REQUIRE(jobs.size() == 2);
    CHECK_FALSE(jobs[1].options.dir_per_title);
    CHECK_FALSE(jobs[0].duplicate_of.has_value());
    CHECK(jobs[1].duplicate_of == std::optional<std::size_t>{2});
}
