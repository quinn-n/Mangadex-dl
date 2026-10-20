// SPDX-License-Identifier: GPL-3.0-or-later
//
// One UTF-8 decoder and one "unsafe for a terminal" code-point set, shared by the app's
// terminal_safe and by the batch list checks (the CSV lexer, validate_list_dir). Header-only and
// constexpr. Frozen interface (batch plan T1); the code is a verbatim copy of terminal_safe's, so
// switching terminal_safe over to it changes no output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mangadex_dl {

/// One UTF-8 sequence: its length in bytes (0 = not well-formed) and its code point.
struct Utf8Decoded {
    std::size_t length = 0; ///< 0 = not well-formed (overlong, surrogate, > U+10FFFF, truncated)
    std::uint32_t code_point = 0;

    friend constexpr bool operator==(const Utf8Decoded&, const Utf8Decoded&) = default;
};

namespace detail {

[[nodiscard]] constexpr bool is_utf8_continuation(unsigned char byte) noexcept {
    return byte >= 0x80 && byte <= 0xBF;
}

} // namespace detail

/// Decodes the well-formed UTF-8 sequence (1 to 4 bytes) starting at `text[pos]`. Precondition:
/// `pos < text.size()` (every caller checks it). Length 0 when the bytes there are not
/// well-formed: a stray continuation byte, C0/C1 lead bytes, overlong forms, surrogates, values
/// above U+10FFFF and sequences cut short by the end of `text` included (Unicode table 3-7).
[[nodiscard]] constexpr Utf8Decoded decode_utf8_at(std::string_view text,
                                                   std::size_t pos) noexcept {
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        return Utf8Decoded{.length = 1, .code_point = lead};
    }
    std::size_t length = 0;
    std::uint32_t cp = 0;
    unsigned char low = 0x80; // allowed range of the second byte (Unicode table 3-7)
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        cp = lead & 0x1FU;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        cp = lead & 0x0FU;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        cp = lead & 0x07U;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return Utf8Decoded{};
    }
    if (text.size() - pos < length) {
        return Utf8Decoded{};
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[pos + i]);
        const bool ok = i == 1 ? (byte >= low && byte <= high) : detail::is_utf8_continuation(byte);
        if (!ok) {
            return Utf8Decoded{};
        }
        cp = (cp << 6U) | (byte & 0x3FU);
    }
    return Utf8Decoded{.length = length, .code_point = cp};
}

/// Code points that may change how a terminal renders the rest of the line, or that are invisible
/// and could disguise a message: C1 controls U+0080-U+009F, U+061C, U+200B-U+200F,
/// U+202A-U+202E, U+2028/9, U+2060-U+206F, U+FEFF. C0 controls and DEL are byte-level checks left
/// to the callers.
[[nodiscard]] constexpr bool is_unsafe_code_point(std::uint32_t cp) noexcept {
    return (cp >= 0x80 && cp <= 0x9F)        // C1 controls (CSI is U+009B)
           || cp == 0x061C                   // ARABIC LETTER MARK
           || (cp >= 0x200B && cp <= 0x200F) // zero-width space/joiners, LRM, RLM
           || (cp >= 0x202A && cp <= 0x202E) // bidi embeddings and overrides
           || cp == 0x2028 || cp == 0x2029   // line and paragraph separators
           || (cp >= 0x2060 && cp <= 0x206F) // word joiner, invisible operators, bidi isolates
           || cp == 0xFEFF;                  // zero-width no-break space (BOM)
}

} // namespace mangadex_dl
