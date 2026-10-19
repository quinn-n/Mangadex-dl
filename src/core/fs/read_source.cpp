// SPDX-License-Identifier: GPL-3.0-or-later
//
// FdReadSource (skip-existing plan T5): positioned, bounded reads with pread(2).
//
// The range is checked against the size recorded at inspection before any syscall, so an
// untrusted offset or length (taken from a zip header) never reaches the kernel unchecked. The
// loop follows fact-check V13: pread may return a positive short count (the MAX_RW_COUNT cap, a
// signal after a partial transfer, NFS or FUSE), so any positive count advances and continues;
// EINTR retries; only a 0 return before the buffer is full is short_read (the file shrank).
#include "mangadex_dl/fs/read_source.hpp"

#include "mangadex_dl/fs/output_dir.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {

std::expected<void, ReadError> FdReadSource::read_exact(std::uint64_t offset,
                                                        std::span<std::byte> out) {
    // Overflow-free form of `offset + out.size() > size_`: no syscall for a range outside the
    // recorded size, whatever the two values are.
    if (offset > size_ || out.size() > size_ - offset) {
        return std::unexpected(ReadError{.fault = ReadFault::short_read, .error = 0});
    }
    // offset + out.size() <= size_ now, so the sum cannot wrap. pread takes a signed off_t, so
    // the whole range must be representable (a size_ beyond off_t can only come from a caller,
    // never from fstat).
    const std::uint64_t end = offset + out.size();
    if (!std::in_range<off_t>(end)) {
        return std::unexpected(ReadError{.fault = ReadFault::io, .error = EOVERFLOW});
    }

    std::uint64_t position = offset; // <= end, so representable as off_t throughout
    std::span<std::byte> rest = out;
    while (!rest.empty()) {
        const ssize_t got =
            ::pread(fd_->get(), rest.data(), rest.size(), static_cast<off_t>(position));
        if (got > 0) {
            const auto count = static_cast<std::size_t>(got);
            if (count > rest.size()) {
                // pread never returns more than it was asked for; refuse rather than run past
                // the caller's buffer if a broken filesystem ever did.
                return std::unexpected(ReadError{.fault = ReadFault::io, .error = EIO});
            }
            rest = rest.subspan(count);
            position += count;
            continue;
        }
        if (got == 0) {
            // End of file before the range was filled: the file is shorter than recorded.
            return std::unexpected(ReadError{.fault = ReadFault::short_read, .error = 0});
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        return std::unexpected(ReadError{.fault = ReadFault::io, .error = err});
    }
    return {};
}

} // namespace mangadex_dl
