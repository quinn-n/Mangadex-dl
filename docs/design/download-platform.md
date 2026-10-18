# Platform verification (local, 2026-10-18) — summary for the User Test Plan

Tier grid (only failure everywhere: `e2e.download.help`, golden intentionally unblessed pending cli-ux):

| target | configured | built | smoke-ran | contract-verified | sanitizer-clean |
|---|---|---|---|---|---|
| linux/x86_64 · clang 23.1.3 · dev (ASan+UBSan) | ✅ | ✅ | — | ✅* 512/513 | ✅* |
| linux/x86_64 · gcc 16.2.0 · dev (ASan+UBSan) | ✅ | ✅ | — | ✅* 512/513 | ✅* |
| linux/x86_64 · clang 23.1.3 · release | ✅ | ✅ | ✅ | ✅* 505/506 | n/a |
| linux/x86_64 · gcc 16.2.0 · release | ✅ | ✅ | ✅ | ✅* 505/506 | n/a |
| linux/x86_64 · clang 23.1.3 · tsan | ✅ | ✅ | — | ✅* 504/505 | TSan ✅* |
| docker glibc-floor image | R5 | R5 | R5 | R5 | R5 |
| mingw, aarch64 | R2 | R9 | R9 | R9 | R9 |
| macOS / Windows MSVC | T1 | T1 | T1 | T1 | T1 |

Host libcurl a release binary would load: `/lib/x86_64-linux-gnu/libcurl.so.4` (libcurl4t64 8.14.1, OpenSSL); fallback libcurl-gnutls.so.4 also present. Runtime gate against it not exercised (fixture mode only).

Not covered: container glibc-floor smoke (R5, no image named); valgrind (R2); mingw/aarch64 (R2/R9); CI for HEAD (R10/R8 — uncommitted); other libcurl builds (R5); macOS/MSVC (T1); MSan (T2); help layout judgment (T4).
Still human, unconditionally: real terminal rendering; real MangaDex data/network/TLS; Windows/macOS.
