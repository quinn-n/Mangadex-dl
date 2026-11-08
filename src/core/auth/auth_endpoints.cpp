// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the login flow sends its token and revoke POSTs (auth.hpp; follows plan T11).
//
// Only two kinds of API base have a login server: the official API (MangaDex's Keycloak realm,
// official_auth_issuer) and a file:// fixture tree (the fixture transport's stand-in under
// <root>/auth/realms/mangadex). Every other base, a custom https mirror or a loopback http server,
// gets nullopt: a custom API base must never inherit the official auth host, or MangaDex tokens
// would end up in requests to someone else's server. The URLs are fixed (follows plan spec issue
// 2): no discovery request.
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/options.hpp"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

/// The realm path under a fixture root, as the fixture transport serves it.
inline constexpr std::string_view fixture_realm{"/auth/realms/mangadex"};
/// The two Keycloak endpoints, relative to the issuer.
inline constexpr std::string_view token_suffix{"/protocol/openid-connect/token"};
inline constexpr std::string_view revoke_suffix{"/protocol/openid-connect/revoke"};

[[nodiscard]] AuthEndpoints endpoints_under(std::string issuer) {
    std::string token_url = std::format("{}{}", issuer, token_suffix);
    std::string revoke_url = std::format("{}{}", issuer, revoke_suffix);
    return AuthEndpoints{
        .issuer = std::move(issuer),
        .token_url = std::move(token_url),
        .revoke_url = std::move(revoke_url),
    };
}

} // namespace

std::optional<AuthEndpoints> auth_endpoints_for(const ApiBase& base) {
    switch (base.kind()) {
    case ApiBase::Kind::official:
        return endpoints_under(std::string{official_auth_issuer});
    case ApiBase::Kind::fixture:
        // base.url() is `file://<absolute dir>` without a trailing '/'.
        return endpoints_under(std::format("{}{}", base.url(), fixture_realm));
    case ApiBase::Kind::https:
    case ApiBase::Kind::http_loopback:
        return std::nullopt;
    }
    return std::nullopt; // out-of-range enum value: no login server
}

} // namespace mangadex_dl
