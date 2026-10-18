#pragma once
// tests/support/zip_reader.hpp: a strict, bounds-safe zip reader for tests (download plan T5).
//
// Header-only and independent of mangadex_dl::core (its CRC-32 is a separate bitwise
// implementation, so it cross-checks core's), usable from every test tier:
//
//     namespace zip = mangadex_dl::test::zip;
//     const auto archive = zip::read_archive_file(path); // or zip::read_archive(bytes)
//     REQUIRE(archive.has_value());                      // else archive.error() says why
//     const zip::Entry* entry = archive->find("mimetype"); // nullptr if absent
//     REQUIRE(entry != nullptr);
//     CHECK(entry->method == 0);
//     CHECK(entry->local_extra_length == 0);
//     CHECK(entry->text() == "application/epub+zip");
//
// It accepts exactly the profile ZipWriter produces and rejects everything else with a message:
// - the end-of-central-directory record (EOCD) sits in the last 22 bytes (no archive comment),
//   single disk, no ZIP64 sentinels (0xFFFF entries, 0xFFFFFFFF size or offset);
// - the central directory ends exactly where the EOCD starts and holds exactly the EOCD's entry
//   count, with non-empty, unique names;
// - local headers and data tile [0, central directory offset) in central-directory order with no
//   gap or overlap;
// - each local header repeats the central header's version needed, flags, method, time, date,
//   CRC, sizes and name;
// - no encryption (flag bit 0), no data descriptor (flag bit 3), method 0 (STORED) with
//   compressed size == uncompressed size, and the CRC-32 of the data matches.
// Each fixed-size record is sliced out of the input (bounds-checked) before any field is read, so
// truncated or corrupt input of any length returns an error, never an out-of-bounds read.
// Entries own copies of their names and data: an Archive never borrows the input.

#include "mangadex_dl/attributes.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::test::zip {

inline constexpr std::uint32_t local_header_signature = 0x04034B50U;
inline constexpr std::uint32_t central_header_signature = 0x02014B50U;
inline constexpr std::uint32_t end_of_central_directory_signature = 0x06054B50U;
inline constexpr std::size_t local_header_size = 30;
inline constexpr std::size_t central_header_size = 46;
inline constexpr std::size_t end_of_central_directory_size = 22;

/// One archive member, as recorded in the central directory and its local header.
struct Entry {
    std::string name{};
    std::uint16_t version_made_by = 0;
    std::uint16_t version_needed = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint16_t mod_time = 0;
    std::uint16_t mod_date = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint16_t local_extra_length = 0;
    std::uint16_t central_extra_length = 0;
    std::uint16_t comment_length = 0;
    std::uint16_t internal_attributes = 0;
    std::uint32_t external_attributes = 0;
    std::uint32_t local_header_offset = 0;
    std::uint64_t data_offset = 0; ///< first data byte, after the local header, name and extra
    std::vector<std::byte> data{}; ///< the extracted (STORED) bytes, CRC-verified

    /// The data as a std::string (a copy).
    [[nodiscard]] std::string text() const {
        std::string out;
        out.reserve(data.size());
        for (const std::byte b : data) {
            out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
        }
        return out;
    }
};

struct Archive {
    std::vector<Entry> entries{}; ///< central-directory order (== file order)
    std::uint64_t central_directory_offset = 0;
    std::uint64_t central_directory_size = 0;
    std::uint64_t end_of_central_directory_offset = 0; ///< always input size - 22

    /// The entry called `name`, or nullptr. Points into `entries`: do not keep it past the
    /// Archive, and never call this on a temporary (`read_archive(b)->find("x")` dangles).
    [[nodiscard]] const Entry*
    find(std::string_view name) const noexcept MANGADEX_DL_LIFETIMEBOUND {
        for (const Entry& entry : entries) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    /// Every entry name, in archive order.
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(entries.size());
        for (const Entry& entry : entries) {
            out.push_back(entry.name);
        }
        return out;
    }
};

/// Views the characters of `text` as bytes. Borrows `text`: never pass a temporary string and
/// keep the result.
[[nodiscard]] inline std::span<const std::byte>
bytes_of(std::string_view text MANGADEX_DL_LIFETIMEBOUND) noexcept {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

/// An owning byte copy of `text`, sized exactly (so ASan flags a read one past the end).
[[nodiscard]] inline std::vector<std::byte> byte_vector(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

/// CRC-32 (reflected polynomial 0xEDB88320), bitwise: deliberately not core's table version.
[[nodiscard]] inline std::uint32_t reference_crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : bytes) {
        crc ^= std::to_integer<std::uint32_t>(b);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

namespace detail {

using Bytes = std::span<const std::byte>;

/// The bytes [offset, offset + count) of `bytes`, or nullopt if any of them lies outside.
/// Overflow-free for any 64-bit offset and count.
[[nodiscard]] inline std::optional<Bytes> slice(Bytes bytes, std::uint64_t offset,
                                                std::uint64_t count) noexcept {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        return std::nullopt;
    }
    return bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(count));
}

/// Little-endian fields of a record that was sliced to its full fixed size first. The range
/// check is a second line of defence only (it returns 0, which every caller then rejects as a
/// signature or consistency mismatch); it never triggers for a correctly sliced record.
[[nodiscard]] inline std::uint32_t field(Bytes record, std::size_t offset,
                                         std::size_t width) noexcept {
    if (offset > record.size() || width > record.size() - offset || width > 4) {
        return 0;
    }
    std::uint32_t value = 0;
    for (std::size_t i = width; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(record[offset + i - 1]);
    }
    return value;
}

[[nodiscard]] inline std::uint16_t le16(Bytes record, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(field(record, offset, 2));
}

[[nodiscard]] inline std::uint32_t le32(Bytes record, std::size_t offset) noexcept {
    return field(record, offset, 4);
}

[[nodiscard]] inline std::string to_string(Bytes bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
    }
    return out;
}

} // namespace detail

/// Parses and verifies a whole archive (see the profile at the top of this file).
[[nodiscard]] inline std::expected<Archive, std::string>
read_archive(std::span<const std::byte> bytes) {
    using detail::le16;
    using detail::le32;
    using detail::slice;
    using std::unexpected;

    // ---- end of central directory record: exactly the last 22 bytes ----
    if (bytes.size() < end_of_central_directory_size) {
        return unexpected(
            std::format("zip: {} bytes is too short for an end record", bytes.size()));
    }
    const std::uint64_t eocd = bytes.size() - end_of_central_directory_size;
    const std::optional<detail::Bytes> end = slice(bytes, eocd, end_of_central_directory_size);
    if (!end || le32(*end, 0) != end_of_central_directory_signature) {
        return unexpected("zip: no end-of-central-directory record in the last 22 bytes");
    }
    const std::uint16_t total_entries = le16(*end, 10);
    const std::uint32_t cd_size = le32(*end, 12);
    const std::uint32_t cd_offset = le32(*end, 16);
    if (le16(*end, 4) != 0 || le16(*end, 6) != 0 || le16(*end, 8) != total_entries) {
        return unexpected("zip: multi-disk archives are not supported");
    }
    if (const std::uint16_t comment = le16(*end, 20); comment != 0) {
        return unexpected(std::format("zip: unexpected {}-byte archive comment", comment));
    }
    if (total_entries == 0xFFFFU || cd_size == 0xFFFFFFFFU || cd_offset == 0xFFFFFFFFU) {
        return unexpected("zip: ZIP64 sentinel in the end record");
    }
    if (std::uint64_t{cd_offset} + cd_size != eocd) {
        return unexpected(
            std::format("zip: central directory at {} (+{}) does not end at the end record ({})",
                        cd_offset, cd_size, eocd));
    }

    Archive archive{
        .entries = {},
        .central_directory_offset = cd_offset,
        .central_directory_size = cd_size,
        .end_of_central_directory_offset = eocd,
    };
    archive.entries.reserve(total_entries);
    std::set<std::string, std::less<>> seen;

    // ---- central directory, each header checked against its local header ----
    std::uint64_t pos = cd_offset;
    std::uint64_t expected_local = 0; // where the next local header must start
    for (std::size_t index = 0; index < total_entries; ++index) {
        const std::optional<detail::Bytes> ch = slice(bytes, pos, central_header_size);
        if (!ch || pos + central_header_size > eocd) {
            return unexpected(std::format("zip: central header {} is truncated", index));
        }
        if (le32(*ch, 0) != central_header_signature) {
            return unexpected(std::format("zip: bad central header signature at {}", pos));
        }
        const std::uint16_t flags = le16(*ch, 8);
        const std::uint16_t method = le16(*ch, 10);
        const std::uint32_t crc = le32(*ch, 16);
        const std::uint32_t compressed_size = le32(*ch, 20);
        const std::uint32_t uncompressed_size = le32(*ch, 24);
        const std::uint16_t name_length = le16(*ch, 28);
        const std::uint16_t extra_length = le16(*ch, 30);
        const std::uint16_t comment_length = le16(*ch, 32);
        const std::uint32_t lho = le32(*ch, 42);

        const std::uint64_t next =
            pos + central_header_size + name_length + extra_length + std::uint64_t{comment_length};
        const std::optional<detail::Bytes> name_bytes =
            slice(bytes, pos + central_header_size, name_length);
        if (next > eocd || !name_bytes) {
            return unexpected(std::format("zip: central header {} overruns the directory", index));
        }
        std::string name = detail::to_string(*name_bytes);
        if (name.empty()) {
            return unexpected(std::format("zip: central header {} has an empty name", index));
        }
        if (!seen.insert(name).second) {
            return unexpected(std::format("zip: duplicate entry name '{}'", name));
        }
        if (le16(*ch, 34) != 0) {
            return unexpected(std::format("zip: '{}' starts on another disk", name));
        }
        if ((flags & 0x0001U) != 0U) {
            return unexpected(std::format("zip: '{}' is encrypted", name));
        }
        if ((flags & 0x0008U) != 0U) {
            return unexpected(std::format("zip: '{}' uses a data descriptor", name));
        }
        if (method != 0) {
            return unexpected(std::format("zip: '{}' uses method {}, not STORED", name, method));
        }
        if (compressed_size != uncompressed_size) {
            return unexpected(std::format("zip: STORED '{}' has differing sizes", name));
        }
        if (compressed_size == 0xFFFFFFFFU || lho == 0xFFFFFFFFU) {
            return unexpected(std::format("zip: ZIP64 sentinel in '{}'", name));
        }

        // ---- the matching local header ----
        if (lho != expected_local) {
            return unexpected(
                std::format("zip: '{}' local header at {}, expected {} (gap or overlap)", name, lho,
                            expected_local));
        }
        const std::optional<detail::Bytes> lh = slice(bytes, lho, local_header_size);
        if (!lh || std::uint64_t{lho} + local_header_size > cd_offset) {
            return unexpected(std::format("zip: '{}' local header is truncated", name));
        }
        if (le32(*lh, 0) != local_header_signature) {
            return unexpected(std::format("zip: '{}' bad local header signature", name));
        }
        const bool same_fields = le16(*lh, 4) == le16(*ch, 6) && le16(*lh, 6) == flags &&
                                 le16(*lh, 8) == method && le16(*lh, 10) == le16(*ch, 12) &&
                                 le16(*lh, 12) == le16(*ch, 14) && le32(*lh, 14) == crc &&
                                 le32(*lh, 18) == compressed_size &&
                                 le32(*lh, 22) == uncompressed_size && le16(*lh, 26) == name_length;
        if (!same_fields) {
            return unexpected(
                std::format("zip: '{}' local header disagrees with the central directory", name));
        }
        const std::uint16_t local_extra = le16(*lh, 28);
        const std::uint64_t local_name_at = std::uint64_t{lho} + local_header_size;
        const std::optional<detail::Bytes> local_name = slice(bytes, local_name_at, name_length);
        if (!local_name || detail::to_string(*local_name) != name) {
            return unexpected(std::format("zip: '{}' local name differs or is truncated", name));
        }
        const std::uint64_t data_offset = local_name_at + name_length + local_extra;
        const std::uint64_t data_end = data_offset + compressed_size;
        const std::optional<detail::Bytes> data = slice(bytes, data_offset, compressed_size);
        if (!data || data_end > cd_offset) {
            return unexpected(std::format("zip: '{}' data overruns the central directory", name));
        }
        if (const std::uint32_t actual = reference_crc32(*data); actual != crc) {
            return unexpected(
                std::format("zip: '{}' CRC-32 {:08x} != recorded {:08x}", name, actual, crc));
        }

        archive.entries.push_back(Entry{
            .name = std::move(name),
            .version_made_by = le16(*ch, 4),
            .version_needed = le16(*ch, 6),
            .flags = flags,
            .method = method,
            .mod_time = le16(*ch, 12),
            .mod_date = le16(*ch, 14),
            .crc32 = crc,
            .compressed_size = compressed_size,
            .uncompressed_size = uncompressed_size,
            .local_extra_length = local_extra,
            .central_extra_length = extra_length,
            .comment_length = comment_length,
            .internal_attributes = le16(*ch, 36),
            .external_attributes = le32(*ch, 38),
            .local_header_offset = lho,
            .data_offset = data_offset,
            .data = std::vector<std::byte>(data->begin(), data->end()),
        });
        expected_local = data_end;
        pos = next;
    }
    if (pos != eocd) {
        return unexpected(std::format("zip: {} trailing central directory bytes", eocd - pos));
    }
    if (expected_local != cd_offset) {
        return unexpected(std::format("zip: {} unaccounted bytes before the central directory",
                                      cd_offset - expected_local));
    }
    return archive;
}

/// Reads `path` whole and parses it with read_archive.
[[nodiscard]] inline std::expected<Archive, std::string>
read_archive_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("zip: cannot open {}", path.string()));
    }
    const std::string contents{std::istreambuf_iterator<char>{file},
                               std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return std::unexpected(std::format("zip: cannot read {}", path.string()));
    }
    return read_archive(bytes_of(contents));
}

} // namespace mangadex_dl::test::zip
