// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/api_client/api_client_authorized_test.cpp: the two authorized GETs (follows plan
// T11, AC8 at the client level) and auth_endpoints_for.
// - auth_endpoints_for: the official issuer's fixed URLs, the fixture tree's realm URLs, nullopt
//   for every custom base (a custom API never inherits the official auth host).
// - get_authorized: exact URLs (the follows page always carries offset=, the status map has no
//   query), the exact Authorization value, the API body cap and limiter; a 401 comes back unmapped
//   and is answered once; 5xx and 429 retry as every API request does; a custom base is
//   Errc::usage with no request; a stop is interrupted; the -vv trace never holds the token.
// - Every other fetch_* sends no Authorization, before and after an authorized call.
// - The real fixture transport: it serves both paths for a known Bearer token, answers an unknown
//   one 401, and trips loudly on an Authorization anywhere else (so the plain fetches prove that
//   nothing leaked).
//
// Every token here is an obviously fake value.

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_transport.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using Catch::Matchers::ContainsSubstring;
using md::test::FakeClock;
using md::test::FakeTransport;
using md::test::RecordedRequest;
using std::chrono::milliseconds;

constexpr std::string_view api = md::default_api_base;
constexpr std::string_view fake_token{"fake-access-token-not-a-secret"};
constexpr std::string_view follows_url_0 =
    "https://api.mangadex.org/user/follows/manga?limit=100&offset=0";
constexpr std::string_view status_url = "https://api.mangadex.org/manga/status";

/// A TraceSink that keeps every line (thread-safe).
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

/// No limiter spacing, so the only sleeps are retry waits.
[[nodiscard]] md::ClientConfig unlimited_config(md::Endpoints endpoints = {}) {
    return md::ClientConfig{.endpoints = std::move(endpoints),
                            .api_interval = milliseconds{0},
                            .at_home_interval = milliseconds{0}};
}

[[nodiscard]] md::Secret fake_authorization() {
    return md::Secret::copy_of(mdx::bearer(fake_token));
}

[[nodiscard]] md::ApiBase parsed_base(std::string_view text) {
    auto base = md::ApiBase::parse(text);
    REQUIRE(base.has_value());
    return *base;
}

[[nodiscard]] std::vector<milliseconds> requested_sleeps(const FakeClock& clock) {
    std::vector<milliseconds> out;
    for (const md::test::SleepRecord& sleep : clock.sleeps()) {
        out.push_back(sleep.requested);
    }
    return out;
}

/// A temp dir holding a fixture tree (the clean title, the default fake account, one follows page
/// and a status map) under `<dir>/root`, and the fixture transport over it.
struct AuthorizedTree {
    std::optional<md::test::TempDir> dir{}; ///< always engaged; optional for the initializer
    std::filesystem::path root{};
    std::unique_ptr<md::Transport> transport{};
};

constexpr std::string_view follows_page_body = R"({"result":"ok","data":[],"total":0})";
constexpr std::string_view status_map_body = R"({"result":"ok","statuses":{}})";

[[nodiscard]] AuthorizedTree make_authorized_tree() {
    auto dir = md::test::TempDir::create("mangadex-dl-api-authorized");
    REQUIRE(dir.has_value());
    const std::filesystem::path root = std::filesystem::absolute(dir->path()) / "root";
    const auto tree = mdx::write_fixture_tree(root, mdx::clean_fixture());
    INFO((tree.has_value() ? std::string{} : tree.error()));
    REQUIRE(tree.has_value());
    const auto auth = mdx::write_auth_files(root, mdx::FixtureAuth{});
    INFO((auth.has_value() ? std::string{} : auth.error()));
    REQUIRE(auth.has_value());
    REQUIRE(mdx::detail::write_file(root / "api" / "user" / "follows" / "manga" / "offset-0.json",
                                    follows_page_body)
                .has_value());
    REQUIRE(mdx::detail::write_file(root / "api" / "manga" / "status.json", status_map_body)
                .has_value());
    auto transport = md::make_fixture_transport(root);
    REQUIRE(transport.has_value());
    return AuthorizedTree{.dir = std::move(*dir), .root = root, .transport = std::move(*transport)};
}

} // namespace

// ---- auth_endpoints_for -----------------------------------------------------------------------

TEST_CASE("api client authorized: auth_endpoints_for gives the official issuer's fixed URLs",
          "[api_client][auth]") {
    const auto endpoints = md::auth_endpoints_for(md::ApiBase{});
    REQUIRE(endpoints.has_value());
    CHECK(
        endpoints ==
        md::AuthEndpoints{
            .issuer = "https://auth.mangadex.org/realms/mangadex",
            .token_url = "https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token",
            .revoke_url =
                "https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/revoke",
        });
    CHECK(endpoints.value_or(md::AuthEndpoints{}).issuer == md::official_auth_issuer);
    // The parsed official base (with a trailing '/', or in upper case) is the same base.
    CHECK(md::auth_endpoints_for(parsed_base("https://api.mangadex.org/")) == endpoints);
    const md::ApiBase upper = parsed_base("HTTPS://API.MANGADEX.ORG/");
    CHECK(upper.kind() == md::ApiBase::Kind::official);
    CHECK(md::auth_endpoints_for(upper) == endpoints);
}

TEST_CASE("api client authorized: auth_endpoints_for puts a fixture's realm under its root",
          "[api_client][auth]") {
    const md::ApiBase base = parsed_base("file:///srv/fixtures/clean/");
    const auto endpoints = md::auth_endpoints_for(base);
    REQUIRE(endpoints.has_value());
    CHECK(endpoints ==
          md::AuthEndpoints{
              .issuer = "file:///srv/fixtures/clean/auth/realms/mangadex",
              .token_url =
                  "file:///srv/fixtures/clean/auth/realms/mangadex/protocol/openid-connect/token",
              .revoke_url =
                  "file:///srv/fixtures/clean/auth/realms/mangadex/protocol/openid-connect/revoke",
          });
    // The same URLs the fixture writer's helpers (and the fixture transport) use.
    const md::AuthEndpoints got = endpoints.value_or(md::AuthEndpoints{});
    CHECK(got.token_url == mdx::token_url("/srv/fixtures/clean"));
    CHECK(got.revoke_url == mdx::revoke_url("/srv/fixtures/clean"));
}

TEST_CASE("api client authorized: a custom API base has no login server", "[api_client][auth]") {
    for (const std::string_view text :
         {"https://mirror.example/api", "https://api.mangadex.org/v5", "https://auth.mangadex.org",
          "https://api.mangadex.org.evil.example", "https://api.mangadex.org:443",
          "https://api.mangadex.org.", "http://127.0.0.1:8080", "http://localhost",
          "http://[::1]:9000/api"}) {
        INFO(text);
        const md::ApiBase base = parsed_base(text);
        CHECK(base.kind() != md::ApiBase::Kind::official);
        CHECK(!md::auth_endpoints_for(base).has_value());
    }
    // Userinfo never reaches either function: such a base does not parse.
    for (const std::string_view text :
         {"https://api.mangadex.org@evil.example", "https://user@api.mangadex.org"}) {
        INFO(text);
        const auto base = md::ApiBase::parse(text);
        REQUIRE(!base.has_value());
        CHECK(base.error().code == md::Errc::usage);
    }
}

// ---- get_authorized over FakeTransport -----------------------------------------------------

TEST_CASE("api client authorized: the follows page URL and Authorization are exact",
          "[api_client][auth]") {
    FakeTransport transport;
    const std::string url =
        std::format("{}/user/follows/manga?limit=100&offset={}", api, std::size_t{200});
    transport.push_status(url, 200, "follows-page");
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::Secret authorization = fake_authorization();

    const auto response =
        client.get_authorized(md::AuthorizedPath::user_follows_manga, 200, authorization, {});
    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->body == "follows-page");
    CHECK(transport.requests() ==
          std::vector<RecordedRequest>{RecordedRequest{
              .method = md::HttpMethod::get,
              .url = "https://api.mangadex.org/user/follows/manga?limit=100&offset=200",
              .max_body_bytes = md::max_api_json_bytes,
              .authorization = mdx::bearer(fake_token),
          }});
    // The caller's Secret is borrowed, never consumed.
    CHECK(authorization.reveal() == mdx::bearer(fake_token));

    // Offset 0 is spelled out too (the fixture needs offset=).
    transport.push_status(std::string{follows_url_0}, 200, "first-page");
    const auto first =
        client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, authorization, {});
    REQUIRE(first.has_value());
    CHECK(first->body == "first-page");
    CHECK(transport.count(follows_url_0) == 1);
    CHECK(transport.drained());
}

TEST_CASE("api client authorized: the status map URL has no query and ignores the offset",
          "[api_client][auth]") {
    FakeTransport transport;
    transport.push_status(std::string{status_url}, 200, "status-map");
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};

    const auto response =
        client.get_authorized(md::AuthorizedPath::manga_status, 7, fake_authorization(), {});
    REQUIRE(response.has_value());
    CHECK(response->body == "status-map");
    CHECK(transport.requests() == std::vector<RecordedRequest>{RecordedRequest{
                                      .method = md::HttpMethod::get,
                                      .url = std::string{status_url},
                                      .max_body_bytes = md::max_api_json_bytes,
                                      .authorization = mdx::bearer(fake_token),
                                  }});
}

TEST_CASE("api client authorized: a 401 comes back unmapped and is answered once",
          "[api_client][auth]") {
    FakeTransport transport;
    transport.push_status(std::string{follows_url_0}, 401, R"({"result":"error"})");
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};

    const auto response =
        client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, fake_authorization(), {});
    REQUIRE(response.has_value());
    CHECK(response->status == 401);
    CHECK(response->body == R"({"result":"error"})");
    CHECK(transport.count(follows_url_0) == 1);
    CHECK(clock.sleeps().empty());

    // 403 and 404 come back the same way (the caller maps them).
    for (const int status : {403, 404}) {
        transport.push_status(std::string{status_url}, status);
        const auto other =
            client.get_authorized(md::AuthorizedPath::manga_status, 0, fake_authorization(), {});
        REQUIRE(other.has_value());
        CHECK(other->status == status);
    }
    CHECK(transport.count(status_url) == 2);
    CHECK(clock.sleeps().empty());
}

TEST_CASE("api client authorized: 5xx is retried like every API request, each with the header",
          "[api_client][auth][retry]") {
    FakeTransport transport;
    transport.push_status(std::string{status_url}, 503);
    transport.push_error(std::string{status_url}, md::TransportErrc::timeout);
    transport.push_status(std::string{status_url}, 200, "status-map");
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};

    const auto response =
        client.get_authorized(md::AuthorizedPath::manga_status, 0, fake_authorization(), {});
    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->body == "status-map");
    CHECK(requested_sleeps(clock) ==
          std::vector<milliseconds>{milliseconds{1000}, milliseconds{2000}});
    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 3);
    for (const RecordedRequest& request : requests) {
        CHECK(request.authorization == mdx::bearer(fake_token));
    }

    // Retries exhausted: the last 5xx response itself, unmapped.
    for (int i = 0; i < md::RetryConfig{}.max_attempts; ++i) {
        transport.push_status(std::string{follows_url_0}, 502);
    }
    const auto exhausted =
        client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, fake_authorization(), {});
    REQUIRE(exhausted.has_value());
    CHECK(exhausted->status == 502);
    CHECK(transport.drained());

    // A hard transport error is Errc::network naming the URL, never the header value.
    transport.push_error(std::string{status_url}, md::TransportErrc::connect, "refused");
    const auto failed =
        client.get_authorized(md::AuthorizedPath::manga_status, 0, fake_authorization(), {});
    REQUIRE(!failed.has_value());
    CHECK(failed.error().code == md::Errc::network);
    CHECK(failed.error().message == std::format("{}: refused", status_url));
}

TEST_CASE("api client authorized: 429 waits for the rate limit as every API request does",
          "[api_client][auth][retry]") {
    FakeTransport transport;
    transport.push(std::string{status_url}, md::HttpResponse{.status = 429, .retry_after = "3"});
    transport.push_status(std::string{status_url}, 200, "status-map");
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};

    const auto response =
        client.get_authorized(md::AuthorizedPath::manga_status, 0, fake_authorization(), {});
    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{3000}});
    CHECK(transport.count(status_url) == 2);
}

TEST_CASE("api client authorized: authorized GETs share the API rate limiter",
          "[api_client][auth]") {
    FakeTransport transport;
    transport.set_fallback(md::HttpResponse{.status = 200});
    FakeClock clock;
    md::ApiClient client{
        transport,
        clock,
        md::ClientConfig{.api_interval = milliseconds{250}, .at_home_interval = milliseconds{0}},
        {},
        nullptr};
    const md::Secret authorization = fake_authorization();
    REQUIRE(client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, authorization, {})
                .has_value());
    REQUIRE(
        client.get_authorized(md::AuthorizedPath::manga_status, 0, authorization, {}).has_value());
    CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{250}});
}

TEST_CASE("api client authorized: a custom API base is usage, with no request",
          "[api_client][auth]") {
    for (const std::string_view text :
         {"https://mirror.example/api", "https://api.mangadex.org/v5",
          "https://api.mangadex.org:443", "https://api.mangadex.org.", "http://127.0.0.1:8080"}) {
        INFO(text);
        FakeTransport transport;
        transport.set_fallback(md::HttpResponse{.status = 200});
        FakeClock clock;
        RecordingTrace trace;
        md::ApiClient client{transport,
                             clock,
                             unlimited_config(md::endpoints_for(parsed_base(text), false)),
                             {},
                             &trace};
        for (const md::AuthorizedPath path :
             {md::AuthorizedPath::user_follows_manga, md::AuthorizedPath::manga_status}) {
            const auto response = client.get_authorized(path, 0, fake_authorization(), {});
            REQUIRE(!response.has_value());
            CHECK(response.error().code == md::Errc::usage);
            CHECK_THAT(response.error().message, ContainsSubstring("official"));
            CHECK_THAT(response.error().message, !ContainsSubstring(std::string{fake_token}));
        }
        CHECK(transport.requests().empty());
        CHECK(trace.lines().empty());
        CHECK(clock.sleeps().empty());
    }

    // A hand-built endpoint that only looks like the official one is refused too.
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{
        transport,
        clock,
        unlimited_config(md::Endpoints{.api = "https://api.mangadex.org.evil.example"}),
        {},
        nullptr};
    const auto response =
        client.get_authorized(md::AuthorizedPath::manga_status, 0, fake_authorization(), {});
    REQUIRE(!response.has_value());
    CHECK(response.error().code == md::Errc::usage);
    CHECK(transport.requests().empty());
}

TEST_CASE("api client authorized: a stop request is interrupted", "[api_client][auth][stop]") {
    // Declared before the transport, whose on_fetch hook (set below) borrows it: the hook never
    // outlives its target.
    std::stop_source during;
    FakeTransport transport;
    transport.set_fallback(md::HttpResponse{.status = 200});
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const std::stop_source source;
    source.request_stop();

    const auto response = client.get_authorized(md::AuthorizedPath::user_follows_manga, 0,
                                                fake_authorization(), source.get_token());
    REQUIRE(!response.has_value());
    CHECK(response.error().code == md::Errc::interrupted);
    CHECK(transport.requests().empty());

    // A stop during the transfer (the transport sees it as cancelled).
    transport.set_on_fetch(
        [stopper = &during](const md::HttpRequest&) { stopper->request_stop(); });
    const auto cancelled = client.get_authorized(md::AuthorizedPath::manga_status, 0,
                                                 fake_authorization(), during.get_token());
    REQUIRE(!cancelled.has_value());
    CHECK(cancelled.error().code == md::Errc::interrupted);
    CHECK(transport.count(status_url) == 1);
}

TEST_CASE("api client authorized: the trace is method, URL and status, never the token",
          "[api_client][auth][trace]") {
    FakeTransport transport;
    transport.push_status(std::string{follows_url_0}, 401);
    transport.push_status(std::string{follows_url_0}, 200);
    transport.push_error(std::string{status_url}, md::TransportErrc::connect, "refused");
    FakeClock clock;
    RecordingTrace trace;
    md::ApiClient client{transport, clock, unlimited_config(), {}, &trace};
    const md::Secret authorization = fake_authorization();

    const auto refused =
        client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, authorization, {});
    CHECK((refused.has_value() && refused->status == 401));
    const auto page =
        client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, authorization, {});
    CHECK((page.has_value() && page->status == 200));
    const auto failed =
        client.get_authorized(md::AuthorizedPath::manga_status, 0, authorization, {});
    CHECK(!failed.has_value());

    const std::vector<std::string> lines = trace.lines();
    CHECK(lines == std::vector<std::string>{
                       std::format("GET {} -> 401 (0 ms)", follows_url_0),
                       std::format("GET {} -> 200 (0 ms)", follows_url_0),
                       std::format("GET {} -> error: refused (0 ms)", status_url),
                   });
    for (const std::string& line : lines) {
        CHECK_THAT(line, !ContainsSubstring(std::string{fake_token}));
        CHECK_THAT(line, !ContainsSubstring("Bearer"));
    }
}

TEST_CASE("api client authorized: no other request ever carries an Authorization (AC8)",
          "[api_client][auth]") {
    FakeTransport transport;
    transport.set_fallback(md::HttpResponse{.status = 404});
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const auto id = md::MangaId::parse(mdx::clean_fixture().id);
    REQUIRE(id.has_value());
    const auto list = md::ListId::parse("22222222-2222-4222-8222-222222222222");
    REQUIRE(list.has_value());
    const md::AtHomeServer server{.base_url = "https://abc123.node.example",
                                  .hash = "abcdef0123456789",
                                  .data = {"p001.png"},
                                  .data_saver = {"s001.png"}};

    const auto every_plain_fetch = [&client, &id, &list, &server] {
        // Every one fails on the fallback 404; only the requests matter here.
        CHECK(!client.fetch_manga(*id, {}).has_value());
        CHECK(!client.fetch_feed(*id, "en", {}).has_value());
        CHECK(!client.fetch_covers(*id, {}).has_value());
        CHECK(!client.fetch_at_home("33333333-3333-4333-8333-333333333333", {}).has_value());
        CHECK(!client.fetch_page(server, md::ImageQuality::data, "p001.png", {}).has_value());
        CHECK(!client.fetch_cover(*id, "cover.jpg", {}).has_value());
        CHECK(!client.fetch_list_document(*list, {}).has_value());
    };

    every_plain_fetch();
    const md::Secret authorization = fake_authorization();
    REQUIRE(client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, authorization, {})
                .has_value());
    REQUIRE(
        client.get_authorized(md::AuthorizedPath::manga_status, 0, authorization, {}).has_value());
    every_plain_fetch();

    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 16);
    std::size_t authorized = 0;
    for (const RecordedRequest& request : requests) {
        INFO(request.url);
        if (request.url == follows_url_0 || request.url == status_url) {
            CHECK(request.authorization == mdx::bearer(fake_token));
            ++authorized;
        } else {
            CHECK(request.authorization.empty());
        }
    }
    CHECK(authorized == 2);
}

// ---- get_authorized over the fixture transport ---------------------------------------------

TEST_CASE("api client authorized: the fixture serves both paths for a known Bearer token",
          "[api_client][auth][fixture]") {
    const AuthorizedTree tree = make_authorized_tree();
    const md::ApiBase base = parsed_base(std::format("file://{}", tree.root.string()));
    const std::unique_ptr<md::Clock> clock = md::make_instant_clock();
    RecordingTrace trace;
    md::ApiClient client{*tree.transport,
                         *clock,
                         md::ClientConfig{.endpoints = md::endpoints_for(base, true)},
                         {},
                         &trace};
    const md::Secret good = md::Secret::copy_of(mdx::bearer(mdx::fixture_access_1));

    const auto page = client.get_authorized(md::AuthorizedPath::user_follows_manga, 0, good, {});
    REQUIRE(page.has_value());
    CHECK(page->status == 200);
    CHECK(page->body == follows_page_body);

    const auto statuses = client.get_authorized(md::AuthorizedPath::manga_status, 0, good, {});
    REQUIRE(statuses.has_value());
    CHECK(statuses->status == 200);
    CHECK(statuses->body == status_map_body);

    // An unknown token: the API's 401, unmapped.
    const md::Secret unknown = md::Secret::copy_of(mdx::bearer("fixture-access-unknown"));
    const auto refused = client.get_authorized(md::AuthorizedPath::manga_status, 0, unknown, {});
    REQUIRE(refused.has_value());
    CHECK(refused->status == 401);

    // The plain fetches still work: the fixture would answer a leaked header with a local error.
    const auto id = md::MangaId::parse(mdx::clean_fixture().id);
    REQUIRE(id.has_value());
    const auto info = client.fetch_manga(*id, {});
    REQUIRE(info.has_value());
    CHECK(client.fetch_feed(*id, "en", {}).has_value());
    CHECK(client.fetch_covers(*id, {}).has_value());

    for (const std::string& line : trace.lines()) {
        CHECK_THAT(line, !ContainsSubstring("Bearer"));
        CHECK_THAT(line, !ContainsSubstring(std::string{mdx::fixture_access_1}));
    }
}
