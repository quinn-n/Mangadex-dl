// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/filename/filename_test.cpp: output file names (download plan T9, acceptance 5):
// the sanitizing table, label formatting and the per-kind file name shapes.
#include "mangadex_dl/filename.hpp"
#include "mangadex_dl/model.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view fallback = "11111111-1111-4111-8111-111111111111";

struct StemCase {
    std::string input{};
    std::string_view expected{}; ///< a literal or `fallback` (static storage)
};

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

/// True when `text` is well-formed UTF-8 that does not end inside a character (a simple check
/// that is independent of the implementation's decoder: lead-byte length plus continuations).
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

[[nodiscard]] md::PlannedEpub
epub(md::EpubKind kind, std::optional<std::string> volume, std::optional<std::string> chapter,
     std::string chapter_id = "abcdef01-2345-4678-89ab-cdef01234567") {
    return md::PlannedEpub{.kind = kind,
                           .volume = std::move(volume),
                           .chapter = std::move(chapter),
                           .chapters = {md::Chapter{.id = std::move(chapter_id)}}};
}

} // namespace

TEST_CASE("sanitize_file_stem: the sanitizing table", "[filename]") {
    const StemCase c = GENERATE(
        StemCase{.input = "Clean Title", .expected = "Clean Title"},
        StemCase{.input = "クリーン・タイトル", .expected = "クリーン・タイトル"},
        StemCase{.input = R"(a/b\c<d>e:f"g|h?i*j)", .expected = "a_b_c_d_e_f_g_h_i_j"},
        StemCase{.input = "bell" + utf8(0x07) + "del" + utf8(0x7F) + "end",
                 .expected = "bell_del_end"},
        StemCase{.input = "tab\there", .expected = "tab_here"},
        StemCase{.input = "c1" + utf8(0x85) + "x" + utf8(0x9B) + "y", .expected = "c1_x_y"},
        StemCase{.input = "  many   spaces  ", .expected = "many spaces"},
        StemCase{.input = "nbsp" + utf8(0xA0) + utf8(0x3000) + utf8(0x2003) + "run",
                 .expected = "nbsp run"},
        StemCase{.input = "abc" + utf8(0x202E) + "def", .expected = "abcdef"},
        StemCase{.input = utf8(0x2066) + "iso" + utf8(0x2069) + " " + utf8(0x200E) + "lrm" +
                          utf8(0x200F) + " " + utf8(0x061C) + "alm",
                 .expected = "iso lrm alm"},
        StemCase{.input = "line" + utf8(0x2028) + "para" + utf8(0x2029) + "end",
                 .expected = "lineparaend"},
        StemCase{.input = "a " + utf8(0x202A) + utf8(0x202D) + " b", .expected = "a b"},
        StemCase{.input = "a\xff"
                          "b",
                 .expected = "a_b"},
        StemCase{.input = "overlong\xc0\xaf", .expected = "overlong__"},
        StemCase{.input = "surrogate\xed\xa0\x80", .expected = "surrogate___"},
        StemCase{.input = "big\xf4\x90\x80\x80", .expected = "big____"},
        StemCase{.input = "cut\xe3\x81", .expected = "cut__"},
        StemCase{.input = "e0overlong\xe0\x80\x80", .expected = "e0overlong___"},
        StemCase{.input = "f0overlong\xf0\x80\x80\x80", .expected = "f0overlong____"},
        StemCase{.input = "third\xe3\x81"
                          "X",
                 .expected = "third__X"},
        StemCase{.input = "fourth\xf0\x9f\x98"
                          "Y",
                 .expected = "fourth___Y"},
        StemCase{.input = "min3\xe0\xa0\x80", .expected = "min3\xe0\xa0\x80"},
        StemCase{.input = "min4\xf0\x90\x80\x80", .expected = "min4\xf0\x90\x80\x80"},
        StemCase{.input = "zw" + utf8(0x200B) + utf8(0x200C) + utf8(0x200D) + "j" + utf8(0x2060) +
                          utf8(0x2065) + "o" + utf8(0x206A) + utf8(0x206F) + "in" + utf8(0xFEFF),
                 .expected = "zwjoin"},
        StemCase{.input = "kept" + utf8(0x200A) + utf8(0x2010) + utf8(0x2070) + utf8(0xFEFE),
                 .expected = "kept "
                             "\xe2\x80\x90"
                             "\xe2\x81\xb0"
                             "\xef\xbb\xbe"},
        StemCase{.input = "non" + utf8(0xFDD0) + utf8(0xFDEF) + "char" + utf8(0xFFFE) +
                          utf8(0xFFFF) + "act" + utf8(0x1FFFE) + utf8(0x2FFFF) + "ers" +
                          utf8(0x10FFFE) + utf8(0x10FFFF),
                 .expected = "noncharacters"},
        StemCase{.input = "near" + utf8(0xFDCF) + utf8(0xFDF0) + utf8(0xFFFD) + utf8(0x1FFFD),
                 .expected = "near"
                             "\xef\xb7\x8f"
                             "\xef\xb7\xb0"
                             "\xef\xbf\xbd"
                             "\xf0\x9f\xbf\xbd"},
        StemCase{.input = utf8(0xFDD0) + utf8(0xFFFF), .expected = fallback},
        StemCase{.input = "..hidden..", .expected = "hidden"},
        StemCase{.input = "-rf", .expected = "rf"},
        StemCase{.input = " . - -name- . ", .expected = "name-"},
        StemCase{.input = "///", .expected = "___"}, StemCase{.input = "", .expected = fallback},
        StemCase{.input = ".", .expected = fallback}, StemCase{.input = "..", .expected = fallback},
        StemCase{.input = "   ", .expected = fallback},
        StemCase{.input = utf8(0x202E) + utf8(0x2028) + utf8(0x200F), .expected = fallback},
        StemCase{.input = "- . -", .expected = fallback});
    CAPTURE(c.input);
    CHECK(md::sanitize_file_stem(c.input, fallback) == c.expected);
}

TEST_CASE("sanitize_file_stem cuts at 150 bytes on a UTF-8 boundary", "[filename]") {
    SECTION("inside a three-byte character") {
        std::string title = "a";
        for (int i = 0; i < 60; ++i) {
            title += "あ"; // 3 bytes each: 1 + 49 * 3 = 148, the 50th would end at byte 151
        }
        const std::string stem = md::sanitize_file_stem(title, fallback);
        std::string expected = "a";
        for (int i = 0; i < 49; ++i) {
            expected += "あ";
        }
        CHECK(stem == expected);
        CHECK(stem.size() == 148);
        CHECK(ends_on_utf8_boundary(stem));
    }
    SECTION("exactly on a boundary after four-byte characters") {
        std::string title = "ab";
        for (int i = 0; i < 40; ++i) {
            title += utf8(0x1F600); // 2 + 37 * 4 = 150: byte 150 starts a character, no walk-back
        }
        const std::string stem = md::sanitize_file_stem(title, fallback);
        CHECK(stem.size() == 150);
        CHECK(ends_on_utf8_boundary(stem));
    }
    SECTION("inside a four-byte character") {
        std::string title = "abc";
        for (int i = 0; i < 40; ++i) {
            title += utf8(0x1F600); // 3 + 36 * 4 = 147; byte 150 is the 37th's last byte
        }
        const std::string stem = md::sanitize_file_stem(title, fallback);
        std::string expected = "abc";
        for (int i = 0; i < 36; ++i) {
            expected += utf8(0x1F600);
        }
        CHECK(stem == expected);
        CHECK(stem.size() == 147); // walked back three continuation bytes
        CHECK(ends_on_utf8_boundary(stem));
    }
    SECTION("a cut that leaves a trailing space or dot is trimmed again") {
        const std::string title = std::string(148, 'x') + " .yz";
        const std::string stem = md::sanitize_file_stem(title, fallback);
        CHECK(stem == std::string(148, 'x'));
    }
    SECTION("ASCII is cut at exactly 150") {
        const std::string stem = md::sanitize_file_stem(std::string(400, 'q'), fallback);
        CHECK(stem == std::string(150, 'q'));
    }
}

TEST_CASE("sanitize_file_stem never returns an unusable component", "[filename]") {
    CHECK(md::sanitize_file_stem("", "") == "_");
    CHECK(md::sanitize_file_stem("..", "..") == "_");
    CHECK(md::sanitize_file_stem("", "a/b") == "a_b");
}

TEST_CASE("format_volume_label and format_chapter_label pad numeric labels", "[filename]") {
    CHECK(md::format_volume_label("3") == "03");
    CHECK(md::format_volume_label("007") == "07");
    CHECK(md::format_volume_label("12") == "12");
    CHECK(md::format_volume_label("123") == "123");
    CHECK(md::format_volume_label("3.50") == "03.5");
    CHECK(md::format_volume_label("0") == "00");
    CHECK(md::format_volume_label("Extra") == "Extra");
    CHECK(md::format_volume_label("") == "");
    CHECK(md::format_volume_label("1.") == "1."); // not numeric: unchanged

    CHECK(md::format_chapter_label("12") == "012");
    CHECK(md::format_chapter_label("12.50") == "012.5");
    CHECK(md::format_chapter_label("0") == "000");
    CHECK(md::format_chapter_label("1234") == "1234");
    CHECK(md::format_chapter_label("0010.5") == "010.5");
    CHECK(md::format_chapter_label("Prologue") == "Prologue");
}

TEST_CASE("epub_file_name shapes per kind", "[filename]") {
    using md::EpubKind;
    CHECK(md::epub_file_name("T", epub(EpubKind::volume, "3", std::nullopt)) == "T - Vol. 03.epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::chapter, std::nullopt, "12")) ==
          "T - Ch. 012.epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::chapter, std::nullopt, "12.5")) ==
          "T - Ch. 012.5.epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::chapter, "3", "21")) ==
          "T - Vol. 03 Ch. 021.epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::oneshot, std::nullopt, std::nullopt,
                                       "ABCDEF01-2345-4678-89ab-cdef01234567")) ==
          "T - Oneshot [abcdef01].epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::single, std::nullopt, std::nullopt)) == "T.epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::volume, "Extra", std::nullopt)) ==
          "T - Vol. Extra.epub");
}

TEST_CASE("epub_file_name sanitizes labels from the API", "[filename]") {
    using md::EpubKind;
    CHECK(md::epub_file_name("T", epub(EpubKind::volume, "../a/b", std::nullopt)) ==
          "T - Vol. _a_b.epub");
    const std::string rlo_dot = utf8(0x202E) + ".";
    CHECK(md::epub_file_name("T", epub(EpubKind::chapter, std::nullopt, rlo_dot)) ==
          "T - Ch. _.epub");
    CHECK(md::epub_file_name("T", epub(EpubKind::chapter, "x\ny", "a:b")) ==
          "T - Vol. x_y Ch. a_b.epub");
}

TEST_CASE("epub_file_name stays one component of at most 255 bytes", "[filename]") {
    const std::string stem = md::sanitize_file_stem(std::string(1000, 's'), fallback);
    const std::string long_label = std::string(500, 'L') + "/";
    const std::string name =
        md::epub_file_name(stem, epub(md::EpubKind::chapter, long_label, long_label));
    CHECK(name.size() <= 255);
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.ends_with(".epub"));
    CHECK(name.starts_with(stem + " - Vol. "));
}
