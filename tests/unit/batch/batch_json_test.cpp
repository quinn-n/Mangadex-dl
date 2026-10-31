// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch/batch_json_test.cpp: the `batch --json` document (batch plan T3b, AC8). The
// document is asserted key by key after an order-preserving parse: exactly the listed keys, in
// order, with their types and nulls; each row's `report` re-dumped standalone is byte-identical to
// `download --json` for that report; invalid UTF-8 becomes U+FFFD; no ESC, DEL or C1 byte; the
// empty batch; and (rev1) the parsed document holds no floating-point number anywhere.
#include "json_output.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "support/json_bytes.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
using OrderedJson = nlohmann::ordered_json;
using md::TitleStatus;

constexpr std::string_view manga_id = "0a1b2c3d-0000-4000-8000-000000000001";
constexpr std::string_view other_id = "0a1b2c3d-0000-4000-8000-000000000002";
constexpr std::string_view replacement = "\xEF\xBF\xBD"; // U+FFFD

[[nodiscard]] md::Chapter chapter(std::string id, std::optional<std::string> volume,
                                  std::optional<std::string> number, int pages) {
    return md::Chapter{
        .id = std::move(id),
        .volume = std::move(volume),
        .chapter = std::move(number),
        .title = "A title",
        .language = "en",
        .groups = {md::ScanGroup{.id = "0000000a-0002-4000-8000-000000000001", .name = "Group"}},
        .pages = pages,
    };
}

/// A report with every kind of value the download document holds: written, exists and failed
/// files, nullable volume/chapter, skipped chapters of each reason, big byte counts.
[[nodiscard]] md::RunReport full_report(std::string title) {
    const md::PlannedEpub volume{
        .kind = md::EpubKind::volume,
        .file_name = "T - Vol. 01.epub",
        .volume = "1",
        .chapters = {chapter("0000000c-0001-4000-8000-000000000001", "1", "1", 3),
                     chapter("0000000c-0001-4000-8000-000000000002", "1", "2.5", 2)},
        .planned_pages = 5,
    };
    const md::PlannedEpub oneshot{
        .kind = md::EpubKind::oneshot,
        .file_name = "T - Oneshot.epub",
        .chapters = {chapter("0000000c-0001-4000-8000-000000000003", std::nullopt, std::nullopt,
                             1)},
        .planned_pages = 1,
    };
    return md::RunReport{
        .layout = md::Layout::volumes,
        .manga =
            md::MangaSummary{
                .id = std::string{manga_id},
                .title = std::move(title),
                .url = std::format("https://mangadex.org/title/{}", manga_id),
                .language = "en",
                .direction = md::Direction::rtl,
            },
        .epubs =
            {
                md::EpubOutcome{.epub = volume,
                                .path = "out/T - Vol. 01.epub",
                                .status = md::EpubStatus::written,
                                .replaced = true,
                                .pages = 5,
                                .bytes = std::uint64_t{1} << 40U},
                md::EpubOutcome{.epub = oneshot,
                                .path = "out/T - Oneshot.epub",
                                .status = md::EpubStatus::exists},
                md::EpubOutcome{.epub = oneshot,
                                .path = "out/T - Oneshot (2).epub",
                                .status = md::EpubStatus::failed,
                                .error = "network: timed out"},
            },
        .skipped =
            {
                md::SkippedChapter{.id = "0000000c-0001-4000-8000-000000000004",
                                   .volume = "1",
                                   .chapter = "3",
                                   .reason = md::SkipReason::external},
                md::SkippedChapter{.id = "0000000c-0001-4000-8000-000000000005",
                                   .chapter = "4",
                                   .reason = md::SkipReason::duplicate,
                                   .kept = "0000000c-0001-4000-8000-000000000001"},
                md::SkippedChapter{.id = "0000000c-0001-4000-8000-000000000006",
                                   .reason = md::SkipReason::no_pages},
                md::SkippedChapter{.id = "0000000c-0001-4000-8000-000000000007",
                                   .reason = md::SkipReason::unavailable},
            },
    };
}

/// One row of every shape: ok with a report, failed (row-scoped dir) without one, a duplicate and
/// a not-run row.
[[nodiscard]] md::BatchResult mixed_result(const md::RunReport& report) {
    md::BatchResult result{};
    result.titles = {
        md::TitleResult{.line = 2,
                        .manga_id = std::string{manga_id},
                        .dir = "out",
                        .status = TitleStatus::incomplete,
                        .exit_code = 6,
                        .error = "1 of 3 files failed",
                        .files = md::summarize(report),
                        .report = report},
        md::TitleResult{.line = 3,
                        .manga_id = std::string{other_id},
                        .dir = "out/blocked/x",
                        .status = TitleStatus::failed,
                        .exit_code = 1,
                        .error = "cannot open directory 'out/blocked': Not a directory"},
        md::TitleResult{.line = 4,
                        .manga_id = std::string{manga_id},
                        .dir = "out",
                        .status = TitleStatus::duplicate,
                        .duplicate_of = 2},
        md::TitleResult{.line = 5,
                        .manga_id = std::string{other_id},
                        .dir = "out/sub",
                        .status = TitleStatus::not_run},
    };
    return result;
}

/// Parses with key order kept; strict UTF-8 (no replacement), so invalid UTF-8 in the output
/// fails the test.
[[nodiscard]] OrderedJson parse(const std::string& text) {
    OrderedJson doc = OrderedJson::parse(text, nullptr, false);
    REQUIRE(!doc.is_discarded());
    return doc;
}

[[nodiscard]] std::vector<std::string> keys(const OrderedJson& object) {
    REQUIRE(object.is_object());
    std::vector<std::string> out{};
    for (const auto& [key, value] : object.items()) {
        out.push_back(key);
    }
    return out;
}

/// The re-dump technique of fact-check V-C2: a subtree dumped standalone like report_to_json. It
/// must go through dump_document: the parsed embedded report holds the real DEL and C1 bytes again,
/// so a plain dump() would print them raw (json-c1 fact-check V18).
[[nodiscard]] std::string redump(const OrderedJson& value) {
    return md::json_output::dump_document(value);
}

/// Counts the number_float nodes anywhere in `doc` (iterative, no recursion).
[[nodiscard]] std::size_t float_nodes(const OrderedJson& doc) {
    std::size_t floats = 0;
    std::vector<const OrderedJson*> stack{&doc};
    while (!stack.empty()) {
        const OrderedJson* node = stack.back();
        stack.pop_back();
        if (node->is_number_float()) {
            ++floats;
        }
        if (node->is_structured()) {
            for (const OrderedJson& child : *node) {
                stack.push_back(&child);
            }
        }
    }
    return floats;
}

/// The keys of one `titles[]` entry, in order.
[[nodiscard]] std::vector<std::string> title_keys() {
    return {"line", "manga_id", "dir", "status", "exit_code", "error", "duplicate_of", "report"};
}

} // namespace

TEST_CASE("title_status_name: the JSON spellings", "[batch][json]") {
    CHECK(md::title_status_name(TitleStatus::ok) == "ok");
    CHECK(md::title_status_name(TitleStatus::not_found) == "not_found");
    CHECK(md::title_status_name(TitleStatus::no_chapters) == "no_chapters");
    CHECK(md::title_status_name(TitleStatus::incomplete) == "incomplete");
    CHECK(md::title_status_name(TitleStatus::network) == "network");
    CHECK(md::title_status_name(TitleStatus::failed) == "failed");
    CHECK(md::title_status_name(TitleStatus::duplicate) == "duplicate");
    CHECK(md::title_status_name(TitleStatus::not_run) == "not_run");
}

TEST_CASE("batch_to_json: exactly the listed keys, in order, with their types", "[batch][json]") {
    const md::RunReport report = full_report("Test Manga");
    const md::BatchResult result = mixed_result(report);
    const std::string text = md::batch_to_json(result, "titles.csv", false);

    REQUIRE(!text.empty());
    CHECK(text.back() == '\n');
    CHECK(text.starts_with("{\n  \"format_version\": 1,\n  \"dry_run\": false,\n"));

    const OrderedJson doc = parse(text);
    CHECK(keys(doc) ==
          std::vector<std::string>{"format_version", "dry_run", "source", "titles", "summary"});
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("dry_run") == false);
    CHECK(doc.at("source") == "titles.csv");

    const OrderedJson& titles = doc.at("titles");
    REQUIRE(titles.is_array());
    REQUIRE(titles.size() == 4);
    for (const OrderedJson& row : titles) {
        CHECK(keys(row) == title_keys());
        CHECK(row.at("line").is_number_unsigned());
        CHECK(row.at("manga_id").is_string());
        CHECK(row.at("dir").is_string());
        CHECK(row.at("status").is_string());
        CHECK((row.at("exit_code").is_number_integer() || row.at("exit_code").is_null()));
        CHECK((row.at("error").is_string() || row.at("error").is_null()));
        CHECK((row.at("duplicate_of").is_number_unsigned() || row.at("duplicate_of").is_null()));
        CHECK((row.at("report").is_object() || row.at("report").is_null()));
    }

    const OrderedJson& first = titles.at(0);
    CHECK(first.at("line") == 2);
    CHECK(first.at("manga_id") == manga_id);
    CHECK(first.at("dir") == "out");
    CHECK(first.at("status") == "incomplete");
    CHECK(first.at("exit_code") == 6);
    CHECK(first.at("error") == "1 of 3 files failed");
    CHECK(first.at("duplicate_of").is_null());
    CHECK(first.at("report").is_object());

    const OrderedJson& failed = titles.at(1);
    CHECK(failed.at("status") == "failed");
    CHECK(failed.at("exit_code") == 1);
    CHECK(failed.at("error") == "cannot open directory 'out/blocked': Not a directory");
    CHECK(failed.at("report").is_null());

    const OrderedJson& duplicate = titles.at(2);
    CHECK(duplicate.at("status") == "duplicate");
    CHECK(duplicate.at("exit_code").is_null());
    CHECK(duplicate.at("error").is_null());
    CHECK(duplicate.at("duplicate_of") == 2);
    CHECK(duplicate.at("report").is_null());

    const OrderedJson& not_run = titles.at(3);
    CHECK(not_run.at("status") == "not_run");
    CHECK(not_run.at("dir") == "out/sub");
    CHECK(not_run.at("exit_code").is_null());
    CHECK(not_run.at("error").is_null());
    CHECK(not_run.at("duplicate_of").is_null());
    CHECK(not_run.at("report").is_null());

    const OrderedJson& summary = doc.at("summary");
    CHECK(keys(summary) == std::vector<std::string>{"titles", "files"});
    const OrderedJson& counts = summary.at("titles");
    CHECK(keys(counts) == std::vector<std::string>{"total", "ok", "not_found", "no_chapters",
                                                   "incomplete", "network", "failed", "duplicate",
                                                   "not_run"});
    CHECK(counts.at("total") == 4);
    CHECK(counts.at("ok") == 0);
    CHECK(counts.at("not_found") == 0);
    CHECK(counts.at("no_chapters") == 0);
    CHECK(counts.at("incomplete") == 1);
    CHECK(counts.at("network") == 0);
    CHECK(counts.at("failed") == 1);
    CHECK(counts.at("duplicate") == 1);
    CHECK(counts.at("not_run") == 1);
    const OrderedJson& files = summary.at("files");
    CHECK(keys(files) == std::vector<std::string>{"planned", "written", "exists", "failed"});
    CHECK(files.at("planned") == 0);
    CHECK(files.at("written") == 1);
    CHECK(files.at("exists") == 1);
    CHECK(files.at("failed") == 1);
}

TEST_CASE("batch_to_json: summary.files sums every row's files", "[batch][json]") {
    md::BatchResult result{};
    result.titles = {
        md::TitleResult{.line = 2,
                        .status = TitleStatus::ok,
                        .exit_code = 0,
                        .files = {.planned = 3, .written = 0, .exists = 1, .failed = 0}},
        md::TitleResult{.line = 3,
                        .status = TitleStatus::incomplete,
                        .exit_code = 6,
                        .files = {.planned = 2, .written = 5, .exists = 0, .failed = 4}},
        md::TitleResult{.line = 4, .status = TitleStatus::ok, .exit_code = 0},
    };
    const OrderedJson doc = parse(md::batch_to_json(result, "-", true));
    CHECK(doc.at("dry_run") == true);
    CHECK(doc.at("source") == "-");
    CHECK(doc.at("summary").at("titles").at("total") == 3);
    CHECK(doc.at("summary").at("titles").at("ok") == 2);
    CHECK(doc.at("summary").at("titles").at("incomplete") == 1);
    const OrderedJson& files = doc.at("summary").at("files");
    CHECK(files.at("planned") == 5);
    CHECK(files.at("written") == 5);
    CHECK(files.at("exists") == 1);
    CHECK(files.at("failed") == 4);
}

TEST_CASE("batch_to_json: one row of each status counts once under its own key", "[batch][json]") {
    // Each status exactly once: a swapped increment in the per-status counter would put a 2 and a
    // 0 somewhere. The files differ per row and per field, so a swapped field sum shows too.
    const std::vector<std::pair<TitleStatus, std::string>> statuses{
        {TitleStatus::ok, "ok"},
        {TitleStatus::not_found, "not_found"},
        {TitleStatus::no_chapters, "no_chapters"},
        {TitleStatus::incomplete, "incomplete"},
        {TitleStatus::network, "network"},
        {TitleStatus::failed, "failed"},
        {TitleStatus::duplicate, "duplicate"},
        {TitleStatus::not_run, "not_run"},
    };
    md::BatchResult result{};
    std::size_t line = 1;
    for (const auto& [status, name] : statuses) {
        ++line;
        result.titles.push_back(md::TitleResult{.line = line,
                                                .status = status,
                                                .files = {.planned = line,
                                                          .written = 10 * line,
                                                          .exists = 100 * line,
                                                          .failed = 1000 * line}});
    }
    const OrderedJson doc = parse(md::batch_to_json(result, "titles.csv", false));

    const OrderedJson& titles = doc.at("titles");
    REQUIRE(titles.size() == statuses.size());
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        CHECK(titles.at(i).at("status") == statuses[i].second);
    }

    const OrderedJson& counts = doc.at("summary").at("titles");
    CHECK(counts.size() == 9);
    CHECK(counts.at("total") == 8);
    for (const auto& [status, name] : statuses) {
        INFO(name);
        CHECK(counts.at(name) == 1);
    }

    // Lines 2..9 sum to 44.
    const OrderedJson& files = doc.at("summary").at("files");
    CHECK(files.at("planned") == 44);
    CHECK(files.at("written") == 440);
    CHECK(files.at("exists") == 4400);
    CHECK(files.at("failed") == 44000);
}

TEST_CASE("batch_to_json: each report is byte-identical to download's document", "[batch][json]") {
    const bool hostile = GENERATE(false, true);
    INFO("hostile title: " << hostile);
    // Invalid UTF-8 and an ESC in the title: download's document replaces/escapes them, and the
    // embedded copy must round-trip to the same bytes.
    const md::RunReport report = full_report(
        hostile ? std::string{"Bad \xff\xfe title \x1b[31m red \x7f\xc2\x9b[0m"} : "Clean Title");
    const md::BatchResult result = mixed_result(report);
    const OrderedJson doc = parse(md::batch_to_json(result, "titles.csv", false));
    const OrderedJson& embedded = doc.at("titles").at(0).at("report");
    CHECK(redump(embedded) == md::report_to_json(report));
}

TEST_CASE("batch_to_json: invalid UTF-8 in source and dir gives valid JSON with U+FFFD",
          "[batch][json]") {
    md::BatchResult result{};
    result.titles = {md::TitleResult{.line = 2,
                                     .manga_id = std::string{manga_id},
                                     .dir = std::string{"out/\xfe"
                                                        "dir"},
                                     .status = TitleStatus::ok,
                                     .exit_code = 0,
                                     .error = std::string{"bad \xc3 byte"}}};
    const std::string text = md::batch_to_json(result, "list\xff.csv", false);
    const OrderedJson doc = parse(text); // strict: fails on any invalid UTF-8
    CHECK(doc.at("source") == std::format("list{}.csv", replacement));
    CHECK(doc.at("titles").at(0).at("dir") == std::format("out/{}dir", replacement));
    CHECK(doc.at("titles").at(0).at("error") == std::format("bad {} byte", replacement));
}

TEST_CASE("batch_to_json: never an ESC, DEL or C1 byte", "[batch][json]") {
    // C1: U+009B (the 8-bit CSI), U+009D (OSC), U+0085 (NEL), U+0080 and U+009F (the range ends).
    const std::string title = "Evil \x1b[2J\x1b]0;title\x07 manga \x7f end";
    const std::string source = "\x1b[32mlist\xc2\x9b"
                               "1m.csv";
    const std::string dir = "out/\x1b[31mred\xc2\x9d"
                            "0;t\xc2\x85";
    const std::string error = "\x1b[1mbold\x1b[0m\xc2\x80\xc2\x9f";
    const md::RunReport report = full_report(title);
    md::BatchResult result = mixed_result(report);
    result.titles.at(1).dir = dir;
    result.titles.at(1).error = error;
    const std::string text = md::batch_to_json(result, source, false);
    CHECK(!text.contains('\x1b'));
    CHECK(!text.contains('\x07'));
    CHECK(!md::test::has_raw_del_or_c1(text));
    CHECK(text.contains("list\\u009b1m.csv"));
    CHECK(text.contains("red\\u009d0;t\\u0085"));
    CHECK(text.contains("\\u0080\\u009f"));
    CHECK(text.contains("manga \\u007f end"));
    const OrderedJson doc = parse(text);
    // The bytes survive as \u001b / \u007f / \u0080..\u009f escapes.
    CHECK(doc.at("source") == source);
    CHECK(doc.at("titles").at(1).at("dir") == dir);
    CHECK(doc.at("titles").at(1).at("error") == error);
    CHECK(doc.at("titles").at(0).at("report").at("manga").at("title") == title);
}

TEST_CASE("batch_to_json: the empty batch", "[batch][json]") {
    const std::string text = md::batch_to_json(md::BatchResult{}, "-", false);
    CHECK(text.contains("\"titles\": []"));
    const OrderedJson doc = parse(text);
    CHECK(doc.at("titles") == OrderedJson::array());
    const OrderedJson& counts = doc.at("summary").at("titles");
    REQUIRE(counts.size() == 9);
    for (const auto& [key, value] : counts.items()) {
        INFO(key);
        CHECK(value == 0);
    }
    const OrderedJson& files = doc.at("summary").at("files");
    REQUIRE(files.size() == 4);
    for (const auto& [key, value] : files.items()) {
        INFO(key);
        CHECK(value == 0);
    }
}

TEST_CASE("batch_to_json: no floating-point number anywhere (the V-C2 caveat guard)",
          "[batch][json]") {
    // The byte-equal round trip is exact for strings, integers, booleans, arrays, objects and
    // null; a double could re-serialize differently. Neither document may contain one.
    const md::RunReport report = full_report("Test Manga");
    const OrderedJson doc = parse(md::batch_to_json(mixed_result(report), "titles.csv", false));
    CHECK(float_nodes(doc) == 0);
    CHECK(float_nodes(parse(md::report_to_json(report))) == 0);
    // The walk does see nested nodes: the control below has exactly one float.
    CHECK(float_nodes(OrderedJson::parse(R"({"a":[1,{"b":2.5}]})")) == 1);
}
