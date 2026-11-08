// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/curl_transport_auth_test.cpp: the curl transport's Authorization header and its
// borrowed POST body (follows plan T4, AC9), against the scripted loopback HTTP server
// (tests/support/loopback_http_server.hpp; 127.0.0.1 only, never the real network). Covers: one
// Authorization header on a GET, none (and no Expect) without one, CR/LF/NUL refused before any
// I/O with the value never echoed, no header leftovers on a reused pooled handle, POST bodies sent
// byte for byte from the borrowed buffer (small, with an embedded NUL, and larger than libcurl's
// upload buffer), an Authorization refused over plain http except to loopback, a 307 returned
// unfollowed with the header sent once, per-thread tokens and borrowed POST bodies on a shared
// handle pool, and FakeTransport recording the authorization. Every token is a fake value.

#include "mangadex_dl/net/curl_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "support/fake_transport.hpp"
#include "support/loopback_http_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace loopback = mangadex_dl::test::loopback;
using Catch::Matchers::ContainsSubstring;
using mangadex_dl::CurlTransportConfig;
using mangadex_dl::HttpMethod;
using mangadex_dl::HttpRequest;
using mangadex_dl::make_curl_transport;
using mangadex_dl::Transport;
using mangadex_dl::TransportErrc;
using mangadex_dl::TransportResult;
using mangadex_dl::test::FakeTransport;
using mangadex_dl::test::RecordedRequest;

constexpr std::string_view user_agent = "mangadex-dl/0.0.0-test";
constexpr std::size_t generous = std::size_t{1} << 20U;
constexpr std::string_view fake_token{"fixture-token-not-a-secret"};

[[nodiscard]] std::unique_ptr<Transport> make_transport() {
    auto transport =
        make_curl_transport(CurlTransportConfig{.user_agent = std::string{user_agent}});
    if (!transport.has_value()) {
        FAIL("make_curl_transport: " << transport.error().message);
    }
    return std::move(*transport);
}

[[nodiscard]] std::string bearer(std::string_view token) { return std::format("Bearer {}", token); }

[[nodiscard]] HttpRequest get(std::string url, std::string authorization = {}) {
    return HttpRequest{
        .url = std::move(url),
        .authorization = std::move(authorization),
        .max_body_bytes = generous,
    };
}

/// For INFO(): what a result was (never a header value).
[[nodiscard]] std::string describe(const TransportResult& result) {
    if (result.has_value()) {
        return std::format("HTTP {} ({} body bytes)", result->status, result->body.size());
    }
    return std::format("error {}: {}", std::to_underlying(result.error().kind),
                       result.error().message);
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// How many headers named `name` (ASCII case-insensitive) the request carried.
[[nodiscard]] std::size_t header_count(const loopback::Request& request, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::count_if(request.headers, [name](const auto& h) {
        return std::ranges::equal(h.first, name, {}, ascii_lower, ascii_lower);
    }));
}

[[nodiscard]] loopback::Response ok_body(std::string body) {
    return loopback::Response{.body = std::move(body)};
}

} // namespace

TEST_CASE("curl auth: a GET with an authorization sends exactly that one header", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("ok"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto result = transport->fetch(get(server.url("/user/follows/manga"), "Bearer abc"));
    INFO(describe(result));
    REQUIRE(result.has_value());
    CHECK(result->status == 200);

    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == "GET");
    CHECK(header_count(requests[0], "Authorization") == 1);
    CHECK(requests[0].header("Authorization") == "Bearer abc");
    // A GET gets no POST-only headers from the shared header list.
    CHECK(!requests[0].header("Expect").has_value());
    CHECK(!requests[0].header("Content-Type").has_value());
    CHECK(requests[0].header("User-Agent") == std::string{user_agent});
}

TEST_CASE("curl auth: a GET without an authorization sends no Authorization and no Expect",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("ok"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto result = transport->fetch(get(server.url("/manga/abc")));
    INFO(describe(result));
    REQUIRE(result.has_value());

    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(header_count(requests[0], "Authorization") == 0);
    CHECK(header_count(requests[0], "Expect") == 0);
    CHECK(header_count(requests[0], "Content-Type") == 0);
}

TEST_CASE("curl auth: CR, LF or NUL in the authorization is local, before any I/O, never echoed",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("ok"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    using namespace std::string_view_literals;
    const std::string token{fake_token};
    const std::array<std::string, 4> bad{
        bearer(token) + "\r\nX-Evil: 1",
        bearer(token) + "\n",
        "\r" + bearer(token),
        std::string{"Bearer fixture-token\0-not-a-secret"sv},
    };
    for (const std::string& authorization : bad) {
        for (const HttpMethod method : {HttpMethod::get, HttpMethod::post}) {
            const auto refused = transport->fetch(HttpRequest{
                .method = method,
                .url = server.url("/x"),
                .body = "a=1",
                .content_type = "application/x-www-form-urlencoded",
                .authorization = authorization,
                .max_body_bytes = generous,
            });
            REQUIRE(!refused.has_value());
            CHECK(refused.error().kind == TransportErrc::local);
            CHECK(refused.error().message ==
                  "invalid Authorization header (contains CR, LF or NUL)");
            CHECK_THAT(refused.error().message, !ContainsSubstring("fixture-token"));
            CHECK_THAT(refused.error().message, !ContainsSubstring("Bearer"));
        }
    }
    CHECK(server.requests().empty());
    CHECK(server.connections_accepted() == 0);
}

TEST_CASE("curl auth: a reused pooled handle keeps no Authorization from the previous fetch",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("ok"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto with = transport->fetch(get(server.url("/with"), bearer(fake_token)));
    INFO(describe(with));
    REQUIRE(with.has_value());
    const auto without = transport->fetch(get(server.url("/without")));
    INFO(describe(without));
    REQUIRE(without.has_value());
    const auto posted = transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = server.url("/post"),
        .body = "x=1",
        .content_type = "application/x-www-form-urlencoded",
        .max_body_bytes = generous,
    });
    INFO(describe(posted));
    REQUIRE(posted.has_value());

    const auto requests = server.requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].header("Authorization") == bearer(fake_token));
    CHECK(header_count(requests[1], "Authorization") == 0);
    CHECK(header_count(requests[2], "Authorization") == 0);
    CHECK(server.connections_accepted() == 1); // one handle, reset and reused
}

TEST_CASE("curl auth: a POST sends the borrowed body byte for byte with its Content-Type",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("{}"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    SECTION("a form body with & = % and an embedded NUL") {
        using namespace std::string_view_literals;
        const std::string body{"grant_type=password&username=a%26b&password=p%3D%25\0x"sv};
        const auto posted = transport->fetch(HttpRequest{
            .method = HttpMethod::post,
            .url = server.url("/protocol/openid-connect/token"),
            .body = body,
            .content_type = "application/x-www-form-urlencoded",
            .max_body_bytes = generous,
        });
        INFO(describe(posted));
        REQUIRE(posted.has_value());
        CHECK(posted->body == "{}");

        const auto requests = server.requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].method == "POST");
        CHECK(requests[0].body == body);
        CHECK(requests[0].header("Content-Length") == std::to_string(body.size()));
        CHECK(requests[0].header("Content-Type") == "application/x-www-form-urlencoded");
        CHECK(header_count(requests[0], "Authorization") == 0);
        CHECK(header_count(requests[0], "Expect") == 0);
    }

    SECTION("a body larger than libcurl's upload buffer, with an Authorization") {
        std::string body((std::size_t{256} * 1024) + 7, '\0');
        for (std::size_t i = 0; i < body.size(); ++i) {
            body[i] = static_cast<char>('a' + static_cast<char>(i % 26));
        }
        const auto posted = transport->fetch(HttpRequest{
            .method = HttpMethod::post,
            .url = server.url("/report"),
            .body = body,
            .content_type = "application/octet-stream",
            .authorization = bearer(fake_token),
            .max_body_bytes = generous,
        });
        INFO(describe(posted));
        REQUIRE(posted.has_value());

        const auto requests = server.requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].body == body);
        CHECK(requests[0].header("Content-Type") == "application/octet-stream");
        CHECK(header_count(requests[0], "Authorization") == 1);
        CHECK(requests[0].header("Authorization") == bearer(fake_token));
        CHECK(header_count(requests[0], "Expect") == 0);
    }

    SECTION("an empty body is an empty POST") {
        const auto posted = transport->fetch(HttpRequest{
            .method = HttpMethod::post,
            .url = server.url("/empty"),
            .max_body_bytes = generous,
        });
        INFO(describe(posted));
        REQUIRE(posted.has_value());

        const auto requests = server.requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].method == "POST");
        CHECK(requests[0].body.empty());
        CHECK(requests[0].header("Content-Length") == "0");
    }
}

TEST_CASE("curl auth: an Authorization goes only over https or plain http to loopback",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("ok"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();
    constexpr std::string_view refusal = "refusing to send an Authorization header over plain "
                                         "http (only https, or http to 127.0.0.1, localhost or "
                                         "[::1])";

    SECTION("a non-loopback http host is refused before any I/O, the value never echoed") {
        const std::string port = std::format(":{}", server.port());
        // The last ones look like loopback but are not: user information, a longer host name, a
        // junk suffix after an IPv6 literal, another scheme.
        for (const std::string& url : {
                 std::string{"http://example.invalid/user/follows/manga"},
                 std::string{"HTTP://example.invalid"},
                 "http://127.0.0.1@example.invalid" + port + "/x",
                 "http://127.0.0.1.example.invalid" + port + "/x",
                 std::string{"http://localhost.example.invalid/x"},
                 std::string{"http://[::1]x/"},
                 std::string{"ftp://127.0.0.1/x"},
             }) {
            for (const HttpMethod method : {HttpMethod::get, HttpMethod::post}) {
                INFO(url);
                const auto refused = transport->fetch(HttpRequest{
                    .method = method,
                    .url = url,
                    .body = "a=1",
                    .authorization = bearer(fake_token),
                    .max_body_bytes = generous,
                });
                REQUIRE(!refused.has_value());
                CHECK(refused.error().kind == TransportErrc::local);
                CHECK(refused.error().message == refusal);
                CHECK_THAT(refused.error().message, !ContainsSubstring(std::string{fake_token}));
            }
        }
        CHECK(server.requests().empty());
        CHECK(server.connections_accepted() == 0);
    }

    SECTION("plain http to 127.0.0.1 (any case, with a port) still sends it") {
        const std::string url =
            std::format("HTTP://127.0.0.1:{}/user/follows/manga?limit=1", server.port());
        const auto sent = transport->fetch(get(url, bearer(fake_token)));
        INFO(describe(sent));
        REQUIRE(sent.has_value());
        CHECK(sent->status == 200);
        const auto requests = server.requests();
        REQUIRE(requests.size() == 1);
        CHECK(header_count(requests[0], "Authorization") == 1);
        CHECK(requests[0].header("Authorization") == bearer(fake_token));
    }
}

TEST_CASE("curl auth: a 307 is returned, never followed, and the header is sent once",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/moved") {
            return loopback::Response{.status = 307,
                                      .reason = "Temporary Redirect",
                                      .headers = {{"Location", "/elsewhere"}}};
        }
        return ok_body("elsewhere");
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto moved = transport->fetch(get(server.url("/moved"), bearer(fake_token)));
    INFO(describe(moved));
    REQUIRE(moved.has_value());
    CHECK(moved->status == 307);

    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].target == "/moved");
    CHECK(header_count(requests[0], "Authorization") == 1);
}

TEST_CASE("curl auth: a transport error never carries the authorization value", "[net][curl]") {
    const auto port = loopback::unused_port();
    REQUIRE(port != 0);
    const auto transport = make_transport();

    const auto refused =
        transport->fetch(get(std::format("http://127.0.0.1:{}/x", port), bearer(fake_token)));
    INFO(describe(refused));
    REQUIRE(!refused.has_value());
    CHECK(refused.error().kind == TransportErrc::connect);
    CHECK_THAT(refused.error().message, !ContainsSubstring(std::string{fake_token}));
}

TEST_CASE("curl auth: 4 threads with their own tokens and bodies on one handle pool never mix them",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("ok"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    constexpr std::size_t threads = 4;
    constexpr std::size_t rounds = 12;
    /// What thread `t` sends in round `i`. Every third round is a POST with a token and a heap
    /// body (well past the SSO size), so borrowed POSTFIELDS meet the shared pool across threads.
    /// Of the GETs, every other one has no token, so a header left over on a pooled handle would
    /// show up.
    struct Planned {
        HttpMethod method = HttpMethod::get;
        std::string authorization{};
        std::string body{};
    };
    const auto plan_for = [](std::size_t t, std::size_t i) {
        if (i % 3 == 2) {
            return Planned{
                .method = HttpMethod::post,
                .authorization = bearer(std::format("token-{}", t)),
                .body = std::format("thread={}&round={}&pad={}", t, i, std::string(64, 'p')),
            };
        }
        return Planned{.authorization =
                           i % 2 == 0 ? bearer(std::format("token-{}", t)) : std::string{}};
    };
    std::array<int, threads> failures{};
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            // `transport`, `server` and `failures` outlive the threads: they are joined at the end
            // of this block.
            workers.emplace_back(
                [tr = transport.get(), srv = &server, slot = &failures.at(t), t, plan_for] {
                    for (std::size_t i = 0; i < rounds; ++i) {
                        Planned planned = plan_for(t, i);
                        const auto result = tr->fetch(HttpRequest{
                            .method = planned.method,
                            .url = srv->url(std::format("/t/{}/{}", t, i)),
                            .body = std::move(planned.body),
                            .content_type = planned.method == HttpMethod::post
                                                ? "application/x-www-form-urlencoded"
                                                : "",
                            .authorization = std::move(planned.authorization),
                            .max_body_bytes = generous,
                        });
                        if (!result.has_value() || result->status != 200) {
                            ++*slot;
                        }
                    }
                });
        }
    }
    CHECK(failures == std::array<int, threads>{});

    const auto requests = server.requests();
    REQUIRE(requests.size() == threads * rounds);
    std::size_t posts = 0;
    for (std::size_t t = 0; t < threads; ++t) {
        for (std::size_t i = 0; i < rounds; ++i) {
            const std::string target = std::format("/t/{}/{}", t, i);
            INFO(target);
            const auto found = std::ranges::find(requests, target, &loopback::Request::target);
            REQUIRE(found != requests.end());
            const Planned expected = plan_for(t, i);
            if (expected.method == HttpMethod::post) {
                ++posts;
                CHECK(found->method == "POST");
                CHECK(found->body == expected.body);
                CHECK(found->header("Content-Type") == "application/x-www-form-urlencoded");
            } else {
                CHECK(found->method == "GET");
                CHECK(found->body.empty());
            }
            if (expected.authorization.empty()) {
                CHECK(header_count(*found, "Authorization") == 0);
            } else {
                CHECK(header_count(*found, "Authorization") == 1);
                CHECK(found->header("Authorization") == expected.authorization);
            }
        }
    }
    CHECK(posts == threads * (rounds / 3));
}

TEST_CASE("curl auth: FakeTransport records the authorization", "[net][fake-transport]") {
    FakeTransport transport;
    transport.push_status("https://api.example/user/follows/manga", 200, "{}");
    transport.push_status("https://api.example/manga", 200, "{}");

    const auto authorized = transport.fetch(HttpRequest{
        .url = "https://api.example/user/follows/manga",
        .authorization = bearer(fake_token),
        .max_body_bytes = 16,
    });
    REQUIRE(authorized.has_value());
    const auto anonymous = transport.fetch(HttpRequest{
        .url = "https://api.example/manga",
        .max_body_bytes = 16,
    });
    REQUIRE(anonymous.has_value());

    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0] == RecordedRequest{.url = "https://api.example/user/follows/manga",
                                         .max_body_bytes = 16,
                                         .authorization = bearer(fake_token)});
    CHECK(requests[1] == RecordedRequest{.url = "https://api.example/manga", .max_body_bytes = 16});
    CHECK(requests[1].authorization.empty());
    CHECK(transport.drained());
}
