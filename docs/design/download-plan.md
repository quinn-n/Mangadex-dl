# Implementation plan: `mangadex-dl download` (MangaDex to fixed-layout EPUB)

Design: `/home/quinn/Scripts/Mangadex-dl/docs/design/download-spec.md` (APPROVED). Where they conflict, the gate decisions override the body: remove `hello`, keep ".epub or .pdf", add `--no-report`, dedupe default = most-covering group.

What I checked in the tree on 2026-10-17 (orientation only; the fact-checker re-confirms):
- `pixi list` (0.82.0): libcurl 8.23.0 is already in both envs, pulled in transitively by cmake. Also present: openssl 3.6.5, krb5, libssh2, libnghttp2, libpsl, libzlib, zstd, ca-certificates, and CLI11 2.7.2, nlohmann_json 3.12.0, Catch2 3.16.1, cmake 4.4.4.
- The env lib dir has only shared objects: no `libssl.a`, `libcrypto.a`, `libz.a` or `libkrb5*.a`. There is no `lib/cmake/CURL*` config, but `include/curl/curl.h` is there.
- `pixi search` shows a `libcurl-static` package, but its dependencies (openssl, krb5, libssh2, libzlib, zstd) are shared-only.
- conda's `libcurl.so.4` exports unversioned symbols. The host's (Debian 13) exports `curl_easy_init@@CURL_OPENSSL_4`.

---

## Approach

All logic goes into `mangadex_dl_core` as small pure modules. Each fallible function returns `Result<T>`. Network access sits behind a `Transport` interface and time behind a `Clock` interface, so planning, retries, rate limiting and the download pipeline are unit-tested with a fake transport, a fake clock and an in-repo fixture transport (`file://`).

**HTTPS: libcurl, loaded at runtime with `dlopen("libcurl.so.4")` in every build.** It is never linked at build time; builds compile only against conda-forge's `curl/curl.h`.
- **Rejected: a static libcurl.** conda-forge has no static OpenSSL, krb5 or zlib, so `libcurl-static` cannot produce a glibc-only binary.
- **Rejected: vendoring curl + mbedTLS through FetchContent.** That means a pinned TLS stack in our own release surface, our own CA discovery, security re-pins, and a dev TLS stack that differs from release.
- **Rejected: `NEEDED libcurl.so.4`.** The binary would not even start (`--help`, fixture mode) without libcurl, and the symbol versions differ per distro.
- **What dlopen gives:**
  - `NEEDED` stays within glibc (`libdl.so.2`, already allowlisted). `scripts/release-check.sh` is unchanged.
  - The CA store is found by the distro's own libcurl through its compiled-in defaults, with a probed fallback list.
  - A missing libcurl is a clean typed error (exit 1). Fixture mode never loads libcurl.
- **The trade-off:** a new runtime requirement (a TLS-capable `libcurl.so.4` on the user's machine), documented in the README and the error message, plus about 12 `dlsym` function pointers behind one RAII wrapper.

**ZIP: an in-house STORED-only writer with a table-driven CRC-32.**
- Page images are already compressed, so DEFLATE gains essentially nothing.
- The OCF `mimetype` entry must be stored anyway.
- zlib isn't available statically from conda-forge.
- STORED with sizes known up front (entries are buffered in memory) means no data descriptors and deterministic bytes.
- No image or zlib dependencies at all: the image sniffer only parses headers.

---

## Dependencies & pixi/CMake changes

| Change | Where | Task |
|---|---|---|
| `pixi add libcurl` (conda-forge; it is already in the lock transitively, so the solve should barely move). This is an **ask** prompt and a serialization point (`pixi.toml` + `pixi.lock`); never hand-edit the lock | common `[dependencies]` | T0 |
| `find_path(MANGADEX_DL_CURL_INCLUDE_DIR NAMES curl/curl.h REQUIRED)`, plus `add_library(mangadex_dl_curl_headers INTERFACE IMPORTED)` with `INTERFACE_INCLUDE_DIRECTORIES` set (imported means SYSTEM). Headers only, no link. This is a **documented exception to `find_package(CONFIG)`**: the env ships no CURLConfig.cmake, and FindCURL's `CURL::libcurl` would link the library, which we deliberately don't. It stays manager-agnostic (works on any distro with curl headers) | `CMakeLists.txt`, after `find_package(Threads)` | T0 |
| `target_link_libraries(mangadex_dl_core PRIVATE mangadex_dl_curl_headers ${CMAKE_DL_LIBS} PUBLIC Threads::Threads)`. dlopen lives in libdl below glibc 2.34, which matters at the 2.28 floor | `CMakeLists.txt` | T0 |
| `misc-include-cleaner.IgnoreHeaders` gains `curl/.*` (curl's umbrella header) | `.clang-tidy` | T0 |
| No FetchContent. No new presets, pixi tasks or CI steps. No change to `scripts/release-check.sh` (NEEDED stays within glibc) or to `cmake/Install.cmake` (`release-check.staged-files` unaffected) | — | — |
| Test-only executable `mangadex_dl_fixture_gen` and CTest fixture `e2e.fixtures` (`FIXTURES_SETUP e2e_fixtures`) | `tests/CMakeLists.txt` (serialization point) | T2 |
| E2E driver: `{{FIXTURES}}` placeholder, `EXPECT_FILES` / `EXPECT_ABSENT` / `EXPECT_EPUB`, tool cwd = `{{WORK}}`, every case requires `e2e_installed;e2e_fixtures` | `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake` (serialization point) | T2 |

**Static release, CA bundle and release-check (risk, addressed explicitly)**
- **NEEDED:** only `libc.so.6`, `libm.so.6`, `libdl.so.2`, `libpthread.so.0` and `ld-linux-x86-64.so.2`, all already in the allowlist. `-static-libstdc++ -static-libgcc` is unchanged. libcurl and OpenSSL are C libraries and don't clash with the static libstdc++.
- **Never `dlclose`:** that keeps symbolization intact and avoids LSan false positives on unloaded modules.
- **glibc floor 2.28:** new imports are `dlopen`/`dlsym`/`dlerror` (2.2.5), `renameat2` (exactly 2.28), `getrandom` (2.25), `sigtimedwait`, `pthread_sigmask` and `fstatat`. `release-check.binary` re-proves the floor.
- **Soname candidates, in order:** `libcurl.so.4`, then `libcurl-gnutls.so.4`. Never a path from the env or argv.
- **Minimum runtime check:** `curl_version_info()->version_num >= 0x073D00` (7.61.0, the RHEL 8 / glibc 2.28 era), `features & CURL_VERSION_SSL`, and `https` in `protocols`. Otherwise the error is Errc::failure, mapped to **exit 1**: `mangadex-dl: libcurl.so.4 with TLS support is required (…)`.
- **CA discovery:**
  1. libcurl's compiled-in `cainfo`/`capath`, from `curl_version_info` when `age >= CURLVERSION_SEVENTH`, if readable. Leave it unset so the distro default applies.
  2. Otherwise, the first readable path among `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`, `/etc/ssl/cert.pem` and `/etc/pki/tls/cacert.pem`, set as `CURLOPT_CAINFO`.
  3. Otherwise, leave it alone. `CURLE_SSL_CACERT_BADFILE` then maps to exit 3 with a "no CA certificates found; install ca-certificates" hint.
  - `SSL_VERIFYPEER`/`VERIFYHOST` are always on. The tool reads no env var itself. libcurl reads the proxy variables by itself, which goes in the README.
- **Dev/test builds:** dlopen finds conda's libcurl through the binary's/test executable's RUNPATH (`CMAKE_BUILD_RPATH`/`CMAKE_INSTALL_RPATH` = env lib) and uses conda's CA bundle.
- **Release-test** loads the *host's* libcurl. Only the `e2e.download.network-unreachable` case touches it, and CI's ubuntu-26.04 has libcurl4 (see V-I7). On a host without libcurl that case fails with exit 1 instead of 3, which correctly flags the runtime requirement.

---

## Assumptions to verify

**Cluster A — CLI11 (installed 2.7.2)**
- V-A1 `Option::group("")` hides an option from `--help` (for `--api-base`).
- V-A2 A `std::vector<std::string>` option with `->expected(1)` (plus `MultiOptionPolicy::TakeAll` if needed) collects repeated `--group X --group Y` and never swallows the following positional (`--group X <MANGA>`).
- V-A3 Subcommands created after root `fallthrough()` accept root `--json/-q/-v/--color` after `download`. `sub->footer()` plus the root formatter's `enable_footer_formatting(false)` applies to the subcommand's help.
- V-A4 `CLI::Range(1,8)` on an `int` (`-j 9`) and `CLI::IsMember` (`--layout`, `--direction`) raise `ParseError` subclasses (so `run()` maps them to 2).
- V-A5 A required positional `manga` followed by an optional positional `dir` (default `"."`) parses `download ID`, `download ID DIR`, and options interleaved before or after. A plain `add_flag("--no-report", bool)` is not treated as a negation of some `--report`. `-n` does not clash with the root's `-v`/`-q` in combined short flags.
- V-A6 Positionals containing invalid UTF-8 bytes are passed through unmodified (edge `invalid-utf8-echo` with DIR).

**Cluster B — libcurl C API semantics**
- V-B1 When each option was introduced: `CURLOPT_XFERINFOFUNCTION`/`XFERINFODATA` 7.32.0, `CURLOPT_ACCEPT_ENCODING` 7.21.6, `CURLOPT_MAXFILESIZE_LARGE` 7.11, `CURLOPT_CONNECTTIMEOUT_MS` 7.16.2, `CURLOPT_PROTOCOLS_STR` 7.85.0. `curl_easy_setopt` returns `CURLE_UNKNOWN_OPTION` for options the runtime library lacks.
- V-B2 Variadic argument types per option: `long` for numeric/bool options, `curl_off_t` for `*_LARGE`, function pointers with curl's exact callback signatures, `void*` for `*DATA`. Passing `int` where `long` is required is UB.
- V-B3 `XFERINFOFUNCTION` returning nonzero aborts with `CURLE_ABORTED_BY_CALLBACK`. `WRITEFUNCTION` returning `!= size*nmemb` aborts with `CURLE_WRITE_ERROR`. `HEADERFUNCTION` gets each header line, including the status line, not NUL-terminated, with `size == 1`.
- V-B4 `LOW_SPEED_LIMIT=1` plus `LOW_SPEED_TIME=30` aborts a stalled transfer with `CURLE_OPERATION_TIMEDOUT`. `MAXFILESIZE_LARGE` yields `CURLE_FILESIZE_EXCEEDED` when Content-Length is too big (the write callback cap covers chunked responses).
- V-B5 Thread safety:
  - `curl_global_init(CURL_GLOBAL_DEFAULT)` must run once before any thread uses curl.
  - An easy handle may be reused sequentially by different threads.
  - `CURLOPT_NOSIGNAL=1` is required in multi-threaded programs.
  - With NOSIGNAL, libcurl does **not** ignore SIGPIPE. Check whether 8.x sends with `MSG_NOSIGNAL` on the TLS path; it is informational, because the plan blocks SIGPIPE around `perform` anyway.
- V-B6 `curl_easy_reset` clears `WRITEDATA`/`HEADERDATA`/`XFERINFODATA` and keeps live connections.
- V-B7 Error codes: `CURLE_COULDNT_CONNECT`=7, `CURLE_COULDNT_RESOLVE_HOST`=6, `CURLE_OPERATION_TIMEDOUT`=28, `CURLE_PEER_FAILED_VERIFICATION`=60, `CURLE_SSL_CACERT_BADFILE`=77, `CURLE_SSL_CONNECT_ERROR`=35, `CURLE_RECV_ERROR`=56, `CURLE_GOT_NOTHING`=52. `CURLINFO_RESPONSE_CODE` takes a `long*`.
- V-B8 `decltype(&curl_easy_setopt)` names the real function despite typecheck-gcc.h's function-like macro (the macro expands only before `(`).

**Cluster C — Linux / glibc syscalls and signals**
- V-C1 `open(dir, O_TMPFILE|O_WRONLY|O_CLOEXEC, 0644)` (needs `_GNU_SOURCE`, kernel ≥ 3.11) fails with `EOPNOTSUPP` (or `EISDIR`/`EINVAL` on old kernels or filesystems) where unsupported. Those trigger the fallback; other errnos are real failures.
- V-C2 `linkat(AT_FDCWD, "/proc/self/fd/<n>", dirfd, name, AT_SYMLINK_FOLLOW)` publishes an O_TMPFILE inode without `CAP_DAC_READ_SEARCH` (`AT_EMPTY_PATH` would need it). It fails with `EEXIST` if `name` exists. An O_TMPFILE opened without `O_EXCL` is linkable.
- V-C3 glibc 2.28 exports `renameat2` (symbol version `GLIBC_2.28`, declared in `<stdio.h>` under `_GNU_SOURCE`). `RENAME_NOREPLACE` returns `EINVAL` on filesystems that don't support it (fallback: `linkat` + `unlinkat`).
- V-C4 `getrandom` (glibc 2.25, `<sys/random.h>`) for temp-name suffixes and jitter seeding.
- V-C5 Async-signal safety:
  - A `sigaction` handler that only stores to a lock-free `std::atomic<int>` (`is_always_lock_free` on x86-64) is async-signal-safe.
  - Restoring `SIG_DFL`, unblocking and `raise(sig)` from `main` gives the shell 130/143.
  - `pthread_sigmask(SIG_BLOCK,{SIGPIPE})` plus `sigtimedwait` with a zero timeout drains a pending thread-directed SIGPIPE.
- V-C6 `fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)` reports a dangling symlink as existing. `mkdirat` respects umask. `openat(..., O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)` fails with `ELOOP`/`ENOTDIR` on a symlink. `fsync` on a directory fd is permitted (EINVAL on some filesystems is ignorable).

**Cluster D — libstdc++ 16 / C++23 (clang 23 and gcc 16, both envs)**
- V-D1 `std::jthread`, `std::stop_source`/`stop_token`/`stop_callback` and `std::condition_variable_any::wait(lock, stop_token, pred)` are available and work under ASan and TSan.
- V-D2 `std::expected<void, Error>` with `and_then`/`transform_error`, and `std::print(std::ostream&, …)`.
- V-D3 `std::chrono::year_month_day`/`sys_days` arithmetic and `std::format("{:%FT%TZ}", sys_seconds)` (for `dcterms:modified`, without `gmtime`).
- V-D4 `std::from_chars` (integers) and `std::to_chars` are locale-free.
- V-D5 Static libstdc++ with `std::jthread`/`stop_token`/atomic wait pulls no glibc symbol newer than 2.28 (re-proven by `release-check.binary`).

**Cluster E — nlohmann_json 3.12.0**
- V-E1 `json::parse(first, last, nullptr, /*allow_exceptions=*/false)` returns `is_discarded()` on syntax errors **and** on invalid UTF-8 inside strings, and never throws.
- V-E2 `dump(2, ' ', false, error_handler_t::replace)` replaces invalid UTF-8 with U+FFFD.
- V-E3 Whether parsing or destroying deeply nested input recurses on the C stack. The plan keeps a depth pre-scan (limit 64) regardless; this decides only whether it is defence in depth or required.
- V-E4 `get_ptr<const json::string_t*>()` / `get_ptr<const json::number_integer_t*>()` / `get_ptr<const json::boolean_t*>()` return nullptr on a type mismatch (non-throwing access). `<nlohmann/json_fwd.hpp>` exists.

**Cluster F — MangaDex API (all designer API claims)**
- V-F1 Shape of `GET /manga/{id}?includes[]=cover_art&includes[]=author&includes[]=artist`: `{result, data:{id, attributes:{title{lang:str}, altTitles[{lang:str}], originalLanguage}, relationships[{id,type,attributes{name|fileName…}}]}}`. An unknown UUID returns HTTP 404; a malformed id returns 400.
- V-F2 Feed parameters: `translatedLanguage[]`, `limit` max 500, `offset`, `order[volume]=asc`, `order[chapter]=asc`, `includes[]=scanlation_group`, `contentRating[]` ∈ {safe, suggestive, erotica, pornographic}, `includeExternalUrl=1`, `includeEmptyPages=1`, `includeFuturePublishAt=0`. Response `{data[], limit, offset, total}`. The `offset + limit ≤ 10000` window cap and its error response.
  - **Superseded 2026-10-18:** `includeExternalUrl` / `includeEmptyPages` are *filters* on the live API (`=1` returns only those chapters) and are no longer sent; see download-plan-rev1.md (## T6) and download-factcheck.md (V-F2).
- V-F3 Chapter attributes: `volume` (string|null), `chapter` (string|null), `title`, `translatedLanguage`, `externalUrl` (string|null), `publishAt`, `updatedAt`, `pages` (int). Also whether `isUnavailable` exists and whether the feed needs an `includeUnavailable` parameter to return such chapters.
- V-F4 `GET /cover?manga[]=<id>&limit=100&offset=n` (max 100), with attributes `volume`, `fileName`, `locale`. Image URL `https://uploads.mangadex.org/covers/<mangaId>/<fileName>`.
- V-F5 `GET /at-home/server/{chapterId}` returns `{result, baseUrl, chapter:{hash, data[], dataSaver[]}}`. Image URL `{baseUrl}/data/{hash}/{file}` or `/data-saver/…`. Lifetime of `baseUrl`.
- V-F6 Rate limits: about 5 req/s per IP globally and 40/min for at-home. 429 semantics: `Retry-After` in delta-seconds, and **`X-RateLimit-Retry-After` as a Unix epoch timestamp** (not seconds), which changes `parse_retry_after`.
- V-F7 MD@H report: `POST https://api.mangadex.network/report` with JSON `{url, success, cached, bytes, duration}` (duration in ms). `cached` derives from an `X-Cache` header beginning with `HIT`. Only nodes whose host is not under `mangadex.org` are reported.
- V-F8 The User-Agent policy accepts `mangadex-dl/<version>`.

**Cluster G — EPUB 3.3 / OCF / ZIP (all designer EPUB claims)**
- V-G1 OCF: `mimetype` first, STORED, no extra field, content exactly `application/epub+zip` with no newline. `META-INF/container.xml` rootfile media type is `application/oebps-package+xml`.
- V-G2 `<package version="3.0" unique-identifier=…>` passes epubcheck 5.x. Required: `dc:identifier`, `dc:title`, `dc:language`, and `dcterms:modified` in the form `CCYY-MM-DDThh:mm:ssZ`. A free-form `urn:mangadex:<id>:<lang>:<unit>` identifier is accepted.
- V-G3 Fixed layout: `rendition:layout pre-paginated`, `rendition:spread landscape` (not deprecated in 3.3), `rendition:orientation auto`, the `rendition:` prefix reserved (no declaration needed), and `<meta name="viewport" content="width=W, height=H"/>` per page.
- V-G4 `belongs-to-collection` with refines `collection-type` = `series` and `group-position`.
- V-G5 Spine `page-progression-direction="rtl"`. Spine `toc="ncx"` plus NCX in a 3.x package (EPUB 2 compatibility).
- V-G6 Manifest `properties="cover-image"`, plus a legacy `<meta name="cover" content="…"/>`. Nav doc `properties="nav"` with `<nav epub:type="toc"><ol>`, nested `<ol>` per volume.
- V-G7 WebP is a core media type in EPUB 3.3 (the design warns on it for older readers).
- V-G8 XML 1.0 legal characters: `#x9 #xA #xD [#x20-#xD7FF] [#xE000-#xFFFD] [#x10000-#x10FFFF]`.
- V-G9 APPNOTE: local header, central directory and EOCD layouts. DOS date 1980-01-01 00:00 (date `0x0021`, time `0`). Version needed 10 for STORED. Flag bit 11 = UTF-8. Limits without ZIP64: 65535 entries, offsets and sizes `< 0xFFFFFFFF`. CRC-32 reflected polynomial `0xEDB88320`.

**Cluster H — CMake / CTest / E2E harness (CMake 4.4.4, floor 3.31)**
- V-H1 `find_path(... REQUIRED)` works, and an `INTERFACE IMPORTED` target's include dirs are treated as SYSTEM. `CMAKE_DL_LIBS` is `dl` with the conda sysroot.
- V-H2 `file(ARCHIVE_EXTRACT INPUT <zip> DESTINATION <dir>)` extracts a STORED zip and errors on a corrupt one (the driver captures it), and `file(READ <f> OFFSET <n> LIMIT <m> HEX)`.
- V-H3 `FIXTURES_REQUIRED "e2e_installed;e2e_fixtures"`: CTest auto-adds both setup tests under `-R`/`-L`, and they run in the release preset (test executables build there because BUILD_TESTING is ON).
- V-H4 `execute_process(... WORKING_DIRECTORY <dir>)` applies to every command of a two-command pipeline and to the `script -qec` TTY path. Existing edge cases don't depend on the cwd.
- V-H5 `file(GLOB)` patterns containing spaces (`out/Clean Title - Vol. 01.epub`) and leading-dot names (`out/.*.part-*`) behave as expected inside a `-P` script.

**Cluster I — packaging and runtime linkage (conda-forge, dlopen, CA)**
- V-I1 conda-forge `libcurl` provides `include/curl/curl.h` and `lib/libcurl.so.4`, and no CURLConfig.cmake. `pixi add libcurl` resolves without changing other packages in either env.
- V-I2 conda-forge ships no static `libssl.a`/`libcrypto.a`/`libz.a`/`libkrb5.a`, which is why static libcurl is rejected.
- V-I3 `dlopen("libcurl.so.4", RTLD_NOW|RTLD_LOCAL)` plus `dlsym` (default version) binds to distro libcurls with versioned symbols (`CURL_OPENSSL_4`, `CURL_GNUTLS_3`), and the libcurl.so.4 ABI is stable across 7.61 to 8.x.
- V-I4 `dlopen` from the executable searches its DT_RUNPATH (or DT_RPATH). The dev install's RUNPATH points at the env lib, and the release binary has none (so it gets the host's library). `-z nodlopen` doesn't affect calls to `dlopen`.
- V-I5 Distro libcurls ship a compiled-in CA default (Debian/Arch/Fedora/openSUSE). Confirm the fallback probe list above.
- V-I6 libcurl itself reads `http_proxy`/`HTTPS_PROXY`/`ALL_PROXY`/`NO_PROXY`. libcurl does **not** read `SSL_CERT_FILE`/`CURL_CA_BUNDLE` (only the curl CLI does). OpenSSL's `SSL_CERT_FILE` is ignored once curl sets a CAfile. These are README claims.
- V-I7 The ubuntu-26.04 GitHub runner image has `libcurl.so.4` (with OpenSSL) installed.
- V-I8 ASan/LSan with a dlopen'd, uninstrumented libcurl + OpenSSL 3 that is never dlclosed and has `curl_global_cleanup` called: no leak report at exit. TSan reports no false positives for a per-thread-sequential easy-handle pool. Probe this with `cxx-probe` in the real dev and tsan contexts.

**Cluster J — compiler-specific warnings and clang-tidy (both compilers, project flags)**
- V-J1 `reinterpret_cast<Fn*>(void*)` (dlsym result) under `-Wpedantic -Wcast-function-type` produces no warning on clang 23 or gcc 16.
- V-J2 clang-tidy 23 fires `cppcoreguidelines-pro-type-vararg` (curl_easy_setopt/getinfo), `cppcoreguidelines-pro-type-reinterpret-cast` (dlsym) and `concurrency-mt-unsafe` (whether it flags `dlerror`), so each occurs only inside the 5–6 centralised wrappers with `NOLINTNEXTLINE(check): reason`.
- V-J3 `bugprone-signal-handler` accepts a `static` function (or one with C linkage) whose body is only `std::atomic<int>::store(…, relaxed)`. If it requires `extern "C"`, use that.
- V-J4 curl headers through the SYSTEM include produce no `-Wold-style-cast`/`-Wcast-qual`/`-Wuseless-cast` noise. Using curl enum constants (not macros like `CURL_ZERO_TERMINATED`) in our TUs is warning-free. `O_TMPFILE`, `SIG_DFL` and `AT_*` macro expansions from system headers stay suppressed (as they already do in `io.cpp`).
- V-J5 GCC `-Wuseless-cast` on `static_cast<long>(x)` where `x` is already `long` (the curl wrappers take `long` parameters directly to avoid this).

**Clusters:** Cluster A: V-A1–V-A6 · Cluster B: V-B1–V-B8 · Cluster C: V-C1–V-C6 · Cluster D: V-D1–V-D5 · Cluster E: V-E1–V-E4 · Cluster F: V-F1–V-F8 · Cluster G: V-G1–V-G9 · Cluster H: V-H1–V-H5 · Cluster I: V-I1–V-I8 · Cluster J: V-J1–V-J5.

The riskiest cluster is I. Its probes (V-I3, V-I4, V-I8) should run before plan approval.

---

## Task breakdown

Conventions for all tasks:
- Namespace `mangadex_dl`. Every value type is an aggregate with default member initializers. Every type with an invariant is a class with a `[[nodiscard]] static Result<T> parse/make`.
- `#pragma once`. No `<CLI/CLI.hpp>` outside `src/app/`. No nlohmann or curl in public headers (`json_fwd.hpp` at most).
- Every track's "Done when" includes `nice pixi run check` green (long timeout) **plus** both track reviewers APPROVED.
- Within a batch, a track's tests may call only symbols defined by itself or by an already-integrated batch, because all unit tests link into one executable.
- Interface headers come from T1 and are **frozen**: changing one is a coordination event for the orchestrator, never an in-track edit.

### T0 — Dependency and build surface
- **Goal:** libcurl headers and libdl available to core, with no behaviour change.
- **Files:** `pixi.toml`, `pixi.lock` (via `pixi add libcurl`), `CMakeLists.txt`, `.clang-tidy`.
- **Owns:** `pixi.toml`, `pixi.lock`, `CMakeLists.txt`, `.clang-tidy`.
- **Build files for build-fixer:** the same set.
- **What changes:**
  - `find_path` plus the `mangadex_dl_curl_headers` INTERFACE IMPORTED target, with `message(STATUS "curl headers: …")`.
  - `target_link_libraries(mangadex_dl_core PRIVATE mangadex_dl_curl_headers ${CMAKE_DL_LIBS} PUBLIC Threads::Threads)`.
  - Comment the dlopen decision next to it.
  - `.clang-tidy` IgnoreHeaders adds `curl/.*`.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` **and** `nice pixi run -e gcc check` are green; the configure log shows the curl include dir in both envs; `nice pixi run release-test` is green (release surface touched).
- **Tests:** none new; the existing suite proves no regression.

### T1 — Frozen interfaces (public headers + exit codes + one stub)
- **Goal:** every cross-module declaration lands once, so batches 2–4 compile against stable interfaces.
- **Owns:**
  - `include/mangadex_dl/{attributes,error,exit_codes,manga_id,selection,model,options,filename,planner,report,image,zip,epub,mangadex_api,api_client,fetch_pool,interrupt,download}.hpp`
  - `include/mangadex_dl/net/{transport,retry,fixture_transport,curl_transport}.hpp`
  - `include/mangadex_dl/fs/output_dir.hpp`
  - `src/core/download.cpp` (stub only)
  - `tests/unit/interfaces/headers_test.cpp`
- **Build files for build-fixer:** none.
- **What changes (signatures are binding; doc comments state ownership and thread rules):**
  - `attributes.hpp`: the `MANGADEX_DL_LIFETIMEBOUND` shim (modern-cpp-conventions form).
  - `exit_codes.hpp`: `network = 3`, `not_found = 4`, `no_chapters = 5`, `incomplete = 6`, documented.
  - `error.hpp`:
    - `Errc` gains `network, not_found, no_chapters, incomplete, interrupted`.
    - `exit_code_for` maps them to 3/4/5/6. `interrupted` maps to `failure`, with a comment that main re-raises the signal first.
  - `manga_id.hpp`:
    - `class MangaId`: `static Result<MangaId> parse(std::string_view)` (UUID or URL forms, lowercased), `std::string_view str() const LIFETIMEBOUND`, defaulted `==`/`<=>`, and a default constructor giving the nil UUID (never produced by `parse`).
    - `[[nodiscard]] bool looks_like_manga_id(std::string_view) noexcept;`
    - `[[nodiscard]] bool is_uuid(std::string_view) noexcept;`
  - `selection.hpp`:
    - `class ChapterNumber`: `static std::optional<ChapterNumber> parse(std::string_view)` (digits with an optional `.digits`), `std::string normalized() const`, `operator<=>`, `operator==`. It stores digit strings, so it can't overflow.
    - `struct SelectionRange { std::optional<ChapterNumber> low{}; std::optional<ChapterNumber> high{}; };`
    - `class Selection`: `static Result<Selection> parse(std::string_view spec)`, `static Selection all() noexcept`, `bool is_all()`, `bool is_none()`, `bool matches(std::string_view label) const` (a non-numeric label matches only `all`).
    - `[[nodiscard]] std::string normalize_label(std::string_view)` (normalized numeric form, or the raw label).
  - `model.hpp` (aggregates):
    - Enums: `Direction{ltr,rtl}`, `DirectionChoice{automatic,ltr,rtl}`, `Layout{volumes,chapters,single}`, `EpubKind{volume,chapter,oneshot,single}`, `SkipReason{external,no_pages,unavailable,duplicate}`, `ImageQuality{data,data_saver}`.
    - Structs: `ScanGroup{id,name}`, `Chapter{id, std::optional<std::string> volume, chapter; title; language; std::vector<ScanGroup> groups; int pages=0; bool external=false; bool unavailable=false; publish_at; updated_at}`, `Cover{std::optional<std::string> volume; file_name; locale}`, `MangaInfo{id; titles (vector<pair<lang,text>> in API order); alt_titles; original_language; authors; artists}`, `FeedPage{chapters; offset; limit; total}`, `CoverPage{covers; offset; limit; total}`, `AtHomeServer{base_url; hash; data; data_saver}`.
    - Plan types: `PlannedEpub{EpubKind kind; file_name; display_title; std::optional<std::string> volume, chapter; std::vector<Chapter> chapters; std::optional<Cover> cover; identifier; std::optional<std::string> series_position; std::size_t planned_pages=0}`, `SkippedChapter{id; volume; chapter; SkipReason reason; std::optional<std::string> kept}`, `DedupeNote{key; kept_id; std::vector<std::string> dropped_ids; rule}`, `MangaSummary{id,title,url,language; Direction direction}`, `Plan{MangaSummary manga; Layout layout; std::vector<PlannedEpub> epubs; std::vector<SkippedChapter> skipped; std::vector<DedupeNote> dedupe}`.
  - `options.hpp`:
    - `inline constexpr std::string_view default_api_base{"https://api.mangadex.org"};`
    - `class ApiBase` with `enum class Kind{official,https,http_loopback,fixture}`: `static Result<ApiBase> parse(std::string_view)`, `kind()`, `std::string_view url() const LIFETIMEBOUND` (no trailing `/`), `const std::filesystem::path& fixture_root() const LIFETIMEBOUND`, and a default = official.
    - `struct RawDownloadArgs{manga; dir="."; volumes="all"; loose_chapters="all"; layout="volumes"; lang="en"; std::vector<std::string> groups; bool data_saver=false; direction="auto"; std::optional<std::string> title; bool overwrite=false, dry_run=false; int jobs=4; api_base=std::string{default_api_base}; bool no_report=false;}`
    - `struct DownloadOptions{MangaId manga{}; std::filesystem::path dir{"."}; Selection volumes=Selection::all(); Selection loose_chapters=Selection::all(); Layout layout=Layout::volumes; std::string lang="en"; std::vector<std::string> groups{}; ImageQuality quality=ImageQuality::data; DirectionChoice direction=DirectionChoice::automatic; std::optional<std::string> title{}; bool overwrite=false, dry_run=false; unsigned jobs=4; ApiBase api_base{}; bool report=true;}`
    - `[[nodiscard]] Result<DownloadOptions> validate_download_args(const RawDownloadArgs&);` returns Errc::usage with a one-line message plus a hint. `report` is false when `no_report` is set, the base isn't the default, or in fixture mode.
    - `[[nodiscard]] Result<std::string> validate_language(std::string_view);`
    - `[[nodiscard]] Result<std::string> validate_title_override(std::string_view);`
  - `filename.hpp`:
    - `[[nodiscard]] std::string sanitize_file_stem(std::string_view text, std::string_view fallback);`
    - `[[nodiscard]] std::string format_volume_label(std::string_view);` (`03`)
    - `[[nodiscard]] std::string format_chapter_label(std::string_view);` (`012`, `012.5`, non-numeric raw)
    - `[[nodiscard]] std::string epub_file_name(std::string_view stem, const PlannedEpub&);`
  - `planner.hpp`:
    - `struct PlanInput{MangaInfo manga{}; std::vector<Chapter> chapters{}; std::vector<Cover> covers{};}`
    - `struct PlanOptions{Selection volumes=Selection::all(); Selection loose_chapters=Selection::all(); Layout layout=Layout::volumes; std::string lang="en"; std::vector<std::string> preferred_groups{}; DirectionChoice direction=DirectionChoice::automatic; std::optional<std::string> title_override{};}`
    - `[[nodiscard]] Result<Plan> build_plan(const PlanInput&, const PlanOptions&);` An empty `epubs` list is valid; the caller maps it to exit 5.
    - `[[nodiscard]] std::string resolve_title(const MangaInfo&, std::string_view lang);`
    - `[[nodiscard]] Direction resolve_direction(DirectionChoice, std::string_view original_language) noexcept;`
    - `inline constexpr std::size_t max_zip_entries_per_epub = 65535;`
  - `report.hpp`:
    - `enum class EpubStatus{planned,written,exists,failed};`
    - `struct EpubOutcome{PlannedEpub epub{}; std::string path{}; EpubStatus status=EpubStatus::planned; bool replaced=false; std::size_t pages=0; std::uint64_t bytes=0; std::string error{};}`
    - `struct RunSummary{std::size_t planned=0, written=0, exists=0, failed=0;}`
    - `struct RunReport{bool dry_run=false; Layout layout=Layout::volumes; MangaSummary manga{}; std::vector<EpubOutcome> epubs{}; std::vector<SkippedChapter> skipped{};}`
    - `[[nodiscard]] RunSummary summarize(const RunReport&) noexcept;`
    - `[[nodiscard]] int exit_code_for(const RunReport&) noexcept;` returns 6 if any failed, else 5 if there are no epubs, else 0.
    - `[[nodiscard]] std::string report_to_json(const RunReport&);` produces one document: `format_version:1`, the schema in the design, `replace` handler, trailing `\n`.
  - `image.hpp`:
    - `enum class MediaType{jpeg,png,gif,webp};`
    - `struct ImageInfo{MediaType type=MediaType::jpeg; std::uint32_t width=0, height=0;}`
    - `[[nodiscard]] Result<ImageInfo> sniff_image(std::span<const std::byte>);`
    - `[[nodiscard]] std::string_view media_type_name(MediaType) noexcept;`
    - `[[nodiscard]] std::string_view file_extension(MediaType) noexcept;`
    - `inline constexpr std::uint32_t max_image_side = 65535;`
  - `zip.hpp`:
    - `class ByteSink` (abstract; protected ctor; copy/move deleted; virtual dtor): `[[nodiscard]] virtual Result<void> write(std::span<const std::byte>) = 0;`
    - `class MemorySink final : public ByteSink` with `const std::vector<std::byte>& bytes() const LIFETIMEBOUND`.
    - `[[nodiscard]] std::uint32_t crc32(std::span<const std::byte>, std::uint32_t crc = 0) noexcept;`
    - `class ZipWriter{ explicit ZipWriter(ByteSink& sink LIFETIMEBOUND); Result<void> add_stored(std::string_view name, std::span<const std::byte> data); Result<void> finish(); std::uint64_t bytes_written() const noexcept; std::size_t entry_count() const noexcept; }` Names are ASCII, unique and ≤ 65535 bytes. The class enforces the entry/offset limits and refuses `add_stored` after `finish`.
  - `epub.hpp`:
    - `struct EpubNavEntry{std::string label{}; std::size_t first_page=0; std::optional<std::string> group{};}` (`group` = the volume label in `single`).
    - `struct EpubMetadata{identifier; title; language; creators; contributors; source; modified; series; std::optional<std::string> series_position; Direction direction=Direction::ltr;}`
    - `class EpubWriter{ EpubWriter(ZipWriter& zip LIFETIMEBOUND, EpubMetadata meta); Result<void> begin(); Result<void> add_cover(std::span<const std::byte>, ImageInfo); Result<void> add_page(std::span<const std::byte>, ImageInfo); void add_nav_entry(EpubNavEntry); Result<void> finish(); }`
    - Pure generators: `std::string xml_escape(std::string_view)`, `std::string container_xml()`, `std::string page_xhtml(std::string_view image_href, ImageInfo, std::string_view title)`, `Result<std::string> format_modified(std::string_view iso8601)`.
  - `mangadex_api.hpp`:
    - Constants: `max_api_json_bytes = 16 MiB`, `max_image_bytes = 32 MiB`, `max_pages_per_chapter = 5000`, `feed_page_limit = 500`, `feed_offset_cap = 10000`, `cover_page_limit = 100`, `max_json_depth = 64`.
    - Parsers, all returning Errc::network on malformed input: `parse_manga(std::string_view) -> Result<MangaInfo>`, `parse_feed_page -> Result<FeedPage>`, `parse_cover_page -> Result<CoverPage>`, `parse_at_home -> Result<AtHomeServer>` (validates base URL, hash, file names and page count).
    - Validators: `bool is_valid_at_home_token(std::string_view) noexcept`, `Result<void> validate_at_home_base_url(std::string_view)`.
    - URL builders: `manga_request_url(api, id)`, `feed_request_url(api, id, lang, offset)`, `cover_request_url(api, id, offset)`, `at_home_request_url(api, chapter_id)`, `image_url(base, ImageQuality, hash, file)`, `cover_image_url(uploads, id, file)`.
    - MD@H: `bool is_mangadex_org_host(std::string_view url) noexcept`, `struct AtHomeReport{url; bool success=false; bool cached=false; std::uint64_t bytes=0; std::uint64_t duration_ms=0;}`, `std::string at_home_report_json(const AtHomeReport&)`.
  - `net/transport.hpp`:
    - `enum class HttpMethod{get,post};`
    - `struct HttpRequest{HttpMethod method=HttpMethod::get; std::string url{}; std::string body{}; std::string content_type{}; std::size_t max_body_bytes=0; std::chrono::milliseconds connect_timeout{15000}; std::chrono::seconds stall_timeout{30}; std::stop_token cancel{};}`
    - `struct HttpResponse{int status=0; std::string body{}; std::string retry_after{}; std::string x_ratelimit_retry_after{}; std::string x_cache{}; std::chrono::milliseconds elapsed{};}`
    - `enum class TransportErrc{connect,resolve,tls,timeout,cancelled,too_large,protocol,local};`
    - `struct TransportError{TransportErrc kind=TransportErrc::protocol; std::string message{};}`
    - `using TransportResult = std::expected<HttpResponse, TransportError>;`
    - `class Transport` (abstract, thread-safe contract, Command-style special members): `[[nodiscard]] virtual TransportResult fetch(const HttpRequest&) = 0;`
    - `class Clock` (abstract, thread-safe): `virtual steady_clock::time_point now() const noexcept = 0; virtual system_clock::time_point wall_now() const noexcept = 0; [[nodiscard]] virtual bool sleep_until(steady_clock::time_point, std::stop_token) = 0;` (returns false if stopped).
    - `[[nodiscard]] Clock& system_clock();` and `[[nodiscard]] std::unique_ptr<Clock> make_instant_clock();` (virtual time; sleeps return immediately).
  - `net/retry.hpp`:
    - `struct RetryConfig{int max_attempts=4; std::chrono::milliseconds base_delay{1000}; std::chrono::milliseconds max_jitter{500}; std::chrono::seconds retry_after_cap{60};}`
    - `struct AttemptOutcome{int attempt=1; std::optional<int> status{}; std::optional<TransportErrc> error{}; std::optional<std::chrono::milliseconds> retry_after{};}`
    - `struct RetryDecision{bool retry=false; std::chrono::milliseconds delay{};}`
    - `[[nodiscard]] RetryDecision decide_retry(const AttemptOutcome&, const RetryConfig&, std::chrono::milliseconds jitter) noexcept;`
    - `[[nodiscard]] std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view retry_after, std::string_view x_ratelimit_retry_after, std::chrono::system_clock::time_point now);`
    - `class RateLimiter{ RateLimiter(Clock& clock LIFETIMEBOUND, std::chrono::milliseconds min_interval); [[nodiscard]] bool acquire(std::stop_token); }` (mutex inside; returns false if stopped).
  - `net/fixture_transport.hpp`: `[[nodiscard]] Result<std::unique_ptr<Transport>> make_fixture_transport(std::filesystem::path root);`
  - `net/curl_transport.hpp`:
    - `struct CurlTransportConfig{std::string user_agent{};}`
    - `[[nodiscard]] Result<std::unique_ptr<Transport>> make_curl_transport(const CurlTransportConfig&);`
    - `struct CaDefaults{std::optional<std::string> cainfo{}; std::optional<std::string> capath{};}`
    - `inline constexpr std::array<std::string_view,6> ca_bundle_candidates{…};`
    - `[[nodiscard]] std::optional<std::string> select_ca_bundle(const CaDefaults&, const std::function<bool(const std::string&)>& readable);` (nullopt = keep libcurl's default)
  - `api_client.hpp`:
    - `struct Endpoints{std::string api{std::string{default_api_base}}; std::string uploads{"https://uploads.mangadex.org"}; std::optional<std::string> at_home_override{}; std::optional<std::string> report_url{};}`
    - `[[nodiscard]] Endpoints endpoints_for(const ApiBase&, bool report);`
    - `class TraceSink` (abstract, thread-safe): `virtual void trace(std::string line) = 0;`
    - `struct ClientConfig{Endpoints endpoints{}; RetryConfig retry{}; std::chrono::milliseconds api_interval{250}; std::chrono::milliseconds at_home_interval{1500};}`
    - `class ApiClient{ ApiClient(Transport& LIFETIMEBOUND, Clock& LIFETIMEBOUND, ClientConfig, std::function<std::uint32_t(std::uint32_t)> jitter, TraceSink* trace); Result<MangaInfo> fetch_manga(const MangaId&, std::stop_token); Result<std::vector<Chapter>> fetch_feed(const MangaId&, std::string_view lang, std::stop_token); Result<std::vector<Cover>> fetch_covers(const MangaId&, std::stop_token); Result<AtHomeServer> fetch_at_home(std::string_view chapter_id, std::stop_token); Result<std::string> fetch_page(const AtHomeServer&, ImageQuality, std::string_view file, std::stop_token); Result<std::string> fetch_cover(const MangaId&, std::string_view file, std::stop_token); }` The class is thread-safe. A 404 on the manga gives Errc::not_found; other failures give Errc::network; a stop gives Errc::interrupted.
  - `fs/output_dir.hpp`:
    - `class UniqueFd` (move-only RAII; `int get() const noexcept`; `release`).
    - `enum class PublishOutcome{written,replaced,exists};`
    - `class TempFile final : public ByteSink` (move-only; buffered; `Result<void> write(...) override; Result<void> sync_and_close();`). The destructor discards: it closes the O_TMPFILE fd or unlinks the fallback name.
    - `class OutputDir{ static Result<OutputDir> open(const std::filesystem::path&, bool create); static Result<std::optional<OutputDir>> open_if_exists(const std::filesystem::path&); Result<bool> exists(std::string_view name) const; Result<TempFile> create_temp(std::string_view final_name); Result<PublishOutcome> publish(TempFile&&, std::string_view final_name, bool overwrite); const std::filesystem::path& path() const LIFETIMEBOUND; }`
  - `fetch_pool.hpp`:
    - `struct FetchedPage{std::string bytes{}; ImageInfo info{};}`
    - `using PageFetcher = std::function<Result<FetchedPage>(std::size_t index, std::stop_token)>;` (called concurrently on workers)
    - `using PageConsumer = std::function<Result<void>(std::size_t index, FetchedPage&&)>;` (called on the caller's thread, in index order)
    - `class FetchPool{ explicit FetchPool(unsigned workers); ~FetchPool(); Result<void> run(std::size_t count, const PageFetcher&, const PageConsumer&, std::stop_token); }` At most `workers` results are buffered. The first error stops new work, drains in-flight items and is returned. The function arguments are borrowed only for the duration of `run`.
  - `interrupt.hpp`:
    - `class ScopedInterruptHandlers{ [[nodiscard]] static Result<ScopedInterruptHandlers> install(); ~ScopedInterruptHandlers(); }` Move-only; restores the previous SIGINT/SIGTERM dispositions.
    - `[[nodiscard]] bool interrupt_requested() noexcept;` and `[[nodiscard]] int interrupt_signal() noexcept;`
    - `void reraise_pending_interrupt() noexcept;` (SIG_DFL, unblock, `raise`; returns if nothing is pending)
    - `void reset_interrupt_for_testing() noexcept;` and `void simulate_interrupt_for_testing(int) noexcept;`
    - `class InterruptBridge{ explicit InterruptBridge(std::stop_source); ~InterruptBridge(); }` A jthread polls `interrupt_requested()` every 50 ms and calls `request_stop`.
  - `download.hpp`:
    - `class DownloadObserver` (abstract; **every call happens on the thread that called `run_download`**): `on_plan(const Plan&)`, `on_epub_begin(const PlannedEpub&, std::size_t index, std::size_t total)`, `on_pages(std::size_t done, std::size_t total)`, `on_epub_end(const EpubOutcome&)`, `on_warning(std::string_view)`, `on_verbose(int level, std::string_view)`.
    - `struct DownloadServices{Transport* transport=nullptr; Clock* clock=nullptr; std::function<std::uint32_t(std::uint32_t)> jitter{}; std::stop_source stop{}; std::string user_agent{};}` (pointers borrowed for the call)
    - `[[nodiscard]] Result<RunReport> run_download(const DownloadOptions&, const DownloadServices&, DownloadObserver&);` Its Error is fatal (exits 1/3/4, or interrupted). On success it returns a `RunReport` (exit 0/5/6).
  - `src/core/download.cpp`: a **stub** `run_download` returning `Errc::failure` "not implemented" (unnamed parameters). It is the only stub; T14 replaces it.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` is green and `headers_test.cpp` compiles every new header on its own.
- **Tests (unit):**
  - `headers_test.cpp` includes each header in isolation (one TU per header is not needed; one TU including all, in sorted order).
  - `static_assert`s on `exit_code_for(Errc::network)==3` … `==6`.
  - Header-inline defaults (`Selection::all().is_all()` only if inline; otherwise skip).

### T2 — E2E harness extension and the fixture builder/generator
- **Goal:** hermetic MangaDex fixtures for every tier, and file assertions in the E2E driver.
- **Owns:**
  - `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake`
  - `tests/support/mdx_fixture.hpp`, `tests/support/temp_dir.hpp`
  - `tests/e2e/fixtures/fixture_gen.cpp`, `tests/e2e/inputs/README`
  - `tests/unit/support/mdx_fixture_test.cpp`
- **Build files for build-fixer:** `tests/CMakeLists.txt`.
- **What changes:**
  - `mdx_fixture.hpp` is header-only and self-contained: its own CRC-32/Adler-32, depends only on nlohmann and std, never on core.
    - Types: `ImageKind{png,gif,jpeg_header,webp_header,garbage,truncated_png}`, `PageImage{kind; uint16 w,h; bool missing=false}`, `FixtureGroup{id,name}`, `FixtureChapter{id; optional volume, chapter; title; lang="en"; group_ids; int pages; external_url; bool unavailable; publish_at; updated_at; hash; images}`, `FixtureCover{volume,file,locale}`, `FixtureManga{id; title_en; alt_titles; original_language; authors; artists; groups; chapters; covers}`.
    - Functions: `manga_json`, `feed_pages_json(manga, limit=500)`, `covers_json`, `at_home_json(chapter)`, `image_bytes(PageImage)`, `write_fixture_tree(root, manga)` (layout per the design: `api/manga/<id>.json`, `api/manga/<id>/feed/offset-<n>.json`, `api/cover/offset-<n>.json`, `api/at-home/server/<chapterId>.json`, `uploads/covers/<id>/<file>`, `at-home/{data,data-saver}/<hash>/<file>`).
    - Canned fixtures:
      - `clean_fixture()`: `11111111-1111-4111-8111-111111111111`, "Clean Title", original `ja`; vol 1 ch 1–2, vol 2 ch 3; PNG and GIF pages; covers for vol 1 and 2; no skips.
      - `basic_fixture()`: `22222222-2222-4222-8222-222222222222`; dedupe across two groups, loose ch 10 and 10.5, a oneshot, external, `pages==0`, unavailable, an `fr` chapter, JPEG/PNG/GIF/WebP pages.
      - `bulk_fixture()`: `33333333-3333-4333-8333-333333333333`; 4000 loose chapters, a long title, no images, so dry-run stdout is ≥ 128 KiB.
      - `incomplete_fixture()`: `44444444-…`; one chapter's page file is missing.
      - `nochapters_fixture()`: `55555555-…`; only `de` chapters.
    - PNG and GIF are real, decodable minimal images. JPEG and WebP are header-only (sniffable, not decodable), documented.
  - `temp_dir.hpp`: an RAII unique temp dir (created under `std::filesystem::temp_directory_path()`, `mkdtemp`-style via `getrandom` suffix, removed recursively).
  - `fixture_gen.cpp`: `mangadex_dl_fixture_gen <out-dir>` writes `<out>/{clean,basic,bulk,incomplete,nochapters}`.
  - `tests/CMakeLists.txt`: builds it (warnings + sanitizer options) and registers `add_test(e2e.fixtures …)` with `FIXTURES_SETUP e2e_fixtures`, label `e2e`, and the sanitizer ENVIRONMENT.
  - Driver:
    - `{{FIXTURES}}` = `${PROJECT_BINARY_DIR}/e2e-fixtures` (expanded in ARGS, ENV and STDIN; normalized in goldens).
    - Every case gets `FIXTURES_REQUIRED "e2e_installed;e2e_fixtures"`.
    - `WORKING_DIRECTORY "${CASE_WORK_DIR}"` on every `execute_process`.
    - New multi-value keywords:
      - `EXPECT_FILES <rel>...`: each exists under WORK, is a regular file and is non-empty.
      - `EXPECT_ABSENT <rel-or-glob>...`: nothing matches.
      - `EXPECT_EPUB <rel>...`: the file exists; bytes 0–3 are `504b0304`; the name at offset 30 is `mimetype`; the method is 0; the extra length is 0; the content is `application/epub+zip`; then `file(ARCHIVE_EXTRACT)` into `WORK/epub-check/<n>` must succeed and contain `META-INF/container.xml` and `OEBPS/content.opf`.
    - The header comment documents the keywords.
- **Dependencies:** none (no core symbols).
- **Done when:** `nice pixi run check` is green; `e2e.fixtures` passes and produces the five trees; the whole existing E2E suite (still on `hello`) stays green with the cwd change.
- **Tests (unit):** `mdx_fixture_test.cpp`:
  - Every JSON blob parses with nlohmann.
  - The feed page count for bulk is 8.
  - PNG/GIF bytes carry correct signatures and IHDR CRCs.
  - `write_fixture_tree` creates the expected files.

### T3 — Sanitizer positive controls for the dangling-view risk class
- **Goal:** prove ASan catches the view/userdata use-after-free class this feature introduces.
- **Owns:** `tests/sanitizer-controls/CMakeLists.txt`, `tests/sanitizer-controls/dangling_view_heap.cpp`, `tests/sanitizer-controls/dangling_view_scope.cpp`.
- **Build files for build-fixer:** `tests/sanitizer-controls/CMakeLists.txt`.
- **What changes:**
  - `dangling-view-heap`: a `std::string_view` into a destroyed long (heap) `std::string`, read afterwards. Marker `ERROR: AddressSanitizer: heap-use-after-free`, EXIT 86.
  - `dangling-view-scope`: a view to a block-scoped buffer, read after the block (`O0`). Marker `ERROR: AddressSanitizer: stack-use-after-scope`, EXIT 86.
  - Both get NOLINT-with-reason, like the existing controls.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` is green, both controls fire in the default env, and `nice pixi run -e gcc test -L sanitizer-control` is green. If GCC doesn't report use-after-scope for the pattern, report it as a refutation; do not weaken the marker.
- **Tests:** the two controls themselves.

### T4 — Value parsing: manga id, selection grammar, option validation
- **Owns:**
  - `src/core/manga_id.cpp`, `src/core/selection.cpp`, `src/core/options.cpp`
  - `tests/unit/manga_id/manga_id_test.cpp`, `tests/unit/selection/selection_test.cpp`, `tests/unit/options/options_test.cpp`
- **Build files for build-fixer:** none.
- **What changes:** implementations of `MangaId`, `looks_like_manga_id`, `is_uuid`, `ChapterNumber`, `Selection`, `normalize_label`, `ApiBase::parse`, `validate_language`, `validate_title_override` and `validate_download_args`.
  - **`ApiBase::parse`:** `https://host[:port][/path]` with no userinfo, query or fragment. `http://` only for `127.0.0.1`, `localhost`, `[::1]`. `file://` + an absolute path with no `.`/`..`/empty segments and no `?#`.
  - **Title validation:** non-empty after trimming, valid UTF-8, no C0/DEL, ≤ 1024 bytes.
  - **Usage errors:** both selections `none`; a DIR that looks like an id ("only one manga per run"); `jobs` outside 1–8 as a defensive check.
  - ASCII case folding only, `from_chars` only.
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Acceptance 1: the full accept/reject sets (bare UUID upper/lower; `https://`/`http://`/`www.`/no scheme; slug; trailing `/`; `?q`; `#f`; wrong host; short UUID; extra path segments; empty).
  - Acceptance 2: `all`, `none`, `1,3-5,8-`, `-3`, `1.5`, `10.50` == `10.5`, `007` == `7`; `5-3` is an error; whitespace is an error; an empty item is an error; non-numeric labels match only `all`.
  - Every `validate_download_args` usage error, with its message text.
  - The `report` derivation.

### T5 — Binary formats: image sniffer and ZIP writer
- **Owns:**
  - `src/core/image.cpp`, `src/core/zip.cpp`
  - `tests/unit/image/image_test.cpp`, `tests/unit/zip/zip_test.cpp`
  - `tests/support/zip_reader.hpp` (test-only reader: walks the central directory, checks local headers, extracts entries, verifies CRCs, reports method and extra length)
- **What changes:**
  - **Sniffer:** JPEG SOFn scan (bounded marker walk; skips RSTn/fill bytes), PNG IHDR, GIF logical screen, WebP RIFF VP8/VP8L/VP8X. Every read is bounds-checked; a side of 0 or > 65535 is an error; unknown formats give Errc::failure "unsupported image type".
  - **CRC-32:** table built `constexpr`.
  - **ZipWriter:** DOS timestamp fixed at 1980-01-01, version needed 10, UTF-8 flag only when a name is non-ASCII (all generated names are ASCII), checked `uint64` arithmetic for offsets with failure at `0xFFFFFFFF`, failure at the 65536th entry, duplicate-name rejection.
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Acceptance 7: each type and its dimensions; every truncation length 0…N of each header returns an error and never reads out of bounds (ASan proves it); garbage; side limits.
  - CRC-32 known vectors (`"123456789"` → `0xCBF43926`).
  - Zip: one entry, then many; round trip through `zip_reader`; mimetype-first with extra length 0; 65535/65536 entries (cheap, empty data); offset overflow through a fake sink that reports large sizes; `add_stored` after `finish` errors; byte-identical output from two runs.

### T6 — MangaDex API JSON parsing, validation and URL building
- **Owns:**
  - `src/core/mangadex_api.cpp`, `src/core/json_util.hpp`, `src/core/json_util.cpp` (private: `depth_ok(std::string_view, std::size_t)`, non-throwing getters)
  - `tests/unit/mangadex_api/mangadex_api_test.cpp`
- **What changes:** every `mangadex_api.hpp` function.
  - **Parsing:** size cap before parsing; depth pre-scan; `parse(..., nullptr, false)`; field access only through `get_ptr`.
  - **Feed parsing:** keeps chapters with any language (the planner re-filters).
  - **Chapter ids** must satisfy `is_uuid` (otherwise malformed, Errc::network), so they are path-safe.
  - **At-home validation:** `baseUrl` is https with no userinfo, query or fragment; hash and file names match `^[A-Za-z0-9][A-Za-z0-9._-]*$`; at most 5000 pages.
  - **URL builders** append exactly the design's query parameters in a fixed order (deterministic; fixture-mappable).
  - **`is_mangadex_org_host`:** the host equals `mangadex.org` or ends with `.mangadex.org`.
- **Dependencies:** T1, T2 (fixture JSON).
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Each parser on fixture JSON (clean/basic) and on hand-written variants: null volume/chapter, missing optional fields, wrong types, invalid UTF-8 in a string (must be Errc::network), 16 MiB + 1 bytes, depth 65, trailing garbage, an empty object.
  - Acceptance 9: at-home URL/hash/file validation accept and reject sets (`http://`, `https://u@h`, `?x`, `#f`, `../x`, `.x`, `x/y`, empty).
  - URL builders produce exact strings.
  - The report JSON shape.

### T7 — Network core: clocks, retry and rate limiting, fixture transport, test fakes
- **Owns:**
  - `src/core/net/clock.cpp`, `src/core/net/retry.cpp`, `src/core/net/fixture_transport.cpp`
  - `tests/unit/net/retry_test.cpp`, `tests/unit/net/rate_limiter_test.cpp`, `tests/unit/net/fixture_transport_test.cpp`
  - `tests/support/fake_transport.hpp`: scripted per-URL response queues, thread-safe, records requests
  - `tests/support/fake_clock.hpp`: manual steady/wall time; `sleep_until` advances virtual time and records sleeps; honours the stop token
- **What changes:**
  - **`system_clock()`:** a real clock whose `sleep_until` uses `std::condition_variable_any::wait_until(lock, stop_token, …)`.
  - **`make_instant_clock()`:** virtual time.
  - **`decide_retry`:** retries 5xx, 429, `timeout` and `protocol` (reset or empty reply) up to `max_attempts`, with delays of 1/2/4 s plus jitter; 429 uses `retry_after` capped at 60 s. It does **not** retry 4xx (other than 429), `connect`, `resolve`, `tls`, `too_large` or `cancelled` (⏸ decision 2).
  - **`parse_retry_after`:** delta-seconds, or an `X-RateLimit-Retry-After` epoch converted against `now`; garbage gives nullopt.
  - **`RateLimiter`:** mutex-protected min-interval scheduling, with `Clock::sleep_until` outside the lock.
  - **Fixture transport** (thread-safe; no `std::filesystem` traversal for opens):
    - Parses `file://<root>/…` and requires the root prefix; rejects `.`, `..` and empty segments.
    - Maps `api/<path>?…offset=<n>` to `api/<path>/offset-<n>.json`, other `api/<path>` to `api/<path>.json`, and non-api paths verbatim.
    - Walks components with `openat(dirfd, comp, O_NOFOLLOW|O_CLOEXEC|(O_DIRECTORY for intermediate components))`. `ENOENT`/`ENOTDIR` give status 404. `ELOOP` is a 404 plus a trace.
    - Reads up to `max_body_bytes` and returns `too_large` beyond that. POST gives 404 (no reporting in fixture mode).
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Acceptance 8: the retry table across status × error × attempt; the jitter bound; the Retry-After cap; the epoch form against a fake wall clock.
  - The limiter spacing under a fake clock; concurrent acquisition from 4 threads keeps spacing (also under `nice pixi run tsan -R unit.net`); a stop token interrupts a sleep.
  - Fixture transport: the mapping table, missing → 404, a symlink component → 404, `..` rejected, the size cap, `file://` outside the root rejected.

### T8 — Systems: output directory and atomic publish, interrupts, fetch pool
- **Owns:**
  - `src/core/fs/output_dir.cpp`, `src/core/interrupt.cpp`, `src/core/fetch_pool.cpp`
  - `tests/unit/fs/output_dir_test.cpp`, `tests/unit/interrupt/interrupt_test.cpp`, `tests/unit/fetch_pool/fetch_pool_test.cpp`
- **What changes:**
  - **`OutputDir::open`:** `mkdir`-p with 0755 (`mkdirat` per component); `openat(O_DIRECTORY|O_CLOEXEC)`. ENOTDIR/EACCES/EROFS give Errc::failure with a strerror_r message.
  - **`exists`:** `fstatat(AT_SYMLINK_NOFOLLOW)`.
  - **`create_temp`:** `openat(dirfd, ".", O_TMPFILE|O_WRONLY|O_CLOEXEC, 0644)`. On EOPNOTSUPP/EISDIR/EINVAL it falls back to `openat(dirfd, ".<final>.part-<12 hex getrandom>", O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW, 0644)` with a retry on EEXIST. The name is truncated to ≤ 255 bytes.
  - **`TempFile::write`:** a 1 MiB buffer and a full-write loop handling EINTR. ENOSPC/EIO give Errc::failure.
  - **`publish`:** fsync, then close-check, then:
    - O_TMPFILE, no-clobber: `linkat("/proc/self/fd/N", AT_SYMLINK_FOLLOW)`; EEXIST gives `exists`.
    - O_TMPFILE, overwrite: link to a fallback temp name, then `renameat` (giving `replaced` if the name existed).
    - Fallback, no-clobber: `renameat2(RENAME_NOREPLACE)`; on EINVAL, `linkat` + `unlinkat`.
    - Fallback, overwrite: `renameat`.
    - Then a best-effort directory fsync.
  - **Interrupts:** a `sigaction` flag handler (SA_RESTART, empty mask) storing into `static std::atomic<int>`. The scoped install saves and restores the old actions. `reraise_pending_interrupt`. The bridge is a jthread polling at 50 ms.
  - **FetchPool:** N `std::jthread`s and a work queue (mutex + `condition_variable_any`). The result window is a `std::vector<std::optional<Result<FetchedPage>>>` ring of size N. The consumer waits with the stop token. Workers block SIGPIPE/SIGINT/SIGTERM in their own masks at start (signals land on the main thread; SIGPIPE from sockets never kills a worker). The destructor requests stop and joins.
- **Dependencies:** T1, T2 (`temp_dir.hpp`).
- **Done when:** `nice pixi run check`, `nice pixi run -e gcc check` (syscall/compiler-sensitive) and `nice pixi run tsan -R 'unit\.(fetch_pool|interrupt)'` are all green.
- **Tests (unit):**
  - Publish no-clobber against an existing file, a symlink and a directory (all `exists`; no temp left).
  - Overwrite replaces the content.
  - The fallback path forced through a test-only seam: `OutputDir::open` takes an internal flag via a `testing::force_fallback_tempfiles(bool)` free function declared in `src/core/fs/output_dir_testing.hpp` (private to core plus tests).
  - A dropped TempFile leaves the dir empty (both paths).
  - DIR is a file gives an error. A read-only dir (when not root; otherwise SKIP with the reason).
  - `simulate_interrupt_for_testing` with flag, signal and reset. A real `raise(SIGINT)` with the handler installed sets the flag and the process survives. The bridge requests stop within 200 ms.
  - Pool: order preserved under random fetch delays; a window of `workers` is never exceeded (counter); the first error propagates and the rest are cancelled; a stop token cancels promptly; count 0; workers=1 vs 8 give an identical sequence.

### T9 — Planner and filenames
- **Owns:** `src/core/planner.cpp`, `src/core/filename.cpp`, `tests/unit/planner/planner_test.cpp`, `tests/unit/filename/filename_test.cpp`.
- **What changes:**
  - **Language re-filter.**
  - **Skips:** external, `pages==0`, unavailable (each a `SkippedChapter`).
  - **Selection:** volumes match `--volumes` on the volume label; loose chapters match `--loose-chapters`; oneshots match only `all`.
  - **Dedupe:**
    - Key = (normalized volume, normalized chapter); a null chapter is never deduped.
    - The deciding "group identity" is a chapter's sorted group-id set (⏸ decision 5).
    - Rules: first `--group` match (case-insensitive ASCII name or UUID), then the identity covering the most keys in the selection, then more pages, then earlier `publishAt` (parsed to UTC), then the smallest UUID.
    - Losers become `duplicate` skips with `kept`; a `DedupeNote` is recorded.
  - **Ordering:** numeric, then non-numeric bytewise, with the `none` volume last.
  - **Grouping per layout:** volumes (per volume, plus per loose chapter, plus oneshot), chapters, single.
  - **Covers per volume:** prefer locale == `--lang`, then the original language, then the smallest file name. Loose/oneshot epubs get no cover (first page). Single uses the first volume's cover.
  - **Per epub:** identifier `urn:mangadex:<id>:<lang>:<unit>`, display titles `<T> Vol. 3` / `Ch. 12`, the series position, and `planned_pages`. If the entry count (fixed entries + 2 × pages + cover) exceeds 65535, the epub is still planned but marked for failure (⏸ decision 6; the downloader reports it as `failed` without downloading).
  - **Filenames:** per the design's sanitize rules (controls/DEL/`/\<>:"|?*` become `_`; bidi controls and U+2028/9 dropped; whitespace collapsed; trim spaces, dots and a leading `-`; ≤ 150 bytes on a UTF-8 boundary; empty/`.`/`..` become the UUID; invalid UTF-8 bytes become `_`). The formats `Vol. 03`, `Ch. 012`, `Ch. 012.5`, `Vol. 03 Ch. 021`, `Oneshot [<8 hex>]`, single `<T>.epub`. Component ≤ 255 bytes, asserted.
- **Dependencies:** T1, T4 (Selection), T6 (fixture parsing in tests).
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Acceptance 3: canned feeds (basic) for all three layouts, exact `file_name` lists and order; the language filter; every skip reason.
  - Acceptance 4: the five dedupe rules, each isolated with one deciding factor; input order shuffled gives an identical plan.
  - Acceptance 5: the filename table, including 150-byte truncation inside a multibyte character, bidi, `..`, an all-illegal title, a leading `-`.
  - Title resolution and direction `auto` for `ja`/`ja-ro`/`ko`.

### T10 — EPUB generation
- **Owns:** `src/core/epub.cpp`, `tests/unit/epub/epub_test.cpp`.
- **What changes:**
  - **Entries:** `mimetype`; `META-INF/container.xml`; `OEBPS/images/cover.<ext>` + `OEBPS/cover.xhtml` when there is a cover; `OEBPS/images/p00001.<ext>` + `OEBPS/pages/p00001.xhtml` per page in order; finally `OEBPS/content.opf` (3.0, metadata per the design, the `rendition:*` properties, spine `page-progression-direction` and `toc="ncx"`, the cover-image property or a fallback on the first page image); `OEBPS/nav.xhtml` (nested under volume groups when `group` is set); `OEBPS/toc.ncx`.
  - **`xml_escape`** escapes `& < > " '`, drops illegal XML characters and invalid UTF-8.
  - **`format_modified`** converts ISO 8601 with an offset to UTC `Z` using chrono only.
  - Output is deterministic (fixed ordering, no clocks).
- **Dependencies:** T1, T5.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Acceptance 6: build into a `MemorySink`, read back with `zip_reader`. Check entry order, mimetype stored with no extra field and exact content, the container rootfile, OPF metadata and properties (string checks on the generated XML), viewport W×H per page, the nav and NCX labels, the series and position, rtl vs ltr, cover vs fallback.
  - The escaping table (control chars, U+FFFE, lone surrogate bytes, `]]>`).
  - `format_modified` with `+09:00` crossing a day.
  - Two builds are byte-identical.

### T11 — libcurl transport (dlopen) and CA selection
- **Owns:**
  - `src/core/net/curl_transport.cpp`, `src/core/net/curl_api.hpp` (private), `src/core/net/curl_api.cpp`
  - `tests/unit/net/curl_transport_test.cpp`, `tests/unit/net/ca_bundle_test.cpp`
  - `tests/support/loopback_http_server.hpp`: binds 127.0.0.1:0; a jthread serves scripted responses (status, headers, body, delay, drop-connection, slow drip); records requests; RAII join
- **Build files for build-fixer:** none (T0 provided the headers and libdl).
- **What changes:**
  - **`CurlApi`** (private class): `static Result<std::shared_ptr<const CurlApi>> load()`.
    - A process-wide singleton guarded by a `std::once_flag`, never dlclosed.
    - dlopen order: `libcurl.so.4`, then `libcurl-gnutls.so.4`.
    - Resolves 12 symbols through one `template<class Fn> Fn* sym(void*, const char*)` (the single NOLINTed `reinterpret_cast`).
    - Version, SSL and https checks; `curl_global_init` once.
    - Typed wrappers `setopt_long(CURL*, CURLoption, long)`, `setopt_str(..., const char*)`, `setopt_ptr(..., void*)`, `setopt_off(..., curl_off_t)`, `setopt_write_cb`, `setopt_header_cb`, `setopt_xfer_cb`, and `getinfo_long`. They are the only vararg call sites, each with a NOLINT reason.
  - **`CurlTransport final : Transport`:**
    - A mutex-protected free list of RAII easy handles, with `curl_easy_reset` on return to the pool (clears the userdata pointers).
    - Per `fetch`: a stack-local `TransferState{std::string body; std::size_t cap; headers; std::stop_token cancel}` passed as userdata. The write callback enforces the cap (returns 0 to abort). The header callback copies `Retry-After`, `X-RateLimit-Retry-After` and `X-Cache` (case-insensitive ASCII). The xferinfo callback returns 1 when `cancel.stop_requested()`.
    - Options: `NOSIGNAL=1`, `FOLLOWLOCATION=0`, `PROTOCOLS_STR="https,http"` (ignore `UNKNOWN_OPTION`), `CONNECTTIMEOUT_MS`, `LOW_SPEED_LIMIT 1`/`LOW_SPEED_TIME` = stall, `MAXFILESIZE_LARGE` = cap, `ACCEPT_ENCODING ""`, `USERAGENT`, `SSL_VERIFYPEER 1`/`VERIFYHOST 2`, `CAINFO` from `select_ca_bundle` (computed once at load), and POSTFIELDS/POSTFIELDSIZE_LARGE plus a Content-Type slist owned by an RAII `SlistPtr`.
    - `perform` runs inside a `SigpipeGuard` (block SIGPIPE; after perform, drain with `sigtimedwait` at zero timeout; restore the mask).
    - Error mapping per V-B7 into `TransportErrc`.
  - **`make_curl_transport`:** loads the API (Errc::failure with an actionable message) and returns the transport.
- **Dependencies:** T0, T1.
- **Done when:** `nice pixi run check`, `nice pixi run -e gcc check`, `nice pixi run tsan -R unit.net.curl` and `nice pixi run release-test` are green.
- **Tests (unit, hermetic, loopback only):**
  - 200 body; 404 status passthrough; 503 status passthrough (retries belong to ApiClient).
  - Header capture; the User-Agent was sent; POST body and Content-Type.
  - A `max_body_bytes` overflow via Content-Length and via chunked gives `too_large`.
  - A stall with a 1 s `stall_timeout` gives `timeout`.
  - `cancel` via stop_source mid-drip gives `cancelled` within 2 s.
  - Connection refused on a closed port gives `connect`.
  - The server dropping the connection mid-body: SIGPIPE does not kill the test, and the result is `protocol`/`timeout`.
  - 4 threads × 20 sequential fetches (handle pool reuse; tsan).
  - `ca_bundle_test`: the selection table with an injected `readable` predicate.

### T12 — ApiClient (pagination, limits, retries, at-home, reporting)
- **Owns:** `src/core/api_client.cpp`, `tests/unit/api_client/api_client_test.cpp`.
- **What changes:**
  - **`endpoints_for`:**
    - Official: the defaults plus the report URL when `report` is on.
    - https/http custom: `api` = base, uploads default, no report.
    - Fixture: `api` = `file://<root>/api`, uploads = `file://<root>/uploads`, at-home override = `file://<root>/at-home`, no report.
  - **Requests:** every request goes through the shared API limiter (`api_interval`), plus the at-home limiter for `/at-home/server`, plus the retry loop (`decide_retry`, `Clock::sleep_until` with the stop token, jitter from the injected function). Tracing goes to `TraceSink` (`GET <url> -> <status> (<ms>)`).
  - **Feed:** paginates until `offset + limit >= total`. If `total > feed_offset_cap` (or the next offset + limit would exceed it), the result is Errc::network "title has more than 10000 chapters in this language; refusing to truncate".
  - **Covers:** paginate at 100.
  - **Pages:** `fetch_page` uses the override base when set; at-home validation is already in the parse. After each page from a non-mangadex.org host, it sends a best-effort report POST when a report URL exists (errors ignored; 5 s timeout).
  - **Caps:** JSON responses at `max_api_json_bytes`; images at `max_image_bytes`.
  - **Error mapping:** a stop gives Errc::interrupted; a 404 on the manga gives Errc::not_found; everything else exhausted gives Errc::network with `<url>: <reason>`.
- **Dependencies:** T1, T6, T7.
- **Done when:** `nice pixi run check` and `nice pixi run tsan -R unit.api_client` are green.
- **Tests (unit, `FakeTransport` + `FakeClock`):**
  - Pagination across 3 pages; the offset cap.
  - 429 with each Retry-After form gives the exact recorded sleeps; 5xx gives 1/2/4 s + jitter, then exhaustion maps to network.
  - Connection refused is not retried.
  - Limiter spacing across concurrent `fetch_page` calls.
  - Report POST sent only for non-mangadex.org hosts and only with a report URL; the report payload shape.
  - Fixture endpoints against `make_fixture_transport` on a `temp_dir` with the `clean` fixture.
  - Stop during a backoff sleep gives interrupted.

### T13 — Run report JSON and the console observer
- **Owns:**
  - `src/core/report.cpp`, `src/app/cmd_download_output.hpp`, `src/app/cmd_download_output.cpp`
  - `tests/unit/report/report_test.cpp`, `tests/cli/download_output_test.cpp`
- **What changes:**
  - **Core:** `summarize`, `exit_code_for(const RunReport&)`, and `report_to_json` (keys and enum spellings exactly per the design; `error` null when empty; `chapters[].groups[{id,name}]`; `skipped_chapters[].kept` null or an id).
  - **App** (`cmd_download_output.hpp`):
    - `struct OutputPolicy{bool json=false; bool quiet=false; int verbose=0; bool progress=false; bool color_stderr=false;}`
    - `[[nodiscard]] OutputPolicy output_policy(const GlobalOptions&, const Io&);` Progress is on only when `stderr_tty && !quiet && !json && !env.term_dumb`. Color via `resolve_color(globals.color, env, stderr_tty)`.
    - `class ConsoleObserver final : public DownloadObserver` (holds `Io*` and the policy):
      - Streams `<path>\n` to `io.out` for `written` (and for `planned` in dry-run) unless json.
      - Warnings: `mangadex-dl: warning: …` (`warning:` yellow), not with `-q`. Errors via `print_error`.
      - Verbose levels 1 and 2.
      - Progress: `\r[3/12] Vol. 03: 45/180 pages`, padded with spaces to clear the previous line, cleared (`\r` + spaces + `\r`) before any warning or error and at the end. No cursor escapes.
      - Counts `exists` and emits one `N files already exist` warning at the end.
    - `void print_error(Io&, const OutputPolicy&, std::string_view)` (`error:` red).
    - `void print_report_json(Io&, const RunReport&)`.
- **Dependencies:** T1 (types and interfaces only; `run_download` is not called).
- **Done when:** `nice pixi run check` is green.
- **Tests:**
  - Unit: a JSON golden-by-assertion for success, dry-run, exit-5 (`epubs: []`) and exit-6 shapes; invalid UTF-8 in a title is still valid JSON (U+FFFD); exit code priority.
  - In-process (`download_output_test.cpp`, constructing `ConsoleObserver` over `ostringstream` Io):
    - Paths streamed; `-q` suppresses warnings.
    - Progress only with `stderr_tty`; none with json/quiet/`term_dumb`.
    - `NO_COLOR` or `--color=never` gives no ANSI; `--color=always` gives a yellow `warning:`.
    - The JSON path never contains ANSI even with `--color=always`.

### T14 — Download orchestration (`run_download`)
- **Owns:**
  - `src/core/download.cpp` (replaces the stub)
  - `src/core/download/plan_phase.cpp`, `src/core/download/execute_phase.cpp`, `src/core/download/internal.hpp`
  - `tests/unit/download/download_test.cpp`
- **What changes:**
  - **Plan phase:**
    - ApiClient from `endpoints_for(options.api_base, options.report)`.
    - `fetch_manga` (not_found gives exit 4), `fetch_feed`, `fetch_covers` (failure is a warning plus no covers).
    - `build_plan`, then `observer.on_plan` and the skip warnings. An empty plan gives a report with epubs `[]`.
  - **Dry-run:** `OutputDir::open_if_exists`; each epub is `exists` (omitted from stdout) or `planned`; no downloads, no directory creation.
  - **Execute phase** (sequential per epub, on the calling thread):
    - exists check (skip unless `--overwrite`).
    - `fetch_at_home` per chapter, right before the chapter.
    - Pages through `FetchPool::run`. The fetcher runs on workers: `fetch_page`, plus up to 3 at-home refreshes on repeated failure (a mutex-protected per-chapter server snapshot), plus `sniff_image`. Unsupported media fails the chapter; WebP warns once per epub.
    - The consumer runs on the calling thread: `EpubWriter::add_page` into a `TempFile` through a `ZipWriter`, and `on_pages`.
    - Cover download: a failure is a warning and falls back to the first page.
    - Finish, then `publish`. EEXIST at publish gives `exists`.
  - **Failure handling:**
    - A per-epub network or media failure: the TempFile is dropped, the epub is `failed` with an error, and the run continues.
    - A local FS failure (Errc::failure from OutputDir/TempFile): abort, return the error (exit 1).
    - Interruption: an `InterruptBridge` from `services.stop`; any Errc::interrupted unwinds (TempFile destructors clean up) and is returned.
  - **Trace lines:** queued by a private mutex-protected `TraceSink` impl and drained to `on_verbose(2, …)` on the calling thread between page events.
  - `byte-identical for any --jobs` holds by construction: the consumer runs in index order.
- **Dependencies:** batches 1–3 (T4–T12).
- **Done when:** `nice pixi run check`, `nice pixi run -e gcc check` and `nice pixi run tsan -R unit.download` are green.
- **Tests (unit; fixture transport plus instant clock on `temp_dir` trees, plus FakeTransport for failure injection):**
  - The clean run writes 2 epubs; `zip_reader` validates them.
  - A re-run gives all `exists`. `--overwrite` gives `replaced` with byte-identical content.
  - `jobs` 1 vs 8 are byte-identical.
  - Dry-run writes nothing and creates no DIR.
  - Basic: skips, dedupe, the WebP warning.
  - Incomplete: one `failed` plus others written, and `exit_code_for == 6`.
  - Nochapters gives an empty plan (5). A 404 manga gives Errc::not_found. A feed 503 ×4 gives Errc::network.
  - A DIR that is a file gives Errc::failure.
  - A simulated interrupt during a page fetch (FakeTransport hook calling `simulate_interrupt_for_testing`) gives Errc::interrupted and no files or temps in DIR (both temp paths, via the T8 testing seam).
  - The observer sees calls only on the test thread (records `std::this_thread::get_id()`).

### T15 — `download` command (CLI binding and service wiring)
- **Owns:** `src/app/cmd_download.hpp`, `src/app/cmd_download.cpp`.
- **What changes:**
  - **`[[nodiscard]] std::unique_ptr<Command> register_download(CLI::App& parent, Command*& selected);`** `DownloadCommand final : Command` owns a `RawDownloadArgs` member and binds it:
    - Positionals `manga` (required) and `dir`.
    - Options `--volumes`, `--loose-chapters`, `--layout` (IsMember), `-l,--lang`, `--group` (vector, `expected(1)`), `--data-saver`, `--direction` (IsMember), `--title`, `--overwrite`, `-n,--dry-run`, `-j,--jobs` (Range 1–8), `--no-report` ("Don't send image-server reliability reports to MangaDex@Home"), and `--api-base` (`group("")`, hidden).
    - A help footer with examples.
    - The callback only selects.
  - **`run`:**
    1. `validate_download_args`; usage gives `mangadex-dl: <msg>` plus a hint, exit 2, empty stdout.
    2. `output_policy`.
    3. `ScopedInterruptHandlers::install`.
    4. Transport: fixture gives `make_fixture_transport` + `make_instant_clock`; otherwise `make_curl_transport({.user_agent = "mangadex-dl/" + version()})` + `system_clock()`.
    5. Jitter seeded once from `getrandom` (falling back to the steady clock).
    6. `run_download`.
    7. On Error: `print_error` and `error.exit_code()`. Interrupted returns 1; main re-raises.
    8. On a report: `print_report_json` when json, then `exit_code_for(report)`.
  - With `--json`, exits 2, 3, 4 and 1 print nothing on stdout (⏸ decision 3).
- **Dependencies:** T1 (stub `run_download` links), T13.
- **Done when:** `nice pixi run check` is green (compiles and lints; it is not registered yet, so behaviour tests land in T17/T18a).
- **Tests:** none at this tier (registration is pending). This is stated in the hand-off, and the reviewers check it against T18a's cases.

### T16 — User documentation
- **Owns:** `README.md`.
- **What changes:**
  - Usage and examples.
  - The exit-code table 0–6, 70.
  - **Runtime requirement:** a TLS-capable `libcurl.so.4` (or `libcurl-gnutls.so.4`), loaded at runtime.
  - CA bundle behaviour; proxy env vars read by libcurl; no other env or config.
  - Fixture mode is test-only (hidden `--api-base`).
  - Interruption guarantees, and the SIGPIPE/SIGKILL `.part-` caveat on fallback filesystems.
  - What `--no-report` disables.
- **Dependencies:** none (content from this plan); the batch 4 slot is only for review cadence.
- **Done when:** `nice pixi run check` is green (README is installed under docs; the `staged-files` allowlist already covers the doc dir).
- **Tests:** none.

### T17 — Registration, `hello` removal, edge knobs, main re-raise (small, late)
- **Owns:**
  - Edited: `src/app/cli.cpp`, `src/app/main.cpp`, `tests/cli/cli_test.cpp`, `tests/e2e/cases/_edge.cmake`
  - Deleted: `src/app/cmd_hello.hpp`, `src/app/cmd_hello.cpp`, `include/mangadex_dl/greet.hpp`, `src/core/greet.cpp`, `tests/unit/core/greet_test.cpp`, `tests/e2e/cases/hello.cmake`
- **What changes:**
  - **`cli.cpp`:** `commands.push_back(register_download(app, inv.selected));` replaces hello. The exit footer lists 0–6 and 70 from the constants (network, not found, no chapters, incomplete).
  - **`main.cpp`:** after `rc = finish_stdout(rc)`, call `mangadex_dl::reraise_pending_interrupt();` then `return rc;` (comment: signal deaths only, never by hand).
  - **`_edge.cmake` knobs:**
    - `EDGE_BULK_ARGS download --dry-run --layout chapters --api-base file://{{FIXTURES}}/bulk 33333333-3333-4333-8333-333333333333 out`
    - `EDGE_JSON_ARGS download --json --dry-run --api-base file://{{FIXTURES}}/clean 11111111-1111-4111-8111-111111111111`
    - `EDGE_ECHO_ARGS` = the same without `--json` (the appended last argument becomes DIR)
    - `EDGE_COLOR_ARGS ""`, with a comment that stdout is never colored (design "Color"; ⏸ decision 7)
  - `cli_test.cpp`: `download --help` exits 0 and lists `--no-report` but not `--api-base`; the root help lists the codes 3–6.
- **Dependencies:** T14, T15.
- **Done when:** `nice pixi run check` is green except `e2e.edge.help`, which needs a bless. The track hands off a **bless request**: `pixi run bless '^e2e\.edge\.help$'` (reason: SUBCOMMANDS now shows `download`; exit table 0–6, 70), plus a request that the human runs `git rm tests/e2e/golden/hello.default.stdout` (orphaned by the removed case). After the bless, `check` is fully green.
- **Tests:** `cli_test.cpp` additions; the whole edge suite re-pointed (bulk ≥ 128 KiB, so the SIGPIPE cases are meaningful).

### T18a — In-process CLI tests for `download`
- **Owns:** `tests/cli/download_cli_test.cpp`.
- **Tests:** build fixtures with `mdx_fixture` into a `temp_dir`, then `run_cli({"download","--api-base","file://"+root, id, out})`.
  - Acceptance 10: the expected files exist, the zip structure is valid (`zip_reader`), stdout is exactly the paths, exit 0.
  - Acceptance 11: a re-run gives exit 0, empty stdout and an `already exist` warning; `--overwrite` is byte-identical; `-j1` vs `-j8` are byte-identical.
  - Acceptance 12: `--json` for success, dry-run, exit 5 and exit 6 (parse with nlohmann; check the keys); `--color=always --json` has no ESC.
  - Acceptance 13: each semantic usage error gives 2, empty stdout and a hint on stderr; `-j 9` and `--layout bogus` give 2.
  - Exit 4 and 5 against the fixtures.
  - A `stderr_tty` progress line; none with `--json`.
  - An interrupt: a fixture-transport run with `simulate_interrupt_for_testing` before the call gives rc 1, `interrupt_signal()==SIGINT`, nothing written; then reset.
- **Dependencies:** T17.
- **Done when:** `nice pixi run check` and `nice pixi run tsan -R cli.download` are green.

### T18b — Installed-binary E2E cases for `download`
- **Owns:** `tests/e2e/cases/download.cmake`.
- **Cases** (suite `download`; exact exit; streams separate):
  - `help` (GOLDEN, stderr empty, `--api-base` absent).
  - `success-clean` (exit 0; stdout regex of exactly 2 lines; `EXPECT_EPUB "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub"`; `EXPECT_ABSENT "out/.*.part-*"`; stderr empty).
  - `dry-run-clean` (GOLDEN; `EXPECT_ABSENT out`).
  - `json-dry-run` (GOLDEN, `EXPECT_JSON`, `EXPECT_NO_ANSI`).
  - `json-success` (`EXPECT_JSON`, `"status": "written"`).
  - `layout-single`, `layout-chapters` (file names).
  - `not-found` (`66666666-6666-4666-8666-666666666666` on clean: exit 4, stdout empty).
  - `no-chapters` (exit 5, stdout empty) and `no-chapters-json` (exit 5, `EXPECT_JSON`, `"epubs": \[\]`).
  - `incomplete` (exit 6; stdout lists the good file; `EXPECT_FILES` good; `EXPECT_ABSENT` the failed one and `.part-` files).
  - `network-unreachable` (`--api-base http://127.0.0.1:1`, exit 3, stdout empty).
  - `dir-is-file` (DIR `{{INPUTS}}/README`, exit 1).
  - `dev-full-dry-run` (`STDOUT_FILE /dev/full`, exit 1, `write error`).
  - Usage: `both-none`, `bad-spec`, `bad-lang`, `dir-looks-like-id`, `bad-api-base` (`ftp://x`), `jobs-9` (all exit 2, stdout empty).
  - `webp-warning` (basic: exit 0, stderr `warning:.*WebP`).
  - `quiet` (basic `-q`: stderr empty).
  - TTY: `tty-progress` (clean, merged output matches `\[1/2\]`), `tty-progress-json` (no `\[1/`), `tty-progress-term-dumb` (no `\[1/`), `tty-no-color-stderr` (basic `NO_COLOR=1`, `EXPECT_NO_ANSI`).
  - `pipe-clicolor-force-stderr` (basic, `CLICOLOR_FORCE=1`, `EXPECT_STDERR_REGEX "${ESC}\\["`; stdout has no ANSI).
- **Dependencies:** T17 (plus T2 driver features).
- **Done when:** `nice pixi run check` is green after the human blesses `pixi run bless '^e2e\.download\.(help|dry-run-clean|json-dry-run)$'` (reason: new GOLDEN cases). Also `nice pixi run release-test` is green (the release install runs fixture mode without libcurl; the network case loads the host's libcurl).

---

## Parallelization plan

| Batch | Tracks (parallel within a batch) | Serialization points held (one track each) |
|---|---|---|
| **Batch 1** | T0, T1, T2, T3 | T0: `CMakeLists.txt`, `pixi.toml`+`pixi.lock`, `.clang-tidy` · T1: `include/mangadex_dl/error.hpp`, `exit_codes.hpp` (+ all new headers) · T2: `tests/CMakeLists.txt`, the E2E driver |
| **Batch 2** | T4, T5, T6, T7, T8 | none (new `src/core` files and tests only) |
| **Batch 3** | T9, T10, T11, T12, T13 | none |
| **Batch 4** | T14, T15, T16 | none |
| **Batch 5** | T17 (cli.cpp registration + hello removal + edge knobs + main.cpp) | `src/app/cli.cpp`, `include/mangadex_dl/greet.hpp` (deletion), `_edge.cmake` |
| **Batch 6** | T18a, T18b | none |

Notes:
- **Shared-build hazard.** All unit tests link into one executable (`mangadex_dl_unit_tests`), and all `cmd_*.cpp` into the app.
  - The batch boundaries above guarantee that every symbol a track's tests or code reference is defined by that track or an integrated batch.
  - T15 links only T1's `run_download` stub, so it can run beside T14.
  - T14 must keep `src/core/download.cpp` compiling at every intermediate build: work in `src/core/download/*.cpp` and swap the stub body last.
- A compile error in another track's file is a coordination event: report it, don't fix it.
- The T1 headers are frozen from batch 2 on. A needed signature change is a coordination event (the orchestrator amends T1's header in a mini-task between batches).
- The gcc gate (`-e gcc check`) is required for T0, T8, T11 and T14. The tsan runs are listed per task.

---

## Test plan (per tier)

- **Unit (Catch2, `mangadex_dl::core`):** per task above. Coverage targets: manga id, selection, options/ApiBase, image sniff (every truncation), zip and CRC, API parsing (malformed, oversized, deep, invalid UTF-8), retry and limiter (fake clock), fixture transport (path escapes), planner (layouts, skips, dedupe rules 1–5, ordering), filenames, EPUB XML and escaping, curl transport (loopback server: caps, stall, cancel, refused, SIGPIPE survival, pool reuse), ApiClient (pagination, offset cap, 429/5xx schedules, reporting), output dir (O_TMPFILE and fallback, no-clobber, overwrite, cleanup), interrupts, pool (order, window, cancel), downloader (end-to-end over the fixture transport, interrupt cleanup, observer thread).
- **In-process CLI (`tests/cli/`):** T13 `download_output_test.cpp` (output policy, progress, color, JSON without ANSI), T17 `cli_test.cpp` (help surface, exit table), T18a `download_cli_test.cpp` (acceptance 10–13 plus exits 4/5 plus interrupt plus progress).
- **E2E (`tests/e2e/cases/download.cmake`):** T18b's list covers acceptance 14, 15, 16, 18 and 19. The driver gains `EXPECT_FILES`/`EXPECT_ABSENT`/`EXPECT_EPUB` and `{{FIXTURES}}` (T2). Fixture data:
  - generated at test time by `mangadex_dl_fixture_gen` (CTest fixture `e2e.fixtures`, including the about 2 MB bulk tree);
  - committed inputs stay limited to the existing `tests/e2e/inputs/README` (used as a regular-file DIR).
- **Edge suite (`_edge.cmake`, T17):** knobs re-pointed per T17. Bulk = 4000-chapter dry-run, ≥ 128 KiB of newline-separated paths, stderr empty. JSON and echo use the clean fixture (no warnings, so `EXPECT_STDERR_EMPTY` holds). `EDGE_COLOR_ARGS` is empty, so `tty-color*`, `pipe-clicolor-force*`, `no-color-beats-force` and `pipe-no-ansi` are no longer registered. That needs **cli-ux-reviewer + quality-reviewer sign-off**; stderr-color coverage moves into T18b.
- **Sanitizer controls (T3):** `dangling-view-heap` (heap-use-after-free) and `dangling-view-scope` (stack-use-after-scope). The existing `data-race` control covers the threading class under `tsan`.
- **TSan:** `nice pixi run tsan` must pass whole before the board (pool, limiter, api client, curl, downloader, in-process download). CI already runs it in both envs.
- **Release-test:** `nice pixi run release-test` in T0, T11, T18b and `/harden`.
  - Expect `release-check.binary`: NEEDED ⊆ glibc (libdl.so.2 new and allowed), GLIBC ≤ 2.28 (renameat2 = 2.28 exactly), no RPATH.
  - `release-check.staged-files` is unchanged.
  - Fixture-mode E2E needs no libcurl; `network-unreachable` loads the host's `libcurl.so.4`.
- **Goldens:**
  - **Changed:** `tests/e2e/golden/edge.help.stdout` (T17).
  - **Removed:** `tests/e2e/golden/hello.default.stdout` (T17; the human runs `git rm`).
  - **Added:** `download.help.stdout`, `download.dry-run-clean.stdout`, `download.json-dry-run.stdout` (T18b).
  - **Unchanged:** `edge.version.stdout`.
  - Implementers never write goldens; each is a bless request with its regex and reason.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**Views**
- `MangaId::str()`, `ApiBase::url()`/`fixture_root()`, `OutputDir::path()` and `MemorySink::bytes()` return views or references: each needs LIFETIMEBOUND, and no call site may bind them to a temporary (`ApiBase::parse(x)->url()`).
- `ZipWriter(ByteSink&)` and `EpubWriter(ZipWriter&)` borrow: the sink (TempFile) must outlive the writers, so declare it first in `execute_phase`.
- `RateLimiter(Clock&)` and `ApiClient(Transport&, Clock&)` borrow: the transport and clock must outlive the client (they are owned by `cmd_download::run`, declared before `run_download`).
- `DownloadServices` raw pointers are borrowed for the call only.

**CLI11**
- `RawDownloadArgs` is a member of `DownloadCommand`. That command is owned by `run()`'s `commands` vector, declared before the `CLI::App`.
- The callback captures `slot = &selected, self = command.get()` by value (the existing pattern). No locals of `register_download` are bound.

**curl callbacks and userdata**
- `TransferState*` passed as `WRITEDATA`/`HEADERDATA`/`XFERINFODATA` is stack-local to `fetch`, valid only during `curl_easy_perform`. `curl_easy_reset` must run before the handle returns to the pool (otherwise the next user of the handle holds a dangling userdata).
- Callback signatures must exactly match curl's C types (`size_t(char*, size_t, size_t, void*)`, `int(void*, curl_off_t ×4)`); a lambda converted to a function pointer must be captureless.
- Header callback data is not NUL-terminated: use `string_view(ptr, n)` and copy to `std::string` before returning. `size*nitems` uses checked multiplication.
- Varargs types: `long` vs `int`, `curl_off_t` for `_LARGE`. The `curl_slist` is owned by an RAII `SlistPtr`, alive until perform returns. The POSTFIELDS buffer (`request.body`) is not copied by curl and must outlive perform.
- dlsym `reinterpret_cast`s happen in one place. The handle is never dlclosed. The `CurlApi` singleton outlives every transport: `shared_ptr<const CurlApi>` is held by each transport, and `curl_global_cleanup` runs only after the last one is gone.

**Threads and stop tokens**
- `FetchPool` executes the borrowed `PageFetcher`. Its lambdas in `execute_phase` capture explicit pointers to the `ApiClient`, the per-chapter at-home snapshot (mutex-guarded) and the observer-free state; they never capture `[&]`. `run` must not return while any worker still references them (drain before return, including the error and stop paths).
- `FetchedPage` moves across threads only through the mutex-guarded ring.
- `DownloadObserver` is never called from a worker; trace lines are copied into the queue (no views).
- `InterruptBridge` jthread lifetime: it holds a copy of the `stop_source`, and is destroyed before the `ApiClient`/pool it cancels.
- `system_clock()` is a function-local static (thread-safe init).
- The limiter mutex is never held across `sleep_until`.

**Iterators and containers**
- The planner sorts and dedupes by value and index. `PlannedEpub` holds copies of `Chapter`, never pointers or iterators into the feed vector.
- The `FetchPool` ring is indexed modulo the window; no reference to an element is held across an unlock.
- The `TempFile` buffer flushes on write: no span into it escapes.

**Signals**
- The handler body is a single `std::atomic<int>::store(sig, relaxed)` (assert `is_always_lock_free`): no allocation, no errno change, no locks.
- `ScopedInterruptHandlers` restores the previous `sigaction`s on every path, including in-process tests.
- `reraise_pending_interrupt` runs only in `main` after `finish_stdout`.
- `SigpipeGuard` restores the thread mask on every path; workers block SIGPIPE/SIGINT/SIGTERM.

**File descriptors**
- `UniqueFd` is move-only. `TempFile` moved-from has fd −1 and an empty fallback name, so its destructor is a no-op.
- `publish(TempFile&&)` consumes. On failure the TempFile is still destroyed (unlinks the fallback).
- The `/proc/self/fd/N` string is built before `linkat`.
- `O_CLOEXEC` everywhere. The fixture transport closes each intermediate dirfd (RAII) during the component walk.
- `OutputDir`'s fd outlives every TempFile created from it.

**C APIs**
- `openat`/`linkat`/`renameat2`/`fstatat` get `std::string` names (NUL-terminated), never `string_view::data()`.
- `strerror_r` uses the existing GNU/XSI overload trick.

**Size math**
- ZIP offsets accumulate in `uint64_t`, checked against `0xFFFFFFFF`.
- The image sniffer's offset arithmetic is bounds-checked before every read.
- JSON and image caps are enforced in the write callback and the fixture read loop.

---

## Rollout & risk

**CLI contract (additive)**
- New subcommand `download`; `hello` is removed.
- The root help gains exit codes 3–6. 70 and 86 are unchanged.
- No renamed flags. The JSON contract is new (`format_version: 1`).

**⏸ Decisions for the gate.** These are planner calls where the design is silent or I deviated:
1. **libcurl loaded at runtime**: a new user-visible runtime requirement. A missing or too-old libcurl gives **exit 1** (local environment), not 3.
2. Connection refused, DNS failure and TLS verification failure are **not retried** (fast exit 3). Only timeouts, resets, 5xx and 429 are.
3. With `--json`, exit 1 (local abort) prints **no** JSON document, the same as 2/3/4.
4. stdout paths are `DIR/<name>` with DIR as typed. The default prints `./<name>.epub`.
5. Dedupe "group" identity = a chapter's sorted scanlation-group set (joint releases form their own identity).
6. An epub over 65535 zip entries (or a chapter over 5000 pages) becomes a `failed` entry (exit 6), not a run abort.
7. `EDGE_COLOR_ARGS` is empty (stdout is never colored), which unregisters the stdout-color edge cases. Stderr color is covered in `download.cmake`. This needs cli-ux and quality sign-off.
8. Cover choice per volume: `--lang` locale, then the original language, then the smallest file name. Loose and oneshot epubs use the first page as the cover.
9. Interrupt mechanism: the design's flag-only handler plus a 50 ms polling bridge thread into a `stop_source`. The main thread unwinds (TempFile destructors clean up), then `main` re-raises after `finish_stdout`.

**Riskiest task: T11, the dlopen'd libcurl** (packaging, sanitizers, TSan, SIGPIPE). De-risking:
- Run cluster I's probes before approval: a `cxx-probe` that dlopens `libcurl.so.4`, calls `curl_version_info`, then init/cleanup, under the dev and tsan contexts (V-I4, V-I8). Then `readelf -d` on the dev install for RUNPATH, and `objdump -T` of a release probe for GLIBC versions.
- T11's loopback tests exercise the real library under ASan and TSan in batch 3, well before the CLI exists.
- If V-I3, V-I4 or V-I8 is refuted, the fallback is to link `CURL::libcurl` dynamically in dev/test and keep dlopen only in release. Only T0 and `curl_api.cpp` change.

**Second risk: T8 publish semantics on unusual filesystems.** The fallback path is forced in tests through the testing seam. Tests never require NFS; that stays human.

**Migration:** none (first real feature). The user must `git rm` the orphan hello golden and bless 4 goldens over two batches.

**Human-only (feeds the User Test Plan):** acceptance 20–23 (live download, ≥ 2 readers plus epubcheck, Ctrl-C on a real terminal, the static binary on a machine without pixi, which now also means "with the distro's libcurl").

---

## Refactor triggers

| If refuted | Tasks that change |
|---|---|
| V-A1 (`group("")` hides) | T15 (alternative hiding API), T17/T18b goldens |
| V-A2 (vector `--group` swallows positionals) | T15 (switch to `->expected(1)->allow_extra_args(false)` or a callback-append), T18a |
| V-A3 (fallthrough or subcommand footer) | T15, T17, T18b (help golden) |
| V-A4 (Range/IsMember not ParseError) | T15 (manual validation into core), T18a |
| V-A5 (positional ordering, `--no-report`, `-n`) | T15, T18a/b |
| V-A6 (invalid UTF-8 positional) | T15, T17 (edge echo) |
| V-B1 (option versions) | T11 (raise the runtime minimum or make options optional), T16 README |
| V-B2 (vararg types) | T11 wrappers |
| V-B3 (callback semantics) | T11 |
| V-B4 (stall/maxfilesize) | T11 (own stall timer in xferinfo), T7 error mapping |
| V-B5 (thread safety, NOSIGNAL, SIGPIPE) | T11 (handle pool, guard), T8 (worker masks) |
| V-B6 (`curl_easy_reset` scope) | T11 (fresh handle per request instead of a pool) |
| V-B7 (error codes) | T11 mapping, T7 retry table |
| V-B8 (`decltype(&curl_easy_setopt)`) | T11 (`#define CURL_DISABLE_TYPECHECK` in `curl_api.cpp`) |
| V-C1/V-C2 (O_TMPFILE + linkat) | T8 (fallback-only), T14 tests |
| V-C3 (renameat2 in 2.28) | T8 (`syscall(SYS_renameat2)` or linkat+unlinkat only), T0 (glibc floor note) |
| V-C4 (getrandom) | T2, T8, T15 (`/dev/urandom` read) |
| V-C5 (signal semantics) | T8 interrupt design (sigwait thread), T17 main |
| V-C6 (fstatat/mkdirat/openat details) | T8, T7 (fixture walk) |
| V-D1 (jthread/stop_token/cv_any) | T7, T8, T14 (std::thread plus an atomic flag) |
| V-D2 (expected<void>, print) | all core tasks (mechanical) |
| V-D3 (chrono calendar/format) | T9 (publishAt compare), T10 (`format_modified` by hand) |
| V-D4 (from/to_chars) | T4, T9 |
| V-D5 (static libstdc++ glibc symbols) | T0 (release linking), T8 (avoid the offending API) |
| V-E1 (non-throwing parse, UTF-8) | T6 (SAX or try/catch boundary), T9 |
| V-E2 (dump replace) | T13 |
| V-E3 (recursion) | T6 (the depth pre-scan becomes mandatory, already planned) |
| V-E4 (get_ptr, json_fwd) | T6, T13 |
| V-F1 (manga shape, 404) | T6, T2 fixtures, T12 (not-found mapping) |
| V-F2 (feed params, offset cap) | T6 URL builder, T12 pagination, T2 fixtures |
| V-F3 (chapter attributes, isUnavailable) | T6, T9 skips, T2 fixtures, T12 (extra parameter) |
| V-F4 (covers) | T6, T12, T2 |
| V-F5 (at-home shape, URL) | T6, T12, T2, T14 refresh logic |
| V-F6 (rate limits, Retry-After forms) | T7 `parse_retry_after`, T12 intervals |
| V-F7 (MD@H report) | T6 payload, T12 trigger, T15 flag text |
| V-F8 (User-Agent) | T15 |
| V-G1–V-G8 (EPUB/OCF) | T10 (and T2's E2E `EXPECT_EPUB` for G1) |
| V-G7 (WebP core) | T10, T14 (warning text) |
| V-G9 (ZIP format) | T5, T2 (driver checks) |
| V-H1 (find_path/IMPORTED/dl) | T0 |
| V-H2 (`file(ARCHIVE_EXTRACT)`) | T2 (drop extraction; byte-level checks only), T18b |
| V-H3 (two fixtures, release preset) | T2 |
| V-H4 (WORKING_DIRECTORY) | T2, T17 (edge knobs pass `{{WORK}}/out` explicitly) |
| V-H5 (globs with spaces/dots) | T2, T18b |
| V-I1 (conda libcurl contents) | T0 |
| V-I2 (no static TLS libs) | Approach (static linking becomes viable, so re-plan T0/T11) |
| V-I3/V-I4/V-I8 (dlopen binding, RUNPATH, sanitizer behaviour) | T0 + T11 (dynamic link in dev/test, dlopen only in release), T16 |
| V-I5 (CA defaults/paths) | T11 `ca_bundle_candidates`, T16 |
| V-I6 (env vars libcurl reads) | T16 README only |
| V-I7 (runner has libcurl) | T18b (`network-unreachable` gets a label excluded from release-test, or CI installs nothing and the case is documented) |
| V-J1/V-J2/V-J3/V-J5 (warnings, tidy) | T11 (wrapper shape, NOLINT placement), T8 (handler linkage) |
| V-J4 (system-header macro warnings) | T0 (SYSTEM include confirmation), T8, T11 |

---

## Relevant paths
- Design: `/home/quinn/Scripts/Mangadex-dl/docs/design/download-spec.md`
- Build: `/home/quinn/Scripts/Mangadex-dl/CMakeLists.txt`, `/home/quinn/Scripts/Mangadex-dl/pixi.toml`, `/home/quinn/Scripts/Mangadex-dl/.clang-tidy`, `/home/quinn/Scripts/Mangadex-dl/scripts/release-check.sh` (unchanged; NEEDED allowlist already covers libdl)
- Shared headers: `/home/quinn/Scripts/Mangadex-dl/include/mangadex_dl/error.hpp`, `/home/quinn/Scripts/Mangadex-dl/include/mangadex_dl/exit_codes.hpp`
- App seam: `/home/quinn/Scripts/Mangadex-dl/src/app/cli.cpp`, `/home/quinn/Scripts/Mangadex-dl/src/app/main.cpp`, `/home/quinn/Scripts/Mangadex-dl/src/app/command.hpp`, `/home/quinn/Scripts/Mangadex-dl/src/app/io.hpp`
- Test harness: `/home/quinn/Scripts/Mangadex-dl/tests/CMakeLists.txt`, `/home/quinn/Scripts/Mangadex-dl/tests/e2e/CliE2E.cmake`, `/home/quinn/Scripts/Mangadex-dl/tests/e2e/run_case.cmake`, `/home/quinn/Scripts/Mangadex-dl/tests/e2e/cases/_edge.cmake`, `/home/quinn/Scripts/Mangadex-dl/tests/support/io_capture.hpp`
- Goldens affected: `/home/quinn/Scripts/Mangadex-dl/tests/e2e/golden/edge.help.stdout` (changes), `/home/quinn/Scripts/Mangadex-dl/tests/e2e/golden/hello.default.stdout` (removed)
- conda libcurl headers in use: `/home/quinn/Scripts/Mangadex-dl/.pixi/envs/default/include/curl/curl.h`

---

**Amended by `download-plan-rev1.md` (post fact-check), which wins on conflict.**
