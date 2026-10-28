// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/title_dir/title_dir_test.cpp: `mdlist --dir-per-title` directory names (list plan
// T5). TitleDirRegistry's "first claim wins" contract (AC37), the registry owning its keys, and
// the property half of AC36 / fact-check V12: every sanitize_file_stem output, plain or with the
// registry's ` [<8 hex>]` / ` [<uuid>]` suffix, is one path component that validate_list_dir
// accepts unchanged.
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/filename.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/title_dir.hpp"

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;

constexpr std::string_view id_a = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view id_b = "77777777-7777-4777-8777-777777777777";
constexpr std::string_view id_c = "77777777-0000-4000-8000-000000000001"; ///< b's 8-hex prefix
constexpr std::string_view id_d = "99999999-9999-4999-8999-999999999999";

/// The registry's longest name: a 150-byte stem plus " [" + 36 + "]".
constexpr std::size_t max_name_bytes = 150 + 39;

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto parsed = md::MangaId::parse(text);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

/// UTF-8 for one code point. Bidi controls and separators are built at run time: GCC's
/// -Wbidi-chars=any and clang-tidy's misc-misleading-bidirectional reject them in literals.
[[nodiscard]] std::string utf8(char32_t code_point) {
    const auto value = static_cast<std::uint32_t>(code_point);
    std::string out{};
    const auto put = [&out](std::uint32_t byte) { out += static_cast<char>(byte & 0xFFU); };
    if (value < 0x80U) {
        put(value);
    } else if (value < 0x800U) {
        put(0xC0U | (value >> 6U));
        put(0x80U | (value & 0x3FU));
    } else if (value < 0x10000U) {
        put(0xE0U | (value >> 12U));
        put(0x80U | ((value >> 6U) & 0x3FU));
        put(0x80U | (value & 0x3FU));
    } else {
        put(0xF0U | (value >> 18U));
        put(0x80U | ((value >> 12U) & 0x3FU));
        put(0x80U | ((value >> 6U) & 0x3FU));
        put(0x80U | (value & 0x3FU));
    }
    return out;
}

/// True when `text` is well-formed UTF-8 that does not end inside a character.
[[nodiscard]] bool ends_on_utf8_boundary(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto lead = static_cast<unsigned char>(text[pos]);
        std::size_t length = 1;
        if (lead >= 0xF0) {
            length = 4;
        } else if (lead >= 0xE0) {
            length = 3;
        } else if (lead >= 0xC0) {
            length = 2;
        } else if (lead >= 0x80) {
            return false; // a stray continuation byte
        }
        if (text.size() - pos < length) {
            return false;
        }
        for (std::size_t i = 1; i < length; ++i) {
            const auto next = static_cast<unsigned char>(text[pos + i]);
            if (next < 0x80 || next > 0xBF) {
                return false;
            }
        }
        pos += length;
    }
    return true;
}

/// One directory name must be usable as a single component below DIR, unchanged.
void check_one_component(const std::string& name) {
    CAPTURE(name);
    CHECK(!name.empty());
    CHECK(!name.contains('/'));
    CHECK(name != ".");
    CHECK(name != "..");
    CHECK(name.size() <= max_name_bytes);
    const md::Result<std::string> validated = md::validate_list_dir(name);
    REQUIRE(validated.has_value());
    CHECK(*validated == name);
}

/// Feeds `stem` through a fresh registry as three different titles: the plain stem, the
/// ` [<8 hex>]` name and the ` [<full uuid>]` name, and checks each one.
void check_stem_and_suffixes(const std::string& title) {
    const std::string stem = md::sanitize_file_stem(title, id_a);
    CAPTURE(title);
    CHECK(stem.size() <= 150);
    CHECK(ends_on_utf8_boundary(stem));
    md::TitleDirRegistry registry{};
    const md::TitleDirClaim plain = registry.claim(stem, manga(id_a), 1);
    const md::TitleDirClaim short_suffix = registry.claim(stem, manga(id_b), 2);
    const md::TitleDirClaim full_suffix = registry.claim(stem, manga(id_c), 3);
    CHECK(plain.name == stem);
    CHECK(short_suffix.name == std::format("{} [77777777]", stem));
    CHECK(full_suffix.name == std::format("{} [{}]", stem, id_c));
    check_one_component(plain.name);
    check_one_component(short_suffix.name);
    check_one_component(full_suffix.name);
}

/// The filename test corpus (tests/unit/filename/filename_test.cpp's sanitizing table) plus the
/// AC36 examples: controls, separators, bidi and invisible characters, invalid UTF-8, trimming
/// and the fallbacks.
[[nodiscard]] std::vector<std::string> corpus() {
    std::vector<std::string> out = {
        "Clean Title",
        "クリーン・タイトル",
        R"(a/b\c<d>e:f"g|h?i*j)",
        "bell" + utf8(0x07) + "del" + utf8(0x7F) + "end",
        "tab\there",
        "c1" + utf8(0x85) + "x" + utf8(0x9B) + "y",
        "  many   spaces  ",
        "nbsp" + utf8(0xA0) + utf8(0x3000) + utf8(0x2003) + "run",
        "abc" + utf8(0x202E) + "def",
        utf8(0x2066) + "iso" + utf8(0x2069) + " " + utf8(0x200E) + "lrm" + utf8(0x200F) + " " +
            utf8(0x061C) + "alm",
        "line" + utf8(0x2028) + "para" + utf8(0x2029) + "end",
        "a " + utf8(0x202A) + utf8(0x202D) + " b",
        std::string{"a\xff"} + "b",
        "overlong\xc0\xaf",
        "surrogate\xed\xa0\x80",
        "big\xf4\x90\x80\x80",
        "cut\xe3\x81",
        "zw" + utf8(0x200B) + utf8(0x200C) + utf8(0x200D) + "j" + utf8(0x2060) + utf8(0x2065) +
            "o" + utf8(0x206A) + utf8(0x206F) + "in" + utf8(0xFEFF),
        "non" + utf8(0xFDD0) + utf8(0xFFFE) + utf8(0x10FFFF) + "char",
        "..hidden..",
        "-rf",
        " . - -name- . ",
        "///",
        "",
        ".",
        "..",
        "...",
        "   ",
        utf8(0x202E),
        utf8(0x202E) + utf8(0x2028) + utf8(0x200F),
        "- . -",
        "A/B: C?",
        "-.-Title . ",
        std::string(400, 'q'),
        std::string(148, 'x') + " .yz",
        "Clean Title [77777777]",
        "x" + std::string(200, '/'),
    };
    std::string cjk{};
    for (int i = 0; i < 100; ++i) {
        cjk += "漢"; // 3 bytes each: 300 bytes
    }
    out.push_back(cjk);
    std::string emoji = "abc";
    for (int i = 0; i < 40; ++i) {
        emoji += utf8(0x1F600);
    }
    out.push_back(emoji);
    return out;
}

/// Code points worth hitting often in random strings: everything sanitize_file_stem drops or
/// replaces, plus ordinary multi-byte text.
constexpr std::array<char32_t, 30> interesting_code_points = {
    0x00,   0x1B,   0x7F,   0x80,   0x9B,   0x9F,   0xA0,    0x061C,   0x200B,  0x200F,
    0x2028, 0x2029, 0x202A, 0x202E, 0x2060, 0x2066, 0x206F,  0xFEFF,   0xFDD0,  0xFFFE,
    0xFFFF, 0x3000, 0x2003, 0x00E9, 0x6F22, 0x3042, 0x1F600, 0x10FFFF, 0x1FFFE, 0xFFFD,
};

/// ASCII bytes that matter to path handling.
constexpr std::string_view interesting_ascii = "/\\<>:\"|?*.- \taZ09[]";

/// A random title: raw bytes (mostly invalid UTF-8), path-relevant ASCII and encoded code points.
[[nodiscard]] std::string random_title(std::mt19937& rng) {
    std::uniform_int_distribution<std::size_t> length_dist{0, 220};
    std::uniform_int_distribution<int> kind_dist{0, 9};
    std::uniform_int_distribution<std::uint32_t> byte_dist{0, 255};
    std::uniform_int_distribution<std::size_t> ascii_dist{0, interesting_ascii.size() - 1};
    std::uniform_int_distribution<std::size_t> cp_dist{0, interesting_code_points.size() - 1};
    const std::size_t length = length_dist(rng);
    std::string out{};
    for (std::size_t i = 0; i < length; ++i) {
        const int kind = kind_dist(rng);
        if (kind < 4) {
            out += static_cast<char>(static_cast<unsigned char>(byte_dist(rng)));
        } else if (kind < 7) {
            out += interesting_ascii[ascii_dist(rng)];
        } else {
            out += utf8(interesting_code_points.at(cp_dist(rng)));
        }
    }
    return out;
}

} // namespace

// ---- AC37: TitleDirRegistry ---------------------------------------------------------------------

TEST_CASE("title dir: a free stem is claimed as is", "[title_dir]") {
    md::TitleDirRegistry registry{};
    CHECK(registry.claim("Clean Title", manga(id_a), 1) ==
          md::TitleDirClaim{.name = "Clean Title"});
    CHECK(registry.claim("Other Title", manga(id_b), 2) ==
          md::TitleDirClaim{.name = "Other Title"});
}

TEST_CASE("title dir: the same manga twice gets the same name, no taken_by", "[title_dir]") {
    md::TitleDirRegistry registry{};
    CHECK(registry.claim("Clean Title", manga(id_a), 1) ==
          md::TitleDirClaim{.name = "Clean Title"});
    CHECK(registry.claim("Clean Title", manga(id_a), 4) ==
          md::TitleDirClaim{.name = "Clean Title"});
    // Even with another stem (a title renamed between items): its earlier name.
    CHECK(registry.claim("Renamed", manga(id_a), 5) == md::TitleDirClaim{.name = "Clean Title"});
    // The other name stays free.
    CHECK(registry.claim("Renamed", manga(id_b), 6) == md::TitleDirClaim{.name = "Renamed"});
}

TEST_CASE("title dir: another manga with the same stem gets the 8-hex suffix", "[title_dir]") {
    md::TitleDirRegistry registry{};
    CHECK(registry.claim("Clean Title", manga(id_a), 1) ==
          md::TitleDirClaim{.name = "Clean Title"});
    CHECK(registry.claim("Clean Title", manga(id_b), 2) ==
          md::TitleDirClaim{.name = "Clean Title [77777777]", .taken_by = 1});
    // The moved-aside manga keeps its suffixed name, without a new warning.
    CHECK(registry.claim("Clean Title", manga(id_b), 3) ==
          md::TitleDirClaim{.name = "Clean Title [77777777]"});
}

TEST_CASE("title dir: a manga sharing the stem and the 8-hex prefix gets the full uuid",
          "[title_dir]") {
    md::TitleDirRegistry registry{};
    CHECK(registry.claim("Clean Title", manga(id_a), 1).name == "Clean Title");
    CHECK(registry.claim("Clean Title", manga(id_b), 2).name == "Clean Title [77777777]");
    CHECK(registry.claim("Clean Title", manga(id_c), 3) ==
          md::TitleDirClaim{.name = std::format("Clean Title [{}]", id_c), .taken_by = 1});
    CHECK(registry.claim("Clean Title", manga(id_c), 7) ==
          md::TitleDirClaim{.name = std::format("Clean Title [{}]", id_c)});
}

TEST_CASE("title dir: a stem equal to an earlier suffixed name is suffixed itself", "[title_dir]") {
    md::TitleDirRegistry registry{};
    CHECK(registry.claim("Clean Title", manga(id_a), 1).name == "Clean Title");
    CHECK(registry.claim("Clean Title", manga(id_b), 2).name == "Clean Title [77777777]");
    CHECK(registry.claim("Clean Title [77777777]", manga(id_d), 3) ==
          md::TitleDirClaim{.name = "Clean Title [77777777] [99999999]", .taken_by = 2});
}

TEST_CASE("title dir: the full-uuid name is returned even when held, without taking it over",
          "[title_dir]") {
    md::TitleDirRegistry registry{};
    const std::string full = std::format("X [{}]", id_c);
    CHECK(registry.claim("X", manga(id_a), 1).name == "X");
    CHECK(registry.claim("X", manga(id_b), 2).name == "X [77777777]");
    CHECK(registry.claim(full, manga(id_d), 3) == md::TitleDirClaim{.name = full});
    // Unreachable in practice (a title literally named "X [<uuid>]"): OutputRegistry is the
    // backstop, so nothing is overwritten.
    CHECK(registry.claim("X", manga(id_c), 4) == md::TitleDirClaim{.name = full, .taken_by = 1});
    // The first holder keeps it.
    CHECK(registry.claim("unused", manga(id_d), 5) == md::TitleDirClaim{.name = full});
}

TEST_CASE("title dir: a 150-byte stem plus the full suffix is at most 189 bytes", "[title_dir]") {
    const std::string stem = md::sanitize_file_stem(std::string(400, 'q'), id_a);
    REQUIRE(stem.size() == 150);
    md::TitleDirRegistry registry{};
    CHECK(registry.claim(stem, manga(id_a), 1).name == stem);
    CHECK(registry.claim(stem, manga(id_b), 2).name.size() == 150 + 11);
    const md::TitleDirClaim full = registry.claim(stem, manga(id_c), 3);
    CHECK(full.name.size() == max_name_bytes);
    check_one_component(full.name);
}

TEST_CASE("title dir: the registry owns its keys (the stem is borrowed for the call only)",
          "[title_dir]") {
    md::TitleDirRegistry registry{};
    {
        std::string temporary = "Short Lived Title";
        CHECK(registry.claim(temporary, manga(id_a), 1).name == "Short Lived Title");
        temporary.assign(300, 'z'); // reallocates: a stored view would now dangle
    }
    const std::string again = "Short Lived Title";
    CHECK(registry.claim(again, manga(id_b), 2) ==
          md::TitleDirClaim{.name = "Short Lived Title [77777777]", .taken_by = 1});
    CHECK(registry.claim("other", manga(id_a), 3).name == "Short Lived Title");
}

// ---- AC36, property half (V12) ------------------------------------------------------------------

TEST_CASE("title dir: the AC36 examples", "[title_dir][ac36]") {
    CHECK(md::sanitize_file_stem("A/B: C?", id_a) == "A_B_ C_");
    CHECK(md::sanitize_file_stem("...", id_a) == id_a);
    CHECK(md::sanitize_file_stem("     ", id_a) == id_a);
    CHECK(md::sanitize_file_stem(utf8(0x202E), id_a) == id_a);
    CHECK(md::sanitize_file_stem("-.-Title . ", id_a) == "Title");
    CHECK(md::sanitize_file_stem("--Title", id_a) == "Title");
    CHECK(md::sanitize_file_stem(".Title.", id_a) == "Title");
    CHECK(md::sanitize_file_stem("Title  ", id_a) == "Title");

    std::string cjk{};
    for (int i = 0; i < 100; ++i) {
        cjk += "漢";
    }
    REQUIRE(cjk.size() == 300);
    const std::string stem = md::sanitize_file_stem(cjk, id_a);
    CHECK(stem.size() <= 150);
    CHECK(stem.size() % 3 == 0);
    CHECK(ends_on_utf8_boundary(stem));
    check_one_component(stem);
}

TEST_CASE("title dir: every stem of the corpus, plain or suffixed, is one valid component",
          "[title_dir][ac36]") {
    for (const std::string& title : corpus()) {
        check_stem_and_suffixes(title);
    }
}

TEST_CASE("title dir: every stem of 10000 seeded random strings, plain or suffixed, is one valid "
          "component",
          "[title_dir][ac36]") {
    // Catch2's run seed ("Randomness seeded to: <n>" in the log): a failure reproduces with
    // --rng-seed <n>.
    const std::uint32_t seed = Catch::getSeed();
    constexpr std::size_t count = 10000;
    INFO("seed " << seed << " (reproduce with --rng-seed " << seed << ")");
    std::mt19937 rng{seed};
    for (std::size_t i = 0; i < count; ++i) {
        CAPTURE(i);
        check_stem_and_suffixes(random_title(rng));
    }
}
