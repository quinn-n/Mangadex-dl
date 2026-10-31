// SPDX-License-Identifier: GPL-3.0-or-later
//
// The token layer's pure half (follows plan T8): BearerToken, the three form bodies, the token
// response parser, and the classification of token-endpoint refusals.
//
// Secret hygiene:
// - The bodies are built in one Secret reserved to the exact final size, from form_encode'd
//   Secrets, so no reallocation leaves a copy behind.
// - Untrusted JSON (a token response, a refusal) is parsed into a DOM whose every string is wiped
//   in place (get_ptr<std::string*>, V25) before the DOM is destroyed, on every path. The DOM is
//   never copied. The response body itself is the caller's to wipe (AuthClient does).
// - Every message is fixed text; nothing from a body reaches an Error except a refusal code that
//   matches [a-z_]{1,64}. `error_description` is kept, cut, in TokenErrorInfo for -v only.
//
// Accepted residuals (out of reach without patching the libraries; best effort, not a gate):
// - nlohmann's lexer keeps the bytes of the string it is scanning in its own token_buffer /
//   token_string, which are freed unwiped when the parse ends;
// - after a syntax error nlohmann discards the partially built DOM itself, so its strings are
//   freed unwiped too (only a complete DOM reaches DomWipe);
// - the transport's and libcurl's own copies of the request and response bytes.
#include "json_util.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/text_safety.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

using json_util::Json;

/// The document name in parse messages ("malformed MangaDex login response: …").
inline constexpr std::string_view response_name = "MangaDex login response";

/// `error_description` is cut to this many bytes (on a UTF-8 boundary).
inline constexpr std::size_t max_description_bytes = 200;
/// The longest refusal code kept.
inline constexpr std::size_t max_code_bytes = 64;
/// The username in a message is cut to this many bytes (on a UTF-8 boundary).
inline constexpr std::size_t max_username_display_bytes = 64;
/// The longest refresh_expires_in kept (100 years). Anything longer (or not a positive integer)
/// reads as "no known expiry", so adding it to a UNIX time can never overflow later.
inline constexpr std::int64_t max_refresh_expires_in = std::int64_t{3'155'760'000};

[[nodiscard]] constexpr bool is_token_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '~' || c == '+' || c == '/' || c == '=' || c == '-';
}

[[nodiscard]] constexpr bool is_code_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || c == '_';
}

/// `[a-z_]{1,64}`: the only shape of a server refusal code that may appear in a message.
[[nodiscard]] bool is_refusal_code(std::string_view code) noexcept {
    return !code.empty() && code.size() <= max_code_bytes &&
           std::ranges::all_of(code, is_code_char);
}

/// The longest prefix of `text` of at most `max` bytes that does not split a UTF-8 sequence.
[[nodiscard]] std::string_view utf8_prefix(std::string_view text, std::size_t max) noexcept {
    if (text.size() <= max) {
        return text;
    }
    std::size_t length = max;
    while (length > 0 && detail::is_utf8_continuation(static_cast<unsigned char>(text[length]))) {
        --length;
    }
    return text.substr(0, length);
}

/// Wipes every string in `value` in place. Recursion is bounded by parse_untrusted's depth cap
/// (max_json_depth), and nothing here allocates, so it is safe in a destructor.
void wipe_strings(Json& value) noexcept {
    if (auto* const text = value.get_ptr<Json::string_t*>()) {
        secure_wipe(*text);
        return;
    }
    if (auto* const object = value.get_ptr<Json::object_t*>()) {
        // Keys are const and name fields; only the values can hold a token.
        for (auto& member : *object) {
            wipe_strings(member.second);
        }
        return;
    }
    if (auto* const array = value.get_ptr<Json::array_t*>()) {
        for (Json& item : *array) {
            wipe_strings(item);
        }
    }
}

/// Wipes a parsed DOM's strings when it goes out of scope. Declared right after the DOM, so it
/// runs before the DOM dies, on every path. Borrows the DOM.
class DomWipe {
public:
    explicit DomWipe(Json& doc MANGADEX_DL_LIFETIMEBOUND) noexcept : doc_(&doc) {}
    DomWipe(const DomWipe&) = delete;
    DomWipe& operator=(const DomWipe&) = delete;
    DomWipe(DomWipe&&) = delete;
    DomWipe& operator=(DomWipe&&) = delete;
    ~DomWipe() { wipe_strings(*doc_); }

private:
    Json* doc_ = nullptr; ///< borrowed; never null
};

[[nodiscard]] Error malformed(std::string_view detail) {
    return Error{.code = Errc::network,
                 .message = std::format("malformed {}: {}", response_name, detail)};
}

/// parse_untrusted at the token-response cap. Its messages name response_name; the code is
/// Errc::network either way (it is built with Errc::failure only to get the bare name).
[[nodiscard]] Result<Json> parse_response(std::string_view body) {
    Result<Json> parsed = json_util::parse_untrusted(body, response_name,
                                                     json_util::ParseLimits{
                                                         .max_bytes = max_token_response_bytes,
                                                         .code = Errc::failure,
                                                     });
    if (!parsed) {
        return std::unexpected(
            Error{.code = Errc::network, .message = std::move(parsed.error().message)});
    }
    return parsed;
}

/// The member `key` of `object`, or nullptr when it is absent or null.
[[nodiscard]] const Json* member(const Json::object_t& object, std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end() || it->second.is_null()) {
        return nullptr;
    }
    return &it->second;
}

/// A token from a DOM string: copied into a Secret (the DOM string is wiped by DomWipe), then
/// validated. nullopt when `value` is not a string or not a valid token.
[[nodiscard]] std::optional<BearerToken> token_from(const Json& value) {
    const auto* const text = value.get_ptr<const Json::string_t*>();
    if (text == nullptr) {
        return std::nullopt;
    }
    Result<BearerToken> token = BearerToken::parse(Secret::copy_of(*text));
    if (!token) {
        return std::nullopt;
    }
    return std::move(*token);
}

/// A JSON integer as int64, or nullopt (a float, a string, an unsigned value above int64).
[[nodiscard]] std::optional<std::int64_t> integer_of(const Json& value) noexcept {
    if (const auto* const u = value.get_ptr<const Json::number_unsigned_t*>()) {
        if (std::in_range<std::int64_t>(*u)) {
            return static_cast<std::int64_t>(*u);
        }
        return std::nullopt;
    }
    if (const auto* const i = value.get_ptr<const Json::number_integer_t*>()) {
        return *i;
    }
    return std::nullopt;
}

/// `expires_in`: an integer 1..max_access_lifetime, else default_access_lifetime.
[[nodiscard]] std::chrono::seconds access_lifetime(const Json* value) noexcept {
    if (value != nullptr) {
        if (const auto seconds = integer_of(*value);
            seconds.has_value() && *seconds >= 1 && *seconds <= max_access_lifetime.count()) {
            return std::chrono::seconds{*seconds};
        }
    }
    return default_access_lifetime;
}

/// `refresh_expires_in`: a positive integer up to max_refresh_expires_in, else nullopt (0 and
/// absent mean "no known expiry"; anything unusable is treated the same way).
[[nodiscard]] std::optional<std::chrono::seconds> refresh_lifetime(const Json* value) noexcept {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto seconds = integer_of(*value);
        seconds.has_value() && *seconds >= 1 && *seconds <= max_refresh_expires_in) {
        return std::chrono::seconds{*seconds};
    }
    return std::nullopt;
}

/// One `name=value` field of a form body; `value` is borrowed for the call.
struct FormInput {
    std::string_view name{}; ///< a literal field name (unreserved characters only)
    std::string_view value{};
};

/// `name=<form_encode(value)>` joined by '&', in order, built in one exact-size Secret.
[[nodiscard]] Secret form_body(std::span<const FormInput> fields) {
    std::vector<Secret> encoded{};
    encoded.reserve(fields.size());
    std::size_t total = 0;
    for (const FormInput& field : fields) {
        encoded.push_back(form_encode(field.value));
        total += (total == 0 ? 0U : 1U) + field.name.size() + 1 + encoded.back().size();
    }
    Secret body{};
    body.reserve(total);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            body.push_back('&');
        }
        body.append(fields[i].name);
        body.push_back('=');
        body.append(encoded[i].reveal());
    }
    return body;
}

} // namespace

// ---- BearerToken --------------------------------------------------------------------------------

Result<BearerToken> BearerToken::parse(Secret raw) {
    const std::string_view text = raw.reveal();
    if (text.empty() || text.size() > max_token_bytes ||
        !std::ranges::all_of(text, is_token_char)) {
        return std::unexpected(Error{
            .code = Errc::network,
            .message = std::format("a token is empty, longer than {} bytes, or has characters "
                                   "outside [A-Za-z0-9._~+/=-]",
                                   max_token_bytes),
        });
    }
    return BearerToken{std::move(raw)};
}

Secret BearerToken::authorization() const {
    constexpr std::string_view scheme = "Bearer ";
    Secret value{};
    value.reserve(scheme.size() + value_.size());
    value.append(scheme);
    value.append(value_.reveal());
    return value;
}

// ---- Form bodies --------------------------------------------------------------------------------

Secret password_grant_body(const ClientCredentials& client, std::string_view username,
                           const Secret& password) {
    const std::array fields{
        FormInput{.name = "grant_type", .value = "password"},
        FormInput{.name = "username", .value = username},
        FormInput{.name = "password", .value = password.reveal()},
        FormInput{.name = "client_id", .value = client.client_id},
        FormInput{.name = "client_secret", .value = client.client_secret.reveal()},
    };
    return form_body(fields);
}

Secret refresh_grant_body(const ClientCredentials& client, const BearerToken& refresh) {
    const std::array fields{
        FormInput{.name = "grant_type", .value = "refresh_token"},
        FormInput{.name = "refresh_token", .value = refresh.value().reveal()},
        FormInput{.name = "client_id", .value = client.client_id},
        FormInput{.name = "client_secret", .value = client.client_secret.reveal()},
    };
    return form_body(fields);
}

Secret revoke_body(const ClientCredentials& client, const BearerToken& refresh) {
    const std::array fields{
        FormInput{.name = "token", .value = refresh.value().reveal()},
        FormInput{.name = "token_type_hint", .value = "refresh_token"},
        FormInput{.name = "client_id", .value = client.client_id},
        FormInput{.name = "client_secret", .value = client.client_secret.reveal()},
    };
    return form_body(fields);
}

// ---- Token responses ----------------------------------------------------------------------------

Result<TokenGrant> parse_token_response(std::string_view body) {
    Result<Json> parsed = parse_response(body);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    Json& doc = *parsed;
    const DomWipe wipe{doc};

    const auto* const object = doc.get_ptr<const Json::object_t*>();
    if (object == nullptr) {
        return std::unexpected(malformed("not a JSON object"));
    }

    const Json* const access = member(*object, "access_token");
    if (access == nullptr) {
        return std::unexpected(malformed("no access_token"));
    }
    std::optional<BearerToken> access_token = token_from(*access);
    if (!access_token.has_value()) {
        return std::unexpected(malformed("access_token is not a valid token"));
    }

    std::optional<BearerToken> refresh_token{};
    if (const Json* const refresh = member(*object, "refresh_token"); refresh != nullptr) {
        refresh_token = token_from(*refresh);
        if (!refresh_token.has_value()) {
            return std::unexpected(malformed("refresh_token is not a valid token"));
        }
    }

    return TokenGrant{
        .access = std::move(*access_token),
        .refresh = std::move(refresh_token),
        .expires_in = access_lifetime(member(*object, "expires_in")),
        .refresh_expires_in = refresh_lifetime(member(*object, "refresh_expires_in")),
    };
}

// ---- Refusals -----------------------------------------------------------------------------------

std::optional<TokenErrorInfo> classify_token_error(GrantKind grant, std::string_view body) {
    Result<Json> parsed = parse_response(body);
    if (!parsed) {
        return std::nullopt;
    }
    Json& doc = *parsed;
    const DomWipe wipe{doc};

    const auto* const object = doc.get_ptr<const Json::object_t*>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const Json* const error = member(*object, "error");
    const auto* const code = error == nullptr ? nullptr : error->get_ptr<const Json::string_t*>();
    if (code == nullptr || !is_refusal_code(*code)) {
        return std::nullopt;
    }

    TokenErrorInfo info{.code = *code};
    if (*code == "invalid_grant") {
        info.refusal = grant == GrantKind::refresh ? TokenRefusal::session_ended
                                                   : TokenRefusal::bad_credentials;
    } else if (*code == "invalid_client") {
        info.refusal = TokenRefusal::bad_client;
    } else if (*code == "unauthorized_client") {
        info.refusal = TokenRefusal::client_not_allowed;
    } else {
        info.refusal = TokenRefusal::other;
    }
    if (const Json* const description = member(*object, "error_description");
        description != nullptr) {
        if (const auto* const text = description->get_ptr<const Json::string_t*>()) {
            info.description = std::string{utf8_prefix(*text, max_description_bytes)};
        }
    }
    return info;
}

Error token_refusal_error(const TokenErrorInfo& info, GrantKind grant, std::string_view username) {
    const auto session_ended = [] {
        return Error{.code = Errc::login_required,
                     .message = "your saved MangaDex login has expired or was ended; run "
                                "'mangadex-dl login' in a terminal"};
    };
    switch (info.refusal) {
    case TokenRefusal::bad_credentials: {
        // invalid_grant on a refresh means the session ended, not a wrong password.
        if (grant == GrantKind::refresh) {
            return session_ended();
        }
        const std::string_view shown = utf8_prefix(username, max_username_display_bytes);
        return Error{
            .code = Errc::auth_rejected,
            .message = std::format(
                "MangaDex rejected the username or password{}{} (invalid_grant); check them, or "
                "log in on the website to see whether the account is locked",
                shown.empty() ? "" : " for ", shown),
        };
    }
    case TokenRefusal::bad_client:
        return Error{
            .code = Errc::auth_rejected,
            .message = std::format("MangaDex rejected the API client (invalid_client); check "
                                   "client_id and client_secret, and whether the client is "
                                   "approved at {}",
                                   api_clients_url),
        };
    case TokenRefusal::client_not_allowed:
        return Error{
            .code = Errc::auth_rejected,
            .message = std::format("MangaDex did not let the API client log in "
                                   "(unauthorized_client); a new personal client may need "
                                   "approval by MangaDex first, see {}",
                                   api_clients_url),
        };
    case TokenRefusal::session_ended:
        return session_ended();
    case TokenRefusal::other:
        break;
    }
    // TokenRefusal::other, or an out-of-range value. The code is shown only in its safe shape.
    if (info.code.has_value() && is_refusal_code(*info.code)) {
        return Error{.code = Errc::auth_rejected,
                     .message = std::format("MangaDex refused the login ({})", *info.code)};
    }
    return Error{.code = Errc::auth_rejected, .message = "MangaDex refused the login"};
}

} // namespace mangadex_dl
