#pragma once
// tests/support/zip_forge.hpp: building and corrupting zip archives for tests (skip-existing plan
// T5, fact-check V8). Header-only.
//
//     namespace forge = mangadex_dl::test::forge;
//     const auto bytes = forge::build_archive({{"mimetype", forge::text_bytes("x")}});
//     REQUIRE(bytes.has_value());
//     const auto layout = forge::layout_of(*bytes);   // locate offsets BEFORE patching
//     REQUIRE(layout.has_value());
//     std::vector<std::byte> hostile = *bytes;
//     forge::put_le16(hostile, layout->end_record + forge::end_field::comment_length, 1);
//
// - build_archive writes through core ZipWriter (the exact bytes the tool makes).
// - build_raw_archive writes shapes ZipWriter never emits (extra fields, comments, flags,
//   methods, empty names), so readers can be tested on them.
// - layout_of finds every record through tests/support/zip_reader.hpp, which verifies CRCs:
//   after a patch it may refuse the archive, so take the layout of the intact bytes first.
// - rewrite_entries re-reads an archive, lets the caller edit its (name, data) list and rebuilds
//   it through ZipWriter (a clean archive with different content, not a byte patch).
// - The mutators throw std::out_of_range for an offset outside the buffer (a test bug, which
//   Catch2 reports as an unexpected exception), never write out of bounds.

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/zip.hpp"
#include "support/zip_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mangadex_dl::test::forge {

using Bytes = std::vector<std::byte>;

/// Field offsets inside the end-of-central-directory record (22 bytes).
namespace end_field {
inline constexpr std::size_t signature = 0;
inline constexpr std::size_t this_disk = 4;
inline constexpr std::size_t directory_disk = 6;
inline constexpr std::size_t disk_entries = 8;
inline constexpr std::size_t total_entries = 10;
inline constexpr std::size_t directory_size = 12;
inline constexpr std::size_t directory_offset = 16;
inline constexpr std::size_t comment_length = 20;
} // namespace end_field

/// Field offsets inside a central-directory header (46 bytes, then name, extra, comment).
namespace central_field {
inline constexpr std::size_t signature = 0;
inline constexpr std::size_t version_made_by = 4;
inline constexpr std::size_t version_needed = 6;
inline constexpr std::size_t flags = 8;
inline constexpr std::size_t method = 10;
inline constexpr std::size_t mod_time = 12;
inline constexpr std::size_t mod_date = 14;
inline constexpr std::size_t crc32 = 16;
inline constexpr std::size_t compressed_size = 20;
inline constexpr std::size_t uncompressed_size = 24;
inline constexpr std::size_t name_length = 28;
inline constexpr std::size_t extra_length = 30;
inline constexpr std::size_t comment_length = 32;
inline constexpr std::size_t disk_start = 34;
inline constexpr std::size_t internal_attributes = 36;
inline constexpr std::size_t external_attributes = 38;
inline constexpr std::size_t local_header_offset = 42;
inline constexpr std::size_t name = 46;
} // namespace central_field

/// Field offsets inside a local header (30 bytes, then name, extra, data).
namespace local_field {
inline constexpr std::size_t signature = 0;
inline constexpr std::size_t version_needed = 4;
inline constexpr std::size_t flags = 6;
inline constexpr std::size_t method = 8;
inline constexpr std::size_t mod_time = 10;
inline constexpr std::size_t mod_date = 12;
inline constexpr std::size_t crc32 = 14;
inline constexpr std::size_t compressed_size = 18;
inline constexpr std::size_t uncompressed_size = 22;
inline constexpr std::size_t name_length = 26;
inline constexpr std::size_t extra_length = 28;
inline constexpr std::size_t name = 30;
} // namespace local_field

/// One (name, data) member for build_archive / rewrite_entries.
struct ForgeEntry {
    std::string name{};
    Bytes data{};

    friend bool operator==(const ForgeEntry&, const ForgeEntry&) = default;
};

/// An owning byte copy of `text`.
[[nodiscard]] inline Bytes text_bytes(std::string_view text) { return zip::byte_vector(text); }

/// A STORED archive of `entries` in order, written by core ZipWriter into a MemorySink.
[[nodiscard]] inline std::expected<Bytes, std::string>
build_archive(std::span<const ForgeEntry> entries) {
    MemorySink sink;
    ZipWriter writer{sink};
    for (const ForgeEntry& entry : entries) {
        if (const Result<void> added = writer.add_stored(entry.name, entry.data); !added) {
            return std::unexpected(added.error().message);
        }
    }
    if (const Result<void> done = writer.finish(); !done) {
        return std::unexpected(done.error().message);
    }
    return sink.bytes(); // a copy; the sink dies here
}

[[nodiscard]] inline std::expected<Bytes, std::string>
build_archive(std::initializer_list<ForgeEntry> entries) {
    return build_archive(std::span<const ForgeEntry>{entries.begin(), entries.size()});
}

// ---- raw little-endian access (bounds-checked; throws std::out_of_range) ---------------------

inline void put_le16(Bytes& bytes, std::uint64_t at, std::uint16_t value) {
    if (at > bytes.size() || bytes.size() - at < 2) {
        throw std::out_of_range(std::format("put_le16 at {} of {}", at, bytes.size()));
    }
    const auto i = static_cast<std::size_t>(at);
    bytes[i] = static_cast<std::byte>(value & 0xFFU);
    bytes[i + 1] = static_cast<std::byte>(value >> 8U);
}

inline void put_le32(Bytes& bytes, std::uint64_t at, std::uint32_t value) {
    put_le16(bytes, at, static_cast<std::uint16_t>(value & 0xFFFFU));
    put_le16(bytes, at + 2, static_cast<std::uint16_t>(value >> 16U));
}

[[nodiscard]] inline std::uint16_t get_le16(std::span<const std::byte> bytes, std::uint64_t at) {
    if (at > bytes.size() || bytes.size() - at < 2) {
        throw std::out_of_range(std::format("get_le16 at {} of {}", at, bytes.size()));
    }
    const auto i = static_cast<std::size_t>(at);
    return static_cast<std::uint16_t>(std::to_integer<std::uint32_t>(bytes[i]) |
                                      (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U));
}

[[nodiscard]] inline std::uint32_t get_le32(std::span<const std::byte> bytes, std::uint64_t at) {
    return std::uint32_t{get_le16(bytes, at)} | (std::uint32_t{get_le16(bytes, at + 2)} << 16U);
}

/// XORs the byte at `at` with `mask`.
inline void flip_byte(Bytes& bytes, std::uint64_t at, std::byte mask) {
    if (at >= bytes.size()) {
        throw std::out_of_range(std::format("flip_byte at {} of {}", at, bytes.size()));
    }
    bytes[static_cast<std::size_t>(at)] ^= mask;
}

/// Sets the byte at `at`.
inline void set_byte(Bytes& bytes, std::uint64_t at, std::byte value) {
    if (at >= bytes.size()) {
        throw std::out_of_range(std::format("set_byte at {} of {}", at, bytes.size()));
    }
    bytes[static_cast<std::size_t>(at)] = value;
}

/// The first `length` bytes of `bytes` (a copy; the whole buffer if it is shorter).
[[nodiscard]] inline Bytes truncated(std::span<const std::byte> bytes, std::size_t length) {
    const std::span<const std::byte> kept = bytes.first(std::min(length, bytes.size()));
    return Bytes{kept.begin(), kept.end()};
}

// ---- layout ----------------------------------------------------------------------------------

/// Where one member's records sit in the archive.
struct EntryLayout {
    std::string name{};
    std::uint64_t central_header = 0; ///< its central-directory header
    std::uint64_t local_header = 0;
    std::uint64_t data = 0; ///< first data byte
    std::uint32_t size = 0;
};

struct Layout {
    std::uint64_t end_record = 0; ///< always size - 22
    std::uint64_t central_directory = 0;
    std::uint64_t central_directory_size = 0;
    std::vector<EntryLayout> entries{}; ///< central-directory order

    /// The entry called `name`. Throws std::out_of_range when there is none (a test bug).
    [[nodiscard]] const EntryLayout& at(std::string_view name) const MANGADEX_DL_LIFETIMEBOUND {
        for (const EntryLayout& entry : entries) {
            if (entry.name == name) {
                return entry;
            }
        }
        throw std::out_of_range(std::format("no entry '{}' in the layout", name));
    }
};

/// The layout of an intact archive (zip_reader verifies it, CRCs included).
[[nodiscard]] inline std::expected<Layout, std::string>
layout_of(std::span<const std::byte> bytes) {
    const std::expected<zip::Archive, std::string> archive = zip::read_archive(bytes);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    Layout layout{
        .end_record = archive->end_of_central_directory_offset,
        .central_directory = archive->central_directory_offset,
        .central_directory_size = archive->central_directory_size,
        .entries = {},
    };
    std::uint64_t central = archive->central_directory_offset;
    for (const zip::Entry& entry : archive->entries) {
        layout.entries.push_back(EntryLayout{
            .name = entry.name,
            .central_header = central,
            .local_header = entry.local_header_offset,
            .data = entry.data_offset,
            .size = entry.compressed_size,
        });
        central += zip::central_header_size + entry.name.size() + entry.central_extra_length +
                   std::uint64_t{entry.comment_length};
    }
    return layout;
}

/// Reads `bytes` as an archive, passes its (name, data) list to `edit`, and rebuilds it through
/// ZipWriter.
[[nodiscard]] inline std::expected<Bytes, std::string>
rewrite_entries(std::span<const std::byte> bytes,
                const std::function<void(std::vector<ForgeEntry>&)>& edit) {
    const std::expected<zip::Archive, std::string> archive = zip::read_archive(bytes);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    std::vector<ForgeEntry> entries;
    entries.reserve(archive->entries.size());
    for (const zip::Entry& entry : archive->entries) {
        entries.push_back(ForgeEntry{.name = entry.name, .data = entry.data});
    }
    edit(entries);
    return build_archive(entries);
}

// ---- raw archives ----------------------------------------------------------------------------

/// One member for build_raw_archive: every field ZipWriter fixes is free here. Lengths are
/// derived from the vectors (each must stay below 65536 bytes).
struct RawEntry {
    std::string name{};
    Bytes data{};
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    Bytes local_extra{};
    Bytes central_extra{};
    Bytes comment{};
};

namespace detail {

inline void append16(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

inline void append32(Bytes& out, std::uint32_t value) {
    append16(out, value & 0xFFFFU);
    append16(out, value >> 16U);
}

inline void append(Bytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] inline std::uint32_t value32(std::size_t value) {
    if (value > 0xFFFFFFFEU) {
        throw std::out_of_range(std::format("raw archive value {} exceeds 32 bits", value));
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline std::uint32_t length16(std::size_t value) {
    if (value > 0xFFFFU) {
        throw std::out_of_range(std::format("raw archive length {} exceeds 16 bits", value));
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace detail

/// A STORED-layout archive with local and central headers built field by field (version 10,
/// DOS time 0, date 1980-01-01, CRC of the data, compressed == uncompressed size). Single disk,
/// no archive comment.
[[nodiscard]] inline Bytes build_raw_archive(std::span<const RawEntry> entries) {
    using detail::append;
    using detail::append16;
    using detail::append32;
    Bytes out;
    std::vector<std::uint32_t> offsets;
    for (const RawEntry& entry : entries) {
        offsets.push_back(detail::value32(out.size()));
        const std::span<const std::byte> name = zip::bytes_of(entry.name);
        append32(out, zip::local_header_signature);
        append16(out, 10);
        append16(out, entry.flags);
        append16(out, entry.method);
        append16(out, 0);
        append16(out, 0x0021);
        append32(out, zip::reference_crc32(entry.data));
        append32(out, detail::value32(entry.data.size()));
        append32(out, detail::value32(entry.data.size()));
        append16(out, detail::length16(name.size()));
        append16(out, detail::length16(entry.local_extra.size()));
        append(out, name);
        append(out, entry.local_extra);
        append(out, entry.data);
    }
    const std::uint32_t directory_offset = detail::value32(out.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const RawEntry& entry = entries[i];
        const std::span<const std::byte> name = zip::bytes_of(entry.name);
        append32(out, zip::central_header_signature);
        append16(out, 10);
        append16(out, 10);
        append16(out, entry.flags);
        append16(out, entry.method);
        append16(out, 0);
        append16(out, 0x0021);
        append32(out, zip::reference_crc32(entry.data));
        append32(out, detail::value32(entry.data.size()));
        append32(out, detail::value32(entry.data.size()));
        append16(out, detail::length16(name.size()));
        append16(out, detail::length16(entry.central_extra.size()));
        append16(out, detail::length16(entry.comment.size()));
        append16(out, 0); // disk start
        append16(out, 0); // internal attributes
        append32(out, 0); // external attributes
        append32(out, offsets[i]);
        append(out, name);
        append(out, entry.central_extra);
        append(out, entry.comment);
    }
    const std::uint32_t directory_size = detail::value32(out.size() - directory_offset);
    append32(out, zip::end_of_central_directory_signature);
    append16(out, 0);
    append16(out, 0);
    append16(out, detail::length16(entries.size()));
    append16(out, detail::length16(entries.size()));
    append32(out, directory_size);
    append32(out, directory_offset);
    append16(out, 0);
    return out;
}

[[nodiscard]] inline Bytes build_raw_archive(std::initializer_list<RawEntry> entries) {
    return build_raw_archive(std::span<const RawEntry>{entries.begin(), entries.size()});
}

// ---- files -----------------------------------------------------------------------------------

/// Writes `bytes` to `path` (created or truncated). False on any stream failure.
[[nodiscard]] inline bool write_file(const std::filesystem::path& path,
                                     std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (const std::byte b : bytes) {
        file.put(static_cast<char>(std::to_integer<unsigned char>(b)));
    }
    file.close();
    return !file.fail();
}

/// Overwrites the one byte at `offset` of an existing file in place (the file keeps its inode
/// and size). False when the file cannot be opened, or `offset` is not inside it.
[[nodiscard]] inline bool patch_file_byte(const std::filesystem::path& path, std::uint64_t offset,
                                          std::byte value) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || offset >= size || !std::in_range<std::streamoff>(offset)) {
        return false;
    }
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return false;
    }
    file.seekp(static_cast<std::streamoff>(offset));
    file.put(static_cast<char>(std::to_integer<unsigned char>(value)));
    file.close();
    return !file.fail();
}

} // namespace mangadex_dl::test::forge
