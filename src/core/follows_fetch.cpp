// SPDX-License-Identifier: GPL-3.0-or-later
//
// fetch_follows_collection (follows.hpp; follows plan T13): every page of GET /user/follows/manga
// and, when asked for, the one GET /manga/status, through ApiClient::get_authorized with the
// TokenProvider's Authorization value.
//
// Token handling, per request: provider.authorization() (it refreshes first when the token
// expires within refresh_ahead), then the request; a 401 gets one refresh_after_unauthorized()
// and one retry of that same request; a second 401 is {refused, Errc::auth_rejected}. The
// Authorization value lives in one Secret per attempt, is only borrowed by get_authorized, and is
// wiped when that Secret dies at the end of the attempt: no copy outlives a request.
//
// Paging mirrors ApiClient::State::paginate (Spec issue 1): step by the items received, stop on
// an empty page or once offset >= total, and refuse (never truncate) anything past the
// max_follows window. Failures keep the mdlist split: transport, HTTP status and interrupt are
// `request` (the app adds the network remedy); a malformed page or map, or the cap, is `content`.
//
// Threads: the ApiClient is built with endpoints_for(api_base, report = false), so it starts no
// reporter thread; the InterruptBridge's poller is the only thread this file starts, and the
// provider is called on the calling thread only. Declaration order is the borrow order and
// everything is destroyed in reverse: the bridge is joined first, then the client, then the stop
// source, all before fetch_follows_collection returns.
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

using std::chrono::milliseconds;

/// The refusal text of max_follows (spec rev3 S3).
inline constexpr std::string_view too_many_follows =
    "you follow more than 10000 titles on MangaDex; refusing to truncate the list";
static_assert(max_follows == 10000, "too_many_follows spells the cap out");
static_assert(follows_page_limit <= max_follows);

/// The longest "rate limited until" horizon (as ApiClient's, matching parse_retry_after's clamp).
inline constexpr std::chrono::seconds max_rate_limit_wait = std::chrono::days{366};

/// Which authorized collection a request reads: it names the collection in the 401 message.
enum class Collection {
    follows,
    statuses,
};

[[nodiscard]] std::string_view collection_name(Collection collection) noexcept {
    switch (collection) {
    case Collection::follows:
        return "your follows";
    case Collection::statuses:
        return "your reading statuses";
    }
    return "your follows"; // only reachable through an out-of-range enum value
}

[[nodiscard]] AuthFailure failure(AuthFailureKind kind, Error error) {
    return AuthFailure{.kind = kind, .info = {}, .error = std::move(error)};
}

[[nodiscard]] AuthFailure interrupted_failure() {
    return failure(AuthFailureKind::request,
                   Error{.code = Errc::interrupted, .message = "interrupted"});
}

/// A get_authorized error: its own guard (Errc::usage, no I/O) is a local problem; a transport
/// failure, exhausted retries or an interrupt is a request failure.
[[nodiscard]] AuthFailure client_failure(Error error) {
    const AuthFailureKind kind =
        error.code == Errc::usage ? AuthFailureKind::local : AuthFailureKind::request;
    return failure(kind, std::move(error));
}

/// The second 401 in a row: the API refused a token the provider had just obtained.
[[nodiscard]] AuthFailure unauthorized_failure(Collection collection) {
    return AuthFailure{
        .kind = AuthFailureKind::refused,
        .info = TokenErrorInfo{.refusal = TokenRefusal::other, .status = 401},
        .error = Error{.code = Errc::auth_rejected,
                       .message = std::format("MangaDex rejected a new access token for {} (HTTP "
                                              "401); check that the API client is approved and "
                                              "allowed to read your account",
                                              collection_name(collection))},
    };
}

[[nodiscard]] constexpr bool is_success(int status) noexcept {
    return status >= 200 && status <= 299;
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

/// Errc::network "<url>: <reason>" for a final non-2xx, non-401 response. The same texts as
/// ApiClient's own status mapping (get_authorized returns the response unmapped). Never quotes
/// the response body.
[[nodiscard]] Error status_error(std::string_view url, const HttpResponse& response,
                                 const Clock& clock, const RetryConfig& retry) {
    const auto network = [url](std::string_view reason) {
        return Error{.code = Errc::network, .message = std::format("{}: {}", url, reason)};
    };
    const int status = response.status;
    if (status == 429) {
        const auto now = clock.wall_now();
        const milliseconds wait =
            parse_retry_after(response.x_ratelimit_retry_after, response.retry_after, now)
                .value_or(std::chrono::duration_cast<milliseconds>(std::clamp(
                    retry.default_429_delay, std::chrono::seconds{0}, max_rate_limit_wait)));
        const auto until = std::chrono::floor<std::chrono::seconds>(saturating_later(now, wait));
        return network(
            std::format("HTTP 429 (rate limited until {:%Y-%m-%d %H:%M:%S} UTC)", until));
    }
    if (status == 403) {
        return network("HTTP 403 (access denied — possibly a temporary rate-limit ban; wait "
                       "before retrying)");
    }
    if (status == 400) {
        return network("HTTP 400 (bad request: the API rejected the query)");
    }
    if (status == 404) {
        return network("HTTP 404 (not found)");
    }
    if (status >= 500 && status <= 599) {
        return network(std::format("HTTP {} (server error)", status));
    }
    return network(std::format("HTTP {}", status));
}

/// One authorized request: where it goes and what it reads.
struct AuthorizedRequest {
    AuthorizedPath path = AuthorizedPath::user_follows_manga;
    std::size_t offset = 0;
    std::string url{}; ///< the URL get_authorized builds (for messages only)
    Collection collection = Collection::follows;
};

/// What every request of one collection fetch shares. Every pointer is borrowed from
/// fetch_follows_collection's frame (never null) and used only during the call.
struct FetchContext {
    ApiClient* client = nullptr;
    TokenProvider* provider = nullptr;
    const Clock* clock = nullptr;
    RetryConfig retry{};
    std::stop_token stop{};
};

/// The body of a 2xx answer to `request`, with the token handling described at the top.
[[nodiscard]] AuthResult<std::string> authorized_body(const FetchContext& context,
                                                      const AuthorizedRequest& request) {
    for (int attempt = 1;; ++attempt) {
        // A signal that is already pending sends nothing (the bridge notices it only after its
        // next 50 ms poll).
        if (context.stop.stop_requested() || interrupt_requested()) {
            return std::unexpected(interrupted_failure());
        }
        // Wiped when it goes out of scope at the end of this attempt (~Secret).
        AuthResult<Secret> authorization =
            attempt == 1 ? context.provider->authorization(context.stop)
                         : context.provider->refresh_after_unauthorized(context.stop);
        if (!authorization) {
            return std::unexpected(std::move(authorization.error()));
        }
        Result<HttpResponse> response = context.client->get_authorized(
            request.path, request.offset, *authorization, context.stop);
        if (!response) {
            return std::unexpected(client_failure(std::move(response.error())));
        }
        if (is_success(response->status)) {
            return std::move(response->body);
        }
        if (response->status == 401) {
            if (attempt == 1) {
                continue; // one refresh, one retry of this same request
            }
            return std::unexpected(unauthorized_failure(request.collection));
        }
        return std::unexpected(
            failure(AuthFailureKind::request,
                    status_error(request.url, *response, *context.clock, context.retry)));
    }
}

/// A parse error of an untrusted document, prefixed with its URL (as every API document's).
[[nodiscard]] AuthFailure content_failure(std::string_view url, const Error& error) {
    return failure(AuthFailureKind::content,
                   Error{.code = error.code, .message = std::format("{}: {}", url, error.message)});
}

/// The cap refusal (a self-contained message: no URL, no remedy).
[[nodiscard]] AuthFailure cap_failure() {
    return failure(AuthFailureKind::content,
                   Error{.code = Errc::network, .message = std::string{too_many_follows}});
}

/// A server that pages short (fewer than follows_page_limit per page) with total <= max_follows
/// would need a request past the window MangaDex documents for collection endpoints. Refused,
/// never truncated; the message names only numbers this file computed or validated (`total` is
/// at most max_follows), never response bytes.
[[nodiscard]] AuthFailure window_failure(std::size_t offset, std::size_t total) {
    return failure(AuthFailureKind::content,
                   Error{.code = Errc::network,
                         .message = std::format("MangaDex sent the follows in more pages than "
                                                "expected (offset {} of {}); refusing to continue "
                                                "past the {}-title window",
                                                offset, total, max_follows)});
}

/// Every follows page, in API order (duplicates kept).
[[nodiscard]] AuthResult<std::vector<MangaId>> fetch_follows(const FetchContext& context,
                                                             std::string_view api) {
    std::vector<MangaId> all;
    std::size_t offset = 0;
    std::size_t total = 0; ///< the last page's total (validated <= max_follows)
    while (true) {
        // offset + follows_page_limit > max_follows, written so that neither side can wrap
        // (follows_page_limit <= max_follows is asserted above). Never true for offset 0, so a
        // page (and its total) has always been read when it trips.
        if (offset > max_follows - follows_page_limit) {
            return std::unexpected(window_failure(offset, total));
        }
        const AuthorizedRequest request{
            .path = AuthorizedPath::user_follows_manga,
            .offset = offset,
            .url = follows_request_url(api, offset),
            .collection = Collection::follows,
        };
        const AuthResult<std::string> body = authorized_body(context, request);
        if (!body) {
            return std::unexpected(body.error());
        }
        Result<FollowsPage> page = parse_follows_page(*body);
        if (!page) {
            return std::unexpected(content_failure(request.url, page.error()));
        }
        if (page->total > max_follows) {
            return std::unexpected(cap_failure());
        }
        total = page->total;
        const std::size_t count = page->titles.size(); // at most follows_page_limit (parser)
        all.insert(all.end(), std::make_move_iterator(page->titles.begin()),
                   std::make_move_iterator(page->titles.end()));
        // offset <= max_follows - follows_page_limit and count <= follows_page_limit: no wrap.
        const std::size_t next = offset + count;
        if (count == 0 || next >= total) {
            return all;
        }
        offset = next;
    }
}

/// The one status map.
[[nodiscard]] AuthResult<StatusMap> fetch_statuses(const FetchContext& context,
                                                   std::string_view api) {
    const AuthorizedRequest request{
        .path = AuthorizedPath::manga_status,
        .offset = 0,
        .url = status_map_request_url(api),
        .collection = Collection::statuses,
    };
    const AuthResult<std::string> body = authorized_body(context, request);
    if (!body) {
        return std::unexpected(body.error());
    }
    Result<StatusMap> map = parse_status_map(*body);
    if (!map) {
        return std::unexpected(content_failure(request.url, map.error()));
    }
    return std::move(*map);
}

} // namespace

AuthResult<FollowsCollection> fetch_follows_collection(const ApiBase& api_base,
                                                       const FollowsFetchServices& services,
                                                       TokenProvider& provider,
                                                       bool with_statuses) {
    if (services.transport == nullptr || services.clock == nullptr || !services.jitter) {
        return std::unexpected(failure(
            AuthFailureKind::local,
            Error{.code = Errc::failure,
                  .message = "internal error: fetch_follows_collection needs a transport, a "
                             "clock and a jitter function"}));
    }
    // Before the provider is asked for anything: a custom --api-base never sees a token (the app
    // refuses it earlier; get_authorized guards again).
    if (Result<void> base = check_auth_api_base(api_base, "follows"); !base) {
        return std::unexpected(failure(AuthFailureKind::local, std::move(base.error())));
    }

    // The transport, the clock and the trace sink are the caller's and outlive the call.
    const ClientConfig config{.endpoints = endpoints_for(api_base, /*report=*/false)};
    const std::string api = config.endpoints.api;
    const std::stop_source stop{};
    ApiClient client{*services.transport, *services.clock, config, services.jitter, services.trace};
    const InterruptBridge bridge{stop};

    const FetchContext context{
        .client = &client,
        .provider = &provider,
        .clock = services.clock,
        .retry = config.retry,
        .stop = stop.get_token(),
    };
    AuthResult<std::vector<MangaId>> follows = fetch_follows(context, api);
    if (!follows) {
        return std::unexpected(std::move(follows.error()));
    }
    FollowsCollection collection{.follows = std::move(*follows), .statuses = std::nullopt};
    if (with_statuses && !collection.follows.empty()) {
        AuthResult<StatusMap> statuses = fetch_statuses(context, api);
        if (!statuses) {
            return std::unexpected(std::move(statuses.error()));
        }
        collection.statuses = std::move(*statuses);
    }
    return collection;
}

} // namespace mangadex_dl
