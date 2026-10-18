// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/fetch_pool/fetch_pool_test.cpp: FetchPool (download plan T8). In-order delivery
// under random completion order, the window bound, error and stop propagation with draining,
// exceptions on both sides, and identical output for any worker count. These also run under the
// tsan preset (`pixi run tsan -R 'unit\.fetch_pool'`).
//
// Fetchers run on worker threads, so they never use Catch2 assertions (not thread-safe); they only
// record into atomics, and the test thread asserts afterwards. Every lambda borrows test locals
// through explicit pointers: run() never returns while a worker still uses its fetcher.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fetch_pool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): pthread_sigmask is POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// Whether SIGINT, SIGTERM and SIGPIPE are all blocked in the calling thread. No Catch2 macros:
/// it also runs on pool and poller threads.
[[nodiscard]] bool async_signals_blocked() noexcept {
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t mask{};
    if (pthread_sigmask(SIG_BLOCK, nullptr, &mask) != 0) {
        return false;
    }
    return sigismember(&mask, SIGINT) == 1 && sigismember(&mask, SIGTERM) == 1 &&
           sigismember(&mask, SIGPIPE) == 1;
}

/// The calling thread's whole signal mask, as sigismember results for signals 1..64.
[[nodiscard]] std::array<int, 64> signal_mask_snapshot() {
    sigset_t mask{};
    REQUIRE(pthread_sigmask(SIG_BLOCK, nullptr, &mask) == 0);
    std::array<int, 64> members{};
    int signal = 1;
    for (int& member : members) {
        member = sigismember(&mask, signal);
        ++signal;
    }
    return members;
}

/// A deterministic but scrambled 0-4 ms delay, so pages complete out of order.
[[nodiscard]] milliseconds scrambled_delay(std::size_t index) noexcept {
    return milliseconds{static_cast<long>(((index * 7919U) + 13U) % 5U)};
}

[[nodiscard]] md::FetchedPage page_for(std::size_t index) {
    return md::FetchedPage{.bytes = "page-" + std::to_string(index), .info = {}};
}

/// Raises `peak` to at least `value`.
void raise_peak(std::atomic<int>& peak, int value) noexcept {
    int seen = peak.load();
    while (value > seen && !peak.compare_exchange_weak(seen, value)) {
    }
}

/// Blocks until `stop` is requested (or 10 s pass, so a bug fails instead of hanging).
void wait_for_stop(const std::stop_token& stop) {
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds{10};
    while (!stop.stop_requested() && Clock::now() < give_up) {
        std::this_thread::sleep_for(milliseconds{1});
    }
}

/// Blocks on a condition variable that only a std::stop_callback on `stop` wakes, the way a
/// transport cancels a blocking transfer. True when the callback woke it (false after 10 s).
[[nodiscard]] bool wait_woken_by_stop_callback(const std::stop_token& stop) {
    std::mutex mutex;
    std::condition_variable wake;
    bool woken = false;
    // Declared after the state it borrows, so ~stop_callback (which waits for a callback running
    // on another thread) completes before mutex, wake and woken are destroyed.
    const std::stop_callback on_stop{stop,
                                     [mutex_ptr = &mutex, wake_ptr = &wake, woken_ptr = &woken] {
                                         {
                                             const std::scoped_lock lock{*mutex_ptr};
                                             *woken_ptr = true;
                                         }
                                         wake_ptr->notify_all();
                                     }};
    std::unique_lock lock{mutex};
    return wake.wait_for(lock, std::chrono::seconds{10}, [&woken] { return woken; });
}

/// Polls `value` until it reaches `at_least` (or 10 s pass).
void wait_until_at_least(const std::atomic<int>& value, int at_least) {
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds{10};
    while (value.load() < at_least && Clock::now() < give_up) {
        std::this_thread::sleep_for(milliseconds{1});
    }
}

/// Runs `count` pages through `pool` with scrambled delays; the consumed byte strings in order.
[[nodiscard]] std::vector<std::string> collect(md::FetchPool& pool, std::size_t count) {
    std::vector<std::string> seen{};
    const md::PageFetcher fetch = [](std::size_t index, const std::stop_token&) {
        std::this_thread::sleep_for(scrambled_delay(index));
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [out = &seen](std::size_t index, md::FetchedPage page) {
        if (index != out->size()) {
            return md::Result<void>{std::unexpected(md::Error{.message = "out of order"})};
        }
        out->push_back(std::move(page.bytes));
        return md::Result<void>{};
    };
    const auto result = pool.run(count, fetch, consume, std::stop_token{});
    REQUIRE(result.has_value());
    return seen;
}

} // namespace

TEST_CASE("fetch_pool delivers every page in index order", "[fetch_pool]") {
    const unsigned workers = GENERATE(1U, 2U, 4U, 8U);
    CAPTURE(workers);
    md::FetchPool pool{workers};
    const std::vector<std::string> seen = collect(pool, 60);
    REQUIRE(seen.size() == 60);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i] == "page-" + std::to_string(i));
    }
}

TEST_CASE("fetch_pool output is identical for one and eight workers", "[fetch_pool]") {
    md::FetchPool one{1};
    md::FetchPool eight{8};
    CHECK(collect(one, 40) == collect(eight, 40));
}

TEST_CASE("fetch_pool never holds more than workers pages", "[fetch_pool]") {
    const unsigned workers = GENERATE(1U, 3U, 8U);
    CAPTURE(workers);
    md::FetchPool pool{workers};
    std::atomic<int> held{0}; // fetch started, consume not yet finished
    std::atomic<int> peak{0};
    std::atomic<int> running{0}; // fetches running right now
    std::atomic<int> peak_running{0};

    const md::PageFetcher fetch = [held_ptr = &held, peak_ptr = &peak, running_ptr = &running,
                                   peak_running_ptr = &peak_running](std::size_t index,
                                                                     const std::stop_token&) {
        raise_peak(*peak_ptr, held_ptr->fetch_add(1) + 1);
        raise_peak(*peak_running_ptr, running_ptr->fetch_add(1) + 1);
        std::this_thread::sleep_for(scrambled_delay(index));
        running_ptr->fetch_sub(1);
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    // A slow consumer lets the fetchers run ahead as far as the window allows.
    const md::PageConsumer consume = [held_ptr = &held](std::size_t, const md::FetchedPage&) {
        std::this_thread::sleep_for(milliseconds{1});
        held_ptr->fetch_sub(1);
        return md::Result<void>{};
    };
    REQUIRE(pool.run(80, fetch, consume, std::stop_token{}).has_value());
    CHECK(held.load() == 0);
    CHECK(peak.load() <= static_cast<int>(workers));
    CHECK(peak_running.load() <= static_cast<int>(workers));
}

TEST_CASE("fetch_pool clamps the worker count into 1..8", "[fetch_pool]") {
    const unsigned requested = GENERATE(0U, 9U, 1000U);
    CAPTURE(requested);
    md::FetchPool pool{requested};
    std::atomic<int> running{0};
    std::atomic<int> peak_running{0};
    const md::PageFetcher fetch = [running_ptr = &running, peak_running_ptr = &peak_running](
                                      std::size_t index, const std::stop_token&) {
        raise_peak(*peak_running_ptr, running_ptr->fetch_add(1) + 1);
        std::this_thread::sleep_for(milliseconds{2});
        running_ptr->fetch_sub(1);
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [](std::size_t, const md::FetchedPage&) {
        return md::Result<void>{};
    };
    REQUIRE(pool.run(40, fetch, consume, std::stop_token{}).has_value());
    CHECK(peak_running.load() >= 1);
    CHECK(peak_running.load() <= 8);
}

TEST_CASE("fetch_pool returns the first fetch error and starts no more work", "[fetch_pool]") {
    md::FetchPool pool{4};
    std::atomic<int> calls{0};
    std::vector<std::size_t> consumed{};
    const md::PageFetcher fetch = [calls_ptr = &calls](std::size_t index, const std::stop_token&) {
        calls_ptr->fetch_add(1);
        std::this_thread::sleep_for(scrambled_delay(index));
        if (index == 10) {
            return md::Result<md::FetchedPage>{
                std::unexpected(md::Error{.code = md::Errc::network, .message = "boom 10"})};
        }
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [out = &consumed](std::size_t index, const md::FetchedPage&) {
        out->push_back(index);
        return md::Result<void>{};
    };
    const auto result = pool.run(100, fetch, consume, std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == md::Error{.code = md::Errc::network, .message = "boom 10"});
    CHECK(consumed.size() == 10);
    // Indices are handed out in order, only while index < consumed + workers (consumed <= 10).
    CHECK(calls.load() <= 10 + 4);
}

TEST_CASE("fetch_pool reports the lowest failing index whatever finishes first", "[fetch_pool]") {
    md::FetchPool pool{4};
    const md::PageFetcher fetch = [](std::size_t index, const std::stop_token&) {
        if (index == 1) {
            std::this_thread::sleep_for(milliseconds{30}); // fails last
            return md::Result<md::FetchedPage>{
                std::unexpected(md::Error{.code = md::Errc::network, .message = "slow 1"})};
        }
        if (index == 2) {
            return md::Result<md::FetchedPage>{
                std::unexpected(md::Error{.code = md::Errc::failure, .message = "fast 2"})};
        }
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [](std::size_t, const md::FetchedPage&) {
        return md::Result<void>{};
    };
    const auto result = pool.run(20, fetch, consume, std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "slow 1");
}

TEST_CASE("fetch_pool stops on a consumer error and cancels in-flight fetches", "[fetch_pool]") {
    md::FetchPool pool{4};
    std::atomic<int> active{0};
    std::vector<std::size_t> consumed{};
    const md::PageFetcher fetch = [active_ptr = &active](std::size_t index,
                                                         const std::stop_token& stop) {
        active_ptr->fetch_add(1);
        if (index >= 3) {
            wait_for_stop(stop); // only the end of the run releases these
        }
        active_ptr->fetch_sub(1);
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [out = &consumed](std::size_t index, const md::FetchedPage&) {
        out->push_back(index);
        if (index == 2) {
            return md::Result<void>{
                std::unexpected(md::Error{.code = md::Errc::io, .message = "disk full"})};
        }
        return md::Result<void>{};
    };
    const Clock::time_point start = Clock::now();
    const auto result = pool.run(50, fetch, consume, std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == md::Error{.code = md::Errc::io, .message = "disk full"});
    CHECK(consumed == std::vector<std::size_t>{0, 1, 2});
    CHECK(active.load() == 0); // drained before returning
    CHECK(Clock::now() - start < std::chrono::seconds{5});
}

TEST_CASE("fetch_pool a stop request cancels promptly with Errc::interrupted", "[fetch_pool]") {
    md::FetchPool pool{4};
    std::atomic<int> active{0};
    std::atomic<int> consumed{0};
    const md::PageFetcher fetch = [active_ptr = &active](std::size_t index,
                                                         const std::stop_token& stop) {
        active_ptr->fetch_add(1);
        if (index >= 2) {
            wait_for_stop(stop);
        }
        active_ptr->fetch_sub(1);
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [consumed_ptr = &consumed](std::size_t,
                                                                const md::FetchedPage&) {
        consumed_ptr->fetch_add(1);
        return md::Result<void>{};
    };

    SECTION("requested while running") {
        const std::stop_source source{};
        // Stops the run once pages 0 and 1 are consumed and the fetch of page 2 is blocked.
        const std::jthread stopper{[source, consumed_ptr = &consumed](const std::stop_token&) {
            const Clock::time_point give_up = Clock::now() + std::chrono::seconds{10};
            while (consumed_ptr->load() < 2 && Clock::now() < give_up) {
                std::this_thread::sleep_for(milliseconds{1});
            }
            std::this_thread::sleep_for(milliseconds{20});
            static_cast<void>(source.request_stop());
        }};
        const Clock::time_point start = Clock::now();
        const auto result = pool.run(100, fetch, consume, source.get_token());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::interrupted);
        CHECK(Clock::now() - start < std::chrono::seconds{5});
        CHECK(consumed.load() == 2);
        CHECK(active.load() == 0);
    }
    SECTION("requested before the run") {
        const std::stop_source source{};
        static_cast<void>(source.request_stop());
        const auto result = pool.run(100, fetch, consume, source.get_token());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::interrupted);
        CHECK(consumed.load() == 0);
        CHECK(active.load() == 0);
    }
}

TEST_CASE("fetch_pool count zero succeeds without calling anything", "[fetch_pool]") {
    md::FetchPool pool{2};
    std::atomic<int> calls{0};
    const md::PageFetcher fetch = [calls_ptr = &calls](std::size_t index, const std::stop_token&) {
        calls_ptr->fetch_add(1);
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [calls_ptr = &calls](std::size_t, const md::FetchedPage&) {
        calls_ptr->fetch_add(1);
        return md::Result<void>{};
    };
    CHECK(pool.run(0, fetch, consume, std::stop_token{}).has_value());
    CHECK(pool.run(0, md::PageFetcher{}, md::PageConsumer{}, std::stop_token{}).has_value());
    CHECK(calls.load() == 0);
}

TEST_CASE("fetch_pool rejects an empty function before any work", "[fetch_pool]") {
    md::FetchPool pool{2};
    std::atomic<int> calls{0};
    const md::PageFetcher fetch = [calls_ptr = &calls](std::size_t index, const std::stop_token&) {
        calls_ptr->fetch_add(1);
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [](std::size_t, const md::FetchedPage&) {
        return md::Result<void>{};
    };
    const auto no_fetch = pool.run(3, md::PageFetcher{}, consume, std::stop_token{});
    REQUIRE_FALSE(no_fetch.has_value());
    CHECK(no_fetch.error().code == md::Errc::failure);
    const auto no_consume = pool.run(3, fetch, md::PageConsumer{}, std::stop_token{});
    REQUIRE_FALSE(no_consume.has_value());
    CHECK(no_consume.error().code == md::Errc::failure);
    CHECK(calls.load() == 0);
}

TEST_CASE("fetch_pool turns exceptions from either side into Errc::failure", "[fetch_pool]") {
    md::FetchPool pool{3};
    std::atomic<int> active{0};
    const md::PageConsumer accept = [](std::size_t, const md::FetchedPage&) {
        return md::Result<void>{};
    };

    SECTION("a std::exception from a fetch") {
        const md::PageFetcher fetch = [](std::size_t index,
                                         const std::stop_token&) -> md::Result<md::FetchedPage> {
            if (index == 3) {
                throw std::runtime_error{"kaput"};
            }
            return page_for(index);
        };
        const auto result = pool.run(10, fetch, accept, std::stop_token{});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::failure);
        CHECK(result.error().message.contains("kaput"));
    }
    SECTION("a non-std exception from a fetch") {
        const md::PageFetcher fetch = [](std::size_t index,
                                         const std::stop_token&) -> md::Result<md::FetchedPage> {
            if (index == 0) {
                // NOLINTNEXTLINE(bugprone-std-exception-baseclass): tests the catch (...) path
                throw 42;
            }
            return page_for(index);
        };
        const auto result = pool.run(10, fetch, accept, std::stop_token{});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::failure);
        CHECK(result.error().message.contains("unknown exception"));
    }
    SECTION("an exception from the consumer, after the workers drained") {
        const md::PageFetcher fetch = [active_ptr = &active](std::size_t index,
                                                             const std::stop_token& stop) {
            active_ptr->fetch_add(1);
            if (index >= 1) {
                wait_for_stop(stop);
            }
            active_ptr->fetch_sub(1);
            return md::Result<md::FetchedPage>{page_for(index)};
        };
        const md::PageConsumer consume = [](std::size_t,
                                            const md::FetchedPage&) -> md::Result<void> {
            throw std::logic_error{"consumer broke"};
        };
        const auto result = pool.run(10, fetch, consume, std::stop_token{});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::failure);
        CHECK(result.error().message.contains("consumer broke"));
        CHECK(active.load() == 0);
    }
}

TEST_CASE("fetch_pool can run again after success and after failure", "[fetch_pool]") {
    md::FetchPool pool{3};
    CHECK(collect(pool, 10).size() == 10);
    const md::PageFetcher failing = [](std::size_t, const std::stop_token&) {
        return md::Result<md::FetchedPage>{std::unexpected(md::Error{.message = "nope"})};
    };
    const md::PageConsumer consume = [](std::size_t, const md::FetchedPage&) {
        return md::Result<void>{};
    };
    CHECK_FALSE(pool.run(5, failing, consume, std::stop_token{}).has_value());
    CHECK(collect(pool, 25).size() == 25);
}

TEST_CASE("fetch_pool refuses a nested run and keeps the outer one intact", "[fetch_pool]") {
    md::FetchPool pool{2};
    std::vector<std::string> seen{};
    std::vector<md::Error> nested_errors{};
    const md::PageFetcher fetch = [](std::size_t index, const std::stop_token&) {
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [pool_ptr = &pool, fetch_ptr = &fetch, out = &seen,
                                      errors = &nested_errors](std::size_t index,
                                                               md::FetchedPage page) {
        if (index == 1) {
            const md::PageConsumer ignore = [](std::size_t, const md::FetchedPage&) {
                return md::Result<void>{};
            };
            const auto nested = pool_ptr->run(3, *fetch_ptr, ignore, std::stop_token{});
            if (!nested.has_value()) {
                errors->push_back(nested.error());
            }
        }
        out->push_back(std::move(page.bytes));
        return md::Result<void>{};
    };
    REQUIRE(pool.run(5, fetch, consume, std::stop_token{}).has_value());
    REQUIRE(nested_errors.size() == 1);
    CHECK(nested_errors.front().code == md::Errc::failure);
    CHECK(nested_errors.front().message.contains("not reentrant"));
    CHECK(seen == std::vector<std::string>{"page-0", "page-1", "page-2", "page-3", "page-4"});
}

TEST_CASE("fetch_pool wakes fetchers blocked behind a stop_callback when a run ends",
          "[fetch_pool]") {
    md::FetchPool pool{4};
    std::atomic<int> blocked{0}; // fetches waiting for their stop_callback
    std::atomic<int> woken{0};   // fetches whose stop_callback woke them
    std::atomic<int> consumed{0};
    const md::PageFetcher fetch = [blocked_ptr = &blocked, woken_ptr = &woken](
                                      std::size_t index, const std::stop_token& stop) {
        if (index >= 2) {
            blocked_ptr->fetch_add(1);
            if (wait_woken_by_stop_callback(stop)) {
                woken_ptr->fetch_add(1);
            }
        }
        return md::Result<md::FetchedPage>{page_for(index)};
    };

    SECTION("the consumer fails: the drain stops the run token") {
        const md::PageConsumer consume = [blocked_ptr = &blocked, consumed_ptr = &consumed](
                                             std::size_t index, const md::FetchedPage&) {
            consumed_ptr->fetch_add(1);
            if (index == 1) {
                wait_until_at_least(*blocked_ptr, 1); // make sure a fetch is really blocked
                return md::Result<void>{
                    std::unexpected(md::Error{.code = md::Errc::io, .message = "disk full"})};
            }
            return md::Result<void>{};
        };
        const auto result = pool.run(50, fetch, consume, std::stop_token{});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::io);
    }
    SECTION("the caller requests stop: the forwarded stop reaches the fetchers") {
        const md::PageConsumer consume = [consumed_ptr = &consumed](std::size_t,
                                                                    const md::FetchedPage&) {
            consumed_ptr->fetch_add(1);
            return md::Result<void>{};
        };
        const std::stop_source source{};
        const std::jthread stopper{
            [source, blocked_ptr = &blocked, consumed_ptr = &consumed](const std::stop_token&) {
                wait_until_at_least(*consumed_ptr, 2);
                wait_until_at_least(*blocked_ptr, 1);
                static_cast<void>(source.request_stop());
            }};
        const auto result = pool.run(50, fetch, consume, source.get_token());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == md::Errc::interrupted);
    }
    CHECK(blocked.load() >= 1);
    CHECK(woken.load() == blocked.load()); // every blocked fetch was woken, none timed out
    CHECK(consumed.load() == 2);
}

TEST_CASE("fetch_pool workers start with SIGINT SIGTERM and SIGPIPE blocked", "[fetch_pool]") {
    REQUIRE_FALSE(async_signals_blocked()); // otherwise inheritance alone would pass the test
    const std::array<int, 64> before = signal_mask_snapshot();
    md::FetchPool pool{4};
    CHECK(signal_mask_snapshot() == before); // the creating thread's mask is restored exactly

    std::atomic<int> masked{0};
    const md::PageFetcher fetch = [masked_ptr = &masked](std::size_t index,
                                                         const std::stop_token&) {
        if (async_signals_blocked()) {
            masked_ptr->fetch_add(1);
        }
        std::this_thread::sleep_for(scrambled_delay(index));
        return md::Result<md::FetchedPage>{page_for(index)};
    };
    const md::PageConsumer consume = [](std::size_t, const md::FetchedPage&) {
        return md::Result<void>{};
    };
    REQUIRE(pool.run(20, fetch, consume, std::stop_token{}).has_value());
    CHECK(masked.load() == 20);
    CHECK(signal_mask_snapshot() == before);
}
