// SPDX-License-Identifier: GPL-3.0-or-later
//
// TerminalPrompter (follows plan T7). The order inside ask() matters:
// 1. the big allocations (the terminal_safe question, the answer's reserved buffer) happen before
//    the terminal is touched, so their failure leaves the terminal as it was. One allocation does
//    happen while echo is off: stripping a '\r' before '\n' (Secret::copy_of in finish_line). If
//    it throws, std::optional<EchoOff> restores the termios and ReadBuffer wipes itself while the
//    exception unwinds, like any other exception out of ask();
// 2. Echo::off: ECHO is cleared (TCSAFLUSH) *before* the question is written, as getpass(3) does,
//    so nothing the user types after seeing the question is echoed or flushed away;
// 3. the read loop: poll() for at most 100 ms, then interrupt_requested(). The handlers use
//    SA_RESTART, so a blocking read() would never return on Ctrl-C; poll() is never restarted
//    after a handler, and the 100 ms timeout bounds the wait even if the signal hit another
//    thread;
// 4. the saved termios is restored (RAII, on every path, exceptions included), then a newline is
//    written, because the user's Enter was not echoed.
// ISIG stays on, so Ctrl-C still raises SIGINT while echo is off.
#include "cmd_auth_prompt.hpp"

#include "cmd_download_output.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <ios>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <sys/poll.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): explicit_bzero is a glibc extension of <string.h>
#include <string.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace mangadex_dl::app {
namespace {

/// The longest wait before interrupt_requested() is checked again (prompt.hpp: at least every
/// 100 ms).
constexpr int poll_interval_ms = 100;

/// Bytes kept while the line ending has not arrived yet: the longest answer, plus a '\r' that a
/// following '\n' would strip. One more byte is too long whatever comes next.
constexpr std::size_t kept_bytes_cap = max_prompt_answer_bytes + 1;

/// One read() at most fills this (the plan's 4097-byte stack buffer).
constexpr std::size_t read_buffer_bytes = max_prompt_answer_bytes + 1;

constexpr tcflag_t echo_flag = ECHO;

/// Is `event` set in a pollfd's `revents`? (The flags are signed shorts and ints.)
[[nodiscard]] constexpr bool has_event(short revents, int event) noexcept {
    return (static_cast<unsigned>(revents) & static_cast<unsigned>(event)) != 0U;
}

/// Does `echo` hide the answer while it is typed?
[[nodiscard]] constexpr bool hides_answer(Echo echo) noexcept {
    switch (echo) {
    case Echo::on:
        return false;
    case Echo::off:
        return true;
    }
    return true; // out of range: the safe side
}

/// tcsetattr(TCSAFLUSH), retried on EINTR. TCSAFLUSH also discards input not read yet: on the way
/// in that is type-ahead the user typed before echo went off, on the way out what was typed while
/// echo was off and not consumed (it may be part of a secret), as getpass(3) does.
[[nodiscard]] bool set_attributes(int fd, const termios& attributes) noexcept {
    for (;;) {
        if (::tcsetattr(fd, TCSAFLUSH, &attributes) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

/// Clears ECHO on `fd` for its lifetime and restores the saved termios on destruction, on every
/// path. Neither copyable nor movable: it lives in place (std::optional::emplace).
class EchoOff {
public:
    explicit EchoOff(int fd) noexcept : fd_(fd) {
        if (::tcgetattr(fd_, &saved_) != 0) {
            return; // not a terminal: nothing changed, nothing to restore
        }
        // From here on the saved termios is put back, even if the change below only partly
        // succeeded (tcsetattr reports success when any part of a change was applied).
        engaged_ = true;
        termios quiet = saved_;
        quiet.c_lflag &= ~echo_flag;
        if (!set_attributes(fd_, quiet)) {
            return;
        }
        termios now{};
        echo_off_ = ::tcgetattr(fd_, &now) == 0 && (now.c_lflag & echo_flag) == 0;
    }
    EchoOff(const EchoOff&) = delete;
    EchoOff& operator=(const EchoOff&) = delete;
    EchoOff(EchoOff&&) = delete;
    EchoOff& operator=(EchoOff&&) = delete;
    ~EchoOff() {
        if (engaged_) {
            static_cast<void>(set_attributes(fd_, saved_));
        }
    }

    /// True when the terminal's ECHO flag is confirmed off.
    [[nodiscard]] bool echo_off() const noexcept { return echo_off_; }

private:
    int fd_ = -1;
    termios saved_{};
    bool engaged_ = false;
    bool echo_off_ = false;
};

/// The stack buffer read() fills, wiped when it goes out of scope, on every path.
class ReadBuffer {
public:
    ReadBuffer() noexcept = default;
    ReadBuffer(const ReadBuffer&) = delete;
    ReadBuffer& operator=(const ReadBuffer&) = delete;
    ReadBuffer(ReadBuffer&&) = delete;
    ReadBuffer& operator=(ReadBuffer&&) = delete;
    ~ReadBuffer() { ::explicit_bzero(bytes_.data(), bytes_.size()); }

    [[nodiscard]] char* data() noexcept { return bytes_.data(); }
    [[nodiscard]] static constexpr std::size_t size() noexcept { return read_buffer_bytes; }

private:
    std::array<char, read_buffer_bytes> bytes_{};
};

/// Writes `text` and flushes. Stream errors are left in the stream state (stderr is best effort).
void write_text(std::ostream& out, std::string_view text) {
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
}

/// The status of a finished line of `line.size()` kept bytes. `line_ended`: it ended with '\n'
/// (then a trailing '\r' is stripped), rather than with end of input.
[[nodiscard]] PromptStatus finish_line(Secret& line, bool line_ended) {
    if (line_ended && !line.empty() && line.reveal().back() == '\r') {
        // copy_of reserves exactly, then the move-assignment wipes the old buffer.
        Secret stripped = Secret::copy_of(line.reveal().substr(0, line.size() - 1));
        line = std::move(stripped);
    }
    if (line.empty()) {
        return PromptStatus::empty;
    }
    if (line.size() > max_prompt_answer_bytes) {
        return PromptStatus::too_long;
    }
    return PromptStatus::answered;
}

/// Is `error` (an errno value) worth another poll() round?
[[nodiscard]] constexpr bool retryable(int error) noexcept {
    return error == EINTR || error == EAGAIN;
}

/// Appends the bytes of `chunk` up to its first '\n' to `line`; bytes after it are dropped. The
/// finished status, or nullopt while the line ending has not arrived.
[[nodiscard]] std::optional<PromptStatus> take_chunk(Secret& line, std::string_view chunk) {
    const std::size_t newline = chunk.find('\n');
    const std::string_view data = chunk.substr(0, newline); // npos: the whole chunk
    if (data.size() > kept_bytes_cap - line.size()) {
        return PromptStatus::too_long;
    }
    line.append(data); // fits the reservation: never reallocates
    if (newline == std::string_view::npos) {
        return std::nullopt;
    }
    return finish_line(line, true);
}

/// One poll() of at most poll_interval_ms, then at most one read(). The finished status, or
/// nullopt to keep waiting.
[[nodiscard]] std::optional<PromptStatus> read_step(int fd, ReadBuffer& buffer, Secret& line) {
    pollfd entry{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&entry, 1, poll_interval_ms);
    if (ready == 0 || (ready < 0 && retryable(errno))) {
        return std::nullopt; // the caller checks for an interrupt first
    }
    if (ready < 0 || has_event(entry.revents, POLLNVAL)) {
        return PromptStatus::unavailable;
    }
    // POLLIN, POLLHUP or POLLERR: read() tells which. After a hang-up the bytes still buffered
    // come first, then 0 (a pipe) or EIO (a pty slave whose master closed); POLLIN alone is not
    // proof of data.
    const bool hung_up = has_event(entry.revents, POLLHUP);
    const ssize_t got = ::read(fd, buffer.data(), ReadBuffer::size());
    if (got < 0) {
        const int error = errno;
        if (retryable(error)) {
            return std::nullopt;
        }
        if (error == EIO && hung_up) {
            return finish_line(line, false);
        }
        return PromptStatus::unavailable;
    }
    if (got == 0) {
        return finish_line(line, false);
    }
    return take_chunk(line, std::string_view{buffer.data(), static_cast<std::size_t>(got)});
}

/// Reads one line from `fd` into `line` (reserved to kept_bytes_cap, never grown past it),
/// checking for an interrupt before every wait.
[[nodiscard]] PromptStatus read_line(int fd, Secret& line) {
    ReadBuffer buffer{};
    for (;;) {
        if (interrupt_requested()) {
            return PromptStatus::interrupted;
        }
        if (const std::optional<PromptStatus> done = read_step(fd, buffer, line)) {
            return *done;
        }
    }
}

} // namespace

TerminalPrompter::TerminalPrompter(int fd, std::ostream& err) noexcept : fd_(fd), err_(&err) {}

void TerminalPrompter::notice(std::string_view line) {
    std::string shown = terminal_safe(line);
    shown.push_back('\n');
    write_text(*err_, shown);
}

PromptReply TerminalPrompter::ask(std::string_view question, Echo echo) {
    if (fd_ < 0) {
        return PromptReply{.status = PromptStatus::unavailable};
    }
    const std::string shown = terminal_safe(question);
    Secret line{};
    line.reserve(kept_bytes_cap);

    std::optional<EchoOff> quiet{};
    if (hides_answer(echo)) {
        quiet.emplace(fd_);
        if (!quiet->echo_off()) {
            return PromptReply{.status = PromptStatus::unavailable}; // ~EchoOff restores
        }
    }
    write_text(*err_, shown);
    const PromptStatus status = read_line(fd_, line);
    if (quiet.has_value()) {
        quiet.reset(); // echo back on before anything else is written
        write_text(*err_, "\n");
    }
    if (status != PromptStatus::answered) {
        return PromptReply{.status = status}; // `line` wipes itself
    }
    return PromptReply{.status = status, .answer = std::move(line)};
}

} // namespace mangadex_dl::app
