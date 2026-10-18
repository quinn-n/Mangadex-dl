// SPDX-License-Identifier: GPL-3.0-or-later
//
// Page-image identification from header bytes only (download plan T5). Nothing is decoded: each
// format's fixed header (or, for JPEG, the marker segments before the first frame header) is
// parsed just far enough to read the dimensions.
//
// Bounds safety: every multi-byte read goes through read_be/read_le, which check
// `offset + count <= bytes.size()` (written overflow-free) before touching memory and return
// nullopt otherwise. No pointer arithmetic, no reinterpret_cast, no unaligned loads.
#include "mangadex_dl/image.hpp"

#include "mangadex_dl/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

using Bytes = std::span<const std::byte>;

[[nodiscard]] constexpr bool has_bytes(Bytes bytes, std::size_t offset,
                                       std::size_t count) noexcept {
    return offset <= bytes.size() && count <= bytes.size() - offset;
}

/// Big-endian unsigned integer of `count` (1..4) bytes at `offset`, or nullopt if out of range.
[[nodiscard]] constexpr std::optional<std::uint32_t> read_be(Bytes bytes, std::size_t offset,
                                                             std::size_t count) noexcept {
    if (count == 0 || count > 4 || !has_bytes(bytes, offset, count)) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[offset + i]);
    }
    return value;
}

/// Little-endian unsigned integer of `count` (1..4) bytes at `offset`, or nullopt if out of range.
[[nodiscard]] constexpr std::optional<std::uint32_t> read_le(Bytes bytes, std::size_t offset,
                                                             std::size_t count) noexcept {
    if (count == 0 || count > 4 || !has_bytes(bytes, offset, count)) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t i = count; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[offset + i - 1]);
    }
    return value;
}

/// True if `bytes` holds exactly the characters of `magic` at `offset` (false when too short).
[[nodiscard]] constexpr bool matches(Bytes bytes, std::size_t offset,
                                     std::string_view magic) noexcept {
    if (!has_bytes(bytes, offset, magic.size())) {
        return false;
    }
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (std::to_integer<unsigned char>(bytes[offset + i]) !=
            static_cast<unsigned char>(magic[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::unexpected<Error> image_error(std::string message) {
    return std::unexpected(Error{.code = Errc::failure, .message = std::move(message)});
}

[[nodiscard]] std::unexpected<Error> truncated(MediaType type) {
    return image_error(std::format("truncated {} header", media_type_name(type)));
}

[[nodiscard]] std::unexpected<Error> malformed(MediaType type, std::string_view what) {
    return image_error(std::format("malformed {} header: {}", media_type_name(type), what));
}

/// The single place the side limits are enforced.
[[nodiscard]] Result<ImageInfo> make_info(MediaType type, std::uint32_t width,
                                          std::uint32_t height) {
    if (width == 0 || height == 0 || width > max_image_side || height > max_image_side) {
        return image_error(std::format("{} dimensions {}x{} are out of range (each side 1-{})",
                                       media_type_name(type), width, height, max_image_side));
    }
    return ImageInfo{.type = type, .width = width, .height = height};
}

// ---- PNG: signature, then the IHDR chunk (length 13), width and height big-endian ------------

constexpr std::string_view png_signature{"\x89PNG\r\n\x1a\n"};

[[nodiscard]] Result<ImageInfo> sniff_png(Bytes bytes) {
    constexpr MediaType type = MediaType::png;
    const std::optional<std::uint32_t> length = read_be(bytes, 8, 4);
    if (!length || !has_bytes(bytes, 12, 4)) {
        return truncated(type);
    }
    if (*length != 13 || !matches(bytes, 12, "IHDR")) {
        return malformed(type, "the first chunk is not IHDR");
    }
    const std::optional<std::uint32_t> width = read_be(bytes, 16, 4);
    const std::optional<std::uint32_t> height = read_be(bytes, 20, 4);
    if (!width || !height) {
        return truncated(type);
    }
    return make_info(type, *width, *height);
}

// ---- GIF: "GIF87a"/"GIF89a", then the logical screen width and height little-endian ----------

[[nodiscard]] Result<ImageInfo> sniff_gif(Bytes bytes) {
    constexpr MediaType type = MediaType::gif;
    const std::optional<std::uint32_t> width = read_le(bytes, 6, 2);
    const std::optional<std::uint32_t> height = read_le(bytes, 8, 2);
    if (!width || !height) {
        return truncated(type);
    }
    return make_info(type, *width, *height);
}

// ---- JPEG: SOI, then a marker walk up to the first SOFn frame header --------------------------

/// SOF0-SOF3, SOF5-SOF7, SOF9-SOF11, SOF13-SOF15. 0xC4 (DHT), 0xC8 (JPG) and 0xCC (DAC) share
/// the range but are not frame headers.
[[nodiscard]] constexpr bool is_start_of_frame(std::uint32_t marker) noexcept {
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

/// Markers without a length field: TEM and RST0-RST7.
[[nodiscard]] constexpr bool is_standalone(std::uint32_t marker) noexcept {
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7);
}

struct JpegMarker {
    std::uint32_t code = 0;
    std::size_t next = 0; ///< offset just past the marker code
};

/// The marker at `pos`: 0xFF, any number of 0xFF fill bytes, then the marker code.
[[nodiscard]] Result<JpegMarker> read_marker(Bytes bytes, std::size_t pos) {
    constexpr MediaType type = MediaType::jpeg;
    const std::optional<std::uint32_t> lead = read_be(bytes, pos, 1);
    if (!lead) {
        return truncated(type);
    }
    if (*lead != 0xFF) {
        return malformed(type, "expected a marker");
    }
    std::size_t code_at = pos + 1;
    std::optional<std::uint32_t> code = read_be(bytes, code_at, 1);
    while (code && *code == 0xFF) {
        ++code_at;
        code = read_be(bytes, code_at, 1);
    }
    if (!code) {
        return truncated(type);
    }
    return JpegMarker{.code = *code, .next = code_at + 1};
}

/// A SOFn segment at `pos` (its length field, already read as `length`): sample precision (1),
/// number of lines (2), samples per line (2), number of components (1).
[[nodiscard]] Result<ImageInfo> read_frame_header(Bytes bytes, std::size_t pos,
                                                  std::uint32_t length) {
    constexpr MediaType type = MediaType::jpeg;
    if (length < 8) {
        return malformed(type, "frame header too short");
    }
    const std::optional<std::uint32_t> height = read_be(bytes, pos + 3, 2);
    const std::optional<std::uint32_t> width = read_be(bytes, pos + 5, 2);
    if (!height || !width) {
        return truncated(type);
    }
    return make_info(type, *width, *height);
}

[[nodiscard]] Result<ImageInfo> sniff_jpeg(Bytes bytes) {
    constexpr MediaType type = MediaType::jpeg;
    // The walk is bounded: every iteration advances `pos` by at least 2 bytes, and every read is
    // checked, so it ends at the input's end at the latest. `pos` stays below bytes.size() +
    // 0x10000 (at most one unchecked segment length past the end), so the additions cannot wrap.
    std::size_t pos = 2; // after SOI (FF D8)
    while (true) {
        const Result<JpegMarker> marker = read_marker(bytes, pos);
        if (!marker) {
            return std::unexpected(marker.error());
        }
        pos = marker->next;
        const std::uint32_t code = marker->code;
        if (is_standalone(code)) {
            continue;
        }
        if (code == 0x00 || code == 0xD8) {
            return malformed(type, "unexpected marker");
        }
        if (code == 0xD9 || code == 0xDA) {
            return malformed(type, "no frame header before the image data");
        }
        const std::optional<std::uint32_t> length = read_be(bytes, pos, 2);
        if (!length) {
            return truncated(type);
        }
        if (*length < 2) {
            return malformed(type, "segment length below 2");
        }
        if (is_start_of_frame(code)) {
            return read_frame_header(bytes, pos, *length);
        }
        pos += *length; // the length counts itself; <= 0xFFFF, so no wrap
    }
}

// ---- WebP: RIFF container, "WEBP", then one of the VP8, VP8L or VP8X chunks ------------------

[[nodiscard]] Result<ImageInfo> sniff_webp(Bytes bytes) {
    constexpr MediaType type = MediaType::webp;
    constexpr std::size_t payload = 20; // after "RIFF" size "WEBP" fourcc size
    const std::optional<std::uint32_t> chunk_size = read_le(bytes, 16, 4);
    if (!chunk_size) {
        return truncated(type);
    }
    if (matches(bytes, 12, "VP8 ")) {
        // Lossy: 3-byte frame tag (bit 0 clear on a key frame), start code 9D 01 2A, then 14-bit
        // width and height (the top 2 bits of each are a scaling code).
        if (*chunk_size < 10) {
            return malformed(type, "VP8 chunk too short");
        }
        const std::optional<std::uint32_t> tag = read_le(bytes, payload, 3);
        const std::optional<std::uint32_t> start = read_be(bytes, payload + 3, 3);
        const std::optional<std::uint32_t> width = read_le(bytes, payload + 6, 2);
        const std::optional<std::uint32_t> height = read_le(bytes, payload + 8, 2);
        if (!tag || !start || !width || !height) {
            return truncated(type);
        }
        if ((*tag & 1U) != 0U || *start != 0x9D012AU) {
            return malformed(type, "VP8 data is not a key frame with a start code");
        }
        return make_info(type, *width & 0x3FFFU, *height & 0x3FFFU);
    }
    if (matches(bytes, 12, "VP8L")) {
        // Lossless: signature 0x2F, then 14 bits width - 1, 14 bits height - 1, 1 bit alpha and
        // a 3-bit version that must be 0.
        if (*chunk_size < 5) {
            return malformed(type, "VP8L chunk too short");
        }
        const std::optional<std::uint32_t> signature = read_be(bytes, payload, 1);
        const std::optional<std::uint32_t> bits = read_le(bytes, payload + 1, 4);
        if (!signature || !bits) {
            return truncated(type);
        }
        if (*signature != 0x2FU || (*bits >> 29U) != 0U) {
            return malformed(type, "bad VP8L signature or version");
        }
        return make_info(type, (*bits & 0x3FFFU) + 1U, ((*bits >> 14U) & 0x3FFFU) + 1U);
    }
    if (matches(bytes, 12, "VP8X")) {
        // Extended: 4 bytes of flags and reserved bits, then the canvas width - 1 and height - 1,
        // 24-bit little-endian each.
        if (*chunk_size < 10) {
            return malformed(type, "VP8X chunk too short");
        }
        const std::optional<std::uint32_t> width = read_le(bytes, payload + 4, 3);
        const std::optional<std::uint32_t> height = read_le(bytes, payload + 7, 3);
        if (!width || !height) {
            return truncated(type);
        }
        return make_info(type, *width + 1U, *height + 1U);
    }
    return malformed(type, "unknown first chunk");
}

} // namespace

Result<ImageInfo> sniff_image(std::span<const std::byte> bytes) {
    if (matches(bytes, 0, png_signature)) {
        return sniff_png(bytes);
    }
    if (matches(bytes, 0, "GIF87a") || matches(bytes, 0, "GIF89a")) {
        return sniff_gif(bytes);
    }
    if (matches(bytes, 0, "\xFF\xD8")) {
        return sniff_jpeg(bytes);
    }
    if (matches(bytes, 0, "RIFF") && matches(bytes, 8, "WEBP")) {
        return sniff_webp(bytes);
    }
    if (matches(bytes, 0, "RIFF") && bytes.size() < 12) {
        return truncated(MediaType::webp); // "RIFF" but the form type is cut off
    }
    return image_error("unsupported image type");
}

std::string_view media_type_name(MediaType type) noexcept {
    switch (type) {
    case MediaType::jpeg:
        return "image/jpeg";
    case MediaType::png:
        return "image/png";
    case MediaType::gif:
        return "image/gif";
    case MediaType::webp:
        return "image/webp";
    }
    return "application/octet-stream"; // only reachable through an out-of-range enum value
}

std::string_view file_extension(MediaType type) noexcept {
    switch (type) {
    case MediaType::jpeg:
        return "jpg";
    case MediaType::png:
        return "png";
    case MediaType::gif:
        return "gif";
    case MediaType::webp:
        return "webp";
    }
    return "bin"; // only reachable through an out-of-range enum value
}

} // namespace mangadex_dl
