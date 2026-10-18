// SPDX-License-Identifier: GPL-3.0-or-later
//
// SIGINT/SIGTERM (download plan T8 + rev1). The handler only stores the signal number into a
// lock-free atomic; everything else (cancelling work, cleaning up temporaries, re-raising) runs
// on ordinary threads. InterruptBridge turns the flag into a std::stop_source request.
#include "mangadex_dl/interrupt.hpp"

#include "mangadex_dl/error.hpp"
#include "signal_block.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction and pthread_sigmask are POSIX <signal.h>
#include <signal.h>

namespace mangadex_dl {
namespace {

static_assert(std::atomic<int>::is_always_lock_free,
              "the signal handler may only touch a lock-free atomic");
static_assert(std::atomic<bool>::is_always_lock_free);

// Written by the signal handler, read by any thread. constinit: initialized before any code runs,
// so a signal can never observe it unconstructed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): async-signal flag
constinit std::atomic<int> g_interrupt_signal{0};

// Whether a ScopedInterruptHandlers is alive (at most one at a time).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): one live install at a time
constinit std::atomic<bool> g_handlers_installed{false};

inline constexpr auto bridge_poll_interval = std::chrono::milliseconds{50};

/// The SIGINT/SIGTERM handler. Async-signal-safe by construction: one relaxed store to a
/// lock-free atomic; no allocation, no locks, no errno change, no other call.
void on_interrupt_signal(int signal) noexcept {
    g_interrupt_signal.store(signal, std::memory_order_relaxed);
}

[[nodiscard]] bool is_interrupt_signal(int signal) noexcept {
    return signal == SIGINT || signal == SIGTERM;
}

[[nodiscard]] bool has_flag(int flags, int flag) noexcept {
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0U;
}

/// Whether `action` is SIG_IGN (an ignored disposition inherited from the parent).
[[nodiscard]] bool is_ignored(const struct sigaction& action) noexcept {
    return !has_flag(action.sa_flags, SA_SIGINFO) && action.sa_handler == SIG_IGN;
}

/// Installs `action` for `signal`, saving the previous disposition in `old`, unless that
/// disposition is SIG_IGN: an ignored signal stays ignored (see install()). False on a
/// sigaction failure. `installed` says whether the handler went in (and must be restored).
[[nodiscard]] bool install_one(int signal, const struct sigaction& action, struct sigaction& old,
                               bool& installed) noexcept {
    installed = false;
    if (::sigaction(signal, nullptr, &old) != 0) {
        return false;
    }
    if (is_ignored(old)) {
        return true;
    }
    if (::sigaction(signal, &action, nullptr) != 0) {
        return false;
    }
    installed = true;
    return true;
}

/// The bridge's poller thread, started with the async signals blocked.
[[nodiscard]] std::jthread start_poller(std::stop_source stop) {
    const detail::ScopedAsyncSignalBlock blocked{};
    // The lambda owns its stop_source copy (shared stop state); nothing is captured by reference.
    return std::jthread{[source = std::move(stop)](const std::stop_token& token) mutable {
        // Waiting on the jthread's own token wakes the poller as soon as the destructor requests
        // stop, instead of sleeping out the interval.
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock{mutex};
        while (!token.stop_requested()) {
            if (interrupt_requested()) {
                // stop_callbacks registered on the bridged source run here, on the poller thread.
                source.request_stop();
                return;
            }
            static_cast<void>(
                wake.wait_for(lock, token, bridge_poll_interval, [] { return false; }));
        }
    }};
}

} // namespace

struct ScopedInterruptHandlers::Saved {
    struct sigaction old_int{};
    struct sigaction old_term{};
    bool int_installed = false;  ///< false when SIGINT was inherited as SIG_IGN
    bool term_installed = false; ///< false when SIGTERM was inherited as SIG_IGN
};

// A SIGINT or SIGTERM inherited as SIG_IGN (nohup, a non-interactive shell's background job) is
// left ignored: the user or the parent asked for the process to survive that signal, so the run
// is not interruptible by it. Only the dispositions actually replaced are restored.
Result<ScopedInterruptHandlers> ScopedInterruptHandlers::install() {
    // Allocate before touching any disposition, so nothing below can throw.
    auto saved = std::make_unique<Saved>();
    if (g_handlers_installed.exchange(true)) {
        return std::unexpected(
            Error{.code = Errc::failure, .message = "interrupt handlers are already installed"});
    }

    struct sigaction action{};
    action.sa_handler = on_interrupt_signal;
    action.sa_flags = SA_RESTART;
    static_cast<void>(sigemptyset(&action.sa_mask));

    if (!install_one(SIGINT, action, saved->old_int, saved->int_installed)) {
        g_handlers_installed.store(false);
        return std::unexpected(
            Error{.code = Errc::failure, .message = "cannot install the SIGINT handler"});
    }
    if (!install_one(SIGTERM, action, saved->old_term, saved->term_installed)) {
        if (saved->int_installed) {
            static_cast<void>(::sigaction(SIGINT, &saved->old_int, nullptr));
        }
        g_handlers_installed.store(false);
        return std::unexpected(
            Error{.code = Errc::failure, .message = "cannot install the SIGTERM handler"});
    }
    return ScopedInterruptHandlers{std::move(saved)};
}

ScopedInterruptHandlers::ScopedInterruptHandlers(std::unique_ptr<Saved> saved) noexcept
    : saved_(std::move(saved)) {}

ScopedInterruptHandlers::ScopedInterruptHandlers(ScopedInterruptHandlers&& other) noexcept =
    default;

ScopedInterruptHandlers::~ScopedInterruptHandlers() {
    if (saved_) {
        if (saved_->term_installed) {
            static_cast<void>(::sigaction(SIGTERM, &saved_->old_term, nullptr));
        }
        if (saved_->int_installed) {
            static_cast<void>(::sigaction(SIGINT, &saved_->old_int, nullptr));
        }
        g_handlers_installed.store(false);
    }
}

bool interrupt_requested() noexcept {
    return g_interrupt_signal.load(std::memory_order_relaxed) != 0;
}

int interrupt_signal() noexcept { return g_interrupt_signal.load(std::memory_order_relaxed); }

void reraise_pending_interrupt() noexcept {
    const int signal = g_interrupt_signal.load(std::memory_order_relaxed);
    if (!is_interrupt_signal(signal)) {
        return;
    }
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    static_cast<void>(sigemptyset(&action.sa_mask));
    static_cast<void>(::sigaction(signal, &action, nullptr));

    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t only{};
    static_cast<void>(sigemptyset(&only));
    static_cast<void>(sigaddset(&only, signal));
    static_cast<void>(pthread_sigmask(SIG_UNBLOCK, &only, nullptr));
    // The default action terminates the process here; raise() returns only if it could not.
    static_cast<void>(::raise(signal));
}

void reset_interrupt_for_testing() noexcept {
    g_interrupt_signal.store(0, std::memory_order_relaxed);
}

void simulate_interrupt_for_testing(int signal) noexcept {
    g_interrupt_signal.store(signal, std::memory_order_relaxed);
}

InterruptBridge::InterruptBridge(std::stop_source stop) : poller_(start_poller(std::move(stop))) {}

InterruptBridge::~InterruptBridge() = default;

} // namespace mangadex_dl
