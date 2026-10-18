// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/curl_transport_test.cpp: the dlopen()ed libcurl transport against a scripted
// loopback HTTP server (tests/support/loopback_http_server.hpp; 127.0.0.1 only, never the real
// network). Covers status passthrough, header capture, the User-Agent and POST body, the body cap
// (Content-Length, chunked, after content decoding, and max_body_bytes == 0), stall timeouts,
// cancellation, refused and dropped connections, redirects not followed, refused protocols,
// handle-pool reuse (sequential and 4 threads x 20 fetches, also under TSan), the runtime version
// gate on fake curl_version_info structs, the SIGPIPE guard, the overall deadline, the shared
// CurlApi across coexisting and concurrently created transports, and a burst larger than the pool.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/curl_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "net/curl_api.hpp"
#include "support/loopback_http_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <pthread.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): sigset_t, sigpending and pthread_kill are POSIX
#include <signal.h>

namespace {

namespace loopback = mangadex_dl::test::loopback;
using Catch::Matchers::ContainsSubstring;
using mangadex_dl::CurlTransportConfig;
using mangadex_dl::Errc;
using mangadex_dl::HttpMethod;
using mangadex_dl::HttpRequest;
using mangadex_dl::make_curl_transport;
using mangadex_dl::Transport;
using mangadex_dl::TransportErrc;
using mangadex_dl::TransportResult;
using mangadex_dl::detail::check_runtime;
using mangadex_dl::detail::curl_max_pooled_handles;
using mangadex_dl::detail::curl_min_version_num;
using mangadex_dl::detail::CurlApi;
using mangadex_dl::detail::make_curl_transport_with_deadline;
using mangadex_dl::detail::SigpipeGuard;

constexpr std::string_view user_agent = "mangadex-dl/0.0.0-test";
constexpr std::size_t generous = std::size_t{1} << 20U;

[[nodiscard]] std::unique_ptr<Transport> make_transport() {
    auto transport =
        make_curl_transport(CurlTransportConfig{.user_agent = std::string{user_agent}});
    if (!transport.has_value()) {
        FAIL("make_curl_transport: " << transport.error().message);
    }
    return std::move(*transport);
}

[[nodiscard]] HttpRequest get(std::string url, std::size_t max_body_bytes = generous) {
    return HttpRequest{.url = std::move(url), .max_body_bytes = max_body_bytes};
}

/// For INFO(): what a result was.
[[nodiscard]] std::string describe(const TransportResult& result) {
    if (result.has_value()) {
        return std::format("HTTP {} ({} body bytes)", result->status, result->body.size());
    }
    return std::format("error {}: {}", std::to_underlying(result.error().kind),
                       result.error().message);
}

[[nodiscard]] loopback::Response ok_body(std::string body) {
    return loopback::Response{.body = std::move(body)};
}

/// A zlib stream (RFC 1950/1951) that inflates to `1 + 258 * matches` copies of `c`: one literal,
/// then `matches` back-references of length 258 at distance 1, in one fixed-Huffman block. About
/// 13 bits per 258 output bytes, so a small Content-Length decodes to a large body.
class DeflateBits {
public:
    /// Header fields: least significant bit first.
    void bits(std::uint32_t value, unsigned int count) {
        for (unsigned int i = 0; i < count; ++i) {
            push((value >> i) & 1U);
        }
    }
    /// Huffman codes: most significant bit first.
    void code(std::uint32_t value, unsigned int length) {
        for (unsigned int i = length; i > 0; --i) {
            push((value >> (i - 1)) & 1U);
        }
    }
    [[nodiscard]] std::string finish() {
        if (used_ > 0U) {
            out_.push_back(static_cast<char>(current_));
        }
        return out_;
    }

private:
    void push(std::uint32_t bit) {
        current_ |= bit << used_;
        if (++used_ == 8U) {
            out_.push_back(static_cast<char>(current_));
            current_ = 0;
            used_ = 0;
        }
    }

    std::string out_{};
    std::uint32_t current_ = 0;
    unsigned int used_ = 0;
};

[[nodiscard]] std::string zlib_run(char c, std::size_t matches) {
    REQUIRE(static_cast<unsigned char>(c) < 144); // an 8-bit fixed literal code
    DeflateBits deflate;
    deflate.bits(1, 1); // BFINAL
    deflate.bits(1, 2); // BTYPE = fixed Huffman
    deflate.code(0x30U + static_cast<unsigned char>(c), 8);
    for (std::size_t i = 0; i < matches; ++i) {
        deflate.code(0xC5, 8); // length symbol 285 = 258, no extra bits
        deflate.code(0, 5);    // distance code 0 = 1, no extra bits
    }
    deflate.code(0, 7); // end of block (256)

    std::uint32_t a = 1;
    std::uint32_t b = 0;
    const std::size_t total = 1 + (258 * matches);
    for (std::size_t i = 0; i < total; ++i) {
        a = (a + static_cast<unsigned char>(c)) % 65521U;
        b = (b + a) % 65521U;
    }
    const std::uint32_t adler = (b << 16U) | a;
    std::string out = "\x78\x01"; // CMF/FLG: deflate, 32 KiB window, no dictionary
    out += deflate.finish();
    for (const unsigned int shift : {24U, 16U, 8U, 0U}) {
        out.push_back(static_cast<char>((adler >> shift) & 0xFFU));
    }
    return out;
}

[[nodiscard]] curl_version_info_data runtime_info(unsigned int version,
                                                  const char* const* protocols) {
    curl_version_info_data info{};
    info.age = CURLVERSION_TENTH;
    info.version = "test";
    info.version_num = version;
    info.features = CURL_VERSION_SSL;
    info.protocols = protocols;
    return info;
}

[[nodiscard]] bool sigpipe_pending() {
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t pending{};
    REQUIRE(sigpending(&pending) == 0);
    return sigismember(&pending, SIGPIPE) == 1;
}

[[nodiscard]] bool sigpipe_blocked() {
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t mask{};
    REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0);
    return sigismember(&mask, SIGPIPE) == 1;
}

} // namespace

TEST_CASE("curl runtime: 7.85.0 and newer with TLS and https pass the gate", "[net][curl]") {
    constexpr std::array<const char*, 3> protocols{"http", "https", nullptr};
    CHECK(check_runtime(runtime_info(curl_min_version_num, protocols.data())).has_value());
    CHECK(check_runtime(runtime_info(0x081700, protocols.data())).has_value());
}

TEST_CASE("curl runtime: old, TLS-less or https-less builds are refused", "[net][curl]") {
    constexpr std::array<const char*, 3> protocols{"http", "https", nullptr};
    constexpr std::array<const char*, 2> http_only{"http", nullptr};

    const auto old = check_runtime(runtime_info(0x0754FF, protocols.data()));
    REQUIRE(!old.has_value());
    CHECK(old.error().code == Errc::failure);
    CHECK_THAT(old.error().message, ContainsSubstring("7.84.255") && ContainsSubstring("7.85.0"));

    auto no_tls = runtime_info(0x080000, protocols.data());
    no_tls.features = 0;
    const auto tls = check_runtime(no_tls);
    REQUIRE(!tls.has_value());
    CHECK_THAT(tls.error().message, ContainsSubstring("TLS"));

    const auto http = check_runtime(runtime_info(0x080000, http_only.data()));
    REQUIRE(!http.has_value());
    CHECK_THAT(http.error().message, ContainsSubstring("https"));

    CHECK(!check_runtime(runtime_info(0x080000, nullptr)).has_value());
}

TEST_CASE("curl transport: SigpipeGuard consumes a SIGPIPE and restores the mask", "[net][curl]") {
    REQUIRE(!sigpipe_blocked()); // Catch2's main thread starts with the default mask
    {
        const SigpipeGuard guard;
        CHECK(sigpipe_blocked());
        // Without the guard this would kill the test process.
        REQUIRE(pthread_kill(pthread_self(), SIGPIPE) == 0);
        CHECK(sigpipe_pending());
    }
    CHECK(!sigpipe_pending());
    CHECK(!sigpipe_blocked());

    SECTION("nested: an outer block survives the inner guard") {
        const SigpipeGuard outer;
        {
            const SigpipeGuard inner;
            REQUIRE(pthread_kill(pthread_self(), SIGPIPE) == 0);
        }
        CHECK(sigpipe_blocked());
        CHECK(!sigpipe_pending());
    }
}

TEST_CASE("curl transport: make_curl_transport refuses a header-breaking User-Agent",
          "[net][curl]") {
    for (const std::string_view bad :
         {std::string_view{""}, std::string_view{"a\r\nX-Evil: 1"}, std::string_view{"a\0b", 3}}) {
        const auto transport =
            make_curl_transport(CurlTransportConfig{.user_agent = std::string{bad}});
        REQUIRE(!transport.has_value());
        CHECK(transport.error().code == Errc::failure);
    }
}

TEST_CASE("curl transport: 200 with body, User-Agent and Accept-Encoding sent", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) { return ok_body("hello, world"); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto result = transport->fetch(get(server.url("/manga/abc?x=1")));
    INFO(describe(result));
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    CHECK(result->body == "hello, world");
    CHECK(result->retry_after.empty());
    CHECK(result->x_cache.empty());
    CHECK(result->elapsed >= std::chrono::milliseconds{0});

    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].target == "/manga/abc?x=1");
    CHECK(requests[0].header("User-Agent") == std::string{user_agent});
    CHECK(requests[0].header("Accept-Encoding").has_value());
    CHECK(!requests[0].header("Content-Type").has_value());
}

TEST_CASE("curl transport: 4xx and 5xx statuses are responses, not errors", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/missing") {
            return loopback::Response{.status = 404, .reason = "Not Found", .body = "nope"};
        }
        return loopback::Response{.status = 503, .reason = "Service Unavailable", .body = "busy"};
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto missing = transport->fetch(get(server.url("/missing")));
    INFO(describe(missing));
    REQUIRE(missing.has_value());
    CHECK(missing->status == 404);
    CHECK(missing->body == "nope");

    const auto busy = transport->fetch(get(server.url("/busy")));
    INFO(describe(busy));
    REQUIRE(busy.has_value());
    CHECK(busy->status == 503);
    CHECK(busy->body == "busy");
    CHECK(server.requests().size() == 2); // no retries at this level
}

TEST_CASE("curl transport: rate-limit and cache headers are captured", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/oversized") {
            return loopback::Response{
                .status = 429,
                .reason = "Too Many Requests",
                .headers = {{"Retry-After", std::string(300, '9')}, {"X-Cache", "HIT"}},
            };
        }
        return loopback::Response{
            .status = 429,
            .reason = "Too Many Requests",
            .headers = {{"retry-after", "120"},
                        {"X-RateLimit-Retry-After", " 1700000000 "},
                        {"X-CACHE", "HIT from node-1"},
                        {"X-Other", "ignored"}},
        };
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto limited = transport->fetch(get(server.url("/limited")));
    INFO(describe(limited));
    REQUIRE(limited.has_value());
    CHECK(limited->status == 429);
    CHECK(limited->retry_after == "120");
    CHECK(limited->x_ratelimit_retry_after == "1700000000");
    CHECK(limited->x_cache == "HIT from node-1");

    // A value over the copy cap is dropped, never truncated into a different number.
    const auto oversized = transport->fetch(get(server.url("/oversized")));
    INFO(describe(oversized));
    REQUIRE(oversized.has_value());
    CHECK(oversized->retry_after.empty());
    CHECK(oversized->x_cache == "HIT");
    CHECK(oversized->x_ratelimit_retry_after.empty()); // nothing leaks from the previous fetch
}

TEST_CASE("curl transport: POST sends the exact body and Content-Type", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) {
        return loopback::Response{.status = 204, .reason = "No Content"};
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const std::string body{"{\"url\":\"x\"}\0tail", 16}; // an embedded NUL: sized, not strlen
    const auto posted = transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = server.url("/report"),
        .body = body,
        .content_type = "application/json",
        .max_body_bytes = generous,
    });
    INFO(describe(posted));
    REQUIRE(posted.has_value());
    CHECK(posted->status == 204);
    CHECK(posted->body.empty());

    // The same pooled handle, reset: a following GET carries no POST leftovers.
    const auto again = transport->fetch(get(server.url("/after")));
    INFO(describe(again));
    REQUIRE(again.has_value());

    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].method == "POST");
    CHECK(requests[0].body == body);
    CHECK(requests[0].header("Content-Type") == "application/json");
    CHECK(!requests[0].header("Expect").has_value());
    CHECK(requests[1].method == "GET");
    CHECK(requests[1].body.empty());
    CHECK(!requests[1].header("Content-Type").has_value());
    CHECK(server.connections_accepted() == 1); // keep-alive survived curl_easy_reset
}

TEST_CASE("curl transport: a Content-Type with CR/LF and a URL with NUL are local errors",
          "[net][curl]") {
    const auto transport = make_transport();
    const auto injected = transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = "http://127.0.0.1:9/",
        .content_type = "text/plain\r\nX-Evil: 1",
        .max_body_bytes = generous,
    });
    REQUIRE(!injected.has_value());
    CHECK(injected.error().kind == TransportErrc::local);

    const auto nul = transport->fetch(get(std::string{"http://127.0.0.1:9/\0x", 21}));
    REQUIRE(!nul.has_value());
    CHECK(nul.error().kind == TransportErrc::local);
}

TEST_CASE("curl transport: the body cap is exact, by Content-Length and by chunks", "[net][curl]") {
    const std::string kib(1024, 'k');
    const loopback::Server server{[kib](const loopback::Request& request) {
        if (request.target == "/chunked") {
            return loopback::Response{
                .body = kib, .framing = loopback::Framing::chunked, .drip_bytes = 100};
        }
        if (request.target == "/large") {
            return ok_body(std::string(generous, 'L'));
        }
        return ok_body(kib);
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    SECTION("exactly the cap is accepted") {
        const auto fits = transport->fetch(get(server.url("/fixed"), 1024));
        INFO(describe(fits));
        REQUIRE(fits.has_value());
        CHECK(fits->body == kib);
        const auto chunked = transport->fetch(get(server.url("/chunked"), 1024));
        INFO(describe(chunked));
        REQUIRE(chunked.has_value());
        CHECK(chunked->body == kib);
    }
    SECTION("one byte over, announced by Content-Length") {
        const auto over = transport->fetch(get(server.url("/fixed"), 1023));
        INFO(describe(over));
        REQUIRE(!over.has_value());
        CHECK(over.error().kind == TransportErrc::too_large);
        CHECK_THAT(over.error().message, ContainsSubstring("1023"));
    }
    SECTION("one byte over, chunked (no length to check up front)") {
        const auto over = transport->fetch(get(server.url("/chunked"), 1023));
        INFO(describe(over));
        REQUIRE(!over.has_value());
        CHECK(over.error().kind == TransportErrc::too_large);
    }
    SECTION("a much larger body is cut off") {
        const auto over = transport->fetch(get(server.url("/large"), 4096));
        INFO(describe(over));
        REQUIRE(!over.has_value());
        CHECK(over.error().kind == TransportErrc::too_large);
    }
    SECTION("the transport stays usable after an aborted transfer") {
        REQUIRE(!transport->fetch(get(server.url("/chunked"), 10)).has_value());
        const auto next = transport->fetch(get(server.url("/fixed"), 1024));
        INFO(describe(next));
        REQUIRE(next.has_value());
        CHECK(next->body == kib);
    }
}

TEST_CASE("curl transport: max_body_bytes == 0 refuses any body", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/empty") {
            return loopback::Response{.status = 204, .reason = "No Content"};
        }
        if (request.target == "/chunked") {
            return loopback::Response{.body = "x", .framing = loopback::Framing::chunked};
        }
        return ok_body("x");
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    for (const char* path : {"/one", "/chunked"}) {
        const auto refused = transport->fetch(get(server.url(path), 0));
        INFO(path << ": " << describe(refused));
        REQUIRE(!refused.has_value());
        CHECK(refused.error().kind == TransportErrc::too_large);
    }
    const auto empty = transport->fetch(get(server.url("/empty"), 0));
    INFO(describe(empty));
    REQUIRE(empty.has_value());
    CHECK(empty->status == 204);
    CHECK(empty->body.empty());
}

TEST_CASE("curl transport: the cap applies to the decoded body", "[net][curl]") {
    // 1 + 258 * 1024 = 264193 decoded bytes from about 1.7 KB on the wire.
    const std::string compressed = zlib_run('a', 1024);
    const std::size_t decoded = 1 + (258 * std::size_t{1024});
    REQUIRE(compressed.size() < 4096);
    const loopback::Server server{[compressed](const loopback::Request& request) {
        loopback::Response response{
            .headers = {{"Content-Encoding", "deflate"}},
            .body = compressed,
        };
        if (request.target == "/chunked") {
            response.framing = loopback::Framing::chunked;
            response.drip_bytes = 256;
        }
        return response;
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    SECTION("control: under the cap it decodes to the full run") {
        const auto fits = transport->fetch(get(server.url("/fixed"), decoded));
        INFO(describe(fits));
        REQUIRE(fits.has_value());
        CHECK(fits->body == std::string(decoded, 'a'));
        const auto requests = server.requests();
        REQUIRE(requests.size() == 1);
        CHECK_THAT(requests[0].header("Accept-Encoding").value_or(""),
                   ContainsSubstring("deflate"));
    }
    for (const char* path : {"/fixed", "/chunked"}) {
        DYNAMIC_SECTION("over the cap after decoding: " << path) {
            // Content-Length (compressed) is far below the cap; only the write callback sees
            // the decoded size.
            const auto over = transport->fetch(get(server.url(path), 64 * std::size_t{1024}));
            INFO(describe(over));
            REQUIRE(!over.has_value());
            CHECK(over.error().kind == TransportErrc::too_large);
        }
    }
}

TEST_CASE("curl transport: a stalled response times out", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/silent") {
            return loopback::Response{.body = "late",
                                      .delay = std::chrono::seconds{20}}; // no byte at all
        }
        return loopback::Response{.body = std::string(1000, 's'), .stall_after = 10};
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    for (const char* path : {"/stall", "/silent"}) {
        HttpRequest request = get(server.url(path));
        request.stall_timeout = std::chrono::seconds{1};
        const auto start = std::chrono::steady_clock::now();
        const auto stalled = transport->fetch(request);
        const auto took = std::chrono::steady_clock::now() - start;
        INFO(path << ": " << describe(stalled));
        REQUIRE(!stalled.has_value());
        CHECK(stalled.error().kind == TransportErrc::timeout);
        // libcurl averages the speed over a ~5 s window, so a burst before the stall delays the
        // abort by up to that much past stall_timeout. The bound only proves it is not a hang.
        CHECK(took < std::chrono::seconds{20});
    }
}

TEST_CASE("curl transport: a stop request cancels a slow transfer promptly", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request&) {
        return loopback::Response{.body = std::string(1000, 'd'),
                                  .drip_bytes = 1,
                                  .drip_interval = std::chrono::milliseconds{100}};
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    SECTION("mid-transfer") {
        const std::stop_source source;
        HttpRequest request = get(server.url("/drip"));
        request.cancel = source.get_token();
        std::atomic<std::int64_t> stopped_at{0};
        // `source` is copied (shared stop state); `stopped_at` outlives the jthread, joined below.
        std::jthread canceller{[source, at = &stopped_at] mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            at->store(std::chrono::steady_clock::now().time_since_epoch().count());
            source.request_stop();
        }};
        const auto cancelled = transport->fetch(request);
        const auto returned = std::chrono::steady_clock::now();
        canceller.join();
        INFO(describe(cancelled));
        REQUIRE(!cancelled.has_value());
        CHECK(cancelled.error().kind == TransportErrc::cancelled);
        const std::chrono::steady_clock::time_point stop_time{
            std::chrono::steady_clock::duration{stopped_at.load()}};
        // The xferinfo callback runs about once a second; 5 s leaves room for a loaded machine.
        CHECK(returned - stop_time < std::chrono::seconds{5});
    }
    SECTION("already requested: nothing is sent") {
        const std::stop_source source;
        source.request_stop();
        HttpRequest request = get(server.url("/drip"));
        request.cancel = source.get_token();
        const auto cancelled = transport->fetch(request);
        REQUIRE(!cancelled.has_value());
        CHECK(cancelled.error().kind == TransportErrc::cancelled);
        CHECK(server.requests().empty());
    }
}

TEST_CASE("curl transport: a refused connection is `connect`", "[net][curl]") {
    const std::uint16_t port = loopback::unused_port();
    REQUIRE(port != 0);
    const auto transport = make_transport();
    const auto refused = transport->fetch(get(std::format("http://127.0.0.1:{}/", port)));
    INFO(describe(refused));
    REQUIRE(!refused.has_value());
    CHECK(refused.error().kind == TransportErrc::connect);
    CHECK(!refused.error().message.empty());
    CHECK(refused.error().message.find('\n') == std::string::npos);
}

TEST_CASE("curl transport: a dropped connection is a protocol error, never a SIGPIPE death",
          "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/nothing") {
            return loopback::Response{.drop_before_response = true};
        }
        return loopback::Response{.body = std::string(100000, 'p'), .drop_after = 1000};
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    for (const char* path : {"/mid-body", "/nothing"}) {
        const auto dropped = transport->fetch(get(server.url(path)));
        INFO(path << ": " << describe(dropped));
        REQUIRE(!dropped.has_value());
        const bool expected = dropped.error().kind == TransportErrc::protocol ||
                              dropped.error().kind == TransportErrc::timeout;
        CHECK(expected);
    }
    CHECK(!sigpipe_blocked()); // the guard restored this thread's mask
    // The transport recovers on a fresh connection.
    const loopback::Server healthy{[](const loopback::Request&) { return ok_body("fine"); }};
    REQUIRE(healthy.ok());
    const auto after = transport->fetch(get(healthy.url("/")));
    INFO(describe(after));
    REQUIRE(after.has_value());
    CHECK(after->body == "fine");
}

TEST_CASE("curl transport: redirects are returned, never followed", "[net][curl]") {
    const loopback::Server server{[](const loopback::Request& request) {
        if (request.target == "/moved") {
            return loopback::Response{
                .status = 302, .reason = "Found", .headers = {{"Location", "/elsewhere"}}};
        }
        return ok_body("elsewhere");
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();
    const auto moved = transport->fetch(get(server.url("/moved")));
    INFO(describe(moved));
    REQUIRE(moved.has_value());
    CHECK(moved->status == 302);
    CHECK(server.requests().size() == 1);
}

TEST_CASE("curl transport: only http and https are spoken", "[net][curl]") {
    const auto transport = make_transport();
    for (const char* url : {"file:///etc/passwd", "ftp://127.0.0.1:9/", "dict://127.0.0.1:9/x"}) {
        const auto refused = transport->fetch(get(url));
        INFO(url << ": " << describe(refused));
        REQUIRE(!refused.has_value());
        CHECK(refused.error().kind == TransportErrc::local);
    }
}

TEST_CASE("curl transport: sequential fetches reuse one pooled connection", "[net][curl]") {
    const loopback::Server server{
        [](const loopback::Request& request) { return ok_body(request.target); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();
    for (int i = 0; i < 5; ++i) {
        const std::string path = std::format("/seq/{}", i);
        const auto result = transport->fetch(get(server.url(path)));
        INFO(describe(result));
        REQUIRE(result.has_value());
        CHECK(result->body == path);
    }
    CHECK(server.connections_accepted() == 1);
}

TEST_CASE("curl transport: 4 threads x 20 sequential fetches share the handle pool",
          "[net][curl]") {
    const loopback::Server server{
        [](const loopback::Request& request) { return ok_body(request.target); }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    constexpr int threads = 4;
    constexpr int per_thread = 20;
    // Catch2 assertions are not thread-safe: workers record outcomes, the main thread checks.
    std::array<std::vector<std::string>, threads> failures{};
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            // `transport`, `server` and `failures` outlive the workers (joined at scope end).
            workers.emplace_back([t, client = transport.get(), srv = &server,
                                  out = &failures.at(static_cast<std::size_t>(t))] {
                for (int i = 0; i < per_thread; ++i) {
                    const std::string path = std::format("/t{}/{}", t, i);
                    const auto result = client->fetch(get(srv->url(path)));
                    if (!result.has_value() || result->status != 200 || result->body != path) {
                        out->push_back(std::format("{}: {}", path, describe(result)));
                    }
                }
            });
        }
    }
    for (const auto& failed : failures) {
        for (const auto& line : failed) {
            UNSCOPED_INFO(line);
        }
        CHECK(failed.empty());
    }
    CHECK(server.requests().size() == threads * per_thread);
}

TEST_CASE("curl transport: the overall deadline ends a drip that never stalls", "[net][curl]") {
    // 10 bytes/s is well above the 1 byte/s stall threshold, so only the deadline can end it.
    const loopback::Server server{[](const loopback::Request&) {
        return loopback::Response{.body = std::string(1000, 'd'),
                                  .drip_bytes = 1,
                                  .drip_interval = std::chrono::milliseconds{100}};
    }};
    REQUIRE(server.ok());
    auto made = make_curl_transport_with_deadline(
        CurlTransportConfig{.user_agent = std::string{user_agent}},
        std::chrono::milliseconds{1500});
    REQUIRE(made.has_value());
    const std::unique_ptr<Transport> transport = std::move(*made);

    const auto start = std::chrono::steady_clock::now();
    const auto slow = transport->fetch(get(server.url("/drip")));
    const auto took = std::chrono::steady_clock::now() - start;
    INFO(describe(slow));
    REQUIRE(!slow.has_value());
    CHECK(slow.error().kind == TransportErrc::timeout);
    CHECK(took >= std::chrono::milliseconds{1000});
    CHECK(took < std::chrono::seconds{10});
}

TEST_CASE("curl transport: coexisting transports share one live CurlApi", "[net][curl]") {
    const loopback::Server server{
        [](const loopback::Request& request) { return ok_body(request.target); }};
    REQUIRE(server.ok());
    {
        const auto first = CurlApi::load();
        const auto second = CurlApi::load();
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->get() == second->get()); // the registry's live.lock() hit path
    }
    auto one = make_transport();
    auto two = make_transport();
    for (auto* const transport : {one.get(), two.get()}) {
        const auto result = transport->fetch(get(server.url("/both")));
        INFO(describe(result));
        REQUIRE(result.has_value());
    }
    one.reset(); // the other transport still holds the global-init reference
    const auto still = two->fetch(get(server.url("/still")));
    INFO(describe(still));
    REQUIRE(still.has_value());
    two.reset(); // last reference: curl_global_cleanup

    const auto again = make_transport(); // a new init cycle
    const auto fresh = again->fetch(get(server.url("/fresh")));
    INFO(describe(fresh));
    REQUIRE(fresh.has_value());
    CHECK(fresh->body == "/fresh");
}

TEST_CASE("curl transport: transports created, used and destroyed on 2 threads at once",
          "[net][curl]") {
    const loopback::Server server{
        [](const loopback::Request& request) { return ok_body(request.target); }};
    REQUIRE(server.ok());

    constexpr int threads = 2;
    constexpr int rounds = 10;
    // Catch2 assertions are not thread-safe: workers record outcomes, the main thread checks.
    std::array<std::vector<std::string>, threads> failures{};
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            // `server` and `failures` outlive the workers (joined at scope end).
            workers.emplace_back(
                [t, srv = &server, out = &failures.at(static_cast<std::size_t>(t))] {
                    for (int i = 0; i < rounds; ++i) {
                        auto made = make_curl_transport(
                            CurlTransportConfig{.user_agent = std::string{user_agent}});
                        if (!made.has_value()) {
                            out->push_back(made.error().message);
                            continue;
                        }
                        const std::string path = std::format("/c{}/{}", t, i);
                        const auto result = (*made)->fetch(get(srv->url(path)));
                        if (!result.has_value() || result->body != path) {
                            out->push_back(std::format("{}: {}", path, describe(result)));
                        }
                    } // each transport (and possibly the last CurlApi reference) dies here
                });
        }
    }
    for (const auto& failed : failures) {
        for (const auto& line : failed) {
            UNSCOPED_INFO(line);
        }
        CHECK(failed.empty());
    }
    CHECK(server.requests().size() == threads * rounds);
}

TEST_CASE("curl transport: a burst beyond the pool size closes the surplus handles",
          "[net][curl]") {
    constexpr std::size_t burst = curl_max_pooled_handles + 4;
    // The server holds every response until the whole burst has arrived, so all `burst` handles
    // are checked out at once; on return, only curl_max_pooled_handles of them are kept.
    std::atomic<std::size_t> arrivals{0}; // outlives the server (declared first)
    const loopback::Server server{[count = &arrivals](const loopback::Request& request) {
        const std::size_t n = count->fetch_add(1) + 1;
        const std::size_t target = (((n - 1) / burst) + 1) * burst;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (count->load() < target && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return ok_body(request.target);
    }};
    REQUIRE(server.ok());
    const auto transport = make_transport();

    const auto run_burst = [client = transport.get(), srv = &server](int round) {
        std::array<std::vector<std::string>, burst> failures{};
        {
            std::vector<std::jthread> workers;
            workers.reserve(burst);
            for (std::size_t t = 0; t < burst; ++t) {
                // `client`, `srv` and `failures` outlive the workers (joined at scope end).
                workers.emplace_back([t, round, client, srv, out = &failures.at(t)] {
                    const std::string path = std::format("/b{}/{}", round, t);
                    const auto result = client->fetch(get(srv->url(path)));
                    if (!result.has_value() || result->body != path) {
                        out->push_back(std::format("{}: {}", path, describe(result)));
                    }
                });
            }
        }
        return failures;
    };

    for (int round = 0; round < 2; ++round) {
        for (const auto& failed : run_burst(round)) {
            for (const auto& line : failed) {
                UNSCOPED_INFO(line);
            }
            CHECK(failed.empty());
        }
    }
    CHECK(server.requests().size() == 2 * burst);
    // Round 1 opens `burst` connections. The pooled handles keep theirs for round 2; the 4
    // surplus handles were closed on return, so round 2 opens exactly 4 new connections.
    CHECK(server.connections_accepted() == burst + (burst - curl_max_pooled_handles));
}
