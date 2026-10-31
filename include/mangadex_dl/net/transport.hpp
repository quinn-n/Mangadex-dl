// SPDX-License-Identifier: GPL-3.0-or-later
//
// The network and time seams. Everything above them (retries, rate limits, the API client, the
// downloader) is unit-tested with fakes; the real implementations are the libcurl transport and
// the system clock. Frozen interface (download plan T1); clocks in src/core/net/clock.cpp.
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>

namespace mangadex_dl {

enum class HttpMethod {
    get,
    post,
};

/// One request. The transport only reads it during fetch(), and nothing in it needs to outlive the
/// call. `body` is the one member a transport may borrow rather than copy (see its doc).
///
/// Secrets: `authorization` and `body` may hold one (a bearer token, a login form body).
/// - The caller that built such a request wipes both with secure_wipe (secret.hpp) after fetch()
///   returns, on every path, and never copies a request that carries them.
/// - A transport never logs them, never puts them in a TransportError, never follows a redirect
///   with the Authorization header, and wipes any copy it makes itself (the header line, the
///   header-list node) on a best-effort basis. libcurl's internal copies are out of reach; that
///   residual is accepted.
struct HttpRequest {
    HttpMethod method = HttpMethod::get;
    std::string url{};
    /// POST only. Borrowed by the curl transport (CURLOPT_POSTFIELDS): it must stay unchanged
    /// until fetch() returns, and the transport resets the handle before then.
    std::string body{};
    std::string content_type{}; ///< POST only
    /// The whole Authorization value ("Bearer <t>"); empty = no header. CR, LF or NUL ->
    /// TransportErrc::local before any I/O. Set only by ApiClient::get_authorized.
    std::string authorization{};
    std::size_t max_body_bytes = 0; ///< a larger response body is TransportErrc::too_large
    std::chrono::milliseconds connect_timeout{15000};
    std::chrono::seconds stall_timeout{30}; ///< abort when no byte arrives for this long
    std::stop_token cancel{};               ///< a stop request aborts with TransportErrc::cancelled
};

/// Any HTTP status is a response (retry decisions belong to the caller). Header values are
/// copies, empty when absent.
struct HttpResponse {
    int status = 0;
    std::string body{};
    std::string retry_after{};             ///< Retry-After
    std::string x_ratelimit_retry_after{}; ///< X-RateLimit-Retry-After (a UNIX epoch)
    std::string x_cache{};                 ///< X-Cache (MD@H reporting)
    std::chrono::milliseconds elapsed{};

    friend bool operator==(const HttpResponse&, const HttpResponse&) = default;
};

/// Why no HTTP response was obtained.
enum class TransportErrc {
    connect,   ///< connection refused or unreachable
    resolve,   ///< DNS failure
    tls,       ///< handshake or certificate verification failure
    timeout,   ///< connect timeout or stall
    cancelled, ///< HttpRequest::cancel was triggered
    too_large, ///< the body exceeded max_body_bytes
    protocol,  ///< reset, empty reply, malformed response
    local,     ///< a local problem (out of memory, unusable library, bad URL)
};

struct TransportError {
    TransportErrc kind = TransportErrc::protocol;
    std::string message{}; ///< one line, for diagnostics

    friend bool operator==(const TransportError&, const TransportError&) = default;
};

using TransportResult = std::expected<HttpResponse, TransportError>;

/// Performs HTTP(S) requests. Implementations are thread-safe: fetch() may be called
/// concurrently from several threads on the same object. Failures are returned, not thrown;
/// if an implementation throws anyway (std::bad_alloc) on a FetchPool worker, the pool catches
/// it there and returns it as Errc::failure.
class Transport {
public:
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    [[nodiscard]] virtual TransportResult fetch(const HttpRequest& request) = 0;

protected:
    Transport() = default;
};

/// Time source and sleeper. Implementations are thread-safe.
class Clock {
public:
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    /// Monotonic time, for scheduling.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
    /// Wall-clock time, only for converting X-RateLimit-Retry-After epochs.
    [[nodiscard]] virtual std::chrono::system_clock::time_point wall_now() const noexcept = 0;
    /// Sleeps until `deadline`. Returns false (early) when `stop` is requested, true otherwise.
    [[nodiscard]] virtual bool sleep_until(std::chrono::steady_clock::time_point deadline,
                                           std::stop_token stop) = 0;

protected:
    Clock() = default;
};

/// The real clock: a function-local static (thread-safe initialization), alive until exit.
/// sleep_until waits on a condition variable with the stop token, so a stop wakes it promptly.
[[nodiscard]] Clock& system_clock();

/// A virtual clock for fixture mode and tests: sleep_until advances the virtual time and returns
/// at once (false if `stop` is already requested).
[[nodiscard]] std::unique_ptr<Clock> make_instant_clock();

} // namespace mangadex_dl
