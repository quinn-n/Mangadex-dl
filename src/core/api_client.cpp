// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MangaDex API client (api_client.hpp; download plan T12 + rev1).
//
// Every request goes through one retry loop (State::perform):
//   limiter(s) -> Transport::fetch -> trace -> [MD@H report] -> decide_retry -> sleep or return.
// - Limiters: the shared API limiter for API documents and covers, plus the at-home limiter for
//   /at-home/server. Page images skip the API limiter (image hosts are exempt from the API rate
//   limit; --jobs bounds their concurrency) unless the image URL's host is mangadex.org or a
//   subdomain. MD@H report POSTs are not limited: they go to a different host and are
//   best-effort. (Orchestrator decision 2026-10-18, download-plan-rev1.md.)
// - Retries follow net/retry.hpp exactly; the client sleeps only when decide_retry says so, and a
//   hard ceiling on attempts keeps the loop bounded whatever the configuration says.
// - Status mapping happens after the loop: 2xx is success, 404 on the title is Errc::not_found,
//   everything else is Errc::network as `<url>: <reason>` (403/400 with a hint, 429 with the UTC
//   time the limit ends). A stop request is Errc::interrupted, wherever it lands.
//
// Thread safety: the transport, clock, trace sink and limiters are thread-safe by contract; the
// configuration is immutable after construction; the jitter function is called under its own
// mutex. Nothing here keeps a view into a response or a request beyond the call that made it.
#include "mangadex_dl/api_client.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

using std::chrono::milliseconds;

/// The MD@H reliability report endpoint (official API only).
inline constexpr std::string_view report_endpoint = "https://api.mangadex.network/report";
/// The report answer is ignored; anything larger than this is cut off as too_large.
inline constexpr std::size_t max_report_response_bytes = std::size_t{64} * 1024;
inline constexpr std::chrono::seconds report_timeout{5};

/// Attempts per logical request, whatever RetryConfig says (decide_retry clamps its attempt
/// number at 64, so a huge max_attempts would otherwise retry forever).
inline constexpr int max_attempts_ceiling = 64;

/// The longest "rate limited until" horizon (matches parse_retry_after's clamp).
inline constexpr std::chrono::seconds max_rate_limit_wait = std::chrono::days{366};

[[nodiscard]] constexpr bool is_success(int status) noexcept {
    return status >= 200 && status <= 299;
}

[[nodiscard]] Error interrupted_error() {
    return Error{.code = Errc::interrupted, .message = "interrupted"};
}

[[nodiscard]] Error network_error(std::string_view url, std::string_view reason) {
    return Error{.code = Errc::network, .message = std::format("{}: {}", url, reason)};
}

/// `a + b`, or nullopt when it would overflow.
[[nodiscard]] constexpr std::optional<std::size_t> checked_add(std::size_t a,
                                                               std::size_t b) noexcept {
    if (a > std::numeric_limits<std::size_t>::max() - b) {
        return std::nullopt;
    }
    return a + b;
}

/// `now + wait`, saturating at the latest representable time point.
[[nodiscard]] std::chrono::system_clock::time_point
saturating_later(std::chrono::system_clock::time_point now, milliseconds wait) noexcept {
    using Duration = std::chrono::system_clock::duration;
    // wait is in [0, 366 days] here, so the cast to the clock's (nanosecond) duration is exact.
    const auto by = std::chrono::duration_cast<Duration>(wait);
    if (now.time_since_epoch() > Duration::max() - by) {
        return std::chrono::system_clock::time_point::max();
    }
    return now + by;
}

static_assert(feed_page_limit <= feed_offset_cap && cover_page_limit <= feed_offset_cap);

/// One logical GET (possibly several attempts).
struct GetSpec {
    std::string url{};
    std::size_t max_body_bytes = 0;
    bool api_limit = true;      ///< wait for the shared API limiter
    bool at_home_limit = false; ///< also wait for the at-home limiter
    bool report = false;        ///< send an MD@H report after every attempt (page images)
};

} // namespace

Endpoints endpoints_for(const ApiBase& base, bool report) {
    switch (base.kind()) {
    case ApiBase::Kind::official: {
        Endpoints endpoints{};
        if (report) {
            endpoints.report_url = std::string{report_endpoint};
        }
        return endpoints;
    }
    case ApiBase::Kind::https:
    case ApiBase::Kind::http_loopback:
        return Endpoints{.api = std::string{base.url()}};
    case ApiBase::Kind::fixture: {
        // base.url() is `file://<absolute dir>` without a trailing '/'.
        const std::string_view root = base.url();
        return Endpoints{
            .api = std::format("{}/api", root),
            .uploads = std::format("{}/uploads", root),
            .at_home_override = std::format("{}/at-home", root),
        };
    }
    }
    return Endpoints{}; // out-of-range enum value: the official defaults, without reports
}

struct ApiClient::State {
    State(Transport& transport_ref, Clock& clock_ref, ClientConfig client_config,
          std::function<std::uint32_t(std::uint32_t)> jitter_fn, TraceSink* trace_sink)
        : transport(&transport_ref), clock(&clock_ref), config(std::move(client_config)),
          trace(trace_sink), api_limiter(clock_ref, config.api_interval),
          at_home_limiter(clock_ref, config.at_home_interval), jitter(std::move(jitter_fn)) {}

    // ---- the retry loop ----------------------------------------------------------------------

    /// GETs `spec.url` until success or until the retry policy gives up. The value is the last
    /// response (2xx or not); the error is a transport failure (Errc::network) or a stop
    /// (Errc::interrupted).
    [[nodiscard]] Result<HttpResponse> perform(const GetSpec& spec, std::stop_token stop_token) {
        const HttpRequest request{
            .method = HttpMethod::get,
            .url = spec.url,
            .max_body_bytes = spec.max_body_bytes,
            .cancel = std::move(stop_token),
        };
        const std::stop_token& stop = request.cancel; // the request owns the token
        for (int attempt = 1;; ++attempt) {
            if (!wait_for_slot(spec, stop)) {
                return std::unexpected(interrupted_error());
            }
            const auto start = clock->now();
            TransportResult result = transport->fetch(request);
            const milliseconds elapsed = since(start);
            trace_attempt("GET", request.url, result, elapsed);
            if (spec.report) {
                send_report(request.url, result, elapsed, stop);
            }
            if (stop.stop_requested() ||
                (!result.has_value() && result.error().kind == TransportErrc::cancelled)) {
                return std::unexpected(interrupted_error());
            }
            if (result.has_value() && is_success(result->status)) {
                return std::move(*result);
            }

            const RetryDecision decision = decide(attempt, result);
            if (!decision.retry || attempt >= max_attempts_ceiling) {
                if (!result.has_value()) {
                    return std::unexpected(network_error(request.url, result.error().message));
                }
                return std::move(*result);
            }
            // decision.delay is clamped to 366 days by decide_retry: the sum cannot overflow.
            if (!clock->sleep_until(clock->now() + decision.delay, stop)) {
                return std::unexpected(interrupted_error());
            }
        }
    }

    /// Waits for the limiters `spec` asks for. False when a stop was requested first.
    [[nodiscard]] bool wait_for_slot(const GetSpec& spec, const std::stop_token& stop) {
        if (stop.stop_requested()) {
            return false;
        }
        if (spec.api_limit && !api_limiter.acquire(stop)) {
            return false;
        }
        return !spec.at_home_limit || at_home_limiter.acquire(stop);
    }

    /// The retry policy for a failed attempt.
    [[nodiscard]] RetryDecision decide(int attempt, const TransportResult& result) {
        const AttemptOutcome outcome = outcome_of(attempt, result);
        const RetryDecision decision = decide_retry(outcome, config.retry, milliseconds{0});
        if (decision.retry && outcome.status != 429) {
            // An exponential backoff: only now is a jitter value needed.
            return decide_retry(outcome, config.retry, draw_jitter());
        }
        return decision;
    }

    /// A failed attempt in the shape decide_retry wants.
    [[nodiscard]] AttemptOutcome outcome_of(int attempt, const TransportResult& result) const {
        AttemptOutcome outcome{.attempt = attempt};
        if (!result.has_value()) {
            outcome.error = result.error().kind;
            return outcome;
        }
        outcome.status = result->status;
        if (result->status == 429) {
            outcome.retry_after = parse_retry_after(result->x_ratelimit_retry_after,
                                                    result->retry_after, clock->wall_now());
        }
        return outcome;
    }

    /// A jitter value in [0, max_jitter], from the injected function (serialized calls).
    [[nodiscard]] milliseconds draw_jitter() {
        const milliseconds::rep max_count = config.retry.max_jitter.count();
        if (max_count <= 0) {
            return milliseconds{0};
        }
        const std::uint32_t max = std::in_range<std::uint32_t>(max_count)
                                      ? static_cast<std::uint32_t>(max_count)
                                      : std::numeric_limits<std::uint32_t>::max();
        std::uint32_t value = 0;
        {
            const std::scoped_lock lock{jitter_mutex};
            if (jitter) {
                value = jitter(max);
            }
        }
        return milliseconds{std::min(value, max)};
    }

    [[nodiscard]] milliseconds since(std::chrono::steady_clock::time_point start) const noexcept {
        return std::max(std::chrono::duration_cast<milliseconds>(clock->now() - start),
                        milliseconds{0});
    }

    // ---- diagnostics and reports -------------------------------------------------------------

    void trace_attempt(std::string_view method, std::string_view url, const TransportResult& result,
                       milliseconds elapsed) const {
        if (trace == nullptr) {
            return;
        }
        if (result.has_value()) {
            trace->trace(
                std::format("{} {} -> {} ({} ms)", method, url, result->status, elapsed.count()));
        } else {
            trace->trace(std::format("{} {} -> error: {} ({} ms)", method, url,
                                     result.error().message, elapsed.count()));
        }
    }

    /// One best-effort MD@H report for one image attempt: success or failure, never retried,
    /// the answer ignored. Skipped without a report URL, for mangadex.org hosts, and when the
    /// attempt ended because of a stop request (that says nothing about the node).
    void send_report(std::string_view image_url, const TransportResult& result,
                     milliseconds elapsed, const std::stop_token& stop) {
        if (!config.endpoints.report_url.has_value() || is_mangadex_org_host(image_url) ||
            stop.stop_requested() ||
            (!result.has_value() && result.error().kind == TransportErrc::cancelled)) {
            return;
        }
        const bool success = result.has_value() && is_success(result->status);
        const AtHomeReport report{
            .url = std::string{image_url},
            .success = success,
            .cached = success && result->x_cache.starts_with("HIT"),
            .bytes = success ? result->body.size() : 0U,
            .duration_ms = static_cast<std::uint64_t>(elapsed.count()), // since() is >= 0
        };
        // `post.body` lives until fetch() returns, as Transport requires.
        const HttpRequest post{
            .method = HttpMethod::post,
            .url = *config.endpoints.report_url,
            .body = at_home_report_json(report),
            .content_type = "application/json",
            .max_body_bytes = max_report_response_bytes,
            .connect_timeout = report_timeout,
            .stall_timeout = report_timeout,
            .cancel = stop,
        };
        const auto start = clock->now();
        const TransportResult answer = transport->fetch(post);
        trace_attempt("POST", post.url, answer, since(start));
    }

    // ---- status mapping ----------------------------------------------------------------------

    /// Errc::network for a final non-2xx response (404 on the title is handled by fetch_manga).
    [[nodiscard]] Error status_error(std::string_view url, const HttpResponse& response) const {
        const int status = response.status;
        if (status == 429) {
            const auto now = clock->wall_now();
            const milliseconds wait =
                parse_retry_after(response.x_ratelimit_retry_after, response.retry_after, now)
                    .value_or(std::chrono::duration_cast<milliseconds>(
                        std::clamp(config.retry.default_429_delay, std::chrono::seconds{0},
                                   max_rate_limit_wait)));
            const auto until =
                std::chrono::floor<std::chrono::seconds>(saturating_later(now, wait));
            return network_error(
                url, std::format("HTTP 429 (rate limited until {:%Y-%m-%d %H:%M:%S} UTC)", until));
        }
        if (status == 403) {
            return network_error(url, "HTTP 403 (access denied — possibly a temporary rate-limit "
                                      "ban; wait before retrying)");
        }
        if (status == 400) {
            return network_error(url, "HTTP 400 (bad request: the API rejected the query)");
        }
        if (status == 404) {
            return network_error(url, "HTTP 404 (not found)");
        }
        if (status >= 500 && status <= 599) {
            return network_error(url, std::format("HTTP {} (server error)", status));
        }
        return network_error(url, std::format("HTTP {}", status));
    }

    /// The body of a successful GET, or the mapped error.
    [[nodiscard]] Result<std::string> get_body(const GetSpec& spec, std::stop_token stop) {
        Result<HttpResponse> response = perform(spec, std::move(stop));
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }
        if (!is_success(response->status)) {
            return std::unexpected(status_error(spec.url, *response));
        }
        return std::move(response->body);
    }

    /// A JSON document from the API, parsed by `parse`; parse errors are prefixed with the URL.
    template <class T, class Parse>
    [[nodiscard]] Result<T> get_json(GetSpec spec, std::stop_token stop, const Parse& parse) {
        spec.max_body_bytes = max_api_json_bytes;
        const Result<std::string> body = get_body(spec, std::move(stop));
        if (!body) {
            return std::unexpected(body.error());
        }
        Result<T> parsed = parse(*body);
        if (!parsed) {
            return std::unexpected(Error{
                .code = parsed.error().code,
                .message = std::format("{}: {}", spec.url, parsed.error().message),
            });
        }
        return parsed;
    }

    /// Collects every item of an offset-paginated collection. Steps by the number of items
    /// actually received (so a server that returns fewer than asked never makes the client skip
    /// any), stops on an empty page or once `offset >= total` (an absent total reads as 0 and
    /// stops after the first page), and refuses, rather than truncates, a collection whose
    /// window would exceed feed_offset_cap. Every offset is bounded by the cap, so the loop ends.
    template <class Page, class Item, class UrlFor, class Parse>
    [[nodiscard]] Result<std::vector<Item>>
    paginate(std::size_t page_limit, std::vector<Item> Page::* items, const UrlFor& url_for,
             const Parse& parse, std::string_view too_many,
             // The frozen by-value fetch_* tokens are moved in; each page request copies it.
             // NOLINTNEXTLINE(performance-unnecessary-value-param): owned, copied per request
             std::stop_token stop) {
        std::vector<Item> all;
        std::size_t offset = 0;
        while (true) {
            // offset + page_limit > cap, written so that neither side can wrap.
            if (page_limit > feed_offset_cap || offset > feed_offset_cap - page_limit) {
                return std::unexpected(
                    Error{.code = Errc::network, .message = std::string{too_many}});
            }
            Result<Page> page = get_json<Page>(GetSpec{.url = url_for(offset)}, stop, parse);
            if (!page) {
                return std::unexpected(std::move(page.error()));
            }
            if (page->total > feed_offset_cap) {
                return std::unexpected(
                    Error{.code = Errc::network, .message = std::string{too_many}});
            }
            std::vector<Item>& received = (*page).*items;
            const std::size_t count = received.size();
            all.insert(all.end(), std::make_move_iterator(received.begin()),
                       std::make_move_iterator(received.end()));
            const std::optional<std::size_t> next = checked_add(offset, count);
            if (count == 0 || !next.has_value() || *next >= page->total) {
                return all;
            }
            offset = *next;
        }
    }

    Transport* transport = nullptr; ///< borrowed; never null
    Clock* clock = nullptr;         ///< borrowed; never null
    ClientConfig config{};          ///< never modified after construction
    TraceSink* trace = nullptr;     ///< borrowed; nullable
    // RateLimiter has no default constructor (it borrows the clock): initialized in the
    // constructor's member-initializer list, after `config`.
    RateLimiter api_limiter;
    RateLimiter at_home_limiter;
    std::mutex jitter_mutex{};
    std::function<std::uint32_t(std::uint32_t)> jitter{}; ///< guarded by jitter_mutex
};

ApiClient::ApiClient(Transport& transport, Clock& clock, ClientConfig config,
                     std::function<std::uint32_t(std::uint32_t)> jitter, TraceSink* trace)
    : state_(
          std::make_unique<State>(transport, clock, std::move(config), std::move(jitter), trace)) {}

ApiClient::~ApiClient() = default;

Result<MangaInfo> ApiClient::fetch_manga(const MangaId& id, std::stop_token stop) {
    State& s = *state_;
    const std::string url = manga_request_url(s.config.endpoints.api, id);
    Result<HttpResponse> response =
        s.perform(GetSpec{.url = url, .max_body_bytes = max_api_json_bytes}, std::move(stop));
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->status == 404) {
        return std::unexpected(Error{
            .code = Errc::not_found,
            // The id is validated and canonical, so echoing it is safe (the raw argument is not).
            .message = std::format(
                "title {} not found on MangaDex (HTTP 404); check the UUID or URL", id.str()),
        });
    }
    if (!is_success(response->status)) {
        return std::unexpected(s.status_error(url, *response));
    }
    Result<MangaInfo> info = parse_manga(response->body);
    if (!info) {
        return std::unexpected(Error{
            .code = info.error().code,
            .message = std::format("{}: {}", url, info.error().message),
        });
    }
    return info;
}

Result<std::vector<Chapter>> ApiClient::fetch_feed(const MangaId& id, std::string_view lang,
                                                   std::stop_token stop) {
    State& s = *state_;
    const std::string_view api = s.config.endpoints.api;
    return s.paginate(
        feed_page_limit, &FeedPage::chapters,
        [api, &id, lang](std::size_t offset) { return feed_request_url(api, id, lang, offset); },
        [](std::string_view json) { return parse_feed_page(json); },
        std::format("title has more than {} chapters in this language; refusing to truncate",
                    feed_offset_cap),
        std::move(stop));
}

Result<std::vector<Cover>> ApiClient::fetch_covers(const MangaId& id, std::stop_token stop) {
    State& s = *state_;
    const std::string_view api = s.config.endpoints.api;
    return s.paginate(
        cover_page_limit, &CoverPage::covers,
        [api, &id](std::size_t offset) { return cover_request_url(api, id, offset); },
        [](std::string_view json) { return parse_cover_page(json); },
        std::format("title has more than {} covers; refusing to truncate", feed_offset_cap),
        std::move(stop));
}

Result<AtHomeServer> ApiClient::fetch_at_home(std::string_view chapter_id, std::stop_token stop) {
    State& s = *state_;
    return s.get_json<AtHomeServer>(
        GetSpec{.url = at_home_request_url(s.config.endpoints.api, chapter_id),
                .at_home_limit = true},
        std::move(stop), [](std::string_view json) { return parse_at_home(json); });
}

Result<std::string> ApiClient::fetch_page(const AtHomeServer& server, ImageQuality quality,
                                          std::string_view file, std::stop_token stop) {
    State& s = *state_;
    const std::optional<std::string>& override_base = s.config.endpoints.at_home_override;
    // The server normally comes from parse_at_home (already validated); a hand-built one is
    // checked the same way before any of it reaches a URL.
    if (!override_base.has_value()) {
        if (const Result<void> valid = validate_at_home_base_url(server.base_url); !valid) {
            return std::unexpected(valid.error());
        }
    }
    if (!is_valid_at_home_token(server.hash) || !is_valid_at_home_token(file)) {
        return std::unexpected(Error{
            .code = Errc::network,
            .message = "at-home server: unsafe chapter hash or page file name",
        });
    }
    const std::string_view base = override_base.has_value() ? *override_base : server.base_url;
    std::string url = image_url(base, quality, server.hash, file);
    // Image hosts are exempt from the API rate limit; a mangadex.org host is not.
    const bool api_limit = is_mangadex_org_host(url);
    return s.get_body(GetSpec{.url = std::move(url),
                              .max_body_bytes = max_image_bytes,
                              .api_limit = api_limit,
                              .report = true},
                      std::move(stop));
}

Result<std::string> ApiClient::fetch_cover(const MangaId& id, std::string_view file,
                                           std::stop_token stop) {
    State& s = *state_;
    if (!is_valid_at_home_token(file)) {
        return std::unexpected(Error{.code = Errc::network, .message = "cover: unsafe file name"});
    }
    return s.get_body(GetSpec{.url = cover_image_url(s.config.endpoints.uploads, id, file),
                              .max_body_bytes = max_image_bytes},
                      std::move(stop));
}

} // namespace mangadex_dl
