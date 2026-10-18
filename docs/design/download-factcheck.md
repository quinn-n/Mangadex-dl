# Fact-check report: `download` plan

Merged verdicts per cluster (plan: `download-plan.md`). Each section lists only deviations and plan amendments.

## Cluster C — Linux/glibc syscalls & signals: ALL CONFIRMED
Kernel 6.12, unprivileged, sysroot glibc 2.28; tmpfs + ext4.
- V-C1: O_TMPFILE works. Fallback only on EOPNOTSUPP/EISDIR/EINVAL; other errnos are real failures. (Unsupported-FS path not exercisable here.)
- V-C2: `/proc/self/fd` linkat works unprivileged, EEXIST on existing name. **Amend T8:** a linkat ENOENT (no /proc) is another fallback trigger.
- V-C3: `renameat2@@GLIBC_2.28`, declared in `<stdio.h>`. **Amend T8:** call `renameat2` directly (no `syscall`); EEXIST → exists; EINVAL/ENOSYS → linkat+unlinkat.
- V-C4: `getrandom@@GLIBC_2.25`. OK.
- V-C5: atomic flag handler, SIG_DFL+raise (130/143), sigtimedwait drain all confirmed.
- V-C6: confirmed. **Amend T7/T8:** `openat(O_DIRECTORY|O_NOFOLLOW)` on a symlink gives ENOTDIR (not ELOOP) here — accept either.

## Cluster F — MangaDex API: PARTIAL (refutations → plan revision)
Sources: api.mangadex.org/docs (limitations, retrieving-chapter), OpenAPI api.yaml. No live calls.
- V-F1 PARTIAL: manga GET documents 200/403/404 (no 400). **Amend T12:** 404 → not_found; 400/403 → clear error (network/usage), not 70. Attribute shapes unchecked → keep parsers tolerant.
- V-F2 PARTIAL: feed `limit` max 500 confirmed; `contentRating[]` default excludes `pornographic` → send all four explicitly (as designed). `includeExternalUrl`/`includeEmptyPages`/`includeFuturePublishAt` are int 0|1 with undocumented defaults → always send explicitly. **Superseded 2026-10-18** for `includeExternalUrl`/`includeEmptyPages`: live-API observation shows they are filters (`=1` → only such chapters, `=0` → none, omitted → all), so the feed URL omits both. Window cap `offset+limit ≤ 10000` confirmed in prose; error body undocumented → client-side guard (exit 3, as designed).
- V-F3 PARTIAL: attributes confirmed (`isUnavailable` bool, `readableAt`, `chapter` ≤8 chars). **REFUTED detail:** feed has `includeUnavailable` = **string "0"|"1", default "0"**. **Amend T6:** send `includeUnavailable=1` so unavailable chapters are reported as skipped (designer intent), still skipped by T9.
- V-F4 PARTIAL: `/cover` limit default 10, max 100 → send `limit=100` explicitly; `volume`/`locale` nullable. Uploads URL form unverified (human).
- V-F5 CONFIRMED: at-home shape and URL; baseUrl valid ≥15 min, then 403; optional `forcePort443`. **Amend T14:** 403 from an image host triggers an at-home refresh.
- V-F6 PARTIAL/**REFUTED**: ~5 req/s global and at-home 40/min confirmed. `X-RateLimit-Retry-After` is a **UNIX epoch** (documented primary); `Retry-After` is **not documented** → fallback only. Escalation 429 → temporary 403 ban → silent block. **Amend T7/T12:** epoch-primary parsing; on 429 back off and do not retry aggressively (keep cap 60 s; at most 2 attempts on 429).
- V-F7 CONFIRMED, **amended**: report **every** image fetch (success *and* failure, `bytes:0` on failure) from non-`mangadex.org` baseUrls. **Amend T12.**
- V-F8 PARTIAL: UA required, must not be spoofed, no `Via`. **Amend T15:** UA `mangadex-dl/<version>`; append `(+<repo url>)` once a remote exists.

## Cluster B — libcurl C API (installed 8.23.0, OpenSSL 3.6.5): CONFIRMED (2 PARTIAL wording)
- V-B1..B4, B7 CONFIRMED. Runtime minimum **7.85.0** (`PROTOCOLS_STR`) covers every option used; check at runtime, not baked from memory. Decompressed-size cap (8.20) and `CURL_WRITEFUNC_ERROR` (7.87) are optional extras only.
- V-B5 PARTIAL: curl docs say OpenSSL may still raise SIGPIPE → keep the SIGPIPE block around `perform`. `NOSIGNAL=1` disables DNS timeouts with the synchronous resolver (informational; overall stall timeout still applies).
- V-B6 PARTIAL: `curl_easy_reset` resets *all* options (DATA pointers included), keeps connections. If pooling, re-set every callback/DATA option per request.
- V-B8 CONFIRMED, premise wrong: `typecheck-gcc.h` is never active in C++ (`curl.h` guards on `!__cplusplus`). **Amend T11:** drop the `CURL_DISABLE_TYPECHECK` rationale; type safety comes from the wrappers.
- Side finding: `cxx-probe` has no curl include path wired (probe used an absolute include). T0's `find_path` target supplies it for real TUs.

## Cluster A — CLI11 2.7.2: 5 CONFIRMED, V-A2 REFUTED
- V-A1, A3, A4, A5, A6 CONFIRMED (`group("")` hides; fallthrough + subcommand footer; Range/IsMember → `ValidationError` ⊂ `ParseError`; positional ordering, `-nvq`; invalid UTF-8 passthrough).
- V-A2 **REFUTED**: `expected(1)` alone rejects repeated `--group`; `+TakeAll` swallows the positional. **Amend T15:** `--group` = `->expected(1)->allow_extra_args(false)->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)` (all five argv shapes verified), with a comment. **Amend T18a:** cases `--group X --group Y M`, `--group X M DIR`, `M --group X --group Y`, `M DIR --group X`.
- Note: `cxx-probe` adds no `-I` for the env include dir (probes used absolute includes).

## Cluster J — warnings & clang-tidy: 3 CONFIRMED, 2 PARTIAL (NOLINT placement changes)
- V-J1 CONFIRMED: `reinterpret_cast<Fn*>(dlsym(...))` clean on both compilers with full flags + `-Wcast-function-type`.
- V-J2 PARTIAL: `pro-type-reinterpret-cast` fires on dlsym cast; `concurrency-mt-unsafe` fires on `dlerror`. **`pro-type-vararg` does NOT fire on `curl_easy_setopt/getinfo`** (system header, no typecheck in C++) but **does** fire on `open(..., O_TMPFILE, mode)`. **Amend T11/T8:** no vararg NOLINT on curl wrappers; NOLINT only where lint actually fires.
- V-J3 PARTIAL: `bugprone-signal-handler` is inert for every tested shape (no guard value — manual review). A `static` handler trips `misc-use-anonymous-namespace`. **Amend T8:** handler in an anonymous namespace; use `sigaction` (or `(void)std::signal` for `cert-err33-c`); the global flag needs `NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): async-signal flag`.
- V-J4 CONFIRMED: curl/system macros clean under `-isystem`; don't add `-I`.
- V-J5 PARTIAL: gcc `-Wuseless-cast` fires only on prvalue same-type casts; clang-tidy `readability-redundant-casting` flags lvalue ones. Keep "take `long` params directly" (justification fixed).
- Side findings: `misc-include-cleaner` wants `<signal.h>` symbols spelled via the right header; `misc-const-correctness` on read-only out-params.

## Cluster H — CMake/CTest/E2E harness (CMake 4.4.4): 4 CONFIRMED, V-H2 PARTIAL
- V-H1 CONFIRMED (`find_path REQUIRED`; IMPORTED → `-isystem`; `CMAKE_DL_LIBS=dl` on clang — re-check under gcc at use-time).
- V-H2 PARTIAL: `file(ARCHIVE_EXTRACT)` extracts STORED zips, but a corrupt archive is a **fatal, uncatchable** script error, and a missing EOCD / 0-byte file extracts **silently**. **Amend T2:** `EXPECT_EPUB` uses byte-level checks (`file(READ … HEX)`, `file(SIZE)`, local header signature + mimetype at offset 30, EOCD `504b0506` at end); any extraction runs in a child `cmake -P` via `execute_process` and asserts rc + a non-empty file list. Deep structural checks stay in the `[cli]` tier with `zip_reader`.
- V-H3, V-H4 CONFIRMED (two FIXTURES_SETUP auto-pulled under `-R/-L`, release preset builds tests; WORKING_DIRECTORY applies to pipes and `script -qec`). **Amend T2:** create `{{WORK}}` dirs before use; compare rc numerically.
- V-H5 CONFIRMED with caveats: glob `*` matches dot-files; `[` is a character class; `;` breaks lists. **Amend T2/T18b:** use `LIST_DIRECTORIES false`, `EXISTS` for known names, fixture titles without `[`/`;`.

## Cluster G — EPUB 3.3 / OCF / ZIP: CONFIRMED (2 PARTIAL)
Sources: W3C EPUB 3.3 Rec, PKWARE APPNOTE.TXT.
- V-G1 CONFIRMED (+ `mimetype` is never listed in the manifest).
- V-G2 CONFIRMED on spec; epubcheck acceptance of `urn:mangadex:…` untested (no epubcheck locally). `dcterms:modified` as `<meta property>`, no fractional seconds, literal `Z`.
- V-G3 CONFIRMED (`spread=landscape` not deprecated; do **not** emit the deprecated package-level `rendition:viewport`).
- V-G4, V-G5, V-G7 CONFIRMED (WebP is a core media type; NCX optional legacy).
- V-G6 PARTIAL: **Amend T10:** nested `<ol>` only inside an `<li>` that begins with `<a>`/`<span>`, never empty; a volume with no chapter entries is a leaf `<a>`.
- V-G8 CONFIRMED (from the XML `Char` production).
- V-G9 PARTIAL: 0xFFFF / 0xFFFFFFFF are ZIP64 sentinels. **Amend T5/T9:** max **65534** entries, every size/offset ≤ **0xFFFFFFFE**; `max_zip_entries_per_epub = 65534`.
- `epubcheck` is **not** on conda-forge/bioconda → stays `[human]` (acceptance 21).

## Clusters D + E — libstdc++ 16 C++23 / nlohmann_json 3.12.0: ALL CONFIRMED
- V-D1: jthread/stop_token/stop_callback/cv_any work under ASan+UBSan and **TSan** on both compilers (`cxx-probe --preset tsan` works). Fallback not needed.
- V-D2, V-D4 CONFIRMED.
- V-D3 CONFIRMED. **Note T9/T10:** day arithmetic on `sys_days` (`year_month_day + days` does not compile).
- V-D5 CONFIRMED for the probe: static libstdc++ + jthread → max GLIBC_2.26. `release-check.binary` remains the real proof.
- V-E1, E2, E4 CONFIRMED.
- V-E3: parse and destroy are iterative; **`dump()` (and presumably copy/`==`) recurse** — ASan stack-overflow at depth 1e6. **Amend T6:** the depth pre-scan (64) is *mandatory*; never `dump()`/copy unvalidated JSON. Keep a try/catch at the parser boundary for `get<>` exceptions.

## Cluster I — packaging & runtime linkage: dlopen approach STANDS
- V-I1 CONFIRMED: libcurl 8.23.0 is in both envs transitively (via cmake); headers + `libcurl.so.4`, no CURLConfig.cmake. `pixi add libcurl` should be a near no-op (real run needed — T0, human-approved ask).
- V-I2 CONFIRMED: no static ssl/crypto/z/krb5 → static libcurl rejected.
- V-I3 CONFIRMED (binding): dlopen+dlsym bind to Debian 13 host `libcurl.so.4` (`@@CURL_OPENSSL_4`) and `libcurl-gnutls.so.4` (`@@CURL_GNUTLS_3`) and to conda's unversioned lib. 7.61–7.x untested → keep the runtime version gate.
- V-I4 PARTIAL: mechanism confirmed, but the dev binary carries **DT_RPATH** (conda toolchain), not RUNPATH — searched for libcurl *and its deps* (dev loads conda OpenSSL even for a host libcurl; dev-only, harmless). Release binary: no RPATH/RUNPATH, NEEDED ⊆ {libpthread, libm, libc, ld-linux}. **Amend wording.**
- V-I5 PARTIAL: Debian compiled-in CA default confirmed (`/etc/ssl/certs/ca-certificates.crt`); conda reports `capath=NULL`. Non-Debian paths best-effort (human/CI). Handle NULL and unreadable `cainfo`.
- V-I6 PARTIAL: libcurl reads `http_proxy` (lowercase only), `https_proxy`/`HTTPS_PROXY`, `ALL_PROXY`, `NO_PROXY`; `HTTP_PROXY` is deliberately ignored. No CA env vars. **Amend T16 README.**
- V-I7 PARTIAL: ubuntu-26.04 image lists `curl 8.18.0` (→ `libcurl4t64`) by inference. **Amend T18b:** label `network-unreachable` (e.g. `needs-libcurl`) so it can be excluded if absent; it must fail red, not skip, when present and broken.
- V-I8 CONFIRMED on exercised paths: ASan/UBSan/LSan clean with dlopen'd conda and host libcurl (4-thread handle pool + TLS handshake, no dlclose); TSan clean (TLS part single-threaded). Concurrent TLS under TSan → T11 tests.

## Summary
Refuted / changed: **V-A2** (`--group` binding), **V-F3** (`includeUnavailable` string param), **V-F6** (epoch `X-RateLimit-Retry-After` primary, `Retry-After` undocumented), **V-F7** (report failures too), **V-G9** (65534 / 0xFFFFFFFE limits), **V-H2** (no in-script extraction), **V-J2/J3** (NOLINT placement, handler linkage), **V-E3** (depth pre-scan mandatory). Each refutation's fix was itself verified by the checker (probe or spec), so no re-check round is needed beyond the planner's revision.
Unverifiable locally (→ User Test Plan): real API shapes/error bodies, uploads cover URL, non-Debian CA paths, epubcheck acceptance, concurrent TLS under TSan (T11 tests cover).
