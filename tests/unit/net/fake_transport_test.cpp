// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/fake_transport_test.cpp: the scripted test transport itself
// (tests/support/fake_transport.hpp). Later tracks (ApiClient, downloader) trust it, so it gets
// its own checks: per-URL FIFO order, fallback and unscripted URLs, cancellation, the body cap,
// the on_fetch hook (including one that pushes), the recording accessors, and concurrent
// fetch + push from 4 threads (also run under TSan with `pixi run tsan -L net`).

#include "mangadex_dl/net/transport.hpp"
#include "support/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using mangadex_dl::HttpMethod;
using mangadex_dl::HttpRequest;
using mangadex_dl::HttpResponse;
using mangadex_dl::TransportErrc;
using mangadex_dl::TransportError;
using mangadex_dl::TransportResult;
using mangadex_dl::test::FakeTransport;
using mangadex_dl::test::RecordedRequest;

constexpr std::size_t generous = std::size_t{1} << 20U;
// Plain literals: no static std::string whose construction could throw before main.
constexpr const char* url_a = "https://api.example/a";
constexpr const char* url_b = "https://api.example/b";

[[nodiscard]] TransportResult get(FakeTransport& transport, const std::string& url,
                                  std::size_t max_body_bytes = generous) {
    return transport.fetch(HttpRequest{.url = url, .max_body_bytes = max_body_bytes});
}

/// The status of a response; -1 for a transport error (so CHECKs stay one-liners).
[[nodiscard]] int status_of(const TransportResult& result) {
    return result.has_value() ? result->status : -1;
}

[[nodiscard]] std::optional<TransportErrc> error_of(const TransportResult& result) {
    if (result.has_value()) {
        return std::nullopt;
    }
    return result.error().kind;
}

} // namespace

TEST_CASE("FakeTransport: each URL answers from its own FIFO queue", "[net][fake-transport]") {
    FakeTransport transport;
    transport.push_status(url_a, 503);
    transport.push_status(url_b, 404);
    transport.push_status(url_a, 200, "second");
    transport.push_error(url_a, TransportErrc::timeout, "slow");

    CHECK(status_of(get(transport, url_a)) == 503);
    const TransportResult second = get(transport, url_a);
    REQUIRE(second.has_value());
    CHECK(second->status == 200);
    CHECK(second->body == "second");
    CHECK(status_of(get(transport, url_b)) == 404);
    const TransportResult third = get(transport, url_a);
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error() == TransportError{.kind = TransportErrc::timeout, .message = "slow"});
}

TEST_CASE("FakeTransport: push keeps every header field of a scripted response",
          "[net][fake-transport]") {
    FakeTransport transport;
    const HttpResponse scripted{
        .status = 429,
        .body = "{}",
        .retry_after = "7",
        .x_ratelimit_retry_after = "1700000120",
        .x_cache = "HIT",
    };
    transport.push(url_a, scripted);
    const TransportResult result = get(transport, url_a);
    REQUIRE(result.has_value());
    CHECK(*result == scripted);
}

TEST_CASE("FakeTransport: unscripted URLs are a local error, or the fallback",
          "[net][fake-transport]") {
    FakeTransport transport;
    const TransportResult unscripted = get(transport, url_a);
    REQUIRE_FALSE(unscripted.has_value());
    CHECK(unscripted.error().kind == TransportErrc::local);
    CHECK(unscripted.error().message.contains(url_a));

    transport.set_fallback(HttpResponse{.status = 404});
    CHECK(status_of(get(transport, url_a)) == 404);
    CHECK(status_of(get(transport, url_b)) == 404); // the fallback is never consumed

    // A queued answer still comes first; the fallback covers the URL again once it is used up.
    transport.push_status(url_a, 200);
    CHECK(status_of(get(transport, url_a)) == 200);
    CHECK(status_of(get(transport, url_a)) == 404);
}

TEST_CASE("FakeTransport: a stopped request is cancelled and consumes nothing",
          "[net][fake-transport]") {
    FakeTransport transport;
    transport.push_status(url_a, 200, "kept");
    const std::stop_source source;
    source.request_stop();
    const TransportResult cancelled = transport.fetch(
        HttpRequest{.url = url_a, .max_body_bytes = generous, .cancel = source.get_token()});
    CHECK(error_of(cancelled) == TransportErrc::cancelled);
    CHECK_FALSE(transport.drained());
    CHECK(transport.count(url_a) == 1); // still recorded

    const TransportResult kept = get(transport, url_a);
    REQUIRE(kept.has_value());
    CHECK(kept->body == "kept");
    CHECK(transport.drained());
}

TEST_CASE("FakeTransport: bodies over max_body_bytes are too_large", "[net][fake-transport]") {
    FakeTransport transport;
    transport.push_status(url_a, 200, "12345");
    transport.push_status(url_a, 200, "12345");
    transport.push_status(url_a, 200, "12345");
    transport.push_status(url_a, 200, "");
    transport.push_status(url_a, 200, "x");

    CHECK(error_of(get(transport, url_a, 4)) == TransportErrc::too_large);
    const TransportResult exact = get(transport, url_a, 5);
    REQUIRE(exact.has_value());
    CHECK(exact->body == "12345");
    CHECK(error_of(get(transport, url_a, 0)) == TransportErrc::too_large);
    // 0 refuses any non-empty body, but an empty one is fine.
    CHECK(status_of(get(transport, url_a, 0)) == 200);
    CHECK(error_of(get(transport, url_a, 0)) == TransportErrc::too_large);
    CHECK(transport.drained()); // a too_large answer still consumed its queue entry
}

TEST_CASE("FakeTransport: the on_fetch hook sees each request before it is answered",
          "[net][fake-transport]") {
    std::vector<RecordedRequest> seen; // declared before `transport`, so it outlives the hook
    FakeTransport transport;
    transport.set_on_fetch([out = &seen](const HttpRequest& request) {
        out->push_back(RecordedRequest{
            .method = request.method,
            .url = request.url,
            .body = request.body,
            .content_type = request.content_type,
            .max_body_bytes = request.max_body_bytes,
        });
    });
    transport.push_status(url_b, 204);
    const TransportResult result = transport.fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = url_b,
        .body = R"({"ok":true})",
        .content_type = "application/json",
        .max_body_bytes = 10,
    });
    CHECK(status_of(result) == 204);
    const RecordedRequest expected{
        .method = HttpMethod::post,
        .url = url_b,
        .body = R"({"ok":true})",
        .content_type = "application/json",
        .max_body_bytes = 10,
    };
    REQUIRE(seen.size() == 1);
    CHECK(seen.front() == expected);
    CHECK(transport.requests() == std::vector<RecordedRequest>{expected});
}

TEST_CASE("FakeTransport: a hook may push (it runs outside the lock)", "[net][fake-transport]") {
    FakeTransport transport;
    // `transport` outlives its own hook (the hook is part of it).
    transport.set_on_fetch([self = &transport](const HttpRequest& request) {
        self->push_status(request.url, 201, "from the hook");
        static_cast<void>(self->count(request.url)); // other accessors are fine too
    });
    const TransportResult result = get(transport, url_a);
    REQUIRE(result.has_value());
    CHECK(result->status == 201);
    CHECK(result->body == "from the hook");
    CHECK(transport.drained());
}

TEST_CASE("FakeTransport: a hook may request the stop that cancels its request",
          "[net][fake-transport]") {
    std::stop_source source; // declared before `transport`, so it outlives the hook
    FakeTransport transport;
    transport.push_status(url_a, 200);
    transport.set_on_fetch([src = &source](const HttpRequest&) { src->request_stop(); });
    const TransportResult result = transport.fetch(
        HttpRequest{.url = url_a, .max_body_bytes = generous, .cancel = source.get_token()});
    CHECK(error_of(result) == TransportErrc::cancelled);
    CHECK_FALSE(transport.drained());
}

TEST_CASE("FakeTransport: requests(), count() and drained()", "[net][fake-transport]") {
    FakeTransport transport;
    CHECK(transport.drained());
    CHECK(transport.requests().empty());
    CHECK(transport.count(url_a) == 0);

    transport.push_status(url_a, 200);
    transport.push_status(url_b, 200);
    CHECK_FALSE(transport.drained());
    CHECK(status_of(get(transport, url_a, 7)) == 200);
    CHECK_FALSE(transport.drained());
    CHECK(status_of(get(transport, url_b)) == 200);
    CHECK(transport.drained());
    CHECK(error_of(get(transport, url_a)) == TransportErrc::local); // unscripted, still recorded

    CHECK(transport.count(url_a) == 2);
    CHECK(transport.count(url_b) == 1);
    CHECK(transport.count("https://api.example/other") == 0);
    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0] == RecordedRequest{.url = url_a, .max_body_bytes = 7});
    CHECK(requests[1].url == url_b);
    CHECK(requests[2].url == url_a);
    CHECK(requests[0].method == HttpMethod::get);
}

TEST_CASE("FakeTransport: 4 threads push and fetch concurrently", "[net][fake-transport]") {
    constexpr int threads = 4;
    constexpr int rounds = 50;
    const std::string shared = "https://api.example/shared";
    FakeTransport transport;
    std::vector<int> failures(threads, 0);
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            // `transport`, `shared` and `failures` outlive the jthreads (joined at the end of this
            // block); each worker writes only its own `failures` slot.
            workers.emplace_back(
                [tr = &transport, sh = &shared, out = &failures[static_cast<std::size_t>(t)], t] {
                    const std::string own = "https://api.example/own-" + std::to_string(t);
                    for (int i = 0; i < rounds; ++i) {
                        // Own URL: FIFO order per thread.
                        tr->push_status(own, 200, std::to_string(i));
                        const TransportResult mine = get(*tr, own);
                        if (!mine.has_value() || mine->body != std::to_string(i)) {
                            ++*out;
                        }
                        // Shared URL: this thread pushed before fetching, so the queue is never
                        // empty here (pushes so far >= fetches so far + 1).
                        tr->push_status(*sh, 200, "shared");
                        const TransportResult other = get(*tr, *sh);
                        if (!other.has_value() || other->body != "shared") {
                            ++*out;
                        }
                        // Readers race with the writers.
                        static_cast<void>(tr->count(*sh));
                        static_cast<void>(tr->drained());
                        static_cast<void>(tr->requests().size());
                    }
                });
        }
    }
    CHECK(failures == std::vector<int>(threads, 0));
    CHECK(transport.count(shared) == static_cast<std::size_t>(threads * rounds));
    CHECK(transport.requests().size() == static_cast<std::size_t>(2 * threads * rounds));
    CHECK(transport.drained());
}
