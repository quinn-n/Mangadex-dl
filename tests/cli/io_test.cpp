// tests/cli/io_test.cpp: tier-2 tests for the process-I/O helpers in app/io.cpp that the
// installed-binary edge suite cannot reach. The E2E driver can hand the tool an *ignored* SIGPIPE
// (sigpipe-parent-ignores), but no portable shell can hand it a *blocked* one, so the unblock half
// of SIGPIPE policy A is exercised here, in-process. RecordingStdioBuf is driven over its own FILE*
// (never the real std::cout), one path per test; the installed binary covers finish_stdout itself
// (e2e.edge.dev-full-update, e2e.edge.dev-full-bulk). process_io() with fd 0 closed runs here
// with the test process's own fd 0 closed for the test and put back afterwards.
#include "app/io.hpp"
#include "app/recording_stdio_buf.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/secret.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction and pthread_sigmask are POSIX <signal.h>
#include <signal.h>
#include <unistd.h>

namespace {

/// True when `fd` is not an open descriptor: dup2(fd, fd) returns fd while it is open and fails
/// with EBADF once it is closed, with no side effect either way.
[[nodiscard]] bool is_closed(int fd) noexcept { return ::dup2(fd, fd) == -1 && errno == EBADF; }

/// Closes the test process's fd 0 for its lifetime and puts the original back on scope exit (a
/// failed REQUIRE included). The original is kept on a CLOEXEC duplicate numbered 3 or above, so
/// it never takes the place of a closed fd 1 or 2 and no child the test spawns inherits it. A stdin
/// that was already closed stays closed; one that cannot be duplicated is left open, and the test
/// then fails its is_closed REQUIRE instead of losing stdin.
class StdinClosedGuard {
public:
    StdinClosedGuard() noexcept
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
        : saved_{::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1)} {
        if (saved_ >= 0) {
            static_cast<void>(::close(STDIN_FILENO));
        }
    }
    ~StdinClosedGuard() {
        if (saved_ >= 0) {
            static_cast<void>(::dup2(saved_, STDIN_FILENO));
            static_cast<void>(::close(saved_));
        }
    }
    StdinClosedGuard(const StdinClosedGuard&) = delete;
    StdinClosedGuard& operator=(const StdinClosedGuard&) = delete;
    StdinClosedGuard(StdinClosedGuard&&) = delete;
    StdinClosedGuard& operator=(StdinClosedGuard&&) = delete;

private:
    int saved_ = -1;
};

/// Saves the calling thread's signal mask and the SIGPIPE disposition, and restores both on scope
/// exit, so a test that changes them leaves the test process as it found it.
class SignalStateGuard {
public:
    SignalStateGuard() noexcept {
        static_cast<void>(pthread_sigmask(SIG_SETMASK, nullptr, &mask_));
        static_cast<void>(sigaction(SIGPIPE, nullptr, &action_));
    }
    ~SignalStateGuard() {
        static_cast<void>(sigaction(SIGPIPE, &action_, nullptr));
        static_cast<void>(pthread_sigmask(SIG_SETMASK, &mask_, nullptr));
    }
    SignalStateGuard(const SignalStateGuard&) = delete;
    SignalStateGuard& operator=(const SignalStateGuard&) = delete;
    SignalStateGuard(SignalStateGuard&&) = delete;
    SignalStateGuard& operator=(SignalStateGuard&&) = delete;

private:
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t mask_{};
    struct sigaction action_{};
};

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/// A fresh write stream on `path`. /dev/full and /dev/null are not terminals, so stdio buffers the
/// stream fully, as it does stdout redirected to them.
[[nodiscard]] FilePtr open_for_write(const char* path) {
    FilePtr file{std::fopen(path, "w")};
    REQUIRE(file != nullptr);
    return file;
}

/// Well over any stdio buffer (st_blksize of /dev/full is 4 KiB).
inline constexpr std::size_t past_the_buffer = 64UL * 1024UL;

} // namespace

TEST_CASE("RecordingStdioBuf keeps the errno of a flush forced by a tied stream",
          "[io][write-error]") {
    const FilePtr file = open_for_write("/dev/full");
    mangadex_dl::app::RecordingStdioBuf buffer{file.get()};
    std::ostream out{&buffer};
    std::ostringstream err;
    err.tie(&out); // as std::cerr is tied to std::cout

    out << "data\n"; // fits in the stdio buffer: nothing is written yet
    CHECK(out.good());
    CHECK(buffer.first_errno() == 0);

    err << "note\n"; // the sentry flushes `out` first, and that write fails
    CHECK(out.bad());
    CHECK(buffer.first_errno() == ENOSPC);
    CHECK(err.str() == "note\n");
}

TEST_CASE("RecordingStdioBuf keeps the errno of a write that overflows the stdio buffer",
          "[io][write-error]") {
    const FilePtr file = open_for_write("/dev/full");
    mangadex_dl::app::RecordingStdioBuf buffer{file.get()};
    std::ostream out{&buffer};

    const std::string bulk(past_the_buffer, 'x');
    out << bulk; // xsputn -> fwrite: the buffer fills, the write fails, fwrite comes up short
    CHECK(out.bad());
    CHECK(buffer.first_errno() == ENOSPC);
}

TEST_CASE("RecordingStdioBuf keeps the errno of a failed single-character put",
          "[io][write-error]") {
    const FilePtr file = open_for_write("/dev/full");
    mangadex_dl::app::RecordingStdioBuf buffer{file.get()};
    std::ostream out{&buffer};

    // No put area of its own, so every put() reaches overflow() -> putc; one of them fills the
    // stdio buffer and fails. put() does nothing once badbit is set.
    for (std::size_t i = 0; i < past_the_buffer; ++i) {
        out.put('x');
    }
    CHECK(out.bad());
    CHECK(buffer.first_errno() == ENOSPC);
}

TEST_CASE("RecordingStdioBuf reports the first error only", "[io][write-error]") {
    const FilePtr file = open_for_write("/dev/full");
    mangadex_dl::app::RecordingStdioBuf buffer{file.get()};
    std::ostream out{&buffer};

    buffer.record(0); // never an error
    CHECK(buffer.first_errno() == 0);
    buffer.record(EIO);
    out << "data" << std::flush; // fails with ENOSPC, after the first error
    CHECK(out.bad());
    CHECK(buffer.first_errno() == EIO);
}

TEST_CASE("RecordingStdioBuf passes output through and leaves errno alone on success",
          "[io][write-error]") {
    const FilePtr file = open_for_write("/dev/null");
    mangadex_dl::app::RecordingStdioBuf buffer{file.get()};
    std::ostream out{&buffer};

    const std::string bulk(past_the_buffer, 'x');
    errno = EINTR; // after the allocation: a caller's errno must survive successful writes/flushes
    out << "line\n" << bulk;
    out.put('x');
    out.flush();
    CHECK(out.good());
    CHECK(errno == EINTR);
    CHECK(buffer.first_errno() == 0);
    CHECK(std::ferror(file.get()) == 0);
}

TEST_CASE("restore_default_sigpipe unblocks a blocked, ignored SIGPIPE and resets it to default",
          "[io][sigpipe]") {
    const SignalStateGuard guard;

    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t pipe_only{};
    REQUIRE(sigemptyset(&pipe_only) == 0);
    REQUIRE(sigaddset(&pipe_only, SIGPIPE) == 0);
    REQUIRE(pthread_sigmask(SIG_BLOCK, &pipe_only, nullptr) == 0);
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    REQUIRE(sigemptyset(&ignore.sa_mask) == 0);
    REQUIRE(sigaction(SIGPIPE, &ignore, nullptr) == 0);

    mangadex_dl::app::restore_default_sigpipe();

    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t blocked_now{};
    REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &blocked_now) == 0);
    CHECK(sigismember(&blocked_now, SIGPIPE) == 0);
    struct sigaction action_now{};
    REQUIRE(sigaction(SIGPIPE, nullptr, &action_now) == 0);
    CHECK(action_now.sa_handler == SIG_DFL);
}

TEST_CASE("process_io: a closed fd 0 is stdin_fd -1, even once a later open reuses descriptor 0",
          "[io][stdin]") {
    const StdinClosedGuard closed; // declared first: destroyed after `reused` below
    REQUIRE(is_closed(STDIN_FILENO));

    const mangadex_dl::app::Io io = mangadex_dl::app::process_io();
    CHECK(io.stdin_fd == -1);
    CHECK_FALSE(io.stdin_tty);

    // What the tool does next (the fixture transport's root, a lock file, a socket): the lowest
    // free descriptor is 0. Here it is a pipe of the tool's own that holds a password-shaped line;
    // "stdin" must still be the closed-stdin usage error, never these bytes.
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    const mangadex_dl::UniqueFd reused{ends[0]};
    mangadex_dl::UniqueFd writer{ends[1]};
    REQUIRE(reused.get() == STDIN_FILENO);
    const std::string_view line = "fixture-password-not-a-secret\n";
    REQUIRE(::write(writer.get(), line.data(), line.size()) == std::ssize(line));
    writer.reset(); // EOF after the line
    const mangadex_dl::Result<mangadex_dl::Secret> password =
        mangadex_dl::read_password_stdin(io.stdin_fd);
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == mangadex_dl::Errc::usage);
    CHECK(password.error().message == "--password-stdin was given, but stdin held no password");
}
