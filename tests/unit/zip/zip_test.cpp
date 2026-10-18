// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/zip/zip_test.cpp: CRC-32, MemorySink and ZipWriter (download plan T5), read back
// through the independent test reader tests/support/zip_reader.hpp, which is itself checked here
// (every truncation, corruption) because later tracks rely on it.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/zip.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace zip = mangadex_dl::test::zip;
namespace mdx = mangadex_dl::test::mdx;

using Bytes = std::vector<std::byte>;

void put16(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void put32(Bytes& out, std::uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}

void put_text(Bytes& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

[[nodiscard]] std::uint32_t read32(std::span<const std::byte> bytes, std::size_t at) {
    REQUIRE(at + 4 <= bytes.size());
    std::uint32_t value = 0;
    for (std::size_t i = 4; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[at + i - 1]);
    }
    return value;
}

[[nodiscard]] std::uint32_t read16(std::span<const std::byte> bytes, std::size_t at) {
    REQUIRE(at + 2 <= bytes.size());
    return std::to_integer<std::uint32_t>(bytes[at]) |
           (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U);
}

/// Deterministic pseudo-random bytes (xorshift32), so content is not all zeros or ASCII.
[[nodiscard]] Bytes pattern(std::size_t size, std::uint32_t seed) {
    Bytes out;
    out.reserve(size);
    std::uint32_t state = seed == 0 ? 1U : seed;
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        out.push_back(static_cast<std::byte>(state & 0xFFU));
    }
    return out;
}

/// Fails the N-th write call (0-based) and every one after it; stores the rest in memory.
class FailingSink final : public md::ByteSink {
public:
    FailingSink(std::size_t fail_at, md::Errc code) : fail_at_(fail_at), code_(code) {}

    [[nodiscard]] md::Result<void> write(std::span<const std::byte> data) override {
        const std::size_t call = calls_++;
        if (call >= fail_at_) {
            return std::unexpected(md::Error{.code = code_, .message = "sink: disk full"});
        }
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        return {};
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::size_t fail_at_ = 0;
    md::Errc code_ = md::Errc::io;
    std::size_t calls_ = 0;
    Bytes bytes_{};
};

/// Counts bytes without storing them (multi-GiB archives), keeping only the last small write.
class CountingSink final : public md::ByteSink {
public:
    [[nodiscard]] md::Result<void> write(std::span<const std::byte> data) override {
        total_ += data.size();
        if (data.size() <= max_kept) {
            last_.assign(data.begin(), data.end());
        }
        return {};
    }

    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] std::span<const std::byte> last_write() const noexcept { return last_; }

private:
    static constexpr std::size_t max_kept = std::size_t{1} << 20U;
    std::uint64_t total_ = 0;
    Bytes last_{};
};

[[nodiscard]] zip::Archive read_back(const md::MemorySink& sink) {
    auto archive = zip::read_archive(sink.bytes());
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

void require_failure(const md::Result<void>& result, md::Errc code = md::Errc::failure) {
    REQUIRE(!result.has_value());
    CHECK(result.error().code == code);
    CHECK(!result.error().message.empty());
}

} // namespace

// ---- CRC-32 ------------------------------------------------------------------------------------

TEST_CASE("crc32 matches the standard check values", "[zip][crc32]") {
    CHECK(md::crc32({}) == 0U);
    CHECK(md::crc32(zip::bytes_of("a")) == 0xE8B7BE43U);
    CHECK(md::crc32(zip::bytes_of("123456789")) == 0xCBF43926U);
    CHECK(md::crc32(zip::bytes_of("The quick brown fox jumps over the lazy dog")) == 0x414FA339U);
}

TEST_CASE("crc32 agrees with two independent bitwise implementations", "[zip][crc32]") {
    for (std::size_t size = 0; size <= 100; ++size) {
        INFO("size " << size);
        const Bytes data = pattern(size, static_cast<std::uint32_t>(size) + 7U);
        const std::string text{zip::detail::to_string(data)};
        const std::uint32_t expected = zip::reference_crc32(data);
        CHECK(md::crc32(data) == expected);
        CHECK(mdx::crc32(text) == expected);
    }
}

TEST_CASE("crc32 continues across buffers at every split point", "[zip][crc32]") {
    const Bytes data = pattern(67, 42);
    const std::span<const std::byte> all{data};
    const std::uint32_t whole = md::crc32(all);
    for (std::size_t split = 0; split <= all.size(); ++split) {
        INFO("split " << split);
        CHECK(md::crc32(all.subspan(split), md::crc32(all.first(split))) == whole);
    }
    std::uint32_t running = 0;
    for (const std::byte b : data) {
        running = md::crc32(std::span<const std::byte>{&b, 1}, running);
    }
    CHECK(running == whole);
}

// ---- MemorySink --------------------------------------------------------------------------------

TEST_CASE("MemorySink appends every write", "[zip][sink]") {
    md::MemorySink sink;
    REQUIRE(sink.write(zip::bytes_of("ab")).has_value());
    REQUIRE(sink.write({}).has_value());
    REQUIRE(sink.write(zip::bytes_of("cde")).has_value());
    CHECK(zip::detail::to_string(sink.bytes()) == "abcde");
}

TEST_CASE("MemorySink copes with a write of its own bytes", "[zip][sink]") {
    // Against the documented contract, but must not be a use-after-free when the vector grows.
    md::MemorySink sink;
    REQUIRE(sink.write(zip::bytes_of("0123456789")).has_value());
    for (int i = 0; i < 6; ++i) {
        REQUIRE(sink.write(sink.bytes()).has_value());
    }
    REQUIRE(sink.bytes().size() == 640);
    const std::string all = zip::detail::to_string(sink.bytes());
    for (std::size_t at = 0; at < all.size(); at += 10) {
        CHECK(all.substr(at, 10) == "0123456789");
    }
    // A write of the tail of its own bytes.
    REQUIRE(sink.write(std::span<const std::byte>{sink.bytes()}.last(3)).has_value());
    CHECK(zip::detail::to_string(sink.bytes()).ends_with("789789"));
}

// ---- ZipWriter: byte layout --------------------------------------------------------------------

TEST_CASE("an empty archive is a bare end record", "[zip]") {
    md::MemorySink sink;
    md::ZipWriter writer{sink};
    REQUIRE(writer.finish().has_value());
    Bytes expected;
    put32(expected, 0x06054B50U);
    for (int i = 0; i < 9; ++i) {
        put16(expected, 0);
    }
    CHECK(sink.bytes() == expected);
    CHECK(writer.bytes_written() == 22);
    CHECK(writer.entry_count() == 0);
    CHECK(read_back(sink).entries.empty());
}

TEST_CASE("a one-entry archive has exactly the documented bytes", "[zip]") {
    constexpr std::string_view name = "mimetype";
    constexpr std::string_view content = "application/epub+zip";
    const std::uint32_t crc = zip::reference_crc32(zip::bytes_of(content));

    md::MemorySink sink;
    md::ZipWriter writer{sink};
    REQUIRE(writer.add_stored(name, zip::bytes_of(content)).has_value());
    CHECK(writer.bytes_written() == 30 + name.size() + content.size());
    REQUIRE(writer.finish().has_value());

    Bytes expected;
    put32(expected, 0x04034B50U); // local file header
    put16(expected, 10);          // version needed 1.0
    put16(expected, 0);           // flags
    put16(expected, 0);           // STORED
    put16(expected, 0);           // 00:00:00
    put16(expected, 0x0021);      // 1980-01-01
    put32(expected, crc);
    put32(expected, 20);
    put32(expected, 20);
    put16(expected, 8);
    put16(expected, 0); // no extra field
    put_text(expected, name);
    put_text(expected, content);
    const std::size_t cd_offset = expected.size();
    put32(expected, 0x02014B50U); // central directory header
    put16(expected, 10);          // made by: MS-DOS host, 1.0
    put16(expected, 10);
    put16(expected, 0);
    put16(expected, 0);
    put16(expected, 0);
    put16(expected, 0x0021);
    put32(expected, crc);
    put32(expected, 20);
    put32(expected, 20);
    put16(expected, 8);
    put16(expected, 0); // extra
    put16(expected, 0); // comment
    put16(expected, 0); // disk
    put16(expected, 0); // internal attributes
    put32(expected, 0); // external attributes
    put32(expected, 0); // local header offset
    put_text(expected, name);
    const std::size_t cd_size = expected.size() - cd_offset;
    put32(expected, 0x06054B50U); // end of central directory
    put16(expected, 0);
    put16(expected, 0);
    put16(expected, 1);
    put16(expected, 1);
    put32(expected, static_cast<std::uint32_t>(cd_size));
    put32(expected, static_cast<std::uint32_t>(cd_offset));
    put16(expected, 0); // no comment
    CHECK(sink.bytes() == expected);
    CHECK(writer.bytes_written() == expected.size());

    // What T2's EXPECT_EPUB checks on an installed-binary run, at the same offsets.
    const std::span<const std::byte> out{sink.bytes()};
    CHECK(read32(out, 0) == 0x04034B50U);
    CHECK(read16(out, 8) == 0);
    CHECK(read32(out, 18) == 20);
    CHECK(read32(out, 22) == 20);
    CHECK(read16(out, 26) == 8);
    CHECK(read16(out, 28) == 0);
    CHECK(zip::detail::to_string(out.subspan(30, 8)) == name);
    CHECK(zip::detail::to_string(out.subspan(38, 20)) == content);
    CHECK(read32(out, out.size() - 22) == 0x06054B50U);
}

TEST_CASE("many entries round-trip through the reader", "[zip]") {
    md::MemorySink sink;
    md::ZipWriter writer{sink};
    const std::vector<std::size_t> sizes{0, 1, 7, 8, 9, 15, 16, 17, 1000, 70000};
    std::vector<std::pair<std::string, Bytes>> inputs;
    inputs.emplace_back("mimetype", zip::byte_vector("application/epub+zip"));
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        inputs.emplace_back(std::format("OEBPS/images/p{:03}.png", i),
                            pattern(sizes[i], static_cast<std::uint32_t>(i) + 1U));
    }
    for (const auto& [name, data] : inputs) {
        REQUIRE(writer.add_stored(name, data).has_value());
        CHECK(writer.bytes_written() == sink.bytes().size());
    }
    CHECK(writer.entry_count() == inputs.size());
    REQUIRE(writer.finish().has_value());
    CHECK(writer.bytes_written() == sink.bytes().size());

    const zip::Archive archive = read_back(sink);
    REQUIRE(archive.entries.size() == inputs.size());
    CHECK(archive.end_of_central_directory_offset + 22 == sink.bytes().size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const zip::Entry& entry = archive.entries[i];
        INFO(entry.name);
        CHECK(entry.name == inputs[i].first);
        CHECK(entry.data == inputs[i].second);
        CHECK(entry.crc32 == md::crc32(inputs[i].second));
        CHECK(entry.method == 0);
        CHECK(entry.flags == 0);
        CHECK(entry.version_needed == 10);
        CHECK(entry.version_made_by == 10);
        CHECK(entry.mod_time == 0);
        CHECK(entry.mod_date == 0x0021);
        CHECK(entry.local_extra_length == 0);
        CHECK(entry.central_extra_length == 0);
        CHECK(entry.comment_length == 0);
        CHECK(entry.external_attributes == 0);
    }
    const zip::Entry* mimetype = archive.find("mimetype");
    REQUIRE(mimetype != nullptr);
    CHECK(mimetype->local_header_offset == 0);
    CHECK(mimetype->data_offset == 38);
    CHECK(mimetype->text() == "application/epub+zip");
    CHECK(archive.find("absent") == nullptr);
}

TEST_CASE("identical input gives identical bytes", "[zip]") {
    const auto build = [] {
        md::MemorySink sink;
        md::ZipWriter writer{sink};
        REQUIRE(writer.add_stored("mimetype", zip::bytes_of("application/epub+zip")).has_value());
        REQUIRE(writer.add_stored("OEBPS/a.png", pattern(5000, 3)).has_value());
        REQUIRE(writer.add_stored("OEBPS/b.png", pattern(0, 3)).has_value());
        REQUIRE(writer.finish().has_value());
        return sink.bytes();
    };
    const Bytes first = build();
    const Bytes second = build();
    CHECK(first == second);
}

// ---- ZipWriter: names --------------------------------------------------------------------------

TEST_CASE("invalid and duplicate entry names are rejected without changing the archive",
          "[zip][names]") {
    using namespace std::string_view_literals;
    md::MemorySink sink;
    md::ZipWriter writer{sink};
    REQUIRE(writer.add_stored("dir/file.txt", zip::bytes_of("x")).has_value());
    const std::uint64_t before = writer.bytes_written();

    const std::vector<std::string> invalid{
        "",
        std::string(65536, 'n'),
        "caf\xC3\xA9",
        "tab\there",
        "del\x7F",
        "a\\b",
        "/absolute",
        "a//b",
        "trailing/",
        "./a",
        "a/./b",
        "../a",
        "a/..",
        ".",
        "..",
        "dir/file.txt", // duplicate
        std::string{"nul\0byte"sv},
    };
    for (const std::string& name : invalid) {
        INFO("name '" << name << "' (" << name.size() << " bytes)");
        require_failure(writer.add_stored(name, zip::bytes_of("data")));
        CHECK(writer.bytes_written() == before);
        CHECK(writer.entry_count() == 1);
    }

    const std::string longest(65535, 'L');
    const std::vector<std::string> valid{"a",    "a/b/c", "OEBPS/p 1.png", ".hidden",
                                         "a..b", "~x",    longest};
    for (const std::string& name : valid) {
        INFO("name of " << name.size() << " bytes");
        REQUIRE(writer.add_stored(name, zip::bytes_of(name.substr(0, 3))).has_value());
    }
    REQUIRE(writer.finish().has_value());
    const zip::Archive archive = read_back(sink);
    REQUIRE(archive.entries.size() == valid.size() + 1);
    CHECK(archive.entries.back().name == longest);
    CHECK(archive.entries.back().text() == "LLL");
}

// ---- ZipWriter: lifecycle and sink failures ----------------------------------------------------

TEST_CASE("add_stored and finish fail after finish", "[zip]") {
    md::MemorySink sink;
    md::ZipWriter writer{sink};
    REQUIRE(writer.add_stored("a", zip::bytes_of("1")).has_value());
    REQUIRE(writer.finish().has_value());
    const Bytes done = sink.bytes();
    require_failure(writer.add_stored("b", zip::bytes_of("2")));
    require_failure(writer.finish());
    CHECK(sink.bytes() == done);
    CHECK(writer.entry_count() == 1);
}

TEST_CASE("a sink failure is returned unchanged and poisons the writer", "[zip][sink]") {
    SECTION("while writing a local header") {
        FailingSink sink{0, md::Errc::io};
        md::ZipWriter writer{sink};
        const md::Result<void> result = writer.add_stored("a", zip::bytes_of("1"));
        require_failure(result, md::Errc::io);
        CHECK(result.error().message == "sink: disk full");
        CHECK(writer.bytes_written() == 0);
        require_failure(writer.add_stored("b", zip::bytes_of("2")));
        require_failure(writer.finish());
        CHECK(sink.calls() == 1); // nothing reached the sink after the failure
    }
    SECTION("while writing entry data") {
        FailingSink sink{1, md::Errc::interrupted};
        md::ZipWriter writer{sink};
        require_failure(writer.add_stored("a", zip::bytes_of("1")), md::Errc::interrupted);
        CHECK(writer.bytes_written() == sink.size());
        require_failure(writer.finish());
        CHECK(sink.calls() == 2);
    }
    SECTION("while writing the central directory") {
        FailingSink sink{2, md::Errc::io};
        md::ZipWriter writer{sink};
        REQUIRE(writer.add_stored("a", zip::bytes_of("1")).has_value());
        require_failure(writer.finish(), md::Errc::io);
        require_failure(writer.finish());
        require_failure(writer.add_stored("b", zip::bytes_of("2")));
        CHECK(sink.calls() == 3);
    }
    SECTION("empty data needs no data write") {
        FailingSink sink{1, md::Errc::io};
        md::ZipWriter writer{sink};
        REQUIRE(writer.add_stored("empty", {}).has_value());
        require_failure(writer.finish(), md::Errc::io);
    }
}

// ---- ZipWriter: ZIP64 limits -------------------------------------------------------------------

TEST_CASE("the entry limit is 65534", "[zip][limits]") {
    md::MemorySink sink;
    md::ZipWriter writer{sink};
    for (std::size_t i = 0; i < md::zip_max_entries; ++i) {
        const md::Result<void> added = writer.add_stored(std::format("e{:05}", i), {});
        if (!added) {
            FAIL("entry " << i << ": " << added.error().message);
        }
    }
    CHECK(writer.entry_count() == 65534);
    const std::uint64_t full = writer.bytes_written();
    require_failure(writer.add_stored("one-too-many", {}));
    CHECK(writer.entry_count() == 65534);
    CHECK(writer.bytes_written() == full);
    REQUIRE(writer.finish().has_value());

    const std::span<const std::byte> out{sink.bytes()};
    CHECK(read16(out, out.size() - 22 + 8) == 0xFFFEU);
    CHECK(read16(out, out.size() - 22 + 10) == 0xFFFEU);
    const zip::Archive archive = read_back(sink);
    REQUIRE(archive.entries.size() == 65534);
    CHECK(archive.entries.front().name == "e00000");
    CHECK(archive.entries.back().name == "e65533");
}

TEST_CASE("sizes and offsets stop at 0xFFFFFFFE", "[zip][limits][slow]") {
    // About 4 GiB of entry data is handed to a counting sink, reusing one 64 MiB buffer: the
    // writer tracks offsets from what it writes, so the boundary needs real (CRC'd) bytes.
    constexpr std::size_t chunk_size = std::size_t{64} << 20U;
    // One allocation serves every entry, including the slightly larger last one.
    const Bytes buffer(chunk_size + 128, std::byte{0x5A});
    const std::span<const std::byte> chunk = std::span<const std::byte>{buffer}.first(chunk_size);

    CountingSink sink;
    md::ZipWriter writer{sink};
    std::size_t index = 0;
    while (md::zip_max_u32 - writer.bytes_written() >= chunk_size + 100) {
        REQUIRE(writer.add_stored(std::format("big-{:02}", index), chunk).has_value());
        ++index;
    }
    CHECK(writer.bytes_written() == sink.total());

    // The last entry ends exactly at 0xFFFFFFFE (the central directory offset); one byte more
    // would put that offset on the ZIP64 sentinel.
    constexpr std::string_view last = "last";
    const std::uint64_t last_offset = writer.bytes_written();
    const std::uint64_t room = md::zip_max_u32 - last_offset - 30 - last.size();
    REQUIRE(room + 1 <= buffer.size());
    const std::span<const std::byte> tail_span =
        std::span<const std::byte>{buffer}.first(static_cast<std::size_t>(room) + 1);

    require_failure(writer.add_stored(last, tail_span)); // ends at 0xFFFFFFFF
    CHECK(writer.bytes_written() == last_offset);
    CHECK(writer.entry_count() == index);
    REQUIRE(writer.add_stored(last, tail_span.first(tail_span.size() - 1)).has_value());
    CHECK(writer.bytes_written() == md::zip_max_u32);
    CHECK(sink.total() == md::zip_max_u32);
    require_failure(writer.add_stored("x", {})); // a 31-byte header no longer fits

    REQUIRE(writer.finish().has_value());
    const std::span<const std::byte> cd = sink.last_write();
    const std::size_t entries = index + 1;
    REQUIRE(cd.size() == (entries * 46) + (index * 6) + last.size() + 22);
    const std::size_t eocd = cd.size() - 22;
    CHECK(read32(cd, eocd) == 0x06054B50U);
    CHECK(read16(cd, eocd + 10) == entries);
    CHECK(read32(cd, eocd + 12) == eocd);            // central directory size
    CHECK(read32(cd, eocd + 16) == md::zip_max_u32); // central directory offset
    // The last central header records the last local header's offset and size.
    const std::size_t last_header = eocd - 46 - last.size();
    CHECK(read32(cd, last_header) == 0x02014B50U);
    CHECK(read32(cd, last_header + 24) == room);
    CHECK(read32(cd, last_header + 42) == last_offset);
}

// ---- the test reader itself --------------------------------------------------------------------

namespace {

[[nodiscard]] Bytes sample_archive() {
    md::MemorySink sink;
    md::ZipWriter writer{sink};
    REQUIRE(writer.add_stored("mimetype", zip::bytes_of("application/epub+zip")).has_value());
    REQUIRE(writer.add_stored("META-INF/container.xml", pattern(40, 9)).has_value());
    REQUIRE(writer.add_stored("empty", {}).has_value());
    REQUIRE(writer.finish().has_value());
    return sink.bytes();
}

[[nodiscard]] bool reads(const Bytes& bytes) { return zip::read_archive(bytes).has_value(); }

} // namespace

TEST_CASE("zip_reader rejects every truncation without reading past the end",
          "[zip][zip_reader][truncation]") {
    const Bytes archive = sample_archive();
    REQUIRE(reads(archive));
    for (std::size_t length = 0; length < archive.size(); ++length) {
        INFO("prefix length " << length);
        // Exactly sized: ASan flags any overread.
        const Bytes prefix(archive.begin(), archive.begin() + static_cast<std::ptrdiff_t>(length));
        CHECK(!reads(prefix));
    }
}

TEST_CASE("zip_reader rejects corruption and never overreads", "[zip][zip_reader]") {
    const Bytes archive = sample_archive();
    const std::size_t eocd = archive.size() - 22;

    SECTION("every byte overwritten: an error or a consistent archive, never a crash") {
        for (std::size_t at = 0; at < archive.size(); ++at) {
            for (const unsigned value : {0x00U, 0xFFU}) {
                Bytes corrupted = archive;
                corrupted[at] = static_cast<std::byte>(value);
                const auto result = zip::read_archive(corrupted);
                if (result) {
                    CHECK(result->entries.size() == 3);
                }
            }
        }
    }
    SECTION("a data byte changes the CRC") {
        Bytes corrupted = archive;
        corrupted[40] ^= std::byte{0x01};
        const auto result = zip::read_archive(corrupted);
        REQUIRE(!result.has_value());
        CHECK(result.error().find("CRC-32") != std::string::npos);
    }
    SECTION("a local name that differs from the central one") {
        Bytes corrupted = archive;
        corrupted[30] = std::byte{'M'};
        CHECK(!reads(corrupted));
    }
    SECTION("an archive comment or trailing bytes") {
        Bytes commented = archive;
        commented[eocd + 20] = std::byte{2};
        commented.push_back(std::byte{'h'});
        commented.push_back(std::byte{'i'});
        CHECK(!reads(commented));
        Bytes trailing = archive;
        trailing.push_back(std::byte{0});
        CHECK(!reads(trailing));
    }
    SECTION("a central directory offset that leaves a gap") {
        Bytes shifted = archive;
        shifted.insert(shifted.begin() + static_cast<std::ptrdiff_t>(read32(archive, eocd + 16)),
                       std::byte{0});
        const std::size_t new_eocd = shifted.size() - 22;
        const std::uint32_t cd_offset = read32(archive, eocd + 16) + 1;
        for (std::size_t i = 0; i < 4; ++i) {
            shifted[new_eocd + 16 + i] = static_cast<std::byte>((cd_offset >> (8U * i)) & 0xFFU);
        }
        const auto result = zip::read_archive(shifted);
        REQUIRE(!result.has_value());
        CHECK(result.error().find("unaccounted") != std::string::npos);
    }
    SECTION("ZIP64 sentinels and multi-disk fields") {
        Bytes count = archive;
        count[eocd + 8] = std::byte{0xFF};
        count[eocd + 9] = std::byte{0xFF};
        count[eocd + 10] = std::byte{0xFF};
        count[eocd + 11] = std::byte{0xFF};
        CHECK(!reads(count));
        Bytes disk = archive;
        disk[eocd + 4] = std::byte{1};
        CHECK(!reads(disk));
    }
}

TEST_CASE("zip_reader reads an archive file from disk", "[zip][zip_reader]") {
    auto dir = mangadex_dl::test::TempDir::create("mangadex-dl-zip");
    REQUIRE(dir.has_value());
    const std::filesystem::path path = dir->path() / "sample.zip";
    {
        const std::string contents = zip::detail::to_string(sample_archive());
        std::ofstream file(path, std::ios::binary);
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        REQUIRE(file.good());
    }
    const auto archive = zip::read_archive_file(path);
    REQUIRE(archive.has_value());
    CHECK(archive->names() ==
          std::vector<std::string>{"mimetype", "META-INF/container.xml", "empty"});

    const auto missing = zip::read_archive_file(dir->path() / "absent.zip");
    REQUIRE(!missing.has_value());
    CHECK(missing.error().starts_with("zip: cannot open"));
}
