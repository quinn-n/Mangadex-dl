// SPDX-License-Identifier: GPL-3.0-or-later
//
// std::cout's buffer (process_io() installs it; finish_stdout() reads it). A header of its own so
// that io.hpp, which every command includes, does not pull in <cstdio>: glibc's <stdio.h> also
// declares ssize_t and off_t, which would then no longer come from the headers that use them.
// Defined in io.cpp.
#pragma once

#include <atomic>
#include <cstdio>
#include <streambuf>

namespace mangadex_dl::app {

/// An output-only std::streambuf that passes every call straight through to a C FILE*, exactly as
/// libstdc++'s stdio_sync_filebuf does for std::cout (xsputn -> fwrite, overflow -> putc or fflush,
/// sync -> fflush). It has no buffer of its own, so the FILE's buffering is unchanged and
/// sync_with_stdio semantics hold. It also records the errno of the FIRST failed write or flush:
/// glibc discards the FILE's buffer after a failed write, so a later fflush has nothing to write
/// and succeeds, and the errno of the real failure would otherwise be lost (std::cerr is tied to
/// std::cout, so any stderr write flushes stdout first). Not seekable (nothing seeks std::cout).
/// errno is left as the failing call set it, and is preserved across calls that succeed.
class RecordingStdioBuf final : public std::streambuf {
public:
    /// `file` is borrowed and must outlive the buffer (for std::cout: `stdout`, never closed).
    explicit RecordingStdioBuf(std::FILE* file) noexcept : file_{file} {}

    /// The errno of the first failed write or flush (or of the first record() call), 0 if none.
    [[nodiscard]] int first_errno() const noexcept {
        return first_errno_.load(std::memory_order_relaxed);
    }

    /// Records `error` unless it is 0 or an earlier error is already recorded.
    void record(int error) noexcept;

protected:
    int_type overflow(int_type ch) override;
    std::streamsize xsputn(const char_type* text, std::streamsize count) override;
    int sync() override;

private:
    std::FILE* file_ = nullptr; ///< borrowed; never null
    /// Atomic so the buffer stays as race-free as stdio itself (glibc locks each FILE call). Today
    /// only the main thread writes io.out: every DownloadObserver call runs on the calling thread
    /// (core/download/execute_phase.cpp), and pool workers only queue lines for it to drain.
    std::atomic<int> first_errno_{0};
};

} // namespace mangadex_dl::app
