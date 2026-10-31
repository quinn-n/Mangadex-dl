// SPDX-License-Identifier: GPL-3.0-or-later
//
// The core-dump guard (follows plan T2): keeps passwords, client secrets and tokens out of core
// files and out of same-user /proc/<pid>/mem readers. Called once, from the main thread, before
// any secret is read (by follows, login and logout).
//
// Fact-check V20/V21/V22: with both settings applied, ASan, LSan and UBSan still report (and exit
// 86) in the dev preset, TSan still reports races, and /proc/self/fd publishing (linkat) keeps
// working, as root and as an unprivileged user, on the main thread and on workers.
#include "mangadex_dl/secret.hpp"

#include <linux/prctl.h> // PR_SET_DUMPABLE (<sys/prctl.h> only forwards it)
#include <sys/prctl.h>
#include <sys/resource.h>

namespace mangadex_dl {

DumpGuard disable_core_dumps() noexcept {
    // RLIMIT_CORE first: lowering both limits to 0 is always permitted, and it is the only guard
    // left should PR_SET_DUMPABLE be refused (a kernel or LSM policy, plan "Refactor triggers").
    const struct rlimit no_core{.rlim_cur = 0, .rlim_max = 0};
    const bool core_limited = ::setrlimit(RLIMIT_CORE, &no_core) == 0;
    // prctl reads its arguments as unsigned long, so pass them as such.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): prctl is variadic in C
    const bool not_dumpable = ::prctl(PR_SET_DUMPABLE, 0UL, 0UL, 0UL, 0UL) == 0;
    // Lowering RLIMIT_CORE to {0,0} cannot fail for a valid resource; should it anyway, a process
    // that is not dumpable writes no core file either, so the stronger guard is what is reported.
    if (not_dumpable) {
        return DumpGuard::not_dumpable;
    }
    if (core_limited) {
        return DumpGuard::no_core_only;
    }
    return DumpGuard::none;
}

} // namespace mangadex_dl
