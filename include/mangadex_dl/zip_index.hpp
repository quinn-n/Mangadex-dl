// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bounded reader for the central directory of an existing STORED, non-ZIP64 zip (the EPUBs
// ZipWriter makes), over an abstract ReadSource. Every field is untrusted: all offset math is done
// in std::uint64_t on widened u16/u32 fields, and every slice is bounds-checked before it is read.
// Frozen interface (skip-existing plan T1); implemented in src/core/zip_index.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/fs/read_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// Largest central directory accepted (checked before allocating).
inline constexpr std::uint64_t max_central_directory_bytes = std::uint64_t{8} * 1024 * 1024;

/// One central-directory entry, as recorded (not yet checked against its local header).
struct ZipEntryRecord {
    std::string name{}; ///< an owned copy of the name bytes
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_header_offset = 0;

    friend bool operator==(const ZipEntryRecord&, const ZipEntryRecord&) = default;
};

enum class ZipFault {
    read_error,         ///< the source reported io (ZipError::error holds the errno)
    short_read,         ///< the source reported short_read
    size_out_of_range,  ///< smaller than an end record, or larger than zip_max_u32
    no_end_record,      ///< no end-of-central-directory record exactly at size - 22
    unsupported,        ///< multi-disk, a comment, a ZIP64 sentinel, or too many entries
    bad_directory,      ///< the central directory is out of bounds, too large or inconsistent
    duplicate_name,     ///< two entries share a name
    bad_local_header,   ///< a local header disagrees with its central-directory entry
    data_out_of_bounds, ///< an entry's data does not lie within [0, central_directory_offset)
};

struct ZipError {
    ZipFault fault = ZipFault::bad_directory;
    int error = 0; ///< errno for read_error; 0 otherwise

    friend bool operator==(const ZipError&, const ZipError&) = default;
};

/// The parsed central directory. Immutable after read(): entries are kept in central-directory
/// order, with a name index for find(). Move-only value; it owns copies of everything and keeps
/// no reference to the source or to any read buffer.
class ZipIndex {
public:
    /// Reads and validates the end record and the whole central directory (at most
    /// max_central_directory_bytes, at most zip_max_entries entries, unique names).
    [[nodiscard]] static std::expected<ZipIndex, ZipError> read(ReadSource& source);

    ZipIndex(const ZipIndex&) = delete;
    ZipIndex& operator=(const ZipIndex&) = delete;
    ZipIndex(ZipIndex&&) noexcept = default;
    ZipIndex& operator=(ZipIndex&&) noexcept = default;
    ~ZipIndex() = default;

    /// The entry named `name`, or nullptr. Borrows from *this: do not keep the pointer past the
    /// index. Calling it on an rvalue (`ZipIndex::read(s).value().find(n)`) is deleted. No compiler
    /// catches `ZipIndex::read(s)->find(n)`, which dangles too: bind the Result to a named local.
    [[nodiscard]] const ZipEntryRecord*
    find(std::string_view name) const& noexcept MANGADEX_DL_LIFETIMEBOUND;
    const ZipEntryRecord* find(std::string_view name) const&& = delete;

    /// Every entry, in central-directory order. Borrows from *this; deleted on an rvalue.
    [[nodiscard]] const std::vector<ZipEntryRecord>&
    entries() const& noexcept MANGADEX_DL_LIFETIMEBOUND {
        return entries_;
    }
    const std::vector<ZipEntryRecord>& entries() const&& = delete;

    /// Where the central directory starts: every entry's data must end at or before it.
    [[nodiscard]] std::uint64_t central_directory_offset() const noexcept {
        return central_directory_offset_;
    }

private:
    ZipIndex() = default;

    std::vector<ZipEntryRecord> entries_{};
    std::vector<std::size_t> by_name_{}; ///< indices into entries_, sorted by name
    std::uint64_t central_directory_offset_ = 0;
};

/// The offset of `entry`'s first data byte. Reads its local header and requires the signature,
/// flags, method, CRC, sizes, name length and name bytes to equal the central directory, and the
/// data to lie within [0, index.central_directory_offset()). `entry` must come from `index`.
[[nodiscard]] std::expected<std::uint64_t, ZipError>
locate_entry_data(ReadSource& source, const ZipIndex& index, const ZipEntryRecord& entry);

} // namespace mangadex_dl
