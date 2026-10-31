// SPDX-License-Identifier: GPL-3.0-or-later
//
// Secret and secure_wipe (follows plan T2). A Secret owns exactly one std::string buffer and makes
// sure no copy of its bytes outlives it inside memory this process still owns:
// - every buffer it gives up (destruction, assignment, clear(), growth) is wiped over its whole
//   capacity first, not just over size();
// - a moved-from libstdc++ SSO string keeps bytes 1..n-1 of its local buffer (only byte 0 is set
//   to NUL, fact-check V19), so every move also wipes the source;
// - growth is done here, never by std::string itself: reserve() and append() copy into a new
//   buffer, wipe the old one and swap, so std::string::append never reallocates behind our back;
// - append() accepts a view into *this (s.append(s.reveal())): on growth it copies the view into
//   the new buffer before the old one is wiped and freed.
// What libcurl, the kernel or nlohmann copy is out of reach; see the plan's watch-list.
#include "mangadex_dl/secret.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// NOLINTNEXTLINE(modernize-deprecated-headers): explicit_bzero is a glibc extension of <string.h>
#include <string.h>

namespace mangadex_dl {
namespace {

/// Replaces `value`'s buffer by a new one of `capacity` bytes holding `value` + `tail`, then wipes
/// the old buffer (it is freed when `grown` dies). `tail` may borrow from `value`: both are read
/// before the wipe. Precondition: capacity >= value.size() + tail.size(). If the allocation throws
/// (std::length_error / std::bad_alloc), `value` is unchanged.
void regrow(std::string& value, std::size_t capacity, std::string_view tail) {
    std::string grown{};
    grown.reserve(capacity);
    grown.append(value); // fits: no reallocation, no throw
    grown.append(tail);  // fits as well
    secure_wipe(value);
    value.swap(grown);
}

} // namespace

void secure_wipe(std::string& text) noexcept {
    // resize() up to capacity() never reallocates (V19), so the whole owned buffer lies in
    // [0, size()) and explicit_bzero (GLIBC_2.25, within the 2.28 floor, V15) covers every byte
    // of it. The compiler may not elide it, unlike a memset before the buffer dies.
    text.resize(text.capacity());
    ::explicit_bzero(text.data(), text.size());
    text.clear();
}

Secret Secret::copy_of(std::string_view text) {
    Secret secret{};
    secret.reserve(text.size());
    secret.value_.append(text);
    return secret;
}

Secret Secret::adopt(std::string&& text) noexcept {
    Secret secret{};
    // Move assignment into an empty string is noexcept: a heap buffer is taken over as is; an SSO
    // value is copied into secret's local buffer, and `text` keeps bytes 1..n-1 of its own (only
    // byte 0 becomes NUL, V19). clear() puts the moved-from `text` back into a specified state
    // (empty) before the wipe covers its whole buffer.
    secret.value_ = std::move(text);
    text.clear();
    secure_wipe(text);
    return secret;
}

Secret::Secret(Secret&& other) noexcept : value_(std::move(other.value_)) {
    secure_wipe(other.value_);
}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    secure_wipe(value_);
    // libstdc++ may hand this's old (already wiped) heap buffer to `other`; the second wipe covers
    // that buffer as well as the SSO bytes a move leaves behind.
    value_ = std::move(other.value_);
    secure_wipe(other.value_);
    return *this;
}

Secret::~Secret() { secure_wipe(value_); }

void Secret::reserve(std::size_t bytes) {
    if (bytes <= value_.capacity()) {
        return;
    }
    regrow(value_, bytes, std::string_view{});
}

void Secret::append(std::string_view text) {
    const std::size_t size = value_.size();
    if (text.size() > value_.max_size() - size) {
        throw std::length_error("Secret::append: size overflow");
    }
    const std::size_t needed = size + text.size();
    if (needed > value_.capacity()) {
        // Geometric growth, so a byte-by-byte build (push_back) costs O(n) copies, not O(n^2).
        const std::size_t capacity = value_.capacity();
        const std::size_t doubled =
            capacity <= value_.max_size() / 2 ? capacity * 2 : value_.max_size();
        // `text` may borrow from value_ (s.append(s.reveal())): regrow reads it before the old
        // buffer is wiped and freed.
        regrow(value_, needed > doubled ? needed : doubled, text);
        return;
    }
    // Within capacity: never reallocates. A borrowed `text` lies in [data(), data() + size()),
    // which the copy to [data() + size(), …) never overlaps.
    value_.append(text);
}

void Secret::push_back(char c) { append(std::string_view{&c, 1}); }

void Secret::clear() noexcept { secure_wipe(value_); }

bool Secret::empty() const noexcept { return value_.empty(); }

std::size_t Secret::size() const noexcept { return value_.size(); }

std::string_view Secret::reveal() const noexcept { return value_; }

} // namespace mangadex_dl
