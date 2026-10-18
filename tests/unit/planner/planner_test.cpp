// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/planner/planner_test.cpp: the planner (download plan T9, acceptance 3 and 4). Canned
// feeds come from tests/support/mdx_fixture.hpp through the real API parsers; the dedupe, ordering,
// cover and limit rules use small hand-built inputs, each isolating one deciding factor.
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/planner.hpp"
#include "mangadex_dl/selection.hpp"
#include "support/mdx_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;

namespace {

constexpr std::string_view hand_id = "99999999-9999-4999-8999-999999999999";
constexpr std::string_view alpha_id = "aaaaaaaa-0002-4000-8000-000000000001";
constexpr std::string_view beta_id = "bbbbbbbb-0002-4000-8000-000000000002";
constexpr std::string_view day_one = "2020-01-01T00:00:00+00:00";

// ---- fixture input -----------------------------------------------------------------------------

/// The PlanInput the downloader would build for `manga`: every document goes through the real
/// parsers (T6), exactly as fetched.
[[nodiscard]] md::PlanInput fixture_input(const mdx::FixtureManga& manga) {
    md::PlanInput input{};
    md::Result<md::MangaInfo> info = md::parse_manga(mdx::manga_json(manga).dump());
    REQUIRE(info.has_value());
    input.manga = std::move(*info);
    for (const nlohmann::json& page : mdx::feed_pages_json(manga)) {
        md::Result<md::FeedPage> feed = md::parse_feed_page(page.dump());
        REQUIRE(feed.has_value());
        std::ranges::move(feed->chapters, std::back_inserter(input.chapters));
    }
    for (const nlohmann::json& page : mdx::covers_json(manga)) {
        md::Result<md::CoverPage> covers = md::parse_cover_page(page.dump());
        REQUIRE(covers.has_value());
        std::ranges::move(covers->covers, std::back_inserter(input.covers));
    }
    return input;
}

[[nodiscard]] md::Plan plan_of(const md::PlanInput& input, const md::PlanOptions& options = {}) {
    md::Result<md::Plan> plan = md::build_plan(input, options);
    REQUIRE(plan.has_value());
    return std::move(*plan);
}

[[nodiscard]] md::Selection selection(std::string_view spec) {
    md::Result<md::Selection> parsed = md::Selection::parse(spec);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] std::vector<std::string> file_names(const md::Plan& plan) {
    std::vector<std::string> names{};
    names.reserve(plan.epubs.size());
    for (const md::PlannedEpub& epub : plan.epubs) {
        names.push_back(epub.file_name);
    }
    return names;
}

[[nodiscard]] std::vector<std::string> chapter_ids(const md::PlannedEpub& epub) {
    std::vector<std::string> ids{};
    ids.reserve(epub.chapters.size());
    for (const md::Chapter& chapter : epub.chapters) {
        ids.push_back(chapter.id);
    }
    return ids;
}

[[nodiscard]] std::optional<std::string> cover_file(const md::PlannedEpub& epub) {
    return epub.cover.transform([](const md::Cover& cover) { return cover.file_name; });
}

/// The basic fixture's chapter ids by fixture index.
[[nodiscard]] std::string basic_chapter(std::uint64_t n) {
    return mdx::detail::fixture_uuid('2', mdx::detail::IdKind::chapter, n);
}

// ---- hand-built input --------------------------------------------------------------------------

[[nodiscard]] std::string chapter_id(std::uint64_t n) {
    return std::format("cccccccc-0001-4000-8000-{:012x}", n);
}

[[nodiscard]] md::ScanGroup alpha() {
    return md::ScanGroup{.id = std::string{alpha_id}, .name = "Alpha Scans"};
}

[[nodiscard]] md::ScanGroup beta() {
    return md::ScanGroup{.id = std::string{beta_id}, .name = "Beta Scans"};
}

struct ChapterSpec {
    std::uint64_t n = 0;
    std::optional<std::string> volume{};
    std::optional<std::string> chapter{};
    std::vector<md::ScanGroup> groups{};
    int pages = 1;
    std::string publish_at{day_one};
};

[[nodiscard]] md::Chapter make_chapter(const ChapterSpec& spec) {
    return md::Chapter{.id = chapter_id(spec.n),
                       .volume = spec.volume,
                       .chapter = spec.chapter,
                       .title = {},
                       .language = "en",
                       .groups = spec.groups,
                       .pages = spec.pages,
                       .publish_at = spec.publish_at,
                       .updated_at = std::string{day_one}};
}

[[nodiscard]] md::PlanInput hand_input(const std::vector<ChapterSpec>& specs,
                                       std::vector<md::Cover> covers = {}) {
    md::PlanInput input{
        .manga = md::MangaInfo{.id = std::string{hand_id},
                               .titles = {{"en", "Hand Title"}},
                               .original_language = "ja"},
        .covers = std::move(covers),
    };
    for (const ChapterSpec& spec : specs) {
        input.chapters.push_back(make_chapter(spec));
    }
    return input;
}

} // namespace

// ---- acceptance 3: canned feeds ----------------------------------------------------------------

TEST_CASE("Basic fixture, volumes layout: files, chapters, covers, skips", "[planner]") {
    const md::Plan plan = plan_of(fixture_input(mdx::basic_fixture()));

    CHECK(plan.manga == md::MangaSummary{.id = "22222222-2222-4222-8222-222222222222",
                                         .title = "Basic Title",
                                         .url = "https://mangadex.org/title/"
                                                "22222222-2222-4222-8222-222222222222",
                                         .language = "en",
                                         .direction = md::Direction::rtl});
    CHECK(plan.layout == md::Layout::volumes);
    CHECK(file_names(plan) == std::vector<std::string>{
                                  "Basic Title - Vol. 01.epub",
                                  "Basic Title - Vol. 02.epub",
                                  "Basic Title - Ch. 010.epub",
                                  "Basic Title - Ch. 010.5.epub",
                                  "Basic Title - Oneshot [22222222].epub",
                              });
    REQUIRE(plan.epubs.size() == 5);

    const std::string urn = "urn:mangadex:22222222-2222-4222-8222-222222222222:en:";
    const md::PlannedEpub& vol1 = plan.epubs[0];
    CHECK(vol1.kind == md::EpubKind::volume);
    CHECK(vol1.volume == "1");
    CHECK_FALSE(vol1.chapter.has_value());
    CHECK(chapter_ids(vol1) == std::vector{basic_chapter(1), basic_chapter(3)});
    CHECK(cover_file(vol1) == "cover-v1-en.png");
    CHECK(vol1.display_title == "Basic Title Vol. 1");
    CHECK(vol1.identifier == urn + "vol-1");
    CHECK(vol1.series_position == "1");
    CHECK(vol1.planned_pages == 5);
    CHECK_FALSE(vol1.plan_error.has_value());

    const md::PlannedEpub& vol2 = plan.epubs[1];
    CHECK(chapter_ids(vol2) == std::vector{basic_chapter(7)});
    CHECK(cover_file(vol2) == "cover-v2-nolocale.png");
    CHECK(vol2.planned_pages == 2);

    const md::PlannedEpub& loose = plan.epubs[3];
    CHECK(loose.kind == md::EpubKind::chapter);
    CHECK_FALSE(loose.volume.has_value());
    CHECK(loose.chapter == "10.5");
    CHECK(chapter_ids(loose) == std::vector{basic_chapter(9)});
    CHECK_FALSE(loose.cover.has_value());
    CHECK(loose.display_title == "Basic Title Ch. 10.5");
    CHECK(loose.identifier == urn + "ch-10.5");
    CHECK(loose.series_position == "10.5");

    const md::PlannedEpub& oneshot = plan.epubs[4];
    CHECK(oneshot.kind == md::EpubKind::oneshot);
    CHECK(chapter_ids(oneshot) == std::vector{basic_chapter(10)});
    CHECK_FALSE(oneshot.cover.has_value());
    CHECK(oneshot.display_title == "Basic Title Oneshot: Special Oneshot");
    CHECK(oneshot.identifier == urn + "oneshot-" + basic_chapter(10));
    CHECK_FALSE(oneshot.series_position.has_value());

    CHECK(plan.skipped == std::vector<md::SkippedChapter>{
                              {.id = basic_chapter(2),
                               .volume = "1",
                               .chapter = "1",
                               .reason = md::SkipReason::duplicate,
                               .kept = basic_chapter(1)},
                              {.id = basic_chapter(4),
                               .volume = "2",
                               .chapter = "3",
                               .reason = md::SkipReason::external},
                              {.id = basic_chapter(5),
                               .volume = "2",
                               .chapter = "4",
                               .reason = md::SkipReason::no_pages},
                              {.id = basic_chapter(6),
                               .volume = "2",
                               .chapter = "5",
                               .reason = md::SkipReason::unavailable},
                          });
    CHECK(plan.dedupe == std::vector<md::DedupeNote>{
                             {.key = "Vol. 1 Ch. 1",
                              .kept_id = basic_chapter(1),
                              .dropped_ids = {basic_chapter(2)},
                              .rule = "group covering the most chapters"},
                         });
}

TEST_CASE("Basic fixture, chapters layout", "[planner]") {
    const md::Plan plan =
        plan_of(fixture_input(mdx::basic_fixture()), {.layout = md::Layout::chapters});
    CHECK(file_names(plan) == std::vector<std::string>{
                                  "Basic Title - Vol. 01 Ch. 001.epub",
                                  "Basic Title - Vol. 01 Ch. 002.epub",
                                  "Basic Title - Vol. 02 Ch. 006.epub",
                                  "Basic Title - Ch. 010.epub",
                                  "Basic Title - Ch. 010.5.epub",
                                  "Basic Title - Oneshot [22222222].epub",
                              });
    REQUIRE(plan.epubs.size() == 6);
    const md::PlannedEpub& first = plan.epubs[0];
    CHECK(first.kind == md::EpubKind::chapter);
    CHECK(first.volume == "1");
    CHECK(first.chapter == "1");
    CHECK(first.display_title == "Basic Title Vol. 1 Ch. 1");
    CHECK(first.identifier == "urn:mangadex:22222222-2222-4222-8222-222222222222:en:vol-1-ch-1");
    CHECK_FALSE(first.cover.has_value());
    CHECK(plan.skipped.size() == 4);
}

TEST_CASE("Basic fixture, single layout", "[planner]") {
    const md::Plan plan =
        plan_of(fixture_input(mdx::basic_fixture()), {.layout = md::Layout::single});
    CHECK(file_names(plan) == std::vector<std::string>{"Basic Title.epub"});
    REQUIRE(plan.epubs.size() == 1);
    const md::PlannedEpub& book = plan.epubs.front();
    CHECK(book.kind == md::EpubKind::single);
    CHECK(chapter_ids(book) == std::vector{basic_chapter(1), basic_chapter(3), basic_chapter(7),
                                           basic_chapter(8), basic_chapter(9), basic_chapter(10)});
    CHECK(cover_file(book) == "cover-v1-en.png"); // the first volume's cover
    CHECK(book.display_title == "Basic Title");
    CHECK(book.identifier == "urn:mangadex:22222222-2222-4222-8222-222222222222:en:single");
    CHECK_FALSE(book.series_position.has_value());
    CHECK(book.planned_pages == 11);
}

TEST_CASE("Clean fixture plans one file per volume and skips nothing", "[planner]") {
    const md::Plan plan = plan_of(fixture_input(mdx::clean_fixture()));
    CHECK(file_names(plan) ==
          std::vector<std::string>{"Clean Title - Vol. 01.epub", "Clean Title - Vol. 02.epub"});
    CHECK(plan.skipped.empty());
    CHECK(plan.dedupe.empty());
}

TEST_CASE("The language filter drops other languages silently", "[planner]") {
    SECTION("fr picks only the French release, with the original-language cover") {
        const md::Plan plan = plan_of(fixture_input(mdx::basic_fixture()), {.lang = "fr"});
        CHECK(file_names(plan) == std::vector<std::string>{"Basic Title - Vol. 01.epub"});
        REQUIRE(plan.epubs.size() == 1);
        CHECK(chapter_ids(plan.epubs.front()) == std::vector{basic_chapter(11)});
        CHECK(cover_file(plan.epubs.front()) == "cover-v1-ja.png");
        CHECK(plan.epubs.front().identifier ==
              "urn:mangadex:22222222-2222-4222-8222-222222222222:fr:vol-1");
        CHECK(plan.skipped.empty());
        CHECK(plan.manga.language == "fr");
    }
    SECTION("no chapter in the language is an empty, valid plan") {
        const md::Plan plan = plan_of(fixture_input(mdx::nochapters_fixture()));
        CHECK(plan.epubs.empty());
        CHECK(plan.skipped.empty());
        CHECK(plan.manga.direction == md::Direction::ltr); // ko original
    }
}

TEST_CASE("Selections pick volumes, loose chapters and oneshots", "[planner]") {
    const md::PlanInput input = fixture_input(mdx::basic_fixture());
    SECTION("--volumes 2 keeps every loose chapter; skips are reported only when selected") {
        const md::Plan plan = plan_of(input, {.volumes = selection("2")});
        CHECK(file_names(plan) == std::vector<std::string>{
                                      "Basic Title - Vol. 02.epub",
                                      "Basic Title - Ch. 010.epub",
                                      "Basic Title - Ch. 010.5.epub",
                                      "Basic Title - Oneshot [22222222].epub",
                                  });
        REQUIRE(plan.skipped.size() == 3);
        CHECK(plan.skipped.front().id == basic_chapter(4));
        CHECK(plan.dedupe.empty());
    }
    SECTION("--volumes none --loose-chapters 10") {
        const md::Plan plan =
            plan_of(input, {.volumes = selection("none"), .loose_chapters = selection("10")});
        CHECK(file_names(plan) == std::vector<std::string>{"Basic Title - Ch. 010.epub"});
        CHECK(plan.skipped.empty());
    }
    SECTION("a oneshot matches only all") {
        const md::Plan plan =
            plan_of(input, {.volumes = selection("none"), .loose_chapters = selection("0-")});
        CHECK(file_names(plan) == std::vector<std::string>{"Basic Title - Ch. 010.epub",
                                                           "Basic Title - Ch. 010.5.epub"});
    }
    SECTION("--volumes 1 in single layout") {
        const md::Plan plan = plan_of(input, {.volumes = selection("1"),
                                              .loose_chapters = selection("none"),
                                              .layout = md::Layout::single});
        REQUIRE(plan.epubs.size() == 1);
        CHECK(chapter_ids(plan.epubs.front()) == std::vector{basic_chapter(1), basic_chapter(3)});
    }
}

TEST_CASE("Ordering: numeric, then non-numeric bytewise, the none volume last", "[planner]") {
    const md::PlanInput input = hand_input({
        {.n = 1, .volume = "10", .chapter = "1"},
        {.n = 2, .volume = "Extra", .chapter = "1"},
        {.n = 3, .volume = "2", .chapter = "10"},
        {.n = 4, .volume = "2", .chapter = "2"},
        {.n = 5, .volume = "2", .chapter = "1.5"},
        {.n = 6, .volume = "Bonus", .chapter = "1"},
        {.n = 7, .volume = "1.5", .chapter = "1"},
        {.n = 8, .chapter = "3"},
        {.n = 9, .chapter = "Side"},
        {.n = 10},
        {.n = 11, .chapter = "1"},
        {.n = 12, .volume = "", .chapter = "7"}, // an empty volume is volume-less
        {.n = 13, .volume = "02", .chapter = "Omake"},
    });
    const md::Plan plan = plan_of(input);
    CHECK(file_names(plan) == std::vector<std::string>{
                                  "Hand Title - Vol. 01.5.epub",
                                  "Hand Title - Vol. 02.epub",
                                  "Hand Title - Vol. 10.epub",
                                  "Hand Title - Vol. Bonus.epub",
                                  "Hand Title - Vol. Extra.epub",
                                  "Hand Title - Ch. 001.epub",
                                  "Hand Title - Ch. 003.epub",
                                  "Hand Title - Ch. 007.epub",
                                  "Hand Title - Ch. Side.epub",
                                  "Hand Title - Oneshot [cccccccc].epub",
                              });
    REQUIRE(plan.epubs.size() == 10);
    // Inside a volume: 1.5, 2, 10, then the non-numeric chapter; "02" groups with "2".
    CHECK(chapter_ids(plan.epubs[1]) ==
          std::vector{chapter_id(5), chapter_id(4), chapter_id(3), chapter_id(13)});
    CHECK(plan.epubs[1].series_position == "2");
    CHECK_FALSE(plan.epubs[3].series_position.has_value()); // "Bonus" is not a position
    CHECK(plan.epubs[3].identifier == std::format("urn:mangadex:{}:en:vol-Bonus", hand_id));
}

TEST_CASE("Identifier units percent-encode labels", "[planner]") {
    const md::Plan plan = plan_of(hand_input({{.n = 1, .volume = "A b:c", .chapter = "x-y"}}),
                                  {.layout = md::Layout::chapters});
    REQUIRE(plan.epubs.size() == 1);
    CHECK(plan.epubs.front().identifier ==
          std::format("urn:mangadex:{}:en:vol-A%20b%3Ac-ch-x%2Dy", hand_id));
    CHECK(plan.epubs.front().display_title == "Hand Title Vol. A b:c Ch. x-y");
    CHECK(plan.epubs.front().file_name == "Hand Title - Vol. A b_c Ch. x-y.epub");
}

TEST_CASE("Colliding file names get a numbered suffix", "[planner]") {
    const md::Plan plan = plan_of(hand_input({
        {.n = 1, .volume = "a_b", .chapter = "1"},
        {.n = 2, .volume = "a/b", .chapter = "1"},
    }));
    CHECK(file_names(plan) ==
          std::vector<std::string>{"Hand Title - Vol. a_b.epub", "Hand Title - Vol. a_b (2).epub"});
}

// ---- acceptance 4: dedupe ----------------------------------------------------------------------

TEST_CASE("Dedupe rule 1: the first matching --group wins", "[planner][dedupe]") {
    // Alpha covers more keys and has more pages, so only the preference can pick Beta.
    const md::PlanInput input = hand_input({
        {.n = 1, .volume = "1", .chapter = "1", .groups = {alpha()}, .pages = 5},
        {.n = 2, .volume = "1", .chapter = "1", .groups = {beta()}, .pages = 1},
        {.n = 3, .volume = "1", .chapter = "2", .groups = {alpha()}},
    });
    const std::string preferred = GENERATE(std::string{"beta scans"}, std::string{"BETA SCANS"},
                                           std::string{"BBBBBBBB-0002-4000-8000-000000000002"});
    CAPTURE(preferred);
    const md::Plan plan =
        plan_of(input, {.preferred_groups = {"Nobody", preferred, "Alpha Scans"}});
    REQUIRE(plan.dedupe.size() == 1);
    CHECK(plan.dedupe.front().kept_id == chapter_id(2));
    CHECK(plan.dedupe.front().dropped_ids == std::vector{chapter_id(1)});
    CHECK(plan.dedupe.front().rule == "preferred group (--group)");
    REQUIRE(plan.skipped.size() == 1);
    CHECK(plan.skipped.front().id == chapter_id(1));
    CHECK(plan.skipped.front().reason == md::SkipReason::duplicate);
    CHECK(plan.skipped.front().kept == chapter_id(2));
    REQUIRE(plan.epubs.size() == 1);
    CHECK(chapter_ids(plan.epubs.front()) == std::vector{chapter_id(2), chapter_id(3)});
}

TEST_CASE("Dedupe rule 1 follows --group priority order", "[planner][dedupe]") {
    const md::PlanInput input = hand_input({
        {.n = 1, .volume = "1", .chapter = "1", .groups = {alpha()}},
        {.n = 2, .volume = "1", .chapter = "1", .groups = {beta()}, .pages = 9},
    });
    const md::Plan plan = plan_of(input, {.preferred_groups = {"alpha scans", "beta scans"}});
    REQUIRE(plan.dedupe.size() == 1);
    CHECK(plan.dedupe.front().kept_id == chapter_id(1));
    CHECK(plan.dedupe.front().rule == "preferred group (--group)");
}

TEST_CASE("Dedupe rule 2: the group covering the most keys wins", "[planner][dedupe]") {
    const md::PlanInput input = hand_input({
        {.n = 1, .volume = "1", .chapter = "1", .groups = {alpha()}, .pages = 1},
        {.n = 2, .volume = "1", .chapter = "1", .groups = {beta()}, .pages = 5},
        {.n = 3, .volume = "1", .chapter = "2", .groups = {alpha()}},
    });
    const md::Plan plan = plan_of(input, {.preferred_groups = {"Nobody"}});
    REQUIRE(plan.dedupe.size() == 1);
    CHECK(plan.dedupe.front().kept_id == chapter_id(1));
    CHECK(plan.dedupe.front().rule == "group covering the most chapters");
}

TEST_CASE("Dedupe rule 2: group identity is the sorted group-id set", "[planner][dedupe]") {
    // {Alpha, Beta} covers keys 1, 3 and 4 (listed in either order); {Alpha} covers 1 and 2.
    const md::PlanInput input = hand_input({
        {.n = 1, .volume = "1", .chapter = "1", .groups = {alpha(), beta()}},
        {.n = 2, .volume = "1", .chapter = "1", .groups = {alpha()}, .pages = 5},
        {.n = 3, .volume = "1", .chapter = "2", .groups = {alpha()}},
        {.n = 4, .volume = "1", .chapter = "3", .groups = {beta(), alpha()}},
        {.n = 5, .volume = "1", .chapter = "4", .groups = {alpha(), beta()}},
    });
    const md::Plan plan = plan_of(input);
    REQUIRE(plan.dedupe.size() == 1);
    CHECK(plan.dedupe.front().kept_id == chapter_id(1));
    CHECK(plan.dedupe.front().rule == "group covering the most chapters");
}

TEST_CASE("Dedupe rule 3: more pages wins", "[planner][dedupe]") {
    const md::PlanInput input = hand_input({
        {.n = 1,
         .volume = "1",
         .chapter = "1",
         .groups = {alpha()},
         .pages = 2,
         .publish_at = "2019-01-01T00:00:00+00:00"},
        {.n = 2, .volume = "1", .chapter = "1", .groups = {beta()}, .pages = 3},
    });
    const md::Plan plan = plan_of(input);
    REQUIRE(plan.dedupe.size() == 1);
    CHECK(plan.dedupe.front().kept_id == chapter_id(2));
    CHECK(plan.dedupe.front().rule == "more pages");
}

TEST_CASE("Dedupe rule 4: the earlier publishAt (in UTC) wins", "[planner][dedupe]") {
    SECTION("offsets are applied before comparing") {
        // 2020-01-02T00:00+09:00 is 2020-01-01T15:00Z, earlier than 20:00Z (though it sorts later
        // as a string), and the earlier release has the larger id.
        const md::PlanInput input = hand_input({
            {.n = 1, .volume = "1", .chapter = "1", .publish_at = "2020-01-01T20:00:00+00:00"},
            {.n = 2, .volume = "1", .chapter = "1", .publish_at = "2020-01-02T00:00:00+09:00"},
        });
        const md::Plan plan = plan_of(input);
        REQUIRE(plan.dedupe.size() == 1);
        CHECK(plan.dedupe.front().kept_id == chapter_id(2));
        CHECK(plan.dedupe.front().rule == "earlier publishAt");
    }
    SECTION("Z, negative offsets and fractions parse") {
        const md::PlanInput input = hand_input({
            {.n = 1, .volume = "1", .chapter = "1", .publish_at = "2020-03-01T00:00:00Z"},
            {.n = 2, .volume = "1", .chapter = "1", .publish_at = "2020-02-29T21:00:00.5-03:30"},
        });
        const md::Plan plan = plan_of(input);
        REQUIRE(plan.dedupe.size() == 1);
        CHECK(plan.dedupe.front().kept_id == chapter_id(1)); // 2020-03-01T00:30Z is later
        CHECK(plan.dedupe.front().rule == "earlier publishAt");
    }
    SECTION("an unparseable publishAt ranks after any parseable one") {
        const std::string bad =
            GENERATE(std::string{"garbage"}, std::string{"2020-02-30T00:00:00Z"},
                     std::string{"2020-01-01T24:00:00Z"}, std::string{"2020-01-01T00:00:00"},
                     std::string{"2020-01-01T00:00:00+0900"}, std::string{});
        CAPTURE(bad);
        const md::PlanInput input = hand_input({
            {.n = 1, .volume = "1", .chapter = "1", .publish_at = bad},
            {.n = 2, .volume = "1", .chapter = "1", .publish_at = "2030-01-01T00:00:00+00:00"},
        });
        const md::Plan plan = plan_of(input);
        REQUIRE(plan.dedupe.size() == 1);
        CHECK(plan.dedupe.front().kept_id == chapter_id(2));
    }
}

TEST_CASE("Dedupe rule 5: the smallest UUID wins", "[planner][dedupe]") {
    const md::PlanInput input = hand_input({
        {.n = 7, .volume = "1", .chapter = "01"},
        {.n = 3, .volume = "01", .chapter = "1.0"},
        {.n = 5, .volume = "1", .chapter = "1"},
    });
    const md::Plan plan = plan_of(input);
    REQUIRE(plan.dedupe.size() == 1);
    CHECK(plan.dedupe.front().key == "Vol. 1 Ch. 1"); // normalized labels
    CHECK(plan.dedupe.front().kept_id == chapter_id(3));
    CHECK(plan.dedupe.front().dropped_ids == std::vector{chapter_id(5), chapter_id(7)});
    CHECK(plan.dedupe.front().rule == "smallest chapter id");
    CHECK(plan.skipped.size() == 2);
}

TEST_CASE("Dedupe notes label the key like the skip warnings", "[planner][dedupe]") {
    const md::PlanInput input = hand_input({
        {.n = 1, .chapter = "10"},
        {.n = 2, .chapter = "010.0"},
        {.n = 3, .volume = "a b", .chapter = "x"},
        {.n = 4, .volume = "a b", .chapter = "x"},
    });
    const md::Plan plan = plan_of(input);
    REQUIRE(plan.dedupe.size() == 2);
    std::vector<std::string> keys =
        plan.dedupe | std::views::transform(&md::DedupeNote::key) | std::ranges::to<std::vector>();
    std::ranges::sort(keys);
    CHECK(keys == std::vector<std::string>{"Ch. 10", "Vol. a b Ch. x"});
}

TEST_CASE("A null chapter is never deduped", "[planner][dedupe]") {
    const md::PlanInput input = hand_input({
        {.n = 1, .groups = {alpha()}},
        {.n = 2, .groups = {beta()}},
        {.n = 3, .volume = "1"},
        {.n = 4, .volume = "1"},
    });
    const md::Plan plan = plan_of(input);
    CHECK(plan.dedupe.empty());
    CHECK(plan.skipped.empty());
    REQUIRE(plan.epubs.size() == 3);
    CHECK(chapter_ids(plan.epubs[0]) == std::vector{chapter_id(3), chapter_id(4)});
    CHECK(plan.epubs[1].kind == md::EpubKind::oneshot);
    CHECK(plan.epubs[2].kind == md::EpubKind::oneshot);
    CHECK(plan.epubs[1].file_name == "Hand Title - Oneshot [cccccccc].epub");
    CHECK(plan.epubs[2].file_name == "Hand Title - Oneshot [cccccccc] (2).epub");
}

TEST_CASE("Skipped releases never take part in dedupe", "[planner][dedupe]") {
    md::PlanInput input = hand_input({
        {.n = 1, .volume = "1", .chapter = "1", .groups = {alpha()}, .pages = 9},
        {.n = 2, .volume = "1", .chapter = "1", .groups = {beta()}},
    });
    input.chapters.front().external = true;
    const md::Plan plan = plan_of(input);
    CHECK(plan.dedupe.empty());
    REQUIRE(plan.skipped.size() == 1);
    CHECK(plan.skipped.front().reason == md::SkipReason::external);
    REQUIRE(plan.epubs.size() == 1);
    CHECK(chapter_ids(plan.epubs.front()) == std::vector{chapter_id(2)});
}

TEST_CASE("Shuffled or repeated input yields an identical plan", "[planner][dedupe]") {
    const md::PlanInput input = fixture_input(mdx::basic_fixture());
    const md::Layout layout =
        GENERATE(md::Layout::volumes, md::Layout::chapters, md::Layout::single);
    const md::PlanOptions options{.layout = layout};
    const md::Plan expected = plan_of(input, options);
    for (std::uint32_t seed = 1; seed <= 16; ++seed) {
        CAPTURE(seed);
        md::PlanInput shuffled = input;
        std::mt19937 rng{seed};
        // A feed page boundary that moved during paging returns a chapter twice.
        const md::Chapter repeated = shuffled.chapters[seed % shuffled.chapters.size()];
        shuffled.chapters.push_back(repeated);
        std::ranges::shuffle(shuffled.chapters, rng);
        std::ranges::shuffle(shuffled.covers, rng);
        CHECK(plan_of(shuffled, options) == expected);
    }
}

// ---- covers ------------------------------------------------------------------------------------

TEST_CASE("Cover choice per volume", "[planner][covers]") {
    const std::vector<ChapterSpec> one_volume{{.n = 1, .volume = "1", .chapter = "1"}};
    const auto cover = [](std::optional<std::string> volume, std::string file,
                          std::optional<std::string> locale) {
        return md::Cover{
            .volume = std::move(volume), .file_name = std::move(file), .locale = std::move(locale)};
    };
    SECTION("--lang beats the original language beats any other locale") {
        const md::Plan plan = plan_of(
            hand_input(one_volume, {cover("1", "a-de.png", "de"), cover("1", "b-ja.png", "ja"),
                                    cover("1", "c-en.png", "en")}));
        CHECK(cover_file(plan.epubs.front()) == "c-en.png");
        const md::Plan original = plan_of(
            hand_input(one_volume, {cover("1", "a-de.png", "de"), cover("1", "b-ja.png", "ja")}));
        CHECK(cover_file(original.epubs.front()) == "b-ja.png");
    }
    SECTION("a null locale ranks after any non-null locale") {
        const md::Plan plan = plan_of(hand_input(
            one_volume, {cover("1", "a-null.png", std::nullopt), cover("1", "z-de.png", "de")}));
        CHECK(cover_file(plan.epubs.front()) == "z-de.png");
    }
    SECTION("ties go to the smallest file name") {
        const md::Plan plan =
            plan_of(hand_input(one_volume, {cover("1", "b.png", "en"), cover("1", "a.png", "en")}));
        CHECK(cover_file(plan.epubs.front()) == "a.png");
    }
    SECTION("cover volumes are compared normalized") {
        const md::Plan plan = plan_of(hand_input(one_volume, {cover("01", "v.png", "en")}));
        CHECK(cover_file(plan.epubs.front()) == "v.png");
    }
    SECTION("a null cover volume never matches") {
        const md::Plan plan =
            plan_of(hand_input(one_volume, {cover(std::nullopt, "none.png", "en")}));
        CHECK_FALSE(plan.epubs.front().cover.has_value());
        const md::Plan loose = plan_of(
            hand_input({{.n = 1, .chapter = "1"}}, {cover(std::nullopt, "none.png", "en")}));
        CHECK_FALSE(loose.epubs.front().cover.has_value());
    }
    SECTION("single layout uses the first volume that has a cover") {
        const md::Plan plan = plan_of(hand_input({{.n = 1, .volume = "1", .chapter = "1"},
                                                  {.n = 2, .volume = "2", .chapter = "2"}},
                                                 {cover("2", "v2.png", "en")}),
                                      {.layout = md::Layout::single});
        CHECK(cover_file(plan.epubs.front()) == "v2.png");
    }
}

// ---- limits ------------------------------------------------------------------------------------

TEST_CASE("A chapter over max_pages_per_chapter marks its file as failed", "[planner][limits]") {
    const int pages = GENERATE(5000, 5001);
    CAPTURE(pages);
    const md::Plan plan = plan_of(hand_input({
        {.n = 1, .volume = "1", .chapter = "1", .pages = pages},
        {.n = 2, .volume = "2", .chapter = "2"},
    }));
    REQUIRE(plan.epubs.size() == 2);
    CHECK(plan.epubs[0].plan_error.has_value() == (pages > 5000));
    CHECK(plan.epubs[0].planned_pages == static_cast<std::size_t>(pages));
    CHECK_FALSE(plan.epubs[1].plan_error.has_value());
    const std::optional<std::string>& error = plan.epubs[0].plan_error;
    if (error) {
        CHECK(error->find(chapter_id(1)) != std::string::npos); // names the chapter, by id
    }
}

TEST_CASE("A file over the zip entry limit is planned but marked as failed", "[planner][limits]") {
    // Entries: 5 fixed + 2 per page (+ 2 for a cover). The count is always odd, so the boundary
    // is 65533 entries (fits in 65534) against 65535 (does not).
    struct Boundary {
        bool with_cover = false;
        int last_pages = 0;
        bool fails = false;
    };
    const Boundary boundary =
        GENERATE(Boundary{.with_cover = false, .last_pages = 2764},                // 65533
                 Boundary{.with_cover = false, .last_pages = 2765, .fails = true}, // 65535
                 Boundary{.with_cover = true, .last_pages = 2763},                 // 65533
                 Boundary{.with_cover = true, .last_pages = 2764, .fails = true}); // 65535
    CAPTURE(boundary.with_cover, boundary.last_pages);
    std::vector<ChapterSpec> specs{};
    for (std::uint64_t n = 1; n <= 6; ++n) {
        specs.push_back({.n = n, .volume = "1", .chapter = std::to_string(n), .pages = 5000});
    }
    specs.push_back({.n = 7, .volume = "1", .chapter = "7", .pages = boundary.last_pages});
    std::vector<md::Cover> covers{};
    if (boundary.with_cover) {
        covers.push_back({.volume = "1", .file_name = "c.png", .locale = "en"});
    }
    const md::Plan plan = plan_of(hand_input(specs, std::move(covers)));
    REQUIRE(plan.epubs.size() == 1);
    CHECK(plan.epubs.front().cover.has_value() == boundary.with_cover);
    CHECK(plan.epubs.front().plan_error.has_value() == boundary.fails);
    static_assert(md::max_zip_entries_per_epub == 65534);
}

// ---- title, direction, errors ------------------------------------------------------------------

TEST_CASE("resolve_title: title.en, then an alt title in --lang, then the first title",
          "[planner][title]") {
    md::MangaInfo manga{.id = std::string{hand_id},
                        .titles = {{"ja", "日本語"}, {"en", "English"}},
                        .alt_titles = {{"fr", "Français"}, {"de", "Deutsch"}, {"fr", "Zweiter"}}};
    CHECK(md::resolve_title(manga, "fr") == "English");
    manga.titles = {{"ko", "한국어"}, {"de", "Erster"}};
    CHECK(md::resolve_title(manga, "fr") == "Français");
    CHECK(md::resolve_title(manga, "pt-br") == "Erster"); // lexicographically first language
    manga.titles = {{"en", ""}, {"ja", "日本語"}};
    CHECK(md::resolve_title(manga, "pt-br") == "日本語"); // empty values do not count
    manga.titles.clear();
    CHECK(md::resolve_title(manga, "pt-br") == hand_id);
}

TEST_CASE("resolve_direction", "[planner][title]") {
    using md::Direction;
    using md::DirectionChoice;
    CHECK(md::resolve_direction(DirectionChoice::automatic, "ja") == Direction::rtl);
    CHECK(md::resolve_direction(DirectionChoice::automatic, "ja-ro") == Direction::rtl);
    CHECK(md::resolve_direction(DirectionChoice::automatic, "ko") == Direction::ltr);
    CHECK(md::resolve_direction(DirectionChoice::automatic, "zh") == Direction::ltr);
    CHECK(md::resolve_direction(DirectionChoice::automatic, "") == Direction::ltr);
    CHECK(md::resolve_direction(DirectionChoice::ltr, "ja") == Direction::ltr);
    CHECK(md::resolve_direction(DirectionChoice::rtl, "en") == Direction::rtl);
}

TEST_CASE("The title override and the direction choice reach the plan", "[planner][title]") {
    const md::PlanInput input = hand_input({{.n = 1, .volume = "1", .chapter = "1"}});
    const md::Plan plan =
        plan_of(input, {.direction = md::DirectionChoice::ltr, .title_override = "Over/ride"});
    CHECK(plan.manga.title == "Over/ride");
    CHECK(plan.manga.direction == md::Direction::ltr);
    CHECK(file_names(plan) == std::vector<std::string>{"Over_ride - Vol. 01.epub"});
    CHECK(plan.epubs.front().display_title == "Over/ride Vol. 1");

    const md::Plan empty_override = plan_of(input, {.title_override = ""});
    CHECK(empty_override.manga.title == "Hand Title");
}

TEST_CASE("A title that sanitizes to nothing falls back to the UUID", "[planner][title]") {
    md::PlanInput input = hand_input({{.n = 1, .volume = "1", .chapter = "1"}});
    input.manga.titles = {{"en", "..."}};
    const md::Plan plan = plan_of(input);
    CHECK(file_names(plan) == std::vector<std::string>{std::format("{} - Vol. 01.epub", hand_id)});
}

TEST_CASE("Ids that are not UUIDs are malformed API data", "[planner]") {
    SECTION("title id") {
        md::PlanInput input = hand_input({});
        input.manga.id = "../etc";
        const md::Result<md::Plan> plan = md::build_plan(input, {});
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error().code == md::Errc::network);
    }
    SECTION("chapter id") {
        md::PlanInput input = hand_input({{.n = 1, .volume = "1", .chapter = "1"}});
        input.chapters.front().id = "not-a-uuid";
        const md::Result<md::Plan> plan = md::build_plan(input, {});
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error().code == md::Errc::network);
    }
}
