// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/mdlist/mdlist_parse_test.cpp: the custom-list document (list spec AC3, AC4; list plan
// T7; fact-check V18 and amendment 3): parse_custom_list's accepted shapes and every rejection
// (each a fixed Errc::network message naming the requested list, never response bytes), the name
// cut, is_custom_list_error_body, list_name_for_display, list_summary and the two URLs.

#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "support/mdx_fixture.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using nlohmann::json;

/// Planted in every hostile document: no error message may contain it.
constexpr std::string_view marker = "MARKER-7f3a9c";

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";

[[nodiscard]] md::ListId list(std::uint64_t n) {
    auto id = md::ListId::parse(mdx::list_id(n));
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

/// L1's document ("Clean List", the clean title), as JSON to mutate.
[[nodiscard]] json l1_doc() {
    return mdx::list_json(
        {.id = mdx::list_id(1), .name = "Clean List", .manga_ids = {std::string{clean_id}}});
}

/// True when `text` is well-formed UTF-8 (no overlongs, surrogates or code points > U+10FFFF).
[[nodiscard]] bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        unsigned min = 0;
        unsigned cp = 0;
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            min = 0x80U;
            cp = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            min = 0x800U;
            cp = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            min = 0x10000U;
            cp = lead & 0x07U;
        } else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto byte = static_cast<unsigned char>(text[i + k]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (byte & 0x3FU);
        }
        if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += length;
    }
    return true;
}

/// Checks the AC4 contract for one rejected document: Errc::network (exit 3), a message that
/// starts with "list <requested id>" and never quotes the planted marker.
void check_rejected(const md::Result<md::CustomList>& parsed, const md::ListId& requested) {
    REQUIRE(!parsed.has_value());
    const md::Error& error = parsed.error();
    INFO(error.message);
    CHECK(error.code == md::Errc::network);
    CHECK(md::exit_code_for(error.code) == md::exit_code::network);
    CHECK(error.message.starts_with(std::format("list {}", requested.str())));
    CHECK(!error.message.contains(marker));
}

/// As check_rejected, plus the FieldReader / parse_untrusted wording after the prefix.
void check_malformed(const md::Result<md::CustomList>& parsed, const md::ListId& requested) {
    check_rejected(parsed, requested);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message.starts_with(std::format(
            "list {}: malformed MangaDex API response (custom list): ", requested.str())));
    }
}

/// A document whose manga relationships are `count` distinct valid ids.
[[nodiscard]] std::string doc_with_titles(std::size_t count) {
    std::vector<std::string> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(std::format("c0000000-0000-4000-8000-{:012x}", i));
    }
    return mdx::list_json({.id = mdx::list_id(1), .manga_ids = std::move(ids)}).dump();
}

// ---- URLs -----------------------------------------------------------------------------------

TEST_CASE("mdlist: the request and website URLs use the canonical id", "[mdlist]") {
    auto upper = md::ListId::parse("A0000000-0000-4000-8000-00000000000A");
    REQUIRE(upper.has_value());
    CHECK(md::list_request_url("https://api.mangadex.org", *upper) ==
          "https://api.mangadex.org/list/a0000000-0000-4000-8000-00000000000a");
    CHECK(md::list_request_url("file:///tmp/t/api", list(1)) ==
          "file:///tmp/t/api/list/a0000000-0000-4000-8000-000000000001");
    CHECK(md::list_web_url(*upper) ==
          "https://mangadex.org/list/a0000000-0000-4000-8000-00000000000a");
}

// ---- AC3: accepted documents ----------------------------------------------------------------

TEST_CASE("mdlist: the live shape gives the manga in array order, user last ignored", "[mdlist]") {
    const auto parsed = md::parse_custom_list(
        mdx::list_json({.id = mdx::list_id(2),
                        .name = "Clean And Unknown",
                        .manga_ids = {std::string{clean_id}, std::string{mdx::unknown_manga_id}}})
            .dump(),
        list(2));
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == list(2));
    CHECK(parsed->name == "Clean And Unknown");
    CHECK(parsed->titles == std::vector{manga(clean_id), manga(mdx::unknown_manga_id)});
}

TEST_CASE("mdlist: unknown relationship types and unknown fields are ignored", "[mdlist]") {
    json doc = l1_doc();
    json& data = doc.at("data");
    data["extra"] = {{"nested", {1, 2, 3}}};
    data.at("attributes")["description"] = "anything";
    doc["meta"] = "ignored";
    json rels = json::array();
    rels.push_back({{"id", "99999999-9999-4999-8999-999999999999"}, {"type", "scanlation_group"}});
    rels.push_back({{"id", std::string{clean_id}}, {"type", "manga"}, {"related", "x"}});
    rels.push_back({{"id", std::string{mdx::list_owner_id}}, {"type", "user"}});
    rels.push_back({{"type", "future_type"}}); // no id: only manga entries need one
    rels.push_back({{"id", std::string{mdx::unknown_manga_id}},
                    {"type", "manga"},
                    {"attributes", {{"title", "x"}}}});
    data["relationships"] = rels;
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->titles == std::vector{manga(clean_id), manga(mdx::unknown_manga_id)});
}

TEST_CASE("mdlist: an absent or null name is no name", "[mdlist]") {
    using Edit = void (*)(json&);
    const Edit edit =
        GENERATE(values<Edit>({[](json& d) { d.at("data").at("attributes")["name"] = nullptr; },
                               [](json& d) { d.at("data").at("attributes").erase("name"); },
                               [](json& d) { d.at("data").erase("attributes"); },
                               [](json& d) { d.at("data")["attributes"] = nullptr; }}));
    json doc = l1_doc();
    edit(doc);
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(!parsed->name.has_value());
    CHECK(parsed->titles == std::vector{manga(clean_id)});
}

TEST_CASE("mdlist: a long name is cut to 1024 bytes on a UTF-8 boundary", "[mdlist]") {
    // An ASCII prefix of every length mod 4, then multi-byte characters up to 2000 bytes, so the
    // 1024-byte cut lands inside a 2-, 3- and 4-byte sequence and on boundaries.
    const std::string_view character = GENERATE(as<std::string_view>{}, "\xC3\xA9", // é
                                                "\xE2\x82\xAC",                     // €
                                                "\xF0\x9F\x98\x80");                // U+1F600
    const std::size_t ascii = GENERATE(range<std::size_t>(1020, 1025));
    std::string name(ascii, 'n');
    while (name.size() < 2000) {
        name += character;
    }
    CAPTURE(character.size(), ascii);
    json doc = l1_doc();
    doc.at("data").at("attributes")["name"] = name;
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.has_value());
    const std::string kept = parsed->name.value_or(std::string{});
    CHECK(kept.size() <= md::max_list_name_bytes);
    CHECK(kept.size() > md::max_list_name_bytes - character.size());
    CHECK(valid_utf8(kept));
    CHECK(name.starts_with(kept));
}

TEST_CASE("mdlist: a name of exactly 1024 bytes is kept whole", "[mdlist]") {
    const std::string name(md::max_list_name_bytes, 'x');
    json doc = l1_doc();
    doc.at("data").at("attributes")["name"] = name;
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == name);
}

TEST_CASE("mdlist: an escaped control character in the name is kept raw (display only)",
          "[mdlist]") {
    json doc = l1_doc();
    doc.at("data").at("attributes")["name"] = "a\x1b[31mb";
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "a\x1b[31mb"); // the app passes it through terminal_safe
}

TEST_CASE("mdlist: ids are canonicalised and data.id matches in any case", "[mdlist]") {
    json doc = l1_doc();
    doc.at("data")["id"] = "A0000000-0000-4000-8000-000000000001";
    doc.at("data").at("relationships").at(0)["id"] = "11111111-1111-4111-8111-11111111111F";
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->id.str() == "a0000000-0000-4000-8000-000000000001");
    REQUIRE(parsed->titles.size() == 1);
    CHECK(parsed->titles.front().str() == "11111111-1111-4111-8111-11111111111f");
}

TEST_CASE("mdlist: only the user entry gives an empty list; duplicates are kept", "[mdlist]") {
    const auto empty = md::parse_custom_list(
        mdx::list_json({.id = mdx::list_id(3), .name = "Empty"}).dump(), list(3));
    REQUIRE(empty.has_value());
    CHECK(empty->titles.empty());
    CHECK(empty->name == "Empty");

    const auto twice = md::parse_custom_list(
        mdx::list_json(
            {.id = mdx::list_id(4), .manga_ids = {std::string{clean_id}, std::string{clean_id}}})
            .dump(),
        list(4));
    REQUIRE(twice.has_value());
    CHECK(twice->titles == std::vector{manga(clean_id), manga(clean_id)});
}

TEST_CASE("mdlist: an empty relationships array is an empty list", "[mdlist]") {
    json doc = l1_doc();
    doc.at("data")["relationships"] = json::array();
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->titles.empty());
}

TEST_CASE("mdlist: 10000 manga entries are accepted", "[mdlist]") {
    const auto parsed = md::parse_custom_list(doc_with_titles(md::max_list_titles), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->titles.size() == md::max_list_titles);
}

// ---- AC4: rejected documents ------------------------------------------------------------------

TEST_CASE("mdlist: documents that are not safe JSON are rejected", "[mdlist]") {
    const md::ListId l1 = list(1);
    std::string named = l1_doc().dump();
    {
        json doc = l1_doc();
        doc.at("data").at("attributes")["name"] = marker;
        named = doc.dump();
    }
    SECTION("not JSON") { check_malformed(md::parse_custom_list(marker, l1), l1); }
    SECTION("truncated") {
        check_malformed(md::parse_custom_list(named.substr(0, named.size() - 1), l1), l1);
    }
    SECTION("trailing data") {
        check_malformed(md::parse_custom_list(named + std::string{marker}, l1), l1);
    }
    SECTION("empty") { check_malformed(md::parse_custom_list("", l1), l1); }
    SECTION("a UTF-8 BOM") {
        check_malformed(md::parse_custom_list("\xEF\xBB\xBF" + named, l1), l1);
    }
    SECTION("a raw NUL") {
        std::string text = named;
        text.insert(text.find(marker), 1, '\0');
        check_malformed(md::parse_custom_list(text, l1), l1);
    }
    SECTION("nested 65 deep") {
        const std::string deep =
            std::string(65, '[') + std::format("\"{}\"", marker) + std::string(65, ']');
        check_malformed(md::parse_custom_list(deep, l1), l1);
        const auto parsed = md::parse_custom_list(deep, l1);
        REQUIRE(!parsed.has_value());
        CHECK(parsed.error().message.contains("nested deeper than 64 levels"));
    }
    SECTION("200001 separators") {
        std::string dense = "[";
        for (std::size_t i = 0; i < md::max_list_separators; ++i) {
            dense += "0,";
        }
        dense += std::format("\"{}\"]", marker);
        const auto parsed = md::parse_custom_list(dense, l1);
        check_malformed(parsed, l1);
        REQUIRE(!parsed.has_value());
        CHECK(parsed.error().message.contains("more than 200000 structural separators"));
    }
    SECTION("16 MiB + 1") {
        std::string big = named;
        big.append(md::max_api_json_bytes + 1 - big.size(), ' ');
        const auto parsed = md::parse_custom_list(big, l1);
        check_rejected(parsed, l1);
        REQUIRE(!parsed.has_value());
        CHECK(parsed.error().message ==
              std::format("list {}: MangaDex API response (custom list) is larger than 16 MiB",
                          l1.str()));
    }
}

TEST_CASE("mdlist: a separator count at the cap is accepted", "[mdlist]") {
    // L1's own separators plus padding inside an ignored field, exactly at the cap.
    json doc = l1_doc();
    doc["pad"] = json::array();
    std::string text = doc.dump();
    std::size_t separators = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
            in_string = !in_string;
        } else if (!in_string && (c == ',' || c == ':' || c == '[' || c == '{')) {
            ++separators;
        }
    }
    REQUIRE(separators < md::max_list_separators);
    const std::size_t pos = text.find("\"pad\":[") + 7;
    std::string pad = "0";
    for (std::size_t i = separators; i < md::max_list_separators; ++i) {
        pad += ",0";
    }
    text.insert(pos, pad);
    CHECK(md::parse_custom_list(text, list(1)).has_value());
    text.insert(pos, "0,");
    CHECK(!md::parse_custom_list(text, list(1)).has_value());
}

TEST_CASE("mdlist: invalid UTF-8 or a lone surrogate in the name is malformed", "[mdlist]") {
    const md::ListId l1 = list(1);
    const std::string bad = GENERATE(as<std::string>{}, "\xFF\xFE", // invalid bytes
                                     "\xC0\xAF",                    // an overlong '/'
                                     "\xE2\x82",                    // a truncated sequence
                                     "\xED\xA0\x80",                // an encoded surrogate
                                     "\\ud800",                     // a lone high surrogate
                                     "\\udc00x");                   // a lone low surrogate
    std::string text = l1_doc().dump();
    const std::string at_name = "\"Clean List\"";
    const std::size_t name_pos = text.find(at_name);
    REQUIRE(name_pos != std::string::npos);
    text.replace(name_pos, at_name.size(), std::format("\"{}{}\"", marker, bad));
    check_malformed(md::parse_custom_list(text, l1), l1);
}

TEST_CASE("mdlist: invalid UTF-8 outside the name is malformed too", "[mdlist]") {
    const md::ListId l1 = list(1);
    const std::string doc = l1_doc().dump();
    std::string in_field = doc;
    in_field.insert(in_field.size() - 1, std::format(",\"x\":\"{}\xFF\"", marker));
    check_malformed(md::parse_custom_list(in_field, l1), l1);
    std::string in_key = doc;
    in_key.insert(1, std::format("\"{}\xFF\":1,", marker));
    check_malformed(md::parse_custom_list(in_key, l1), l1);
}

/// The detail of every manga relationship id that is not a UUID.
constexpr std::string_view not_uuid = "manga relationship 'id' is not a UUID";

/// One hostile edit of L1's document, and the detail its message must end with (empty: any).
struct Mutation {
    std::string_view name{};
    void (*apply)(json&) = nullptr;
    std::string_view detail{};
};

[[nodiscard]] json& relationships(json& doc) { return doc.at("data").at("relationships"); }

/// Applies `mutation` to L1's document (which also carries the marker) and checks the rejection.
void check_mutation(const Mutation& mutation) {
    INFO(mutation.name);
    const md::ListId l1 = list(1);
    json doc = l1_doc();
    doc["note"] = marker;
    mutation.apply(doc);
    const auto parsed = md::parse_custom_list(doc.dump(), l1);
    check_malformed(parsed, l1);
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().message.ends_with(mutation.detail));
}

TEST_CASE("mdlist: a document missing a required field is malformed", "[mdlist]") {
    const Mutation mutation = GENERATE(values<Mutation>({
        {.name = "data", .apply = [](json& d) { d.erase("data"); }, .detail = "'data' is missing"},
        {.name = "data null",
         .apply = [](json& d) { d["data"] = nullptr; },
         .detail = "'data' is missing"},
        {.name = "data not an object",
         .apply = [](json& d) { d["data"] = json::array({marker}); },
         .detail = "'data' has the wrong type"},
        {.name = "data.id",
         .apply = [](json& d) { d.at("data").erase("id"); },
         .detail = "'id' is missing"},
        {.name = "data.type",
         .apply = [](json& d) { d.at("data").erase("type"); },
         .detail = "'type' is missing"},
        {.name = "data.relationships",
         .apply = [](json& d) { d.at("data").erase("relationships"); },
         .detail = "'relationships' is missing"},
        {.name = "data.relationships null",
         .apply = [](json& d) { d.at("data")["relationships"] = nullptr; },
         .detail = "'relationships' is missing"},
        {.name = "data.relationships not an array",
         .apply = [](json& d) { d.at("data")["relationships"] = {{"id", marker}}; },
         .detail = "'relationships' has the wrong type"},
        {.name = "the document is an array",
         .apply = [](json& d) { d = json::array({marker}); },
         .detail = "'document' has the wrong type"},
        {.name = "the document is a string",
         .apply = [](json& d) { d = marker; },
         .detail = "'document' has the wrong type"},
    }));
    check_mutation(mutation);
}

TEST_CASE("mdlist: a wrong data.type, data.id or name type is malformed", "[mdlist]") {
    const Mutation mutation = GENERATE(values<Mutation>({
        {.name = "type manga",
         .apply = [](json& d) { d.at("data")["type"] = "manga"; },
         .detail = "'type' is not custom_list"},
        {.name = "type with a marker",
         .apply = [](json& d) { d.at("data")["type"] = marker; },
         .detail = "'type' is not custom_list"},
        {.name = "type not a string",
         .apply = [](json& d) { d.at("data")["type"] = 7; },
         .detail = "'type' has the wrong type"},
        {.name = "id not a UUID",
         .apply = [](json& d) { d.at("data")["id"] = marker; },
         .detail = "'id' is not a UUID"},
        {.name = "id not a string",
         .apply = [](json& d) { d.at("data")["id"] = 42; },
         .detail = "'id' has the wrong type"},
        {.name = "id of another list",
         .apply = [](json& d) { d.at("data")["id"] = mdx::list_id(6); },
         .detail = "'id' is not the requested list"},
        {.name = "name not a string",
         .apply = [](json& d) { d.at("data").at("attributes")["name"] = json::array({marker}); },
         .detail = "'name' has the wrong type"},
        {.name = "attributes not an object",
         .apply = [](json& d) { d.at("data")["attributes"] = marker; },
         .detail = "'attributes' has the wrong type"},
    }));
    check_mutation(mutation);
}

TEST_CASE("mdlist: the L6 document (data.id is L1) is rejected when L6 was asked for", "[mdlist]") {
    const auto doc = mdx::list_json({.id = mdx::list_id(6),
                                     .manga_ids = {std::string{clean_id}},
                                     .data_id = mdx::list_id(1)})
                         .dump();
    const auto parsed = md::parse_custom_list(doc, list(6));
    check_malformed(parsed, list(6));
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().message ==
          "list a0000000-0000-4000-8000-000000000006: malformed MangaDex API response (custom "
          "list): 'id' is not the requested list");
    // The same bytes are fine when L1 is the one asked for.
    CHECK(md::parse_custom_list(doc, list(1)).has_value());
}

TEST_CASE("mdlist: the L5 document (a manga id ../x) is malformed", "[mdlist]") {
    const auto parsed = md::parse_custom_list(
        mdx::list_json({.id = mdx::list_id(5), .name = "Bad Id", .manga_ids = {"../x"}}).dump(),
        list(5));
    check_malformed(parsed, list(5));
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().message ==
          "list a0000000-0000-4000-8000-000000000005: malformed MangaDex API response (custom "
          "list): manga relationship 'id' is not a UUID");
}

TEST_CASE("mdlist: a bad manga relationship makes the whole document malformed", "[mdlist]") {
    const Mutation mutation = GENERATE(values<Mutation>({
        {.name = "a path-like id",
         .apply =
             [](json& d) {
                 auto& rels = relationships(d);
                 rels.insert(rels.begin(), json{{"id", "../x"}, {"type", "manga"}, {"m", marker}});
             },
         .detail = not_uuid},
        {.name = "35 characters",
         .apply =
             [](json& d) {
                 auto& rels = relationships(d);
                 rels.insert(rels.begin(), json{{"id", "11111111-1111-4111-8111-11111111111"},
                                                {"type", "manga"}});
             },
         .detail = not_uuid},
        {.name = "37 characters",
         .apply =
             [](json& d) {
                 auto& rels = relationships(d);
                 rels.insert(rels.begin(), json{{"id", "11111111-1111-4111-8111-1111111111111"},
                                                {"type", "manga"}});
             },
         .detail = not_uuid},
        {.name = "a title URL",
         .apply =
             [](json& d) {
                 auto& rels = relationships(d);
                 rels.insert(
                     rels.begin(),
                     json{{"id", "https://mangadex.org/title/11111111-1111-4111-8111-111111111111"},
                          {"type", "manga"}});
             },
         .detail = not_uuid},
        {.name = "a marker",
         .apply = [](json& d) { relationships(d).push_back({{"id", marker}, {"type", "manga"}}); },
         .detail = not_uuid},
        {.name = "a number",
         .apply = [](json& d) { relationships(d).push_back({{"id", 42}, {"type", "manga"}}); },
         .detail = "'id' has the wrong type"},
        {.name = "no id",
         .apply = [](json& d) { relationships(d).push_back({{"type", "manga"}, {"m", marker}}); },
         .detail = "'id' is missing"},
        {.name = "a null id",
         .apply = [](json& d) { relationships(d).push_back({{"type", "manga"}, {"id", nullptr}}); },
         .detail = "'id' is missing"},
        {.name = "a non-object entry",
         .apply = [](json& d) { relationships(d).push_back(marker); },
         .detail = "a relationship is not an object"},
        {.name = "a null entry",
         .apply = [](json& d) { relationships(d).push_back(nullptr); },
         .detail = "a relationship is not an object"},
        {.name = "an entry without type",
         .apply = [](json& d) { relationships(d).push_back({{"id", marker}}); },
         .detail = "a relationship has no 'type'"},
        {.name = "an entry with a null type",
         .apply = [](json& d) { relationships(d).push_back({{"id", marker}, {"type", nullptr}}); },
         .detail = "a relationship has no 'type'"},
        {.name = "an entry whose type is not a string",
         .apply =
             [](json& d) {
                 relationships(d).push_back({{"id", marker}, {"type", json::array({"manga"})}});
             },
         .detail = "'type' has the wrong type"},
    }));
    check_mutation(mutation);
}

TEST_CASE("mdlist: more than 10000 manga entries is refused with the cap message", "[mdlist]") {
    const md::ListId l1 = list(1);
    const auto parsed = md::parse_custom_list(doc_with_titles(md::max_list_titles + 1), l1);
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().code == md::Errc::network);
    CHECK(parsed.error().message ==
          "list a0000000-0000-4000-8000-000000000001 has more than 10000 titles; refusing to "
          "truncate it; split it into smaller lists");
}

TEST_CASE("mdlist: user entries do not count towards the title cap", "[mdlist]") {
    json doc = json::parse(doc_with_titles(md::max_list_titles));
    json& rels = doc.at("data").at("relationships");
    for (int i = 0; i < 5; ++i) {
        rels.push_back({{"id", std::string{mdx::list_owner_id}}, {"type", "user"}});
    }
    const auto parsed = md::parse_custom_list(doc.dump(), list(1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->titles.size() == md::max_list_titles);
}

// ---- the 401/403 error body ---------------------------------------------------------------------

TEST_CASE("mdlist: a CustomList error body is recognised", "[mdlist]") {
    CHECK(md::is_custom_list_error_body(
        R"({"result":"error","errors":[{"id":"e1","status":404,"title":"not_found_http_exception",)"
        R"("detail":"CustomList `a0000000-0000-4000-8000-000000000001` not found.","context":null}]})"));
    CHECK(md::is_custom_list_error_body(
        R"({"result":"error","errors":[{"detail":"Something else"},7,null,)"
        R"({"detail":"CustomList is private"}]})"));
}

TEST_CASE("mdlist: anything else is not a CustomList error body", "[mdlist]") {
    const std::string body = GENERATE(
        as<std::string>{}, "", "not json", "{}", "[]", R"("CustomList x")",
        R"({"result":"ok","errors":[{"detail":"CustomList x"}]})",
        R"({"result":7,"errors":[{"detail":"CustomList x"}]})",
        R"({"errors":[{"detail":"CustomList x"}]})", R"({"result":"error"})",
        R"({"result":"error","errors":{"detail":"CustomList x"}})",
        R"({"result":"error","errors":[]})", R"({"result":"error","errors":[{"detail":7}]})",
        R"({"result":"error","errors":[{"detail":"Manga x not found"}]})",
        R"({"result":"error","errors":[{"detail":"CustomList"}]})",
        R"({"result":"error","errors":[{"detail":"customlist x"}]})",
        R"({"result":"error","errors":[{"title":"CustomList x"}]})",
        "\xEF\xBB\xBF{\"result\":\"error\",\"errors\":[{\"detail\":\"CustomList x\"}]}",
        R"({"result":"error","errors":[{"detail":"CustomList x"}]} trailing)");
    CAPTURE(body);
    CHECK(!md::is_custom_list_error_body(body));
}

TEST_CASE("mdlist: an error body over 64 KiB or nested too deep is not recognised", "[mdlist]") {
    const std::string good = R"({"result":"error","errors":[{"detail":"CustomList x"}]})";
    REQUIRE(md::is_custom_list_error_body(good));
    std::string big = good;
    big.append((std::size_t{64} * 1024) - big.size(), ' ');
    CHECK(md::is_custom_list_error_body(big)); // exactly 64 KiB
    big.push_back(' ');
    CHECK(!md::is_custom_list_error_body(big));

    std::string deep = R"({"result":"error","errors":[{"detail":"CustomList x","x":)";
    deep += std::string(62, '[') + std::string(62, ']') + "}]}";
    CHECK(!md::is_custom_list_error_body(deep)); // depth 65
}

// ---- display name and summary -----------------------------------------------------------------

TEST_CASE("mdlist: the display name is cut to 64 bytes on a UTF-8 boundary", "[mdlist]") {
    CHECK(md::list_name_for_display("").empty());
    CHECK(md::list_name_for_display("Clean List") == "Clean List");
    const std::string exact(md::max_list_display_name_bytes, 'x');
    CHECK(md::list_name_for_display(exact) == exact);
    CHECK(md::list_name_for_display(exact + "y") == exact + "...");

    // 63 ASCII bytes then "€": the cut at 64 falls inside the euro sign, so it backs off to 63.
    const std::string ascii63(md::max_list_display_name_bytes - 1, 'a');
    CHECK(md::list_name_for_display(ascii63 + "\xE2\x82\xAC") == ascii63 + "...");
    // 62 ASCII bytes then "é": exactly 64 bytes, kept whole.
    const std::string ascii62(md::max_list_display_name_bytes - 2, 'a');
    CHECK(md::list_name_for_display(ascii62 + "\xC3\xA9") == ascii62 + "\xC3\xA9");
    CHECK(md::list_name_for_display(ascii62 + "\xC3\xA9z") == ascii62 + "\xC3\xA9...");

    // Raw: control bytes are the app's business (terminal_safe), not this function's.
    CHECK(md::list_name_for_display("a\x1b[2Jb") == "a\x1b[2Jb");
}

TEST_CASE("mdlist: a display cut never splits a sequence, whatever the input", "[mdlist]") {
    const std::string_view character =
        GENERATE(as<std::string_view>{}, "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80");
    const std::size_t ascii = GENERATE(range<std::size_t>(0, 5));
    std::string name(ascii, 'n');
    while (name.size() < 200) {
        name += character;
    }
    CAPTURE(character.size(), ascii);
    const std::string shown = md::list_name_for_display(name);
    REQUIRE(shown.ends_with("..."));
    const std::string_view kept = std::string_view{shown}.substr(0, shown.size() - 3);
    CHECK(kept.size() <= md::max_list_display_name_bytes);
    CHECK(kept.size() > md::max_list_display_name_bytes - character.size());
    CHECK(valid_utf8(kept));
    CHECK(name.starts_with(kept));
}

TEST_CASE("mdlist: the summary carries the id, the name and the website URL", "[mdlist]") {
    const md::CustomList named{.id = list(1), .name = "Clean List", .titles = {manga(clean_id)}};
    CHECK(md::list_summary(named) ==
          md::ListSummary{.id = "a0000000-0000-4000-8000-000000000001",
                          .name = "Clean List",
                          .url = "https://mangadex.org/list/a0000000-0000-4000-8000-000000000001"});
    const md::CustomList unnamed{.id = list(10)};
    const md::ListSummary summary = md::list_summary(unnamed);
    CHECK(summary.id == "a0000000-0000-4000-8000-00000000000a");
    CHECK(!summary.name.has_value());
    CHECK(summary.url == "https://mangadex.org/list/a0000000-0000-4000-8000-00000000000a");
}

} // namespace
