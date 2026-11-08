// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/token_test.cpp: the token layer's pure half (follows plan T8). BearerToken's
// charset and size; the exact password, refresh and revoke bodies (AC1); parse_token_response's
// accepted shapes and rejects (AC3); classify_token_error on the `error` code only (AC4, V2); and
// the user-facing refusal texts of token_refusal_error. No message ever contains a token, a
// secret or a password.
//
// Every credential is a fake from support/mdx_auth_fixture.hpp or is built at run time.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/mdx_auth_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using Catch::Matchers::ContainsSubstring;
using md::BearerToken;
using md::Errc;
using md::GrantKind;
using md::Secret;
using md::TokenErrorInfo;
using md::TokenRefusal;
using std::chrono::seconds;

/// The value of `optional`, which the caller has just REQUIREd to be engaged. An empty one (a
/// test bug) throws, which Catch2 reports; unlike REQUIRE, the check is visible to
/// bugprone-unchecked-optional-access.
template <class T> [[nodiscard]] const T& engaged(const std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

[[nodiscard]] std::string text_of(const Secret& secret) { return std::string{secret.reveal()}; }

/// A token that parse() accepts; REQUIREs it.
[[nodiscard]] BearerToken token(std::string_view text) {
    md::Result<BearerToken> parsed = BearerToken::parse(Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] md::ClientCredentials fixture_client() {
    return md::ClientCredentials{
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = Secret::copy_of(mdx::fixture_client_secret),
    };
}

/// A token response with the given access token and extra members.
[[nodiscard]] std::string response(std::string_view access, const nlohmann::json& extra = {}) {
    nlohmann::json body{{"access_token", access}};
    if (extra.is_object()) {
        body.update(extra);
    }
    return body.dump();
}

/// `n` copies of a token character (never a real-looking credential).
[[nodiscard]] std::string long_token(std::size_t n) {
    std::string text(n, 'k');
    return text;
}

} // namespace

// ---- BearerToken --------------------------------------------------------------------------------

TEST_CASE("token: BearerToken accepts its charset up to 16384 bytes", "[auth][token]") {
    const std::string text = GENERATE(std::string{"a"}, std::string{mdx::fixture_access_1},
                                      std::string{"AZaz09._~+/=-"}, long_token(16384));
    const md::Result<BearerToken> parsed = BearerToken::parse(Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    CHECK(parsed->value().reveal() == text);
    CHECK(text_of(parsed->authorization()) == mdx::bearer(text));
}

TEST_CASE("token: BearerToken rejects anything else without echoing it", "[auth][token]") {
    const std::string marker{mdx::fixture_access_1};
    const std::string text =
        GENERATE_COPY(std::string{}, long_token(16385), marker + "\r", marker + "\n", marker + " x",
                      marker + "\"", marker + std::string(1, '\0'), marker + "\xC3\xA9",
                      marker + ":", marker + ",");
    const md::Result<BearerToken> parsed = BearerToken::parse(Secret::copy_of(text));
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == Errc::network);
    CHECK_THAT(parsed.error().message, !ContainsSubstring(marker));
    CHECK_THAT(parsed.error().message, ContainsSubstring("16384"));
}

// ---- Form bodies (AC1) --------------------------------------------------------------------------

TEST_CASE("token: the password grant body is exact", "[auth][token]") {
    const Secret password = Secret::copy_of("a&b=c+d%e f \xC3\xA9");
    const Secret body = md::password_grant_body(fixture_client(), mdx::fixture_username, password);
    CHECK(text_of(body) ==
          std::format("grant_type=password&username={}&password={}&client_id={}&client_secret={}",
                      mdx::fixture_username, "a%26b%3Dc%2Bd%25e%20f%20%C3%A9",
                      mdx::fixture_client_id, mdx::fixture_client_secret));
}

TEST_CASE("token: the password grant body encodes the username and the client too",
          "[auth][token]") {
    const md::ClientCredentials client{.client_id = "id with space",
                                       .client_secret = Secret::copy_of("s/e+c=r")};
    const Secret password = Secret::copy_of(mdx::fixture_password);
    const Secret body = md::password_grant_body(client, "user@example.org", password);
    CHECK(text_of(body) ==
          std::format("grant_type=password&username=user%40example.org&password={}&client_id="
                      "id%20with%20space&client_secret=s%2Fe%2Bc%3Dr",
                      mdx::fixture_password));
}

TEST_CASE("token: the refresh grant body is exact", "[auth][token]") {
    const BearerToken refresh = token("r+/=.~_-1");
    const Secret body = md::refresh_grant_body(fixture_client(), refresh);
    CHECK(text_of(body) ==
          std::format("grant_type=refresh_token&refresh_token={}&client_id={}&client_secret={}",
                      "r%2B%2F%3D.~_-1", mdx::fixture_client_id, mdx::fixture_client_secret));
}

TEST_CASE("token: the revoke body is exact", "[auth][token]") {
    const BearerToken refresh = token(mdx::fixture_refresh_1);
    const Secret body = md::revoke_body(fixture_client(), refresh);
    CHECK(text_of(body) ==
          std::format("token={}&token_type_hint=refresh_token&client_id={}&client_secret={}",
                      mdx::fixture_refresh_1, mdx::fixture_client_id, mdx::fixture_client_secret));
}

// ---- parse_token_response (AC3) -----------------------------------------------------------------

TEST_CASE("token: a full token response", "[auth][token]") {
    const mdx::FixtureGrant grant{
        .access_token = std::string{mdx::fixture_access_1},
        .refresh_token = std::string{mdx::fixture_refresh_1},
        .expires_in = 300,
        .refresh_expires_in = 2'592'000,
    };
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(mdx::grant_json(grant).dump());
    REQUIRE(parsed.has_value());
    CHECK(parsed->access.value().reveal() == mdx::fixture_access_1);
    REQUIRE(parsed->refresh.has_value());
    CHECK(engaged(parsed->refresh).value().reveal() == mdx::fixture_refresh_1);
    CHECK(parsed->expires_in == seconds{300});
    CHECK(parsed->refresh_expires_in == std::optional{seconds{2'592'000}});
}

TEST_CASE("token: only access_token is required", "[auth][token]") {
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(response(mdx::fixture_access_2));
    REQUIRE(parsed.has_value());
    CHECK(parsed->access.value().reveal() == mdx::fixture_access_2);
    CHECK_FALSE(parsed->refresh.has_value());
    CHECK(parsed->expires_in == md::default_access_lifetime);
    CHECK_FALSE(parsed->refresh_expires_in.has_value());
}

TEST_CASE("token: a null refresh_token is absent", "[auth][token]") {
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(response(mdx::fixture_access_1, {{"refresh_token", nullptr}}));
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->refresh.has_value());
}

TEST_CASE("token: expires_in outside 1..86400 or not an integer reads as 900", "[auth][token]") {
    const nlohmann::json value =
        GENERATE(nlohmann::json(0), nlohmann::json(-5), nlohmann::json(86401), nlohmann::json(1.5),
                 nlohmann::json("300"), nlohmann::json(nullptr), nlohmann::json(true),
                 nlohmann::json::array(), nlohmann::json(18'446'744'073'709'551'615ULL));
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(response(mdx::fixture_access_1, {{"expires_in", value}}));
    REQUIRE(parsed.has_value());
    CHECK(parsed->expires_in == seconds{900});
}

TEST_CASE("token: expires_in 1 and 86400 are kept", "[auth][token]") {
    const int value = GENERATE(1, 86400);
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(response(mdx::fixture_access_1, {{"expires_in", value}}));
    REQUIRE(parsed.has_value());
    CHECK(parsed->expires_in == seconds{value});
}

TEST_CASE("token: refresh_expires_in 0, absent or unusable is no known expiry", "[auth][token]") {
    const nlohmann::json value =
        GENERATE(nlohmann::json(0), nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("1800"),
                 nlohmann::json(nullptr), nlohmann::json(std::int64_t{3'155'760'001}));
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(response(mdx::fixture_access_1, {{"refresh_expires_in", value}}));
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->refresh_expires_in.has_value());
}

TEST_CASE("token: a positive refresh_expires_in is kept", "[auth][token]") {
    const std::int64_t value =
        GENERATE(std::int64_t{1}, std::int64_t{1800}, std::int64_t{3'155'760'000});
    const md::Result<md::TokenGrant> parsed =
        md::parse_token_response(response(mdx::fixture_access_1, {{"refresh_expires_in", value}}));
    REQUIRE(parsed.has_value());
    CHECK(parsed->refresh_expires_in == std::optional{seconds{value}});
}

TEST_CASE("token: malformed token responses are Errc::network without the token", "[auth][token]") {
    const std::string marker{mdx::fixture_access_1};
    const auto [body, reason] = GENERATE_COPY(table<std::string, std::string>({
        {"", "not valid JSON"},
        {"not json", "not valid JSON"},
        {std::format(R"({{"access_token":"{}"}} trailing)", marker), "not valid JSON"},
        {std::format(R"(["{}"])", marker), "not a JSON object"},
        {std::format(R"("{}")", marker), "not a JSON object"},
        {"{}", "no access_token"},
        {R"({"access_token":null})", "no access_token"},
        {std::format(R"({{"refresh_token":"{}"}})", marker), "no access_token"},
        {R"({"access_token":42})", "access_token is not a valid token"},
        {response(marker + "\r"), "access_token is not a valid token"},
        {response(marker + "\n"), "access_token is not a valid token"},
        {response(marker + " x"), "access_token is not a valid token"},
        {response(marker + "\""), "access_token is not a valid token"},
        {response(""), "access_token is not a valid token"},
        {response(long_token(16385)), "access_token is not a valid token"},
        {response(marker, {{"refresh_token", marker + " x"}}),
         "refresh_token is not a valid token"},
        {response(marker, {{"refresh_token", 7}}), "refresh_token is not a valid token"},
    }));
    CAPTURE(reason);
    const md::Result<md::TokenGrant> parsed = md::parse_token_response(body);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == Errc::network);
    CHECK_THAT(parsed.error().message, ContainsSubstring("MangaDex login response"));
    CHECK_THAT(parsed.error().message, ContainsSubstring(reason));
    CHECK_THAT(parsed.error().message, !ContainsSubstring(marker));
}

TEST_CASE("token: a token response over 64 KiB is refused before parsing", "[auth][token]") {
    std::string body = response(mdx::fixture_access_1);
    body.append(md::max_token_response_bytes - body.size(), ' ');
    CHECK(md::parse_token_response(body).has_value()); // exactly the cap: trailing JSON space
    body.push_back(' ');
    const md::Result<md::TokenGrant> parsed = md::parse_token_response(body);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == Errc::network);
    CHECK_THAT(parsed.error().message, ContainsSubstring("larger than 65536 bytes"));
}

TEST_CASE("token: a deeply nested token response is refused", "[auth][token]") {
    const std::string body =
        std::format(R"({{"access_token":"{}","x":{}{}}})", mdx::fixture_access_1,
                    std::string(100, '['), std::string(100, ']'));
    const md::Result<md::TokenGrant> parsed = md::parse_token_response(body);
    REQUIRE_FALSE(parsed.has_value());
    CHECK_THAT(parsed.error().message, ContainsSubstring("nested deeper"));
}

TEST_CASE("token: nested strings and unknown members are tolerated", "[auth][token]") {
    const md::Result<md::TokenGrant> parsed = md::parse_token_response(response(
        mdx::fixture_access_1, {{"scope", "openid"}, {"nested", {{"a", {"x", {{"b", "y"}}}}}}}));
    REQUIRE(parsed.has_value());
    CHECK(parsed->access.value().reveal() == mdx::fixture_access_1);
}

// ---- classify_token_error (AC4, V2) -------------------------------------------------------------

namespace {

[[nodiscard]] std::string error_body(std::string_view code, std::string_view description = {}) {
    return mdx::scripted_error(400, code, description).body.dump();
}

} // namespace

TEST_CASE("token: classification keys on the error code and the grant only", "[auth][token]") {
    const auto [grant, code, refusal] = GENERATE(table<GrantKind, std::string, TokenRefusal>({
        {GrantKind::password, "invalid_grant", TokenRefusal::bad_credentials},
        {GrantKind::refresh, "invalid_grant", TokenRefusal::session_ended},
        {GrantKind::password, "invalid_client", TokenRefusal::bad_client},
        {GrantKind::refresh, "invalid_client", TokenRefusal::bad_client},
        {GrantKind::password, "unauthorized_client", TokenRefusal::client_not_allowed},
        {GrantKind::refresh, "unauthorized_client", TokenRefusal::client_not_allowed},
        {GrantKind::password, "invalid_request", TokenRefusal::other},
        {GrantKind::refresh, "unsupported_grant_type", TokenRefusal::other},
        {GrantKind::password, std::string(64, 'z'), TokenRefusal::other},
    }));
    CAPTURE(code);
    const std::optional<TokenErrorInfo> info = md::classify_token_error(grant, error_body(code));
    REQUIRE(info.has_value());
    CHECK(engaged(info).refusal == refusal);
    CHECK(engaged(info).code == std::optional{code});
    CHECK(engaged(info).status == 0);
    CHECK_FALSE(engaged(info).description.has_value());
}

TEST_CASE("token: a body without a well-formed error code is not classifiable", "[auth][token]") {
    const std::string body = GENERATE(
        std::string{}, std::string{"<html>Forbidden</html>"}, std::string{"[]"}, std::string{"{}"},
        std::string{R"({"error":null})"}, std::string{R"({"error":42})"},
        std::string{R"({"error":""})"}, std::string{R"({"error":"Invalid_Grant"})"},
        std::string{R"({"error":"invalid-grant"})"}, std::string{R"({"error":"invalid grant"})"},
        error_body(std::string(65, 'z')),
        std::string{R"({"error_description":"Invalid user credentials"})"},
        std::string{R"({"result":"error","errors":[{"status":401}]})"});
    CAPTURE(body);
    CHECK_FALSE(md::classify_token_error(GrantKind::password, body).has_value());
    CHECK_FALSE(md::classify_token_error(GrantKind::refresh, body).has_value());
}

TEST_CASE("token: error_description is kept raw, cut to 200 bytes on a UTF-8 boundary",
          "[auth][token]") {
    SECTION("short") {
        const auto info = md::classify_token_error(
            GrantKind::password, error_body("invalid_grant", "Invalid user credentials"));
        REQUIRE(info.has_value());
        CHECK(engaged(info).description == std::optional<std::string>{"Invalid user credentials"});
    }
    SECTION("ASCII over the cut") {
        const auto info = md::classify_token_error(
            GrantKind::password, error_body("invalid_grant", std::string(300, 'd')));
        REQUIRE(info.has_value());
        CHECK(engaged(info).description == std::optional{std::string(200, 'd')});
    }
    SECTION("a two-byte character across byte 200") {
        const std::string text = std::string(199, 'd') + "\xC3\xA9" + "tail";
        const auto info =
            md::classify_token_error(GrantKind::password, error_body("invalid_grant", text));
        REQUIRE(info.has_value());
        CHECK(engaged(info).description == std::optional{std::string(199, 'd')});
    }
    SECTION("a three- or four-byte character across byte 200") {
        const auto [sequence, kept] = GENERATE(table<std::string, std::size_t>({
            {"\xE2\x82\xAC", 198},     // U+20AC from byte 198: bytes 198..200
            {"\xE2\x82\xAC", 199},     // from byte 199: bytes 199..201
            {"\xF0\x9F\x98\x80", 197}, // U+1F600 from byte 197: bytes 197..200
            {"\xF0\x9F\x98\x80", 199}, // from byte 199: bytes 199..202
        }));
        const std::string text = std::string(kept, 'd') + sequence + "tail";
        const auto info =
            md::classify_token_error(GrantKind::password, error_body("invalid_grant", text));
        REQUIRE(info.has_value());
        CHECK(engaged(info).description == std::optional{std::string(kept, 'd')});
    }
    SECTION("a character that ends exactly at byte 200 is kept") {
        const std::string text = std::string(196, 'd') + "\xF0\x9F\x98\x80" + "tail";
        const auto info =
            md::classify_token_error(GrantKind::password, error_body("invalid_grant", text));
        REQUIRE(info.has_value());
        CHECK(engaged(info).description ==
              std::optional{std::string(196, 'd') + "\xF0\x9F\x98\x80"});
    }
    SECTION("a description that is not a string is dropped") {
        const auto info = md::classify_token_error(
            GrantKind::password, R"({"error":"invalid_grant","error_description":7})");
        REQUIRE(info.has_value());
        CHECK_FALSE(engaged(info).description.has_value());
    }
}

// ---- token_refusal_error ------------------------------------------------------------------------

TEST_CASE("token: refusal texts", "[auth][token]") {
    SECTION("a wrong password names the user") {
        const auto info =
            md::classify_token_error(GrantKind::password, error_body("invalid_grant"));
        REQUIRE(info.has_value());
        const md::Error error =
            md::token_refusal_error(engaged(info), GrantKind::password, mdx::fixture_username);
        CHECK(error.code == Errc::auth_rejected);
        CHECK(error.message ==
              std::format("MangaDex rejected the username or password for {} (invalid_grant); "
                          "check them, or log in on the website to see whether the account is "
                          "locked",
                          mdx::fixture_username));
    }
    SECTION("without a username") {
        const md::Error error = md::token_refusal_error(
            TokenErrorInfo{.refusal = TokenRefusal::bad_credentials, .code = "invalid_grant"},
            GrantKind::password, {});
        CHECK(error.message ==
              "MangaDex rejected the username or password (invalid_grant); check them, or log in "
              "on the website to see whether the account is locked");
    }
    SECTION("a long username is cut to 64 bytes on a UTF-8 boundary") {
        const std::string username = std::string(63, 'u') + "\xC3\xA9";
        const md::Error error =
            md::token_refusal_error(TokenErrorInfo{.refusal = TokenRefusal::bad_credentials},
                                    GrantKind::password, username);
        CHECK_THAT(error.message, ContainsSubstring(std::format("for {} (", std::string(63, 'u'))));
    }
    SECTION("the client") {
        const md::Error error = md::token_refusal_error(
            TokenErrorInfo{.refusal = TokenRefusal::bad_client, .code = "invalid_client"},
            GrantKind::password, mdx::fixture_username);
        CHECK(error.code == Errc::auth_rejected);
        CHECK(error.message ==
              "MangaDex rejected the API client (invalid_client); check client_id and "
              "client_secret, and whether the client is approved at "
              "https://mangadex.org/settings");
    }
    SECTION("a client that is not allowed mentions approval") {
        const md::Error error =
            md::token_refusal_error(TokenErrorInfo{.refusal = TokenRefusal::client_not_allowed,
                                                   .code = "unauthorized_client"},
                                    GrantKind::password, mdx::fixture_username);
        CHECK(error.code == Errc::auth_rejected);
        CHECK(error.message == "MangaDex did not let the API client log in (unauthorized_client); "
                               "a new personal client may need approval by MangaDex first, see "
                               "https://mangadex.org/settings");
    }
    SECTION("the session ended, or invalid_grant on a refresh") {
        const TokenRefusal refusal =
            GENERATE(TokenRefusal::session_ended, TokenRefusal::bad_credentials);
        const md::Error error =
            md::token_refusal_error(TokenErrorInfo{.refusal = refusal, .code = "invalid_grant"},
                                    GrantKind::refresh, mdx::fixture_username);
        CHECK(error.code == Errc::login_required);
        CHECK(error.message == "your saved MangaDex login has expired or was ended; run "
                               "'mangadex-dl login' in a terminal");
    }
    SECTION("any other code") {
        const auto info =
            md::classify_token_error(GrantKind::password, error_body("temporarily_unavailable"));
        REQUIRE(info.has_value());
        const md::Error error = md::token_refusal_error(engaged(info), GrantKind::password, {});
        CHECK(error.code == Errc::auth_rejected);
        CHECK(error.message == "MangaDex refused the login (temporarily_unavailable)");
    }
    SECTION("a hand-built code outside [a-z_]{1,64} is never shown") {
        const std::string code =
            GENERATE(std::string{"Bad\x1b[31m"}, std::string{}, std::string(65, 'z'));
        const md::Error error = md::token_refusal_error(
            TokenErrorInfo{.refusal = TokenRefusal::other, .code = code}, GrantKind::password, {});
        CHECK(error.code == Errc::auth_rejected);
        CHECK(error.message == "MangaDex refused the login");
    }
    SECTION("no code at all") {
        const md::Error error = md::token_refusal_error(TokenErrorInfo{}, GrantKind::refresh, {});
        CHECK(error.message == "MangaDex refused the login");
    }
}

TEST_CASE("token: no refusal text contains the description", "[auth][token]") {
    const std::string description{mdx::fixture_password};
    const auto [grant, code] = GENERATE(table<GrantKind, std::string>({
        {GrantKind::password, "invalid_grant"},
        {GrantKind::refresh, "invalid_grant"},
        {GrantKind::password, "invalid_client"},
        {GrantKind::password, "unauthorized_client"},
        {GrantKind::password, "other_code"},
    }));
    const auto info = md::classify_token_error(grant, error_body(code, description));
    REQUIRE(info.has_value());
    CHECK(engaged(info).description == std::optional{description});
    const md::Error error = md::token_refusal_error(engaged(info), grant, mdx::fixture_username);
    CHECK_THAT(error.message, !ContainsSubstring(description));
}
