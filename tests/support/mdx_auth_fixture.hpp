#pragma once
// tests/support/mdx_auth_fixture.hpp: the login side of a fixture tree (follows plan T6).
//
// Header-only, nlohmann/json and the standard library only (like mdx_fixture.hpp), so the fixture
// generator can write auth files too. write_auth_files() lays out what the fixture transport's
// Keycloak stand-in serves (include/mangadex_dl/net/fixture_transport.hpp):
//   auth/password/<username>.json   the password grant: {expect, [mismatch_status], status, body}
//   auth/refresh/<token>.json       one refresh grant:  {expect, status, body}
//   auth/revoke/<token>.json        one revoke answer:  {expect, status, body}
//   auth/access/<token>.json        an access token the API accepts: {} or {"valid_requests": n}
//
// Every value is obviously fake (`fixture-…`, `…-not-a-secret`); none is a real credential.
// Tests build Authorization values with bearer() at run time rather than spelling them out.

#include "support/mdx_fixture.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::test::mdx {

// ---- the fake account ---------------------------------------------------------------------------

inline constexpr std::string_view fixture_client_id{"fixture-client"};
inline constexpr std::string_view fixture_client_secret{"fixture-client-secret-not-a-secret"};
inline constexpr std::string_view fixture_username{"fixture-user"};
inline constexpr std::string_view fixture_password{"fixture-password-not-a-secret"};
inline constexpr std::string_view fixture_access_1{"fixture-access-1"};
inline constexpr std::string_view fixture_access_2{"fixture-access-2"};
inline constexpr std::string_view fixture_refresh_1{"fixture-refresh-1"};
inline constexpr std::string_view fixture_refresh_2{"fixture-refresh-2"};

/// A scripted answer in place of a grant (for example 400 `unauthorized_client`). A string body is
/// sent verbatim, any other JSON value as its text.
struct ScriptedAnswer {
    int status = 400;
    nlohmann::json body = nlohmann::json::object();
};

/// A Keycloak-style error answer: `{"error": code}` (plus `error_description` when given).
[[nodiscard]] inline ScriptedAnswer scripted_error(int status, std::string_view code,
                                                   std::string_view description = {}) {
    nlohmann::json body{{"error", code}};
    if (!description.empty()) {
        body["error_description"] = description;
    }
    return ScriptedAnswer{.status = status, .body = std::move(body)};
}

/// One successful token response, and whether the API accepts its access token.
struct FixtureGrant {
    std::string access_token{};
    std::optional<std::string> refresh_token{}; ///< nullopt: the response has none
    std::int64_t expires_in = 900;
    std::optional<std::int64_t> refresh_expires_in{}; ///< nullopt: the member is left out
    /// False: no auth/access file, so the API answers this access token with 401.
    bool access_accepted = true;
    /// The access file's `valid_requests`; nullopt: unlimited.
    std::optional<std::size_t> valid_requests{};
};

/// A scripted refresh answer for one refresh token (it replaces the chain's file, if any).
struct FixtureRefreshScript {
    std::string token{};
    ScriptedAnswer answer{};
};

/// A scripted revoke answer for one token. Tokens without one are answered 200
/// `{"error":"invalid_token"}` by the transport (Keycloak's behaviour, V3).
struct FixtureRevoke {
    std::string token{};
    int status = 200;
    nlohmann::json body = ""; ///< Keycloak's success body is empty
};

/// One account on the fixture's Keycloak.
///
/// The token chain: chain[0] answers the password grant; chain[i] (i >= 1) answers a refresh with
/// chain[i-1].refresh_token. The last refresh token of the chain has no refresh file, so using it
/// is 400 `invalid_grant` "Token is not active" (the session ended). Writing several accounts
/// into one root is fine as long as their tokens differ or their files agree.
struct FixtureAuth {
    std::string client_id = std::string{fixture_client_id};
    std::string client_secret = std::string{fixture_client_secret};
    std::string username = std::string{fixture_username};
    std::string password = std::string{fixture_password};
    /// The status of a wrong password; nullopt: the transport's default, 401. Newer Keycloak
    /// answers 400 (V2).
    std::optional<int> mismatch_status{};
    /// Answers a correct password grant instead of chain[0] (for example 400 unauthorized_client).
    std::optional<ScriptedAnswer> password_refusal{};
    std::vector<FixtureGrant> chain{
        FixtureGrant{.access_token = std::string{fixture_access_1},
                     .refresh_token = std::string{fixture_refresh_1}},
        FixtureGrant{.access_token = std::string{fixture_access_2},
                     .refresh_token = std::string{fixture_refresh_2}},
    };
    /// Written after the chain, so a script replaces the chain's file for the same token.
    std::vector<FixtureRefreshScript> refresh_scripts{};
    /// By default both refresh tokens of the default chain revoke with a plain 200.
    std::vector<FixtureRevoke> revokes{
        FixtureRevoke{.token = std::string{fixture_refresh_1}},
        FixtureRevoke{.token = std::string{fixture_refresh_2}},
    };
};

/// "Bearer <token>", the Authorization value the API expects.
[[nodiscard]] inline std::string bearer(std::string_view token) {
    return std::string{"Bearer "}.append(token);
}

/// The fixture's token and revoke URLs for a tree at `root` (auth_endpoints_for builds the same).
[[nodiscard]] inline std::string token_url(const std::filesystem::path& root) {
    return std::format("file://{}/auth/realms/mangadex/protocol/openid-connect/token",
                       root.string());
}
[[nodiscard]] inline std::string revoke_url(const std::filesystem::path& root) {
    return std::format("file://{}/auth/realms/mangadex/protocol/openid-connect/revoke",
                       root.string());
}

/// The token response for `grant`, shaped like Keycloak's (unknown members included on purpose).
[[nodiscard]] inline nlohmann::json grant_json(const FixtureGrant& grant) {
    nlohmann::json body{
        {"access_token", grant.access_token},
        {"expires_in", grant.expires_in},
        {"token_type", "Bearer"},
        {"not-before-policy", 0},
        {"scope", "openid groups profile email"},
    };
    if (grant.refresh_token.has_value()) {
        body["refresh_token"] = *grant.refresh_token;
    }
    if (grant.refresh_expires_in.has_value()) {
        body["refresh_expires_in"] = *grant.refresh_expires_in;
    }
    return body;
}

namespace detail {

/// The transport's auth name rule: `[A-Za-z0-9._-]{1,128}`, and neither `.` nor `..`.
[[nodiscard]] inline bool auth_name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

[[nodiscard]] inline nlohmann::json client_expect(const FixtureAuth& auth) {
    return nlohmann::json{{"client_id", auth.client_id}, {"client_secret", auth.client_secret}};
}

[[nodiscard]] inline nlohmann::json answer_file(nlohmann::json expect, int status,
                                                nlohmann::json body) {
    return nlohmann::json{
        {"expect", std::move(expect)}, {"status", status}, {"body", std::move(body)}};
}

[[nodiscard]] inline std::expected<void, std::string>
write_auth_file(const std::filesystem::path& root, std::string_view dir, std::string_view name,
                const nlohmann::json& doc) {
    if (!auth_name_ok(name)) {
        return std::unexpected(
            std::format("unsafe auth/{} name (must match [A-Za-z0-9._-]{{1,128}})", dir));
    }
    return write_json(root / "auth" / dir / (std::string{name} + ".json"), doc);
}

[[nodiscard]] inline std::expected<void, std::string> check_chain(const FixtureAuth& auth) {
    if (auth.chain.empty() && !auth.password_refusal.has_value()) {
        return std::unexpected(std::string{"a FixtureAuth needs a token chain or a refusal"});
    }
    for (std::size_t i = 0; i + 1 < auth.chain.size(); ++i) {
        if (!auth.chain[i].refresh_token.has_value()) {
            return std::unexpected(
                std::format("chain[{}] has no refresh token, but chain[{}] follows it", i, i + 1));
        }
    }
    if (auth.mismatch_status.has_value() &&
        (*auth.mismatch_status < 400 || *auth.mismatch_status > 499)) {
        return std::unexpected(std::string{"mismatch_status must be a 4xx status"});
    }
    return {};
}

} // namespace detail

/// Writes `auth`'s files under `root` (created if needed; existing files are overwritten, stale
/// ones are NOT removed). Every name must follow the transport's auth name rule.
[[nodiscard]] inline std::expected<void, std::string>
write_auth_files(const std::filesystem::path& root, const FixtureAuth& auth) {
    if (const auto ok = detail::check_chain(auth); !ok) {
        return ok;
    }

    nlohmann::json password_expect = detail::client_expect(auth);
    password_expect["password"] = auth.password;
    const ScriptedAnswer first =
        auth.password_refusal.has_value()
            ? *auth.password_refusal
            : ScriptedAnswer{.status = 200, .body = grant_json(auth.chain.front())};
    nlohmann::json password_file =
        detail::answer_file(std::move(password_expect), first.status, first.body);
    if (auth.mismatch_status.has_value()) {
        password_file["mismatch_status"] = *auth.mismatch_status;
    }
    if (const auto ok = detail::write_auth_file(root, "password", auth.username, password_file);
        !ok) {
        return ok;
    }

    for (std::size_t i = 0; i < auth.chain.size(); ++i) {
        const FixtureGrant& grant = auth.chain[i];
        // check_chain guarantees a refresh token on every grant but the last.
        if (i + 1 < auth.chain.size() && grant.refresh_token.has_value()) {
            const nlohmann::json file = detail::answer_file(detail::client_expect(auth), 200,
                                                            grant_json(auth.chain[i + 1]));
            if (const auto ok =
                    detail::write_auth_file(root, "refresh", *grant.refresh_token, file);
                !ok) {
                return ok;
            }
        }
        if (grant.access_accepted) {
            nlohmann::json access = nlohmann::json::object();
            if (grant.valid_requests.has_value()) {
                access["valid_requests"] = *grant.valid_requests;
            }
            if (const auto ok = detail::write_auth_file(root, "access", grant.access_token, access);
                !ok) {
                return ok;
            }
        }
    }

    for (const FixtureRefreshScript& script : auth.refresh_scripts) {
        const nlohmann::json file = detail::answer_file(detail::client_expect(auth),
                                                        script.answer.status, script.answer.body);
        if (const auto ok = detail::write_auth_file(root, "refresh", script.token, file); !ok) {
            return ok;
        }
    }
    for (const FixtureRevoke& revoke : auth.revokes) {
        const nlohmann::json file =
            detail::answer_file(detail::client_expect(auth), revoke.status, revoke.body);
        if (const auto ok = detail::write_auth_file(root, "revoke", revoke.token, file); !ok) {
            return ok;
        }
    }
    return {};
}

} // namespace mangadex_dl::test::mdx
