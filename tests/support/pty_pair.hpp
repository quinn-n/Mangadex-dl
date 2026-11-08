#pragma once
// tests/support/pty_pair.hpp: a pseudo-terminal master/slave pair for in-process terminal tests
// (follows plan T7). Header-only, no Catch2: the caller decides between SKIP and FAIL.
//
//     mangadex_dl::test::PtyPair pty = mangadex_dl::test::open_pty_pair();
//     if (!pty.master.valid()) {
//         SKIP("no pseudo-terminal: " + pty.failure);  // posix_openpt itself failed
//     }
//     REQUIRE(pty.slave.valid());                      // any later step failing is a real failure
//
// The slave is the "terminal" the code under test reads; the test types into the master. Both
// descriptors are close-on-exec and opened with O_NOCTTY, so the test process never acquires a
// controlling terminal (writing ^C to the master therefore raises no signal here). The new pty
// has the kernel's default termios: canonical mode, ECHO and ISIG on.

#include "mangadex_dl/fs/output_dir.hpp"

#include <array>
#include <cerrno>
#include <string>
#include <system_error>

#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): posix_openpt, grantpt, unlockpt, ptsname_r
#include <stdlib.h>

namespace mangadex_dl::test {

struct PtyPair {
    UniqueFd master{};     ///< invalid when posix_openpt failed (no /dev/ptmx: SKIP)
    UniqueFd slave{};      ///< invalid when any later step failed (a real failure)
    std::string failure{}; ///< "<call>: <strerror>" of the step that failed; empty on success
};

namespace detail {

[[nodiscard]] inline std::string errno_failure(const char* call, int error) {
    return std::string{call} + ": " + std::error_code{error, std::generic_category()}.message();
}

} // namespace detail

/// posix_openpt → grantpt → unlockpt → ptsname_r → open(slave). Each step's failure is recorded
/// with its errno text; the descriptors opened so far stay owned by the result.
[[nodiscard]] inline PtyPair open_pty_pair() {
    PtyPair pair{};
    pair.master.reset(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (!pair.master.valid()) {
        pair.failure = detail::errno_failure("posix_openpt", errno);
        return pair;
    }
    if (::grantpt(pair.master.get()) != 0) {
        pair.failure = detail::errno_failure("grantpt", errno);
        return pair;
    }
    if (::unlockpt(pair.master.get()) != 0) {
        pair.failure = detail::errno_failure("unlockpt", errno);
        return pair;
    }
    std::array<char, 128> name{};
    const int named = ::ptsname_r(pair.master.get(), name.data(), name.size());
    if (named != 0) {
        pair.failure = detail::errno_failure("ptsname_r", named);
        return pair;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    pair.slave.reset(::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (!pair.slave.valid()) {
        pair.failure = detail::errno_failure("open(pty slave)", errno);
    }
    return pair;
}

} // namespace mangadex_dl::test
