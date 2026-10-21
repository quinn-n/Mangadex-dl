// SPDX-License-Identifier: GPL-3.0-or-later
//
// ZipIndex and locate_entry_data (skip-existing plan T5; layout per PKWARE APPNOTE.TXT 4.3).
//
//   [local header 30 B + name + extra][data] ... [central header 46 B + name + extra + comment]
//   ... [end of central directory record (EOCD) 22 B, no comment]
//
// Every byte read here is untrusted. The rules that keep it memory-safe:
// - Sizes are checked before allocating: the EOCD is a fixed 22-byte array, the central
//   directory (CD) is at most max_central_directory_bytes, a local header is at most
//   30 + 65535 bytes.
// - Offset math on file positions is done in std::uint64_t on u16/u32 fields (at most five terms
//   of at most 2^32 each, so no sum can wrap); positions inside the CD buffer are std::size_t
//   bounded by its size (<= 8 MiB) plus three u16 lengths.
// - Every record is sliced out of its buffer (offset > size || count > size - offset rejects)
//   before any field is read, and fields are read from fixed-extent spans at compile-time
//   offsets (a static_assert proves each one lies inside its record).
// - Names are copied into owned strings; nothing returned borrows a read buffer, and the CD
//   buffer is freed before ZipIndex::read returns.
#include "mangadex_dl/zip_index.hpp"

#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/zip.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

constexpr std::uint32_t local_header_signature = 0x04034B50U;
constexpr std::uint32_t central_header_signature = 0x02014B50U;
constexpr std::uint32_t end_record_signature = 0x06054B50U;
constexpr std::size_t local_header_size = 30;
constexpr std::size_t central_header_size = 46;
constexpr std::size_t end_record_size = 22;
constexpr std::uint16_t u16_sentinel = 0xFFFFU;     // ZIP64: "see the ZIP64 record"
constexpr std::uint32_t u32_sentinel = 0xFFFFFFFFU; // ZIP64: "see the ZIP64 extra field"

/// General-purpose flag bits that change how an entry's bytes must be read: encrypted (0), data
/// descriptor after the data (3), strong encryption (6), masked local header (13).
constexpr std::uint16_t unsupported_flags = 0x0001U | 0x0008U | 0x0040U | 0x2000U;

using ByteView = std::span<const std::byte>;
template <std::size_t N> using Record = std::span<const std::byte, N>;

/// Little-endian u16 at the compile-time offset `At` of a fixed-size record.
template <std::size_t At, std::size_t N>
[[nodiscard]] constexpr std::uint16_t le16(Record<N> record) noexcept {
    static_assert(N != std::dynamic_extent && At + 2 <= N, "field outside the record");
    return static_cast<std::uint16_t>(std::to_integer<std::uint32_t>(record[At]) |
                                      (std::to_integer<std::uint32_t>(record[At + 1]) << 8U));
}

/// Little-endian u32 at the compile-time offset `At` of a fixed-size record.
template <std::size_t At, std::size_t N>
[[nodiscard]] constexpr std::uint32_t le32(Record<N> record) noexcept {
    static_assert(N != std::dynamic_extent && At + 4 <= N, "field outside the record");
    return std::to_integer<std::uint32_t>(record[At]) |
           (std::to_integer<std::uint32_t>(record[At + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(record[At + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(record[At + 3]) << 24U);
}

/// The `count` bytes at `offset` of `bytes`, or nullopt when any of them lies outside.
/// Overflow-free for any offset and count.
[[nodiscard]] std::optional<ByteView> slice(ByteView bytes, std::size_t offset,
                                            std::size_t count) noexcept {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        return std::nullopt;
    }
    return bytes.subspan(offset, count);
}

/// The fixed-size record at `offset` of `bytes`, or nullopt when it does not fit.
template <std::size_t N>
[[nodiscard]] std::optional<Record<N>> slice_record(ByteView bytes, std::size_t offset) noexcept {
    const std::optional<ByteView> bytes_at = slice(bytes, offset, N);
    if (!bytes_at) {
        return std::nullopt;
    }
    return bytes_at->template first<N>();
}

[[nodiscard]] std::unexpected<ZipError> fail(ZipFault fault) noexcept {
    return std::unexpected(ZipError{.fault = fault, .error = 0});
}

/// A source failure, as the zip fault that reports it.
[[nodiscard]] std::unexpected<ZipError> read_failure(const ReadError& error) noexcept {
    switch (error.fault) {
    case ReadFault::io:
        return std::unexpected(ZipError{.fault = ZipFault::read_error, .error = error.error});
    case ReadFault::short_read:
        return fail(ZipFault::short_read);
    }
    // Only reachable through an out-of-range enum value: report it as an I/O failure.
    return std::unexpected(ZipError{.fault = ZipFault::read_error, .error = error.error});
}

[[nodiscard]] std::string copy_name(ByteView bytes) {
    std::string name(bytes.size(), '\0');
    std::ranges::transform(bytes, name.begin(), [](std::byte b) {
        return static_cast<char>(std::to_integer<unsigned char>(b));
    });
    return name;
}

[[nodiscard]] bool same_name(ByteView bytes, std::string_view name) noexcept {
    return std::ranges::equal(bytes, name, std::ranges::equal_to{}, std::identity{}, [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
}

/// Walks `count` central headers that must exactly fill `directory`. Every entry is checked on
/// its own; cross-entry checks (duplicate names) and local headers come later.
[[nodiscard]] std::expected<std::vector<ZipEntryRecord>, ZipError>
parse_directory(ByteView directory, std::size_t count) {
    std::vector<ZipEntryRecord> entries;
    entries.reserve(count);   // count * 46 <= directory.size() <= 8 MiB, checked by the caller
    std::size_t position = 0; // <= directory.size() at the top of every iteration
    for (std::size_t index = 0; index < count; ++index) {
        const std::optional<Record<central_header_size>> header =
            slice_record<central_header_size>(directory, position);
        if (!header) {
            return fail(ZipFault::bad_directory);
        }
        const Record<central_header_size> h = *header;
        if (le32<0>(h) != central_header_signature) {
            return fail(ZipFault::bad_directory);
        }
        const std::uint16_t flags = le16<8>(h);
        const std::uint16_t method = le16<10>(h);
        const std::uint32_t crc = le32<16>(h);
        const std::uint32_t compressed_size = le32<20>(h);
        const std::uint32_t uncompressed_size = le32<24>(h);
        const std::size_t name_length = le16<28>(h);
        const std::size_t extra_length = le16<30>(h);
        const std::size_t comment_length = le16<32>(h);
        const std::uint16_t disk_start = le16<34>(h);
        const std::uint32_t local_header_offset = le32<42>(h);

        // position <= 8 MiB and each length < 2^16, so neither sum can wrap.
        const std::size_t name_at = position + central_header_size;
        const std::size_t next = name_at + name_length + extra_length + comment_length;
        const std::optional<ByteView> name = slice(directory, name_at, name_length);
        if (!name || next > directory.size()) {
            return fail(ZipFault::bad_directory);
        }
        if (name->empty()) {
            return fail(ZipFault::bad_directory);
        }
        if (disk_start != 0 || (flags & unsupported_flags) != 0U || method != 0) {
            return fail(ZipFault::unsupported); // multi-disk, encrypted/descriptor, compressed
        }
        if (compressed_size == u32_sentinel || uncompressed_size == u32_sentinel ||
            local_header_offset == u32_sentinel) {
            return fail(ZipFault::unsupported); // ZIP64
        }
        if (compressed_size != uncompressed_size) {
            return fail(ZipFault::bad_directory); // STORED data is its own size
        }
        entries.push_back(ZipEntryRecord{
            .name = copy_name(*name),
            .flags = flags,
            .method = method,
            .crc32 = crc,
            .compressed_size = compressed_size,
            .uncompressed_size = uncompressed_size,
            .local_header_offset = local_header_offset,
        });
        position = next;
    }
    if (position != directory.size()) {
        return fail(ZipFault::bad_directory); // bytes the entry count does not account for
    }
    return entries;
}

} // namespace

std::expected<ZipIndex, ZipError> ZipIndex::read(ReadSource& source) {
    const std::uint64_t size = source.size();
    if (size < end_record_size || size > zip_max_u32) {
        return fail(ZipFault::size_out_of_range);
    }

    // ---- the end record: exactly the last 22 bytes (so no archive comment) ----
    const std::uint64_t end_at = size - end_record_size;
    std::array<std::byte, end_record_size> end_bytes{};
    if (const std::expected<void, ReadError> got = source.read_exact(end_at, end_bytes); !got) {
        return read_failure(got.error());
    }
    const Record<end_record_size> end{end_bytes};
    if (le32<0>(end) != end_record_signature) {
        return fail(ZipFault::no_end_record);
    }
    const std::uint16_t this_disk = le16<4>(end);
    const std::uint16_t directory_disk = le16<6>(end);
    const std::uint16_t disk_entries = le16<8>(end);
    const std::uint16_t total_entries = le16<10>(end);
    const std::uint32_t directory_size = le32<12>(end);
    const std::uint32_t directory_offset = le32<16>(end);
    const std::uint16_t comment_length = le16<20>(end);
    if (comment_length != 0) {
        return fail(ZipFault::unsupported);
    }
    if (this_disk != 0 || directory_disk != 0 || disk_entries != total_entries) {
        return fail(ZipFault::unsupported);
    }
    if (total_entries == u16_sentinel || std::size_t{total_entries} > zip_max_entries ||
        directory_size == u32_sentinel || directory_offset == u32_sentinel) {
        return fail(ZipFault::unsupported);
    }
    if (directory_size > max_central_directory_bytes) {
        return fail(ZipFault::bad_directory); // before anything is allocated
    }
    // Widened: two u32 terms cannot wrap a u64.
    if (std::uint64_t{directory_offset} + directory_size > end_at) {
        return fail(ZipFault::bad_directory);
    }
    if (std::uint64_t{total_entries} * central_header_size > directory_size) {
        return fail(ZipFault::bad_directory); // too small for the claimed entry count
    }

    ZipIndex index;
    {
        // directory_size <= 8 MiB (checked above). Freed at the end of this block, so no view
        // of it can outlive read().
        std::vector<std::byte> directory(std::size_t{directory_size});
        if (!directory.empty()) {
            if (const std::expected<void, ReadError> got =
                    source.read_exact(directory_offset, directory);
                !got) {
                return read_failure(got.error());
            }
        }
        std::expected<std::vector<ZipEntryRecord>, ZipError> entries =
            parse_directory(directory, total_entries);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        index.entries_ = std::move(*entries);
    }

    // ---- the name index; unique names ----
    const std::vector<ZipEntryRecord>& entries = index.entries_;
    const auto name_of = [&entries](std::size_t i) -> std::string_view { return entries[i].name; };
    std::vector<std::size_t> order(entries.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::sort(order, std::ranges::less{}, name_of);
    if (std::ranges::adjacent_find(order, std::ranges::equal_to{}, name_of) != order.end()) {
        return fail(ZipFault::duplicate_name);
    }
    index.by_name_ = std::move(order);
    index.central_directory_offset_ = directory_offset;
    return index;
}

const ZipEntryRecord* ZipIndex::find(std::string_view name) const& noexcept {
    const auto name_of = [this](std::size_t i) -> std::string_view { return entries_[i].name; };
    const auto found = std::ranges::lower_bound(by_name_, name, std::ranges::less{}, name_of);
    if (found == by_name_.end() || name_of(*found) != name) {
        return nullptr;
    }
    return &entries_[*found];
}

std::expected<std::uint64_t, ZipError> locate_entry_data(ReadSource& source, const ZipIndex& index,
                                                         const ZipEntryRecord& entry) {
    // The precondition (`entry` comes from `index`) is checked by value, so a copy also works and
    // a record from another index (or a modified one) is rejected instead of trusted.
    const ZipEntryRecord* const known = index.find(entry.name);
    if (known == nullptr || *known != entry) {
        return fail(ZipFault::bad_directory);
    }

    // The local header and its name must lie before the central directory. name.size() < 2^16
    // (it came from a u16 length), so header_size cannot wrap.
    const std::uint64_t directory_offset = index.central_directory_offset();
    const std::uint64_t header_at = entry.local_header_offset;
    const std::size_t header_size = local_header_size + entry.name.size();
    if (header_at > directory_offset || header_size > directory_offset - header_at) {
        return fail(ZipFault::bad_local_header);
    }
    std::vector<std::byte> header_bytes(header_size);
    if (const std::expected<void, ReadError> got = source.read_exact(header_at, header_bytes);
        !got) {
        return read_failure(got.error());
    }
    const ByteView all{header_bytes};
    const Record<local_header_size> header = all.first<local_header_size>();
    const bool agrees =
        le32<0>(header) == local_header_signature && le16<6>(header) == entry.flags &&
        le16<8>(header) == entry.method && le32<14>(header) == entry.crc32 &&
        le32<18>(header) == entry.compressed_size && le32<22>(header) == entry.uncompressed_size &&
        std::size_t{le16<26>(header)} == entry.name.size() &&
        same_name(all.subspan(local_header_size), entry.name);
    if (!agrees) {
        return fail(ZipFault::bad_local_header);
    }

    // header_at + header_size <= directory_offset < 2^32 and the extra length < 2^16, so the
    // sum cannot wrap; the data must end at or before the central directory.
    const std::uint64_t data_at = header_at + header_size + std::uint64_t{le16<28>(header)};
    if (data_at > directory_offset || entry.compressed_size > directory_offset - data_at) {
        return fail(ZipFault::data_out_of_bounds);
    }
    return data_at;
}

} // namespace mangadex_dl
