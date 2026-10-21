// SPDX-License-Identifier: GPL-3.0-or-later
//
// Positioned, bounded reads from an existing file (or a fake in tests). The zip index and the
// EPUB inspection read only through this seam. Frozen interface (skip-existing plan T1);
// FdReadSource is implemented in src/core/fs/read_source.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/fs/output_dir.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace mangadex_dl {

enum class ReadFault {
    io,         ///< the read failed (ReadError::error holds the errno)
    short_read, ///< the range lies outside size(), or the file ended before it was filled
};

struct ReadError {
    ReadFault fault = ReadFault::io;
    int error = 0; ///< errno for io; 0 for short_read

    friend bool operator==(const ReadError&, const ReadError&) = default;
};

/// A source of bytes with a size fixed when it was made. Abstract; neither copyable nor movable
/// (it is only ever borrowed). Not thread-safe.
class ReadSource {
public:
    ReadSource(const ReadSource&) = delete;
    ReadSource& operator=(const ReadSource&) = delete;
    ReadSource(ReadSource&&) = delete;
    ReadSource& operator=(ReadSource&&) = delete;
    virtual ~ReadSource() = default;

    /// The size every read is bounded by.
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    /// Fills all of `out` with the bytes at [offset, offset + out.size()), or fails. A range
    /// outside [0, size()) is short_read without touching the file. `out` is only written during
    /// the call; on failure its contents are unspecified.
    [[nodiscard]] virtual std::expected<void, ReadError> read_exact(std::uint64_t offset,
                                                                    std::span<std::byte> out) = 0;

protected:
    ReadSource() = default;
};

/// Reads an open regular file with pread. The size is the one recorded at inspection
/// (FileIdentity::size), so a file that grows later is still read as it was.
///
/// Borrow contract: the source keeps a pointer to the UniqueFd itself, not a copy of the number.
/// - The UniqueFd must outlive the source, and must not be moved from, reset or released while
///   the source exists (a moved-from UniqueFd holds -1, so reads then fail with io / EBADF).
/// - It must not live in a container that may reallocate (a vector of handles) while borrowed.
/// - Construction from a temporary is deleted, but no compiler catches a borrow through an
///   accessor of a temporary: never write `FdReadSource{dir.open_existing(n)->fd, size}`. Bind
///   the Result to a named local first and borrow that local's `fd`.
///
/// read_exact continues after any positive short count, retries EINTR, reports another -1 as io
/// with the errno, and reports short_read only when pread returns 0 before `out` is full.
class FdReadSource final : public ReadSource {
public:
    FdReadSource(const UniqueFd& fd MANGADEX_DL_LIFETIMEBOUND, std::uint64_t size) noexcept
        : fd_(&fd), size_(size) {}
    /// Never borrow a temporary descriptor (it would dangle at the end of the full-expression).
    FdReadSource(const UniqueFd&&, std::uint64_t) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::expected<void, ReadError> read_exact(std::uint64_t offset,
                                                            std::span<std::byte> out) override;

private:
    const UniqueFd* fd_ = nullptr; ///< borrowed, never null after construction
    std::uint64_t size_ = 0;
};

} // namespace mangadex_dl
