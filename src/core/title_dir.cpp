// SPDX-License-Identifier: GPL-3.0-or-later
//
// `--dir-per-title` (batch and mdlist): the per-run "first claim wins" registry of title
// directories (list plan T5; list-spec.md "Per-title subdirectories"), plus batch's up-front
// reservations of the first segment of every explicit `dir` cell (batch-dir-per-title plan T1).
// Every key and value is an owned string built from the borrowed `stem` or `row_dir` and the
// MangaId's canonical text, and MangaIds are stored by value; nothing here keeps a view.
//
// Threads: not thread-safe by contract; used only on the run_download thread.
#include "mangadex_dl/title_dir.hpp"

#include "mangadex_dl/manga_id.hpp"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

/// The length of the short suffix: the first group of the canonical UUID.
constexpr std::size_t short_suffix_length = 8;

/// "<stem> [<tag>]" as a new string.
[[nodiscard]] std::string suffixed(std::string_view stem, std::string_view tag) {
    return std::format("{} [{}]", stem, tag);
}

} // namespace

void TitleDirRegistry::reserve(std::string_view row_dir, const MangaId& manga, std::size_t line) {
    // A sub-view of the borrowed `row_dir`, used only within this call (npos keeps it whole).
    const std::string_view segment = row_dir.substr(0, row_dir.find('/'));
    if (segment.empty()) {
        return;
    }
    auto found = reserved_.find(segment); // heterogeneous lookup: the view is not stored
    if (found == reserved_.end()) {
        found = reserved_.try_emplace(std::string{segment}, Reservation{.line = line}).first;
    }
    found->second.mangas.insert(manga); // a copy: nothing borrows the caller's MangaId
}

bool TitleDirRegistry::reserved_for_others(std::string_view name, const MangaId& manga) const {
    const auto found = reserved_.find(name);
    return found != reserved_.end() && !found->second.mangas.contains(manga);
}

TitleDirClaim TitleDirRegistry::claim(std::string_view stem, const MangaId& manga,
                                      std::size_t item) {
    // 1. The same manga again keeps the name it already has.
    if (const auto known = by_manga_.find(manga); known != by_manga_.end()) {
        return TitleDirClaim{.name = known->second};
    }

    // 2. Reserved by another manga's `dir` cell, or 3. held by an earlier claim (necessarily
    // another manga: the same manga returned above). Both values are read before any insertion.
    std::optional<std::size_t> taken_by{};
    bool reserved = false;
    // Heterogeneous lookups: the view `stem` is never stored.
    if (const auto reservation = reserved_.find(stem);
        reservation != reserved_.end() && !reservation->second.mangas.contains(manga)) {
        taken_by = reservation->second.line;
        reserved = true;
    } else if (const auto holder = by_name_.find(stem); holder != by_name_.end()) {
        taken_by = holder->second.item;
    }

    // 4. The stem is free (or reserved only by this manga).
    if (!taken_by) {
        std::string name{stem};
        by_name_.try_emplace(name, Holder{.manga = manga, .item = item});
        by_manga_.try_emplace(manga, name);
        return TitleDirClaim{.name = std::move(name)};
    }

    // Moved aside.
    const std::string_view id = manga.str(); // borrows `manga`, which outlives this call
    std::string name = suffixed(stem, id.substr(0, short_suffix_length));
    if (by_name_.contains(name) || reserved_for_others(name, manga)) {
        // Two ids that share the 8-hex prefix and the title, a title that is literally the
        // suffixed name, or a `dir` cell that names it: the full UUID. Returned even if that is
        // held too; OutputRegistry is the backstop, so nothing is overwritten.
        name = suffixed(stem, id);
    }
    by_name_.try_emplace(name, Holder{.manga = manga, .item = item});
    by_manga_.try_emplace(manga, name);
    return TitleDirClaim{.name = std::move(name), .taken_by = taken_by, .reserved = reserved};
}

} // namespace mangadex_dl
