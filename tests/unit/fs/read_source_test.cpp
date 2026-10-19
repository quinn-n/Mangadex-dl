// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/fs/read_source_test.cpp: FdReadSource on real files and descriptors, and the
// fault-injecting FakeReadSource test double (skip-existing plan T5, fact-check V13).
//
// FdReadSource's range checks are proven to happen before any syscall by reading through an
// invalid descriptor: a rejected range reports short_read (or EOVERFLOW), while a range that did
// reach pread would report io / EBADF.

#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "support/fake_read_source.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_forge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace forge = mangadex_dl::test::forge;
using mangadex_dl::test::FakeFault;
using mangadex_dl::test::FakeReadCall;
using mangadex_dl::test::FakeReadSource;
using mangadex_dl::test::TempDir;
using md::FdReadSource;
using md::ReadError;
using md::ReadFault;
using md::UniqueFd;

using Bytes = std::vector<std::byte>;

constexpr ReadError short_read{.fault = ReadFault::short_read, .error = 0};

[[nodiscard]] ReadError io_error(int err) {
    return ReadError{.fault = ReadFault::io, .error = err};
}

/// The error of a read, or nullopt when it succeeded (never calls error() on a value).
[[nodiscard]] std::optional<ReadError> error_of(const std::expected<void, ReadError>& result) {
    if (result.has_value()) {
        return std::nullopt;
    }
    return result.error();
}

[[nodiscard]] TempDir make_temp_dir() {
    auto dir = TempDir::create("mangadex-dl-readsrc");
    REQUIRE(dir.has_value());
    return std::move(*dir);
}

/// Deterministic, non-repeating-looking bytes (xorshift32).
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

/// open(2) read-only. The only variadic call in this file.
[[nodiscard]] UniqueFd open_read_only(const fs::path& path, int extra_flags = 0) {
    const std::string name = path.string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | extra_flags);
    REQUIRE(fd >= 0);
    return UniqueFd{fd};
}

/// A file holding `bytes` under `dir`, opened read-only.
struct OpenFile {
    fs::path path{};
    UniqueFd fd{};
};

[[nodiscard]] OpenFile make_file(const TempDir& dir, std::span<const std::byte> bytes) {
    fs::path path = dir.path() / "data.bin";
    REQUIRE(forge::write_file(path, bytes));
    UniqueFd fd = open_read_only(path);
    return OpenFile{.path = std::move(path), .fd = std::move(fd)};
}

[[nodiscard]] Bytes slice_of(const Bytes& bytes, std::size_t offset, std::size_t count) {
    REQUIRE(offset + count <= bytes.size());
    const std::span<const std::byte> part = std::span{bytes}.subspan(offset, count);
    return Bytes{part.begin(), part.end()};
}

} // namespace

// ---- FdReadSource ------------------------------------------------------------------------------

TEST_CASE("FdReadSource reads exactly the requested range", "[read_source][fd]") {
    const TempDir dir = make_temp_dir();
    // Larger than a page and not a multiple of one, so ranges cross page boundaries.
    const Bytes contents = pattern(70'001, 7);
    const OpenFile file = make_file(dir, contents);
    FdReadSource source{file.fd, contents.size()};
    CHECK(source.size() == contents.size());

    SECTION("the whole file") {
        Bytes out(contents.size());
        const std::expected<void, ReadError> got = source.read_exact(0, out);
        REQUIRE(got.has_value());
        CHECK(out == contents);
    }
    SECTION("an inner range across a page boundary") {
        Bytes out(5000);
        const std::expected<void, ReadError> got = source.read_exact(4000, out);
        REQUIRE(got.has_value());
        CHECK(out == slice_of(contents, 4000, 5000));
    }
    SECTION("the last byte") {
        Bytes out(1);
        const std::expected<void, ReadError> got = source.read_exact(contents.size() - 1, out);
        REQUIRE(got.has_value());
        CHECK(out == slice_of(contents, contents.size() - 1, 1));
    }
    SECTION("an empty range at the very end succeeds") {
        const std::expected<void, ReadError> got =
            source.read_exact(contents.size(), std::span<std::byte>{});
        CHECK(got.has_value());
    }
    SECTION("repeated reads are independent of each other (no file position)") {
        Bytes first(16);
        Bytes second(16);
        REQUIRE(source.read_exact(100, first).has_value());
        REQUIRE(source.read_exact(100, second).has_value());
        CHECK(first == second);
        CHECK(first == slice_of(contents, 100, 16));
    }
}

TEST_CASE("FdReadSource rejects a range outside the recorded size without a syscall",
          "[read_source][fd]") {
    const UniqueFd invalid{}; // -1: any pread would fail with EBADF
    FdReadSource source{invalid, 16};
    Bytes one(1);
    Bytes seventeen(17);
    Bytes four(4);

    CHECK(error_of(source.read_exact(16, one)) == short_read);
    CHECK(error_of(source.read_exact(17, std::span<std::byte>{})) == short_read);
    CHECK(error_of(source.read_exact(0, seventeen)) == short_read);
    CHECK(error_of(source.read_exact(13, four)) == short_read);
    CHECK(error_of(source.read_exact(std::numeric_limits<std::uint64_t>::max(), one)) ==
          short_read);

    // offset + length wraps a u64: the overflow-free comparison still rejects it.
    FdReadSource huge{invalid, std::numeric_limits<std::uint64_t>::max()};
    CHECK(error_of(huge.read_exact(std::numeric_limits<std::uint64_t>::max() - 1, four)) ==
          short_read);

    // In range, the read does reach pread, which proves the checks above decided.
    CHECK(error_of(source.read_exact(12, four)) == io_error(EBADF));
    CHECK(source.read_exact(16, std::span<std::byte>{}).has_value()); // empty: nothing to read
}

TEST_CASE("FdReadSource rejects a range beyond off_t without a syscall", "[read_source][fd]") {
    const UniqueFd invalid{};
    FdReadSource source{invalid, std::numeric_limits<std::uint64_t>::max()};
    constexpr auto off_max = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    Bytes two(2);
    Bytes one(1);
    // Ends past off_t's maximum: not representable for pread.
    CHECK(error_of(source.read_exact(off_max, two)) == io_error(EOVERFLOW));
    CHECK(error_of(source.read_exact(off_max + 1, std::span<std::byte>{})) == io_error(EOVERFLOW));
    // Ends exactly at the maximum: representable, so it reaches pread (EBADF here).
    CHECK(error_of(source.read_exact(off_max - 1, one)) == io_error(EBADF));
}

TEST_CASE("FdReadSource reports short_read when the file ends before the recorded size",
          "[read_source][fd]") {
    const TempDir dir = make_temp_dir();
    const Bytes contents = pattern(16, 3);
    const OpenFile file = make_file(dir, contents);
    // Recorded as 32 bytes, as if the file shrank after inspection.
    FdReadSource source{file.fd, 32};

    Bytes out(16);
    // pread returns a positive short count (8), the loop continues, then pread returns 0.
    CHECK(error_of(source.read_exact(8, out)) == short_read);
    Bytes four(4);
    CHECK(error_of(source.read_exact(16, four)) == short_read); // 0 on the very first pread
    REQUIRE(source.read_exact(0, out).has_value());             // what exists still reads
    CHECK(out == contents);
}

TEST_CASE("FdReadSource keeps the size recorded at inspection", "[read_source][fd]") {
    const TempDir dir = make_temp_dir();
    const Bytes contents = pattern(64, 5);
    const OpenFile file = make_file(dir, contents);
    FdReadSource source{file.fd, contents.size()};

    SECTION("a file truncated later is short_read past its new end") {
        fs::resize_file(file.path, 10);
        Bytes all(64);
        CHECK(error_of(source.read_exact(0, all)) == short_read);
        Bytes head(10);
        REQUIRE(source.read_exact(0, head).has_value());
        CHECK(head == slice_of(contents, 0, 10));
    }
    SECTION("a file grown later is still bounded by the recorded size") {
        fs::resize_file(file.path, 128);
        Bytes one(1);
        CHECK(error_of(source.read_exact(64, one)) == short_read);
        CHECK(source.size() == 64);
    }
}

TEST_CASE("FdReadSource reports io with the errno", "[read_source][fd]") {
    const TempDir dir = make_temp_dir();
    Bytes out(4);

    SECTION("a directory descriptor (EISDIR)") {
        const UniqueFd directory = open_read_only(dir.path(), O_DIRECTORY);
        FdReadSource source{directory, 4096};
        CHECK(error_of(source.read_exact(0, out)) == io_error(EISDIR));
    }
    SECTION("a borrowed UniqueFd that was moved from holds -1 (EBADF)") {
        OpenFile file = make_file(dir, pattern(16, 9));
        FdReadSource source{file.fd, 16};
        REQUIRE(source.read_exact(0, out).has_value());
        const UniqueFd taken = std::move(file.fd); // against the borrow contract, on purpose
        CHECK(taken.valid());
        CHECK(error_of(source.read_exact(0, out)) == io_error(EBADF));
    }
}

// ---- FakeReadSource (the test double later tasks inject faults with) ---------------------------

TEST_CASE("FakeReadSource serves its bytes and logs every call", "[read_source][fake]") {
    const Bytes contents = pattern(32, 11);
    FakeReadSource source{contents};
    CHECK(source.size() == 32);
    Bytes out(8);
    REQUIRE(source.read_exact(4, out).has_value());
    CHECK(out == slice_of(contents, 4, 8));
    Bytes big(40);
    CHECK(error_of(source.read_exact(0, big)) == short_read); // outside size(): no transfer
    CHECK(source.reads() == 2);
    CHECK(source.transfers() == 1);
    CHECK(source.largest_read() == 40);
    CHECK(source.calls() ==
          std::vector<FakeReadCall>{{.offset = 4, .length = 8}, {.offset = 0, .length = 40}});
}

TEST_CASE("FakeReadSource continues after a positive short count", "[read_source][fake]") {
    const Bytes contents = pattern(32, 13);
    for (const std::size_t count : {std::size_t{1}, std::size_t{3}, std::size_t{15}}) {
        CAPTURE(count);
        FakeReadSource source{contents};
        source.inject({.at_read = 0, .kind = FakeFault::short_count, .error = 0, .count = count});
        Bytes out(16);
        const std::expected<void, ReadError> got = source.read_exact(8, out);
        REQUIRE(got.has_value());
        CHECK(out == slice_of(contents, 8, 16));
        CHECK(source.transfers() == 2);
    }
}

TEST_CASE("FakeReadSource injects a 0 return and EIO at one read", "[read_source][fake]") {
    const Bytes contents = pattern(32, 17);
    Bytes out(4);

    SECTION("a 0 return is short_read, and only at that read") {
        FakeReadSource source{contents};
        source.inject({.at_read = 1, .kind = FakeFault::zero});
        CHECK(source.read_exact(0, out).has_value());
        CHECK(error_of(source.read_exact(0, out)) == short_read);
        CHECK(source.read_exact(0, out).has_value());
    }
    SECTION("an io fault carries its errno") {
        FakeReadSource source{contents};
        source.inject({.at_read = 0, .kind = FakeFault::io, .error = EIO});
        source.inject({.at_read = 2, .kind = FakeFault::io, .error = EACCES});
        CHECK(error_of(source.read_exact(0, out)) == io_error(EIO));
        CHECK(source.read_exact(0, out).has_value());
        CHECK(error_of(source.read_exact(0, out)) == io_error(EACCES));
    }
}

TEST_CASE("FakeReadSource models a size that differs from its data", "[read_source][fake]") {
    const Bytes contents = pattern(16, 19);

    SECTION("a reported size larger than the data: the read runs off the end") {
        FakeReadSource source{contents};
        source.report_size(64);
        Bytes out(8);
        CHECK(error_of(source.read_exact(12, out)) == short_read); // 4 bytes, then a 0 return
        CHECK(error_of(source.read_exact(40, out)) == short_read);
        REQUIRE(source.read_exact(8, out).has_value());
        CHECK(out == slice_of(contents, 8, 8));
    }
    SECTION("a reported size smaller than the data bounds every read") {
        FakeReadSource source{contents};
        source.report_size(8);
        Bytes out(4);
        CHECK(error_of(source.read_exact(6, out)) == short_read);
        CHECK(source.read_exact(4, out).has_value());
    }
}

TEST_CASE("FakeReadSource mutates its bytes after a given read", "[read_source][fake]") {
    const Bytes contents = pattern(16, 23);
    FakeReadSource source{contents};
    source.mutate({.after_read = 0, .offset = 5, .value = std::byte{0xAB}});
    Bytes before(16);
    Bytes after(16);
    REQUIRE(source.read_exact(0, before).has_value());
    REQUIRE(source.read_exact(0, after).has_value());
    CHECK(before == contents);
    CHECK(after[5] == std::byte{0xAB});
    CHECK(source.data()[5] == std::byte{0xAB});
}
