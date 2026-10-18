// SPDX-License-Identifier: GPL-3.0-or-later
//
// STORED-only, non-ZIP64 zip writer (download plan T5; layout per PKWARE APPNOTE.TXT 4.3).
//
//   [local header 30 B + name][data] ... [central header 46 B + name] ... [EOCD 22 B]
//
// Every entry: version needed 1.0, general-purpose flags 0 (no data descriptor: sizes and CRC
// are known before the local header is written; no UTF-8 flag: names are restricted to printable
// ASCII by the frozen interface), method 0 (STORED), DOS time 00:00:00 and date 1980-01-01, no
// extra field, no comments, external attributes 0. The output is a pure function of the
// (name, data) sequence.
//
// Limits (frozen interface, download-plan-rev1 T5): 0xFFFF and 0xFFFFFFFF are the ZIP64
// sentinels, so at most zip_max_entries (65534) entries, and every local header offset, size,
// central directory offset and central directory size is <= zip_max_u32 (0xFFFFFFFE). Positions
// are tracked in uint64_t; each limit is checked before anything is written, so a rejected entry
// leaves the archive unchanged and the writer usable.
#include "mangadex_dl/zip.hpp"

#include "mangadex_dl/error.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

// ---- CRC-32: slicing-by-8 over tables built at compile time ----------------------------------

using CrcRow = std::array<std::uint32_t, 256>;
using CrcTables = std::array<CrcRow, 8>;
using CrcRowView = std::span<const std::uint32_t, 256>;

// Evaluated at compile time only (crc_tables is constexpr), so an out-of-range index would be a
// compile error. Non-constant indices go through fixed-extent spans, as clang-tidy asks.
[[nodiscard]] constexpr CrcTables make_crc_tables() noexcept {
    CrcTables tables{};
    const std::span<std::uint32_t, 256> first{tables[0]};
    for (std::uint32_t i = 0; i < 256U; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1U) != 0U ? (c >> 1U) ^ 0xEDB88320U : c >> 1U;
        }
        first[i] = c;
    }
    // Row k, entry i: the CRC register after byte i followed by k zero bytes.
    const std::span<CrcRow, 8> rows{tables};
    for (std::size_t k = 1; k < rows.size(); ++k) {
        const CrcRowView prev{rows[k - 1]};
        const std::span<std::uint32_t, 256> row{rows[k]};
        for (std::size_t i = 0; i < row.size(); ++i) {
            row[i] = (prev[i] >> 8U) ^ first[prev[i] & 0xFFU];
        }
    }
    return tables;
}

constexpr CrcTables crc_tables = make_crc_tables();

static_assert(crc_tables[0][1] == 0x77073096U, "CRC-32 table (reflected 0xEDB88320)");

/// Entry `value & 0xFF` of a table row: always in range, whatever `value` is. The iterator form
/// avoids the per-access assertion and instrumentation of operator[] (span or array), which
/// dominates the cost of CRC-32 in the sanitized dev build. ASan still checks every load.
[[nodiscard]] constexpr std::uint32_t lookup(const CrcRow& row, std::uint32_t value) noexcept {
    return *std::next(row.cbegin(), value & 0xFFU);
}

// ---- little-endian record building -----------------------------------------------------------

constexpr std::uint32_t local_header_signature = 0x04034B50U;
constexpr std::uint32_t central_header_signature = 0x02014B50U;
constexpr std::uint32_t end_of_central_directory_signature = 0x06054B50U;
constexpr std::size_t local_header_size = 30;
constexpr std::size_t central_header_size = 46;
constexpr std::uint16_t version_needed = 10;  // 1.0: STORED, no directories, no ZIP64
constexpr std::uint16_t version_made_by = 10; // host 0 (MS-DOS/FAT attributes), spec 1.0
constexpr std::uint16_t dos_time = 0;         // 00:00:00
constexpr std::uint16_t dos_date = 0x0021;    // (1980 - 1980) << 9 | 1 << 5 | 1: 1980-01-01
constexpr std::size_t max_name_size = 0xFFFF; // the 16-bit name length field
constexpr std::size_t central_flush_size = std::size_t{64} * 1024;

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>(value >> 8U));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    put_u16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    put_u16(out, static_cast<std::uint16_t>(value >> 16U));
}

void put_name(std::vector<std::byte>& out, std::string_view name) {
    for (const char c : name) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

[[nodiscard]] std::unexpected<Error> zip_error(std::string message) {
    return std::unexpected(Error{.code = Errc::failure, .message = std::move(message)});
}

/// Printable ASCII, '/'-separated, relative, no empty, "." or ".." component (APPNOTE 4.4.17:
/// forward slashes only, no drive letter or leading slash). Returns the reason it is invalid.
[[nodiscard]] std::string_view invalid_name_reason(std::string_view name) noexcept {
    if (name.empty()) {
        return "is empty";
    }
    if (name.size() > max_name_size) {
        return "is longer than 65535 bytes";
    }
    const bool printable_ascii = std::ranges::all_of(name, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x20U && byte < 0x7FU;
    });
    if (!printable_ascii) {
        return "contains a byte outside printable ASCII";
    }
    if (name.contains('\\')) {
        return "contains a backslash";
    }
    std::string_view rest = name;
    while (true) {
        const std::size_t slash = rest.find('/');
        const std::string_view component = rest.substr(0, slash);
        if (component.empty() || component == "." || component == "..") {
            return R"(has an empty, "." or ".." path component)";
        }
        if (slash == std::string_view::npos) {
            return {};
        }
        rest.remove_prefix(slash + 1);
    }
}

/// What the central directory needs to remember about one entry.
struct CentralRecord {
    std::string name{};
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
};

} // namespace

// ---- MemorySink ------------------------------------------------------------------------------

Result<void> MemorySink::write(std::span<const std::byte> data) {
    if (data.size() > bytes_.max_size() - bytes_.size()) {
        return zip_error("in-memory archive is too large");
    }
    if (data.empty()) {
        return {};
    }
    // vector::insert requires the source range not to alias the vector. A caller that passes a
    // span into bytes() (against the documented contract) still gets defined behaviour: copy
    // first, because the insert may reallocate and free the source. std::less gives a total
    // order on unrelated pointers, so the comparison itself is well defined.
    const std::less<const std::byte*> before{};
    const std::byte* const first = bytes_.data();
    const std::byte* const last = std::to_address(bytes_.cend());
    const bool aliases =
        !bytes_.empty() && !before(data.data(), first) && before(data.data(), last);
    if (aliases) {
        const std::vector<std::byte> copy(data.begin(), data.end());
        bytes_.insert(bytes_.end(), copy.begin(), copy.end());
    } else {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }
    return {};
}

// ---- CRC-32 ----------------------------------------------------------------------------------

std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc) noexcept {
    const CrcRow& t0 = crc_tables[0];
    const CrcRow& t1 = crc_tables[1];
    const CrcRow& t2 = crc_tables[2];
    const CrcRow& t3 = crc_tables[3];
    const CrcRow& t4 = crc_tables[4];
    const CrcRow& t5 = crc_tables[5];
    const CrcRow& t6 = crc_tables[6];
    const CrcRow& t7 = crc_tables[7];
    std::uint32_t c = ~crc;
    // 8-byte words are copied out a block at a time: one bounds-checked memcpy per block instead
    // of per-byte span accessors, which dominated the cost in sanitized builds. The words are
    // read little-endian, whatever the host order.
    constexpr std::size_t word_size = sizeof(std::uint64_t);
    constexpr std::size_t block_words = 256;
    std::array<std::uint64_t, block_words> words{};
    std::span<const std::byte> rest = data;
    while (rest.size() >= word_size) {
        const std::size_t count = std::min(block_words, rest.size() / word_size);
        const std::span<const std::byte> block = rest.first(count * word_size);
        std::memcpy(words.data(), block.data(), block.size());
        for (std::uint64_t word : std::span{words}.first(count)) {
            if constexpr (std::endian::native == std::endian::big) {
                word = std::byteswap(word);
            }
            const std::uint32_t lo = c ^ static_cast<std::uint32_t>(word & 0xFFFFFFFFU);
            const auto hi = static_cast<std::uint32_t>(word >> 32U);
            c = lookup(t7, lo) ^ lookup(t6, lo >> 8U) ^ lookup(t5, lo >> 16U) ^
                lookup(t4, lo >> 24U) ^ lookup(t3, hi) ^ lookup(t2, hi >> 8U) ^
                lookup(t1, hi >> 16U) ^ lookup(t0, hi >> 24U);
        }
        rest = rest.subspan(block.size());
    }
    for (const std::byte b : rest) {
        c = (c >> 8U) ^ lookup(t0, c ^ std::to_integer<std::uint32_t>(b));
    }
    return ~c;
}

// ---- ZipWriter -------------------------------------------------------------------------------

struct ZipWriter::State {
    ByteSink* sink = nullptr; ///< borrowed; outlives the writer (constructor contract)
    std::vector<CentralRecord> entries{};
    std::set<std::string, std::less<>> names{}; ///< duplicate detection
    std::uint64_t offset = 0;                   ///< bytes handed to the sink so far
    bool finished = false;
    bool failed = false; ///< a sink write failed: the archive is truncated, refuse further use

    /// Hands `data` to the sink and advances the offset; a failure poisons the writer.
    [[nodiscard]] Result<void> write(std::span<const std::byte> data) {
        if (data.empty()) {
            return {};
        }
        Result<void> result = sink->write(data);
        if (!result) {
            failed = true;
            return result; // the sink's own error (Errc::io, Errc::interrupted, ...) unchanged
        }
        offset += data.size();
        return {};
    }

    [[nodiscard]] Result<void> usable() const {
        if (failed) {
            return zip_error("zip archive is unusable after an earlier write failure");
        }
        if (finished) {
            return zip_error("zip archive is already finished");
        }
        return {};
    }
};

ZipWriter::ZipWriter(ByteSink& sink) : state_(std::make_unique<State>()) { state_->sink = &sink; }

ZipWriter::~ZipWriter() = default;

// Precondition: `name` and `data` must not borrow from the sink's own storage (for example a
// span of MemorySink::bytes()). The local header is written before the data, and that write may
// reallocate the sink's buffer, leaving `data` dangling for the second write. (`name` is copied
// before anything is written today, but the contract covers both.) Callers pass page images and
// generated XML, never the archive being written.
Result<void> ZipWriter::add_stored(std::string_view name, std::span<const std::byte> data) {
    State& s = *state_;
    if (Result<void> ok = s.usable(); !ok) {
        return ok;
    }
    if (const std::string_view reason = invalid_name_reason(name); !reason.empty()) {
        return zip_error(std::format("zip entry name {}", reason));
    }
    if (s.names.contains(name)) {
        return zip_error(std::format("duplicate zip entry name '{}'", name));
    }
    if (s.entries.size() >= zip_max_entries) {
        return zip_error(std::format("zip archive would exceed {} entries", zip_max_entries));
    }
    // The entry's end is the next entry's local header offset or the central directory offset,
    // so it must not pass zip_max_u32 either. offset <= zip_max_u32 (an invariant), the header is
    // <= 30 + 0xFFFF bytes, and data.size() is checked first, so the sum cannot wrap.
    if (data.size() > zip_max_u32) {
        return zip_error(std::format("zip entry '{}' is larger than {} bytes", name, zip_max_u32));
    }
    const std::uint64_t entry_end = s.offset + local_header_size + name.size() + data.size();
    if (entry_end > zip_max_u32) {
        return zip_error(
            std::format("zip archive would exceed {} bytes (no ZIP64 support)", zip_max_u32));
    }

    const auto size = static_cast<std::uint32_t>(data.size());
    const auto offset = static_cast<std::uint32_t>(s.offset);
    const std::uint32_t crc = crc32(data);

    std::vector<std::byte> header;
    header.reserve(local_header_size + name.size());
    put_u32(header, local_header_signature);
    put_u16(header, version_needed);
    put_u16(header, 0); // general-purpose flags
    put_u16(header, 0); // method: STORED
    put_u16(header, dos_time);
    put_u16(header, dos_date);
    put_u32(header, crc);
    put_u32(header, size); // compressed size
    put_u32(header, size); // uncompressed size
    put_u16(header, static_cast<std::uint16_t>(name.size()));
    put_u16(header, 0); // extra field length
    put_name(header, name);

    // Record the entry before writing: if a write fails the writer is poisoned anyway.
    s.names.emplace(name);
    s.entries.push_back(
        CentralRecord{.name = std::string{name}, .crc = crc, .size = size, .offset = offset});
    if (Result<void> ok = s.write(header); !ok) {
        return ok;
    }
    return s.write(data);
}

Result<void> ZipWriter::finish() {
    State& s = *state_;
    if (Result<void> ok = s.usable(); !ok) {
        return ok;
    }
    s.finished = true;

    // s.offset <= zip_max_u32 holds after every accepted entry.
    const std::uint64_t cd_offset = s.offset;
    std::uint64_t cd_size = 0;
    for (const CentralRecord& entry : s.entries) {
        cd_size += central_header_size + entry.name.size();
    }
    if (cd_size > zip_max_u32) {
        return zip_error(std::format("zip central directory would exceed {} bytes", zip_max_u32));
    }

    std::vector<std::byte> buffer;
    buffer.reserve(central_flush_size + central_header_size + max_name_size);
    for (const CentralRecord& entry : s.entries) {
        put_u32(buffer, central_header_signature);
        put_u16(buffer, version_made_by);
        put_u16(buffer, version_needed);
        put_u16(buffer, 0); // general-purpose flags
        put_u16(buffer, 0); // method: STORED
        put_u16(buffer, dos_time);
        put_u16(buffer, dos_date);
        put_u32(buffer, entry.crc);
        put_u32(buffer, entry.size); // compressed size
        put_u32(buffer, entry.size); // uncompressed size
        put_u16(buffer, static_cast<std::uint16_t>(entry.name.size()));
        put_u16(buffer, 0); // extra field length
        put_u16(buffer, 0); // file comment length
        put_u16(buffer, 0); // disk number start
        put_u16(buffer, 0); // internal attributes
        put_u32(buffer, 0); // external attributes
        put_u32(buffer, entry.offset);
        put_name(buffer, entry.name);
        if (buffer.size() >= central_flush_size) {
            if (Result<void> ok = s.write(buffer); !ok) {
                return ok;
            }
            buffer.clear();
        }
    }

    // entries.size() <= zip_max_entries < 0xFFFF and both CD fields <= zip_max_u32, so the
    // narrowing casts below are exact.
    const auto count = static_cast<std::uint16_t>(s.entries.size());
    put_u32(buffer, end_of_central_directory_signature);
    put_u16(buffer, 0); // number of this disk
    put_u16(buffer, 0); // disk where the central directory starts
    put_u16(buffer, count);
    put_u16(buffer, count);
    put_u32(buffer, static_cast<std::uint32_t>(cd_size));
    put_u32(buffer, static_cast<std::uint32_t>(cd_offset));
    put_u16(buffer, 0); // comment length
    return s.write(buffer);
}

std::uint64_t ZipWriter::bytes_written() const noexcept { return state_->offset; }

std::size_t ZipWriter::entry_count() const noexcept { return state_->entries.size(); }

} // namespace mangadex_dl
