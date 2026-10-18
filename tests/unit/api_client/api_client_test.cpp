// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/api_client/api_client_test.cpp: the MangaDex API client (download plan T12 + rev1).
// endpoints_for; status mapping (404 title -> not_found, 400/403 -> network with a hint, never
// retried); the retry loop (429 with each header form, 5xx backoff with jitter, transport errors,
// the attempt ceiling); feed and cover pagination (3 pages, the offset cap, absent or hostile
// totals); at-home and page fetches (limiters, override base, image-host 403); MD@H reports on
// every image attempt; stop requests; tracing; concurrent use (also under TSan); and a real fixture
// tree through make_fixture_transport.

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_transport.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
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
constexpr std::string_view report_url = "https://api.mangadex.network/report";
constexpr std::string_view node = "https://abc123.node.example"; ///< an MD@H node
constexpr std::string_view hash = "abcdef0123456789";

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

/// Records the arguments the client passes to its jitter function and answers `answer`.
struct JitterLog {
    std::mutex mutex{};
    std::vector<std::uint32_t> args{}; ///< guarded by mutex
    std::uint32_t answer = 0;
};

/// The returned function is stored in the client: `log` must outlive it (every test declares the
/// log before the client).
[[nodiscard]] std::function<std::uint32_t(std::uint32_t)> recording_jitter(JitterLog* log) {
    return [log](std::uint32_t max) {
        const std::scoped_lock lock{log->mutex};
        log->args.push_back(max);
        return log->answer;
    };
}

/// No limiter spacing, so the only sleeps are retry waits.
[[nodiscard]] md::ClientConfig unlimited_config() {
    return md::ClientConfig{.api_interval = milliseconds{0}, .at_home_interval = milliseconds{0}};
}

[[nodiscard]] md::ClientConfig reporting_config() {
    md::ClientConfig config = unlimited_config();
    config.endpoints.report_url = std::string{report_url};
    return config;
}

[[nodiscard]] md::MangaId clean_id() {
    const auto id = md::MangaId::parse(mdx::clean_fixture().id);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] md::AtHomeServer node_server() {
    return md::AtHomeServer{.base_url = std::string{node},
                            .hash = std::string{hash},
                            .data = {"p001.png", "p002.png"},
                            .data_saver = {"s001.png", "s002.png"}};
}

[[nodiscard]] std::string node_page_url(std::string_view file) {
    return md::image_url(node, md::ImageQuality::data, hash, file);
}

/// The 429 answer with the given rate-limit headers.
[[nodiscard]] md::HttpResponse too_many(std::string epoch = {}, std::string retry_after = {}) {
    return md::HttpResponse{.status = 429,
                            .retry_after = std::move(retry_after),
                            .x_ratelimit_retry_after = std::move(epoch)};
}

[[nodiscard]] std::vector<milliseconds> requested_sleeps(const FakeClock& clock) {
    std::vector<milliseconds> out;
    for (const md::test::SleepRecord& sleep : clock.sleeps()) {
        out.push_back(sleep.requested);
    }
    return out;
}

[[nodiscard]] std::vector<RecordedRequest> posts(const FakeTransport& transport) {
    std::vector<RecordedRequest> out;
    for (const RecordedRequest& request : transport.requests()) {
        if (request.method == md::HttpMethod::post) {
            out.push_back(request);
        }
    }
    return out;
}

template <class T> [[nodiscard]] std::optional<md::Errc> error_code(const md::Result<T>& result) {
    if (result.has_value()) {
        return std::nullopt;
    }
    return result.error().code;
}

template <class T> [[nodiscard]] std::string error_message(const md::Result<T>& result) {
    return result.has_value() ? std::string{} : result.error().message;
}

/// A minimal feed entry the parser accepts.
[[nodiscard]] std::string minimal_chapter_json(std::uint64_t n) {
    return std::format(R"({{"id":"{}","attributes":{{"pages":1}}}})",
                       mdx::detail::fixture_uuid('3', mdx::detail::IdKind::chapter, n));
}

} // namespace

// ---- endpoints_for
// --------------------------------------------------------------------------------

TEST_CASE("ApiClient: endpoints_for maps each API base kind", "[api_client]") {
    const md::ApiBase official{};
    CHECK(md::endpoints_for(official, false) == md::Endpoints{});
    CHECK(md::endpoints_for(official, true) ==
          md::Endpoints{.report_url = std::string{report_url}});

    const auto https = md::ApiBase::parse("https://mirror.example/api/");
    REQUIRE(https.has_value());
    CHECK(md::endpoints_for(*https, true) == md::Endpoints{.api = "https://mirror.example/api"});

    const auto loopback = md::ApiBase::parse("http://127.0.0.1:8080");
    REQUIRE(loopback.has_value());
    CHECK(md::endpoints_for(*loopback, true) == md::Endpoints{.api = "http://127.0.0.1:8080"});

    const auto fixture = md::ApiBase::parse("file:///srv/fixtures/clean/");
    REQUIRE(fixture.has_value());
    CHECK(md::endpoints_for(*fixture, true) ==
          md::Endpoints{.api = "file:///srv/fixtures/clean/api",
                        .uploads = "file:///srv/fixtures/clean/uploads",
                        .at_home_override = "file:///srv/fixtures/clean/at-home"});
}

// ---- fetch_manga and status mapping
// ----------------------------------------------------------------

TEST_CASE("ApiClient: fetch_manga parses the title and caps the JSON body", "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, md::ClientConfig{}, {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    transport.push_status(url, 200, mdx::manga_json(mdx::clean_fixture()).dump());

    const auto info = client.fetch_manga(id, {});
    REQUIRE(info.has_value());
    CHECK(info->id == id.str());
    CHECK(info->original_language == "ja");
    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0] == RecordedRequest{.method = md::HttpMethod::get,
                                         .url = url,
                                         .max_body_bytes = md::max_api_json_bytes});
    CHECK(clock.sleeps().empty());
}

TEST_CASE("ApiClient: a 404 on the title is not_found, never retried", "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    transport.push_status(url, 404, R"({"result":"error"})");

    const auto info = client.fetch_manga(id, {});
    CHECK(error_code(info) == md::Errc::not_found);
    CHECK(error_message(info) == std::format("title {} not found on MangaDex (HTTP 404); check "
                                             "the UUID or URL",
                                             id.str()));
    CHECK(transport.count(url) == 1);
    CHECK(clock.sleeps().empty());
}

TEST_CASE("ApiClient: the 404 message echoes the canonical id, never the raw argument",
          "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const std::string raw = "https://mangadex.org/title/AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE/Slug";
    const auto id = md::MangaId::parse(raw);
    REQUIRE(id.has_value());
    transport.push_status(md::manga_request_url(api, *id), 404);

    const auto info = client.fetch_manga(*id, {});
    CHECK(error_code(info) == md::Errc::not_found);
    CHECK(error_message(info) == "title aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee not found on MangaDex "
                                 "(HTTP 404); check the UUID or URL");
    CHECK(!error_message(info).contains("Slug"));
}

TEST_CASE("ApiClient: 400 and 403 are network errors with a hint, never retried", "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    JitterLog jitter;
    md::ApiClient client{transport, clock, unlimited_config(), recording_jitter(&jitter), nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);

    SECTION("403") {
        transport.push_status(url, 403);
        const auto info = client.fetch_manga(id, {});
        CHECK(error_code(info) == md::Errc::network);
        CHECK_THAT(error_message(info), ContainsSubstring(url + ": HTTP 403") &&
                                            ContainsSubstring("temporary rate-limit ban"));
    }
    SECTION("400") {
        transport.push_status(url, 400);
        const auto info = client.fetch_manga(id, {});
        CHECK(error_code(info) == md::Errc::network);
        CHECK_THAT(error_message(info), ContainsSubstring(url + ": HTTP 400 (bad request"));
    }
    CHECK(transport.count(url) == 1);
    CHECK(clock.sleeps().empty());
    CHECK(jitter.args.empty());
}

TEST_CASE("ApiClient: malformed or oversized JSON is a network error, never retried",
          "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);

    SECTION("malformed") {
        transport.push_status(url, 200, R"({"data": [)");
        const auto info = client.fetch_manga(id, {});
        CHECK(error_code(info) == md::Errc::network);
        CHECK_THAT(error_message(info), ContainsSubstring(url + ": malformed"));
    }
    SECTION("too large") {
        transport.push_error(url, md::TransportErrc::too_large, "body exceeds the cap");
        const auto info = client.fetch_manga(id, {});
        CHECK(error_code(info) == md::Errc::network);
        CHECK(error_message(info) == url + ": body exceeds the cap");
    }
    CHECK(transport.count(url) == 1);
    CHECK(clock.sleeps().empty());
}

// ---- retries ------------------------------------------------------------------------------------

TEST_CASE("ApiClient: 429 without a rate-limit header waits 60 s once, then gives up",
          "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock; // wall clock starts at 2023-11-14 22:13:20 UTC
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    transport.push(url, too_many());
    transport.push(url, too_many());

    const auto info = client.fetch_manga(id, {});
    CHECK(error_code(info) == md::Errc::network);
    // The second 429 arrives at 22:14:20; the default wait ends 60 s later.
    CHECK(error_message(info) == url + ": HTTP 429 (rate limited until 2023-11-14 22:15:20 UTC)");
    CHECK(transport.count(url) == 2);
    CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{60'000}});
}

TEST_CASE("ApiClient: 429 honours X-RateLimit-Retry-After, then Retry-After",
          "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    const std::string body = mdx::manga_json(mdx::clean_fixture()).dump();

    SECTION("epoch header") {
        transport.push(url, too_many("1700000005", "30"));
        transport.push_status(url, 200, body);
        REQUIRE(client.fetch_manga(id, {}).has_value());
        CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{5000}});
    }
    SECTION("an epoch in the past retries at once") {
        transport.push(url, too_many("1600000000"));
        transport.push_status(url, 200, body);
        REQUIRE(client.fetch_manga(id, {}).has_value());
        CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{0}});
    }
    SECTION("Retry-After fallback") {
        transport.push(url, too_many("garbage", "7"));
        transport.push_status(url, 200, body);
        REQUIRE(client.fetch_manga(id, {}).has_value());
        CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{7000}});
    }
    SECTION("a wait beyond the 60 s cap is not retried") {
        transport.push(url, too_many("1700000120"));
        const auto info = client.fetch_manga(id, {});
        CHECK(error_code(info) == md::Errc::network);
        CHECK(error_message(info) ==
              url + ": HTTP 429 (rate limited until 2023-11-14 22:15:20 UTC)");
        CHECK(clock.sleeps().empty());
        CHECK(transport.count(url) == 1);
    }
    CHECK(transport.drained());
}

TEST_CASE("ApiClient: 5xx backs off 1/2/4 s plus jitter, then maps to network",
          "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock;
    JitterLog jitter;
    jitter.answer = 123;
    md::ApiClient client{transport, clock, unlimited_config(), recording_jitter(&jitter), nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    for (int i = 0; i < 4; ++i) {
        transport.push_status(url, 503);
    }

    const auto info = client.fetch_manga(id, {});
    CHECK(error_code(info) == md::Errc::network);
    CHECK(error_message(info) == url + ": HTTP 503 (server error)");
    CHECK(transport.count(url) == 4);
    CHECK(requested_sleeps(clock) ==
          std::vector<milliseconds>{milliseconds{1123}, milliseconds{2123}, milliseconds{4123}});
    CHECK(jitter.args == std::vector<std::uint32_t>{500, 500, 500});
}

TEST_CASE("ApiClient: a retried 5xx or timeout can still succeed", "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr}; // no jitter function
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    transport.push_status(url, 502);
    transport.push_error(url, md::TransportErrc::timeout);
    transport.push_error(url, md::TransportErrc::protocol);
    transport.push_status(url, 200, mdx::manga_json(mdx::clean_fixture()).dump());

    REQUIRE(client.fetch_manga(id, {}).has_value());
    CHECK(requested_sleeps(clock) ==
          std::vector<milliseconds>{milliseconds{1000}, milliseconds{2000}, milliseconds{4000}});
    CHECK(transport.drained());
}

TEST_CASE("ApiClient: connection refused and other hard transport errors are not retried",
          "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock;
    JitterLog jitter;
    md::ApiClient client{transport, clock, unlimited_config(), recording_jitter(&jitter), nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    transport.push_error(url, md::TransportErrc::connect, "connection refused");

    const auto info = client.fetch_manga(id, {});
    CHECK(error_code(info) == md::Errc::network);
    CHECK(error_message(info) == url + ": connection refused");
    CHECK(transport.count(url) == 1);
    CHECK(clock.sleeps().empty());
    CHECK(jitter.args.empty());
}

TEST_CASE("ApiClient: the jitter bound is narrowed safely and the answer clamped",
          "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock;
    JitterLog jitter;
    md::ClientConfig config = unlimited_config();
    config.retry.max_attempts = 2;
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);
    transport.push_status(url, 500);
    transport.push_status(url, 500);

    SECTION("larger than uint32_t") {
        config.retry.max_jitter = milliseconds{1'099'511'627'776}; // 2^40
        jitter.answer = 7;
        md::ApiClient client{transport, clock, config, recording_jitter(&jitter), nullptr};
        CHECK(error_code(client.fetch_manga(id, {})) == md::Errc::network);
        CHECK(jitter.args == std::vector<std::uint32_t>{std::numeric_limits<std::uint32_t>::max()});
        CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{1007}});
    }
    SECTION("zero or negative: the function is never called") {
        config.retry.max_jitter = GENERATE(milliseconds{0}, milliseconds{-5});
        md::ApiClient client{transport, clock, config, recording_jitter(&jitter), nullptr};
        CHECK(error_code(client.fetch_manga(id, {})) == md::Errc::network);
        CHECK(jitter.args.empty());
        CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{1000}});
    }
    SECTION("an answer above the bound is clamped") {
        jitter.answer = 100'000;
        md::ApiClient client{transport, clock, config, recording_jitter(&jitter), nullptr};
        CHECK(error_code(client.fetch_manga(id, {})) == md::Errc::network);
        CHECK(requested_sleeps(clock) == std::vector<milliseconds>{milliseconds{1500}});
    }
}

TEST_CASE("ApiClient: a huge max_attempts is still bounded", "[api_client][retry]") {
    FakeTransport transport;
    FakeClock clock;
    md::ClientConfig config = unlimited_config();
    config.retry.max_attempts = std::numeric_limits<int>::max();
    config.retry.base_delay = milliseconds{1};
    config.retry.max_jitter = milliseconds{0};
    md::ApiClient client{transport, clock, config, {}, nullptr};
    transport.set_fallback(md::HttpResponse{.status = 503});
    const md::MangaId id = clean_id();

    CHECK(error_code(client.fetch_manga(id, {})) == md::Errc::network);
    CHECK(transport.count(md::manga_request_url(api, id)) == 64);
    CHECK(clock.sleeps().size() == 63);
}

// ---- stop requests ------------------------------------------------------------------------------

TEST_CASE("ApiClient: a stop request gives interrupted", "[api_client][stop]") {
    std::stop_source source; // declared first: the hooks below borrow it
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, reporting_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::manga_request_url(api, id);

    SECTION("before the call: nothing is sent") {
        source.request_stop();
        CHECK(error_code(client.fetch_manga(id, source.get_token())) == md::Errc::interrupted);
        CHECK(transport.requests().empty());
    }
    SECTION("during a backoff sleep") {
        transport.push_status(url, 503);
        // The hook runs on this thread; `source` outlives the clock (declared before it).
        clock.set_on_sleep([src = &source] { static_cast<void>(src->request_stop()); });
        CHECK(error_code(client.fetch_manga(id, source.get_token())) == md::Errc::interrupted);
        CHECK(transport.count(url) == 1);
        REQUIRE(clock.sleeps().size() == 1);
        CHECK(clock.sleeps()[0].stopped);
    }
    SECTION("during a page fetch: no report is sent for it") {
        const std::string page = node_page_url("p001.png");
        transport.set_on_fetch(
            [src = &source](const md::HttpRequest&) { static_cast<void>(src->request_stop()); });
        CHECK(error_code(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png",
                                           source.get_token())) == md::Errc::interrupted);
        CHECK(transport.count(page) == 1);
        CHECK(posts(transport).empty());
    }
    SECTION("while waiting for the rate limiter") {
        md::ApiClient limited{transport, clock, md::ClientConfig{}, {}, nullptr};
        transport.push_status(url, 200, mdx::manga_json(mdx::clean_fixture()).dump());
        REQUIRE(limited.fetch_manga(id, source.get_token()).has_value());
        clock.set_on_sleep([src = &source] { static_cast<void>(src->request_stop()); });
        CHECK(error_code(limited.fetch_manga(id, source.get_token())) == md::Errc::interrupted);
        CHECK(transport.count(url) == 1);
    }
}

// ---- pagination ---------------------------------------------------------------------------------

TEST_CASE("ApiClient: the feed is paginated across three pages", "[api_client][feed]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const mdx::FixtureManga manga = mdx::clean_fixture();
    const md::MangaId id = clean_id();
    const std::vector<nlohmann::json> pages = mdx::feed_pages_json(manga, 1); // 1 chapter each
    REQUIRE(pages.size() == 3);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        transport.push_status(md::feed_request_url(api, id, "en", i), 200, pages[i].dump());
    }

    const auto chapters = client.fetch_feed(id, "en", {});
    REQUIRE(chapters.has_value());
    REQUIRE(chapters->size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK((*chapters)[i].id == manga.chapters[i].id);
    }
    CHECK(transport.drained());
    CHECK(transport.requests().size() == 3);
}

TEST_CASE("ApiClient: the feed URL carries every explicit parameter", "[api_client][feed]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    transport.set_fallback(md::HttpResponse{.status = 200, .body = R"({"data":[],"total":0})"});

    REQUIRE(client.fetch_feed(id, "pt-br", {}).has_value());
    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 1);
    const std::string& url = requests[0].url;
    CHECK(url.starts_with(std::format("{}/manga/{}/feed?", api, id.str())));
    for (const std::string_view param :
         {"translatedLanguage[]=pt-br", "limit=500", "offset=0", "order[volume]=asc",
          "order[chapter]=asc", "includes[]=scanlation_group", "contentRating[]=safe",
          "contentRating[]=suggestive", "contentRating[]=erotica", "contentRating[]=pornographic",
          "includeExternalUrl=1", "includeEmptyPages=1", "includeFuturePublishAt=0",
          "includeUnavailable=1"}) {
        CHECK_THAT(url, ContainsSubstring(std::string{param}));
    }
    CHECK(requests[0].max_body_bytes == md::max_api_json_bytes);
}

TEST_CASE("ApiClient: the feed offset cap refuses to truncate", "[api_client][feed]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string first = md::feed_request_url(api, id, "en", 0);
    const auto expect_refusal = [&client, &transport, &id] {
        const auto chapters = client.fetch_feed(id, "en", {});
        CHECK(error_code(chapters) == md::Errc::network);
        CHECK(error_message(chapters) ==
              "title has more than 10000 chapters in this language; refusing to truncate");
        CHECK(transport.requests().size() == 1);
    };

    SECTION("total above the cap") {
        const std::string total = GENERATE(as<std::string>{}, "10001", "18446744073709551615");
        transport.push_status(first, 200,
                              std::format(R"({{"data":[{}],"limit":500,"offset":0,"total":{}}})",
                                          minimal_chapter_json(1), total));
        expect_refusal();
    }
    SECTION("the next window would cross the cap") {
        // 9501 chapters on the first page: the next request would be offset 9501 + limit 500.
        std::string data;
        for (std::uint64_t n = 0; n < 9501; ++n) {
            if (n != 0) {
                data.push_back(',');
            }
            data += minimal_chapter_json(n);
        }
        transport.push_status(
            first, 200,
            std::format(R"({{"data":[{}],"limit":500,"offset":0,"total":10000}})", data));
        expect_refusal();
    }
}

TEST_CASE("ApiClient: odd window fields always stop the pagination", "[api_client][feed]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string first = md::feed_request_url(api, id, "en", 0);

    SECTION("total, limit and offset absent") {
        transport.push_status(first, 200,
                              std::format(R"({{"data":[{}]}})", minimal_chapter_json(1)));
        const auto chapters = client.fetch_feed(id, "en", {});
        REQUIRE(chapters.has_value());
        CHECK(chapters->size() == 1);
    }
    SECTION("an empty page although total says more") {
        transport.push_status(
            first, 200, std::format(R"({{"data":[{}],"total":50}})", minimal_chapter_json(1)));
        transport.push_status(md::feed_request_url(api, id, "en", 1), 200,
                              R"({"data":[],"limit":0,"offset":18446744073709551615,"total":50})");
        const auto chapters = client.fetch_feed(id, "en", {});
        REQUIRE(chapters.has_value());
        CHECK(chapters->size() == 1);
    }
    SECTION("total smaller than what was received") {
        transport.push_status(first, 200,
                              std::format(R"({{"data":[{},{}],"limit":1,"total":1}})",
                                          minimal_chapter_json(1), minimal_chapter_json(2)));
        const auto chapters = client.fetch_feed(id, "en", {});
        REQUIRE(chapters.has_value());
        CHECK(chapters->size() == 2);
    }
    CHECK(transport.drained());
}

TEST_CASE("ApiClient: covers are paginated at 100", "[api_client][covers]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    mdx::FixtureManga manga = mdx::clean_fixture();
    manga.covers.clear();
    for (std::uint64_t n = 1; n <= 150; ++n) {
        manga.covers.push_back({.id = mdx::detail::fixture_uuid('1', mdx::detail::IdKind::cover, n),
                                .volume = std::to_string(n),
                                .file = std::format("c{:03}.png", n),
                                .locale = "ja"});
    }
    const md::MangaId id = clean_id();
    const std::vector<nlohmann::json> pages = mdx::covers_json(manga, md::cover_page_limit);
    REQUIRE(pages.size() == 2);
    transport.push_status(md::cover_request_url(api, id, 0), 200, pages[0].dump());
    transport.push_status(md::cover_request_url(api, id, 100), 200, pages[1].dump());

    const auto covers = client.fetch_covers(id, {});
    REQUIRE(covers.has_value());
    REQUIRE(covers->size() == 150);
    CHECK(covers->front().file_name == "c001.png");
    CHECK(covers->back().file_name == "c150.png");
    CHECK(transport.drained());
}

// ---- at-home, pages and covers
// --------------------------------------------------------------------

TEST_CASE("ApiClient: fetch_at_home waits for both limiters", "[api_client][at-home]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, md::ClientConfig{}, {}, nullptr}; // 250 ms and 1500 ms
    const mdx::FixtureChapter chapter = mdx::clean_fixture().chapters.front();
    const std::string url = md::at_home_request_url(api, chapter.id);
    transport.push_status(url, 200, mdx::at_home_json(chapter).dump());
    transport.push_status(url, 200, mdx::at_home_json(chapter).dump());
    const auto start = clock.now();

    const auto first = client.fetch_at_home(chapter.id, {});
    const auto second = client.fetch_at_home(chapter.id, {});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->base_url == mdx::at_home_base_url); // the override applies in fetch_page only
    CHECK(first->hash == chapter.hash);
    CHECK(first->data.size() == chapter.images.size());
    const std::vector<md::test::SleepRecord> sleeps = clock.sleeps();
    REQUIRE(sleeps.size() == 2);
    CHECK(sleeps[0].deadline == start + milliseconds{250});
    CHECK(sleeps[1].deadline == start + milliseconds{1500});
}

TEST_CASE("ApiClient: fetch_page builds the image URL and caps the body", "[api_client][page]") {
    FakeTransport transport;
    FakeClock clock;
    md::ClientConfig config = unlimited_config();

    SECTION("data and data-saver from the server's base") {
        md::ApiClient client{transport, clock, config, {}, nullptr};
        transport.push_status(node_page_url("p001.png"), 200, "page one");
        transport.push_status(md::image_url(node, md::ImageQuality::data_saver, hash, "s002.png"),
                              200, "saver two");
        const auto one = client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", {});
        const auto two =
            client.fetch_page(node_server(), md::ImageQuality::data_saver, "s002.png", {});
        REQUIRE(one.has_value());
        REQUIRE(two.has_value());
        CHECK(*one == "page one");
        CHECK(*two == "saver two");
        for (const RecordedRequest& request : transport.requests()) {
            CHECK(request.max_body_bytes == md::max_image_bytes);
        }
    }
    SECTION("the at-home override replaces the base") {
        config.endpoints.at_home_override = "file:///srv/fx/at-home";
        md::ApiClient client{transport, clock, config, {}, nullptr};
        transport.push_status("file:///srv/fx/at-home/data/abcdef0123456789/p002.png", 200, "x");
        const auto page = client.fetch_page(node_server(), md::ImageQuality::data, "p002.png", {});
        REQUIRE(page.has_value());
        CHECK(*page == "x");
    }
    CHECK(transport.drained());
}

TEST_CASE("ApiClient: unsafe page and cover inputs never reach the transport",
          "[api_client][page]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    md::AtHomeServer server = node_server();

    SECTION("file name") {
        CHECK(error_code(client.fetch_page(server, md::ImageQuality::data, "../p001.png", {})) ==
              md::Errc::network);
    }
    SECTION("hash") {
        server.hash = "a/b";
        CHECK(error_code(client.fetch_page(server, md::ImageQuality::data, "p001.png", {})) ==
              md::Errc::network);
    }
    SECTION("base URL") {
        server.base_url = "http://node.example";
        CHECK(error_code(client.fetch_page(server, md::ImageQuality::data, "p001.png", {})) ==
              md::Errc::network);
    }
    SECTION("cover file name") {
        CHECK(error_code(client.fetch_cover(clean_id(), ".hidden", {})) == md::Errc::network);
    }
    CHECK(transport.requests().empty());
}

TEST_CASE("ApiClient: an image-host 403 returns at once", "[api_client][page]") {
    FakeTransport transport;
    FakeClock clock;
    JitterLog jitter;
    md::ApiClient client{transport, clock, unlimited_config(), recording_jitter(&jitter), nullptr};
    const std::string url = node_page_url("p001.png");
    transport.push_status(url, 403);

    const auto page = client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", {});
    CHECK(error_code(page) == md::Errc::network);
    CHECK_THAT(error_message(page), ContainsSubstring(url + ": HTTP 403"));
    CHECK(transport.count(url) == 1);
    CHECK(clock.sleeps().empty());
    CHECK(jitter.args.empty());
}

TEST_CASE("ApiClient: fetch_cover reads from the uploads host", "[api_client][covers]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, reporting_config(), {}, nullptr};
    const md::MangaId id = clean_id();
    const std::string url = md::cover_image_url("https://uploads.mangadex.org", id, "cover.png");
    transport.push_status(url, 200, "cover bytes");

    const auto cover = client.fetch_cover(id, "cover.png", {});
    REQUIRE(cover.has_value());
    CHECK(*cover == "cover bytes");
    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 1); // no MD@H report for covers
    CHECK(requests[0].max_body_bytes == md::max_image_bytes);
}

// ---- MD@H reports
// ---------------------------------------------------------------------------------

TEST_CASE("ApiClient: every image attempt from an MD@H node is reported", "[api_client][report]") {
    FakeClock clock; // declared first: the transport's hook borrows it
    FakeTransport transport;
    md::ApiClient client{transport, clock, reporting_config(), {}, nullptr};
    const std::string url = node_page_url("p001.png");
    // Each GET takes 42 ms of virtual time; the report POSTs take none.
    transport.set_on_fetch([clk = &clock](const md::HttpRequest& request) {
        if (request.method == md::HttpMethod::get) {
            clk->advance(milliseconds{42});
        }
    });
    transport.set_fallback(md::HttpResponse{.status = 200}); // the report endpoint's answer

    const auto report_body = [&url](bool success, bool cached, std::uint64_t bytes) {
        return md::at_home_report_json(md::AtHomeReport{
            .url = url, .success = success, .cached = cached, .bytes = bytes, .duration_ms = 42});
    };

    SECTION("success, cached") {
        transport.push(url, md::HttpResponse{.status = 200, .body = "12345", .x_cache = "HIT"});
        REQUIRE(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", {}));
        const std::vector<RecordedRequest> sent = posts(transport);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].url == report_url);
        CHECK(sent[0].content_type == "application/json");
        CHECK(sent[0].body == report_body(true, true, 5));
    }
    SECTION("a 404 reports a failure with zero bytes") {
        transport.push(url, md::HttpResponse{.status = 404, .body = "nope", .x_cache = "HIT"});
        CHECK(error_code(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png",
                                           {})) == md::Errc::network);
        const std::vector<RecordedRequest> sent = posts(transport);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].body == report_body(false, false, 0));
    }
    SECTION("a transport error reports a failure with zero bytes") {
        transport.push_error(url, md::TransportErrc::tls, "certificate verify failed");
        CHECK(error_code(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png",
                                           {})) == md::Errc::network);
        const std::vector<RecordedRequest> sent = posts(transport);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].body == report_body(false, false, 0));
    }
    SECTION("a retried attempt is reported each time") {
        transport.push_status(url, 503);
        transport.push(url, md::HttpResponse{.status = 200, .body = "123", .x_cache = "MISS"});
        REQUIRE(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", {}));
        const std::vector<RecordedRequest> sent = posts(transport);
        REQUIRE(sent.size() == 2);
        CHECK(sent[0].body == report_body(false, false, 0));
        CHECK(sent[1].body == report_body(true, false, 3));
    }
    SECTION("a failing report is never retried and does not fail the page") {
        transport.push_status(url, 200, "ok");
        transport.push_status(std::string{report_url}, 503);
        REQUIRE(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", {}));
        CHECK(posts(transport).size() == 1);
        CHECK(clock.sleeps().empty());
    }
    CHECK(transport.drained());
}

TEST_CASE("ApiClient: report POSTs use a 5 s timeout and the stop token", "[api_client][report]") {
    struct Seen {
        std::mutex mutex{};
        std::vector<md::HttpRequest> posts{}; ///< guarded by mutex
    };
    Seen seen; // declared first: the transport's hook borrows it
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, reporting_config(), {}, nullptr};
    transport.set_on_fetch([out = &seen](const md::HttpRequest& request) {
        if (request.method == md::HttpMethod::post) {
            const std::scoped_lock lock{out->mutex};
            out->posts.push_back(request);
        }
    });
    transport.set_fallback(md::HttpResponse{.status = 200, .body = "img"});
    const std::stop_source source;

    REQUIRE(
        client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", source.get_token()));
    REQUIRE(seen.posts.size() == 1);
    CHECK(seen.posts[0].connect_timeout == std::chrono::seconds{5});
    CHECK(seen.posts[0].stall_timeout == std::chrono::seconds{5});
    source.request_stop();
    CHECK(seen.posts[0].cancel.stop_requested());
}

TEST_CASE("ApiClient: no report for mangadex.org hosts or without a report URL",
          "[api_client][report]") {
    FakeTransport transport;
    FakeClock clock;
    transport.set_fallback(md::HttpResponse{.status = 200, .body = "img"});
    md::AtHomeServer server = node_server();

    SECTION("mangadex.org host") {
        md::ApiClient client{transport, clock, reporting_config(), {}, nullptr};
        server.base_url = "https://uploads.mangadex.org";
        REQUIRE(client.fetch_page(server, md::ImageQuality::data, "p001.png", {}));
    }
    SECTION("no report URL") {
        md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
        REQUIRE(client.fetch_page(server, md::ImageQuality::data, "p001.png", {}));
    }
    CHECK(transport.requests().size() == 1);
    CHECK(posts(transport).empty());
}

// ---- tracing ------------------------------------------------------------------------------------

TEST_CASE("ApiClient: every request is traced", "[api_client][trace]") {
    FakeTransport transport;
    FakeClock clock;
    RecordingTrace trace;
    md::ApiClient client{transport, clock, reporting_config(), {}, &trace};
    const std::string url = node_page_url("p001.png");
    transport.push_error(url, md::TransportErrc::timeout, "stalled for 30 s");
    transport.push_status(url, 200, "img");
    transport.set_fallback(md::HttpResponse{.status = 204});

    REQUIRE(client.fetch_page(node_server(), md::ImageQuality::data, "p001.png", {}));
    CHECK(trace.lines() ==
          std::vector<std::string>{std::format("GET {} -> error: stalled for 30 s (0 ms)", url),
                                   std::format("POST {} -> 204 (0 ms)", report_url),
                                   std::format("GET {} -> 200 (0 ms)", url),
                                   std::format("POST {} -> 204 (0 ms)", report_url)});
}

// ---- concurrency --------------------------------------------------------------------------------

TEST_CASE("ApiClient: concurrent page fetches skip the API limiter unless on mangadex.org",
          "[api_client][threads]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, md::ClientConfig{}, {}, nullptr}; // 250 ms spacing
    transport.set_fallback(md::HttpResponse{.status = 200, .body = "img"});
    md::AtHomeServer server = node_server();
    const auto start = clock.now();
    constexpr std::size_t threads = 4;
    constexpr int per_thread = 5;
    constexpr int total = 20; // threads * per_thread
    const auto fetch_concurrently = [&client, &server] {
        std::vector<int> ok(threads, 0);
        {
            std::vector<std::jthread> workers;
            workers.reserve(threads);
            for (std::size_t t = 0; t < threads; ++t) {
                // Each worker writes only its own slot; all are joined before `ok` is read.
                workers.emplace_back([client_ptr = &client, server_ptr = &server, slot = &ok[t]] {
                    for (int i = 0; i < per_thread; ++i) {
                        if (client_ptr
                                ->fetch_page(*server_ptr, md::ImageQuality::data, "p001.png", {})
                                .has_value()) {
                            ++*slot;
                        }
                    }
                });
            }
        }
        return ok;
    };

    SECTION("an MD@H node: no limiter sleeps") {
        CHECK(fetch_concurrently() == std::vector<int>(threads, per_thread));
        CHECK(clock.sleeps().empty());
        CHECK(transport.requests().size() == static_cast<std::size_t>(total));
    }
    SECTION("a mangadex.org host: every fetch gets its own 250 ms slot") {
        server.base_url = "https://cdn.mangadex.org";
        CHECK(fetch_concurrently() == std::vector<int>(threads, per_thread));
        // The first acquisition is immediate; every other one sleeps to its own slot.
        std::vector<std::chrono::steady_clock::time_point> deadlines;
        for (const md::test::SleepRecord& sleep : clock.sleeps()) {
            deadlines.push_back(sleep.deadline);
        }
        std::ranges::sort(deadlines);
        std::vector<std::chrono::steady_clock::time_point> expected;
        for (int k = 1; k < total; ++k) {
            expected.push_back(start + (milliseconds{250} * k));
        }
        CHECK(deadlines == expected);
    }
}

TEST_CASE("ApiClient: API documents and covers keep the API limiter", "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, md::ClientConfig{}, {}, nullptr}; // 250 ms spacing
    const md::MangaId id = clean_id();
    const auto start = clock.now();

    SECTION("manga") {
        const std::string body = mdx::manga_json(mdx::clean_fixture()).dump();
        transport.push_status(md::manga_request_url(api, id), 200, body);
        transport.push_status(md::manga_request_url(api, id), 200, body);
        REQUIRE(client.fetch_manga(id, {}).has_value());
        REQUIRE(client.fetch_manga(id, {}).has_value());
    }
    SECTION("covers") {
        const std::string url =
            md::cover_image_url("https://uploads.mangadex.org", id, "cover.png");
        transport.push_status(url, 200, "one");
        transport.push_status(url, 200, "two");
        REQUIRE(client.fetch_cover(id, "cover.png", {}).has_value());
        REQUIRE(client.fetch_cover(id, "cover.png", {}).has_value());
    }
    const std::vector<md::test::SleepRecord> sleeps = clock.sleeps();
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps[0].deadline == start + milliseconds{250});
    CHECK(transport.drained());
}

TEST_CASE("ApiClient: malformed at-home documents and other statuses map to network",
          "[api_client]") {
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, unlimited_config(), {}, nullptr};
    const md::MangaId id = clean_id();

    SECTION("a malformed at-home document is prefixed with its URL") {
        const std::string chapter = mdx::clean_fixture().chapters.front().id;
        const std::string url = md::at_home_request_url(api, chapter);
        transport.push_status(url, 200, R"({"baseUrl": )");
        const auto server = client.fetch_at_home(chapter, {});
        CHECK(error_code(server) == md::Errc::network);
        CHECK_THAT(error_message(server), ContainsSubstring(url + ": malformed"));
        CHECK(transport.count(url) == 1);
    }
    SECTION("an unexpected status is reported as is") {
        const std::string url = md::cover_image_url("https://uploads.mangadex.org", id, "c.png");
        transport.push_status(url, 418, "teapot");
        const auto cover = client.fetch_cover(id, "c.png", {});
        CHECK(error_code(cover) == md::Errc::network);
        CHECK(error_message(cover) == url + ": HTTP 418");
        CHECK(transport.count(url) == 1);
    }
    CHECK(clock.sleeps().empty());
}

TEST_CASE("ApiClient: concurrent retries, reports, jitter and tracing", "[api_client][threads]") {
    FakeTransport transport;
    FakeClock clock;
    RecordingTrace trace;
    JitterLog jitter;
    jitter.answer = 1;
    md::ApiClient client{transport, clock, reporting_config(), recording_jitter(&jitter), &trace};
    constexpr std::size_t threads = 4;
    const md::AtHomeServer server = node_server();
    std::vector<std::string> files;
    for (std::size_t t = 0; t < threads; ++t) {
        files.push_back(std::format("p{:03}.png", t + 1));
        const std::string url = node_page_url(files.back());
        transport.push_status(url, 503);
        transport.push_status(url, 200, files.back());
    }
    transport.set_fallback(md::HttpResponse{.status = 200}); // report answers
    std::vector<std::optional<std::string>> results(threads);
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            // Each worker writes only its own slot; all are joined before `results` is read.
            workers.emplace_back([client_ptr = &client, server_ptr = &server, file = files[t],
                                  slot = &results[t]] {
                auto page = client_ptr->fetch_page(*server_ptr, md::ImageQuality::data, file, {});
                if (page.has_value()) {
                    *slot = std::move(*page);
                }
            });
        }
    }
    for (std::size_t t = 0; t < threads; ++t) {
        CHECK(results[t] == files[t]);
    }
    CHECK(transport.drained());
    CHECK(posts(transport).size() == 2 * threads);
    CHECK(trace.lines().size() == 4 * threads); // 2 GETs and 2 POSTs each
    CHECK(jitter.args.size() == threads);
}

// ---- the fixture tree ---------------------------------------------------------------------------

TEST_CASE("ApiClient: fixture endpoints over make_fixture_transport", "[api_client][fixture]") {
    auto dir = md::test::TempDir::create("mangadex-dl-api-client");
    REQUIRE(dir.has_value());
    const std::filesystem::path root = std::filesystem::absolute(dir->path()) / "clean";
    const mdx::FixtureManga manga = mdx::clean_fixture();
    const auto written = mdx::write_fixture_tree(root, manga);
    REQUIRE(written.has_value());
    auto transport = md::make_fixture_transport(root);
    REQUIRE(transport.has_value());
    const auto base = md::ApiBase::parse(std::format("file://{}", root.string()));
    REQUIRE(base.has_value());
    const md::Endpoints endpoints = md::endpoints_for(*base, true);
    CHECK(!endpoints.report_url.has_value());
    const std::unique_ptr<md::Clock> clock = md::make_instant_clock();
    RecordingTrace trace;
    md::ApiClient client{**transport, *clock, md::ClientConfig{.endpoints = endpoints}, {}, &trace};
    const md::MangaId id = clean_id();

    const auto info = client.fetch_manga(id, {});
    REQUIRE(info.has_value());
    CHECK(info->id == manga.id);

    const auto chapters = client.fetch_feed(id, "en", {});
    REQUIRE(chapters.has_value());
    REQUIRE(chapters->size() == manga.chapters.size());

    const auto covers = client.fetch_covers(id, {});
    REQUIRE(covers.has_value());
    REQUIRE(covers->size() == manga.covers.size());
    for (std::size_t i = 0; i < covers->size(); ++i) {
        const auto bytes = client.fetch_cover(id, (*covers)[i].file_name, {});
        REQUIRE(bytes.has_value());
        CHECK(*bytes == mdx::image_bytes(manga.covers[i].image));
    }

    for (const mdx::FixtureChapter& chapter : manga.chapters) {
        const auto server = client.fetch_at_home(chapter.id, {});
        REQUIRE(server.has_value());
        REQUIRE(server->data.size() == chapter.images.size());
        REQUIRE(server->data_saver.size() == chapter.images.size());
        for (std::size_t i = 0; i < chapter.images.size(); ++i) {
            const std::string expected = mdx::image_bytes(chapter.images[i]);
            const auto page =
                client.fetch_page(*server, md::ImageQuality::data, server->data[i], {});
            REQUIRE(page.has_value());
            CHECK(*page == expected);
            const auto saver =
                client.fetch_page(*server, md::ImageQuality::data_saver, server->data_saver[i], {});
            REQUIRE(saver.has_value());
            CHECK(*saver == expected);
        }
    }

    const auto missing = md::MangaId::parse("99999999-9999-4999-8999-999999999999");
    REQUIRE(missing.has_value());
    CHECK(error_code(client.fetch_manga(*missing, {})) == md::Errc::not_found);
    for (const std::string& line : trace.lines()) {
        CHECK(!line.starts_with("POST"));
    }
}
