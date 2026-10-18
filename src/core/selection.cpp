// SPDX-License-Identifier: GPL-3.0-or-later
//
// The selection grammar and numeric labels (download plan T4). ASCII-only and locale-independent:
// numbers stay digit strings, so no overflow and no from_chars range to check.
#include "mangadex_dl/selection.hpp"

#include "mangadex_dl/error.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// Longest item echoed back in an error message; longer ones are cut and marked with "...".
inline constexpr std::size_t max_echo_bytes = 32;

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// A copy of `item` that is safe to put in a one-line diagnostic: every byte outside printable
/// ASCII becomes '?' (no terminal escapes, no invalid UTF-8), and it is at most max_echo_bytes.
[[nodiscard]] std::string echo(std::string_view item) {
    const bool truncated = item.size() > max_echo_bytes;
    std::string out(item.substr(0, max_echo_bytes));
    std::ranges::replace_if(
        out,
        [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte < 0x20 || byte >= 0x7F;
        },
        '?');
    if (truncated) {
        out += "...";
    }
    return out;
}

[[nodiscard]] Error usage_error(std::string message) {
    return Error{.code = Errc::usage, .message = std::move(message)};
}

/// One bound of an ITEM: absent when `text` is empty (an open end), an error when not a number.
[[nodiscard]] Result<std::optional<ChapterNumber>> parse_bound(std::string_view text) {
    if (text.empty()) {
        return std::optional<ChapterNumber>{};
    }
    std::optional<ChapterNumber> number = ChapterNumber::parse(text);
    if (!number) {
        return std::unexpected(usage_error(std::format("'{}' is not a number", echo(text))));
    }
    return number;
}

/// ITEM := N | N-M | N- | -M (`item` is non-empty and has no whitespace or ',').
[[nodiscard]] Result<SelectionRange> parse_item(std::string_view item) {
    const std::size_t dash = item.find('-');
    if (dash == std::string_view::npos) {
        Result<std::optional<ChapterNumber>> single = parse_bound(item);
        if (!single) {
            return std::unexpected(std::move(single).error());
        }
        return SelectionRange{.low = *single, .high = *single};
    }
    if (item.find('-', dash + 1) != std::string_view::npos) {
        return std::unexpected(
            usage_error(std::format("'{}' has more than one '-' (expected N-M)", echo(item))));
    }
    if (item.size() == 1) {
        return std::unexpected(usage_error("'-' needs at least one bound (N-, -M or N-M)"));
    }
    Result<std::optional<ChapterNumber>> low = parse_bound(item.substr(0, dash));
    if (!low) {
        return std::unexpected(std::move(low).error());
    }
    Result<std::optional<ChapterNumber>> high = parse_bound(item.substr(dash + 1));
    if (!high) {
        return std::unexpected(std::move(high).error());
    }
    const std::optional<ChapterNumber>& low_bound = *low;
    const std::optional<ChapterNumber>& high_bound = *high;
    if (low_bound && high_bound && *high_bound < *low_bound) {
        return std::unexpected(usage_error(std::format(
            "'{}' is an empty range: the low bound is greater than the high bound", echo(item))));
    }
    return SelectionRange{.low = std::move(*low), .high = std::move(*high)};
}

[[nodiscard]] bool range_contains(const SelectionRange& range,
                                  const ChapterNumber& number) noexcept {
    return (!range.low || *range.low <= number) && (!range.high || number <= *range.high);
}

} // namespace

std::optional<ChapterNumber> ChapterNumber::parse(std::string_view text) {
    const std::size_t dot = text.find('.');
    const std::string_view integer = text.substr(0, dot);
    const std::string_view fraction =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (integer.empty() || !std::ranges::all_of(integer, is_digit)) {
        return std::nullopt;
    }
    if (dot != std::string_view::npos &&
        (fraction.empty() || !std::ranges::all_of(fraction, is_digit))) {
        return std::nullopt; // "1." and "1.2.3" (a second '.' is not a digit)
    }
    const std::size_t first_significant = integer.find_first_not_of('0');
    const std::string_view canonical_integer = first_significant == std::string_view::npos
                                                   ? std::string_view{"0"}
                                                   : integer.substr(first_significant);
    const std::size_t last_significant = fraction.find_last_not_of('0');
    const std::string_view canonical_fraction = last_significant == std::string_view::npos
                                                    ? std::string_view{}
                                                    : fraction.substr(0, last_significant + 1);
    return ChapterNumber{std::string{canonical_integer}, std::string{canonical_fraction}};
}

std::string ChapterNumber::normalized() const {
    if (fraction_.empty()) {
        return integer_;
    }
    return std::format("{}.{}", integer_, fraction_);
}

std::strong_ordering operator<=>(const ChapterNumber& lhs, const ChapterNumber& rhs) noexcept {
    // Canonical integers have no leading zeros, so a longer one is larger; equal lengths compare
    // digit by digit. Canonical fractions have no trailing zeros, so plain lexicographic order of
    // the digit strings is numeric order ("05" < "1" < "15" < "5").
    if (const auto by_length = lhs.integer_.size() <=> rhs.integer_.size();
        std::is_neq(by_length)) {
        return by_length;
    }
    if (const auto by_integer = lhs.integer_.compare(rhs.integer_) <=> 0; std::is_neq(by_integer)) {
        return by_integer;
    }
    return lhs.fraction_.compare(rhs.fraction_) <=> 0;
}

Result<Selection> Selection::parse(std::string_view spec) {
    if (spec == "all") {
        return Selection{Kind::all, {}};
    }
    if (spec == "none") {
        return Selection{Kind::none, {}};
    }
    if (spec.empty()) {
        return std::unexpected(usage_error("the selection is empty (expected all, none or a list "
                                           "such as 1,3-5,8-)"));
    }
    if (std::ranges::any_of(spec, is_ascii_space)) {
        return std::unexpected(usage_error("the selection must not contain whitespace"));
    }
    std::vector<SelectionRange> ranges{};
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = spec.find(',', begin);
        const std::string_view item = spec.substr(begin, comma - begin);
        if (item.empty()) {
            return std::unexpected(usage_error("the selection has an empty item"));
        }
        Result<SelectionRange> range = parse_item(item);
        if (!range) {
            return std::unexpected(std::move(range).error());
        }
        ranges.push_back(std::move(*range));
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return Selection{Kind::ranges, std::move(ranges)};
}

bool Selection::matches(std::string_view label) const {
    switch (kind_) {
    case Kind::all:
        return true;
    case Kind::none:
        return false;
    case Kind::ranges: {
        const std::optional<ChapterNumber> number = ChapterNumber::parse(label);
        if (!number) {
            return false; // a non-numeric label matches only `all`
        }
        const ChapterNumber& value = *number;
        return std::ranges::any_of(ranges_, [&value](const SelectionRange& range) {
            return range_contains(range, value);
        });
    }
    }
    return false; // only reachable through an out-of-range enum value
}

std::string normalize_label(std::string_view label) {
    const std::optional<ChapterNumber> number = ChapterNumber::parse(label);
    return number ? number->normalized() : std::string{label};
}

} // namespace mangadex_dl
