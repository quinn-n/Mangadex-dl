// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch_csv/batch_csv_test.cpp: parse_title_list (batch plan T2): the spec's "CSV
// format" accept set (AC1), reject set with exact messages and start lines (AC2), the limits
// (AC3), the echo cap and line numbering. Cell *values* are validated later by plan_batch (T3a),
// so a garbage `manga` cell in header mode is accepted here.
#include "mangadex_dl/batch_csv.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using mangadex_dl::ListErrors;
using mangadex_dl::ListRecord;
using mangadex_dl::max_list_errors;
using mangadex_dl::max_list_field_bytes;
using mangadex_dl::max_list_titles;
using mangadex_dl::parse_title_list;
using mangadex_dl::TitleList;
using std::string_view_literals::operator""sv;

constexpr std::string_view id1 = "11111111-1111-1111-1111-111111111111";
constexpr std::string_view id2 = "22222222-2222-2222-2222-222222222222";
constexpr std::string_view known =
    "(known: manga, title, dir, volumes, loose-chapters, layout, lang, group, direction, note)";
constexpr std::string_view unterminated =
    "unterminated quote; the field that opens with '\"' on this line never closes";
constexpr std::string_view after_quote = "unexpected text after a closing quote; quote the whole "
                                         "field and write '\"\"' for a quote";
constexpr std::string_view bare_cr =
    "a carriage return (CR) outside quotes; save the file with LF or CRLF line endings";

/// "<label>:<line>: <what>" for label list.csv.
[[nodiscard]] std::string at(std::size_t line, std::string_view what) {
    return std::format("list.csv:{}: {}", line, what);
}

/// The message for a would-be header whose only non-empty field `echoed` is no known column.
[[nodiscard]] std::string lone_word(std::size_t line, std::string_view echoed) {
    return at(line, std::format("'{}' is not a MangaDex title UUID or "
                                "https://mangadex.org/title/<uuid> URL, nor a known column "
                                "(manga, title, dir, volumes, loose-chapters, layout, lang, "
                                "group, direction, note)",
                                echoed));
}

/// parse_title_list(csv, "list.csv"), which must succeed.
[[nodiscard]] TitleList parsed(std::string_view csv) {
    auto result = parse_title_list(csv, "list.csv");
    if (!result) {
        for (const std::string& message : result.error().messages) {
            UNSCOPED_INFO(message);
        }
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// parse_title_list(csv, "list.csv"), which must fail.
[[nodiscard]] ListErrors failed(std::string_view csv) {
    auto result = parse_title_list(csv, "list.csv");
    REQUIRE_FALSE(result.has_value());
    return std::move(result).error();
}

/// The single diagnostic of a failing parse.
[[nodiscard]] std::string error_of(std::string_view csv) {
    const ListErrors errors = failed(csv);
    REQUIRE(errors.messages.size() == 1);
    CHECK(errors.more == 0);
    return errors.messages.front();
}

/// parse_title_list over an exact-size heap copy of `csv` (std::vector's iterator-range
/// constructor allocates exactly csv.size() bytes), so ASan reports any read past the last byte.
[[nodiscard]] mangadex_dl::ListResult<TitleList> parse_exact(std::string_view csv) {
    const std::vector<char> buffer(csv.begin(), csv.end());
    return parse_title_list(std::string_view{buffer.data(), buffer.size()}, "list.csv");
}

[[nodiscard]] ListRecord record(std::size_t line, std::string_view manga) {
    return ListRecord{.line = line, .manga = std::string{manga}};
}

} // namespace

// ---- AC1: the accept set --------------------------------------------------------------------

TEST_CASE("parse_title_list: a headerless list with one id per line", "[batch_csv]") {
    const TitleList list = parsed(std::format("{}\n{}\n", id1, id2));
    CHECK_FALSE(list.has_header);
    CHECK(list.records == std::vector{record(1, id1), record(2, id2)});
}

TEST_CASE("parse_title_list: a UTF-8 BOM is skipped and does not count as a line", "[batch_csv]") {
    const TitleList list = parsed(std::format("\xEF\xBB\xBF{}\n{}", id1, id2));
    CHECK_FALSE(list.has_header);
    CHECK(list.records == std::vector{record(1, id1), record(2, id2)});

    const TitleList with_header = parsed(std::format("\xEF\xBB\xBFmanga,title\r\n{},T\r\n", id1));
    CHECK(with_header.has_header);
    REQUIRE(with_header.records.size() == 1);
    CHECK(with_header.records[0].line == 2);
}

TEST_CASE("parse_title_list: LF, CRLF and mixed line endings; no final newline", "[batch_csv]") {
    const TitleList list =
        parsed(std::format("manga,title\r\n{},One\n{},Two\r\n{},Three", id1, id2, id1));
    CHECK(list.has_header);
    REQUIRE(list.records.size() == 3);
    CHECK(list.records[0].line == 2);
    CHECK(list.records[0].title == "One");
    CHECK(list.records[1].line == 3);
    CHECK(list.records[1].title == "Two");
    CHECK(list.records[2].line == 4);
    CHECK(list.records[2].title == "Three");
}

TEST_CASE("parse_title_list: RFC 4180 quoting: ',', '\"\"', CR and LF inside quotes",
          "[batch_csv]") {
    const TitleList list = parsed(
        std::format("manga,title\n{},\"a, \"\"b\"\"\r\nc\rd\"\n{},\"\"\"quoted\"\"\"\n", id1, id2));
    REQUIRE(list.records.size() == 2);
    CHECK(list.records[0].line == 2);
    CHECK(list.records[0].title == "a, \"b\"\r\nc\rd");
    CHECK(list.records[1].line == 4); // the quoted LF advanced the line count
    CHECK(list.records[1].title == "\"quoted\"");
}

TEST_CASE("parse_title_list: fields are trimmed inside and outside quotes", "[batch_csv]") {
    const TitleList list = parsed(
        std::format(" Manga ,\ttitle\t, dir\n  {}  ,  \"  spaced out  \"  ,\t\" a \"\t\n", id1));
    REQUIRE(list.records.size() == 1);
    CHECK(list.records[0].manga == id1);
    CHECK(list.records[0].title == "spaced out");
    CHECK(list.records[0].dir == "a");

    const TitleList inner = parsed(std::format("manga,title\n{},  two  words  \n", id1));
    REQUIRE(inner.records.size() == 1);
    CHECK(inner.records[0].title == "two  words");
}

TEST_CASE("parse_title_list: comments, blank and whitespace-only lines are skipped",
          "[batch_csv]") {
    const TitleList list = parsed(std::format("# my list, with \"quotes\" and a stray \r here\n"
                                              "\n"
                                              "manga,title\n"
                                              "   \n"
                                              "  # an indented comment\n"
                                              "\t\n"
                                              ",,\n"
                                              " , \"\" ,\n"
                                              "{},One\n"
                                              "\"#not a comment\",Two\n"
                                              "#last",
                                              id1));
    CHECK(list.has_header);
    REQUIRE(list.records.size() == 2);
    CHECK(list.records[0].line == 9);
    CHECK(list.records[0].manga == id1);
    CHECK(list.records[1].line == 10);
    CHECK(list.records[1].manga == "#not a comment"); // quoted: a cell, not a comment
    CHECK(list.records[1].title == "Two");
}

TEST_CASE("parse_title_list: a headerless list ignores empty extra fields", "[batch_csv]") {
    const TitleList list = parsed(std::format("{},,\n{}, ,\t\n", id1, id2));
    CHECK_FALSE(list.has_header);
    CHECK(list.records == std::vector{record(1, id1), record(2, id2)});
}

TEST_CASE("parse_title_list: a MangaDex URL in field 1 means there is no header", "[batch_csv]") {
    const std::string url = std::format("https://mangadex.org/title/{}/some-title", id1);
    const TitleList list = parsed(std::format("{}\n{}\n", url, id2));
    CHECK_FALSE(list.has_header);
    CHECK(list.records == std::vector{record(1, url), record(2, id2)});
}

TEST_CASE("parse_title_list: header columns in any order and any ASCII case", "[batch_csv]") {
    const TitleList list =
        parsed(std::format("Note,DIRECTION,Group,lang,LAYOUT,Loose-Chapters,volumes,Dir,TITLE,"
                           "Manga\nskip me,rtl,A|B,en,chapters,none,1-3,sub/x,My Title,{}\n",
                           id1));
    CHECK(list.has_header);
    REQUIRE(list.records.size() == 1);
    const ListRecord expected{
        .line = 2,
        .manga = std::string{id1},
        .title = "My Title",
        .dir = "sub/x",
        .volumes = "1-3",
        .loose_chapters = "none",
        .layout = "chapters",
        .lang = "en",
        .group = "A|B",
        .direction = "rtl",
    };
    CHECK(list.records[0] == expected); // `note` is dropped
}

TEST_CASE("parse_title_list: ragged short rows and empty cells are absent", "[batch_csv]") {
    const TitleList list =
        parsed(std::format("manga,title,dir,lang\n{},T\n{},,,\n{}\n", id1, id2, id1));
    REQUIRE(list.records.size() == 3);
    CHECK(list.records[0] == ListRecord{.line = 2, .manga = std::string{id1}, .title = "T"});
    CHECK(list.records[1] == record(3, id2));
    CHECK(list.records[2] == record(4, id1));
}

TEST_CASE("parse_title_list: a quoted field at the very end of the input", "[batch_csv]") {
    SECTION("closing quote as the last byte") {
        const auto list = parse_exact(std::format("manga,title\n{},\"T\"", id1));
        REQUIRE(list.has_value());
        REQUIRE(list->records.size() == 1);
        CHECK(list->records[0].title == "T");
    }
    SECTION("blanks after the closing quote, then the end") {
        const auto list = parse_exact(std::format("manga,title\n{},\"T\"  \t", id1));
        REQUIRE(list.has_value());
        REQUIRE(list->records.size() == 1);
        CHECK(list->records[0].title == "T");
    }
    SECTION("an escaped quote as the last two bytes") {
        const auto list = parse_exact(std::format("manga,title\n{},\"T\"\"", id1));
        REQUIRE_FALSE(list.has_value());
        CHECK(list.error().messages == std::vector<std::string>{at(2, unterminated)});
    }
    SECTION("a lone opening quote as the last byte") {
        const auto list = parse_exact(std::format("manga,title\n{},\"", id1));
        REQUIRE_FALSE(list.has_value());
        CHECK(list.error().messages == std::vector<std::string>{at(2, unterminated)});
    }
    SECTION("a headerless record ending in ',' at the end") {
        const auto list = parse_exact(std::format("{},", id1));
        REQUIRE(list.has_value());
        CHECK(list->records == std::vector{record(1, id1)});
    }
}

TEST_CASE("parse_title_list: a long row is fine when the extra fields are empty", "[batch_csv]") {
    const TitleList list = parsed(std::format("manga,title\n{},T,,\t, \"\"\n", id1));
    REQUIRE(list.records.size() == 1);
    CHECK(list.records[0].title == "T");
}

TEST_CASE("parse_title_list: empty inputs", "[batch_csv]") {
    CHECK(parsed("") == TitleList{});
    CHECK(parsed("\xEF\xBB\xBF") == TitleList{});
    CHECK(parsed("\n\n  \n# only comments\n") == TitleList{});
    CHECK(parsed("manga,title\n") == TitleList{.has_header = true});
}

TEST_CASE("parse_title_list: nothing in the result borrows from the input or the label",
          "[batch_csv]") {
    TitleList list{};
    std::vector<std::string> messages{};
    {
        const auto csv = std::make_unique<std::string>(std::format("manga,title\n{},\"T\"\n", id1));
        const auto label = std::make_unique<std::string>("a-heap-allocated-label.csv");
        auto ok = parse_title_list(*csv, *label);
        REQUIRE(ok.has_value());
        list = std::move(*ok);
        auto bad = parse_title_list(std::format("manga,title\n{},T,x\n", id1), *label);
        REQUIRE_FALSE(bad.has_value());
        messages = std::move(bad).error().messages;
    } // both buffers freed: a stored view would now be a heap-use-after-free under ASan
    REQUIRE(list.records.size() == 1);
    CHECK(list.records[0].manga == id1);
    CHECK(list.records[0].title == "T");
    CHECK(messages == std::vector<std::string>{"a-heap-allocated-label.csv:2: 3 fields but the "
                                               "header has 2"});
}

// ---- AC2: the reject set --------------------------------------------------------------------

TEST_CASE("parse_title_list: an unterminated quote names the record's start line", "[batch_csv]") {
    CHECK(error_of(std::format("manga,title\n{},ok\n{},\"never\nclosed\n", id1, id2)) ==
          at(3, unterminated));
}

TEST_CASE("parse_title_list: a quote inside an unquoted field", "[batch_csv]") {
    CHECK(error_of(std::format("manga,title\n{},ab\"c\"\n", id1)) ==
          "list.csv:2: a '\"' inside an unquoted field; quote the whole field and write '\"\"' "
          "for a quote");
}

TEST_CASE("parse_title_list: text after a closing quote", "[batch_csv]") {
    CHECK(error_of(std::format("manga,title\n{},\"a\"b\n", id1)) == at(2, after_quote));
    // Reported at the start line of a record whose quoted field spans lines.
    CHECK(error_of(std::format("manga,title\n{},\"a\nb\" x\n", id1)) == at(2, after_quote));
}

TEST_CASE("parse_title_list: a bare CR outside quotes", "[batch_csv]") {
    CHECK(error_of(std::format("manga\r{}\n", id1)) == at(1, bare_cr));
    CHECK(error_of(std::format("manga\n{}\rx\n", id1)) == at(2, bare_cr));
    CHECK(error_of(std::format("manga\n{}\r", id1)) == at(2, bare_cr));
}

TEST_CASE("parse_title_list: invalid UTF-8 names the line of the first bad byte", "[batch_csv]") {
    CHECK(error_of(std::format("manga,title\n{},\"multi\nline \xFF\"\n", id1)) ==
          "list.csv:3: not valid UTF-8");
    CHECK(error_of("\xFF\n") == "list.csv:1: not valid UTF-8");
    CHECK(error_of("manga\n\xC3") == "list.csv:2: not valid UTF-8");         // truncated sequence
    CHECK(error_of("manga\n\xC0\xAF") == "list.csv:2: not valid UTF-8");     // overlong '/'
    CHECK(error_of("manga\n\xED\xA0\x80") == "list.csv:2: not valid UTF-8"); // a surrogate
    // Even inside a comment: the whole input is checked before lexing.
    CHECK(error_of("# \xFE\nmanga\n") == "list.csv:1: not valid UTF-8");
}

TEST_CASE("parse_title_list: a NUL byte is not valid UTF-8", "[batch_csv]") {
    const std::string csv = std::string{"manga\n"} + std::string{id1} + '\0' + "\n";
    CHECK(error_of(csv) == "list.csv:2: not valid UTF-8");
}

TEST_CASE("parse_title_list: a UTF-16 BOM", "[batch_csv]") {
    CHECK(error_of("\xFF\xFEm\0a\0"sv) == "list.csv: is UTF-16; save it as UTF-8");
    CHECK(error_of("\xFE\xFF\0m\0a"sv) == "list.csv: is UTF-16; save it as UTF-8");
}

TEST_CASE("parse_title_list: unknown, empty and duplicate header names", "[batch_csv]") {
    CHECK(error_of("manga,titel\n") == std::format("list.csv:1: unknown column 'titel' {}", known));
    CHECK(error_of("manga,,title\n") == std::format("list.csv:1: unknown column '' {}", known));
    CHECK(error_of("manga,Title,TITLE\n") ==
          std::format("list.csv:1: duplicate column 'TITLE' {}", known));
    // Several bad names: one message each, on the header's line (after a comment here).
    const ListErrors errors = failed(std::format("# c\nmanga,x,y\n{},1,2\n", id1));
    CHECK(errors.messages == std::vector<std::string>{
                                 std::format("list.csv:2: unknown column 'x' {}", known),
                                 std::format("list.csv:2: unknown column 'y' {}", known),
                             });
}

TEST_CASE("parse_title_list: a lone unknown word on the first line may be a title or a header",
          "[batch_csv]") {
    // A chapter URL (not a title URL) as the first line of a headerless list.
    const std::string chapter = "https://mangadex.org/chapter/0a1b2c3d-0000-4000-8000-0000000000aa";
    CHECK(error_of(std::format("{}\n{}\n", chapter, id1)) ==
          lone_word(1, "https://mangadex.org/chapter/0a1..."));
    CHECK(error_of("manga_id\n") == lone_word(1, "manga_id"));
    // Empty fields around it do not count; the line is the record's own (after a comment).
    CHECK(error_of("# titles\n,titel,\n") == lone_word(2, "titel"));
    // The echo is capped at 32 bytes on a UTF-8 boundary, as for header names.
    const std::string long_word = std::string(31, 'a') + "\xC3\xA9" + std::string(7, 'b');
    CHECK(error_of(long_word + "\n") == lone_word(1, std::string(31, 'a') + "..."));

    SECTION("a known column alone is still a header") {
        CHECK(error_of("title\n") == "list.csv:1: the header has no 'manga' column");
        CHECK(parsed(std::format("Manga\n{}\n", id1)).has_header);
    }
    SECTION("two non-empty fields keep the per-name header messages") {
        CHECK(error_of("manga,titel\n") ==
              std::format("list.csv:1: unknown column 'titel' {}", known));
    }
}

TEST_CASE("parse_title_list: a header without a manga column", "[batch_csv]") {
    CHECK(error_of(std::format("title,dir\nT,{}\n", id1)) ==
          "list.csv:1: the header has no 'manga' column");
}

TEST_CASE("parse_title_list: a ';'-separated header gets the separator hint", "[batch_csv]") {
    CHECK(error_of("manga;title;dir\n") ==
          std::format("list.csv:1: unknown column 'manga;title;dir' {}; the separator is ','",
                      known));
    // With a ',' present the hint does not apply.
    CHECK(error_of("manga,title;dir\n") ==
          std::format("list.csv:1: unknown column 'title;dir' {}", known));
}

TEST_CASE("parse_title_list: an echoed header name is capped at 32 bytes on a UTF-8 boundary",
          "[batch_csv]") {
    // 31 ASCII bytes, then U+00E9 (2 bytes) across the 32-byte mark, then 7 more: 40 bytes.
    const std::string name = std::string(31, 'a') + "\xC3\xA9" + std::string(7, 'b');
    REQUIRE(name.size() == 40);
    CHECK(error_of(std::format("manga,{}\n", name)) ==
          std::format("list.csv:1: unknown column '{}...' {}", std::string(31, 'a'), known));

    // Exactly 32 bytes is echoed whole.
    const std::string fits(32, 'c');
    CHECK(error_of(std::format("manga,{}\n", fits)) ==
          std::format("list.csv:1: unknown column '{}' {}", fits, known));
}

TEST_CASE("parse_title_list: a non-empty extra field", "[batch_csv]") {
    CHECK(error_of(std::format("manga,title\n{},T,extra\n", id1)) ==
          "list.csv:2: 3 fields but the header has 2");
    CHECK(error_of(std::format("manga,title\n{},T,,x,\n", id1)) ==
          "list.csv:2: 5 fields but the header has 2");
}

TEST_CASE("parse_title_list: a headerless list with two columns", "[batch_csv]") {
    CHECK(error_of(std::format("{},My Title\n", id1)) ==
          "list.csv:1: more than one column; add a header row (manga,title,...)");
    CHECK(error_of(std::format("{}\n{},,x\n", id1, id2)) ==
          "list.csv:2: more than one column; add a header row (manga,title,...)");
}

TEST_CASE("parse_title_list: a record without a manga cell", "[batch_csv]") {
    CHECK(error_of("title,manga\nOnly a title,\n") == "list.csv:2: manga: the title is missing");
    CHECK(error_of("manga,note\n,just a note\n") == "list.csv:2: manga: the title is missing");
}

TEST_CASE("parse_title_list: the manga cell is never echoed", "[batch_csv]") {
    // Header mode accepts any manga text here (plan_batch validates it), and no message quotes it.
    const TitleList list = parsed("manga\nnot-an-id-SECRET\n");
    REQUIRE(list.records.size() == 1);
    const ListErrors errors = failed("manga,title\nnot-an-id-SECRET,T,extra\n\"SECRET\",T\"x\n");
    REQUIRE(errors.messages.size() == 2);
    for (const std::string& message : errors.messages) {
        CHECK_FALSE(message.contains("SECRET"));
    }
}

TEST_CASE("parse_title_list: record errors accumulate; a lexical error ends the scan",
          "[batch_csv]") {
    const ListErrors errors =
        failed(std::format("manga,title\n{},a,b\n,T\n{},ok\n{},\"oops\n{}\n", id1, id2, id1, id2));
    CHECK(errors.messages == std::vector<std::string>{
                                 "list.csv:2: 3 fields but the header has 2",
                                 "list.csv:3: manga: the title is missing",
                                 at(5, unterminated),
                             });
    CHECK(errors.more == 0);
}

TEST_CASE("parse_title_list: header errors stop before the records are read", "[batch_csv]") {
    const ListErrors errors = failed(std::format("manga,bogus\n{},a,b,c\n,T\n", id1));
    CHECK(errors.messages ==
          std::vector<std::string>{std::format("list.csv:1: unknown column 'bogus' {}", known)});
}

TEST_CASE("parse_title_list: the label is copied into messages as given", "[batch_csv]") {
    const auto result = parse_title_list(std::format("{},x\n", id1), "<stdin>");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().messages ==
          std::vector<std::string>{"<stdin>:1: more than one column; add a header row "
                                   "(manga,title,...)"});
}

// ---- AC3: limits ----------------------------------------------------------------------------

TEST_CASE("parse_title_list: a field longer than 4096 bytes", "[batch_csv]") {
    const std::string fits(max_list_field_bytes, 't');
    const TitleList list = parsed(std::format("manga,title\n{},{}\n", id1, fits));
    REQUIRE(list.records.size() == 1);
    CHECK(list.records[0].title == fits);

    const std::string too_long(max_list_field_bytes + 1, 't');
    CHECK(error_of(std::format("manga,title\n{},{}\n", id1, too_long)) ==
          "list.csv:2: a field is longer than 4096 bytes");
    // Measured after unquoting and trimming.
    CHECK(parsed(std::format("manga,title\n{},  \"{}\"  \n", id1, fits)).records.size() == 1);
    CHECK(error_of(std::format("{}\n{}\n", id1, std::string(max_list_field_bytes + 1, 'x'))) ==
          "list.csv:2: a field is longer than 4096 bytes");
}

TEST_CASE("parse_title_list: at most 10000 titles", "[batch_csv]") {
    std::string csv = "manga\n";
    for (std::size_t i = 0; i < max_list_titles; ++i) {
        csv += std::format("{}\n", id1);
    }
    const TitleList list = parsed(csv);
    CHECK(list.records.size() == max_list_titles);
    CHECK(list.records.back().line == max_list_titles + 1);

    csv += std::format("{}\n", id2);
    CHECK(error_of(csv) == "list.csv: lists more than 10000 titles");
}

TEST_CASE("parse_title_list: comments and blank lines do not count towards the title limit",
          "[batch_csv]") {
    std::string csv{};
    for (std::size_t i = 0; i < max_list_titles; ++i) {
        csv += std::format("# {}\n\n{}\n", i, id1);
    }
    CHECK(parsed(csv).records.size() == max_list_titles);
}

TEST_CASE("parse_title_list: at most 20 messages, then a count of the rest", "[batch_csv]") {
    std::string csv = "manga,title\n";
    for (int i = 0; i < 25; ++i) {
        csv += std::format("{},T,extra\n", id1);
    }
    const ListErrors errors = failed(csv);
    REQUIRE(errors.messages.size() == max_list_errors);
    CHECK(errors.more == 5);
    CHECK(errors.messages.front() == "list.csv:2: 3 fields but the header has 2");
    CHECK(errors.messages.back() == "list.csv:21: 3 fields but the header has 2");
}
