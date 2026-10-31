// SPDX-License-Identifier: GPL-3.0-or-later
//
// Secret memory: a self-wiping string for passwords, client secrets, tokens and the form bodies and
// session documents that hold them, plus the process-wide core-dump guard.
// Frozen interface (follows plan T1); implemented in src/core/secret.cpp and
// src/core/process_guard.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace mangadex_dl {

/// explicit_bzero over the whole buffer via resize(capacity()), then clear(). Never reallocates
/// (V19), so no copy of the bytes is left behind by the wipe itself.
void secure_wipe(std::string& text) noexcept;

/// A secret string (password, client secret, token, or a form body / session document holding
/// one). Move-only; no formatter, no operator<<, no ==, no implicit conversion. Wiped on
/// destruction, assignment, clear() and growth.
class Secret {
public:
    Secret() noexcept = default;
    /// A copy of `text`, reserved up front, so building it never reallocates.
    [[nodiscard]] static Secret copy_of(std::string_view text);
    /// Takes the buffer of `text`, then wipes `text`.
    [[nodiscard]] static Secret adopt(std::string&& text) noexcept;

    /// Steals, then wipes other's buffer (a moved-from SSO string keeps bytes 1..n-1, V19).
    Secret(Secret&& other) noexcept;
    /// Wipes self first, then steals and wipes other's buffer.
    Secret& operator=(Secret&& other) noexcept;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret();

    /// Growth copies into a new buffer, then wipes the old one.
    void reserve(std::size_t bytes);
    /// Grows as reserve() does.
    void append(std::string_view text);
    void push_back(char c);
    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    /// Borrows *this: never bind it to a temporary Secret, never store it.
    [[nodiscard]] std::string_view reveal() const noexcept MANGADEX_DL_LIFETIMEBOUND;

private:
    std::string value_{};
};

/// What disable_core_dumps() achieved.
enum class DumpGuard {
    not_dumpable, ///< the process is no longer dumpable (RLIMIT_CORE is normally 0 too)
    no_core_only, ///< RLIMIT_CORE is 0, but PR_SET_DUMPABLE failed
    none,         ///< neither call succeeded
};

/// setrlimit(RLIMIT_CORE, {0,0}), then prctl(PR_SET_DUMPABLE, 0) (V20, V21 confirmed). Best
/// effort; called once, from the main thread, before any secret is read.
[[nodiscard]] DumpGuard disable_core_dumps() noexcept;

} // namespace mangadex_dl
