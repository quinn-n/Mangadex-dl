// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/secret/process_guard_test.cpp: disable_core_dumps (follows plan T2). The guard is
// process-wide and cannot be undone (without CAP_SYS_RESOURCE, RLIMIT_CORE's hard limit only goes
// down), so it runs in a fork()ed child that reports its verdict through its exit status; the test
// process itself stays as it was.
#include "mangadex_dl/secret.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cerrno>

#include <linux/prctl.h> // PR_GET_DUMPABLE (<sys/prctl.h> only forwards it)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace md = mangadex_dl;

namespace {

// The child's verdicts (exit statuses).
inline constexpr int verdict_ok = 0;
/// Passed, but the soft core limit could not be raised first (hard limit 0, or a sandbox that
/// refuses or ignores setrlimit), so the RLIMIT_CORE transition was not shown, only that the limit
/// is {0,0} afterwards. The parent records it with a WARN.
inline constexpr int verdict_ok_without_core_transition = 3;
inline constexpr int verdict_setup_dumpable = 1;
inline constexpr int verdict_first_call = 10;
inline constexpr int verdict_still_dumpable = 20;
inline constexpr int verdict_core_limit = 30;
inline constexpr int verdict_second_call = 40;

/// PR_GET_DUMPABLE for the calling process (-1 on failure).
[[nodiscard]] int dumpable() noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): prctl is variadic in C
    return ::prctl(PR_GET_DUMPABLE, 0UL, 0UL, 0UL, 0UL);
}

/// Raises the soft core limit to the hard limit, so the guard has a transition to prove even on a
/// host with `ulimit -c 0` (soft). Whether that worked. A hard limit of 0 is left alone: raising
/// it would need CAP_SYS_RESOURCE, and an unprivileged contributor's machine must not go red for
/// that; neither may a sandbox that refuses or ignores setrlimit(RLIMIT_CORE).
[[nodiscard]] bool make_core_limit_nonzero() noexcept {
    struct rlimit limit{.rlim_cur = 0, .rlim_max = 0};
    if (::getrlimit(RLIMIT_CORE, &limit) != 0 || limit.rlim_max == 0) {
        return false;
    }
    limit.rlim_cur = limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &limit) != 0) {
        return false;
    }
    struct rlimit check{.rlim_cur = 0, .rlim_max = 0};
    return ::getrlimit(RLIMIT_CORE, &check) == 0 && check.rlim_cur != 0 && check.rlim_max != 0;
}

/// Runs in the child: only plain system calls, no Catch2, no allocation.
[[nodiscard]] int child_verdict() noexcept {
    // Always permitted; this alone gives the guard a dumpable transition to prove.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): prctl is variadic in C
    if (::prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL) != 0 || dumpable() != 1) {
        return verdict_setup_dumpable;
    }
    // Not mandatory (see make_core_limit_nonzero); {0,0} is still asserted after the guard.
    const bool core_raised = make_core_limit_nonzero();
    if (md::disable_core_dumps() != md::DumpGuard::not_dumpable) {
        return verdict_first_call;
    }
    if (dumpable() != 0) {
        return verdict_still_dumpable;
    }
    struct rlimit limit{.rlim_cur = 1, .rlim_max = 1};
    if (::getrlimit(RLIMIT_CORE, &limit) != 0 || limit.rlim_cur != 0 || limit.rlim_max != 0) {
        return verdict_core_limit;
    }
    // A second call is harmless: lowering a zero limit to zero is still permitted.
    if (md::disable_core_dumps() != md::DumpGuard::not_dumpable) {
        return verdict_second_call;
    }
    return core_raised ? verdict_ok : verdict_ok_without_core_transition;
}

/// waitpid, retried on EINTR. The pid, or -1 on any other failure.
[[nodiscard]] pid_t wait_for(pid_t child, int& status) noexcept {
    while (true) {
        const pid_t waited = ::waitpid(child, &status, 0);
        if (waited != -1 || errno != EINTR) {
            return waited;
        }
    }
}

} // namespace

TEST_CASE("dump guard: a forked child ends up not dumpable with a zero core limit",
          "[dump-guard]") {
    const int parent_dumpable = dumpable();
    REQUIRE(parent_dumpable >= 0);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::_exit(child_verdict());
    }
    int status = 0;
    REQUIRE(wait_for(child, status) == child);
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX <sys/wait.h> macro, defined in a bits/ header
    REQUIRE(WIFEXITED(status));
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX <sys/wait.h> macro, defined in a bits/ header
    const int verdict = WEXITSTATUS(status);
    // 1: the child could not make itself dumpable first; 10-40: the guard itself failed (see
    // child_verdict).
    CAPTURE(verdict);
    CHECK((verdict == verdict_ok || verdict == verdict_ok_without_core_transition));
    if (verdict == verdict_ok_without_core_transition) {
        WARN("RLIMIT_CORE could not be raised first on this host (hard limit 0, or setrlimit "
             "refused): only the dumpable transition was proven; the core limit was checked to "
             "be {0,0} after the guard");
    }

    // The guard ran only in the child.
    CHECK(dumpable() == parent_dumpable);
}
