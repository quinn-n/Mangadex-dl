// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session document (follows plan T9): parse_session reads a saved login, session_to_json
// writes one. Format version 1, at most max_session_file_bytes:
//
//   {
//     "format_version": 1,
//     "issuer": "<the Keycloak realm URL>",
//     "client_id": "…", "client_secret": "…", "username": "…",
//     "refresh_token": "…" | null, "refresh_expires_at": <UNIX seconds> | null,
//     "access_token": "…" | null, "access_expires_at": <UNIX seconds>
//   }
//
// The file is the user's, but it is still untrusted (a world-writable session could plant someone
// else's token, and the store checks the mode; this checks the content): every field is
// validated, and an Error is a fixed reason phrase that never holds file content.
//
// Secret hygiene:
// - parse_session parses with json_util::parse_untrusted and works on that one DOM in place: it is
//   never copied. The secret strings (client_secret, the tokens) are moved out of the DOM into a
//   Secret (Secret::adopt wipes what it leaves behind), and a scope guard wipes every string still
//   in the DOM before it dies, on every path. `json` itself is the caller's to wipe.
// - Residual, out of reach (none is detectable from here):
//   - nlohmann's lexer assembles each string in its own buffer, which grows and is freed without a
//     wipe;
//   - a duplicate key is silently last-wins (V23), so nlohmann destroys the earlier value without
//     a wipe;
//   - when Json::parse fails late (a syntax error after some members), nlohmann frees the partly
//     built DOM without a wipe, and it may already hold the client secret or a token.
// - session_to_json is hand-written: it measures the document first, then writes it into one
//   Secret reserved to that size, so nothing reallocates and no plain std::string ever holds a
//   secret (only the two numbers go through a std::string). A \u00XX escape is assembled on the
//   stack, but only for a C0 control, which parse_session never lets into a saved value.
#include "json_util.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/text_safety.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

using json_util::Json;

inline constexpr std::uint64_t session_format_version = 1;
/// The document is one flat object; a little room for unknown (ignored) members.
inline constexpr std::size_t session_max_depth = 4;
/// The nine members cost 18 separators; this bounds the nodes a hostile 64 KiB file can make.
inline constexpr std::size_t session_max_separators = 1024;
/// client_id, client_secret and username: as long as a prompt answer can be.
inline constexpr std::size_t max_detail_bytes = max_prompt_answer_bytes;

/// Errc::failure with a fixed reason phrase (never file content).
[[nodiscard]] Error reason(std::string message) {
    return Error{.code = Errc::failure, .message = std::move(message)};
}

/// Wipes every string value in `value`. The recursion is bounded by session_max_depth (the
/// document came from parse_untrusted with that limit).
void wipe_strings(Json& value) noexcept {
    if (auto* text = value.get_ptr<Json::string_t*>(); text != nullptr) {
        secure_wipe(*text);
        return;
    }
    if (auto* object = value.get_ptr<Json::object_t*>(); object != nullptr) {
        for (auto& member : *object) {
            wipe_strings(member.second);
        }
        return;
    }
    if (auto* array = value.get_ptr<Json::array_t*>(); array != nullptr) {
        for (Json& item : *array) {
            wipe_strings(item);
        }
    }
}

/// Wipes the strings of the borrowed DOM when the scope ends, on every path.
class DomWipe {
public:
    explicit DomWipe(Json& doc MANGADEX_DL_LIFETIMEBOUND) noexcept : doc_(&doc) {}
    DomWipe(const DomWipe&) = delete;
    DomWipe& operator=(const DomWipe&) = delete;
    DomWipe(DomWipe&&) = delete;
    DomWipe& operator=(DomWipe&&) = delete;
    ~DomWipe() { wipe_strings(*doc_); }

private:
    Json* doc_ = nullptr;
};

/// A C0 control, DEL or a C1 control (or ill-formed UTF-8, which the parser already refuses).
[[nodiscard]] constexpr bool has_control(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const Utf8Decoded decoded = decode_utf8_at(text, pos);
        const std::uint32_t cp = decoded.code_point;
        if (decoded.length == 0 || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            return true;
        }
        pos += decoded.length;
    }
    return false;
}

/// The member `key`, or nullptr when it is absent or null.
[[nodiscard]] Json* member(Json::object_t& object, std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end() || it->second.is_null()) {
        return nullptr;
    }
    return &it->second;
}

[[nodiscard]] Error missing(std::string_view key) {
    return reason(std::format("'{}' is missing", key));
}

[[nodiscard]] Error wrong_type(std::string_view key) {
    return reason(std::format("'{}' has the wrong type", key));
}

/// A required string member, borrowed from the DOM (so it can be moved out and wiped).
[[nodiscard]] Result<std::string*> required_string(Json::object_t& object, std::string_view key) {
    Json* value = member(object, key);
    if (value == nullptr) {
        return std::unexpected(missing(key));
    }
    auto* text = value->get_ptr<Json::string_t*>();
    if (text == nullptr) {
        return std::unexpected(wrong_type(key));
    }
    return text;
}

/// client_id, client_secret or username: 1..max_detail_bytes bytes, no control characters.
[[nodiscard]] Result<std::string*> login_detail(Json::object_t& object, std::string_view key) {
    Result<std::string*> text = required_string(object, key);
    if (!text) {
        return text;
    }
    const std::string& value = **text;
    if (value.empty()) {
        return std::unexpected(reason(std::format("'{}' is empty", key)));
    }
    if (value.size() > max_detail_bytes) {
        return std::unexpected(
            reason(std::format("'{}' is longer than {} bytes", key, max_detail_bytes)));
    }
    if (has_control(value)) {
        return std::unexpected(reason(std::format("'{}' contains a control character", key)));
    }
    return text;
}

/// A token member: absent or null -> nullopt; otherwise it must pass BearerToken::parse. The DOM
/// string is moved into the Secret (and so wiped) either way.
[[nodiscard]] Result<std::optional<BearerToken>> token(Json::object_t& object,
                                                       std::string_view key) {
    Json* value = member(object, key);
    if (value == nullptr) {
        return std::optional<BearerToken>{};
    }
    auto* text = value->get_ptr<Json::string_t*>();
    if (text == nullptr) {
        return std::unexpected(wrong_type(key));
    }
    Result<BearerToken> parsed = BearerToken::parse(Secret::adopt(std::move(*text)));
    if (!parsed) {
        // BearerToken::parse's message is fixed and token-free, but the reason is ours.
        return std::unexpected(reason(std::format("'{}' is not a valid token", key)));
    }
    return std::optional<BearerToken>{std::move(*parsed)};
}

/// A UNIX time in seconds: a non-negative integer (a float or a negative number is refused).
[[nodiscard]] Result<std::optional<std::int64_t>>
unix_time(Json::object_t& object, std::string_view key, json_util::Need need) {
    const Json* value = member(object, key);
    if (value == nullptr) {
        if (need == json_util::Need::required) {
            return std::unexpected(missing(key));
        }
        return std::optional<std::int64_t>{};
    }
    // nlohmann stores a non-negative integer literal as number_unsigned_t and a negative one as
    // number_integer_t; a literal with a fraction or an exponent is a float.
    if (const auto* number = value->get_ptr<const Json::number_unsigned_t*>(); number != nullptr) {
        if (!std::in_range<std::int64_t>(*number)) {
            return std::unexpected(reason(std::format("'{}' is not a valid time", key)));
        }
        return std::optional<std::int64_t>{static_cast<std::int64_t>(*number)};
    }
    if (value->get_ptr<const Json::number_integer_t*>() != nullptr) {
        return std::unexpected(reason(std::format("'{}' is not a valid time", key)));
    }
    return std::unexpected(wrong_type(key));
}

// ---- the writer -----------------------------------------------------------------------------

inline constexpr std::string_view hex_digits = "0123456789abcdef";

/// Counts what a Secret would receive, so the Secret can be reserved once.
struct Measure {
    std::size_t size = 0;

    void append(std::string_view text) noexcept { size += text.size(); }
};

/// `text` as a JSON string literal: '"' and '\' escaped, C0 controls as \u00XX, every other byte
/// as is (the values are UTF-8; parse_session validates that on the way back).
template <class Sink> void append_string(Sink& out, std::string_view text) {
    out.append("\"");
    std::size_t run = 0; // start of the bytes not yet written
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0x20 && byte != '"' && byte != '\\') {
            continue;
        }
        out.append(text.substr(run, i - run));
        if (byte == '"') {
            out.append("\\\"");
        } else if (byte == '\\') {
            out.append("\\\\");
        } else {
            const std::array<char, 6> escape{
                '\\', 'u', '0', '0', hex_digits[byte >> 4U], hex_digits[byte & 0x0FU]};
            out.append(std::string_view{escape.data(), escape.size()});
        }
        run = i + 1;
    }
    out.append(text.substr(run));
    out.append("\"");
}

/// A number is no secret: a plain std::string is fine here.
template <class Sink> void append_number(Sink& out, std::int64_t value) {
    out.append(std::format("{}", value));
}

template <class Sink> void append_token(Sink& out, const std::optional<BearerToken>& value) {
    if (value.has_value()) {
        append_string(out, value->value().reveal());
    } else {
        out.append("null");
    }
}

template <class Sink> void write_session(Sink& out, const Session& session) {
    out.append("{\n  \"format_version\": 1,\n  \"issuer\": ");
    append_string(out, session.issuer);
    out.append(",\n  \"client_id\": ");
    append_string(out, session.client_id);
    out.append(",\n  \"client_secret\": ");
    append_string(out, session.client_secret.reveal());
    out.append(",\n  \"username\": ");
    append_string(out, session.username);
    out.append(",\n  \"refresh_token\": ");
    append_token(out, session.refresh_token);
    out.append(",\n  \"refresh_expires_at\": ");
    if (session.refresh_expires_at.has_value()) {
        append_number(out, *session.refresh_expires_at);
    } else {
        out.append("null");
    }
    out.append(",\n  \"access_token\": ");
    append_token(out, session.access_token);
    out.append(",\n  \"access_expires_at\": ");
    append_number(out, session.access_expires_at);
    out.append("\n}\n");
}

} // namespace

Result<Session> parse_session(std::string_view json, std::string_view issuer) {
    if (json.size() > max_session_file_bytes) {
        return std::unexpected(
            reason(std::format("it is larger than {} bytes", max_session_file_bytes)));
    }
    Result<Json> parsed = json_util::parse_untrusted(json, "saved login",
                                                     json_util::ParseLimits{
                                                         .max_bytes = max_session_file_bytes,
                                                         .max_depth = session_max_depth,
                                                         .max_separators = session_max_separators,
                                                         .code = Errc::failure,
                                                     });
    if (!parsed) {
        return std::unexpected(reason("it is not valid JSON"));
    }
    Json& doc = *parsed;
    const DomWipe wipe{doc};

    auto* object = doc.get_ptr<Json::object_t*>();
    if (object == nullptr) {
        return std::unexpected(reason("it is not a JSON object"));
    }

    const Json* version = member(*object, "format_version");
    if (version == nullptr) {
        return std::unexpected(reason("it has no format_version"));
    }
    const auto* version_number = version->get_ptr<const Json::number_unsigned_t*>();
    if (version_number == nullptr || *version_number != session_format_version) {
        return std::unexpected(reason("it has an unknown format_version"));
    }

    Result<std::string*> saved_issuer = required_string(*object, "issuer");
    if (!saved_issuer) {
        return std::unexpected(std::move(saved_issuer).error());
    }
    if (**saved_issuer != issuer) {
        return std::unexpected(reason("it is for another login server"));
    }

    Result<std::string*> client_id = login_detail(*object, "client_id");
    if (!client_id) {
        return std::unexpected(std::move(client_id).error());
    }
    Result<std::string*> client_secret = login_detail(*object, "client_secret");
    if (!client_secret) {
        return std::unexpected(std::move(client_secret).error());
    }
    Result<std::string*> username = login_detail(*object, "username");
    if (!username) {
        return std::unexpected(std::move(username).error());
    }
    Result<std::optional<BearerToken>> refresh_token = token(*object, "refresh_token");
    if (!refresh_token) {
        return std::unexpected(std::move(refresh_token).error());
    }
    Result<std::optional<std::int64_t>> refresh_expires_at =
        unix_time(*object, "refresh_expires_at", json_util::Need::optional);
    if (!refresh_expires_at) {
        return std::unexpected(std::move(refresh_expires_at).error());
    }
    Result<std::optional<BearerToken>> access_token = token(*object, "access_token");
    if (!access_token) {
        return std::unexpected(std::move(access_token).error());
    }
    Result<std::optional<std::int64_t>> access_expires_at =
        unix_time(*object, "access_expires_at", json_util::Need::required);
    if (!access_expires_at) {
        return std::unexpected(std::move(access_expires_at).error());
    }

    // client_id, username and issuer are not secrets: plain copies (the DOM originals are wiped
    // with the rest). client_secret's buffer moves into the Secret.
    return Session{
        .issuer = std::string{issuer},
        .client_id = **client_id,
        .client_secret = Secret::adopt(std::move(**client_secret)),
        .username = **username,
        .refresh_token = std::move(*refresh_token),
        .refresh_expires_at = *refresh_expires_at,
        .access_token = std::move(*access_token),
        .access_expires_at = access_expires_at->value_or(0),
    };
}

Secret session_to_json(const Session& session) {
    Measure measure{};
    write_session(measure, session);
    Secret out{};
    out.reserve(measure.size);
    write_session(out, session);
    return out;
}

} // namespace mangadex_dl
