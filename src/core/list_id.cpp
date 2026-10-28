// SPDX-License-Identifier: GPL-3.0-or-later
//
// LIST parsing (list plan T4). ASCII-only and locale-independent: no <cctype>, no regex. The
// mangadex.org URL walk is shared with the MANGA argument: src/core/mangadex_url.cpp.
#include "mangadex_dl/list_id.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_url.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl {
namespace {

// extract_mangadex_uuid trusts its `path` argument to be a `/…/` literal (T2 security review):
// this constant is the only path ever passed from here.
inline constexpr std::string_view list_path = "/list/";

// Neither message contains the input: LIST is never echoed, like MANGA.
inline constexpr std::string_view title_not_list_message =
    "LIST is a MangaDex title, not a list; use 'mangadex-dl download <title>' for one title";
inline constexpr std::string_view invalid_list_message =
    "LIST must be a MangaDex list UUID or a https://mangadex.org/list/<uuid> URL; for a CSV file "
    "of titles use 'mangadex-dl batch <file>'";

} // namespace

bool looks_like_list_id(std::string_view text) noexcept {
    return detail::extract_mangadex_uuid(text, list_path).has_value();
}

Result<ListId> ListId::parse(std::string_view text) {
    const std::optional<std::string_view> uuid = detail::extract_mangadex_uuid(text, list_path);
    if (!uuid) {
        // A bare UUID is a list spelling and never gets here; the is_uuid test keeps the rule
        // exactly as documented in list_id.hpp.
        const std::string_view message = looks_like_manga_id(text) && !is_uuid(text)
                                             ? title_not_list_message
                                             : invalid_list_message;
        return std::unexpected(Error{.code = Errc::usage, .message = std::string{message}});
    }
    return ListId{detail::lowercase_ascii(*uuid)};
}

} // namespace mangadex_dl
