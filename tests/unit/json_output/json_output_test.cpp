// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/json_output/json_output_test.cpp: DEL and C1 escaping of the stdout --json documents
// (json-c1 plan). escape_del_and_c1 byte by byte: the empty text, text that must stay identical
// (CJK, emoji, U+FFFD, and the look-alikes C4 9B / E2 80 9B / C4 80 / C2 A0..BF), DEL and every
// C1 control, adjacent and mixed escapes, a C2 at the end; then dump_document: equal to the plain
// dump plus '\n' when there is nothing to escape, every scalar value U+0000..U+FFFF (plus three
// astral ones) against the raw dump with a strict round trip, keys at depth, and the replace
// handler's output for invalid UTF-8.
#include "json_output.hpp"
#include "support/json_bytes.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace jo = mangadex_dl::json_output;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view replacement = "\xEF\xBF\xBD"; // U+FFFD

/// The UTF-8 encoding of the scalar value `cp` (never a surrogate, at most U+10FFFF).
[[nodiscard]] std::string utf8(std::uint32_t cp) {
    std::string out{};
    const auto put = [&out](std::uint32_t byte) { out += static_cast<char>(byte); };
    if (cp < 0x80U) {
        put(cp);
    } else if (cp < 0x800U) {
        put(0xC0U | (cp >> 6U));
        put(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
        put(0xE0U | (cp >> 12U));
        put(0x80U | ((cp >> 6U) & 0x3FU));
        put(0x80U | (cp & 0x3FU));
    } else {
        put(0xF0U | (cp >> 18U));
        put(0x80U | ((cp >> 12U) & 0x3FU));
        put(0x80U | ((cp >> 6U) & 0x3FU));
        put(0x80U | (cp & 0x3FU));
    }
    return out;
}

/// The plain dump of `doc` as the producers did it before json-c1.
[[nodiscard]] std::string raw_dump(const OrderedJson& doc) {
    return doc.dump(2, ' ', false, OrderedJson::error_handler_t::replace);
}

/// Strict parse (no replacement): invalid UTF-8 in `text` fails the test.
[[nodiscard]] OrderedJson parse(const std::string& text) {
    OrderedJson doc = OrderedJson::parse(text, nullptr, false);
    REQUIRE(!doc.is_discarded());
    return doc;
}

[[nodiscard]] bool is_del_or_c1(std::uint32_t cp) {
    return cp == 0x7FU || (cp >= 0x80U && cp <= 0x9FU);
}

} // namespace

TEST_CASE("escape_del_and_c1: the empty text stays empty", "[json][json_output]") {
    CHECK(jo::escape_del_and_c1("").empty());
}

TEST_CASE("escape_del_and_c1: text without DEL or C1 is byte-identical", "[json][json_output]") {
    const std::vector<std::string> inputs = {
        R"({"a": [1, true, null, "x\u001b y"]})",
        "\"\xE9\x80\xB2\xE6\x92\x83\xE3\x81\xAE\xE5\xB7\xA8\xE4\xBA\xBA\"", // 進撃の巨人
        "\"\xF0\x9F\x98\x80\"",                                             // U+1F600
        std::string{"\""} + std::string{replacement} + "\"",                // U+FFFD
        "\"\xC4\x9B\"",     // U+011B: 9B as a continuation byte, not a C1 control
        "\"\xE2\x80\x9B\"", // U+201B: 80 and 9B as continuation bytes
        "\"\xC4\x80\"",     // U+0100
        "\"\xC2\xA0\"",     // U+00A0, the first C2 pair after the C1 range
        "\"\xC2\xAD\"",     // U+00AD
        "\"\xC2\xBF\"",     // U+00BF, the last C2 pair
    };
    for (const std::string& input : inputs) {
        INFO("input: " << input);
        CHECK(jo::escape_del_and_c1(input) == input);
    }
}

TEST_CASE("escape_del_and_c1: DEL and every C1 control become lowercase \\u00xx",
          "[json][json_output]") {
    CHECK(jo::escape_del_and_c1("\"a\x7f"
                                "b\"") == "\"a\\u007fb\"");
    for (std::uint32_t cp = 0x80; cp <= 0x9F; ++cp) {
        INFO(std::format("U+{:04X}", cp));
        const std::string input = "\"a" + utf8(cp) + "b\"";
        CHECK(jo::escape_del_and_c1(input) == std::format("\"a\\u{:04x}b\"", cp));
    }
}

TEST_CASE("escape_del_and_c1: adjacent and mixed escapes", "[json][json_output]") {
    CHECK(jo::escape_del_and_c1("\x7f\x7f") == "\\u007f\\u007f");
    CHECK(jo::escape_del_and_c1("\xc2\x9b\xc2\x9b") == "\\u009b\\u009b");
    CHECK(jo::escape_del_and_c1("a\x7f\xc2\x85"
                                "b") == "a\\u007f\\u0085b");
}

TEST_CASE("escape_del_and_c1: a C2 that is not a C1 lead byte is copied unchanged",
          "[json][json_output]") {
    // At the very end: the second byte is never read past the end (ASan would not see it: the
    // NUL terminator is valid memory; _GLIBCXX_ASSERTIONS would abort on the string_view index).
    CHECK(jo::escape_del_and_c1("x\xc2") == "x\xc2");
    CHECK(jo::escape_del_and_c1("\xc2") == "\xc2");
    // Followed by a DEL: the C2 stays, the DEL is escaped on its own.
    CHECK(jo::escape_del_and_c1("\xc2\x7f") == "\xc2\\u007f");
}

TEST_CASE("dump_document: the plain dump plus one newline when nothing needs escaping",
          "[json][json_output]") {
    OrderedJson doc = OrderedJson::object();
    doc["c0"] = "\x1b[31m\t\r\n\x07\x01";
    doc["cjk"] = "\xE9\x80\xB2\xE6\x92\x83"; // 進撃
    doc["bad"] = OrderedJson::array({"\xff\xfe", OrderedJson::object({{"deep", "x\xc3"}})});
    doc["n"] = 3;
    const std::string text = jo::dump_document(doc);
    CHECK(text == raw_dump(doc) + "\n");
    CHECK(text.ends_with("}\n"));
    CHECK(!text.ends_with("\n\n"));
    CHECK(!mangadex_dl::test::has_raw_del_or_c1(text));
}

TEST_CASE("dump_document: every scalar value matches the raw dump except DEL and C1",
          "[json][json_output]") {
    std::vector<std::uint32_t> code_points{};
    for (std::uint32_t cp = 0; cp <= 0xFFFFU; ++cp) {
        if (cp < 0xD800U || cp > 0xDFFFU) {
            code_points.push_back(cp);
        }
    }
    code_points.push_back(0x10000U);
    code_points.push_back(0x1F600U);
    code_points.push_back(0x10FFFFU);

    // Collected, then checked once: one assertion per code point would swamp the report.
    std::vector<std::string> wrong{};
    for (const std::uint32_t cp : code_points) {
        const std::string value = utf8(cp);
        const OrderedJson doc = value;
        const std::string text = jo::dump_document(doc);
        const std::string expected =
            is_del_or_c1(cp) ? std::format("\"\\u{:04x}\"\n", cp) : raw_dump(doc) + "\n";
        if (text != expected) {
            wrong.push_back(std::format("U+{:04X}: text", cp));
            continue;
        }
        const OrderedJson back = OrderedJson::parse(text, nullptr, false);
        if (back.is_discarded() || back != doc) {
            wrong.push_back(std::format("U+{:04X}: round trip", cp));
        }
    }
    INFO("checked " << code_points.size() << " code points");
    for (std::size_t i = 0; i < std::min<std::size_t>(wrong.size(), 10); ++i) {
        UNSCOPED_INFO(wrong[i]);
    }
    CHECK(wrong.empty());
}

TEST_CASE("dump_document: DEL and C1 are escaped in keys and values at depth",
          "[json][json_output]") {
    OrderedJson doc = OrderedJson::object();
    doc["k\x7f"] = OrderedJson::object({{"n", OrderedJson::array({"v\xc2\x9b"})}});
    const std::string text = jo::dump_document(doc);
    CHECK(!mangadex_dl::test::has_raw_del_or_c1(text));
    CHECK(text.contains("\"k\\u007f\": {"));
    CHECK(text.contains("\"v\\u009b\""));
    const OrderedJson back = parse(text);
    CHECK(back == doc);
    CHECK(back.at("k\x7f").at("n").at(0) == "v\xc2\x9b");
}

TEST_CASE("dump_document: invalid UTF-8 never turns into a raw DEL or C1 byte",
          "[json][json_output]") {
    struct Case {
        std::string input{};
        std::string expected{}; // the document text without the quotes and the newline
    };
    const std::string fffd{replacement};
    const std::vector<Case> cases = {
        // C1 is never a valid lead byte, and 9B on its own is a stray continuation byte.
        {.input = "\xc1\x9b", .expected = fffd + fffd},
        // The first C2 has no continuation; the second C2 9B is a real U+009B.
        {.input = "\xc2\xc2\x9b", .expected = fffd + "\\u009b"},
        {.input = "\xc2\x7f", .expected = fffd + "\\u007f"},
        {.input = "\x9b", .expected = fffd},
    };
    for (const Case& c : cases) {
        INFO("expected: " << c.expected);
        const std::string text = jo::dump_document(OrderedJson(c.input));
        CHECK(text == "\"" + c.expected + "\"\n");
        CHECK(!mangadex_dl::test::has_raw_del_or_c1(text));
        CHECK(parse(text).is_string());
    }
}
