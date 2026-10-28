// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LIST argument of `mdlist`: a MangaDex custom-list (MDList) UUID, accepted bare or inside a
// list URL. Light on purpose: api_client.hpp includes it. Frozen interface (list plan T1);
// implemented in src/core/list_id.cpp (list plan T4).
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"

#include <compare>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {

/// A validated MangaDex custom-list (MDList) id: a lowercase 8-4-4-4-12 hex UUID. Path- and
/// URL-safe.
class ListId {
public:
    /// The nil UUID: a placeholder for default-constructed settings; parse() never yields it.
    ListId() = default;

    /// A bare UUID (any hex case), or [https://|http://][www.]mangadex.org/list/<uuid> then
    /// nothing, `/`, `/<slug>`, `/<slug>/`, each optionally followed by `?...` or `#...` (host
    /// case-insensitive). The result is lowercased. Errc::usage otherwise; the message never
    /// contains `text`:
    /// - a title URL (looks_like_manga_id && !is_uuid) -> "LIST is a MangaDex title, not a list;
    ///   use 'mangadex-dl download <title>' for one title";
    /// - anything else -> "LIST must be a MangaDex list UUID or a https://mangadex.org/list/<uuid>
    ///   URL; for a CSV file of titles use 'mangadex-dl batch <file>'".
    [[nodiscard]] static Result<ListId> parse(std::string_view text);

    /// The canonical UUID. The view borrows from *this: never bind it to a temporary ListId.
    [[nodiscard]] std::string_view str() const noexcept MANGADEX_DL_LIFETIMEBOUND { return value_; }

    friend bool operator==(const ListId&, const ListId&) = default;
    friend std::strong_ordering operator<=>(const ListId&, const ListId&) = default;

private:
    /// For parse() only: `canonical` is already validated and lowercased.
    explicit ListId(std::string canonical) noexcept : value_(std::move(canonical)) {}

    std::string value_{"00000000-0000-0000-0000-000000000000"};
};

/// True when ListId::parse would accept `text` (a bare UUID counts). Used for the `download <list
/// URL>` hint and to reject a DIR that is really a list id. noexcept, no allocation.
[[nodiscard]] bool looks_like_list_id(std::string_view text) noexcept;

} // namespace mangadex_dl
