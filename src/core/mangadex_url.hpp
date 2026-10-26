// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): the mangadex.org URL walk shared by the MANGA argument
// (src/core/manga_id.cpp, path "/title/") and the LIST argument (path "/list/"). Implemented in
// src/core/mangadex_url.cpp. ASCII-only and locale-independent: no <cctype>, no regex.
#pragma once

#include "mangadex_dl/attributes.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl::detail {

/// The UUID inside `text`, still in its original case, or nullopt when `text` is not an accepted
/// spelling. Accepted: a bare UUID (is_uuid), or `[http[s]://][www.]mangadex.org<path><uuid>`
/// (scheme and host case-insensitive) followed by nothing, `?…`, `#…`, `/`, or `/<slug>` with an
/// optional trailing `/`, each optionally followed by `?…` or `#…`. Any space, C0 control or DEL
/// rejects the whole text. `path` is matched case-sensitively and is `"/title/"` or `"/list/"`.
///
/// The result borrows from `text` (it is `text` itself or a substring of it): never let it outlive
/// the buffer `text` views. Never allocates.
[[nodiscard]] std::optional<std::string_view>
extract_mangadex_uuid(std::string_view text MANGADEX_DL_LIFETIMEBOUND,
                      std::string_view path) noexcept;

/// `text` with ASCII `A`-`Z` mapped to `a`-`z`; every other byte (including non-ASCII) unchanged.
[[nodiscard]] std::string lowercase_ascii(std::string_view text);

} // namespace mangadex_dl::detail
