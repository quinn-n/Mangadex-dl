// SPDX-License-Identifier: GPL-3.0-or-later
//
// The small local checks of follows/login/logout (follows plan T9): --password-stdin, the raw
// --credentials value, and the rule that a login only ever goes to MangaDex itself (or to a
// file:// fixture tree).
//
// read_password_stdin follows read_bounded_fd's semantics (the fstat type policy, EINTR retried,
// read to EOF, never more than cap + 1 bytes consumed), but reads into one buffer sized up front
// instead of growing a std::string: growth would leave copies of the password in freed memory.
// That buffer is wiped on every path, and only the first line leaves it, as a Secret.
//
// The read is interruptible (rev3 S18): the SIGINT/SIGTERM handlers use SA_RESTART, so a blocking
// read() on a stalled pipe would restart and never see Ctrl-C. Each step therefore poll()s for at
// most 100 ms (poll() is never restarted after a handler) and checks interrupt_requested() before
// every wait, as TerminalPrompter does (src/app/cmd_auth_prompt.cpp); read() is called only once
// poll() reports the descriptor ready, and EAGAIN (a non-blocking stdin) just waits again.
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// <sys/poll.h>, not <poll.h>: glibc declares poll/pollfd/POLL* there, and misc-include-cleaner
// rejects the <poll.h> wrapper (as in src/app/cmd_auth_prompt.cpp).
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

/// The longest wait before interrupt_requested() is checked again (rev3 S18: at least every
/// 100 ms).
inline constexpr int poll_interval_ms = 100;

[[nodiscard]] Error no_password() {
    return Error{.code = Errc::usage, .message = std::string{no_password_stdin_text}};
}

[[nodiscard]] Error interrupted() {
    return Error{.code = Errc::interrupted, .message = "interrupted"};
}

/// Errc::failure "cannot read stdin: <why>" (read_bounded_fd's wording).
[[nodiscard]] Error cannot_read_stdin(std::string_view why) {
    return Error{.code = Errc::failure, .message = std::format("cannot read stdin: {}", why)};
}

[[nodiscard]] Error cannot_read_stdin_errno(int err) {
    return cannot_read_stdin(std::system_category().message(err));
}

/// Wipes the borrowed buffer when the scope ends, on every path (an exception included).
class WipeOnExit {
public:
    explicit WipeOnExit(std::string& text MANGADEX_DL_LIFETIMEBOUND) noexcept : text_(&text) {}
    WipeOnExit(const WipeOnExit&) = delete;
    WipeOnExit& operator=(const WipeOnExit&) = delete;
    WipeOnExit(WipeOnExit&&) = delete;
    WipeOnExit& operator=(WipeOnExit&&) = delete;
    ~WipeOnExit() { secure_wipe(*text_); }

private:
    std::string* text_ = nullptr;
};

/// read_bounded_fd's type policy for a borrowed descriptor: a regular file, FIFO, terminal or
/// socket is read; a directory or anything else is an error.
[[nodiscard]] Result<void> check_type(mode_t mode) {
    if (S_ISREG(mode) || S_ISFIFO(mode) || S_ISCHR(mode) || S_ISSOCK(mode)) {
        return {};
    }
    if (S_ISDIR(mode)) {
        return std::unexpected(cannot_read_stdin("is a directory"));
    }
    return std::unexpected(cannot_read_stdin("not a regular file, pipe or terminal"));
}

/// Is `event` set in a pollfd's `revents`? (The flags are signed shorts and ints.)
[[nodiscard]] constexpr bool has_event(short revents, int event) noexcept {
    return (static_cast<unsigned>(revents) & static_cast<unsigned>(event)) != 0U;
}

/// Is `error` (an errno value) worth another wait?
[[nodiscard]] constexpr bool retryable(int error) noexcept {
    return error == EINTR || error == EAGAIN;
}

/// What one read_step did.
enum class Step {
    read,  ///< `count` bytes arrived
    end,   ///< end of input
    again, ///< nothing yet: check for an interrupt, then wait again
};

struct StepResult {
    Step step = Step::again;
    std::size_t count = 0; ///< bytes read (Step::read only)
};

/// One poll() of at most poll_interval_ms, then at most one read() into `unread` (never empty).
[[nodiscard]] Result<StepResult> read_step(int fd, std::span<char> unread) {
    pollfd entry{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&entry, 1, poll_interval_ms);
    if (ready < 0) {
        const int err = errno;
        if (retryable(err)) {
            return StepResult{};
        }
        return std::unexpected(cannot_read_stdin_errno(err));
    }
    if (ready == 0) {
        return StepResult{};
    }
    if (has_event(entry.revents, POLLNVAL)) {
        return std::unexpected(no_password()); // not an open descriptor: a closed stdin
    }
    // POLLIN, POLLHUP or POLLERR: read() tells which (buffered bytes come before the end).
    const ::ssize_t count = ::read(fd, unread.data(), unread.size());
    if (count < 0) {
        const int err = errno;
        if (err == EAGAIN && has_event(entry.revents, POLLERR)) {
            // poll() would report the error again at once: fail instead of spinning.
            return std::unexpected(cannot_read_stdin("the descriptor reported an error"));
        }
        if (retryable(err)) {
            return StepResult{};
        }
        if (err == EBADF) {
            return std::unexpected(no_password());
        }
        return std::unexpected(cannot_read_stdin_errno(err));
    }
    if (count == 0) {
        return StepResult{.step = Step::end};
    }
    return StepResult{.step = Step::read, .count = static_cast<std::size_t>(count)};
}

} // namespace

Result<Secret> read_password_stdin(int fd) {
    if (fd < 0) {
        return std::unexpected(no_password()); // a closed stdin
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        if (err == EBADF) {
            return std::unexpected(no_password()); // a closed stdin
        }
        return std::unexpected(cannot_read_stdin_errno(err));
    }
    if (Result<void> type = check_type(st.st_mode); !type) {
        return std::unexpected(std::move(type).error());
    }

    // One allocation of cap + 1 bytes, read into in place: it never grows, so no copy of the
    // password is left behind in freed memory. Wiped (over its whole capacity) on every path.
    constexpr std::size_t limit = max_password_stdin_bytes + 1;
    std::string buffer(limit, '\0');
    const WipeOnExit wipe{buffer};
    std::size_t got = 0;
    bool ended = false;
    while (!ended && got < limit) {
        if (interrupt_requested()) {
            return std::unexpected(interrupted());
        }
        // got <= max_password_stdin_bytes here, so at least one byte is asked for, and never more
        // than cap + 1 bytes are consumed in total.
        const Result<StepResult> step = read_step(fd, std::span<char>{buffer}.subspan(got));
        if (!step) {
            return std::unexpected(step.error());
        }
        switch (step->step) {
        case Step::read:
            got += step->count;
            break;
        case Step::end:
            ended = true;
            break;
        case Step::again:
            break;
        }
    }
    if (got == 0 || got > max_password_stdin_bytes) {
        return std::unexpected(no_password());
    }

    const std::string_view text{buffer.data(), got};
    if (text.contains('\0')) {
        return std::unexpected(no_password());
    }
    std::string_view line = text.substr(0, text.find('\n'));
    if (line.size() < text.size() && line.ends_with('\r')) {
        line.remove_suffix(1); // "\r\n"; a lone '\r' at the very end is part of the password
    }
    if (line.empty()) {
        return std::unexpected(no_password());
    }
    return Secret::copy_of(line);
}

Result<void> check_raw_auth_args(const RawAuthArgs& raw) {
    if (raw.credentials_given && raw.credentials.empty()) {
        return std::unexpected(
            Error{.code = Errc::usage, .message = "--credentials needs a file name"});
    }
    if (raw.switch_account && !raw.credentials_given) {
        return std::unexpected(
            Error{.code = Errc::usage, .message = "--switch-account needs --credentials"});
    }
    return {};
}

Result<void> check_auth_api_base(const ApiBase& base, std::string_view command) {
    switch (base.kind()) {
    case ApiBase::Kind::official:
    case ApiBase::Kind::fixture:
        return {};
    case ApiBase::Kind::https:
    case ApiBase::Kind::http_loopback:
        break;
    }
    return std::unexpected(Error{
        .code = Errc::usage,
        .message = std::format(
            "{} logs in only to MangaDex itself; --api-base must be the official API", command),
    });
}

} // namespace mangadex_dl
