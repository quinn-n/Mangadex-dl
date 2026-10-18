// SPDX-License-Identifier: GPL-3.0-or-later
//
// `download` option validation (download plan T4). ASCII-only and locale-independent: no
// <cctype>, no regex; numbers go through std::from_chars.
#include "mangadex_dl/options.hpp"

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/selection.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mangadex_dl {
namespace {

inline constexpr int min_jobs = 1;
inline constexpr int max_jobs = 8;
inline constexpr std::size_t max_title_bytes = 1024;
inline constexpr std::string_view ascii_whitespace = " \t\n\r\f\v";

[[nodiscard]] Error usage_error(std::string message) {
    return Error{.code = Errc::usage, .message = std::move(message)};
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool is_lower_alpha(char c) noexcept { return c >= 'a' && c <= 'z'; }

[[nodiscard]] constexpr bool is_alnum(char c) noexcept {
    return is_digit(c) || is_lower_alpha(c) || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string ascii_lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), ascii_lower);
    return out;
}

[[nodiscard]] constexpr bool equals_icase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b,
                              [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

[[nodiscard]] constexpr bool starts_with_icase(std::string_view text,
                                               std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && equals_icase(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] constexpr bool is_control(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7F;
}

/// RFC 3986 `pchar` plus '/': unreserved, pct-encoded ('%'), sub-delims, ':' and '@'.
[[nodiscard]] constexpr bool is_path_char(char c) noexcept {
    return is_alnum(c) || std::string_view{"-._~%!$&'()*+,;=:@/"}.contains(c);
}

/// A registered name (letters, digits, '-', '.'), or a bracketed IPv6 literal.
[[nodiscard]] constexpr bool is_valid_host(std::string_view host) noexcept {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        const std::string_view inner = host.substr(1, host.size() - 2);
        return !inner.empty() && std::ranges::all_of(inner, [](char c) {
            return is_hex_digit(c) || c == ':' || c == '.';
        });
    }
    return !host.empty() && host.front() != '-' && host.front() != '.' &&
           std::ranges::all_of(host, [](char c) { return is_alnum(c) || c == '-' || c == '.'; });
}

/// `:<1-65535>` or empty.
[[nodiscard]] bool is_valid_port_suffix(std::string_view suffix) noexcept {
    if (suffix.empty()) {
        return true;
    }
    if (suffix.front() != ':') {
        return false;
    }
    const std::string_view digits = suffix.substr(1);
    if (digits.empty() || digits.size() > 5 || !std::ranges::all_of(digits, is_digit)) {
        return false;
    }
    std::uint32_t port = 0;
    // from_chars takes an explicit [first, last) range, so no NUL terminator is needed.
    const char* const first = std::to_address(digits.begin());
    const char* const last = std::to_address(digits.end());
    const auto [end, ec] = std::from_chars(first, last, port);
    return ec == std::errc{} && end == last && port >= 1 && port <= 65535;
}

[[nodiscard]] bool is_loopback_host(std::string_view lowered_host) noexcept {
    return lowered_host == "127.0.0.1" || lowered_host == "localhost" || lowered_host == "[::1]";
}

/// The directory of a `file://` base: an absolute path with no `.`, `..` or empty segments (one
/// trailing '/' is dropped). The result is a sub-view of `path`. The caller has already rejected
/// controls, spaces, '?' and '#'.
[[nodiscard]] std::optional<std::string_view>
fixture_dir(std::string_view path MANGADEX_DL_LIFETIMEBOUND) noexcept {
    if (path.size() > 1 && path.ends_with('/')) {
        path.remove_suffix(1);
    }
    if (!path.starts_with('/')) {
        return std::nullopt;
    }
    std::string_view segments = path.substr(1);
    while (true) {
        const std::size_t slash = segments.find('/');
        const std::string_view segment = segments.substr(0, slash);
        if (segment.empty() || segment == "." || segment == "..") {
            return std::nullopt;
        }
        if (slash == std::string_view::npos) {
            return path;
        }
        segments.remove_prefix(slash + 1);
    }
}

/// The UTF-8 well-formedness rules of Unicode 15 table 3-7: no overlong forms, no surrogates,
/// nothing above U+10FFFF, no truncated sequence.
[[nodiscard]] constexpr bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
            minimum = 0x80U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
            minimum = 0x800U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false; // a continuation byte or 0xF8..0xFF in lead position
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        i += length;
    }
    return true;
}

[[nodiscard]] Result<Layout> parse_layout(std::string_view word) {
    if (word == "volumes") {
        return Layout::volumes;
    }
    if (word == "chapters") {
        return Layout::chapters;
    }
    if (word == "single") {
        return Layout::single;
    }
    return std::unexpected(usage_error("--layout must be volumes, chapters or single"));
}

[[nodiscard]] Result<DirectionChoice> parse_direction(std::string_view word) {
    if (word == "auto") {
        return DirectionChoice::automatic;
    }
    if (word == "ltr") {
        return DirectionChoice::ltr;
    }
    if (word == "rtl") {
        return DirectionChoice::rtl;
    }
    return std::unexpected(usage_error("--direction must be auto, ltr or rtl"));
}

/// Selection::parse with the flag name in front of its message.
[[nodiscard]] Result<Selection> parse_selection(std::string_view flag, std::string_view spec) {
    Result<Selection> selection = Selection::parse(spec);
    if (!selection) {
        return std::unexpected(usage_error(std::format("{}: {}", flag, selection.error().message)));
    }
    return selection;
}

} // namespace

Result<ApiBase> ApiBase::parse(std::string_view text) {
    if (std::ranges::any_of(text, [](char c) { return is_control(c) || c == ' '; })) {
        return std::unexpected(
            usage_error("--api-base must not contain spaces or control characters"));
    }
    if (text.find_first_of("?#") != std::string_view::npos) {
        return std::unexpected(usage_error("--api-base must not have a query or fragment"));
    }
    if (starts_with_icase(text, "file://")) {
        const std::optional<std::string_view> dir =
            fixture_dir(text.substr(std::string_view{"file://"}.size()));
        if (!dir) {
            return std::unexpected(usage_error("--api-base file:// needs an absolute directory "
                                               "path without '.', '..' or empty segments"));
        }
        return ApiBase{Kind::fixture, std::format("file://{}", *dir), std::filesystem::path{*dir}};
    }
    const bool https = starts_with_icase(text, "https://");
    if (!https && !starts_with_icase(text, "http://")) {
        return std::unexpected(
            usage_error("--api-base must start with https://, http:// (loopback only) or file://"));
    }
    const std::string_view scheme = https ? "https://" : "http://";
    const std::string_view rest = text.substr(scheme.size());
    const std::string_view authority = rest.substr(0, rest.find('/'));
    std::string_view path = rest.substr(authority.size());
    if (authority.contains('@')) {
        return std::unexpected(usage_error("--api-base must not contain user information"));
    }
    // A bracketed IPv6 literal contains ':'; the port separator is the first ':' after it.
    const std::size_t host_end =
        authority.starts_with('[') ? authority.find(']') : authority.find(':');
    const std::size_t host_size =
        host_end == std::string_view::npos
            ? authority.size()
            : host_end + (authority.starts_with('[') ? std::size_t{1} : std::size_t{0});
    const std::string_view host = authority.substr(0, host_size);
    const std::string_view port_suffix = authority.substr(host_size);
    if (!is_valid_host(host)) {
        return std::unexpected(usage_error("--api-base has a missing or invalid host"));
    }
    if (!is_valid_port_suffix(port_suffix)) {
        return std::unexpected(usage_error("--api-base has an invalid port (expected 1-65535)"));
    }
    if (!std::ranges::all_of(path, is_path_char)) {
        return std::unexpected(usage_error("--api-base has an invalid character in its path"));
    }
    while (path.ends_with('/')) {
        path.remove_suffix(1);
    }
    std::string lowered_host = ascii_lowered(host);
    if (!https && !is_loopback_host(lowered_host)) {
        return std::unexpected(
            usage_error("--api-base http:// is allowed only for 127.0.0.1, localhost or [::1]"));
    }
    std::string url = std::format("{}{}{}{}", scheme, lowered_host, port_suffix, path);
    Kind kind = Kind::http_loopback;
    if (https) {
        kind = url == default_api_base ? Kind::official : Kind::https;
    }
    return ApiBase{kind, std::move(url), std::filesystem::path{}};
}

Result<std::string> validate_language(std::string_view code) {
    const std::size_t dash = code.find('-');
    const std::string_view primary = code.substr(0, dash);
    const std::string_view region =
        dash == std::string_view::npos ? std::string_view{} : code.substr(dash + 1);
    const bool primary_ok =
        primary.size() >= 2 && primary.size() <= 3 && std::ranges::all_of(primary, is_lower_alpha);
    const bool region_ok =
        dash == std::string_view::npos ||
        (region.size() >= 2 && region.size() <= 4 && std::ranges::all_of(region, is_lower_alpha));
    if (!primary_ok || !region_ok) {
        return std::unexpected(
            usage_error("--lang must be a lowercase language code such as en, pt-br or es-la"));
    }
    return std::string{code};
}

Result<std::string> validate_title_override(std::string_view title) {
    const std::size_t first = title.find_first_not_of(ascii_whitespace);
    if (first == std::string_view::npos) {
        return std::unexpected(usage_error("--title must not be empty"));
    }
    const std::size_t last = title.find_last_not_of(ascii_whitespace);
    const std::string_view trimmed = title.substr(first, last - first + 1);
    if (trimmed.size() > max_title_bytes) {
        return std::unexpected(
            usage_error(std::format("--title is longer than {} bytes", max_title_bytes)));
    }
    if (!is_valid_utf8(trimmed)) {
        return std::unexpected(usage_error("--title must be valid UTF-8"));
    }
    if (std::ranges::any_of(trimmed, is_control)) {
        return std::unexpected(usage_error("--title must not contain control characters"));
    }
    return std::string{trimmed};
}

Result<DownloadOptions> validate_download_args(const RawDownloadArgs& raw) {
    Result<MangaId> manga = MangaId::parse(raw.manga);
    if (!manga) {
        return std::unexpected(std::move(manga).error());
    }
    if (raw.dir.empty()) {
        return std::unexpected(usage_error("DIR must not be empty"));
    }
    if (looks_like_manga_id(raw.dir)) {
        return std::unexpected(usage_error("DIR looks like a manga id: only one manga per run"));
    }
    Result<Selection> volumes = parse_selection("--volumes", raw.volumes);
    if (!volumes) {
        return std::unexpected(std::move(volumes).error());
    }
    Result<Selection> loose_chapters = parse_selection("--loose-chapters", raw.loose_chapters);
    if (!loose_chapters) {
        return std::unexpected(std::move(loose_chapters).error());
    }
    if (volumes->is_none() && loose_chapters->is_none()) {
        return std::unexpected(
            usage_error("--volumes and --loose-chapters are both none: nothing to download"));
    }
    const Result<Layout> layout = parse_layout(raw.layout);
    if (!layout) {
        return std::unexpected(layout.error());
    }
    Result<std::string> lang = validate_language(raw.lang);
    if (!lang) {
        return std::unexpected(std::move(lang).error());
    }
    const Result<DirectionChoice> direction = parse_direction(raw.direction);
    if (!direction) {
        return std::unexpected(direction.error());
    }
    std::optional<std::string> title{};
    if (raw.title) {
        Result<std::string> validated = validate_title_override(*raw.title);
        if (!validated) {
            return std::unexpected(std::move(validated).error());
        }
        title = std::move(*validated);
    }
    // Defensive: CLI11 range-checks --jobs too, but core code must not trust its caller.
    if (raw.jobs < min_jobs || raw.jobs > max_jobs) {
        return std::unexpected(
            usage_error(std::format("--jobs must be between {} and {}", min_jobs, max_jobs)));
    }
    Result<ApiBase> api_base = ApiBase::parse(raw.api_base);
    if (!api_base) {
        return std::unexpected(std::move(api_base).error());
    }
    const bool report = !raw.no_report && api_base->kind() == ApiBase::Kind::official;
    return DownloadOptions{
        .manga = std::move(*manga),
        .dir = std::filesystem::path{raw.dir},
        .volumes = std::move(*volumes),
        .loose_chapters = std::move(*loose_chapters),
        .layout = *layout,
        .lang = std::move(*lang),
        .groups = raw.groups,
        .quality = raw.data_saver ? ImageQuality::data_saver : ImageQuality::data,
        .direction = *direction,
        .title = std::move(title),
        .overwrite = raw.overwrite,
        .dry_run = raw.dry_run,
        .jobs = static_cast<unsigned>(raw.jobs),
        .api_base = std::move(*api_base),
        .report = report,
    };
}

} // namespace mangadex_dl
