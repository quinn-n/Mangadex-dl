// SPDX-License-Identifier: GPL-3.0-or-later
//
// MANGA parsing (download plan T4). ASCII-only and locale-independent: no <cctype>, no regex.
// The mangadex.org URL walk is shared with the LIST argument: src/core/mangadex_url.cpp.
#include "mangadex_dl/manga_id.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_url.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl {
namespace {

inline constexpr std::size_t uuid_length = 36;
inline constexpr std::string_view title_path = "/title/";
inline constexpr std::string_view invalid_manga_message =
    "MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL";

[[nodiscard]] constexpr bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

bool is_uuid(std::string_view text) noexcept {
    if (text.size() != uuid_length) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash_position = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash_position ? text[i] != '-' : !is_hex_digit(text[i])) {
            return false;
        }
    }
    return true;
}

bool looks_like_manga_id(std::string_view text) noexcept {
    return detail::extract_mangadex_uuid(text, title_path).has_value();
}

Result<MangaId> MangaId::parse(std::string_view text) {
    const std::optional<std::string_view> uuid = detail::extract_mangadex_uuid(text, title_path);
    if (!uuid) {
        return std::unexpected(
            Error{.code = Errc::usage, .message = std::string{invalid_manga_message}});
    }
    return MangaId{detail::lowercase_ascii(*uuid)};
}

} // namespace mangadex_dl
