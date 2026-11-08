// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/follows/follows_no_leak_test.cpp: AC8, the token never leaves the two allowlisted
// collection URLs (follows plan T18). One whole `follows` run in core terms, over FakeTransport:
// LoginSession::open (the password grant) -> fetch_follows_collection (the follows pages and,
// with --status, the status map) -> close() -> filter_follows / plan_follows -> run_download for
// every planned title, which for the clean title is its manga, feed, covers, at-home documents
// and images. It runs once without --status and once with --status reading, each both against a
// fixture base (`file://<tree>`, where MD@H reports are always off) and against the official API
// with MD@H reports on, so at least one report POST is sent: there the hook maps every official
// URL onto the tree and answers the report endpoint itself, so nothing touches a network.
//
// FakeTransport records every request (its URL, Authorization and body, as fake values). Its
// answers come from the real fixture transport over the follows-status tree (FS1): an on_fetch
// hook forwards each request to the fixture and queues the fixture's answer for that URL, so the
// fixture's leak tripwire ("fixture: Authorization sent to <url>") sees every request too. The
// tests assert:
// - every request off the allowlist (`<api>/user/follows/manga?…`, `<api>/manga/status`) has an
//   empty Authorization, and every request on it carries the session's Bearer value;
// - only auth-host requests (the token POST) carry a form body; every other POST is an MD@H
//   report without an Authorization; no other request's URL or body holds a credential;
// - the tripwire never fires (and, as a positive control, does fire for a leaked header).
//
// The download runs start FetchPool workers and the fetch an InterruptBridge poller, so these run
// under TSan too (`pixi run tsan -R 'unit\.follows no leak'`). Every credential is a fake from
// support/mdx_auth_fixture.hpp; nothing contacts a real host.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/session_store.hpp"
#include "support/fake_transport.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/mdx_follows_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using md::test::FakeTransport;
using md::test::RecordedRequest;

constexpr std::string_view form_type = "application/x-www-form-urlencoded";
constexpr std::string_view tripwire_prefix = "fixture: Authorization sent to ";

/// Every fixture credential: none may appear in a URL, and none in a body sent off the auth host.
constexpr std::array<std::string_view, 7> credentials{
    mdx::fixture_password,  mdx::fixture_client_secret, mdx::fixture_access_1,
    mdx::fixture_access_2,  mdx::fixture_refresh_1,     mdx::fixture_refresh_2,
    mdx::fixture_client_id,
};

/// Is `result` the fixture transport's leak tripwire?
[[nodiscard]] bool is_tripwire(const md::TransportResult& result) {
    return !result.has_value() && result.error().kind == md::TransportErrc::local &&
           result.error().message.starts_with(tripwire_prefix);
}

/// Swallows every download event (the run's outcome is what the tests check).
class SilentObserver final : public md::DownloadObserver {
public:
    SilentObserver() = default;
    void on_plan(const md::Plan& /*plan*/) override {}
    void on_epub_begin(const md::PlannedEpub& /*epub*/, std::size_t /*index*/,
                       std::size_t /*total*/) override {}
    void on_pages(std::size_t /*done*/, std::size_t /*total*/) override {}
    void on_epub_end(const md::EpubOutcome& /*outcome*/) override {}
    void on_warning(std::string_view /*message*/) override {}
    void on_verbose(int /*level*/, std::string_view /*message*/) override {}
};

/// Where the run's requests go.
enum class Api {
    fixture,  ///< `--api-base file://<tree>`: the URLs are the fixture's own (reports off)
    official, ///< the official API with MD@H reports on: every URL is mapped onto the tree
};

/// The official hosts the run talks to, each with the tree directory that answers for it. The
/// MD@H report endpoint is answered 200 by the hook itself; nothing reaches a network.
constexpr std::string_view official_api = "https://api.mangadex.org";
constexpr std::string_view official_uploads = "https://uploads.mangadex.org";
constexpr std::string_view official_auth = "https://auth.mangadex.org";
constexpr std::string_view report_url = "https://api.mangadex.network/report";

/// `url` on the tree at `root` (`file://<tree>`): itself in fixture mode; in official mode the
/// API, uploads, auth and at-home hosts mapped onto their fixture directories. nullopt: a URL
/// nothing should ask for.
[[nodiscard]] std::optional<std::string> fixture_url(Api api, const std::string& root,
                                                     const std::string& url) {
    if (api == Api::fixture) {
        return url;
    }
    const std::array<std::pair<std::string_view, std::string>, 4> hosts{{
        {official_api, root + "/api"},
        {official_uploads, root + "/uploads"},
        {official_auth, root + "/auth"},
        {mdx::at_home_base_url, root + "/at-home"},
    }};
    for (const auto& [host, dir] : hosts) {
        if (url.starts_with(host) && (url.size() == host.size() || url[host.size()] == '/')) {
            return dir + url.substr(host.size());
        }
    }
    return std::nullopt;
}

/// How long the hook waits for the reporter thread (only a failing test waits this long).
constexpr std::chrono::seconds report_wait{20};

/// FS1 in a fresh TempDir, the fixture transport over it and a FakeTransport that answers from
/// it and records every request. Not copyable or movable: the hook borrows members.
class Harness {
public:
    explicit Harness(Api api = Api::fixture) : api_(api), tmp_(make_tmp()) {
        const auto written = mdx::write_follows_tree(tree(), mdx::follows_status_fixture());
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        md::Result<std::unique_ptr<md::Transport>> made = md::make_fixture_transport(tree());
        REQUIRE(made.has_value());
        fixture_ = std::move(*made);
        md::Result<md::ApiBase> base = md::ApiBase::parse(api_base());
        REQUIRE(base.has_value());
        base_ = std::move(*base);
        // The hook runs on whichever thread fetches (FetchPool workers and the MD@H reporter
        // included) and outside the FakeTransport's lock. It borrows fixture_, fake_ and the
        // counters: all members, and fake_ (which owns the hook) is declared last, so it is
        // destroyed first. The root and the mode are copies.
        fake_.set_on_fetch([api, root = "file://" + tree().string(), fixture = fixture_.get(),
                            fake = &fake_, tripwires = &tripwires_, unmapped = &unmapped_,
                            reports = &reports_,
                            images = &images_](const md::HttpRequest& request) {
            if (request.url == report_url) {
                reports->fetch_add(1);
                fake->push(request.url, md::HttpResponse{.status = 200});
                return;
            }
            const std::optional<std::string> mapped = fixture_url(api, root, request.url);
            if (!mapped.has_value()) {
                unmapped->fetch_add(1); // FakeTransport then answers "unscripted URL"
                return;
            }
            // With reports on (one image at a time, --jobs 1), every image after the first waits
            // until the reporter thread has sent a report: the client discards queued reports
            // when it is destroyed, so this makes at least one report POST certain.
            if (api == Api::official && request.url.starts_with(mdx::at_home_base_url) &&
                images->fetch_add(1) > 0) {
                const auto deadline = std::chrono::steady_clock::now() + report_wait;
                while (reports->load() == 0 && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            // A test-only copy of a request that carries only fake values.
            md::HttpRequest forwarded = request;
            forwarded.url = *mapped;
            md::TransportResult answer = fixture->fetch(forwarded);
            if (is_tripwire(answer)) {
                tripwires->fetch_add(1);
            }
            fake->push(request.url, std::move(answer));
        });
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;
    ~Harness() = default;

    [[nodiscard]] Api api_kind() const noexcept { return api_; }
    [[nodiscard]] fs::path tree() const { return tmp_.path() / "tree"; }
    [[nodiscard]] std::string api_base() const {
        return api_ == Api::official ? std::string{official_api} : "file://" + tree().string();
    }
    [[nodiscard]] std::string api() const {
        return api_ == Api::official ? std::string{official_api} : api_base() + "/api";
    }
    [[nodiscard]] std::string token_url() const {
        return api_ == Api::official
                   ? std::format("{}/realms/mangadex/protocol/openid-connect/token", official_auth)
                   : mdx::token_url(tree());
    }
    [[nodiscard]] std::string revoke_url() const {
        return api_ == Api::official
                   ? std::format("{}/realms/mangadex/protocol/openid-connect/revoke", official_auth)
                   : mdx::revoke_url(tree());
    }
    [[nodiscard]] std::string status_url() const { return md::status_map_request_url(api()); }
    [[nodiscard]] const md::ApiBase& base() const noexcept { return base_; }
    [[nodiscard]] FakeTransport& transport() noexcept { return fake_; }
    [[nodiscard]] md::Transport& fixture() const noexcept { return *fixture_; }
    [[nodiscard]] md::Clock& clock() const noexcept { return *clock_; }
    [[nodiscard]] std::size_t tripwires() const noexcept { return tripwires_.load(); }
    [[nodiscard]] std::size_t unmapped() const noexcept { return unmapped_.load(); }

    [[nodiscard]] md::UserDirs dirs() const {
        const std::string home = (tmp_.path() / "home").string();
        const std::string config = (tmp_.path() / "config").string();
        const std::string state = (tmp_.path() / "state").string();
        return md::make_user_dirs(home, config, state);
    }

    /// The two allowlisted collection URLs (any follows page, the status map).
    [[nodiscard]] bool allowlisted(std::string_view url) const {
        return url.starts_with(api() + "/user/follows/manga?") || url == status_url();
    }

private:
    [[nodiscard]] static md::test::TempDir make_tmp() {
        auto tmp = md::test::TempDir::create("mangadex-dl-follows-no-leak");
        INFO((tmp ? std::string{} : tmp.error()));
        REQUIRE(tmp.has_value());
        return std::move(*tmp);
    }

    Api api_ = Api::fixture;
    md::test::TempDir tmp_; ///< set by the constructor
    md::ApiBase base_{};
    std::unique_ptr<md::Transport> fixture_{};
    std::unique_ptr<md::Clock> clock_ = md::make_instant_clock();
    std::atomic<std::size_t> tripwires_{0};
    std::atomic<std::size_t> unmapped_{0};
    std::atomic<std::size_t> reports_{0};
    std::atomic<std::size_t> images_{0};
    FakeTransport fake_{}; ///< last: its hook borrows the members above
};

[[nodiscard]] std::uint32_t no_jitter(std::uint32_t /*max*/) { return 0; }

/// What one whole run did.
struct Outcome {
    std::vector<md::MangaId> follows{};
    std::vector<md::MangaId> selected{};
    std::size_t downloads = 0;   ///< run_download calls
    std::size_t clean_epubs = 0; ///< files the clean title wrote
};

/// The whole `follows` run over `h`, with `--status reading` when `with_status`.
[[nodiscard]] Outcome run_follows(Harness& h, bool with_status) {
    Outcome outcome{};

    // 1. The login: the credentials file with all four keys -> one password grant.
    const fs::path creds = h.tree() / "creds" / "credentials";
    md::Result<md::CredentialsFile> file = md::load_credentials_file(creds, true);
    REQUIRE(file.has_value());
    md::LoginRequest request{
        .api_base = h.base(),
        .dirs = h.dirs(),
        .mode = md::LoginMode::reuse,
        .credentials = std::move(*file),
        .credentials_path = creds,
        .password_stdin = std::nullopt,
        .prompter = nullptr,
        .lock = md::LockTiming{},
    };
    // Member by member (the clang-analyzer false positive with std::function members).
    md::AuthServices auth{};
    auth.transport = &h.transport();
    auth.clock = &h.clock();
    auth.jitter = &no_jitter;
    auth.trace = nullptr;
    md::LoginNotes notes{};
    md::AuthResult<std::unique_ptr<md::LoginSession>> session =
        md::LoginSession::open(std::move(request), auth, std::stop_token{}, notes);
    INFO((session ? std::string{} : session.error().error.message));
    REQUIRE(session.has_value());
    REQUIRE(*session != nullptr);

    // 2. The collection, then the session is closed before any download.
    md::FollowsFetchServices fetch{};
    fetch.transport = &h.transport();
    fetch.clock = &h.clock();
    fetch.jitter = &no_jitter;
    fetch.trace = nullptr;
    md::AuthResult<md::FollowsCollection> collection =
        md::fetch_follows_collection(h.base(), fetch, **session, with_status);
    (*session)->close();
    INFO((collection ? std::string{} : collection.error().error.message));
    REQUIRE(collection.has_value());
    outcome.follows = collection->follows;

    // 3. The plan, exactly as the command builds it.
    md::RawFollowsArgs raw{};
    raw.defaults.dir = (h.tree().parent_path() / (with_status ? "out-status" : "out")).string();
    raw.defaults.api_base = h.api_base();
    // A file:// base never reports; the official one does, one image at a time (see the hook).
    const bool official = h.api_kind() == Api::official;
    raw.defaults.no_report = !official;
    if (official) {
        raw.defaults.jobs = 1;
    }
    if (with_status) {
        raw.status = {"reading"};
    }
    const md::Result<md::FollowsSettings> settings = md::validate_follows_args(raw);
    REQUIRE(settings.has_value());
    const std::optional<md::StatusFilter>& filter = settings->filter;
    const std::optional<md::StatusMap>& statuses = collection->statuses;
    REQUIRE(filter.has_value() == with_status);
    REQUIRE(statuses.has_value() == with_status); // asked for, and there is a follow
    if (filter.has_value() && statuses.has_value()) {
        outcome.selected = md::filter_follows(collection->follows, *statuses, *filter).titles;
    } else {
        outcome.selected = collection->follows;
    }
    const std::vector<md::BatchJob> jobs = md::plan_follows(outcome.selected, *settings);

    // 4. Every title, anonymously.
    for (const md::BatchJob& job : jobs) {
        if (job.duplicate_of.has_value()) {
            continue;
        }
        md::DownloadServices services{};
        services.transport = &h.transport();
        services.clock = &h.clock();
        services.jitter = &no_jitter;
        services.user_agent = "mangadex-dl-test";
        SilentObserver observer{};
        const md::Result<md::RunReport> report = md::run_download(job.options, services, observer);
        ++outcome.downloads;
        if (job.options.manga.str() == mdx::clean_manga_id) {
            INFO((report ? std::string{} : report.error().message));
            REQUIRE(report.has_value());
            outcome.clean_epubs = report->epubs.size();
        }
    }
    return outcome;
}

/// How many recorded requests satisfy `pred`.
template <class Pred>
[[nodiscard]] std::size_t count_if(const std::vector<RecordedRequest>& requests, Pred pred) {
    return static_cast<std::size_t>(std::ranges::count_if(requests, pred));
}

} // namespace

TEST_CASE("follows no leak: the token goes only to the two collection URLs; downloads are "
          "anonymous (AC8)",
          "[follows][no-leak]") {
    const Api api = GENERATE(Api::fixture, Api::official);
    const bool with_status = GENERATE(false, true);
    CAPTURE(api == Api::official, with_status);
    Harness h{api};
    const Outcome outcome = run_follows(h, with_status);

    // The run really covered the login, the collection and one whole title.
    CHECK(outcome.follows.size() == 4);
    CHECK(outcome.selected.size() == (with_status ? 1U : 4U));
    CHECK(outcome.downloads == outcome.selected.size());
    CHECK(outcome.clean_epubs == 2);
    CHECK(h.unmapped() == 0); // every request was answered from the tree (or as a report)

    const std::vector<RecordedRequest> requests = h.transport().requests();
    const std::string clean{mdx::clean_manga_id};
    const auto starting = [&requests](const std::string& prefix) {
        return count_if(requests,
                        [&prefix](const RecordedRequest& r) { return r.url.starts_with(prefix); });
    };
    const auto exactly = [&requests](const std::string& url) {
        return count_if(requests, [&url](const RecordedRequest& r) { return r.url == url; });
    };
    CHECK(exactly(h.token_url()) == 1);
    CHECK(exactly(md::follows_request_url(h.api(), 0)) == 1);
    CHECK(exactly(h.status_url()) == (with_status ? 1U : 0U));
    CHECK(starting(std::format("{}/manga/{}?", h.api(), clean)) +
              exactly(std::format("{}/manga/{}", h.api(), clean)) >=
          1);
    CHECK(starting(std::format("{}/manga/{}/feed", h.api(), clean)) >= 1);
    CHECK(starting(h.api() + "/cover") >= 1);
    CHECK(starting(h.api() + "/at-home/server/") >= 1);
    const bool official = api == Api::official;
    CHECK(starting(official ? std::string{mdx::at_home_base_url} + "/data"
                            : "file://" + h.tree().string() + "/at-home/data") >= 1);
    CHECK(starting(official ? std::string{official_uploads} + "/covers/"
                            : "file://" + h.tree().string() + "/uploads/covers/") >= 1);
    // MangaDex@Home reports: only with the official API, and then at least one was sent.
    if (official) {
        CHECK(exactly(std::string{report_url}) >= 1);
    } else {
        CHECK(exactly(std::string{report_url}) == 0);
    }

    // The token: only on the allowlist, and always the session's Bearer value there.
    const std::string bearer = mdx::bearer(mdx::fixture_access_1);
    std::size_t authorized = 0;
    for (const RecordedRequest& r : requests) {
        INFO(r.url);
        if (h.allowlisted(r.url)) {
            CHECK(r.authorization == bearer);
            ++authorized;
        } else {
            CHECK(r.authorization.empty());
        }
    }
    CHECK(authorized == (with_status ? 2U : 1U));

    // Bodies: only the auth host gets a form body. Every other POST (an MD@H report) and every
    // other request carries no Authorization and no credential in its URL or body.
    for (const RecordedRequest& r : requests) {
        INFO(r.url);
        const bool auth_host = r.url == h.token_url() || r.url == h.revoke_url();
        if (r.content_type == form_type) {
            CHECK(auth_host);
        }
        if (r.method == md::HttpMethod::post && !auth_host) {
            CHECK(r.url == report_url);
            CHECK(r.authorization.empty());
            CHECK(r.content_type == "application/json");
        }
        if (!r.body.empty()) {
            CHECK(r.method == md::HttpMethod::post);
        }
        for (const std::string_view secret : credentials) {
            CHECK(!r.url.contains(secret));
            if (!auth_host) {
                CHECK(!r.body.contains(secret));
            }
        }
    }

    // The fixture's tripwire saw every request (mapped, with its Authorization) and never fired.
    CHECK(h.tripwires() == 0);
}

TEST_CASE("follows no leak: the fixture's tripwire fires for a leaked Authorization (positive "
          "control)",
          "[follows][no-leak]") {
    Harness h{};
    const std::string bearer = mdx::bearer(mdx::fixture_access_1);
    for (const std::string& url : {std::format("{}/manga/{}", h.api(), mdx::clean_manga_id),
                                   std::format("{}/cover?manga[]={}", h.api(), mdx::clean_manga_id),
                                   std::format("{}/user/follows/group", h.api())}) {
        INFO(url);
        const md::TransportResult result = h.fixture().fetch(md::HttpRequest{
            .url = url, .authorization = bearer, .max_body_bytes = md::max_api_json_bytes});
        CHECK(is_tripwire(result));
        // ... and the message names the URL, never the token.
        if (!result.has_value()) {
            CHECK(!result.error().message.contains(mdx::fixture_access_1));
        }
    }
    // Through the recording FakeTransport, the hook counts it.
    const md::TransportResult through = h.transport().fetch(
        md::HttpRequest{.url = std::format("{}/manga/{}", h.api(), mdx::clean_manga_id),
                        .authorization = bearer,
                        .max_body_bytes = md::max_api_json_bytes});
    CHECK(is_tripwire(through));
    CHECK(h.tripwires() == 1);
}
