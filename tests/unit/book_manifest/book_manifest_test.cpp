// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/book_manifest/book_manifest_test.cpp: the per-book manifest (skip-existing plan T4):
// the writer's exact compact bytes and determinism, the parse round trip, the separator bound
// (fact-check V16), held image entry names, and the Chapter <-> ManifestChapter conversions.
// Hostile input lives in book_manifest_hostile_test.cpp.

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;

constexpr std::string_view chapter_one = "abcdef01-2345-4678-89ab-cdef01234567";
constexpr std::string_view chapter_two = "abcdef01-2345-4678-89ab-cdef01234568";
constexpr std::string_view group_one = "0000000a-0002-4000-8000-000000000001";
constexpr std::string_view group_two = "0000000a-0002-4000-8000-000000000002";

/// The small manifest whose exact bytes are pinned below.
[[nodiscard]] md::BookManifest small_manifest() {
    return md::BookManifest{
        .identifier = "urn:mangadex:11111111-0001-4000-8000-000000000001:en:v1",
        .opf_crc32 = 0x0a1b2c3dU,
        .cover = md::ManifestCover{.file_name = "cover-1.jpg", .entry = "OEBPS/images/cover.jpg"},
        .chapters = {md::ManifestChapter{
            .id = std::string{chapter_one},
            .volume = "1",
            .chapter = std::nullopt,
            .title = "T",
            .groups = {md::ScanGroup{.id = std::string{group_one}, .name = "G"}},
            .publish_at = "2024-01-01T00:00:00+00:00",
            .updated_at = "2024-01-02T00:00:00+00:00",
            .quality = md::ImageQuality::data_saver,
            .images = {"OEBPS/images/p00001.png", "OEBPS/images/p00002.webp"},
        }},
    };
}

/// A richer manifest: two chapters, both qualities, null labels, a unicode title, two groups.
[[nodiscard]] md::BookManifest rich_manifest(bool with_cover) {
    md::BookManifest manifest{
        .identifier = "urn:mangadex:11111111-0001-4000-8000-000000000001:ja:single",
        .opf_crc32 = 0xffffffffU,
        .cover = std::nullopt,
        .chapters =
            {
                md::ManifestChapter{
                    .id = std::string{chapter_one},
                    .volume = std::nullopt,
                    .chapter = std::nullopt,
                    .title = "\xE3\x81\x82\xE3\x81\x84 \xF0\x9F\x93\x96 caf\xC3\xA9",
                    .groups = {},
                    .publish_at = "2023-05-06T07:08:09+00:00",
                    .updated_at = "2023-05-06T07:08:10+00:00",
                    .quality = md::ImageQuality::data,
                    .images = {"OEBPS/images/p00001.jpg"},
                },
                md::ManifestChapter{
                    .id = std::string{chapter_two},
                    .volume = "2",
                    .chapter = "10.5",
                    .title = "",
                    .groups = {md::ScanGroup{.id = std::string{group_one}, .name = "A \"quoted\""},
                               md::ScanGroup{.id = std::string{group_two}, .name = "B\\slash"}},
                    .publish_at = "",
                    .updated_at = "",
                    .quality = md::ImageQuality::data_saver,
                    .images = {"OEBPS/images/p00002.gif", "OEBPS/images/p00003.png",
                               "OEBPS/images/p00004.webp"},
                },
            },
    };
    if (with_cover) {
        manifest.cover =
            md::ManifestCover{.file_name = "f00d.png", .entry = "OEBPS/images/cover.png"};
    }
    return manifest;
}

/// Structural separators (`, : [ {`) outside JSON strings: the quantity the pre-scan bounds,
/// counted independently of src/core/json_util.cpp.
[[nodiscard]] std::size_t count_separators(std::string_view text) {
    std::size_t count = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == ',' || c == ':' || c == '[' || c == '{') {
            ++count;
        }
    }
    return count;
}

/// The upper bound justified in book_manifest.cpp: 17 + C * (20 + 5g) + I.
[[nodiscard]] constexpr std::size_t separator_bound(std::size_t chapters, std::size_t groups,
                                                    std::size_t images) {
    return 17 + (chapters * (20 + (5 * groups))) + images;
}

// At the zip entry cap (32767 images, one per chapter) up to 8 groups per chapter fit under the
// cap, and 9 do not (plan D12; the comment in book_manifest.cpp).
constexpr std::size_t max_images = md::zip_max_entries / 2;
static_assert(max_images == 32767);
static_assert(separator_bound(max_images, 4, max_images) == 1'343'464);
static_assert(separator_bound(max_images, 8, max_images) == 1'998'804);
static_assert(separator_bound(max_images, 8, max_images) < md::max_manifest_separators);
static_assert(separator_bound(max_images, 9, max_images) > md::max_manifest_separators);

} // namespace

TEST_CASE("manifest_to_json writes the exact compact document", "[book_manifest][write]") {
    const std::string expected =
        R"({"format":1,"generator":"mangadex-dl",)"
        R"("identifier":"urn:mangadex:11111111-0001-4000-8000-000000000001:en:v1",)"
        R"("opf_crc32":"0a1b2c3d",)"
        R"("cover":{"file_name":"cover-1.jpg","entry":"OEBPS/images/cover.jpg"},)"
        R"("chapters":[{"id":"abcdef01-2345-4678-89ab-cdef01234567","volume":"1","chapter":null,)"
        R"("title":"T","groups":[{"id":"0000000a-0002-4000-8000-000000000001","name":"G"}],)"
        R"("publish_at":"2024-01-01T00:00:00+00:00","updated_at":"2024-01-02T00:00:00+00:00",)"
        R"("quality":"data-saver","images":["OEBPS/images/p00001.png","OEBPS/images/p00002.webp"]}]})";
    CHECK(md::manifest_to_json(small_manifest()) == expected);
}

TEST_CASE("manifest_to_json is deterministic", "[book_manifest][write]") {
    const md::BookManifest manifest = rich_manifest(true);
    const std::string first = md::manifest_to_json(manifest);
    CHECK(md::manifest_to_json(manifest) == first);
    CHECK(md::manifest_to_json(md::BookManifest{manifest}) == first);
    CHECK_FALSE(first.ends_with('\n'));
}

TEST_CASE("manifest_to_json writes a missing cover as null and the CRC zero-padded",
          "[book_manifest][write]") {
    md::BookManifest manifest = small_manifest();
    manifest.cover = std::nullopt;
    manifest.opf_crc32 = 0;
    const std::string text = md::manifest_to_json(manifest);
    CHECK(text.find(R"("opf_crc32":"00000000","cover":null,"chapters":[)") != std::string::npos);
}

TEST_CASE("manifest_to_json escapes control characters and replaces invalid UTF-8",
          "[book_manifest][write]") {
    md::BookManifest manifest = small_manifest();
    manifest.chapters.front().title = "a\x1b[31mb\xFFz";
    const std::string text = md::manifest_to_json(manifest);
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find(R"("title":"a\u001b[31mb)"
                    "\xEF\xBF\xBD"
                    R"(z")") != std::string::npos);
    // The replaced document parses, with U+FFFD in place of the bad byte.
    const auto parsed = md::parse_manifest(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->chapters.front().title == "a\x1b[31mb\xEF\xBF\xBDz");
}

TEST_CASE("parse_manifest round-trips what manifest_to_json writes", "[book_manifest][parse]") {
    SECTION("the small manifest: a cover, a null chapter label, data-saver") {
        const md::BookManifest manifest = small_manifest();
        const auto parsed = md::parse_manifest(md::manifest_to_json(manifest));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == manifest);
    }
    SECTION("no cover, null labels, unicode and escaped titles, both qualities") {
        const md::BookManifest manifest = rich_manifest(false);
        const auto parsed = md::parse_manifest(md::manifest_to_json(manifest));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == manifest);
        CHECK_FALSE(parsed->cover.has_value());
    }
    SECTION("one cover") {
        const md::BookManifest manifest = rich_manifest(true);
        const auto parsed = md::parse_manifest(md::manifest_to_json(manifest));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == manifest);
        CHECK(parsed->cover.has_value());
    }
    SECTION("the largest CRC and strings at the 4 KiB cap") {
        md::BookManifest manifest = small_manifest();
        manifest.opf_crc32 = 0xdeadbeefU;
        manifest.identifier = std::string(md::max_manifest_string_bytes, 'i');
        manifest.chapters.front().title = std::string(md::max_manifest_string_bytes, 't');
        const auto parsed = md::parse_manifest(md::manifest_to_json(manifest));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == manifest);
    }
}

TEST_CASE("a manifest at the plan's size bound parses and stays under the separator cap",
          "[book_manifest][parse][bound]") {
    // 32764 one-image chapters with 4 groups each (plan T4 bound test, fact-check V16).
    constexpr std::size_t chapter_count = 32764;
    constexpr std::size_t groups_per_chapter = 4;
    md::BookManifest manifest{
        .identifier = "urn:mangadex:11111111-0001-4000-8000-000000000001:en:single",
        .opf_crc32 = 0x12345678U,
        .cover = md::ManifestCover{.file_name = "c.jpg", .entry = "OEBPS/images/cover.jpg"},
        .chapters = {},
    };
    manifest.chapters.reserve(chapter_count);
    for (std::size_t i = 0; i < chapter_count; ++i) {
        md::ManifestChapter chapter{
            .id = std::format("{:08x}-0000-4000-8000-000000000000", i),
            .volume = std::format("{}", (i / 10) + 1),
            .chapter = std::format("{}", i + 1),
            .title = "",
            .groups = {},
            .publish_at = "2024-01-01T00:00:00+00:00",
            .updated_at = "2024-01-01T00:00:00+00:00",
            .quality = md::ImageQuality::data,
            .images = {std::format("OEBPS/images/p{:05}.jpg", i + 1)},
        };
        for (std::size_t g = 0; g < groups_per_chapter; ++g) {
            chapter.groups.push_back(md::ScanGroup{
                .id = std::format("0000000a-0002-4000-8000-{:012x}", g),
                .name = "G",
            });
        }
        manifest.chapters.push_back(std::move(chapter));
    }
    const std::string text = md::manifest_to_json(manifest);
    REQUIRE(text.size() <= md::max_manifest_bytes);
    const std::size_t separators = count_separators(text);
    CHECK(separators <= separator_bound(chapter_count, groups_per_chapter, chapter_count));
    CHECK(separators < md::max_manifest_separators);

    const auto parsed = md::parse_manifest(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->chapters.size() == chapter_count);
    CHECK(*parsed == manifest);
}

TEST_CASE("is_held_image_entry_name matches only page and cover image names",
          "[book_manifest][names]") {
    for (const std::string_view name : {
             "OEBPS/images/p00001.jpg",
             "OEBPS/images/p00000.png",
             "OEBPS/images/p99999.gif",
             "OEBPS/images/p12345.webp",
             "OEBPS/images/cover.jpg",
             "OEBPS/images/cover.png",
             "OEBPS/images/cover.gif",
             "OEBPS/images/cover.webp",
         }) {
        INFO(name);
        CHECK(md::is_held_image_entry_name(name));
    }
    for (const std::string_view name : {
             "",
             "mimetype",
             "META-INF/mangadex-dl.json",
             "OEBPS/content.opf",
             "OEBPS/images/",
             "OEBPS/images/p00001",
             "OEBPS/images/p00001.",
             "OEBPS/images/p1.png",
             "OEBPS/images/p0001.png",
             "OEBPS/images/p000001.png",
             "OEBPS/images/P00001.png",
             "OEBPS/images/p0000a.png",
             "OEBPS/images/p00001.svg",
             "OEBPS/images/p00001.jpeg",
             "OEBPS/images/p00001.JPG",
             "OEBPS/images/p00001.png.png",
             "OEBPS/images/p00001.jpgx",
             "OEBPS/images/../p00001.png",
             "OEBPS/images/sub/p00001.png",
             "OEBPS/images//p00001.png",
             "OEBPS/images/Cover.jpg",
             "OEBPS/images/cover1.jpg",
             "OEBPS/images/.jpg",
             "oebps/images/p00001.png",
             "/OEBPS/images/p00001.png",
             "OEBPS/pages/p00001.png",
             "OEBPS/images/p00001.png\n",
         }) {
        INFO(name);
        // CHECK(!x) rather than CHECK_FALSE: clang-analyzer flags Catch2's Flags | Flags cast
        // (optin.core.EnumCastOutOfRange) when CHECK_FALSE sits in a loop.
        CHECK(!md::is_held_image_entry_name(name));
    }
    // An embedded NUL is not a terminator for a string_view.
    constexpr std::string_view with_nul{"OEBPS/images/p00001.png\0", 24};
    CHECK(!md::is_held_image_entry_name(with_nul));
}

TEST_CASE("manifest_chapter_from copies the planned chapter and leaves images empty",
          "[book_manifest][convert]") {
    const md::Chapter chapter{
        .id = std::string{chapter_one},
        .volume = "3",
        .chapter = std::nullopt,
        .title = "Title",
        .language = "en",
        .groups = {md::ScanGroup{.id = std::string{group_one}, .name = "G"}},
        .pages = 7,
        .external = false,
        .unavailable = false,
        .publish_at = "p",
        .updated_at = "u",
    };
    const md::ManifestChapter record =
        md::manifest_chapter_from(chapter, md::ImageQuality::data_saver);
    CHECK(record == md::ManifestChapter{
                        .id = std::string{chapter_one},
                        .volume = "3",
                        .chapter = std::nullopt,
                        .title = "Title",
                        .groups = {md::ScanGroup{.id = std::string{group_one}, .name = "G"}},
                        .publish_at = "p",
                        .updated_at = "u",
                        .quality = md::ImageQuality::data_saver,
                        .images = {},
                    });
    CHECK(md::manifest_chapter_from(chapter, md::ImageQuality::data).quality ==
          md::ImageQuality::data);
}

TEST_CASE("chapter_from rebuilds a Chapter with the image count as pages",
          "[book_manifest][convert]") {
    const md::ManifestChapter record = rich_manifest(false).chapters.back();
    const md::Chapter chapter = md::chapter_from(record);
    CHECK(chapter == md::Chapter{
                         .id = std::string{chapter_two},
                         .volume = "2",
                         .chapter = "10.5",
                         .title = "",
                         .language = "",
                         .groups = record.groups,
                         .pages = 3,
                         .external = false,
                         .unavailable = false,
                         .publish_at = "",
                         .updated_at = "",
                     });
    CHECK(md::chapter_from(md::ManifestChapter{}).pages == 0);
    // The two conversions agree on every shared field.
    CHECK(md::manifest_chapter_from(chapter, record.quality) == md::ManifestChapter{
                                                                    .id = record.id,
                                                                    .volume = record.volume,
                                                                    .chapter = record.chapter,
                                                                    .title = record.title,
                                                                    .groups = record.groups,
                                                                    .publish_at = record.publish_at,
                                                                    .updated_at = record.updated_at,
                                                                    .quality = record.quality,
                                                                    .images = {},
                                                                });
}
