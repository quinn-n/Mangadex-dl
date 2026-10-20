// SPDX-License-Identifier: GPL-3.0-or-later
//
// The batch title list: limits, the parsed record form and the pure CSV parser.
// Frozen interface (batch plan T1); implemented in src/core/batch_csv.cpp (T2).
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

inline constexpr std::size_t max_list_bytes = std::size_t{1024} * 1024;
inline constexpr std::size_t max_list_titles = 10000;
inline constexpr std::size_t max_list_field_bytes = 4096;
inline constexpr std::size_t max_list_errors = 20;
inline constexpr std::size_t max_list_echo_bytes = 32; ///< cap for echoed header names

/// One title record after unquoting and trimming. Absent or empty cells are nullopt.
/// Every string is owned (no view into the input buffer).
struct ListRecord {
    std::size_t line = 0; ///< 1-based physical start line
    std::string manga{};  ///< never empty
    std::optional<std::string> title{};
    std::optional<std::string> dir{};
    std::optional<std::string> volumes{};
    std::optional<std::string> loose_chapters{};
    std::optional<std::string> layout{};
    std::optional<std::string> lang{};
    std::optional<std::string> group{}; ///< raw "A|B" text; split by plan_batch
    std::optional<std::string> direction{};

    friend bool operator==(const ListRecord&, const ListRecord&) = default;
};

struct TitleList {
    bool has_header = false;
    std::vector<ListRecord> records{};

    friend bool operator==(const TitleList&, const TitleList&) = default;
};

/// Exit-2 diagnostics: at most max_list_errors one-line messages ("<label>:<line>: ..." or
/// "<label>: ..."), raw bytes (the app sanitizes when printing), plus how many more were found.
struct ListErrors {
    std::vector<std::string> messages{};
    std::size_t more = 0;

    friend bool operator==(const ListErrors&, const ListErrors&) = default;
};

/// What the list stages return: the value, or every usage error found (not just the first).
template <class T> using ListResult = std::expected<T, ListErrors>;

/// Pure: bytes -> records. `label` is FILE as typed or "<stdin>", copied into messages. Nothing in
/// the result borrows from `bytes` or `label`.
[[nodiscard]] ListResult<TitleList> parse_title_list(std::string_view bytes,
                                                     std::string_view label);

} // namespace mangadex_dl
