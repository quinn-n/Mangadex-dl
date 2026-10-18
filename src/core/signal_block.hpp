// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): blocks the asynchronous signals in the calling thread while
// new threads are created, so every core worker thread (FetchPool workers, the InterruptBridge
// poller, the ApiClient's MD@H reporter) starts with them blocked. Used by fetch_pool.cpp,
// interrupt.cpp and api_client.cpp.
#pragma once

// NOLINTNEXTLINE(modernize-deprecated-headers): pthread_sigmask and sigset_t are POSIX <signal.h>
#include <signal.h>

namespace mangadex_dl::detail {

/// Blocks SIGINT, SIGTERM and SIGPIPE in the calling thread while alive, and restores the
/// previous mask (exactly) on destruction. A thread created meanwhile inherits the blocked mask,
/// so it never runs a single instruction with those signals deliverable: process-directed
/// SIGINT/SIGTERM land on the main thread, and a SIGPIPE raised by a socket write on a worker
/// never kills the process. Construct it right before the threads, in a scope of its own.
class ScopedAsyncSignalBlock {
public:
    ScopedAsyncSignalBlock() noexcept {
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
        sigset_t set{};
        static_cast<void>(sigemptyset(&set));
        static_cast<void>(sigaddset(&set, SIGINT));
        static_cast<void>(sigaddset(&set, SIGTERM));
        static_cast<void>(sigaddset(&set, SIGPIPE));
        blocked_ = pthread_sigmask(SIG_BLOCK, &set, &previous_) == 0;
    }
    ScopedAsyncSignalBlock(const ScopedAsyncSignalBlock&) = delete;
    ScopedAsyncSignalBlock& operator=(const ScopedAsyncSignalBlock&) = delete;
    ScopedAsyncSignalBlock(ScopedAsyncSignalBlock&&) = delete;
    ScopedAsyncSignalBlock& operator=(ScopedAsyncSignalBlock&&) = delete;
    ~ScopedAsyncSignalBlock() {
        if (blocked_) {
            static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
        }
    }

private:
    sigset_t previous_{};
    bool blocked_ = false;
};

} // namespace mangadex_dl::detail
