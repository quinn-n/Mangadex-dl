// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/text_safety/text_safety_test.cpp: the shared UTF-8 decoder and unsafe-code-point set
// (batch plan T1, rev1). Everything is constexpr, so the table and the decoder are pinned by
// static_asserts: a read past the end in a constant expression does not compile, and
// _GLIBCXX_ASSERTIONS checks string_view indexing in the sanitized build. The TEST_CASEs repeat the
// decoder cases at run time over exact-size heap copies (decode_exact), where ASan watches the
// byte after the last one.
//
// Bidi controls are built from char arrays, never written in a string literal (not even as hex
// escapes): GCC's -Wbidi-chars=any and clang-tidy's misc-misleading-bidirectional reject them.
#include "mangadex_dl/text_safety.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace md = mangadex_dl;

namespace {

using std::string_view_literals::operator""sv;

[[nodiscard]] constexpr md::Utf8Decoded cp(std::size_t length, std::uint32_t code_point) {
    return md::Utf8Decoded{.length = length, .code_point = code_point};
}

constexpr md::Utf8Decoded malformed{};

/// decode_utf8_at over an exact-size heap copy of `bytes`. A short std::string would not do: its
/// text lives inline in the object (SSO), and even a heap one keeps a NUL after the last byte, so
/// ASan would not see a read just past the end. std::vector's iterator-range constructor
/// allocates exactly bytes.size() bytes, so ASan reports any such read.
[[nodiscard]] md::Utf8Decoded decode_exact(std::string_view bytes, std::size_t pos) {
    const std::vector<char> copy(bytes.begin(), bytes.end());
    return md::decode_utf8_at(std::string_view{copy.data(), copy.size()}, pos);
}

/// U+202E RIGHT-TO-LEFT OVERRIDE as UTF-8.
constexpr std::array<char, 3> rlo_bytes{'\xE2', '\x80', '\xAE'};
constexpr std::string_view rlo{rlo_bytes.data(), rlo_bytes.size()};

// ---- the unsafe set (security review: C1, bidi, invisible, separators) ------------------------
static_assert(md::is_unsafe_code_point(0x009B)); // CSI
static_assert(md::is_unsafe_code_point(0x0085)); // NEL
static_assert(md::is_unsafe_code_point(0x202E)); // RIGHT-TO-LEFT OVERRIDE
static_assert(md::is_unsafe_code_point(0x2028)); // LINE SEPARATOR
static_assert(md::is_unsafe_code_point(0x2066)); // LEFT-TO-RIGHT ISOLATE
static_assert(md::is_unsafe_code_point(0x200E)); // LEFT-TO-RIGHT MARK
static_assert(md::is_unsafe_code_point(0x061C)); // ARABIC LETTER MARK
static_assert(md::is_unsafe_code_point(0xFEFF)); // ZERO WIDTH NO-BREAK SPACE (BOM)

static_assert(!md::is_unsafe_code_point(0x00E9));  // e with acute
static_assert(!md::is_unsafe_code_point(U'Z'));    // ASCII
static_assert(!md::is_unsafe_code_point(0x3042));  // HIRAGANA LETTER A
static_assert(!md::is_unsafe_code_point(0x1F600)); // GRINNING FACE

// Range edges: each range is inclusive at both ends and nothing next to it is caught.
static_assert(md::is_unsafe_code_point(0x0080) && md::is_unsafe_code_point(0x009F));
static_assert(!md::is_unsafe_code_point(0x007F) && !md::is_unsafe_code_point(0x00A0));
static_assert(!md::is_unsafe_code_point(0x061B) && !md::is_unsafe_code_point(0x061D));
static_assert(md::is_unsafe_code_point(0x200B) && md::is_unsafe_code_point(0x200F));
static_assert(!md::is_unsafe_code_point(0x200A) && !md::is_unsafe_code_point(0x2010));
static_assert(md::is_unsafe_code_point(0x2029) && md::is_unsafe_code_point(0x202A));
static_assert(!md::is_unsafe_code_point(0x2027) && !md::is_unsafe_code_point(0x202F));
static_assert(md::is_unsafe_code_point(0x2060) && md::is_unsafe_code_point(0x206F));
static_assert(!md::is_unsafe_code_point(0x205F) && !md::is_unsafe_code_point(0x2070));
static_assert(!md::is_unsafe_code_point(0xFEFE) && !md::is_unsafe_code_point(0xFF00));
// C0 controls and DEL are byte-level checks the callers make; the predicate leaves them alone.
static_assert(!md::is_unsafe_code_point(0x0000) && !md::is_unsafe_code_point(0x001B));

// ---- the decoder -----------------------------------------------------------------------------
// ASCII (NUL included) is one byte.
static_assert(md::decode_utf8_at("A"sv, 0) == cp(1, U'A'));
static_assert(md::decode_utf8_at("\0"sv, 0) == cp(1, 0));
static_assert(md::decode_utf8_at("\x7F"sv, 0) == cp(1, 0x7F));
// 2, 3 and 4 bytes, at the edges of each length.
static_assert(md::decode_utf8_at("\xC2\x80"sv, 0) == cp(2, 0x80));
static_assert(md::decode_utf8_at("\xC3\xA9"sv, 0) == cp(2, 0xE9));
static_assert(md::decode_utf8_at("\xDF\xBF"sv, 0) == cp(2, 0x7FF));
static_assert(md::decode_utf8_at("\xE0\xA0\x80"sv, 0) == cp(3, 0x800));
static_assert(md::decode_utf8_at("\xE3\x81\x82"sv, 0) == cp(3, 0x3042));
static_assert(md::decode_utf8_at(rlo, 0) == cp(3, 0x202E));
static_assert(md::decode_utf8_at("\xED\x9F\xBF"sv, 0) == cp(3, 0xD7FF));
static_assert(md::decode_utf8_at("\xEE\x80\x80"sv, 0) == cp(3, 0xE000));
static_assert(md::decode_utf8_at("\xEF\xBF\xBF"sv, 0) == cp(3, 0xFFFF));
static_assert(md::decode_utf8_at("\xF0\x90\x80\x80"sv, 0) == cp(4, 0x10000));
static_assert(md::decode_utf8_at("\xF0\x9F\x98\x80"sv, 0) == cp(4, 0x1F600));
static_assert(md::decode_utf8_at("\xF4\x8F\xBF\xBF"sv, 0) == cp(4, 0x10FFFF));
// At an offset, and with bytes after the sequence (they are not consumed).
static_assert(md::decode_utf8_at("a\xC3\xA9z"sv, 1) == cp(2, 0xE9));
static_assert(md::decode_utf8_at("a\xC3\xA9z"sv, 3) == cp(1, U'z'));
// Overlong forms.
static_assert(md::decode_utf8_at("\xC0\x80"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xC1\xBF"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xE0\x9F\xBF"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xF0\x8F\xBF\xBF"sv, 0) == malformed);
// Surrogates (U+D800-U+DFFF).
static_assert(md::decode_utf8_at("\xED\xA0\x80"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xED\xBF\xBF"sv, 0) == malformed);
// Above U+10FFFF.
static_assert(md::decode_utf8_at("\xF4\x90\x80\x80"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xF5\x80\x80\x80"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xFF"sv, 0) == malformed);
// Truncated by the end of the text, also when the text goes on before `pos`.
static_assert(md::decode_utf8_at("\xC3"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xE2\x80"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xF0\x9F\x98"sv, 0) == malformed);
static_assert(md::decode_utf8_at("abc\xF0\x9F\x98"sv, 3) == malformed);
// A stray continuation byte, and a lead byte followed by a non-continuation.
static_assert(md::decode_utf8_at("\x80"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xBF"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xC3("sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xE2\x28\xA1"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xE2\x82\x28"sv, 0) == malformed);
static_assert(md::decode_utf8_at("\xF0\x9F\x98("sv, 0) == malformed);

} // namespace

TEST_CASE("decode_utf8_at decodes well-formed sequences of every length at run time",
          "[text_safety]") {
    CHECK(decode_exact("A"sv, 0) == cp(1, U'A'));
    CHECK(decode_exact("\xC3\xA9"sv, 0) == cp(2, 0xE9));
    CHECK(decode_exact("\xE3\x81\x82"sv, 0) == cp(3, 0x3042));
    CHECK(decode_exact("\xF0\x9F\x98\x80"sv, 0) == cp(4, 0x1F600));
    const std::string mixed = std::string{"x"} + std::string{rlo} + "y";
    CHECK(decode_exact(mixed, 1) == cp(3, 0x202E));
    CHECK(md::is_unsafe_code_point(decode_exact(mixed, 1).code_point));
    CHECK(decode_exact(mixed, 4) == cp(1, U'y'));
    // A sequence that ends exactly at the end of a longer text.
    CHECK(decode_exact("a long ASCII prefix, then \xF0\x9F\x98\x80"sv, 26) == cp(4, 0x1F600));
}

TEST_CASE("decode_utf8_at rejects malformed input without reading past the end", "[text_safety]") {
    // Each truncated sequence ends the buffer, so a read past the length check is a
    // heap-buffer-overflow under ASan (decode_exact's copy has no slack after the last byte).
    CHECK(decode_exact("\xC3"sv, 0) == malformed);
    CHECK(decode_exact("\xE2\x80"sv, 0) == malformed);
    CHECK(decode_exact("ab\xF0\x9F\x98"sv, 2) == malformed);
    CHECK(decode_exact("a long ASCII prefix, then \xF0\x9F\x98"sv, 26) == malformed);
    CHECK(decode_exact("\xE0\x80\xAF"sv, 0) == malformed);     // overlong
    CHECK(decode_exact("\xED\xB0\x80"sv, 0) == malformed);     // surrogate
    CHECK(decode_exact("\xF4\x90\x80\x80"sv, 0) == malformed); // above U+10FFFF
}

TEST_CASE("Every C1 control decodes as two bytes and is unsafe", "[text_safety]") {
    for (std::uint32_t c1 = 0x80; c1 <= 0x9F; ++c1) {
        const std::string bytes{static_cast<char>(0xC2), static_cast<char>(c1)};
        const md::Utf8Decoded decoded = decode_exact(bytes, 0);
        CHECK(decoded == cp(2, c1));
        CHECK(md::is_unsafe_code_point(decoded.code_point));
    }
}
