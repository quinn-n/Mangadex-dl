// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/logout_test.cpp: logout (follows plan T12, AC24 core, Spec issue 9, rev3 S4):
// the revoke POST and the unlink; any 2xx revokes whatever the body (a 200 with an error JSON
// included), while a 3xx, 4xx, 5xx or transport error gives the warning and the file is still
// removed; not logged in (no file, no directory, no home) is removed == false; an unreadable or
// malformed session is removed without a revoke, with the Spec issue 9 warning; an insecure state
// directory is Errc::failure with nothing touched; a non-login API base is usage before any I/O;
// fixture mode touches only fixture-session.json; an interrupted revoke keeps the file; the lock.
//
// Over FakeTransport + FakeClock in a fresh TempDir (plus one pass against the fixture
// transport's Keycloak stand-in). Every credential is a fake from support/mdx_auth_fixture.hpp;
// nothing contacts a real host.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_transport.hpp"
#include "support/fs_test_util.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using md::Errc;
using md::HttpMethod;
using md::Secret;
using md::SessionSlot;
using md::test::FakeClock;
using md::test::FakeTransport;
using md::test::RecordedRequest;
using md::test::fs_util::make_temp_dir;
using md::test::fs_util::read_file;
using md::test::fs_util::write_file;
using std::chrono::seconds;

inline constexpr std::string_view unreadable_text =
    "could not end the session at MangaDex (the saved login is unreadable); the saved login was "
    "removed anyway";
inline constexpr std::string_view lock_timeout_text =
    "another mangadex-dl is using the saved login; try again";

[[nodiscard]] std::string official_url(std::string_view endpoint) {
    return std::format("{}/protocol/openid-connect/{}", md::official_auth_issuer, endpoint);
}

[[nodiscard]] std::uint32_t no_jitter(std::uint32_t /*max*/) { return 0; }

[[nodiscard]] md::BearerToken token(std::string_view text) {
    md::Result<md::BearerToken> parsed = md::BearerToken::parse(Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] std::string revoke_body(std::string_view refresh = mdx::fixture_refresh_1) {
    const md::ClientCredentials client{
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = Secret::copy_of(mdx::fixture_client_secret),
    };
    return std::string{md::revoke_body(client, token(refresh)).reveal()};
}

/// A fresh home under a TempDir, the fakes and the services that borrow them. Neither copyable
/// nor movable (services points at the members).
struct Harness {
    Harness() : dir(make_temp_dir("mangadex-dl-logout")) {
        // Member by member, not designated initializers (see AuthServices).
        services.transport = &transport;
        services.clock = &clock;
        services.jitter = no_jitter;
        services.trace = nullptr;
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;
    ~Harness() = default;

    [[nodiscard]] fs::path root() const { return dir.path(); }
    [[nodiscard]] md::UserDirs dirs() const {
        return md::UserDirs{.home = root() / "home", .xdg_state_home = root() / "state"};
    }
    [[nodiscard]] fs::path state_dir() const { return root() / "state" / "mangadex-dl"; }
    [[nodiscard]] fs::path session_file(SessionSlot slot = SessionSlot::real) const {
        return state_dir() / md::session_file_name(slot);
    }

    [[nodiscard]] md::Result<md::LogoutOutcome> logout(const md::ApiBase& base = md::ApiBase{},
                                                       std::stop_token stop = {}) const {
        return md::logout(base, dirs(), services, md::LockTiming{.timeout = seconds{0}},
                          std::move(stop));
    }

    /// The fixture account's saved login for `issuer`, with refresh token `refresh`.
    [[nodiscard]] static md::Session
    saved(std::optional<std::string_view> refresh = mdx::fixture_refresh_1,
          std::string_view issuer = md::official_auth_issuer) {
        md::Session session{
            .issuer = std::string{issuer},
            .client_id = std::string{mdx::fixture_client_id},
            .client_secret = Secret::copy_of(mdx::fixture_client_secret),
            .username = std::string{mdx::fixture_username},
            .access_token = token(mdx::fixture_access_1),
            .access_expires_at = 1'700'000'900,
        };
        if (refresh.has_value()) {
            session.refresh_token = token(*refresh);
        }
        return session;
    }

    /// Writes `session` into `slot` through the store (0700 directory, 0600 file), under the lock.
    void seed(const md::Session& session, SessionSlot slot = SessionSlot::real) const {
        md::FileResult<md::StateDir> state = md::StateDir::open(state_dir(), true);
        REQUIRE(state.has_value());
        FakeClock lock_clock{};
        const md::Result<md::SessionLock> lock =
            state->lock(lock_clock, md::LockTiming{.timeout = seconds{0}}, {});
        REQUIRE(lock.has_value());
        REQUIRE(state->write(slot, md::session_to_json(session)).has_value());
    }

    md::test::TempDir dir; ///< set by the constructor
    FakeTransport transport{};
    FakeClock clock{};
    md::AuthServices services{};
};

/// The one revoke POST the transport saw: to the official revoke URL, with the expected body.
void check_one_revoke(const FakeTransport& transport,
                      std::string_view refresh = mdx::fixture_refresh_1) {
    const std::vector<RecordedRequest> requests = transport.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == HttpMethod::post);
    CHECK(requests[0].url == official_url("revoke"));
    CHECK(requests[0].body == revoke_body(refresh));
    CHECK(requests[0].authorization.empty());
}

} // namespace

TEST_CASE("logout: revokes the saved refresh token and removes the file (AC24)", "[auth][logout]") {
    Harness h{};
    h.seed(Harness::saved());
    // A plain 200, a 200 with an error body (still success, V3) and a 204.
    struct Answer {
        int status = 200;
        std::string body{};
    };
    const Answer answer = GENERATE(Answer{.status = 200},
                                   Answer{.status = 200, .body = R"({"error":"invalid_token"})"},
                                   Answer{.status = 204});
    CAPTURE(answer.status, answer.body);
    h.transport.push_status(official_url("revoke"), answer.status, answer.body);

    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE(outcome.has_value());
    CHECK(*outcome == md::LogoutOutcome{.removed = true, .path = h.session_file()});
    check_one_revoke(h.transport);
    CHECK_FALSE(fs::exists(h.session_file()));
    CHECK(fs::exists(h.state_dir()));

    // Again: not logged in, and nothing is sent.
    const md::Result<md::LogoutOutcome> again = h.logout();
    REQUIRE(again.has_value());
    CHECK(*again == md::LogoutOutcome{.removed = false, .path = h.session_file()});
    CHECK(h.transport.requests().size() == 1);
}

TEST_CASE("logout: a failed revoke is a warning, and the file is removed anyway",
          "[auth][logout]") {
    Harness h{};
    h.seed(Harness::saved());
    struct Answer {
        int status = 0; ///< 0: a transport timeout instead
        std::string body{};
        std::string reason{};
    };
    const Answer answer = GENERATE(
        Answer{.status = 503, .reason = "HTTP 503 (server error)"},
        Answer{.status = 400, .body = R"({"error":"invalid_client"})", .reason = "HTTP 400"},
        Answer{.status = 302, .reason = "HTTP 302"}, Answer{.reason = "timed out"});
    CAPTURE(answer.status);
    if (answer.status == 0) {
        h.transport.push_error(official_url("revoke"), md::TransportErrc::timeout, "timed out");
    } else {
        h.transport.push_status(official_url("revoke"), answer.status, answer.body);
    }
    const std::string& reason = answer.reason;

    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE(outcome.has_value());
    CHECK(outcome->removed);
    CHECK(outcome->path == h.session_file());
    CHECK(outcome->warning ==
          std::optional<std::string>{std::format("could not end the session at MangaDex ({}: {}); "
                                                 "the saved login was removed anyway",
                                                 official_url("revoke"), reason)});
    check_one_revoke(h.transport); // never retried
    CHECK_FALSE(fs::exists(h.session_file()));
}

TEST_CASE("logout: not logged in", "[auth][logout]") {
    const Harness h{};
    SECTION("no state directory: nothing is created") {
        const md::Result<md::LogoutOutcome> outcome = h.logout();
        REQUIRE(outcome.has_value());
        CHECK(*outcome == md::LogoutOutcome{.removed = false, .path = h.session_file()});
        CHECK_FALSE(fs::exists(h.root() / "state"));
    }
    SECTION("a state directory without a session") {
        fs::create_directories(h.root() / "state");
        REQUIRE(::mkdir(h.state_dir().c_str(), 0700) == 0);
        const md::Result<md::LogoutOutcome> outcome = h.logout();
        REQUIRE(outcome.has_value());
        CHECK(*outcome == md::LogoutOutcome{.removed = false, .path = h.session_file()});
    }
    SECTION("no home directory") {
        const md::Result<md::LogoutOutcome> outcome = md::logout(
            md::ApiBase{}, md::UserDirs{}, h.services, md::LockTiming{.timeout = seconds{0}}, {});
        REQUIRE(outcome.has_value());
        CHECK(*outcome == md::LogoutOutcome{});
    }
    CHECK(h.transport.requests().empty());
}

TEST_CASE("logout: a session without a refresh token is removed without a request",
          "[auth][logout]") {
    const Harness h{};
    h.seed(Harness::saved(std::nullopt));
    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE(outcome.has_value());
    CHECK(*outcome == md::LogoutOutcome{.removed = true, .path = h.session_file()});
    CHECK(h.transport.requests().empty());
    CHECK_FALSE(fs::exists(h.session_file()));
}

TEST_CASE("logout: an unreadable or malformed session is removed without a revoke",
          "[auth][logout]") {
    const Harness h{};
    h.seed(Harness::saved());
    const std::string damage =
        GENERATE(as<std::string>{}, "not JSON", "an unknown format_version",
                 "for another login server", "readable by others", "a symbolic link");
    CAPTURE(damage);
    if (damage == "not JSON") {
        write_file(h.session_file(), "not json");
    } else if (damage == "an unknown format_version") {
        write_file(h.session_file(), R"({"format_version": 2})");
    } else if (damage == "for another login server") {
        h.seed(Harness::saved(mdx::fixture_refresh_1, "https://elsewhere.example/realms/x"));
    } else if (damage == "readable by others") {
        REQUIRE(::chmod(h.session_file().c_str(), 0644) == 0);
    } else {
        const fs::path target = h.root() / "elsewhere.json";
        fs::rename(h.session_file(), target);
        fs::create_symlink(target, h.session_file());
    }

    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE(outcome.has_value());
    CHECK(*outcome == md::LogoutOutcome{.removed = true,
                                        .path = h.session_file(),
                                        .warning = std::string{unreadable_text}});
    CHECK(h.transport.requests().empty());
    CHECK_FALSE(fs::exists(fs::symlink_status(h.session_file())));
}

TEST_CASE("logout: a symlinked session's target is never touched", "[auth][logout]") {
    const Harness h{};
    h.seed(Harness::saved());
    const fs::path target = h.root() / "elsewhere.json";
    fs::rename(h.session_file(), target);
    const std::string before = read_file(target);
    fs::create_symlink(target, h.session_file());

    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE(outcome.has_value());
    CHECK(outcome->removed);
    CHECK(read_file(target) == before);
}

TEST_CASE("logout: an insecure state directory is a failure, nothing touched", "[auth][logout]") {
    const Harness h{};
    h.seed(Harness::saved());
    const std::string before = read_file(h.session_file());
    const bool symlinked = GENERATE(false, true);
    std::string why{};
    if (symlinked) {
        const fs::path real = h.root() / "real-state";
        fs::rename(h.state_dir(), real);
        fs::create_directory_symlink(real, h.state_dir());
        why = "it is a symbolic link";
    } else {
        REQUIRE(::chmod(h.state_dir().c_str(), 0777) == 0);
        why = "other users can access it (mode 0777)";
    }
    CAPTURE(why);

    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(outcome.error().message ==
          std::format("could not remove the saved login at {}: the directory {} is not usable: {}",
                      h.session_file().string(), h.state_dir().string(), why));
    CHECK(h.transport.requests().empty());
    CHECK(read_file(h.session_file()) == before);
    static_cast<void>(::chmod(h.state_dir().c_str(), 0700));
}

TEST_CASE("logout: an API base without a login server is usage before any I/O", "[auth][logout]") {
    const Harness h{};
    h.seed(Harness::saved());
    md::Result<md::ApiBase> base = md::ApiBase::parse("https://example.org/api");
    REQUIRE(base.has_value());
    const md::Result<md::LogoutOutcome> outcome = h.logout(*base);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == Errc::usage);
    CHECK(h.transport.requests().empty());
    CHECK(fs::exists(h.session_file()));
}

TEST_CASE("logout: fixture mode removes only fixture-session.json", "[auth][logout]") {
    Harness h{};
    const fs::path tree = h.root() / "tree";
    md::Result<md::ApiBase> base = md::ApiBase::parse(std::format("file://{}", tree.string()));
    REQUIRE(base.has_value());
    const std::string issuer = std::format("file://{}/auth/realms/mangadex", tree.string());
    h.seed(Harness::saved());
    const std::string real_before = read_file(h.session_file(SessionSlot::real));
    h.seed(Harness::saved(mdx::fixture_refresh_2, issuer), SessionSlot::fixture);
    h.transport.push_status(mdx::revoke_url(tree), 200);

    const md::Result<md::LogoutOutcome> outcome = h.logout(*base);
    REQUIRE(outcome.has_value());
    CHECK(*outcome ==
          md::LogoutOutcome{.removed = true, .path = h.session_file(SessionSlot::fixture)});
    CHECK_FALSE(fs::exists(h.session_file(SessionSlot::fixture)));
    CHECK(read_file(h.session_file(SessionSlot::real)) == real_before);
    const std::vector<RecordedRequest> requests = h.transport.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == mdx::revoke_url(tree));
    CHECK(requests[0].body == revoke_body(mdx::fixture_refresh_2));
}

TEST_CASE("logout: an interrupted revoke keeps the saved login", "[auth][logout]") {
    Harness h{};
    h.seed(Harness::saved());
    const std::stop_source stop{};
    stop.request_stop();
    h.transport.push_status(official_url("revoke"), 200);
    const md::Result<md::LogoutOutcome> outcome = h.logout(md::ApiBase{}, stop.get_token());
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == Errc::interrupted);
    CHECK(fs::exists(h.session_file()));
}

TEST_CASE("logout: a lock held elsewhere times out, nothing touched", "[auth][logout]") {
    const Harness h{};
    h.seed(Harness::saved());
    md::FileResult<md::StateDir> other = md::StateDir::open(h.state_dir(), false);
    REQUIRE(other.has_value());
    FakeClock other_clock{};
    const md::Result<md::SessionLock> held =
        other->lock(other_clock, md::LockTiming{.timeout = seconds{0}}, {});
    REQUIRE(held.has_value());

    const md::Result<md::LogoutOutcome> outcome = h.logout();
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(outcome.error().message == lock_timeout_text);
    CHECK(h.transport.requests().empty());
    CHECK(fs::exists(h.session_file()));
}

TEST_CASE("logout: against the fixture's Keycloak", "[auth][logout]") {
    Harness h{};
    const fs::path tree = fs::absolute(h.root()) / "tree";
    mdx::FixtureAuth auth{};
    // A known token revokes; an unknown one answers 200 invalid_token, which is success; a
    // scripted 503 fails the revoke.
    const std::string answer = GENERATE(as<std::string>{}, "known", "unknown", "503");
    CAPTURE(answer);
    std::optional<std::string> warning{};
    if (answer == "unknown") {
        auth.revokes = {};
    } else if (answer == "503") {
        auth.revokes = {
            mdx::FixtureRevoke{.token = std::string{mdx::fixture_refresh_1}, .status = 503}};
        warning = "HTTP 503";
    }
    REQUIRE(mdx::write_auth_files(tree, auth).has_value());
    auto made = md::make_fixture_transport(tree);
    REQUIRE(made.has_value());
    const std::unique_ptr<md::Transport> fixture = std::move(*made);
    h.services.transport = fixture.get();
    md::Result<md::ApiBase> base = md::ApiBase::parse(std::format("file://{}", tree.string()));
    REQUIRE(base.has_value());
    h.seed(Harness::saved(mdx::fixture_refresh_1,
                          std::format("file://{}/auth/realms/mangadex", tree.string())),
           SessionSlot::fixture);

    const md::Result<md::LogoutOutcome> outcome = h.logout(*base);
    REQUIRE(outcome.has_value());
    CHECK(outcome->removed);
    if (warning.has_value()) {
        REQUIRE(outcome->warning.has_value());
        CHECK_THAT(outcome->warning.value_or(""), StartsWith("could not end the session at "
                                                             "MangaDex ("));
        CHECK_THAT(outcome->warning.value_or(""), ContainsSubstring(*warning));
    } else {
        CHECK_FALSE(outcome->warning.has_value());
    }
    CHECK_FALSE(fs::exists(h.session_file(SessionSlot::fixture)));
}
