// SPDX-License-Identifier: GPL-3.0-or-later
//
// Page-image identification from header bytes only (no decoding, no image library).
// Frozen interface (download plan T1); implemented in src/core/image.cpp.
#pragma once

#include "mangadex_dl/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mangadex_dl {

/// Largest accepted width or height, in pixels.
inline constexpr std::uint32_t max_image_side = 65535;

enum class MediaType {
    jpeg,
    png,
    gif,
    webp,
};

struct ImageInfo {
    MediaType type = MediaType::jpeg;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const ImageInfo&, const ImageInfo&) = default;
};

/// Identifies JPEG (SOFn), PNG (IHDR), GIF (logical screen) and WebP (VP8, VP8L, VP8X) and reads
/// the dimensions. Every read is bounds-checked against `bytes`. Errc::failure for an unknown or
/// truncated header, and for a side of 0 or above max_image_side.
[[nodiscard]] Result<ImageInfo> sniff_image(std::span<const std::byte> bytes);

/// "image/jpeg", "image/png", "image/gif" or "image/webp". The view points at a string literal
/// (static storage), so it never dangles.
[[nodiscard]] std::string_view media_type_name(MediaType type) noexcept;

/// "jpg", "png", "gif" or "webp" (no dot). Static storage, like media_type_name.
[[nodiscard]] std::string_view file_extension(MediaType type) noexcept;

} // namespace mangadex_dl
