// SPDX-License-Identifier: GPL-3.0-or-later
//
// MANGA parsing (download plan T4). ASCII-only and locale-independent: no <cctype>, no regex.
#include "mangadex_dl/manga_id.hpp"

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

inline constexpr std::size_t uuid_length = 36;
inline constexpr std::string_view mangadex_host = "mangadex.org";
inline constexpr std::string_view title_path = "/title/";
inline constexpr std::string_view invalid_manga_message =
    "MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL";

[[nodiscard]] constexpr bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// ASCII case-insensitive prefix test (URL schemes and host names are case-insensitive).
[[nodiscard]] constexpr bool starts_with_icase(std::string_view text,
                                               std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix,
                              [](char a, char b) { return ascii_lower(a) == ascii_lower(b); });
}

/// Space, C0 controls and DEL never appear in a URL we accept.
[[nodiscard]] constexpr bool is_url_byte(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte > 0x20 && byte != 0x7F;
}

/// After `/title/<uuid>`: nothing, `?…`, `#…`, `/`, or `/<slug>` with an optional trailing `/`,
/// each optionally followed by `?…` or `#…`. A second path segment is rejected.
[[nodiscard]] constexpr bool is_valid_url_tail(std::string_view tail) noexcept {
    if (tail.empty() || tail.front() == '?' || tail.front() == '#') {
        return true;
    }
    if (tail.front() != '/') {
        return false;
    }
    tail.remove_prefix(1);
    const std::size_t path_end = tail.find_first_of("?#");
    const std::string_view path = tail.substr(0, path_end);
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos) {
        return true; // empty (trailing '/') or a single slug segment
    }
    // `/<slug>/` is fine; `//`, `/<slug>/x` and `/<slug>//` are extra path segments.
    return slash != 0 && slash + 1 == path.size();
}

/// The UUID inside `text` (still in its original case), or nullopt when `text` is not an
/// accepted MANGA spelling. Never allocates, so looks_like_manga_id can be noexcept.
[[nodiscard]] std::optional<std::string_view>
extract_uuid(std::string_view text MANGADEX_DL_LIFETIMEBOUND) noexcept {
    if (is_uuid(text)) {
        return text;
    }
    if (!std::ranges::all_of(text, is_url_byte)) {
        return std::nullopt;
    }
    std::string_view rest = text;
    if (starts_with_icase(rest, "https://")) {
        rest.remove_prefix(std::string_view{"https://"}.size());
    } else if (starts_with_icase(rest, "http://")) {
        rest.remove_prefix(std::string_view{"http://"}.size());
    }
    if (starts_with_icase(rest, "www.")) {
        rest.remove_prefix(std::string_view{"www."}.size());
    }
    if (!starts_with_icase(rest, mangadex_host)) {
        return std::nullopt;
    }
    rest.remove_prefix(mangadex_host.size());
    if (!rest.starts_with(title_path)) {
        return std::nullopt; // also rejects a port, userinfo-like tails and a longer host
    }
    rest.remove_prefix(title_path.size());
    if (rest.size() < uuid_length) {
        return std::nullopt;
    }
    const std::string_view uuid = rest.substr(0, uuid_length);
    if (!is_uuid(uuid) || !is_valid_url_tail(rest.substr(uuid_length))) {
        return std::nullopt;
    }
    return uuid;
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

bool looks_like_manga_id(std::string_view text) noexcept { return extract_uuid(text).has_value(); }

Result<MangaId> MangaId::parse(std::string_view text) {
    const std::optional<std::string_view> uuid = extract_uuid(text);
    if (!uuid) {
        return std::unexpected(
            Error{.code = Errc::usage, .message = std::string{invalid_manga_message}});
    }
    std::string canonical(*uuid);
    std::ranges::transform(canonical, canonical.begin(), ascii_lower);
    return MangaId{std::move(canonical)};
}

} // namespace mangadex_dl
