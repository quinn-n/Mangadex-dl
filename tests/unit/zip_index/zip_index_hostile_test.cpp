// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/zip_index/zip_index_hostile_test.cpp: ZipIndex and locate_entry_data on hostile
// bytes (skip-existing plan T5). Every row must give a ZipError, never a crash, and stay
// ASan/UBSan-clean: the hostile central-directory table, every truncation, the size bounds, the
// allocation caps, a fixed-seed mutation loop plus exhaustive single-byte flips, and a read fault
// injected at every read index.
//
// Offsets are taken from the intact archive (forge::layout_of) before it is patched (V8).
// Inputs stay small (a few hundred bytes; one 8 MiB boundary case) to keep ASan memory low under
// ctest -j.

#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/zip.hpp"
#include "mangadex_dl/zip_index.hpp"
#include "support/fake_read_source.hpp"
#include "support/zip_forge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace forge = mangadex_dl::test::forge;
namespace end_field = mangadex_dl::test::forge::end_field;
namespace central_field = mangadex_dl::test::forge::central_field;
namespace local_field = mangadex_dl::test::forge::local_field;
using forge::Bytes;
using forge::Layout;
using mangadex_dl::test::FakeFault;
using mangadex_dl::test::FakeReadSource;
using md::ZipError;
using md::ZipFault;

constexpr std::size_t end_record_size = 22;
constexpr std::size_t local_header_size = 30;
constexpr std::size_t mib = std::size_t{1024} * 1024;

/// Four members; no data contains a zip signature, so a truncated copy can never end in a
/// plausible end record by accident.
[[nodiscard]] Bytes base_archive() {
    const std::vector<forge::ForgeEntry> entries{
        {.name = "mimetype", .data = forge::text_bytes("application/epub+zip")},
        {.name = "OEBPS/a.xhtml", .data = forge::text_bytes("<html>page a</html>")},
        {.name = "OEBPS/b.xhtml", .data = forge::text_bytes("<html>page b</html>")},
        {.name = "OEBPS/images/p00001.png", .data = forge::text_bytes("IMAGE-BYTES-0123456789")},
    };
    std::expected<Bytes, std::string> bytes = forge::build_archive(entries);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

[[nodiscard]] Layout layout_of(const Bytes& bytes) {
    std::expected<Layout, std::string> layout = forge::layout_of(bytes);
    REQUIRE(layout.has_value());
    REQUIRE(layout->entries.size() == 4);
    return std::move(*layout);
}

/// What reading the whole archive gave: the first error (index, then each locate in order), and
/// whether every success kept the documented guarantees.
struct Outcome {
    std::optional<ZipError> error{};
    std::size_t entries = 0;
    std::vector<std::uint64_t> data_offsets{};
    bool sound = true;
};

[[nodiscard]] Outcome classify(md::ReadSource& source) {
    Outcome outcome;
    const std::expected<md::ZipIndex, ZipError> index = md::ZipIndex::read(source);
    if (!index) {
        outcome.error = index.error();
        return outcome;
    }
    const std::uint64_t directory = index->central_directory_offset();
    const std::vector<md::ZipEntryRecord>& entries = index->entries();
    outcome.entries = entries.size();
    outcome.sound =
        directory + end_record_size <= source.size() && entries.size() <= md::zip_max_entries;
    for (const md::ZipEntryRecord& entry : entries) {
        outcome.sound = outcome.sound && !entry.name.empty() && index->find(entry.name) == &entry &&
                        entry.method == 0 && entry.compressed_size == entry.uncompressed_size;
    }
    for (const md::ZipEntryRecord& entry : entries) {
        const std::expected<std::uint64_t, ZipError> data =
            md::locate_entry_data(source, *index, entry);
        if (!data) {
            outcome.error = data.error();
            return outcome;
        }
        const std::uint64_t header_end =
            std::uint64_t{entry.local_header_offset} + local_header_size + entry.name.size();
        outcome.sound =
            outcome.sound && *data >= header_end && *data + entry.compressed_size <= directory;
        outcome.data_offsets.push_back(*data);
    }
    return outcome;
}

/// The fault of the first error, or nullopt when everything succeeded.
[[nodiscard]] std::optional<ZipFault> fault_of(const Outcome& outcome) {
    return outcome.error.transform([](const ZipError& error) { return error.fault; });
}

[[nodiscard]] Outcome classify_bytes(const Bytes& bytes) {
    FakeReadSource source{bytes};
    return classify(source);
}

using Mutator = std::function<void(Bytes&, const Layout&)>;

struct Row {
    std::string_view name{};
    Mutator mutate{};
    ZipFault fault = ZipFault::bad_directory;
};

/// The central header of member `i` plus `field`.
[[nodiscard]] std::uint64_t central(const Layout& layout, std::size_t i, std::size_t field) {
    return layout.entries.at(i).central_header + field;
}

/// The local header of member `i` plus `field`.
[[nodiscard]] std::uint64_t local(const Layout& layout, std::size_t i, std::size_t field) {
    return layout.entries.at(i).local_header + field;
}

/// The end record plus `field`.
[[nodiscard]] std::uint64_t eocd(const Layout& layout, std::size_t field) {
    return layout.end_record + field;
}

/// An end record alone: `count` entries, a directory of `size` bytes at `offset`.
[[nodiscard]] Bytes end_record(std::uint16_t count, std::uint32_t size, std::uint32_t offset) {
    Bytes out(end_record_size);
    forge::put_le32(out, end_field::signature, 0x06054B50U);
    forge::put_le16(out, end_field::disk_entries, count);
    forge::put_le16(out, end_field::total_entries, count);
    forge::put_le32(out, end_field::directory_size, size);
    forge::put_le32(out, end_field::directory_offset, offset);
    return out;
}

[[nodiscard]] std::vector<Row> hostile_rows() {
    using forge::put_le16;
    using forge::put_le32;
    std::vector<Row> rows;
    const auto row = [&rows](std::string_view name, ZipFault fault, Mutator mutate) {
        rows.push_back(Row{.name = name, .mutate = std::move(mutate), .fault = fault});
    };

    // ---- the end record ----
    row("an archive comment of 1 byte", ZipFault::no_end_record, [](Bytes& b, const Layout& l) {
        put_le16(b, eocd(l, end_field::comment_length), 1);
        b.push_back(std::byte{' '});
    });
    row("a comment length with no comment", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, eocd(l, end_field::comment_length), 1); });
    row("a trailing byte: the end record is not at size - 22", ZipFault::no_end_record,
        [](Bytes& b, const Layout&) { b.push_back(std::byte{0}); });
    row("a bad end record signature", ZipFault::no_end_record, [](Bytes& b, const Layout& l) {
        forge::flip_byte(b, eocd(l, end_field::signature), std::byte{0x01});
    });
    row("this disk is 1", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, eocd(l, end_field::this_disk), 1); });
    row("the directory disk is 1", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, eocd(l, end_field::directory_disk), 1); });
    row("the two entry counts differ", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, eocd(l, end_field::disk_entries), 3); });
    row("entry count 0xFFFF (ZIP64)", ZipFault::unsupported, [](Bytes& b, const Layout& l) {
        put_le16(b, eocd(l, end_field::disk_entries), 0xFFFF);
        put_le16(b, eocd(l, end_field::total_entries), 0xFFFF);
    });
    row("directory size 0xFFFFFFFF (ZIP64)", ZipFault::unsupported, [](Bytes& b, const Layout& l) {
        put_le32(b, eocd(l, end_field::directory_size), 0xFFFFFFFFU);
    });
    row("directory offset 0xFFFFFFFF (ZIP64)", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) {
            put_le32(b, eocd(l, end_field::directory_offset), 0xFFFFFFFFU);
        });
    row("directory offset = size = 0xFFFFFFFE", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le32(b, eocd(l, end_field::directory_size), 0xFFFFFFFEU);
            put_le32(b, eocd(l, end_field::directory_offset), 0xFFFFFFFEU);
        });
    row("directory offset 0xFFFFFFFE", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        put_le32(b, eocd(l, end_field::directory_offset), 0xFFFFFFFEU);
    });
    row("a claimed directory of 9 MiB", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        put_le32(b, eocd(l, end_field::directory_size), 9U * 1024U * 1024U);
    });
    row("directory size one byte too large", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le32(b, eocd(l, end_field::directory_size),
                     static_cast<std::uint32_t>(l.central_directory_size + 1));
        });
    row("directory size one byte too small", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le32(b, eocd(l, end_field::directory_size),
                     static_cast<std::uint32_t>(l.central_directory_size - 1));
        });
    row("directory offset one byte later", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        put_le32(b, eocd(l, end_field::directory_offset),
                 static_cast<std::uint32_t>(l.central_directory + 1));
    });
    row("directory offset one byte earlier", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le32(b, eocd(l, end_field::directory_offset),
                     static_cast<std::uint32_t>(l.central_directory - 1));
        });
    row("one entry more than the directory holds", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, eocd(l, end_field::disk_entries), 5);
            put_le16(b, eocd(l, end_field::total_entries), 5);
        });
    row("one entry fewer than the directory holds", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, eocd(l, end_field::disk_entries), 3);
            put_le16(b, eocd(l, end_field::total_entries), 3);
        });
    row("no entries, but a non-empty directory", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, eocd(l, end_field::disk_entries), 0);
            put_le16(b, eocd(l, end_field::total_entries), 0);
        });
    row("more entries than 46-byte headers fit", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, eocd(l, end_field::disk_entries), 60000);
            put_le16(b, eocd(l, end_field::total_entries), 60000);
        });

    // ---- central headers ----
    row("a bad central signature (first)", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        forge::flip_byte(b, central(l, 0, central_field::signature), std::byte{0x80});
    });
    row("a bad central signature (last)", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        forge::flip_byte(b, central(l, 3, central_field::signature + 3), std::byte{0x01});
    });
    row("the last name overruns the directory", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, central(l, 3, central_field::name_length), 24);
        });
    row("name length 0xFFFF", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        put_le16(b, central(l, 0, central_field::name_length), 0xFFFF);
    });
    row("the last extra field overruns the directory", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, central(l, 3, central_field::extra_length), 1);
        });
    row("extra length 0xFFFF", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        put_le16(b, central(l, 1, central_field::extra_length), 0xFFFF);
    });
    row("the last comment overruns the directory", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le16(b, central(l, 3, central_field::comment_length), 1);
        });
    row("comment length 0xFFFF", ZipFault::bad_directory, [](Bytes& b, const Layout& l) {
        put_le16(b, central(l, 2, central_field::comment_length), 0xFFFF);
    });
    row("a duplicate name", ZipFault::duplicate_name, [](Bytes& b, const Layout& l) {
        // "OEBPS/b.xhtml" -> "OEBPS/a.xhtml" in both headers.
        forge::set_byte(b, central(l, 2, central_field::name + 6), std::byte{'a'});
        forge::set_byte(b, local(l, 2, local_field::name + 6), std::byte{'a'});
    });
    row("method 8 recorded in the directory", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, central(l, 1, central_field::method), 8); });
    row("method 8 in both headers", ZipFault::unsupported, [](Bytes& b, const Layout& l) {
        put_le16(b, central(l, 1, central_field::method), 8);
        put_le16(b, local(l, 1, local_field::method), 8);
    });
    row("the encrypted flag", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, central(l, 0, central_field::flags), 0x1); });
    row("the data-descriptor flag", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) { put_le16(b, central(l, 0, central_field::flags), 0x8); });
    row("an entry starting on disk 1", ZipFault::unsupported, [](Bytes& b, const Layout& l) {
        put_le16(b, central(l, 2, central_field::disk_start), 1);
    });
    row("compressed size 0xFFFFFFFF (ZIP64)", ZipFault::unsupported, [](Bytes& b, const Layout& l) {
        put_le32(b, central(l, 1, central_field::compressed_size), 0xFFFFFFFFU);
    });
    row("uncompressed size 0xFFFFFFFF (ZIP64)", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 1, central_field::uncompressed_size), 0xFFFFFFFFU);
        });
    row("local header offset 0xFFFFFFFF (ZIP64)", ZipFault::unsupported,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 1, central_field::local_header_offset), 0xFFFFFFFFU);
        });
    row("compressed and uncompressed sizes differ", ZipFault::bad_directory,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 1, central_field::uncompressed_size), 1);
        });

    // ---- local headers (each must equal the directory) ----
    row("local signature", ZipFault::bad_local_header, [](Bytes& b, const Layout& l) {
        forge::flip_byte(b, local(l, 1, local_field::signature), std::byte{0x01});
    });
    row("local flags", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) { put_le16(b, local(l, 1, local_field::flags), 0x0800); });
    row("local method", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) { put_le16(b, local(l, 1, local_field::method), 8); });
    row("local CRC", ZipFault::bad_local_header, [](Bytes& b, const Layout& l) {
        forge::flip_byte(b, local(l, 1, local_field::crc32 + 2), std::byte{0x10});
    });
    row("local compressed size", ZipFault::bad_local_header, [](Bytes& b, const Layout& l) {
        put_le32(b, local(l, 1, local_field::compressed_size), 1);
    });
    row("local uncompressed size", ZipFault::bad_local_header, [](Bytes& b, const Layout& l) {
        put_le32(b, local(l, 1, local_field::uncompressed_size), 1);
    });
    row("local name length", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) { put_le16(b, local(l, 1, local_field::name_length), 12); });
    row("local name bytes", ZipFault::bad_local_header, [](Bytes& b, const Layout& l) {
        forge::set_byte(b, local(l, 1, local_field::name + 12), std::byte{'X'});
    });
    row("local header offset into the directory", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 3, central_field::local_header_offset),
                     static_cast<std::uint32_t>(l.central_directory));
        });
    row("local header straddling the directory", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 3, central_field::local_header_offset),
                     static_cast<std::uint32_t>(l.central_directory - 10));
        });
    row("local header offset past the end of the file", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 0, central_field::local_header_offset), 0xFFFFFF00U);
        });
    row("local header offset one byte off", ZipFault::bad_local_header,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 1, central_field::local_header_offset),
                     static_cast<std::uint32_t>(l.entries.at(1).local_header + 1));
        });

    // ---- data bounds ----
    row("a local extra length pushes the data past the directory", ZipFault::data_out_of_bounds,
        [](Bytes& b, const Layout& l) { put_le16(b, local(l, 3, local_field::extra_length), 1); });
    row("local extra length 0xFFFF", ZipFault::data_out_of_bounds, [](Bytes& b, const Layout& l) {
        put_le16(b, local(l, 0, local_field::extra_length), 0xFFFF);
    });
    row("the last data one byte longer, in both headers", ZipFault::data_out_of_bounds,
        [](Bytes& b, const Layout& l) {
            const std::uint32_t size = l.entries.at(3).size + 1;
            put_le32(b, central(l, 3, central_field::compressed_size), size);
            put_le32(b, central(l, 3, central_field::uncompressed_size), size);
            put_le32(b, local(l, 3, local_field::compressed_size), size);
            put_le32(b, local(l, 3, local_field::uncompressed_size), size);
        });
    row("data size 0xFFFFFFFE, in both headers", ZipFault::data_out_of_bounds,
        [](Bytes& b, const Layout& l) {
            put_le32(b, central(l, 0, central_field::compressed_size), 0xFFFFFFFEU);
            put_le32(b, central(l, 0, central_field::uncompressed_size), 0xFFFFFFFEU);
            put_le32(b, local(l, 0, local_field::compressed_size), 0xFFFFFFFEU);
            put_le32(b, local(l, 0, local_field::uncompressed_size), 0xFFFFFFFEU);
        });
    return rows;
}

} // namespace

TEST_CASE("The base archive is intact", "[zip_index][hostile]") {
    const Bytes bytes = base_archive();
    const Layout layout = layout_of(bytes);
    const Outcome outcome = classify_bytes(bytes);
    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.entries == 4);
    CHECK(outcome.sound);
    std::vector<std::uint64_t> expected;
    expected.reserve(layout.entries.size());
    for (const forge::EntryLayout& entry : layout.entries) {
        expected.push_back(entry.data);
    }
    CHECK(outcome.data_offsets == expected);
}

TEST_CASE("Every hostile central-directory row is a ZipError", "[zip_index][hostile]") {
    const Bytes base = base_archive();
    const Layout layout = layout_of(base);
    for (const Row& row : hostile_rows()) {
        CAPTURE(row.name);
        Bytes bytes = base;
        row.mutate(bytes, layout);
        const Outcome outcome = classify_bytes(bytes);
        CHECK(outcome.sound);
        CHECK(outcome.error == ZipError{.fault = row.fault, .error = 0});
    }
}

TEST_CASE("Local fields the directory does not constrain are tolerated", "[zip_index][hostile]") {
    // locate_entry_data compares signature, flags, method, CRC, sizes and name; the version
    // needed and the DOS time and date may differ (other writers vary them).
    const Bytes base = base_archive();
    const Layout layout = layout_of(base);
    for (const std::size_t field :
         {local_field::version_needed, local_field::mod_time, local_field::mod_date}) {
        CAPTURE(field);
        Bytes bytes = base;
        forge::put_le16(bytes, local(layout, 1, field), 0x1234);
        const Outcome outcome = classify_bytes(bytes);
        CHECK_FALSE(outcome.error.has_value());
        CHECK(outcome.sound);
    }
}

TEST_CASE("An empty entry name is a bad directory", "[zip_index][hostile]") {
    const Bytes bytes = forge::build_raw_archive({
        {.name = "mimetype", .data = forge::text_bytes("application/epub+zip")},
        {.name = "", .data = forge::text_bytes("nameless")},
    });
    const Outcome outcome = classify_bytes(bytes);
    CHECK(outcome.error == ZipError{.fault = ZipFault::bad_directory, .error = 0});
}

TEST_CASE("Every truncation of an archive is a ZipError", "[zip_index][hostile]") {
    const Bytes base = base_archive();
    for (std::size_t length = 0; length < base.size(); ++length) {
        CAPTURE(length);
        const Outcome outcome = classify_bytes(forge::truncated(base, length));
        CHECK(fault_of(outcome) ==
              (length < end_record_size ? ZipFault::size_out_of_range : ZipFault::no_end_record));
    }
}

TEST_CASE("The size bounds are checked before any read", "[zip_index][hostile]") {
    const Bytes base = base_archive();
    SECTION("21 bytes") {
        FakeReadSource source{base};
        source.report_size(21);
        const Outcome outcome = classify(source);
        CHECK(fault_of(outcome) == ZipFault::size_out_of_range);
        CHECK(source.reads() == 0);
    }
    SECTION("zip_max_u32 + 1 bytes") {
        FakeReadSource source{base};
        source.report_size(md::zip_max_u32 + 1);
        const Outcome outcome = classify(source);
        CHECK(fault_of(outcome) == ZipFault::size_out_of_range);
        CHECK(source.reads() == 0);
    }
    SECTION("exactly zip_max_u32 bytes is in range (the end record read then runs off the data)") {
        FakeReadSource source{base};
        source.report_size(md::zip_max_u32);
        const Outcome outcome = classify(source);
        CHECK(fault_of(outcome) == ZipFault::short_read);
        CHECK(source.reads() == 1);
    }
}

TEST_CASE("The directory size cap is checked before allocating", "[zip_index][hostile]") {
    SECTION("a claimed 9 MiB directory: only the end record is read") {
        Bytes bytes = base_archive();
        const Layout layout = layout_of(bytes);
        forge::put_le32(bytes, eocd(layout, end_field::directory_size), 9U * 1024U * 1024U);
        FakeReadSource source{bytes};
        const Outcome outcome = classify(source);
        CHECK(fault_of(outcome) == ZipFault::bad_directory);
        CHECK(source.reads() == 1);
        CHECK(source.largest_read() == end_record_size);
    }
    SECTION("exactly 8 MiB is read; one byte more is not") {
        // 8 MiB of zeros (no headers), then an end record claiming them as a directory of 0
        // entries: within the cap, so it is read, and the unaccounted bytes are rejected.
        Bytes bytes(8 * mib);
        const Bytes record = end_record(0, static_cast<std::uint32_t>(8 * mib), 0);
        bytes.insert(bytes.end(), record.begin(), record.end());
        {
            FakeReadSource source{bytes};
            const Outcome outcome = classify(source);
            CHECK(fault_of(outcome) == ZipFault::bad_directory);
            CHECK(source.reads() == 2);
            CHECK(source.largest_read() == 8 * mib);
        }
        bytes.insert(bytes.begin(), std::byte{0});
        forge::put_le32(bytes, bytes.size() - end_record_size + end_field::directory_size,
                        static_cast<std::uint32_t>((8 * mib) + 1));
        {
            FakeReadSource source{bytes};
            const Outcome outcome = classify(source);
            CHECK(fault_of(outcome) == ZipFault::bad_directory);
            CHECK(source.reads() == 1);
        }
    }
}

TEST_CASE("A read fault at every read index is reported", "[zip_index][hostile][faults]") {
    const Bytes base = base_archive();
    const Outcome clean = classify_bytes(base);
    REQUIRE_FALSE(clean.error.has_value());
    // The end record, the directory, then one local header per member.
    constexpr std::size_t total_reads = 2 + 4;

    for (std::size_t at = 0; at < total_reads; ++at) {
        CAPTURE(at);
        {
            FakeReadSource source{base};
            source.inject({.at_read = at, .kind = FakeFault::io, .error = EIO});
            const Outcome outcome = classify(source);
            CHECK(outcome.error == ZipError{.fault = ZipFault::read_error, .error = EIO});
            CHECK(source.reads() == at + 1); // stops at the first fault
        }
        {
            FakeReadSource source{base};
            source.inject({.at_read = at, .kind = FakeFault::io, .error = EACCES});
            const Outcome outcome = classify(source);
            CHECK(outcome.error == ZipError{.fault = ZipFault::read_error, .error = EACCES});
        }
        {
            FakeReadSource source{base};
            source.inject({.at_read = at, .kind = FakeFault::zero});
            const Outcome outcome = classify(source);
            CHECK(outcome.error == ZipError{.fault = ZipFault::short_read, .error = 0});
            CHECK(source.reads() == at + 1);
        }
        for (const std::size_t count : {std::size_t{1}, std::size_t{7}}) {
            CAPTURE(count);
            FakeReadSource source{base};
            source.inject({.at_read = at, .kind = FakeFault::short_count, .count = count});
            const Outcome outcome = classify(source);
            CHECK_FALSE(outcome.error.has_value()); // a positive short count is continued
            CHECK(outcome.data_offsets == clean.data_offsets);
            CHECK(source.reads() == total_reads);
        }
    }
}

TEST_CASE("Fixed-seed mutations never crash and never break the guarantees",
          "[zip_index][hostile][fuzz]") {
    const Bytes base = base_archive();
    const Layout layout = layout_of(base);
    constexpr std::uint32_t seed = 0x20261019U;
    INFO("std::mt19937 seed " << seed);
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed makes every failure reproducible
    std::mt19937 rng{seed};
    std::uniform_int_distribution<std::size_t> anywhere{0, base.size() - 1};
    std::uniform_int_distribution<std::size_t> in_directory{
        static_cast<std::size_t>(layout.central_directory), base.size() - 1};
    std::uniform_int_distribution<int> how_many{1, 4};
    std::uniform_int_distribution<unsigned> byte_value{0, 255};
    std::uniform_int_distribution<int> coin{0, 1};

    std::size_t rejected = 0;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        CAPTURE(iteration);
        Bytes bytes = base;
        const int count = how_many(rng);
        for (int i = 0; i < count; ++i) {
            // Half of the mutations hit the directory and end record, where parsing happens.
            const std::size_t at = coin(rng) == 0 ? anywhere(rng) : in_directory(rng);
            forge::set_byte(bytes, at, static_cast<std::byte>(byte_value(rng)));
        }
        const Outcome outcome = classify_bytes(bytes);
        CHECK(outcome.sound);
        if (outcome.error) {
            ++rejected;
        }
    }
    CHECK(rejected > 0); // the loop really reaches the error paths
}

TEST_CASE("Every single-byte flip never crashes and never breaks the guarantees",
          "[zip_index][hostile][fuzz]") {
    const Bytes base = base_archive();
    const Layout layout = layout_of(base);
    std::size_t directory_rejections = 0;
    for (std::size_t at = 0; at < base.size(); ++at) {
        for (const std::byte mask : {std::byte{0x01}, std::byte{0x80}, std::byte{0xFF}}) {
            CAPTURE(at, std::to_integer<int>(mask));
            Bytes bytes = base;
            forge::flip_byte(bytes, at, mask);
            const Outcome outcome = classify_bytes(bytes);
            CHECK(outcome.sound);
            if (at >= layout.central_directory && outcome.error) {
                ++directory_rejections;
            }
        }
    }
    CHECK(directory_rejections > 0);
}
