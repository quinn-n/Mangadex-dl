// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/mangadex_api/mangadex_api_test.cpp: MangaDex API parsing, validation, URL building and
// MD@H report bodies (download plan T6, acceptance 9). Parsers are fed the canned fixture JSON
// (tests/support/mdx_fixture.hpp) and hand-written hostile variants: wrong types, missing fields,
// invalid UTF-8, oversized and over-deep documents, trailing garbage.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "support/mdx_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using nlohmann::json;

constexpr std::string_view nil_uuid = "00000000-0000-0000-0000-000000000000";
constexpr std::string_view chapter_uuid = "abcdef01-2345-4678-89ab-cdef01234567";
constexpr std::string_view group_one = "0000000a-0002-4000-8000-000000000001";
constexpr std::string_view group_two = "0000000a-0002-4000-8000-000000000002";

/// The error code of a failed Result, or nullopt when it holds a value.
template <class T> [[nodiscard]] std::optional<md::Errc> error_code(const md::Result<T>& result) {
    if (result.has_value()) {
        return std::nullopt;
    }
    return result.error().code;
}

template <class T> [[nodiscard]] std::string error_message(const md::Result<T>& result) {
    return result.has_value() ? std::string{} : result.error().message;
}

/// The Chapter the parser must produce for a fixture chapter.
[[nodiscard]] md::Chapter expected_chapter(const mdx::FixtureManga& manga,
                                           const mdx::FixtureChapter& chapter) {
    md::Chapter out{
        .id = chapter.id,
        .volume = chapter.volume,
        .chapter = chapter.chapter,
        .title = chapter.title.value_or(""),
        .language = chapter.lang,
        .pages = mdx::detail::page_count(chapter),
        .external = chapter.external_url.has_value(),
        .unavailable = chapter.unavailable,
        .publish_at = chapter.publish_at,
        .updated_at = chapter.updated_at,
    };
    for (const std::string& group_id : chapter.group_ids) {
        md::ScanGroup group{.id = group_id};
        for (const mdx::FixtureGroup& known : manga.groups) {
            if (known.id == group_id) {
                group.name = known.name;
            }
        }
        out.groups.push_back(std::move(group));
    }
    return out;
}

[[nodiscard]] std::vector<md::Chapter> expected_chapters(const mdx::FixtureManga& manga) {
    std::vector<md::Chapter> out;
    out.reserve(manga.chapters.size());
    for (const mdx::FixtureChapter& chapter : manga.chapters) {
        out.push_back(expected_chapter(manga, chapter));
    }
    return out;
}

[[nodiscard]] std::vector<md::Cover> expected_covers(const mdx::FixtureManga& manga) {
    std::vector<md::Cover> out;
    out.reserve(manga.covers.size());
    for (const mdx::FixtureCover& cover : manga.covers) {
        out.push_back(
            md::Cover{.volume = cover.volume, .file_name = cover.file, .locale = cover.locale});
    }
    return out;
}

/// A feed page holding one chapter whose attributes are `attributes`.
[[nodiscard]] std::string feed_with_attributes(json attributes) {
    return json{
        {"data", json::array({{{"id", chapter_uuid}, {"attributes", std::move(attributes)}}})},
        {"limit", 500},
        {"offset", 0},
        {"total", 1}}
        .dump();
}

/// The clean fixture's first at-home document, with `edit` applied.
template <class Edit> [[nodiscard]] std::string edited_at_home(Edit edit) {
    json doc = mdx::at_home_json(mdx::clean_fixture().chapters.front());
    edit(doc);
    return doc.dump();
}

/// `text` (a JSON object) with an extra leading member nested `levels` arrays deep, so the whole
/// document is 1 + levels deep.
[[nodiscard]] std::string with_nested_member(std::string_view text, std::size_t levels) {
    std::string out = "{\"pad\":";
    out.append(levels, '[');
    out.append(levels, ']');
    out += ',';
    out.append(text.substr(1));
    return out;
}

} // namespace

// ---- parse_manga --------------------------------------------------------------------------------

TEST_CASE("parse_manga reads the clean fixture title record", "[mangadex_api][parse_manga]") {
    const auto info = md::parse_manga(mdx::manga_json(mdx::clean_fixture()).dump());
    REQUIRE(info.has_value());
    CHECK(info->id == "11111111-1111-4111-8111-111111111111");
    CHECK(info->titles == std::vector<md::LocalizedText>{{"en", "Clean Title"}});
    CHECK(info->alt_titles == std::vector<md::LocalizedText>{{"ja", "クリーン・タイトル"},
                                                             {"ja-ro", "Kuriin Taitoru"}});
    CHECK(info->original_language == "ja");
    CHECK(info->authors == std::vector<std::string>{"Clean Author"});
    CHECK(info->artists == std::vector<std::string>{"Clean Artist"});
}

TEST_CASE("parse_manga lists a creator who is both author and artist twice",
          "[mangadex_api][parse_manga]") {
    const auto info = md::parse_manga(mdx::manga_json(mdx::basic_fixture()).dump());
    REQUIRE(info.has_value());
    CHECK(info->authors == std::vector<std::string>{"Basic Mangaka"});
    CHECK(info->artists == std::vector<std::string>{"Basic Mangaka"});
    CHECK(info->alt_titles == std::vector<md::LocalizedText>{{"ja-ro", "Kihon Taitoru"}});
}

TEST_CASE("parse_manga needs only the id and the title map", "[mangadex_api][parse_manga]") {
    const json doc = {{"data", {{"id", nil_uuid}, {"attributes", {{"title", json::object()}}}}}};
    const auto info = md::parse_manga(doc.dump());
    REQUIRE(info.has_value());
    CHECK(info->id == nil_uuid);
    CHECK(info->titles.empty());
    CHECK(info->alt_titles.empty());
    CHECK(info->original_language.empty());
    CHECK(info->authors.empty());
    CHECK(info->artists.empty());
}

TEST_CASE("parse_manga skips relationships without attributes or of other types",
          "[mangadex_api][parse_manga]") {
    const json doc = {
        {"data",
         {{"id", nil_uuid},
          {"attributes", {{"title", {{"en", "T"}}}}},
          {"relationships",
           json::array({{{"id", "a"}, {"type", "author"}},
                        {{"id", "b"}, {"type", "artist"}, {"attributes", {{"name", ""}}}},
                        {{"id", "c"}, {"type", "cover_art"}, {"attributes", {{"name", "X"}}}},
                        {{"id", "d"}, {"attributes", {{"name", "Y"}}}},
                        {{"id", "e"}, {"type", "author"}, {"attributes", {{"name", "Z"}}}}})}}}};
    const auto info = md::parse_manga(doc.dump());
    REQUIRE(info.has_value());
    CHECK(info->authors == std::vector<std::string>{"Z"});
    CHECK(info->artists.empty());
}

TEST_CASE("parse_manga rejects malformed documents as network errors",
          "[mangadex_api][parse_manga]") {
    const json good = mdx::manga_json(mdx::clean_fixture());
    const auto mutated = [&good](auto edit) {
        json doc = good;
        edit(doc);
        return md::parse_manga(doc.dump());
    };
    CHECK(error_code(md::parse_manga("{}")) == md::Errc::network);
    CHECK(error_code(md::parse_manga("[]")) == md::Errc::network);
    CHECK(error_code(md::parse_manga("")) == md::Errc::network);
    CHECK(error_code(md::parse_manga("null")) == md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"].erase("id"); })) == md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["id"] = 42; })) == md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["id"] = "not-a-uuid"; })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"].erase("attributes"); })) == md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["attributes"].erase("title"); })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["attributes"]["title"] = "T"; })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["attributes"]["title"]["en"] = 1; })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["attributes"]["title"]["en"] = nullptr; })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["attributes"]["altTitles"] = "x"; })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) {
              d["data"]["attributes"]["altTitles"] = json::array({"x"});
          })) == md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["attributes"]["originalLanguage"] = 7; })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["relationships"] = json::object(); })) ==
          md::Errc::network);
    CHECK(error_code(mutated([](json& d) { d["data"]["relationships"] = json::array({1}); })) ==
          md::Errc::network);
}

TEST_CASE("parse_manga treats null optional fields as absent", "[mangadex_api][parse_manga]") {
    json doc = mdx::manga_json(mdx::clean_fixture());
    doc["data"]["attributes"]["altTitles"] = nullptr;
    doc["data"]["attributes"]["originalLanguage"] = nullptr;
    doc["data"]["relationships"] = nullptr;
    const auto info = md::parse_manga(doc.dump());
    REQUIRE(info.has_value());
    CHECK(info->alt_titles.empty());
    CHECK(info->original_language.empty());
    CHECK(info->authors.empty());
}

TEST_CASE("parse error messages name the document but never quote response content",
          "[mangadex_api][parse_manga]") {
    json doc = mdx::manga_json(mdx::clean_fixture());
    doc["data"]["id"] = "\x1b[31mevil";
    const auto info = md::parse_manga(doc.dump());
    REQUIRE_FALSE(info.has_value());
    CHECK(error_message(info) == "malformed MangaDex API response (manga): 'id' is not a UUID");
    CHECK(error_message(info).find("evil") == std::string::npos);
}

// ---- parse_feed_page ----------------------------------------------------------------------------

TEST_CASE("parse_feed_page reads the clean fixture feed", "[mangadex_api][parse_feed_page]") {
    const mdx::FixtureManga manga = mdx::clean_fixture();
    const auto pages = mdx::feed_pages_json(manga);
    REQUIRE(pages.size() == 1);
    const auto page = md::parse_feed_page(pages.front().dump());
    REQUIRE(page.has_value());
    CHECK(page->chapters == expected_chapters(manga));
    CHECK(page->offset == 0);
    CHECK(page->limit == 500);
    CHECK(page->total == 3);
}

TEST_CASE("parse_feed_page keeps every chapter of the basic fixture",
          "[mangadex_api][parse_feed_page]") {
    const mdx::FixtureManga manga = mdx::basic_fixture();
    const auto page = md::parse_feed_page(mdx::feed_pages_json(manga).front().dump());
    REQUIRE(page.has_value());
    REQUIRE(page->chapters == expected_chapters(manga));
    const std::vector<md::Chapter>& chapters = page->chapters;
    REQUIRE(chapters.size() == 11);
    CHECK(chapters[3].external); // externalUrl set
    CHECK(chapters[3].pages == 0);
    CHECK(chapters[4].pages == 0);  // pages == 0
    CHECK(chapters[5].unavailable); // isUnavailable
    CHECK_FALSE(chapters[6].unavailable);
    CHECK_FALSE(chapters[7].volume.has_value()); // loose chapter
    CHECK(chapters[7].chapter == "10");
    CHECK_FALSE(chapters[9].volume.has_value()); // oneshot
    CHECK_FALSE(chapters[9].chapter.has_value());
    CHECK(chapters[10].language == "fr"); // other languages are kept
    REQUIRE(chapters[0].groups.size() == 1);
    CHECK(chapters[0].groups.front().name == "Alpha Scans");
}

TEST_CASE("parse_feed_page reads the pagination window", "[mangadex_api][parse_feed_page]") {
    const mdx::FixtureManga manga = mdx::basic_fixture();
    const auto pages = mdx::feed_pages_json(manga, 4);
    REQUIRE(pages.size() == 3);
    const auto page = md::parse_feed_page(pages[1].dump());
    REQUIRE(page.has_value());
    CHECK(page->offset == 4);
    CHECK(page->limit == 4);
    CHECK(page->total == 11);
    REQUIRE(page->chapters.size() == 4);
    CHECK(page->chapters.front().id == manga.chapters[4].id);
}

TEST_CASE("parse_feed_page accepts a minimal chapter and defaults the rest",
          "[mangadex_api][parse_feed_page]") {
    const json doc = {
        {"data", json::array({{{"id", chapter_uuid}, {"attributes", {{"pages", 2}}}}})}};
    const auto page = md::parse_feed_page(doc.dump());
    REQUIRE(page.has_value());
    REQUIRE(page->chapters.size() == 1);
    CHECK(page->chapters.front() == md::Chapter{.id = std::string{chapter_uuid}, .pages = 2});
    CHECK(page->offset == 0);
    CHECK(page->limit == 0);
    CHECK(page->total == 0);
}

TEST_CASE("parse_feed_page treats null volume, chapter and title as absent",
          "[mangadex_api][parse_feed_page]") {
    const auto page = md::parse_feed_page(feed_with_attributes({{"volume", nullptr},
                                                                {"chapter", nullptr},
                                                                {"title", nullptr},
                                                                {"externalUrl", nullptr},
                                                                {"isUnavailable", nullptr},
                                                                {"pages", 1}}));
    REQUIRE(page.has_value());
    REQUIRE(page->chapters.size() == 1);
    const md::Chapter& chapter = page->chapters.front();
    CHECK_FALSE(chapter.volume.has_value());
    CHECK_FALSE(chapter.chapter.has_value());
    CHECK(chapter.title.empty());
    CHECK_FALSE(chapter.external);
    CHECK_FALSE(chapter.unavailable);
}

TEST_CASE("parse_feed_page accepts the full int range of pages",
          "[mangadex_api][parse_feed_page]") {
    const auto page = md::parse_feed_page(feed_with_attributes({{"pages", 2147483647}}));
    REQUIRE(page.has_value());
    REQUIRE(page->chapters.size() == 1);
    CHECK(page->chapters.front().pages == 2147483647);
}

TEST_CASE("parse_feed_page rejects wrong types and missing required fields",
          "[mangadex_api][parse_feed_page]") {
    const auto attrs = [](json attributes) {
        return error_code(md::parse_feed_page(feed_with_attributes(std::move(attributes))));
    };
    CHECK(attrs({{"pages", "3"}}) == md::Errc::network);
    CHECK(attrs({{"pages", -1}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1.5}}) == md::Errc::network);
    CHECK(attrs({{"pages", 2147483648}}) == md::Errc::network);
    CHECK(attrs({{"pages", nullptr}}) == md::Errc::network);
    CHECK(attrs(json::object()) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"volume", 1}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"chapter", false}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"title", json::array()}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"translatedLanguage", 1}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"externalUrl", true}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"isUnavailable", "yes"}}) == md::Errc::network);
    CHECK(attrs({{"pages", 1}, {"publishAt", 0}}) == md::Errc::network);

    const auto doc = [](const json& feed) { return error_code(md::parse_feed_page(feed.dump())); };
    CHECK(error_code(md::parse_feed_page("{}")) == md::Errc::network);
    CHECK(doc({{"data", json::object()}}) == md::Errc::network);
    CHECK(doc({{"data", json::array({1})}}) == md::Errc::network);
    CHECK(doc({{"data", json::array({{{"attributes", {{"pages", 1}}}}})}}) == md::Errc::network);
    CHECK(doc({{"data", json::array({{{"id", chapter_uuid}}})}}) == md::Errc::network);
    CHECK(doc({{"data", json::array()}, {"total", "3"}}) == md::Errc::network);
    CHECK(doc({{"data", json::array()}, {"total", -1}}) == md::Errc::network);
    CHECK(doc({{"data", json::array()}, {"offset", 0.5}}) == md::Errc::network);
}

TEST_CASE("parse_feed_page requires every chapter id to be a UUID",
          "[mangadex_api][parse_feed_page]") {
    for (const std::string_view id :
         {"", "../../etc/passwd", "abcdef01-2345-4678-89ab-cdef0123456", "x/y",
          "abcdef01-2345-4678-89ab-cdef012345670", "abcdef01_2345_4678_89ab_cdef01234567"}) {
        CAPTURE(id);
        const json doc = {{"data", json::array({{{"id", id}, {"attributes", {{"pages", 1}}}}})}};
        CHECK(error_code(md::parse_feed_page(doc.dump())) == md::Errc::network);
    }
    const json upper = {{"data", json::array({{{"id", "ABCDEF01-2345-4678-89AB-CDEF01234567"},
                                               {"attributes", {{"pages", 1}}}}})}};
    CHECK(md::parse_feed_page(upper.dump()).has_value());
}

TEST_CASE("parse_feed_page reads scanlation groups and ignores other relationships",
          "[mangadex_api][parse_feed_page]") {
    const json doc = {
        {"data",
         json::array(
             {{{"id", chapter_uuid},
               {"attributes", {{"pages", 1}}},
               {"relationships",
                json::array({{{"id", group_one}, {"type", "scanlation_group"}},
                             {{"id", "m"}, {"type", "manga"}},
                             {{"id", "u"}, {"type", "user"}, {"attributes", {{"name", "U"}}}},
                             {{"id", group_two},
                              {"type", "scanlation_group"},
                              {"attributes", {{"name", "Two"}}}}})}}})}};
    const auto page = md::parse_feed_page(doc.dump());
    REQUIRE(page.has_value());
    REQUIRE(page->chapters.size() == 1);
    CHECK(page->chapters.front().groups ==
          std::vector<md::ScanGroup>{{.id = std::string{group_one}, .name = ""},
                                     {.id = std::string{group_two}, .name = "Two"}});

    json no_id = doc;
    no_id["data"][0]["relationships"][0].erase("id");
    CHECK(error_code(md::parse_feed_page(no_id.dump())) == md::Errc::network);

    for (const std::string_view bad : {"", "g1", "../x", "abcdef01-2345-4678-89ab-cdef0123456"}) {
        CAPTURE(bad);
        json bad_id = doc;
        bad_id["data"][0]["relationships"][3]["id"] = bad;
        const auto result = md::parse_feed_page(bad_id.dump());
        CHECK(error_code(result) == md::Errc::network);
        CHECK(error_message(result) == "malformed MangaDex API response (feed page): scanlation "
                                       "group 'id' is not a UUID");
    }
}

// ---- hostile input: size, depth, UTF-8, trailing data ----------------------------------------

TEST_CASE("parsers reject invalid UTF-8 inside a string", "[mangadex_api][hostile]") {
    std::string text = mdx::manga_json(mdx::clean_fixture()).dump();
    const std::size_t at = text.find("Clean Title");
    REQUIRE(at != std::string::npos);
    text.replace(at, 5, "Cl\xFF\xFEn");
    CHECK(error_code(md::parse_manga(text)) == md::Errc::network);

    std::string feed = mdx::feed_pages_json(mdx::clean_fixture()).front().dump();
    const std::size_t title = feed.find("Beginnings");
    REQUIRE(title != std::string::npos);
    feed.replace(title, 1, "\xC0");
    CHECK(error_code(md::parse_feed_page(feed)) == md::Errc::network);
}

TEST_CASE("parsers reject trailing garbage and concatenated documents", "[mangadex_api][hostile]") {
    const std::string text = mdx::at_home_json(mdx::clean_fixture().chapters.front()).dump();
    REQUIRE(md::parse_at_home(text).has_value());
    CHECK(md::parse_at_home(text + " \n\t").has_value()); // trailing whitespace is fine
    CHECK(error_code(md::parse_at_home(text + "x")) == md::Errc::network);
    CHECK(error_code(md::parse_at_home(text + text)) == md::Errc::network);
    CHECK(error_code(md::parse_at_home(text + ",")) == md::Errc::network);
    CHECK(error_code(md::parse_at_home(text + "/* c */")) == md::Errc::network);
    CHECK(error_code(md::parse_at_home(text.substr(0, text.size() - 1))) == md::Errc::network);
}

TEST_CASE("parsers accept exactly max_api_json_bytes and reject one byte more",
          "[mangadex_api][hostile]") {
    std::string text = mdx::at_home_json(mdx::clean_fixture().chapters.front()).dump();
    REQUIRE(text.size() < md::max_api_json_bytes);
    text.resize(md::max_api_json_bytes, ' ');
    CHECK(md::parse_at_home(text).has_value());
    text.push_back(' ');
    const auto too_big = md::parse_at_home(text);
    CHECK(error_code(too_big) == md::Errc::network);
    CHECK(error_message(too_big).find("larger than 16 MiB") != std::string::npos);
}

TEST_CASE("parsers accept nesting depth 64 and reject 65", "[mangadex_api][hostile]") {
    const std::string manga = mdx::manga_json(mdx::clean_fixture()).dump();
    REQUIRE(md::parse_manga(manga).has_value());
    const std::string depth_64 = with_nested_member(manga, md::max_json_depth - 1);
    const std::string depth_65 = with_nested_member(manga, md::max_json_depth);
    CHECK(md::parse_manga(depth_64).has_value());
    const auto rejected = md::parse_manga(depth_65);
    CHECK(error_code(rejected) == md::Errc::network);
    CHECK(error_message(rejected).find("nested deeper than 64 levels") != std::string::npos);
    // Objects count like arrays.
    std::string objects = "{\"pad\":";
    for (std::size_t i = 0; i < md::max_json_depth; ++i) {
        objects += "{\"a\":";
    }
    objects += '0';
    objects.append(md::max_json_depth, '}');
    objects += ',';
    objects.append(std::string_view{manga}.substr(1));
    CHECK(error_code(md::parse_manga(objects)) == md::Errc::network);
}

TEST_CASE("the depth pre-scan rejects a million-deep document without recursing",
          "[mangadex_api][hostile]") {
    constexpr std::size_t deep = 1'000'000;
    std::string arrays(deep, '[');
    arrays.append(deep, ']');
    CHECK(error_code(md::parse_feed_page(arrays)) == md::Errc::network);
    const std::string unclosed(deep, '{');
    CHECK(error_code(md::parse_cover_page(unclosed)) == md::Errc::network);
}

TEST_CASE("a page of millions of junk entries is rejected", "[mangadex_api][hostile]") {
    // ~16 MiB of `0,`: the parsers must reject the first entry without reserving for all of them.
    std::string junk = "{\"data\":[";
    while (junk.size() + 4 < md::max_api_json_bytes) {
        junk += "0,";
    }
    junk += "0]}";
    REQUIRE(junk.size() <= md::max_api_json_bytes);
    CHECK(error_code(md::parse_feed_page(junk)) == md::Errc::network);
    CHECK(error_code(md::parse_cover_page(junk)) == md::Errc::network);
}

TEST_CASE("brackets and escaped quotes inside strings do not count as nesting",
          "[mangadex_api][hostile]") {
    const std::string tricky =
        std::string(100, '[') + "\"\\" + std::string(100, '{') + "\\\"" + std::string(100, ']');
    json doc = mdx::manga_json(mdx::clean_fixture());
    doc["data"]["attributes"]["title"]["en"] = tricky;
    const auto info = md::parse_manga(doc.dump());
    REQUIRE(info.has_value());
    CHECK(info->titles == std::vector<md::LocalizedText>{{"en", tricky}});
}

TEST_CASE("stray closing brackets are malformed, not a depth bypass", "[mangadex_api][hostile]") {
    std::string text = "]]]]";
    text.append(md::max_json_depth + 1, '[');
    text.append(md::max_json_depth + 1, ']');
    const auto result = md::parse_feed_page(text);
    CHECK(error_code(result) == md::Errc::network);
    CHECK(error_message(result).find("nested deeper than 64 levels") != std::string::npos);
}

TEST_CASE("an escaped backslash before a closing quote does not hide nesting",
          "[mangadex_api][hostile]") {
    // The string "x\\" ends at the second quote: a scanner that treated the escaped backslash as
    // escaping the quote would stay "inside the string" and under-count the brackets after it.
    std::string text = R"({"a":"x\\","b":)";
    text.append(md::max_json_depth + 1, '[');
    text.append(md::max_json_depth + 1, ']');
    text += '}';
    const auto result = md::parse_manga(text);
    CHECK(error_code(result) == md::Errc::network);
    CHECK(error_message(result).find("nested deeper than 64 levels") != std::string::npos);
}

TEST_CASE("an unterminated string after depth 64 is malformed, not a crash",
          "[mangadex_api][hostile]") {
    // The scan sees depth 64 and then one endless string (its brackets are string content); the
    // parser must reject the unterminated string without recursing.
    std::string text(md::max_json_depth, '[');
    text += '"';
    text.append(1'000'000, '[');
    const auto result = md::parse_feed_page(text);
    CHECK(error_code(result) == md::Errc::network);
    CHECK(error_message(result).find("nested deeper") == std::string::npos);
}

// ---- parse_cover_page ---------------------------------------------------------------------------

TEST_CASE("parse_cover_page reads the basic fixture covers, nullable volume and locale",
          "[mangadex_api][parse_cover_page]") {
    const mdx::FixtureManga manga = mdx::basic_fixture();
    const auto pages = mdx::covers_json(manga);
    REQUIRE(pages.size() == 1);
    const auto page = md::parse_cover_page(pages.front().dump());
    REQUIRE(page.has_value());
    CHECK(page->covers == expected_covers(manga));
    CHECK(page->offset == 0);
    CHECK(page->limit == 100);
    CHECK(page->total == 4);
    REQUIRE(page->covers.size() == 4);
    CHECK(page->covers[2].volume == "2");
    CHECK_FALSE(page->covers[2].locale.has_value());
    CHECK_FALSE(page->covers[3].volume.has_value());
    CHECK(page->covers[3].locale == "ja");
}

TEST_CASE("parse_cover_page reads the clean fixture covers", "[mangadex_api][parse_cover_page]") {
    const mdx::FixtureManga manga = mdx::clean_fixture();
    const auto page = md::parse_cover_page(mdx::covers_json(manga).front().dump());
    REQUIRE(page.has_value());
    CHECK(page->covers == expected_covers(manga));
}

TEST_CASE("parse_cover_page rejects malformed covers", "[mangadex_api][parse_cover_page]") {
    const auto cover = [](json attributes) {
        const json doc = {
            {"data", json::array({{{"id", "c"}, {"attributes", std::move(attributes)}}})}};
        return error_code(md::parse_cover_page(doc.dump()));
    };
    CHECK(cover({{"fileName", "a.png"}}) == std::nullopt);
    CHECK(cover({{"fileName", "a.png"}, {"volume", nullptr}, {"locale", nullptr}}) == std::nullopt);
    CHECK(cover(json::object()) == md::Errc::network);
    CHECK(cover({{"fileName", nullptr}}) == md::Errc::network);
    CHECK(cover({{"fileName", 3}}) == md::Errc::network);
    CHECK(cover({{"fileName", "../a.png"}}) == md::Errc::network);
    CHECK(cover({{"fileName", "a/b.png"}}) == md::Errc::network);
    CHECK(cover({{"fileName", ".hidden"}}) == md::Errc::network);
    CHECK(cover({{"fileName", ""}}) == md::Errc::network);
    CHECK(cover({{"fileName", "a.png"}, {"volume", 1}}) == md::Errc::network);
    CHECK(cover({{"fileName", "a.png"}, {"locale", json::array()}}) == md::Errc::network);
    CHECK(error_code(md::parse_cover_page("{}")) == md::Errc::network);
    CHECK(error_code(md::parse_cover_page(R"({"data":[{"id":"c"}]})")) == md::Errc::network);
    CHECK(error_code(md::parse_cover_page(R"({"data":["c"]})")) == md::Errc::network);
}

// ---- parse_at_home ------------------------------------------------------------------------------

TEST_CASE("parse_at_home reads the clean fixture document", "[mangadex_api][parse_at_home]") {
    const mdx::FixtureChapter chapter = mdx::clean_fixture().chapters.front();
    const json doc = mdx::at_home_json(chapter);
    const auto server = md::parse_at_home(doc.dump());
    REQUIRE(server.has_value());
    CHECK(server->base_url == "https://fixture.invalid");
    CHECK(server->hash == chapter.hash);
    CHECK(server->data == doc["chapter"]["data"].get<std::vector<std::string>>());
    CHECK(server->data_saver == doc["chapter"]["dataSaver"].get<std::vector<std::string>>());
    CHECK(server->data.size() == chapter.images.size());
}

TEST_CASE("parse_at_home strips trailing slashes from baseUrl", "[mangadex_api][parse_at_home]") {
    const auto server = md::parse_at_home(
        edited_at_home([](json& d) { d["baseUrl"] = "https://node.example.net:443/tok//"; }));
    REQUIRE(server.has_value());
    CHECK(server->base_url == "https://node.example.net:443/tok");
}

TEST_CASE("parse_at_home allows a missing or null dataSaver list",
          "[mangadex_api][parse_at_home]") {
    const auto absent =
        md::parse_at_home(edited_at_home([](json& d) { d["chapter"].erase("dataSaver"); }));
    REQUIRE(absent.has_value());
    CHECK(absent->data_saver.empty());
    const auto null =
        md::parse_at_home(edited_at_home([](json& d) { d["chapter"]["dataSaver"] = nullptr; }));
    REQUIRE(null.has_value());
    CHECK(null->data_saver.empty());
}

TEST_CASE("parse_at_home validates baseUrl, hash and every file name (acceptance 9)",
          "[mangadex_api][parse_at_home][acceptance-9]") {
    const auto with = [](auto edit) { return error_code(md::parse_at_home(edited_at_home(edit))); };
    for (const std::string_view url :
         {"http://fixture.invalid", "https://u@fixture.invalid", "https://fixture.invalid?x",
          "https://fixture.invalid/#f", "", "https://"}) {
        CAPTURE(url);
        CHECK(with([url](json& d) { d["baseUrl"] = url; }) == md::Errc::network);
    }
    for (const std::string_view bad : {"../x", ".x", "x/y", "", "-x", "x y", "x%2f"}) {
        CAPTURE(bad);
        CHECK(with([bad](json& d) { d["chapter"]["hash"] = bad; }) == md::Errc::network);
        CHECK(with([bad](json& d) { d["chapter"]["data"][0] = bad; }) == md::Errc::network);
        CHECK(with([bad](json& d) { d["chapter"]["dataSaver"][1] = bad; }) == md::Errc::network);
    }
    CHECK(with([](json& d) { d["chapter"]["data"][0] = 5; }) == md::Errc::network);
    CHECK(with([](json& d) { d["chapter"]["dataSaver"] = "x"; }) == md::Errc::network);
    CHECK(with([](json& d) { d["chapter"]["hash"] = 1; }) == md::Errc::network);
    CHECK(with([](json& d) { d["baseUrl"] = 1; }) == md::Errc::network);
    CHECK(with([](json& d) { d.erase("baseUrl"); }) == md::Errc::network);
    CHECK(with([](json& d) { d.erase("chapter"); }) == md::Errc::network);
    CHECK(with([](json& d) { d["chapter"].erase("hash"); }) == md::Errc::network);
    CHECK(with([](json& d) { d["chapter"].erase("data"); }) == md::Errc::network);
    CHECK(with([](json& d) { d["chapter"]["data"] = nullptr; }) == md::Errc::network);
    CHECK(error_code(md::parse_at_home("{}")) == md::Errc::network);
}

TEST_CASE("parse_at_home accepts 5000 pages and rejects 5001", "[mangadex_api][parse_at_home]") {
    const auto pages = [](std::size_t count, std::string_view key) {
        return md::parse_at_home(edited_at_home([count, key](json& d) {
            json names = json::array();
            for (std::size_t i = 0; i < count; ++i) {
                names.push_back(std::format("p{}.png", i));
            }
            d["chapter"][std::string{key}] = std::move(names);
        }));
    };
    const auto max = pages(md::max_pages_per_chapter, "data");
    REQUIRE(max.has_value());
    CHECK(max->data.size() == md::max_pages_per_chapter);
    CHECK(error_code(pages(md::max_pages_per_chapter + 1, "data")) == md::Errc::network);
    CHECK(error_code(pages(md::max_pages_per_chapter + 1, "dataSaver")) == md::Errc::network);
}

// ---- validators (acceptance 9) ------------------------------------------------------------------

TEST_CASE("is_valid_at_home_token accepts safe path segments", "[mangadex_api][acceptance-9]") {
    for (const std::string_view token :
         {"a", "Z", "0", "x.png", "1-abc_DEF.jpg", "a..b", "a-", "a_", "a.",
          "d4f5e6a7b8c9d0e1f2a3b4c5d6e7f8a9", "11111111-1111-4111-8111-111111111111"}) {
        CAPTURE(token);
        CHECK(md::is_valid_at_home_token(token));
    }
}

TEST_CASE("is_valid_at_home_token rejects unsafe tokens", "[mangadex_api][acceptance-9]") {
    using namespace std::string_view_literals;
    for (const std::string_view token :
         {""sv, "."sv, ".."sv, "../x"sv, ".x"sv, "x/y"sv, "-x"sv, "_x"sv, "x y"sv, "x\\y"sv,
          "x%2fy"sv, "x?y"sv, "x#y"sv, "x:y"sv, "\xC3\xA9"sv, "a\0b"sv, "a\nb"sv}) {
        CAPTURE(token);
        // CHECK(!x) rather than CHECK_FALSE: clang-analyzer flags Catch2's Flags | Flags cast
        // (optin.core.EnumCastOutOfRange) when CHECK_FALSE sits in a loop.
        CHECK(!md::is_valid_at_home_token(token));
    }
}

TEST_CASE("validate_at_home_base_url accepts plain https URLs", "[mangadex_api][acceptance-9]") {
    for (const std::string_view url :
         {"https://fixture.invalid", "https://uploads.mangadex.org",
          "https://abc.def.mangadex.network:443", "https://h/", "https://h/token/abc",
          "https://h:8443/a-b_c.d~e", "https://127.0.0.1"}) {
        CAPTURE(url);
        CHECK(md::validate_at_home_base_url(url).has_value());
    }
}

TEST_CASE("validate_at_home_base_url rejects everything else", "[mangadex_api][acceptance-9]") {
    using namespace std::string_view_literals;
    for (const std::string_view url : {""sv,
                                       "http://h"sv,
                                       "ftp://h"sv,
                                       "HTTPS://h"sv,
                                       "https:/h"sv,
                                       "//h"sv,
                                       "h"sv,
                                       "https://"sv,
                                       "https:///x"sv,
                                       "https://u@h"sv,
                                       "https://u:p@h"sv,
                                       "https://@h"sv,
                                       "https://h?x"sv,
                                       "https://h/?x"sv,
                                       "https://h#f"sv,
                                       "https://h/#f"sv,
                                       "https://h:"sv,
                                       "https://h:abc"sv,
                                       "https://h:123456"sv,
                                       "https://:443"sv,
                                       "https://h/a b"sv,
                                       "https://h\\@evil"sv,
                                       "https://h/.."sv,
                                       "https://h/../x"sv,
                                       "https://h/./x"sv,
                                       "https://h/a/.."sv,
                                       "https://[::1]"sv,
                                       "https://h_x"sv,
                                       "https://h\0x"sv,
                                       "https://h\n"sv,
                                       "https://h\xC3\xA9"sv}) {
        CAPTURE(url);
        const auto result = md::validate_at_home_base_url(url);
        CHECK(error_code(result) == md::Errc::network);
    }
}

// ---- request URLs -------------------------------------------------------------------------------

TEST_CASE("manga_request_url asks for the cover, author and artist includes",
          "[mangadex_api][urls]") {
    CHECK(md::manga_request_url("https://api.mangadex.org", md::MangaId{}) ==
          "https://api.mangadex.org/manga/00000000-0000-0000-0000-000000000000"
          "?includes[]=cover_art&includes[]=author&includes[]=artist");
}

TEST_CASE("feed_request_url sends every parameter in a fixed order", "[mangadex_api][urls]") {
    const std::string tail = "&order[volume]=asc&order[chapter]=asc&includes[]=scanlation_group"
                             "&contentRating[]=safe&contentRating[]=suggestive"
                             "&contentRating[]=erotica&contentRating[]=pornographic"
                             "&includeFuturePublishAt=0&includeUnavailable=1";
    CHECK(md::feed_request_url("https://api.mangadex.org", md::MangaId{}, "en", 0) ==
          "https://api.mangadex.org/manga/00000000-0000-0000-0000-000000000000/feed"
          "?translatedLanguage[]=en&limit=500&offset=0" +
              tail);
    CHECK(md::feed_request_url("file:///fx/api", md::MangaId{}, "pt-br", 9500) ==
          "file:///fx/api/manga/00000000-0000-0000-0000-000000000000/feed"
          "?translatedLanguage[]=pt-br&limit=500&offset=9500" +
              tail);
    // Defensive: a language code can never add a parameter or a fragment.
    CHECK(md::feed_request_url("https://a", md::MangaId{}, "en&x=1#f", 0)
              .starts_with("https://a/manga/00000000-0000-0000-0000-000000000000/feed"
                           "?translatedLanguage[]=en%26x%3D1%23f&limit=500"));
}

// Regression: MangaDex treats includeEmptyPages=1 / includeExternalUrl=1 as filters (only empty /
// only external chapters), so sending them made every normal title look empty (exit 5).
TEST_CASE("feed_request_url never sends the includeEmptyPages or includeExternalUrl filters",
          "[mangadex_api][urls][regression]") {
    using namespace std::string_view_literals;
    for (const std::string_view lang : {"en"sv, "pt-br"sv}) {
        for (const std::size_t offset : {std::size_t{0}, std::size_t{500}}) {
            CAPTURE(lang, offset);
            const std::string url =
                md::feed_request_url("https://api.mangadex.org", md::MangaId{}, lang, offset);
            CHECK(url.find("includeEmptyPages") == std::string::npos);
            CHECK(url.find("includeExternalUrl") == std::string::npos);
        }
    }
}

TEST_CASE("cover_request_url pages by 100", "[mangadex_api][urls]") {
    CHECK(md::cover_request_url("https://api.mangadex.org", md::MangaId{}, 0) ==
          "https://api.mangadex.org/cover?manga[]=00000000-0000-0000-0000-000000000000"
          "&limit=100&offset=0");
    CHECK(md::cover_request_url("https://api.mangadex.org", md::MangaId{}, 200) ==
          "https://api.mangadex.org/cover?manga[]=00000000-0000-0000-0000-000000000000"
          "&limit=100&offset=200");
}

TEST_CASE("at_home_request_url names the chapter", "[mangadex_api][urls]") {
    CHECK(md::at_home_request_url("https://api.mangadex.org", chapter_uuid) ==
          "https://api.mangadex.org/at-home/server/abcdef01-2345-4678-89ab-cdef01234567");
    CHECK(md::at_home_request_url("https://a", "x/../y?z") ==
          "https://a/at-home/server/x%2F..%2Fy%3Fz");
}

TEST_CASE("image_url selects the data or data-saver set", "[mangadex_api][urls]") {
    CHECK(md::image_url("https://node.example.net:443/tok", md::ImageQuality::data, "abc123",
                        "p001.png") == "https://node.example.net:443/tok/data/abc123/p001.png");
    CHECK(md::image_url("https://fixture.invalid", md::ImageQuality::data_saver, "abc123",
                        "s001.jpg") == "https://fixture.invalid/data-saver/abc123/s001.jpg");
}

TEST_CASE("cover_image_url builds the uploads path", "[mangadex_api][urls]") {
    CHECK(md::cover_image_url("https://uploads.mangadex.org", md::MangaId{}, "c.jpg") ==
          "https://uploads.mangadex.org/covers/00000000-0000-0000-0000-000000000000/c.jpg");
}

// ---- MD@H reports -------------------------------------------------------------------------------

TEST_CASE("is_mangadex_org_host matches mangadex.org and its subdomains only",
          "[mangadex_api][report]") {
    for (const std::string_view url :
         {"https://mangadex.org", "https://uploads.mangadex.org/data/h/f.png",
          "https://MangaDex.ORG:443/x", "https://a.b.mangadex.org/", "https://mangadex.org./x",
          "https://u@mangadex.org/x", "https://mangadex.org?x", "https://x.mangadex.org#f"}) {
        CAPTURE(url);
        CHECK(md::is_mangadex_org_host(url));
    }
    for (const std::string_view url :
         {"", "mangadex.org", "https://", "https://fixture.invalid",
          "https://api.mangadex.network/report", "https://evilmangadex.org",
          "https://mangadex.org.evil.example", "https://mangadex.org@evil.example/x",
          "https://.mangadex.org", "https://[::1]/mangadex.org",
          "https://evil.example/mangadex.org", "https://evil.example/?h=.mangadex.org"}) {
        CAPTURE(url);
        CHECK(!md::is_mangadex_org_host(url)); // not CHECK_FALSE in a loop (see above)
    }
}

TEST_CASE("at_home_report_json has the documented shape and key order", "[mangadex_api][report]") {
    CHECK(md::at_home_report_json(md::AtHomeReport{.url = "https://n.example/data/h/p.png",
                                                   .success = true,
                                                   .cached = true,
                                                   .bytes = 123456,
                                                   .duration_ms = 789}) ==
          R"({"url":"https://n.example/data/h/p.png","success":true,"cached":true,)"
          R"("bytes":123456,"duration":789})");
    CHECK(md::at_home_report_json(
              md::AtHomeReport{.url = "https://n.example/data/h/p.png", .duration_ms = 30000}) ==
          R"({"url":"https://n.example/data/h/p.png","success":false,"cached":false,)"
          R"("bytes":0,"duration":30000})");
}

TEST_CASE("at_home_report_json escapes and repairs the URL", "[mangadex_api][report]") {
    const std::string text =
        md::at_home_report_json(md::AtHomeReport{.url = "https://n/\"q\"\x01\xFF", .bytes = 1});
    const json doc = json::parse(text);
    CHECK(doc.at("url") == "https://n/\"q\"\x01\xEF\xBF\xBD");
    CHECK(doc.at("bytes") == 1);
    CHECK(doc.size() == 5);
}
