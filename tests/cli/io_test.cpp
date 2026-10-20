// tests/cli/io_test.cpp: tier-2 tests for the process-I/O helpers in app/io.cpp that the
// installed-binary edge suite cannot reach. The E2E driver can hand the tool an *ignored* SIGPIPE
// (sigpipe-parent-ignores), but no portable shell can hand it a *blocked* one, so the unblock half
// of SIGPIPE policy A is exercised here, in-process. RecordingStdioBuf is driven over its own FILE*
// (never the real std::cout), one path per test; the installed binary covers finish_stdout itself
// (e2e.edge.dev-full-update, e2e.edge.dev-full-bulk).
#include "app/io.hpp"
#include "app/recording_stdio_buf.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction and pthread_sigmask are POSIX <signal.h>
#include <signal.h>

namespace {

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
