// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mangadex.org URL walk (list plan T2), moved verbatim from manga_id.cpp with the title path
// turned into a parameter. ASCII-only and locale-independent: no <cctype>, no regex.
#include "mangadex_url.hpp"

#include "mangadex_dl/manga_id.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl::detail {
namespace {

inline constexpr std::size_t uuid_length = 36; ///< the length is_uuid accepts
inline constexpr std::string_view mangadex_host = "mangadex.org";

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

/// After `<path><uuid>`: nothing, `?…`, `#…`, `/`, or `/<slug>` with an optional trailing `/`,
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

} // namespace

std::optional<std::string_view> extract_mangadex_uuid(std::string_view text,
                                                      std::string_view path) noexcept {
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
    if (!rest.starts_with(path)) {
        return std::nullopt; // also rejects a port, userinfo-like tails and a longer host
    }
    rest.remove_prefix(path.size());
    if (rest.size() < uuid_length) {
        return std::nullopt;
    }
    const std::string_view uuid = rest.substr(0, uuid_length);
    if (!is_uuid(uuid) || !is_valid_url_tail(rest.substr(uuid_length))) {
        return std::nullopt;
    }
    return uuid;
}

std::string lowercase_ascii(std::string_view text) {
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), ascii_lower);
    return lowered;
}

} // namespace mangadex_dl::detail
