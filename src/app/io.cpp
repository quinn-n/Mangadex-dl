// SPDX-License-Identifier: GPL-3.0-or-later
#include "io.hpp"

#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/version.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction and pthread_sigmask are POSIX <signal.h>
#include <signal.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): strerror_r is POSIX, declared only in <string.h>
#include <string.h>
#include <unistd.h>

namespace mangadex_dl::app {
namespace {

void write_stderr(std::string_view text) noexcept {
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), stderr));
}

/// The program's only getenv call; nullopt when `name` is unset. The view borrows from the process
/// environment block, which stays valid because the program never calls setenv/putenv/unsetenv.
/// glibc has no thread-safe getenv: it is safe only while no thread writes the environment. It is
/// reached only through process_io(), which main() calls once at startup before any thread exists.
[[nodiscard]] std::optional<std::string_view> env_value(const char* name) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): startup-only read before any thread; env never mutated
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view{value};
}

[[nodiscard]] EnvSnapshot read_env() noexcept {
    // All three variables treat "set but empty" like "unset".
    const std::string_view no_color = env_value("NO_COLOR").value_or(std::string_view{});
    const std::string_view force = env_value("CLICOLOR_FORCE").value_or(std::string_view{});
    const std::string_view term = env_value("TERM").value_or(std::string_view{});
    return EnvSnapshot{
        .no_color = !no_color.empty(),
        .clicolor_force = !force.empty() && force != "0",
        .term_dumb = term == "dumb",
    };
}

// strerror_r has two signatures and glibc picks one by feature macros (C++ compilers predefine
// _GNU_SOURCE, so it is the GNU one here). These overloads normalize the result either way;
// whichever flavour the headers declare, the other overload is unused, hence [[maybe_unused]].

/// GNU strerror_r: returns the message, which may point into `buffer` or at a static string.
[[maybe_unused]] [[nodiscard]] const char* strerror_r_text(const char* message,
                                                           const char* /*buffer*/) noexcept {
    return message;
}

/// XSI strerror_r: returns 0 after filling `buffer`, or an error number (nothing usable).
[[maybe_unused]] [[nodiscard]] const char* strerror_r_text(int status,
                                                           const char* buffer) noexcept {
    return status == 0 ? buffer : nullptr;
}

[[nodiscard]] constexpr int sgr_code(Style style) noexcept {
    switch (style) {
    case Style::bold:
        return 1;
    case Style::dim:
        return 2;
    case Style::red:
        return 31;
    case Style::green:
        return 32;
    case Style::yellow:
        return 33;
    }
    return 0;
}

} // namespace

Io process_io() {
    return Io{
        .out = std::cout,
        .err = std::cerr,
        .stdin_tty = ::isatty(STDIN_FILENO) == 1,
        .stdout_tty = ::isatty(STDOUT_FILENO) == 1,
        .stderr_tty = ::isatty(STDERR_FILENO) == 1,
        .env = read_env(),
    };
}

void restore_default_sigpipe() noexcept {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    static_cast<void>(sigemptyset(&action.sa_mask));
    static_cast<void>(sigaction(SIGPIPE, &action, nullptr));

    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t pipe_only{};
    static_cast<void>(sigemptyset(&pipe_only));
    static_cast<void>(sigaddset(&pipe_only, SIGPIPE));
    static_cast<void>(pthread_sigmask(SIG_UNBLOCK, &pipe_only, nullptr));
}

void last_resort(const char* message) noexcept {
    write_stderr(tool_name);
    write_stderr(": ");
    static_cast<void>(std::fputs(message != nullptr ? message : "unknown error", stderr));
    write_stderr("\n");
}

int finish_stdout(int rc) noexcept {
    errno = 0; // isatty() and friends leave stale values (ENOTTY); only report what the flush sets
    bool failed = false;
    try {
        std::cout.flush();
    } catch (...) {
        failed = true; // only if someone enabled exceptions on std::cout
    }
    failed = std::cout.bad() || failed;
    failed = (std::fflush(stdout) != 0) || failed;
    failed = (std::ferror(stdout) != 0) || failed;
    if (!failed) {
        return rc;
    }
    const int saved_errno = errno;
    const char* reason = nullptr;
    std::array<char, 256> buffer{}; // stack only: this noexcept path must not allocate
    if (saved_errno != 0) {
        reason =
            strerror_r_text(strerror_r(saved_errno, buffer.data(), buffer.size()), buffer.data());
    }
    write_stderr(tool_name);
    write_stderr(": write error: ");
    if (reason != nullptr) {
        static_cast<void>(std::fputs(reason, stderr));
    } else {
        write_stderr("output error"); // badbit set by an earlier write; the errno is gone
    }
    write_stderr("\n");
    return rc == exit_code::success ? exit_code::failure : rc;
}

bool resolve_color(ColorMode mode, const EnvSnapshot& env, bool is_tty) noexcept {
    switch (mode) {
    case ColorMode::always:
        return true;
    case ColorMode::never:
        return false;
    case ColorMode::automatic:
        break;
    }
    if (env.no_color) {
        return false;
    }
    if (env.clicolor_force) {
        return true;
    }
    if (env.term_dumb) {
        return false;
    }
    return is_tty;
}

std::string paint(std::string_view text, Style style, bool enabled) {
    if (!enabled) {
        return std::string{text};
    }
    return std::format("\x1b[{}m{}\x1b[0m", sgr_code(style), text);
}

} // namespace mangadex_dl::app
