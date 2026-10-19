// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/book_manifest/book_manifest_hostile_test.cpp: parse_manifest on hostile input
// (skip-existing plan T4): every required key missing or mistyped, the `format` rows (fact-check
// V15), caps on strings, images and totals, bad entry names, depth and separator floods (rejected
// by the pre-scan, V16), the byte cap, invalid UTF-8, trailing garbage, a BOM and empty input.
// Every row must give its fault without a throw or a sanitizer finding.
//
// Memory rule (V16): no test here builds or parses an adversarial document near 16 MiB. The
// largest hostile document that reaches the parser is about 2 MiB; the 16 MiB + 1 row is padding
// that the byte cap rejects before any scan.
//
// Also covered: the json_util::parse_untrusted(…, ParseLimits) overload and FieldReader's error
// class (src/core/json_util.hpp, core-private, on the unit-test include path).

#include "json_util.hpp" // core-private (src/core is on the test include path)
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
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
namespace ju = mangadex_dl::json_util;
using Doc = nlohmann::ordered_json;

constexpr std::string_view chapter_id = "abcdef01-2345-4678-89ab-cdef01234567";
constexpr std::string_view group_id = "0000000a-0002-4000-8000-000000000001";

/// A valid manifest: a cover, one chapter with one group and two images.
[[nodiscard]] md::BookManifest base_manifest() {
    return md::BookManifest{
        .identifier = "urn:mangadex:11111111-0001-4000-8000-000000000001:en:v1",
        .opf_crc32 = 0x0a1b2c3dU,
        .cover = md::ManifestCover{.file_name = "cover-1.jpg", .entry = "OEBPS/images/cover.jpg"},
        .chapters = {md::ManifestChapter{
            .id = std::string{chapter_id},
            .volume = "1",
            .chapter = "1",
            .title = "T",
            .groups = {md::ScanGroup{.id = std::string{group_id}, .name = "G"}},
            .publish_at = "2024-01-01T00:00:00+00:00",
            .updated_at = "2024-01-02T00:00:00+00:00",
            .quality = md::ImageQuality::data,
            .images = {"OEBPS/images/p00001.png", "OEBPS/images/p00002.jpg"},
        }},
    };
}

[[nodiscard]] std::string base_text() { return md::manifest_to_json(base_manifest()); }

/// The base manifest as a (trusted) document to edit.
[[nodiscard]] Doc base_doc() { return Doc::parse(base_text()); }

[[nodiscard]] Doc& first_chapter(Doc& doc) { return doc["chapters"][0]; }

/// The fault of a parse, or nullopt when it succeeded.
[[nodiscard]] std::optional<md::ManifestFault> fault_of(std::string_view text) {
    const auto parsed = md::parse_manifest(text);
    if (parsed.has_value()) {
        return std::nullopt;
    }
    return parsed.error();
}

/// Named apart from fault_of: a Doc converts implicitly from a string.
[[nodiscard]] std::optional<md::ManifestFault> fault_of_doc(const Doc& doc) {
    return fault_of(doc.dump());
}

constexpr std::optional<md::ManifestFault> malformed = md::ManifestFault::malformed;
constexpr std::optional<md::ManifestFault> newer_format = md::ManifestFault::newer_format;
constexpr std::optional<md::ManifestFault> accepted = std::nullopt;

using Edit = void (*)(Doc&);

struct Row {
    std::string_view name{};
    Edit edit = nullptr;
    std::optional<md::ManifestFault> expected = malformed;
};

/// The base text with its leading `{"format":1` replaced by `{"format":<raw>`, so a number can be
/// spelled exactly (nlohmann would re-spell 1e0 or -0).
[[nodiscard]] std::string with_raw_format(std::string_view raw) {
    const std::string text = base_text();
    constexpr std::string_view prefix = R"({"format":1,)";
    REQUIRE(text.starts_with(prefix));
    return std::format(R"({{"format":{},{})", raw, text.substr(prefix.size()));
}

/// The base text with an unknown member `"pad":` + `depth - 1` nested arrays inserted first, so
/// the document is nested exactly `depth` levels deep (the root object is level 1).
[[nodiscard]] std::string with_depth(std::size_t depth) {
    std::string text = base_text();
    std::string pad = R"("pad":)";
    pad.append(depth - 1, '[');
    pad.append(depth - 1, ']');
    pad += ',';
    text.insert(1, pad);
    return text;
}

/// A document of `count` images, all with the same cheap valid name, split into chapters of at
/// most max_pages_per_chapter images.
[[nodiscard]] std::string with_total_images(std::size_t count) {
    Doc doc = base_doc();
    const Doc chapter_template = first_chapter(doc);
    Doc chapters = Doc::array();
    std::size_t left = count;
    while (left > 0) {
        const std::size_t here = std::min(left, md::max_pages_per_chapter);
        Doc chapter = chapter_template;
        chapter["images"] = Doc::array();
        for (std::size_t i = 0; i < here; ++i) {
            chapter["images"].push_back("OEBPS/images/p00001.jpg");
        }
        chapters.push_back(std::move(chapter));
        left -= here;
    }
    doc["chapters"] = std::move(chapters);
    return doc.dump();
}

} // namespace

TEST_CASE("the hostile base manifest is valid", "[book_manifest][hostile]") {
    const auto parsed = md::parse_manifest(base_text());
    REQUIRE(parsed.has_value());
    CHECK(*parsed == base_manifest());
}

TEST_CASE("parse_manifest rejects every missing or mistyped field", "[book_manifest][hostile]") {
    const std::vector<Row> rows{
        // The root.
        {.name = "format missing", .edit = [](Doc& d) { d.erase("format"); }},
        {.name = "format null", .edit = [](Doc& d) { d["format"] = nullptr; }},
        {.name = "generator missing", .edit = [](Doc& d) { d.erase("generator"); }},
        {.name = "generator number", .edit = [](Doc& d) { d["generator"] = 1; }},
        {.name = "identifier missing", .edit = [](Doc& d) { d.erase("identifier"); }},
        {.name = "identifier null", .edit = [](Doc& d) { d["identifier"] = nullptr; }},
        {.name = "identifier array", .edit = [](Doc& d) { d["identifier"] = Doc::array(); }},
        {.name = "opf_crc32 missing", .edit = [](Doc& d) { d.erase("opf_crc32"); }},
        {.name = "opf_crc32 number", .edit = [](Doc& d) { d["opf_crc32"] = 0x0a1b2c3d; }},
        {.name = "chapters missing", .edit = [](Doc& d) { d.erase("chapters"); }},
        {.name = "chapters object", .edit = [](Doc& d) { d["chapters"] = Doc::object(); }},
        {.name = "chapters string", .edit = [](Doc& d) { d["chapters"] = "x"; }},
        {.name = "cover array", .edit = [](Doc& d) { d["cover"] = Doc::array(); }},
        {.name = "cover string", .edit = [](Doc& d) { d["cover"] = "cover.jpg"; }},
        {.name = "cover file_name missing", .edit = [](Doc& d) { d["cover"].erase("file_name"); }},
        {.name = "cover file_name number", .edit = [](Doc& d) { d["cover"]["file_name"] = 1; }},
        {.name = "cover entry missing", .edit = [](Doc& d) { d["cover"].erase("entry"); }},
        {.name = "cover entry bool", .edit = [](Doc& d) { d["cover"]["entry"] = true; }},
        // A chapter.
        {.name = "chapter not an object", .edit = [](Doc& d) { d["chapters"][0] = "x"; }},
        {.name = "chapter null", .edit = [](Doc& d) { d["chapters"][0] = nullptr; }},
        {.name = "chapter id missing", .edit = [](Doc& d) { first_chapter(d).erase("id"); }},
        {.name = "chapter id number", .edit = [](Doc& d) { first_chapter(d)["id"] = 7; }},
        {.name = "volume number", .edit = [](Doc& d) { first_chapter(d)["volume"] = 1; }},
        {.name = "chapter label number", .edit = [](Doc& d) { first_chapter(d)["chapter"] = 1.5; }},
        {.name = "title missing", .edit = [](Doc& d) { first_chapter(d).erase("title"); }},
        {.name = "title null", .edit = [](Doc& d) { first_chapter(d)["title"] = nullptr; }},
        {.name = "groups missing", .edit = [](Doc& d) { first_chapter(d).erase("groups"); }},
        {.name = "groups object",
         .edit = [](Doc& d) { first_chapter(d)["groups"] = Doc::object(); }},
        {.name = "group not an object",
         .edit = [](Doc& d) { first_chapter(d)["groups"][0] = "G"; }},
        {.name = "group id missing",
         .edit = [](Doc& d) { first_chapter(d)["groups"][0].erase("id"); }},
        {.name = "group name missing",
         .edit = [](Doc& d) { first_chapter(d)["groups"][0].erase("name"); }},
        {.name = "group name number",
         .edit = [](Doc& d) { first_chapter(d)["groups"][0]["name"] = 3; }},
        {.name = "publish_at missing",
         .edit = [](Doc& d) { first_chapter(d).erase("publish_at"); }},
        {.name = "updated_at missing",
         .edit = [](Doc& d) { first_chapter(d).erase("updated_at"); }},
        {.name = "updated_at number", .edit = [](Doc& d) { first_chapter(d)["updated_at"] = 0; }},
        {.name = "quality missing", .edit = [](Doc& d) { first_chapter(d).erase("quality"); }},
        {.name = "quality number", .edit = [](Doc& d) { first_chapter(d)["quality"] = 0; }},
        {.name = "images missing", .edit = [](Doc& d) { first_chapter(d).erase("images"); }},
        {.name = "images string",
         .edit = [](Doc& d) { first_chapter(d)["images"] = "OEBPS/images/p00001.png"; }},
        {.name = "image number", .edit = [](Doc& d) { first_chapter(d)["images"][1] = 1; }},
        {.name = "image null", .edit = [](Doc& d) { first_chapter(d)["images"][0] = nullptr; }},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        Doc doc = base_doc();
        row.edit(doc);
        CHECK(fault_of_doc(doc) == row.expected);
    }
}

TEST_CASE("parse_manifest accepts nullable and absent optional fields and ignores unknown keys",
          "[book_manifest][hostile]") {
    const std::vector<Row> rows{
        {.name = "cover null", .edit = [](Doc& d) { d["cover"] = nullptr; }, .expected = accepted},
        {.name = "cover absent", .edit = [](Doc& d) { d.erase("cover"); }, .expected = accepted},
        {.name = "volume null",
         .edit = [](Doc& d) { first_chapter(d)["volume"] = nullptr; },
         .expected = accepted},
        {.name = "chapter label absent",
         .edit = [](Doc& d) { first_chapter(d).erase("chapter"); },
         .expected = accepted},
        {.name = "no groups",
         .edit = [](Doc& d) { first_chapter(d)["groups"] = Doc::array(); },
         .expected = accepted},
        {.name = "unknown root key",
         .edit = [](Doc& d) { d["future"] = Doc::array({1, 2}); },
         .expected = accepted},
        {.name = "unknown chapter key",
         .edit = [](Doc& d) { first_chapter(d)["pages"] = 2; },
         .expected = accepted},
        {.name = "unknown group key",
         .edit = [](Doc& d) { first_chapter(d)["groups"][0]["x"] = 1; },
         .expected = accepted},
        {.name = "unknown cover key",
         .edit = [](Doc& d) { d["cover"]["type"] = "jpeg"; },
         .expected = accepted},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        Doc doc = base_doc();
        row.edit(doc);
        CHECK(fault_of_doc(doc) == row.expected);
    }
    // The document root must be an object.
    for (const std::string_view text : {"[]", "1", R"("x")", "null", "true"}) {
        INFO(text);
        CHECK(fault_of(text) == malformed);
    }
}

TEST_CASE("parse_manifest reads format as an unsigned integer", "[book_manifest][hostile]") {
    struct FormatRow {
        std::string_view raw{};
        std::optional<md::ManifestFault> expected = malformed;
    };
    const std::vector<FormatRow> rows{
        {.raw = "1", .expected = accepted},
        {.raw = "0", .expected = malformed},
        {.raw = "2", .expected = newer_format},
        {.raw = "18446744073709551615",
         .expected = newer_format}, // UINT64_MAX is still an unsigned integer
        {.raw = "1.0", .expected = malformed},
        {.raw = "1e0", .expected = malformed},
        {.raw = "-0", .expected = malformed},
        {.raw = "-1", .expected = malformed},
        {.raw = "18446744073709551616", .expected = malformed}, // 2^64 parses as a float
        {.raw = R"("1")", .expected = malformed},
        {.raw = "true", .expected = malformed},
        {.raw = "null", .expected = malformed},
        {.raw = "[1]", .expected = malformed},
    };
    for (const FormatRow& row : rows) {
        INFO(row.raw);
        CHECK(fault_of(with_raw_format(row.raw)) == row.expected);
    }
}

TEST_CASE("a newer format is reported before any other field is checked",
          "[book_manifest][hostile]") {
    Doc doc = Doc::object();
    doc["format"] = 2;
    doc["generator"] = "mangadex-dl";
    doc["chapters"] = "laid out differently";
    CHECK(fault_of_doc(doc) == newer_format);
    // ...but only from our generator.
    doc["generator"] = "other-tool";
    CHECK(fault_of_doc(doc) == malformed);
    doc.erase("generator");
    CHECK(fault_of_doc(doc) == malformed);
}

TEST_CASE("parse_manifest requires the exact generator", "[book_manifest][hostile]") {
    for (const std::string_view generator :
         {"", "mangadex-dl2", "Mangadex-dl", "mangadex-dl ", "mangadex_dl", "other"}) {
        INFO(generator);
        Doc doc = base_doc();
        doc["generator"] = generator;
        CHECK(fault_of_doc(doc) == malformed);
    }
}

TEST_CASE("parse_manifest caps every string at 4 KiB", "[book_manifest][hostile]") {
    const std::string at_cap(md::max_manifest_string_bytes, 'x');
    const std::string over_cap(md::max_manifest_string_bytes + 1, 'x');
    using Set = void (*)(Doc&, const std::string&);
    struct StringRow {
        std::string_view name{};
        Set set = nullptr;
    };
    const std::vector<StringRow> rows{
        {.name = "identifier", .set = [](Doc& d, const std::string& s) { d["identifier"] = s; }},
        {.name = "cover file_name",
         .set = [](Doc& d, const std::string& s) { d["cover"]["file_name"] = s; }},
        {.name = "volume",
         .set = [](Doc& d, const std::string& s) { first_chapter(d)["volume"] = s; }},
        {.name = "chapter",
         .set = [](Doc& d, const std::string& s) { first_chapter(d)["chapter"] = s; }},
        {.name = "title",
         .set = [](Doc& d, const std::string& s) { first_chapter(d)["title"] = s; }},
        {.name = "group name",
         .set = [](Doc& d, const std::string& s) { first_chapter(d)["groups"][0]["name"] = s; }},
        {.name = "publish_at",
         .set = [](Doc& d, const std::string& s) { first_chapter(d)["publish_at"] = s; }},
        {.name = "updated_at",
         .set = [](Doc& d, const std::string& s) { first_chapter(d)["updated_at"] = s; }},
    };
    for (const StringRow& row : rows) {
        INFO(row.name);
        Doc doc = base_doc();
        row.set(doc, at_cap);
        CHECK(fault_of_doc(doc) == accepted);
        row.set(doc, over_cap);
        CHECK(fault_of_doc(doc) == malformed);
    }
}

TEST_CASE("parse_manifest validates ids, the CRC and the quality", "[book_manifest][hostile]") {
    const std::vector<Row> rows{
        {.name = "chapter id not a UUID",
         .edit = [](Doc& d) { first_chapter(d)["id"] = "chapter-1"; }},
        {.name = "chapter id empty", .edit = [](Doc& d) { first_chapter(d)["id"] = ""; }},
        {.name = "chapter id braced",
         .edit = [](Doc& d) { first_chapter(d)["id"] = "{abcdef01-2345-4678-89ab-cdef01234567}"; }},
        {.name = "group id not a UUID",
         .edit = [](Doc& d) { first_chapter(d)["groups"][0]["id"] = "g"; }},
        {.name = "group id one short",
         .edit =
             [](Doc& d) {
                 first_chapter(d)["groups"][0]["id"] = "0000000a-0002-4000-8000-00000000000";
             }},
        {.name = "upper-case chapter id",
         .edit = [](Doc& d) { first_chapter(d)["id"] = "ABCDEF01-2345-4678-89AB-CDEF01234567"; },
         .expected = accepted},
        {.name = "opf_crc32 upper-case", .edit = [](Doc& d) { d["opf_crc32"] = "0A1B2C3D"; }},
        {.name = "opf_crc32 7 digits", .edit = [](Doc& d) { d["opf_crc32"] = "a1b2c3d"; }},
        {.name = "opf_crc32 9 digits", .edit = [](Doc& d) { d["opf_crc32"] = "0a1b2c3d0"; }},
        {.name = "opf_crc32 0x prefix", .edit = [](Doc& d) { d["opf_crc32"] = "0x1b2c3d"; }},
        {.name = "opf_crc32 non-hex", .edit = [](Doc& d) { d["opf_crc32"] = "0a1b2c3g"; }},
        {.name = "opf_crc32 sign", .edit = [](Doc& d) { d["opf_crc32"] = "-a1b2c3d"; }},
        {.name = "opf_crc32 empty", .edit = [](Doc& d) { d["opf_crc32"] = ""; }},
        {.name = "quality upper-case",
         .edit = [](Doc& d) { first_chapter(d)["quality"] = "Data"; }},
        {.name = "quality underscore",
         .edit = [](Doc& d) { first_chapter(d)["quality"] = "data_saver"; }},
        {.name = "quality empty", .edit = [](Doc& d) { first_chapter(d)["quality"] = ""; }},
        {.name = "quality data-saver",
         .edit = [](Doc& d) { first_chapter(d)["quality"] = "data-saver"; },
         .expected = accepted},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        Doc doc = base_doc();
        row.edit(doc);
        CHECK(fault_of_doc(doc) == row.expected);
    }
}

TEST_CASE("parse_manifest bounds chapters and images", "[book_manifest][hostile]") {
    SECTION("an empty chapters array") {
        Doc doc = base_doc();
        doc["chapters"] = Doc::array();
        CHECK(fault_of_doc(doc) == malformed);
    }
    SECTION("a chapter without images") {
        Doc doc = base_doc();
        first_chapter(doc)["images"] = Doc::array();
        CHECK(fault_of_doc(doc) == malformed);
    }
    SECTION("max_pages_per_chapter images in one chapter, then one more") {
        Doc doc = base_doc();
        Doc& images = first_chapter(doc)["images"];
        images = Doc::array();
        for (std::size_t i = 0; i < md::max_pages_per_chapter; ++i) {
            images.push_back(std::format("OEBPS/images/p{:05}.jpg", i + 1));
        }
        CHECK(fault_of_doc(doc) == accepted);
        images.push_back("OEBPS/images/p05001.jpg");
        CHECK(fault_of_doc(doc) == malformed);
    }
    SECTION("zip_max_entries images in total, then one more") {
        CHECK(fault_of(with_total_images(md::zip_max_entries)) == accepted);
        CHECK(fault_of(with_total_images(md::zip_max_entries + 1)) == malformed);
    }
}

TEST_CASE("parse_manifest rejects image and cover names outside OEBPS/images",
          "[book_manifest][hostile]") {
    for (const std::string_view name : {
             "OEBPS/images/../p00001.png",
             "OEBPS/images/p1.png",
             "OEBPS/images/p00001.svg",
             "mimetype",
             "META-INF/mangadex-dl.json",
             "OEBPS/content.opf",
             "",
             "/OEBPS/images/p00001.png",
         }) {
        INFO(name);
        Doc page = base_doc();
        first_chapter(page)["images"][1] = name;
        CHECK(fault_of_doc(page) == malformed);
        Doc cover = base_doc();
        cover["cover"]["entry"] = name;
        CHECK(fault_of_doc(cover) == malformed);
    }
}

TEST_CASE("parse_manifest rejects deep nesting in the pre-scan", "[book_manifest][hostile]") {
    const ju::ParseLimits limits{
        .max_bytes = md::max_manifest_bytes,
        .max_depth = md::max_manifest_depth,
        .max_separators = md::max_manifest_separators,
        .code = md::Errc::failure,
    };
    CHECK(fault_of(with_depth(md::max_manifest_depth)) == accepted);
    for (const std::size_t depth : {std::size_t{9}, std::size_t{1000}, std::size_t{100000}}) {
        INFO(depth);
        const std::string text = with_depth(depth);
        CHECK(fault_of(text) == malformed);
        // The pre-scan, not the parser, rejected it: nlohmann never saw the document.
        const auto rejected = ju::parse_untrusted(text, "book manifest", limits);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().message == "malformed book manifest: nested deeper than 8 levels");
    }
}

TEST_CASE("parse_manifest rejects a separator flood in the pre-scan", "[book_manifest][hostile]") {
    // About 2 MiB: `[` plus 2^21 commas is 2^21 + 1 separators.
    std::string flood = "[";
    flood.append(md::max_manifest_separators, ',');
    flood += ']';
    REQUIRE(flood.size() < std::size_t{3} * 1024 * 1024);
    CHECK(fault_of(flood) == malformed);
    const auto rejected =
        ju::parse_untrusted(flood, "book manifest",
                            ju::ParseLimits{.max_bytes = md::max_manifest_bytes,
                                            .max_depth = md::max_manifest_depth,
                                            .max_separators = md::max_manifest_separators,
                                            .code = md::Errc::failure});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message ==
          "malformed book manifest: more than 2097152 structural separators");

    // The same flood inside a valid manifest's unknown member: still rejected before parsing.
    std::string padded = base_text();
    padded.insert(1, R"("pad":)" + flood + ",");
    CHECK(fault_of(padded) == malformed);
}

TEST_CASE("parse_manifest applies the byte cap before anything else", "[book_manifest][hostile]") {
    std::string text = base_text();
    REQUIRE(text.size() < md::max_manifest_bytes);
    // Whitespace padding: cheap to scan and parse, so the exact cap is accepted.
    text.resize(md::max_manifest_bytes, ' ');
    CHECK(fault_of(text) == accepted);
    text.push_back(' ');
    CHECK(fault_of(text) == malformed);
    const auto rejected = ju::parse_untrusted(
        text, "book manifest",
        ju::ParseLimits{.max_bytes = md::max_manifest_bytes, .code = md::Errc::failure});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == md::Errc::failure);
    CHECK(rejected.error().message == "book manifest is larger than 16 MiB");
}

TEST_CASE("parse_manifest rejects invalid UTF-8, trailing data, a BOM and empty input",
          "[book_manifest][hostile]") {
    const std::string text = base_text();
    const std::size_t title = text.find(R"("title":"T")");
    REQUIRE(title != std::string::npos);
    const std::size_t t = title + std::string_view{R"("title":")"}.size();
    for (const std::string_view bad : {"\xFF", "\xC3", "\xED\xA0\x80", "\xC0\xAF", "\xF8\x88"}) {
        std::string invalid = text;
        invalid.replace(t, 1, bad);
        CHECK(fault_of(invalid) == malformed);
    }
    // Invalid UTF-8 outside a string.
    CHECK(fault_of(text + "\xFF") == malformed);
    CHECK(fault_of(text + "x") == malformed);
    CHECK(fault_of(text + text) == malformed);
    CHECK(fault_of(text + ",") == malformed);
    CHECK(fault_of(text + "/* c */") == malformed);
    // A raw NUL: nlohmann's lexer stops there, so parse_untrusted rejects it explicitly.
    CHECK(fault_of(text + std::string(1, '\0')) == malformed);
    CHECK(fault_of(text + std::string(1, '\0') + "junk") == malformed);
    std::string nul_in_string = text;
    nul_in_string[t] = '\0';
    CHECK(fault_of(nul_in_string) == malformed);
    CHECK(fault_of(text.substr(0, text.size() - 1)) == malformed);
    CHECK(fault_of("\xEF\xBB\xBF" + text) == malformed);
    CHECK(fault_of("") == malformed);
    CHECK(fault_of("   \n") == malformed);
    CHECK(fault_of("{}") == malformed);
    // Surrounding whitespace is still JSON.
    CHECK(fault_of(" \n" + text + "\n ") == accepted);
}

TEST_CASE("parse_untrusted with ParseLimits applies each limit and the error class",
          "[book_manifest][json_util]") {
    SECTION("the byte cap, inclusive, with a non-MiB size in the message") {
        const ju::ParseLimits limits{.max_bytes = 9, .code = md::Errc::failure};
        CHECK(ju::parse_untrusted("[1,2,3,4]", "doc", limits).has_value());
        const auto rejected = ju::parse_untrusted("[1,2,3,45]", "doc", limits);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() ==
              md::Error{.code = md::Errc::failure, .message = "doc is larger than 9 bytes"});
    }
    SECTION("the depth") {
        const ju::ParseLimits limits{.max_depth = 2, .code = md::Errc::io};
        CHECK(ju::parse_untrusted("[[0],{\"a\":1}]", "doc", limits).has_value());
        const auto rejected = ju::parse_untrusted("[[[0]]]", "doc", limits);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() ==
              md::Error{.code = md::Errc::io,
                        .message = "malformed doc: nested deeper than 2 levels"});
    }
    SECTION("the separators: , : [ { count, closers and string contents do not") {
        const ju::ParseLimits limits{.max_separators = 4, .code = md::Errc::failure};
        CHECK(ju::parse_untrusted("[0,0,0,0]", "doc", limits).has_value());     // [ + 3 commas
        CHECK(ju::parse_untrusted(R"({"a":[{}]})", "doc", limits).has_value()); // { : [ {
        CHECK(ju::parse_untrusted(R"(["a,b:c[d{e",",,,,"])", "doc", limits).has_value());
        CHECK(ju::parse_untrusted(R"(["\",,,,\\",0])", "doc", limits).has_value());
        const auto rejected = ju::parse_untrusted("[0,0,0,0,0]", "doc", limits);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() ==
              md::Error{.code = md::Errc::failure,
                        .message = "malformed doc: more than 4 structural separators"});
        CHECK_FALSE(ju::parse_untrusted(R"({"a":[{},{}]})", "doc", limits).has_value());
    }
    SECTION("max_separators 0 is unlimited") {
        std::string many = "[0";
        for (int i = 0; i < 10000; ++i) {
            many += ",0";
        }
        many += ']';
        CHECK(ju::parse_untrusted(many, "doc", ju::ParseLimits{}).has_value());
    }
    SECTION("a parse error carries the caller's class") {
        const auto rejected =
            ju::parse_untrusted("{", "doc", ju::ParseLimits{.code = md::Errc::failure});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() ==
              md::Error{.code = md::Errc::failure, .message = "malformed doc: not valid JSON"});
    }
    SECTION("the defaults are the API's: same limits, same messages") {
        const auto rejected = ju::parse_untrusted("{", "feed page");
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() ==
              md::Error{.code = md::Errc::network,
                        .message = "malformed MangaDex API response (feed page): not valid JSON"});
        const auto explicit_defaults = ju::parse_untrusted("{", "feed page", ju::ParseLimits{});
        REQUIRE_FALSE(explicit_defaults.has_value());
        CHECK(explicit_defaults.error() == rejected.error());
        const ju::ParseLimits defaults{};
        CHECK(defaults.max_bytes == md::max_api_json_bytes);
        CHECK(defaults.max_depth == md::max_json_depth);
        CHECK(defaults.max_separators == 0);
        CHECK(defaults.code == md::Errc::network);
    }
    SECTION("a leading BOM and a raw NUL are malformed for every caller, with the caller's class") {
        // nlohmann would skip the BOM and stop at the NUL (accepting `{}` and dropping "junk").
        constexpr std::string_view bom_doc = "\xEF\xBB\xBF{}";
        constexpr std::string_view nul_doc{"{}\0junk", 7};

        const auto api_bom = ju::parse_untrusted(bom_doc, "doc");
        REQUIRE_FALSE(api_bom.has_value());
        CHECK(api_bom.error() ==
              md::Error{.code = md::Errc::network,
                        .message = "malformed MangaDex API response (doc): starts with a UTF-8 "
                                   "byte order mark"});
        const auto api_nul = ju::parse_untrusted(nul_doc, "doc");
        REQUIRE_FALSE(api_nul.has_value());
        CHECK(api_nul.error() ==
              md::Error{.code = md::Errc::network,
                        .message = "malformed MangaDex API response (doc): contains a raw NUL "
                                   "byte"});

        const ju::ParseLimits failure{.code = md::Errc::failure};
        const auto doc_bom = ju::parse_untrusted(bom_doc, "doc", failure);
        REQUIRE_FALSE(doc_bom.has_value());
        CHECK(doc_bom.error() ==
              md::Error{.code = md::Errc::failure,
                        .message = "malformed doc: starts with a UTF-8 byte order mark"});
        const auto doc_nul = ju::parse_untrusted(nul_doc, "doc", failure);
        REQUIRE_FALSE(doc_nul.has_value());
        CHECK(doc_nul.error() == md::Error{.code = md::Errc::failure,
                                           .message = "malformed doc: contains a raw NUL byte"});
    }
}

TEST_CASE("FieldReader reports its first error with the chosen class",
          "[book_manifest][json_util]") {
    ju::FieldReader api{"feed page"};
    api.fail("first");
    api.fail("second");
    CHECK(api.error() ==
          md::Error{.code = md::Errc::network,
                    .message = "malformed MangaDex API response (feed page): first"});

    ju::FieldReader manifest{"book manifest", md::Errc::failure};
    const ju::Json doc = ju::Json::parse(R"({"a":1})");
    const auto* root = manifest.as_object(doc, "root");
    REQUIRE(root != nullptr);
    CHECK_FALSE(manifest.string(*root, "a", ju::Need::required).has_value());
    CHECK_FALSE(manifest.ok());
    CHECK(manifest.error() ==
          md::Error{.code = md::Errc::failure,
                    .message = "malformed book manifest: 'a' has the wrong type"});
}
