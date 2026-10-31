// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/auth_client_test.cpp: the token client (follows plan T8, AC4, AC6 client half).
// Over FakeTransport + FakeClock: one form POST per attempt, never an Authorization; refusals
// classified on the `error` code only (invalid_grant at 400 and at 401 alike, V2); exactly one
// POST for any 4xx other than 429; 5xx and transport timeouts retried with backoff, 429 after its
// Retry-After; malformed and oversized 2xx answers are content failures; the 3-request cap; stop
// requests; the URL guard (no I/O for a foreign token or revoke URL); the revoke (any 2xx is
// success whatever the body, V3; never retried, never counted, 5 s limits); and the -vv trace
// (method, URL, status, time; never a body). Then the same client against the fixture
// transport's Keycloak stand-in (support/mdx_auth_fixture.hpp).
//
// Every credential is a fake from support/mdx_auth_fixture.hpp; nothing contacts a real host.

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_transport.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using md::AuthFailureKind;
using md::Errc;
using md::GrantKind;
using md::HttpMethod;
using md::HttpResponse;
using md::Secret;
using md::TokenRefusal;
using md::TransportErrc;
using md::test::FakeClock;
using md::test::FakeTransport;
using md::test::RecordedRequest;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::string_view form_type = "application/x-www-form-urlencoded";

/// The value of `optional`, which the caller has just REQUIREd to be engaged. An empty one (a
/// test bug) throws, which Catch2 reports; unlike REQUIRE, the check is visible to
/// bugprone-unchecked-optional-access.
template <class T> [[nodiscard]] const T& engaged(const std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

[[nodiscard]] std::string official_url(std::string_view endpoint) {
    return std::format("{}/protocol/openid-connect/{}", md::official_auth_issuer, endpoint);
}

[[nodiscard]] md::AuthEndpoints official_endpoints() {
    return md::AuthEndpoints{
        .issuer = std::string{md::official_auth_issuer},
        .token_url = official_url("token"),
        .revoke_url = official_url("revoke"),
    };
}

/// A TraceSink that keeps every line (thread-safe, though the client calls it from one thread).
class RecordingTrace final : public md::TraceSink {
public:
    RecordingTrace() = default;

    void trace(std::string line) override {
        const std::scoped_lock lock{mutex_};
        lines_.push_back(std::move(line));
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::scoped_lock lock{mutex_};
        return lines_;
    }

private:
    mutable std::mutex mutex_{};
    std::vector<std::string> lines_{}; ///< guarded by mutex_
};

[[nodiscard]] std::uint32_t no_jitter(std::uint32_t /*max*/) { return 0; }

/// A client over a fake transport and clock. Neither copyable nor movable (the client borrows the
/// members declared before it).
struct Harness {
    explicit Harness(md::AuthEndpoints endpoints = official_endpoints())
        : client(transport, clock, std::move(endpoints), no_jitter, &trace) {}

    FakeTransport transport{};
    FakeClock clock{};
    RecordingTrace trace{};
    // AuthClient has no default constructor (it borrows the members above): initialized in the
    // constructor's member-initializer list, after them.
    md::AuthClient client;
};

[[nodiscard]] md::ClientCredentials fixture_client() {
    return md::ClientCredentials{
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = Secret::copy_of(mdx::fixture_client_secret),
    };
}

[[nodiscard]] md::BearerToken token(std::string_view text) {
    md::Result<md::BearerToken> parsed = md::BearerToken::parse(Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] Secret password_body(std::string_view password = mdx::fixture_password) {
    const Secret secret = Secret::copy_of(password);
    return md::password_grant_body(fixture_client(), mdx::fixture_username, secret);
}

[[nodiscard]] Secret refresh_body(std::string_view refresh = mdx::fixture_refresh_1) {
    return md::refresh_grant_body(fixture_client(), token(refresh));
}

[[nodiscard]] Secret revoke_body(std::string_view refresh = mdx::fixture_refresh_1) {
    return md::revoke_body(fixture_client(), token(refresh));
}

[[nodiscard]] std::string grant_body(std::string_view access = mdx::fixture_access_1,
                                     std::string_view refresh = mdx::fixture_refresh_1) {
    return mdx::grant_json(mdx::FixtureGrant{.access_token = std::string{access},
                                             .refresh_token = std::string{refresh}})
        .dump();
}

[[nodiscard]] std::string error_body(int status, std::string_view code,
                                     std::string_view description = {}) {
    return mdx::scripted_error(status, code, description).body.dump();
}

/// No recorded request carries an Authorization; every POST is a form.
void check_plain_form_posts(const std::vector<RecordedRequest>& requests) {
    for (const RecordedRequest& request : requests) {
        CHECK(request.method == HttpMethod::post);
        CHECK(request.authorization.empty());
        CHECK(request.content_type == form_type);
        CHECK(request.max_body_bytes == md::max_token_response_bytes);
    }
}

/// No trace line holds a secret, a token or a body.
void check_trace_clean(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        CAPTURE(line);
        CHECK_THAT(line, StartsWith("POST "));
        for (const std::string_view secret :
             {mdx::fixture_password, mdx::fixture_client_secret, mdx::fixture_access_1,
              mdx::fixture_refresh_1, mdx::fixture_access_2, mdx::fixture_refresh_2,
              std::string_view{"grant_type"}}) {
            CHECK_THAT(line, !ContainsSubstring(std::string{secret}));
        }
    }
}

} // namespace

// ---- grants ------------------------------------------------------------------------------------

TEST_CASE("auth client: a password grant is one form POST without an Authorization",
          "[auth][auth client]") {
    Harness h{};
    h.transport.push_status(official_url("token"), 200, grant_body());
    const Secret body = password_body();

    const md::AuthResult<md::TokenGrant> grant =
        h.client.request(GrantKind::password, body, std::stop_token{});
    REQUIRE(grant.has_value());
    CHECK(grant->access.value().reveal() == mdx::fixture_access_1);
    REQUIRE(grant->refresh.has_value());
    CHECK(engaged(grant->refresh).value().reveal() == mdx::fixture_refresh_1);
    CHECK(grant->expires_in == seconds{900});
    CHECK(h.client.requests() == 1);

    const std::vector<RecordedRequest> requests = h.transport.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == official_url("token"));
    CHECK(requests[0].body == body.reveal());
    check_plain_form_posts(requests);
    CHECK(body.reveal() == password_body().reveal()); // the caller's body is untouched

    const std::vector<std::string> lines = h.trace.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::format("POST {} -> 200 (0 ms)", official_url("token")));
    check_trace_clean(lines);
}

TEST_CASE("auth client: a refresh grant replaces the refresh token", "[auth][auth client]") {
    Harness h{};
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const Secret body = refresh_body();
    const auto grant = h.client.request(GrantKind::refresh, body, std::stop_token{});
    REQUIRE(grant.has_value());
    CHECK(grant->access.value().reveal() == mdx::fixture_access_2);
    REQUIRE(grant->refresh.has_value());
    CHECK(engaged(grant->refresh).value().reveal() == mdx::fixture_refresh_2);
    const std::vector<RecordedRequest> requests = h.transport.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].body == body.reveal());
    check_plain_form_posts(requests);
}

TEST_CASE("auth client: invalid_grant on a password grant is refused at 400 and at 401 alike",
          "[auth][auth client]") {
    const int status = GENERATE(400, 401);
    Harness h{};
    h.transport.push_status(official_url("token"), status,
                            error_body(status, "invalid_grant", "Invalid user credentials"));

    const auto grant = h.client.request(GrantKind::password, password_body(), std::stop_token{});
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::refused);
    CHECK(grant.error().info.refusal == TokenRefusal::bad_credentials);
    CHECK(grant.error().info.status == status);
    CHECK(grant.error().info.code == std::optional<std::string>{"invalid_grant"});
    CHECK(grant.error().info.description == std::optional<std::string>{"Invalid user credentials"});
    CHECK(grant.error().error.code == Errc::auth_rejected);
    CHECK_THAT(grant.error().error.message, !ContainsSubstring("Invalid user credentials"));
    CHECK(h.transport.count(official_url("token")) == 1); // never retried
    CHECK(h.clock.sleeps().empty());
    CHECK(h.client.requests() == 1);
}

TEST_CASE("auth client: refusals are classified by code, answered once", "[auth][auth client]") {
    const auto [grant_kind, status, code, refusal,
                errc] = GENERATE(table<GrantKind, int, std::string, TokenRefusal, Errc>({
        {GrantKind::refresh, 400, "invalid_grant", TokenRefusal::session_ended,
         Errc::login_required},
        {GrantKind::refresh, 401, "invalid_grant", TokenRefusal::session_ended,
         Errc::login_required},
        {GrantKind::password, 400, "unauthorized_client", TokenRefusal::client_not_allowed,
         Errc::auth_rejected},
        {GrantKind::password, 401, "invalid_client", TokenRefusal::bad_client, Errc::auth_rejected},
        {GrantKind::refresh, 401, "invalid_client", TokenRefusal::bad_client, Errc::auth_rejected},
        {GrantKind::password, 403, "access_denied", TokenRefusal::other, Errc::auth_rejected},
        {GrantKind::password, 400, "invalid_request", TokenRefusal::other, Errc::auth_rejected},
    }));
    CAPTURE(code, status);
    Harness h{};
    h.transport.push_status(official_url("token"), status, error_body(status, code));
    const Secret body = grant_kind == GrantKind::password ? password_body() : refresh_body();

    const auto grant = h.client.request(grant_kind, body, std::stop_token{});
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::refused);
    CHECK(grant.error().info.refusal == refusal);
    CHECK(grant.error().info.status == status);
    CHECK(grant.error().info.code == std::optional{code});
    CHECK(grant.error().error.code == errc);
    CHECK(h.transport.count(official_url("token")) == 1);
    CHECK(h.clock.sleeps().empty());
}

TEST_CASE("auth client: a 4xx without a parsable error is a request failure, answered once",
          "[auth][auth client]") {
    const auto [status, body, reason] = GENERATE(table<int, std::string, std::string>({
        {400, "", "HTTP 400"},
        {401, "<html>Unauthorized</html>", "HTTP 401"},
        {404, R"({"error":"Not Found"})", "HTTP 404"},
        {400, R"({"error_description":"no code"})", "HTTP 400"},
        {403, "Forbidden",
         "HTTP 403 (access denied — possibly a temporary rate-limit ban; wait before retrying)"},
    }));
    CAPTURE(status, body);
    Harness h{};
    h.transport.push_status(official_url("token"), status, body);

    const auto grant = h.client.request(GrantKind::password, password_body(), std::stop_token{});
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::request);
    CHECK(grant.error().error.code == Errc::network);
    CHECK(grant.error().error.message == std::format("{}: {}", official_url("token"), reason));
    CHECK(h.transport.count(official_url("token")) == 1);
    CHECK(h.clock.sleeps().empty());
}

TEST_CASE("auth client: 5xx is retried with backoff", "[auth][auth client]") {
    Harness h{};
    const std::string url = official_url("token");
    SECTION("503 three times, then 200") {
        for (int i = 0; i < 3; ++i) {
            h.transport.push_status(url, 503, error_body(503, "temporarily_unavailable"));
        }
        h.transport.push_status(url, 200, grant_body());
        const auto grant = h.client.request(GrantKind::password, password_body(), {});
        REQUIRE(grant.has_value());
        CHECK(h.transport.count(url) == 4);
        CHECK(h.clock.total_slept() == milliseconds{1000 + 2000 + 4000});
        CHECK(h.client.requests() == 1); // one logical request
        check_plain_form_posts(h.transport.requests());
        const std::vector<std::string> lines = h.trace.lines();
        REQUIRE(lines.size() == 4);
        CHECK(lines[0] == std::format("POST {} -> 503 (0 ms)", url));
        check_trace_clean(lines);
    }
    SECTION("503 until the attempts run out, even with an error code") {
        for (int i = 0; i < 4; ++i) {
            h.transport.push_status(url, 503, error_body(503, "temporarily_unavailable"));
        }
        const auto grant = h.client.request(GrantKind::password, password_body(), {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().kind == AuthFailureKind::request);
        CHECK(grant.error().error.code == Errc::network);
        CHECK(grant.error().error.message == std::format("{}: HTTP 503 (server error)", url));
        CHECK(h.transport.count(url) == 4);
        CHECK(h.transport.drained());
    }
}

TEST_CASE("auth client: 429 waits for Retry-After, then retries once", "[auth][auth client]") {
    Harness h{};
    const std::string url = official_url("token");
    SECTION("then 200") {
        h.transport.push(url, HttpResponse{.status = 429, .retry_after = "2"});
        h.transport.push_status(url, 200, grant_body());
        const auto grant = h.client.request(GrantKind::password, password_body(), {});
        REQUIRE(grant.has_value());
        CHECK(h.transport.count(url) == 2);
        const auto sleeps = h.clock.sleeps();
        REQUIRE(sleeps.size() == 1);
        CHECK(sleeps[0].requested == milliseconds{2000});
    }
    SECTION("429 twice is a request failure, never a refusal") {
        for (int i = 0; i < 2; ++i) {
            h.transport.push(url, HttpResponse{.status = 429,
                                               .body = error_body(429, "invalid_grant"),
                                               .retry_after = "2"});
        }
        const auto grant = h.client.request(GrantKind::password, password_body(), {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().kind == AuthFailureKind::request);
        CHECK(grant.error().error.message ==
              std::format("{}: HTTP 429 (rate limited; wait before logging in again)", url));
        CHECK(h.transport.count(url) == 2);
    }
}

TEST_CASE("auth client: transport errors", "[auth][auth client]") {
    Harness h{};
    const std::string url = official_url("token");
    SECTION("a timeout is retried") {
        h.transport.push_error(url, TransportErrc::timeout, "timed out");
        h.transport.push_status(url, 200, grant_body());
        const auto grant = h.client.request(GrantKind::refresh, refresh_body(), {});
        REQUIRE(grant.has_value());
        CHECK(h.transport.count(url) == 2);
        const std::vector<std::string> lines = h.trace.lines();
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == std::format("POST {} -> error: timed out (0 ms)", url));
    }
    SECTION("a connect error is not") {
        h.transport.push_error(url, TransportErrc::connect, "connection refused");
        const auto grant = h.client.request(GrantKind::refresh, refresh_body(), {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().kind == AuthFailureKind::request);
        CHECK(grant.error().error.code == Errc::network);
        CHECK(grant.error().error.message == std::format("{}: connection refused", url));
        CHECK(h.transport.count(url) == 1);
    }
}

TEST_CASE("auth client: a malformed 2xx is a content failure", "[auth][auth client]") {
    const std::string body =
        GENERATE(std::string{"not json"}, std::string{"{}"},
                 std::format(R"({{"access_token":"{} x"}})", mdx::fixture_access_1));
    Harness h{};
    h.transport.push_status(official_url("token"), 200, body);
    const auto grant = h.client.request(GrantKind::password, password_body(), {});
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::content);
    CHECK(grant.error().error.code == Errc::network);
    CHECK_THAT(grant.error().error.message,
               StartsWith(official_url("token") + ": malformed MangaDex login response: "));
    CHECK_THAT(grant.error().error.message, !ContainsSubstring(std::string{mdx::fixture_access_1}));
    CHECK(h.transport.count(official_url("token")) == 1);
}

TEST_CASE("auth client: a response over 64 KiB is a content failure", "[auth][auth client]") {
    Harness h{};
    h.transport.push_status(official_url("token"), 200,
                            std::string(md::max_token_response_bytes + 1, ' '));
    const auto grant = h.client.request(GrantKind::password, password_body(), {});
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::content);
    CHECK(grant.error().error.message ==
          std::format("{}: the response is larger than 65536 bytes", official_url("token")));
    CHECK(h.transport.count(official_url("token")) == 1);
}

TEST_CASE("auth client: at most 3 logical requests per run", "[auth][auth client]") {
    Harness h{};
    const std::string url = official_url("token");
    // 1: two attempts, one logical request.
    h.transport.push_status(url, 503);
    h.transport.push_status(url, 200, grant_body());
    // 2: a refusal still counts.
    h.transport.push_status(url, 400, error_body(400, "invalid_grant"));
    // 3: a success.
    h.transport.push_status(url, 200, grant_body());
    h.transport.set_fallback(HttpResponse{.status = 200, .body = grant_body()});

    CHECK(h.client.request(GrantKind::password, password_body(), {}).has_value());
    const auto refused = h.client.request(GrantKind::refresh, refresh_body(), {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == AuthFailureKind::refused);
    CHECK(h.client.request(GrantKind::password, password_body(), {}).has_value());
    CHECK(h.client.requests() == 3);
    CHECK(h.transport.count(url) == 4);

    const auto fourth = h.client.request(GrantKind::password, password_body(), {});
    REQUIRE_FALSE(fourth.has_value());
    CHECK(fourth.error().kind == AuthFailureKind::content);
    CHECK(fourth.error().error ==
          md::Error{.code = Errc::network,
                    .message = "more than 3 MangaDex login requests in one run; refusing to "
                               "continue"});
    CHECK(h.transport.count(url) == 4); // no I/O
    CHECK(h.client.requests() == 3);

    SECTION("the revoke is not counted and still works") {
        h.transport.push_status(official_url("revoke"), 200);
        CHECK(h.client.revoke(revoke_body(), {}).has_value());
        CHECK(h.client.requests() == 3);
    }
}

TEST_CASE("auth client: requests are spaced by the auth limiter", "[auth][auth client]") {
    Harness h{};
    h.transport.set_fallback(HttpResponse{.status = 200, .body = grant_body()});
    REQUIRE(h.client.request(GrantKind::password, password_body(), {}).has_value());
    CHECK(h.clock.sleeps().empty());
    REQUIRE(h.client.request(GrantKind::refresh, refresh_body(), {}).has_value());
    const auto sleeps = h.clock.sleeps();
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps[0].requested == md::auth_request_interval);
}

TEST_CASE("auth client: a stop request", "[auth][auth client]") {
    Harness h{};
    const std::string url = official_url("token");
    std::stop_source source{};
    SECTION("before the request: no I/O") {
        source.request_stop();
        const auto grant =
            h.client.request(GrantKind::password, password_body(), source.get_token());
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().kind == AuthFailureKind::request);
        CHECK(grant.error().error.code == Errc::interrupted);
        CHECK(h.transport.requests().empty());
    }
    SECTION("during the backoff") {
        h.transport.push_status(url, 503);
        // The hook borrows `source`, which outlives every sleep of this section's client.
        h.clock.set_on_sleep([stop = &source] { stop->request_stop(); });
        const auto grant =
            h.client.request(GrantKind::password, password_body(), source.get_token());
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().error.code == Errc::interrupted);
        CHECK(h.transport.count(url) == 1);
    }
    SECTION("while the POST is in flight, and the transport reports the cancel") {
        // The hook borrows `source`, which outlives every fetch of this section's client.
        h.transport.set_on_fetch(
            [stop = &source](const md::HttpRequest&) { stop->request_stop(); });
        const auto grant =
            h.client.request(GrantKind::password, password_body(), source.get_token());
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().kind == AuthFailureKind::request);
        CHECK(grant.error().error.code == Errc::interrupted);
        CHECK(h.transport.count(url) == 1);
    }
    SECTION("the revoke") {
        source.request_stop();
        const auto revoked = h.client.revoke(revoke_body(), source.get_token());
        REQUIRE_FALSE(revoked.has_value());
        CHECK(revoked.error().code == Errc::interrupted);
        CHECK(h.transport.requests().empty());
    }
}

namespace {

/// A transport that requests a stop while "in flight" and still delivers its scripted answer, as
/// a real transport does when the response is complete before it sees the cancel.
class StopThenAnswer final : public md::Transport {
public:
    /// Borrows `source`, which must outlive the transport.
    StopThenAnswer(std::stop_source& source, md::TransportResult answer)
        : source_(&source), answer_(std::move(answer)) {}

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& /*request*/) override {
        ++fetches_;
        source_->request_stop();
        return answer_;
    }

    [[nodiscard]] int fetches() const noexcept { return fetches_; }

private:
    std::stop_source* source_ = nullptr; ///< borrowed; never null
    md::TransportResult answer_{};
    int fetches_ = 0;
};

} // namespace

TEST_CASE("auth client: a 2xx grant is kept even when a stop arrives with it",
          "[auth][auth client]") {
    std::stop_source source{};
    StopThenAnswer transport{source, HttpResponse{.status = 200, .body = grant_body()}};
    FakeClock clock{};
    md::AuthClient client{transport, clock, official_endpoints(), no_jitter, nullptr};

    const auto grant = client.request(GrantKind::refresh, refresh_body(), source.get_token());
    REQUIRE(grant.has_value()); // the rotated refresh token must reach the caller
    CHECK(grant->access.value().reveal() == mdx::fixture_access_1);
    CHECK(engaged(grant->refresh).value().reveal() == mdx::fixture_refresh_1);
    CHECK(client.requests() == 1);
    CHECK(transport.fetches() == 1);
    CHECK(source.stop_requested());
}

TEST_CASE("auth client: a failed attempt with a stop pending is an interrupt, not a retry",
          "[auth][auth client]") {
    const int status = GENERATE(503, 401);
    std::stop_source source{};
    StopThenAnswer transport{source,
                             HttpResponse{.status = status, .body = error_body(status, "x")}};
    FakeClock clock{};
    md::AuthClient client{transport, clock, official_endpoints(), no_jitter, nullptr};

    const auto grant = client.request(GrantKind::password, password_body(), source.get_token());
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::request);
    CHECK(grant.error().error.code == Errc::interrupted);
    CHECK(transport.fetches() == 1);
}

TEST_CASE("auth client: the URL guard refuses foreign servers before any I/O",
          "[auth][auth client]") {
    const std::string url = GENERATE(
        std::string{}, std::string{"https://example.org/realms/mangadex/protocol/openid-connect/x"},
        std::string{"https://auth.mangadex.org.evil.example/realms/mangadex/protocol/x"},
        std::string{"http://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token"},
        std::string{"https://auth.mangadex.org/realms/mangadex"},
        std::string{"https://auth.mangadex.org/realms/mangadexx/protocol/openid-connect/token"},
        std::string{"https://auth.mangadex.org/realms/mangadex.evil.example/token"},
        std::string{"FILE:///tmp/auth/realms/mangadex/protocol/openid-connect/token"},
        std::string{"http://127.0.0.1:8080/realms/mangadex/protocol/openid-connect/token"},
        std::string{"https://auth.mangadex.org@evil.example/realms/mangadex/protocol/x"},
        std::string{"https://auth.mangadex.org:443@evil.example/realms/mangadex/protocol/x"},
        std::string{"https://auth.mangadex.org/realms/mangadex@evil.example/x"},
        std::string{"https://auth.mangadex.org\\@evil.example/realms/mangadex/protocol/x"},
        std::string{"https://auth.mangadex.org/realms/mangadex\\evil.example/x"},
        std::string{"HTTPS://AUTH.MANGADEX.ORG/realms/mangadex/protocol/openid-connect/token"});
    CAPTURE(url);
    Harness h{md::AuthEndpoints{
        .issuer = std::string{md::official_auth_issuer}, .token_url = url, .revoke_url = url}};
    h.transport.set_fallback(HttpResponse{.status = 200, .body = grant_body()});

    const auto grant = h.client.request(GrantKind::password, password_body(), {});
    REQUIRE_FALSE(grant.has_value());
    CHECK(grant.error().kind == AuthFailureKind::local);
    CHECK(grant.error().error.code == Errc::usage);
    CHECK(h.client.requests() == 0);

    const auto revoked = h.client.revoke(revoke_body(), {});
    REQUIRE_FALSE(revoked.has_value());
    CHECK(revoked.error().code == Errc::usage);

    CHECK(h.transport.requests().empty());
    CHECK(h.trace.lines().empty());
}

// ---- revoke ------------------------------------------------------------------------------------

TEST_CASE("auth client: any 2xx revokes, whatever the body", "[auth][auth client]") {
    const std::string answer = GENERATE(std::string{}, std::string{R"({"error":"invalid_token"})"},
                                        std::string{"not json at all"});
    Harness h{};
    const std::string url = official_url("revoke");
    h.transport.push_status(url, 200, answer);
    std::optional<std::pair<milliseconds, seconds>> limits{};
    // The hook borrows `limits`, which outlives every fetch of this test's client.
    h.transport.set_on_fetch([out = &limits](const md::HttpRequest& request) {
        *out = std::pair{request.connect_timeout, request.stall_timeout};
    });
    const Secret body = revoke_body();

    const md::Result<void> revoked = h.client.revoke(body, {});
    CHECK(revoked.has_value());
    CHECK(h.client.requests() == 0);
    const std::vector<RecordedRequest> requests = h.transport.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == url);
    CHECK(requests[0].body == body.reveal());
    check_plain_form_posts(requests);
    CHECK(limits == std::optional{std::pair{milliseconds{md::revoke_timeout}, md::revoke_timeout}});
    const std::vector<std::string> lines = h.trace.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::format("POST {} -> 200 (0 ms)", url));
    check_trace_clean(lines);
}

TEST_CASE("auth client: a failed revoke is never retried", "[auth][auth client]") {
    Harness h{};
    const std::string url = official_url("revoke");
    SECTION("a status") {
        const auto [status, reason] = GENERATE(table<int, std::string>({
            {503, "HTTP 503 (server error)"},
            {500, "HTTP 500 (server error)"},
            {400, "HTTP 400"},
            {401, "HTTP 401"},
            {429, "HTTP 429 (rate limited; wait before logging in again)"},
        }));
        h.transport.push_status(url, status, R"({"error":"invalid_client"})");
        const md::Result<void> revoked = h.client.revoke(revoke_body(), {});
        REQUIRE_FALSE(revoked.has_value());
        CHECK(revoked.error() ==
              md::Error{.code = Errc::network, .message = std::format("{}: {}", url, reason)});
    }
    SECTION("a transport error") {
        h.transport.push_error(url, TransportErrc::timeout, "timed out");
        const md::Result<void> revoked = h.client.revoke(revoke_body(), {});
        REQUIRE_FALSE(revoked.has_value());
        CHECK(revoked.error() ==
              md::Error{.code = Errc::network, .message = std::format("{}: timed out", url)});
    }
    CHECK(h.transport.count(url) == 1);
    CHECK(h.clock.sleeps().empty());
}

TEST_CASE("auth client: grants keep the transport's default limits", "[auth][auth client]") {
    Harness h{};
    h.transport.push_status(official_url("token"), 200, grant_body());
    std::optional<std::pair<milliseconds, seconds>> limits{};
    // The hook borrows `limits`, which outlives every fetch of this test's client.
    h.transport.set_on_fetch([out = &limits](const md::HttpRequest& request) {
        *out = std::pair{request.connect_timeout, request.stall_timeout};
    });
    REQUIRE(h.client.request(GrantKind::password, password_body(), {}).has_value());
    const md::HttpRequest defaults{};
    CHECK(limits == std::optional{std::pair{defaults.connect_timeout, defaults.stall_timeout}});
}

TEST_CASE("auth client: works without a trace sink", "[auth][auth client]") {
    FakeTransport transport{};
    FakeClock clock{};
    md::AuthClient client{transport, clock, official_endpoints(), no_jitter, nullptr};
    transport.push_status(official_url("token"), 503);
    transport.push_status(official_url("token"), 200, grant_body());
    CHECK(client.request(GrantKind::password, password_body(), {}).has_value());
    transport.push_status(official_url("revoke"), 200);
    CHECK(client.revoke(revoke_body(), {}).has_value());
    CHECK(transport.count(official_url("token")) == 2);
}

TEST_CASE("auth client: jitter is optional and clamped to max_jitter", "[auth][auth client]") {
    const md::RetryConfig defaults{};
    const auto run = [](std::function<std::uint32_t(std::uint32_t)> jitter) {
        FakeTransport transport{};
        FakeClock clock{};
        md::AuthClient client{transport, clock, official_endpoints(), std::move(jitter), nullptr};
        transport.push_status(official_url("token"), 503);
        transport.push_status(official_url("token"), 200, grant_body());
        REQUIRE(client.request(GrantKind::password, password_body(), {}).has_value());
        return clock.total_slept();
    };
    SECTION("an empty jitter function adds nothing") { CHECK(run({}) == defaults.base_delay); }
    SECTION("a jitter above max is clamped") {
        std::uint32_t asked = 0;
        // The jitter borrows `asked`, which outlives the client built inside run().
        CHECK(run([seen = &asked](std::uint32_t max) {
                  *seen = max;
                  return max + 1000U;
              }) == defaults.base_delay + defaults.max_jitter);
        CHECK(asked == static_cast<std::uint32_t>(defaults.max_jitter.count()));
    }
}

// ---- against the fixture transport's Keycloak
// ----------------------------------------------------

namespace {

/// A fixture tree with `auth`'s files and a client on the fixture transport.
struct FixtureHarness {
    explicit FixtureHarness(const mdx::FixtureAuth& auth = mdx::FixtureAuth{}) {
        auto made = md::test::TempDir::create("mangadex-dl-auth-client");
        REQUIRE(made.has_value());
        dir.emplace(std::move(*made));
        root = fs::absolute(dir->path()) / "root";
        const auto written = mdx::write_auth_files(root, auth);
        INFO((written.has_value() ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        auto made_transport = md::make_fixture_transport(root);
        REQUIRE(made_transport.has_value());
        transport = std::move(*made_transport);
        client = std::make_unique<md::AuthClient>(
            *transport, clock,
            md::AuthEndpoints{
                .issuer = std::format("file://{}/auth/realms/mangadex", root.string()),
                .token_url = mdx::token_url(root),
                .revoke_url = mdx::revoke_url(root),
            },
            no_jitter, &trace);
    }

    std::optional<md::test::TempDir> dir{}; ///< engaged by the constructor
    fs::path root{};
    std::unique_ptr<md::Transport> transport{};
    FakeClock clock{};
    RecordingTrace trace{};
    std::unique_ptr<md::AuthClient> client{}; ///< last: destroyed before what it borrows
};

} // namespace

TEST_CASE("auth client: the fixture's Keycloak, password then refreshes", "[auth][auth client]") {
    FixtureHarness f{};
    const auto first = f.client->request(GrantKind::password, password_body(), {});
    REQUIRE(first.has_value());
    CHECK(first->access.value().reveal() == mdx::fixture_access_1);
    REQUIRE(first->refresh.has_value());
    CHECK(engaged(first->refresh).value().reveal() == mdx::fixture_refresh_1);
    CHECK(first->expires_in == seconds{900});

    const Secret refresh = md::refresh_grant_body(fixture_client(), engaged(first->refresh));
    const auto second = f.client->request(GrantKind::refresh, refresh, {});
    REQUIRE(second.has_value());
    CHECK(second->access.value().reveal() == mdx::fixture_access_2);
    REQUIRE(second->refresh.has_value());
    CHECK(engaged(second->refresh).value().reveal() == mdx::fixture_refresh_2);

    // The end of the chain: the last refresh token is not active any more.
    const Secret ended = md::refresh_grant_body(fixture_client(), engaged(second->refresh));
    const auto third = f.client->request(GrantKind::refresh, ended, {});
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error().kind == AuthFailureKind::refused);
    CHECK(third.error().info.refusal == TokenRefusal::session_ended);
    CHECK(third.error().info.status == 400);
    CHECK(f.client->requests() == 3);

    const std::vector<std::string> lines = f.trace.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == std::format("POST {} -> 200 (0 ms)", mdx::token_url(f.root)));
    CHECK(lines[2] == std::format("POST {} -> 400 (0 ms)", mdx::token_url(f.root)));
    check_trace_clean(lines);
}

TEST_CASE("auth client: the fixture's Keycloak refuses wrong details", "[auth][auth client]") {
    SECTION("a wrong password, 401 by default") {
        FixtureHarness f{};
        const auto grant = f.client->request(GrantKind::password, password_body("wrong-guess"), {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().kind == AuthFailureKind::refused);
        CHECK(grant.error().info.refusal == TokenRefusal::bad_credentials);
        CHECK(grant.error().info.status == 401);
    }
    SECTION("a wrong password, 400 on newer Keycloak") {
        FixtureHarness f{mdx::FixtureAuth{.mismatch_status = 400}};
        const auto grant = f.client->request(GrantKind::password, password_body("wrong-guess"), {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().info.refusal == TokenRefusal::bad_credentials);
        CHECK(grant.error().info.status == 400);
    }
    SECTION("a wrong client secret") {
        FixtureHarness f{};
        md::ClientCredentials client = fixture_client();
        client.client_secret = Secret::copy_of("wrong-client-guess");
        const Secret password = Secret::copy_of(mdx::fixture_password);
        const Secret body = md::password_grant_body(client, mdx::fixture_username, password);
        const auto grant = f.client->request(GrantKind::password, body, {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().info.refusal == TokenRefusal::bad_client);
        CHECK(grant.error().info.status == 401);
    }
    SECTION("a client that is not approved") {
        FixtureHarness f{mdx::FixtureAuth{
            .password_refusal = mdx::scripted_error(400, "unauthorized_client"),
        }};
        const auto grant = f.client->request(GrantKind::password, password_body(), {});
        REQUIRE_FALSE(grant.has_value());
        CHECK(grant.error().info.refusal == TokenRefusal::client_not_allowed);
        CHECK(grant.error().info.status == 400);
        CHECK(grant.error().error.code == Errc::auth_rejected);
    }
}

TEST_CASE("auth client: the fixture's Keycloak revokes", "[auth][auth client]") {
    SECTION("an unknown token answers 200 invalid_token, which is success") {
        FixtureHarness f{mdx::FixtureAuth{.revokes = {}}};
        CHECK(f.client->revoke(revoke_body(), {}).has_value());
    }
    SECTION("a known token") {
        FixtureHarness f{};
        CHECK(f.client->revoke(revoke_body(), {}).has_value());
    }
    SECTION("a scripted 503 fails, once") {
        FixtureHarness f{mdx::FixtureAuth{
            .revokes = {mdx::FixtureRevoke{.token = std::string{mdx::fixture_refresh_1},
                                           .status = 503}},
        }};
        const md::Result<void> revoked = f.client->revoke(revoke_body(), {});
        REQUIRE_FALSE(revoked.has_value());
        CHECK(revoked.error().code == Errc::network);
        CHECK(revoked.error().message ==
              std::format("{}: HTTP 503 (server error)", mdx::revoke_url(f.root)));
        CHECK(f.trace.lines().size() == 1);
    }
}
