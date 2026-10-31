// SPDX-License-Identifier: GPL-3.0-or-later
#include "io.hpp"

#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/version.hpp"
#include "recording_stdio_buf.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <optional>
#include <streambuf>
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

/// An owned copy of `name`'s value; nullopt when it is unset or empty.
[[nodiscard]] std::optional<std::string> env_copy(const char* name) {
    const std::optional<std::string_view> value = env_value(name);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    return std::string{*value};
}

/// Not noexcept: the three directory variables are copied into owned strings.
[[nodiscard]] EnvSnapshot read_env() {
    // Every variable treats "set but empty" like "unset".
    const std::string_view no_color = env_value("NO_COLOR").value_or(std::string_view{});
    const std::string_view force = env_value("CLICOLOR_FORCE").value_or(std::string_view{});
    const std::string_view term = env_value("TERM").value_or(std::string_view{});
    return EnvSnapshot{
        .no_color = !no_color.empty(),
        .clicolor_force = !force.empty() && force != "0",
        .term_dumb = term == "dumb",
        .home = env_copy("HOME"),
        .xdg_config_home = env_copy("XDG_CONFIG_HOME"),
        .xdg_state_home = env_copy("XDG_STATE_HOME"),
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

/// Clears errno before one stdio call and returns its previous value, so a failure reports its own
/// errno and never a stale one (glibc's first write to a stream probes isatty and leaves ENOTTY).
[[nodiscard]] int clear_errno() noexcept {
    const int saved = errno;
    errno = 0;
    return saved;
}

/// After that call: a failure is recorded (errno stays as the call set it); a success restores the
/// caller's errno, so the buffer is as transparent to errno as stdio_sync_filebuf.
void settle_errno(RecordingStdioBuf& buffer, bool succeeded, int saved) noexcept {
    if (succeeded) {
        errno = saved;
    } else {
        buffer.record(errno);
    }
}

/// Owns std::cout's RecordingStdioBuf: installs it on construction and, if std::cout still uses it,
/// puts the original buffer back before the RecordingStdioBuf is destroyed. Only ever a
/// function-local static (stdout_recorder()).
class StdoutRecorder {
public:
    // basic_ios::rdbuf(sb) never throws here: it clears the state to goodbit (sb is not null), and
    // a cleared state never raises ios_base::failure.
    StdoutRecorder() noexcept : original_{std::cout.rdbuf(&buffer_)} {}
    ~StdoutRecorder() {
        if (std::cout.rdbuf() == &buffer_) {
            std::cout.rdbuf(original_);
        }
    }
    StdoutRecorder(const StdoutRecorder&) = delete;
    StdoutRecorder& operator=(const StdoutRecorder&) = delete;
    StdoutRecorder(StdoutRecorder&&) = delete;
    StdoutRecorder& operator=(StdoutRecorder&&) = delete;

private:
    RecordingStdioBuf buffer_{stdout};   ///< declared first: original_'s initializer installs it
    std::streambuf* original_ = nullptr; ///< borrowed: libstdc++'s stdio_sync_filebuf for stdout
};

/// Installs std::cout's RecordingStdioBuf on the first call (startup, from process_io()).
///
/// Lifetime: the static is constructed during main(), so it is destroyed during static destruction
/// (after main returns, or by std::exit, which the program never calls), and before libstdc++'s
/// ios_base::Init, which was constructed earlier and flushes std::cout and std::cerr when it is
/// destroyed. ~StdoutRecorder puts the original buffer back first, so that flush never reaches a
/// destroyed buffer. Exits that run no static destructors (abort, a signal death, _exit) leave
/// std::cout on a buffer that is still alive. std::cerr's tie points at the std::cout object, which
/// the library never destroys, so it cannot dangle.
StdoutRecorder& stdout_recorder() noexcept {
    static StdoutRecorder recorder{};
    return recorder;
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

void RecordingStdioBuf::record(int error) noexcept {
    if (error == 0) {
        return;
    }
    int none = 0;
    static_cast<void>(first_errno_.compare_exchange_strong(none, error, std::memory_order_relaxed));
}

RecordingStdioBuf::int_type RecordingStdioBuf::overflow(int_type ch) {
    const int saved = clear_errno();
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
        const bool flushed = std::fflush(file_) == 0;
        settle_errno(*this, flushed, saved);
        return flushed ? traits_type::not_eof(ch) : traits_type::eof();
    }
    const int_type put = std::putc(ch, file_); // EOF (== traits_type::eof()) on failure
    settle_errno(*this, put != EOF, saved);
    return put;
}

std::streamsize RecordingStdioBuf::xsputn(const char_type* text, std::streamsize count) {
    if (count <= 0) {
        return 0;
    }
    const auto size = static_cast<std::size_t>(count);
    const int saved = clear_errno();
    const std::size_t written = std::fwrite(text, 1, size, file_);
    settle_errno(*this, written == size, saved);
    return static_cast<std::streamsize>(written); // written <= size, which came from a streamsize
}

int RecordingStdioBuf::sync() {
    const int saved = clear_errno();
    const int result = std::fflush(file_);
    settle_errno(*this, result == 0, saved);
    return result;
}

Io process_io() {
    static_cast<void>(stdout_recorder());
    return Io{
        .out = std::cout,
        .err = std::cerr,
        .stdin_tty = ::isatty(STDIN_FILENO) == 1,
        .stdout_tty = ::isatty(STDOUT_FILENO) == 1,
        .stderr_tty = ::isatty(STDERR_FILENO) == 1,
        .env = read_env(),
        .stdin_fd = STDIN_FILENO,
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
    // Installed by process_io(); null when std::cout still has the library's buffer (no
    // process_io() call), and then only this flush's errno is known.
    auto* const recorder = dynamic_cast<RecordingStdioBuf*>(std::cout.rdbuf());
    errno = 0; // isatty() and friends leave stale values (ENOTTY); only report what the flush sets
    bool failed = false;
    try {
        std::cout.flush(); // through the recorder, which records this flush's errno if it fails
    } catch (...) {
        failed = true; // only if someone enabled exceptions on std::cout
    }
    failed = std::cout.bad() || failed;
    if (std::fflush(stdout) != 0) {
        failed = true;
        if (recorder != nullptr) {
            recorder->record(errno); // ignored when an earlier failure is already recorded
        }
    }
    failed = (std::ferror(stdout) != 0) || failed;
    if (!failed) {
        return rc;
    }
    // The first failure's errno, wherever it happened. glibc drops stdout's buffer after a failed
    // write, so when the first failure came earlier (a flush forced by the tied std::cerr, or a
    // buffer overflow mid-run), the flushes above had nothing to write and set no errno.
    const int saved_errno = recorder != nullptr ? recorder->first_errno() : errno;
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
