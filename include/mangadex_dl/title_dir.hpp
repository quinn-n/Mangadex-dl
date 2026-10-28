// SPDX-License-Identifier: GPL-3.0-or-later
//
// `--dir-per-title` (batch and mdlist): the directory below DIR that one title's files go to. The
// resolver is the seam run_download calls; the registry is the per-run "first claim wins"
// bookkeeping behind the app's resolver, plus batch's up-front reservations of the names that
// explicit `dir` cells use. Frozen interface (list plan T1, batch-dir-per-title plan T1);
// TitleDirRegistry is implemented in src/core/title_dir.cpp (list plan T5).
#pragma once

#include "mangadex_dl/manga_id.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace mangadex_dl {

/// What a TitleDirResolver picked for one title.
struct TitleDirChoice {
    std::string name{}; ///< one path component below DIR (passes validate_list_dir)
    std::optional<std::string> warning{}; ///< one line for observer.on_warning, without prefix

    friend bool operator==(const TitleDirChoice&, const TitleDirChoice&) = default;
};

/// Picks the directory below DIR for one title (--dir-per-title). run_download calls resolve() at
/// most once per run, on its calling thread, after the plan and before anything is written.
/// Polymorphic base per house rules: neither copyable nor movable, so it can't be sliced.
class TitleDirResolver {
public:
    TitleDirResolver(const TitleDirResolver&) = delete;
    TitleDirResolver& operator=(const TitleDirResolver&) = delete;
    TitleDirResolver(TitleDirResolver&&) = delete;
    TitleDirResolver& operator=(TitleDirResolver&&) = delete;
    virtual ~TitleDirResolver() = default;

    /// `stem` is Plan::file_stem, borrowed for the call only; the result owns its strings.
    [[nodiscard]] virtual TitleDirChoice resolve(std::string_view stem, const MangaId& manga) = 0;

protected:
    TitleDirResolver() = default;
};

/// The outcome of one TitleDirRegistry::claim.
struct TitleDirClaim {
    std::string name{};
    std::optional<std::size_t> taken_by{}; ///< item that holds the plain stem, when moved aside
    /// true when taken_by is the first line whose 'dir' cell reserved the name (batch)
    bool reserved = false;

    friend bool operator==(const TitleDirClaim&, const TitleDirClaim&) = default;
};

/// One run's title directories: first claim wins, in list order, after the names reserved up
/// front. Owns its keys (nothing borrows `stem` or `row_dir`). Not thread-safe: used only on the
/// run_download thread.
class TitleDirRegistry {
public:
    /// Reserves the first '/'-segment of `row_dir`, a validated `dir` cell (validate_list_dir),
    /// for `manga`: "Clean Title/extras" reserves "Clean Title". An empty `row_dir` (or segment)
    /// is a no-op. Call it for every row with a cell, in list order, before the first claim():
    /// the first line that reserves a segment is the one claim() reports, and every reserving
    /// manga is kept. Copies everything; never touches the per-manga lookup of claim().
    void reserve(std::string_view row_dir, const MangaId& manga, std::size_t line);

    /// The rules, in this order:
    /// 1. The same manga again -> its earlier name, no taken_by, whatever the stem.
    /// 2. A stem reserved only by other mangas -> moved aside, taken_by = the first reserving
    ///    line, reserved = true.
    /// 3. A stem held by an earlier claim (another manga) -> moved aside, taken_by = the holder's
    ///    item.
    /// 4. Otherwise the stem (also when only `manga` reserved it).
    /// Moved aside = "<stem> [<first 8 hex of manga>]"; if that is held by a claim or reserved by
    /// another manga -> "<stem> [<full uuid>]" (returned even if held: unreachable in practice;
    /// OutputRegistry is the backstop). With no reserve() call, rule 2 and the reservation half
    /// of the full-uuid rule never apply.
    [[nodiscard]] TitleDirClaim claim(std::string_view stem, const MangaId& manga,
                                      std::size_t item);

private:
    struct Holder {
        MangaId manga{};
        std::size_t item = 0;
    };

    struct Reservation {
        std::size_t line = 0;       ///< the first line that reserved the name
        std::set<MangaId> mangas{}; ///< every manga whose row reserved it
    };

    /// True when `name` is reserved and `manga` is not among its reservers.
    [[nodiscard]] bool reserved_for_others(std::string_view name, const MangaId& manga) const;

    std::map<std::string, Holder, std::less<>> by_name_{};
    std::map<MangaId, std::string> by_manga_{};
    std::map<std::string, Reservation, std::less<>> reserved_{};
};

} // namespace mangadex_dl
