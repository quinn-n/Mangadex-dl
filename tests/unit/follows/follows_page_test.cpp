// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/follows/follows_page_test.cpp: the follows collection document (follows plan T10,
// spec "Behaviour" 7.1 and the exit-code table): the two request URLs, parse_follows_page's
// accepted MangaList shapes and every rejection (each a fixed Errc::network message, exit 3, that
// never quotes response bytes).

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace md = mangadex_dl;
using nlohmann::json;

/// Planted in every hostile document: no error message may contain it.
constexpr std::string_view marker = "MARKER-7f3a9c";

constexpr std::string_view id_1 = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view id_6 = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view id_7 = "77777777-7777-4777-8777-777777777777";

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

/// One MangaList entry as MangaDex sends it (attributes present, never read).
[[nodiscard]] json entry(std::string_view id) {
    return {{"id", std::string{id}},
            {"type", "manga"},
            {"attributes", {{"title", {{"en", "A Title"}}}, {"tags", json::array()}}},
            {"relationships", json::array({{{"id", std::string{id_7}}, {"type", "author"}}})}};
}

/// A follows page holding `ids`, at `offset`, with `total`.
[[nodiscard]] json page(const std::vector<std::string_view>& ids, std::size_t offset,
                        std::size_t total) {
    json data = json::array();
    for (const std::string_view id : ids) {
        data.push_back(entry(id));
    }
    return {{"result", "ok"},   {"response", "collection"},
            {"data", data},     {"limit", md::follows_page_limit},
            {"offset", offset}, {"total", total}};
}

/// One rejected page: Errc::network (exit 3), the parser's fixed prefix, never the marker.
void check_rejected(const md::Result<md::FollowsPage>& parsed) {
    REQUIRE(!parsed.has_value());
    const md::Error& error = parsed.error();
    INFO(error.message);
    CHECK(error.code == md::Errc::network);
    CHECK(md::exit_code_for(error.code) == md::exit_code::network);
    CHECK(!error.message.contains(marker));
}

void check_malformed(const md::Result<md::FollowsPage>& parsed) {
    check_rejected(parsed);
    if (!parsed.has_value()) {
        CHECK(
            parsed.error().message.starts_with("malformed MangaDex API response (follows page): "));
    }
}

// ---- URLs -----------------------------------------------------------------------------------

TEST_CASE("follows page: the request URLs are the fixed strings", "[follows]") {
    CHECK(md::follows_request_url("https://api.mangadex.org", 0) ==
          "https://api.mangadex.org/user/follows/manga?limit=100&offset=0");
    CHECK(md::follows_request_url("https://api.mangadex.org", 9900) ==
          "https://api.mangadex.org/user/follows/manga?limit=100&offset=9900");
    CHECK(md::follows_request_url("file:///tmp/t/api", 100) ==
          "file:///tmp/t/api/user/follows/manga?limit=100&offset=100");
    CHECK(md::status_map_request_url("https://api.mangadex.org") ==
          "https://api.mangadex.org/manga/status");
    CHECK(md::status_map_request_url("file:///tmp/t/api") == "file:///tmp/t/api/manga/status");
}

// ---- accepted pages -------------------------------------------------------------------------

TEST_CASE("follows page: the manga come in API order with the total", "[follows]") {
    const auto parsed = md::parse_follows_page(page({id_6, id_1, id_7}, 0, 4).dump());
    REQUIRE(parsed.has_value());
    CHECK(parsed->titles == std::vector{manga(id_6), manga(id_1), manga(id_7)});
    CHECK(parsed->total == 4);
}

TEST_CASE("follows page: ids are canonicalised and duplicates are kept", "[follows]") {
    const auto parsed = md::parse_follows_page(
        page({"AAAAAAAA-0000-4000-8000-00000000000A", id_1, id_1}, 100, 103).dump());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->titles.size() == 3);
    CHECK(parsed->titles[0].str() == "aaaaaaaa-0000-4000-8000-00000000000a");
    CHECK(parsed->titles[1] == manga(id_1));
    CHECK(parsed->titles[2] == manga(id_1));
    CHECK(parsed->total == 103);
}

TEST_CASE("follows page: an empty page and a past-the-end page are empty", "[follows]") {
    const auto none = md::parse_follows_page(page({}, 0, 0).dump());
    REQUIRE(none.has_value());
    CHECK(none->titles.empty());
    CHECK(none->total == 0);

    const auto past = md::parse_follows_page(page({}, 300, 250).dump());
    REQUIRE(past.has_value());
    CHECK(past->titles.empty());
    CHECK(past->total == 250);
}

TEST_CASE("follows page: unknown fields and odd attributes are ignored", "[follows]") {
    json doc = page({id_1}, 0, 1);
    doc["extra"] = {{"nested", {1, 2, 3}}};
    doc.at("data").at(0)["attributes"] = std::string{marker}; // never read
    doc.at("data").at(0)["relationships"] = nullptr;
    doc.at("data").at(0)["future"] = {{"x", 1}};
    const auto parsed = md::parse_follows_page(doc.dump());
    REQUIRE(parsed.has_value());
    CHECK(parsed->titles == std::vector{manga(id_1)});
}

TEST_CASE("follows page: a full page of 100 is accepted", "[follows]") {
    std::vector<std::string> ids{};
    ids.reserve(md::follows_page_limit);
    for (std::size_t i = 0; i < md::follows_page_limit; ++i) {
        ids.push_back(std::format("c0000000-0000-4000-8000-{:012x}", i));
    }
    const std::vector<std::string_view> views(ids.begin(), ids.end());
    const auto parsed = md::parse_follows_page(page(views, 0, 250).dump());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->titles.size() == md::follows_page_limit);
    CHECK(parsed->titles.back().str() == ids.back());
}

// ---- rejected pages -------------------------------------------------------------------------

TEST_CASE("follows page: a malformed structure is rejected with fixed text", "[follows]") {
    using Edit = void (*)(json&);
    const Edit edit = GENERATE(values<Edit>({
        [](json& d) { d = json::array({std::string{marker}}); },
        [](json& d) { d.erase("result"); },
        [](json& d) { d["result"] = nullptr; },
        [](json& d) { d["result"] = "error"; },
        [](json& d) { d["result"] = std::string{marker}; },
        [](json& d) { d["result"] = 1; },
        [](json& d) { d.erase("data"); },
        [](json& d) { d["data"] = {{"id", std::string{marker}}}; },
        [](json& d) { d["data"] = std::string{marker}; },
        [](json& d) { d.at("data").push_back(std::string{marker}); },
        [](json& d) { d.at("data").push_back(nullptr); },
        [](json& d) { d.at("data").at(0).erase("type"); },
        [](json& d) { d.at("data").at(0)["type"] = std::string{marker}; },
        [](json& d) { d.at("data").at(0)["type"] = "author"; },
        [](json& d) { d.at("data").at(0)["type"] = 7; },
        [](json& d) { d.at("data").at(0).erase("id"); },
        [](json& d) { d.at("data").at(0)["id"] = nullptr; },
        [](json& d) { d.at("data").at(0)["id"] = 11; },
        [](json& d) { d.at("data").at(0)["id"] = std::format("../{}", marker); },
        [](json& d) { d.at("data").at(0)["id"] = "11111111-1111-4111-8111-11111111111"; },
        [](json& d) { d.at("data").at(0)["id"] = "11111111-1111-4111-8111-1111111111111"; },
        [](json& d) { d.at("data").at(0)["id"] = "11111111x1111-4111-8111-111111111111"; },
        [](json& d) { d.at("data").at(0)["id"] = "g1111111-1111-4111-8111-111111111111"; },
        [](json& d) { d.erase("limit"); },
        [](json& d) { d.erase("offset"); },
        [](json& d) { d.erase("total"); },
        [](json& d) { d["total"] = -1; },
        [](json& d) { d["total"] = 1.5; },
        [](json& d) { d["total"] = std::string{marker}; },
        [](json& d) { d["offset"] = -100; },
        [](json& d) { d["limit"] = "100"; },
        [](json& d) { d["limit"] = md::follows_page_limit + 1; },
        [](json& d) { d["limit"] = 1; }, // two entries, limit 1
    }));
    json doc = page({id_1, id_6}, 0, 2);
    edit(doc);
    check_malformed(md::parse_follows_page(doc.dump()));
}

TEST_CASE("follows page: non-JSON, trailing data, a BOM and a raw NUL are rejected", "[follows]") {
    const std::string valid = page({id_1}, 0, 1).dump();
    const std::string text = GENERATE_COPY(values<std::string>({
        std::string{marker},
        std::string{},
        valid + std::string{marker},
        valid + "{}",
        std::string{"\xEF\xBB\xBF"} + valid,
        valid.substr(0, 10) + std::string{"\0", 1} + valid.substr(10),
        std::string{"{\"result\":\"ok\",\"data\":[\xFF]}"},
    }));
    check_malformed(md::parse_follows_page(text));
}

TEST_CASE("follows page: a page over 16 MiB is rejected before parsing", "[follows]") {
    json doc = page({id_1}, 0, 1);
    doc["pad"] = std::string(md::max_api_json_bytes, 'x');
    const auto parsed = md::parse_follows_page(doc.dump());
    check_rejected(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message ==
              "MangaDex API response (follows page) is larger than 16 MiB");
    }
}

TEST_CASE("follows page: a separator-dense page is rejected by the separator guard", "[follows]") {
    // 2 000 001 commas in about 4 MB: well under the byte cap, far over any honest page.
    std::string text = R"({"result":"ok","data":[],"limit":100,"offset":0,"total":0,"pad":[0)";
    for (std::size_t i = 0; i < 2000000; ++i) {
        text += ",0";
    }
    text += "]}";
    REQUIRE(text.size() < md::max_api_json_bytes);
    const auto parsed = md::parse_follows_page(text);
    check_malformed(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message.contains("structural separators"));
    }
}

TEST_CASE("follows page: a deeply nested page is rejected by the depth guard", "[follows]") {
    std::string text = R"({"result":"ok","data":[],"limit":100,"offset":0,"total":0,"pad":)";
    text += std::string(100, '[');
    text += std::string(100, ']');
    text += '}';
    const auto parsed = md::parse_follows_page(text);
    check_malformed(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message.contains("nested deeper than"));
    }
}

} // namespace
