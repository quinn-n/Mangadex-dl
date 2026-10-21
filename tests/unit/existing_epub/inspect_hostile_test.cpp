// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/existing_epub/inspect_hostile_test.cpp: inspect_existing on hostile files
// (skip-existing plan T9, spec criteria 4 and 5's structural faults). Every row must classify as
// foreign with the exact reason (and errno), never crash, never allocate for a claimed size above
// its cap, and stay ASan/UBSan-clean: the zip structure, the manifest entry and its JSON, the
// identity checks, every referenced image entry, truncation at every length, read faults at every
// read index, and a fixed-seed mutation loop.
//
// Test inputs stay small (ASan memory under ctest -j): the largest is the ~2 MiB separator flood.

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/zip_index.hpp"
#include "support/fake_read_source.hpp"
#include "support/held_book_builder.hpp"
#include "support/zip_forge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace forge = mangadex_dl::test::forge;
namespace held = mangadex_dl::test::held;
using held::assembled;
using mangadex_dl::test::FakeFault;
using mangadex_dl::test::FakeReadSource;
using md::ForeignReason;

constexpr md::InspectFailure not_ours{.reason = ForeignReason::not_ours, .error = 0};

[[nodiscard]] forge::Layout layout(const forge::Bytes& bytes) {
    std::expected<forge::Layout, std::string> found = forge::layout_of(bytes);
    INFO((found ? std::string{} : found.error()));
    REQUIRE(found.has_value());
    return std::move(*found);
}

[[nodiscard]] std::expected<md::HeldBook, md::InspectFailure>
inspect(const forge::Bytes& bytes, std::string_view identifier = held::sample_identifier) {
    FakeReadSource source{bytes};
    return md::inspect_existing(source, identifier);
}

/// The failure inspect_existing reports for `bytes` (the test fails if it succeeds).
[[nodiscard]] md::InspectFailure failure_of(const forge::Bytes& bytes) {
    const std::expected<md::HeldBook, md::InspectFailure> result = inspect(bytes);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// The failure for the archive of `parts`.
[[nodiscard]] md::InspectFailure failure_of(const held::BookParts& parts) {
    return failure_of(assembled(parts));
}

/// True when ZipIndex accepts `bytes`: the row then exercises inspect_existing's own checks, not
/// the index's.
[[nodiscard]] bool index_accepts(const forge::Bytes& bytes) {
    FakeReadSource source{bytes};
    const std::expected<md::ZipIndex, md::ZipError> index = md::ZipIndex::read(source);
    return index.has_value();
}

/// Sets the flags of member `name` in both its local and its central header.
void set_flags(forge::Bytes& bytes, const forge::Layout& where, std::string_view name,
               std::uint16_t flags) {
    const forge::EntryLayout& entry = where.at(name);
    forge::put_le16(bytes, entry.local_header + forge::local_field::flags, flags);
    forge::put_le16(bytes, entry.central_header + forge::central_field::flags, flags);
}

/// Sets the method of member `name` in both headers.
void set_method(forge::Bytes& bytes, const forge::Layout& where, std::string_view name,
                std::uint16_t method) {
    const forge::EntryLayout& entry = where.at(name);
    forge::put_le16(bytes, entry.local_header + forge::local_field::method, method);
    forge::put_le16(bytes, entry.central_header + forge::central_field::method, method);
}

/// Sets both sizes of member `name` in its central header and, when `local`, in its local header.
void set_sizes(forge::Bytes& bytes, const forge::Layout& where, std::string_view name,
               std::uint32_t size, bool local) {
    const forge::EntryLayout& entry = where.at(name);
    forge::put_le32(bytes, entry.central_header + forge::central_field::compressed_size, size);
    forge::put_le32(bytes, entry.central_header + forge::central_field::uncompressed_size, size);
    if (local) {
        forge::put_le32(bytes, entry.local_header + forge::local_field::compressed_size, size);
        forge::put_le32(bytes, entry.local_header + forge::local_field::uncompressed_size, size);
    }
}

/// The JSON of `manifest`, with its leading `{"format":1,` replaced to give `format`.
[[nodiscard]] forge::Bytes manifest_with_format(const md::BookManifest& manifest,
                                                std::string_view format) {
    std::string json = md::manifest_to_json(manifest);
    constexpr std::string_view head = R"({"format":1,)";
    REQUIRE(json.starts_with(head));
    json.replace(0, head.size(), std::string{R"({"format":)"} + std::string{format} + ",");
    return forge::text_bytes(json);
}

/// The failure for `bytes` inspected for `identifier`.
[[nodiscard]] md::InspectFailure failure_for(const forge::Bytes& bytes,
                                             std::string_view identifier) {
    const std::expected<md::HeldBook, md::InspectFailure> result = inspect(bytes, identifier);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// The first page image of the sample book (chapter 0, page 0).
[[nodiscard]] std::string first_page_name() { return held::page_entry(0, "png"); }
/// The last page image of the sample book (chapter 1, page 1).
[[nodiscard]] std::string last_page_name() { return held::page_entry(3, "webp"); }

} // namespace

TEST_CASE("The zip structure decides not ours, or unreadable on an I/O error",
          "[existing_epub][hostile]") {
    const forge::Bytes good = assembled(held::sample_book());

    SECTION("an empty file") { CHECK(failure_of(forge::Bytes{}) == not_ours); }
    SECTION("random bytes") {
        forge::Bytes random(512);
        std::uint32_t x = 0x9E3779B9U;
        for (std::byte& b : random) {
            x = (x * 1664525U) + 1013904223U;
            b = static_cast<std::byte>(x >> 24U);
        }
        CHECK(failure_of(random) == not_ours);
    }
    SECTION("an archive with no entries") {
        const std::expected<forge::Bytes, std::string> empty = forge::build_archive({});
        REQUIRE(empty.has_value());
        CHECK(failure_of(*empty) == not_ours);
    }
    SECTION("mimetype is not the first entry") {
        held::BookParts parts = held::sample_book();
        std::swap(parts.entries[0], parts.entries[1]);
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("mimetype is missing") {
        held::BookParts parts = held::sample_book();
        held::remove_member(parts, "mimetype");
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("a 1-byte archive comment") {
        forge::Bytes bytes = good;
        const forge::Layout where = layout(good);
        forge::put_le16(bytes, where.end_record + forge::end_field::comment_length, 1);
        bytes.push_back(std::byte{'x'});
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("a ZIP64 entry-count sentinel") {
        forge::Bytes bytes = good;
        const forge::Layout where = layout(good);
        forge::put_le16(bytes, where.end_record + forge::end_field::disk_entries, 0xFFFF);
        forge::put_le16(bytes, where.end_record + forge::end_field::total_entries, 0xFFFF);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("a duplicate name") {
        // ZipWriter refuses duplicates, so the archive is built field by field.
        const held::BookParts parts = held::sample_book();
        std::vector<forge::RawEntry> raw;
        raw.reserve(parts.entries.size() + 2);
        for (const forge::ForgeEntry& entry : parts.entries) {
            raw.push_back({.name = entry.name, .data = entry.data});
        }
        raw.push_back({.name = "META-INF/container.xml", .data = forge::text_bytes("<again/>")});
        raw.push_back({.name = std::string{md::manifest_entry_name},
                       .data = forge::text_bytes(md::manifest_to_json(parts.manifest))});
        CHECK(failure_of(forge::build_raw_archive(raw)) == not_ours);
    }
    SECTION("an overflowing central directory offset") {
        forge::Bytes bytes = good;
        const forge::Layout where = layout(good);
        forge::put_le32(bytes, where.end_record + forge::end_field::directory_offset, 0xFFFFFFFEU);
        CHECK(failure_of(bytes) == not_ours);
    }
}

TEST_CASE("Truncation at every length is never accepted", "[existing_epub][hostile]") {
    const forge::Bytes good = assembled(held::sample_book());
    REQUIRE(good.size() < 8192); // keeps this loop cheap under ASan
    for (std::size_t length = 0; length < good.size(); ++length) {
        CAPTURE(length);
        CHECK(failure_of(forge::truncated(good, length)) == not_ours);
    }
}

TEST_CASE("Read faults: EIO is unreadable with the errno, a 0 return is not ours",
          "[existing_epub][hostile]") {
    const forge::Bytes good = assembled(held::sample_book());
    FakeReadSource probe{good};
    REQUIRE(md::inspect_existing(probe, held::sample_identifier).has_value());
    const std::size_t reads = probe.reads();
    REQUIRE(reads > 4);
    for (std::size_t k = 0; k < reads; ++k) {
        CAPTURE(k);
        for (const int error : {EIO, EACCES}) {
            CAPTURE(error);
            FakeReadSource source{good};
            source.inject({.at_read = k, .kind = FakeFault::io, .error = error});
            const std::expected<md::HeldBook, md::InspectFailure> result =
                md::inspect_existing(source, held::sample_identifier);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error() ==
                  md::InspectFailure{.reason = ForeignReason::unreadable, .error = error});
        }
        FakeReadSource source{good};
        source.inject({.at_read = k, .kind = FakeFault::zero});
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(source, held::sample_identifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == not_ours);
    }
}

TEST_CASE("A hostile manifest entry is not ours", "[existing_epub][hostile]") {
    const held::BookParts parts = held::sample_book();
    const forge::Bytes good = assembled(parts);
    const forge::Layout where = layout(good);
    const std::string manifest_name{md::manifest_entry_name};

    SECTION("no manifest") {
        held::BookParts bare = parts;
        bare.with_manifest = false;
        CHECK(failure_of(bare) == not_ours);
    }
    SECTION("deflated (method 8)") {
        forge::Bytes bytes = good;
        set_method(bytes, where, manifest_name, 8);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("flags != 0, which ZipIndex itself accepts") {
        for (const std::uint16_t flags : {std::uint16_t{0x0800}, std::uint16_t{0x0002},
                                          std::uint16_t{0x0004}, std::uint16_t{0x8000}}) {
            CAPTURE(flags);
            forge::Bytes bytes = good;
            set_flags(bytes, where, manifest_name, flags);
            REQUIRE(index_accepts(bytes));
            CHECK(failure_of(bytes) == not_ours);
        }
    }
    SECTION("compressed size != uncompressed size") {
        forge::Bytes bytes = good;
        const forge::EntryLayout& entry = where.at(manifest_name);
        forge::put_le32(bytes, entry.central_header + forge::central_field::uncompressed_size,
                        entry.size + 1);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("a claimed size of 16 MiB + 1 is rejected before reading or allocating it") {
        forge::Bytes bytes = good;
        constexpr auto claimed = static_cast<std::uint32_t>(md::max_manifest_bytes + 1);
        set_sizes(bytes, where, manifest_name, claimed, true);
        REQUIRE(index_accepts(bytes));
        FakeReadSource source{bytes};
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(source, held::sample_identifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == not_ours);
        CHECK(source.reads() == 2); // the end record and the central directory only
        CHECK(source.largest_read() == where.central_directory_size);
    }
    SECTION("exactly 16 MiB passes the cap and fails later (data past the directory)") {
        forge::Bytes bytes = good;
        constexpr auto claimed = static_cast<std::uint32_t>(md::max_manifest_bytes);
        set_sizes(bytes, where, manifest_name, claimed, true);
        FakeReadSource source{bytes};
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(source, held::sample_identifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == not_ours);
        CHECK(source.largest_read() < std::size_t{64} * 1024); // the data was never read
    }
    SECTION("a CRC mismatch") {
        forge::Bytes bytes = good;
        forge::flip_byte(bytes, where.at(manifest_name).data + 2, std::byte{0x01});
        REQUIRE(index_accepts(bytes));
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("data overlapping the central directory") {
        forge::Bytes bytes = good;
        const forge::EntryLayout& entry = where.at(manifest_name);
        forge::put_le16(bytes, entry.local_header + forge::local_field::extra_length, 1);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("a local header that disagrees with the directory") {
        forge::Bytes bytes = good;
        const forge::EntryLayout& entry = where.at(manifest_name);
        forge::flip_byte(bytes, entry.local_header + forge::local_field::crc32, std::byte{0x01});
        CHECK(failure_of(bytes) == not_ours);
    }
}

TEST_CASE("Hostile manifest JSON is not ours, a newer format is newer_format",
          "[existing_epub][hostile]") {
    held::BookParts parts = held::sample_book();

    SECTION("malformed JSON") {
        parts.manifest_bytes = forge::text_bytes(R"({"format":1,"generator":)");
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("an empty manifest") {
        parts.manifest_bytes = forge::Bytes{};
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("a nesting bomb") {
        std::string bomb(1000, '[');
        bomb.append(1000, ']');
        parts.manifest_bytes = forge::text_bytes(bomb);
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("a ~2 MiB separator flood (V16)") {
        std::string flood = "[";
        flood.append(md::max_manifest_separators, ',');
        flood += ']';
        REQUIRE(flood.size() < std::size_t{3} * 1024 * 1024);
        parts.manifest_bytes = forge::text_bytes(flood);
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("another generator") {
        std::string json = md::manifest_to_json(parts.manifest);
        const std::size_t at = json.find("\"mangadex-dl\"");
        REQUIRE(at != std::string::npos);
        json.replace(at, 13, "\"mangadex-dX\"");
        parts.manifest_bytes = forge::text_bytes(json);
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("format 0 is malformed") {
        parts.manifest_bytes = manifest_with_format(parts.manifest, "0");
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("format 2 is made by a newer mangadex-dl") {
        parts.manifest_bytes = manifest_with_format(parts.manifest, "2");
        CHECK(failure_of(parts) ==
              md::InspectFailure{.reason = ForeignReason::newer_format, .error = 0});
    }
    SECTION("the written format parses (the control for the two rows above)") {
        parts.manifest_bytes = manifest_with_format(parts.manifest, "1");
        CHECK(inspect(assembled(parts)).has_value());
    }
}

TEST_CASE("Identity: another book, a stale OPF CRC, a missing OPF", "[existing_epub][hostile]") {
    held::BookParts parts = held::sample_book();

    SECTION("another identifier holds a different book") {
        constexpr md::InspectFailure other{.reason = ForeignReason::other_book, .error = 0};
        const forge::Bytes bytes = assembled(parts);
        CHECK(failure_for(bytes, "urn:mangadex:11111111-0001-4000-8000-000000000001:ja:v1") ==
              other);
        CHECK(failure_for(bytes, "URN:MANGADEX:11111111-0001-4000-8000-000000000001:EN:V1") ==
              other);
        CHECK(failure_for(bytes, "") == other);
        parts.manifest.identifier = "urn:mangadex:other";
        CHECK(failure_of(parts) == other);
    }
    SECTION("an OPF edited without updating opf_crc32 was changed by another program") {
        held::member(parts, md::opf_entry_name).data = forge::text_bytes("<package edited/>");
        CHECK(failure_of(parts) ==
              md::InspectFailure{.reason = ForeignReason::changed_by_other_program, .error = 0});
    }
    SECTION("a stale opf_crc32 in the manifest") {
        parts.manifest.opf_crc32 ^= 1U;
        CHECK(failure_of(parts) ==
              md::InspectFailure{.reason = ForeignReason::changed_by_other_program, .error = 0});
    }
    SECTION("content.opf missing is not ours") {
        held::remove_member(parts, md::opf_entry_name);
        CHECK(failure_of(parts) == not_ours);
    }
}

TEST_CASE("Hostile image references are not ours", "[existing_epub][hostile]") {
    held::BookParts parts = held::sample_book();
    const std::string first_page = first_page_name();
    const std::string last_page = last_page_name();

    SECTION("a name outside the image pattern") {
        for (const std::string_view name :
             {"OEBPS/images/../p00001.png", "OEBPS/images/p1.png", "OEBPS/images/p00001.svg",
              "OEBPS/images/p00001.PNG", "mimetype", "OEBPS/content.opf",
              "META-INF/mangadex-dl.json"}) {
            CAPTURE(name);
            held::BookParts edited = parts;
            edited.manifest.chapters[0].images[0] = std::string{name};
            CHECK(failure_of(edited) == not_ours);
        }
    }
    SECTION("a duplicate reference in two chapters") {
        parts.manifest.chapters[1].images[0] = parts.manifest.chapters[0].images[0];
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("a duplicate reference in one chapter") {
        parts.manifest.chapters[0].images[1] = parts.manifest.chapters[0].images[0];
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("a duplicate reference by a chapter and the cover") {
        REQUIRE(parts.manifest.cover.has_value());
        if (parts.manifest.cover.has_value()) {
            parts.manifest.cover->entry = first_page;
        }
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("an entry missing from the central directory") {
        held::remove_member(parts, last_page);
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("a cover entry missing from the central directory") {
        held::remove_member(parts, held::sample_cover_entry);
        CHECK(failure_of(parts) == not_ours);
    }
    SECTION("an image of size 0") {
        held::member(parts, last_page).data.clear();
        CHECK(failure_of(parts) == not_ours);
    }
}

TEST_CASE("Hostile image entries are not ours", "[existing_epub][hostile]") {
    const forge::Bytes good = assembled(held::sample_book());
    const forge::Layout where = layout(good);
    const std::string first_page = first_page_name();
    const std::string last_page = last_page_name();
    const forge::EntryLayout& page = where.at(first_page);
    const forge::EntryLayout& last = where.at(last_page);

    SECTION("method 8") {
        forge::Bytes bytes = good;
        set_method(bytes, where, first_page, 8);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("flags != 0, which ZipIndex itself accepts (page and cover)") {
        for (const std::string_view name :
             {std::string_view{first_page}, std::string_view{held::sample_cover_entry}}) {
            CAPTURE(name);
            forge::Bytes bytes = good;
            set_flags(bytes, where, name, 0x0800);
            REQUIRE(index_accepts(bytes));
            CHECK(failure_of(bytes) == not_ours);
        }
    }
    SECTION("compressed size != uncompressed size") {
        forge::Bytes bytes = good;
        forge::put_le32(bytes, page.central_header + forge::central_field::compressed_size,
                        page.size + 1);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("a claimed size of 32 MiB + 1 is rejected before its local header is read") {
        forge::Bytes bytes = good;
        constexpr auto claimed = static_cast<std::uint32_t>(md::max_image_bytes + 1);
        set_sizes(bytes, where, first_page, claimed, false);
        REQUIRE(index_accepts(bytes));
        FakeReadSource source{bytes};
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(source, held::sample_identifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == not_ours);
        // EOCD, CD, manifest local header, manifest: the first page fails before any read.
        CHECK(source.reads() == 4);
    }
    SECTION("data overlapping the central directory") {
        forge::Bytes bytes = good;
        const auto size = static_cast<std::uint32_t>(where.central_directory - last.data + 1);
        set_sizes(bytes, where, last_page, size, true);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("data past the end of the file") {
        forge::Bytes bytes = good;
        const auto size = static_cast<std::uint32_t>(bytes.size());
        set_sizes(bytes, where, first_page, size, true);
        CHECK(failure_of(bytes) == not_ours);
    }
    SECTION("a local extra field pushing the data into the next member is still in bounds") {
        // Only the bounds are checked here, not page content: the shifted data is caught by the
        // CRC when the page is read (held_page_test).
        forge::Bytes bytes = good;
        forge::put_le16(bytes, page.local_header + forge::local_field::extra_length, 1);
        CHECK(inspect(bytes).has_value());
    }
    SECTION("a local header that disagrees with the directory, field by field") {
        for (const std::uint64_t field :
             {forge::local_field::signature, forge::local_field::flags, forge::local_field::method,
              forge::local_field::crc32, forge::local_field::compressed_size,
              forge::local_field::uncompressed_size, forge::local_field::name_length,
              forge::local_field::name + 13}) {
            CAPTURE(field);
            forge::Bytes bytes = good;
            forge::flip_byte(bytes, page.local_header + field, std::byte{0x01});
            CHECK(failure_of(bytes) == not_ours);
        }
    }
    SECTION("a local header offset pointing into the central directory") {
        forge::Bytes bytes = good;
        forge::put_le32(bytes, page.central_header + forge::central_field::local_header_offset,
                        static_cast<std::uint32_t>(where.central_directory));
        CHECK(failure_of(bytes) == not_ours);
    }
}

TEST_CASE("Fixed-seed mutations of a valid book never crash", "[existing_epub][hostile][fuzz]") {
    const forge::Bytes good = assembled(held::sample_book());
    constexpr std::uint32_t seed = 0x20261019U;
    INFO("std::mt19937 seed " << seed);
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed makes every failure reproducible
    std::mt19937 rng{seed};
    std::uniform_int_distribution<std::size_t> position{0, good.size() - 1};
    std::uniform_int_distribution<std::size_t> count{1, 4};
    std::uniform_int_distribution<unsigned> value{0, 255};
    std::size_t accepted = 0;
    for (int round = 0; round < 1000; ++round) {
        CAPTURE(round);
        forge::Bytes bytes = good;
        const std::size_t flips = count(rng);
        for (std::size_t i = 0; i < flips; ++i) {
            forge::set_byte(bytes, position(rng), static_cast<std::byte>(value(rng)));
        }
        FakeReadSource source{bytes};
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(source, held::sample_identifier);
        if (!result) {
            const ForeignReason reason = result.error().reason;
            CHECK((reason == ForeignReason::not_ours || reason == ForeignReason::other_book ||
                   reason == ForeignReason::changed_by_other_program ||
                   reason == ForeignReason::newer_format));
            CHECK(result.error().error == 0);
            continue;
        }
        ++accepted;
        // The guarantees of an accepted book, whatever the mutation hit.
        const md::HeldBook& book = *result;
        REQUIRE(book.pages.size() == book.manifest.chapters.size());
        CHECK(book.cover.has_value() == book.manifest.cover.has_value());
        std::vector<md::HeldEntry> all;
        for (std::size_t c = 0; c < book.pages.size(); ++c) {
            REQUIRE(book.pages[c].size() == book.manifest.chapters[c].images.size());
            all.insert(all.end(), book.pages[c].begin(), book.pages[c].end());
        }
        if (book.cover) {
            all.push_back(*book.cover);
        }
        for (const md::HeldEntry& entry : all) {
            CHECK(entry.size > 0);
            CHECK(entry.size <= md::max_image_bytes);
            CHECK(entry.data_offset + entry.size <= bytes.size());
            // Reading the page must never crash either; any outcome is acceptable.
            const std::expected<md::HeldPage, md::PageFault> page =
                md::read_held_page(source, entry);
            if (page) {
                CHECK(page->bytes.size() == entry.size);
            }
        }
    }
    INFO("accepted " << accepted << " of 1000 mutations");
    CHECK(accepted > 0); // some mutations hit page data, so read_held_page ran on them
    CHECK(accepted < 1000);
}
