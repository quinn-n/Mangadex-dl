# Revision 1 (post fact-check) — amends `download-plan.md`

Grounded in `download-factcheck.md`. **Where this conflicts with `download-plan.md`, this file wins.** Tasks not listed (T3, T13, T17) and the batch structure are unchanged.

New assumptions (use-time checks, not blocking):
- **[VERIFY-R1a]** `fstatat(AT_FDCWD, "/proc/self/fd/<n>", …)` fails ENOENT when /proc is not mounted (not probeable unprivileged; fallback is defensive).
- **[VERIFY-R1b]** whether `openat(…, O_CREAT…, mode)` fires `cppcoreguidelines-pro-type-vararg` — settle at lint; NOLINT only where lint fires.

## Frozen T1 interface changes (batch 1)
- `zip.hpp`: `inline constexpr std::size_t zip_max_entries = 65534;` `inline constexpr std::uint64_t zip_max_u32 = 0xFFFFFFFE;` (0xFFFF/0xFFFFFFFF are ZIP64 sentinels).
- `planner.hpp`: `max_zip_entries_per_epub = 65534`.
- `net/retry.hpp`:
  `struct RetryConfig { int max_attempts = 4; int max_attempts_429 = 2; std::chrono::milliseconds base_delay{1000}; std::chrono::milliseconds max_jitter{500}; std::chrono::seconds retry_after_cap{60}; std::chrono::seconds default_429_delay{60}; };`
  `[[nodiscard]] std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view x_ratelimit_retry_after_epoch, std::string_view retry_after_fallback, std::chrono::system_clock::time_point now);` — delay clamped ≥ 0; nullopt if both absent/garbage.
- `model.hpp`: `struct Cover { std::optional<std::string> volume{}; std::string file_name{}; std::optional<std::string> locale{}; };`
- No other signature changes.

## T0
- Configure log in **both** envs prints `CMAKE_DL_LIBS` (must be `dl`; re-check under gcc).
- Wording: dev/test binaries carry **DT_RPATH** (conda toolchain), searched for libcurl and its deps (dev loads conda OpenSSL; harmless). Release: no RPATH/RUNPATH; NEEDED ⊆ {libpthread, libm, libc, ld-linux, libdl}.
- Note for later (not this task): `scripts/cxx-probe.sh` adds no `-I` for the env include dir.

## T1
- Apply the changes above; `retry.hpp` doc comment states epoch-primary semantics + 429 policy.
- `headers_test.cpp`: `static_assert(zip_max_entries == 65534 && max_zip_entries_per_epub == 65534);`

## T2
- `EXPECT_EPUB` is byte-level in the driver: `file(SIZE) ≥ 60`; HEX checks at offset 0 `504b0304`, 8 `0000` (method), 26 `0800` (name len), 28 `0000` (extra len), 30 `6d696d6574797065`, 38 hex of `application/epub+zip`; last 22 bytes start `504b0506`. Then a **child** `execute_process(COMMAND ${CMAKE_COMMAND} -DARCHIVE=<f> -DDEST=<WORK>/epub-check/<n> -P tests/e2e/epub_extract_check.cmake)` that extracts, globs `LIST_DIRECTORIES false`, requires non-empty list containing `META-INF/container.xml` and `OEBPS/content.opf`. Driver asserts rc numerically (`_rc MATCHES "^[0-9]+$" AND _rc EQUAL 0`). New owned file: `tests/e2e/epub_extract_check.cmake`.
- `EXPECT_FILES` / `EXPECT_ABSENT` take **literal** paths (`EXISTS`/`IS_DIRECTORY`), not globs (oneshot names contain `[`).
- New `EXPECT_NO_GLOB <glob>...` (`file(GLOB … LIST_DIRECTORIES false)`, fail on any match) for `out/.*.part-*`.
- `file(MAKE_DIRECTORY "${CASE_WORK_DIR}")` before every `WORKING_DIRECTORY` use.
- Fixtures: titles contain no `[`, `]`, `;`; `basic_fixture()` adds a cover with `volume: null` and one with `locale: null`, and sets `"isUnavailable": true` on its unavailable chapter; every at-home `baseUrl` is `https://fixture.invalid`.

## T5
- Reject the 65535th entry and any size/offset/CD size/CD offset `> zip_max_u32`. Tests: 65534 accepted, 65535 rejected; fake-sink offset 0xFFFFFFFE accepted, 0xFFFFFFFF rejected.

## T6
- Depth pre-scan (64) **mandatory** before every parse; never `dump()`/copy/`==` unvalidated json; extract into model structs immediately; wrap extraction in `try { … } catch (const nlohmann::json::exception&)` → Errc::network.
- Feed URL params, fixed order: `translatedLanguage[]=<lang>&limit=500&offset=<n>&order[volume]=asc&order[chapter]=asc&includes[]=scanlation_group&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica&contentRating[]=pornographic&includeFuturePublishAt=0&includeUnavailable=1`.
  - 2026-10-18 correction: `includeExternalUrl=1&includeEmptyPages=1` were removed. On the live API they are filters (`=1` returns only external / only empty-page chapters, so a normal title came back with `total: 0` and exit 5; `=0` excludes them; omitted = no filter). Never re-add them; the planner already reports external and empty chapters as `external` / `no_pages` skips.
- Cover URL: `manga[]=<id>&limit=100&offset=<n>`; `volume`/`locale` nullable.
- Tolerant parsing: unknown fields ignored; only `id`, `pages`, title map, `hash`, `data`, `baseUrl` required.
- Tests added: 1e6-deep input rejected by pre-scan (no ASan overflow); depth 64 ok / 65 rejected; wrong-type field → network; null cover volume/locale; exact URL strings; unavailable chapter parses `unavailable = true`.

## T7
- `parse_retry_after`: primary `X-RateLimit-Retry-After` = UNIX epoch seconds → `max(0, epoch − now)`; fallback `Retry-After` delta-seconds only when the epoch header is absent/garbage.
- `decide_retry` 429: ≤ `max_attempts_429` (2) attempts; delay = parsed or `default_429_delay` (60 s); if delay > `retry_after_cap` → no retry (caller exits 3 with the time). 403 and 400 never retried. 5xx/timeout/protocol schedule unchanged.
- Fixture walk: symlink component may yield `ENOTDIR` **or** `ELOOP` → both 404.
- Tests: epoch future/past(0)/garbage + fallback; 429 no header → one 60 s sleep then give up; 429 epoch +120 s → no retry; 403/400 no retry; symlink component → 404.

## T8
- `create_temp`: O_TMPFILE falls back only on `EOPNOTSUPP`/`EISDIR`/`EINVAL`; other errnos → Errc::failure. After O_TMPFILE, `fstatat(AT_FDCWD, "/proc/self/fd/<n>", &st, 0)` must succeed; on ENOENT close and use fallback [VERIFY-R1a]. Publish-time linkat ENOENT → Errc::failure "/proc unavailable".
- Fallback publish: call `renameat2(dirfd, tmp, dirfd, final, RENAME_NOREPLACE)` directly. EEXIST → `exists` (unlink temp); EINVAL/ENOSYS → `linkat(dirfd,tmp,dirfd,final,0)` + `unlinkat(tmp)` (linkat EEXIST → `exists`).
- Signal handler: function in an **anonymous namespace**, installed with `sigaction`; flag declared `// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): async-signal flag` / `std::atomic<int> g_interrupt_signal{0};`. Handler body (single relaxed store) is a manual memory-safety review item. `<signal.h>` symbols spelled per `misc-include-cleaner` (existing `io.cpp` pattern).
- NOLINT `cppcoreguidelines-pro-type-vararg` only on the O_TMPFILE `openat` (and the O_CREAT one only if lint fires [VERIFY-R1b]).
- `misc-const-correctness`: const read-only out-param buffers where lint asks.
- Tests added (via seam): renameat2 EINVAL → linkat path; EEXIST at both stages; /proc ENOENT fallback.

## T9
- Fail-mark epubs over 65534 entries.
- `publishAt` UTC conversion via `sys_days` arithmetic.
- Covers: null `volume` never matches a volume; null `locale` ranks after any non-null.
- Tests: 65534 ok / 65535 fail; null cover volume/locale.

## T10
- Nav: nested `<ol>` only inside an `<li>` beginning with `<a>`/`<span>`, never empty; chapterless volume → leaf `<li><a>`.
- OPF: `mimetype` never in manifest; `dcterms:modified` `<meta property>` without fractional seconds, literal `Z`; no package-level `rendition:viewport` (per-page viewport meta stays).
- `format_modified` via `sys_days`.
- Tests: nesting both cases; no `mimetype` in manifest; no `rendition:viewport`; modified regex `^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$`.

## T11
- Runtime minimum **7.85.0** (`version_num >= 0x075500`) — *subject to gate decision 10*; `CURLOPT_PROTOCOLS_STR "https,http"` mandatory (failure → Errc::failure); constant `curl_min_version_num` in private `curl_api.hpp`; pure `check_runtime(const curl_version_info_data&)` there.
- Pooled handles: after `curl_easy_reset` re-apply the **full** option set every request (callbacks, all `*DATA`, NOSIGNAL, timeouts, USERAGENT, CAINFO, protocols, verify, POST options).
- No typecheck rationale; wrappers take `long` directly.
- NOLINT only: `pro-type-reinterpret-cast` on the dlsym cast, `concurrency-mt-unsafe` on `dlerror`. **No** vararg NOLINT on setopt/getinfo.
- Keep `SigpipeGuard`.
- CA selection: NULL `cainfo`/`capath` and unreadable `cainfo` are "not usable" → probe list.
- Tests: CA selection with NULL/unreadable defaults; 4×20 loopback HTTP fetches under TSan; version gate on a fake struct. Concurrent TLS under TSan → User Test Plan.

## T12
- Manga GET: 404 → not_found (4); 400/403 → network (3) with `<url>: HTTP <code>` + hint (403: "access denied — possibly a temporary rate-limit ban; wait before retrying"). Any API 403 not retried.
- 429 via `decide_retry` (≤ 2 attempts); give up → exit 3 "rate limited until <UTC time>".
- `fetch_page`: 403 from an image host returns immediately (T14 refreshes). MD@H report on **every** image fetch from a non-mangadex.org `baseUrl`, success and failure (failure: `success:false, bytes:0, cached:false`, duration so far; transport errors too); best-effort, never retried.
- Tests: 403/400 manga → network + message; 429 no header → one 60 s fake sleep then network; 404 page and transport error each → failure report with `bytes:0`; mangadex.org host → no report; feed URL carries every explicit param.

## T14
- Any `fetch_page` failure triggers an at-home refresh for that chapter (≤ 3 per chapter), including image-host 403.
- Tests: 403 on first baseUrl → refresh → success; 3 refresh failures → epub `failed`.

## T15
- `--group`:
  ```cpp
  // expected(1) alone rejects a repeated --group; TakeAll alone swallows the next positional.
  sub.add_option("--group", args_.groups, "...")
      ->expected(1)->allow_extra_args(false)
      ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
  ```
- User-Agent exactly `mangadex-dl/<version>`; never a `Via` header. Append ` (+<repo url>)` once a remote exists (follow-up).

## T16 (README)
- Proxy vars: `http_proxy` (lowercase only; `HTTP_PROXY` deliberately ignored), `https_proxy`/`HTTPS_PROXY`, `ALL_PROXY`, `NO_PROXY`. No CA env var honoured.
- Runtime requirement: TLS-capable `libcurl.so.4` or `libcurl-gnutls.so.4`, version ≥ the gate-decision-10 minimum.
- DNS lookups have no separate timeout (NOSIGNAL + sync resolver); the 30 s stall timeout bounds transfers.

## T18a
- `--group` shapes that must parse with positionals intact and groups in order: `download --group X --group Y M`, `download --group X M DIR`, `download M --group X --group Y`, `download M DIR --group X`; `--group` with no value → 2.

## T18b
- `network-unreachable`: `LABELS needs-libcurl`; fails red (never skips) where libcurl exists; exclude on libcurl-less hosts via `-E '^e2e\.download\.network-unreachable$'`. No CI change.
- Known names via literal `EXPECT_FILES`/`EXPECT_ABSENT`; temps via `EXPECT_NO_GLOB "out/.*.part-*"`; no glob contains a `[` name.

## Gate decisions added
- **10.** libcurl runtime minimum (see gate).
- **11.** 429 policy: one retry, 60 s default wait, give up beyond 60 s (MangaDex escalates 429 to a temporary 403 ban).

## Watch-list deltas (memory-safety-reviewer)
- Every `*DATA` pointer re-set per request to that request's stack-local `TransferState` after `curl_easy_reset`.
- No path may `dump()`/copy/compare network JSON before the depth pre-scan.
- Signal handler body is a manual review item.

## Plan gate (user, 2026-10-17): APPROVED
- Planner decisions 1–9 and 11 accepted as written.
- Decision 10: libcurl runtime minimum **7.85.0** (`0x075500`), `CURLOPT_PROTOCOLS_STR` mandatory.

## Requirements carried forward from batch-1 reviews
**T11 (security-reviewer on T0, 2026-10-17):**
- `CURLOPT_PROTOCOLS_STR` failure is fatal (min 7.85.0 makes it always available); `FOLLOWLOCATION=0`; if redirects are ever enabled, `REDIR_PROTOCOLS_STR=https`. Production endpoints https-only; `http` only via hidden `--api-base` loopback.
- `dlopen(..., RTLD_NOW | RTLD_LOCAL)`; null-check every `dlsym` and the `curl_version_info` result; honour `age` before reading `cainfo`/`capath`. Bare-soname dlopen obeys `LD_LIBRARY_PATH` (acceptable, not setuid) → README threat note. Release binary: no RPATH/RUNPATH/`$ORIGIN` (verify with `readelf -d`).
- CA probe: open + `fstat`, regular readable files only; never disable verification on any cert error (→ exit 3 hint). No `SSL_CERT_FILE`/`CURL_CA_BUNDLE`; README documents proxy vars and that host libcurl/OpenSSL CVEs are outside project audit coverage.
- No `CURLOPT_VERBOSE`/`DEBUGFUNCTION`; redact userinfo if any URL is echoed.
- Write-callback cap is the real bound (also post-decompression with `ACCEPT_ENCODING ""`) — test chunked + compressed overrun. Cap header/`Retry-After` copy sizes; parse numbers with `from_chars` + range checks.

**Test wiring (T0 coordination):** unit tests including private `src/core/net/curl_api.hpp` need `target_link_libraries(<unit test target> PRIVATE mangadex_dl_curl_headers)` in `tests/CMakeLists.txt` (serialization point; apply at batch-3 integration, before T11's tests build — or grant T11 that one line).

**From memory-safety review of T1 (2026-10-17):**
- **T7:** `decide_retry` (noexcept) — clamp `attempt` and overflow-check `base_delay * 2^(attempt-1)`; range-check `max_jitter` before narrowing to the `uint32_t` jitter argument (`std::in_range`).
- **T8:** handler flag `constinit std::atomic<int>{0}` + `static_assert(is_always_lock_free)`; if `bugprone-signal-handler` fires at lint, scoped `NOLINTNEXTLINE(bugprone-signal-handler): reason` rather than reshaping; `ScopedInterruptHandlers::install` refuses a second live install (atomic flag); `InterruptBridge` poller waits on its own stop_token via `condition_variable_any` (not `sleep_for`) so the dtor joins promptly; `stop_callback`s run on the poller thread. `TempFile::State` owns an `fcntl(F_DUPFD_CLOEXEC)` dup of the dirfd as a `UniqueFd` (no unlink through a reused fd). FetchPool: catch all exceptions on workers → Error.
- **T11:** `HttpRequest::max_body_bytes == 0` means "refuse any body", never unlimited.
- **T14:** `run_download` null-checks `DownloadServices` pointers → `Errc::failure`. On GCC the LIFETIMEBOUND shim is empty, so T14/T15 declaration order (sink before writers, transport/clock before client, OutputDir before TempFile) needs explicit review.

**T18b must also:** land the permanent E2E driver self-test cases exercising `EXPECT_FILES`, `EXPECT_ABSENT`, `EXPECT_NO_GLOB`, `EXPECT_EPUB` and `{{FIXTURES}}` (deferred from T2 review).
**T4 note:** `Selection::operator==` comparing runtime-unknown kinds still compares the range vectors; GCC release may raise a `-Wnull-dereference` false positive in T4 tests — restructure, never suppress.

**From T7 hand-off (2026-10-17):**
- **T12:** narrow `max_jitter` to the `uint32_t` jitter callback only after `std::in_range<std::uint32_t>(...)`; sleep only when `decide_retry` says retry; `parse_retry_after` values are clamped to 366 days, so `wall_now + delay` cannot overflow (for the "rate limited until" message). Fixture transport reads `file://<root>/at-home/...` and `uploads/...` verbatim.
- Fixture symlink components are a plain 404 (frozen `make_fixture_transport` has no trace sink; plan's "plus a trace" dropped).

**Batch-2 integration tasks (orchestrator, serialization points):**
- `tests/unit/CMakeLists.txt`: add `target_include_directories(mangadex_dl_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/core")` (T8 seam, later T11 private `curl_api.hpp`) and `target_link_libraries(mangadex_dl_unit_tests PRIVATE mangadex_dl_curl_headers)` (T11). Then T8's relative `../../../src/core/...` include → `"fs/output_dir_testing.hpp"`.
- `tests/.clang-tidy`: `-clang-analyzer-optin.core.EnumCastOutOfRange` (fires inside Catch2's `ResultDisposition::Flags operator|` via CHECK_FALSE/REQUIRE_FALSE) — only if still firing at integration.
- `include/mangadex_dl/model.hpp`: correct the `MangaInfo::titles` comment — titles (and keys inside each altTitles object) are in **language-code order** (nlohmann `std::map`), not API order; `ordered_json` rejected (O(n²) duplicate-key check on 16 MiB). **T9:** the "first title value" fallback is the lexicographically first language.
- **T12 (from T6):** feed/cover `total`/`limit`/`offset` read as 0 when absent (pagination must stop, not loop); `data_saver` may be empty.
- **T12 (from T6 memory review):** `FeedPage`/`CoverPage` offset/limit/total may be any `size_t` from the API — pagination math (`offset + limit`, comparisons with `total` and `feed_offset_cap`) must use checked addition.
- **Security board item:** a 16 MiB API document parses to ~0.3–0.5 GB of nlohmann nodes before field checks; bounded, but consider an element-count pre-scan beside `depth_ok` if several documents are ever parsed concurrently.
- `include/mangadex_dl/zip.hpp`: document the `ZipWriter::add_stored` precondition — `name`/`data` must not borrow from the sink's own storage (a header write may reallocate a MemorySink).
- Hoist T4's per-file `ExactBuffer` into `tests/support/exact_buffer.hpp` (single NOLINT) — follow-up for test-engineer at /harden or a later test track.

**From T13 hand-off (2026-10-18):**
- **T15:** call `ConsoleObserver::finish()` right after `run_download` returns, on every path (report, Error, interrupted), before `print_error`/`print_report_json` (prints the exists notice, clears progress; idempotent).
- **T18b:** the exists notice reads `1 file already exists; pass --overwrite to replace it` / `N files already exist; pass --overwrite to replace them` — match on `already exist`. Failed file: `mangadex-dl: error: <path>: <error>`. All stderr text passes `terminal_safe` (controls, C1, bidi, invalid UTF-8 → `?`); stdout paths are raw bytes.

**From T9 hand-off (2026-10-18):**
- **T10 ↔ T9 coupling:** `planner.cpp` `fixed_epub_entries` assumes exactly mimetype, container.xml, content.opf, nav.xhtml, toc.ncx + 2 per page + 2 for a cover. Any extra EPUB entry must update it (T10 review must confirm).
- Entry count is always odd → boundary is 65533 ok / 65535 fail (rev1's 65534 case is impossible).
- Bidi gotcha: GCC `-Wbidi-chars=any` rejects bidi controls even as `\u` escapes; clang-tidy `misc-misleading-bidirectional` fires on byte escapes — use numeric code points / runtime-built strings.
- `terminal_safe` (T13) and the filename sanitizer (T9) should share one dropped-code-point set (U+061C, U+200B–U+200F, U+202A–E, U+2028/9, U+2060–U+206F, U+FEFF) — align in T13/T9 round 2.

**From T11 hand-off (2026-10-18):**
- Header doc (orchestrator, at batch-3 integration): `net/transport.hpp` `HttpRequest::body` — curl copies POST data (`CURLOPT_COPYPOSTFIELDS`), so the "must outlive perform" caller rule is stricter than needed; relax the comment.
- **T15/T18b:** re-prove release NEEDED ⊆ glibc libs (+ libdl.so.2) and no RPATH once the app links the curl transport.
- **T16 README:** libcurl's low-speed check averages over ~5 s, so after a burst a stall can abort up to ~5 s past `stall_timeout`.
- build-fixer: release-preset `mangadex_dl_fixture_gen` links host libstdc++ without RPATH → `e2e.fixtures` fails in `release-test` (GLIBCXX_3.4.36 not found). Being fixed.

**Orchestrator decision (2026-10-18, T12):** page-image fetches do **not** go through the shared API limiter (spec: image hosts are exempt from the ~5 req/s API limit; the plan's "every request" wording would cap downloads at ~4 images/s regardless of `--jobs`). Exception: when the at-home `baseUrl` host is `mangadex.org` or a subdomain (`is_mangadex_org_host`), page fetches DO use the API limiter. Covers (`uploads.mangadex.org`) keep the API limiter. Concurrency for images is bounded by `--jobs`. Report POSTs skip limiters (as implemented).
- Header docs at batch-3 integration (orchestrator): `net/curl_transport.hpp` `select_ca_bundle` comment → unreadable/NULL cainfo falls through to the probe list even if capath is readable; capath alone keeps libcurl's default only when there is no cainfo. `net/transport.hpp` `HttpRequest::body` → copied by the curl transport (COPYPOSTFIELDS).

**From T10 hand-off (2026-10-18) — EpubWriter contract for T14:** `begin()` requires `meta.modified` already in `CCYY-MM-DDThh:mm:ssZ` (use `format_modified`) and non-blank identifier/title/language; add the cover before the first page; nav `first_page` must be < final page count; nav entries sharing a `group` must be added consecutively. Entry layout = planner's `fixed_epub_entries` (5) + 2/page + 2 cover (no stylesheet entry; CSS inline).
- `include/mangadex_dl/filename.hpp` doc (orchestrator, batch-3 integration): dropped set also includes invisible format characters U+200B–U+200D, U+2060–U+2065, U+206A–U+206F, U+FEFF.
- `include/mangadex_dl/epub.hpp` doc (orchestrator, batch-3 integration): `add_cover`/`add_page` image span must not borrow from the ZipWriter's sink (passes the ZipWriter::add_stored precondition through).
- **T16 README (from T11 r2):** every request also has a 10-minute overall deadline (`CURLOPT_TIMEOUT_MS`) on top of the 30 s stall timeout; the libcurl-missing error lists every soname tried.

**From T15 hand-off (2026-10-18):**
- **Orchestrator decision — exit 5 message:** the spec requires exit 5 to print a stderr message naming the language and selection, and the number of external/unavailable chapters skipped (stdout stays empty; `--json` still prints the `epubs: []` document on stdout). T15 owns it: after `run_download` returns a report with no epubs, print `mangadex-dl: error: no downloadable chapters matched --lang <code> with --volumes <spec> --loose-chapters <spec>` plus `(N chapters skipped: external, no pages or unavailable)` when N > 0, through `print_error` (terminal-safe), in both human and `--json` modes, after `observer.finish()`.
- Deviations accepted: `--title` via `add_option_function` (CLI11 assigns nullopt for an empty value); `DownloadServices` filled member-by-member (clang-analyzer NewDeleteLeaks false positive with designated init).
- **T18a must test** the list in T15's hand-off (help surface; every usage error incl. `--title ""`/`"  "`, `-j 0`, `--direction bogus`, `--title A --title B`, each also with `--json`; the 4 `--group` shapes + order via `-v` dedupe output; fixture success/re-run/overwrite/jobs byte-identity/json shapes incl. exit 5/6/4; `--color=always --json` no ESC; simulated interrupt → rc 1, nothing written, handlers restorable; finish() before error line with a TTY progress line; fixture mode fast, no libcurl). Plus the exit-5 message above.
- Hint wording (`Run 'mangadex-dl download --help' …` vs CLI11's `Run with --help …`) → cli-ux board.
- **T18a must also exercise (memory-safety, T15 review):** a fixture success through `run()` (backend → services → observer → report chain, jitter copy, bridge thread); `--json` success + exits 4/5/6; a semantic usage error and `--title ""`; a simulated interrupt followed by a SECOND in-process run (handlers restored, one-live flag released); the 4 `--group` shapes; a backend-construction failure (bad fixture root). Run under dev ASan+UBSan AND `nice pixi run tsan -R cli.download`. Also an exit-5 case asserting the stderr message and exact `--json` stdout (`epubs: []`).
- Board: run `nice pixi run -e gcc check` (GCC release has not compiled cmd_download.cpp yet).
- **T18b (from T14):** warning texts — `skipped Vol. 2 Ch. 3: hosted outside MangaDex`, `<file>: contains WebP images…`, `<file>: could not download the cover image (…)…`, `could not list the cover images (…)…`. `on_epub_begin` fires only for files actually downloaded.
- `include/mangadex_dl/download.hpp` doc (orchestrator, batch-4 integration): `on_epub_end` also fires for files reported `exists`/`failed` by plan_error/dry-run without a preceding `on_epub_begin` (begin fires only for files actually downloaded).
- **T18a (from T15 r2):** exit-5 suffix `(N chapters skipped: …)` (singular `1 chapter skipped`) is unreachable with existing fixtures — T18a builds its own mdx_fixture tree: all-external/no-pages/unavailable feed (N > 0), N == 1, and a duplicate-only skip (not counted). Assert the stderr line in human and `--json` modes plus exact `--json` stdout.
