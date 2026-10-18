// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real clock and the instant (virtual) clock behind the Clock seam (net/transport.hpp).
#include "mangadex_dl/net/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>

namespace mangadex_dl {
namespace {

/// Real time. Stateless apart from the per-call wait objects, so it is trivially thread-safe.
class SystemClock final : public Clock {
public:
    SystemClock() = default;

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const noexcept override {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] bool sleep_until(std::chrono::steady_clock::time_point deadline,
                                   std::stop_token stop) override {
        if (stop.stop_requested()) {
            return false;
        }
        // Each call waits on its own mutex and condition variable: nothing ever notifies them
        // except the stop callback that wait_until registers on `stop`, so concurrent sleepers
        // never wake each other. The predicate is never true; spurious wake-ups loop inside
        // wait_until until the deadline passes or a stop is requested.
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock{mutex};
        static_cast<void>(wake.wait_until(lock, stop, deadline, [] { return false; }));
        return !stop.stop_requested();
    }
};

/// Virtual time: sleep_until jumps forward to the deadline instead of waiting. Both time lines
/// advance together, by the same offset, from where they were at construction. The offset stops
/// at max_offset (100 years), so now() and wall_now() never overflow their nanosecond
/// time_points (system_clock's ends in 2262) whatever deadline a caller passes.
class InstantClock final : public Clock {
public:
    InstantClock() = default;

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return steady_start_ + offset();
    }

    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const noexcept override {
        return wall_start_ +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(offset());
    }

    [[nodiscard]] bool sleep_until(std::chrono::steady_clock::time_point deadline,
                                   std::stop_token stop) override {
        if (stop.stop_requested()) {
            return false;
        }
        if (deadline <= steady_start_) {
            return true;
        }
        // deadline > steady_start_ (a real, positive steady time), so this cannot overflow.
        const Rep wanted = std::min((deadline - steady_start_).count(), max_offset.count());
        // Advance to max(current, wanted): concurrent sleepers never move time backwards.
        Rep current = offset_.load(std::memory_order_relaxed);
        while (current < wanted) {
            // On failure `current` is reloaded; on success the loop ends.
            if (offset_.compare_exchange_weak(current, wanted, std::memory_order_relaxed)) {
                break;
            }
        }
        return true;
    }

private:
    using Duration = std::chrono::steady_clock::duration;
    using Rep = Duration::rep;

    static constexpr Duration max_offset =
        std::chrono::duration_cast<Duration>(std::chrono::years{100});

    [[nodiscard]] Duration offset() const noexcept {
        return Duration{offset_.load(std::memory_order_relaxed)};
    }

    std::chrono::steady_clock::time_point steady_start_{std::chrono::steady_clock::now()};
    std::chrono::system_clock::time_point wall_start_{std::chrono::system_clock::now()};
    std::atomic<Rep> offset_{0}; ///< virtual time elapsed since construction, in steady ticks
};

static_assert(std::atomic<std::chrono::steady_clock::rep>::is_always_lock_free);

} // namespace

Clock& system_clock() {
    // Function-local static: initialization is thread-safe, and it lives until exit.
    static SystemClock clock;
    return clock;
}

std::unique_ptr<Clock> make_instant_clock() { return std::make_unique<InstantClock>(); }

} // namespace mangadex_dl
