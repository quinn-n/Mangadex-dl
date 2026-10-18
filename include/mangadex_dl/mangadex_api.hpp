// SPDX-License-Identifier: GPL-3.0-or-later
//
// MangaDex API: response parsing and validation, request URL building, MD@H reports. Pure: no
// I/O. Every parser treats its input as untrusted. Frozen interface (download plan T1);
// implemented in src/core/mangadex_api.cpp.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mangadex_dl {

inline constexpr std::size_t max_api_json_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t max_image_bytes = std::size_t{32} * 1024 * 1024;
inline constexpr std::size_t max_pages_per_chapter = 5000;
inline constexpr std::size_t feed_page_limit = 500;
inline constexpr std::size_t feed_offset_cap = 10000; ///< offset + limit must stay within it
inline constexpr std::size_t cover_page_limit = 100;
inline constexpr std::size_t max_json_depth = 64; ///< checked before any parse

// ---- parsers: Errc::network on malformed, oversized, too deep or invalid-UTF-8 input ----------

/// GET /manga/{id} with cover_art, author and artist includes.
[[nodiscard]] Result<MangaInfo> parse_manga(std::string_view json);

/// One page of GET /manga/{id}/feed. Keeps chapters of every language (the planner filters);
/// every chapter id satisfies is_uuid().
[[nodiscard]] Result<FeedPage> parse_feed_page(std::string_view json);

/// One page of GET /cover.
[[nodiscard]] Result<CoverPage> parse_cover_page(std::string_view json);

/// GET /at-home/server/{chapterId}. Validates the base URL (validate_at_home_base_url), the hash
/// and every file name (is_valid_at_home_token), and at most max_pages_per_chapter pages.
[[nodiscard]] Result<AtHomeServer> parse_at_home(std::string_view json);

// ---- validators ------------------------------------------------------------------------------

/// `^[A-Za-z0-9][A-Za-z0-9._-]*$`: safe as one URL path segment and one file name.
[[nodiscard]] bool is_valid_at_home_token(std::string_view token) noexcept;

/// https:// with a host, and no userinfo, query or fragment. Errc::network otherwise.
[[nodiscard]] Result<void> validate_at_home_base_url(std::string_view url);

// ---- request URLs: fixed parameter order, so fixture paths are deterministic --------------------
// `api`, `base` and `uploads` have no trailing '/'.

[[nodiscard]] std::string manga_request_url(std::string_view api, const MangaId& id);
[[nodiscard]] std::string feed_request_url(std::string_view api, const MangaId& id,
                                           std::string_view lang, std::size_t offset);
[[nodiscard]] std::string cover_request_url(std::string_view api, const MangaId& id,
                                            std::size_t offset);
[[nodiscard]] std::string at_home_request_url(std::string_view api, std::string_view chapter_id);
/// `<base>/data/<hash>/<file>` or `<base>/data-saver/<hash>/<file>`.
[[nodiscard]] std::string image_url(std::string_view base, ImageQuality quality,
                                    std::string_view hash, std::string_view file);
/// `<uploads>/covers/<id>/<file>`.
[[nodiscard]] std::string cover_image_url(std::string_view uploads, const MangaId& id,
                                          std::string_view file);

// ---- MangaDex@Home reliability reports ----------------------------------------------------------

/// True when the URL's host is `mangadex.org` or ends in `.mangadex.org` (such hosts are never
/// reported).
[[nodiscard]] bool is_mangadex_org_host(std::string_view url) noexcept;

/// The body of one report POST.
struct AtHomeReport {
    std::string url{};
    bool success = false;
    bool cached = false; ///< the X-Cache header began with "HIT"
    std::uint64_t bytes = 0;
    std::uint64_t duration_ms = 0;

    friend bool operator==(const AtHomeReport&, const AtHomeReport&) = default;
};

/// `{"url":…,"success":…,"cached":…,"bytes":…,"duration":…}` (duration in ms).
[[nodiscard]] std::string at_home_report_json(const AtHomeReport& report);

} // namespace mangadex_dl
