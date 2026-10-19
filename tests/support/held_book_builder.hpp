#pragma once
// tests/support/held_book_builder.hpp: small EPUB-shaped archives that carry a mangadex-dl
// manifest, for inspection and held-page tests (skip-existing plan T9). Header-only.
//
//     namespace held = mangadex_dl::test::held;
//     held::BookParts parts = held::sample_book();      // 2 chapters, 4 formats, a cover
//     parts.manifest.identifier = "urn:other";          // edit the record or the members ...
//     const auto bytes = held::assemble(parts);         // ... then build the archive
//     REQUIRE(bytes.has_value());
//     mangadex_dl::test::FakeReadSource source{*bytes};
//
// The archive is written by core ZipWriter (through zip_forge) and the manifest by core
// manifest_to_json, so the bytes have exactly the shape the tool writes: `mimetype` first, the
// manifest last, every member STORED with no flags. It does not use EpubWriter (the archive holds
// only what inspection looks at), so these tests do not depend on the EPUB builder.
// - opf_crc32 is the independent reference CRC (zip_reader) of the content.opf member, so a test
//   that edits that member without updating the manifest gets a stale CRC.
// - Images are the mdx fixture's sniffable bytes (real PNG/GIF, header-only JPEG/WebP).
// - The helpers throw std::out_of_range for a member that does not exist (a test bug, which
//   Catch2 reports as an unexpected exception).

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/model.hpp"
#include "support/mdx_fixture.hpp"
#include "support/zip_forge.hpp"
#include "support/zip_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::test::held {

using forge::Bytes;
using mdx::ImageKind;

/// The identifier every sample book is built for.
inline constexpr std::string_view sample_identifier =
    "urn:mangadex:11111111-0001-4000-8000-000000000001:en:v1";
/// The content.opf member of every sample book (its bytes only matter for opf_crc32).
inline constexpr std::string_view sample_opf =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package version=\"3.0\"/>\n";
inline constexpr std::string_view sample_cover_entry = "OEBPS/images/cover.png";

/// What sample_book builds: one inner vector per chapter, one image kind per page.
struct BookShape {
    std::vector<std::vector<ImageKind>> chapters = {
        {ImageKind::png, ImageKind::gif},
        {ImageKind::jpeg_header, ImageKind::webp_header},
    };
    bool cover = true;
};

/// Everything one test book holds. Edit the parts, then assemble().
struct BookParts {
    BookManifest manifest{};
    /// Every member before the manifest, in archive order (`mimetype` first).
    std::vector<forge::ForgeEntry> entries{};
    /// When set, the manifest member holds these bytes instead of manifest_to_json(manifest).
    std::optional<Bytes> manifest_bytes{};
    /// False: the archive has no manifest member at all.
    bool with_manifest = true;
};

/// The sniffable bytes of one fixture image (8x12 unless given).
[[nodiscard]] inline Bytes image_data(ImageKind kind, std::uint16_t width = 8,
                                      std::uint16_t height = 12) {
    return forge::text_bytes(
        mdx::image_bytes(mdx::PageImage{.kind = kind, .width = width, .height = height}));
}

/// `OEBPS/images/pNNNNN.<ext>` for page index `index` (0-based), as the EPUB builder names it.
[[nodiscard]] inline std::string page_entry(std::size_t index, std::string_view extension) {
    return std::format("OEBPS/images/p{:05}.{}", index + 1, extension);
}

/// A valid UUID that differs per `n`.
[[nodiscard]] inline std::string sample_uuid(std::size_t n) {
    return std::format("abcdef01-2345-4678-89ab-{:012x}", n);
}

/// A book of `shape`: mimetype, container.xml, the cover, the page images, content.opf, and a
/// manifest that describes them (opf_crc32 matches).
[[nodiscard]] inline BookParts sample_book(const BookShape& shape = {}) {
    BookParts parts;
    parts.manifest.identifier = std::string{sample_identifier};
    parts.entries.push_back(
        {.name = "mimetype", .data = forge::text_bytes("application/epub+zip")});
    parts.entries.push_back(
        {.name = "META-INF/container.xml", .data = forge::text_bytes("<container/>")});
    if (shape.cover) {
        parts.entries.push_back(
            {.name = std::string{sample_cover_entry}, .data = image_data(ImageKind::png, 6, 9)});
        parts.manifest.cover =
            ManifestCover{.file_name = "cover-file.png", .entry = std::string{sample_cover_entry}};
    }
    std::size_t page = 0;
    for (std::size_t c = 0; c < shape.chapters.size(); ++c) {
        ManifestChapter chapter{
            .id = sample_uuid(c + 1),
            .volume = "1",
            .chapter = std::format("{}", c + 1),
            .title = std::format("Chapter {}", c + 1),
            .groups = {ScanGroup{.id = sample_uuid(100), .name = "Group A"}},
            .publish_at = "2024-01-01T00:00:00+00:00",
            .updated_at = "2024-01-02T00:00:00+00:00",
            .quality = ImageQuality::data,
            .images = {},
        };
        for (const ImageKind kind : shape.chapters[c]) {
            std::string name = page_entry(page, mdx::image_extension(kind));
            ++page;
            parts.entries.push_back({.name = name, .data = image_data(kind)});
            chapter.images.push_back(std::move(name));
        }
        parts.manifest.chapters.push_back(std::move(chapter));
    }
    const Bytes opf = forge::text_bytes(sample_opf);
    parts.manifest.opf_crc32 = zip::reference_crc32(opf);
    parts.entries.push_back({.name = std::string{opf_entry_name}, .data = opf});
    return parts;
}

/// The archive bytes of `parts` (the manifest member last).
[[nodiscard]] inline std::expected<Bytes, std::string> assemble(const BookParts& parts) {
    std::vector<forge::ForgeEntry> entries = parts.entries;
    if (parts.with_manifest) {
        Bytes manifest = parts.manifest_bytes.has_value()
                             ? *parts.manifest_bytes
                             : forge::text_bytes(manifest_to_json(parts.manifest));
        entries.push_back({.name = std::string{manifest_entry_name}, .data = std::move(manifest)});
    }
    return forge::build_archive(entries);
}

/// The member called `name` (mutable). Throws std::out_of_range when there is none.
[[nodiscard]] inline forge::ForgeEntry& member(BookParts& parts, std::string_view name) {
    for (forge::ForgeEntry& entry : parts.entries) {
        if (entry.name == name) {
            return entry;
        }
    }
    throw std::out_of_range(std::format("no member '{}' in the book", name));
}

/// Removes the member called `name`. Throws std::out_of_range when there is none.
inline void remove_member(BookParts& parts, std::string_view name) {
    const auto erased = std::erase_if(
        parts.entries, [name](const forge::ForgeEntry& entry) { return entry.name == name; });
    if (erased == 0) {
        throw std::out_of_range(std::format("no member '{}' in the book", name));
    }
}

/// Every referenced image name in manifest order (pages, then the cover).
[[nodiscard]] inline std::vector<std::string> referenced_names(const BookManifest& manifest) {
    std::vector<std::string> names;
    for (const ManifestChapter& chapter : manifest.chapters) {
        names.insert(names.end(), chapter.images.begin(), chapter.images.end());
    }
    if (manifest.cover.has_value()) {
        names.push_back(manifest.cover->entry);
    }
    return names;
}

} // namespace mangadex_dl::test::held
