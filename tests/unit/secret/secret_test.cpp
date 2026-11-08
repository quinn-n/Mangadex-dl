// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/secret/secret_test.cpp: Secret and secure_wipe (follows plan T2).
//
// Only buffers that are still owned are ever inspected, never freed memory:
// - a std::string's whole buffer [0, capacity()) through resize_and_overwrite (no reallocation at
//   n == capacity(), and the callback only copies the bytes out);
// - a Secret's or std::string's object representation (std::as_bytes over the live object), which
//   is where a moved-from SSO string keeps bytes 1..n-1 (fact-check V19).
// A buffer a Secret frees on growth cannot be inspected without a use-after-free; that path is
// covered by review and by the secure_wipe tests below.
#include "mangadex_dl/secret.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace md = mangadex_dl;

namespace {

// Obviously fake values. The SSO one fits libstdc++'s 15-byte local buffer; the heap one does not.
constexpr std::string_view sso_value = "fixture-pw-ok";
constexpr std::string_view heap_value =
    "fixture-password-not-a-secret-but-long-enough-to-live-on-the-heap";
constexpr std::string_view other_sso_value = "other-pw-sso";
constexpr std::string_view other_heap_value =
    "other-fixture-password-not-a-secret-also-long-enough-for-the-heap";

static_assert(sso_value.size() <= 15);
static_assert(other_sso_value.size() <= 15);
static_assert(heap_value.size() > 15);
static_assert(other_heap_value.size() > 15);

/// Whether `haystack` contains the bytes of `needle`.
[[nodiscard]] bool contains_bytes(std::span<const std::byte> haystack, std::string_view needle) {
    const auto found = std::ranges::search(haystack, needle, [](std::byte b, char c) {
        return std::to_integer<unsigned char>(b) == static_cast<unsigned char>(c);
    });
    return !found.empty();
}

/// The bytes a moved-from value would leak: everything but byte 0 (V19).
[[nodiscard]] std::string_view leak_fragment(std::string_view value) { return value.substr(1); }

/// The object representation of a live object.
template <class T> [[nodiscard]] std::span<const std::byte> object_bytes(const T& object) {
    return std::as_bytes(std::span<const T, 1>{&object, 1});
}

/// Every byte of `text`'s owned buffer, [0, capacity()). `text` must be empty (it stays empty).
[[nodiscard]] std::vector<unsigned char> buffer_bytes(std::string& text) {
    REQUIRE(text.empty());
    const std::size_t capacity = text.capacity();
    std::vector<unsigned char> bytes(capacity);
    // n == capacity(): no reallocation; returning 0 keeps the string empty.
    text.resize_and_overwrite(capacity, [&bytes](char* data, std::size_t size) {
        std::memcpy(bytes.data(), data, size);
        return std::size_t{0};
    });
    return bytes;
}

[[nodiscard]] bool all_zero(const std::vector<unsigned char>& bytes) {
    return std::ranges::all_of(bytes, [](unsigned char b) { return b == 0; });
}

/// What a moved-from Secret still shows. Taken in one call, so the use after the move is one
/// deliberate inspection.
struct MovedFrom {
    bool empty = false;
    bool leaks = true;
};

[[nodiscard]] MovedFrom inspect(const md::Secret& secret, std::string_view value) {
    return MovedFrom{.empty = secret.empty(),
                     .leaks = contains_bytes(object_bytes(secret), leak_fragment(value))};
}

} // namespace

TEST_CASE("secret: a default Secret is empty", "[secret]") {
    const md::Secret secret{};
    CHECK(secret.empty());
    CHECK(secret.size() == 0);
    CHECK(secret.reveal().empty());
}

TEST_CASE("secret: copy_of keeps SSO and heap values exactly", "[secret]") {
    const std::string_view value = GENERATE(sso_value, heap_value, std::string_view{});
    CAPTURE(value.size());
    const md::Secret secret = md::Secret::copy_of(value);
    CHECK(secret.reveal() == value);
    CHECK(secret.size() == value.size());
    CHECK(secret.empty() == value.empty());
}

TEST_CASE("secret: copy_of keeps embedded NUL bytes", "[secret]") {
    constexpr std::string_view value{"a\0b\0c", 5};
    const md::Secret secret = md::Secret::copy_of(value);
    CHECK(secret.size() == 5);
    CHECK(secret.reveal() == value);
}

TEST_CASE("secret: adopt takes the value and wipes the source string", "[secret]") {
    const std::string_view value = GENERATE(sso_value, heap_value);
    CAPTURE(value.size());
    std::string source{value};
    const md::Secret secret = md::Secret::adopt(std::move(source));
    CHECK(secret.reveal() == value);

    // NOLINTNEXTLINE(bugprone-use-after-move): inspecting the moved-from source is the point
    CHECK(source.empty());
    CHECK(all_zero(buffer_bytes(source)));
    CHECK_FALSE(contains_bytes(object_bytes(source), leak_fragment(value)));
}

TEST_CASE("secret: move construction leaves the source empty and wiped", "[secret]") {
    const std::string_view value = GENERATE(sso_value, heap_value);
    CAPTURE(value.size());
    md::Secret source = md::Secret::copy_of(value);
    const md::Secret target{std::move(source)};
    CHECK(target.reveal() == value);

    // NOLINTNEXTLINE(bugprone-use-after-move): inspecting the moved-from source is the point
    const MovedFrom moved = inspect(source, value);
    CHECK(moved.empty);
    CHECK_FALSE(moved.leaks);
}

TEST_CASE("secret: move assignment wipes the source and replaces the target", "[secret]") {
    const std::string_view old_value = GENERATE(other_sso_value, other_heap_value);
    const std::string_view new_value = GENERATE(sso_value, heap_value);
    CAPTURE(old_value.size(), new_value.size());
    md::Secret target = md::Secret::copy_of(old_value);
    md::Secret source = md::Secret::copy_of(new_value);
    target = std::move(source);
    CHECK(target.reveal() == new_value);
    CHECK_FALSE(contains_bytes(object_bytes(target), leak_fragment(old_value)));

    // NOLINTNEXTLINE(bugprone-use-after-move): inspecting the moved-from source is the point
    const MovedFrom moved = inspect(source, new_value);
    CHECK(moved.empty);
    CHECK_FALSE(moved.leaks);
    // libstdc++ may hand the target's old buffer to the source: it must not show the old value.
    CHECK_FALSE(contains_bytes(object_bytes(source), leak_fragment(old_value)));
    // Gap: a heap buffer handed to the source this way is not inspected here (its capacity is not
    // observable through Secret). It is wiped by secure_wipe, whose whole-capacity wipe is
    // proven by "secure_wipe zeroes the whole buffer without reallocating".
}

TEST_CASE("secret: a moved-from Secret is reusable", "[secret]") {
    md::Secret source = md::Secret::copy_of(heap_value);
    const md::Secret target{std::move(source)};
    CHECK(target.reveal() == heap_value);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): reuse is the point
    source.append(sso_value);
    CHECK(source.reveal() == sso_value);
}

TEST_CASE("secret: append of its own bytes across a growth step", "[secret]") {
    // heap: capacity is exactly 65 (copy_of), so doubling must reallocate while the view still
    // borrows the old buffer (a use-after-free under ASan if the old buffer goes first).
    // sso: 13 bytes in the 15-byte local buffer; 26 crosses the SSO boundary.
    const std::string_view value = GENERATE(heap_value, sso_value);
    CAPTURE(value.size());
    md::Secret secret = md::Secret::copy_of(value);
    secret.append(secret.reveal());
    const std::string expected = std::string{value} + std::string{value};
    CHECK(secret.reveal() == expected);

    // A borrowed suffix, again across a growth step.
    const std::string_view tail = secret.reveal().substr(1);
    const std::string expected_tail = expected + expected.substr(1);
    secret.append(tail);
    CHECK(secret.reveal() == expected_tail);
}

TEST_CASE("secret: append of its own bytes without growth", "[secret]") {
    const std::string_view value = GENERATE(heap_value, sso_value);
    CAPTURE(value.size());
    md::Secret secret = md::Secret::copy_of(value);
    secret.reserve(4 * value.size());
    secret.append(secret.reveal());
    CHECK(secret.reveal() == std::string{value} + std::string{value});
}

TEST_CASE("secret: push_back of its own byte across a growth step", "[secret]") {
    md::Secret secret = md::Secret::copy_of(heap_value);
    secret.push_back(secret.reveal().front());
    CHECK(secret.reveal() == std::string{heap_value} + heap_value.front());
}

TEST_CASE("secret: append grows across several appends", "[secret]") {
    md::Secret secret{};
    std::string expected{};
    // Crosses the SSO boundary and several geometric growth steps.
    for (int round = 0; round < 40; ++round) {
        secret.append(heap_value);
        expected.append(heap_value);
        secret.push_back('#');
        expected.push_back('#');
        REQUIRE(secret.reveal() == expected);
    }
    CHECK(secret.size() == expected.size());
    secret.append(std::string_view{});
    CHECK(secret.reveal() == expected);
}

TEST_CASE("secret: push_back builds a value byte by byte", "[secret]") {
    md::Secret secret{};
    std::string expected{};
    for (int i = 0; i < 5000; ++i) {
        const char c = static_cast<char>('a' + (i % 26));
        secret.push_back(c);
        expected.push_back(c);
    }
    CHECK(secret.size() == 5000);
    CHECK(secret.reveal() == expected);
}

TEST_CASE("secret: reserve keeps the value", "[secret]") {
    const std::string_view value = GENERATE(sso_value, heap_value);
    CAPTURE(value.size());
    md::Secret secret = md::Secret::copy_of(value);
    secret.reserve(4096);
    CHECK(secret.reveal() == value);
    secret.reserve(0); // never shrinks
    CHECK(secret.reveal() == value);
    secret.append(other_heap_value);
    CHECK(secret.size() == value.size() + other_heap_value.size());
    CHECK(secret.reveal().substr(0, value.size()) == value);
    CHECK(secret.reveal().substr(value.size()) == other_heap_value);
}

TEST_CASE("secret: clear empties the value and the Secret stays usable", "[secret]") {
    const std::string_view value = GENERATE(sso_value, heap_value);
    CAPTURE(value.size());
    md::Secret secret = md::Secret::copy_of(value);
    secret.clear();
    CHECK(secret.empty());
    CHECK(secret.size() == 0);
    CHECK_FALSE(contains_bytes(object_bytes(secret), leak_fragment(value)));
    secret.append(other_sso_value);
    CHECK(secret.reveal() == other_sso_value);
}

TEST_CASE("secret: reveal borrows the stored bytes", "[secret]") {
    const md::Secret secret = md::Secret::copy_of(heap_value);
    const std::string_view first = secret.reveal();
    const std::string_view second = secret.reveal();
    CHECK(first.data() == second.data());
    CHECK(first == heap_value);
}

TEST_CASE("secret: secure_wipe zeroes the whole buffer without reallocating", "[secret]") {
    const std::string_view value = GENERATE(sso_value, heap_value, std::string_view{});
    const std::size_t extra = GENERATE(std::size_t{0}, std::size_t{100});
    CAPTURE(value.size(), extra);
    std::string text{};
    text.reserve(value.size() + extra);
    text.assign(value);
    const char* const buffer = text.data();
    const std::size_t capacity = text.capacity();

    md::secure_wipe(text);

    CHECK(text.empty());
    CHECK(text.data() == buffer);
    CHECK(text.capacity() == capacity);
    const std::vector<unsigned char> bytes = buffer_bytes(text);
    CHECK(bytes.size() == capacity);
    CHECK(all_zero(bytes));
    if (!value.empty()) {
        CHECK_FALSE(contains_bytes(object_bytes(text), leak_fragment(value)));
    }
}

TEST_CASE("secret: secure_wipe after a plain move clears the SSO bytes a move leaves", "[secret]") {
    std::string source{sso_value};
    const std::string target{std::move(source)};
    CHECK(target == sso_value);
    // NOLINTNEXTLINE(bugprone-use-after-move): wiping the moved-from source is the point
    md::secure_wipe(source);
    CHECK(source.empty());
    CHECK(all_zero(buffer_bytes(source)));
    CHECK_FALSE(contains_bytes(object_bytes(source), leak_fragment(sso_value)));
}
