// SPDX-License-Identifier: GPL-3.0-or-later
//
// The form codec (follows plan T6): form_encode (public, auth.hpp) builds the token and revoke
// request bodies, form_decode (core-private, auth/form.hpp) lets the fixture transport read them.
// Both write secret bytes straight into a Secret reserved up front, so no reallocation leaves a
// copy behind, and neither ever puts input bytes into an error message.
#include "auth/form.hpp"

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/secret.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

inline constexpr std::string_view upper_hex = "0123456789ABCDEF";

/// RFC 3986 section 2.3: ALPHA / DIGIT / "-" / "." / "_" / "~".
[[nodiscard]] constexpr bool is_unreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

/// The value of one hex digit, or nullopt.
[[nodiscard]] constexpr std::optional<unsigned> hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A') + 10U;
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a') + 10U;
    }
    return std::nullopt;
}

[[nodiscard]] Error form_error(std::string_view why) {
    return Error{.code = Errc::failure, .message = std::string{"form body: "}.append(why)};
}

/// Decodes `raw` ('+' and %XX) into `out`, which the caller reserved to at least raw.size() (the
/// decoded text is never longer), so Secret::push_back never reallocates. False on a bad '%'.
template <class Out> [[nodiscard]] bool percent_decode(std::string_view raw, Out& out) {
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (raw.size() - i < 3) { // i < raw.size(), so no underflow
            return false;
        }
        const auto high = hex_value(raw[i + 1]);
        const auto low = hex_value(raw[i + 2]);
        if (!high.has_value() || !low.has_value()) {
            return false;
        }
        out.push_back(static_cast<char>(static_cast<unsigned char>((*high << 4U) | *low)));
        i += 2;
    }
    return true;
}

} // namespace

Secret form_encode(std::string_view text) {
    std::size_t encoded = 0;
    for (const char c : text) {
        encoded += is_unreserved(c) ? std::size_t{1} : std::size_t{3};
    }
    Secret out{};
    out.reserve(encoded);
    for (const char c : text) {
        if (is_unreserved(c)) {
            out.push_back(c);
            continue;
        }
        const std::size_t byte = static_cast<unsigned char>(c);
        out.push_back('%');
        out.push_back(upper_hex[byte >> 4U]);
        out.push_back(upper_hex[byte & 0x0FU]);
    }
    return out;
}

namespace detail {

Result<std::vector<FormField>> form_decode(std::string_view body) {
    std::vector<FormField> fields{};
    if (body.empty()) {
        return fields;
    }
    while (true) {
        const auto amp = body.find('&');
        const std::string_view piece = body.substr(0, amp);
        if (piece.empty()) {
            return std::unexpected(form_error("empty field"));
        }
        const auto eq = piece.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected(form_error("a field without '='"));
        }
        if (eq == 0) {
            return std::unexpected(form_error("a field without a name"));
        }
        if (fields.size() + 1 > max_form_fields) {
            return std::unexpected(form_error("more than 16 fields"));
        }
        const std::string_view raw_name = piece.substr(0, eq);
        const std::string_view raw_value = piece.substr(eq + 1);

        FormField field{};
        field.name.reserve(raw_name.size());
        field.value.reserve(raw_value.size());
        if (!percent_decode(raw_name, field.name) || !percent_decode(raw_value, field.value)) {
            return std::unexpected(form_error("a '%' not followed by two hex digits"));
        }
        if (find_form_field(fields, field.name) != nullptr) {
            return std::unexpected(form_error("a duplicate field"));
        }
        fields.push_back(std::move(field));

        if (amp == std::string_view::npos) {
            return fields;
        }
        body.remove_prefix(amp + 1);
    }
}

const FormField* find_form_field(const std::vector<FormField>& fields,
                                 std::string_view name) noexcept {
    for (const FormField& field : fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace detail
} // namespace mangadex_dl
