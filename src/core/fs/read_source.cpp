// SPDX-License-Identifier: GPL-3.0-or-later
//
// FdReadSource (skip-existing plan T5): positioned, bounded reads with pread(2).
//
// The range is checked against the size recorded at inspection before any syscall, so an
// untrusted offset or length (taken from a zip header) never reaches the kernel unchecked. The
// loop follows fact-check V13: pread may return a positive short count (the MAX_RW_COUNT cap, a
// signal after a partial transfer, NFS or FUSE), so any positive count advances and continues;
// EINTR retries; only a 0 return before the buffer is full is short_read (the file shrank).
//
// Every pread goes through pread_attempt, which honours the test-only faults of
// fs/read_source_testing.hpp (a short count, an EINTR) so the unit tests reach both retry
// branches on a real file. With the faults off it is a plain pread.
#include "mangadex_dl/fs/read_source.hpp"

#include "fs/read_source_testing.hpp"
#include "mangadex_dl/fs/output_dir.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

// Test-only fault switches (fs/read_source_testing.hpp). Atomics, so flipping one is never a data
// race; relaxed ordering is enough because each is an independent value that publishes nothing.
struct PreadFaults {
    std::atomic<std::size_t> short_count{0}; ///< 0: off
    std::atomic<int> eintr_after{-1};        ///< negative: off
    std::atomic<bool> counting{false};
    std::atomic<std::uint64_t> attempts{0};
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): test-only fault switches
constinit PreadFaults g_pread_faults{};

/// One pread(2) of at most rest.size() bytes at `position`, through the test faults: the result
/// and errno of pread, or -1 with errno EINTR for an injected EINTR. With every fault off, this
/// only loads three atomics and calls pread with the full length.
[[nodiscard]] ssize_t pread_attempt(int fd, std::span<std::byte> rest, off_t position) noexcept {
    if (g_pread_faults.counting.load(std::memory_order_relaxed)) {
        g_pread_faults.attempts.fetch_add(1, std::memory_order_relaxed);
    }
    if (const int after = g_pread_faults.eintr_after.load(std::memory_order_relaxed); after >= 0) {
        g_pread_faults.eintr_after.store(after - 1, std::memory_order_relaxed); // 0 -> -1: off
        if (after == 0) {
            errno = EINTR;
            return -1;
        }
    }
    std::size_t length = rest.size();
    if (const std::size_t cap = g_pread_faults.short_count.load(std::memory_order_relaxed);
        cap != 0 && cap < length) {
        g_pread_faults.short_count.store(0, std::memory_order_relaxed);
        length = cap;
    }
    return ::pread(fd, rest.data(), length, position);
}

} // namespace

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
        const ssize_t got = pread_attempt(fd_->get(), rest, static_cast<off_t>(position));
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

namespace testing {

void force_pread_short_count(std::size_t count) noexcept {
    g_pread_faults.short_count.store(count, std::memory_order_relaxed);
}

void force_pread_eintr(int after) noexcept {
    g_pread_faults.eintr_after.store(after < 0 ? -1 : after, std::memory_order_relaxed);
}

void count_pread_attempts(bool on) noexcept {
    g_pread_faults.attempts.store(0, std::memory_order_relaxed);
    g_pread_faults.counting.store(on, std::memory_order_relaxed);
}

std::uint64_t pread_attempts() noexcept {
    return g_pread_faults.attempts.load(std::memory_order_relaxed);
}

bool pread_faults_pending() noexcept {
    return g_pread_faults.short_count.load(std::memory_order_relaxed) != 0 ||
           g_pread_faults.eintr_after.load(std::memory_order_relaxed) >= 0;
}

} // namespace testing

} // namespace mangadex_dl
