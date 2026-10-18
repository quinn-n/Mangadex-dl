// SPDX-License-Identifier: GPL-3.0-or-later
//
// Retry policy and rate limiting: pure decisions plus a clock-driven limiter.
// Frozen interface (download plan T1, rev1); implemented in src/core/net/retry.cpp.
//
// Policy (gate decision 11; MangaDex escalates repeated 429s to a temporary 403 ban):
// - 5xx, TransportErrc::timeout and TransportErrc::protocol are retried up to `max_attempts`
//   attempts in total, waiting base_delay * 2^(attempt-1) plus jitter (1 s, 2 s, 4 s + jitter).
// - 429 is retried up to `max_attempts_429` attempts in total. The wait is the parsed rate-limit
//   delay, or `default_429_delay` when none was sent. A wait longer than `retry_after_cap` is not
//   retried at all: the caller gives up and reports when the limit ends.
// - Never retried: any other 4xx (403 and 400 included), connect, resolve, tls, too_large,
//   cancelled, local.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/net/transport.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>

namespace mangadex_dl {

struct RetryConfig {
    int max_attempts = 4;     ///< attempts in total (first try included) for 5xx/timeout/protocol
    int max_attempts_429 = 2; ///< attempts in total for 429
    std::chrono::milliseconds base_delay{1000};
    std::chrono::milliseconds max_jitter{500};
    std::chrono::seconds retry_after_cap{60};
    std::chrono::seconds default_429_delay{60};

    friend bool operator==(const RetryConfig&, const RetryConfig&) = default;
};

/// The result of one attempt: a status, or a transport error.
struct AttemptOutcome {
    int attempt = 1; ///< 1-based number of the attempt that just finished
    std::optional<int> status{};
    std::optional<TransportErrc> error{};
    std::optional<std::chrono::milliseconds> retry_after{}; ///< from parse_retry_after (429)

    friend bool operator==(const AttemptOutcome&, const AttemptOutcome&) = default;
};

struct RetryDecision {
    bool retry = false;
    std::chrono::milliseconds delay{}; ///< wait before the next attempt; zero when !retry

    friend bool operator==(const RetryDecision&, const RetryDecision&) = default;
};

/// Applies the policy above. `jitter` (0..config.max_jitter, chosen by the caller) is added to
/// exponential backoff delays only.
[[nodiscard]] RetryDecision decide_retry(const AttemptOutcome& outcome, const RetryConfig& config,
                                         std::chrono::milliseconds jitter) noexcept;

/// The rate-limit wait. Primary: `X-RateLimit-Retry-After`, a UNIX epoch in seconds, giving
/// max(0, epoch - now). Fallback, only when that header is absent or garbage: `Retry-After` in
/// delta-seconds. Never negative. nullopt when both are absent or garbage.
[[nodiscard]] std::optional<std::chrono::milliseconds>
parse_retry_after(std::string_view x_ratelimit_retry_after_epoch,
                  std::string_view retry_after_fallback, std::chrono::system_clock::time_point now);

/// Spaces acquisitions at least `min_interval` apart, across threads. Thread-safe: a mutex guards
/// the schedule and is never held while sleeping.
///
/// Borrows the clock, which must outlive the limiter. Neither copyable nor movable.
class RateLimiter {
public:
    RateLimiter(Clock& clock MANGADEX_DL_LIFETIMEBOUND, std::chrono::milliseconds min_interval)
        : clock_(&clock), min_interval_(min_interval) {}

    /// Waits for the next slot (via Clock::sleep_until). False if `stop` was requested first.
    [[nodiscard]] bool acquire(std::stop_token stop);

private:
    Clock* clock_ = nullptr; ///< borrowed; never null after construction
    std::chrono::milliseconds min_interval_{};
    std::mutex mutex_{};
    std::chrono::steady_clock::time_point next_slot_{}; ///< guarded by mutex_
};

} // namespace mangadex_dl
