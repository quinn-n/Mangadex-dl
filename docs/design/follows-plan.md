# Implementation plan: `mangadex-dl follows` / `login` / `logout` (download the titles you follow)

Status: **rev1, APPROVED at the ⏸ plan gate (user, 2026-10-30), after fact-check** ([`follows-factcheck.md`](follows-factcheck.md): no refutation blocks the plan). This revision folds in the gate answers and every fact-check correction. Changes are marked **(rev1)**.

## Changelog

**rev1 (2026-10-30)**

**Gate decisions**
- Spec issues 1–13 are all accepted as recommended.
- **Echo policy (issue 5) = hide every value** on `follows`, `login` and `logout`. The concrete rules are in T19 and "Spec text to change".
- **No guard** against a mistyped password becoming DIR (`follows --password-stdin hunter2`). The no-leak tests prove it is never echoed by an auth, usage or collection error.

**Plan gaps the fact-check found**
- **V27 gap fixed (T1, T14):** `cmd_batch_jobs.cpp:166` decides the registry with `source == BatchSource::mdlist`, which `-Wswitch` cannot see. Without a fix, `follows` would silently lose mdlist's same-run `--overwrite` protection. It becomes a `switch` helper, and T14 adds a test.
- **V10 (T1, T9, T14, T16):** `--credentials` binds to a `std::string` plus `Option::count()`. `--credentials ''` is exit 2.
- **V9c (T19):** `logout --help` lists the inherited global flags.

**Transports and the session store**
- **T4 (V12–V14):**
  - `HTTPHEADER` is set for both methods;
  - the POST body is borrowed through `POSTFIELDS`, with `POSTFIELDSIZE_LARGE` set first;
  - the body must outlive `curl_easy_reset`;
  - a borrowed-data setter is added;
  - the declaration order is pinned so the slist wipe runs after `easy_reset`;
  - `CURLOPT_VERBOSE` stays off.
- **T5 (V16, V17):** `flock` is per open file description and the lock fd is never `dup`ed. A symlinked final directory gives `ENOTDIR`.

**Token layer and fixture auth server**
- **T8, T6 (V2):** classification keys on the `error` code only. The fixtures use 401 for a wrong password plus one 400 variant, and 400 for refresh failures and `unauthorized_client`.
- **T12, T6 (V3):** a revoke succeeds on any HTTP 200.

**Prompter and secret memory**
- **T7 (V18):**
  - the ICANON line limit is 4095 data bytes plus the newline;
  - after the master closes, `poll` reports `POLLIN|POLLHUP`;
  - a pty test SKIPs loudly when `posix_openpt` is unavailable;
  - too-long and EOF are tested on a pipe.
- **T2 (V15c, V19c):** the wording is fixed and the fallback is a volatile loop.
- **T8, T9 (V25):** strings are wiped through `get_ptr<std::string*>()`, the DOM is never copied, and the body is wiped separately.
- **T11:** `perform`'s request is non-const so its authorization copy can be wiped.

**Parsers and caps**
- **T10 (V23, V24):** duplicate JSON keys are silent and the last wins. A status-map entry costs 2 separators. A 100 001-entry test is added.
- **T13 (V5):** the cap wording is fixed.

**Fixtures and test infrastructure**
- **T15 (V29):** `fixture_gen` links only nlohmann, so the fixture writer uses raw POSIX helpers. Seeding `login` runs use `--credentials`.
- **V32:** the "wait, then succeed" lock case stays at the unit tier.
- **V34:** test-name prefixes are fixed per task and the TSan filters use them (ctest names are case-sensitive).
- **V20c:** the sanitizer-control wording is fixed.

**New items to check**
- **New [VERIFY] V35–V37:** the CLI11 ConversionError mapping, `Option::count()`, and Catch2 `SKIP` reaching ctest. They are small, re-check only.

**Housekeeping**
- Citation fixes.
- The probe note for the nlohmann amalgam.
- A new section, "Spec text to change (follows-spec rev3)".

**rev0 (2026-10-28):** the draft, which went to the fact-check and the gate.

---

Design: `/home/user/Mangadex-dl/docs/design/follows-spec.md` (rev2; rev3 edits listed below). The gate decisions are binding:
- Q1 = C: TTY prompt, an optional 0600 credentials file and `--password-stdin`. Env vars are deferred.
- Q2 = B: the session is persisted, the password never is.
- Q3 = B: top-level `follows`, `login` and `logout`.
- Q4 + QA1 = b: follows by default; `--status S` narrows them with one `GET /manga/status`.
- Q5: exit 8 `login_required` and exit 9 `auth_rejected`.
- Q6: flat by default; `--dir-per-title` opts in.

Precedent: `docs/design/list-plan.md` (mdlist).

**What the tree holds** (2026-10-28, confirmed by the fact-check):
- **`pixi list`:** cli11 2.7.2, nlohmann_json 3.12.0, catch2 3.16.1, cmake 4.4.4, ninja 1.13.2, clang 23.1.3, gxx 16.2.0, libstdcxx 16.2.0, libcurl 8.23.0 (headers only; the runtime is dlopen'ed from the host, floor 7.85.0), sysroot 2.28, kernel-headers 4.18. **No new dependency.**
- **Globs:**
  - `CMakeLists.txt:90` is a recursive `src/core/*.cpp`.
  - `CMakeLists.txt:108` is a flat `src/app/cmd_*.cpp`, so **every new app source is named `cmd_*.cpp`**.
  - `tests/unit` is recursive; `tests/cli` is flat.
  - `tests/CMakeLists.txt:57` globs `e2e/cases/*.cmake`.
- **No task edits** a CMake file, a preset, `pixi.toml`/`pixi.lock`, `scripts/*`, `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake` or a workflow.

---

## Spec issues: gate outcome (rev1: all accepted)

1. **`paginate` reuse.**
   - `ApiClient::State::paginate` is private and maps non-2xx to `Errc::network`, so it has no hook for a 401.
   - `ApiClient::get_authorized` returns the final response unmapped.
   - `fetch_follows` (T13) applies paginate's stepping rules itself: step by the items received, stop on an empty page or `offset >= total`, refuse anything past the 10 000 window. Both copies are tested.
2. **Fixed URLs.**
   - Token: `<issuer>/protocol/openid-connect/token`. Revoke: `<issuer>/protocol/openid-connect/revoke`.
   - There is no runtime discovery GET. V1 confirmed both URLs and `client_secret_post` for both endpoints.
3. **The token-request cap counts logical grant requests.**
   - At most 3 password or refresh grants per run.
   - Each grant may retry transport errors, 5xx and 429 within `RetryConfig` (at most 4 attempts). Any other 4xx is answered once.
   - The revoke is not counted.
4. **The `--title` message:** `follows has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title ...'`.
5. **Echo policy (rev1, gate): hide every value** on `follows`, `login` and `logout`. Exact rules in T19:
   - a flag given `=value` gets the fixed `<flag> takes no value`;
   - unknown bare words are only counted;
   - option-looking extras are echoed cut at `=`, through `terminal_safe`;
   - **single-dash words longer than 2 characters are counted, never echoed** (the gate let me pick; I picked "just count");
   - the secret-shaped prefixes `--password*`, `--client-secret*` and `-p*` get the fixed password message;
   - ValidationErrors of non-secret options (`--jobs`, `--layout`, `--color`) keep CLI11's stock text;
   - `disable_flag_override()` is **not** added: the ConversionError mapping covers every flag uniformly (V35).
6. **`--status` binding.** It uses the `--group` pattern (`expected(1)`, `allow_extra_args(false)`, `TakeAll`, `type_name("WORD,...")`), and `StatusFilter::parse` splits the commas. V8 confirmed.
7. **mdlist footer and README.** The mdlist line becomes `To download the titles you follow, see 'mangadex-dl follows --help'.` (T19). The README follows in T21.
8. **A prompt answer over 4096 bytes:** exit 8, `the <field> you typed is longer than 4096 bytes`.
   - **(rev1)** A canonical-mode (ICANON) terminal caps a line at 4095 data bytes plus the newline (V18). On a real TTY the limit is therefore reached only in non-canonical mode, and it is tested through a pipe.
9. **`logout` with a damaged session or directory.**
   - An unreadable or malformed session is removed without a revoke, with the warning `could not end the session at MangaDex (the saved login is unreadable); the saved login was removed anyway`, exit 0.
   - An insecure state directory → exit 1, `could not remove the saved login at <path>: <why>`, and nothing is touched.
10. **Generated credentials (spec C9 decided).** `fixture_gen` writes the 0600, 0644 and password files under `{{FIXTURES}}/<tree>/creds/`. Nothing credential-shaped is committed, and `CliE2E.cmake` stays untouched.
11. **`Endpoints` is unchanged.** A free `auth_endpoints_for(const ApiBase&) -> std::optional<AuthEndpoints>` handles official or fixture bases only (V33).
12. **The login/logout help footers** are drafted in T17. cli-ux owns the wording.
13. **Planner placements:**
    - a core `Prompter` interface plus the app's `TerminalPrompter`;
    - a byte-level session store;
    - `StderrTrace` extracted into `cmd_trace.{hpp,cpp}`;
    - a POST trace line formatted in `auth_client.cpp` (8 duplicated lines; a later refactor).

---

## Spec text to change (follows-spec rev3, for the orchestrator to apply)

| # | Spec section | Change |
|---|---|---|
| S1 | Status filter → Parsing (`parse_status_map`), "Plus a separator cap of 1 000 000" | Reword to "**two** per entry at the entry cap (200 000), plus headroom; 1 000 000". (V24) |
| S2 | Status filter → Keys, "A duplicate JSON key is malformed, if the parser surfaces it (VERIFY D8)" | Replace with "A duplicate JSON key is not detected: the parser keeps the **last** value (V23). Two keys that canonicalise to the same id are kept once, with the first value." Close D8 in the VERIFY list. |
| S3 | Summary ("at most 10 000"), Behaviour phase 7.1, Large input, the All-outcomes row | Cap wording: "at most 10 000 follows, the window MangaDex documents for collection endpoints; more are refused, never truncated". Message: `you follow more than 10000 titles on MangaDex; refusing to truncate the list`. (V5) |
| S4 | `logout` behaviour, step 2 | The revocation URL is the fixed `<issuer>/protocol/openid-connect/revoke`, with no discovery request. **Success is any HTTP 200, whatever the body**: Keycloak answers 200, even with a JSON error, for an invalid token. Only a 4xx, a 5xx or a transport error is a failed revoke and gets the warning. (V1, V3) |
| S5 | Exit codes (rows 8, 9, the texts) and Auth requests → Token responses | Add: "**Classification uses the `error` code only, never the HTTP status** (Keycloak answers a wrong password with 401 up to 26.5.0 and 400 on main): `invalid_grant` on a password grant → 9 (password text); `invalid_grant` on a refresh grant → session ended (password fallback, else 8 "expired"); `invalid_client` → 9 (client text); `unauthorized_client` → 9 (approval text); any other `error` → 9 with `MangaDex refused the login (<code>)`; a 4xx without a parsable `error` → 3 `<url>: HTTP <n>`." (V2) |
| S6 | Auth requests → Retries; AC6 | "At most **3 logical token requests** (password or refresh grants) per run; each may retry transport errors, 5xx and 429; the revoke is not counted." AC6: "never more than 3 token requests". (Spec issue 3) |
| S7 | Semantic usage checks → `--title`, Secret-shaped extras | Full `--title` text (Spec issue 4). Replace the extras bullets with the T19 echo policy: the secret-shaped prefixes `--password*`, `--client-secret*`, `-p*` → the fixed message; single-dash words longer than 2 characters → counted, never echoed; other option-looking extras echoed cut at `=` (terminal_safe); bare words counted; any flag given `=value` → `<flag> takes no value`; a ConversionError on a value option → `<option>: invalid value (not shown)`; ValidationErrors of `--jobs`/`--layout`/`--color` etc. keep CLI11's text. The extras message is `<cmd> does not take these arguments: <names> (and N other words, not shown in case they hold a password)`, or `<cmd> does not take N more argument(s) (not shown in case they hold a password)`. |
| S8 | Command surface → `--credentials FILE`, `--status` | `--credentials ''` → exit 2 `--credentials needs a file name`. The value is taken verbatim, so `--credentials --no-input` reads a file named `--no-input` (exit 1 if missing, the stock CLI11 behaviour). `--status` is bound one value per occurrence and split by the tool, not with `->delimiter(',')` (D7 closed). |
| S9 | `mdlist` (list-spec "help" text, and any follows-spec mention) | The mdlist footer line becomes `To download the titles you follow, see 'mangadex-dl follows --help'.` |
| S10 | Input & output → Prompts | An answer over 4096 bytes → exit 8 `the <field> you typed is longer than 4096 bytes`. Note: a canonical-mode terminal caps a line at 4095 bytes. |
| S11 | `logout` | The malformed-session and insecure-directory rules (Spec issue 9). The help lists the hidden `--api-base` (not shown) **plus the inherited global flags** (V9c). |
| S12 | Hermetic test story #2, #5; AC10, AC29, AC32 | Fixture statuses: a wrong password answers 401 `invalid_grant` by default, plus a 400 variant; refresh failures and `unauthorized_client` answer 400; a revoke of an unknown token answers 200 `{"error":"invalid_token"}`; a scripted status (for example 503) fails a revoke. AC29 and AC32 use generated `{{FIXTURES}}/<tree>/creds/{credentials-0644,password.txt}`. C9 is resolved: no driver change; positive paths use `--credentials` and `PRE_RUN login --credentials …`. |
| S13 | Engine reuse | `Endpoints` unchanged; `auth_endpoints_for`; `ApiClient::get_authorized` instead of reusing `paginate`; `Prompter` is a core interface (`prompt.hpp`); `secret.hpp`, `session_store.hpp` are new headers. |
| S14 | Secret hygiene, memory row | Adopted: `CURLOPT_POSTFIELDS` (libcurl keeps no copy of the body); `CURLOPT_VERBOSE` is never enabled (it would print the Bearer header); `PR_SET_DUMPABLE 0` plus `RLIMIT_CORE 0` confirmed compatible with ASan/LSan/UBSan/TSan and with `/proc/self/fd` publishing (V20, V21). |
| S15 | Open questions → VERIFY list | Mark resolved: A1, A2 (partial: status varies, see S5), A7 (+ the S4 caveat), D1 (both `{}` and `[]` accepted), D2 (not sent), D7, D8, C1–C12. A3–A6, B1–B6, D3–D6 stay for the human ACs. |
| S16 | login/logout | The help footers per Spec issue 12, worded by cli-ux. |

---

## Approach

The flow becomes a core pipeline of small, separately reviewed seams:
- `Secret`, a move-only string that wipes itself;
- transports that can carry an `Authorization` header and a borrowed POST body;
- a byte-level state-directory store (0700 dir, `O_NOFOLLOW`, temp file + `fsync` + `renameat`, `flock`);
- a token layer (`AuthClient`) and a `LoginSession` that implements a `TokenProvider`;
- a collection fetch that is the only caller of the two allowlisted authorized GETs.

The app layer stays thin: one auth glue file, then `login`/`logout` and `follows` in parallel. `follows` hands the filtered ids to mdlist's **unchanged** `run_batch_jobs`, so downloads stay anonymous.

**Key trade-off:** front-load the risky foundations (transports, fixture auth server, `Secret`, session-file I/O, termios prompter) as four parallel tracks reviewed in batch 2, behind headers frozen in batch 1. The cost is eight batches. The gain:
- every security-sensitive primitive gets its own small diff and its own `security-reviewer` pass;
- `cli.cpp` and `exit_help.hpp` change once, late, with **one** bless point.

---

## Dependencies & pixi/CMake changes

| Change | Where | Task |
|---|---|---|
| None: no `pixi add`, `find_package`, FetchContent or `pixi.lock` change | — | — |
| No preset, pixi task, CMake option, `scripts/*` or CI change | — | — |
| New core sources (recursive glob): `src/core/secret.cpp`, `process_guard.cpp`, `follows.cpp`, `follows_status.cpp`, `follows_fetch.cpp`, `follows_plan.cpp`, `src/core/auth/{form,session_store,token,auth_client,credentials_file,session_json,auth_inputs,auth_endpoints,login_session,logout}.cpp` | glob | T2, T5, T6, T8–T14 |
| New app sources (`cmd_*.cpp` glob): `cmd_auth_prompt.cpp`, `cmd_auth.cpp`, `cmd_trace.cpp`, `cmd_login.cpp`, `cmd_follows.cpp` | glob | T7, T16–T18 |
| New tests: `tests/unit/**`, flat `tests/cli/*.cpp`, `tests/e2e/cases/{follows,login,logout}.cmake` | glob | most |
| New generated fixture trees `{{FIXTURES}}/follows-*` | `fixture_gen.cpp` (not CMake) | T15 |

- **Build files for build-fixer, all tasks:** none beyond each task's Owns. A task that seems to need a shared CMake edit is a coordination event.
- **Tasks:** `nice pixi run check` is the gate everywhere. `nice pixi run -e gcc check`, `nice pixi run tsan -R '…'` and `nice pixi run release-test` apply where noted.

---

## Resolved in-repo, confirmed by the fact-check

- **`SA_RESTART`:** `src/core/interrupt.cpp:124` sets `SA_RESTART`, so the prompter polls and checks `interrupt_requested()` (V31).
- **`with_remedy`:** it keys only on exit 3 (`src/app/cmd_download_run.cpp:90`). Exits 8 and 9 never get the remedy; content failures are routed around it (V30).
- **Switches:** the exhaustive `switch (Errc)` blocks are `error.hpp:exit_code_for` and `cmd_batch_jobs.cpp:is_fatal`. The `switch (BatchSource)` blocks are `src/core/batch_run.cpp:41` and `src/app/cmd_batch_jobs.cpp:55, 215, 227`.
  - **(rev1) Plus one non-switch site:** `src/app/cmd_batch_jobs.cpp:166`, `.claimed = settings_->source == BatchSource::mdlist ? registry_ : nullptr`, which T1 fixes (V27).
- **Fixture transport:**
  - POST → 404 at `src/core/net/fixture_transport.cpp:105`.
  - `map_url` gives `api/manga/status.json` and `…/offset-k.json` (V28).
- **Curl transport:**
  - `src/core/net/curl_transport.cpp:356-388` builds the header slist for POST only and uses `COPYPOSTFIELDS`.
  - `validate` (`:399`) checks only the URL and Content-Type.
  - The `headers`/`lease` declaration order (`:424-426`) resets the handle before the slist dies (V14).
- **FakeTransport:** `RecordedRequest` has no `authorization`.
- **E2E driver:**
  - `PRE_RUN` shares `{{WORK}}` and the XDG dirs, with stdin `/dev/null` and no TTY;
  - `{{FIXTURES}}` expands in ARGS, STDIN and PRE_RUN;
  - `HOME`/`XDG_*` are fresh per case (`tests/e2e/run_case.cmake:82-87`);
  - `MANGADEX_DL_*` is scrubbed (`:76`).
- **Fixture generator:** `e2e.fixtures` runs `fixture_gen` as the same euid as the cases, and `fixture_gen` links only nlohmann (`tests/CMakeLists.txt:25-56`) (V29).
- **Instant clock:** `InstantClock` (`src/core/net/clock.cpp`) runs lock waits virtually. Fixture-mode timeouts are instant; "wait, then succeed" is unit-tier only (V32).
- **Publishing:** `OutputDir` publishes through `/proc/self/fd` (`src/core/fs/output_dir.cpp:243/246/261`). This still works after `PR_SET_DUMPABLE 0` (V21).
- **`--group`** (`src/app/cmd_download_options.cpp:27-32`) is the `--status` pattern.
- **The loopback server** (`tests/support/loopback_http_server.hpp`) records request headers.

---

## Assumptions to verify

**Fact-checker rules (unchanged):**
- **Never** POST to `auth.mangadex.org`, send any credential, or call an authenticated endpoint.
- Allowed: the installed env, `cxx-probe` probes, local loopback servers, and anonymous GETs of public documents.

**Probe notes (rev1):**
- An absolute include of nlohmann fails in a probe. Use the amalgam at `.platform-runs/20261028T162703Z-factcheck-AF/nlohmann_amalgam.hpp`, or a scratch tier-2 test. The absolute CLI11 include works.
- Earlier probes: `.platform-runs/20261028T000000Z-factcheck/` (CLI11) and `.platform-runs/20261028T162458Z-factcheck/` (C, D, H).

**Fact-check results**, as recorded in `follows-factcheck.md`. Each "→" names the plan text that applies.

**Cluster A: MangaDex/Keycloak**
- V1 CONFIRMED.
- V2 PARTIAL → classify on `error` only (T8, T6).
- V3 CONFIRMED with a caveat → a revoke succeeds on any 200 (T12, T6).
- V4 CONFIRMED.
- V5 PARTIAL → cap wording (T13, T21).
- V6 PARTIAL → defensive defaults; rotation stays human-observable (AC44).

**Cluster B: CLI11**
- V7 CONFIRMED.
- V8 CONFIRMED.
- V9a/b CONFIRMED.
- V9c REFUTED → logout's help lists the inherited globals (T19).
- V10 → `std::string` + `count()` (T1, T9, T16).
- V11a CONFIRMED leak → the ConversionError mapping (T19).
- V11b safe.
- V11c → counted.
- V11d → stock text kept by the gate.
- Short clusters → counted.
- DIR swallowing a password → no guard (gate).

**Cluster C: libcurl**
- V12 CONFIRMED (VERBOSE must stay off).
- V13 CONFIRMED (the pointer is kept until `curl_easy_reset`).
- V14 CONFIRMED (the wipe runs after `easy_reset`).

**Cluster D: glibc/POSIX**
- V15 CONFIRMED (new symbols at most GLIBC_2.25).
- V15c REFUTED → the volatile-loop fallback.
- V16 CONFIRMED.
- V17 CONFIRMED (+ `ENOTDIR`).
- V18a–d CONFIRMED (4095-byte line; `POLLIN|POLLHUP`).
- V18e PARTIAL → confirmed by the first CI run; loud SKIP.

**Cluster H: libstdc++**
- V19 CONFIRMED (a moved-from SSO string keeps bytes 1..n−1; only byte 0 is cleared).

**Cluster E: sanitizers**
- V20a/b CONFIRMED.
- V20c CONFIRMED (TSan has no leak detection).
- V21 CONFIRMED (CI kernel/LSM is the residual).
- V22 CONFIRMED.

**Cluster F: nlohmann**
- V23 → silent, last wins.
- V24 CONFIRMED (2 separators per entry; 246 ms under ASan for 100 000 entries).
- V25 CONFIRMED (`get_ptr`).
- V26 CONFIRMED.

**Cluster G: repo internals**
- V27 PARTIAL → the `:166` fix.
- V28–V31 CONFIRMED.
- V32 CONFIRMED.
- V33 CONFIRMED.
- V34 CONFIRMED (ctest names are `unit.<Catch2 name>` / `cli.<…>`, case-sensitive).

**New in rev1** (small; re-check before the task they block):

**Cluster B′: CLI11 2.7.2** (scratch tier-2 test; blocks T16, T19)
- **V35** For a flag given `=value` (`--no-input=x`, `--dry-run=x`, `--json=x` after `follows`), CLI11 raises a `ConversionError` whose `what()` is exactly `Could not convert: <name> = <value>`. `<name>` must be resolvable with `get_option_no_throw(<name>)` on the reached subcommand or on the root (for globals). The check: `get_expected_max() == 0` (or `get_type_size() == 0`) identifies a flag. Also: which exception does `--jobs abc` raise (ConversionError or ValidationError), and with what text? It decides which branch of T19's mapping applies.
- **V36** After `parse()`, `CLI::Option::count()` is 1 for `--credentials ''` (the bound string empty) and 0 when absent. The `Option*` from `add_option` stays valid until the `CLI::App` is destroyed, which happens after dispatch.

**Cluster I: Catch2 3.16.1** (blocks T7)
- **V37** A test that calls `SKIP("reason")` makes ctest report the test as **Skipped**, never Passed. That means `catch_discover_tests` sets `SKIP_RETURN_CODE 4` in the installed version, and the reason appears in the output.

**Clusters (rev1):** Cluster B′: V35, V36 · Cluster I: V37. Everything from rev0 is closed.

| Cluster | Must be confirmed before |
|---|---|
| B′ | T16 (V36), T19 (V35) |
| I | T7 (batch 2) |

**Language and library:** nothing beyond what the tree already uses (`std::expected`, `std::format`/`std::print`, ranges, `std::span`, heterogeneous `std::map`, `std::jthread`/`std::stop_token`).

---

## Task breakdown

**Conventions** (inherited from list-plan):
- **Naming and style:**
  - Namespaces `mangadex_dl`, `mangadex_dl::detail`, `mangadex_dl::app`.
  - Aggregates have DMIs and are built with designated initializers. **Exception:** service structs holding `std::function` (`AuthServices`, `FollowsFetchServices`) are filled member by member (the clang-analyzer false positive noted in `run_title`).
  - `enum class` switches list every enumerator, have no `default:`, and end with a fallback return.
  - No `<CLI/CLI.hpp>` outside `src/app/`, and no nlohmann in public headers.
- **Core never prints.** Every stderr line goes through `print_error`, `print_warning`, `print_note` or the observer, which `terminal_safe` the whole line.
- **Secrets:**
  - never in a `std::format` argument, a log line, an `Error::message`, `--json` or `-vv`;
  - bodies and headers are built with `Secret::append`;
  - every plain `std::string` that held a secret is `secure_wipe`d before it dies;
  - `CURLOPT_VERBOSE` is never set.
- **Frozen headers:** T1's public headers are frozen. A later change is a coordination event.
- **Linking:** a track's tests call only symbols from itself or from an already-integrated batch.
- **Test names (rev1, V34):** every Catch2 name starts with its task's **lowercase prefix**, listed per task as "Test prefix". `-R` filters are case-sensitive and match `unit.<name>` / `cli.<name>`.
- **GCC rule:** never `static_cast` to the type an expression already has (`-Wuseless-cast`).
- **Done when** always means:
  - `nice pixi run check` is green (with a long `timeout`);
  - `git diff --stat tests/e2e/golden` is empty (except T19's bless);
  - **every listed reviewer** is at `VERDICT: APPROVED`. The track reviewers are `quality-reviewer` + `memory-safety-reviewer`, plus `security-reviewer` where marked.

**Sizes:** XS < 50 changed lines · S < 250 · M 250–600 · L 600–1000 (code + tests).

---

### T1: Frozen interfaces (headers, `Errc` 8/9, `BatchSource::follows`, env and prompter seams)
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `headers:` (existing file).
- **Goal:** every cross-module declaration lands once, so batches 2–4 run in parallel. Behaviour changes are only what `-Wswitch` forces, the env reads, and the V27 fix.
- **Files:**
  - edit `include/mangadex_dl/{error,exit_codes,api_client,batch}.hpp` and `include/mangadex_dl/net/{transport,fixture_transport}.hpp`;
  - create `include/mangadex_dl/{secret,prompt,session_store,auth,follows}.hpp`;
  - edit `src/app/io.hpp`, `src/app/io.cpp`, `src/app/cmd_batch_jobs.cpp` (switch cases + the `:166` fix), `src/core/batch_run.cpp` (one case) and `tests/unit/interfaces/headers_test.cpp`.
- **Owns:** exactly those 17 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`error.hpp`:** `login_required` (exit 8) and `auth_rejected` (exit 9) after `row_dir`, each with a doc comment. `exit_code_for` gains both cases.
  - **`exit_codes.hpp`:**
    - `login_required = 8`: "follows, login: no usable saved login and the missing detail can't be asked for, no API client set up, or the saved login expired and no password source exists";
    - `auth_rejected = 9`: "follows, login: MangaDex refused the login details, or a freshly obtained access token";
    - the `mixed_failures` doc becomes "batch/mdlist/follows".
  - **`net/transport.hpp`:**
    - `HttpRequest` gains `std::string authorization{}; ///< the whole Authorization value ("Bearer <t>"); empty = no header. CR, LF or NUL → TransportErrc::local before any I/O. Set only by ApiClient::get_authorized.`
    - **(rev1)** The `body` doc becomes: "borrowed by the curl transport (CURLOPT_POSTFIELDS): it must stay unchanged until fetch() returns, and the transport resets the handle before then".
  - **`net/fixture_transport.hpp`:** doc for T6's auth routing.
  - **`secret.hpp`:**
    ```cpp
    /// explicit_bzero over the whole buffer via resize(capacity()), then clear(). Never reallocates (V19).
    void secure_wipe(std::string& text) noexcept;
    /// A secret string (password, client secret, token, or a form body / session document holding one). Move-only;
    /// no formatter, no operator<<, no ==, no implicit conversion. Wiped on destruction, assignment, clear(), growth.
    class Secret {
    public:
        Secret() noexcept = default;
        [[nodiscard]] static Secret copy_of(std::string_view text);     ///< exact-capacity copy
        [[nodiscard]] static Secret adopt(std::string&& text) noexcept; ///< takes the buffer, wipes `text`
        Secret(Secret&& other) noexcept;                ///< steals, then wipes other's buffer (SSO keeps bytes 1..n-1, V19)
        Secret& operator=(Secret&& other) noexcept;     ///< wipes self first
        Secret(const Secret&) = delete; Secret& operator=(const Secret&) = delete;
        ~Secret();
        void reserve(std::size_t bytes);  ///< growth copies, then wipes the old buffer
        void append(std::string_view text);
        void push_back(char c);
        void clear() noexcept;
        [[nodiscard]] bool empty() const noexcept; [[nodiscard]] std::size_t size() const noexcept;
        /// Borrows *this: never bind it to a temporary Secret, never store it.
        [[nodiscard]] std::string_view reveal() const noexcept MANGADEX_DL_LIFETIMEBOUND;
    private:
        std::string value_{};
    };
    enum class DumpGuard { not_dumpable, no_core_only, none };
    /// setrlimit(RLIMIT_CORE, {0,0}), then prctl(PR_SET_DUMPABLE, 0) (V20, V21 confirmed). Best effort; once, from
    /// the main thread, before any secret is read.
    [[nodiscard]] DumpGuard disable_core_dumps() noexcept;
    ```
  - **`prompt.hpp`** (core):
    - `enum class Echo { on, off };`
    - `enum class PromptStatus { answered, empty, too_long, interrupted, unavailable };`
    - `struct PromptReply { PromptStatus status = PromptStatus::unavailable; Secret answer{}; };`
    - `inline constexpr std::size_t max_prompt_answer_bytes = 4096;` with **(rev1)** the doc "an ICANON terminal caps a line at 4095 bytes";
    - the polymorphic base:
      ```cpp
      class Prompter {  // polymorphic base: copy/move deleted, protected ctor, virtual dtor
      public:
          virtual void notice(std::string_view line) = 0;
          /// Writes `question`, reads one line (≤ max_prompt_answer_bytes; '\n'/'\r\n' stripped). Echo::off: off for the
          /// read, restored on every path, then a newline. Polls interrupt_requested() at least every 100 ms.
          [[nodiscard]] virtual PromptReply ask(std::string_view question, Echo echo) = 0;
      };
      ```
  - **`session_store.hpp`** (core, file I/O):
    - `struct UserDirs { std::optional<std::filesystem::path> home{}, xdg_config_home{}, xdg_state_home{}; };`
    - `make_user_dirs(std::optional<std::string_view> home, config, state)` keeps only non-empty absolute paths.
    - `state_dir_path(const UserDirs&)` and `default_credentials_path(const UserDirs&)` return `std::optional<path>`.
    - `enum class SessionSlot { real, fixture };` and `session_file_name()`, giving `"session.json"` / `"fixture-session.json"`.
    - `struct LockTiming { std::chrono::milliseconds poll{250}; std::chrono::seconds timeout{60}; };`
    - `enum class FileProblemKind { missing, symlink, not_regular, not_owned, too_open, too_large, io };`
    - `struct FileProblem { FileProblemKind kind = FileProblemKind::io; unsigned mode = 0; std::string reason{}; };` and `template <class T> using FileResult = std::expected<T, FileProblem>;`
    - `[[nodiscard]] FileResult<Secret> read_private_file(const std::filesystem::path& path, std::size_t cap);`
    - `class SessionLock`: move-only RAII; the only owner of the lock fd (**never `dup`ed**, V16).
    - `class StateDir`:
      ```cpp
      class StateDir {  // move-only; owns an O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC fd (not O_PATH: fsync, V17)
      public:
          [[nodiscard]] static FileResult<StateDir> open(const std::filesystem::path& path, bool create);
          [[nodiscard]] Result<SessionLock> lock(Clock& clock, LockTiming timing, std::stop_token stop) const;
          [[nodiscard]] FileResult<std::optional<Secret>> read(SessionSlot slot) const;
          [[nodiscard]] Result<void> write(SessionSlot slot, const Secret& bytes) const;
          [[nodiscard]] Result<bool> remove(SessionSlot slot) const;
          [[nodiscard]] const std::filesystem::path& path() const noexcept MANGADEX_DL_LIFETIMEBOUND;
      };
      ```
      The contracts are as in T5.
  - **`auth.hpp`** (core):
    - **Constants:**
      - `official_auth_issuer` (`https://auth.mangadex.org/realms/mangadex`) and `api_clients_url` (`https://mangadex.org/settings`);
      - `max_token_bytes` 16384, `max_token_response_bytes` 64 KiB, `max_credentials_file_bytes` 16 KiB, `max_session_file_bytes` 64 KiB, `max_password_stdin_bytes` 4096;
      - `max_token_requests` 3 (**logical grants**, rev1 doc);
      - `reuse_min_validity` 60 s, `refresh_ahead` 30 s, `default_access_lifetime` 900 s, `max_access_lifetime` 86400 s;
      - `auth_request_interval` 1000 ms, `revoke_timeout` 5 s.
    - **`class BearerToken`:**
      - `static Result<BearerToken> parse(Secret raw)` accepts the charset `[A-Za-z0-9._~+/=-]{1,16384}`; anything else is Errc::network with a fixed text;
      - `const Secret& value() const LIFETIMEBOUND`;
      - `Secret authorization() const` returns `"Bearer <t>"`;
      - the default ctor is an empty placeholder that `parse` never yields.
    - **Aggregates:**
      - `ClientCredentials { std::string client_id{}; Secret client_secret{}; }`;
      - `CredentialsFile { std::optional<std::string> client_id{}; std::optional<Secret> client_secret{}; std::optional<std::string> username{}; std::optional<Secret> password{}; }`;
      - `Session { std::string issuer{}, client_id{}; Secret client_secret{}; std::string username{}; std::optional<BearerToken> refresh_token{}; std::optional<std::int64_t> refresh_expires_at{}; std::optional<BearerToken> access_token{}; std::int64_t access_expires_at = 0; }`;
      - `TokenGrant { BearerToken access{}; std::optional<BearerToken> refresh{}; std::chrono::seconds expires_in{900}; std::optional<std::chrono::seconds> refresh_expires_in{}; }`.
    - **(rev1, V10) Raw CLI aggregates:**
      - `RawAuthArgs { std::string credentials{}; bool credentials_given = false; ///< Option::count() > 0, set by the Command after parse` with `bool password_stdin = false; bool no_input = false; }`;
      - `RawLoginArgs { RawAuthArgs auth{}; std::string api_base = std::string{default_api_base}; }`;
      - `RawLogoutArgs { std::string api_base = …; }`.
    - **Pure functions:**
      - `Secret form_encode(std::string_view)`;
      - `password_grant_body`, `refresh_grant_body` and `revoke_body` (the fixed field orders as in rev0);
      - `parse_credentials_file` and `load_credentials_file(const path&, bool explicit_path)`;
      - `read_password_stdin(int fd)`;
      - **(rev1)** `Result<void> check_raw_auth_args(const RawAuthArgs&)` → `credentials_given && credentials.empty()` → Errc::usage `--credentials needs a file name`;
      - `check_auth_api_base(const ApiBase&, std::string_view command)`;
      - `parse_session(json, issuer)` and `session_to_json(const Session&)`;
      - `parse_token_response`.
    - **Token errors:**
      - `enum class GrantKind { password, refresh };`
      - `enum class TokenRefusal { bad_credentials, bad_client, client_not_allowed, session_ended, other };`
      - `struct TokenErrorInfo { TokenRefusal refusal = TokenRefusal::other; int status = 0; std::optional<std::string> code{}; std::optional<std::string> description{}; };`
      - **(rev1)** `classify_token_error(GrantKind, std::string_view body)` takes **no status** (V2). It returns `std::optional<TokenErrorInfo>`, which is nullopt when the body has no parsable `error`;
      - `Error token_refusal_error(const TokenErrorInfo&, GrantKind, std::string_view username)`.
    - **Endpoints:** `struct AuthEndpoints { std::string issuer{}, token_url{}, revoke_url{}; };` and `std::optional<AuthEndpoints> auth_endpoints_for(const ApiBase&)`.
    - **Failure type:**
      - `enum class AuthFailureKind { request, content, refused, local };`
      - `struct AuthFailure { AuthFailureKind kind = AuthFailureKind::local; TokenErrorInfo info{}; Error error{}; };`
      - `template <class T> using AuthResult = std::expected<T, AuthFailure>;`
    - **`class AuthClient`** (non-copyable, non-movable; borrows transport, clock and trace):
      - `AuthResult<TokenGrant> request(GrantKind, const Secret& body, std::stop_token)`;
      - `Result<void> revoke(const Secret& body, std::stop_token)`: **(rev1)** success = any HTTP 200;
      - `int requests() const noexcept`.
    - **`class TokenProvider`** (polymorphic base):
      - `virtual AuthResult<Secret> authorization(std::stop_token) = 0;`
      - `virtual AuthResult<Secret> refresh_after_unauthorized(std::stop_token) = 0;`
    - **Login flow:** `LoginMode { reuse, password }`, `LoginHow { saved, refreshed, password }`, `LoginRequest`, `AuthServices`, `LoginNotes`, `class LoginSession final : public TokenProvider` (`static AuthResult<std::unique_ptr<LoginSession>> open(LoginRequest, const AuthServices&, std::stop_token, LoginNotes&)`; `void close() noexcept`), `LogoutOutcome` and `Result<LogoutOutcome> logout(const ApiBase&, const UserDirs&, const AuthServices&, LockTiming, std::stop_token)`. All as in rev0.
  - **`follows.hpp`** (core):
    - **Constants:** `follows_page_limit` 100; `max_follows` 10000 (**rev1 doc:** "the window MangaDex documents for collection endpoints; refused, not truncated"); `max_status_entries` 100000; `max_status_separators` 1000000 (**rev1 doc:** "two per entry at the cap, plus headroom"); `follows_row_label` "follows".
    - **Reading statuses:** `ReadingStatus`, `reading_status_name`, `parse_reading_status` and `StatusFilter` (`parse(std::span<const std::string>)`, `contains`, `names`).
    - **Documents:** `FollowsPage`, `StatusMap`, `FilteredFollows`, `follows_request_url`, `status_map_request_url`, `parse_follows_page`, `parse_status_map` and `filter_follows`.
    - **Settings:** `RawFollowsArgs` (with `RawAuthArgs auth{}`), `FollowsSettings`, `validate_follows_args` and `plan_follows`.
    - **Fetch:** `FollowsCollection`, `FollowsFetchServices` and `fetch_follows_collection(const ApiBase&, const FollowsFetchServices&, TokenProvider&, bool with_statuses) -> AuthResult<FollowsCollection>`.
    - All as in rev0.
  - **`api_client.hpp`:** `enum class AuthorizedPath { user_follows_manga, manga_status };` plus the member:
    ```cpp
    /// GET <api>/user/follows/manga?limit=100&offset=<offset> or <api>/manga/status (offset ignored) with
    /// Authorization = `authorization`. The URL is built here from the enum: no other request can carry a token.
    /// Errc::usage (no I/O) unless endpoints.api is the official API or a file:// fixture. Retries as perform();
    /// returns the final response unmapped (2xx or not); a stop → Errc::interrupted.
    [[nodiscard]] Result<HttpResponse> get_authorized(AuthorizedPath path, std::size_t offset,
                                                      const Secret& authorization, std::stop_token stop);
    ```
  - **`batch.hpp`:**
    - `BatchSource::follows`;
    - `struct FollowsSummary { std::string username{}; std::size_t titles = 0; std::optional<std::vector<std::string>> statuses{}; std::size_t selected = 0; };`
    - the overload `batch_to_json(const BatchResult&, std::string_view source, bool dry_run, const FollowsSummary&)` (T14 defines it).
  - **`io.hpp` / `io.cpp`:**
    - `EnvSnapshot` gains `std::optional<std::string> home{}, xdg_config_home{}, xdg_state_home{}` (raw and non-empty, owned copies). `read_env` reads them and drops `noexcept`.
    - `Io` gains `mangadex_dl::Prompter* prompter = nullptr;` (borrowed; null means the real terminal prompter), through a forward declaration.
  - **`cmd_batch_jobs.cpp`:**
    - `is_fatal`: `case Errc::login_required: case Errc::auth_rejected: return true;`
    - `row_noun`, `row_prefix` and `collision_message`: `case BatchSource::follows:` falls through with `mdlist`.
    - **(rev1, V27)** a new `[[nodiscard]] constexpr bool claims_paths(BatchSource) noexcept` switch (`csv` → false; `mdlist`, `follows` → true; fallback false). Line 166 becomes `.claimed = claims_paths(settings_->source) ? registry_ : nullptr`, so `-Wswitch` guards any future source. T14 adds the behaviour test.
  - **`batch_run.cpp:41`:** the same fall-through.
  - **`headers_test.cpp`:**
    - `static_assert`s for exits 8 and 9;
    - the Secret traits (not copyable, nothrow movable);
    - `Prompter` and `TokenProvider` are abstract;
    - the constants;
    - `HttpRequest{}.authorization.empty()`;
    - `RawAuthArgs{}.credentials_given == false`;
    - signatures in unevaluated contexts only;
    - **no Secret instantiated at run time** (`secret.cpp` lands in the parallel T2).
- **Dependencies:** none.
- **Done when:**
  - `check` **and** `-e gcc check` are green;
  - every existing test passes unmodified (except `headers_test.cpp`): mdlist's `--overwrite` tests prove line 166 is unchanged for mdlist;
  - goldens are unchanged;
  - the three reviewers approve.
- **Tests:** `headers_test.cpp`.

### T2: `Secret` and the core-dump guard
- **Size:** S. **Security-reviewer:** yes. **Test prefix:** `secret:`, `dump guard:`.
- **Files:** create `src/core/secret.cpp`, `src/core/process_guard.cpp`, `tests/unit/secret/secret_test.cpp` and `tests/unit/secret/process_guard_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`secure_wipe`:** `resize(capacity())`, then `explicit_bzero(data(), size())`, then `clear()`. `explicit_bzero` is GLIBC_2.25, within the floor (V15).
    - **(rev1)** Should a platform lack it, the fallback is a `volatile unsigned char*` loop or an asm barrier. `memset_explicit` is not in glibc 2.28 (V15c).
  - **`Secret`:**
    - move ctor/assign: steal, then `secure_wipe(other.value_)`. **(rev1)** A moved-from SSO string keeps bytes 1..n−1, with only byte 0 cleared, which is exactly why this wipe exists (V19);
    - growth: allocate, copy, wipe the old buffer, swap;
    - `copy_of` reserves exactly;
    - the dtor wipes.
  - **`disable_core_dumps`:** `setrlimit(RLIMIT_CORE, {0,0})`, then `prctl(PR_SET_DUMPABLE, 0)`. V20, V21 and V22 confirmed this keeps every sanitizer report and `/proc/self/fd` publishing.
- **Dependencies:** none.
- **Done when:** `check` and `nice pixi run release-test` (`release-check.binary` glibc floor) are green; the three reviewers approve.
- **Tests (unit):**
  - **`Secret`:**
    - SSO and heap values;
    - a move leaves the source empty with every byte of its still-owned buffer zeroed (inspect only buffers that are still owned, never freed memory);
    - growth across several appends;
    - `reveal()`, `clear()`;
    - `adopt` wipes the source.
  - **Guard:** in a `fork()`ed child, `PR_GET_DUMPABLE == 0` and `RLIMIT_CORE == {0,0}`; the child `_exit`s its verdict.

### T3: Docs: close list-spec V1 (private list → 404 → exit 4, confirmed live)
- **Size:** XS. **Security-reviewer:** no.
- **Files:** edit `docs/design/list-spec.md` and `README.md` (only the mdlist private-list sentences at `:484` and `:575-578`).
- **Owns:** those 2 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`list-spec.md`:** the status lines (5, 32), the "Private lists (V1, unresolved…)" block (:226-230) and the "[VERIFY] V1" item (:563-566) become "V1 RESOLVED (user live test, 2026-10-28): an anonymous `GET /list/{id}` for a private list answers 404; mdlist exits 4". The 401/403-with-CustomList-body fallback is noted as kept defensively.
  - **README:**
    - "a private list counts as not found (exit 4)" loses its hedge;
    - "a private list may give `(HTTP 403)` or `(HTTP 401)`" becomes "MangaDex answers a private list with HTTP 404 (the code also accepts a 401/403 that names the list, with the same message)".
  - **Not touched here:** the "follows not supported yet" line (T21).
- **Dependencies:** none.
- **Done when:** `check` green; `quality-reviewer` approves. `memory-safety-reviewer` confirms "no C++ changed".
- **Tests:** none.

### T4: Transports: `Authorization` on requests, borrowed POST bodies, FakeTransport recording
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `curl auth:`.
- **Files:** edit `src/core/net/curl_transport.cpp`, `src/core/net/curl_api.hpp`, `src/core/net/curl_api.cpp` and `tests/support/fake_transport.hpp`; create `tests/unit/net/curl_transport_auth_test.cpp`.
- **Owns:** those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`validate`:** an `authorization` containing CR, LF or NUL → `TransportErrc::local` "invalid Authorization header (contains CR, LF or NUL)", with no value echoed.
  - **(rev1, V12) `configure`:**
    - the header slist is built for **both** methods: `Expect:` and `Content-Type` for POST as today, plus `Authorization: <value>` when it is non-empty;
    - the Authorization line is assembled in a `Secret`, never in a `std::string` temporary;
    - **`setopt_slist(HTTPHEADER)` moves out of the POST-only case** and is set whenever the list is non-empty;
    - a GET with an empty authorization sets no list (exactly today's options).
  - **(rev1, V13) POST:**
    - `POSTFIELDSIZE_LARGE` first, then `POSTFIELDS` pointing at `request.body.data()`, through a new `CurlApi::setopt_borrowed(CURL*, CURLoption, const char*)` whose doc says "libcurl does **not** copy: the data must stay valid until `curl_easy_reset`";
    - `setopt_str`'s "libcurl copies" doc is corrected to exclude this option;
    - **the body must outlive `curl_easy_reset`, not just `perform`.** `request` is the caller's and outlives `fetch()`, and the lease (which resets the handle) is destroyed inside `fetch()`.
  - **(rev1, V14) Declaration order** (`:424-426`) stays `state` → `headers` → `lease`, so the lease's `easy_reset` runs **before** `SlistFree`. `SlistFree` `explicit_bzero`s each node's `data` before `curl_slist_free_all`.
  - **Best effort:** libcurl's internal header copies are out of reach and documented as such.
  - **`CURLOPT_VERBOSE` is never set**; a comment at `configure` says why (V12: it would print the Bearer header).
  - **`FakeTransport`:** `RecordedRequest` gains `std::string authorization{}` as its last member.
- **Dependencies:** T1.
- **Done when:**
  - `check`, `-e gcc check` and `release-test` are green;
  - `unit.net*`, `unit.api_client*` and `e2e.*` pass unmodified;
  - the three reviewers approve.
- **Tests (unit, loopback server):**
  - **AC9:**
    - a GET with `Bearer abc` → exactly one `Authorization: Bearer abc`;
    - empty → no `Authorization`, no `Expect`;
    - CR, LF and NUL → `local`, with no request reaching the server.
  - **POST:** exact body bytes (`&=%`), the Content-Type; the existing POST test and the MD@H path are unchanged.
  - **Redirect:** a 307 is returned, never followed; the header is sent once.

### T5: State directory, session file store, lock and private-file reader
- **Size:** L. **Security-reviewer:** yes. **Test prefix:** `session store:`, `private file:`.
- **Files:** create `src/core/auth/session_store.cpp`, `tests/unit/auth/session_store_test.cpp` and `tests/unit/auth/private_file_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:** T1's contract:
  - **`StateDir::open`:**
    - mkdir -p with 0700 per missing component (parents may be symlinks);
    - the final component is opened `O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC` and its owner and mode are checked;
    - **(rev1, V17)** a symlinked final component fails with **`ENOTDIR`**. On `ENOTDIR` (or `ELOOP`), `fstatat(…, AT_SYMLINK_NOFOLLOW)` picks the reason: "it is a symbolic link" or "it is not a directory".
  - **`lock`:**
    - `openat(…, "session.lock", O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600)`, then `flock(LOCK_EX|LOCK_NB)` polled on the injected `Clock`, checking `interrupt_requested()`;
    - timeout → `another mangadex-dl is using the saved login; try again`;
    - **(rev1, V16)** a lock is per open file description: the fd is never `dup`ed, `SessionLock`'s move transfers ownership, and closing the last fd releases it.
  - **`read`:** bounded, straight into a pre-reserved Secret (never through `read_bounded_fd`, whose growth would leave copies). `ELOOP` → `symlink`.
  - **`write`:**
    1. create `.<name>.<12 hex>` with `openat(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600)`, with bounded retries;
    2. `fchmod 0600`;
    3. write all, then `fsync`;
    4. `close`, checked;
    5. `renameat`, then `fsync(dir)`;
    6. on every failure path, a scope guard `unlinkat`s the temp.
  - **`remove`.**
  - **`read_private_file`:** the same checks with symlinks followed.
  - **The paths:** `make_user_dirs`, `state_dir_path` and `default_credentials_path`.
  - **Reasons** are fixed phrases plus `strerror`.
- **Dependencies:** T1, T2.
- **Done when:** `check` and `-e gcc check` are green; the three reviewers approve.
- **Tests (unit, `TempDir`):**
  - **Directories:**
    - 0700 under umask 022 and 077, parents too;
    - an existing 0755 dir → `too_open`, nothing written;
    - a symlinked final dir → `symlink` (via `ENOTDIR`);
    - a regular file at the dir path → "it is not a directory".
  - **Files:**
    - a symlinked `session.json` → `symlink`, the target untouched;
    - a write under umask 0 is still 0600;
    - the temp is gone after success and after a forced `renameat` failure (a directory at the final name);
    - read: round trip, absent, `too_large`, 0644 → `too_open`;
    - remove: present → true, absent → false.
  - **Lock (AC26 core):**
    - a second `open()` + `flock` holds it → `FakeClock`-polled timeout with the exact message;
    - an `on_sleep` hook that closes the other fd at the 3rd poll → success (the "wait, then succeed" case lives here only, V32);
    - a simulated interrupt → interrupted.
  - **`read_private_file`:** 0600 ok; 0640 and 0604 → `too_open` with `mode`; a FIFO never blocks (`not_regular`); a symlink to a 0600 file → ok; missing; 16 KiB + 1.

### T6: Fixture auth server, form codec and the auth fixture writer
- **Size:** L. **Security-reviewer:** yes. **Test prefix:** `form:`, `fixture auth:`.
- **Files:**
  - edit `src/core/net/fixture_transport.cpp`;
  - create `src/core/auth/form.hpp` (core-private `form_decode`), `src/core/auth/form.cpp`, `tests/support/mdx_auth_fixture.hpp`, `tests/unit/auth/form_test.cpp` and `tests/unit/net/fixture_transport_auth_test.cpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`form_encode`:** RFC 3986 unreserved characters kept; everything else `%XX` (upper case); the output goes into a reserved Secret.
  - **`form_decode`:** `+` is a space; a bad `%` or a duplicate key is an error; at most 16 fields.
  - **Fixture POST routing** (before the 404 at `:105`):
    - `…/auth/realms/mangadex/protocol/openid-connect/token`: the form Content-Type is required and the body is capped at 64 KiB;
      - `grant_type=password` → `<root>/auth/password/<username>.json`;
      - `grant_type=refresh_token` → `<root>/auth/refresh/<token>.json`;
    - `…/revoke` → `<root>/auth/revoke/<token>.json`;
    - names must match `[A-Za-z0-9._-]{1,128}` and not be `.` or `..`, else `local` "fixture: unsafe auth name";
    - files are opened with the existing `O_NOFOLLOW` walk.
  - **(rev1, V2) Statuses mirror Keycloak:**
    - a missing password file, or a password mismatch → **401** `{"error":"invalid_grant","error_description":"Invalid user credentials"}` by default;
    - a password file may say `"mismatch_status": 400` (the newer-Keycloak variant);
    - a missing refresh file → **400** `invalid_grant` "Token is not active";
    - a client mismatch → 401 `invalid_client`;
    - a file may script `{"status": 400, "body": {"error": "unauthorized_client"}}`;
    - otherwise `status` + `body.dump()`.
  - **(rev1, V3) Revoke:** a missing revoke file → **200** `{"error":"invalid_token"}` (Keycloak's behaviour); a revoke file may script a failure status (for example 503).
  - **Other POSTs** → 404 (MD@H).
  - **Authorized GETs:**
    - an exact allowlist: `api/user/follows/manga` (with `offset=`) and `api/manga/status`;
    - a token T with a matching `<root>/auth/access/<T>.json` → served;
    - otherwise → 401 `{"result":"error","errors":[{"status":401,"title":"unauthorized_http_exception"}]}`;
    - `{"valid_requests": n}` counts down per T in a mutex-guarded `std::map<std::string, std::size_t>`;
    - any `api/user/…` GET without a token → 401.
  - **Leak tripwire:** an authorization to any URL off the allowlist → `local` "fixture: Authorization sent to <url>".
  - **`mdx_auth_fixture.hpp`:**
    - `FixtureAuth` (client, user, password, the token chain, `valid_requests`, `mismatch_status`, scripted refusals and revoke statuses);
    - `write_auth_files(root, const FixtureAuth&)`;
    - obviously fake constants (`fixture-user`, `fixture-password-not-a-secret`, `fixture-client`, `fixture-client-secret-not-a-secret`, `fixture-access-1`/`-2`, `fixture-refresh-1`/`-2`).
- **Dependencies:** T1.
- **Done when:**
  - `check` is green;
  - `nice pixi run tsan -R 'unit\.fixture auth'` is green;
  - existing `unit.net*`, `cli.*` and `e2e.*` pass unmodified;
  - the three reviewers approve.
- **Tests (unit):**
  - **AC1 encode/decode.**
  - **AC10:**
    - routing with exact statuses: 401 wrong password; the 400 variant; 400 refresh; 401 invalid client; 400 `unauthorized_client`; 200 for an unknown revoke; a scripted 503 revoke;
    - `../x`, `a/b`, `.` → local;
    - `valid_requests` 2 → 200, 200, 401;
    - the tripwire on `api/manga/<uuid>`, while `api/manga/status` with a token is served;
    - MD@H → 404;
    - no token → 401;
    - concurrent counter use (TSan).

### T7: Terminal prompter (termios, echo off, interrupt polling)
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `prompt:`.
- **Files:** create `src/app/cmd_auth_prompt.hpp`, `src/app/cmd_auth_prompt.cpp`, `tests/support/pty_pair.hpp` and `tests/cli/auth_prompt_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - `class TerminalPrompter final : public Prompter { TerminalPrompter(int fd, std::ostream& err) noexcept; }`.
  - **`notice`:** write and flush.
  - **`ask`:**
    1. write the question and flush;
    2. `Echo::off`: `tcgetattr` (failure → `unavailable`), then the `TermiosRestore` RAII, which clears `ECHO` with `TCSAFLUSH` and restores on every path;
    3. loop `poll(100 ms)`, checking `interrupt_requested()` → `interrupted`;
    4. **(rev1, V18)** `POLLHUP` (or `POLLIN` with a 0-byte read) after the data is drained → EOF;
    5. read into a wiped stack buffer of 4097 bytes, appending to a Secret reserved to 4097;
    6. stop at `\n` (strip `\r`), or empty/EOF → `empty`, or > 4096 → `too_long`;
    7. `Echo::off` → write a newline.
- **Dependencies:** T1, T2.
- **Done when:**
  - `check` and `-e gcc check` are green;
  - `nice pixi run tsan -R 'cli\.prompt'` is green;
  - the three reviewers approve.
- **Tests (cli):**
  - **Pty tests** (`pty_pair.hpp`: `posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r`, RAII fds). **(rev1, V18e, V37)** If `posix_openpt` fails, the test calls `SKIP("no pseudo-terminal: <strerror>")`, so ctest shows *Skipped*, never Passed. The first CI run on ubuntu-26.04 confirms they run there; a Skipped there is a release-blocking finding for the User Test Plan. The pty cases:
    - echo off: a writer thread waits for `!ECHO` (`tcgetattr(master)`), writes `s3cret\n` → the answer; the master reads no `s3cret`; termios restored; `err` holds the question plus `\n`;
    - echo on: echoed;
    - a 4095-byte line → answered (the ICANON maximum);
    - an interrupt simulated after echo goes off → `interrupted`, echo restored (AC21 prompt half, AC40 core);
    - the master closed → `empty` (`POLLHUP`).
  - **Pipe tests (rev1):** `Echo::on` on a pipe: 5000 bytes → `too_long`; EOF → `empty`; an empty line → `empty`. `Echo::off` on a pipe → `unavailable`.

### T8: Token layer: bodies, token response, error classification, `AuthClient`
- **Size:** L. **Security-reviewer:** yes. **Test prefix:** `token:`, `auth client:`.
- **Files:** create `src/core/auth/token.cpp`, `src/core/auth/auth_client.cpp`, `tests/unit/auth/token_test.cpp` and `tests/unit/auth/auth_client_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **The body builders**, through `Secret::append`.
  - **`parse_token_response`:**
    - `parse_untrusted` at 64 KiB;
    - `access_token` required; `refresh_token` optional;
    - `expires_in` an integer 1–86400, else 900;
    - `refresh_expires_in` 0 or absent → nullopt;
    - **(rev1, V25)** token strings are wiped in place through `get_ptr<std::string*>()` before the DOM dies; **the DOM is never copied**; the response body string is wiped separately by the caller (`AuthClient`).
  - **(rev1, V2) `classify_token_error(grant, body)`** keys on the **`error` code only**:
    - `invalid_grant` → `bad_credentials` (password grant) or `session_ended` (refresh grant);
    - `invalid_client` → `bad_client`;
    - `unauthorized_client` → `client_not_allowed`;
    - any other `[a-z_]{1,64}` code → `other`;
    - no parsable `error` → nullopt;
    - the status is recorded for `-v` only.
  - **`token_refusal_error`:** 9 for `bad_credentials`, `bad_client`, `client_not_allowed` and `other` (`MangaDex refused the login (<code>)`); `session_ended` is the flow's decision (fallback or 8).
  - **`AuthClient`:**
    - its own 1 s limiter and `RetryConfig` (429, 5xx, timeout/protocol retried; any other 4xx answered once);
    - the 4th logical request → `{content, network "more than 3 MangaDex login requests in one run; refusing to continue"}`;
    - a 4xx with a classifiable body → `{refused, info}`; a 4xx without one, or 5xx exhausted → `{request, "<url>: HTTP n"}`;
    - a malformed 2xx → `content`;
    - **(rev1, V3)** `revoke`: any HTTP 200 is success (body ignored); 4xx, 5xx or a transport error → an error; 5 s limits; no retries;
    - the `-vv` trace is `POST <url> -> <status> (<ms> ms)`;
    - `HttpRequest.body` is filled from the Secret and `secure_wipe`d after `fetch`, together with the response body.
- **Dependencies:** T1, T2, T4, T6.
- **Done when:** `check` green; the three reviewers approve.
- **Tests (unit, FakeTransport + FakeClock):**
  - **AC1:** exact bodies.
  - **AC3:** the response shapes and rejects.
  - **AC4 (rev1):**
    - the classification table with **both** 400 and 401 for `invalid_grant` on the password grant → the same refusal;
    - refresh 400 `invalid_grant` → `session_ended`;
    - 400 `unauthorized_client`;
    - a 4xx without a body → `request`;
    - exactly **one** POST per 4xx;
    - 503 ×3 then 200; 429 with `Retry-After: 2`;
    - the cap;
    - the revoke: 200 with `{"error":"invalid_token"}` → success, 503 → failure;
    - empty authorization on every POST; no body in the trace.

### T9: Local auth inputs: credentials file, session JSON, `--password-stdin`, raw-arg and API-base checks
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `credentials file:`, `session json:`, `auth inputs:`.
- **Files:** create `src/core/auth/credentials_file.cpp`, `src/core/auth/session_json.cpp`, `src/core/auth/auth_inputs.cpp`, `tests/unit/auth/credentials_file_test.cpp`, `tests/unit/auth/session_json_test.cpp` and `tests/unit/auth/auth_inputs_test.cpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **Credentials file:** `parse_credentials_file` and `load_credentials_file`, as in rev0.
  - **Session JSON:** `parse_session` and `session_to_json`, with the hand-written serializer into a Secret. **(rev1, V25)** The parsed DOM's token strings are wiped through `get_ptr<std::string*>()`, the DOM is never copied, and the input Secret is the caller's.
  - **`read_password_stdin`.**
  - **(rev1, V10) `check_raw_auth_args`:** `credentials_given && credentials.empty()` → 2 `--credentials needs a file name`.
  - **`check_auth_api_base`.**
- **Dependencies:** T1, T2, T5.
- **Done when:** `check` green; the three reviewers approve.
- **Tests (unit):**
  - **AC2:** the accept/reject rows; no value in any message.
  - **AC5.**
  - **`--password-stdin`** via `pipe2`.
  - **`check_raw_auth_args`:** absent → ok; given and empty → 2; given `--no-input` as the value → ok (the stock CLI11 behaviour is documented, S8).
  - **API base.**

### T10: Follows documents: page parser, status map, `StatusFilter`, filter
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `follows page:`, `status map:`, `status filter:`.
- **Files:** create `src/core/follows.cpp`, `src/core/follows_status.cpp`, `tests/unit/follows/follows_page_test.cpp`, `tests/unit/follows/status_map_test.cpp` and `tests/unit/follows/status_filter_test.cpp`.
- **Owns:** exactly those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **The URL builders** and **`parse_follows_page`**.
  - **`parse_status_map`:**
    - `max_separators = 1 000 000`. **(rev1, V24)** An entry costs 2 separators, so the entry cap (100 000 → about 200 000 separators) always fires first for honest maps; the separator cap guards hostile shapes;
    - `[]` accepted as empty;
    - more than 100 000 entries → the refusal text;
    - keys `is_uuid` → canonical, first wins on a canonical duplicate;
    - **(rev1, V23)** a duplicate JSON key is not detected: nlohmann keeps the last value, which is documented and not treated as malformed;
    - values must match `[a-z_]{1,32}`.
  - **`StatusFilter::parse`** and **`filter_follows`**, as in rev0.
- **Dependencies:** T1.
- **Done when:** `check` green; the three reviewers approve.
- **Tests (unit):**
  - AC50, AC51, AC52.
  - **(rev1)** a 100 001-entry map (about 5 MB) → refused by the **entry** cap with the exact text, not the separator cap;
  - a 100 000-entry map → accepted;
  - a document that repeats a key with two values → the last value, documented.

### T11: `ApiClient::get_authorized` and `auth_endpoints_for`
- **Size:** S. **Security-reviewer:** yes. **Test prefix:** `api client authorized:`.
- **Files:** edit `src/core/api_client.cpp`; create `src/core/auth/auth_endpoints.cpp` and `tests/unit/api_client/api_client_authorized_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - `GetSpec` gains `const Secret* authorization = nullptr`.
  - **(rev1)** `perform`'s `const HttpRequest request{...}` becomes **non-const**: `authorization` is copied into it, and `secure_wipe(request.authorization)` runs on every return path through a scope guard.
  - `get_authorized` builds its two URLs itself. These are the fixed strings `<api>/user/follows/manga?limit=100&offset=<k>` and `<api>/manga/status`, so T11 doesn't link against the parallel T10; T13's test pins equality with T10's builders.
  - The official/fixture check runs before any I/O.
  - `auth_endpoints_for`: official and fixture only.
- **Dependencies:** T1, T2, T4.
- **Done when:** `check` green; `unit.api_client*` unmodified (including `:304-321`); the three reviewers approve.
- **Tests (unit):**
  - exact URLs and authorization;
  - a 401 is returned once (not retried); 503 → retried;
  - a custom https base → usage, no request;
  - after a `get_authorized` call, every other `fetch_*` records an empty authorization (AC8, client level).

### T12: Login session flow and logout (core)
- **Size:** L. **Security-reviewer:** yes. **Test prefix:** `login session:`, `logout:`.
- **Files:** create `src/core/auth/login_session.cpp`, `src/core/auth/logout.cpp`, `tests/support/fake_prompter.hpp`, `tests/unit/auth/login_session_test.cpp` and `tests/unit/auth/logout_test.cpp`.
- **Owns:** exactly those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`LoginSession::open`,** as in rev0:
    - endpoints, state dir, lock, slot read and parse, the credentials-file match;
    - mode `reuse`: saved token → refresh → password; mode `password`: straight to the password grant;
    - the Q1 order per field, the hint, persisting after every token response, save errors as warnings;
    - `authorization` (proactive refresh) and `refresh_after_unauthorized` (refresh, then the password fallback);
    - `close()` wipes and unlocks.
  - **(rev1) Refusal mapping**, by `TokenRefusal`:
    - password grant: any refusal → 9 via `token_refusal_error`;
    - refresh grant: `session_ended` → the password fallback, or 8 "expired"; `bad_client` / `client_not_allowed` → 9.
  - **`logout`:** as in rev0, with Spec issue 9's rules. **(rev1, V3)** A revoke counts as successful on any 200; only a 4xx, 5xx or transport failure gives the warning.
  - **`FakePrompter`:** a scripted queue, a call log, scripted `interrupted`/`empty`/`too_long`.
- **Dependencies:** T5, T8, T9.
- **Done when:**
  - `check` and `-e gcc check` are green;
  - `nice pixi run tsan -R 'unit\.(login session|logout)'` is green;
  - the three reviewers approve.
- **Tests (unit):**
  - AC6 (logical grants);
  - AC13 core (prompt order and hint);
  - AC14/15 core (zero requests);
  - AC17 (rev1: wrong password at 401 **and** at 400 → 9, the session bytes unchanged);
  - AC18 (refresh 400 `invalid_grant` → silent password login / 8);
  - AC21 core (persisted before the interrupt);
  - AC22 core;
  - AC23 (fixture slot only);
  - AC25 core;
  - AC24 core (rev1: a revoke answered 200 with an error JSON → "logged out"; 503 → warning; malformed → removed plus warning);
  - credentials-file mismatch;
  - FakePrompter `too_long` → 8 with the Spec issue 8 text.

### T13: Collection fetch: follows pages and the status map
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `follows fetch:`.
- **Files:** create `src/core/follows_fetch.cpp` and `tests/unit/follows/follows_fetch_test.cpp`.
- **Owns:** exactly those 2 files.
- **Build files for build-fixer:** none.
- **What changes:** as in rev0:
  - `stop_source` → `ApiClient` (no reporter) → `InterruptBridge`;
  - per page: authorization, then `get_authorized`; on a 401, refresh and retry that page once; a second 401 → 9 "your follows";
  - other non-2xx → `request`; parse errors → `content`;
  - paginate's stepping rules;
  - the status map once, with ≥ 1 follow ("your reading statuses").
  - **(rev1, V5)** Cap: `{content, network "you follow more than 10000 titles on MangaDex; refusing to truncate the list"}`. The constant's doc names it the documented collection window.
- **Dependencies:** T10, T11.
- **Done when:**
  - `check` is green;
  - `nice pixi run tsan -R 'unit\.follows fetch'` is green;
  - the three reviewers approve.
- **Tests (unit, FakeTransport + a test-local `FakeTokenProvider`):** AC7, AC53, AC54, as in rev0. The URLs equal T10's builders and T11's literals.

### T14: Follows settings, plan, the `follows` JSON object, the runner overload and the claimed-registry test
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `follows plan:`, `batch json follows:`, `batch jobs follows:`.
- **Files:**
  - create `src/core/follows_plan.cpp`, `tests/unit/follows/follows_plan_test.cpp`, `tests/unit/batch/batch_json_follows_test.cpp` and `tests/cli/batch_jobs_follows_test.cpp`;
  - edit `src/core/batch_json.cpp`, `src/app/cmd_batch_jobs.hpp` and `src/app/cmd_batch_jobs.cpp`.
- **Owns:** exactly those 7 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`validate_follows_args`**, in this order:
    1. `DIR must not be empty`;
    2. DIR is a manga or list id → the spec text;
    3. **(rev1)** `check_raw_auth_args`;
    4. `detail::validate_common`;
    5. `StatusFilter::parse`.
  - **`plan_follows`.**
  - **The JSON overload:** `"follows"` right after `"source"`.
  - **The `run_batch_jobs(…, const FollowsSummary&)` overload.**
- **Dependencies:** T10, T1.
- **Done when:**
  - `check` is green;
  - `unit.batch*`, `cli.batch*`, `cli.mdlist*`, `e2e.batch.*` and `e2e.mdlist.*` pass unmodified;
  - the three reviewers approve.
- **Tests:**
  - AC11 (unit);
  - AC19 (unit half), plus **(rev1)** `--credentials ''` → 2;
  - **cli:** the follows prefix, wording, closing line and duplicate.
  - **(rev1, V27)** two follows jobs producing the same path, with `--overwrite`: the second row's run receives the registry (`RowContext::claimed` non-null), never replaces the first row's file, and prints mdlist's collision line. This mirrors the mdlist test from commit 2a67cc0, over the `clean-twin` tree (`tests/support/mdx_twin.hpp`) with `BatchSource::follows`.

### T15: Follows fixture trees for the cli and E2E tiers
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `follows fixture:`.
- **Files:** create `tests/support/mdx_follows_fixture.hpp` and `tests/unit/support/mdx_follows_fixture_test.cpp`; edit `tests/e2e/fixtures/fixture_gen.cpp` and `tests/e2e/inputs/README`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`follows_fixtures()`,** separate from `all_fixtures()`, which stays at twelve. The trees:
    - `follows-clean` (F1);
    - `follows-unknown` (F2);
    - `follows-empty` (F3);
    - `follows-twice` (F4);
    - `follows-bad-id` (F5);
    - `follows-bulk` (F6: 2 pages of page size 1);
    - `follows-nochapters` (F7);
    - `follows-status` (FS1);
    - `follows-status-empty` (FS2);
    - `follows-status-bad-value` (FS3a);
    - `follows-status-bad-key` (FS3b).
  - **(rev1) Auth variants**, through `FixtureAuth`:
    - a second user `fixture-user-400`, whose wrong-password answer is 400;
    - a user with a scripted `unauthorized_client` refusal;
    - a revoke script that answers 503.
  - **`creds/` per tree:** `credentials` (0600, all four keys), `credentials-client` (0600, no password), `credentials-wrong-password` (0600), `credentials-0644`, `password.txt` and `password-wrong.txt`.
  - **(rev1, V29)** `fixture_gen` links only nlohmann (`tests/CMakeLists.txt:25-56`), so the creds are written by **raw POSIX helpers inside `mdx_follows_fixture.hpp`**: `openat(AT_FDCWD, …, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, mode)`, a write loop, then `fchmod(mode)`. No `std::ofstream` (it gives `0666 & ~umask`) and no core `StateDir`.
  - **`fixture_gen.cpp`:** writes `follows_fixtures()`.
  - **README:** documents the trees, that every value is fake, and **(rev1)** that seeding cases run `PRE_RUN login --credentials …`, because PRE_RUN has stdin `/dev/null` and no TTY.
- **Dependencies:** T6, T10, T11.
- **Done when:**
  - `check` is green;
  - `e2e.fixtures` stays within its 120 s timeout;
  - the twelve existing trees are byte-identical;
  - the three reviewers approve.
- **Tests (unit):**
  - every tree writes;
  - the creds modes are exactly 0600 and 0644 under umask 077 and 022;
  - each follows set served and parsed;
  - FS3a/FS3b → malformed.

### T16: App auth glue (options, the login phase, prompter choice) and the `StderrTrace` extraction
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `auth phase:`.
- **Files:** create `src/app/cmd_auth.hpp`, `src/app/cmd_auth.cpp`, `src/app/cmd_trace.hpp`, `src/app/cmd_trace.cpp` and `tests/cli/auth_phase_test.cpp`; edit `src/app/cmd_mdlist.cpp` (use the extracted trace).
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cmd_trace`:** `StderrTrace` moved verbatim.
  - **(rev1, V10, V36) `bind_auth_options(CLI::App&, RawAuthArgs&) -> CLI::Option*`:**
    - adds `--credentials FILE`, bound to the **`std::string`** `RawAuthArgs::credentials` (no default, not ExistingFile), and returns its `Option*`;
    - the Command keeps that pointer, borrowed from the `CLI::App`, which outlives dispatch. In `run()` it sets `credentials_given = option->count() > 0` before anything reads the args;
    - adds `--password-stdin` and `--no-input`;
    - no `disable_flag_override` (Spec issue 5).
  - **`user_dirs(const EnvSnapshot&)`.**
  - **`run_login_phase`** and **`guard_secrets`,** as in rev0. `run_login_phase` uses `credentials_given ? credentials : default_credentials_path` and the explicit/default distinction.
- **Dependencies:** T7, T9, T12, T15.
- **Done when:** `check` green; `cli.mdlist*` and `e2e.mdlist.*` unmodified; the three reviewers approve.
- **Tests (cli):**
  - AC14, AC15, AC16, AC17, AC18 and AC25;
  - **AC26 (cli, rev1):** a lock held by a second fd → exit 1 at once (InstantClock; the wait-then-succeed case is T5's);
  - the TTY rule;
  - `--password-stdin` via a pipe;
  - **(rev1)** `--credentials` naming a missing file `--no-input` → exit 1 with that path, and no prompt.

### T17: `login` and `logout` commands (not yet registered)
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `login cli:`.
- **Files:** create `src/app/cmd_login.hpp`, `src/app/cmd_login.cpp` and `tests/cli/login_cli_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:** as in rev0, plus:
  - **(rev1) Phase 1 for `login`:** `check_raw_auth_args`, then `check_auth_api_base`.
  - **Help footers** (Spec issue 12).
  - **`logout` messages:** `logged out; removed <path>`; `not logged in (no saved login at <path>)`; the revoke warning when the revoke failed (4xx, 5xx or transport).
- **Dependencies:** T16.
- **Done when:**
  - `check` is green;
  - `nice pixi run tsan -R 'cli\.login cli'` is green;
  - the three reviewers approve.
- **Tests (cli):**
  - AC24 (rev1: a revoke answered 200 with an error body → "logged out");
  - AC22 (the login half);
  - the interrupt at the password prompt (AC21);
  - `--json` → empty stdout;
  - AC23.

### T18: `follows` command (not yet registered)
- **Size:** L. **Security-reviewer:** yes. **Test prefix:** `follows cli:`, `follows no leak:`.
- **Files:** create `src/app/cmd_follows.hpp`, `src/app/cmd_follows.cpp`, `tests/cli/follows_cli_test.cpp` and `tests/unit/follows/follows_no_leak_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:** as in rev0:
  - the bind order: DIR, shared options, `--fail-fast`, `--dir-per-title`, `--status`, then `bind_auth_options` (the Command keeps the `Option*`);
  - the footer and description;
  - the phases: validate → handlers → guard → agent → backend → trace → login phase → collection → `close()` → filter, `-v`, empty warnings → `run_batch_jobs(…, follows_summary)`.
- **Dependencies:** T13, T14, T16.
- **Done when:**
  - `check` and `-e gcc check` are green;
  - `nice pixi run tsan -R '(cli|unit)\.follows (cli|no leak)'` is green;
  - the three reviewers approve, with the rev0 evidence requirement.
- **Tests:**
  - **unit (`follows_no_leak_test.cpp`), AC8:** as in rev0. Also: any request off the allowlist with an authorization would trip T6's fixture, which the test asserts never happens.
  - **cli:**
    - AC13, AC19 (validate level), AC20, AC21, AC22 (follows half), AC55–AC58, `/dev/full`, color;
    - **(rev1, gate) a DIR that holds the user's password:** `RawFollowsArgs{.defaults.dir = "hunter2", .auth.password_stdin = true}`. With an empty stdin → 2; with no session and no TTY → 8; with a wrong password through stdin → 9; with a fixture collection failure → 3. In each, `hunter2` appears **nowhere on stderr**. The argv form is in T19.

### T19: Registration, echo policy, exit rows 7–9, footers and every help golden (the single bless point)
- **Size:** M. **Security-reviewer:** yes. **Test prefix:** `follows parse:`.
- **Files:**
  - edit `src/app/cli.cpp`, `src/app/exit_help.hpp`, `src/app/cmd_mdlist.cpp` (the Spec issue 7 line) and `tests/cli/{cli_test,download_cli_test,batch_parse_test,mdlist_parse_test}.cpp`;
  - create `tests/cli/follows_parse_test.cpp` and `tests/e2e/cases/{follows,login,logout}.cmake` (help and dry-run golden cases only).
- **Owns:** exactly those 11 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cli.cpp` registration:** `register_follows`, `register_login` and `register_logout` after mdlist; the commands list `download, batch, mdlist, follows, login, logout`; `title_option_owner` covers `follows` (Spec issue 4).
  - **(rev1, gate) Echo policy in `failure_text`**, applied when the reached subcommand is `follows`, `login` or `logout`:
    1. **ExtrasError** (from `remaining(true)`). Each token is classified:
       - **secret-shaped**: a prefix of `--password`, `--client-secret` or `-p` (`-p`, `-phunter2`, `--password=x`, `--client-secret-x`). If any token is secret-shaped, print the spec's fixed password message and nothing else;
       - **a single-dash word longer than 2 characters** (`-qphunter2`, `-xyz`): counted, never echoed;
       - **an option-looking word** (`--name[=…]`, or exactly `-x`): echoed as the part before `=`, through `terminal_safe`, cut to 64 bytes;
       - **anything else** (a bare word, an empty word, invalid UTF-8): counted.
       - The message is `<cmd> does not take these arguments: <names, comma-separated> (and N other words, not shown in case they hold a password)`. The parenthesis appears only when N > 0. With no names, it is `<cmd> does not take N more argument(s) (not shown in case they hold a password)`. Then the hint.
    2. **ConversionError (V35):**
       - the name is parsed from `Could not convert: <name> = …`;
       - the name must match `^--?[A-Za-z0-9][A-Za-z0-9-]*$`;
       - look the name up with `get_option_no_throw` on the reached subcommand, then on the root;
       - a flag (`get_expected_max() == 0`) → `<name> takes no value`;
       - a value option → `<name>: invalid value (not shown)`;
       - anything unparsable → `<cmd>: an option was given a value it can't take (the value is not shown)`.
    3. **Every other ParseError** keeps its stock text (ValidationErrors of `--jobs`, `--layout`, `--color`: gate).
  - **`exit_help.hpp`:** row 7 `several titles failed in different ways (see per-title results)`; rows 8 and 9 as in the spec.
  - **`cmd_mdlist.cpp`:** `To download the titles you follow, see 'mangadex-dl follows --help'.`
  - **Pinned tests:** `cli_test.cpp:66, :85`; `download_cli_test.cpp:446`; `batch_parse_test.cpp:228`; `mdlist_parse_test.cpp:223, :229`.
  - **`follows.cmake`:** `help`, `dry-run` and `json-dry-run` (GOLDEN): `follows out --dry-run --credentials {{FIXTURES}}/follows-clean/creds/credentials --api-base file://{{FIXTURES}}/follows-clean`.
  - **`login.cmake` / `logout.cmake`:** `help` (GOLDEN). **(rev1, V9c)** The logout golden's OPTIONS block lists the inherited globals (`-h,--help`, `--version`, `--json`, `-q`, `-v`, `--color`) and no `--api-base`. That is CLI11's stock layout, and nothing is suppressed.
- **Dependencies:** T17, T18.
- **Done when:** the human has blessed (one round); `check` green; the three reviewers approve.
- **Tests (cli, `follows_parse_test.cpp` through `run_cli`)**, each asserting `hunter2` absent from stderr and the exact exit 2:
  - `follows --password hunter2`, `--password=hunter2`, `-p hunter2`, `-phunter2`, `-qphunter2`;
  - `follows out hunter2`;
  - `--no-input=hunter2`, `--password-stdin=hunter2`, `--dry-run=hunter2`, `--json=hunter2` (→ `<flag> takes no value`);
  - `--client-secret=hunter2`;
  - `login hunter2`, `logout hunter2`;
  - **(rev1, gate)** `follows --password-stdin hunter2` with stdin closed → 2 `--password-stdin was given, but stdin held no password`, with `hunter2` (now DIR) never echoed;
  - `--unknown-opt=hunter2` → `--unknown-opt` echoed, `hunter2` not;
  - `--jobs 9` → the stock CLI11 text (unchanged).
  - **Other cases:**
    - `--title X`, `--title=X`;
    - globals after DIR;
    - `--status reading out`;
    - `login --status reading` → 2;
    - the root help lists the three one-liners;
    - no `--api-base` in any help;
    - logout help as V9c;
    - smokes.

### T20: E2E cases and edge-suite additions
- **Size:** L. **Security-reviewer:** no (the board covers it).
- **Files:** append to `tests/e2e/cases/{follows,login,logout}.cmake` and `tests/e2e/cases/_edge.cmake`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **Dependencies:** T19.
- **Done when:** `check` and `release-test` are green; `quality-reviewer` and `memory-safety-reviewer` approve.
- **Tests:** the tables below.

### T21: README
- **Size:** M. **Security-reviewer:** yes.
- **Files:** edit `README.md`.
- **Owns:** `README.md`.
- **Build files for build-fixer:** none.
- **What changes:** as in rev0, plus:
  - **(rev1)** the follows cap wording ("the window MangaDex documents for collection endpoints; refused, not truncated");
  - logout's "MangaDex accepts the revoke even for an already-ended session";
  - the echo policy: "values typed on the command line are never repeated in error messages";
  - `--credentials` takes the next word verbatim;
  - "a mistyped password after `--password-stdin` becomes DIR; it is never repeated in a login error, but files go into that directory if the run succeeds".
- **Dependencies:** T19.
- **Done when:** `check` green; `quality-reviewer` and `security-reviewer` approve; `memory-safety-reviewer` confirms "no C++ changed".

---

## Parallelization plan

**Batch 1 (parallel): T1, T2, T3 · Batch 2 (parallel): T4, T5, T6, T7 · Batch 3 (parallel): T8, T9, T10, T11 · Batch 4 (parallel): T12, T13, T14, T15 · Batch 5: T16 · Batch 6 (parallel): T17, T18 · Batch 7: T19 (cli.cpp registration + exit_help + bless) · Batch 8 (parallel): T20, T21**

| Batch | Tracks (size) | Why parallel-safe |
|---|---|---|
| 1 | T1 (M), T2 (S), T3 (XS) | T1 is the **only** track touching `include/mangadex_dl/*`, `src/app/io.hpp` and (rev1) `cmd_batch_jobs.cpp:166`. T2 implements T1's Secret declarations in new files. T3 is docs |
| 2 | T4 (M), T5 (L), T6 (L), T7 (M) | **The risky foundations, reviewed first**, in disjoint files |
| 3 | T8 (L), T9 (M), T10 (M), T11 (S) | T11 inlines its URL literals so it doesn't link against T10 |
| 4 | T12 (L), T13 (M), T14 (M), T15 (M) | T13 uses a fake TokenProvider; T14 is batch JSON/plan; T15 is test support |
| 5 | T16 (M) | Glue every command needs |
| 6 | T17 (M), T18 (L) | Disjoint `cmd_login.*` / `cmd_follows.*` |
| 7 | T19 (M) | The late registration task: sole owner of `cli.cpp` and `exit_help.hpp` |
| 8 | T20 (L), T21 (M) | E2E vs README |

At most four tracks run at once, all sharing `build/default/dev` under the lock. A compile error in a file another track owns is a coordination event.

**Serialization points and their single owner:**

| Serialization point | Task (batch) |
|---|---|
| `include/mangadex_dl/{error,exit_codes,api_client,batch}.hpp`, `net/{transport,fixture_transport}.hpp`, new `secret.hpp`, `prompt.hpp`, `session_store.hpp`, `auth.hpp`, `follows.hpp` | T1 (B1) |
| `src/app/io.hpp` (+ `io.cpp`) | T1 (B1) |
| `src/app/cli.cpp`, `src/app/exit_help.hpp` | T19 (B7) |
| `tests/e2e/cases/_edge.cmake` | T20 (B8) |
| top-level `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake`, `scripts/*`, `.github/workflows/*` | none |

**Other shared files, one owner per batch:**

| File | Owners, by batch |
|---|---|
| `src/app/cmd_batch_jobs.cpp` | T1 (B1), T14 (B4) |
| `src/app/cmd_batch_jobs.hpp`, `src/core/batch_json.cpp` | T14 (B4) |
| `src/core/batch_run.cpp` | T1 (B1) |
| `src/app/cmd_mdlist.cpp` | T16 (B5), T19 (B7) |
| `src/core/api_client.cpp` | T11 (B3) |
| `src/core/net/curl_transport.cpp`, `curl_api.*` | T4 (B2) |
| `src/core/net/fixture_transport.cpp` | T6 (B2) |
| `tests/support/fake_transport.hpp` | T4 (B2) |
| `tests/support/mdx_auth_fixture.hpp` | T6 (B2) |
| `tests/support/mdx_follows_fixture.hpp`, `tests/e2e/fixtures/fixture_gen.cpp`, `tests/e2e/inputs/README` | T15 (B4) |
| `tests/support/fake_prompter.hpp` | T12 (B4) |
| `tests/unit/interfaces/headers_test.cpp` | T1 (B1) |
| `tests/cli/{cli_test,download_cli_test,batch_parse_test,mdlist_parse_test}.cpp` | T19 (B7) |
| `tests/e2e/cases/{follows,login,logout}.cmake` | T19 (B7), T20 (B8) |
| `README.md` | T3 (B1), T21 (B8) |
| `docs/design/list-spec.md` | T3 (B1) |

**`security-reviewer` in the track loop:** T1, T2, T4–T19 and T21. That is every task except T3 and T20.

---

## Test plan (per tier)

### AC → task → tier

| AC | Task | Tier | AC | Task | Tier |
|---|---|---|---|---|---|
| 1 | T6, T8 | unit | 27 | T19 | e2e golden |
| 2 | T9 | unit | 28–33 | T20 (33's goldens: T19) | e2e |
| 3, 4 | T8 | unit | 34–38 | T20 | edge |
| 5 | T9 | unit | 39, 40 | T7 (pty), cli-tester | cli + tty-pty |
| 6 | T12 | unit | 41–49 | — | human |
| 7 | T13 | unit | 50–52 | T10 | unit |
| 8 | T11, T18 | unit | 53, 54 | T13 | unit |
| 9 | T4 | unit | 55–58 | T18 | cli |
| 10 | T6 | unit | 59 | T18, T19 | cli |
| 11 | T14 | unit, cli | 60 | T20 | e2e |
| 12 | T1 | unit | 61 | T20 | edge |
| 13 | T12, T18 | unit, cli | 62 | — | human |
| 14–18 | T12, T16 | unit, cli | | | |
| 19 | T14, T18, T19 | unit, cli | | | |
| 20, 21 | T18 (21: also T7, T17) | cli | | | |
| 22 | T12, T16, T17, T18 | unit, cli | | | |
| 23, 24 | T12, T17 | unit, cli | | | |
| 25, 26 | T5, T12, T16 | unit, cli | | | |
| (rev1) same-run `--overwrite` protection for follows | T14 | cli | | | |

### Unit (Catch2 against `mangadex_dl::core`)

| File | Task | Covers |
|---|---|---|
| `tests/unit/interfaces/headers_test.cpp` | T1 | 8/9 mapping, traits, constants |
| `tests/unit/secret/{secret,process_guard}_test.cpp` | T2 | wipe, move, growth; dump guard in a child |
| `tests/unit/net/curl_transport_auth_test.cpp` | T4 | AC9, POSTFIELDS, the slist |
| `tests/unit/auth/{session_store,private_file}_test.cpp` | T5 | modes, symlinks (ENOTDIR), atomic write, lock |
| `tests/unit/auth/form_test.cpp`, `tests/unit/net/fixture_transport_auth_test.cpp` | T6 | AC1, AC10 (401/400 variants, revoke 200) |
| `tests/unit/auth/{token,auth_client}_test.cpp` | T8 | AC1, AC3, AC4 (classification on `error` only) |
| `tests/unit/auth/{credentials_file,session_json,auth_inputs}_test.cpp` | T9 | AC2, AC5, stdin, `--credentials ''`, API base |
| `tests/unit/follows/{follows_page,status_map,status_filter}_test.cpp` | T10 | AC50–52, the 100 001-entry cap, duplicate keys |
| `tests/unit/api_client/api_client_authorized_test.cpp` | T11 | the allowlist, AC8 (client) |
| `tests/unit/auth/{login_session,logout}_test.cpp` | T12 | AC6, AC13–18, AC21–25 (core) |
| `tests/unit/follows/follows_fetch_test.cpp` | T13 | AC7, 53, 54 |
| `tests/unit/follows/follows_plan_test.cpp`, `tests/unit/batch/batch_json_follows_test.cpp` | T14 | AC11, AC19 (validate) |
| `tests/unit/support/mdx_follows_fixture_test.cpp` | T15 | trees, exact modes |
| `tests/unit/follows/follows_no_leak_test.cpp` | T18 | AC8 (full) |

Every existing `unit.*` test stays green **unmodified** (except `headers_test.cpp`).

### In-process CLI (`tests/cli/`, flat)
- **`auth_prompt_test.cpp` (T7):** pty echo, 4095-byte line, interrupt, POLLHUP; pipe too-long, EOF and unavailable; a loud `SKIP` without a pty.
- **`batch_jobs_follows_test.cpp` (T14):** wording, and **(rev1)** the claimed registry under `--overwrite`.
- **`auth_phase_test.cpp` (T16):** AC14–18, 25, 26; the TTY rule; `--password-stdin`; the `--credentials` verbatim value.
- **`login_cli_test.cpp` (T17):** AC21, AC22, AC23, AC24.
- **`follows_cli_test.cpp` (T18):** AC13, 19, 20, 21, 22, 55–58, `/dev/full`, color, and the DIR-holds-the-password cases.
- **`follows_parse_test.cpp` (T19):** the echo policy (every `hunter2` form), `--title`, globals, help, AC59, logout help.
- **Edited in T19:** `cli_test.cpp`, `download_cli_test.cpp`, `batch_parse_test.cpp`, `mdlist_parse_test.cpp`.
- **(rev1) TSan filters**, case-sensitive against the prefixes:
  - T6: `-R 'unit\.fixture auth'`;
  - T7: `-R 'cli\.prompt'`;
  - T12: `-R 'unit\.(login session|logout)'`;
  - T13: `-R 'unit\.follows fetch'`;
  - T17: `-R 'cli\.login cli'`;
  - T18: `-R '(cli|unit)\.follows (cli|no leak)'`;
  - board: `nice pixi run tsan -R '(cli|unit)\.(follows|login|logout|auth|prompt|session|token|fixture auth|batch|mdlist)'`.

### E2E cases (tests `e2e.follows.*`, `e2e.login.*`, `e2e.logout.*`)
Fixture mode `--api-base file://{{FIXTURES}}/<tree>` throughout. `C` stands for `--credentials {{FIXTURES}}/<tree>/creds/credentials`. DIR is `out`. Exit codes are exact.

| Case | Task | Asserts |
|---|---|---|
| `follows.help`, `login.help`, `logout.help` | T19 | 0, stderr empty, no ANSI, GOLDEN (AC27); logout lists the inherited globals (V9c) |
| `follows.dry-run`, `follows.json-dry-run` | T19 | `follows-clean` + C `--dry-run` → 0, `out` absent, GOLDEN (AC33) |
| `follows.no-session` | T20 | stdin NULL, no C → 8, the exact message, stdout empty, `out` absent (AC28) |
| `follows.no-client` | T20 | → 8 "no MangaDex API client is set up…" |
| `follows.creds-0644` | T20 | `--credentials …/creds/credentials-0644` → 1, the chmod hint, no fixture value (AC29) |
| `follows.creds-empty` (rev1) | T20 | `--credentials ''`: not expressible (an empty ARGS item); covered at cli (T14, T16). Documented, no case |
| `follows.clean`, `follows.json`, `follows.dir-per-title` | T20 | AC30 |
| `follows.seeded` | T20 | `PRE_RUN login C --api-base …`, then `follows out` with no C → 0 |
| `follows.unknown`, `follows.nochapters`, `follows.empty`, `follows.twice` | T20 | 4 with `follows:2: 6666…:`; 7; 0 plus "you follow no titles" plus `out` absent; the duplicate warning (AC31) |
| `follows.bad-id` | T20 | → 3, malformed, **no** remedy, `out` absent |
| `follows.password-stdin`, `follows.password-stdin-null` | T20 | `credentials-client` + `STDIN {{FIXTURES}}/follows-clean/creds/password.txt --password-stdin` → 0; `STDIN NULL` → 2 (AC32) |
| `follows.wrong-password`, `follows.wrong-password-400` (rev1) | T20 | `credentials-wrong-password` → 9, the password message, for both the 401 user and `fixture-user-400` |
| `follows.status-reading` | T20 | `follows-status` + C `--status reading` → 0, clean's paths (AC60) |
| `follows.status-json` | T20 | `--status reading,completed --json` → 4, `EXPECT_JSON`, `"selected": 2` (AC60) |
| `follows.status-dry-run` | T20 | `--dry-run --status reading` → 0, `out` absent (AC60) |
| `follows.status-bad`, `follows.status-all` | T20 | `--status paused` / `all` → 2, the exact text, stdout empty |
| `follows.usage-*` | T20 | `--api-base https://example.org` → 2; a uuid DIR → 2; `--password hunter2`, `-phunter2`, `--no-input=hunter2` → 2 with **`hunter2` not matched on stderr** (the regex asserts its absence through an exact stderr regex) |
| `login.ok`, `login.no-tty` | T20 | `login C` → 0, stdout empty, "logged in … saved in"; `login` alone, stdin NULL → 8 |
| `logout.after-login`, `logout.twice`, `logout.revoke-fails` (rev1) | T20 | `PRE_RUN login C`, then `logout` → 0 "logged out; removed"; no session → 0 "not logged in"; the 503-revoke tree → 0 with the warning |

### Edge-suite additions (`_edge.cmake`, T20 only; tests `e2e.edge.follows-*`)

| Case | Asserts |
|---|---|
| `follows-stdin-null-session`, `follows-stdin-closed-session` | `PRE_RUN login C`, then `--dry-run` with `STDIN NULL` / `CLOSED` → 0 (AC34) |
| `follows-stdin-closed-no-session` | → 8, never 70, no hang |
| `follows-password-stdin-closed` | → 2 |
| `follows-dev-full` | C `--dry-run`, `STDOUT_FILE /dev/full` → 1, `^mangadex-dl: write error` (AC35) |
| `follows-sigpipe`, `follows-sigpipe-parent-ignores` | `follows-bulk` + C `--dry-run --layout chapters`, `PIPE_HEAD 1` → SIGPIPE, stderr empty (AC36) |
| `follows-json-clicolor-force`, `follows-pipe-stderr-no-ansi` | AC37 |
| `follows-invalid-utf8-dir-json` | AC38 |
| `follows-invalid-utf8-extra` | `follows out {{INVALID_UTF8}}` → 2, the bytes not echoed (counted), not 70 (AC38) |
| `follows-status-invalid-utf8-lc-c` | AC61 |
| `follows-status-json-clicolor-force` | AC61 |
| `login-stdin-null` | → 8 |

- **TTY cases:** the existing driver skips them with 77 when `script` is absent (V18e). Prompt typing stays with `cli-tester` and the human (AC39, AC40, AC47).
- **(rev1, V32)** No E2E case asserts "wait for the lock, then succeed": with InstantClock, every lock wait times out at once.

### Sanitizer-control additions
None. Evidence for `memory-safety-reviewer` **(rev1 wording, V20c)**:
- In the dev preset, the ASan/LSan leak and heap controls and the UBSan `signed_overflow` control fired in the T2, T12 and T18 gate runs.
- In the tsan preset, the `data_race` control fired. The tsan preset has no leak detection, so leaks are licensed only by the ASan/LSan run.
- V20 additionally confirmed that LSan, ASan and UBSan still report and exit 86 after `PR_SET_DUMPABLE 0`.

### Release-test
- **T2:** the glibc floor with `explicit_bzero`.
- **T4:** the curl changes.
- **T20 and the board:** every new E2E/edge case on the hardened install.
- The gcc env runs in T1, T4, T5, T7, T12 and T18, and on the board.

### Goldens that will change (human-blessed, **one bless point, after T19**)

| Golden | Why |
|---|---|
| `edge.help` | root SUBCOMMANDS gains follows, login and logout; exit row 7 reworded, rows 8 and 9 added |
| `download.help`, `batch.help` | exit rows 7–9 |
| `mdlist.help` | exit rows 7–9; the line `To download the titles you follow, see 'mangadex-dl follows --help'.` |
| `follows.help` (new) | the new command |
| `login.help`, `logout.help` (new) | the new commands (logout with the inherited globals) |
| `follows.dry-run`, `follows.json-dry-run` (new) | planned paths, and batch's document plus `follows` |

**Bless request** (T19): `pixi run bless '^e2e\.((edge|download|batch|mdlist|follows|login|logout)\.help|follows\.(json-)?dry-run)$'`.

**Must not change:** `edge.version`, `download.dry-run-clean`, `download.json-dry-run`, `batch.dry-run`, `batch.json-dry-run`, `mdlist.dry-run` and `mdlist.json-dry-run`.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**Secrets**
1. **`Secret` (T2):**
   - the SSO wipe-after-move (rev1: bytes 1..n−1 survive a plain move);
   - `resize(capacity())` never reallocates;
   - growth wipes the old buffer;
   - `reveal()` is never stored or bound to a temporary.
2. **Plain-string copies, each wiped before it dies:**
   - `HttpRequest.body` and `.authorization` (T8, T11);
   - **(rev1)** the non-const `request` in `perform`, wiped by a scope guard;
   - the curl slist (T4);
   - the nlohmann DOM strings, wiped via `get_ptr<std::string*>()`, with the DOM never copied (T8, T9);
   - the response body (T8);
   - `read_bounded_fd`'s result (T9).

   `std::format` never receives `reveal()`.
3. **(rev1) `CURLOPT_POSTFIELDS`** borrows `request.body` **until `curl_easy_reset`**. The lease (which resets) dies inside `fetch()`, before the caller's request does.
   - The declaration order `state` → `headers` → `lease` makes the reset run before `SlistFree` wipes and frees.
   - The new `setopt_borrowed` documents "not copied".
   - `CURLOPT_VERBOSE` is never set.

**File descriptors and file-system races (T5)**

4. **`StateDir`, `SessionLock` and the temp file:**
   - `UniqueFd` and `O_CLOEXEC` everywhere;
   - the temp file is unlinked on every error path;
   - the `renameat`/`fsync` order;
   - a moved-from object never closes a live fd;
   - **(rev1)** the lock fd is never `dup`ed: a dup would share the lock and keep it alive past `SessionLock`.
5. **The bounded read into a reserved Secret:** the `cap + 1` math is in `size_t`.

**Terminal (T7)**

6. **The termios restorer** restores on every path. The stack buffer is wiped. Reads are bounded at 4097. The pty test's writer thread joins before the fds close.

**Ownership in the login flow (T12, T16, T18)**

7. **`LoginSession`** (a `unique_ptr`) borrows the Backend's transport and clock and the `StderrTrace` declared before it, and owns `AuthClient`, `StateDir`, `SessionLock` and `Session`.
   - `close()` runs before downloads.
   - `TerminalPrompter` outlives every `ask`.
   - `Io::prompter` outlives the run.
8. **`TokenProvider&`** is passed into the collection fetch for the call only, on the calling thread. No token reaches `run_download`, `FetchPool` or the reporter.
9. **The collection fetch:** `stop_source` → `ApiClient` (no reporter) → `InterruptBridge`; the bridge is joined before return.

**Untrusted input**

10. **Parsers:** `form_decode`'s `%` bounds; the status-map, follows-page, token, session and credentials-file parsers keep no views into their input; counts are checked with `>` before `push_back`.
11. **The fixture counter map:** owned keys under a mutex; names validated before `openat`.

**CLI11 bindings**

12. **Commands:**
    - Each Command owns its raw args.
    - **(rev1)** The Command stores the `CLI::Option*` returned by `bind_auth_options`. It is borrowed from the `CLI::App` in `run()`, which is declared after `commands` and destroyed first, but only after dispatch returns. `count()` is read in `run()`, before the app dies. The pointer is never used after `run()` returns.
    - The callbacks capture `[slot = &selected, self = command.get()]`.
13. **(rev1) `failure_text`'s echo policy** works on copies of `remaining(true)` and on `std::string_view`s into `error.what()`, used only within the call.

**Process-wide state**

14. **`disable_core_dumps`:** once, on the main thread, before any secret is read.
15. **`EnvSnapshot`:** owned copies, read once before any thread starts.

---

## Rollout & risk

**CLI contract**
- `download`, `batch` and `mdlist` keep every flag, exit code, stdout byte and `--json` byte.
- **Changed text:**
  - every `--help` gets the reworded row 7 and new rows 8 and 9;
  - the root SUBCOMMANDS list and the `unknown command (…)` list;
  - mdlist's footer line.
- **New:**
  - exit codes 8 and 9;
  - `follows [DIR]` with `--status`, `--credentials`, `--password-stdin` and `--no-input`;
  - `login` and `logout`;
  - **(rev1)** for these three commands, CLI11 error texts that would echo a value are replaced by value-free ones.
- **New on-disk state:** `$XDG_STATE_HOME/mangadex-dl/{session.json,session.lock}`.
- **The PR body** calls out:
  - exits 8 and 9;
  - the state dir;
  - the three env vars;
  - the dump guard;
  - the echo policy;
  - **(rev1)** that follows inherits mdlist's same-run `--overwrite` protection.

**Riskiest tasks**
1. **T5 and T12.** De-risked by the byte-level store tests, persist-before-anything, FakeTransport POST counts (never retry a 4xx) and three reviewers each.
2. **T4.** "Empty authorization = identical options", the loopback tests, `release-test`, and **(rev1)** the borrowed-body lifetime pinned by the declaration-order review.
3. **T6.** A bounded parser, `O_NOFOLLOW` walks, the tripwire.
4. **(rev1) T19's echo policy** depends on CLI11's ConversionError text format (V35). It is pinned by `follows_parse_test.cpp` against the installed CLI11; a CLI11 upgrade that changes the text falls back to the generic value-free message, never to an echo.
5. **(rev1) The V27 site:** without `claims_paths`, follows would silently lose the overwrite protection. T14's test fails if it regresses.

**Behaviour choices:** all settled at the gate (Spec issues 1–13, echo policy, no DIR guard).

**Draft wording for the cli-ux board:**
- the follows/login/logout footers;
- rows 7–9;
- the extras message and `<flag> takes no value`;
- the `--title` message;
- the over-long prompt answer;
- the logout warnings;
- `--credentials needs a file name`;
- the `-v` lines.

---

## Refactor triggers

**rev0 items, still standing.** Every rev0 item is now CONFIRMED or folded in, so only conditional triggers remain:

| [VERIFY] | If a later run (CI, a CLI11 upgrade) refutes it, these tasks change |
|---|---|
| V2 (error-code-only classification) | T8 table, T6 fixture bodies, T12 mapping; no API change |
| V12/V13/V14 (curl semantics) | T4 falls back to `COPYPOSTFIELDS` / drops the slist wipe; watch-list notes the libcurl copy |
| V18e (pty on CI) | T7's pty cases Skip on CI → AC39/40 stay cli-tester + human; recorded in the User Test Plan |
| V20/V21 on the CI kernel or LSM | T2: `RLIMIT_CORE` only (`DumpGuard::no_core_only`); T21 wording |
| V24 (separator arithmetic) | T10 constants only |
| V29 (fixture modes) | T20 positive cases move to the cli tier (the spec's fallback) |

**New in rev1:**

| [VERIFY] | If refuted, these tasks change |
|---|---|
| V35 (ConversionError text and option lookup) | T19 uses only the generic value-free text for every ConversionError on the three commands (no name), and adds `->disable_flag_override()` on `--no-input`/`--password-stdin` if that produces a value-free error; `follows_parse_test.cpp` assertions follow |
| V36 (`Option::count()` and pointer lifetime) | T16 detects "given" with an `->each()` callback setting `credentials_given` (bound to a Command member), instead of `count()` |
| V37 (Catch2 SKIP → ctest Skipped) | T7: when `posix_openpt` fails, a pty test FAILs instead of skipping (red, per CLAUDE.md rule 2); the orchestrator records the environment limit |

---

## Relevant paths

- **Spec:** `/home/user/Mangadex-dl/docs/design/follows-spec.md` (rev3 edits in "Spec text to change").
- **Fact-check:** `/home/user/Mangadex-dl/docs/design/follows-factcheck.md`.
- **Precedent:** `/home/user/Mangadex-dl/docs/design/list-plan.md`, `/home/user/Mangadex-dl/docs/design/list-spec.md`.
- **Frozen headers (T1):**
  - `/home/user/Mangadex-dl/include/mangadex_dl/{error,exit_codes,api_client,batch}.hpp`;
  - `/home/user/Mangadex-dl/include/mangadex_dl/net/{transport,fixture_transport}.hpp`;
  - new `/home/user/Mangadex-dl/include/mangadex_dl/{secret,prompt,session_store,auth,follows}.hpp`;
  - `/home/user/Mangadex-dl/src/app/io.hpp`.
- **Code sites cited in rev1:**
  - `/home/user/Mangadex-dl/src/app/cmd_batch_jobs.cpp:166` (V27);
  - `/home/user/Mangadex-dl/src/core/net/curl_transport.cpp:356-388, :399, :424-426`;
  - `/home/user/Mangadex-dl/src/core/net/curl_api.hpp`;
  - `/home/user/Mangadex-dl/src/core/net/fixture_transport.cpp:105`;
  - `/home/user/Mangadex-dl/src/core/interrupt.cpp:124`;
  - `/home/user/Mangadex-dl/src/core/net/clock.cpp`;
  - `/home/user/Mangadex-dl/src/core/fs/output_dir.cpp:243/246/261`;
  - `/home/user/Mangadex-dl/src/app/cmd_download_run.cpp:90`;
  - `/home/user/Mangadex-dl/src/app/cmd_download_options.cpp:27-32`;
  - `/home/user/Mangadex-dl/tests/CMakeLists.txt:25-56`;
  - `/home/user/Mangadex-dl/tests/e2e/run_case.cmake:76, :82-87`;
  - `/home/user/Mangadex-dl/tests/unit/api_client/api_client_test.cpp:304-321`.
- **Tests pinning text that T19 changes:** `/home/user/Mangadex-dl/tests/cli/cli_test.cpp` (:66, :85), `/home/user/Mangadex-dl/tests/cli/download_cli_test.cpp` (:446), `/home/user/Mangadex-dl/tests/cli/batch_parse_test.cpp` (:228), `/home/user/Mangadex-dl/tests/cli/mdlist_parse_test.cpp` (:223, :229).
- **Goldens:**
  - change: `/home/user/Mangadex-dl/tests/e2e/golden/{edge,download,batch,mdlist}.help.stdout`;
  - new: `follows.{help,dry-run,json-dry-run}.stdout`, `login.help.stdout`, `logout.help.stdout`.
- **Probe scratch (not committed):**
  - `/home/user/Mangadex-dl/.platform-runs/20261028T162703Z-factcheck-AF/nlohmann_amalgam.hpp`;
  - `/home/user/Mangadex-dl/.platform-runs/20261028T000000Z-factcheck/`;
  - `/home/user/Mangadex-dl/.platform-runs/20261028T162458Z-factcheck/`.
