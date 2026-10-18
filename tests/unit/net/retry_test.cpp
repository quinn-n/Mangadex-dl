// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/retry_test.cpp: decide_retry (acceptance 8: status x error x attempt, jitter
// bound, the 429 policy and its cap) and parse_retry_after (epoch-primary, delta fallback).

#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "support/fake_clock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using mangadex_dl::AttemptOutcome;
using mangadex_dl::decide_retry;
using mangadex_dl::parse_retry_after;
using mangadex_dl::RetryConfig;
using mangadex_dl::RetryDecision;
using mangadex_dl::TransportErrc;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr RetryConfig defaults{};
constexpr RetryDecision no_retry{};

// Every delay decide_retry/parse_retry_after produce is clamped to this (retry.cpp max_delay), so
// callers can add it to a nanosecond time_point without overflowing.
constexpr milliseconds max_delay{std::chrono::days{366}};

[[nodiscard]] AttemptOutcome status(int code, int attempt = 1,
                                    std::optional<milliseconds> retry_after = std::nullopt) {
    return AttemptOutcome{.attempt = attempt, .status = code, .retry_after = retry_after};
}

[[nodiscard]] AttemptOutcome error(TransportErrc kind, int attempt = 1) {
    return AttemptOutcome{.attempt = attempt, .error = kind};
}

[[nodiscard]] RetryDecision retry_after_ms(milliseconds::rep ms) {
    return RetryDecision{.retry = true, .delay = milliseconds{ms}};
}

// The fake clock's wall time: 1'700'000'000 s since the epoch.
const std::chrono::system_clock::time_point now = mangadex_dl::test::FakeClock::default_wall_start;

} // namespace

TEST_CASE("decide_retry: the default config is the documented policy", "[net][retry]") {
    STATIC_REQUIRE(defaults.max_attempts == 4);
    STATIC_REQUIRE(defaults.max_attempts_429 == 2);
    STATIC_REQUIRE(defaults.base_delay == milliseconds{1000});
    STATIC_REQUIRE(defaults.max_jitter == milliseconds{500});
    STATIC_REQUIRE(defaults.retry_after_cap == seconds{60});
    STATIC_REQUIRE(defaults.default_429_delay == seconds{60});
}

TEST_CASE("decide_retry: 5xx, timeout and protocol back off 1/2/4 s, then give up",
          "[net][retry]") {
    const AttemptOutcome first =
        GENERATE(status(500), status(502), status(503), status(504), status(599),
                 error(TransportErrc::timeout), error(TransportErrc::protocol));
    const milliseconds no_jitter{0};
    AttemptOutcome outcome = first;

    outcome.attempt = 1;
    CHECK(decide_retry(outcome, defaults, no_jitter) == retry_after_ms(1000));
    outcome.attempt = 2;
    CHECK(decide_retry(outcome, defaults, no_jitter) == retry_after_ms(2000));
    outcome.attempt = 3;
    CHECK(decide_retry(outcome, defaults, no_jitter) == retry_after_ms(4000));
    outcome.attempt = 4; // max_attempts reached
    CHECK(decide_retry(outcome, defaults, no_jitter) == no_retry);
    outcome.attempt = 5;
    CHECK(decide_retry(outcome, defaults, no_jitter) == no_retry);
}

TEST_CASE("decide_retry: other statuses are never retried", "[net][retry]") {
    const int code = GENERATE(100, 200, 204, 301, 304, 400, 401, 403, 404, 410, 418, 499, 600);
    for (int attempt = 1; attempt <= 4; ++attempt) {
        CAPTURE(code, attempt);
        CHECK(decide_retry(status(code, attempt), defaults, milliseconds{100}) == no_retry);
    }
}

TEST_CASE("decide_retry: connect, resolve, tls, cancelled, too_large and local are final",
          "[net][retry]") {
    const TransportErrc kind =
        GENERATE(TransportErrc::connect, TransportErrc::resolve, TransportErrc::tls,
                 TransportErrc::cancelled, TransportErrc::too_large, TransportErrc::local);
    CHECK(decide_retry(error(kind), defaults, milliseconds{0}) == no_retry);
}

TEST_CASE("decide_retry: a transport error wins over a status", "[net][retry]") {
    // 503 alone would be retried; with a non-retryable error there was no response at all.
    const AttemptOutcome both{.attempt = 1, .status = 503, .error = TransportErrc::connect};
    CHECK(decide_retry(both, defaults, milliseconds{0}) == no_retry);
    // ... and 429 alone would wait 60 s; a timeout backs off instead.
    const AttemptOutcome timeout{.attempt = 1, .status = 429, .error = TransportErrc::timeout};
    CHECK(decide_retry(timeout, defaults, milliseconds{0}) == retry_after_ms(1000));
}

TEST_CASE("decide_retry: an outcome with neither status nor error is not retried", "[net][retry]") {
    CHECK(decide_retry(AttemptOutcome{}, defaults, milliseconds{0}) == no_retry);
}

TEST_CASE("decide_retry: jitter is added to backoff and clamped to [0, max_jitter]",
          "[net][retry]") {
    CHECK(decide_retry(status(503), defaults, milliseconds{0}) == retry_after_ms(1000));
    CHECK(decide_retry(status(503), defaults, milliseconds{1}) == retry_after_ms(1001));
    CHECK(decide_retry(status(503), defaults, milliseconds{500}) == retry_after_ms(1500));
    CHECK(decide_retry(status(503), defaults, milliseconds{501}) == retry_after_ms(1500));
    CHECK(decide_retry(status(503, 3), defaults, milliseconds{99'999}) == retry_after_ms(4500));
    CHECK(decide_retry(status(503), defaults, milliseconds{-1}) == retry_after_ms(1000));
    CHECK(decide_retry(status(503), defaults, milliseconds::min()) == retry_after_ms(1000));
    CHECK(decide_retry(status(503), defaults, milliseconds::max()) == retry_after_ms(1500));

    RetryConfig negative = defaults;
    negative.max_jitter = milliseconds{-10};
    CHECK(decide_retry(status(503), negative, milliseconds{300}) == retry_after_ms(1000));
}

TEST_CASE("decide_retry: every backoff delay stays within its documented bound", "[net][retry]") {
    for (int attempt = 1; attempt < defaults.max_attempts; ++attempt) {
        for (const milliseconds jitter :
             {milliseconds{0}, milliseconds{250}, milliseconds{500}, milliseconds{10'000}}) {
            const RetryDecision d = decide_retry(status(500, attempt), defaults, jitter);
            milliseconds base = defaults.base_delay; // base_delay * 2^(attempt-1)
            for (int i = 1; i < attempt; ++i) {
                base *= 2;
            }
            CAPTURE(attempt, jitter.count());
            REQUIRE(d.retry);
            CHECK(d.delay >= base);
            CHECK(d.delay <= base + defaults.max_jitter);
        }
    }
}

TEST_CASE("decide_retry: malformed attempt numbers are clamped, never overflow", "[net][retry]") {
    // 0 and negatives count as the first attempt.
    CHECK(decide_retry(status(503, 0), defaults, milliseconds{0}) == retry_after_ms(1000));
    CHECK(decide_retry(status(503, -7), defaults, milliseconds{0}) == retry_after_ms(1000));
    CHECK(decide_retry(status(503, std::numeric_limits<int>::min()), defaults, milliseconds{0}) ==
          retry_after_ms(1000));
    CHECK(decide_retry(status(503, std::numeric_limits<int>::max()), defaults, milliseconds{0}) ==
          no_retry);

    // A config that allows enormous attempt counts: the doubling saturates instead of
    // overflowing (UBSan would flag a signed overflow here), and the result is clamped to
    // max_delay.
    RetryConfig huge = defaults;
    huge.max_attempts = std::numeric_limits<int>::max();
    const RetryDecision big =
        decide_retry(status(503, std::numeric_limits<int>::max() - 1), huge, milliseconds{500});
    CHECK(big.retry);
    CHECK(big.delay == max_delay);
    // The clamp keeps `now + delay` representable on the nanosecond steady clock.
    const auto steady_now = std::chrono::steady_clock::now();
    CHECK(steady_now + big.delay > steady_now);

    huge.base_delay = milliseconds::max();
    CHECK(decide_retry(status(503, 1), huge, milliseconds{500}) ==
          RetryDecision{.retry = true, .delay = max_delay});
    CHECK(decide_retry(status(503, 2), huge, milliseconds{0}) ==
          RetryDecision{.retry = true, .delay = max_delay});

    huge.base_delay = milliseconds{-5}; // treated as zero
    CHECK(decide_retry(status(503, 3), huge, milliseconds{7}) == retry_after_ms(7));
}

TEST_CASE("decide_retry: 429 without a rate-limit header waits default_429_delay once",
          "[net][retry]") {
    CHECK(decide_retry(status(429, 1), defaults, milliseconds{0}) == retry_after_ms(60'000));
    CHECK(decide_retry(status(429, 2), defaults, milliseconds{0}) == no_retry);
    CHECK(decide_retry(status(429, 3), defaults, milliseconds{0}) == no_retry);
}

TEST_CASE("decide_retry: 429 uses the parsed delay, without jitter, up to the cap",
          "[net][retry]") {
    CHECK(decide_retry(status(429, 1, milliseconds{0}), defaults, milliseconds{400}) ==
          retry_after_ms(0));
    CHECK(decide_retry(status(429, 1, milliseconds{1500}), defaults, milliseconds{400}) ==
          retry_after_ms(1500));
    CHECK(decide_retry(status(429, 1, milliseconds{60'000}), defaults, milliseconds{400}) ==
          retry_after_ms(60'000)); // exactly the cap is still retried
    CHECK(decide_retry(status(429, 1, milliseconds{-3}), defaults, milliseconds{0}) ==
          retry_after_ms(0)); // never negative
}

TEST_CASE("decide_retry: 429 beyond the cap is not retried at all", "[net][retry]") {
    CHECK(decide_retry(status(429, 1, milliseconds{60'001}), defaults, milliseconds{0}) ==
          no_retry);
    CHECK(decide_retry(status(429, 1, seconds{120}), defaults, milliseconds{0}) == no_retry);
    CHECK(decide_retry(status(429, 1, milliseconds::max()), defaults, milliseconds{0}) == no_retry);

    // A default wait above the cap means "give up" too.
    RetryConfig long_default = defaults;
    long_default.default_429_delay = seconds{61};
    CHECK(decide_retry(status(429, 1), long_default, milliseconds{0}) == no_retry);
    long_default.default_429_delay = seconds::max(); // saturated, not overflowed
    CHECK(decide_retry(status(429, 1), long_default, milliseconds{0}) == no_retry);
    long_default.retry_after_cap = seconds::max();
    CHECK(decide_retry(status(429, 1), long_default, milliseconds{0}) ==
          RetryDecision{.retry = true, .delay = max_delay}); // clamped, not seconds::max()
    CHECK(decide_retry(status(429, 1, milliseconds::max()), long_default, milliseconds{0}) ==
          RetryDecision{.retry = true, .delay = max_delay});
}

TEST_CASE("decide_retry: 403 and 400 are never retried, whatever the headers say", "[net][retry]") {
    const int code = GENERATE(400, 403);
    CHECK(decide_retry(status(code, 1, milliseconds{1000}), defaults, milliseconds{0}) == no_retry);
}

TEST_CASE("decide_retry: a 429 with an epoch 120 s ahead gives up without sleeping",
          "[net][retry]") {
    const auto wait = parse_retry_after("1700000120", "", now);
    REQUIRE(wait == milliseconds{120'000});
    CHECK(decide_retry(status(429, 1, wait), defaults, milliseconds{0}) == no_retry);
}

TEST_CASE("decide_retry: a 429 loop without headers sleeps 60 s once, then gives up",
          "[net][retry]") {
    // The caller's loop, driven by the fake clock (what T12 does around Transport::fetch).
    mangadex_dl::test::FakeClock clock;
    int attempts = 0;
    for (int attempt = 1;; ++attempt) {
        ++attempts;
        const RetryDecision d = decide_retry(status(429, attempt), defaults, milliseconds{0});
        if (!d.retry) {
            break;
        }
        REQUIRE(clock.sleep_until(clock.now() + d.delay, std::stop_token{}));
    }
    CHECK(attempts == 2);
    const std::vector<mangadex_dl::test::SleepRecord> sleeps = clock.sleeps();
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps.front().requested == milliseconds{60'000});
}

TEST_CASE("parse_retry_after: X-RateLimit-Retry-After is a UNIX epoch", "[net][retry]") {
    CHECK(parse_retry_after("1700000120", "", now) == milliseconds{120'000});
    CHECK(parse_retry_after("1700000001", "", now) == milliseconds{1000});
    // Sub-second "now": the wait is exact to the millisecond.
    CHECK(parse_retry_after("1700000120", "", now + milliseconds{250}) == milliseconds{119'750});
    // HTTP optional whitespace is ignored.
    CHECK(parse_retry_after(" \t1700000010 ", "", now) == milliseconds{10'000});
}

TEST_CASE("parse_retry_after: an epoch in the past or now is a zero wait", "[net][retry]") {
    CHECK(parse_retry_after("1700000000", "", now) == milliseconds{0});
    CHECK(parse_retry_after("1699999000", "", now) == milliseconds{0});
    CHECK(parse_retry_after("0", "", now) == milliseconds{0});
    CHECK(parse_retry_after("1700000000", "", now + milliseconds{999}) == milliseconds{0});
}

TEST_CASE("parse_retry_after: the epoch wins over Retry-After", "[net][retry]") {
    CHECK(parse_retry_after("1700000005", "30", now) == milliseconds{5000});
    CHECK(parse_retry_after("1600000000", "30", now) ==
          milliseconds{0}); // past epoch, still primary
}

TEST_CASE("parse_retry_after: Retry-After delta-seconds is the fallback", "[net][retry]") {
    const std::string garbage_epoch = GENERATE(as<std::string>{}, "", "   ", "abc", "-5", "+5",
                                               "12.5", "1e9", "17000001200x", "0x10", "1 2");
    CAPTURE(garbage_epoch);
    CHECK(parse_retry_after(garbage_epoch, "30", now) == milliseconds{30'000});
    CHECK(parse_retry_after(garbage_epoch, " 0 ", now) == milliseconds{0});
}

TEST_CASE("parse_retry_after: nullopt when both are absent or garbage", "[net][retry]") {
    CHECK_FALSE(parse_retry_after("", "", now).has_value());
    CHECK_FALSE(parse_retry_after("soon", "later", now).has_value());
    CHECK_FALSE(parse_retry_after("", "-1", now).has_value());
    CHECK_FALSE(parse_retry_after("", "1.5", now).has_value());
    // The HTTP-date form of Retry-After is not supported (MangaDex does not send it).
    CHECK_FALSE(parse_retry_after("", "Wed, 21 Oct 2015 07:28:00 GMT", now).has_value());
}

TEST_CASE("parse_retry_after: absurd values are clamped, never overflow", "[net][retry]") {
    const milliseconds year{std::chrono::days{365}};
    const auto far_epoch = parse_retry_after("99999999999999999999999999", "", now);
    REQUIRE(far_epoch.has_value());
    CHECK(far_epoch >= year);
    CHECK(far_epoch <= max_delay);

    const auto max_epoch = parse_retry_after("9223372036854775807", "", now);
    CHECK(max_epoch == far_epoch);

    const auto far_delta = parse_retry_after("", "99999999999999999999999999", now);
    CHECK(far_delta == far_epoch);

    // A wall clock far before 1970 must not overflow the subtraction either.
    const std::chrono::system_clock::time_point ancient{std::chrono::system_clock::duration::min()};
    const auto from_ancient = parse_retry_after("9223372036854775807", "", ancient);
    CHECK(from_ancient == far_epoch);

    // Clamped waits are far beyond the cap: decide_retry gives up.
    CHECK(decide_retry(status(429, 1, far_epoch), defaults, milliseconds{0}) == no_retry);
    // And the clamp keeps "now + wait" representable for the caller's message.
    CHECK(now + far_epoch.value_or(milliseconds{0}) > now);
}
