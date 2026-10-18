// SPDX-License-Identifier: GPL-3.0-or-later
//
// SIGINT/SIGTERM handling: a flag-only async-signal-safe handler, a bridge into std::stop_source,
// and the re-raise in main() after cleanup. Frozen interface (download plan T1); implemented in
// src/core/interrupt.cpp.
#pragma once

#include "mangadex_dl/error.hpp"

#include <memory>
#include <stop_token>
#include <thread>

namespace mangadex_dl {

/// Installs the flag handler for SIGINT and SIGTERM (sigaction) and restores the previous
/// dispositions on destruction, on every path. Move-constructible only (Result needs it); a
/// moved-from object restores nothing. Install from the main thread, at most one at a time.
class ScopedInterruptHandlers {
public:
    [[nodiscard]] static Result<ScopedInterruptHandlers> install();

    ScopedInterruptHandlers(const ScopedInterruptHandlers&) = delete;
    ScopedInterruptHandlers& operator=(const ScopedInterruptHandlers&) = delete;
    ScopedInterruptHandlers(ScopedInterruptHandlers&& other) noexcept;
    ScopedInterruptHandlers& operator=(ScopedInterruptHandlers&&) = delete;
    ~ScopedInterruptHandlers();

private:
    struct Saved; ///< the previous sigactions, defined in interrupt.cpp

    explicit ScopedInterruptHandlers(std::unique_ptr<Saved> saved) noexcept;

    std::unique_ptr<Saved> saved_{};
};

/// True once SIGINT or SIGTERM arrived (since the last reset). Async-signal-safe and thread-safe.
[[nodiscard]] bool interrupt_requested() noexcept;

/// The signal that arrived (SIGINT or SIGTERM), or 0.
[[nodiscard]] int interrupt_signal() noexcept;

/// If a signal is pending: restore SIG_DFL, unblock it and raise it, so the process dies by that
/// signal (the shell sees 130/143). Returns when nothing is pending. Called only from main(),
/// after finish_stdout().
void reraise_pending_interrupt() noexcept;

/// Tests only: clear the flag.
void reset_interrupt_for_testing() noexcept;

/// Tests only: set the flag as if `signal` had arrived (no signal is sent).
void simulate_interrupt_for_testing(int signal) noexcept;

/// While alive, a jthread polls interrupt_requested() every 50 ms and calls request_stop() on
/// the given stop_source (a copy that shares its stop state). Destroy it before anything the
/// stop source cancels. Neither copyable nor movable; the destructor stops and joins the thread.
class InterruptBridge {
public:
    explicit InterruptBridge(std::stop_source stop);
    InterruptBridge(const InterruptBridge&) = delete;
    InterruptBridge& operator=(const InterruptBridge&) = delete;
    InterruptBridge(InterruptBridge&&) = delete;
    InterruptBridge& operator=(InterruptBridge&&) = delete;
    ~InterruptBridge();

private:
    std::jthread poller_{};
};

} // namespace mangadex_dl
