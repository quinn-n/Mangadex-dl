// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private testing seam for src/core/fs/read_source.cpp (the FdReadSource::read_exact retry loop).
// Not part of the public interface: only read_source.cpp and the unit tests include it. Test-only:
// production code never calls these, and with every switch off (the default) read_exact issues
// exactly the pread calls it would without the seam.
//
// The faults make the pread loop take the two retry branches a regular file on a local filesystem
// never takes: a positive short count (the MAX_RW_COUNT cap, a signal after a partial transfer,
// NFS or FUSE) and EINTR. The short count is a real one: the next pread asks the kernel for fewer
// bytes, so the bytes really come from the file. The EINTR never reaches the kernel.
//
// The switches are atomics, so flipping one is never a data race, but they are process-wide: arm
// them only from single-threaded test code, through ScopedReadSourceFaults, which disarms them on
// destruction. The read path only loads them while they are off; a fault is consumed (stored) only
// after a test armed it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mangadex_dl::testing {

/// One-shot: the next pread that asks for more than `count` bytes asks for `count` instead, so it
/// returns a positive short count. Stays armed through reads of `count` bytes or fewer. 0 disarms.
void force_pread_short_count(std::size_t count) noexcept;

/// One-shot: `after` further pread attempts go through, then the next attempt fails with EINTR
/// without reaching the kernel. A negative `after` disarms.
void force_pread_eintr(int after) noexcept;

/// While on, every pread attempt of FdReadSource::read_exact (an injected EINTR included) is
/// counted. Turning it on or off resets the count to 0.
void count_pread_attempts(bool on) noexcept;

/// The pread attempts counted since count_pread_attempts(true).
[[nodiscard]] std::uint64_t pread_attempts() noexcept;

/// Whether a fault armed by force_pread_short_count or force_pread_eintr has not fired yet.
[[nodiscard]] bool pread_faults_pending() noexcept;

/// Arms the requested faults and counts pread attempts for the lifetime of the object; on
/// destruction disarms both faults and stops counting. Read the count with pread_attempts() and
/// whether the faults fired with pread_faults_pending().
class ScopedReadSourceFaults {
public:
    struct Faults {
        std::size_t short_count = 0; ///< see force_pread_short_count; 0: none
        int eintr_after = -1;        ///< see force_pread_eintr; negative: none
    };

    explicit ScopedReadSourceFaults(Faults faults) noexcept {
        count_pread_attempts(true);
        force_pread_short_count(faults.short_count);
        force_pread_eintr(faults.eintr_after);
    }
    ScopedReadSourceFaults(const ScopedReadSourceFaults&) = delete;
    ScopedReadSourceFaults& operator=(const ScopedReadSourceFaults&) = delete;
    ScopedReadSourceFaults(ScopedReadSourceFaults&&) = delete;
    ScopedReadSourceFaults& operator=(ScopedReadSourceFaults&&) = delete;
    ~ScopedReadSourceFaults() {
        force_pread_short_count(0);
        force_pread_eintr(-1);
        count_pread_attempts(false);
    }
};

} // namespace mangadex_dl::testing
