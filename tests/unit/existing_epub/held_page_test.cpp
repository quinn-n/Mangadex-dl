// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/existing_epub/held_page_test.cpp: read_held_page (skip-existing plan T9, spec
// "Copying pages" and criterion 5's content and copy-time faults). Content faults (CRC, not an
// image, a side out of range, a type that disagrees with the extension) are PageFault::content;
// source faults (EIO, a 0 return, a range outside the file) are PageFault::read. Fact-check V7:
// a held page sniffs to the same ImageInfo as the downloaded bytes, for all four fixture formats.
// Each hostile page is built into an archive that inspect_existing accepts, because inspection
// never reads page data.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "support/fake_read_source.hpp"
#include "support/held_book_builder.hpp"
#include "support/mdx_fixture.hpp"
#include "support/read_only_fd.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_forge.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace zip = mangadex_dl::test::zip;
namespace forge = mangadex_dl::test::forge;
namespace held = mangadex_dl::test::held;
using held::assembled;
namespace mdx = mangadex_dl::test::mdx;
using held::ImageKind;
using mangadex_dl::test::FakeFault;
using mangadex_dl::test::FakeReadSource;
using mangadex_dl::test::open_read_only;
using mangadex_dl::test::TempDir;

/// A one-page book whose page is `name` holding `data` (the CRC in the archive matches it).
[[nodiscard]] held::BookParts one_page_book(const std::string& name, const forge::Bytes& data) {
    held::BookParts parts =
        held::sample_book(held::BookShape{.chapters = {{ImageKind::png}}, .cover = false});
    forge::ForgeEntry& page = held::member(parts, held::page_entry(0, "png"));
    page.name = name;
    page.data = data;
    parts.manifest.chapters[0].images[0] = name;
    return parts;
}

/// Inspects `bytes` (which must succeed) and returns the held book.
[[nodiscard]] md::HeldBook inspected(const forge::Bytes& bytes) {
    FakeReadSource source{bytes};
    std::expected<md::HeldBook, md::InspectFailure> book =
        md::inspect_existing(source, held::sample_identifier);
    REQUIRE(book.has_value());
    return std::move(*book);
}

/// The only page of a one-page book.
[[nodiscard]] md::HeldEntry only_page(const md::HeldBook& book) {
    REQUIRE(book.pages.size() == 1);
    REQUIRE(book.pages[0].size() == 1);
    return book.pages[0][0];
}

/// read_held_page on a fresh fake source over `bytes`.
[[nodiscard]] std::expected<md::HeldPage, md::PageFault> read_page(const forge::Bytes& bytes,
                                                                   const md::HeldEntry& entry) {
    FakeReadSource source{bytes};
    return md::read_held_page(source, entry);
}

/// The fault read_held_page reports for the only page of a one-page book `name` holding `data`.
[[nodiscard]] md::PageFault fault_of_page(const std::string& name, const forge::Bytes& data) {
    const forge::Bytes bytes = assembled(one_page_book(name, data));
    const md::HeldEntry entry = only_page(inspected(bytes));
    const std::expected<md::HeldPage, md::PageFault> page = read_page(bytes, entry);
    REQUIRE_FALSE(page.has_value());
    return page.error();
}

/// A fixture PNG whose IHDR claims `width` x `height` (32-bit, so beyond max_image_side too).
[[nodiscard]] forge::Bytes png_with_sides(std::uint32_t width, std::uint32_t height) {
    forge::Bytes bytes = held::image_data(ImageKind::png);
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint32_t shift = 8U * (3U - static_cast<std::uint32_t>(i));
        forge::set_byte(bytes, 16 + i, static_cast<std::byte>((width >> shift) & 0xFFU));
        forge::set_byte(bytes, 20 + i, static_cast<std::byte>((height >> shift) & 0xFFU));
    }
    return bytes;
}

} // namespace

TEST_CASE("read_held_page returns every page and the cover, sniffed as when downloaded (V7)",
          "[existing_epub][held_page]") {
    const held::BookParts parts = held::sample_book();
    const forge::Bytes bytes = assembled(parts);
    const md::HeldBook book = inspected(bytes);
    const std::expected<zip::Archive, std::string> archive = zip::read_archive(bytes);
    REQUIRE(archive.has_value());

    std::vector<md::HeldEntry> entries;
    for (const std::vector<md::HeldEntry>& chapter : book.pages) {
        entries.insert(entries.end(), chapter.begin(), chapter.end());
    }
    REQUIRE(book.cover.has_value());
    if (book.cover.has_value()) {
        entries.push_back(*book.cover);
    }
    REQUIRE(entries.size() == 5);

    FakeReadSource source{bytes};
    for (const md::HeldEntry& entry : entries) {
        CAPTURE(entry.name);
        const std::expected<md::HeldPage, md::PageFault> page = md::read_held_page(source, entry);
        REQUIRE(page.has_value());
        const zip::Entry* const member = archive->find(entry.name);
        REQUIRE(member != nullptr);
        CHECK(page->bytes == member->data);
        // The same ImageInfo the download path sniffs from the same bytes.
        const md::Result<md::ImageInfo> downloaded = md::sniff_image(member->data);
        REQUIRE(downloaded.has_value());
        CHECK(page->info == *downloaded);
        CHECK(page->info.type == entry.type);
        CHECK(entry.name.ends_with(std::string{"."} +
                                   std::string{md::file_extension(page->info.type)}));
    }

    SECTION("for every fixture format, against the fixture's own bytes") {
        for (const ImageKind kind :
             {ImageKind::png, ImageKind::gif, ImageKind::jpeg_header, ImageKind::webp_header}) {
            CAPTURE(mdx::image_extension(kind));
            const forge::Bytes data = held::image_data(kind, 300, 41);
            const forge::Bytes one =
                assembled(one_page_book(held::page_entry(0, mdx::image_extension(kind)), data));
            const std::expected<md::HeldPage, md::PageFault> page =
                read_page(one, only_page(inspected(one)));
            REQUIRE(page.has_value());
            const md::Result<md::ImageInfo> downloaded = md::sniff_image(data);
            REQUIRE(downloaded.has_value());
            CHECK(page->info == *downloaded);
            CHECK(page->info.width == 300);
            CHECK(page->info.height == 41);
            CHECK(page->bytes == data);
        }
    }
}

TEST_CASE("Content faults: CRC, not an image, sides, type versus extension",
          "[existing_epub][held_page]") {
    using md::PageFault;
    const std::string png_name = held::page_entry(0, "png");

    SECTION("a CRC mismatch (inspection still accepts the file: it never reads page data)") {
        const held::BookParts parts = one_page_book(png_name, held::image_data(ImageKind::png));
        forge::Bytes bytes = assembled(parts);
        const md::HeldEntry entry = only_page(inspected(bytes));
        forge::flip_byte(bytes, entry.data_offset + entry.size - 1, std::byte{0x01});
        const md::HeldEntry again = only_page(inspected(bytes));
        CHECK(again == entry);
        const std::expected<md::HeldPage, md::PageFault> page = read_page(bytes, again);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == PageFault::content);
    }
    SECTION("data shifted by a local extra field fails its CRC") {
        const held::BookParts parts = held::sample_book();
        forge::Bytes bytes = assembled(parts);
        const std::expected<forge::Layout, std::string> where = forge::layout_of(bytes);
        REQUIRE(where.has_value());
        const forge::EntryLayout& first = where->at(png_name);
        forge::put_le16(bytes, first.local_header + forge::local_field::extra_length, 1);
        const md::HeldBook book = inspected(bytes);
        const md::HeldEntry& entry = book.pages[0][0];
        CHECK(entry.data_offset == first.data + 1);
        const std::expected<md::HeldPage, md::PageFault> page = read_page(bytes, entry);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == PageFault::content);
    }
    SECTION("non-image bytes") {
        CHECK(fault_of_page(png_name, held::image_data(ImageKind::garbage)) == PageFault::content);
        CHECK(fault_of_page(png_name, held::image_data(ImageKind::truncated_png)) ==
              PageFault::content);
        CHECK(fault_of_page(png_name, forge::text_bytes("x")) == PageFault::content);
    }
    SECTION("a side of 0 or 65536") {
        CHECK(fault_of_page(png_name, png_with_sides(0, 12)) == PageFault::content);
        CHECK(fault_of_page(png_name, png_with_sides(8, 0)) == PageFault::content);
        CHECK(fault_of_page(png_name, png_with_sides(65536, 12)) == PageFault::content);
        CHECK(fault_of_page(png_name, png_with_sides(8, 65536)) == PageFault::content);
    }
    SECTION("a side of 65535 is accepted (the control)") {
        const forge::Bytes bytes =
            assembled(one_page_book(png_name, png_with_sides(md::max_image_side, 1)));
        const std::expected<md::HeldPage, md::PageFault> page =
            read_page(bytes, only_page(inspected(bytes)));
        REQUIRE(page.has_value());
        CHECK(page->info.width == md::max_image_side);
    }
    SECTION("an extension that disagrees with the sniffed type") {
        CHECK(fault_of_page(png_name, held::image_data(ImageKind::gif)) == PageFault::content);
        CHECK(fault_of_page(held::page_entry(0, "jpg"), held::image_data(ImageKind::png)) ==
              PageFault::content);
        CHECK(fault_of_page(held::page_entry(0, "webp"),
                            held::image_data(ImageKind::jpeg_header)) == PageFault::content);
        CHECK(fault_of_page(held::page_entry(0, "gif"), held::image_data(ImageKind::webp_header)) ==
              PageFault::content);
        CHECK(fault_of_page("OEBPS/images/cover.png", held::image_data(ImageKind::gif)) ==
              PageFault::content);
    }
}

TEST_CASE("Hand-made entries are checked before allocating", "[existing_epub][held_page]") {
    const forge::Bytes bytes = assembled(held::sample_book());
    const md::HeldBook book = inspected(bytes);
    const md::HeldEntry good = book.pages[0][0];

    SECTION("size 0 and size above max_image_bytes read nothing") {
        for (const std::uint32_t size :
             {std::uint32_t{0}, static_cast<std::uint32_t>(md::max_image_bytes + 1)}) {
            CAPTURE(size);
            md::HeldEntry entry = good;
            entry.size = size;
            FakeReadSource source{bytes};
            const std::expected<md::HeldPage, md::PageFault> page =
                md::read_held_page(source, entry);
            REQUIRE_FALSE(page.has_value());
            CHECK(page.error() == md::PageFault::content);
            CHECK(source.reads() == 0);
        }
    }
    SECTION("a recorded type that disagrees with the name") {
        md::HeldEntry entry = good;
        entry.type = md::MediaType::gif; // the name says png, and so do the bytes
        const std::expected<md::HeldPage, md::PageFault> page = read_page(bytes, entry);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == md::PageFault::content);
    }
    SECTION("a name without an extension") {
        md::HeldEntry entry = good;
        entry.name = "OEBPS/images/p00001";
        const std::expected<md::HeldPage, md::PageFault> page = read_page(bytes, entry);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == md::PageFault::content);
    }
}

TEST_CASE("Read faults: EIO, a 0 return and a range outside the file are read faults",
          "[existing_epub][held_page]") {
    const forge::Bytes bytes = assembled(held::sample_book());
    const md::HeldEntry entry = inspected(bytes).pages[1][0];

    SECTION("EIO") {
        FakeReadSource source{bytes};
        source.inject({.at_read = 0, .kind = FakeFault::io, .error = EIO});
        const std::expected<md::HeldPage, md::PageFault> page = md::read_held_page(source, entry);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == md::PageFault::read);
    }
    SECTION("a 0 return before the page is complete") {
        FakeReadSource source{bytes};
        source.inject({.at_read = 0, .kind = FakeFault::zero});
        const std::expected<md::HeldPage, md::PageFault> page = md::read_held_page(source, entry);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == md::PageFault::read);
    }
    SECTION("a file that shrank after inspection") {
        FakeReadSource source{forge::truncated(bytes, entry.data_offset + 3)};
        source.report_size(bytes.size());
        const std::expected<md::HeldPage, md::PageFault> page = md::read_held_page(source, entry);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == md::PageFault::read);
    }
    SECTION("an offset past the end") {
        md::HeldEntry moved = entry;
        moved.data_offset = bytes.size() - 1;
        const std::expected<md::HeldPage, md::PageFault> page = read_page(bytes, moved);
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error() == md::PageFault::read);
    }
    SECTION("a positive short count is continued, not failed") {
        FakeReadSource source{bytes};
        source.inject({.at_read = 0, .kind = FakeFault::short_count, .count = 3});
        const std::expected<md::HeldPage, md::PageFault> page = md::read_held_page(source, entry);
        REQUIRE(page.has_value());
        CHECK(page->bytes.size() == entry.size);
        CHECK(source.transfers() > 1);
    }
}

TEST_CASE("read_held_page on a real file, and after the file changed in place",
          "[existing_epub][held_page][fs]") {
    std::expected<TempDir, std::string> dir = TempDir::create();
    REQUIRE(dir.has_value());
    const forge::Bytes bytes = assembled(held::sample_book());
    const fs::path path = dir->path() / "book.epub";
    REQUIRE(forge::write_file(path, bytes));

    const md::UniqueFd fd = open_read_only(path);
    REQUIRE(fd.valid());
    md::FdReadSource source{fd, bytes.size()};
    const std::expected<md::HeldBook, md::InspectFailure> book =
        md::inspect_existing(source, held::sample_identifier);
    REQUIRE(book.has_value());
    for (const std::vector<md::HeldEntry>& chapter : book->pages) {
        for (const md::HeldEntry& entry : chapter) {
            CAPTURE(entry.name);
            CHECK(md::read_held_page(source, entry).has_value());
        }
    }

    // Another program rewrites one byte of a page between the pre-pass and the copy.
    const md::HeldEntry& entry = book->pages[0][1];
    REQUIRE(forge::patch_file_byte(path, entry.data_offset, std::byte{0x00}));
    const std::expected<md::HeldPage, md::PageFault> page = md::read_held_page(source, entry);
    REQUIRE_FALSE(page.has_value());
    CHECK(page.error() == md::PageFault::content);
}
