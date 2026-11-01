// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/follows/follows_fetch_test.cpp: fetch_follows_collection (follows plan T13; spec AC7,
// AC53, AC54) over FakeTransport, FakeClock and a test-local FakeTokenProvider, plus the real
// fixture transport with the fake account of support/mdx_auth_fixture.hpp.
// - The URL pin: follows_request_url / status_map_request_url (T10) are exactly the URLs
//   ApiClient::get_authorized (T11) sends, `<api>/user/follows/manga?limit=100&offset=<k>` and
//   `<api>/manga/status`, for the official API and for a fixture tree.
// - Paging: paginate's rules (step by the items received, stop on an empty page or offset >=
//   total, refuse past the 10 000 window with the spec rev3 S3 text).
// - Token handling: the provider is asked before every request (its proactive refresh lands
//   before the page that needs it); a 401 gets one refresh and one retry of that request; a second
//   401 is {refused, auth_rejected} naming "your follows" or "your reading statuses".
// - The status map: only with with_statuses and at least one follow, once, after the pages.
// - Failures: transport, HTTP status and interrupts are `request`; malformed documents and the cap
//   are `content`; a custom API base is `local` before the provider is asked for anything.
// The fetch starts an InterruptBridge poller, so these run under TSan too (`unit.follows fetch`).
//
// Every token here is an obviously fake value.

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_transport.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): SIGINT is POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using md::test::FakeClock;
using md::test::FakeTransport;
using md::test::RecordedRequest;
using std::chrono::seconds;

constexpr std::string_view fake_access_1{"fake-access-1-not-a-secret"};
constexpr std::string_view fake_access_2{"fake-access-2-not-a-secret"};
constexpr std::string_view fake_access_3{"fake-access-3-not-a-secret"};

constexpr std::string_view status_url = "https://api.mangadex.org/manga/status";
constexpr std::string_view cap_message =
    "you follow more than 10000 titles on MangaDex; refusing to truncate the list";
constexpr std::string_view follows_401_message =
    "MangaDex rejected a new access token for your follows (HTTP 401); check that the API client "
    "is approved and allowed to read your account";
constexpr std::string_view statuses_401_message =
    "MangaDex rejected a new access token for your reading statuses (HTTP 401); check that the "
    "API client is approved and allowed to read your account";

/// How long a test waits for the InterruptBridge (it polls every 50 ms). Only a failing test
/// waits this long.
constexpr std::chrono::seconds bridge_limit{20};

[[nodiscard]] std::string follows_url(std::size_t offset) {
    return std::format("https://api.mangadex.org/user/follows/manga?limit=100&offset={}", offset);
}

/// The i-th distinct fake title id.
[[nodiscard]] std::string title_id(std::size_t i) {
    return std::format("c0000000-0000-4000-8000-{:012x}", i);
}

/// `count` title ids starting at `first`.
[[nodiscard]] std::vector<std::string> title_ids(std::size_t first, std::size_t count) {
    std::vector<std::string> ids;
    ids.reserve(count);
    for (std::size_t i = first; i < first + count; ++i) {
        ids.push_back(title_id(i));
    }
    return ids;
}

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] std::vector<md::MangaId> mangas(const std::vector<std::string>& ids) {
    std::vector<md::MangaId> out;
    out.reserve(ids.size());
    for (const std::string& id : ids) {
        out.push_back(manga(id));
    }
    return out;
}

/// One follows page shaped like the API's MangaList: `result`, `limit` (100), `offset`, `total`.
[[nodiscard]] std::string page_json(const std::vector<std::string>& ids, std::size_t offset,
                                    std::size_t total) {
    nlohmann::json data = nlohmann::json::array();
    for (const std::string& id : ids) {
        data.push_back(nlohmann::json{{"id", id},
                                      {"type", "manga"},
                                      {"attributes", nlohmann::json::object()},
                                      {"relationships", nlohmann::json::array()}});
    }
    return nlohmann::json{{"result", "ok"},   {"response", "collection"},
                          {"data", data},     {"limit", md::follows_page_limit},
                          {"offset", offset}, {"total", total}}
        .dump();
}

/// A status map giving every id `status`.
[[nodiscard]] std::string status_json(const std::vector<std::string>& ids,
                                      std::string_view status = "reading") {
    nlohmann::json statuses = nlohmann::json::object();
    for (const std::string& id : ids) {
        statuses[id] = status;
    }
    return nlohmann::json{{"result", "ok"}, {"statuses", statuses}}.dump();
}

[[nodiscard]] md::StatusMap status_map(const std::vector<std::string>& ids,
                                       std::string_view status = "reading") {
    md::StatusMap map;
    for (const std::string& id : ids) {
        map.statuses.emplace(manga(id), std::string{status});
    }
    return map;
}

[[nodiscard]] md::ApiBase parsed_base(std::string_view text) {
    auto base = md::ApiBase::parse(text);
    REQUIRE(base.has_value());
    return *base;
}

/// Clears the process-wide interrupt flag on entry and exit, so test order does not matter.
class InterruptFlagReset {
public:
    InterruptFlagReset() noexcept { md::reset_interrupt_for_testing(); }
    InterruptFlagReset(const InterruptFlagReset&) = delete;
    InterruptFlagReset& operator=(const InterruptFlagReset&) = delete;
    InterruptFlagReset(InterruptFlagReset&&) = delete;
    InterruptFlagReset& operator=(InterruptFlagReset&&) = delete;
    ~InterruptFlagReset() { md::reset_interrupt_for_testing(); }
};

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

// ---- the fake token provider --------------------------------------------------------------------

enum class ProviderCall {
    authorization,     ///< authorization() was called
    proactive_refresh, ///< ...and refreshed first (the token expired within refresh_ahead)
    reactive_refresh,  ///< refresh_after_unauthorized() was called
};

struct CallRecord {
    ProviderCall call = ProviderCall::authorization;
    std::size_t requests_before = 0; ///< transport requests recorded when the call came
    std::thread::id thread{};
};

/// Hands out "Bearer <tokens[i]>": tokens[0] first, the next one after every refresh (the last
/// one repeats). authorization() refreshes first when the token expires within refresh_ahead on
/// the clock, exactly the contract LoginSession has. Records every call with the number of
/// requests the transport had seen, so a test can check the order. Not thread-safe (the fetch
/// calls it on one thread only, which the records prove).
///
/// Borrows the clock and the transport: both must outlive the provider.
class FakeTokenProvider final : public md::TokenProvider {
public:
    FakeTokenProvider(const FakeClock& clock, const FakeTransport* transport,
                      std::vector<std::string> tokens)
        : clock_(&clock), transport_(transport), tokens_(std::move(tokens)),
          expires_(clock.now() + lifetime_) {}

    [[nodiscard]] md::AuthResult<md::Secret> authorization(std::stop_token stop) override {
        record(ProviderCall::authorization);
        if (on_authorization_) {
            on_authorization_(stop);
        }
        if (stop.stop_requested()) {
            return std::unexpected(interrupted());
        }
        if (authorization_failure_.has_value()) {
            return std::unexpected(*authorization_failure_);
        }
        if (clock_->now() + md::refresh_ahead >= expires_) {
            record(ProviderCall::proactive_refresh);
            rotate();
        }
        return current();
    }

    [[nodiscard]] md::AuthResult<md::Secret>
    refresh_after_unauthorized(std::stop_token stop) override {
        record(ProviderCall::reactive_refresh);
        ++refreshes_;
        if (stop.stop_requested()) {
            return std::unexpected(interrupted());
        }
        if (refresh_failure_.has_value() && refreshes_ >= refresh_failure_from_) {
            return std::unexpected(*refresh_failure_);
        }
        rotate();
        return current();
    }

    /// The current token expires `after` from now; refreshed ones live `lifetime`.
    void set_expiry(std::chrono::steady_clock::duration after,
                    std::chrono::steady_clock::duration lifetime) {
        expires_ = clock_->now() + after;
        lifetime_ = lifetime;
    }
    void fail_authorization(md::AuthFailure failure) {
        authorization_failure_ = std::move(failure);
    }
    /// refresh_after_unauthorized() fails with `failure` from its `from`-th call on (1-based);
    /// the earlier calls refresh normally.
    void fail_refresh(md::AuthFailure failure, std::size_t from = 1) {
        refresh_failure_ = std::move(failure);
        refresh_failure_from_ = from;
    }
    /// Runs inside authorization(), before the stop token is checked.
    void set_on_authorization(std::function<void(const std::stop_token&)> hook) {
        on_authorization_ = std::move(hook);
    }

    [[nodiscard]] const std::vector<CallRecord>& calls() const noexcept { return calls_; }
    [[nodiscard]] std::vector<ProviderCall> call_kinds() const {
        std::vector<ProviderCall> kinds;
        kinds.reserve(calls_.size());
        for (const CallRecord& record : calls_) {
            kinds.push_back(record.call);
        }
        return kinds;
    }

private:
    [[nodiscard]] static md::AuthFailure interrupted() {
        return md::AuthFailure{.kind = md::AuthFailureKind::request,
                               .info = {},
                               .error = {.code = md::Errc::interrupted, .message = "interrupted"}};
    }

    void record(ProviderCall call) {
        calls_.push_back(CallRecord{
            .call = call,
            .requests_before = transport_ == nullptr ? 0 : transport_->requests().size(),
            .thread = std::this_thread::get_id(),
        });
    }

    void rotate() {
        if (index_ + 1 < tokens_.size()) {
            ++index_;
        }
        expires_ = clock_->now() + lifetime_;
    }

    [[nodiscard]] md::Secret current() const {
        md::Secret value;
        value.append("Bearer ");
        value.append(tokens_.at(index_));
        return value;
    }

    const FakeClock* clock_ = nullptr;         ///< borrowed; never null
    const FakeTransport* transport_ = nullptr; ///< borrowed; nullable (no request counts)
    std::vector<std::string> tokens_{};
    std::size_t index_ = 0;
    std::chrono::steady_clock::duration lifetime_{std::chrono::minutes{15}};
    std::chrono::steady_clock::time_point expires_{};
    std::optional<md::AuthFailure> authorization_failure_{};
    std::optional<md::AuthFailure> refresh_failure_{};
    std::size_t refresh_failure_from_ = 1;
    std::size_t refreshes_ = 0; ///< refresh_after_unauthorized() calls so far
    std::function<void(const std::stop_token&)> on_authorization_{};
    std::vector<CallRecord> calls_{};
};

[[nodiscard]] md::FollowsFetchServices services_for(md::Transport& transport, md::Clock& clock,
                                                    md::TraceSink* trace = nullptr) {
    // Member by member, as the header asks (the clang-analyzer leak false positive).
    md::FollowsFetchServices services{};
    services.transport = &transport;
    services.clock = &clock;
    services.jitter = [](std::uint32_t) { return std::uint32_t{0}; };
    services.trace = trace;
    return services;
}

/// The fake transport, clock and provider, and fetch_follows_collection over them.
class Harness {
public:
    Harness() = default;

    [[nodiscard]] FakeTransport& transport() noexcept { return transport_; }
    [[nodiscard]] FakeClock& clock() noexcept { return clock_; }
    [[nodiscard]] FakeTokenProvider& provider() noexcept { return provider_; }

    [[nodiscard]] md::AuthResult<md::FollowsCollection>
    fetch(bool with_statuses, const md::ApiBase& base = md::ApiBase{},
          md::TraceSink* trace = nullptr) {
        return md::fetch_follows_collection(base, services_for(transport_, clock_, trace),
                                            provider_, with_statuses);
    }

    /// Queues one page for `follows_url(offset)`.
    void push_page(const std::vector<std::string>& ids, std::size_t offset, std::size_t total) {
        transport_.push_status(follows_url(offset), 200, page_json(ids, offset, total));
    }

private:
    // The clock is declared first, so it outlives the transport and the hooks a test installs on
    // it; the provider borrows both and is declared last.
    FakeClock clock_{};
    FakeTransport transport_{};
    FakeTokenProvider provider_{
        clock_,
        &transport_,
        {std::string{fake_access_1}, std::string{fake_access_2}, std::string{fake_access_3}}};
};

void check_failure(const md::AuthResult<md::FollowsCollection>& result, md::AuthFailureKind kind,
                   md::Errc code) {
    REQUIRE(!result.has_value());
    INFO(result.error().error.message);
    CHECK(result.error().kind == kind);
    CHECK(result.error().error.code == code);
}

[[nodiscard]] std::vector<std::string> urls(const std::vector<RecordedRequest>& requests) {
    std::vector<std::string> out;
    out.reserve(requests.size());
    for (const RecordedRequest& request : requests) {
        out.push_back(request.url);
    }
    return out;
}

// ---- the URL pin (T10's builders == T11's literals) --------------------------------------------

TEST_CASE("follows fetch: the URL builders are exactly the URLs get_authorized sends",
          "[follows][fetch]") {
    const std::string base_text =
        GENERATE(as<std::string>{}, "https://api.mangadex.org", "file:///srv/fixtures/clean");
    CAPTURE(base_text);
    const md::ApiBase base = parsed_base(base_text);
    const md::Endpoints endpoints = md::endpoints_for(base, /*report=*/false);
    FakeTransport transport;
    FakeClock clock;
    transport.set_fallback(md::HttpResponse{.status = 200, .body = "{}"});
    md::ApiClient client{transport, clock, md::ClientConfig{.endpoints = endpoints},
                         [](std::uint32_t) { return std::uint32_t{0}; }, nullptr};
    const md::Secret authorization = md::Secret::copy_of(mdx::bearer(fake_access_1));

    const std::vector<std::size_t> offsets{0, 1, 100, 9900, 123456789};
    for (const std::size_t offset : offsets) {
        REQUIRE(
            client.get_authorized(md::AuthorizedPath::user_follows_manga, offset, authorization, {})
                .has_value());
    }
    REQUIRE(
        client.get_authorized(md::AuthorizedPath::manga_status, 0, authorization, {}).has_value());

    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == offsets.size() + 1);
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        CAPTURE(offsets[i]);
        const std::string literal =
            std::format("{}/user/follows/manga?limit=100&offset={}", endpoints.api, offsets[i]);
        CHECK(requests[i].url == literal);
        CHECK(md::follows_request_url(endpoints.api, offsets[i]) == literal);
    }
    CHECK(requests.back().url == std::format("{}/manga/status", endpoints.api));
    CHECK(md::status_map_request_url(endpoints.api) == requests.back().url);
    if (base.kind() == md::ApiBase::Kind::official) {
        CHECK(requests.front().url == follows_url(0));
        CHECK(requests.back().url == status_url);
    } else {
        CHECK(requests.front().url ==
              "file:///srv/fixtures/clean/api/user/follows/manga?limit=100&offset=0");
        CHECK(requests.back().url == "file:///srv/fixtures/clean/api/manga/status");
    }
}

// ---- paging -------------------------------------------------------------------------------------

TEST_CASE("follows fetch: pages are read until offset reaches total, each with the token",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 250);
    h.push_page(title_ids(100, 100), 100, 250);
    h.push_page(title_ids(200, 50), 200, 250);
    const auto collection = h.fetch(/*with_statuses=*/false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 250)));
    CHECK(!collection->statuses.has_value());

    const auto requests = h.transport().requests();
    CHECK(urls(requests) ==
          std::vector<std::string>{follows_url(0), follows_url(100), follows_url(200)});
    for (const RecordedRequest& request : requests) {
        CHECK(request.method == md::HttpMethod::get);
        CHECK(request.authorization == mdx::bearer(fake_access_1));
        CHECK(request.max_body_bytes == md::max_api_json_bytes);
        CHECK(request.body.empty());
    }
    CHECK(h.transport().drained());
    // One authorization() per request, no refresh needed.
    CHECK(h.provider().call_kinds() == std::vector<ProviderCall>(3, ProviderCall::authorization));
}

TEST_CASE("follows fetch: an empty page ends the follows, whatever total says",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 500);
    h.push_page({}, 100, 500);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 100)));
    CHECK(urls(h.transport().requests()) ==
          std::vector<std::string>{follows_url(0), follows_url(100)});
}

TEST_CASE("follows fetch: the offset steps by the items received", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    // A server that sends fewer than asked: the next offset is 60, never 100.
    h.push_page(title_ids(0, 60), 0, 120);
    h.push_page(title_ids(60, 60), 60, 120);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 120)));
    CHECK(urls(h.transport().requests()) ==
          std::vector<std::string>{follows_url(0), follows_url(60)});
}

TEST_CASE("follows fetch: no follows is an empty collection", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const bool with_statuses = GENERATE(false, true);
    CAPTURE(with_statuses);
    h.push_page({}, 0, 0);
    const auto collection = h.fetch(with_statuses);
    REQUIRE(collection.has_value());
    CHECK(*collection == md::FollowsCollection{});
    // AC54: with zero follows, no status request is sent.
    CHECK(urls(h.transport().requests()) == std::vector<std::string>{follows_url(0)});
    CHECK(h.transport().count(status_url) == 0);
}

TEST_CASE("follows fetch: duplicate follows are kept in API order", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const std::vector<std::string> ids{title_id(1), title_id(2), title_id(1)};
    h.push_page(ids, 0, 3);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(ids));
}

TEST_CASE("follows fetch: a total over 10000 is refused, not truncated", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 10001);
    const auto collection = h.fetch(false);
    REQUIRE(!collection.has_value());
    CHECK(collection.error() == md::AuthFailure{.kind = md::AuthFailureKind::content,
                                                .info = {},
                                                .error = {.code = md::Errc::network,
                                                          .message = std::string{cap_message}}});
    CHECK(collection.error().error.exit_code() == 3);
    CHECK(h.transport().requests().size() == 1);
}

TEST_CASE("follows fetch: exactly 10000 follows are read in full", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    for (std::size_t offset = 0; offset < md::max_follows; offset += 100) {
        h.push_page(title_ids(offset, 100), offset, md::max_follows);
    }
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows.size() == md::max_follows);
    CHECK(collection->follows.back() == manga(title_id(md::max_follows - 1)));
    CHECK(h.transport().requests().size() == 100);
    CHECK(h.transport().drained());
}

TEST_CASE("follows fetch: short pages past the 10000 window are refused with their own message",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    // A server that pages short with total <= 10000: the offsets that fit the window (offset +
    // 100 <= 10000) are read; the next one would ask past it, which the API refuses. The message
    // is not the "more than 10000 titles" one: the user does not follow too many.
    const auto [per_page, pages, refused_at] =
        GENERATE(table<std::size_t, std::size_t, std::size_t>({
            {99, 101, 9999}, // offsets 0, 99, ..., 9900
            {60, 166, 9960}, // offsets 0, 60, ..., 9900
        }));
    CAPTURE(per_page);
    for (std::size_t page = 0; page < pages; ++page) {
        h.push_page(title_ids(page * per_page, per_page), page * per_page, md::max_follows);
    }
    const auto collection = h.fetch(true);
    REQUIRE(!collection.has_value());
    CHECK(collection.error() ==
          md::AuthFailure{
              .kind = md::AuthFailureKind::content,
              .info = {},
              .error = {.code = md::Errc::network,
                        .message = std::format("MangaDex sent the follows in more pages than "
                                               "expected (offset {} of 10000); refusing to "
                                               "continue past the 10000-title window",
                                               refused_at)}});
    CHECK(collection.error().error.exit_code() == 3);
    CHECK(collection.error().error.message != cap_message);
    CHECK(h.transport().requests().size() == pages);
    CHECK(h.transport().requests().back().url == follows_url(9900));
    CHECK(h.transport().count(status_url) == 0);
    CHECK(h.transport().drained());
}

// ---- token handling -----------------------------------------------------------------------------

TEST_CASE("follows fetch: a token expiring before page 2 is refreshed before page 2",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    h.provider().set_expiry(seconds{60}, seconds{60});
    // Each request takes 40 s of (fake) time: page 1 starts with 60 s left, page 2 with 20 s.
    // Borrows the harness's clock, which outlives the transport's hook (both are its members).
    h.transport().set_on_fetch(
        [clock = &h.clock()](const md::HttpRequest&) { clock->advance(seconds{40}); });
    h.push_page(title_ids(0, 100), 0, 150);
    h.push_page(title_ids(100, 50), 100, 150);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 150)));

    const auto requests = h.transport().requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].authorization == mdx::bearer(fake_access_1));
    CHECK(requests[1].authorization == mdx::bearer(fake_access_2));
    const auto& calls = h.provider().calls();
    REQUIRE(calls.size() == 3);
    CHECK(calls[0].call == ProviderCall::authorization);
    CHECK(calls[0].requests_before == 0);
    CHECK(calls[1].call == ProviderCall::authorization);
    CHECK(calls[1].requests_before == 1);
    CHECK(calls[2].call == ProviderCall::proactive_refresh);
    CHECK(calls[2].requests_before == 1); // after page 1, before page 2
}

TEST_CASE("follows fetch: a 401 on page 2 gets one refresh and one retry of page 2",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 150);
    h.transport().push_status(follows_url(100), 401, R"({"result":"error"})");
    h.push_page(title_ids(100, 50), 100, 150);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 150)));

    const auto requests = h.transport().requests();
    CHECK(urls(requests) ==
          std::vector<std::string>{follows_url(0), follows_url(100), follows_url(100)});
    REQUIRE(requests.size() == 3);
    CHECK(requests[1].authorization == mdx::bearer(fake_access_1));
    CHECK(requests[2].authorization == mdx::bearer(fake_access_2));
    const auto& calls = h.provider().calls();
    REQUIRE(calls.size() == 3);
    CHECK(calls[1].call == ProviderCall::authorization);
    CHECK(calls[2].call == ProviderCall::reactive_refresh);
    CHECK(calls[2].requests_before == 2); // after the 401, before the retry
    CHECK(h.transport().drained());
}

TEST_CASE("follows fetch: a second 401 on the same page is auth_rejected",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 150);
    h.transport().push_status(follows_url(100), 401);
    h.transport().push_status(follows_url(100), 401);
    const auto collection = h.fetch(false);
    REQUIRE(!collection.has_value());
    CHECK(collection.error() ==
          md::AuthFailure{.kind = md::AuthFailureKind::refused,
                          .info =
                              md::TokenErrorInfo{.refusal = md::TokenRefusal::other, .status = 401},
                          .error = {.code = md::Errc::auth_rejected,
                                    .message = std::string{follows_401_message}}});
    CHECK(collection.error().error.exit_code() == 9);
    CHECK(h.transport().count(follows_url(100)) == 2); // retried exactly once
    CHECK(h.provider().call_kinds() == std::vector<ProviderCall>{ProviderCall::authorization,
                                                                 ProviderCall::authorization,
                                                                 ProviderCall::reactive_refresh});
}

TEST_CASE("follows fetch: a 401 on page 1 is retried too, and each page gets a fresh 401 budget",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    h.transport().push_status(follows_url(0), 401);
    h.push_page(title_ids(0, 100), 0, 150);
    h.transport().push_status(follows_url(100), 401);
    h.push_page(title_ids(100, 50), 100, 150);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows.size() == 150);
    const auto requests = h.transport().requests();
    REQUIRE(requests.size() == 4);
    CHECK(requests[0].authorization == mdx::bearer(fake_access_1));
    CHECK(requests[1].authorization == mdx::bearer(fake_access_2));
    CHECK(requests[2].authorization == mdx::bearer(fake_access_2));
    CHECK(requests[3].authorization == mdx::bearer(fake_access_3));
}

TEST_CASE("follows fetch: a provider failure is returned as is, with no request",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    const md::AuthFailure refusal{
        .kind = md::AuthFailureKind::refused,
        .info = md::TokenErrorInfo{.refusal = md::TokenRefusal::bad_client, .status = 401},
        .error = {.code = md::Errc::auth_rejected, .message = "fake refusal"}};
    const md::AuthFailure request_failure{
        .kind = md::AuthFailureKind::request,
        .info = {},
        .error = {.code = md::Errc::network, .message = "fake auth host: refused"}};
    const md::AuthFailure failure = GENERATE_COPY(refusal, request_failure);
    h.provider().fail_authorization(failure);
    h.push_page(title_ids(0, 1), 0, 1);
    const auto collection = h.fetch(true);
    REQUIRE(!collection.has_value());
    CHECK(collection.error() == failure);
    CHECK(h.transport().requests().empty());
}

TEST_CASE("follows fetch: a failed refresh after a 401 is returned as is",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    const md::AuthFailure ended{
        .kind = md::AuthFailureKind::local,
        .info = {},
        .error = {.code = md::Errc::login_required, .message = "fake: the login expired"}};
    h.provider().fail_refresh(ended);
    h.transport().push_status(follows_url(0), 401);
    const auto collection = h.fetch(false);
    REQUIRE(!collection.has_value());
    CHECK(collection.error() == ended);
    CHECK(h.transport().requests().size() == 1);
}

TEST_CASE("follows fetch: the grant cap hit by a later page's refresh ends the fetch",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    // AuthClient's refusal of a 4th logical grant request (auth.hpp), as LoginSession returns it.
    const md::AuthFailure grant_cap{
        .kind = md::AuthFailureKind::content,
        .info = {},
        .error = {.code = md::Errc::network,
                  .message = "more than 3 MangaDex login requests in one run; refusing to "
                             "continue"}};
    // The refresh after page 1's 401 works; the one after page 2's 401 hits the cap.
    h.provider().fail_refresh(grant_cap, 2);
    h.transport().push_status(follows_url(0), 401);
    h.push_page(title_ids(0, 100), 0, 250);
    h.transport().push_status(follows_url(100), 401);
    h.push_page(title_ids(100, 100), 100, 250); // never asked for
    h.push_page(title_ids(200, 50), 200, 250);  // never asked for
    const auto collection = h.fetch(true);
    REQUIRE(!collection.has_value());
    CHECK(collection.error() == grant_cap);
    CHECK(urls(h.transport().requests()) ==
          std::vector<std::string>{follows_url(0), follows_url(0), follows_url(100)});
    CHECK(h.provider().call_kinds() ==
          std::vector<ProviderCall>{ProviderCall::authorization, ProviderCall::reactive_refresh,
                                    ProviderCall::authorization, ProviderCall::reactive_refresh});
}

TEST_CASE("follows fetch: the provider is called on the calling thread only",
          "[follows][fetch][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 101);
    h.transport().push_status(follows_url(100), 401);
    h.push_page(title_ids(100, 1), 100, 101);
    h.transport().push_status(std::string{status_url}, 200, status_json(title_ids(0, 1)));
    REQUIRE(h.fetch(true).has_value());
    REQUIRE(h.provider().calls().size() == 4);
    for (const CallRecord& call : h.provider().calls()) {
        CHECK(call.thread == std::this_thread::get_id());
    }
}

// ---- the status map -----------------------------------------------------------------------------

TEST_CASE("follows fetch: with statuses, the pages come first, then exactly one status request",
          "[follows][fetch][status]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 101);
    h.push_page(title_ids(100, 1), 100, 101);
    const std::vector<std::string> with_status{title_id(5), title_id(100), title_id(7777)};
    h.transport().push_status(std::string{status_url}, 200, status_json(with_status));
    const auto collection = h.fetch(true);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 101)));
    CHECK(collection->statuses == status_map(with_status));

    const auto requests = h.transport().requests();
    CHECK(urls(requests) ==
          std::vector<std::string>{follows_url(0), follows_url(100), std::string{status_url}});
    REQUIRE(requests.size() == 3);
    CHECK(requests[2].method == md::HttpMethod::get);
    CHECK(requests[2].authorization == mdx::bearer(fake_access_1));
    CHECK(requests[2].max_body_bytes == md::max_api_json_bytes);
    CHECK(h.provider().call_kinds() == std::vector<ProviderCall>(3, ProviderCall::authorization));
}

TEST_CASE("follows fetch: an empty status map is a map with no entries",
          "[follows][fetch][status]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 2), 0, 2);
    const std::string body = GENERATE(as<std::string>{}, R"({"result":"ok","statuses":{}})",
                                      R"({"result":"ok","statuses":[]})");
    CAPTURE(body);
    h.transport().push_status(std::string{status_url}, 200, body);
    const auto collection = h.fetch(true);
    REQUIRE(collection.has_value());
    CHECK(collection->statuses == md::StatusMap{});
}

TEST_CASE("follows fetch: an expiring token is refreshed before the status request",
          "[follows][fetch][status]") {
    const InterruptFlagReset reset;
    Harness h;
    h.provider().set_expiry(seconds{60}, seconds{60});
    // Borrows the harness's clock, which outlives the transport's hook (both are its members).
    h.transport().set_on_fetch(
        [clock = &h.clock()](const md::HttpRequest&) { clock->advance(seconds{40}); });
    h.push_page(title_ids(0, 2), 0, 2);
    h.transport().push_status(std::string{status_url}, 200, status_json(title_ids(0, 1)));
    const auto collection = h.fetch(true);
    REQUIRE(collection.has_value());
    const auto requests = h.transport().requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].authorization == mdx::bearer(fake_access_1));
    CHECK(requests[1].url == status_url);
    CHECK(requests[1].authorization == mdx::bearer(fake_access_2));
    CHECK(h.provider().call_kinds() == std::vector<ProviderCall>{ProviderCall::authorization,
                                                                 ProviderCall::authorization,
                                                                 ProviderCall::proactive_refresh});
}

TEST_CASE("follows fetch: a 401 on the status map gets one refresh and one retry",
          "[follows][fetch][status]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 2), 0, 2);
    h.transport().push_status(std::string{status_url}, 401);
    SECTION("then 200") {
        h.transport().push_status(std::string{status_url}, 200, status_json(title_ids(0, 2)));
        const auto collection = h.fetch(true);
        REQUIRE(collection.has_value());
        CHECK(collection->statuses == status_map(title_ids(0, 2)));
        const auto requests = h.transport().requests();
        REQUIRE(requests.size() == 3);
        CHECK(requests[1].authorization == mdx::bearer(fake_access_1));
        CHECK(requests[2].authorization == mdx::bearer(fake_access_2));
    }
    SECTION("then 401 again") {
        h.transport().push_status(std::string{status_url}, 401);
        const auto collection = h.fetch(true);
        check_failure(collection, md::AuthFailureKind::refused, md::Errc::auth_rejected);
        CHECK(collection.error().error.message == statuses_401_message);
        CHECK(collection.error().info.status == 401);
        CHECK(h.transport().count(status_url) == 2);
    }
    CHECK(h.provider().call_kinds() == std::vector<ProviderCall>{ProviderCall::authorization,
                                                                 ProviderCall::authorization,
                                                                 ProviderCall::reactive_refresh});
}

// ---- request failures (the network remedy applies) ----------------------------------------------

TEST_CASE("follows fetch: 5xx is retried, then a request failure", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const bool on_status = GENERATE(false, true);
    CAPTURE(on_status);
    const std::string url = on_status ? std::string{status_url} : follows_url(0);
    if (on_status) {
        h.push_page(title_ids(0, 1), 0, 1);
    }
    SECTION("three 503s then 200") {
        for (int i = 0; i < 3; ++i) {
            h.transport().push_status(url, 503);
        }
        h.transport().push_status(url, 200,
                                  on_status ? status_json({}) : page_json(title_ids(0, 1), 0, 1));
        const auto collection = h.fetch(on_status);
        REQUIRE(collection.has_value());
        CHECK(h.transport().count(url) == 4);
        CHECK(h.transport().drained());
    }
    SECTION("four 503s") {
        for (int i = 0; i < 4; ++i) {
            h.transport().push_status(url, 503);
        }
        const auto collection = h.fetch(on_status);
        check_failure(collection, md::AuthFailureKind::request, md::Errc::network);
        CHECK(collection.error().error.message == std::format("{}: HTTP 503 (server error)", url));
        CHECK(collection.error().error.exit_code() == 3);
        CHECK(h.transport().count(url) == 4);
    }
}

TEST_CASE("follows fetch: 429 is retried once, then a request failure", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const bool on_status = GENERATE(false, true);
    CAPTURE(on_status);
    const std::string url = on_status ? std::string{status_url} : follows_url(0);
    if (on_status) {
        h.push_page(title_ids(0, 1), 0, 1);
    }
    h.transport().push_status(url, 429);
    h.transport().push_status(url, 429);
    const auto collection = h.fetch(on_status);
    check_failure(collection, md::AuthFailureKind::request, md::Errc::network);
    CHECK_THAT(collection.error().error.message,
               StartsWith(std::format("{}: HTTP 429 (rate limited until ", url)));
    CHECK_THAT(collection.error().error.message, ContainsSubstring(" UTC)"));
    CHECK(h.transport().count(url) == 2);
    CHECK(h.clock().total_slept() >= seconds{60});
}

TEST_CASE("follows fetch: other HTTP statuses are request failures, answered once",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const auto [status, reason] = GENERATE(table<int, std::string>({
        {403, "HTTP 403 (access denied — possibly a temporary rate-limit ban; wait before "
              "retrying)"},
        {400, "HTTP 400 (bad request: the API rejected the query)"},
        {404, "HTTP 404 (not found)"},
        {418, "HTTP 418"},
        {302, "HTTP 302"},
    }));
    CAPTURE(status);
    h.transport().push_status(follows_url(0), status, R"({"result":"error"})");
    const auto collection = h.fetch(false);
    check_failure(collection, md::AuthFailureKind::request, md::Errc::network);
    CHECK(collection.error().error.message == std::format("{}: {}", follows_url(0), reason));
    CHECK(h.transport().count(follows_url(0)) == 1);
    CHECK(h.provider().call_kinds() == std::vector<ProviderCall>{ProviderCall::authorization});
}

TEST_CASE("follows fetch: a transport failure or an oversized body is a request failure",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const bool on_status = GENERATE(false, true);
    CAPTURE(on_status);
    const std::string url = on_status ? std::string{status_url} : follows_url(0);
    if (on_status) {
        h.push_page(title_ids(0, 1), 0, 1);
    }
    SECTION("connect") {
        h.transport().push_error(url, md::TransportErrc::connect, "refused");
        const auto collection = h.fetch(on_status);
        check_failure(collection, md::AuthFailureKind::request, md::Errc::network);
        CHECK(collection.error().error.message == std::format("{}: refused", url));
    }
    SECTION("16 MiB + 1") {
        // FakeTransport turns a body over the request's cap into too_large, as the real ones do.
        h.transport().push_status(url, 200, std::string(md::max_api_json_bytes + 1, ' '));
        const auto collection = h.fetch(on_status);
        check_failure(collection, md::AuthFailureKind::request, md::Errc::network);
        CHECK(collection.error().error.message ==
              std::format("{}: body exceeds {} bytes", url, md::max_api_json_bytes));
    }
}

// ---- content failures (no network remedy) -------------------------------------------------------

TEST_CASE("follows fetch: a malformed page is a content failure", "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    nlohmann::json not_ok = nlohmann::json::parse(page_json(title_ids(0, 1), 0, 1));
    not_ok["result"] = "error";
    const std::string body =
        GENERATE_COPY(as<std::string>{}, "not json", page_json({"../not-a-uuid"}, 0, 1),
                      page_json({"c0000000-0000-4000-8000-00000000000"}, 0, 1), not_ok.dump(),
                      std::string{R"({"result":"ok","data":[],"limit":100,"offset":0})"});
    CAPTURE(body);
    h.transport().push_status(follows_url(0), 200, body);
    const auto collection = h.fetch(true);
    check_failure(collection, md::AuthFailureKind::content, md::Errc::network);
    CHECK_THAT(collection.error().error.message,
               StartsWith(std::format("{}: malformed MangaDex API response (follows page): ",
                                      follows_url(0))));
    CHECK(h.transport().count(status_url) == 0);
}

TEST_CASE("follows fetch: a malformed status map is a content failure",
          "[follows][fetch][status]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 1), 0, 1);
    const std::string body = GENERATE(
        as<std::string>{}, "not json", R"({"result":"ok","statuses":{"not-a-uuid":"reading"}})",
        R"({"result":"ok","statuses":{"c0000000-0000-4000-8000-000000000000":"Reading"}})",
        R"({"result":"error","statuses":{}})", R"({"result":"ok"})");
    CAPTURE(body);
    h.transport().push_status(std::string{status_url}, 200, body);
    const auto collection = h.fetch(true);
    check_failure(collection, md::AuthFailureKind::content, md::Errc::network);
    CHECK_THAT(collection.error().error.message,
               StartsWith(std::format("{}: malformed MangaDex API response (reading statuses): ",
                                      status_url)));
}

// ---- local failures -----------------------------------------------------------------------------

TEST_CASE("follows fetch: a custom API base is refused before the provider is asked",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const std::string text =
        GENERATE(as<std::string>{}, "https://mirror.example/api", "http://127.0.0.1:8080");
    CAPTURE(text);
    h.transport().set_fallback(md::HttpResponse{.status = 200, .body = page_json({}, 0, 0)});
    const auto collection = h.fetch(true, parsed_base(text));
    check_failure(collection, md::AuthFailureKind::local, md::Errc::usage);
    CHECK(collection.error().error.message ==
          "follows logs in only to MangaDex itself; --api-base must be the official API");
    CHECK(h.transport().requests().empty());
    CHECK(h.provider().calls().empty());
}

TEST_CASE("follows fetch: missing services are an internal error", "[follows][fetch]") {
    FakeTransport transport;
    FakeClock clock;
    FakeTokenProvider provider{clock, &transport, {std::string{fake_access_1}}};
    md::FollowsFetchServices services = services_for(transport, clock);
    const int missing = GENERATE(0, 1, 2);
    CAPTURE(missing);
    if (missing == 0) {
        services.transport = nullptr;
    } else if (missing == 1) {
        services.clock = nullptr;
    } else {
        services.jitter = nullptr;
    }
    const auto collection = md::fetch_follows_collection(md::ApiBase{}, services, provider, false);
    check_failure(collection, md::AuthFailureKind::local, md::Errc::failure);
    CHECK(transport.requests().empty());
    CHECK(provider.calls().empty());
}

// ---- interrupts ---------------------------------------------------------------------------------

TEST_CASE("follows fetch: a pending interrupt asks nothing and sends nothing",
          "[follows][fetch][interrupt]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 1), 0, 1);
    md::simulate_interrupt_for_testing(SIGINT);
    const auto collection = h.fetch(true);
    check_failure(collection, md::AuthFailureKind::request, md::Errc::interrupted);
    CHECK(h.transport().requests().empty());
    CHECK(h.provider().calls().empty());
}

TEST_CASE("follows fetch: an interrupt during a page request cancels it through the bridge",
          "[follows][fetch][interrupt]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 100), 0, 150);
    h.push_page(title_ids(100, 50), 100, 150);
    // Runs on the fetching thread, inside Transport::fetch of page 2: the "signal" arrives
    // mid-request, and the hook waits until the InterruptBridge has turned it into a stop on the
    // request's token, so FakeTransport answers `cancelled`. Captures only a copy of the URL.
    h.transport().set_on_fetch([page_2 = follows_url(100)](const md::HttpRequest& request) {
        if (request.url != page_2) {
            return;
        }
        md::simulate_interrupt_for_testing(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + bridge_limit;
        while (!request.cancel.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    const auto collection = h.fetch(false);
    check_failure(collection, md::AuthFailureKind::request, md::Errc::interrupted);
    CHECK(h.transport().count(follows_url(100)) == 1);
    CHECK(md::interrupt_signal() == SIGTERM);
}

TEST_CASE("follows fetch: an interrupt during the status request cancels it",
          "[follows][fetch][interrupt][status]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 1), 0, 1);
    h.transport().push_status(std::string{status_url}, 200, status_json({}));
    // As above, for the status request only. Captures only a copy of the URL.
    h.transport().set_on_fetch([url = std::string{status_url}](const md::HttpRequest& request) {
        if (request.url != url) {
            return;
        }
        md::simulate_interrupt_for_testing(SIGINT);
        const auto deadline = std::chrono::steady_clock::now() + bridge_limit;
        while (!request.cancel.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    const auto collection = h.fetch(true);
    check_failure(collection, md::AuthFailureKind::request, md::Errc::interrupted);
    CHECK(h.transport().count(status_url) == 1);
}

TEST_CASE("follows fetch: the provider gets the bridged stop token",
          "[follows][fetch][interrupt][auth]") {
    const InterruptFlagReset reset;
    Harness h;
    h.push_page(title_ids(0, 1), 0, 1);
    // Runs on the fetching thread, inside authorization(): a signal arrives while the provider
    // works (for example during a refresh POST), and the stop token it was given must see it.
    h.provider().set_on_authorization([](const std::stop_token& stop) {
        md::simulate_interrupt_for_testing(SIGINT);
        const auto deadline = std::chrono::steady_clock::now() + bridge_limit;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    const auto collection = h.fetch(false);
    check_failure(collection, md::AuthFailureKind::request, md::Errc::interrupted);
    CHECK(h.transport().requests().empty());
}

TEST_CASE("follows fetch: a fetch after an interrupted one works", "[follows][fetch][interrupt]") {
    const InterruptFlagReset reset;
    Harness h;
    md::simulate_interrupt_for_testing(SIGINT);
    check_failure(h.fetch(false), md::AuthFailureKind::request, md::Errc::interrupted);
    md::reset_interrupt_for_testing();
    h.push_page(title_ids(0, 1), 0, 1);
    const auto collection = h.fetch(false);
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 1)));
}

// ---- trace --------------------------------------------------------------------------------------

TEST_CASE("follows fetch: the trace has one GET line per request and never the token",
          "[follows][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    RecordingTrace trace;
    h.push_page(title_ids(0, 1), 0, 1);
    h.transport().push_status(std::string{status_url}, 401);
    h.transport().push_status(std::string{status_url}, 200, status_json({}));
    REQUIRE(h.fetch(true, md::ApiBase{}, &trace).has_value());
    const std::vector<std::string> lines = trace.lines();
    REQUIRE(lines.size() == 3);
    CHECK_THAT(lines[0], StartsWith(std::format("GET {} -> 200 (", follows_url(0))));
    CHECK_THAT(lines[1], StartsWith(std::format("GET {} -> 401 (", status_url)));
    CHECK_THAT(lines[2], StartsWith(std::format("GET {} -> 200 (", status_url)));
    for (const std::string& line : lines) {
        CHECK_THAT(line, !ContainsSubstring("fake-access"));
        CHECK_THAT(line, !ContainsSubstring("Bearer"));
    }
    // No reporter: report = false sends no MD@H POST.
    for (const RecordedRequest& request : h.transport().requests()) {
        CHECK(request.method == md::HttpMethod::get);
    }
}

// ---- the real fixture transport -----------------------------------------------------------------

/// A Transport that records (url, authorization) and forwards to another one (thread-safe).
class RecordingTransport final : public md::Transport {
public:
    explicit RecordingTransport(std::unique_ptr<md::Transport> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& request) override {
        {
            const std::scoped_lock lock{mutex_};
            seen_.emplace_back(request.url, request.authorization);
        }
        return inner_->fetch(request);
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> seen() const {
        const std::scoped_lock lock{mutex_};
        return seen_;
    }

private:
    std::unique_ptr<md::Transport> inner_{};
    mutable std::mutex mutex_{};
    std::vector<std::pair<std::string, std::string>> seen_{}; ///< guarded by mutex_
};

/// A fixture tree with the fake account, two follows pages (150 titles) and a status map, under
/// `<dir>/root`.
struct FollowsTree {
    std::optional<md::test::TempDir> dir{}; ///< always engaged; optional for the initializer
    std::filesystem::path root{};
};

[[nodiscard]] FollowsTree make_follows_tree(const mdx::FixtureAuth& auth) {
    auto dir = md::test::TempDir::create("mangadex-dl-follows-fetch");
    REQUIRE(dir.has_value());
    const std::filesystem::path root = std::filesystem::absolute(dir->path()) / "root";
    const auto written = mdx::write_auth_files(root, auth);
    INFO((written.has_value() ? std::string{} : written.error()));
    REQUIRE(written.has_value());
    const std::filesystem::path pages = root / "api" / "user" / "follows" / "manga";
    REQUIRE(mdx::detail::write_file(pages / "offset-0.json", page_json(title_ids(0, 100), 0, 150))
                .has_value());
    REQUIRE(
        mdx::detail::write_file(pages / "offset-100.json", page_json(title_ids(100, 50), 100, 150))
            .has_value());
    REQUIRE(mdx::detail::write_file(root / "api" / "manga" / "status.json",
                                    status_json({title_id(3), title_id(120)}, "on_hold"))
                .has_value());
    return FollowsTree{.dir = std::move(*dir), .root = root};
}

TEST_CASE("follows fetch: over the fixture transport, a used-up token is refreshed once",
          "[follows][fetch][fixture]") {
    const InterruptFlagReset reset;
    mdx::FixtureAuth auth{};
    // The first access token serves one request only: page 2 is answered 401.
    auth.chain.at(0).valid_requests = 1;
    const FollowsTree tree = make_follows_tree(auth);
    auto inner = md::make_fixture_transport(tree.root);
    REQUIRE(inner.has_value());
    RecordingTransport transport{std::move(*inner)};
    FakeClock clock;
    FakeTokenProvider provider{
        clock, nullptr, {std::string{mdx::fixture_access_1}, std::string{mdx::fixture_access_2}}};
    const md::ApiBase base = parsed_base(std::format("file://{}", tree.root.string()));

    const auto collection = md::fetch_follows_collection(base, services_for(transport, clock),
                                                         provider, /*with_statuses=*/true);
    INFO((collection.has_value() ? std::string{} : collection.error().error.message));
    REQUIRE(collection.has_value());
    CHECK(collection->follows == mangas(title_ids(0, 150)));
    CHECK(collection->statuses == status_map({title_id(3), title_id(120)}, "on_hold"));

    const std::string api = md::endpoints_for(base, false).api;
    using Seen = std::pair<std::string, std::string>;
    CHECK(transport.seen() ==
          std::vector<Seen>{
              {md::follows_request_url(api, 0), mdx::bearer(mdx::fixture_access_1)},
              {md::follows_request_url(api, 100), mdx::bearer(mdx::fixture_access_1)},
              {md::follows_request_url(api, 100), mdx::bearer(mdx::fixture_access_2)},
              {md::status_map_request_url(api), mdx::bearer(mdx::fixture_access_2)},
          });
    CHECK(provider.call_kinds() ==
          std::vector<ProviderCall>{ProviderCall::authorization, ProviderCall::authorization,
                                    ProviderCall::reactive_refresh, ProviderCall::authorization});
}

TEST_CASE("follows fetch: over the fixture transport, a refused new token is auth_rejected",
          "[follows][fetch][fixture]") {
    const InterruptFlagReset reset;
    mdx::FixtureAuth auth{};
    auth.chain.at(0).valid_requests = 2; // both follows pages, then 401 on the status map
    auth.chain.at(1).access_accepted = false;
    const FollowsTree tree = make_follows_tree(auth);
    auto transport = md::make_fixture_transport(tree.root);
    REQUIRE(transport.has_value());
    FakeClock clock;
    FakeTokenProvider provider{
        clock, nullptr, {std::string{mdx::fixture_access_1}, std::string{mdx::fixture_access_2}}};
    const md::ApiBase base = parsed_base(std::format("file://{}", tree.root.string()));

    const auto collection =
        md::fetch_follows_collection(base, services_for(**transport, clock), provider, true);
    check_failure(collection, md::AuthFailureKind::refused, md::Errc::auth_rejected);
    CHECK(collection.error().error.message == statuses_401_message);
}

TEST_CASE("follows fetch: over the fixture transport, an unknown token on page 1 is rejected",
          "[follows][fetch][fixture]") {
    const InterruptFlagReset reset;
    const FollowsTree tree = make_follows_tree(mdx::FixtureAuth{});
    auto transport = md::make_fixture_transport(tree.root);
    REQUIRE(transport.has_value());
    FakeClock clock;
    FakeTokenProvider provider{clock, nullptr, {std::string{fake_access_1}}};
    const md::ApiBase base = parsed_base(std::format("file://{}", tree.root.string()));

    const auto collection =
        md::fetch_follows_collection(base, services_for(**transport, clock), provider, false);
    check_failure(collection, md::AuthFailureKind::refused, md::Errc::auth_rejected);
    CHECK(collection.error().error.message == follows_401_message);
    CHECK(provider.call_kinds() ==
          std::vector<ProviderCall>{ProviderCall::authorization, ProviderCall::reactive_refresh});
}

} // namespace
