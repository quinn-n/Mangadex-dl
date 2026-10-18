# Feature spec: `mangadex-dl download` — MangaDex → EPUB

Status: APPROVED at the design gate, 2026-10-17. Produced by `cli-designer`.

**Gate decisions (user):**
- Remove the `hello` scaffold subcommand (code, tests, E2E case, golden) and re-point the edge knobs to `download`.
- **Keep** ".epub or .pdf" in the app description — PDF output will be added later; design core so an output-format seam is easy to add (no PDF code now).
- Dedupe default: most-covering group (as specified).
- MD@H reporting on by default **with an opt-out flag** `--no-report` ("Don't send image-server reliability reports to MangaDex@Home"). Also off in fixture mode and with a non-default `--api-base`.

**Intent:** one subcommand takes a MangaDex title (UUID or URL) and writes fixed-layout EPUB 3 files. By default: one `.epub` per volume plus one per volume-less ("loose") chapter. `--layout single` turns the whole selection into a single `.epub`.

**Assumptions:** one manga and one translated language per run; anonymous public API only; no PDF output; real network is human-verified, every automated tier is hermetic.

## Command surface (CLI11)

`mangadex-dl download <MANGA> [DIR] [OPTIONS]` — `src/app/cmd_download.{hpp,cpp}` (`register_download`), logic in `<proj>_core`.

| Name | Type / default | Meaning |
|---|---|---|
| `MANGA` | required | Title UUID or `https://mangadex.org/title/<uuid>[/<slug>]` (also `http://`, `www.`, no scheme, trailing `/`, `?q`, `#f`). UUID normalised to lowercase. Else exit 2. |
| `DIR` | optional, `.` | Output dir, created 0755 if missing. If it looks like a manga id → exit 2 ("only one manga per run"). |
| `--volumes SPEC` | `all` | Volumes to download: `all`, `none`, or `1,3-5,8-` |
| `--loose-chapters SPEC` | `all` | Volume-less chapters to download, same grammar |
| `--layout WORD` | `volumes` | `volumes` (per volume + per loose chapter), `chapters` (one per chapter), `single` (everything in one file) |
| `-l,--lang CODE` | `en` | Translated language (`^[a-z]{2,3}(-[a-z]{2,4})?$`) |
| `--group NAME\|UUID` | repeatable | Preferred scanlation group(s), priority order |
| `--data-saver` | flag | Compressed images |
| `--direction WORD` | `auto` | `auto` (rtl for `ja`/`ja-ro` originals, else ltr), `rtl`, `ltr` |
| `--title TEXT` | — | Override title for file names and metadata |
| `--overwrite` | flag | Replace existing files instead of skipping |
| `-n,--dry-run` | flag | List files that would be written; download nothing, write nothing |
| `-j,--jobs N` | `4`, range 1–8 | Parallel image downloads |
| `--api-base URL` | hidden | `https://…`, `http://` loopback only, or `file://<abs dir>` (fixture mode). Non-default disables at-home reporting. |

Semantic usage errors (exit 2): both selections `none`; malformed SPEC; bad `--lang`/`--api-base`/`--title`; `DIR` looks like an id.

Selection grammar: `SPEC := all | none | ITEM(,ITEM)*`, `ITEM := N | N-M | N- | -M`, N decimal with optional fraction, inclusive, no whitespace, N>M is an error. Non-numeric labels match only `all`. No chapter filter inside volumes (out of scope).

Global flags (`--json`, `-q`, `-v`/`-vv`, `--color`) via `fallthrough()`. No prompts. Help footer has examples; root exit table gains 3–6 (re-bless `edge.help`).

## Input & output

- **Precedence:** command line > default only. No config file, no env vars, no `envname()`. Library-read env (proxy, `SSL_CERT_FILE`) documented `[VERIFY]`.
- **stdin:** never read.
- **stdout:** one path per written file, streamed (dry-run: files that would be written; existing skipped files omitted). `--json`: one document at the end:
  ```
  format_version:1, dry_run, layout, manga{id,title,url,language,direction},
  epubs[{path,kind,volume,chapter,status(planned|written|exists|failed),replaced,pages,bytes,error,
         chapters[{id,volume,chapter,title,groups[{id,name}],pages}]}],
  skipped_chapters[{id,volume,chapter,reason(external|no_pages|unavailable|duplicate),kept}],
  summary{planned,written,exists,failed}
  ```
  Dumped with `error_handler_t::replace`, never ANSI. Exits 2/3/4 print no stdout; exit 5 prints `epubs: []`; exit 6 prints failed entries.
- **stderr:** `mangadex-dl: …`; warnings `warning:` (skipped chapters, WebP, "N files already exist"); `-q` errors only; `-v` per-file + dedupe choices; `-vv` per-request.
- **Progress:** stderr TTY only, not with `-q`/`--json`; one `\r`-rewritten line `[3/12] Vol. 03: 45/180 pages`, no cursor escapes; cleared before warnings.
- **Color:** stdout never; stderr `warning:` yellow / `error:` red via `resolve_color`.
- **Locale:** no `L` specs; `from_chars`/`to_chars`; ASCII case folding.

## Exit codes

| Code | Name | Meaning |
|---|---|---|
| 0 | success | Every selected file written / existed / planned |
| 1 | failure | Local fatal (DIR unusable, EACCES, EROFS, ENOSPC), stdout write error — aborts run |
| 2 | usage | ParseError and semantic usage errors |
| 3 | network | Plan could not be built (connection/TLS/timeout/5xx/429 exhausted/malformed or oversized API response) |
| 4 | not_found | Title UUID does not exist |
| 5 | no_chapters | Title exists, nothing downloadable matched language+selection |
| 6 | incomplete | At least one file failed; others may be written |
| 70 | internal | — |

Priority: 1 aborts immediately; else 6 if any failed, 5 if plan empty, else 0. "All already exist" = 0, empty stdout.

## Behaviour (all API/EPUB claims `[VERIFY]`)

- **Title:** `GET /manga/{id}?includes[]=cover_art&includes[]=author&includes[]=artist`; title = `title.en` → first `altTitles` in `--lang` → first `title` value. `originalLanguage` drives direction.
- **Feed:** `GET /manga/{id}/feed`, `translatedLanguage[]`, `limit=500`, `offset`, `order[volume]=asc&order[chapter]=asc`, `includes[]=scanlation_group`, all `contentRating[]`, `includeFuturePublishAt=0`, `includeUnavailable=1`; `includeExternalUrl` and `includeEmptyPages` are **omitted** (2026-10-18, observed on the live API: they are filters, `=1` returns *only* external / *only* empty-page chapters, `=0` excludes them, omitted = no filter; omitting them lets such chapters reach the planner, which reports them as `external` / `no_pages` skips). Offset window cap (≈10000) → exit 3 if exceeded, never truncate. Client re-filters language and sorts itself. Aggregate endpoint not used.
- **Covers:** `GET /cover?manga[]=<id>&limit=100` (paginated), per-volume match, `https://uploads.mangadex.org/covers/<id>/<file>`.
- **Skips:** `externalUrl` → external; `pages==0` → no_pages; `isUnavailable` → unavailable. Warning + JSON entry each.
- **Dedupe** (key = volume + normalised chapter; null chapter never deduped): first matching `--group` → group covering most keys in selection → more pages → earlier `publishAt` → smallest UUID.
- **Ordering:** numeric, then non-numeric (bytewise), `none` volume last; `single`: volume then chapter, loose last.
- **At-home:** `GET /at-home/server/{chapterId}` right before the chapter; `{baseUrl}/{data|data-saver}/{hash}/{file}`; refresh ≤3× on repeated failure. Validate `baseUrl` https, no userinfo/query/fragment; hash/filenames `^[A-Za-z0-9][A-Za-z0-9._-]*$`.
- **Rate limits:** shared API limiter ≤4 req/s; separate at-home limiter (≈40/min). 429 honours `Retry-After`/`X-RateLimit-Retry-After`, cap 60 s. 5xx/timeouts: 4 attempts, 1/2/4 s + jitter. Connect timeout 15 s, stall abort 30 s.
- **User-Agent** `mangadex-dl/<version>`. **MD@H report** best-effort POST to `https://api.mangadex.network/report` for non-mangadex.org nodes. TLS always verified.
- **EPUB:** `mimetype` first STORED no extra field; `META-INF/container.xml` → `OEBPS/content.opf` (3.0); `nav.xhtml`; `toc.ncx`; one XHTML per page with viewport W×H from image header. Metadata: stable `dc:identifier` `urn:mangadex:<id>:<lang>:<unit>`, title `<T> Vol. 3`/`Ch. 12`, language, creators, contributors (groups), source, `dcterms:modified` = latest chapter `updatedAt`, `belongs-to-collection` series + position. `rendition:layout=pre-paginated`, `spread=landscape`, `orientation=auto`; spine `page-progression-direction`. Media type from magic bytes (JPEG/PNG/GIF/WebP; WebP warns); other → chapter fails. Cover item `cover-image` + first spine page (fallback to first page). Nav: chapter entries; `single` nests chapters under volumes. XML escaping drops illegal chars. No ZIP64: >65535 entries fails at plan time; >4 GiB fails. Fixed timestamps; byte-identical output for any `--jobs`.
- **Filenames:** `<T> - Vol. 03.epub`, `<T> - Ch. 012.epub` (`012.5`), chapters layout `<T> - Vol. 03 Ch. 021.epub`, `<T> - Oneshot [<8 hex>].epub`, single `<T>.epub`. Sanitise: controls/DEL/`/\<>:"|?*`→`_`, drop bidi + U+2028/9, collapse whitespace, trim spaces/dots/leading `-`, ≤150 bytes on UTF-8 boundary, empty/`.`/`..` → UUID; component ≤255 bytes; joined via DIR fd.
- **Failures:** pre-plan network → 3; unknown title → 4; per-file failure → continue, exit 6; local FS failure → remove temp, abort 1.
- **Interruption:** SIGINT/SIGTERM → never a partial final `.epub`, no temp left (prefer `O_TMPFILE` + `linkat`, fallback `mkostemp(".<final>.part-XXXXXX")`); flag-only handler, main thread cleans, restores `SIG_DFL`, re-raises. SIGPIPE/SIGKILL may leave `.part-` on fallback FS (documented).
- **No-clobber:** pre-download `fstatat(AT_SYMLINK_NOFOLLOW)`; publish via `linkat`/`renameat2(RENAME_NOREPLACE)`, EEXIST → `exists`.
- **Limits:** API JSON ≤16 MiB, image ≤32 MiB, ≤5000 pages/chapter, image sides ≤65535; JSON parsed non-throwing, malformed → 3. Memory ≈ jobs × image cap.
- **Fixture mode** (`--api-base file:///abs`): `<root>/api/<path>[/offset-<n>].json`, `<root>/uploads/covers/<id>/<file>`, `<root>/at-home/<data|data-saver>/<hash>/<file>`; `openat`+`O_NOFOLLOW`; missing = 404; no sleeps, no reporting.

## Data & state

Writes only `DIR/<name>.epub` (0644) and its temp. No config/cache/XDG/TMPDIR. Atomic publish: write → fsync → close-check → link/rename → optional dir fsync. Worker pool of `--jobs` threads; library global init once before threads; `tsan` covers the downloader.

Dependencies (planner decides, conda-forge, `find_package CONFIG`): HTTPS client (libcurl? CA-bundle discovery for static release binary is the biggest packaging risk), zip writer (in-house STORED-only recommended), nlohmann_json.

## Acceptance criteria

1. `[unit]` MANGA parsing (accept/reject set above).
2. `[unit]` Selection grammar accept/reject; numeric matching.
3. `[unit]` Planning from canned feed: loose/oneshot, skips, language filter, ordering, three layouts.
4. `[unit]` Dedupe rules 1–5 deterministic.
5. `[unit]` Filename sanitising.
6. `[unit]` EPUB generation structure + metadata + nav/NCX + XML escaping.
7. `[unit]` Image sniffer types/dimensions, bounds-safe on truncated input.
8. `[unit]` Retry/rate limiting with fake clock.
9. `[unit]` At-home URL/hash/filename validation.
10. `[cli]` Fixture-mode run writes expected files; zip structure verified; stdout = written paths; exit 0.
11. `[cli]` Re-run skips (exit 0, empty stdout, notice); `--overwrite` byte-identical; `-j1` vs `-j8` byte-identical.
12. `[cli]` `--json` schema for success, dry-run, exit 5, exit 6; no ANSI with `--color=always`.
13. `[cli]` Semantic usage errors → 2, empty stdout, hint.
14. `[e2e]` Installed-binary fixture success; `--dry-run` writes nothing.
15. `[e2e]` Exit 4 / 5 / 6 / 3 (`http://127.0.0.1:1`) / 1 (DIR is a file).
16. `[e2e]` `--dry-run > /dev/full` → 1 + `write error`.
17. `[edge]` Edge knobs re-pointed to fixture-mode `download --dry-run` (bulk >64 KiB, json, echo via DIR).
18. `[tty-pty]` Progress line under pty; none with `--json`/`-q`/`TERM=dumb`; `NO_COLOR` honoured.
19. `[e2e]` `--help` lists 0–6, 70, hides `--api-base`; `download --help` examples (re-bless).
20. `[human]` Real live download: grouping, dedupe, covers, no 429 storm, readable progress.
21. `[human]` EPUBs open in ≥2 readers; fixed layout, RTL, TOC, cover, series; `epubcheck` passes.
22. `[human]` Ctrl-C mid-download: 130, no partial/temp files, re-run resumes.
23. `[human]` Static release binary does HTTPS on a machine without the pixi env.

## Out of scope

PDF; multiple manga; search, chapter URLs, login; chapter filter inside volumes; mixed languages; transcoding; webtoon layouts; Kindle; ZIP64; filename templates beyond `--title`; config/env defaults; resume within file; stale `.part-` cleanup; completions/man.

## Errata (accepted during implementation)
- MANGA and `--title` values are **never echoed** in diagnostics (closes terminal-escape injection); the MANGA error is the fixed text `MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL`. Selection items are echoed only sanitised (non-printable → `?`, ≤ 32 bytes + `...`). (T4 review, 2026-10-17.)
- Extra usage error: empty `DIR` → exit 2 `DIR must not be empty`.
