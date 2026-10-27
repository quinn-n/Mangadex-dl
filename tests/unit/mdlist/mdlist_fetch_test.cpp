// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/mdlist/mdlist_fetch_test.cpp: ApiClient::fetch_list_document and fetch_custom_list
// over FakeTransport and FakeClock (list spec AC5, AC12 for the list fetch; list plan T7;
// fact-check V20, V21 and amendment 2): the exact lowercase URL and body cap, 200/404, the
// 401/403 CustomList fallback (answered once, with its body), 5xx and 429 retries, a bare 403,
// request vs content failures, interrupts before and during the request, and the -vv trace.
// The fetch starts an InterruptBridge poller, so these run under TSan too (`unit.mdlist*`).

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_transport.hpp"
#include "support/mdx_fixture.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
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
using md::test::FakeClock;
using md::test::FakeTransport;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view l1_text = "a0000000-0000-4000-8000-000000000001";
constexpr std::string_view l1_url =
    "https://api.mangadex.org/list/a0000000-0000-4000-8000-000000000001";
constexpr std::string_view not_found_message =
    "list a0000000-0000-4000-8000-000000000001 not found on MangaDex (HTTP 404): it does not exist "
    "or is private; check the id, make the list public on MangaDex, or use 'mangadex-dl download' "
    "if it is a title";
/// The live 404 body for a list that does not exist (list spec "Exit codes").
constexpr std::string_view custom_list_error_body =
    R"({"result":"error","errors":[{"id":"8f2b","status":404,"title":"not_found_http_exception",)"
    R"("detail":"CustomList `a0000000-0000-4000-8000-000000000001` not found.","context":null}]})";

/// How long a test waits for the InterruptBridge (it polls every 50 ms). Only a failing test
/// waits this long.
constexpr std::chrono::seconds bridge_limit{20};

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

[[nodiscard]] md::ListId l1() {
    auto id = md::ListId::parse(l1_text);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

/// L1's document ("Clean List": the clean title).
[[nodiscard]] std::string l1_body() {
    return mdx::list_json({.id = std::string{l1_text},
                           .name = "Clean List",
                           .manga_ids = {std::string{clean_id}}})
        .dump();
}

/// The fake transport and clock, and fetch_custom_list over them against the official API.
class Harness {
public:
    Harness() = default;

    [[nodiscard]] FakeTransport& transport() noexcept { return transport_; }
    [[nodiscard]] FakeClock& clock() noexcept { return clock_; }

    [[nodiscard]] md::ListFetchResult<md::CustomList> fetch(const md::ListId& id,
                                                            md::TraceSink* trace = nullptr) {
        // Member by member, as the header asks (the clang-analyzer leak false positive).
        md::ListFetchServices services{};
        services.transport = &transport_;
        services.clock = &clock_;
        services.jitter = [](std::uint32_t) { return std::uint32_t{0}; };
        services.trace = trace;
        return md::fetch_custom_list(id, md::ApiBase{}, services);
    }

private:
    FakeTransport transport_{};
    FakeClock clock_{};
};

void check_request_error(const md::ListFetchResult<md::CustomList>& result, md::Errc code) {
    REQUIRE(!result.has_value());
    INFO(result.error().error.message);
    CHECK(result.error().kind == md::ListFailureKind::request);
    CHECK(result.error().error.code == code);
}

// ---- the request ------------------------------------------------------------------------------

TEST_CASE("mdlist fetch: the URL is <api>/list/<lowercase id> with the API body cap",
          "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const std::string typed =
        GENERATE(as<std::string>{}, "A0000000-0000-4000-8000-000000000001",
                 "https://MangaDex.org/list/A0000000-0000-4000-8000-000000000001/Clean-List?x=1#y",
                 "a0000000-0000-4000-8000-000000000001");
    CAPTURE(typed);
    const auto id = md::ListId::parse(typed);
    REQUIRE(id.has_value());
    h.transport().push_status(std::string{l1_url}, 200, l1_body());
    const auto list = h.fetch(*id);
    REQUIRE(list.has_value());
    const auto requests = h.transport().requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().method == md::HttpMethod::get);
    CHECK(requests.front().url == l1_url);
    CHECK(requests.front().max_body_bytes == md::max_api_json_bytes);
    CHECK(h.transport().drained());
}

TEST_CASE("mdlist fetch: 200 gives the list", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    h.transport().push_status(std::string{l1_url}, 200, l1_body());
    const auto list = h.fetch(l1());
    REQUIRE(list.has_value());
    CHECK(*list == md::CustomList{.id = l1(), .name = "Clean List", .titles = {manga(clean_id)}});
}

TEST_CASE("mdlist fetch: fetch_list_document returns a 2xx body unparsed", "[mdlist][fetch]") {
    const int status = GENERATE(200, 203);
    FakeTransport transport;
    FakeClock clock;
    md::ApiClient client{transport, clock, md::ClientConfig{}, {}, nullptr};
    transport.push_status(std::string{l1_url}, status, "not json at all");
    const auto body = client.fetch_list_document(l1(), {});
    REQUIRE(body.has_value());
    CHECK(*body == "not json at all");
}

TEST_CASE("mdlist fetch: 404 is not_found with the list message", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const std::string body = GENERATE(as<std::string>{}, "", std::string{custom_list_error_body},
                                      "<html>gateway</html>");
    h.transport().push_status(std::string{l1_url}, 404, body);
    const auto list = h.fetch(l1());
    check_request_error(list, md::Errc::not_found);
    CHECK(list.error().error.message == not_found_message);
    CHECK(h.transport().count(l1_url) == 1); // a 404 is never retried
}

TEST_CASE("mdlist fetch: 401 or 403 with a CustomList error body is not_found, asked once",
          "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const int status = GENERATE(401, 403);
    CAPTURE(status);
    h.transport().push_status(std::string{l1_url}, status, std::string{custom_list_error_body});
    const auto list = h.fetch(l1());
    check_request_error(list, md::Errc::not_found);
    CHECK(list.error().error.message == not_found_message);
    CHECK(h.transport().count(l1_url) == 1);
    CHECK(h.clock().sleeps().empty()); // no retry wait (the first API slot is free)
}

TEST_CASE("mdlist fetch: a bare 403 is a network error with the ban hint", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const std::string body = GENERATE(
        as<std::string>{}, "", R"({"result":"error","errors":[{"detail":"Too many requests"}]})",
        "CustomList as plain text");
    CAPTURE(body);
    h.transport().push_status(std::string{l1_url}, 403, body);
    const auto list = h.fetch(l1());
    check_request_error(list, md::Errc::network);
    CHECK(list.error().error.message ==
          std::format("{}: HTTP 403 (access denied — possibly a temporary rate-limit ban; wait "
                      "before retrying)",
                      l1_url));
    CHECK(h.transport().count(l1_url) == 1);
}

TEST_CASE("mdlist fetch: a bare 401 or another 4xx is a network error", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    h.transport().push_status(std::string{l1_url}, 401, "{}");
    const auto unauthorized = h.fetch(l1());
    check_request_error(unauthorized, md::Errc::network);
    CHECK(unauthorized.error().error.message == std::format("{}: HTTP 401", l1_url));

    h.transport().push_status(std::string{l1_url}, 400, std::string{custom_list_error_body});
    const auto bad = h.fetch(l1());
    check_request_error(bad, md::Errc::network);
    CHECK(bad.error().error.message ==
          std::format("{}: HTTP 400 (bad request: the API rejected the query)", l1_url));
}

TEST_CASE("mdlist fetch: 5xx is retried, then succeeds or gives up", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    SECTION("three 503s then 200") {
        for (int i = 0; i < 3; ++i) {
            h.transport().push_status(std::string{l1_url}, 503);
        }
        h.transport().push_status(std::string{l1_url}, 200, l1_body());
        const auto list = h.fetch(l1());
        REQUIRE(list.has_value());
        CHECK(list->titles == std::vector{manga(clean_id)});
        CHECK(h.transport().count(l1_url) == 4);
        CHECK(h.transport().drained());
    }
    SECTION("four 503s") {
        for (int i = 0; i < 4; ++i) {
            h.transport().push_status(std::string{l1_url}, 503);
        }
        const auto list = h.fetch(l1());
        check_request_error(list, md::Errc::network);
        CHECK(list.error().error.message == std::format("{}: HTTP 503 (server error)", l1_url));
        CHECK(h.transport().count(l1_url) == 4);
    }
}

TEST_CASE("mdlist fetch: 429 is retried once after the default wait", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    SECTION("429 then 200") {
        h.transport().push_status(std::string{l1_url}, 429);
        h.transport().push_status(std::string{l1_url}, 200, l1_body());
        const auto list = h.fetch(l1());
        REQUIRE(list.has_value());
        CHECK(h.transport().count(l1_url) == 2);
        CHECK(h.clock().total_slept() >= std::chrono::seconds{60});
    }
    SECTION("429 twice") {
        h.transport().push_status(std::string{l1_url}, 429);
        h.transport().push_status(std::string{l1_url}, 429);
        const auto list = h.fetch(l1());
        check_request_error(list, md::Errc::network);
        CHECK_THAT(list.error().error.message,
                   ContainsSubstring(std::format("{}: HTTP 429 (rate limited until ", l1_url)));
        CHECK(h.transport().count(l1_url) == 2);
    }
}

TEST_CASE("mdlist fetch: a transport failure or an oversized body is a request error",
          "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    SECTION("connect") {
        h.transport().push_error(std::string{l1_url}, md::TransportErrc::connect, "refused");
        const auto list = h.fetch(l1());
        check_request_error(list, md::Errc::network);
        CHECK(list.error().error.message == std::format("{}: refused", l1_url));
    }
    SECTION("16 MiB + 1") {
        // FakeTransport turns a body over the request's cap into too_large, as the real ones do.
        h.transport().push_status(std::string{l1_url}, 200,
                                  std::string(md::max_api_json_bytes + 1, ' '));
        const auto list = h.fetch(l1());
        check_request_error(list, md::Errc::network);
        CHECK(list.error().error.message ==
              std::format("{}: body exceeds {} bytes", l1_url, md::max_api_json_bytes));
    }
}

// ---- the document -------------------------------------------------------------------------------

TEST_CASE("mdlist fetch: a malformed 200 is a content error", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    const std::string body = GENERATE(
        as<std::string>{}, "not json",
        mdx::list_json({.id = std::string{l1_text}, .manga_ids = {"../x"}}).dump(),
        mdx::list_json({.id = mdx::list_id(6), .manga_ids = {std::string{clean_id}}}).dump());
    CAPTURE(body);
    h.transport().push_status(std::string{l1_url}, 200, body);
    const auto list = h.fetch(l1());
    REQUIRE(!list.has_value());
    CHECK(list.error().kind == md::ListFailureKind::content);
    CHECK(list.error().error.code == md::Errc::network);
    CHECK(list.error().error.message.starts_with(
        std::format("list {}: malformed MangaDex API response (custom list): ", l1_text)));
    CHECK(!list.error().error.message.contains(l1_url)); // names the id, never the URL
}

TEST_CASE("mdlist fetch: a list over the title cap is a content error", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    std::vector<std::string> ids;
    for (std::size_t i = 0; i <= md::max_list_titles; ++i) {
        ids.push_back(std::format("c0000000-0000-4000-8000-{:012x}", i));
    }
    h.transport().push_status(
        std::string{l1_url}, 200,
        mdx::list_json({.id = std::string{l1_text}, .manga_ids = std::move(ids)}).dump());
    const auto list = h.fetch(l1());
    REQUIRE(!list.has_value());
    CHECK(list.error() ==
          md::ListFetchError{.kind = md::ListFailureKind::content,
                             .error = {.code = md::Errc::network,
                                       .message = "list a0000000-0000-4000-8000-000000000001 has "
                                                  "more than 10000 titles; refusing to truncate "
                                                  "it; split it into smaller lists"}});
}

// ---- interrupts ---------------------------------------------------------------------------------

TEST_CASE("mdlist fetch: a pending interrupt sends nothing", "[mdlist][fetch][interrupt]") {
    const InterruptFlagReset reset;
    Harness h;
    h.transport().push_status(std::string{l1_url}, 200, l1_body());
    md::simulate_interrupt_for_testing(SIGINT);
    const auto list = h.fetch(l1());
    check_request_error(list, md::Errc::interrupted);
    CHECK(list.error().error.message == "interrupted");
    CHECK(h.transport().requests().empty());
}

TEST_CASE("mdlist fetch: an interrupt during the request cancels it through the bridge",
          "[mdlist][fetch][interrupt]") {
    const InterruptFlagReset reset;
    Harness h;
    h.transport().push_status(std::string{l1_url}, 200, l1_body());
    // Runs on the fetching thread, inside Transport::fetch: the "signal" arrives mid-request, and
    // the hook waits until the InterruptBridge has turned it into a stop on the request's token,
    // so FakeTransport answers `cancelled`. Captures nothing: it outlives no local.
    h.transport().set_on_fetch([](const md::HttpRequest& request) {
        md::simulate_interrupt_for_testing(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + bridge_limit;
        while (!request.cancel.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    const auto list = h.fetch(l1());
    check_request_error(list, md::Errc::interrupted);
    CHECK(h.transport().count(l1_url) == 1);
    CHECK(md::interrupt_signal() == SIGTERM);
}

TEST_CASE("mdlist fetch: a second fetch after an interrupted one works", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    md::simulate_interrupt_for_testing(SIGINT);
    check_request_error(h.fetch(l1()), md::Errc::interrupted);
    md::reset_interrupt_for_testing();
    h.transport().push_status(std::string{l1_url}, 200, l1_body());
    const auto list = h.fetch(l1());
    REQUIRE(list.has_value());
    CHECK(list->name == "Clean List");
}

// ---- trace and services -------------------------------------------------------------------------

TEST_CASE("mdlist fetch: the trace sink gets exactly one GET line", "[mdlist][fetch]") {
    const InterruptFlagReset reset;
    Harness h;
    RecordingTrace trace;
    h.transport().push_status(std::string{l1_url}, 200, l1_body());
    const auto list = h.fetch(l1(), &trace);
    REQUIRE(list.has_value());
    const std::vector<std::string> lines = trace.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().starts_with(std::format("GET {} -> 200 (", l1_url)));
    CHECK(lines.front().ends_with(" ms)"));
    // No reporter: report = false sends no MD@H POST.
    for (const auto& request : h.transport().requests()) {
        CHECK(request.method == md::HttpMethod::get);
    }
}

TEST_CASE("mdlist fetch: a missing transport or clock is an internal error", "[mdlist][fetch]") {
    FakeTransport transport;
    FakeClock clock;
    md::ListFetchServices services{};
    services.jitter = [](std::uint32_t) { return std::uint32_t{0}; };
    const bool has_transport = GENERATE(true, false);
    if (has_transport) {
        services.transport = &transport; // no clock
    } else {
        services.clock = &clock; // no transport
    }
    const auto list = md::fetch_custom_list(l1(), md::ApiBase{}, services);
    check_request_error(list, md::Errc::failure);
    CHECK(transport.requests().empty());
}

} // namespace
