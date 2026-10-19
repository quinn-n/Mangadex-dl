// SPDX-License-Identifier: GPL-3.0-or-later
//
// The per-book manifest: the record of what an EPUB made by mangadex-dl holds, stored as the last
// STORED zip entry `META-INF/mangadex-dl.json`. A re-run reads it to decide whether the file is up
// to date and which pages it can reuse. Frozen interface (skip-existing plan T1); implemented in
// src/core/book_manifest.cpp.
#pragma once

#include "mangadex_dl/model.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// The zip entry name. It lives in META-INF, so it is never listed in the OPF manifest.
inline constexpr std::string_view manifest_entry_name = "META-INF/mangadex-dl.json";
/// The `generator` value; any other value makes the file foreign.
inline constexpr std::string_view manifest_generator = "mangadex-dl";
/// The `format` this build writes and reads. A larger value is "made by a newer mangadex-dl".
inline constexpr std::uint64_t manifest_format = 1;
/// Largest manifest entry (checked before allocating), the same cap as an API response.
inline constexpr std::size_t max_manifest_bytes = std::size_t{16} * 1024 * 1024;
/// Largest string value inside a manifest, in bytes.
inline constexpr std::size_t max_manifest_string_bytes = 4096;
/// Deepest array/object nesting accepted by the pre-scan (a top-level `{}` is depth 1).
inline constexpr std::size_t max_manifest_depth = 8;
/// Most structural separators (`, : [ {` outside strings) accepted by the pre-scan, which bounds
/// the parsed document's memory (fact-check V16; the bound is justified in book_manifest.cpp).
inline constexpr std::size_t max_manifest_separators = std::size_t{1} << 21U;

/// The MangaDex cover a book was built with.
struct ManifestCover {
    std::string file_name{}; ///< the MangaDex cover file name (Cover::file_name)
    std::string entry{};     ///< its zip entry, `OEBPS/images/cover.<ext>`

    friend bool operator==(const ManifestCover&, const ManifestCover&) = default;
};

/// One chapter as held in the file, in reading order.
struct ManifestChapter {
    std::string id{}; ///< satisfies is_uuid()
    std::optional<std::string> volume{};
    std::optional<std::string> chapter{};
    std::string title{};
    std::vector<ScanGroup> groups{}; ///< ids satisfy is_uuid()
    std::string publish_at{};
    std::string updated_at{};
    ImageQuality quality = ImageQuality::data;
    std::vector<std::string> images{}; ///< zip entry names in page order (is_held_image_entry_name)

    friend bool operator==(const ManifestChapter&, const ManifestChapter&) = default;
};

/// The whole record. Owns all of its data.
struct BookManifest {
    std::string identifier{}; ///< equals the OPF dc:identifier
    std::uint32_t opf_crc32 = 0;
    std::optional<ManifestCover> cover{};
    std::vector<ManifestChapter> chapters{};

    friend bool operator==(const BookManifest&, const BookManifest&) = default;
};

/// Compact, deterministic JSON (fixed key order, no clock, no version string), without a
/// trailing newline. Invalid UTF-8 in strings is replaced by U+FFFD.
[[nodiscard]] std::string manifest_to_json(const BookManifest& manifest);

/// Why parse_manifest rejected a document.
enum class ManifestFault {
    malformed,    ///< anything that is not a valid format-1 manifest (never a throw)
    newer_format, ///< `format` is an unsigned integer above manifest_format
};

/// Parses untrusted manifest text: the byte, depth and separator caps first, then a non-throwing
/// parse and a type check of every field. Unknown keys are ignored. `json` is only read during
/// the call; the result owns copies.
[[nodiscard]] std::expected<BookManifest, ManifestFault> parse_manifest(std::string_view json);

/// True for `OEBPS/images/pNNNNN.<ext>` (five digits) and `OEBPS/images/cover.<ext>`, with `<ext>`
/// one of jpg, png, gif and webp: `^OEBPS/images/(p[0-9]{5}|cover)\.(jpg|png|gif|webp)$`.
[[nodiscard]] bool is_held_image_entry_name(std::string_view name) noexcept;

/// The manifest record of a planned chapter, with `images` left empty (the writer fills it).
[[nodiscard]] ManifestChapter manifest_chapter_from(const Chapter& chapter, ImageQuality quality);

/// A Chapter for a held record: `pages` is the image count and `language` is empty; the flags
/// (external, unavailable) are false.
[[nodiscard]] Chapter chapter_from(const ManifestChapter& record);

} // namespace mangadex_dl
