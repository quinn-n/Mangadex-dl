// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private: decoding application/x-www-form-urlencoded bodies (follows plan T6). The fixture
// transport's stand-in for MangaDex's Keycloak decodes the token and revoke forms with it; the
// encoder is public (form_encode in mangadex_dl/auth.hpp). Not part of the public interface.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/secret.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl::detail {

/// The most fields form_decode accepts in one body.
inline constexpr std::size_t max_form_fields = 16;

/// One decoded `name=value` pair. The name is a field name (never secret); the value may be a
/// password, a client secret or a token, so it lives in a Secret.
struct FormField {
    std::string name{};
    Secret value{};
};

/// Decodes an application/x-www-form-urlencoded body: fields separated by '&', each `name=value`
/// (the first '=' splits), '+' is a space and `%XX` (either hex case) is the byte XX, in names and
/// values alike. An empty body is no fields. Errc::failure with a fixed message that never
/// contains body bytes for: an empty field (`a=1&&b=2`, a trailing '&'), a field without '=', an
/// empty name, a '%' not followed by two hex digits, a duplicate name (compared after decoding)
/// and more than max_form_fields fields. The result owns its bytes (no view into `body`).
[[nodiscard]] Result<std::vector<FormField>> form_decode(std::string_view body);

/// The field called `name`, or nullptr. Borrows `fields`: the pointer dies with the vector.
[[nodiscard]] const FormField*
find_form_field(const std::vector<FormField>& fields MANGADEX_DL_LIFETIMEBOUND,
                std::string_view name) noexcept;

} // namespace mangadex_dl::detail
