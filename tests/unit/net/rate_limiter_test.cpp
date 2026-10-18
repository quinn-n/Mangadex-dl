// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/rate_limiter_test.cpp: RateLimiter spacing (fake clock, and concurrently from 4
// threads, also under TSan), stop tokens, and the two real clocks (system_clock and the instant
// clock).

#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "support/fake_clock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

using mangadex_dl::Clock;
using mangadex_dl::RateLimiter;
using mangadex_dl::test::FakeClock;
using mangadex_dl::test::SleepRecord;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

constexpr milliseconds interval{250};

} // namespace

TEST_CASE("RateLimiter: the first acquisition is immediate, later ones are spaced",
          "[net][rate-limiter]") {
    FakeClock clock;
    RateLimiter limiter{clock, interval};
    const steady_clock::time_point start = clock.now();

    REQUIRE(limiter.acquire({}));
    CHECK(clock.sleeps().empty());
    CHECK(clock.now() == start);

    for (int i = 1; i <= 4; ++i) {
        REQUIRE(limiter.acquire({}));
        CHECK(clock.now() == start + i * interval);
    }
    const std::vector<SleepRecord> sleeps = clock.sleeps();
    REQUIRE(sleeps.size() == 4);
    for (std::size_t i = 0; i < sleeps.size(); ++i) {
        CAPTURE(i);
        CHECK(sleeps[i].deadline == start + static_cast<int>(i + 1) * interval);
        CHECK(sleeps[i].requested == interval);
        CHECK_FALSE(sleeps[i].stopped);
    }
}

TEST_CASE("RateLimiter: time that already passed counts towards the interval",
          "[net][rate-limiter]") {
    FakeClock clock;
    RateLimiter limiter{clock, interval};
    REQUIRE(limiter.acquire({}));

    clock.advance(milliseconds{100});
    REQUIRE(limiter.acquire({}));
    REQUIRE(clock.sleeps().size() == 1);
    CHECK(clock.sleeps().front().requested == milliseconds{150});

    clock.advance(milliseconds{1000}); // idle for longer than the interval: no wait at all
    REQUIRE(limiter.acquire({}));
    CHECK(clock.sleeps().size() == 1);
}

TEST_CASE("RateLimiter: a huge interval is clamped, never overflows", "[net][rate-limiter]") {
    FakeClock clock;
    RateLimiter limiter{clock, milliseconds::max()};
    const steady_clock::time_point start = clock.now();
    REQUIRE(limiter.acquire({}));
    REQUIRE(limiter.acquire({})); // waits the clamped interval (366 days), in virtual time
    REQUIRE(limiter.acquire({}));
    const milliseconds max_delay{std::chrono::days{366}};
    const std::vector<SleepRecord> sleeps = clock.sleeps();
    REQUIRE(sleeps.size() == 2);
    CHECK(sleeps[0].deadline == start + max_delay);
    CHECK(sleeps[1].deadline == start + (2 * max_delay));
    CHECK(clock.now() == start + (2 * max_delay));
}

TEST_CASE("RateLimiter: a zero or negative interval never sleeps", "[net][rate-limiter]") {
    const milliseconds none = GENERATE(milliseconds{0}, milliseconds{-5});
    FakeClock clock;
    RateLimiter limiter{clock, none};
    for (int i = 0; i < 10; ++i) {
        REQUIRE(limiter.acquire({}));
    }
    CHECK(clock.sleeps().empty());
}

TEST_CASE("RateLimiter: a stop requested before acquire returns false and reserves nothing",
          "[net][rate-limiter]") {
    FakeClock clock;
    RateLimiter limiter{clock, interval};
    const std::stop_source stopped;
    stopped.request_stop();

    CHECK_FALSE(limiter.acquire(stopped.get_token()));
    CHECK_FALSE(limiter.acquire(stopped.get_token()));
    CHECK(clock.sleeps().empty());
    // No slot was taken, so a live token still gets the immediate first slot.
    REQUIRE(limiter.acquire({}));
    CHECK(clock.sleeps().empty());
}

TEST_CASE("RateLimiter: a stop during the wait returns false", "[net][rate-limiter]") {
    std::stop_source source; // declared before `clock`, so it outlives the hook stored there
    FakeClock clock;
    RateLimiter limiter{clock, interval};
    REQUIRE(limiter.acquire({}));

    clock.set_on_sleep([src = &source] { src->request_stop(); });
    CHECK_FALSE(limiter.acquire(source.get_token()));
    const std::vector<SleepRecord> sleeps = clock.sleeps();
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps.front().stopped);
}

TEST_CASE("RateLimiter: 4 threads keep the spacing (fake clock)", "[net][rate-limiter]") {
    constexpr int threads = 4;
    constexpr int per_thread = 25;
    FakeClock clock;
    RateLimiter limiter{clock, interval};
    const steady_clock::time_point start = clock.now();
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            // `limiter` outlives the jthreads: they are joined at the end of this block.
            workers.emplace_back([lim = &limiter] {
                for (int i = 0; i < per_thread; ++i) {
                    static_cast<void>(lim->acquire({}));
                }
            });
        }
    }
    // Every acquisition but the first sleeps until its own slot: start + k * interval, each k
    // exactly once.
    std::vector<steady_clock::time_point> deadlines;
    for (const SleepRecord& sleep : clock.sleeps()) {
        CHECK_FALSE(sleep.stopped);
        deadlines.push_back(sleep.deadline);
    }
    std::ranges::sort(deadlines);
    REQUIRE(deadlines.size() == static_cast<std::size_t>((threads * per_thread) - 1));
    for (std::size_t k = 0; k < deadlines.size(); ++k) {
        CAPTURE(k);
        CHECK(deadlines[k] == start + static_cast<int>(k + 1) * interval);
    }
    CHECK(clock.now() == deadlines.back());
}

TEST_CASE("RateLimiter: 4 threads keep the spacing (system clock)", "[net][rate-limiter]") {
    constexpr int threads = 4;
    constexpr int per_thread = 5;
    constexpr milliseconds small{2};
    RateLimiter limiter{mangadex_dl::system_clock(), small};
    std::mutex mutex;
    std::vector<steady_clock::time_point> granted; // guarded by mutex
    const steady_clock::time_point start = steady_clock::now();
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            // Everything borrowed outlives the jthreads: they are joined at the end of this block.
            workers.emplace_back([lim = &limiter, m = &mutex, out = &granted] {
                for (int i = 0; i < per_thread; ++i) {
                    if (lim->acquire({})) {
                        const std::scoped_lock lock{*m};
                        out->push_back(steady_clock::now());
                    }
                }
            });
        }
    }
    REQUIRE(granted.size() == static_cast<std::size_t>(threads * per_thread));
    // Return times are not ordered by slot (threads wake late), but the last slot is at least
    // (n - 1) intervals after the first, so the run cannot finish sooner.
    const steady_clock::time_point last = std::ranges::max(granted);
    CHECK(last - start >= ((threads * per_thread) - 1) * small);
}

TEST_CASE("system_clock: one instance, real time", "[net][clock]") {
    Clock& clock = mangadex_dl::system_clock();
    CHECK(&clock == &mangadex_dl::system_clock());

    const steady_clock::time_point before = steady_clock::now();
    const steady_clock::time_point now = clock.now();
    CHECK(now >= before);
    CHECK(now <= steady_clock::now());

    const auto wall = clock.wall_now();
    const auto real = std::chrono::system_clock::now();
    CHECK(real - wall < std::chrono::minutes{1});
    CHECK(wall - real < std::chrono::minutes{1});
}

TEST_CASE("system_clock: sleep_until waits for a near deadline", "[net][clock]") {
    Clock& clock = mangadex_dl::system_clock();
    const steady_clock::time_point deadline = steady_clock::now() + milliseconds{20};
    CHECK(clock.sleep_until(deadline, {}));
    CHECK(steady_clock::now() >= deadline);
    // A deadline in the past returns at once, still true.
    CHECK(clock.sleep_until(steady_clock::now() - milliseconds{5}, {}));
}

TEST_CASE("system_clock: a stop already requested returns false at once", "[net][clock]") {
    const std::stop_source source;
    source.request_stop();
    const steady_clock::time_point before = steady_clock::now();
    CHECK_FALSE(mangadex_dl::system_clock().sleep_until(before + std::chrono::hours{1},
                                                        source.get_token()));
    CHECK(steady_clock::now() - before < std::chrono::seconds{10});
}

TEST_CASE("system_clock: a stop from another thread interrupts a long sleep", "[net][clock]") {
    const std::stop_source source;
    bool slept = true;
    const steady_clock::time_point before = steady_clock::now();
    {
        // `slept` and the token outlive the jthread: it is joined at the end of this block.
        const std::jthread sleeper{[out = &slept, token = source.get_token()] {
            *out = mangadex_dl::system_clock().sleep_until(
                steady_clock::now() + std::chrono::minutes{10}, token);
        }};
        std::this_thread::sleep_for(milliseconds{20});
        source.request_stop();
    }
    CHECK_FALSE(slept);
    CHECK(steady_clock::now() - before < std::chrono::seconds{30});
}

TEST_CASE("RateLimiter: a stop interrupts a real wait", "[net][rate-limiter]") {
    RateLimiter limiter{mangadex_dl::system_clock(), std::chrono::minutes{10}};
    REQUIRE(limiter.acquire({}));
    const std::stop_source source;
    bool granted = true;
    const steady_clock::time_point before = steady_clock::now();
    {
        // `limiter` and `granted` outlive the jthread: it is joined at the end of this block.
        const std::jthread waiter{[lim = &limiter, out = &granted, token = source.get_token()] {
            *out = lim->acquire(token);
        }};
        std::this_thread::sleep_for(milliseconds{20});
        source.request_stop();
    }
    CHECK_FALSE(granted);
    CHECK(steady_clock::now() - before < std::chrono::seconds{30});
}

TEST_CASE("make_instant_clock: sleeping advances virtual time at once", "[net][clock]") {
    const std::unique_ptr<Clock> clock = mangadex_dl::make_instant_clock();
    REQUIRE(clock != nullptr);
    const steady_clock::time_point start = clock->now();
    const auto wall_start = clock->wall_now();

    const steady_clock::time_point real_before = steady_clock::now();
    CHECK(clock->sleep_until(start + std::chrono::hours{2}, {}));
    CHECK(steady_clock::now() - real_before < std::chrono::seconds{10}); // did not really sleep
    CHECK(clock->now() == start + std::chrono::hours{2});
    CHECK(clock->wall_now() == wall_start + std::chrono::hours{2});

    // An earlier deadline never moves time backwards.
    CHECK(clock->sleep_until(start + std::chrono::hours{1}, {}));
    CHECK(clock->now() == start + std::chrono::hours{2});
    CHECK(clock->sleep_until(start - std::chrono::hours{1}, {}));
    CHECK(clock->now() == start + std::chrono::hours{2});

    // A requested stop returns false and leaves time alone.
    const std::stop_source source;
    source.request_stop();
    CHECK_FALSE(clock->sleep_until(start + std::chrono::hours{5}, source.get_token()));
    CHECK(clock->now() == start + std::chrono::hours{2});
}

TEST_CASE("make_instant_clock: extreme deadlines never overflow", "[net][clock]") {
    const std::unique_ptr<Clock> clock = mangadex_dl::make_instant_clock();
    const steady_clock::time_point start = clock->now();
    const auto wall_start = clock->wall_now();
    CHECK(clock->sleep_until(steady_clock::time_point::min(), {}));
    CHECK(clock->now() == start);
    CHECK(clock->sleep_until(steady_clock::time_point::max(), {}));
    // Virtual time stops at 100 years; both time lines moved by the same amount.
    const auto moved = clock->now() - start;
    CHECK(moved == std::chrono::duration_cast<steady_clock::duration>(std::chrono::years{100}));
    CHECK(clock->wall_now() - wall_start == moved);
}

TEST_CASE("FakeClock: extreme deadlines and advances never overflow", "[net][clock]") {
    FakeClock clock;
    CHECK(clock.sleep_until(steady_clock::time_point::min(), {}));
    CHECK(clock.now() == FakeClock::steady_start);
    CHECK(clock.sleep_until(steady_clock::time_point::max(), {}));
    CHECK(clock.now() == FakeClock::steady_start + FakeClock::max_elapsed);
    clock.advance(FakeClock::Duration::max());
    CHECK(clock.now() == FakeClock::steady_start + FakeClock::max_elapsed);
    CHECK(clock.wall_now() > FakeClock::default_wall_start);

    FakeClock far{std::chrono::system_clock::time_point::max() - std::chrono::hours{1}};
    far.advance(std::chrono::hours{2});
    CHECK(far.wall_now() == std::chrono::system_clock::time_point::max()); // saturated
}

TEST_CASE("make_instant_clock: concurrent sleepers end at the latest deadline", "[net][clock]") {
    const std::unique_ptr<Clock> clock = mangadex_dl::make_instant_clock();
    const steady_clock::time_point start = clock->now();
    {
        std::vector<std::jthread> workers;
        workers.reserve(4);
        for (int t = 0; t < 4; ++t) {
            // `clock` outlives the jthreads: they are joined at the end of this block.
            workers.emplace_back([c = clock.get(), start, t] {
                for (int i = 1; i <= 50; ++i) {
                    static_cast<void>(c->sleep_until(start + milliseconds{(i * 4) + t}, {}));
                    static_cast<void>(c->now());
                }
            });
        }
    }
    CHECK(clock->now() == start + milliseconds{(50 * 4) + 3});
}

TEST_CASE("RateLimiter over the instant clock: spaced in virtual time", "[net][rate-limiter]") {
    const std::unique_ptr<Clock> clock = mangadex_dl::make_instant_clock();
    RateLimiter limiter{*clock, milliseconds{1500}};
    const steady_clock::time_point start = clock->now();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(limiter.acquire({}));
    }
    CHECK(clock->now() == start + milliseconds{3000});
}
