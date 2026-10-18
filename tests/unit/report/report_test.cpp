// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/report/report_test.cpp: the run report (download plan T13): status counts, the
// exit-code priority and the --json document. The document is asserted key by key after parsing
// ("golden by assertion") for the success, dry-run, exit-5 and exit-6 shapes, plus the hostile
// cases: invalid UTF-8 and terminal escapes in titles.

#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace md = mangadex_dl;
using nlohmann::json;

constexpr std::string_view manga_id = "0a1b2c3d-0000-4000-8000-000000000001";
constexpr std::string_view chapter_one = "0000000c-0001-4000-8000-000000000001";
constexpr std::string_view chapter_two = "0000000c-0001-4000-8000-000000000002";
constexpr std::string_view chapter_dup = "0000000c-0001-4000-8000-000000000003";
constexpr std::string_view chapter_ext = "0000000c-0001-4000-8000-000000000004";
constexpr std::string_view group_id = "0000000a-0002-4000-8000-000000000001";

[[nodiscard]] md::MangaSummary summary_manga() {
    return md::MangaSummary{
        .id = std::string{manga_id},
        .title = "Test Manga",
        .url = std::string{"https://mangadex.org/title/"} + std::string{manga_id},
        .language = "en",
        .direction = md::Direction::rtl,
    };
}

[[nodiscard]] md::Chapter chapter(std::string_view id, std::string volume, std::string number,
                                  int pages) {
    return md::Chapter{
        .id = std::string{id},
        .volume = std::move(volume),
        .chapter = std::move(number),
        .title = "A title",
        .language = "en",
        .groups = {md::ScanGroup{.id = std::string{group_id}, .name = "Group One"}},
        .pages = pages,
    };
}

[[nodiscard]] md::EpubOutcome outcome(md::EpubStatus status, std::string path) {
    md::PlannedEpub epub{
        .kind = md::EpubKind::volume,
        .file_name = "Test Manga - Vol. 01.epub",
        .display_title = "Test Manga Vol. 1",
        .volume = "1",
        .chapters = {chapter(chapter_one, "1", "1", 3), chapter(chapter_two, "1", "2", 2)},
        .planned_pages = 5,
    };
    return md::EpubOutcome{
        .epub = std::move(epub),
        .path = std::move(path),
        .status = status,
        .pages = status == md::EpubStatus::failed ? 0U : 5U,
        .bytes = status == md::EpubStatus::written ? std::uint64_t{12345} : 0U,
    };
}

[[nodiscard]] md::RunReport base_report() {
    return md::RunReport{
        .layout = md::Layout::volumes,
        .manga = summary_manga(),
        .skipped =
            {
                md::SkippedChapter{.id = std::string{chapter_ext},
                                   .volume = "1",
                                   .chapter = "3",
                                   .reason = md::SkipReason::external},
                md::SkippedChapter{.id = std::string{chapter_dup},
                                   .volume = std::nullopt,
                                   .chapter = "1",
                                   .reason = md::SkipReason::duplicate,
                                   .kept = std::string{chapter_one}},
            },
    };
}

/// Parses the document strictly (no replacement), so invalid UTF-8 in the output fails the test.
[[nodiscard]] json parse(const std::string& text) {
    json doc = json::parse(text, nullptr, false);
    REQUIRE(!doc.is_discarded());
    return doc;
}

void check_summary(const json& doc, std::size_t planned, std::size_t written, std::size_t exists,
                   std::size_t failed) {
    const json& summary = doc.at("summary");
    CHECK(summary.size() == 4);
    CHECK(summary.at("planned") == planned);
    CHECK(summary.at("written") == written);
    CHECK(summary.at("exists") == exists);
    CHECK(summary.at("failed") == failed);
}

} // namespace

TEST_CASE("summarize counts every status", "[report]") {
    md::RunReport report = base_report();
    CHECK(md::summarize(report) == md::RunSummary{});
    report.epubs = {outcome(md::EpubStatus::written, "a"), outcome(md::EpubStatus::written, "b"),
                    outcome(md::EpubStatus::exists, "c"), outcome(md::EpubStatus::failed, "d"),
                    outcome(md::EpubStatus::planned, "e")};
    CHECK(md::summarize(report) ==
          md::RunSummary{.planned = 1, .written = 2, .exists = 1, .failed = 1});
}

TEST_CASE("exit_code_for: failed beats empty beats success; all-exists is success", "[report]") {
    md::RunReport report = base_report();
    CHECK(md::exit_code_for(report) == md::exit_code::no_chapters);

    report.epubs = {outcome(md::EpubStatus::exists, "a"), outcome(md::EpubStatus::exists, "b")};
    CHECK(md::exit_code_for(report) == md::exit_code::success);

    report.epubs = {outcome(md::EpubStatus::written, "a")};
    CHECK(md::exit_code_for(report) == md::exit_code::success);

    report.epubs = {outcome(md::EpubStatus::planned, "a")};
    CHECK(md::exit_code_for(report) == md::exit_code::success);

    report.epubs = {outcome(md::EpubStatus::written, "a"), outcome(md::EpubStatus::failed, "b")};
    CHECK(md::exit_code_for(report) == md::exit_code::incomplete);

    report.epubs = {outcome(md::EpubStatus::failed, "a")};
    CHECK(md::exit_code_for(report) == md::exit_code::incomplete);
}

TEST_CASE("report_to_json: success shape", "[report][json]") {
    md::RunReport report = base_report();
    md::EpubOutcome written = outcome(md::EpubStatus::written, "out/Test Manga - Vol. 01.epub");
    written.replaced = true;
    report.epubs = {written, outcome(md::EpubStatus::exists, "out/Test Manga - Vol. 02.epub")};

    const std::string text = md::report_to_json(report);
    REQUIRE(!text.empty());
    CHECK(text.back() == '\n');
    CHECK(!text.contains('\x1b'));
    // Keys are written in the documented order.
    CHECK(text.starts_with("{\n  \"format_version\": 1,\n  \"dry_run\": false,\n"));

    const json doc = parse(text);
    REQUIRE(doc.is_object());
    CHECK(doc.size() == 7);
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("dry_run") == false);
    CHECK(doc.at("layout") == "volumes");

    const json& manga = doc.at("manga");
    CHECK(manga.size() == 5);
    CHECK(manga.at("id") == manga_id);
    CHECK(manga.at("title") == "Test Manga");
    CHECK(manga.at("url") == "https://mangadex.org/title/" + std::string{manga_id});
    CHECK(manga.at("language") == "en");
    CHECK(manga.at("direction") == "rtl");

    const json& epubs = doc.at("epubs");
    REQUIRE(epubs.is_array());
    REQUIRE(epubs.size() == 2);
    const json& first = epubs.at(0);
    CHECK(first.size() == 10);
    CHECK(first.at("path") == "out/Test Manga - Vol. 01.epub");
    CHECK(first.at("kind") == "volume");
    CHECK(first.at("volume") == "1");
    CHECK(first.at("chapter").is_null());
    CHECK(first.at("status") == "written");
    CHECK(first.at("replaced") == true);
    CHECK(first.at("pages") == 5);
    CHECK(first.at("bytes") == 12345);
    CHECK(first.at("error").is_null());
    const json& chapters = first.at("chapters");
    REQUIRE(chapters.size() == 2);
    const json& ch = chapters.at(0);
    CHECK(ch.size() == 6);
    CHECK(ch.at("id") == chapter_one);
    CHECK(ch.at("volume") == "1");
    CHECK(ch.at("chapter") == "1");
    CHECK(ch.at("title") == "A title");
    CHECK(ch.at("pages") == 3);
    REQUIRE(ch.at("groups").size() == 1);
    CHECK(ch.at("groups").at(0) == json{{"id", group_id}, {"name", "Group One"}});

    const json& second = epubs.at(1);
    CHECK(second.at("status") == "exists");
    CHECK(second.at("replaced") == false);

    const json& skipped = doc.at("skipped_chapters");
    REQUIRE(skipped.size() == 2);
    CHECK(skipped.at(0) == json{{"id", chapter_ext},
                                {"volume", "1"},
                                {"chapter", "3"},
                                {"reason", "external"},
                                {"kept", nullptr}});
    CHECK(skipped.at(1) == json{{"id", chapter_dup},
                                {"volume", nullptr},
                                {"chapter", "1"},
                                {"reason", "duplicate"},
                                {"kept", chapter_one}});

    check_summary(doc, 0, 1, 1, 0);
}

TEST_CASE("report_to_json: dry-run shape", "[report][json]") {
    md::RunReport report = base_report();
    report.dry_run = true;
    report.layout = md::Layout::single;
    report.manga.direction = md::Direction::ltr;
    md::EpubOutcome planned = outcome(md::EpubStatus::planned, "Test Manga.epub");
    planned.epub.kind = md::EpubKind::single;
    planned.epub.volume = std::nullopt;
    report.epubs = {planned};

    const json doc = parse(md::report_to_json(report));
    CHECK(doc.at("dry_run") == true);
    CHECK(doc.at("layout") == "single");
    CHECK(doc.at("manga").at("direction") == "ltr");
    REQUIRE(doc.at("epubs").size() == 1);
    const json& epub = doc.at("epubs").at(0);
    CHECK(epub.at("kind") == "single");
    CHECK(epub.at("status") == "planned");
    CHECK(epub.at("volume").is_null());
    CHECK(epub.at("bytes") == 0);
    CHECK(epub.at("error").is_null());
    check_summary(doc, 1, 0, 0, 0);
    CHECK(md::exit_code_for(report) == md::exit_code::success);
}

TEST_CASE("report_to_json: exit-5 shape has an empty epubs array", "[report][json]") {
    md::RunReport report = base_report();
    report.layout = md::Layout::chapters;
    report.skipped.clear();

    const json doc = parse(md::report_to_json(report));
    CHECK(doc.at("layout") == "chapters");
    REQUIRE(doc.at("epubs").is_array());
    CHECK(doc.at("epubs").empty());
    REQUIRE(doc.at("skipped_chapters").is_array());
    CHECK(doc.at("skipped_chapters").empty());
    check_summary(doc, 0, 0, 0, 0);
    CHECK(md::exit_code_for(report) == md::exit_code::no_chapters);
}

TEST_CASE("report_to_json: exit-6 shape lists the failed file with its error", "[report][json]") {
    md::RunReport report = base_report();
    md::EpubOutcome failed = outcome(md::EpubStatus::failed, "Test Manga - Ch. 012.epub");
    failed.epub.kind = md::EpubKind::chapter;
    failed.epub.volume = std::nullopt;
    failed.epub.chapter = "12";
    failed.error = "image server returned HTTP 503";
    md::EpubOutcome oneshot =
        outcome(md::EpubStatus::written, "Test Manga - Oneshot [0a1b2c3d].epub");
    oneshot.epub.kind = md::EpubKind::oneshot;
    oneshot.epub.volume = std::nullopt;
    report.epubs = {failed, oneshot};

    const json doc = parse(md::report_to_json(report));
    REQUIRE(doc.at("epubs").size() == 2);
    const json& first = doc.at("epubs").at(0);
    CHECK(first.at("kind") == "chapter");
    CHECK(first.at("chapter") == "12");
    CHECK(first.at("status") == "failed");
    CHECK(first.at("error") == "image server returned HTTP 503");
    CHECK(first.at("pages") == 0);
    CHECK(doc.at("epubs").at(1).at("kind") == "oneshot");
    check_summary(doc, 0, 1, 0, 1);
    CHECK(md::exit_code_for(report) == md::exit_code::incomplete);
}

TEST_CASE("report_to_json: every skip reason has its documented spelling", "[report][json]") {
    md::RunReport report = base_report();
    report.skipped = {
        md::SkippedChapter{.id = "a", .reason = md::SkipReason::external},
        md::SkippedChapter{.id = "b", .reason = md::SkipReason::no_pages},
        md::SkippedChapter{.id = "c", .reason = md::SkipReason::unavailable},
        md::SkippedChapter{.id = "d", .reason = md::SkipReason::duplicate, .kept = "a"},
    };
    const json doc = parse(md::report_to_json(report));
    const json& skipped = doc.at("skipped_chapters");
    REQUIRE(skipped.size() == 4);
    CHECK(skipped.at(0).at("reason") == "external");
    CHECK(skipped.at(1).at("reason") == "no_pages");
    CHECK(skipped.at(2).at("reason") == "unavailable");
    CHECK(skipped.at(3).at("reason") == "duplicate");
    CHECK(skipped.at(3).at("kept") == "a");
}

TEST_CASE("report_to_json: invalid UTF-8 becomes U+FFFD and the document stays valid",
          "[report][json]") {
    md::RunReport report = base_report();
    report.manga.title = "Bad \xff\xfe title";
    md::EpubOutcome written = outcome(md::EpubStatus::written, "dir-\xc3/x.epub");
    written.epub.chapters.at(0).title = "\xe3\x81";         // truncated 3-byte sequence
    written.epub.chapters.at(0).groups.at(0).name = "\x80"; // lone continuation byte
    report.epubs = {written};

    const std::string text = md::report_to_json(report);
    const json doc = parse(text); // strict parse: the output itself is valid UTF-8
    const std::string replacement = "\xef\xbf\xbd";
    CHECK(doc.at("manga").at("title") == std::format("Bad {0}{0} title", replacement));
    const json& epub = doc.at("epubs").at(0);
    CHECK(epub.at("path") == "dir-" + replacement + "/x.epub");
    CHECK(epub.at("chapters").at(0).at("title") == replacement);
    CHECK(epub.at("chapters").at(0).at("groups").at(0).at("name") == replacement);
}

TEST_CASE("report_to_json: terminal escapes in titles are escaped, never raw ANSI",
          "[report][json]") {
    md::RunReport report = base_report();
    report.manga.title = "\x1b[31mred\x1b[0m\r\n\x07";
    md::EpubOutcome written = outcome(md::EpubStatus::written, "x.epub");
    written.epub.chapters.at(0).title = "\x1b]0;pwned\x07";
    report.epubs = {written};

    const std::string text = md::report_to_json(report);
    CHECK(!text.contains('\x1b'));
    CHECK(!text.contains('\x07'));
    CHECK(!text.contains('\r'));
    CHECK(text.contains("\\u001b[31mred"));
    const json doc = parse(text);
    CHECK(doc.at("manga").at("title") == "\x1b[31mred\x1b[0m\r\n\x07"); // round-trips exactly
}

TEST_CASE("report_to_json: valid UTF-8 is kept verbatim (not \\u-escaped)", "[report][json]") {
    md::RunReport report = base_report();
    report.manga.title = "進撃の巨人";
    const std::string text = md::report_to_json(report);
    CHECK(text.contains("\"title\": \"進撃の巨人\""));
}
