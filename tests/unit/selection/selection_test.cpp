// tests/unit/selection/selection_test.cpp: the SPEC grammar and numeric labels (download spec
// acceptance criterion 2).
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/selection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace md = mangadex_dl;

namespace {

/// Kept as an optional (clang-tidy cannot see that REQUIRE throws, so it would flag a deref);
/// REQUIRE guarantees every comparison below is between two engaged values.
[[nodiscard]] std::optional<md::ChapterNumber> number(std::string_view text) {
    std::optional<md::ChapterNumber> parsed = md::ChapterNumber::parse(text);
    REQUIRE(parsed.has_value());
    return parsed;
}

[[nodiscard]] std::optional<std::string> normalized(std::string_view text) {
    return md::ChapterNumber::parse(text).transform(&md::ChapterNumber::normalized);
}

[[nodiscard]] md::Selection selection(std::string_view spec) {
    md::Result<md::Selection> parsed = md::Selection::parse(spec);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// The usage-error message for `spec`, or "<accepted>" when it parsed.
[[nodiscard]] std::string parse_error(std::string_view spec) {
    const md::Result<md::Selection> parsed = md::Selection::parse(spec);
    if (parsed.has_value()) {
        return "<accepted>";
    }
    CHECK(parsed.error().code == md::Errc::usage);
    return parsed.error().message;
}

/// A heap copy of `text` with no terminator: the last byte of the view is the last byte of the
/// allocation, so ASan reports any read one past the end of the view.
class ExactBuffer {
    // An exact-size heap array is the point: std::string adds a NUL and std::vector may
    // over-allocate, either of which would hide a one-past-end read from ASan.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): exact-size alloc
    using Bytes = char[];

public:
    explicit ExactBuffer(std::string_view text)
        : size_(text.size()), bytes_(std::make_unique<Bytes>(text.size())) {
        std::ranges::copy(text, bytes_.get());
    }

    [[nodiscard]] std::string_view view() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return {bytes_.get(), size_};
    }

private:
    std::size_t size_ = 0;
    std::unique_ptr<Bytes> bytes_{};
};

} // namespace

TEST_CASE("ChapterNumber accepts digits with an optional fraction", "[selection]") {
    CHECK(normalized("0") == "0");
    CHECK(normalized("7") == "7");
    CHECK(normalized("007") == "7");
    CHECK(normalized("000") == "0");
    CHECK(normalized("1.5") == "1.5");
    CHECK(normalized("10.50") == "10.5");
    CHECK(normalized("3.0") == "3");
    CHECK(normalized("00.000") == "0");
    CHECK(normalized("012.05") == "12.05");
    // Digit strings never overflow.
    CHECK(normalized("123456789012345678901234567890.1") == "123456789012345678901234567890.1");
}

TEST_CASE("ChapterNumber rejects everything that is not plain digits[.digits]", "[selection]") {
    const std::string text = GENERATE(as<std::string>{}, "", ".", "1.", ".5", "-1", "+1", "1e3",
                                      " 1", "1 ", "1.2.3", "a", "1a", "0x10", "1,5",
                                      "\xef\xbc\x91", // U+FF11 FULLWIDTH DIGIT ONE
                                      "Extra");
    CAPTURE(text);
    CHECK_FALSE(md::ChapterNumber::parse(text).has_value());
}

TEST_CASE("ChapterNumber compares numerically", "[selection]") {
    CHECK(number("007") == number("7"));
    CHECK(number("10.50") == number("10.5"));
    CHECK(number("3.0") == number("3"));
    CHECK(number("1") < number("1.5"));
    CHECK(number("1.5") < number("2"));
    CHECK(number("2") < number("10"));
    CHECK(number("10") < number("10.5"));
    CHECK(number("10.5") < number("100"));
    CHECK(number("0.05") < number("0.1"));
    CHECK(number("0.1") < number("0.15"));
    CHECK(number("0.15") < number("0.5"));
    CHECK(number("99999999999999999999") < number("123456789012345678901234567890"));
    CHECK((number("09") <=> number("9")) == std::strong_ordering::equal);
    CHECK((number("12") <=> number("9")) == std::strong_ordering::greater);
}

TEST_CASE("normalize_label canonicalises numbers and keeps other labels", "[selection]") {
    CHECK(md::normalize_label("007") == "7");
    CHECK(md::normalize_label("10.50") == "10.5");
    CHECK(md::normalize_label("3.0") == "3");
    CHECK(md::normalize_label("00") == "0");
    CHECK(md::normalize_label("Extra") == "Extra");
    CHECK(md::normalize_label("1.") == "1.");
    CHECK(md::normalize_label("").empty());
}

TEST_CASE("all matches every label and none matches nothing", "[selection]") {
    const md::Selection all = selection("all");
    CHECK(all.is_all());
    CHECK_FALSE(all.is_none());
    CHECK(all == md::Selection::all());
    const md::Selection none = selection("none");
    CHECK(none.is_none());
    CHECK_FALSE(none.is_all());
    CHECK(none != md::Selection::all());

    for (const std::string_view label : {"1", "0", "10.5", "Extra", "", "x1"}) {
        CAPTURE(label);
        CHECK(all.matches(label));
        CHECK(md::Selection::all().matches(label));
        CHECK_FALSE(none.matches(label));
    }
}

TEST_CASE("A range list matches numeric labels inclusively", "[selection]") {
    const md::Selection list = selection("1,3-5,8-");
    CHECK_FALSE(list.is_all());
    CHECK_FALSE(list.is_none());
    for (const std::string_view label : {"1", "01", "1.0", "3", "4", "4.5", "5", "5.0", "8", "8.0",
                                         "9", "1000", "123456789012345678901234567890"}) {
        CAPTURE(label);
        CHECK(list.matches(label));
    }
    for (const std::string_view label : {"0", "0.5", "1.1", "2", "2.9", "5.01", "7", "7.99"}) {
        CAPTURE(label);
        CHECK_FALSE(list.matches(label));
    }
}

TEST_CASE("Open-low, single and fractional items", "[selection]") {
    const md::Selection up_to_three = selection("-3");
    CHECK(up_to_three.matches("0"));
    CHECK(up_to_three.matches("2.5"));
    CHECK(up_to_three.matches("3"));
    CHECK_FALSE(up_to_three.matches("3.1"));

    const md::Selection one_and_a_half = selection("1.5");
    CHECK(one_and_a_half.matches("1.5"));
    CHECK(one_and_a_half.matches("1.50"));
    CHECK(one_and_a_half.matches("01.5"));
    CHECK_FALSE(one_and_a_half.matches("1"));
    CHECK_FALSE(one_and_a_half.matches("2"));

    CHECK(selection("10.50").matches("10.5"));
    CHECK(selection("007").matches("7"));
    CHECK(selection("2-2").matches("2"));
    CHECK(selection("1.5-2.5").matches("2"));
    CHECK_FALSE(selection("1.5-2.5").matches("2.51"));
}

TEST_CASE("Non-numeric labels match only all", "[selection]") {
    for (const std::string_view spec : {"1,3-5,8-", "0-", "-999999", "none"}) {
        CAPTURE(spec);
        const md::Selection parsed = selection(spec);
        CHECK_FALSE(parsed.matches("Extra"));
        CHECK_FALSE(parsed.matches(""));
        CHECK_FALSE(parsed.matches("1a"));
        CHECK_FALSE(parsed.matches("-1"));
    }
    CHECK(selection("all").matches("Extra"));
}

TEST_CASE("Range-list selections compare by meaning of their items", "[selection]") {
    CHECK(selection("1,3-5,8-") == selection("1,3-5,8-"));
    CHECK(selection("10.50") == selection("10.5"));
    CHECK(selection("007") == selection("7"));
    CHECK(selection("01-05,8-") == selection("1-5,8.0-"));
    CHECK(selection("1,3-5") != selection("1,3-6"));
    CHECK(selection("1,3-5") != selection("3-5,1")); // order is part of the value
    CHECK(selection("3") != selection("3-"));
    CHECK(selection("-3") != selection("3-"));
    CHECK(selection("1") != selection("1,1"));
    CHECK(selection("1") != md::Selection::all());
    CHECK(selection("1") != selection("none"));
    CHECK(selection("all") == md::Selection::all());
    CHECK(selection("none") == selection("none"));
}

TEST_CASE("Malformed SPECs are usage errors with a one-line message", "[selection]") {
    CHECK(parse_error("") ==
          "the selection is empty (expected all, none or a list such as 1,3-5,8-)");
    CHECK(parse_error("5-3") ==
          "'5-3' is an empty range: the low bound is greater than the high bound");
    CHECK(parse_error("1,10.5-10.25") ==
          "'10.5-10.25' is an empty range: the low bound is greater than the high bound");
    CHECK(parse_error("1,,2") == "the selection has an empty item");
    CHECK(parse_error(",1") == "the selection has an empty item");
    CHECK(parse_error("1,") == "the selection has an empty item");
    CHECK(parse_error(",") == "the selection has an empty item");
    CHECK(parse_error("-") == "'-' needs at least one bound (N-, -M or N-M)");
    CHECK(parse_error("1-2-3") == "'1-2-3' has more than one '-' (expected N-M)");
    CHECK(parse_error("--3") == "'--3' has more than one '-' (expected N-M)");
    CHECK(parse_error("a") == "'a' is not a number");
    CHECK(parse_error("1-b") == "'b' is not a number");
    CHECK(parse_error("1.-2") == "'1.' is not a number");
    CHECK(parse_error("all,1") == "'all' is not a number");
    CHECK(parse_error("ALL") == "'ALL' is not a number");
    for (const std::string_view spec : {"1, 2", " all", "all ", "1\t", "1 -3", "\n", "none\r"}) {
        CAPTURE(spec);
        CHECK(parse_error(spec) == "the selection must not contain whitespace");
    }
}

TEST_CASE("Selection error messages never echo raw control or non-ASCII bytes", "[selection]") {
    CHECK(parse_error("1,\x1b[31m") == "'?[31m' is not a number");
    // "'?\?'" spells '??' without forming the ??' trigraph.
    CHECK(parse_error("\xc3\xa9") == "'?\?' is not a number");
    CHECK(parse_error(std::string(40, 'x')) == "'" + std::string(32, 'x') + "...' is not a number");
}

TEST_CASE("ChapterNumber::parse never reads past the end of its view", "[selection][bounds]") {
    constexpr std::string_view text = "0012.500";
    for (std::size_t length = 0; length <= text.size(); ++length) {
        const std::string_view prefix = text.substr(0, length);
        CAPTURE(prefix);
        const ExactBuffer buffer{prefix};
        const std::optional<md::ChapterNumber> parsed = md::ChapterNumber::parse(buffer.view());
        CHECK(parsed.has_value() == (!prefix.empty() && !prefix.ends_with('.')));
        CHECK(md::normalize_label(buffer.view()) ==
              (parsed.has_value() ? parsed->normalized() : std::string{prefix}));
    }
}

TEST_CASE("Selection::parse never reads past the end of its view", "[selection][bounds]") {
    constexpr std::string_view spec = "1,3-5,8-,-3.5";
    for (std::size_t length = 0; length <= spec.size(); ++length) {
        const std::string_view prefix = spec.substr(0, length);
        CAPTURE(prefix);
        const ExactBuffer buffer{prefix};
        const md::Result<md::Selection> parsed = md::Selection::parse(buffer.view());
        // The invalid prefixes: empty, ending in ',' or '.', and the lone "-" item.
        const bool valid = !prefix.empty() && !prefix.ends_with(',') && !prefix.ends_with('.') &&
                           !prefix.ends_with(",-");
        CHECK(parsed.has_value() == valid);
        if (!parsed.has_value()) {
            CHECK(parsed.error().code == md::Errc::usage);
        }
    }
    const ExactBuffer whole{spec};
    const md::Result<md::Selection> parsed = md::Selection::parse(whole.view());
    REQUIRE(parsed.has_value());
    CHECK(parsed->matches("3.5"));
    CHECK(parsed->matches("4"));
    CHECK_FALSE(parsed->matches("6"));
    const ExactBuffer label{"8"};
    CHECK(parsed->matches(label.view()));
}
