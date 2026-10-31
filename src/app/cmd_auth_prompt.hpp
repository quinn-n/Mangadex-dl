// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real terminal prompter (follows plan T7): the Prompter the login flow uses when Io injects
// none. It writes questions and notices to the diagnostics stream and reads one answer per ask()
// from a borrowed file descriptor (normally stdin, a TTY).
//
// Echo::off turns the terminal's ECHO flag off with TCSAFLUSH before the question is written and
// restores the saved termios on every path out of ask() (an answer, end of input, a too-long line,
// an interrupt, an exception), then writes a newline. The read never blocks for more than 100 ms:
// SIGINT/SIGTERM handlers use SA_RESTART, so ask() polls and checks interrupt_requested() between
// waits. The answer goes straight into a Secret; the read buffer is wiped before ask() returns.
#pragma once

#include "mangadex_dl/prompt.hpp"

#include <iosfwd>
#include <string_view>

namespace mangadex_dl::app {

/// Prompts on `err` and reads answers from `fd`. Both are borrowed: they must outlive the
/// prompter, and `fd` is never closed. Not thread-safe: one ask() at a time.
///
/// ask() behaviour beyond the Prompter contract:
/// - `fd` < 0, or an fd that poll() reports invalid → unavailable. Echo::off on an fd that is not
///   a terminal (tcgetattr fails), or whose ECHO flag can't be cleared → unavailable, and nothing
///   is written.
/// - The question and notices pass through terminal_safe (they may hold a username).
/// - A line ends at '\n' (a '\r' right before it is stripped too). End of input after some bytes
///   ends the line as well; end of input before any byte, or an empty line, is `empty`.
/// - At most max_prompt_answer_bytes + 1 bytes are kept while waiting for the line ending, so the
///   answer never grows past its reserved capacity. Bytes that arrive after the '\n' within the
///   same read() are discarded and wiped: a canonical-mode terminal returns at most one line per
///   read(), so nothing is lost there.
/// - A read error other than EINTR/EAGAIN → unavailable; EIO after a hang-up counts as end of
///   input (a pty slave whose master is gone).
class TerminalPrompter final : public Prompter {
public:
    TerminalPrompter(int fd, std::ostream& err) noexcept;

    void notice(std::string_view line) override;
    [[nodiscard]] PromptReply ask(std::string_view question, Echo echo) override;

private:
    int fd_ = -1;
    std::ostream* err_ = nullptr; ///< never null: set from a reference
};

} // namespace mangadex_dl::app
