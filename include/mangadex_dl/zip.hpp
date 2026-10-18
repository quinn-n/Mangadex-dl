// SPDX-License-Identifier: GPL-3.0-or-later
//
// A STORED-only (no compression), non-ZIP64 zip writer over an abstract byte sink.
// Frozen interface (download plan T1); implemented in src/core/zip.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// Most entries one archive may hold: 0xFFFF is the ZIP64 sentinel in the 16-bit count fields.
inline constexpr std::size_t zip_max_entries = 65534;
/// Largest size, offset, central-directory size or central-directory offset: 0xFFFFFFFF is the
/// ZIP64 sentinel in the 32-bit fields.
inline constexpr std::uint64_t zip_max_u32 = 0xFFFFFFFE;

/// Where archive bytes go (an in-memory buffer, or a temporary file in the output directory).
/// Abstract. Copying is deleted; moving is protected, so a derived sink may be move-only but a
/// ByteSink can never be sliced out of one. Not thread-safe: one writer at a time.
class ByteSink {
public:
    ByteSink(const ByteSink&) = delete;
    ByteSink& operator=(const ByteSink&) = delete;
    virtual ~ByteSink() = default;

    /// Appends all of `data` (or fails). The span is only read during the call.
    [[nodiscard]] virtual Result<void> write(std::span<const std::byte> data) = 0;

protected:
    ByteSink() = default;
    ByteSink(ByteSink&&) noexcept = default;
    ByteSink& operator=(ByteSink&&) noexcept = default;
};

/// Collects everything in memory (tests, and anything small).
class MemorySink final : public ByteSink {
public:
    /// Appends `data`. Errc::failure only if the allocation would overflow the vector.
    [[nodiscard]] Result<void> write(std::span<const std::byte> data) override;

    /// Everything written so far. Borrows from *this, and the next write() may reallocate it: do
    /// not keep the reference (or a span into it) across a write or past the sink's lifetime.
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_{};
};

/// CRC-32 (reflected polynomial 0xEDB88320, as zip uses it). Pass the previous result as `crc` to
/// continue over several buffers; start with 0.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc = 0) noexcept;

/// Writes local headers and data as entries are added, then the central directory on finish().
/// Fixed DOS timestamp (1980-01-01 00:00), version needed 10, no extra fields, no data
/// descriptors: identical input gives identical bytes. Entry names are ASCII, unique, non-empty
/// and at most 65535 bytes. Enforces zip_max_entries and zip_max_u32 (checked 64-bit offsets).
///
/// Borrows the sink: it must outlive the writer (declare the sink first). Not thread-safe.
/// Neither copyable nor movable. The state lives in zip.cpp (pimpl), so the layout can change
/// without touching this frozen interface.
class ZipWriter {
public:
    explicit ZipWriter(ByteSink& sink MANGADEX_DL_LIFETIMEBOUND);
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ZipWriter(ZipWriter&&) = delete;
    ZipWriter& operator=(ZipWriter&&) = delete;
    ~ZipWriter();

    /// Writes one STORED entry. Errc::failure for an invalid or duplicate name, for the entry
    /// that would exceed zip_max_entries, for a size or offset above zip_max_u32, after finish(),
    /// and when the sink fails (the writer is unusable afterwards). `data` is only read during
    /// the call. Precondition: neither `name` nor `data` may borrow from the sink's own storage
    /// (e.g. a span of MemorySink::bytes()): the header write may reallocate it first.
    [[nodiscard]] Result<void> add_stored(std::string_view name, std::span<const std::byte> data);

    /// Writes the central directory and the end record. Called once; later calls fail.
    [[nodiscard]] Result<void> finish();

    /// Bytes handed to the sink so far.
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

    /// Entries added so far.
    [[nodiscard]] std::size_t entry_count() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace mangadex_dl
