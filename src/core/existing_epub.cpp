// SPDX-License-Identifier: GPL-3.0-or-later
//
// inspect_existing and read_held_page (skip-existing plan T9; spec "Inspection" steps 2-6 and
// "Copying pages").
//
// Every byte of the existing file is untrusted. The rules that keep this safe:
// - All structure comes from ZipIndex (bounded central-directory reader) and locate_entry_data
//   (local header agrees with the CD, data lies within [0, central directory offset)).
// - Sizes are checked before allocating: the manifest against max_manifest_bytes, an image
//   against max_image_bytes. Each buffer is an exact-size owned vector or string.
// - The manifest text lives only inside load_manifest (freed once parsed), and parse_manifest
//   applies its own byte, depth and separator caps before nlohmann sees it.
// - ZipIndex accepts flag bits outside its reject mask (0x0800, the UTF-8 flag, for one), so
//   flags == 0 is checked here for the manifest and every referenced image (the writer never
//   sets a flag).
// - Nothing read from the file reaches a message: failures are an InspectFailure / PageFault
//   enum plus, for unreadable, the errno the read reported.
// - Results are bound to named locals before use; ZipIndex::find pointers never outlive the
//   index, and the HeldBook owns copies of everything it holds.
#include "mangadex_dl/existing_epub.hpp"

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/zip.hpp"
#include "mangadex_dl/zip_index.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// The first entry of every EPUB (OCF 3.3 §4.1.4).
constexpr std::string_view mimetype_entry_name = "mimetype";

/// Every media type a held image may have, for the extension lookup.
constexpr std::array<MediaType, 4> held_media_types{MediaType::jpeg, MediaType::png, MediaType::gif,
                                                    MediaType::webp};

[[nodiscard]] std::unexpected<InspectFailure> foreign(ForeignReason reason,
                                                      int error = 0) noexcept {
    return std::unexpected(InspectFailure{.reason = reason, .error = error});
}

/// A zip structure failure as the one reason the spec gives for it: an I/O error makes the file
/// unreadable (with its errno); every other fault means it is not an EPUB made by mangadex-dl.
[[nodiscard]] std::unexpected<InspectFailure> from_zip(const ZipError& error) noexcept {
    switch (error.fault) {
    case ZipFault::read_error:
        return foreign(ForeignReason::unreadable, error.error);
    case ZipFault::short_read:
    case ZipFault::size_out_of_range:
    case ZipFault::no_end_record:
    case ZipFault::unsupported:
    case ZipFault::bad_directory:
    case ZipFault::duplicate_name:
    case ZipFault::bad_local_header:
    case ZipFault::data_out_of_bounds:
        return foreign(ForeignReason::not_ours);
    }
    // Only reachable through an out-of-range enum value.
    return foreign(ForeignReason::not_ours);
}

/// A source failure during inspection, mapped as from_zip maps read_error / short_read.
[[nodiscard]] std::unexpected<InspectFailure> from_read(const ReadError& error) noexcept {
    switch (error.fault) {
    case ReadFault::io:
        return foreign(ForeignReason::unreadable, error.error);
    case ReadFault::short_read:
        return foreign(ForeignReason::not_ours);
    }
    return foreign(ForeignReason::not_ours);
}

/// What follows the last '.' of `name` (a view into `name`), or empty when there is none.
[[nodiscard]] std::string_view extension_of(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    return name.substr(dot + 1);
}

/// The media type whose file_extension (the function the EPUB builder names entries with) is the
/// extension of `name`.
[[nodiscard]] std::optional<MediaType> media_type_of(std::string_view name) noexcept {
    const std::string_view extension = extension_of(name);
    if (extension.empty()) {
        return std::nullopt;
    }
    for (const MediaType type : held_media_types) {
        if (file_extension(type) == extension) {
            return type;
        }
    }
    return std::nullopt;
}

/// STORED, no flag bits at all, and the same size both ways: the only shape ZipWriter writes.
[[nodiscard]] bool plain_stored(const ZipEntryRecord& record) noexcept {
    return record.method == 0 && record.flags == 0 &&
           record.compressed_size == record.uncompressed_size;
}

/// Step 3 and 4: the manifest entry, validated against the CD and its local header, read into an
/// exact-size buffer (freed on return), CRC-checked, then parsed.
[[nodiscard]] std::expected<BookManifest, InspectFailure> load_manifest(ReadSource& source,
                                                                        const ZipIndex& index) {
    const ZipEntryRecord* const record = index.find(manifest_entry_name);
    if (record == nullptr) {
        return foreign(ForeignReason::not_ours);
    }
    // Before anything is allocated: u32 widened to size_t, compared against the 16 MiB cap.
    if (!plain_stored(*record) || std::size_t{record->uncompressed_size} > max_manifest_bytes) {
        return foreign(ForeignReason::not_ours);
    }
    const std::expected<std::uint64_t, ZipError> data_at =
        locate_entry_data(source, index, *record);
    if (!data_at) {
        return from_zip(data_at.error());
    }

    std::string text(std::size_t{record->uncompressed_size}, '\0');
    if (const std::expected<void, ReadError> got =
            source.read_exact(*data_at, std::as_writable_bytes(std::span{text}));
        !got) {
        return from_read(got.error());
    }
    if (crc32(std::as_bytes(std::span{text})) != record->crc32) {
        return foreign(ForeignReason::not_ours);
    }

    std::expected<BookManifest, ManifestFault> parsed = parse_manifest(text);
    if (!parsed) {
        switch (parsed.error()) {
        case ManifestFault::newer_format:
            return foreign(ForeignReason::newer_format);
        case ManifestFault::malformed:
            return foreign(ForeignReason::not_ours);
        }
        return foreign(ForeignReason::not_ours);
    }
    return std::move(*parsed);
}

/// True when no entry name is referenced twice (by two pages, or by a page and the cover). The
/// views borrow `manifest`, which outlives them (they die with this call).
[[nodiscard]] bool references_are_unique(const BookManifest& manifest) {
    std::size_t total = manifest.cover.has_value() ? std::size_t{1} : std::size_t{0};
    for (const ManifestChapter& chapter : manifest.chapters) {
        total += chapter.images.size(); // parse_manifest bounds the sum by zip_max_entries
    }
    std::vector<std::string_view> names;
    names.reserve(total);
    for (const ManifestChapter& chapter : manifest.chapters) {
        for (const std::string& image : chapter.images) {
            names.emplace_back(image);
        }
    }
    if (manifest.cover.has_value()) {
        names.emplace_back(manifest.cover->entry);
    }
    std::ranges::sort(names);
    return std::ranges::adjacent_find(names) == names.end();
}

/// Step 6 for one referenced name: the image pattern, present in the CD, STORED with no flags,
/// 0 < size <= max_image_bytes, and its local header and data validated. Reads one local header,
/// never the data.
[[nodiscard]] std::expected<HeldEntry, InspectFailure>
held_entry(ReadSource& source, const ZipIndex& index, std::string_view name) {
    if (!is_held_image_entry_name(name)) {
        return foreign(ForeignReason::not_ours);
    }
    const std::optional<MediaType> type = media_type_of(name);
    if (!type) {
        return foreign(ForeignReason::not_ours);
    }
    const ZipEntryRecord* const record = index.find(name);
    if (record == nullptr || !plain_stored(*record) || record->compressed_size == 0 ||
        std::size_t{record->compressed_size} > max_image_bytes) {
        return foreign(ForeignReason::not_ours);
    }
    const std::expected<std::uint64_t, ZipError> data_at =
        locate_entry_data(source, index, *record);
    if (!data_at) {
        return from_zip(data_at.error());
    }
    return HeldEntry{
        .name = record->name,
        .type = *type,
        .crc32 = record->crc32,
        .size = record->compressed_size,
        .data_offset = *data_at,
    };
}

} // namespace

std::string_view foreign_reason_text(ForeignReason reason) noexcept {
    switch (reason) {
    case ForeignReason::not_regular:
        return "not a regular file";
    case ForeignReason::unreadable:
        return "cannot be read";
    case ForeignReason::not_ours:
        return "not an EPUB made by mangadex-dl";
    case ForeignReason::other_book:
        return "holds a different book";
    case ForeignReason::changed_by_other_program:
        return "changed by another program";
    case ForeignReason::newer_format:
        return "made by a newer mangadex-dl";
    case ForeignReason::changed_during_update:
        return "changed while it was being updated";
    }
    // Only reachable through an out-of-range enum value.
    return "not an EPUB made by mangadex-dl";
}

std::string_view foreign_remedy(ForeignReason reason) noexcept {
    // Plan Rollout D8.
    switch (reason) {
    case ForeignReason::not_regular:
    case ForeignReason::unreadable:
        return "rename or remove it, or use another DIR";
    case ForeignReason::not_ours:
    case ForeignReason::other_book:
        return "use another DIR or --title";
    case ForeignReason::changed_by_other_program:
    case ForeignReason::newer_format:
    case ForeignReason::changed_during_update:
        return "pass --overwrite to replace it";
    }
    // Only reachable through an out-of-range enum value.
    return "use another DIR or --title";
}

std::expected<HeldBook, InspectFailure> inspect_existing(ReadSource& source,
                                                         std::string_view expected_identifier) {
    // Step 2: the zip structure. The index is a named local: every pointer find() returns below
    // borrows it and dies before it does.
    const std::expected<ZipIndex, ZipError> indexed = ZipIndex::read(source);
    if (!indexed) {
        return from_zip(indexed.error());
    }
    const ZipIndex& index = *indexed;
    const std::vector<ZipEntryRecord>& entries = index.entries();
    if (entries.empty() || entries.front().name != mimetype_entry_name) {
        return foreign(ForeignReason::not_ours);
    }

    // Steps 3 and 4: the manifest.
    std::expected<BookManifest, InspectFailure> loaded = load_manifest(source, index);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    BookManifest& manifest = *loaded;

    // Step 5: identity. Only the CD CRC of content.opf is compared; its bytes are never read.
    if (manifest.identifier != expected_identifier) {
        return foreign(ForeignReason::other_book);
    }
    const ZipEntryRecord* const opf = index.find(opf_entry_name);
    if (opf == nullptr) {
        return foreign(ForeignReason::not_ours);
    }
    if (opf->crc32 != manifest.opf_crc32) {
        return foreign(ForeignReason::changed_by_other_program);
    }

    // Step 6: references, in the CD only (local headers are read, page data never).
    if (!references_are_unique(manifest)) {
        return foreign(ForeignReason::not_ours);
    }
    HeldBook book;
    // One element per manifest chapter, in order: HeldBook::pages.size() ==
    // manifest.chapters.size() on every successful return (callers index both with one index).
    book.pages.reserve(manifest.chapters.size());
    for (const ManifestChapter& chapter : manifest.chapters) {
        std::vector<HeldEntry> pages;
        pages.reserve(chapter.images.size());
        for (const std::string& image : chapter.images) {
            std::expected<HeldEntry, InspectFailure> entry = held_entry(source, index, image);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            pages.push_back(std::move(*entry));
        }
        book.pages.push_back(std::move(pages));
    }
    if (manifest.cover.has_value()) {
        std::expected<HeldEntry, InspectFailure> entry =
            held_entry(source, index, manifest.cover->entry);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        book.cover = std::move(*entry);
    }
    book.manifest = std::move(manifest);
    return book;
}

std::expected<HeldPage, PageFault> read_held_page(ReadSource& source, const HeldEntry& entry) {
    // An entry from inspect_existing already satisfies this; re-checked before allocating so a
    // hand-made HeldEntry cannot ask for an unbounded buffer.
    if (entry.size == 0 || std::size_t{entry.size} > max_image_bytes) {
        return std::unexpected(PageFault::content);
    }
    std::vector<std::byte> bytes(std::size_t{entry.size});
    if (const std::expected<void, ReadError> got = source.read_exact(entry.data_offset, bytes);
        !got) {
        return std::unexpected(PageFault::read);
    }
    if (crc32(bytes) != entry.crc32) {
        return std::unexpected(PageFault::content);
    }
    const Result<ImageInfo> info = sniff_image(bytes);
    if (!info) {
        return std::unexpected(PageFault::content);
    }
    // Fact-check V7: the sniffed type must name entries the way the builder does.
    if (file_extension(info->type) != extension_of(entry.name) || info->type != entry.type) {
        return std::unexpected(PageFault::content);
    }
    return HeldPage{.bytes = std::move(bytes), .info = *info};
}

} // namespace mangadex_dl
