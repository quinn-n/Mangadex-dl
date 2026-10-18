// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output file names (download plan T9). Byte-level and locale-independent: UTF-8 is decoded
// strictly by hand, so invalid input can never reach a path or a terminal unchanged.
#include "mangadex_dl/filename.hpp"

#include "mangadex_dl/model.hpp"
#include "mangadex_dl/selection.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl {
namespace {

/// The stem limit from the design; leaves room for " - Vol. … Ch. ….epub" within 255 bytes.
inline constexpr std::size_t max_stem_bytes = 150;
/// A volume or chapter label inside a file name. 150 + 32 + 32 + the fixed text stays far below
/// the 255-byte component limit.
inline constexpr std::size_t max_label_bytes = 32;
/// Digits of the chapter id shown in a oneshot's name.
inline constexpr std::size_t oneshot_id_digits = 8;

/// One decoded code point and how many bytes it used.
struct Decoded {
    char32_t code_point = 0;
    std::size_t length = 0;
};

[[nodiscard]] constexpr bool is_continuation(unsigned char byte) noexcept {
    return byte >= 0x80 && byte <= 0xBF;
}

/// Strict UTF-8 decoding of the sequence starting at `text[pos]` (no overlongs, no surrogates,
/// nothing above U+10FFFF). nullopt when the bytes at `pos` do not start a valid sequence.
[[nodiscard]] std::optional<Decoded> decode_utf8(std::string_view text, std::size_t pos) noexcept {
    const auto byte_at = [text](std::size_t i) noexcept {
        return static_cast<unsigned char>(text[i]);
    };
    const unsigned char lead = byte_at(pos);
    if (lead < 0x80) {
        return Decoded{.code_point = lead, .length = 1};
    }
    std::size_t length = 0;
    unsigned char low = 0x80; // allowed range of the second byte
    unsigned char high = 0xBF;
    char32_t code_point = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        code_point = lead & 0x1FU;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        code_point = lead & 0x0FU;
        if (lead == 0xE0) {
            low = 0xA0; // overlong
        } else if (lead == 0xED) {
            high = 0x9F; // UTF-16 surrogates
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        code_point = lead & 0x07U;
        if (lead == 0xF0) {
            low = 0x90; // overlong
        } else if (lead == 0xF4) {
            high = 0x8F; // above U+10FFFF
        }
    } else {
        return std::nullopt; // a continuation byte, C0/C1 (overlong) or F5..FF
    }
    if (text.size() - pos < length) {
        return std::nullopt;
    }
    const unsigned char second = byte_at(pos + 1);
    if (second < low || second > high) {
        return std::nullopt;
    }
    code_point = (code_point << 6U) | (second & 0x3FU);
    for (std::size_t i = 2; i < length; ++i) {
        const unsigned char next = byte_at(pos + i);
        if (!is_continuation(next)) {
            return std::nullopt;
        }
        code_point = (code_point << 6U) | (next & 0x3FU);
    }
    return Decoded{.code_point = code_point, .length = length};
}

enum class CharClass {
    keep,    ///< copied unchanged
    replace, ///< becomes '_'
    drop,    ///< removed without a trace
    space,   ///< part of a whitespace run (collapsed to one ' ')
};

/// Code points are written as numbers: GCC's -Wbidi-chars=any also rejects bidi controls spelled
/// as universal character names.
[[nodiscard]] constexpr CharClass classify(char32_t c) noexcept {
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) {
        return CharClass::replace; // C0 controls, DEL, C1 controls
    }
    switch (c) {
    case U'/':
    case U'\\':
    case U'<':
    case U'>':
    case U':':
    case U'"':
    case U'|':
    case U'?':
    case U'*':
        return CharClass::replace;
    case U' ':
    case 0x00A0: // no-break space
    case 0x1680: // ogham space mark
    case 0x202F: // narrow no-break space
    case 0x205F: // medium mathematical space
    case 0x3000: // ideographic space
        return CharClass::space;
    case 0x061C: // arabic letter mark
    case 0x200E: // left-to-right mark
    case 0x200F: // right-to-left mark
    case 0x2028: // line separator
    case 0x2029: // paragraph separator
        return CharClass::drop;
    default:
        break;
    }
    if (c >= 0x2000 && c <= 0x200A) {
        return CharClass::space; // en quad .. hair space
    }
    if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) {
        return CharClass::drop; // bidi embeddings, overrides and isolates
    }
    // Invisible format characters, the same set T13's terminal_safe drops: zero-width space,
    // non-joiner and joiner; word joiner and invisible operators (U+2060..U+2065); deprecated
    // format characters (U+206A..U+206F); the byte order mark / zero-width no-break space.
    if ((c >= 0x200B && c <= 0x200D) || (c >= 0x2060 && c <= 0x2065) ||
        (c >= 0x206A && c <= 0x206F) || c == 0xFEFF) {
        return CharClass::drop;
    }
    // Unicode non-characters: U+FDD0..U+FDEF and the last two code points of every plane
    // (U+xFFFE, U+xFFFF). Never meant for interchange, so they don't belong in a file name.
    if ((c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFEU) == 0xFFFEU) {
        return CharClass::drop;
    }
    return CharClass::keep;
}

/// Removes trailing spaces and dots.
void trim_back(std::string& text) {
    const std::size_t last = text.find_last_not_of(" .");
    text.erase(last == std::string::npos ? 0 : last + 1);
}

/// The sanitizing rules of sanitize_file_stem with a byte limit; an empty result stays empty.
/// The output is always valid UTF-8 (every invalid input byte became '_').
[[nodiscard]] std::string sanitize(std::string_view text, std::size_t max_bytes) {
    std::string out{};
    out.reserve(text.size());
    bool pending_space = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::optional<Decoded> decoded = decode_utf8(text, pos);
        const CharClass kind = decoded ? classify(decoded->code_point) : CharClass::replace;
        const std::size_t length = decoded ? decoded->length : 1;
        switch (kind) {
        case CharClass::drop:
            break;
        case CharClass::space:
            pending_space = true;
            break;
        case CharClass::replace:
        case CharClass::keep:
            if (pending_space && !out.empty()) {
                out += ' ';
            }
            pending_space = false;
            if (kind == CharClass::replace) {
                out += '_';
            } else {
                out.append(text.substr(pos, length));
            }
            break;
        }
        pos += length;
    }
    // Leading spaces were never emitted; trim leading dots and dashes (and any space they hid).
    const std::size_t first = out.find_first_not_of(" .-");
    out.erase(0, first == std::string::npos ? out.size() : first);
    trim_back(out);
    if (out.size() > max_bytes) {
        // In bounds: out.size() > max_bytes, so out[cut] exists for every cut <= max_bytes.
        std::size_t cut = max_bytes;
        while (cut > 0 && is_continuation(static_cast<unsigned char>(out[cut]))) {
            --cut; // out[cut] starts the character that would be split
        }
        out.resize(cut);
        trim_back(out);
    }
    return out;
}

/// Pads the integer part of a numeric label with zeros to `width` digits.
[[nodiscard]] std::string format_label(std::string_view label, std::size_t width) {
    const std::optional<ChapterNumber> number = ChapterNumber::parse(label);
    if (!number) {
        return std::string{label};
    }
    std::string normalized = number->normalized();
    // npos (no fraction) is larger than any size, so min() yields the whole length.
    const std::size_t integer_digits = std::min(normalized.find('.'), normalized.size());
    if (integer_digits < width) {
        normalized.insert(0, width - integer_digits, '0');
    }
    return normalized;
}

/// A formatted label made safe inside a file name; never empty.
[[nodiscard]] std::string file_label(const std::string& formatted) {
    const std::string safe = sanitize(formatted, max_label_bytes);
    return safe.empty() ? std::string{"_"} : safe;
}

[[nodiscard]] constexpr bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// The first eight hex digits of the oneshot's chapter id, lowercased.
[[nodiscard]] std::string oneshot_tag(const PlannedEpub& epub) {
    std::string tag{};
    if (epub.chapters.empty()) {
        return tag;
    }
    for (const char c : epub.chapters.front().id) {
        if (tag.size() == oneshot_id_digits) {
            break;
        }
        if (is_hex_digit(c)) {
            tag += ascii_lower(c);
        }
    }
    return tag;
}

} // namespace

std::string sanitize_file_stem(std::string_view text, std::string_view fallback) {
    std::string stem = sanitize(text, max_stem_bytes);
    if (stem.empty() || stem == "." || stem == "..") {
        stem = sanitize(fallback, max_stem_bytes);
    }
    if (stem.empty() || stem == "." || stem == "..") {
        stem = "_"; // a fallback that was itself unusable; never return an empty component
    }
    return stem;
}

std::string format_volume_label(std::string_view label) { return format_label(label, 2); }

std::string format_chapter_label(std::string_view label) { return format_label(label, 3); }

std::string epub_file_name(std::string_view stem, const PlannedEpub& epub) {
    std::string name{stem};
    switch (epub.kind) {
    case EpubKind::volume:
        if (epub.volume) {
            name += " - Vol. ";
            name += file_label(format_volume_label(*epub.volume));
        }
        break;
    case EpubKind::chapter:
        if (epub.volume) {
            name += " - Vol. ";
            name += file_label(format_volume_label(*epub.volume));
            name += " Ch. ";
        } else {
            name += " - Ch. ";
        }
        name += file_label(format_chapter_label(epub.chapter.value_or(std::string{})));
        break;
    case EpubKind::oneshot: {
        name += " - Oneshot";
        const std::string tag = oneshot_tag(epub);
        if (!tag.empty()) {
            name += " [";
            name += tag;
            name += ']';
        }
        break;
    }
    case EpubKind::single:
        break;
    }
    name += ".epub";
    return name;
}

} // namespace mangadex_dl
