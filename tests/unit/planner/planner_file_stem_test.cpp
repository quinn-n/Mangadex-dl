// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/planner/planner_file_stem_test.cpp: Plan::file_stem (list plan T6). The planner
// computes the stem once, sets it on the plan and builds every file name from it, so a
// --dir-per-title directory and the files inside it always agree. Also the AC36 language half:
// with no `en` title the stem follows --lang (an altTitle in that language), else the first title.
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/filename.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/planner.hpp"
#include "support/mdx_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;

/// The PlanInput the downloader would build for `manga`, through the real API parsers.
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

/// U+202E RIGHT-TO-LEFT OVERRIDE as UTF-8, built at run time: GCC's -Wbidi-chars=any and
/// clang-tidy's misc-misleading-bidirectional reject bidi controls in literals, escaped or not.
[[nodiscard]] std::string right_to_left_override() {
    return std::string{static_cast<char>(0xE2), static_cast<char>(0x80), static_cast<char>(0xAE)};
}

/// The single source of truth, and every file name built from it.
void check_stem_invariant(const md::Plan& plan) {
    CHECK(plan.file_stem == md::sanitize_file_stem(plan.manga.title, plan.manga.id));
    for (const md::PlannedEpub& epub : plan.epubs) {
        INFO(epub.file_name);
        CHECK(epub.file_name.starts_with(plan.file_stem));
    }
}

} // namespace

TEST_CASE("planner file stem: set from the resolved title and shared by every file name",
          "[planner][file_stem]") {
    const md::Layout layout =
        GENERATE(md::Layout::volumes, md::Layout::chapters, md::Layout::single);
    INFO("layout " << std::to_underlying(layout));
    const md::Plan plan =
        plan_of(fixture_input(mdx::clean_fixture()), md::PlanOptions{.layout = layout});
    CHECK(plan.file_stem == "Clean Title");
    REQUIRE_FALSE(plan.epubs.empty());
    check_stem_invariant(plan);
}

TEST_CASE("planner file stem: sanitized exactly as the file names are", "[planner][file_stem]") {
    md::PlanInput input = fixture_input(mdx::clean_fixture());
    input.manga.titles = {{"en", "A/B: C?"}};
    const md::Plan plan = plan_of(input);
    CHECK(plan.manga.title == "A/B: C?");
    CHECK(plan.file_stem == "A_B_ C_");
    REQUIRE(plan.epubs.size() == 2);
    CHECK(plan.epubs[0].file_name == "A_B_ C_ - Vol. 01.epub");
    check_stem_invariant(plan);
}

TEST_CASE("planner file stem: an unusable title falls back to the title UUID",
          "[planner][file_stem]") {
    const std::string title =
        GENERATE(std::string{"..."}, std::string{"   "}, right_to_left_override());
    md::PlanInput input = fixture_input(mdx::clean_fixture());
    input.manga.titles = {{"en", title}};
    const md::Plan plan = plan_of(input);
    CHECK(plan.file_stem == "11111111-1111-4111-8111-111111111111");
    check_stem_invariant(plan);
}

TEST_CASE("planner file stem: --title overrides the resolved title", "[planner][file_stem]") {
    const md::Plan plan = plan_of(fixture_input(mdx::clean_fixture()),
                                  md::PlanOptions{.title_override = "My: Series"});
    CHECK(plan.file_stem == "My_ Series");
    check_stem_invariant(plan);
}

TEST_CASE("planner file stem: set for an empty plan too", "[planner][file_stem]") {
    // The clean title has only `en` chapters: a `de` run plans nothing (exit 5 at the caller).
    const md::Plan plan =
        plan_of(fixture_input(mdx::clean_fixture()), md::PlanOptions{.lang = "de"});
    CHECK(plan.epubs.empty());
    CHECK(plan.file_stem == "Clean Title");
    check_stem_invariant(plan);
}

TEST_CASE("planner file stem: with no en title it follows --lang, else the first title",
          "[planner][file_stem]") {
    md::PlanInput input = fixture_input(mdx::clean_fixture());
    // No `en` title. The parser keeps titles in language-code order; ja < ko.
    input.manga.titles = {{"ja", "Ja Title"}, {"ko", "Ko Title"}};
    input.manga.alt_titles = {{"en", "English Alt"}, {"ja-ro", "Romaji Alt"}};

    SECTION("an altTitle in --lang wins over the first title") {
        const md::Plan plan = plan_of(input);
        CHECK(plan.file_stem == "English Alt");
        REQUIRE_FALSE(plan.epubs.empty());
        CHECK(plan.epubs[0].file_name == "English Alt - Vol. 01.epub");
        check_stem_invariant(plan);
    }
    SECTION("another --lang picks its own altTitle (here: nothing to download, same stem rule)") {
        const md::Plan plan = plan_of(input, md::PlanOptions{.lang = "ja-ro"});
        CHECK(plan.file_stem == "Romaji Alt");
        check_stem_invariant(plan);
    }
    SECTION("no altTitle in --lang: the first title value") {
        input.manga.alt_titles = {{"fr", "Titre"}};
        const md::Plan plan = plan_of(input);
        CHECK(plan.file_stem == "Ja Title");
        REQUIRE_FALSE(plan.epubs.empty());
        CHECK(plan.epubs[0].file_name == "Ja Title - Vol. 01.epub");
        check_stem_invariant(plan);
    }
}
