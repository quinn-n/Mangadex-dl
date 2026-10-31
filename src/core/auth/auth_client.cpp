// SPDX-License-Identifier: GPL-3.0-or-later
//
// The token client (auth.hpp; follows plan T8): POSTs password, refresh and revoke forms to
// MangaDex's Keycloak (or the fixture's stand-in).
//
// request(): one logical grant, possibly several attempts:
//   guard -> cap -> [limiter -> Transport::fetch -> trace -> decide_retry -> sleep]* -> map.
// - The guard refuses (no I/O) a token URL that is neither under official_auth_issuer nor
//   file://, a second line of defence behind auth_endpoints_for.
// - The cap counts logical grants: the (max_token_requests + 1)-th is refused before any I/O.
// - Retries follow net/retry.hpp with the default RetryConfig: 5xx, timeout and protocol errors
//   with backoff, 429 with its rate-limit wait. Any other 4xx is answered once (Keycloak's
//   brute-force protection), and so is every other transport error.
// - Mapping, after the loop: 2xx -> parse_token_response (malformed -> content); a 4xx other than
//   429 whose body has a well-formed `error` -> refused, classified on that code only (never on
//   the status, V2); anything else -> request, "<url>: HTTP <n>" or "<url>: <transport error>".
//
// revoke(): one attempt, never retried and not counted, revoke_timeout connect and stall limits
// (HttpRequest has no total-time field, so the transport's overall deadline applies); any 2xx is
// success whatever the body (Keycloak answers 200 even with a JSON error for an unknown token, V3).
//
// A grant that arrives with a 2xx is returned even if a stop is pending by then: it may carry a
// rotated refresh token, which the caller must persist before it honours the stop.
//
// Secret hygiene: the request never carries an Authorization. Its body is a plain-string copy of
// the caller's Secret, wiped on every path after fetch() returns; every response body is wiped
// once it has been read. The -vv trace line is method, URL, status (or the transport error) and
// time, never a body or a header.
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

using std::chrono::milliseconds;

inline constexpr std::string_view form_content_type = "application/x-www-form-urlencoded";
inline constexpr std::string_view fixture_scheme = "file://";

/// Attempts per logical request, whatever RetryConfig says (as in the API client).
inline constexpr int max_attempts_ceiling = 64;

[[nodiscard]] constexpr bool is_success(int status) noexcept {
    return status >= 200 && status <= 299;
}

/// A refusal is classified only for a final 4xx; 429 is a rate limit, never a refusal.
[[nodiscard]] constexpr bool is_refusal_status(int status) noexcept {
    return status >= 400 && status <= 499 && status != 429;
}

/// Under the official issuer (`<issuer>/…`, so the host is auth.mangadex.org) or a fixture.
[[nodiscard]] bool is_allowed_auth_url(std::string_view url) noexcept {
    if (url.starts_with(fixture_scheme)) {
        return true;
    }
    return url.starts_with(official_auth_issuer) && url.size() > official_auth_issuer.size() &&
           url[official_auth_issuer.size()] == '/';
}

[[nodiscard]] Error refused_url_error() {
    return Error{.code = Errc::usage,
                 .message = "refusing to send MangaDex login details to a server that is neither "
                            "MangaDex's login server nor a fixture tree"};
}

[[nodiscard]] Error interrupted_error() {
    return Error{.code = Errc::interrupted, .message = "interrupted"};
}

[[nodiscard]] Error network_error(std::string_view url, std::string_view reason) {
    return Error{.code = Errc::network, .message = std::format("{}: {}", url, reason)};
}

/// The reason for a final non-2xx status that is not a refusal.
[[nodiscard]] std::string status_reason(int status) {
    if (status == 429) {
        return "HTTP 429 (rate limited; wait before logging in again)";
    }
    if (status == 403) {
        return "HTTP 403 (access denied — possibly a temporary rate-limit ban; wait before "
               "retrying)";
    }
    if (status >= 500 && status <= 599) {
        return std::format("HTTP {} (server error)", status);
    }
    return std::format("HTTP {}", status);
}

[[nodiscard]] AuthFailure failure(AuthFailureKind kind, Error error) {
    return AuthFailure{.kind = kind, .error = std::move(error)};
}

/// Wipes a plain string that held a secret when it goes out of scope. Borrows the string, which
/// must outlive the guard (declare the guard after it).
class WipeOnExit {
public:
    explicit WipeOnExit(std::string& text MANGADEX_DL_LIFETIMEBOUND) noexcept : text_(&text) {}
    WipeOnExit(const WipeOnExit&) = delete;
    WipeOnExit& operator=(const WipeOnExit&) = delete;
    WipeOnExit(WipeOnExit&&) = delete;
    WipeOnExit& operator=(WipeOnExit&&) = delete;
    ~WipeOnExit() { secure_wipe(*text_); }

private:
    std::string* text_ = nullptr; ///< borrowed; never null
};

/// Wipes a transport result's body (if it has one) when it goes out of scope.
class WipeResultOnExit {
public:
    explicit WipeResultOnExit(TransportResult& result MANGADEX_DL_LIFETIMEBOUND) noexcept
        : result_(&result) {}
    WipeResultOnExit(const WipeResultOnExit&) = delete;
    WipeResultOnExit& operator=(const WipeResultOnExit&) = delete;
    WipeResultOnExit(WipeResultOnExit&&) = delete;
    WipeResultOnExit& operator=(WipeResultOnExit&&) = delete;
    ~WipeResultOnExit() {
        if (result_->has_value()) {
            secure_wipe((*result_)->body);
        }
    }

private:
    TransportResult* result_ = nullptr; ///< borrowed; never null
};

} // namespace

struct AuthClient::State {
    State(Transport& transport_ref, Clock& clock_ref, AuthEndpoints auth_endpoints,
          std::function<std::uint32_t(std::uint32_t)> jitter_fn, TraceSink* trace_sink)
        : transport(&transport_ref), clock(&clock_ref), endpoints(std::move(auth_endpoints)),
          jitter(std::move(jitter_fn)), trace(trace_sink),
          limiter(clock_ref, auth_request_interval) {}

    /// A form POST to `url`, still without its body: the caller declares a WipeOnExit on
    /// `body` first and only then copies the secret in (fill_body), so no allocation failure can
    /// leave an unwiped copy behind.
    [[nodiscard]] static HttpRequest form_post(const std::string& url, std::stop_token stop) {
        return HttpRequest{
            .method = HttpMethod::post,
            .url = url,
            .content_type = std::string{form_content_type},
            .max_body_bytes = max_token_response_bytes,
            .cancel = std::move(stop),
        };
    }

    /// Copies `body` into `out` (empty, guarded by the caller's WipeOnExit), reserved first so
    /// the append never reallocates.
    static void fill_body(std::string& out, const Secret& body) {
        out.reserve(body.size());
        out.append(body.reveal());
    }

    /// One attempt: the limiter, fetch, the trace line. nullopt when a stop came first.
    [[nodiscard]] std::optional<TransportResult> attempt(const HttpRequest& request) {
        if (request.cancel.stop_requested() || !limiter.acquire(request.cancel)) {
            return std::nullopt;
        }
        const auto start = clock->now();
        TransportResult result = transport->fetch(request);
        // Wipes the body if trace_attempt throws, and the moved-from local (SSO bytes) after the
        // implicit move into the return value.
        const WipeResultOnExit wipe_local{result};
        trace_attempt(request.url, result, since(start));
        return result;
    }

    [[nodiscard]] AuthResult<TokenGrant> grant(GrantKind kind, const Secret& body,
                                               std::stop_token stop) {
        HttpRequest request = form_post(endpoints.token_url, std::move(stop));
        const WipeOnExit wipe_body{request.body};
        fill_body(request.body, body);
        const std::stop_token& cancel = request.cancel; // the request owns the token
        const std::string& url = request.url;

        for (int number = 1;; ++number) {
            std::optional<TransportResult> outcome = attempt(request);
            if (!outcome.has_value()) {
                return std::unexpected(failure(AuthFailureKind::request, interrupted_error()));
            }
            TransportResult& result = *outcome;
            const WipeResultOnExit wipe_response{result};
            // A 2xx is returned even when a stop is already pending: the server may have rotated
            // the refresh token, so the caller must persist the grant before it honours the stop.
            if (result.has_value() && is_success(result->status)) {
                Result<TokenGrant> parsed = parse_token_response(result->body);
                if (!parsed) {
                    return std::unexpected(failure(AuthFailureKind::content,
                                                   network_error(url, parsed.error().message)));
                }
                return std::move(*parsed);
            }
            if (cancel.stop_requested() ||
                (!result.has_value() && result.error().kind == TransportErrc::cancelled)) {
                return std::unexpected(failure(AuthFailureKind::request, interrupted_error()));
            }

            const RetryDecision decision = decide(number, result);
            if (decision.retry && number < max_attempts_ceiling) {
                // decision.delay is clamped to 366 days by decide_retry: the sum cannot overflow.
                if (!clock->sleep_until(clock->now() + decision.delay, cancel)) {
                    return std::unexpected(failure(AuthFailureKind::request, interrupted_error()));
                }
                continue;
            }
            return std::unexpected(final_failure(kind, url, result));
        }
    }

    /// The failure for the last attempt of a grant (not a success, not retried).
    [[nodiscard]] static AuthFailure final_failure(GrantKind kind, std::string_view url,
                                                   const TransportResult& result) {
        if (!result.has_value()) {
            if (result.error().kind == TransportErrc::too_large) {
                return failure(AuthFailureKind::content,
                               network_error(url, std::format("the response is larger than {} "
                                                              "bytes",
                                                              max_token_response_bytes)));
            }
            return failure(AuthFailureKind::request, network_error(url, result.error().message));
        }
        const int status = result->status;
        if (is_refusal_status(status)) {
            if (std::optional<TokenErrorInfo> info = classify_token_error(kind, result->body)) {
                info->status = status;
                // A placeholder message without the username; the login flow builds the one the
                // user sees with token_refusal_error (or decides on a fallback).
                Error error = token_refusal_error(*info, kind, {});
                return AuthFailure{
                    .kind = AuthFailureKind::refused,
                    .info = std::move(*info),
                    .error = std::move(error),
                };
            }
        }
        return failure(AuthFailureKind::request, network_error(url, status_reason(status)));
    }

    [[nodiscard]] Result<void> revoke(const Secret& body, std::stop_token stop) {
        HttpRequest request = form_post(endpoints.revoke_url, std::move(stop));
        // HttpRequest has no total-time limit: the transport's own overall deadline (its
        // configuration, not per request) still applies. The connect and stall limits keep a dead
        // server from holding logout, and the stop token aborts the transfer at once.
        request.connect_timeout = revoke_timeout;
        request.stall_timeout = revoke_timeout;
        const WipeOnExit wipe_body{request.body};
        fill_body(request.body, body);

        std::optional<TransportResult> outcome = attempt(request);
        if (!outcome.has_value()) {
            return std::unexpected(interrupted_error());
        }
        TransportResult& result = *outcome;
        const WipeResultOnExit wipe_response{result};
        if (request.cancel.stop_requested() ||
            (!result.has_value() && result.error().kind == TransportErrc::cancelled)) {
            return std::unexpected(interrupted_error());
        }
        if (!result.has_value()) {
            return std::unexpected(network_error(request.url, result.error().message));
        }
        if (!is_success(result->status)) {
            return std::unexpected(network_error(request.url, status_reason(result->status)));
        }
        return {};
    }

    /// The retry policy for a failed attempt (jitter is drawn only for a backoff).
    [[nodiscard]] RetryDecision decide(int number, const TransportResult& result) const {
        AttemptOutcome outcome{.attempt = number};
        if (!result.has_value()) {
            outcome.error = result.error().kind;
        } else {
            outcome.status = result->status;
            if (result->status == 429) {
                outcome.retry_after = parse_retry_after(result->x_ratelimit_retry_after,
                                                        result->retry_after, clock->wall_now());
            }
        }
        const RetryDecision decision = decide_retry(outcome, retry, milliseconds{0});
        if (decision.retry && outcome.status != 429) {
            return decide_retry(outcome, retry, draw_jitter());
        }
        return decision;
    }

    /// A jitter value in [0, max_jitter], from the injected function (if any).
    [[nodiscard]] milliseconds draw_jitter() const {
        const milliseconds::rep max_count = retry.max_jitter.count();
        if (max_count <= 0 || !jitter) {
            return milliseconds{0};
        }
        const std::uint32_t max = std::in_range<std::uint32_t>(max_count)
                                      ? static_cast<std::uint32_t>(max_count)
                                      : std::numeric_limits<std::uint32_t>::max();
        return milliseconds{std::min(jitter(max), max)};
    }

    [[nodiscard]] milliseconds since(std::chrono::steady_clock::time_point start) const noexcept {
        return std::max(std::chrono::duration_cast<milliseconds>(clock->now() - start),
                        milliseconds{0});
    }

    /// The -vv line: method, URL, status (or the transport error) and time; never a body.
    void trace_attempt(std::string_view url, const TransportResult& result,
                       milliseconds elapsed) const {
        if (trace == nullptr) {
            return;
        }
        if (result.has_value()) {
            trace->trace(
                std::format("POST {} -> {} ({} ms)", url, result->status, elapsed.count()));
        } else {
            trace->trace(std::format("POST {} -> error: {} ({} ms)", url, result.error().message,
                                     elapsed.count()));
        }
    }

    Transport* transport = nullptr; ///< borrowed; never null
    Clock* clock = nullptr;         ///< borrowed; never null
    AuthEndpoints endpoints{};      ///< never modified after construction
    std::function<std::uint32_t(std::uint32_t)> jitter{};
    TraceSink* trace = nullptr; ///< borrowed; nullable
    RetryConfig retry{};
    // RateLimiter has no default constructor (it borrows the clock): initialized in the
    // constructor's member-initializer list.
    RateLimiter limiter;
    int requests = 0; ///< logical grant requests so far
};

AuthClient::AuthClient(Transport& transport, Clock& clock, AuthEndpoints endpoints,
                       std::function<std::uint32_t(std::uint32_t)> jitter, TraceSink* trace)
    : state_(std::make_unique<State>(transport, clock, std::move(endpoints), std::move(jitter),
                                     trace)) {}

AuthClient::~AuthClient() = default;

AuthResult<TokenGrant> AuthClient::request(GrantKind grant, const Secret& body,
                                           std::stop_token stop) {
    State& s = *state_;
    if (!is_allowed_auth_url(s.endpoints.token_url)) {
        return std::unexpected(failure(AuthFailureKind::local, refused_url_error()));
    }
    if (s.requests >= max_token_requests) {
        return std::unexpected(
            failure(AuthFailureKind::content,
                    Error{.code = Errc::network,
                          .message = std::format("more than {} MangaDex login requests in one run; "
                                                 "refusing to continue",
                                                 max_token_requests)}));
    }
    ++s.requests;
    return s.grant(grant, body, std::move(stop));
}

Result<void> AuthClient::revoke(const Secret& body, std::stop_token stop) {
    State& s = *state_;
    if (!is_allowed_auth_url(s.endpoints.revoke_url)) {
        return std::unexpected(refused_url_error());
    }
    return s.revoke(body, std::move(stop));
}

int AuthClient::requests() const noexcept { return state_->requests; }

} // namespace mangadex_dl
