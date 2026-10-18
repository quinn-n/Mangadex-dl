// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MANGA argument: a MangaDex title UUID, accepted bare or inside a title URL.
// Frozen interface (download plan T1); implemented in src/core/manga_id.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"

#include <compare>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {

/// A validated title id in canonical form: a lowercase 8-4-4-4-12 hex UUID. Path- and URL-safe.
class MangaId {
public:
    /// The nil UUID. It is a placeholder for default-constructed options; parse() never yields it.
    MangaId() = default;

    /// Accepts a bare UUID (any hex case) or a title URL: `https://mangadex.org/title/<uuid>`
    /// with an optional `/<slug>`, trailing `/`, `?query` or `#fragment`; also `http://`, `www.`
    /// and no scheme. The result is lowercased. Anything else is Errc::usage with a one-line
    /// message.
    [[nodiscard]] static Result<MangaId> parse(std::string_view text);

    /// The canonical UUID. The view borrows from *this: never bind it to a temporary MangaId.
    [[nodiscard]] std::string_view str() const noexcept MANGADEX_DL_LIFETIMEBOUND { return value_; }

    friend bool operator==(const MangaId&, const MangaId&) = default;
    friend std::strong_ordering operator<=>(const MangaId&, const MangaId&) = default;

private:
    /// For parse() only: `canonical` is already validated and lowercased.
    explicit MangaId(std::string canonical) noexcept : value_(std::move(canonical)) {}

    std::string value_{"00000000-0000-0000-0000-000000000000"};
};

/// True when `text` would be accepted by MangaId::parse (used to reject a DIR that is really a
/// second manga id).
[[nodiscard]] bool looks_like_manga_id(std::string_view text) noexcept;

/// True for exactly 36 bytes in 8-4-4-4-12 hex form (either case). Ids from the API are checked
/// with this before they are used in URLs or fixture paths.
[[nodiscard]] bool is_uuid(std::string_view text) noexcept;

} // namespace mangadex_dl
