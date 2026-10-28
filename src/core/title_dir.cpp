// SPDX-License-Identifier: GPL-3.0-or-later
//
// `mdlist --dir-per-title`: the per-run "first claim wins" registry of title directories (list
// plan T5; list-spec.md "Per-title subdirectories"). Every key and value is an owned string built
// from the borrowed `stem` and the MangaId's canonical text; nothing here keeps a view.
//
// Threads: not thread-safe by contract; used only on the run_download thread.
#include "mangadex_dl/title_dir.hpp"

#include "mangadex_dl/manga_id.hpp"

#include <cstddef>
#include <format>
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

TitleDirClaim TitleDirRegistry::claim(std::string_view stem, const MangaId& manga,
                                      std::size_t item) {
    // The same manga again keeps the name it already has.
    if (const auto known = by_manga_.find(manga); known != by_manga_.end()) {
        return TitleDirClaim{.name = known->second};
    }

    const auto holder = by_name_.find(stem); // heterogeneous lookup: the view is not stored
    if (holder == by_name_.end()) {
        std::string name{stem};
        by_name_.try_emplace(name, Holder{.manga = manga, .item = item});
        by_manga_.try_emplace(manga, name);
        return TitleDirClaim{.name = std::move(name)};
    }

    // The plain stem belongs to another manga (the same manga returned above): move aside.
    const std::size_t taken_by = holder->second.item;
    const std::string_view id = manga.str(); // borrows `manga`, which outlives this call
    std::string name = suffixed(stem, id.substr(0, short_suffix_length));
    if (by_name_.contains(name)) {
        // Two ids that share the 8-hex prefix and the title (or a title that is literally the
        // suffixed name): the full UUID. Returned even if that is held too; OutputRegistry is the
        // backstop, so nothing is overwritten.
        name = suffixed(stem, id);
    }
    by_name_.try_emplace(name, Holder{.manga = manga, .item = item});
    by_manga_.try_emplace(manga, name);
    return TitleDirClaim{.name = std::move(name), .taken_by = taken_by};
}

} // namespace mangadex_dl
