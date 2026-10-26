// SPDX-License-Identifier: GPL-3.0-or-later
//
// `mdlist --dir-per-title`: the directory below DIR that one title's files go to. The resolver is
// the seam run_download calls; the registry is the per-run "first claim wins" bookkeeping behind
// the app's resolver. Frozen interface (list plan T1); TitleDirRegistry is implemented in
// src/core/title_dir.cpp (list plan T5).
#pragma once

#include "mangadex_dl/manga_id.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
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

    friend bool operator==(const TitleDirClaim&, const TitleDirClaim&) = default;
};

/// One run's title directories: first claim wins, in list order. Owns its keys (nothing borrows
/// `stem`). Not thread-safe: used only on the run_download thread.
class TitleDirRegistry {
public:
    /// - The same manga again -> its earlier name, no taken_by.
    /// - A stem that is free -> the stem.
    /// - A stem held by another manga -> "<stem> [<first 8 hex of manga>]", taken_by = the
    ///   holder's item; if that is held too -> "<stem> [<full uuid>]" (returned even if held:
    ///   unreachable in practice; OutputRegistry is the backstop).
    [[nodiscard]] TitleDirClaim claim(std::string_view stem, const MangaId& manga,
                                      std::size_t item);

private:
    struct Holder {
        MangaId manga{};
        std::size_t item = 0;
    };

    std::map<std::string, Holder, std::less<>> by_name_{};
    std::map<MangaId, std::string> by_manga_{};
};

} // namespace mangadex_dl
