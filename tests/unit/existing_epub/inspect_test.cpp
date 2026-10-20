// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/existing_epub/inspect_test.cpp: inspect_existing on well-formed books (skip-existing
// plan T9, criterion 4's happy path): the HeldBook it returns, cross-checked field by field
// against the independent test reader (tests/support/zip_reader.hpp), the guarantee that page data
// is never read, a real file through FdReadSource, and the fixed foreign-reason strings.
// Hostile input lives in inspect_hostile_test.cpp; held-page reads in held_page_test.cpp.

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/image.hpp"
#include "support/fake_read_source.hpp"
#include "support/held_book_builder.hpp"
#include "support/read_only_fd.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_forge.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <set>
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
using mangadex_dl::test::FakeReadCall;
using mangadex_dl::test::FakeReadSource;
using mangadex_dl::test::open_read_only;
using mangadex_dl::test::TempDir;

[[nodiscard]] zip::Archive reference(const forge::Bytes& bytes) {
    std::expected<zip::Archive, std::string> archive = zip::read_archive(bytes);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

[[nodiscard]] md::MediaType type_of_extension(std::string_view name) {
    const std::string_view extension = name.substr(name.rfind('.') + 1);
    for (const md::MediaType type :
         {md::MediaType::jpeg, md::MediaType::png, md::MediaType::gif, md::MediaType::webp}) {
        if (md::file_extension(type) == extension) {
            return type;
        }
    }
    FAIL("no media type for " << name);
    return md::MediaType::jpeg;
}

/// The held entry agrees with the reference reader's record of the same member.
void check_entry(const md::HeldEntry& entry, std::string_view name, const zip::Archive& archive) {
    CAPTURE(name);
    const zip::Entry* const want = archive.find(name);
    REQUIRE(want != nullptr);
    CHECK(entry.name == name);
    CHECK(entry.type == type_of_extension(name));
    CHECK(entry.crc32 == want->crc32);
    CHECK(entry.size == want->compressed_size);
    CHECK(entry.data_offset == want->data_offset);
}

/// The whole HeldBook agrees with the parts it was built from and with the reference reader.
void check_book(const md::HeldBook& book, const held::BookParts& parts,
                const zip::Archive& archive) {
    CHECK(book.manifest == parts.manifest);
    REQUIRE(book.pages.size() == book.manifest.chapters.size());
    for (std::size_t c = 0; c < book.pages.size(); ++c) {
        const std::vector<std::string>& images = book.manifest.chapters[c].images;
        REQUIRE(book.pages[c].size() == images.size());
        for (std::size_t p = 0; p < images.size(); ++p) {
            check_entry(book.pages[c][p], images[p], archive);
        }
    }
    REQUIRE(book.cover.has_value() == book.manifest.cover.has_value());
    if (book.cover.has_value() && book.manifest.cover.has_value()) {
        check_entry(*book.cover, book.manifest.cover->entry, archive);
    }
}

/// True when [offset, offset + length) overlaps any referenced image's data.
[[nodiscard]] bool touches_page_data(const FakeReadCall& call, const md::BookManifest& manifest,
                                     const zip::Archive& archive) {
    return std::ranges::any_of(held::referenced_names(manifest), [&](const std::string& name) {
        const zip::Entry* const entry = archive.find(name);
        if (entry == nullptr) {
            return false;
        }
        const std::uint64_t begin = entry->data_offset;
        const std::uint64_t end = begin + entry->compressed_size;
        return call.offset < end && begin < call.offset + call.length;
    });
}

} // namespace

TEST_CASE("inspect_existing returns the held book of a well-formed file", "[existing_epub]") {
    const held::BookParts parts = held::sample_book();
    const forge::Bytes bytes = assembled(parts);
    const zip::Archive archive = reference(bytes);
    FakeReadSource source{bytes};

    const std::expected<md::HeldBook, md::InspectFailure> inspected =
        md::inspect_existing(source, held::sample_identifier);
    REQUIRE(inspected.has_value());
    check_book(*inspected, parts, archive);

    SECTION("all four formats and the cover are typed from their extensions") {
        const md::HeldBook& book = *inspected;
        REQUIRE(book.pages.size() == 2);
        REQUIRE(book.pages[0].size() == 2);
        REQUIRE(book.pages[1].size() == 2);
        CHECK(book.pages[0][0].type == md::MediaType::png);
        CHECK(book.pages[0][1].type == md::MediaType::gif);
        CHECK(book.pages[1][0].type == md::MediaType::jpeg);
        CHECK(book.pages[1][1].type == md::MediaType::webp);
        REQUIRE(book.cover.has_value());
        const md::HeldEntry cover = book.cover.value_or(md::HeldEntry{});
        CHECK(cover.name == held::sample_cover_entry);
        CHECK(cover.type == md::MediaType::png);
    }

    SECTION("it reads the directory, the manifest and local headers, never page data") {
        REQUIRE(source.reads() > 0);
        for (const FakeReadCall& call : source.calls()) {
            CAPTURE(call.offset, call.length);
            CHECK_FALSE(touches_page_data(call, parts.manifest, archive));
        }
        // EOCD + CD + manifest local header + manifest + one local header per image and cover.
        const std::size_t images = held::referenced_names(parts.manifest).size();
        CHECK(source.reads() == 4 + images);
    }
}

TEST_CASE("inspect_existing handles a book without a cover", "[existing_epub]") {
    const held::BookParts parts =
        held::sample_book(held::BookShape{.chapters = {{held::ImageKind::png}}, .cover = false});
    const forge::Bytes bytes = assembled(parts);
    FakeReadSource source{bytes};
    const std::expected<md::HeldBook, md::InspectFailure> inspected =
        md::inspect_existing(source, held::sample_identifier);
    REQUIRE(inspected.has_value());
    CHECK_FALSE(inspected->cover.has_value());
    CHECK_FALSE(inspected->manifest.cover.has_value());
    check_book(*inspected, parts, reference(bytes));
}

TEST_CASE("inspect_existing accepts members it does not reference", "[existing_epub]") {
    // A real EPUB also holds XHTML pages, nav and toc: they are neither required nor checked.
    held::BookParts parts = held::sample_book();
    parts.entries.push_back(
        {.name = "OEBPS/pages/p00001.xhtml", .data = forge::text_bytes("<html/>")});
    parts.entries.push_back({.name = "OEBPS/images/p00099.png", .data = forge::text_bytes("x")});
    const forge::Bytes bytes = assembled(parts);
    FakeReadSource source{bytes};
    const std::expected<md::HeldBook, md::InspectFailure> inspected =
        md::inspect_existing(source, held::sample_identifier);
    REQUIRE(inspected.has_value());
    check_book(*inspected, parts, reference(bytes));
}

TEST_CASE("inspect_existing continues after positive short counts", "[existing_epub]") {
    const held::BookParts parts = held::sample_book();
    const forge::Bytes bytes = assembled(parts);
    FakeReadSource probe{bytes};
    REQUIRE(md::inspect_existing(probe, held::sample_identifier).has_value());
    for (std::size_t k = 0; k < probe.reads(); ++k) {
        CAPTURE(k);
        FakeReadSource source{bytes};
        source.inject(
            {.at_read = k, .kind = mangadex_dl::test::FakeFault::short_count, .count = 1});
        const std::expected<md::HeldBook, md::InspectFailure> inspected =
            md::inspect_existing(source, held::sample_identifier);
        REQUIRE(inspected.has_value());
        check_book(*inspected, parts, reference(bytes));
    }
}

TEST_CASE("inspect_existing reads a real file through FdReadSource", "[existing_epub][fs]") {
    std::expected<TempDir, std::string> dir = TempDir::create();
    REQUIRE(dir.has_value());
    const held::BookParts parts = held::sample_book();
    const forge::Bytes bytes = assembled(parts);
    const fs::path path = dir->path() / "book.epub";
    REQUIRE(forge::write_file(path, bytes));

    const md::UniqueFd fd = open_read_only(path);
    REQUIRE(fd.valid());
    md::FdReadSource source{fd, bytes.size()};
    const std::expected<md::HeldBook, md::InspectFailure> inspected =
        md::inspect_existing(source, held::sample_identifier);
    REQUIRE(inspected.has_value());
    check_book(*inspected, parts, reference(bytes));

    SECTION("a file shorter than the recorded size is not ours") {
        md::FdReadSource longer{fd, bytes.size() + 1}; // the EOCD is then not at size - 22
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(longer, held::sample_identifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() ==
              md::InspectFailure{.reason = md::ForeignReason::not_ours, .error = 0});
    }

    SECTION("a closed descriptor is unreadable with its errno") {
        const md::UniqueFd closed{};
        md::FdReadSource broken{closed, bytes.size()};
        const std::expected<md::HeldBook, md::InspectFailure> result =
            md::inspect_existing(broken, held::sample_identifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() ==
              md::InspectFailure{.reason = md::ForeignReason::unreadable, .error = EBADF});
    }
}

TEST_CASE("Foreign reasons have fixed texts and the D8 remedies", "[existing_epub]") {
    using md::ForeignReason;
    struct Row {
        ForeignReason reason = ForeignReason::not_ours;
        std::string_view text{};
        std::string_view remedy{};
    };
    constexpr std::string_view rename = "rename or remove it, or use another DIR";
    constexpr std::string_view elsewhere = "use another DIR or --title";
    constexpr std::string_view overwrite = "pass --overwrite to replace it";
    const std::array rows{
        Row{.reason = ForeignReason::not_regular, .text = "not a regular file", .remedy = rename},
        Row{.reason = ForeignReason::unreadable, .text = "cannot be read", .remedy = rename},
        Row{.reason = ForeignReason::not_ours,
            .text = "not an EPUB made by mangadex-dl",
            .remedy = elsewhere},
        Row{.reason = ForeignReason::other_book,
            .text = "holds a different book",
            .remedy = elsewhere},
        Row{.reason = ForeignReason::changed_by_other_program,
            .text = "changed by another program",
            .remedy = overwrite},
        Row{.reason = ForeignReason::newer_format,
            .text = "made by a newer mangadex-dl",
            .remedy = overwrite},
        Row{.reason = ForeignReason::changed_during_update,
            .text = "changed while it was being updated",
            .remedy = overwrite},
    };
    std::set<std::string_view> texts;
    for (const Row& row : rows) {
        CAPTURE(row.text);
        CHECK(md::foreign_reason_text(row.reason) == row.text);
        CHECK(md::foreign_remedy(row.reason) == row.remedy);
        texts.insert(md::foreign_reason_text(row.reason));
    }
    CHECK(texts.size() == rows.size());
}
