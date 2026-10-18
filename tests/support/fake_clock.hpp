#pragma once
// tests/support/fake_clock.hpp: a manual, thread-safe Clock for unit tests.
//
// Time only moves when a test calls advance() or when someone sleeps: sleep_until jumps the
// virtual time forward to the deadline (never backwards, even with concurrent sleepers), records
// the sleep and returns at once. A stop token that is already requested makes sleep_until return
// false without advancing (the sleep is still recorded, with `stopped = true`).
//
//     mangadex_dl::test::FakeClock clock;
//     mangadex_dl::RateLimiter limiter{clock, std::chrono::milliseconds{250}};
//     REQUIRE(limiter.acquire({}));
//     REQUIRE(limiter.acquire({}));
//     CHECK(clock.sleeps().size() == 1);
//
// Virtual time stops at max_elapsed (100 years), so now() and wall_now() never overflow their
// nanosecond time_points; wall_now() also saturates for an extreme `wall_start`.
//
// The optional on_sleep hook runs on the sleeping thread, outside the internal lock, before
// sleep_until checks the stop token: a test can request a stop "during" a wait with it.

#include "mangadex_dl/net/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

namespace mangadex_dl::test {

struct SleepRecord {
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::milliseconds requested{}; ///< deadline - now() at the call, never negative
    bool stopped = false;                  ///< the stop token was requested; time did not move

    friend bool operator==(const SleepRecord&, const SleepRecord&) = default;
};

class FakeClock final : public Clock {
public:
    using Duration = std::chrono::steady_clock::duration;

    static constexpr Duration max_elapsed =
        std::chrono::duration_cast<Duration>(std::chrono::years{100});

    /// The virtual steady time starts at steady_start, the wall time at `wall_start`.
    static constexpr std::chrono::steady_clock::time_point steady_start{std::chrono::hours{1}};
    static constexpr std::chrono::system_clock::time_point default_wall_start{
        std::chrono::seconds{1'700'000'000}}; // 2023-11-14T22:13:20Z

    FakeClock() = default;
    explicit FakeClock(std::chrono::system_clock::time_point wall_start) noexcept
        : wall_start_(wall_start) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return steady_start + elapsed();
    }

    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const noexcept override {
        using WallDuration = std::chrono::system_clock::duration;
        const auto by = std::chrono::duration_cast<WallDuration>(elapsed()); // in [0, max_elapsed]
        if (wall_start_.time_since_epoch() > WallDuration::max() - by) {
            return std::chrono::system_clock::time_point::max();
        }
        return wall_start_ + by;
    }

    [[nodiscard]] bool sleep_until(std::chrono::steady_clock::time_point deadline,
                                   std::stop_token stop) override {
        std::function<void()> hook;
        {
            const std::scoped_lock lock{mutex_};
            hook = on_sleep_;
        }
        if (hook) {
            hook();
        }
        const auto current = now();
        const SleepRecord record{
            .deadline = deadline,
            .requested = deadline > current
                             ? std::chrono::ceil<std::chrono::milliseconds>(deadline - current)
                             : std::chrono::milliseconds{0},
            .stopped = stop.stop_requested(),
        };
        // A deadline at or before now() never moves time (and deadline - steady_start cannot
        // overflow once deadline > now() >= steady_start).
        if (!record.stopped && deadline > current) {
            advance_to(deadline - steady_start);
        }
        {
            const std::scoped_lock lock{mutex_};
            sleeps_.push_back(record);
        }
        return !record.stopped;
    }

    /// Moves virtual time forward by `by` (ignored when negative; stops at max_elapsed).
    void advance(Duration by) noexcept {
        if (by <= Duration::zero()) {
            return;
        }
        Rep current = elapsed_.load(std::memory_order_relaxed);
        while (true) {
            const Rep room = max_elapsed.count() - current;
            const Rep next = by.count() >= room ? max_elapsed.count() : current + by.count();
            if (elapsed_.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    /// Runs on every sleep_until (see the file comment). Replaces any previous hook.
    void set_on_sleep(std::function<void()> hook) {
        const std::scoped_lock lock{mutex_};
        on_sleep_ = std::move(hook);
    }

    /// Every sleep_until call so far, in completion order.
    [[nodiscard]] std::vector<SleepRecord> sleeps() const {
        const std::scoped_lock lock{mutex_};
        return sleeps_;
    }

    /// The sum of `requested` over the sleeps that were not stopped.
    [[nodiscard]] std::chrono::milliseconds total_slept() const {
        const std::scoped_lock lock{mutex_};
        std::chrono::milliseconds total{0};
        for (const SleepRecord& sleep : sleeps_) {
            if (!sleep.stopped) {
                total += sleep.requested;
            }
        }
        return total;
    }

private:
    using Rep = Duration::rep;

    [[nodiscard]] Duration elapsed() const noexcept {
        return Duration{elapsed_.load(std::memory_order_relaxed)};
    }

    /// elapsed = max(elapsed, target): concurrent sleepers never move time backwards.
    void advance_to(Duration target) noexcept {
        const Rep wanted = std::min(target.count(), max_elapsed.count());
        Rep current = elapsed_.load(std::memory_order_relaxed);
        while (current < wanted) {
            if (elapsed_.compare_exchange_weak(current, wanted, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    std::chrono::system_clock::time_point wall_start_{default_wall_start};
    std::atomic<Rep> elapsed_{0};
    mutable std::mutex mutex_{};
    std::vector<SleepRecord> sleeps_{}; ///< guarded by mutex_
    std::function<void()> on_sleep_{};  ///< guarded by mutex_
};

} // namespace mangadex_dl::test
