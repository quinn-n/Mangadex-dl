// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/follows/status_map_test.cpp: the reading-status map (follows spec AC51, A1 rev2,
// rev3 S1/S2; follows plan T10, fact-check V23/V24): parse_status_map's accepted shapes ({} and
// the empty [] form), key canonicalisation, unknown but well-formed values, the duplicate-key
// behaviour, the 100 000-entry cap (fired before the separator cap), and every rejection (each a
// fixed Errc::network message, exit 3, that never quotes response bytes).

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>

namespace {

namespace md = mangadex_dl;
using nlohmann::json;

/// Planted in every hostile document: no error message may contain it.
constexpr std::string_view marker = "MARKER-7f3a9c";
constexpr std::string_view malformed_prefix =
    "malformed MangaDex API response (reading statuses): ";
constexpr std::string_view entry_cap_message =
    "your MangaDex reading statuses hold more than 100000 titles; refusing to truncate them";

constexpr std::string_view id_1 = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view id_6 = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view id_7 = "77777777-7777-4777-8777-777777777777";
constexpr std::string_view id_9 = "99999999-9999-4999-8999-999999999999";

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

/// FS1's map (spec "Fixture data"): `paused` is unknown, 9999… is not followed.
[[nodiscard]] json fs1_doc() {
    return {{"result", "ok"},
            {"statuses",
             {{std::string{id_1}, "reading"},
              {std::string{id_6}, "completed"},
              {std::string{id_7}, "paused"},
              {std::string{id_9}, "reading"}}}};
}

void check_rejected(const md::Result<md::StatusMap>& parsed) {
    REQUIRE(!parsed.has_value());
    const md::Error& error = parsed.error();
    INFO(error.message);
    CHECK(error.code == md::Errc::network);
    CHECK(md::exit_code_for(error.code) == md::exit_code::network);
    CHECK(!error.message.contains(marker));
}

void check_malformed(const md::Result<md::StatusMap>& parsed) {
    check_rejected(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message.starts_with(malformed_prefix));
    }
}

/// `{"result":"ok","statuses":{<count> distinct ids: "reading"}}`, written directly (no DOM).
[[nodiscard]] std::string doc_with_entries(std::size_t count) {
    std::string text = R"({"result":"ok","statuses":{)";
    text.reserve(text.size() + (count * 52) + 2);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += std::format(R"("c0000000-0000-4000-8000-{:012x}":"reading")", i);
    }
    text += "}}";
    return text;
}

/// The structural separators json_util counts (`,` `:` `[` `{`); `text` holds none in strings.
[[nodiscard]] std::size_t separators(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return c == ',' || c == ':' || c == '[' || c == '{'; }));
}

// ---- accepted documents ---------------------------------------------------------------------

TEST_CASE("status map: the documented shape gives every entry, unknown values kept", "[follows]") {
    const auto parsed = md::parse_status_map(fs1_doc().dump());
    REQUIRE(parsed.has_value());
    const std::map<md::MangaId, std::string> expected{{manga(id_1), "reading"},
                                                      {manga(id_6), "completed"},
                                                      {manga(id_7), "paused"},
                                                      {manga(id_9), "reading"}};
    CHECK(parsed->statuses == expected);
}

TEST_CASE("status map: every API value and odd well-formed values are kept", "[follows]") {
    const std::string value =
        GENERATE(as<std::string>{}, "reading", "on_hold", "plan_to_read", "dropped", "re_reading",
                 "completed", "paused", "_", "a", std::string(32, 'z'), "new_status_");
    const json doc = {{"result", "ok"}, {"statuses", {{std::string{id_1}, value}}}};
    const auto parsed = md::parse_status_map(doc.dump());
    REQUIRE(parsed.has_value());
    CHECK(parsed->statuses == std::map<md::MangaId, std::string>{{manga(id_1), value}});
}

TEST_CASE("status map: an empty object and the empty array form are an empty map", "[follows]") {
    const std::string text = GENERATE(as<std::string>{}, R"({"result":"ok","statuses":{}})",
                                      R"({"result":"ok","statuses":[]})",
                                      R"({"statuses":[],"result":"ok","extra":{"a":[1]}})");
    const auto parsed = md::parse_status_map(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->statuses.empty());
}

TEST_CASE("status map: an upper-case key is canonicalised", "[follows]") {
    const json doc = {{"result", "ok"},
                      {"statuses", {{"AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", "dropped"}}}};
    const auto parsed = md::parse_status_map(doc.dump());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->statuses.size() == 1);
    CHECK(parsed->statuses.begin()->first.str() == "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    CHECK(parsed->statuses.begin()->second == "dropped");
}

TEST_CASE("status map: two keys that canonicalise alike are kept once, deterministically",
          "[follows]") {
    // nlohmann's object is a std::map, so "first" is the first in raw-key byte order: the
    // upper-case spelling ('A' < 'a') wins whatever the document order.
    const std::string text = GENERATE(
        as<std::string>{},
        R"({"result":"ok","statuses":{"AAAAAAAA-0000-4000-8000-000000000000":"dropped","aaaaaaaa-0000-4000-8000-000000000000":"reading"}})",
        R"({"result":"ok","statuses":{"aaaaaaaa-0000-4000-8000-000000000000":"reading","AAAAAAAA-0000-4000-8000-000000000000":"dropped"}})");
    const auto parsed = md::parse_status_map(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->statuses == std::map<md::MangaId, std::string>{
                                  {manga("aaaaaaaa-0000-4000-8000-000000000000"), "dropped"}});
}

TEST_CASE("status map: a repeated JSON key is not detected and the last value wins (V23)",
          "[follows]") {
    const auto parsed = md::parse_status_map(
        std::format(R"({{"result":"ok","statuses":{{"{0}":"reading","{0}":"dropped"}}}})", id_1));
    REQUIRE(parsed.has_value());
    CHECK(parsed->statuses == std::map<md::MangaId, std::string>{{manga(id_1), "dropped"}});
}

TEST_CASE("status map: 100 000 entries are accepted", "[follows]") {
    const std::string text = doc_with_entries(md::max_status_entries);
    const auto parsed = md::parse_status_map(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->statuses.size() == md::max_status_entries);
}

// ---- rejected documents ---------------------------------------------------------------------

TEST_CASE("status map: 100 001 entries are refused by the entry cap, not the separator cap",
          "[follows]") {
    const std::string text = doc_with_entries(md::max_status_entries + 1);
    // Two separators per entry (V24): far below the separator cap, so only the entry cap fires.
    REQUIRE(separators(text) < md::max_status_separators);
    REQUIRE(text.size() < md::max_api_json_bytes);
    const auto parsed = md::parse_status_map(text);
    check_rejected(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message == entry_cap_message);
    }
}

TEST_CASE("status map: more than the separator cap is rejected before parsing", "[follows]") {
    std::string text = R"({"result":"ok","statuses":{},"pad":[0)";
    for (std::size_t i = 0; i < md::max_status_separators; ++i) {
        text += ",0";
    }
    text += "]}";
    REQUIRE(separators(text) > md::max_status_separators);
    REQUIRE(text.size() < md::max_api_json_bytes);
    const auto parsed = md::parse_status_map(text);
    check_malformed(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message ==
              std::format("{}more than 1000000 structural separators", malformed_prefix));
    }
}

TEST_CASE("status map: a document over 16 MiB is rejected", "[follows]") {
    json doc = fs1_doc();
    doc["pad"] = std::string(md::max_api_json_bytes, 'x');
    const auto parsed = md::parse_status_map(doc.dump());
    check_rejected(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message ==
              "MangaDex API response (reading statuses) is larger than 16 MiB");
    }
}

TEST_CASE("status map: a malformed structure, key or value is rejected with fixed text",
          "[follows]") {
    using Edit = void (*)(json&);
    const Edit edit = GENERATE(values<Edit>({
        [](json& d) { d = json::array({std::string{marker}}); },
        [](json& d) { d = std::string{marker}; },
        [](json& d) { d.erase("result"); },
        [](json& d) { d["result"] = nullptr; },
        [](json& d) { d["result"] = "error"; },
        [](json& d) { d["result"] = std::string{marker}; },
        [](json& d) { d["result"] = true; },
        [](json& d) { d.erase("statuses"); },
        [](json& d) { d["statuses"] = nullptr; },
        [](json& d) { d["statuses"] = json::array({std::string{marker}}); },
        [](json& d) { d["statuses"] = json::array({json::object()}); },
        [](json& d) { d["statuses"] = std::string{marker}; },
        [](json& d) { d["statuses"] = 0; },
        [](json& d) { d.at("statuses")["../x"] = "reading"; },
        [](json& d) { d.at("statuses")[std::format("../{}", marker)] = "reading"; },
        [](json& d) { d.at("statuses")["11111111-1111-4111-8111-11111111111"] = "reading"; },
        [](json& d) { d.at("statuses")["11111111-1111-4111-8111-1111111111111"] = "reading"; },
        [](json& d) { d.at("statuses")[""] = "reading"; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = 1; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = nullptr; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = json::object(); },
        [](json& d) { d.at("statuses")[std::string{id_1}] = json::array({"reading"}); },
        [](json& d) { d.at("statuses")[std::string{id_1}] = ""; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = std::string(33, 'z'); },
        [](json& d) { d.at("statuses")[std::string{id_1}] = "Reading"; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = "on-hold"; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = "plan to read"; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = "reading2"; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = std::string{marker}; },
        [](json& d) { d.at("statuses")[std::string{id_1}] = "r\xC3\xA9"; },
    }));
    json doc = fs1_doc();
    edit(doc);
    check_malformed(md::parse_status_map(doc.dump()));
}

TEST_CASE("status map: non-JSON, trailing data, a BOM and a raw NUL are rejected", "[follows]") {
    const std::string valid = fs1_doc().dump();
    const std::string text = GENERATE_COPY(values<std::string>({
        std::string{marker},
        std::string{},
        valid + std::string{marker},
        valid + "{}",
        std::string{"\xEF\xBB\xBF"} + valid,
        valid.substr(0, 10) + std::string{"\0", 1} + valid.substr(10),
        std::string{"{\"result\":\"ok\",\"statuses\":{\"\xFF\":\"reading\"}}"},
    }));
    check_malformed(md::parse_status_map(text));
}

TEST_CASE("status map: a deeply nested document is rejected by the depth guard", "[follows]") {
    std::string text = R"({"result":"ok","statuses":{},"pad":)";
    text += std::string(100, '[');
    text += std::string(100, ']');
    text += '}';
    const auto parsed = md::parse_status_map(text);
    check_malformed(parsed);
    if (!parsed.has_value()) {
        CHECK(parsed.error().message.contains("nested deeper than"));
    }
}

} // namespace
