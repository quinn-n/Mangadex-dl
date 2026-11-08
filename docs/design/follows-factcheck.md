# Fact-check: follows / login / logout plan

Date: 2026-10-28. Four fact-checkers, one per cluster group, against `docs/design/follows-plan.md` (draft) and `docs/design/follows-spec.md` (rev2). No credentials were sent and no authenticated endpoint or auth POST was made; only public documents, the installed pixi env, local loopback servers and `cxx-probe` probes. Probe scratch is under `.platform-runs/` (not committed).

**Result: no refutation blocks the plan.** Two real gaps (CLI11 value echo, V11a / short-option clusters; the `BatchSource::mdlist ==` site, V27) and several corrections are folded into plan rev1.

## Clusters A, F — summary (2026-10-28). No refutation.

- V1 CONFIRMED discovery: token .../protocol/openid-connect/token, revoke .../revoke, client_secret_post for token AND revoke, password+refresh_token grants. Spec issue 2 (fixed URLs) fine.
- V2 PARTIAL: wrong-password status varies by Keycloak version (401 up to 26.5.0, 400 on main) -> classify on `error` only: invalid_grant on password grant = 9, on refresh = 8; invalid_client/unauthorized_client = 9. Fixtures: 401 wrong password + one 400 variant; 400 refresh/unauthorized_client. Disabled account = 400 invalid_grant "Account disabled" (lockout likely invalid_grant). Pending client wording: human AC42/43.
- V3 CONFIRMED + caveat: Keycloak revoke returns HTTP 200 with JSON error for invalid token -> logout "success" = any 200; only 4xx/5xx = failed revoke.
- V4 CONFIRMED api.yaml: follows limit 1-100, MangaList total; /manga/status {result, statuses{id: enum6}}, optional ?status filter, no pagination; Bearer via global security.
- V5 PARTIAL: 10000 window documented for collections generally, not follows by name -> wording "documented for collection endpoints; refused rather than truncated"; human check.
- V6 PARTIAL: only "15 minutes" documented; keep defensive defaults; rotation human-observable.
- V23: duplicate JSON keys -> silent, LAST wins (DOM can't surface) -> drop "malformed if surfaced" (spec D8).
- V24 CONFIRMED: 100k entries = 5.07 MB, 200k separators (2/entry, not 3), 246 ms ASan; [] = empty array. Fix wording; test 100001 entries (entry cap, not separator cap).
- V25 CONFIRMED wipe in place; prefer get_ptr<std::string*>; never copy DOM; wipe body separately; keys const (non-secret).
- V26 CONFIRMED ordered_json ordering.
- Probe note: absolute include fails for nlohmann; amalgam at .platform-runs/20261028T162703Z-factcheck-AF/nlohmann_amalgam.hpp or use a tier-2 test.

## Cluster B (CLI11) — summary of fact-checker report (2026-10-28)

- V7a-d CONFIRMED. ExtrasError what() echoes extras verbatim -> build value-free text from remaining(true).
- V8 CONFIRMED (--group pattern for --status). V9a,b CONFIRMED.
- V9c REFUTED: logout help lists inherited globals (-h, --version, --json, -q, -v, --color); fix T19 golden expectation.
- V10: std::optional collapses --credentials '' to absent -> use std::string + count() (T16/T9; plan lines 907 vs 1358 inconsistent).
- V11a CONFIRMED LEAK: ConversionError "Could not convert: --no-input = x" echoes value for ANY flag given =value (--dry-run=hunter2 etc). Fix opt2: map ConversionError on follows/login/logout to "<flag> takes no value" (gate nod); opt1 disable_flag_override on the two auth flags.
- V11b safe; V11c echoes bare words (covered by issue 5 count); V11d ValidationErrors echo values of --jobs/--layout/--color (non-secret; accept? gate).
- Caveat 1: short clusters -phunter2 / -qphunter2 echoed whole -> treat single-dash words >2 chars as secret-shaped; prefix match.
- Caveat 2: `follows --password-stdin hunter2` -> DIR='hunter2' (mistaken password becomes DIR). Designer/gate: guard? no-leak test should cover.
Probes: .platform-runs/20261028T000000Z-factcheck/probe_clusterB*.cpp

## Clusters C, D, H — summary (2026-10-28). All hold; corrections only.

- V12 CONFIRMED on loopback: Authorization header sent once, not re-sent with FOLLOWLOCATION 0, nothing printed w/o VERBOSE (VERBOSE prints Bearer -> must stay off). T4: move setopt_slist(HTTPHEADER) out of the POST-only case; header std::string temp + libcurl internal copies -> wipe is best effort.
- V13 CONFIRMED: POSTFIELDS+POSTFIELDSIZE_LARGE exact bytes; pointer kept until curl_easy_reset (not just perform); order irrelevant (keep SIZE first); reset forgets pointer. setopt_str works but its "libcurl copies" doc is wrong for POSTFIELDS -> add a borrowed-data setter or call-site comment.
- V14 CONFIRMED: slist data is a copy, wipe before free_all legal; wipe must run after easy_reset (keep headers/lease declaration order, curl_transport.cpp:424-426).
- V15 CONFIRMED: all functions present; max GLIBC 2.25 for new syms (explicit_bzero / __explicit_bzero_chk 2.25); release already at 2.28 (renameat2). V15c REFUTED: memset_explicit absent in 2.28 -> fallback = volatile loop/asm barrier (moot).
- V16 CONFIRMED flock per open file description; dup shares lock; release on last close.
- V17 CONFIRMED fsync dir fd (O_PATH -> EBADF); symlink + O_DIRECTORY|O_NOFOLLOW -> ENOTDIR; renameat replaces.
- V18a-d CONFIRMED locally (ICANON cap = 4095 data + \n; poll POLLIN|POLLHUP after master close). V18e PARTIAL: CI ubuntu-26.04 unverifiable here; TTY E2E skip 77 w/o `script`; T7 pty tests must skip loudly / fail when posix_openpt unavailable (decide).
- V19 CONFIRMED (both compilers): resize<=cap no realloc; SSO cap 15; moved-from SSO keeps bytes 1..n-1 (only byte 0 cleared); wipe sequence verified at -O1 and release.
Probes: .platform-runs/20261028T162458Z-factcheck/

## Clusters E, G — summary (2026-10-28). Ran as root AND as uid 65534 (setpriv, no caps).

- V20a/b CONFIRMED: PR_SET_DUMPABLE 0 + RLIMIT_CORE 0 -> LSan leak, ASan UAF/SEGV, UBSan still report, exit 86 (LSan StopTheWorld temporarily re-enables dumpable). V20c CONFIRMED with caveat: TSan races still reported; tsan preset has no leak detection -> word controls "ASan leak / TSan race".
- V21 CONFIRMED root+nobody, main+worker threads: /proc/self/fd linkat publish still works after the guard (kernel proc_fd_permission same_thread_group). Residual: CI kernel/LSM.
- V22 CONFIRMED.
- V27 PARTIAL: switch inventory exact, -Wswitch -Werror active on both compilers. GAP: src/app/cmd_batch_jobs.cpp:166 `.claimed = source == BatchSource::mdlist ? registry_ : nullptr` is an ==, not a switch -> follows would silently lose --overwrite same-run protection. Fix in T1: `!= csv` (or explicit), + test.
- V28 CONFIRMED fixture mapping (api/manga/status.json; .../offset-k.json); POST 404 test unaffected.
- V29 CONFIRMED (cite tests/CMakeLists.txt:25-56). ofstream = 0666&~umask; openat+fchmod exact. fixture_gen links only nlohmann -> raw POSIX helper in fixture_gen/mdx_fixture. PRE_RUN: stdin /dev/null, no TTY -> seeding login must use --credentials.
- V30, V31, V33, V34 CONFIRMED (ctest names unit.<Catch2 name>, case-sensitive -R).
- V32 CONFIRMED: InstantClock -> lock wait times out instantly at E2E; "wait then succeed" stays unit tier.
- Spec issues 1, 10, 11 CONFIRMED. T11 note: perform's `const HttpRequest request` must be non-const to wipe the auth copy.
- Citation fixes: src/core/interrupt.cpp:124, src/core/net/clock.cpp, src/core/fs/output_dir.cpp:243/246/261.
