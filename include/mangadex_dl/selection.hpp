// SPDX-License-Identifier: GPL-3.0-or-later
//
// The --volumes / --loose-chapters grammar and numeric chapter/volume labels.
// Frozen interface (download plan T1); implemented in src/core/selection.cpp.
//
//   SPEC := all | none | ITEM(,ITEM)*      ITEM := N | N-M | N- | -M
//   N    := digits with an optional .digits fraction; ranges are inclusive; no whitespace.
#pragma once

#include "mangadex_dl/error.hpp"

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {

/// A non-negative decimal number kept as digit strings, so it can never overflow. "007" equals
/// "7" and "10.50" equals "10.5".
class ChapterNumber {
public:
    /// Digits with an optional `.digits` fraction, nothing else (no sign, no whitespace, no
    /// exponent). nullopt for anything else, including "" and "1.".
    [[nodiscard]] static std::optional<ChapterNumber> parse(std::string_view text);

    /// The canonical spelling: no leading zeros (but "0"), no trailing fractional zeros, and no
    /// '.' when the fraction is empty. "007" -> "7", "10.50" -> "10.5", "3.0" -> "3".
    [[nodiscard]] std::string normalized() const;

    /// Numeric order. Defined in selection.cpp.
    friend std::strong_ordering operator<=>(const ChapterNumber& lhs,
                                            const ChapterNumber& rhs) noexcept;
    /// Numeric equality (the stored form is canonical, so member-wise equality is numeric).
    friend bool operator==(const ChapterNumber&, const ChapterNumber&) = default;

private:
    /// For parse() only: both parts are already canonical.
    ChapterNumber(std::string integer, std::string fraction) noexcept
        : integer_(std::move(integer)), fraction_(std::move(fraction)) {}

    std::string integer_{};  ///< digits without leading zeros; "0" for zero
    std::string fraction_{}; ///< digits without trailing zeros; empty for a whole number
};

/// One ITEM of a SPEC. An absent bound is open: `N-` has no `high`, `-M` has no `low`, and `N`
/// has `low == high`.
struct SelectionRange {
    std::optional<ChapterNumber> low{};
    std::optional<ChapterNumber> high{};

    friend bool operator==(const SelectionRange&, const SelectionRange&) = default;
};

/// A parsed SPEC: `all`, `none`, or a list of ranges.
class Selection {
public:
    /// Errc::usage with a one-line message for a malformed SPEC: an empty item, whitespace,
    /// a non-numeric bound, or a range whose low bound is greater than its high bound.
    [[nodiscard]] static Result<Selection> parse(std::string_view spec);

    /// The default: matches every label, numeric or not.
    [[nodiscard]] static Selection all() noexcept { return Selection{Kind::all, {}}; }

    [[nodiscard]] bool is_all() const noexcept { return kind_ == Kind::all; }
    [[nodiscard]] bool is_none() const noexcept { return kind_ == Kind::none; }

    /// `all` matches everything and `none` nothing. A list matches a numeric label inside any
    /// range; a non-numeric label (for example "Extra") matches only `all`.
    [[nodiscard]] bool matches(std::string_view label) const;

    /// Same SPEC meaning. The ranges are compared only for a list: they are empty otherwise, and
    /// skipping them keeps GCC -O2 from a -Wnull-dereference false positive on the empty
    /// vectors once all()/parse() is inlined.
    friend bool operator==(const Selection& lhs, const Selection& rhs) {
        return lhs.kind_ == rhs.kind_ && (lhs.kind_ != Kind::ranges || lhs.ranges_ == rhs.ranges_);
    }

private:
    enum class Kind {
        all,
        none,
        ranges,
    };

    Selection(Kind kind, std::vector<SelectionRange> ranges) noexcept
        : kind_(kind), ranges_(std::move(ranges)) {}

    Kind kind_ = Kind::all;
    std::vector<SelectionRange> ranges_{}; ///< non-empty only for Kind::ranges
};

/// The canonical numeric form of a volume or chapter label (ChapterNumber::normalized), or the
/// label unchanged when it is not numeric. Used as the dedupe key and for display.
[[nodiscard]] std::string normalize_label(std::string_view label);

} // namespace mangadex_dl
