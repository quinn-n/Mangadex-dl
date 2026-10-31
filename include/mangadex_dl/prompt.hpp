// SPDX-License-Identifier: GPL-3.0-or-later
//
// The prompt seam: the login flow asks for missing details through a Prompter. The app supplies
// the real terminal prompter (src/app/cmd_auth_prompt.cpp); tests script a fake one.
// Frozen interface (follows plan T1).
#pragma once

#include "mangadex_dl/secret.hpp"

#include <cstddef>
#include <string_view>

namespace mangadex_dl {

/// Whether the terminal echoes the answer while it is typed.
enum class Echo {
    on,
    off,
};

enum class PromptStatus {
    answered,    ///< a non-empty line was read
    empty,       ///< an empty line, or end of input before any byte
    too_long,    ///< the line was longer than max_prompt_answer_bytes
    interrupted, ///< SIGINT/SIGTERM arrived while waiting
    unavailable, ///< no usable terminal (for Echo::off: echo could not be turned off)
};

struct PromptReply {
    PromptStatus status = PromptStatus::unavailable;
    Secret answer{}; ///< the line without its '\n'/'\r\n'; empty unless status == answered
};

/// The longest answer accepted, in bytes (the line ending excluded). Note: an ICANON (canonical
/// mode) terminal caps a line at 4095 bytes, so on a real TTY the limit is reached only in
/// non-canonical mode.
inline constexpr std::size_t max_prompt_answer_bytes = 4096;

/// Asks the user for one detail at a time. Polymorphic base per house rules: neither copyable nor
/// movable, so it can't be sliced.
class Prompter {
public:
    Prompter(const Prompter&) = delete;
    Prompter& operator=(const Prompter&) = delete;
    Prompter(Prompter&&) = delete;
    Prompter& operator=(Prompter&&) = delete;
    virtual ~Prompter() = default;

    /// One line on the user's terminal (a hint, never a secret).
    virtual void notice(std::string_view line) = 0;
    /// Writes `question`, reads one line (≤ max_prompt_answer_bytes; '\n'/'\r\n' stripped).
    /// Echo::off: echo is off for the read and restored on every path, then a newline is written.
    /// Polls interrupt_requested() at least every 100 ms (the handlers use SA_RESTART). Never
    /// throws past the restore.
    [[nodiscard]] virtual PromptReply ask(std::string_view question, Echo echo) = 0;

protected:
    Prompter() = default;
};

} // namespace mangadex_dl
