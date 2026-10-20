// SPDX-License-Identifier: GPL-3.0-or-later
//
// Inspection of an EPUB already in the output directory: is it ours, for this book, and unchanged
// since we wrote it (HeldBook), or foreign (and why)? Plus validated reads of one held page image.
// Everything read from the file is untrusted and never appears in a message. Frozen interface
// (skip-existing plan T1); implemented in src/core/existing_epub.cpp.
#pragma once

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/image.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// Why an existing file is left alone.
enum class ForeignReason {
    not_regular,              ///< a directory, symlink, FIFO, socket or device
    unreadable,               ///< EACCES, EPERM or EIO (InspectFailure::error holds the errno)
    not_ours,                 ///< no valid mangadex-dl manifest, or the archive disagrees with it
    other_book,               ///< the manifest is for another identifier
    changed_by_other_program, ///< content.opf no longer matches the manifest's opf_crc32
    newer_format,             ///< the manifest format is newer than this build reads
    changed_during_update,    ///< the file was replaced or modified while it was being rebuilt
};

/// The reason as a message fragment ("not a regular file", ...). Static storage, never dangles.
[[nodiscard]] std::string_view foreign_reason_text(ForeignReason reason) noexcept;

/// What the user can do about it ("pass --overwrite to replace it", ...). Static storage.
[[nodiscard]] std::string_view foreign_remedy(ForeignReason reason) noexcept;

struct InspectFailure {
    ForeignReason reason = ForeignReason::not_ours;
    int error = 0; ///< the errno for unreadable; 0 otherwise

    friend bool operator==(const InspectFailure&, const InspectFailure&) = default;
};

/// One referenced image entry, validated against the central directory and its local header.
struct HeldEntry {
    std::string name{};               ///< the zip entry name (is_held_image_entry_name)
    MediaType type = MediaType::jpeg; ///< from the name's extension
    std::uint32_t crc32 = 0;
    std::uint32_t size = 0;        ///< compressed == uncompressed (STORED), 0 < size <= 32 MiB
    std::uint64_t data_offset = 0; ///< the first data byte; data ends before the central directory

    friend bool operator==(const HeldEntry&, const HeldEntry&) = default;
};

/// An existing file that is ours. Owns copies of everything (no reference to the source).
struct HeldBook {
    BookManifest manifest{};
    std::vector<std::vector<HeldEntry>> pages{}; ///< parallel to manifest.chapters, page order
    std::optional<HeldEntry> cover{};            ///< set exactly when manifest.cover is

    friend bool operator==(const HeldBook&, const HeldBook&) = default;
};

/// Classifies the file behind `source`: the zip structure, the manifest, the identifier (must
/// equal `expected_identifier`), the OPF CRC, then every referenced image entry. Reads only the
/// central directory, the manifest and local headers, never page data. `source` is only used
/// during the call.
[[nodiscard]] std::expected<HeldBook, InspectFailure>
inspect_existing(ReadSource& source, std::string_view expected_identifier);

enum class PageFault {
    content, ///< CRC mismatch, not an accepted image, or the type disagrees with the extension
    read,    ///< the source failed (io or short_read)
};

/// One held image, read and validated. Owns its bytes.
struct HeldPage {
    std::vector<std::byte> bytes{};
    ImageInfo info{};

    friend bool operator==(const HeldPage&, const HeldPage&) = default;
};

/// Reads `entry` (from a HeldBook inspected through the same `source`) into an exact-size buffer,
/// checks its CRC, sniffs it, and requires the sniffed type's file_extension to equal the entry
/// name's extension.
[[nodiscard]] std::expected<HeldPage, PageFault> read_held_page(ReadSource& source,
                                                                const HeldEntry& entry);

} // namespace mangadex_dl
