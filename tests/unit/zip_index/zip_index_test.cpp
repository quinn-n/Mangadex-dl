// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/zip_index/zip_index_test.cpp: ZipIndex and locate_entry_data on well-formed
// archives (skip-existing plan T5): ZipWriter output, shapes ZipWriter never emits (extra fields,
// comments, the UTF-8 flag), real files through FdReadSource, and the ownership guarantees.
// Every result is cross-checked against the independent test reader tests/support/zip_reader.hpp.
// Hostile input lives in zip_index_hostile_test.cpp.

#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/zip.hpp"
#include "mangadex_dl/zip_index.hpp"
#include "support/fake_read_source.hpp"
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

#include <fcntl.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace zip = mangadex_dl::test::zip;
namespace forge = mangadex_dl::test::forge;
using mangadex_dl::test::FakeReadCall;
using mangadex_dl::test::FakeReadSource;
using mangadex_dl::test::open_read_only;
using mangadex_dl::test::TempDir;

/// Names deliberately out of sorted order, plus an empty member, so find() cannot rely on the
/// central-directory order.
[[nodiscard]] std::vector<forge::ForgeEntry> sample_entries() {
    return {
        {.name = "mimetype", .data = forge::text_bytes("application/epub+zip")},
        {.name = "OEBPS/content.opf", .data = forge::text_bytes("<package/>")},
        {.name = "z-last.txt", .data = forge::text_bytes("zzz")},
        {.name = "a-first.txt", .data = forge::text_bytes("aaa")},
        {.name = "OEBPS/images/p00001.png", .data = forge::text_bytes("not really a png")},
        {.name = "empty", .data = {}},
    };
}

[[nodiscard]] forge::Bytes sample_archive() {
    const std::vector<forge::ForgeEntry> entries = sample_entries();
    std::expected<forge::Bytes, std::string> bytes = forge::build_archive(entries);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

[[nodiscard]] zip::Archive reference(const forge::Bytes& bytes) {
    std::expected<zip::Archive, std::string> archive = zip::read_archive(bytes);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

/// The index agrees with zip_reader field by field, in order, and every data offset matches.
void check_against_reference(md::ReadSource& source, const md::ZipIndex& index,
                             const zip::Archive& archive) {
    const std::vector<md::ZipEntryRecord>& entries = index.entries();
    REQUIRE(entries.size() == archive.entries.size());
    CHECK(index.central_directory_offset() == archive.central_directory_offset);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const md::ZipEntryRecord& got = entries[i];
        const zip::Entry& want = archive.entries[i];
        CAPTURE(want.name);
        CHECK(got.name == want.name);
        CHECK(got.flags == want.flags);
        CHECK(got.method == want.method);
        CHECK(got.crc32 == want.crc32);
        CHECK(got.compressed_size == want.compressed_size);
        CHECK(got.uncompressed_size == want.uncompressed_size);
        CHECK(got.local_header_offset == want.local_header_offset);
        CHECK(index.find(want.name) == &got);
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, index, got);
        REQUIRE(data.has_value());
        CHECK(*data == want.data_offset);
    }
}

} // namespace

TEST_CASE("ZipIndex reads ZipWriter output", "[zip_index]") {
    const forge::Bytes bytes = sample_archive();
    const zip::Archive archive = reference(bytes);
    FakeReadSource source{bytes};

    const std::expected<md::ZipIndex, md::ZipError> index = md::ZipIndex::read(source);
    REQUIRE(index.has_value());

    SECTION("two reads: the end record, then the whole central directory") {
        CHECK(source.calls() ==
              std::vector<FakeReadCall>{
                  {.offset = bytes.size() - 22, .length = 22},
                  {.offset = archive.central_directory_offset,
                   .length = static_cast<std::size_t>(archive.central_directory_size)},
              });
    }
    SECTION("entries keep central-directory order and every field") {
        check_against_reference(source, *index, archive);
    }
    SECTION("each locate reads exactly one local header with its name") {
        const std::size_t before = source.reads();
        const md::ZipEntryRecord* entry = index->find("OEBPS/content.opf");
        REQUIRE(entry != nullptr);
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, *index, *entry);
        REQUIRE(data.has_value());
        REQUIRE(source.reads() == before + 1);
        CHECK(source.calls().back() ==
              FakeReadCall{.offset = entry->local_header_offset, .length = 30 + 17});
    }
    SECTION("find misses names that are not exact") {
        CHECK(index->find("") == nullptr);
        CHECK(index->find("mimetyp") == nullptr);
        CHECK(index->find("mimetype2") == nullptr);
        CHECK(index->find("MIMETYPE") == nullptr);
        CHECK(index->find("OEBPS") == nullptr);
        CHECK(index->find("OEBPS/") == nullptr);
        CHECK(index->find("0") == nullptr);    // sorts before every name
        CHECK(index->find("zzzz") == nullptr); // sorts after every name
        CHECK(index->find(std::string_view{"mimetype\0", 9}) == nullptr);
    }
    SECTION("the empty member's data ends where the next record starts") {
        const md::ZipEntryRecord* entry = index->find("empty");
        REQUIRE(entry != nullptr);
        CHECK(entry->compressed_size == 0);
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, *index, *entry);
        REQUIRE(data.has_value());
        CHECK(*data == index->central_directory_offset()); // the last member, zero bytes
    }
}

TEST_CASE("ZipIndex reads an archive with no entries", "[zip_index]") {
    md::MemorySink sink;
    {
        md::ZipWriter writer{sink};
        REQUIRE(writer.finish().has_value());
    }
    const forge::Bytes bytes = sink.bytes();
    REQUIRE(bytes.size() == 22);
    FakeReadSource source{bytes};
    const std::expected<md::ZipIndex, md::ZipError> index = md::ZipIndex::read(source);
    REQUIRE(index.has_value());
    CHECK(index->entries().empty());
    CHECK(index->central_directory_offset() == 0);
    CHECK(index->find("mimetype") == nullptr);
    CHECK(source.reads() == 1); // an empty directory is never read
}

TEST_CASE("ZipIndex reads extra fields, comments and the UTF-8 flag", "[zip_index]") {
    const forge::Bytes bytes = forge::build_raw_archive({
        {.name = "mimetype",
         .data = forge::text_bytes("application/epub+zip"),
         .local_extra = forge::text_bytes("LX")},
        {.name = "caf\xC3\xA9.txt",
         .data = forge::text_bytes("utf-8 name"),
         .flags = 0x0800,
         .central_extra = forge::text_bytes("CXT"),
         .comment = forge::text_bytes("a comment")},
        {.name = "last",
         .data = forge::text_bytes("x"),
         .local_extra = forge::text_bytes("1234567"),
         .central_extra = forge::text_bytes("9"),
         .comment = forge::text_bytes("!")},
    });
    const zip::Archive archive = reference(bytes);
    FakeReadSource source{bytes};
    const std::expected<md::ZipIndex, md::ZipError> index = md::ZipIndex::read(source);
    REQUIRE(index.has_value());
    check_against_reference(source, *index, archive);
    const md::ZipEntryRecord* utf8 = index->find("caf\xC3\xA9.txt");
    REQUIRE(utf8 != nullptr);
    CHECK(utf8->flags == 0x0800);
}

TEST_CASE("ZipIndex over a real file through FdReadSource", "[zip_index][fd]") {
    auto dir = TempDir::create("mangadex-dl-zipindex");
    REQUIRE(dir.has_value());
    const forge::Bytes bytes = sample_archive();
    const zip::Archive archive = reference(bytes);
    const fs::path path = dir->path() / "book.epub";
    REQUIRE(forge::write_file(path, bytes));
    const md::UniqueFd fd = open_read_only(path);
    md::FdReadSource source{fd, bytes.size()};

    const std::expected<md::ZipIndex, md::ZipError> index = md::ZipIndex::read(source);
    REQUIRE(index.has_value());
    check_against_reference(source, *index, archive);

    SECTION("a file truncated after inspection gives short_read, not a stale read") {
        const md::ZipEntryRecord* last = index->find("empty");
        REQUIRE(last != nullptr);
        fs::resize_file(path, last->local_header_offset);
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, *index, *last);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error() == md::ZipError{.fault = md::ZipFault::short_read, .error = 0});
    }
    SECTION("a read error carries the errno") {
        const md::UniqueFd directory = open_read_only(dir->path(), O_DIRECTORY);
        md::FdReadSource dir_source{directory, 4096};
        const std::expected<md::ZipIndex, md::ZipError> failed = md::ZipIndex::read(dir_source);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error() == md::ZipError{.fault = md::ZipFault::read_error, .error = EISDIR});
    }
}

TEST_CASE("The index owns its data and keeps no reference to the source", "[zip_index]") {
    const forge::Bytes bytes = sample_archive();
    const forge::Layout layout = [&bytes] {
        std::expected<forge::Layout, std::string> found = forge::layout_of(bytes);
        REQUIRE(found.has_value());
        return std::move(*found);
    }();
    const forge::EntryLayout& mimetype = layout.at("mimetype");
    FakeReadSource source{bytes};
    // After the directory read (#1): rename entry 0 in the directory and break its local header.
    source.mutate(
        {.after_read = 1, .offset = mimetype.central_header + 46, .value = std::byte{'X'}});
    source.mutate({.after_read = 1, .offset = mimetype.local_header, .value = std::byte{0}});

    std::expected<md::ZipIndex, md::ZipError> read = md::ZipIndex::read(source);
    REQUIRE(read.has_value());
    const md::ZipIndex index = std::move(*read); // moved out: the index is a self-contained value
    REQUIRE(index.entries().size() == 6);
    CHECK(index.entries().front().name == "mimetype"); // the copy taken during the read
    const md::ZipEntryRecord* entry = index.find("mimetype");
    REQUIRE(entry != nullptr);
    // The local header is read now, so it sees the changed file.
    const std::expected<std::uint64_t, md::ZipError> data =
        md::locate_entry_data(source, index, *entry);
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().fault == md::ZipFault::bad_local_header);
}

TEST_CASE("locate_entry_data checks that the record belongs to the index", "[zip_index]") {
    const forge::Bytes bytes = sample_archive();
    FakeReadSource source{bytes};
    const std::expected<md::ZipIndex, md::ZipError> index = md::ZipIndex::read(source);
    REQUIRE(index.has_value());
    const md::ZipEntryRecord* known = index->find("a-first.txt");
    REQUIRE(known != nullptr);
    const std::size_t reads = source.reads();

    SECTION("a value-equal copy is accepted") {
        const md::ZipEntryRecord copy = *known;
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, *index, copy);
        CHECK(data.has_value());
    }
    SECTION("a modified copy is rejected before any read") {
        md::ZipEntryRecord changed = *known;
        changed.compressed_size += 1000;
        changed.uncompressed_size += 1000;
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, *index, changed);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().fault == md::ZipFault::bad_directory);
        CHECK(source.reads() == reads);
    }
    SECTION("an unknown name is rejected before any read") {
        md::ZipEntryRecord stranger = *known;
        stranger.name = "not-in-the-archive";
        const std::expected<std::uint64_t, md::ZipError> data =
            md::locate_entry_data(source, *index, stranger);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().fault == md::ZipFault::bad_directory);
        CHECK(source.reads() == reads);
    }
}
