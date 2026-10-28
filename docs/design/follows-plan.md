# Implementation plan: `mangadex-dl follows` / `login` / `logout` (download the titles you follow)

Status: **DRAFT for the ⏸ plan gate**, before fact-check. Design: `/home/user/Mangadex-dl/docs/design/follows-spec.md` (rev2). The gate decisions are binding and are not reopened here:
- Q1 = C: TTY prompt, an optional 0600 credentials file and `--password-stdin`. Env vars are deferred.
- Q2 = B: the session is persisted, the password never is.
- Q3 = B: top-level `follows`, `login` and `logout`.
- Q4 + QA1 = b: follows by default; `--status S` narrows them with one `GET /manga/status`.
- Q5: exit 8 `login_required` and exit 9 `auth_rejected`.
- Q6: flat by default; `--dir-per-title` opts in.

Precedent for format and engine seam: `docs/design/list-plan.md` (mdlist).

What I checked in the tree on 2026-10-28 (orientation only; the fact-checkers re-confirm every [VERIFY]):
- **`pixi list`:** cli11 2.7.2, nlohmann_json 3.12.0, catch2 3.16.1, cmake 4.4.4, ninja 1.13.2, clang 23.1.3, gxx 16.2.0, libstdcxx 16.2.0, libcurl 8.23.0 (headers; the runtime is dlopen'ed from the host, floor 7.85.0), sysroot_linux-64 2.28, kernel-headers 4.18. **No new dependency is needed.**
- **Globs:**
  - `CMakeLists.txt:90` is `GLOB_RECURSE src/core/*.cpp`, so `src/core/auth/*.cpp` is picked up.
  - `CMakeLists.txt:108` is a flat `GLOB src/app/cmd_*.cpp`, so **every new app source is named `cmd_*.cpp`**.
  - `tests/unit` is recursive. `tests/cli` is flat. `tests/CMakeLists.txt:57` globs `e2e/cases/*.cmake`.
- **No task edits** a CMake file, a preset, `pixi.toml`/`pixi.lock`, `scripts/*`, `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake` or a workflow.

---

## Spec issues / questions for the gate

Items 2, 3, 5, 7, 9 and 10 need a gate nod. The rest are planner decisions, recorded so nothing is silently decided.

1. **"Through the existing `paginate`" doesn't work as written.** `ApiClient::State::paginate` (`src/core/api_client.cpp`, private template) maps every non-2xx through `get_json` → `status_error`, so a 401 becomes an `Errc::network` string and the refresh-and-retry has no hook.
   - **Plan:** a new member `ApiClient::get_authorized(AuthorizedPath, offset, authorization, stop) -> Result<HttpResponse>` returns the final response unmapped, like `fetch_list_document` uses `perform`.
   - `fetch_follows` (core, T13) applies paginate's exact stepping rules itself: step by the items received, stop on an empty page or `offset >= total`, refuse past the 10 000 window. Both copies are unit-tested.
   - No behaviour difference. A later `code-refactorer` pass can unify them.
2. **Revocation URL.** The spec says logout POSTs to "the discovery document's `revocation_endpoint`", which reads as a runtime discovery GET.
   - **Plan (gate nod):** use the fixed `<issuer>/protocol/openid-connect/revoke`, which the discovery document confirmed at design time. There is no runtime discovery request and nothing extra to validate.
   - The token URL is fixed the same way: `<issuer>/protocol/openid-connect/token`.
3. **What the "3 token POSTs per run" cap counts.**
   - **Plan (gate nod):** it counts **logical** grant requests (password or refresh). Each may retry transport errors, 5xx and 429 within `RetryConfig` (≤4 attempts); any other 4xx is answered once. The revoke doesn't count.
   - AC6's "never more than 3 POSTs" holds whenever no retry happens; AC4 already expects 5xx/429 retries.
4. **The `follows --title` message is elided in the spec** ("follows has no --title option; …").
   - **Recommend** mdlist's shape: `follows has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title ...'`.
5. **The value-free ExtrasError text isn't specified, and other ParseErrors may echo values** (V7, V11).
   - **Recommend** for `follows`/`login`/`logout`: `<cmd> does not take these arguments: --foo, --bar (and N other words, not shown in case they hold a password)`. Option-looking words are echoed cut at `=` and through `terminal_safe`; bare words are only counted.
   - Plus: if V11 shows that a flag given a value (`--no-input=x`, `--password-stdin=x`) has its value echoed by CLI11, map those ParseErrors to a fixed `<flag> takes no value` for these three commands. This needs a gate nod because it changes stock CLI11 wording.
6. **`--status` binding.** The spec suggests `->delimiter(',')` (D7).
   - **Plan:** bind exactly like `--group` (`cmd_download_options.cpp:27-32`: `expected(1)`, `allow_extra_args(false)`, `TakeAll`) and split commas in `StatusFilter::parse`.
   - Why: `--status reading DIR` must never swallow DIR, and `reading,` / `''` must reach the validator verbatim so they get the spec's exit-2 text. User-visible behaviour is the spec's.
7. **mdlist's help footer and README now say something false.** They say "To download the titles you follow, add them to a public MDList" (`cmd_mdlist.cpp:47`, pinned by `tests/cli/mdlist_parse_test.cpp:223`). README `:482-484` says "reading follows directly needs a login, which mangadex-dl doesn't support yet".
   - **Recommend:** `To download the titles you follow, see 'mangadex-dl follows --help'.` `mdlist.help` re-blesses anyway (exit footer).
   - T19 changes help and test; T21 changes the README.
8. **A prompt answer longer than 4096 bytes is unspecified.** **Recommend:** exit 8, `the <field> you typed is longer than 4096 bytes`. The `--password-stdin` > 4096 case stays exit 2, as specified.
9. **`logout` with an unreadable or malformed session, or an insecure state directory.**
   - **Recommend:** an unreadable or malformed `session.json` is still removed, without a revoke, with the warning `could not end the session at MangaDex (the saved login is unreadable); the saved login was removed anyway`, exit 0.
   - An insecure state directory (wrong owner, group/other bits) → exit 1, `could not remove the saved login at <path>: <why>`, and nothing is touched.
10. **The E2E 0600 credentials file (spec VERIFY C9) is decided without a driver change.**
    - The fixture trees are already generated at test time by `mangadex_dl_fixture_gen` (the `e2e.fixtures` setup test, `tests/CMakeLists.txt:39-47`). It runs as the same euid as the cases.
    - **Plan:** T15 writes per tree `creds/credentials` (0600, all four keys), `creds/credentials-client` (0600, no password), `creds/credentials-wrong-password` (0600), `creds/credentials-0644` (explicit `fchmod 0644`) and `creds/password.txt`. All values are obviously fake.
    - Cases use `--credentials {{FIXTURES}}/follows-clean/creds/credentials`, `STDIN {{FIXTURES}}/…/creds/password.txt`, and `PRE_RUN login …` to seed a session.
    - **Deviation (gate nod):** AC29's committed `{{INPUTS}}/follows/credentials-0644` is generated instead. A committed file's mode follows the checkout umask (0600 under umask 077), so a committed case would pass or fail by umask. Nothing credential-shaped is committed (`diff-secret-scan`).
    - `CliE2E.cmake` stays untouched (V29).
11. **`Endpoints::auth` (spec "api_client.hpp changes") would break an existing test.** `tests/unit/api_client/api_client_test.cpp:304-321` compares whole `Endpoints` values.
    - **Plan:** `Endpoints` is unchanged. A free `auth_endpoints_for(const ApiBase&) -> std::optional<AuthEndpoints>` (official or fixture only; nullopt for custom https/loopback) lives in `auth.hpp`. That is also safer: a custom API base can never inherit the official auth host.
12. **The login/logout help footers are not drafted in the spec.**
    - **Recommend:**
      - `login`: the follows LOGIN paragraph, two examples (`mangadex-dl login`; `pass show mangadex | mangadex-dl login --password-stdin`), then the exit table.
      - `logout`: one paragraph ("Ends the session at MangaDex (best effort) and deletes <state path>. The credentials file is left alone: delete it yourself if it holds your password."), then the exit table.
    - cli-ux owns the wording.
13. **Planner placements** (not defects):
    - `TerminalPrompter` is app code (`src/app/cmd_auth_prompt.cpp`) behind a **core** `Prompter` interface (`include/mangadex_dl/prompt.hpp`), so the login flow is unit-testable. `Io::prompter` null means the real terminal prompter.
    - The session store is byte-level core I/O (`session_store.hpp`); JSON lives separately (T9).
    - `StderrTrace` is extracted out of `cmd_mdlist.cpp` into `cmd_trace.{hpp,cpp}` for reuse (T16, behaviour-preserving).
    - The `-vv` POST trace line is formatted in `auth_client.cpp`: `trace_request` is anonymous in `api_client.cpp:124`. That is 8 duplicated lines; a refactor follow-up.

---

## Approach

The flow becomes a core pipeline of small, separately reviewed seams:
- `Secret` (move-only, wiped);
- transports that can carry an `Authorization` header and a non-copied POST body;
- a byte-level state-directory store (0700 dir, `O_NOFOLLOW`, temp + `fsync` + `renameat`, `flock`);
- a token layer (`AuthClient`) and a `LoginSession` that implements a `TokenProvider`;
- a collection fetch that is the only caller of the two allowlisted authorized GETs.

The app layer stays thin: one auth glue file, then `login`/`logout` and `follows` in parallel. `follows` hands the filtered ids to mdlist's **unchanged** `run_batch_jobs`, so downloads stay anonymous.

**Key trade-off:** front-load the risky foundations (transports, fixture auth server, `Secret`, session-file I/O, the termios prompter) as four parallel tracks reviewed in batch 2, behind headers frozen in batch 1. The cost is eight batches. The gain:
- every security-sensitive primitive gets its own small diff and its own `security-reviewer` pass;
- `cli.cpp` / `exit_help.hpp` change once, late, with **one** bless point.

---

## Dependencies & pixi/CMake changes

| Change | Where | Task |
|---|---|---|
| None: no `pixi add`, `find_package`, FetchContent or `pixi.lock` change | — | — |
| No preset, pixi task, CMake option, `scripts/*` or CI change | — | — |
| New core sources (recursive glob): `src/core/secret.cpp`, `process_guard.cpp`, `follows.cpp`, `follows_status.cpp`, `follows_fetch.cpp`, `follows_plan.cpp`, `src/core/auth/{form,session_store,token,auth_client,credentials_file,session_json,auth_inputs,auth_endpoints,login_session,logout}.cpp` | glob | T2, T5, T6, T8–T14 |
| New app sources (`cmd_*.cpp` glob): `cmd_auth_prompt.cpp`, `cmd_auth.cpp`, `cmd_trace.cpp`, `cmd_login.cpp`, `cmd_follows.cpp` | glob | T7, T16–T18 |
| New tests: `tests/unit/**`, flat `tests/cli/*.cpp`, `tests/e2e/cases/{follows,login,logout}.cmake` | glob | most tasks |
| New generated fixture trees `{{FIXTURES}}/follows-*` | `fixture_gen.cpp` (not CMake) | T15 |

- **Build files for build-fixer, all tasks:** none beyond each task's Owns. A task that seems to need a shared CMake edit is a coordination event.
- **Tasks:** `nice pixi run check` is the gate everywhere. `nice pixi run -e gcc check`, `nice pixi run tsan -R …` and `nice pixi run release-test` apply where noted.

---

## Resolved in-repo (evidence; the fact-checker only re-reads these)

- **`SA_RESTART`:** `interrupt.cpp:124` sets `SA_RESTART`, so a blocking `read` never returns `EINTR` on Ctrl-C. The prompter must `poll` and check `interrupt_requested()` (spec C5; V31).
- **`with_remedy`:** it appends the remedy iff `exit_code() == 3` (`cmd_download_run.cpp:90`). Exits 8 and 9 never get it. Content failures (exit 3) are routed around it, as `cmd_mdlist.cpp` does (spec C12; V30).
- **Switches:** the exhaustive `switch (Errc)` blocks are `error.hpp:exit_code_for` and `cmd_batch_jobs.cpp:is_fatal`. The `switch (BatchSource)` blocks are `batch_run.cpp:41` and `cmd_batch_jobs.cpp:55, 215, 227` (spec C11; V27).
- **Fixture transport:**
  - `fixture_transport.cpp:105` answers every POST with 404, so the auth routing goes before that line.
  - `map_url` appends `.json` under `api/`, or `offset-<n>.json` when `offset=` is present. So `api/manga/status` → `api/manga/status.json` and `api/user/follows/manga?limit=100&offset=k` → `…/offset-k.json` (V28).
- **Curl transport:**
  - `curl_transport.cpp:356-388` builds a header slist only for POST and uses `COPYPOSTFIELDS`.
  - `validate` (`:399`) checks only URL and Content-Type.
- **FakeTransport:** `RecordedRequest` has no `authorization`. T4 adds it.
- **E2E driver:** it already offers `PRE_RUN` (same `{{WORK}}` and XDG dirs, stdin `/dev/null`), `{{FIXTURES}}` in ARGS/STDIN/PRE_RUN, and fresh `HOME`/`XDG_*` (`run_case.cmake:82-87`). It scrubs `MANGADEX_DL_*` (`:76`).
- **Fixture-mode clock:** `make_backend` gives an `InstantClock` (`clock.cpp:46-99`). A lock wait polled on `Clock::sleep_until` therefore runs its 60 s virtually, so AC26's timeout is instant at the cli tier (V32).
- **The `--group` binding pattern** (`cmd_download_options.cpp:27-32`) is the precedent for `--status`.
- **The loopback test server** (`tests/support/loopback_http_server.hpp`) records request headers, so T4 can assert the real `Authorization` header without the network.

---

## Assumptions to verify

Fact-checker rules for every cluster:
- **Never** POST to `auth.mangadex.org`, send any credential (real or fake) anywhere, or call an authenticated endpoint.
- Allowed: the installed env (`pixi list`, headers under `.pixi/envs/<env>/include`), `nice pixi run cxx-probe <file>` probes (scratch under `.platform-runs/`), anonymous GETs of public docs, the public discovery document, RFCs and Keycloak docs.
- Probe note (list plan): `scripts/cxx-probe.sh` adds no env include dir, so use `$CONDA_PREFIX/include` or a scratch tier-2 test.

**Cluster A: MangaDex / Keycloak, public documents only.** Blocks T8, T6 (fixture error bodies mirror it), T12 (URLs) and T10.
- **V1** (spec A1/A2/A7) The public discovery document (`/.well-known/openid-configuration`, one anonymous GET):
  - `token_endpoint` is `<issuer>/protocol/openid-connect/token`;
  - `revocation_endpoint` is `<issuer>/protocol/openid-connect/revoke`;
  - `client_secret_post` is supported;
  - `password` and `refresh_token` are among the grants.

  This confirms Spec issue 2.
- **V2** (A5) Per RFC 6749 §5.2 and Keycloak's public docs/source, a token error body is `{"error", "error_description"}`. Keycloak's codes and statuses are:
  - wrong password: `invalid_grant` (400 or 401: which?);
  - bad client or secret: `invalid_client` (401);
  - grant not allowed: `unauthorized_client` (400);
  - expired or revoked refresh: `invalid_grant` (400, "Token is not active" / "Session not active").

  The classifier keys on `error` first and status second. Live shapes are human AC42/43.
- **V3** (A7) RFC 7009: revocation answers 200 also for an unknown or invalid token, and client auth goes in the form (`client_id`, `client_secret`).
- **V4** (B1, D1–D3) From `api.mangadex.org/docs/static/api.yaml`:
  - `/user/follows/manga`: `limit` 1–100, `offset`, a `MangaList` with `total`, Bearer security;
  - `/manga/status`: `{result, statuses: {<id>: enum of six}}`, no pagination parameters, Bearer security.
- **V5** (B3) Whether the documented `offset + limit ≤ 10000` collection window applies to follows. It decides the cap wording.
- **V6** (A3/A4) Any first-party statement on `expires_in`, `refresh_expires_in` or refresh-token rotation. Informational only: the design tolerates absence.

**Cluster B: CLI11 2.7.2** (installed headers plus a scratch tier-2 test). Blocks T16, T17, T18 and T19.
- **V7** (C1) `ExtrasError::what()` lists extras verbatim.
  - Once `follows`/`login`/`logout` is reached, `app.get_subcommands().front()` names it.
  - `app.remaining(true)` holds every leftover token: bare words, `--x`, `--x=v`, `-p`. `--password hunter2` arrives as two tokens.
  - `follows out hunter2` is an ExtrasError at the root.
- **V8** (D7) `--status` bound to `std::vector<std::string>` with `type_name("WORD,...")->expected(1)->allow_extra_args(false)->multi_option_policy(TakeAll)`:
  - accumulates repeats and takes exactly one value per occurrence;
  - never swallows DIR (`--status reading out`);
  - delivers `reading,` and `''` verbatim;
  - help prints `--status WORD,...` with no ` ...` suffix and no default.
- **V9** (C2) `bind_selection_options`, `bind_run_options`, `--fail-fast`, `--dir-per-title`, `--status` and `bind_auth_options` register on a 4th sibling without `ConstructionError`, and help lists them in add order.
  - `logout`, with only the hidden `--api-base`, shows just `-h,--help` under OPTIONS.
  - Root `fallthrough()` still lets `follows out --json -q -v` parse.
- **V10** `add_option("--credentials", std::optional<std::string>&)->type_name("FILE")` parses and shows no default.
- **V11** What CLI11 prints for these:
  - `--no-input=x`, `--password-stdin=x` (a flag given a value);
  - `--credentials` without a value;
  - `login extra`.

  Does any message contain the value? This decides Spec issue 5's second half.

**Cluster C: libcurl** (env headers 8.23.0; behaviour at the 7.85.0 runtime floor). Blocks T4.
- **V12** (C3) `CURLOPT_HTTPHEADER` with `Authorization: Bearer x` on a GET sends exactly that header. With `FOLLOWLOCATION 0` it is never re-sent. libcurl prints no header without `CURLOPT_VERBOSE`.
- **V13** `CURLOPT_POSTFIELDS` (not copied) plus `CURLOPT_POSTFIELDSIZE_LARGE`:
  - sends exactly `size` bytes with no `strlen`;
  - the data must only stay valid until the transfer ends (`curl_easy_perform` returns);
  - whether the order of the two setopts matters;
  - `curl_easy_reset` forgets the pointer;
  - the repo's `CurlApi::setopt_str` forwards the pointer unchanged (else a `setopt_ptr`-style wrapper is needed in `curl_api.{hpp,cpp}`).
- **V14** `struct curl_slist { char *data; struct curl_slist *next; }` is public API and `curl_slist_append` duplicates its argument, so wiping each `data` before `curl_slist_free_all` is legal.

**Cluster D: glibc / POSIX / Linux** (sysroot 2.28, kernel-headers 4.18; `cxx-probe`). Blocks T2, T5 and T7.
- **V15** (C6) The conda sysroot declares all of these, and the release binary's highest `GLIBC_x.y` stays ≤ 2.28 (`explicit_bzero` is 2.25):
  - `explicit_bzero`, `flock`, `mkdirat`, `openat`, `renameat`, `unlinkat`, `fchmod`, `fsync`, `poll`;
  - `tcgetattr`/`tcsetattr` (`TCSAFLUSH`);
  - `posix_openpt`, `grantpt`, `unlockpt`, `ptsname_r`;
  - `prctl(PR_SET_DUMPABLE)`, `setrlimit(RLIMIT_CORE)`.
- **V16** `flock` locks belong to the open file description: a second `open` + `flock(LOCK_EX|LOCK_NB)` in the same process gets `EWOULDBLOCK`, and `close` releases it (AC26 in-process).
- **V17** `fsync` works on a directory fd opened `O_RDONLY|O_DIRECTORY|O_NOFOLLOW` (not `O_PATH`), and `renameat` replaces atomically.
- **V18** (C8) Linux ptys, as T7's tests need them:
  - `tcgetattr` on the master reports the slave's termios;
  - `TCSAFLUSH` discards unread input;
  - an `ICANON` line caps at 4096 bytes (N_TTY);
  - `poll` on the slave reports readable for a complete line or EOF;
  - `posix_openpt` works in the local sandbox and on ubuntu-26.04 CI. The E2E TTY cases already use `script`.

**Cluster E: sanitizer interplay** (`cxx-probe` with the dev preset's exact flags and options; also the `tsan` preset). Blocks T2, and T18 via the guard.
- **V20** (C7) `prctl(PR_SET_DUMPABLE, 0)` first thing under ASan+UBSan+LSan: a deliberate leak still reports `ERROR: LeakSanitizer` and exits 86 (StopTheWorld still works). The TSan build still runs and reports.
- **V21** After `PR_SET_DUMPABLE 0`, `OutputDir`'s publish (`linkat("/proc/self/fd/N", …, AT_SYMLINK_FOLLOW)` of an `O_TMPFILE`, `output_dir.cpp:243`) still works, from the main thread and from a worker thread.
- **V22** `setrlimit(RLIMIT_CORE, {0, 0})` changes no sanitizer exit code or report.

**Cluster F: nlohmann_json 3.12.0.** Blocks T8, T9 and T10.
- **V23** (D8) `json::parse(text, nullptr, false)` with a duplicate object key: no error. Which value survives? That decides the documented behaviour; detection is not attempted.
- **V24** A 100 000-entry status map (about 5 MB) passes `parse_untrusted` with `max_separators = 1 000 000`. It parses under ASan within the unit-test timeout, and `"statuses": []` reads as an empty array.
- **V25** On a non-const parsed DOM, `get_ref<std::string&>()` reaches each string node, so T8/T9 can wipe token strings in place before the DOM is destroyed.
- **V26** `ordered_json`: `doc["follows"]` assigned right after `"source"` keeps that order; `nullptr` → `null` for `statuses`.

**Cluster G: repo internals** (re-read). Blocks T1, T6, T11, T15, T16 and T20.
- **V27** (C11) The switches listed under "Resolved in-repo" are the only ones. `-Wswitch -Werror` flags a new enumerator in both clang 23 and gcc 16.
- **V28** (C4) Fixture mapping as above. The POST branch can route `auth/realms/mangadex/protocol/openid-connect/{token,revoke}` before `map_url`. No existing test depends on "every POST is 404" except the MD@H one, which stays true.
- **V29** (C9) `e2e.fixtures` runs `fixture_gen` as the case's euid.
  - A file created with `openat(…, O_CREAT|O_EXCL, 0600)` plus `fchmod` keeps its exact mode; `std::ofstream` files get `0666 & ~umask`.
  - `PRE_RUN` shares `{{WORK}}` and `XDG_STATE_HOME` with the main run.
  - `{{FIXTURES}}` expands in `STDIN` and `PRE_RUN`.
  - `run_case.cmake` creates (or the tool must create) `{{WORK}}/state`.
- **V30** (C12) `with_remedy` keys only on exit 3.
- **V31** (C5) `SA_RESTART` plus `interrupt_requested()` as the query.
- **V32** InstantClock advances virtual time inside `sleep_until`.
- **V33** `api_client_test.cpp:304-321` pins `Endpoints` equality (Spec issue 11).
- **V34** `nice pixi run tsan -R '<re>'` forwards the filter (carried from list-plan V24).

**Cluster H: libstdc++ 16.** Blocks T2.
- **V19**
  - `std::string::resize(n)` with `n <= capacity()` never reallocates.
  - The SSO capacity is 15.
  - A moved-from SSO string keeps its old bytes in its own buffer (`size() == 0`). This motivates wiping after a move.

**Clusters:** Cluster A: V1, V2, V3, V4, V5, V6 · Cluster B: V7, V8, V9, V10, V11 · Cluster C: V12, V13, V14 · Cluster D: V15, V16, V17, V18 · Cluster E: V20, V21, V22 · Cluster F: V23, V24, V25, V26 · Cluster G: V27–V34 · Cluster H: V19.

| Cluster | Must be confirmed before |
|---|---|
| G (V27) | T1 (batch 1) |
| D, E, H | T2 (batch 1); T5 and T7 (batch 2) |
| C | T4 (batch 2) |
| G (V28, V29) | T6 (batch 2), T15 (batch 4) |
| A, F | T8, T9, T10 (batch 3) |
| G (V32, V33) | T5, T11 |
| B | T16 (batch 5) |

Run all clusters before batch 1. Only E, D, H and G(V27) are needed for batch 1 itself.

**Language and library:** nothing beyond what the tree already uses (`std::expected`, `std::format`/`std::print`, ranges, `std::span`, heterogeneous `std::map`, `std::jthread`/`std::stop_token`). No CMake/CTest/CPack behaviour is relied on beyond what mdlist uses.

---

## Task breakdown

**Conventions** for every task (inherited from list-plan):
- **Naming and style:**
  - Namespaces `mangadex_dl`, `mangadex_dl::detail`, `mangadex_dl::app`.
  - Aggregates have DMIs and are built with designated initializers. **Exception:** service structs holding `std::function` (`AuthServices`, `FollowsFetchServices`) are filled member by member (the clang-analyzer false positive noted in `run_title`).
  - `enum class` switches list every enumerator, have no `default:`, and end with a fallback return.
  - No `<CLI/CLI.hpp>` outside `src/app/`, and no nlohmann in public headers.
- **Core never prints.** Every stderr line goes through `print_error`, `print_warning`, `print_note` or the observer, which `terminal_safe` the whole line.
- **Secrets:**
  - Never in a `std::format` argument, a log line, an `Error::message`, `--json` or `-vv`.
  - Bodies and headers are built by `Secret::append`, never by `std::format` (its temporaries are never wiped).
  - Every plain `std::string` that held a secret is `secure_wipe`d before it dies.
- **Frozen headers:** T1's public headers are frozen. A later change is a coordination event.
- **Linking:** a track's tests call only symbols from itself or an already-integrated batch.
- **Test names** start with the module (`"auth: …"`, `"session store: …"`, `"follows: …"`, `"login: …"`), so `-R 'unit\.(auth|follows)'` works.
- **GCC rule:** never `static_cast` to the type an expression already has (`-Wuseless-cast`).
- **Done when** always means: `nice pixi run check` green (long `timeout`), `git diff --stat tests/e2e/golden` empty (except T19's bless), and **every listed reviewer** at `VERDICT: APPROVED`. The track reviewers are `quality-reviewer` + `memory-safety-reviewer`, plus `security-reviewer` where marked.

**Sizes:** XS < 50 changed lines · S < 250 · M 250–600 · L 600–1000 (code + tests).

---

### T1: Frozen interfaces (headers, `Errc` 8/9, `BatchSource::follows`, env and prompter seams)
- **Size:** M (≈550 lines, mostly declarations). **Security-reviewer in loop:** yes (it freezes the secret/transport contracts).
- **Goal:** every cross-module declaration lands once, so batches 2–4 run in parallel. Behaviour changes are limited to what `-Wswitch` forces (follows takes mdlist's wording, which is unreachable until T19) plus the three env reads.
- **Files:**
  - edit `include/mangadex_dl/{error,exit_codes,api_client,batch}.hpp`, `include/mangadex_dl/net/{transport,fixture_transport}.hpp`;
  - create `include/mangadex_dl/{secret,prompt,session_store,auth,follows}.hpp`;
  - edit `src/app/io.hpp`, `src/app/io.cpp`, `src/app/cmd_batch_jobs.cpp` (switch cases only), `src/core/batch_run.cpp` (one case) and `tests/unit/interfaces/headers_test.cpp`.
- **Owns:** exactly those 17 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`error.hpp`:** after `row_dir`, `login_required` (exit 8) and `auth_rejected` (exit 9), each with a doc comment. `exit_code_for` gains both cases.
  - **`exit_codes.hpp`:**
    - `inline constexpr int login_required = 8;` "follows, login: no usable saved login and the missing detail can't be asked for, no API client set up, or the saved login expired and no password source exists";
    - `inline constexpr int auth_rejected = 9;` "follows, login: MangaDex refused the login details, or a freshly obtained access token";
    - the `mixed_failures` doc becomes "batch/mdlist/follows".
  - **`net/transport.hpp`:** `HttpRequest` gains `std::string authorization{}; ///< the whole Authorization value ("Bearer <t>"); empty = no header. CR, LF or NUL → TransportErrc::local before any I/O. Set only by ApiClient::get_authorized.` The `body` doc changes to "read only during fetch(); the curl transport points libcurl at it (CURLOPT_POSTFIELDS), so it must stay unchanged until fetch() returns".
  - **`net/fixture_transport.hpp`:** doc for the auth routing (T6's contract: token/revoke POSTs, the two authorized GETs, the leak tripwire).
  - **`secret.hpp`:**
    ```cpp
    /// explicit_bzero over [data(), data()+capacity()) via resize(capacity()), then clear(). Never reallocates (V19).
    void secure_wipe(std::string& text) noexcept;
    /// A secret string (password, client secret, token, a form body or session document that holds one). Move-only;
    /// no formatter, no operator<<, no ==, no implicit conversion. Wiped on destruction, assignment, clear() and growth.
    class Secret {
    public:
        Secret() noexcept = default;
        [[nodiscard]] static Secret copy_of(std::string_view text);  ///< exact-capacity copy
        [[nodiscard]] static Secret adopt(std::string&& text) noexcept; ///< takes the buffer, wipes `text`
        Secret(Secret&& other) noexcept;             ///< steals, then wipes other's (SSO) buffer
        Secret& operator=(Secret&& other) noexcept;  ///< wipes self first
        Secret(const Secret&) = delete; Secret& operator=(const Secret&) = delete;
        ~Secret();
        void reserve(std::size_t bytes);             ///< growth copies, then wipes the old buffer
        void append(std::string_view text);          ///< grows as reserve() does
        void push_back(char c);
        void clear() noexcept;
        [[nodiscard]] bool empty() const noexcept; [[nodiscard]] std::size_t size() const noexcept;
        /// Borrows *this: never bind it to a temporary Secret, never store it.
        [[nodiscard]] std::string_view reveal() const noexcept MANGADEX_DL_LIFETIMEBOUND;
    private:
        std::string value_{};
    };
    enum class DumpGuard { not_dumpable, no_core_only, none };
    /// setrlimit(RLIMIT_CORE, {0,0}), then prctl(PR_SET_DUMPABLE, 0) (V20). Best effort; called once, from the
    /// main thread, before any secret is read.
    [[nodiscard]] DumpGuard disable_core_dumps() noexcept;
    ```
  - **`prompt.hpp`** (core): `enum class Echo { on, off };` `enum class PromptStatus { answered, empty, too_long, interrupted, unavailable };`, `struct PromptReply { PromptStatus status = PromptStatus::unavailable; Secret answer{}; };`, `inline constexpr std::size_t max_prompt_answer_bytes = 4096;` and:
    ```cpp
    class Prompter {  // polymorphic base: copy/move deleted, protected ctor, virtual dtor
    public:
        virtual void notice(std::string_view line) = 0;  ///< one line on the user's terminal
        /// Writes `question`, reads one line (≤ max_prompt_answer_bytes, '\n'/'\r\n' stripped). Echo::off: echo is off
        /// for the read and restored on every path, then a newline is written. Polls interrupt_requested() at
        /// least every 100 ms (SA_RESTART). Never throws past the restore.
        [[nodiscard]] virtual PromptReply ask(std::string_view question, Echo echo) = 0;
    };
    ```
  - **`session_store.hpp`** (core, file I/O):
    - `struct UserDirs { std::optional<std::filesystem::path> home{}, xdg_config_home{}, xdg_state_home{}; };`
    - `[[nodiscard]] UserDirs make_user_dirs(std::optional<std::string_view> home, std::optional<std::string_view> config, std::optional<std::string_view> state);` keeps only non-empty absolute paths.
    - `state_dir_path(const UserDirs&)` and `default_credentials_path(const UserDirs&)` return `std::optional<path>`.
    - `enum class SessionSlot { real, fixture };` and `session_file_name(SessionSlot)`, giving `"session.json"` / `"fixture-session.json"`.
    - `struct LockTiming { std::chrono::milliseconds poll{250}; std::chrono::seconds timeout{60}; };`
    - `enum class FileProblemKind { missing, symlink, not_regular, not_owned, too_open, too_large, io };`
    - `struct FileProblem { FileProblemKind kind = FileProblemKind::io; unsigned mode = 0; std::string reason{}; };`, where `reason` is a phrase without the path. `template <class T> using FileResult = std::expected<T, FileProblem>;`
    - `[[nodiscard]] FileResult<Secret> read_private_file(const std::filesystem::path& path, std::size_t cap);`: `O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK`, symlinks followed, `fstat`: regular, `st_uid == geteuid()`, `(mode & 077) == 0`, then at most `cap` bytes read straight into a pre-reserved Secret.
    - `class SessionLock`: move-only RAII; closing the fd releases the `flock`.
    - `class StateDir`:
      ```cpp
      class StateDir {  // move-only; owns an O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC fd
      public:
          /// create: missing components made with mode 0700; the final one must be owned by euid with (mode & 077) == 0.
          [[nodiscard]] static FileResult<StateDir> open(const std::filesystem::path& path, bool create);
          /// session.lock (openat O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600), flock(LOCK_EX|LOCK_NB) polled on `clock`
          /// every timing.poll until timing.timeout; interrupt_requested()/stop → Errc::interrupted; timeout →
          /// Errc::failure "another mangadex-dl is using the saved login; try again".
          [[nodiscard]] Result<SessionLock> lock(Clock& clock, LockTiming timing, std::stop_token stop) const;
          /// openat O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_CLOEXEC; regular, owner, mode; ≤ max_session_file_bytes.
          /// nullopt when absent; ELOOP → FileProblemKind::symlink.
          [[nodiscard]] FileResult<std::optional<Secret>> read(SessionSlot slot) const;
          /// .<name>.<12 hex> via openat O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC 0600 (bounded retries), fchmod 0600,
          /// write all, fsync, close (checked), renameat, fsync(dir); unlinkat(temp) on any failure.
          [[nodiscard]] Result<void> write(SessionSlot slot, const Secret& bytes) const;
          [[nodiscard]] Result<bool> remove(SessionSlot slot) const;  ///< false = was absent
          [[nodiscard]] const std::filesystem::path& path() const noexcept MANGADEX_DL_LIFETIMEBOUND;
      private: /* UniqueFd fd_{}; std::filesystem::path path_{}; */
      };
      ```
  - **`auth.hpp`** (core):
    - **Constants:**
      - `official_auth_issuer` = `https://auth.mangadex.org/realms/mangadex`, `api_clients_url` = `https://mangadex.org/settings`;
      - `max_token_bytes` 16384, `max_token_response_bytes` 64 KiB, `max_credentials_file_bytes` 16 KiB, `max_session_file_bytes` 64 KiB, `max_password_stdin_bytes` 4096;
      - `max_token_requests` 3;
      - `reuse_min_validity` 60 s, `refresh_ahead` 30 s, `default_access_lifetime` 900 s, `max_access_lifetime` 86400 s;
      - `auth_request_interval` 1000 ms, `revoke_timeout` 5 s.
    - **`class BearerToken`:** `static Result<BearerToken> parse(Secret raw)`, charset `[A-Za-z0-9._~+/=-]{1,16384}`, Errc::network with a fixed message. `const Secret& value() const LIFETIMEBOUND`. `Secret authorization() const` gives `"Bearer <t>"`. The default ctor gives an empty placeholder that parse never yields. Move-only through Secret.
    - **Aggregates:**
      - `ClientCredentials { std::string client_id{}; Secret client_secret{}; }`;
      - `CredentialsFile { std::optional<std::string> client_id{}; std::optional<Secret> client_secret{}; std::optional<std::string> username{}; std::optional<Secret> password{}; }`;
      - `Session { std::string issuer{}, client_id{}; Secret client_secret{}; std::string username{}; std::optional<BearerToken> refresh_token{}; std::optional<std::int64_t> refresh_expires_at{}; std::optional<BearerToken> access_token{}; std::int64_t access_expires_at = 0; }`;
      - `TokenGrant { BearerToken access{}; std::optional<BearerToken> refresh{}; std::chrono::seconds expires_in{900}; std::optional<std::chrono::seconds> refresh_expires_in{}; }`.
    - **Raw CLI aggregates:** `RawAuthArgs { std::optional<std::string> credentials{}; bool password_stdin = false; bool no_input = false; }`, `RawLoginArgs { RawAuthArgs auth{}; std::string api_base = std::string{default_api_base}; }`, `RawLogoutArgs { std::string api_base = …; }`.
    - **Pure functions (T6/T8/T9):**
      - `Secret form_encode(std::string_view)`;
      - `Secret password_grant_body(const ClientCredentials&, std::string_view username, const Secret& password)`, `refresh_grant_body(const ClientCredentials&, const BearerToken&)`, `revoke_body(const ClientCredentials&, const BearerToken&)`. Fixed field order: `grant_type`, then `username`/`password` or `refresh_token`, then `client_id`, `client_secret`; revoke is `token`, `token_type_hint=refresh_token`, `client_id`, `client_secret`;
      - `Result<CredentialsFile> parse_credentials_file(std::string_view)` and `Result<CredentialsFile> load_credentials_file(const path&, bool explicit_path)`;
      - `Result<Secret> read_password_stdin(int fd)`;
      - `Result<void> check_auth_api_base(const ApiBase&, std::string_view command)`;
      - `Result<Session> parse_session(std::string_view json, std::string_view issuer)` and `Secret session_to_json(const Session&)`;
      - `Result<TokenGrant> parse_token_response(std::string_view)`.
    - **Token errors:**
      - `enum class GrantKind { password, refresh };`
      - `enum class TokenRefusal { bad_credentials, bad_client, client_not_allowed, session_ended, other };`
      - `struct TokenErrorInfo { TokenRefusal refusal = TokenRefusal::other; int status = 0; std::optional<std::string> code{}; /*[a-z_]{1,64}*/ std::optional<std::string> description{}; /*≤200 bytes, raw*/ };`
      - `TokenErrorInfo classify_token_error(GrantKind, int status, std::string_view body)` and `Error token_refusal_error(const TokenErrorInfo&, GrantKind, std::string_view username)`, which produces the spec's 8/9 texts.
    - **Endpoints:** `struct AuthEndpoints { std::string issuer{}, token_url{}, revoke_url{}; };` and `std::optional<AuthEndpoints> auth_endpoints_for(const ApiBase&)`: official, or fixture `file://<root>/auth/realms/mangadex`; nullopt otherwise.
    - **Failure type:** `enum class AuthFailureKind { request, content, refused, local };`, `struct AuthFailure { AuthFailureKind kind = AuthFailureKind::local; TokenErrorInfo info{}; Error error{}; };`, `template <class T> using AuthResult = std::expected<T, AuthFailure>;`
    - **`class AuthClient`** (non-copyable, non-movable; borrows transport, clock and trace):
      - `AuthResult<TokenGrant> request(GrantKind, const Secret& body, std::stop_token)`;
      - `Result<void> revoke(const Secret& body, std::stop_token)`;
      - `int requests() const noexcept`.
    - **`class TokenProvider`** (polymorphic base):
      - `virtual AuthResult<Secret> authorization(std::stop_token) = 0;`: `"Bearer <t>"`, valid for ≥ `refresh_ahead`, refreshing first if needed;
      - `virtual AuthResult<Secret> refresh_after_unauthorized(std::stop_token) = 0;`: one refresh (or the password fallback).
    - **Login flow:**
      - `enum class LoginMode { reuse, password };`, `enum class LoginHow { saved, refreshed, password };`
      - `struct LoginRequest { ApiBase api_base{}; UserDirs dirs{}; LoginMode mode = LoginMode::reuse; CredentialsFile credentials{}; std::filesystem::path credentials_path{}; std::optional<Secret> password_stdin{}; Prompter* prompter = nullptr; LockTiming lock{}; };`, where `prompter == nullptr` means prompts are not allowed;
      - `struct AuthServices { Transport* transport = nullptr; Clock* clock = nullptr; std::function<std::uint32_t(std::uint32_t)> jitter{}; TraceSink* trace = nullptr; };`
      - `struct LoginNotes { std::vector<std::string> warnings{}; std::string username{}; LoginHow how = LoginHow::saved; std::optional<std::filesystem::path> session_path{}; std::optional<std::string> save_error{}; };`
      - `class LoginSession final : public TokenProvider`:
        - `[[nodiscard]] static AuthResult<std::unique_ptr<LoginSession>> open(LoginRequest request, const AuthServices&, std::stop_token, LoginNotes& notes);`
        - `void close() noexcept;` releases the lock and wipes; idempotent; also run by the dtor.
    - **Logout:** `struct LogoutOutcome { bool removed = false; std::filesystem::path path{}; std::optional<std::string> warning{}; };` and `[[nodiscard]] Result<LogoutOutcome> logout(const ApiBase&, const UserDirs&, const AuthServices&, LockTiming, std::stop_token);`
  - **`follows.hpp`** (core):
    - **Constants:** `follows_page_limit` 100, `max_follows` 10000, `max_status_entries` 100000, `max_status_separators` 1000000, `follows_row_label` "follows".
    - **Reading statuses:**
      - `enum class ReadingStatus { reading, on_hold, plan_to_read, dropped, re_reading, completed };`
      - `std::string_view reading_status_name(ReadingStatus) noexcept` and `std::optional<ReadingStatus> parse_reading_status(std::string_view api_value) noexcept`;
      - `class StatusFilter`: `static Result<StatusFilter> parse(std::span<const std::string> words)` splits commas, applies the aliases, de-duplicates keeping the first, and gives the spec's exit-2 texts; `bool contains(ReadingStatus) const noexcept`; `std::vector<std::string> names() const`.
    - **Documents:**
      - `struct FollowsPage { std::vector<MangaId> titles{}; std::size_t total = 0; };`
      - `struct StatusMap { std::map<MangaId, std::string> statuses{}; };`
      - `struct FilteredFollows { std::vector<MangaId> titles{}; std::size_t follows = 0; std::map<std::string, std::size_t, std::less<>> unknown{}; };`
      - `follows_request_url(api, offset)` gives `<api>/user/follows/manga?limit=100&offset=<k>`; `status_map_request_url(api)` gives `<api>/manga/status`;
      - `parse_follows_page`, `parse_status_map`, and `filter_follows(std::span<const MangaId>, const StatusMap&, const StatusFilter&)`.
    - **Settings:**
      - `struct RawFollowsArgs { RawDownloadArgs defaults{}; bool fail_fast = false; bool dir_per_title = false; std::vector<std::string> status{}; RawAuthArgs auth{}; };`
      - `struct FollowsSettings { BatchSettings batch{}; std::optional<StatusFilter> filter{}; bool dir_per_title = false; RawAuthArgs auth{}; };`
      - `Result<FollowsSettings> validate_follows_args(const RawFollowsArgs&)` and `std::vector<BatchJob> plan_follows(std::span<const MangaId>, const FollowsSettings&)`.
    - **Fetch:**
      - `struct FollowsCollection { std::vector<MangaId> follows{}; std::optional<StatusMap> statuses{}; };`
      - `struct FollowsFetchServices { Transport* transport = nullptr; Clock* clock = nullptr; std::function<…> jitter{}; TraceSink* trace = nullptr; };`
      - `[[nodiscard]] AuthResult<FollowsCollection> fetch_follows_collection(const ApiBase&, const FollowsFetchServices&, TokenProvider&, bool with_statuses);`
  - **`api_client.hpp`:** `enum class AuthorizedPath { user_follows_manga, manga_status };` plus the member:
    ```cpp
    /// GET <api>/user/follows/manga?limit=100&offset=<offset> or <api>/manga/status (offset ignored) with
    /// Authorization = `authorization`. The URL is built here from the enum: no other request can carry a token.
    /// Refuses (Errc::usage, no I/O) unless endpoints.api is the official API or a file:// fixture. Retries as
    /// perform(); returns the final response unmapped (2xx or not); a stop → Errc::interrupted.
    [[nodiscard]] Result<HttpResponse> get_authorized(AuthorizedPath path, std::size_t offset,
                                                      const Secret& authorization, std::stop_token stop);
    ```
  - **`batch.hpp`:**
    - `BatchSource::follows` ("item" wording, prefix `follows:<n>: <uuid>: `);
    - `struct FollowsSummary { std::string username{}; std::size_t titles = 0; std::optional<std::vector<std::string>> statuses{}; std::size_t selected = 0; };`
    - a new overload `batch_to_json(const BatchResult&, std::string_view source, bool dry_run, const FollowsSummary&)` (defined in T14).
  - **`io.hpp` / `io.cpp`:**
    - `EnvSnapshot` gains `std::optional<std::string> home{}, xdg_config_home{}, xdg_state_home{};` (raw, non-empty). `read_env` reads them and drops `noexcept`.
    - `Io` gains `mangadex_dl::Prompter* prompter = nullptr; ///< borrowed; null = the real terminal prompter (cmd_auth_prompt.cpp)`, through a forward declaration.
  - **`cmd_batch_jobs.cpp`:**
    - `is_fatal`: `case Errc::login_required: case Errc::auth_rejected: return true;` (never produced by `run_download`; global if ever).
    - `row_noun`, `row_prefix` and `collision_message`: `case BatchSource::follows:` falls through with `mdlist`.
  - **`batch_run.cpp:41`:** the same fall-through.
  - **`headers_test.cpp`:**
    - `static_assert(exit_code_for(Errc::login_required) == 8 && … auth_rejected == 9)`;
    - `!std::is_copy_constructible_v<Secret> && std::is_nothrow_move_constructible_v<Secret>`;
    - `Prompter` and `TokenProvider` are abstract;
    - the constants;
    - `HttpRequest{}.authorization.empty()`;
    - `EnvSnapshot{.no_color = true}` still builds;
    - signatures in unevaluated contexts only.
  - **Secret-holding types are checked only with traits:** nothing instantiates a Secret at run time here, because `secret.cpp` lands in the parallel T2.
- **Dependencies:** none.
- **Done when:**
  - `nice pixi run check` **and** `nice pixi run -e gcc check` are green (new enumerators, `-Wswitch`);
  - every existing test passes unmodified (except `headers_test.cpp`);
  - goldens are unchanged;
  - the three reviewers approve.
- **Tests:** unit `headers_test.cpp` as above.

### T2: `Secret` and the core-dump guard
- **Size:** S. **Security-reviewer:** yes.
- **Files:** create `src/core/secret.cpp`, `src/core/process_guard.cpp`, `tests/unit/secret/secret_test.cpp` and `tests/unit/secret/process_guard_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`secure_wipe`:** `resize(capacity())`, then `explicit_bzero(data(), size())`, then `clear()` (V19).
  - **`Secret`:**
    - move ctor/assign: steal, then `secure_wipe(other.value_)`, which covers SSO;
    - growth: allocate a new string with the target capacity, copy, wipe the old buffer, swap;
    - `copy_of` reserves exactly;
    - the dtor wipes.
  - **`disable_core_dumps`:** `setrlimit(RLIMIT_CORE, {0,0})`, then `prctl(PR_SET_DUMPABLE, 0)`. It returns which ones took. If V20/V21 refute prctl, the prctl call is dropped (refactor trigger), keeping the same signature.
- **Dependencies:** none (parallel with T1; it implements T1's declarations).
- **Done when:**
  - `check` is green;
  - `nice pixi run release-test` is green: `release-check.binary` keeps the glibc floor with `explicit_bzero`;
  - the three reviewers approve.
- **Tests (unit):**
  - **`Secret`:**
    - SSO and heap values;
    - move leaves the source empty, and its old buffer zeroed (inspect through a captured `data()` pointer of a **still-owned** buffer only: the SSO case via a moved-from object, the heap case via a custom check before release, never reading freed memory);
    - growth across several appends keeps the content;
    - `reveal()` equals the input;
    - `clear()`;
    - `adopt` wipes the source.
  - **Guard:** in a `fork()`ed child, `disable_core_dumps()` then `prctl(PR_GET_DUMPABLE) == 0` and `getrlimit(RLIMIT_CORE)` gives `{0,0}`; the child `_exit`s its verdict, so the test runner stays dumpable.

### T3: Docs: close list-spec V1 (private list → 404 → exit 4, confirmed live)
- **Size:** XS. **Security-reviewer:** no.
- **Files:** edit `docs/design/list-spec.md` and `README.md` (only the mdlist private-list sentences at `:484` and `:575-578`).
- **Owns:** those 2 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`list-spec.md`:** status lines 5 and 32, the "Private lists (V1, unresolved…)" block (:226-230) and the "[VERIFY] V1" item (:563-566) become "V1 RESOLVED (user live test, 2026-10-28): an anonymous `GET /list/{id}` for a private list answers 404; mdlist exits 4". The 401/403-with-CustomList-body fallback is noted as kept defensively.
  - **README:**
    - "a private list counts as not found (exit 4)" stays, hedge-free: MangaDex answers 404 for a private list;
    - the "a private list may give `(HTTP 403)` or `(HTTP 401)`" sentence becomes "MangaDex answers a private list with HTTP 404 (the code also accepts a 401/403 that names the list, with the same message)".
  - **Not touched here:** the "follows needs a login, not supported yet" line (T21 changes it once `follows` ships).
- **Dependencies:** none.
- **Done when:** `check` green; `quality-reviewer` approves. `memory-safety-reviewer` confirms "no C++ changed".
- **Tests:** none.

### T4: Transports: `Authorization` on requests, non-copied POST bodies, FakeTransport recording
- **Size:** M. **Security-reviewer:** yes.
- **Files:**
  - edit `src/core/net/curl_transport.cpp` and `tests/support/fake_transport.hpp`;
  - edit `src/core/net/curl_api.{hpp,cpp}` only if V13 needs a pointer setopt;
  - create `tests/unit/net/curl_transport_auth_test.cpp`.
- **Owns:** those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`validate`:** an `authorization` containing CR, LF or NUL → `TransportErrc::local` "invalid Authorization header (contains CR, LF or NUL)", with no value echoed.
  - **`configure`:**
    - the header slist is built for **both** methods: `Expect:` and `Content-Type` for POST as today, plus `"Authorization: " + authorization` when it is non-empty. The line is built inside a `Secret` and appended from `reveal()`'s data; curl copies it;
    - with an empty authorization on a GET, no slist is set (exactly today's options);
    - POST switches from `COPYPOSTFIELDS` to `CURLOPT_POSTFIELDS` + `POSTFIELDSIZE_LARGE` (V13). The doc comment states that `request.body` outlives `easy_perform`.
  - **`SlistFree`:** `explicit_bzero` each node's `data` before `curl_slist_free_all` (V14).
  - **`FakeTransport`:** `RecordedRequest` gains `std::string authorization{}`, appended as the **last** member (no existing designated init breaks). It is recorded like `body`.
- **Dependencies:** T1.
- **Done when:**
  - `check`, `-e gcc check` and `nice pixi run release-test` are green (this changes every request of the shipped binary);
  - `unit.net*`, `unit.api_client*` and `e2e.*` pass unmodified;
  - the three reviewers approve.
- **Tests (unit, loopback server):**
  - **AC9:**
    - a GET with `authorization = "Bearer abc"` → the server sees exactly `Authorization: Bearer abc`;
    - a GET with it empty → no `Authorization` header and no `Expect`;
    - CR, LF and NUL each → `local` before any connection (the server sees no request).
  - **POST:**
    - exact body bytes with an embedded `&=%`, and `Content-Type: application/x-www-form-urlencoded`;
    - the existing POST test still passes;
    - the MD@H report path is unchanged.
  - A 307 response is returned, never followed; the header is sent once.

### T5: State directory, session file store, lock and private-file reader
- **Size:** L. **Security-reviewer:** yes.
- **Files:** create `src/core/auth/session_store.cpp`, `tests/unit/auth/session_store_test.cpp` and `tests/unit/auth/private_file_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:** T1's `session_store.hpp` contract:
  - **Paths:** `make_user_dirs` / `state_dir_path` / `default_credentials_path` (XDG only when absolute, else `$HOME/.local/state/mangadex-dl`, `$HOME/.config/mangadex-dl/credentials`).
  - **`StateDir::open`:**
    - mkdir -p with 0700 per missing component; parents may be symlinks (dotfile managers), the final component may not;
    - opens the final component `O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC` (not `O_PATH`: it is fsynced; V17) and checks owner and mode.
  - **`lock`:** polls on the injected `Clock` (V32).
  - **`read`:** bounded, straight into a pre-reserved Secret. Never `read_bounded_fd`: its growth would leave unwiped copies.
  - **`write`:** the spec's 5 steps, with a scope guard that `unlinkat`s the temp on every failure path.
  - **`remove`.**
  - **`read_private_file`:** the same checks with symlinks followed.
  - **Reasons are fixed phrases plus `strerror`:**
    - "it is readable or writable by others (mode 0755)";
    - "it is not owned by you";
    - "it is a symbolic link";
    - "it is not a regular file";
    - "it is larger than 65536 bytes".
- **Dependencies:** T1, T2.
- **Done when:** `check` and `-e gcc check` are green; the three reviewers approve.
- **Tests (unit, `TempDir`):**
  - **Directories and modes:**
    - dir created 0700 (with umask 022 and 077), parents too;
    - an existing 0755 final dir → `too_open`, nothing written;
    - a symlinked final dir → `symlink`;
    - a symlinked `session.json` → `symlink`, the target untouched (AC25 core half).
  - **Write and read:**
    - the write is 0600 even under umask 0;
    - the temp is gone after success **and** after a forced failure (the dir made read-only mid-way, or a `write` to a full FS simulated by a dir whose file is pre-created as a directory at the final name → `renameat` fails);
    - read: round trip, absent → nullopt, oversize → `too_large`, 0644 → `too_open`;
    - remove: present → true, absent → false.
  - **Lock (AC26 core):**
    - a second fd holding `flock` → wait polled on `FakeClock`; timeout → the exact message;
    - an `on_sleep` hook that releases the other fd on the 3rd poll → success (V16);
    - `simulate_interrupt_for_testing` → `interrupted`.
  - **`read_private_file` (AC16 core):** 0600 ok; 0640 and 0604 → `too_open` with `mode` set; a FIFO → `not_regular` (never blocks); a symlink to a 0600 file → ok; missing → `missing`; 16 KiB + 1 → `too_large`.

### T6: Fixture auth server, form codec and the auth fixture writer
- **Size:** L. **Security-reviewer:** yes (untrusted form bodies and paths inside the shipped binary's fixture mode).
- **Files:**
  - edit `src/core/net/fixture_transport.cpp`;
  - create `src/core/auth/form.hpp` (core-private: `form_decode`), `src/core/auth/form.cpp` (`form_encode` from `auth.hpp` plus `form_decode`), `tests/support/mdx_auth_fixture.hpp`, `tests/unit/auth/form_test.cpp` and `tests/unit/net/fixture_transport_auth_test.cpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`form_encode`:** RFC 3986 unreserved characters kept, every other byte `%XX` (upper hex), into a reserved Secret.
  - **`form_decode(std::string_view, std::size_t max_fields)`:** `std::expected<std::vector<std::pair<std::string,std::string>>, std::string>`. `+` becomes a space; a bad `%` escape or a duplicate key is an error; at most 16 fields.
  - **Fixture POST routing**, before today's `return status_only(404)`:
    - `file://<root>/auth/realms/mangadex/protocol/openid-connect/token`: the Content-Type must be the form type; the body is capped at 64 KiB.
      - `grant_type=password` → `<root>/auth/password/<username>.json`;
      - `grant_type=refresh_token` → `<root>/auth/refresh/<token>.json`.
    - `…/revoke` → `<root>/auth/revoke/<token>.json`.
    - Names must match `[A-Za-z0-9._-]{1,128}` and not be `.` or `..`; otherwise `TransportErrc::local` "fixture: unsafe auth name". They are opened with the existing `O_NOFOLLOW` walk.
    - A missing file → 400 `{"error":"invalid_grant","error_description":"Invalid user credentials"}` (refresh: "Token is not active").
    - The file shape is `{"expect": {client_id, client_secret, password?}, "status": n, "body": {...}}`. A client mismatch → 401 `invalid_client`; a password mismatch → 400 `invalid_grant`. Otherwise `status` + `body.dump()`.
    - Every other POST → 404 (MD@H, as today).
  - **Authorized GETs:**
    - the exact allowlist is `api/user/follows/manga` (with `offset=`) and `api/manga/status`;
    - authorization `Bearer <T>` with T a safe name and `<root>/auth/access/<T>.json` present → served from the normal mapping;
    - otherwise → 401 `{"result":"error","errors":[{"status":401,"title":"unauthorized_http_exception"}]}`;
    - `{"valid_requests": n}` counts down per T in a `std::map<std::string, std::size_t>` under a `std::mutex` member; past n → 401;
    - any `api/user/…` GET without a token → 401.
  - **Leak tripwire:** a non-empty authorization to any URL outside the allowlist → `TransportErrc::local` "fixture: Authorization sent to <url>". The URL is a fixture path, never the token.
  - **`mdx_auth_fixture.hpp`:**
    - `struct FixtureAuth`: client_id, client_secret, username, password, the access/refresh token chain (rotation), `valid_requests`, and the error variants;
    - `write_auth_files(root, const FixtureAuth&)`;
    - the constants `fixture_user = "fixture-user"`, `fixture_password = "fixture-password-not-a-secret"`, `fixture-client`, `fixture-client-secret-not-a-secret`, `fixture-access-1`/`-2`, `fixture-refresh-1`/`-2`.
- **Dependencies:** T1.
- **Done when:**
  - `check` is green;
  - `nice pixi run tsan -R 'unit\.(fixture|net)'` is green (the counter mutex);
  - every existing `unit.net*`, `cli.*` and `e2e.*` passes unmodified;
  - the three reviewers approve.
- **Tests (unit):**
  - **AC1 encode:** unreserved kept; `a&b=c+d%e f é` → the exact `%XX` string. Decode: round trip, a bad escape, a duplicate key, 17 fields.
  - **AC10:**
    - password, refresh and revoke routing with exact statuses and bodies;
    - an `expect` mismatch for client and for password;
    - `../x`, `a/b`, `.` → local;
    - `valid_requests` 2 → 200, 200, 401;
    - authorization to `api/manga/<uuid>` → local tripwire, while `api/manga/status` with a valid token → 200;
    - an MD@H POST → 404;
    - an allowlisted URL without a token → 401;
    - two threads hitting the counter concurrently (TSan).

### T7: Terminal prompter (termios, echo off, interrupt polling)
- **Size:** M. **Security-reviewer:** yes.
- **Files:** create `src/app/cmd_auth_prompt.hpp`, `src/app/cmd_auth_prompt.cpp`, `tests/support/pty_pair.hpp` and `tests/cli/auth_prompt_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - `class TerminalPrompter final : public Prompter { TerminalPrompter(int fd, std::ostream& err) noexcept; … }`.
  - **`notice`:** writes `line + "\n"` to `err` and flushes.
  - **`ask`:**
    1. write the question and flush;
    2. `Echo::off`: `tcgetattr`, then a `TermiosRestore` RAII that saves, clears `ECHO` and applies `TCSAFLUSH`; its dtor restores with `TCSAFLUSH` on every path. `tcgetattr` failing → `unavailable`;
    3. loop `poll(fd, POLLIN, 100 ms)`, then `interrupt_requested()` → `interrupted`;
    4. `read` into a stack `std::array<char, 4097>` wiped after use, appending to a Secret reserved to 4097;
    5. stop at `\n` (strip `\r`), or EOF with nothing → `empty`, or > 4096 → `too_long`;
    6. `Echo::off` → write `"\n"`.
  - Nothing is ever colored.
- **Dependencies:** T1, T2.
- **Done when:**
  - `check` and `-e gcc check` are green;
  - `nice pixi run tsan -R 'cli\.auth prompt'` is green (the test writer thread);
  - the three reviewers approve.
- **Tests (cli, real pty via `pty_pair.hpp`: `posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r`, RAII fds; V18):**
  - **Echo off:**
    - a writer thread waits until `tcgetattr(master)` shows `!ECHO`, then writes `s3cret\n`;
    - the answer is `s3cret`;
    - the master reads back no `s3cret`;
    - termios afterwards equals before;
    - `err` holds the question plus `\n`.
  - **Echo on:** the answer is returned and echoed on the master.
  - **Edge reads:** EOF (master closed) → `empty`; an empty line → `empty`; 5000 bytes → `too_long`.
  - **Interrupt:** `simulate_interrupt_for_testing(SIGINT)` from the writer after echo goes off → `interrupted`, echo restored (AC21 prompt half, AC40 core).
  - A non-tty fd (pipe) with `Echo::off` → `unavailable`.

### T8: Token layer: bodies, token response, error classification, `AuthClient`
- **Size:** L. **Security-reviewer:** yes.
- **Files:** create `src/core/auth/token.cpp`, `src/core/auth/auth_client.cpp`, `tests/unit/auth/token_test.cpp` and `tests/unit/auth/auth_client_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **The three body builders**, using T6's `form_encode` through `Secret::append`.
  - **`parse_token_response`:**
    - `parse_untrusted` (64 KiB, `Errc::network`, "MangaDex login response");
    - `access_token` required, `BearerToken::parse`;
    - `refresh_token` optional;
    - `expires_in` an integer 1–86400, else 900;
    - `refresh_expires_in` 0 or absent → nullopt;
    - the DOM's token strings are wiped before destruction (V25).
  - **`classify_token_error` (V2):** `invalid_grant` → `bad_credentials` (password grant) or `session_ended` (refresh grant); `invalid_client` → `bad_client`; `unauthorized_client` → `client_not_allowed`; anything else → `other`. `code` is kept only if it matches `[a-z_]{1,64}`; `description` is cut to 200 bytes.
  - **`token_refusal_error`:** the spec's exact 8/9 texts.
  - **`AuthClient`:**
    - its own `RateLimiter(clock, 1000 ms)`;
    - a POST with `RetryConfig` defaults: 429 honours `Retry-After`/`X-RateLimit-Retry-After`, 5xx and timeout/protocol are retried, **any other 4xx is answered once**;
    - the 4th logical request → `{content, network "more than 3 MangaDex login requests in one run; refusing to continue"}`;
    - non-2xx after retries → `{refused, info}` for 400/401/403 with a parsable body, else `{request, network "<url>: HTTP n"}`;
    - transport failure → `{request, …}`;
    - malformed 2xx → `{content, …}`;
    - `-vv` trace `POST <url> -> <status> (<ms> ms)` only;
    - `revoke` uses 5 s connect/stall and no retries;
    - `HttpRequest.body` is filled from the Secret and `secure_wipe`d after `fetch`.
- **Dependencies:** T1, T2, T4 (the RecordedRequest field), T6 (`form_encode`).
- **Done when:** `check` green; the three reviewers approve.
- **Tests (unit, FakeTransport + FakeClock):**
  - **AC1:** exact password, refresh and revoke bodies (a password with `& = + % space é`).
  - **AC3:** the response shapes; `expires_in` absent → 900; `refresh_expires_in` 0 → null; the rejects (missing access, CR/LF/space/`"` in a token, 16 385 bytes, non-JSON, 64 KiB + 1).
  - **AC4:**
    - the classification table;
    - 400/401 → exactly **one** POST recorded;
    - 503 ×3 then 200 → a token; 429 with `Retry-After: 2` → a 2 s sleep, then retry;
    - the 4th request → the cap error;
    - every recorded POST has an empty `authorization`;
    - the trace line contains no body.

### T9: Local auth inputs: credentials file, session JSON, `--password-stdin`, the API-base rule
- **Size:** M. **Security-reviewer:** yes.
- **Files:** create `src/core/auth/credentials_file.cpp`, `src/core/auth/session_json.cpp`, `src/core/auth/auth_inputs.cpp`, `tests/unit/auth/credentials_file_test.cpp`, `tests/unit/auth/session_json_test.cpp` and `tests/unit/auth/auth_inputs_test.cpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`parse_credentials_file`:**
    - the spec's format: comments, blanks, CRLF, first `=`, trim spaces and tabs, empty value means absent;
    - messages are `line <n>: …`, never the value. Unknown keys are echoed only if they match `[a-z_]{1,32}`;
    - UTF-8 is checked with `decode_utf8_at`.
  - **`load_credentials_file`:** `read_private_file` (T5), with the spec's messages built from the path and the `FileProblem`. An absent default file → an empty value.
  - **`parse_session` / `session_to_json`:**
    - `format_version` 1, every field validated, the issuer must equal the expected one, a reason phrase without tokens, wipe the DOM;
    - the serializer is hand-written into a Secret, with JSON escaping for issuer/client_id/username.
  - **`read_password_stdin`:**
    - `read_bounded_fd(fd, "stdin", 4096)`, then the first line;
    - EOF before any byte, a NUL, an empty first line, or > 4096 bytes → `Errc::usage` "--password-stdin was given, but stdin held no password";
    - the intermediate string is `secure_wipe`d.
  - **`check_auth_api_base`:** official or fixture, else the spec's exit-2 text with the command name.
- **Dependencies:** T1, T2, T5.
- **Done when:** `check` green; the three reviewers approve.
- **Tests (unit):**
  - **AC2:** every accept and reject row; a marker value planted in every bad line is absent from every message; an unknown key `Bad-Key!` is not echoed.
  - **AC5:** round trip; unknown `format_version`; wrong issuer; a bad token charset; 64 KiB + 1. No reason contains a token marker.
  - **`--password-stdin`** via `pipe2`: `pw\n`, `pw\r\n`, `pw` without a newline, `""`, `\n`, NUL, 4097 bytes, closed fd.
  - **API base:** official ok, fixture ok, `https://example.org` → 2 with `follows logs in only to MangaDex itself; --api-base must be the official API`.

### T10: Follows documents: page parser, status map, `StatusFilter`, filter
- **Size:** M. **Security-reviewer:** yes (untrusted JSON).
- **Files:** create `src/core/follows.cpp`, `src/core/follows_status.cpp`, `tests/unit/follows/follows_page_test.cpp`, `tests/unit/follows/status_map_test.cpp` and `tests/unit/follows/status_filter_test.cpp`.
- **Owns:** exactly those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **The URL builders.**
  - **`parse_follows_page`:** `MangaList`, `data[i].type == "manga"`, `is_uuid` → `MangaId`, `total`, attributes ignored. Fixed messages.
  - **`parse_status_map`:**
    - `parse_untrusted` with `max_separators = 1 000 000`;
    - `result == "ok"`; `statuses` an object or an empty `[]` (V24);
    - more than 100 000 entries → the exact refusal;
    - keys `is_uuid` → canonical, first wins on a canonical duplicate;
    - values `[a-z_]{1,32}`, else malformed;
    - a duplicate JSON key follows V23's behaviour, documented.
  - **`StatusFilter::parse`:** split commas, aliases (`on-hold`, `plan-to-read`, `re-reading`), exact lower case, de-duplication keeping the first, the spec's two exit-2 texts, the word through a 32-byte cut. The app sanitizes at print.
  - **`filter_follows`:** follows order, counts, unknown values.
- **Dependencies:** T1.
- **Done when:** `check` green; the three reviewers approve.
- **Tests (unit):** AC50, AC51 (every listed row, a planted marker never echoed), AC52 (FS1 exactly as the spec's table, duplicates survive), plus the follows page accepts and rejects.

### T11: `ApiClient::get_authorized` and `auth_endpoints_for`
- **Size:** S. **Security-reviewer:** yes.
- **Files:** edit `src/core/api_client.cpp`; create `src/core/auth/auth_endpoints.cpp` and `tests/unit/api_client/api_client_authorized_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - `GetSpec` gains `const Secret* authorization = nullptr`. `perform` copies it into `HttpRequest.authorization` and wipes the copy before returning, on every path.
  - `get_authorized` builds the URL from the enum (`follows_request_url` / `status_map_request_url`, T10, linked at T13 time; here the format is inlined or T10's declaration is used only from batch 4). **Note:** to avoid a cross-track link dependency in batch 3, T11 formats the two URLs itself with the same fixed strings, and T13's test pins equality with T10's builders.
  - The official/fixture check runs before any I/O.
  - `auth_endpoints_for`: official → issuer, token, revoke; fixture → the `file://<root>/auth/realms/mangadex` forms; other kinds → nullopt.
- **Dependencies:** T1, T2, T4.
- **Done when:**
  - `check` is green;
  - `unit.api_client*` passes **unmodified**, including `:304-321`;
  - the three reviewers approve.
- **Tests (unit, FakeTransport):**
  - the exact URLs and authorization;
  - a 401 is returned as a response (not an error), once (not retried);
  - 503 → retried;
  - a custom https endpoint → usage, with no request recorded;
  - **no other `fetch_*` ever records a non-empty authorization** (AC8 at the client level: a manga/feed/covers/at-home/page/cover/list run after a `get_authorized` call).

### T12: Login session flow and logout (core)
- **Size:** L. **Security-reviewer:** yes.
- **Files:** create `src/core/auth/login_session.cpp`, `src/core/auth/logout.cpp`, `tests/support/fake_prompter.hpp`, `tests/unit/auth/login_session_test.cpp` and `tests/unit/auth/logout_test.cpp`.
- **Owns:** exactly those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`LoginSession::open`:**
    1. `auth_endpoints_for` (nullopt → usage);
    2. `state_dir_path`, then `StateDir::open(create)`. Problems become `notes.warnings`: `ignoring the saved login at <path>: <why>`, or `could not save … no home directory` later;
    3. the lock (timeout → `{local, failure}`);
    4. read and parse the slot (fixture slot iff fixture), checking issuer, client and username against the credentials file → warning plus ignore;
    5. **mode `reuse`:** saved access valid ≥ 60 s (`clock.wall_now()`) → `how = saved`, 0 POSTs; else refresh if a refresh token exists; `session_ended` → the password fallback;
    6. **mode `password` (login):** straight to the password grant;
    7. the password grant gathers per field in the Q1 order (file > session > prompt for client_id/secret/username; stdin > file > prompt for the password). The client hint `notice` comes before the first client prompt, and the password question names the username. A missing detail → `login_required` with the right spec text (no client / not logged in / session ended);
    8. **after every token response:** persist `session_to_json` before anything else, also before checking a pending interrupt. A failed write → `notes.save_error` and a warning, continue.
  - **`authorization()`:** refresh first if the token expires within 30 s.
  - **`refresh_after_unauthorized()`:** refresh, `session_ended` → the password fallback (stdin/file Secrets kept until `close`; a prompt if a prompter was given).
  - **The 3-request cap** comes through `AuthClient`.
  - **`close()`:** wipes every Secret and releases the lock; idempotent.
  - **`logout`:** `StateDir::open(create=false)` (missing → not logged in), lock, read, parse → `revoke_body` → `AuthClient::revoke` (failure → the spec's warning), `remove`. Spec issue 9's rules apply.
  - **`FakePrompter`:** a scripted answer queue, a call log (question and echo), and a scripted `interrupted`/`empty`.
- **Dependencies:** T5, T8, T9 (and T1, T2, T6 for its fixture-free FakeTransport scripts).
- **Done when:**
  - `check` and `-e gcc check` are green;
  - `nice pixi run tsan -R 'unit\.auth'` is green;
  - the three reviewers approve.
- **Tests (unit, FakeTransport + FakeClock + FakePrompter + `TempDir` dirs):**
  - **AC6:** every order and count. The rotated refresh is the one stored: read the file back.
  - **AC13 core:** the prompt order and texts, then the hint.
  - **AC14/15 core:** no prompter and no session → `login_required` with the exact text and **zero** requests.
  - **AC17/18 core:**
    - a password mismatch → 9, the session unchanged (bytes compared);
    - refresh `invalid_grant` + a password in the file → silent, rewritten;
    - without one → 8 "expired".
  - **AC21 core:** `simulate_interrupt` after a token response → the session was written first.
  - **AC22 core:** the state dir is a regular file → a warning, the session still usable in memory.
  - **AC23:** fixture mode writes only `fixture-session.json`.
  - **AC25 core.**
  - **AC24 core:**
    - logout: revoke POST sent, file removed;
    - again → not logged in;
    - a revoke 500 → warning, removed;
    - malformed → removed plus warning.
  - **Mismatch:** the credentials file names another client → warning plus a fresh login.

### T13: Collection fetch: follows pages and the status map
- **Size:** M. **Security-reviewer:** yes.
- **Files:** create `src/core/follows_fetch.cpp` and `tests/unit/follows/follows_fetch_test.cpp`.
- **Owns:** exactly those 2 files.
- **Build files for build-fixer:** none.
- **What changes:** `fetch_follows_collection`, in mdlist's `fetch_custom_list` shape:
  - **Declaration order:** `stop_source` → `ApiClient` (`endpoints_for(api_base, false)`: no reporter) → `InterruptBridge`.
  - **Per page:**
    1. `provider.authorization(stop)`;
    2. `get_authorized(user_follows_manga, offset, …)`;
    3. 401 → `refresh_after_unauthorized`, then retry **that page once**; a second 401 → `{refused, auth_rejected "MangaDex rejected a new access token for your follows (HTTP 401); …"}`;
    4. other non-2xx → `{request, status mapping}`;
    5. parse → `{content, …}` on error.
  - **Paging:** paginate's stepping rules (Spec issue 1); more than 10 000 → `{content, network cap message}`.
  - **Status map:** with `with_statuses` and ≥ 1 follow, one status-map request with the same token handling ("your reading statuses").
  - **The provider is called only on this thread.**
- **Dependencies:** T10, T11 (and T1).
- **Done when:**
  - `check` is green;
  - `nice pixi run tsan -R 'unit\.follows'` is green (InterruptBridge);
  - the three reviewers approve.
- **Tests (unit, FakeTransport + a test-local `FakeTokenProvider`):**
  - **AC7:**
    - URLs equal T10's builders;
    - an expiry before page 2 → the provider refreshes first (recorded order);
    - a 401 on page 2 → refresh plus one retry; a second 401 → 9;
    - total 10 001 → cap; a non-UUID → content;
    - a stop → interrupted;
    - the flag set before → no request.
  - **AC53, AC54:** follows first, then exactly one `/manga/status`; zero follows → no status request; 429/5xx retried, then `request`; 16 MiB + 1 → `request` (the transport cap).

### T14: Follows settings, plan, the `follows` JSON object and the runner overload
- **Size:** M. **Security-reviewer:** yes (the JSON must carry no secret).
- **Files:**
  - create `src/core/follows_plan.cpp`, `tests/unit/follows/follows_plan_test.cpp`, `tests/unit/batch/batch_json_follows_test.cpp` and `tests/cli/batch_jobs_follows_test.cpp`;
  - edit `src/core/batch_json.cpp`, `src/app/cmd_batch_jobs.hpp` and `src/app/cmd_batch_jobs.cpp`.
- **Owns:** exactly those 7 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`validate_follows_args`**, in this order: `DIR must not be empty`; DIR is a manga or list id → the spec text; `detail::validate_common`; `StatusFilter::parse` when `status` is non-empty. The result has label `follows`, source `follows` and `file = "follows"`.
  - **`plan_follows`:** `plan_list`'s rules with positions 1..k (`mark_duplicates`).
  - **`batch_to_json(…, FollowsSummary)`:** `"follows": {username, titles, statuses|null, selected}` right after `source`. The existing overloads stay byte-identical.
  - **`run_batch_jobs`:** an overload taking `const FollowsSummary&`; both forward to one internal body that holds a `std::variant<std::monostate, ListSummary, FollowsSummary>`.
- **Dependencies:** T10 (StatusFilter), T1.
- **Done when:**
  - `check` is green;
  - every `unit.batch*`, `cli.batch*`, `cli.mdlist*`, `e2e.batch.*` and `e2e.mdlist.*` passes unmodified (byte-identical documents and goldens);
  - the three reviewers approve.
- **Tests:**
  - **AC11 unit:** the key order; `statuses` null; `selected == titles`; empty `titles: []`; batch and mdlist documents byte-equal to the old overloads.
  - **AC19 unit half:** `''`, a uuid, a list URL.
  - **cli:** hand-built follows jobs through `run_batch_jobs`: the prefix `follows:2: 6666…: `, item wording, the closing line, a duplicate.

### T15: Follows fixture trees for the cli and E2E tiers
- **Size:** M. **Security-reviewer:** yes (committed and generated fake credentials).
- **Files:** create `tests/support/mdx_follows_fixture.hpp` and `tests/unit/support/mdx_follows_fixture_test.cpp`; edit `tests/e2e/fixtures/fixture_gen.cpp` and `tests/e2e/inputs/README`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`follows_fixtures()`**, a list separate from `all_fixtures()`, which stays at twelve. Each entry is a base tree (`clean`, `bulk` or `nochapters` maker), plus `FixtureAuth`, plus a follows set (pages and page size), plus an optional status document. Trees:
    - `follows-clean` (F1);
    - `follows-unknown` (F2);
    - `follows-empty` (F3);
    - `follows-twice` (F4);
    - `follows-bad-id` (F5);
    - `follows-bulk` (F6: 2 pages of size 1, bulk twice);
    - `follows-nochapters` (F7: 5555…, 6666…);
    - `follows-status` (FS1);
    - `follows-status-empty` (FS2, `[]`);
    - `follows-status-bad-value` (FS3a);
    - `follows-status-bad-key` (FS3b).
  - **`creds/` in each tree:** `credentials` (0600, all four keys), `credentials-client` (0600, no password), `credentials-wrong-password` (0600), `credentials-0644` (`fchmod 0644`), `password.txt` and `password-wrong.txt`. Written with `openat(O_CREAT|O_EXCL|O_NOFOLLOW, mode)` plus `fchmod` (V29).
  - **`fixture_gen.cpp`:** writes `follows_fixtures()` after `all_fixtures()`; the final count message is updated.
  - **README:** documents the trees and states that the credential values are fake.
- **Dependencies:** T6 (`mdx_auth_fixture.hpp`, routing), T10, T11 (its test parses what it serves).
- **Done when:**
  - `check` is green;
  - `e2e.fixtures` still writes every tree within its 120 s timeout;
  - the existing twelve trees are byte-identical;
  - the three reviewers approve.
- **Tests (unit):**
  - every tree writes;
  - the creds modes are exactly 0600 and 0644 under umask 077 and 022;
  - each follows set, served through `make_fixture_transport` with the fixture access token, parses with T10's parsers to the expected ids;
  - FS3a/FS3b give the expected malformed error.

### T16: App auth glue (options, the login phase, prompter choice) and the `StderrTrace` extraction
- **Size:** M. **Security-reviewer:** yes.
- **Files:** create `src/app/cmd_auth.hpp`, `src/app/cmd_auth.cpp`, `src/app/cmd_trace.hpp`, `src/app/cmd_trace.cpp` and `tests/cli/auth_phase_test.cpp`; edit `src/app/cmd_mdlist.cpp` (use the extracted trace).
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cmd_trace`:** `StderrTrace` moved verbatim.
  - **`cmd_auth.hpp`:**
    - `void bind_auth_options(CLI::App&, RawAuthArgs&)` adds `--credentials FILE` ("Read login details from FILE", `std::optional`, no default, not ExistingFile), `--password-stdin` ("Read the password from stdin") and `--no-input` ("Never prompt; fail if a login is needed");
    - `UserDirs user_dirs(const EnvSnapshot&)`;
    - `struct LoginPhase { std::unique_ptr<LoginSession> session{}; LoginNotes notes{}; };`
    - `[[nodiscard]] std::expected<LoginPhase, int> run_login_phase(const RawAuthArgs&, const ApiBase&, LoginMode, Io&, const OutputPolicy&, const Backend&, StderrTrace&, std::string_view command);`. It reads `--password-stdin` (exit 2 on failure), loads the credentials file (exit 1), chooses the prompter (`stdin_tty && stderr_tty && !no_input && !password_stdin` → `io.prompter` or a local `TerminalPrompter{io.stdin_fd, io.err}` declared before the session), runs `LoginSession::open`, prints the warnings and the `-v` line `logged in to MangaDex as <user> (saved login|refreshed|password)` (username cut to 64 bytes; `print_note` sanitizes), and on failure prints `with_remedy` (`request`) or the plain message, returning the code;
    - `void guard_secrets()` calls `disable_core_dumps()`.
- **Dependencies:** T7, T9, T12, T15.
- **Done when:**
  - `check` is green;
  - every `cli.mdlist*` and `e2e.mdlist.*` passes unmodified (the extraction);
  - the three reviewers approve.
- **Tests (cli, via `run_login_phase` against `follows-*` trees in a `TempDir`, `EnvSnapshot` paths pointing at temp XDG dirs):**
  - AC14, AC15, AC16 (0644 → 1 with the chmod hint; all four keys → 0 and no prompt; an explicit missing path → 1), AC17, AC18, AC25 and AC26 (cli: the lock held through a second fd → exit 1 instantly via InstantClock);
  - the TTY rule (each of the four conditions);
  - `--password-stdin` via a pipe;
  - no test ever has a null `EnvSnapshot` HOME pointing at the real home.

### T17: `login` and `logout` commands (not yet registered)
- **Size:** M. **Security-reviewer:** yes.
- **Files:** create `src/app/cmd_login.hpp`, `src/app/cmd_login.cpp` and `tests/cli/login_cli_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - `register_login` / `register_logout` (`LoginCommand` owns `RawLoginArgs`, `LogoutCommand` owns `RawLogoutArgs`), `run_login_command` / `run_logout_command`, and the help footers (Spec issue 12).
  - **`login`:**
    1. `check_auth_api_base` (2);
    2. handlers, `guard_secrets`, agent, backend, trace;
    3. `run_login_phase(…, LoginMode::password, …, "login")`;
    4. `notes.save_error` → exit 1 `login could not save the session to <path>: <why>`;
    5. else `logged in to MangaDex as <user>; the login is saved in <path>` (not with `-q`). stdout is always empty.
  - **`logout`:** handlers, guard, backend, `logout()` → the spec's messages. stdout empty.
- **Dependencies:** T16.
- **Done when:**
  - `check` is green;
  - `nice pixi run tsan -R 'cli\.login'` is green;
  - the three reviewers approve.
- **Tests (cli):** AC24; AC22 (login half: the state dir is a file → 1); interrupt at the password prompt via FakePrompter `interrupted` → interrupted path, no session file (AC21); `--json` → empty stdout; the fixture slot only (AC23).

### T18: `follows` command (not yet registered)
- **Size:** L. **Security-reviewer:** yes.
- **Files:** create `src/app/cmd_follows.hpp`, `src/app/cmd_follows.cpp`, `tests/cli/follows_cli_test.cpp` and `tests/unit/follows/follows_no_leak_test.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`FollowsCommand`** owns `RawFollowsArgs`. `bind()` adds `DIR`, `bind_selection_options`, `bind_run_options`, `--fail-fast`, `--dir-per-title`, `--status` (the `--group` pattern, V8), then `bind_auth_options`.
  - **Footer:** the spec's LOGIN / READING STATUS / Examples / re-run / symlink text, then `exit_status_footer()`.
  - **Description:** "Download every title you follow on MangaDex (needs a login)".
  - **`run_follows_command`, phases:**
    1. `validate_follows_args` + `check_auth_api_base` → 2;
    2. handlers → `guard_secrets` → agent → backend → trace;
    3. `run_login_phase(reuse)`;
    4. `fetch_follows_collection` (members filled one by one): failure → `with_remedy` for `request`, plain for `content`/`refused`, interrupted → silent, no JSON;
    5. `session->close()` (lock released, secrets wiped) **before** any download;
    6. `filter_follows` if `--status`, plus the `-v` lines;
    7. the empty and none-matched warnings plus JSON;
    8. `run_batch_jobs(jobs, settings.batch, *backend, agent, io, policy, follows_summary)`.
  - **Declaration order:** handlers → agent → backend → trace → (prompter inside the glue) → `LoginPhase` → collection → jobs.
- **Dependencies:** T13, T14, T16.
- **Done when:**
  - `check` and `-e gcc check` are green;
  - `nice pixi run tsan -R 'cli\.follows|unit\.follows'` is green;
  - the three reviewers approve. `memory-safety-reviewer` needs evidence that the login phase, the collection bridge and per-item runs were exercised under ASan+UBSan and TSan.
- **Tests:**
  - **unit (`follows_no_leak_test.cpp`), AC8:** a `RecordingTransport` wrapping the fixture transport records `(url, authorization)`. `LoginSession` → `fetch_follows_collection` → `run_download` for item 1 runs on `follows-status` without and with `--status reading`. Every request off the two-path allowlist has an empty authorization, and only auth-host POSTs carry a body.
  - **cli (`run_follows_command` on `follows-*` trees):**
    - AC13 (FakePrompter, then a second run with no prompt and ≤1 POST);
    - AC19 (validate-level: `''`, uuid, `--api-base https://example.org`, `--password-stdin` empty);
    - AC20 (grep stdout and stderr for every fixture token, secret and password: absent);
    - AC21 (the flag set before → no JSON, `out` absent);
    - AC22 (follows half);
    - AC55–AC58;
    - `/dev/full`;
    - color.

### T19: Registration, extras policy, exit rows 7–9, footers and every help golden (the single bless point)
- **Size:** M. **Security-reviewer:** yes (argv echo policy).
- **Files:**
  - edit `src/app/cli.cpp`, `src/app/exit_help.hpp`, `src/app/cmd_mdlist.cpp` (Spec issue 7 line) and `tests/cli/{cli_test,download_cli_test,batch_parse_test,mdlist_parse_test}.cpp`;
  - create `tests/cli/follows_parse_test.cpp` and `tests/e2e/cases/{follows,login,logout}.cmake` (help and dry-run golden cases only).
- **Owns:** exactly those 11 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cli.cpp`:**
    - `register_follows`, `register_login`, `register_logout` after mdlist;
    - the commands list `download, batch, mdlist, follows, login, logout`;
    - `title_option_owner` covers `follows` (Spec issue 4);
    - for `follows`/`login`/`logout`: secret-shaped extras (`--password…`, `--client-secret…`, `-p`) → the spec's fixed message, and every other ExtrasError → value-free (Spec issue 5, V7, V11).
  - **`exit_help.hpp`:** row 7 `several titles failed in different ways (see per-title results)`; rows 8 and 9 as in the spec.
  - **`cmd_mdlist.cpp`:** the follows line (Spec issue 7).
  - **Pinned tests:**
    - `cli_test.cpp:66, :85`;
    - `download_cli_test.cpp:446`;
    - `batch_parse_test.cpp:228`;
    - `mdlist_parse_test.cpp:223, :229`.
  - **`follows.cmake`:**
    - `help` (GOLDEN);
    - `dry-run` (GOLDEN): `follows out --dry-run --credentials {{FIXTURES}}/follows-clean/creds/credentials --api-base file://{{FIXTURES}}/follows-clean`, exit 0, stderr empty, `out` absent;
    - `json-dry-run` (GOLDEN).
  - **`login.cmake` / `logout.cmake`:** `help` (GOLDEN) each.
- **Dependencies:** T17, T18.
- **Done when:**
  - the human has blessed (one round, below);
  - `check` is green;
  - the three reviewers approve.
- **Tests (cli, `follows_parse_test.cpp` through `run_cli`):**
  - AC19's argv cases: `hunter2` absent from stderr for `--password hunter2`, `--password=hunter2`, `follows out hunter2`, `-p x`;
  - `--title X` and `--title=X`;
  - globals after DIR;
  - `--status reading out` keeps DIR;
  - `login --status reading` → 2 (AC59);
  - root help lists the three one-liners;
  - no `--api-base` in any help;
  - `download`/`batch`/`mdlist` smoke runs.

### T20: E2E cases and edge-suite additions
- **Size:** L. **Security-reviewer:** no (tests only; the board covers it).
- **Files:** edit `tests/e2e/cases/{follows,login,logout}.cmake` (append) and `tests/e2e/cases/_edge.cmake` (append a `follows-*` block; `EDGE_*` knobs unchanged).
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **Dependencies:** T19.
- **Done when:**
  - `check` is green;
  - `nice pixi run release-test` is green (every new case on the hardened install);
  - `quality-reviewer` and `memory-safety-reviewer` approve.
- **Tests:** see the E2E and edge tables below.

### T21: README
- **Size:** M. **Security-reviewer:** yes (security notes and credential handling text).
- **Files:** edit `README.md`.
- **Owns:** `README.md`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **Contents and intro.**
  - **New section "The titles you follow: `follows`, `login`, `logout`":**
    - creating a personal client and its approval;
    - the first run, cron, the credentials file format and chmod 600;
    - `--password-stdin` (and leading/trailing spaces);
    - the session path, and that it never holds the password;
    - `--status` semantics, including that `titles[].status` is unrelated to the reading status;
    - order and dedupe, the 10 000 cap, anonymous downloads, `--dry-run` still logs in, MFA bypass, logout and the credentials file, the JSON `follows` object, stderr shapes.
  - **mdlist's "No login" bullet** now points to `follows` (Spec issue 7).
  - **Exit table:** row 7 reworded, rows 8 and 9 added; the `mixed_failures` wording.
  - **"Environment and configuration":** "No cache, config or state directories are used" becomes false. State the state dir, the optional config credentials file, and the three env vars read (`HOME`, `XDG_CONFIG_HOME`, `XDG_STATE_HOME`).
  - **Security notes:** secrets never on argv, the core-dump guard, the file checks.
  - **Limits:** the follows and status caps.
- **Dependencies:** T19.
- **Done when:** `check` green; `quality-reviewer` and `security-reviewer` approve; `memory-safety-reviewer` confirms "no C++ changed".

---

## Parallelization plan

**Batch 1 (parallel): T1, T2, T3 · Batch 2 (parallel): T4, T5, T6, T7 · Batch 3 (parallel): T8, T9, T10, T11 · Batch 4 (parallel): T12, T13, T14, T15 · Batch 5: T16 · Batch 6 (parallel): T17, T18 · Batch 7: T19 (cli.cpp registration + exit_help + bless) · Batch 8 (parallel): T20, T21**

| Batch | Tracks (size) | Why parallel-safe |
|---|---|---|
| 1 | T1 (M), T2 (S), T3 (XS) | T1 is the **only** track touching `include/mangadex_dl/*` and `src/app/io.hpp`. T2 implements T1's `Secret` declarations in its own new files. T3 is docs |
| 2 | T4 (M), T5 (L), T6 (L), T7 (M) | **The risky foundations, reviewed first.** Disjoint files: curl + FakeTransport; the session store; the fixture transport + form + auth fixture header; the app prompter. All use only T1 headers and T2's `Secret` |
| 3 | T8 (L), T9 (M), T10 (M), T11 (S) | Token layer, local inputs, follows documents, ApiClient. T11 inlines the two URL formats so it doesn't link against T10 |
| 4 | T12 (L), T13 (M), T14 (M), T15 (M) | T13 uses a fake TokenProvider, not T12. T14 is batch JSON/plan. T15 is test support. **Shared-header caution:** T12 and T13 include `fake_transport.hpp` (frozen since T4) |
| 5 | T16 (M) | Glue every command needs. It also extracts `StderrTrace` from `cmd_mdlist.cpp` |
| 6 | T17 (M), T18 (L) | Both depend only on T16, T13 and T14. Disjoint `cmd_login.*` / `cmd_follows.*` |
| 7 | T19 (M) | The late registration task: sole owner of `cli.cpp` and `exit_help.hpp` |
| 8 | T20 (L), T21 (M) | E2E vs README, disjoint |

At most four tracks run at once; all share `build/default/dev` under the lock. A half-written file in one track breaking another's build is a coordination event, never a fix.

**Serialization points and their single owner:**

| Serialization point | Task (batch) |
|---|---|
| `include/mangadex_dl/error.hpp`, `exit_codes.hpp`, `net/transport.hpp`, `net/fixture_transport.hpp`, `api_client.hpp`, `batch.hpp`, and new `secret.hpp`, `prompt.hpp`, `session_store.hpp`, `auth.hpp`, `follows.hpp` | T1 (B1) only |
| `src/app/io.hpp` (+ `io.cpp`) | T1 (B1) only |
| `src/app/cli.cpp`, `src/app/exit_help.hpp` | T19 (B7) only |
| `tests/e2e/cases/_edge.cmake` | T20 (B8) only |
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

**`security-reviewer` in the track loop:** T1, T2, T4–T19 and T21. That is every task except T3 (docs) and T20 (tests only, covered by the board).

---

## Test plan (per tier)

### AC → task → tier

| AC | Task | Tier | AC | Task | Tier |
|---|---|---|---|---|---|
| 1 | T6 (encode), T8 (bodies) | unit | 27 | T19 | e2e golden |
| 2 | T9 | unit | 28–33 | T20 (33's goldens in T19) | e2e |
| 3, 4 | T8 | unit | 34–38 | T20 | edge |
| 5 | T9 | unit | 39, 40 | T7 (pty, cli) + cli-tester | cli + tty-pty |
| 6 | T12 | unit | 41–49 | — | human |
| 7 | T13 | unit | 50–52 | T10 | unit |
| 8 | T11 (client), T18 (full run) | unit | 53, 54 | T13 | unit |
| 9 | T4 | unit | 55–58 | T18 | cli |
| 10 | T6 | unit | 59 | T18 (status words), T19 (`login --status`) | cli |
| 11 | T14 | unit, cli | 60 | T20 | e2e |
| 12 | T1 | unit | 61 | T20 | edge |
| 13 | T12 (core), T18 | unit, cli | 62 | — | human |
| 14–18 | T12 (core), T16 | unit, cli | | | |
| 19 | T14, T18, T19 | unit, cli | | | |
| 20, 21 | T18 (21 prompt half: T7, T17) | cli | | | |
| 22 | T12, T16, T17, T18 | unit, cli | | | |
| 23 | T12, T17 | unit, cli | | | |
| 24 | T12, T17 | unit, cli | | | |
| 25, 26 | T5, T12, T16 | unit, cli | | | |

### Unit (Catch2 against `mangadex_dl::core`)

| File | Task | Covers |
|---|---|---|
| `tests/unit/interfaces/headers_test.cpp` | T1 | 8/9 mapping, traits, constants |
| `tests/unit/secret/{secret,process_guard}_test.cpp` | T2 | Secret wipe/move/growth; dump guard in a child |
| `tests/unit/net/curl_transport_auth_test.cpp` | T4 | AC9, POSTFIELDS, the slist |
| `tests/unit/auth/{session_store,private_file}_test.cpp` | T5 | modes, symlinks, atomic write, lock (AC26 core) |
| `tests/unit/auth/form_test.cpp`, `tests/unit/net/fixture_transport_auth_test.cpp` | T6 | AC1 (encode), AC10 |
| `tests/unit/auth/{token,auth_client}_test.cpp` | T8 | AC1 (bodies), AC3, AC4 |
| `tests/unit/auth/{credentials_file,session_json,auth_inputs}_test.cpp` | T9 | AC2, AC5, stdin, API base |
| `tests/unit/follows/{follows_page,status_map,status_filter}_test.cpp` | T10 | AC50–52 |
| `tests/unit/api_client/api_client_authorized_test.cpp` | T11 | allowlist, AC8 (client) |
| `tests/unit/auth/{login_session,logout}_test.cpp` | T12 | AC6, 13–18, 21–25 (core) |
| `tests/unit/follows/follows_fetch_test.cpp` | T13 | AC7, 53, 54 |
| `tests/unit/follows/follows_plan_test.cpp`, `tests/unit/batch/batch_json_follows_test.cpp` | T14 | AC11, AC19 (validate) |
| `tests/unit/support/mdx_follows_fixture_test.cpp` | T15 | trees, modes |
| `tests/unit/follows/follows_no_leak_test.cpp` | T18 | AC8 (full) |

Every existing `unit.*` test stays green **unmodified** (except `headers_test.cpp`).

### In-process CLI (`tests/cli/`, flat)
- **`auth_prompt_test.cpp` (T7):** pty echo-off/on, EOF, too long, interrupt, non-tty.
- **`batch_jobs_follows_test.cpp` (T14):** follows wording through `run_batch_jobs`.
- **`auth_phase_test.cpp` (T16):** AC14–18, 25, 26; the TTY rule; `--password-stdin`.
- **`login_cli_test.cpp` (T17):** AC21 (prompt), 22 (login), 23, 24.
- **`follows_cli_test.cpp` (T18):** AC13, 19, 20, 21, 22, 55–58, `/dev/full`, color.
- **`follows_parse_test.cpp` (T19):** argv and extras policy (no `hunter2`), `--title`, globals, help content, AC59.
- **Edited in T19:** `cli_test.cpp`, `download_cli_test.cpp`, `batch_parse_test.cpp`, `mdlist_parse_test.cpp`.
- **TSan runs:**
  - T6: `-R 'unit\.(fixture|net)'`;
  - T7: `-R 'cli\.auth prompt'`;
  - T12: `-R 'unit\.auth'`;
  - T13: `-R 'unit\.follows'`;
  - T17: `-R 'cli\.login'`;
  - T18: `-R 'cli\.follows|unit\.follows'`;
  - board: `nice pixi run tsan -R '(cli|unit)\.(follows|login|auth|batch|mdlist)'`.

### E2E cases (tests `e2e.follows.*`, `e2e.login.*`, `e2e.logout.*`)
Fixture mode `--api-base file://{{FIXTURES}}/<tree>` throughout. `C` stands for `--credentials {{FIXTURES}}/<tree>/creds/credentials`. DIR is `out`. Exit codes are exact.

| Case | Task | Asserts |
|---|---|---|
| `follows.help`, `login.help`, `logout.help` | T19 | 0, stderr empty, no ANSI, GOLDEN (AC27) |
| `follows.dry-run`, `follows.json-dry-run` | T19 | `follows-clean` + C `--dry-run` → 0, `out` absent, GOLDEN (AC33) |
| `follows.no-session` | T20 | stdin NULL, no C → 8, the exact "not logged in" message, stdout empty, `out` absent (AC28) |
| `follows.no-client` | T20 | → 8 "no MangaDex API client is set up…" |
| `follows.creds-0644` | T20 | `--credentials …/creds/credentials-0644` → 1, the chmod hint, no fixture value on stderr (AC29) |
| `follows.clean` | T20 | C → 0, the two clean paths, `EXPECT_EPUB`, no `.part-` (AC30) |
| `follows.json` | T20 | `--json` → `EXPECT_JSON`, `"source": "follows"`, `"follows": {` (AC30) |
| `follows.dir-per-title` | T20 | → files under `out/Clean Title/` (AC30) |
| `follows.seeded` | T20 | `PRE_RUN login C …`, then `follows out` with **no C** → 0 (the session reused) |
| `follows.unknown`, `follows.nochapters`, `follows.empty`, `follows.twice` | T20 | 4 with `follows:2: 6666…:`; 7; 0 plus "you follow no titles" plus `out` absent; the duplicate warning (AC31) |
| `follows.bad-id` | T20 | → 3, malformed, **no** remedy, `out` absent |
| `follows.password-stdin`, `follows.password-stdin-null` | T20 | `credentials-client` + `STDIN {{FIXTURES}}/…/password.txt --password-stdin` → 0; `STDIN NULL` → 2 (AC32) |
| `follows.wrong-password` | T20 | `credentials-wrong-password` → 9, the password message |
| `follows.status-reading`, `follows.status-json`, `follows.status-dry-run` | T20 | `follows-status`: → 4? No: `--status reading` → 0 with the clean paths; `reading,completed --json` → 4 with `EXPECT_JSON` and `"selected": 2`; `--dry-run --status reading` → 0, `out` absent (AC60) |
| `follows.status-bad`, `follows.status-all` | T20 | `--status paused` / `all` → 2, the exact text, stdout empty |
| `follows.usage-*` | T20 | `''` DIR, a uuid DIR, `--api-base https://example.org`, `--password x` (no `x` on stderr) → 2 |
| `login.ok`, `login.no-tty` | T20 | `login C` → 0, stdout empty, stderr "logged in … saved in"; `login` alone stdin NULL → 8 |
| `logout.after-login`, `logout.twice` | T20 | `PRE_RUN login C`, then `logout` → 0 "logged out; removed"; with no session → 0 "not logged in" |

### Edge-suite additions (`_edge.cmake`, T20 only; tests `e2e.edge.follows-*`)

| Case | Asserts |
|---|---|
| `follows-stdin-null-session`, `follows-stdin-closed-session` | `PRE_RUN login C`, then `--dry-run` with `STDIN NULL` / `CLOSED` → 0 (AC34) |
| `follows-stdin-closed-no-session` | → 8, never 70, no hang (AC34) |
| `follows-password-stdin-closed` | `--password-stdin`, `STDIN CLOSED` → 2 |
| `follows-dev-full` | C `--dry-run`, `STDOUT_FILE /dev/full` → 1, `^mangadex-dl: write error` (AC35) |
| `follows-sigpipe`, `follows-sigpipe-parent-ignores` | `follows-bulk` + C `--dry-run --layout chapters`, `PIPE_HEAD 1` → SIGPIPE, stderr empty (AC36) |
| `follows-json-clicolor-force`, `follows-pipe-stderr-no-ansi` | `EXPECT_JSON EXPECT_NO_ANSI`; `follows-unknown` piped → 4, no ANSI (AC37) |
| `follows-invalid-utf8-dir-json` | DIR `{{INVALID_UTF8}}`, `--json --dry-run` → 0, `EXPECT_JSON` (AC38) |
| `follows-invalid-utf8-extra` | `follows out {{INVALID_UTF8}}` → 2, the bytes not echoed, not 70 (AC38) |
| `follows-status-invalid-utf8-lc-c` | `--status {{INVALID_UTF8}}`, `LC_ALL=C` → 2, not 70 (AC61) |
| `follows-status-json-clicolor-force` | `--json --status reading --dry-run`, `CLICOLOR_FORCE=1` → no ANSI (AC61) |
| `login-stdin-null` | `login`, `STDIN NULL` → 8 |

The `EDGE_*` knobs stay on `download`. TTY prompt typing is not expressible: the driver runs `script` with `</dev/null`. AC39/40 stay with `cli-tester` (pty with piped keystrokes) and the human.

### Sanitizer-control additions
None. The feature adds no new sanitizer-detectable risk class:
- the new threads are one `InterruptBridge` poller (as in mdlist) and test-only pty writers;
- the C-string interop (curl slist, `openat` names) is through owned `std::string`s;
- views over secrets are covered by the existing `dangling_view_heap` / `dangling_view_scope` controls.

**Evidence for `memory-safety-reviewer`:** those controls plus `heap_overflow`, `signed_overflow`, `leak` and, under tsan, `data_race` fired in the T2, T12 and T18 gate runs. V20 additionally proves LSan still fires after `PR_SET_DUMPABLE 0`.

### Release-test
- **T2:** `release-check.binary` must stay within the glibc floor with `explicit_bzero`.
- **T4:** curl changes in the shipped binary.
- **T20 and the board:** every new E2E/edge case on the hardened static-libstdc++ install.
- The gcc env runs in T1, T4, T5, T7, T12 and T18, and on the board.

### Goldens that will change (human-blessed, **one bless point, after T19**)

| Golden | Why |
|---|---|
| `edge.help` | root SUBCOMMANDS gains follows, login and logout; exit row 7 reworded, rows 8 and 9 added |
| `download.help`, `batch.help` | exit rows 7–9 |
| `mdlist.help` | exit rows 7–9; the follows line (Spec issue 7) |
| `follows.help` (new) | the new command (including `--status` and READING STATUS) |
| `login.help`, `logout.help` (new) | the new commands |
| `follows.dry-run`, `follows.json-dry-run` (new) | planned paths, and batch's document plus `follows` |

**Bless request** (T19): `pixi run bless '^e2e\.((edge|download|batch|mdlist|follows|login|logout)\.help|follows\.(json-)?dry-run)$'`. Reason: three new commands, exit rows 7–9, the mdlist footer line, and two new GOLDEN dry-run cases.

**Must not change:** `edge.version`, `download.dry-run-clean`, `download.json-dry-run`, `batch.dry-run`, `batch.json-dry-run`, `mdlist.dry-run` and `mdlist.json-dry-run`. Any golden diff in T1–T18 or T20–T21 is a regression, never a bless.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**Secrets**
1. **`Secret` (T2):**
   - SSO move leaves the bytes in the source buffer: the wipe-after-move must run in both the move ctor and move assignment;
   - `resize(capacity())` must never reallocate (V19);
   - growth wipes the old buffer;
   - `reveal()` is LIFETIMEBOUND: never `Secret::copy_of(x).reveal()` stored, never a `string_view` member pointing into a Secret.
2. **Plain-string copies of secrets:**
   - `HttpRequest.body` and `.authorization` (T8, T11);
   - `GetSpec`'s copy;
   - the curl slist (T4);
   - the nlohmann DOM strings (T8, T9);
   - `read_bounded_fd`'s result (T9).

   Each is wiped before it dies. `std::format` never receives `reveal()`.
3. **`CURLOPT_POSTFIELDS`** points into `request.body`. The caller's `HttpRequest` outlives `easy_perform`; the handle is reset (lease destroyed first) before `fetch` returns. Check the declaration order in `CurlTransport::fetch`.

**File descriptors and file-system races (T5)**

4. **`StateDir`, `SessionLock` and the temp file:**
   - every fd lives in a `UniqueFd` and has `O_CLOEXEC`;
   - the temp is `unlinkat`ed on every error path (scope guard);
   - `renameat` and `fsync` order;
   - a moved-from `StateDir` must not close a live fd;
   - `lock` polls with the injected Clock and checks `interrupt_requested()`;
   - `FakeClock`'s `on_sleep` hook releases the other fd (test only).
5. **The bounded read into a reserved Secret:** `cap + 1` math is `size_t`; never read past the reserved capacity.

**Terminal (T7)**

6. **The termios restorer:** restores on every path, including exceptions from `err` writes. The stack buffer is wiped. `poll` and `read` sizes are bounded at 4097. The pty test's writer thread joins before the fds close (TSan).

**Ownership in the login flow (T12, T16, T18)**

7. **`LoginSession`** (held by `unique_ptr`) borrows `Transport`, `Clock` and `TraceSink` from the `Backend` and the `StderrTrace` declared before it, and owns `AuthClient` (`RateLimiter` with a mutex: never moved), `StateDir`, `SessionLock` and `Session`.
   - `close()` runs before downloads; the dtor is idempotent.
   - `TerminalPrompter` (local in the glue) is declared before the session and outlives every `ask`.
   - `Io::prompter` (tests) outlives the run.
8. **`TokenProvider&`** is passed into `fetch_follows_collection` for the call only, and called only on the calling thread, never from `InterruptBridge`'s poller. No token reaches `run_download`, `FetchPool` or the reporter.
9. **`fetch_follows_collection`:** `stop_source` → `ApiClient` (no reporter) → `InterruptBridge`. Reverse destruction joins the bridge before return, so before item 1's own bridge.

**Untrusted input**

10. **Parsers:**
    - `form_decode` (T6) checks `%` bounds (`i + 2 < size`), caps the field count, and returns owned pairs;
    - `parse_status_map`, `parse_follows_page`, `parse_token_response`, `parse_session` and `parse_credentials_file` keep no view into the input past return; ids become `MangaId` before use;
    - the counts use `std::size_t` and are compared with `>` before `push_back`.
11. **Fixture transport (T6):** the per-token counter map (owned keys) is guarded by a mutex; token names are validated before `openat`; nothing borrowed from the request outlives `fetch`.

**CLI11 bindings**

12. **Commands:**
    - `FollowsCommand` owns `RawFollowsArgs` (`status` vector, `auth.credentials` optional);
    - `LoginCommand` and `LogoutCommand` own theirs;
    - selecting callbacks capture `[slot = &selected, self = command.get()]`;
    - `run_*_command` takes `const&` and copies into settings.

**Process-wide state**

13. **`disable_core_dumps`:** once, main thread, before any secret read. It interacts with LSan (V20) and `/proc/self/fd` (V21).
14. **`EnvSnapshot`:** the new strings are owned copies (no `getenv` views), read once in `process_io` before any thread.

---

## Rollout & risk

**CLI contract**
- `download`, `batch` and `mdlist` keep every flag, exit code, stdout byte and `--json` byte. The `batch`/`mdlist` documents never get `follows`, and `format_version` stays 1.
- **Changed text:**
  - every `--help` gets the reworded row 7 and new rows 8 and 9;
  - root SUBCOMMANDS and `unknown command (…)` list the new commands;
  - mdlist's footer line about follows (Spec issue 7).
- **New exit codes 8 and 9** (additive). New surface: `follows [DIR]` with mdlist's options plus `--status`, `--credentials`, `--password-stdin` and `--no-input`; `login`; `logout`.
- **New on-disk state** (a first for this tool): `$XDG_STATE_HOME/mangadex-dl/{session.json,session.lock}`. The README "no state directories" sentence is rewritten in T21. Migration: none; logout removes it.
- **PR body:** call out exits 8/9, the new state dir, the three env vars read, and the dump guard.

**Riskiest tasks**
1. **T5 (session store)** and **T12 (login flow).** A wrong mode, a followed symlink or a lost rotated refresh token logs the user out or leaks tokens. De-risked by:
   - a byte-level store with exhaustive mode/symlink/failure tests before any JSON exists;
   - persist-before-anything after every token response;
   - FakeTransport POST counts (never retry 4xx: brute-force lockout);
   - three reviewers on each.
2. **T4 (curl):** it touches every request of the shipped binary. Guarded by "empty authorization = identical options" (the loopback test), `release-test`, and the unmodified `e2e.*` suite.
3. **T6 (fixture auth server)** adds a form parser to the shipped binary's hidden fixture mode. Bounded, `O_NOFOLLOW`-walked, and reviewed by `security-reviewer`. The leak tripwire turns any Authorization leak in later tasks into a loud test failure.
4. **V20/V21 (dump guard vs LSan and `/proc/self/fd`):** probed before batch 1. Fallback to `RLIMIT_CORE` only.

**Behaviour choices for the gate:** Spec issues 2, 3, 5, 7, 9 and 10.

**Draft wording for the cli-ux board:**
- the follows/login/logout help footers;
- rows 7–9;
- the value-free extras message;
- the `--title` message;
- the over-long prompt answer;
- the logout warnings;
- the `-v` lines.

---

## Refactor triggers

| [VERIFY] | If refuted, these tasks change |
|---|---|
| V1 (endpoint paths, `client_secret_post`) | T11 `auth_endpoints_for`; T6 routing paths; T12 none (it takes URLs). If revocation is absent: logout skips the revoke (T12), spec back to the designer |
| V2 (Keycloak error codes/statuses) | T8 `classify_token_error` table and tests; T6 fixture error bodies; AC4 rows. No API change |
| V3 (revoke semantics) | T12 treats non-200 as the warning anyway; only test expectations change |
| V4 (API shapes, no status pagination) | T10 parsers; T13 (paginate `/manga/status` if it is paginated: new page loop); T15 fixture documents |
| V5 (10 000 window) | T13 cap message and constant; T21 wording |
| V6 | T21 wording only |
| V7 (ExtrasError, `remaining(true)`) | T19: add hidden decoy options on the three subcommands that fail with the fixed text (the list-plan V7c pattern) instead of the `cli.cpp` rewrite |
| V8 (`--status` binding) | T18: bind as a single-value repeatable option with a custom `each()` collector; T19 parse tests follow |
| V9 (4th sibling, help order) | T18 duplicates the `add_option` calls with shared `constexpr` help strings; T19 help asserts follow |
| V10 (optional binding) | T16 binds `--credentials` to a `std::string` plus a "was given" flag (`->count()`) |
| V11 (values echoed by other ParseErrors) | T19 maps those errors to fixed text (gate wording) |
| V12 (HTTPHEADER on GET) | T4 uses `CURLOPT_XOAUTH2_BEARER` + `CURLAUTH_BEARER` instead (then V-check that version floor) |
| V13 (POSTFIELDS semantics) | T4 keeps `COPYPOSTFIELDS` (the spec's accepted best-effort copy); the watch-list notes the libcurl copy |
| V14 (slist layout) | T4 drops the slist wipe (best effort; noted) |
| V15 (sysroot symbols, glibc floor) | T2 uses `memset_explicit`/a volatile loop if `explicit_bzero` is missing or too new; T5 the `O_TMPFILE`/`linkat` path if `renameat` is unusable (unlikely) |
| V16 (flock per description) | T5 tests use a forked child to hold the lock; AC26 cli uses the same |
| V17 (dir fsync) | T5 opens a second `O_RDONLY` dir fd for the fsync, or treats the fsync as best effort |
| V18 (pty behaviour) | T7 tests drive ECHO checks through a seam (a `TerminalOps` interface) instead of the master; AC39/40 stay cli-tester + human |
| V19 (string capacity) | T2 implements `Secret` over `std::unique_ptr<char[]>` + size/capacity (own allocator logic) |
| V20, V21 (prctl vs LSan / `/proc/self/fd`) | T2: `disable_core_dumps` does `RLIMIT_CORE` only (spec's fallback); `DumpGuard::no_core_only` everywhere; T21 security wording |
| V22 (RLIMIT_CORE under sanitizers) | T2 skips `setrlimit` when built with sanitizers (`__SANITIZE_ADDRESS__`/`__has_feature`), documented |
| V23 (duplicate keys) | T10/T9 docs only (behaviour documented as "last/first wins") |
| V24 (100 000 entries within limits) | T10 tunes `max_status_separators`, or lowers the entry cap (spec back to the designer) |
| V25 (DOM string wipe) | T8/T9 drop the DOM wipe (best effort), noted |
| V26 (ordered_json order) | T14 builds the `follows` object explicitly in order; AC11 asserts semantics |
| V27 (switch inventory) | T1 widens its Owns to every missed switch (or the owner joins batch 1) |
| V28 (fixture mapping, POST branch) | T6 adjusts routing paths; T15 fixture layout; T13/T15 tests |
| V29 (fixture_gen euid/modes, PRE_RUN, STDIN placeholders) | T20 positive cases move to the cli tier only (the spec's fallback); or a `CliE2E.cmake` driver option as a separate serialized task |
| V30, V31, V32 (remedy, SA_RESTART, instant clock) | T16/T18 remedy routing; T7 poll design (already assumed); AC26 cli case uses a test-only short `LockTiming` passed through `run_login_phase` |
| V33 (Endpoints pinned) | None (the plan already avoids it) |
| V34 (tsan `-R`) | The listed tasks run the full `nice pixi run tsan` (longer) |

---

## Relevant paths

- **Spec:** `/home/user/Mangadex-dl/docs/design/follows-spec.md`. Precedent: `/home/user/Mangadex-dl/docs/design/list-plan.md`, `/home/user/Mangadex-dl/docs/design/list-spec.md`.
- **Frozen headers (T1):**
  - `/home/user/Mangadex-dl/include/mangadex_dl/{error,exit_codes,api_client,batch}.hpp`;
  - `/home/user/Mangadex-dl/include/mangadex_dl/net/{transport,fixture_transport}.hpp`;
  - new `/home/user/Mangadex-dl/include/mangadex_dl/{secret,prompt,session_store,auth,follows}.hpp`;
  - `/home/user/Mangadex-dl/src/app/io.hpp`.
- **Code read for this plan:**
  - app: `/home/user/Mangadex-dl/src/app/cli.cpp`, `/home/user/Mangadex-dl/src/app/cmd_mdlist.cpp`, `/home/user/Mangadex-dl/src/app/cmd_batch_jobs.{hpp,cpp}`, `/home/user/Mangadex-dl/src/app/cmd_download_run.{hpp,cpp}`, `/home/user/Mangadex-dl/src/app/cmd_download_options.cpp`, `/home/user/Mangadex-dl/src/app/io.{hpp,cpp}`, `/home/user/Mangadex-dl/src/app/exit_help.hpp`;
  - core: `/home/user/Mangadex-dl/src/core/api_client.cpp`, `/home/user/Mangadex-dl/src/core/net/{curl_transport,fixture_transport,clock}.cpp`, `/home/user/Mangadex-dl/src/core/json_util.hpp`, `/home/user/Mangadex-dl/src/core/interrupt.cpp`, `/home/user/Mangadex-dl/include/mangadex_dl/{interrupt,options,mdlist,mangadex_api,net/retry,fs/bounded_read}.hpp`;
  - CMake: `/home/user/Mangadex-dl/CMakeLists.txt` (:90, :108).
- **Tests and support read:**
  - `/home/user/Mangadex-dl/tests/support/{fake_transport,fake_clock,io_capture,mdx_fixture,mdx_twin,loopback_http_server}.hpp`;
  - `/home/user/Mangadex-dl/tests/e2e/{CliE2E.cmake,run_case.cmake,fixtures/fixture_gen.cpp,cases/mdlist.cmake,cases/_edge.cmake,inputs/README}`;
  - `/home/user/Mangadex-dl/tests/CMakeLists.txt`;
  - `/home/user/Mangadex-dl/tests/unit/api_client/api_client_test.cpp` (:304-321).
- **Tests pinning text that T19 changes:**
  - `/home/user/Mangadex-dl/tests/cli/cli_test.cpp` (:66, :85);
  - `/home/user/Mangadex-dl/tests/cli/download_cli_test.cpp` (:446);
  - `/home/user/Mangadex-dl/tests/cli/batch_parse_test.cpp` (:228);
  - `/home/user/Mangadex-dl/tests/cli/mdlist_parse_test.cpp` (:223, :229).
- **Goldens:**
  - change: `/home/user/Mangadex-dl/tests/e2e/golden/{edge,download,batch,mdlist}.help.stdout`;
  - new: `follows.{help,dry-run,json-dry-run}.stdout`, `login.help.stdout`, `logout.help.stdout`.
