// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/follows/status_filter_test.cpp: the --status words and the follows ∩ status filter
// (follows spec AC50, AC52, A1 rev2; follows plan T10): reading_status_name /
// parse_reading_status, StatusFilter::parse (comma splitting, kebab aliases, exact lower case,
// de-duplication keeping the first, the two exit-2 texts, the 32-byte echo cut), and
// filter_follows over the spec's FS1 set.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;

constexpr std::string_view word_list =
    "reading, on_hold, plan_to_read, dropped, re_reading or completed";

constexpr std::string_view id_1 = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view id_6 = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view id_7 = "77777777-7777-4777-8777-777777777777";
constexpr std::string_view id_8 = "88888888-8888-4888-8888-888888888888";
constexpr std::string_view id_9 = "99999999-9999-4999-8999-999999999999";

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] md::Result<md::StatusFilter> parse(std::initializer_list<std::string> words) {
    const std::vector<std::string> owned(words);
    return md::StatusFilter::parse(owned);
}

[[nodiscard]] md::StatusFilter filter(std::initializer_list<std::string> words) {
    auto parsed = parse(words);
    REQUIRE(parsed.has_value());
    return *parsed;
}

[[nodiscard]] std::string unknown_message(std::string_view shown) {
    return std::format("--status: unknown reading status '{}'; use {}", shown, word_list);
}

[[nodiscard]] std::string empty_message() {
    return std::format("--status needs a reading status ({})", word_list);
}

void check_usage(const md::Result<md::StatusFilter>& parsed, std::string_view message) {
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().code == md::Errc::usage);
    CHECK(md::exit_code_for(parsed.error().code) == md::exit_code::usage);
    CHECK(parsed.error().message == message);
}

/// FS1 (spec "Fixture data"): follows 6666…, 1111…, 7777…, 8888…; statuses 1111 reading,
/// 6666 completed, 7777 paused (unknown), 9999 reading (not followed); 8888 has no status.
[[nodiscard]] std::vector<md::MangaId> fs1_follows() {
    return {manga(id_6), manga(id_1), manga(id_7), manga(id_8)};
}

[[nodiscard]] md::StatusMap fs1_map() {
    return md::StatusMap{.statuses = {{manga(id_1), "reading"},
                                      {manga(id_6), "completed"},
                                      {manga(id_7), "paused"},
                                      {manga(id_9), "reading"}}};
}

// ---- the six statuses -----------------------------------------------------------------------

TEST_CASE("status filter: every status round-trips through its API name", "[follows]") {
    constexpr std::array all{md::ReadingStatus::reading,      md::ReadingStatus::on_hold,
                             md::ReadingStatus::plan_to_read, md::ReadingStatus::dropped,
                             md::ReadingStatus::re_reading,   md::ReadingStatus::completed};
    constexpr std::array names{"reading", "on_hold",    "plan_to_read",
                               "dropped", "re_reading", "completed"};
    for (std::size_t i = 0; i < all.size(); ++i) {
        CAPTURE(i);
        CHECK(md::reading_status_name(all.at(i)) == names.at(i));
        CHECK(md::parse_reading_status(names.at(i)) == all.at(i));
    }
}

TEST_CASE("status filter: parse_reading_status takes only exact API values", "[follows]") {
    const std::string_view value =
        GENERATE(as<std::string_view>{}, "", "Reading", "READING", "on-hold", "plan-to-read",
                 "re-reading", "paused", "all", "reading ", " reading", "readin", "readingg");
    CAPTURE(value);
    CHECK(!md::parse_reading_status(value).has_value());
}

// ---- StatusFilter::parse (AC50) -------------------------------------------------------------

TEST_CASE("status filter: API words and kebab aliases are normalised", "[follows]") {
    using Case = std::pair<std::string, std::vector<std::string>>;
    const Case c = GENERATE(values<Case>({
        {"reading", {"reading"}},
        {"on-hold", {"on_hold"}},
        {"on_hold", {"on_hold"}},
        {"plan-to-read", {"plan_to_read"}},
        {"plan_to_read", {"plan_to_read"}},
        {"re-reading", {"re_reading"}},
        {"re_reading", {"re_reading"}},
        {"dropped", {"dropped"}},
        {"completed", {"completed"}},
        {"on_hold,reading", {"on_hold", "reading"}},
        {"plan-to-read,re-reading,dropped", {"plan_to_read", "re_reading", "dropped"}},
    }));
    CAPTURE(c.first);
    CHECK(filter({c.first}).names() == c.second);
}

TEST_CASE("status filter: repeated flags equal one comma-separated value", "[follows]") {
    const md::StatusFilter repeated = filter({"reading", "on_hold"});
    const md::StatusFilter joined = filter({"reading,on_hold"});
    CHECK(repeated == joined);
    CHECK(repeated.names() == std::vector<std::string>{"reading", "on_hold"});
    CHECK(repeated.contains(md::ReadingStatus::reading));
    CHECK(repeated.contains(md::ReadingStatus::on_hold));
    CHECK(!repeated.contains(md::ReadingStatus::completed));
    CHECK(!repeated.contains(md::ReadingStatus::plan_to_read));
}

TEST_CASE("status filter: duplicates are dropped keeping the first occurrence", "[follows]") {
    CHECK(filter({"reading", "on-hold", "reading"}).names() ==
          std::vector<std::string>{"reading", "on_hold"});
    CHECK(filter({"on_hold,reading,on-hold"}).names() ==
          std::vector<std::string>{"on_hold", "reading"});
    CHECK(filter({"completed", "completed,completed"}).names() ==
          std::vector<std::string>{"completed"});
    CHECK(filter({"on-hold", "on_hold"}) == filter({"on_hold"}));
}

TEST_CASE("status filter: all six words select all six statuses", "[follows]") {
    const md::StatusFilter every =
        filter({"completed,re-reading", "dropped", "plan_to_read,on-hold,reading"});
    CHECK(every.names() == std::vector<std::string>{"completed", "re_reading", "dropped",
                                                    "plan_to_read", "on_hold", "reading"});
}

TEST_CASE("status filter: an unknown word, `all` included, gets the exact exit-2 text",
          "[follows]") {
    const std::string word = GENERATE(as<std::string>{}, "Reading", "READING", "all", "paused",
                                      "on hold", "On-Hold", " reading", "reading ", "on--hold");
    CAPTURE(word);
    check_usage(parse({word}), unknown_message(word));
}

TEST_CASE("status filter: the first bad word is the one reported", "[follows]") {
    check_usage(parse({"reading,all,paused"}), unknown_message("all"));
    check_usage(parse({"reading", "paused", "all"}), unknown_message("paused"));
    check_usage(parse({"reading", "paused,"}), unknown_message("paused"));
    check_usage(parse({"reading,", "paused"}), empty_message());
}

TEST_CASE("status filter: an empty word gets the exact exit-2 text", "[follows]") {
    const std::string value =
        GENERATE(as<std::string>{}, "", ",", "reading,", ",reading", "reading,,on_hold", ",,");
    CAPTURE(value);
    check_usage(parse({value}), empty_message());
    check_usage(parse({"reading", value}), empty_message());
}

TEST_CASE("status filter: no words at all is the empty-word error", "[follows]") {
    check_usage(md::StatusFilter::parse({}), empty_message());
}

TEST_CASE("status filter: a long unknown word is echoed cut to 32 bytes", "[follows]") {
    const std::string word = std::string(32, 'a') + "MARKER-after-the-cut";
    check_usage(parse({word}), unknown_message(std::string(32, 'a')));

    const std::string exactly = std::string(32, 'b');
    check_usage(parse({exactly}), unknown_message(exactly));
}

TEST_CASE("status filter: the cut never splits a UTF-8 sequence", "[follows]") {
    // "é" (2 bytes) straddling byte 32: the cut backs off to 31 bytes.
    const std::string straddle = std::string(31, 'a') + "\xC3\xA9" + "tail";
    check_usage(parse({straddle}), unknown_message(std::string(31, 'a')));
    // A 4-byte sequence starting at byte 30: the cut backs off to 30.
    const std::string four = std::string(30, 'a') + "\xF0\x9F\x98\x80" + "tail";
    check_usage(parse({four}), unknown_message(std::string(30, 'a')));
    // Ending exactly on a boundary: kept whole up to 32.
    const std::string fits = std::string(30, 'a') + "\xC3\xA9" + "tail";
    check_usage(parse({fits}), unknown_message(std::string(30, 'a') + "\xC3\xA9"));
}

TEST_CASE("status filter: invalid UTF-8 and control bytes are passed on raw for the app",
          "[follows]") {
    // The core never prints; the app's terminal_safe sanitizes the message (spec A1). Here the
    // bytes only have to survive, bounded, and never crash.
    const std::string invalid = "\xFF\xFE";
    check_usage(parse({invalid}), unknown_message(invalid));
    const std::string escape = "\x1b[31mred";
    check_usage(parse({escape}), unknown_message(escape));
    // A run of stray continuation bytes past the limit is cut at 32 bytes.
    const std::string continuation(40, '\x80');
    check_usage(parse({continuation}), unknown_message(std::string(32, '\x80')));
}

// ---- filter_follows (AC52) ------------------------------------------------------------------

TEST_CASE("status filter: reading keeps only the followed title read", "[follows]") {
    const std::vector<md::MangaId> follows = fs1_follows();
    const md::FilteredFollows kept = md::filter_follows(follows, fs1_map(), filter({"reading"}));
    CHECK(kept.titles == std::vector{manga(id_1)});
    CHECK(kept.follows == 4);
    CHECK(kept.unknown == std::map<std::string, std::size_t, std::less<>>{{"paused", 1}});
}

TEST_CASE("status filter: the follows order is kept whatever the word order", "[follows]") {
    const std::string words = GENERATE(as<std::string>{}, "reading,completed", "completed,reading",
                                       "completed,re-reading,reading");
    CAPTURE(words);
    const std::vector<md::MangaId> follows = fs1_follows();
    const md::FilteredFollows kept = md::filter_follows(follows, fs1_map(), filter({words}));
    CHECK(kept.titles == std::vector{manga(id_6), manga(id_1)});
    CHECK(kept.follows == 4);
    CHECK(kept.unknown == std::map<std::string, std::size_t, std::less<>>{{"paused", 1}});
}

TEST_CASE("status filter: no status, an unknown status or not followed never match", "[follows]") {
    const md::StatusFilter every =
        filter({"reading,on_hold,plan_to_read,dropped,re_reading,completed"});
    const std::vector<md::MangaId> follows = fs1_follows();
    const md::FilteredFollows kept = md::filter_follows(follows, fs1_map(), every);
    // 8888 (no status), 7777 (paused) and 9999 (not followed) are absent.
    CHECK(kept.titles == std::vector{manga(id_6), manga(id_1)});
    CHECK(kept.follows == 4);
}

TEST_CASE("status filter: nothing matching gives an empty selection with the counts", "[follows]") {
    const std::vector<md::MangaId> follows = fs1_follows();
    const md::FilteredFollows kept = md::filter_follows(follows, fs1_map(), filter({"dropped"}));
    CHECK(kept.titles.empty());
    CHECK(kept.follows == 4);
    CHECK(kept.unknown == std::map<std::string, std::size_t, std::less<>>{{"paused", 1}});

    const md::FilteredFollows none = md::filter_follows({}, fs1_map(), filter({"reading"}));
    CHECK(none == md::FilteredFollows{});

    const md::FilteredFollows empty_map =
        md::filter_follows(follows, md::StatusMap{}, filter({"reading"}));
    CHECK(empty_map.titles.empty());
    CHECK(empty_map.follows == 4);
    CHECK(empty_map.unknown.empty());
}

TEST_CASE("status filter: duplicate follows survive and each counts", "[follows]") {
    const std::vector<md::MangaId> follows{manga(id_1), manga(id_7), manga(id_6), manga(id_1),
                                           manga(id_7)};
    const md::FilteredFollows kept =
        md::filter_follows(follows, fs1_map(), filter({"reading", "completed"}));
    CHECK(kept.titles == std::vector{manga(id_1), manga(id_6), manga(id_1)});
    CHECK(kept.follows == 5);
    CHECK(kept.unknown == std::map<std::string, std::size_t, std::less<>>{{"paused", 2}});
}

TEST_CASE("status filter: unknown values are counted per value", "[follows]") {
    const std::vector<md::MangaId> follows{manga(id_1), manga(id_6), manga(id_7), manga(id_8)};
    const md::StatusMap map{.statuses = {{manga(id_1), "paused"},
                                         {manga(id_6), "abandoned"},
                                         {manga(id_7), "paused"},
                                         {manga(id_8), "reading"}}};
    const md::FilteredFollows kept = md::filter_follows(follows, map, filter({"reading"}));
    CHECK(kept.titles == std::vector{manga(id_8)});
    CHECK(kept.unknown ==
          std::map<std::string, std::size_t, std::less<>>{{"abandoned", 1}, {"paused", 2}});
}

TEST_CASE("status filter: a malformed value in a hand-built map matches nothing and is not noted",
          "[follows]") {
    // parse_status_map never yields these; filter_follows still keeps the -v note well-formed.
    const std::vector<md::MangaId> follows{manga(id_1), manga(id_6)};
    const md::StatusMap map{
        .statuses = {{manga(id_1), "\x1b[31m"}, {manga(id_6), std::string(64, 'z')}}};
    const md::FilteredFollows kept = md::filter_follows(follows, map, filter({"reading"}));
    CHECK(kept.titles.empty());
    CHECK(kept.follows == 2);
    CHECK(kept.unknown.empty());
}

} // namespace
