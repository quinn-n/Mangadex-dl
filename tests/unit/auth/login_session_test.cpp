// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/login_session_test.cpp: LoginSession (follows plan T12): AC6 (the token order
// and the 3-request cap), AC13 core (the prompt order and the hint), AC14/15 core (no way to ask:
// exit 8 with zero requests), AC17 (a wrong password at 401 and at 400 -> 9, the session bytes
// unchanged), AC18 (a refresh answered invalid_grant -> a silent password login, or 8 "expired"),
// AC21 core (the session is persisted before an interrupt is honoured), AC22 core (a failed save
// is a warning), AC23 (fixture mode uses only fixture-session.json), AC25 core (an insecure state
// directory or a symlinked session is ignored with a warning), the credentials-file mismatch, the
// prompt-answer rules, close() and the session lock.
//
// Over FakeTransport + FakeClock + FakePrompter in a fresh TempDir (HOME and XDG_STATE_HOME point
// into it), plus one pass against the fixture transport's Keycloak stand-in. Every credential is a
// fake from support/mdx_auth_fixture.hpp; nothing contacts a real host.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_prompter.hpp"
#include "support/fake_transport.hpp"
#include "support/fs_test_util.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using md::AuthFailureKind;
using md::Echo;
using md::Errc;
using md::HttpMethod;
using md::LoginHow;
using md::LoginMode;
using md::PromptStatus;
using md::Secret;
using md::SessionSlot;
using md::test::FakeClock;
using md::test::FakePrompter;
using md::test::FakeTransport;
using md::test::RecordedRequest;
using md::test::fs_util::make_temp_dir;
using md::test::fs_util::read_file;
using md::test::fs_util::write_file;
using std::chrono::seconds;

inline constexpr std::string_view lock_timeout_text =
    "another mangadex-dl is using the saved login; try again";
inline constexpr std::string_view not_logged_in_text =
    "you are not logged in to MangaDex; run 'mangadex-dl login' in a terminal (or pass "
    "--password-stdin)";
inline constexpr std::string_view expired_text =
    "your saved MangaDex login has expired or was ended; run 'mangadex-dl login' in a terminal";
inline constexpr std::string_view hint_text =
    "mangadex-dl: reading your follows needs a MangaDex personal API client; create one at "
    "https://mangadex.org/settings (API Clients)";
inline constexpr std::string_view closed_text = "the MangaDex login session is already closed";
inline constexpr std::string_view other_password{"wrong-password-not-a-secret"};
inline constexpr std::string_view fixture_access_3{"fixture-access-3"};
inline constexpr std::string_view fixture_refresh_3{"fixture-refresh-3"};

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

[[nodiscard]] std::uint32_t no_jitter(std::uint32_t /*max*/) { return 0; }

[[nodiscard]] md::BearerToken token(std::string_view text) {
    md::Result<md::BearerToken> parsed = md::BearerToken::parse(Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] md::ClientCredentials fixture_client() {
    return md::ClientCredentials{
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = Secret::copy_of(mdx::fixture_client_secret),
    };
}

/// The password-grant body for the fixture client and user.
[[nodiscard]] std::string password_body(std::string_view password = mdx::fixture_password) {
    const Secret secret = Secret::copy_of(password);
    return std::string{
        md::password_grant_body(fixture_client(), mdx::fixture_username, secret).reveal()};
}

[[nodiscard]] std::string refresh_body(std::string_view refresh) {
    return std::string{md::refresh_grant_body(fixture_client(), token(refresh)).reveal()};
}

/// A Keycloak-shaped token response.
[[nodiscard]] std::string grant_body(std::string_view access,
                                     std::optional<std::string_view> refresh,
                                     std::int64_t expires_in = 900) {
    mdx::FixtureGrant grant{.access_token = std::string{access}, .expires_in = expires_in};
    if (refresh.has_value()) {
        grant.refresh_token = std::string{*refresh};
    }
    return mdx::grant_json(grant).dump();
}

[[nodiscard]] std::string error_body(int status, std::string_view code) {
    return mdx::scripted_error(status, code).body.dump();
}

/// All four details in the credentials file.
[[nodiscard]] md::CredentialsFile full_file(std::string_view password = mdx::fixture_password) {
    return md::CredentialsFile{
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = Secret::copy_of(mdx::fixture_client_secret),
        .username = std::string{mdx::fixture_username},
        .password = Secret::copy_of(password),
    };
}

/// Only the API client in the credentials file.
[[nodiscard]] md::CredentialsFile client_file() {
    return md::CredentialsFile{
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = Secret::copy_of(mdx::fixture_client_secret),
    };
}

/// lstat permission bits (0777) of `path`, which must exist.
[[nodiscard]] mode_t mode_of(const fs::path& path) {
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    return st.st_mode & 0777U;
}

/// Clears the interrupt flag on entry and on exit, so a simulated signal never leaks into
/// another test.
class InterruptReset {
public:
    InterruptReset() noexcept { md::reset_interrupt_for_testing(); }
    InterruptReset(const InterruptReset&) = delete;
    InterruptReset& operator=(const InterruptReset&) = delete;
    InterruptReset(InterruptReset&&) = delete;
    InterruptReset& operator=(InterruptReset&&) = delete;
    ~InterruptReset() { md::reset_interrupt_for_testing(); }
};

/// A Transport that answers through a FakeTransport, then requests a stop: the answer arrives
/// "with a stop pending", as when Ctrl-C lands during a token POST.
class StopAfterFetch final : public md::Transport {
public:
    StopAfterFetch(FakeTransport& inner, std::stop_source stop) noexcept
        : inner_(&inner), stop_(std::move(stop)) {}

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& request) override {
        md::TransportResult result = inner_->fetch(request);
        stop_.request_stop();
        return result;
    }

private:
    FakeTransport* inner_ = nullptr; ///< borrowed; outlives this transport
    std::stop_source stop_{};
};

/// A fresh home under a TempDir, the fakes, and the services that borrow them. Neither copyable
/// nor movable (services points at the members).
struct Harness {
    Harness() : dir(make_temp_dir("mangadex-dl-login-session")) {
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
    [[nodiscard]] fs::path credentials_path() const { return root() / "credentials"; }
    [[nodiscard]] std::int64_t now() const {
        return std::chrono::duration_cast<seconds>(clock.wall_now().time_since_epoch()).count();
    }

    /// A request for the official API (the real slot); prompts only with `prompts`.
    [[nodiscard]] md::LoginRequest request(LoginMode mode, md::CredentialsFile file = {},
                                           bool prompts = false) {
        return md::LoginRequest{
            .api_base = md::ApiBase{},
            .dirs = dirs(),
            .mode = mode,
            .credentials = std::move(file),
            .credentials_path = credentials_path(),
            .prompter = prompts ? &prompter : nullptr,
            .lock = md::LockTiming{.poll = std::chrono::milliseconds{250}, .timeout = seconds{0}},
        };
    }

    [[nodiscard]] md::AuthResult<std::unique_ptr<md::LoginSession>>
    open(md::LoginRequest login, std::stop_token stop = {}) {
        return md::LoginSession::open(std::move(login), services, std::move(stop), notes);
    }

    /// The saved session for the fixture account, with the default chain's first tokens.
    [[nodiscard]] md::Session saved(std::int64_t access_valid_for,
                                    std::optional<std::int64_t> refresh_expires_at = {}) const {
        return md::Session{
            .issuer = std::string{md::official_auth_issuer},
            .client_id = std::string{mdx::fixture_client_id},
            .client_secret = Secret::copy_of(mdx::fixture_client_secret),
            .username = std::string{mdx::fixture_username},
            .refresh_token = token(mdx::fixture_refresh_1),
            .refresh_expires_at = refresh_expires_at,
            .access_token = token(mdx::fixture_access_1),
            .access_expires_at = now() + access_valid_for,
        };
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

    /// The session file in `slot`, parsed back (for `issuer`).
    [[nodiscard]] md::Session read_back(SessionSlot slot = SessionSlot::real,
                                        std::string_view issuer = md::official_auth_issuer) const {
        const std::string bytes = read_file(session_file(slot));
        md::Result<md::Session> parsed = md::parse_session(bytes, issuer);
        INFO((parsed ? std::string{} : parsed.error().message));
        REQUIRE(parsed.has_value());
        return std::move(*parsed);
    }

    /// True when the lock is free (taken and released at once).
    [[nodiscard]] bool lock_is_free() const {
        md::FileResult<md::StateDir> state = md::StateDir::open(state_dir(), false);
        REQUIRE(state.has_value());
        FakeClock lock_clock{};
        return state->lock(lock_clock, md::LockTiming{.timeout = seconds{0}}, {}).has_value();
    }

    md::test::TempDir dir; ///< set by the constructor
    FakeTransport transport{};
    FakeClock clock{};
    FakePrompter prompter{};
    md::AuthServices services{};
    md::LoginNotes notes{};
};

[[nodiscard]] std::string token_of(const std::optional<md::BearerToken>& value) {
    return std::string{engaged(value).value().reveal()};
}

[[nodiscard]] std::vector<std::string> bodies(const FakeTransport& transport) {
    std::vector<std::string> out{};
    for (const RecordedRequest& request : transport.requests()) {
        CHECK(request.method == HttpMethod::post);
        CHECK(request.authorization.empty());
        out.push_back(request.body);
    }
    return out;
}

} // namespace

// ---- AC6: the token order --------------------------------------------------------------------

TEST_CASE("login session: a saved access token valid for 60 s is reused with no request",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(61));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    CHECK(h.transport.requests().empty());
    CHECK(h.notes.how == LoginHow::saved);
    CHECK(h.notes.username == mdx::fixture_username);
    CHECK(h.notes.session_path == std::optional<fs::path>{h.session_file()});
    CHECK(h.notes.warnings.empty());
    CHECK_FALSE(h.notes.save_error.has_value());

    const md::AuthResult<Secret> authorization = (*session)->authorization({});
    REQUIRE(authorization.has_value());
    CHECK(authorization->reveal() == mdx::bearer(mdx::fixture_access_1));
    CHECK(h.transport.requests().empty());
}

TEST_CASE("login session: an access token with less than 60 s left is refreshed once",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(59));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::refreshed);
    CHECK(bodies(h.transport) == std::vector{refresh_body(mdx::fixture_refresh_1)});

    // The rotated refresh token is the one stored.
    const md::Session stored = h.read_back();
    CHECK(token_of(stored.refresh_token) == mdx::fixture_refresh_2);
    CHECK(token_of(stored.access_token) == mdx::fixture_access_2);
    CHECK(stored.access_expires_at == h.now() + 900);
    CHECK_FALSE(stored.refresh_expires_at.has_value());
    CHECK(mode_of(h.session_file()) == 0600);
    CHECK(mode_of(h.state_dir()) == 0700);

    const md::AuthResult<Secret> authorization = (*session)->authorization({});
    REQUIRE(authorization.has_value());
    CHECK(authorization->reveal() == mdx::bearer(mdx::fixture_access_2));
}

TEST_CASE("login session: a refresh grant without a new refresh token keeps the old one",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0, h.now() + 5000));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, std::nullopt));
    const auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    const md::Session stored = h.read_back();
    CHECK(token_of(stored.refresh_token) == mdx::fixture_refresh_1);
    CHECK(stored.refresh_expires_at == std::optional<std::int64_t>{h.now() + 5000});
    CHECK(token_of(stored.access_token) == mdx::fixture_access_2);
}

TEST_CASE("login session: a refresh answered invalid_grant falls back to the password (AC18)",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0));
    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password);
    CHECK(h.notes.warnings.empty());
    CHECK(bodies(h.transport) ==
          std::vector{refresh_body(mdx::fixture_refresh_1), password_body()});

    const md::Session stored = h.read_back();
    CHECK(token_of(stored.refresh_token) == mdx::fixture_refresh_2);
    CHECK(token_of(stored.access_token) == mdx::fixture_access_2);
}

TEST_CASE("login session: an ended session without a password source is 8 'expired' (AC18)",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0));
    const std::string before = read_file(h.session_file());
    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));

    const auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().kind == AuthFailureKind::local);
    CHECK(session.error().error.code == Errc::login_required);
    CHECK(session.error().error.message == expired_text);
    CHECK(h.transport.requests().size() == 1);
    CHECK(read_file(h.session_file()) == before); // the session bytes are unchanged
    CHECK(h.lock_is_free());
}

TEST_CASE("login session: an ended session prompts for the password only, without the hint",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0));
    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    h.prompter.answer(mdx::fixture_password);

    const auto session = h.open(h.request(LoginMode::reuse, {}, true));
    REQUIRE(session.has_value());
    CHECK(h.prompter.events() ==
          std::vector<std::string>{"ask (echo off): MangaDex password for fixture-user: "});
    CHECK(bodies(h.transport) ==
          std::vector{refresh_body(mdx::fixture_refresh_1), password_body()});
}

TEST_CASE("login session: a refresh token past its known expiry is not sent",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0, h.now() - 1));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password);
    CHECK(bodies(h.transport) == std::vector{password_body()});
}

TEST_CASE("login session: other refresh refusals and failures never fall back to the password",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0));
    SECTION("invalid_client -> 9 with the client text") {
        h.transport.push_status(official_url("token"), 401, error_body(401, "invalid_client"));
        const auto session = h.open(h.request(LoginMode::reuse, full_file()));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().kind == AuthFailureKind::refused);
        CHECK(session.error().error.code == Errc::auth_rejected);
        CHECK_THAT(session.error().error.message, ContainsSubstring("(invalid_client)"));
    }
    SECTION("unauthorized_client -> 9 with the approval text") {
        h.transport.push_status(official_url("token"), 400, error_body(400, "unauthorized_client"));
        const auto session = h.open(h.request(LoginMode::reuse, full_file()));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::auth_rejected);
        CHECK_THAT(session.error().error.message, ContainsSubstring("(unauthorized_client)"));
    }
    SECTION("a transport failure -> a request failure (3)") {
        h.transport.push_error(official_url("token"), md::TransportErrc::connect,
                               "connection refused");
        const auto session = h.open(h.request(LoginMode::reuse, full_file()));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().kind == AuthFailureKind::request);
        CHECK(session.error().error.code == Errc::network);
    }
    CHECK(h.transport.requests().size() == 1);
}

TEST_CASE("login session: never more than 3 token requests (AC6)", "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0));
    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(fixture_access_3, fixture_refresh_3));
    auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.transport.requests().size() == 2);

    const md::AuthResult<Secret> third = (*session)->refresh_after_unauthorized({});
    REQUIRE(third.has_value());
    CHECK(third->reveal() == mdx::bearer(fixture_access_3));
    CHECK(token_of(h.read_back().refresh_token) == fixture_refresh_3);

    const md::AuthResult<Secret> fourth = (*session)->refresh_after_unauthorized({});
    REQUIRE_FALSE(fourth.has_value());
    CHECK(fourth.error().kind == AuthFailureKind::content);
    CHECK(fourth.error().error.code == Errc::network);
    CHECK(fourth.error().error.message ==
          "more than 3 MangaDex login requests in one run; refusing to continue");
    CHECK(h.transport.requests().size() == 3);
}

// ---- authorization() and refresh_after_unauthorized() ----------------------------------------

TEST_CASE("login session: authorization() refreshes within 30 s of expiry, then saves",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(100));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());

    h.clock.advance(seconds{69}); // 31 s left: still fresh
    const md::AuthResult<Secret> first = (*session)->authorization({});
    REQUIRE(first.has_value());
    CHECK(first->reveal() == mdx::bearer(mdx::fixture_access_1));
    CHECK(h.transport.requests().empty());

    h.clock.advance(seconds{1}); // 30 s left: refreshed first
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> second = (*session)->authorization({});
    REQUIRE(second.has_value());
    CHECK(second->reveal() == mdx::bearer(mdx::fixture_access_2));
    CHECK(bodies(h.transport) == std::vector{refresh_body(mdx::fixture_refresh_1)});
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
}

TEST_CASE("login session: refresh_after_unauthorized() always refreshes", "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized({});
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(mdx::fixture_access_2));
    CHECK(bodies(h.transport) == std::vector{refresh_body(mdx::fixture_refresh_1)});
    CHECK(token_of(h.read_back().access_token) == mdx::fixture_access_2);
}

TEST_CASE("login session: refresh_after_unauthorized() falls back to the kept password",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    md::LoginRequest login = h.request(LoginMode::reuse, client_file());
    login.credentials.username = std::string{mdx::fixture_username};
    login.password_stdin = Secret::copy_of(mdx::fixture_password);
    auto session = h.open(std::move(login));
    REQUIRE(session.has_value());
    CHECK(h.transport.requests().empty());

    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized({});
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(mdx::fixture_access_2));
    CHECK(bodies(h.transport) ==
          std::vector{refresh_body(mdx::fixture_refresh_1), password_body()});
}

TEST_CASE("login session: an ended session during the fetch without a password is 8",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));
    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized({});
    REQUIRE_FALSE(renewed.has_value());
    CHECK(renewed.error().error.code == Errc::login_required);
    CHECK(renewed.error().error.message == expired_text);
}

// ---- close() and the lock --------------------------------------------------------------------

TEST_CASE("login session: the lock is held until close(); afterwards nothing is touched",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    CHECK_FALSE(h.lock_is_free());
    const std::string before = read_file(h.session_file());

    (*session)->close();
    CHECK(h.lock_is_free());
    (*session)->close(); // idempotent

    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> authorization = (*session)->authorization({});
    REQUIRE_FALSE(authorization.has_value());
    CHECK(authorization.error().kind == AuthFailureKind::local);
    CHECK(authorization.error().error.code == Errc::failure);
    CHECK(authorization.error().error.message == closed_text);

    const md::AuthResult<Secret> refreshed = (*session)->refresh_after_unauthorized({});
    REQUIRE_FALSE(refreshed.has_value());
    CHECK(refreshed.error().kind == AuthFailureKind::local);
    CHECK(refreshed.error().error.code == Errc::failure);
    CHECK(refreshed.error().error.message == closed_text);

    CHECK(h.transport.requests().empty());
    CHECK_FALSE(h.transport.drained());
    CHECK(read_file(h.session_file()) == before);
}

TEST_CASE("login session: destroying the session releases the lock", "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    {
        const auto session = h.open(h.request(LoginMode::reuse));
        REQUIRE(session.has_value());
        CHECK_FALSE(h.lock_is_free());
    }
    CHECK(h.lock_is_free());
}

TEST_CASE("login session: a lock held elsewhere times out before any request or prompt",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    md::FileResult<md::StateDir> other = md::StateDir::open(h.state_dir(), false);
    REQUIRE(other.has_value());
    FakeClock other_clock{};
    const md::Result<md::SessionLock> held =
        other->lock(other_clock, md::LockTiming{.timeout = seconds{0}}, {});
    REQUIRE(held.has_value());

    const auto session = h.open(h.request(LoginMode::password, full_file(), true));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().kind == AuthFailureKind::local);
    CHECK(session.error().error.code == Errc::failure);
    CHECK(session.error().error.message == lock_timeout_text);
    CHECK(h.transport.requests().empty());
    CHECK(h.prompter.events().empty());
}

TEST_CASE("login session: a stop while waiting for the lock is an interrupt, before any I/O",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    md::FileResult<md::StateDir> other = md::StateDir::open(h.state_dir(), false);
    REQUIRE(other.has_value());
    FakeClock other_clock{};
    const md::Result<md::SessionLock> held =
        other->lock(other_clock, md::LockTiming{.timeout = seconds{0}}, {});
    REQUIRE(held.has_value());

    const std::stop_source stop{};
    REQUIRE(stop.request_stop());
    md::LoginRequest login = h.request(LoginMode::password, full_file(), true);
    login.lock = md::LockTiming{.poll = std::chrono::milliseconds{250}, .timeout = seconds{60}};
    const auto session = h.open(std::move(login), stop.get_token());
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().kind == AuthFailureKind::request);
    CHECK(session.error().error.code == Errc::interrupted);
    CHECK(h.transport.requests().empty());
    CHECK(h.prompter.events().empty());
    CHECK(h.clock.sleeps().empty()); // it never waited
}

TEST_CASE("login session: an API base without a login server is usage before any I/O",
          "[auth][login session]") {
    const std::string base =
        GENERATE(as<std::string>{}, "https://example.org/api", "http://127.0.0.1:8080");
    Harness h{};
    md::LoginRequest login = h.request(LoginMode::password, full_file(), true);
    md::Result<md::ApiBase> parsed = md::ApiBase::parse(base);
    REQUIRE(parsed.has_value());
    login.api_base = std::move(*parsed);
    const auto session = h.open(std::move(login));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().kind == AuthFailureKind::local);
    CHECK(session.error().error.code == Errc::usage);
    CHECK(h.transport.requests().empty());
    CHECK(h.prompter.events().empty());
    CHECK_FALSE(fs::exists(h.root() / "state"));
}

// ---- AC13/14/15: prompts and the missing-detail paths ---------------------------------------

TEST_CASE("login session: the first login prompts in order, the hint first (AC13)",
          "[auth][login session]") {
    Harness h{};
    h.prompter.answer(mdx::fixture_client_id);
    h.prompter.answer(mdx::fixture_client_secret);
    h.prompter.answer(mdx::fixture_username);
    h.prompter.answer(mdx::fixture_password);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_1, mdx::fixture_refresh_1));
    {
        const auto session = h.open(h.request(LoginMode::reuse, {}, true));
        REQUIRE(session.has_value());
    }
    CHECK(h.prompter.events() == std::vector<std::string>{
                                     std::string{"notice: "}.append(hint_text),
                                     "ask (echo on): MangaDex API client id: ",
                                     "ask (echo off): MangaDex API client secret: ",
                                     "ask (echo on): MangaDex username: ",
                                     "ask (echo off): MangaDex password for fixture-user: ",
                                 });
    CHECK(h.notes.how == LoginHow::password);
    CHECK(h.notes.username == mdx::fixture_username);
    CHECK(bodies(h.transport) == std::vector{password_body()});

    // Saved at 0600 in a 0700 directory, without the password.
    CHECK(mode_of(h.state_dir()) == 0700);
    CHECK(mode_of(h.session_file()) == 0600);
    const std::string saved = read_file(h.session_file());
    CHECK_THAT(saved, !ContainsSubstring(std::string{mdx::fixture_password}));
    const md::Session stored = h.read_back();
    CHECK(stored.client_id == mdx::fixture_client_id);
    CHECK(stored.client_secret.reveal() == mdx::fixture_client_secret);
    CHECK(stored.username == mdx::fixture_username);

    // A second run asks nothing and sends nothing.
    FakePrompter second{};
    md::LoginRequest again = h.request(LoginMode::reuse);
    again.prompter = &second;
    md::LoginNotes notes{};
    const auto reused = md::LoginSession::open(std::move(again), h.services, {}, notes);
    REQUIRE(reused.has_value());
    CHECK(notes.how == LoginHow::saved);
    CHECK(second.events().empty());
    CHECK(h.transport.requests().size() == 1);
}

TEST_CASE("login session: only the missing details are asked for, the hint only for a client",
          "[auth][login session]") {
    Harness h{};
    h.prompter.answer(mdx::fixture_username);
    h.prompter.answer(mdx::fixture_password);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_1, mdx::fixture_refresh_1));
    const auto session = h.open(h.request(LoginMode::reuse, client_file(), true));
    REQUIRE(session.has_value());
    CHECK(h.prompter.notices().empty());
    CHECK(h.prompter.calls() ==
          std::vector<FakePrompter::Call>{
              {.question = "MangaDex username: ", .echo = Echo::on},
              {.question = "MangaDex password for fixture-user: ", .echo = Echo::off},
          });
}

TEST_CASE("login session: nothing to ask with and nothing saved is 8 with zero requests (AC14)",
          "[auth][login session]") {
    Harness h{};
    SECTION("no client at all") {
        const auto session = h.open(h.request(LoginMode::reuse));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().kind == AuthFailureKind::local);
        CHECK(session.error().error.code == Errc::login_required);
        CHECK(session.error().error.message ==
              std::format("no MangaDex API client is set up; run 'mangadex-dl login' in a "
                          "terminal, or put client_id and client_secret in {}",
                          h.credentials_path().string()));
    }
    SECTION("a client, but no username") {
        const auto session = h.open(h.request(LoginMode::reuse, client_file()));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::login_required);
        CHECK(session.error().error.message == not_logged_in_text);
    }
    SECTION("a client and a username, but no password") {
        md::CredentialsFile file = full_file();
        file.password.reset();
        const auto session = h.open(h.request(LoginMode::password, std::move(file)));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::login_required);
        CHECK(session.error().error.message == not_logged_in_text);
    }
    SECTION("the prompter has no terminal (AC15's --no-input is a null prompter)") {
        h.prompter.reply(PromptStatus::unavailable);
        const auto session = h.open(h.request(LoginMode::reuse, client_file(), true));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::login_required);
        CHECK(session.error().error.message == not_logged_in_text);
        CHECK(h.prompter.calls().size() == 1);
    }
    CHECK(h.transport.requests().empty());
    CHECK_FALSE(fs::exists(h.session_file()));
    CHECK(h.lock_is_free());
}

TEST_CASE("login session: a prompt answer that can't be saved or used is 8",
          "[auth][login session]") {
    struct Case {
        std::vector<std::string> answers{};
        PromptStatus last = PromptStatus::answered;
        std::string message{};
    };
    const std::string too_long(md::max_prompt_answer_bytes + 1, 'x');
    const Case c = GENERATE_REF(
        Case{.last = PromptStatus::empty, .message = "no client id given"},
        Case{.last = PromptStatus::too_long,
             .message = "the client id you typed is longer than 4096 bytes"},
        Case{.answers = {std::string{mdx::fixture_client_id}},
             .last = PromptStatus::too_long,
             .message = "the client secret you typed is longer than 4096 bytes"},
        Case{.answers = {std::string{mdx::fixture_client_id},
                         std::string{mdx::fixture_client_secret},
                         std::string{mdx::fixture_username}},
             .last = PromptStatus::too_long,
             .message = "the password you typed is longer than 4096 bytes"},
        Case{.answers = {std::string{mdx::fixture_client_id},
                         std::string{mdx::fixture_client_secret},
                         std::string{mdx::fixture_username}},
             .last = PromptStatus::empty,
             .message = "no password given"},
        Case{.answers = {too_long}, .message = "the client id you typed is longer than 4096 bytes"},
        Case{.answers = {"fixture\x01"
                         "client"},
             .message = "the client id you typed contains a control character or is not valid "
                        "UTF-8"},
        Case{.answers = {std::string{mdx::fixture_client_id}, "fixture\x7f"
                                                              "secret"},
             .message = "the client secret you typed contains a control character or is not "
                        "valid UTF-8"},
        Case{.answers = {std::string{mdx::fixture_client_id},
                         std::string{mdx::fixture_client_secret},
                         "fixture\xff"
                         "user"},
             .message = "the username you typed contains a control character or is not valid "
                        "UTF-8"},
        Case{.answers = {std::string{mdx::fixture_client_id},
                         std::string{mdx::fixture_client_secret},
                         "fixture\xc2\x9b"
                         "user"},
             .message = "the username you typed contains a control character or is not valid "
                        "UTF-8"});
    CAPTURE(c.message);

    Harness h{};
    for (const std::string& answer : c.answers) {
        h.prompter.answer(answer);
    }
    if (c.last != PromptStatus::answered) {
        h.prompter.reply(c.last);
    }
    const auto session = h.open(h.request(LoginMode::password, {}, true));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().kind == AuthFailureKind::local);
    CHECK(session.error().error.code == Errc::login_required);
    CHECK(session.error().error.message == c.message);
    CHECK(h.prompter.remaining() == 0);
    CHECK(h.transport.requests().empty());
    CHECK_FALSE(fs::exists(h.session_file()));
}

TEST_CASE("login session: the username in the password question is cut to 64 bytes",
          "[auth][login session]") {
    struct Case {
        std::string username{};
        std::string shown{};
    };
    const Case c = GENERATE(
        // byte 64 is the first byte of a 2-byte sequence: the cut backs off to 63 bytes
        Case{.username = std::string(63, 'a') + "\xc3\xa9xyz", .shown = std::string(63, 'a')},
        Case{.username = std::string(65, 'b'), .shown = std::string(64, 'b')},
        Case{.username = std::string(64, 'c'), .shown = std::string(64, 'c')});
    CAPTURE(c.username.size());
    Harness h{};
    md::CredentialsFile file = client_file();
    file.username = c.username;
    h.prompter.reply(PromptStatus::empty);
    const auto session = h.open(h.request(LoginMode::password, std::move(file), true));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().error.message == "no password given");
    CHECK(h.prompter.calls() ==
          std::vector<FakePrompter::Call>{
              {.question = std::format("MangaDex password for {}: ", c.shown), .echo = Echo::off},
          });
    CHECK(h.transport.requests().empty());
}

TEST_CASE("login session: a password with spaces and symbols is sent as typed",
          "[auth][login session]") {
    Harness h{};
    constexpr std::string_view typed{" a&b=c+d%e f \xc3\xa9 "};
    h.prompter.answer(typed);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_1, mdx::fixture_refresh_1));
    md::CredentialsFile file = full_file();
    file.password.reset();
    const auto session = h.open(h.request(LoginMode::password, std::move(file), true));
    REQUIRE(session.has_value());
    CHECK(bodies(h.transport) == std::vector{password_body(typed)});
}

// ---- the Q1 order ------------------------------------------------------------------------------

TEST_CASE("login session: login always runs the password grant, with the saved details",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600)); // valid, but login renews anyway
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    h.prompter.answer(mdx::fixture_password);
    const auto session = h.open(h.request(LoginMode::password, {}, true));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password);
    CHECK(h.prompter.events() ==
          std::vector<std::string>{"ask (echo off): MangaDex password for fixture-user: "});
    CHECK(bodies(h.transport) == std::vector{password_body()});
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
}

TEST_CASE("login session: the credentials file beats the session, stdin beats the file",
          "[auth][login session]") {
    Harness h{};
    md::Session saved = h.saved(600);
    saved.client_secret = Secret::copy_of("saved-client-secret-not-a-secret");
    h.seed(saved);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));

    md::LoginRequest login = h.request(LoginMode::password, full_file(other_password));
    login.password_stdin = Secret::copy_of(mdx::fixture_password);
    const auto session = h.open(std::move(login));
    REQUIRE(session.has_value());
    // The file's client secret (not the saved one) and stdin's password (not the file's).
    CHECK(bodies(h.transport) == std::vector{password_body()});
    CHECK(h.read_back().client_secret.reveal() == mdx::fixture_client_secret);
}

TEST_CASE("login session: a session for another client or account than the file is ignored",
          "[auth][login session]") {
    Harness h{};
    md::Session saved = h.saved(600);
    const bool other_client = GENERATE(true, false);
    std::string why{};
    if (other_client) {
        saved.client_id = "other-client";
        why = "it is for another API client than the one in";
    } else {
        saved.username = "other-user";
        why = "it is for another account than the one in";
    }
    CAPTURE(why);
    h.seed(saved);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password);
    CHECK(h.notes.warnings == std::vector<std::string>{std::format(
                                  "ignoring the saved login at {}: {} {}",
                                  h.session_file().string(), why, h.credentials_path().string())});
    CHECK(bodies(h.transport) == std::vector{password_body()});
    const md::Session stored = h.read_back();
    CHECK(stored.client_id == mdx::fixture_client_id);
    CHECK(stored.username == mdx::fixture_username);
}

// ---- AC17: refusals --------------------------------------------------------------------------

TEST_CASE("login session: a wrong password at 401 and at 400 is 9, the session unchanged (AC17)",
          "[auth][login session]") {
    const int status = GENERATE(401, 400);
    CAPTURE(status);
    Harness h{};
    h.seed(h.saved(600));
    const std::string before = read_file(h.session_file());
    h.transport.push_status(official_url("token"), status, error_body(status, "invalid_grant"));

    const auto session = h.open(h.request(LoginMode::password, full_file(other_password)));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().kind == AuthFailureKind::refused);
    CHECK(session.error().info.refusal == md::TokenRefusal::bad_credentials);
    CHECK(session.error().info.status == status);
    CHECK(session.error().error.code == Errc::auth_rejected);
    CHECK(session.error().error.message ==
          "MangaDex rejected the username or password for fixture-user (invalid_grant); check "
          "them, or log in on the website to see whether the account is locked");
    CHECK_THAT(session.error().error.message, !ContainsSubstring(std::string{other_password}));
    CHECK(h.transport.requests().size() == 1);
    CHECK(read_file(h.session_file()) == before);
    CHECK(h.lock_is_free());
}

TEST_CASE("login session: a wrong or unapproved client is 9 with its own text",
          "[auth][login session]") {
    Harness h{};
    SECTION("invalid_client") {
        h.transport.push_status(official_url("token"), 401, error_body(401, "invalid_client"));
        const auto session = h.open(h.request(LoginMode::password, full_file()));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::auth_rejected);
        CHECK_THAT(session.error().error.message,
                   ContainsSubstring("MangaDex rejected the API client (invalid_client)"));
    }
    SECTION("an unknown refusal code") {
        h.transport.push_status(official_url("token"), 400, error_body(400, "account_locked"));
        const auto session = h.open(h.request(LoginMode::password, full_file()));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::auth_rejected);
        CHECK(session.error().error.message == "MangaDex refused the login (account_locked)");
    }
    CHECK(h.transport.requests().size() == 1);
    CHECK_FALSE(fs::exists(h.session_file()));
}

TEST_CASE("login session: a password grant without a refresh token drops the old one",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600, h.now() + 5000));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, std::nullopt));
    const auto session = h.open(h.request(LoginMode::password, full_file()));
    REQUIRE(session.has_value());
    const md::Session stored = h.read_back();
    CHECK_FALSE(stored.refresh_token.has_value());
    CHECK_FALSE(stored.refresh_expires_at.has_value());
    CHECK(token_of(stored.access_token) == mdx::fixture_access_2);
}

// ---- AC21 core: persisted before an interrupt ----------------------------------------------

TEST_CASE("login session: a grant that arrives with SIGINT pending is saved first (AC21)",
          "[auth][login session]") {
    const InterruptReset reset{};
    Harness h{};
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_1, mdx::fixture_refresh_1));
    h.transport.set_on_fetch(
        [](const md::HttpRequest& /*request*/) { md::simulate_interrupt_for_testing(SIGINT); });
    const auto session = h.open(h.request(LoginMode::password, full_file()));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().error.code == Errc::interrupted);
    const md::Session stored = h.read_back();
    CHECK(token_of(stored.refresh_token) == mdx::fixture_refresh_1);
    CHECK(token_of(stored.access_token) == mdx::fixture_access_1);
    CHECK(h.lock_is_free());
}

TEST_CASE("login session: a rotated refresh token that arrives with a stop is saved first",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(0));
    const std::stop_source stop{};
    StopAfterFetch stopping{h.transport, stop};
    h.services.transport = &stopping;
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));

    const auto session = h.open(h.request(LoginMode::reuse), stop.get_token());
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().error.code == Errc::interrupted);
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
    CHECK(h.transport.requests().size() == 1);
}

TEST_CASE("login session: a refresh during the fetch that meets a stop is saved first",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    const std::stop_source stop{};
    StopAfterFetch stopping{h.transport, stop};
    h.services.transport = &stopping;
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());

    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized(stop.get_token());
    REQUIRE_FALSE(renewed.has_value());
    CHECK(renewed.error().error.code == Errc::interrupted);
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
}

TEST_CASE("login session: an interrupt at the password prompt saves nothing",
          "[auth][login session]") {
    Harness h{};
    md::CredentialsFile file = full_file();
    file.password.reset();
    h.prompter.reply(PromptStatus::interrupted);
    const auto session = h.open(h.request(LoginMode::password, std::move(file), true));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().error.code == Errc::interrupted);
    CHECK(h.transport.requests().empty());
    CHECK_FALSE(fs::exists(h.session_file()));
    CHECK(h.lock_is_free());
}

// ---- AC22/23/25: where the session lives ---------------------------------------------------

TEST_CASE("login session: a session that can't be saved is a warning, and still works (AC22)",
          "[auth][login session]") {
    Harness h{};
    fs::create_directories(h.root() / "state");
    write_file(h.state_dir(), "not a directory");
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_1, mdx::fixture_refresh_1));
    auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    const std::string why = std::format("the directory {} is not usable: it is not a directory",
                                        h.state_dir().string());
    CHECK(h.notes.save_error == std::optional<std::string>{why});
    CHECK(h.notes.warnings ==
          std::vector<std::string>{std::format("could not save the MangaDex login to {}: {}; the "
                                               "next run will need the password again",
                                               h.session_file().string(), why)});
    CHECK(h.notes.session_path == std::optional<fs::path>{h.session_file()});
    CHECK(read_file(h.state_dir()) == "not a directory");

    const md::AuthResult<Secret> authorization = (*session)->authorization({});
    REQUIRE(authorization.has_value());
    CHECK(authorization->reveal() == mdx::bearer(mdx::fixture_access_1));

    // A later refresh can't be saved either; open() already said so, and it is not repeated.
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized({});
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(mdx::fixture_access_2));
    CHECK((*session)->take_late_warnings().empty());
}

TEST_CASE("login session: a save that fails after open() is a late warning",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    CHECK((*session)->take_late_warnings().empty());

    // A refresh that is saved raises nothing.
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    REQUIRE((*session)->refresh_after_unauthorized({}).has_value());
    CHECK((*session)->take_late_warnings().empty());
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);

    // The state directory disappears under the open session (its descriptor stays open), so the
    // next write fails: openat(O_CREAT) in a deleted directory is ENOENT, even as root.
    fs::remove(h.session_file());
    fs::remove(h.state_dir() / "session.lock");
    REQUIRE(fs::remove(h.state_dir()));
    h.transport.push_status(official_url("token"), 200,
                            grant_body(fixture_access_3, fixture_refresh_3));
    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized({});
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(fixture_access_3)); // still usable in memory

    const std::vector<std::string> late = (*session)->take_late_warnings();
    REQUIRE(late.size() == 1);
    CHECK_THAT(late[0],
               StartsWith(std::format("could not save the MangaDex login to {}: cannot create a "
                                      "temporary file: ",
                                      h.session_file().string())));
    CHECK_THAT(late[0], EndsWith("; the next run will need the password again"));
    for (const std::string_view secret : {fixture_refresh_3, fixture_access_3,
                                          mdx::fixture_client_secret, mdx::fixture_refresh_2}) {
        CHECK_THAT(late[0], !ContainsSubstring(std::string{secret}));
    }
    CHECK((*session)->take_late_warnings().empty()); // taken: cleared

    // The same through authorization(), then nothing after close().
    h.clock.advance(seconds{1000});
    h.transport.push_status(official_url("token"), 400, error_body(400, "invalid_grant"));
    const md::AuthResult<Secret> expired = (*session)->authorization({});
    REQUIRE_FALSE(expired.has_value()); // the session ended, no password: nothing to save
    CHECK((*session)->take_late_warnings().empty());
    (*session)->close();
    CHECK((*session)->take_late_warnings().empty());
}

TEST_CASE("login session: a late warning after authorization() refreshes",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(100));
    auto session = h.open(h.request(LoginMode::reuse));
    REQUIRE(session.has_value());
    fs::remove(h.session_file());
    fs::remove(h.state_dir() / "session.lock");
    REQUIRE(fs::remove(h.state_dir()));
    h.clock.advance(seconds{80});
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const md::AuthResult<Secret> authorization = (*session)->authorization({});
    REQUIRE(authorization.has_value());
    CHECK(authorization->reveal() == mdx::bearer(mdx::fixture_access_2));
    const std::vector<std::string> late = (*session)->take_late_warnings();
    REQUIRE(late.size() == 1);
    CHECK_THAT(late[0], StartsWith(std::format("could not save the MangaDex login to {}: ",
                                               h.session_file().string())));
    (*session)->close();
    CHECK((*session)->take_late_warnings().empty());
}

TEST_CASE("login session: without a home directory the login works but is not saved",
          "[auth][login session]") {
    Harness h{};
    md::LoginRequest login = h.request(LoginMode::password, full_file());
    login.dirs = md::UserDirs{};
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_1, mdx::fixture_refresh_1));
    const auto session = h.open(std::move(login));
    REQUIRE(session.has_value());
    CHECK_FALSE(h.notes.session_path.has_value());
    REQUIRE(h.notes.save_error.has_value());
    CHECK(h.notes.warnings ==
          std::vector<std::string>{std::format("could not save the MangaDex login: {}; the next "
                                               "run will need the password again",
                                               engaged(h.notes.save_error))});
    CHECK_FALSE(fs::exists(h.root() / "state"));
}

TEST_CASE("login session: fixture mode uses only fixture-session.json (AC23)",
          "[auth][login session]") {
    Harness h{};
    const fs::path tree = h.root() / "tree";
    md::Result<md::ApiBase> base = md::ApiBase::parse(std::format("file://{}", tree.string()));
    REQUIRE(base.has_value());
    const std::string issuer = std::format("file://{}/auth/realms/mangadex", tree.string());

    h.seed(h.saved(600)); // a real login, valid
    const std::string real_before = read_file(h.session_file(SessionSlot::real));
    h.transport.push_status(mdx::token_url(tree), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));

    md::LoginRequest login = h.request(LoginMode::reuse, full_file());
    login.api_base = *base;
    {
        const auto session = h.open(std::move(login));
        REQUIRE(session.has_value());
        CHECK(h.notes.how == LoginHow::password); // the real slot was never read
        CHECK(h.notes.session_path ==
              std::optional<fs::path>{h.session_file(SessionSlot::fixture)});
    }
    CHECK(read_file(h.session_file(SessionSlot::real)) == real_before);
    const md::Session fixture = h.read_back(SessionSlot::fixture, issuer);
    CHECK(fixture.issuer == issuer);
    CHECK(token_of(fixture.refresh_token) == mdx::fixture_refresh_2);
    CHECK(h.transport.count(official_url("token")) == 0);

    // A fixture login is never read by an official run either.
    md::LoginNotes notes{};
    const auto official =
        md::LoginSession::open(h.request(LoginMode::reuse), h.services, {}, notes);
    REQUIRE(official.has_value());
    CHECK(notes.how == LoginHow::saved);
    CHECK(h.transport.requests().size() == 1);
}

TEST_CASE("login session: an insecure state directory is ignored with warnings (AC25)",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    const std::string before = read_file(h.session_file());
    REQUIRE(::chmod(h.state_dir().c_str(), 0777) == 0);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));

    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password);
    const std::string why =
        std::format("the directory {} is not usable: other users can access it (mode 0777)",
                    h.state_dir().string());
    CHECK(h.notes.warnings ==
          std::vector<std::string>{
              std::format("ignoring the saved login at {}: {}", h.session_file().string(), why),
              std::format("could not save the MangaDex login to {}: {}; the next run will need "
                          "the password again",
                          h.session_file().string(), why),
          });
    CHECK(read_file(h.session_file()) == before); // nothing written
    REQUIRE(::chmod(h.state_dir().c_str(), 0700) == 0);
}

TEST_CASE("login session: a symlinked session file is ignored, never written through (AC25)",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    const fs::path target = h.root() / "elsewhere.json";
    fs::rename(h.session_file(), target);
    const std::string target_before = read_file(target);
    fs::create_symlink(target, h.session_file());
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));

    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password);
    CHECK(h.notes.warnings ==
          std::vector<std::string>{std::format(
              "ignoring the saved login at {}: it is a symbolic link", h.session_file().string())});
    CHECK(read_file(target) == target_before);
    // The new session replaced the link itself (renameat never follows it).
    CHECK_FALSE(fs::is_symlink(h.session_file()));
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
}

TEST_CASE("login session: a saved login for another login server or readable by others is ignored",
          "[auth][login session]") {
    Harness h{};
    const bool other_issuer = GENERATE(true, false);
    std::string why{};
    if (other_issuer) {
        md::Session saved = h.saved(600);
        saved.issuer = "https://elsewhere.example/realms/x";
        h.seed(saved);
        why = "it is for another login server";
    } else {
        h.seed(h.saved(600));
        REQUIRE(::chmod(h.session_file().c_str(), 0644) == 0);
        why = "other users can access it (mode 0644)";
    }
    CAPTURE(why);
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));

    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.how == LoginHow::password); // the saved (valid) access token was not used
    CHECK(h.notes.warnings ==
          std::vector<std::string>{
              std::format("ignoring the saved login at {}: {}", h.session_file().string(), why)});
    const std::vector<std::string> sent = bodies(h.transport);
    CHECK(sent == std::vector{password_body()});
    for (const std::string& body : sent) {
        CHECK_THAT(body, !ContainsSubstring(std::string{mdx::fixture_refresh_1}));
    }
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
}

TEST_CASE("login session: a malformed saved login is ignored with a warning",
          "[auth][login session]") {
    Harness h{};
    h.seed(h.saved(600));
    write_file(h.session_file(), "{\"format_version\": 2}");
    h.transport.push_status(official_url("token"), 200,
                            grant_body(mdx::fixture_access_2, mdx::fixture_refresh_2));
    const auto session = h.open(h.request(LoginMode::reuse, full_file()));
    REQUIRE(session.has_value());
    CHECK(h.notes.warnings ==
          std::vector<std::string>{std::format("ignoring the saved login at {}: it has an unknown "
                                               "format_version",
                                               h.session_file().string())});
    CHECK(token_of(h.read_back().refresh_token) == mdx::fixture_refresh_2);
}

// ---- the fixture's Keycloak ------------------------------------------------------------------

TEST_CASE("login session: against the fixture's Keycloak", "[auth][login session]") {
    Harness h{};
    const fs::path tree = fs::absolute(h.root()) / "tree";
    mdx::FixtureAuth auth{};
    SECTION("a password login, then a refresh") {
        REQUIRE(mdx::write_auth_files(tree, auth).has_value());
    }
    SECTION("a wrong password on newer Keycloak (400)") {
        auth.mismatch_status = 400;
        REQUIRE(mdx::write_auth_files(tree, auth).has_value());
    }
    auto made = md::make_fixture_transport(tree);
    REQUIRE(made.has_value());
    const std::unique_ptr<md::Transport> fixture = std::move(*made);
    h.services.transport = fixture.get();
    md::Result<md::ApiBase> base = md::ApiBase::parse(std::format("file://{}", tree.string()));
    REQUIRE(base.has_value());
    const std::string issuer = std::format("file://{}/auth/realms/mangadex", tree.string());

    if (auth.mismatch_status.has_value()) {
        md::LoginRequest login = h.request(LoginMode::password, full_file(other_password));
        login.api_base = *base;
        const auto session = h.open(std::move(login));
        REQUIRE_FALSE(session.has_value());
        CHECK(session.error().error.code == Errc::auth_rejected);
        CHECK(session.error().info.status == 400);
        CHECK_FALSE(fs::exists(h.session_file(SessionSlot::fixture)));
        return;
    }
    md::LoginRequest login = h.request(LoginMode::password, full_file());
    login.api_base = *base;
    auto session = h.open(std::move(login));
    REQUIRE(session.has_value());
    CHECK(token_of(h.read_back(SessionSlot::fixture, issuer).refresh_token) ==
          mdx::fixture_refresh_1);

    const md::AuthResult<Secret> renewed = (*session)->refresh_after_unauthorized({});
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(mdx::fixture_access_2));
    CHECK(token_of(h.read_back(SessionSlot::fixture, issuer).refresh_token) ==
          mdx::fixture_refresh_2);
    CHECK_FALSE(fs::exists(h.session_file(SessionSlot::real)));
}
