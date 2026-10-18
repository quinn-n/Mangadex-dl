// SPDX-License-Identifier: GPL-3.0-or-later
//
// Retry decisions, Retry-After parsing and the rate limiter (net/retry.hpp).
#include "mangadex_dl/net/retry.hpp"

#include "mangadex_dl/net/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

using std::chrono::milliseconds;
using Ms = milliseconds::rep;

inline constexpr Ms ms_max = std::numeric_limits<Ms>::max();

/// Upper bound for every delay this file produces: backoff, 429 waits, parsed rate-limit waits
/// and the limiter interval. Absurd inputs (an epoch in the year 300000, a config with huge
/// attempt counts) are clamped here, so callers can add any delay to a nanosecond time_point
/// (steady or system clock, good for ~292 years) without overflowing. Anything this long is far
/// above retry_after_cap, i.e. "do not retry" for 429.
inline constexpr milliseconds max_delay = std::chrono::days{366};

/// Seconds that still fit in milliseconds::rep after multiplying by 1000.
inline constexpr std::int64_t max_seconds = ms_max / 1000;

[[nodiscard]] Ms saturating_add(Ms a, Ms b) noexcept {
    // Both are non-negative here, so only the upper bound can overflow.
    return a > ms_max - b ? ms_max : a + b;
}

/// base * 2^(attempt-1), saturating at ms_max. `base` and `attempt` are already clamped.
[[nodiscard]] Ms backoff(Ms base, int attempt) noexcept {
    Ms delay = base;
    for (int i = 1; i < attempt; ++i) {
        if (delay > ms_max / 2) {
            return ms_max;
        }
        delay *= 2;
    }
    return delay;
}

[[nodiscard]] bool retryable_error(TransportErrc error) noexcept {
    switch (error) {
    case TransportErrc::timeout:
    case TransportErrc::protocol:
        return true;
    case TransportErrc::connect:
    case TransportErrc::resolve:
    case TransportErrc::tls:
    case TransportErrc::cancelled:
    case TransportErrc::too_large:
    case TransportErrc::local:
        return false;
    }
    return false; // out-of-range enum value
}

[[nodiscard]] std::string_view trim_ows(std::string_view text) noexcept {
    // HTTP optional whitespace: SP and HTAB only.
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

/// A non-negative decimal integer (digits only, after trimming OWS), saturated at max_seconds.
/// nullopt for anything else (empty, a sign, a fraction, trailing junk, an HTTP-date).
[[nodiscard]] std::optional<std::int64_t> parse_seconds(std::string_view text) noexcept {
    const std::string_view digits = trim_ows(text);
    if (digits.empty() ||
        !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char c : digits) {
        const std::int64_t digit = c - '0';
        if (value > (max_seconds - digit) / 10) {
            return max_seconds; // all digits, just huge
        }
        value = (value * 10) + digit;
    }
    return value;
}

/// seconds -> milliseconds without the signed overflow a plain conversion has for huge values.
[[nodiscard]] milliseconds saturating_ms(std::chrono::seconds s) noexcept {
    return milliseconds{std::clamp(s.count(), -max_seconds, max_seconds) * 1000};
}

[[nodiscard]] milliseconds clamp_delay(Ms delay) noexcept {
    return std::clamp(milliseconds{delay}, milliseconds{0}, max_delay);
}

} // namespace

RetryDecision decide_retry(const AttemptOutcome& outcome, const RetryConfig& config,
                           milliseconds jitter) noexcept {
    // A malformed attempt number (0, negative) counts as the first attempt; very large ones are
    // capped so the backoff loop stays bounded (the delay saturates long before 64 doublings).
    const int attempt = std::clamp(outcome.attempt, 1, 64);

    // An error means no HTTP response was obtained, whatever `status` says.
    if (!outcome.error.has_value() && outcome.status.has_value() && *outcome.status == 429) {
        if (attempt >= config.max_attempts_429) {
            return {};
        }
        const milliseconds delay =
            std::clamp(outcome.retry_after.value_or(saturating_ms(config.default_429_delay)),
                       milliseconds{0}, max_delay);
        if (delay > saturating_ms(config.retry_after_cap)) {
            return {}; // the caller gives up and reports when the limit ends
        }
        return RetryDecision{.retry = true, .delay = delay};
    }

    bool retryable = false;
    if (outcome.error.has_value()) {
        retryable = retryable_error(*outcome.error);
    } else if (outcome.status.has_value()) {
        retryable = *outcome.status >= 500 && *outcome.status <= 599;
    }
    if (!retryable || attempt >= config.max_attempts) {
        return {};
    }

    const Ms base = std::max(config.base_delay.count(), Ms{0});
    const Ms max_jitter = std::max(config.max_jitter.count(), Ms{0});
    const Ms extra = std::clamp(jitter.count(), Ms{0}, max_jitter);
    return RetryDecision{
        .retry = true,
        .delay = clamp_delay(saturating_add(backoff(base, attempt), extra)),
    };
}

std::optional<milliseconds> parse_retry_after(std::string_view x_ratelimit_retry_after_epoch,
                                              std::string_view retry_after_fallback,
                                              std::chrono::system_clock::time_point now) {
    if (const auto epoch = parse_seconds(x_ratelimit_retry_after_epoch)) {
        const Ms epoch_ms = *epoch * 1000; // cannot overflow: *epoch <= max_seconds
        const Ms now_ms = std::chrono::floor<milliseconds>(now.time_since_epoch()).count();
        if (epoch_ms <= now_ms) {
            return milliseconds{0};
        }
        // epoch_ms > now_ms; the difference overflows only when now_ms is very negative.
        if (now_ms < 0 && epoch_ms > ms_max + now_ms) {
            return max_delay;
        }
        return clamp_delay(epoch_ms - now_ms);
    }
    if (const auto delta = parse_seconds(retry_after_fallback)) {
        return clamp_delay(*delta * 1000);
    }
    return std::nullopt;
}

bool RateLimiter::acquire(std::stop_token stop) {
    if (stop.stop_requested()) {
        return false;
    }
    // Clamped like every other delay, so `slot + interval` cannot overflow the time_point.
    const milliseconds interval = std::clamp(min_interval_, milliseconds{0}, max_delay);
    std::chrono::steady_clock::time_point slot{};
    bool must_wait = false;
    {
        const std::scoped_lock lock{mutex_};
        const auto now = clock_->now();
        slot = std::max(now, next_slot_);
        must_wait = slot > now;
        next_slot_ = slot + interval;
    }
    // The lock is released: other threads reserve the following slots while this one sleeps.
    if (must_wait) {
        return clock_->sleep_until(slot, std::move(stop));
    }
    return !stop.stop_requested();
}

} // namespace mangadex_dl
