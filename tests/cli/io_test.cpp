// tests/cli/io_test.cpp: tier-2 tests for the process-I/O helpers in app/io.cpp that the
// installed-binary edge suite cannot reach. The E2E driver can hand the tool an *ignored* SIGPIPE
// (sigpipe-parent-ignores), but no portable shell can hand it a *blocked* one, so the unblock half
// of SIGPIPE policy A is exercised here, in-process.
#include "app/io.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace

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
