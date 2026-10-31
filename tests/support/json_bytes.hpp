#pragma once
// tests/support/json_bytes.hpp: byte-level checks on dumped --json text (json-c1 plan). Header-only
// and Catch2-free.
//
// A --json document must spell DEL as \u007f and the C1 controls U+0080..U+009F as
// \u0080..\u009f, never as raw bytes. In UTF-8 a C1 control is the two-byte sequence C2 80..C2 9F,
// and C2 is never a continuation byte, so a C2 followed by 80..9F is exactly one C1 control. A lone
// byte such as 9B is NOT a C1 control: it also ends U+011B (C4 9B), so the check never looks for
// one on its own.
//
//     CHECK(!mangadex_dl::test::has_raw_del_or_c1(text));

#include <cstddef>
#include <string_view>

namespace mangadex_dl::test {

/// True when `text` holds a raw DEL (7F) or a C1 control (C2 80..C2 9F) anywhere.
[[nodiscard]] constexpr bool has_raw_del_or_c1(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == 0x7F) {
            return true;
        }
        if (byte == 0xC2 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                return true;
            }
        }
    }
    return false;
}

static_assert(!has_raw_del_or_c1("plain \xc2\xa0 \xc4\x9b text")); // U+00A0, U+011B
static_assert(has_raw_del_or_c1("a\x7f"));
static_assert(has_raw_del_or_c1("\xc2\x80"));
static_assert(has_raw_del_or_c1("x\xc2\x9by"));
static_assert(!has_raw_del_or_c1("\xc2"));
static_assert(!has_raw_del_or_c1("\xc4\x9b"));

} // namespace mangadex_dl::test
