// SPDX-License-Identifier: GPL-3.0-or-later
//
// DEL and C1 escaping for the stdout --json documents (json-c1 plan; see json_output.hpp).
#include "json_output.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mangadex_dl::json_output {
namespace {

inline constexpr unsigned char del = 0x7F;
inline constexpr unsigned char c1_lead = 0xC2; ///< UTF-8 lead byte of U+0080..U+00BF
inline constexpr unsigned char c1_first = 0x80;
inline constexpr unsigned char c1_last = 0x9F;

/// "\u00xx": every escape written here is six bytes.
inline constexpr std::size_t escape_size = 6;

/// Lowercase, like nlohmann's "\\u%04x" for the C0 controls.
inline constexpr std::string_view hex_digits = "0123456789abcdef";

[[nodiscard]] unsigned char byte_at(std::string_view text, std::size_t i) noexcept {
    return static_cast<unsigned char>(text[i]);
}

/// How many bytes starting at text[i] (i < text.size()) become one escape: 1 for a DEL, 2 for a
/// C1 pair, 0 for a byte that is copied as is. The single source of the skip logic, so the
/// counting pass and the rewrite pass can never disagree. The second byte of a pair is read only
/// after the bounds check, so a C2 at the very end is copied unchanged.
[[nodiscard]] std::size_t escaped_span(std::string_view text, std::size_t i) noexcept {
    const unsigned char byte = byte_at(text, i);
    if (byte == del) {
        return 1;
    }
    if (byte == c1_lead && i + 1 < text.size()) {
        const unsigned char next = byte_at(text, i + 1);
        if (next >= c1_first && next <= c1_last) {
            return 2;
        }
    }
    return 0;
}

/// Appends \u00xx for `code_point` (<= 0xFF). For a C1 pair C2 xx the code point is xx itself.
void append_escape(std::string& out, unsigned char code_point) {
    const std::size_t value = code_point;
    out += "\\u00";
    out += hex_digits[value >> 4U];
    out += hex_digits[value & 0x0FU];
}

} // namespace

std::string escape_del_and_c1(std::string text) {
    // Pass 1: how many bytes the escapes add (+5 per DEL, +4 per C1 pair). Checked on every step,
    // so `extra` itself can never wrap.
    const std::size_t limit = text.max_size() - text.size();
    std::size_t extra = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t span = escaped_span(text, i);
        if (span == 0) {
            ++i;
            continue;
        }
        extra += escape_size - span;
        if (extra > limit) {
            throw std::length_error("escaped JSON document exceeds std::string::max_size()");
        }
        i += span;
    }
    if (extra == 0) {
        return text; // the common case: nothing to escape, no copy
    }

    // Pass 2: the rewrite, with the same skip logic. reserve() is only an optimization.
    std::string out{};
    out.reserve(text.size() + extra);
    i = 0;
    while (i < text.size()) {
        const std::size_t span = escaped_span(text, i);
        if (span == 0) {
            out += text[i];
            ++i;
            continue;
        }
        // The last byte of the span is the code point: 7F for a DEL, xx for C2 xx.
        append_escape(out, byte_at(text, i + span - 1));
        i += span;
    }
    return out;
}

std::string dump_document(const nlohmann::ordered_json& doc) {
    // replace: every string in these documents may hold untrusted bytes; the strict handler would
    // throw on invalid UTF-8, and replace never synthesizes a DEL or C1 byte. ensure_ascii=false
    // keeps valid UTF-8 readable; nlohmann escapes the C0 controls (ESC included) as \u00xx and
    // escape_del_and_c1 does the same for DEL and C1.
    std::string text = escape_del_and_c1(
        doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
    text += '\n';
    return text;
}

} // namespace mangadex_dl::json_output
