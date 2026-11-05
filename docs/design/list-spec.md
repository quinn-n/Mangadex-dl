# Feature spec: `mangadex-dl mdlist`: downloading every title in a public MangaDex list

Status: DRAFT 2026-10-26. Design gate decided Q1 `mdlist`, Q2 reuse exit 4, Q3 flat by default plus an opt-in `--dir-per-title`. Produced by `cli-designer`.
rev1 (fact-check + amendment A1) 2026-10-26:
- V1–V6 results are folded in: V2–V6 CONFIRMED, V1 UNRESOLVED with strong indirect evidence for 404.
- Amendment **A1** adds `--dir-per-title`, specified in "Per-title subdirectories" and marked **A1** where other sections change.
- No exit code was added. `batch` and `download` output are unchanged.

rev2 (feature-board fixes, chosen by the user) 2026-10-27. These are message changes only, all `mdlist`-only, with `batch` and `download` byte-identical. The quotes below are updated to match:
- Flat-mode same-name clash: the collision line names `--dir-per-title`. It replaces the engine's "left alone: holds a different book" warning for a path an earlier item already claimed, so each file gets one line.
- The A1 unusable-title-directory error ends with `; rename or remove it, or run without --dir-per-title`.
- In the `-v` list line, `\` and `"` in the name are escaped after the 64-byte cut.
- The list-level not-found message shows the status received: `(HTTP 404)`, or `(HTTP 401)`/`(HTTP 403)` for an error body that names the CustomList.

rev3 (bug fix, chosen by the user) 2026-10-27, `mdlist`-only, with `batch` and `download` byte-identical (`batch` gets the same fix later, separately):
- Flat mode with `--overwrite`: a path an earlier item of the same run claimed (written, planned or left alone as `exists`) is never replaced. The later item leaves it alone exactly as without `--overwrite` (status `exists`, existing `foreign`, only the collision line), in a dry run too, so stdout lists each path once. `--overwrite` still replaces files from earlier runs and every path not claimed in this run. The engine asks the run's `OutputRegistry` through `DownloadServices::claimed` before `--overwrite` replaces a file.

rev4 (dry-run parity, with `batch`) 2026-10-28:
- Flat mode `--dry-run` without `--overwrite`: a file an earlier item of the same run reported as planned counts as written, so a later item whose file has that path (another title with the same name) is reported exactly as the real run will report it: status `exists`, existing `foreign`, only the collision line. stdout lists each path once, `--json` carries those statuses (no new keys), `summary.files` moves with them, and exit codes equal the real run's.
- In every dry run (`--overwrite` too) the collision line reads "would already be written by item 2" instead of "was already written by item 2". Real runs are byte-identical. `--overwrite --dry-run` is otherwise unchanged (rev3).
- A `--dry-run` without `--overwrite` (flat or `--dir-per-title`) keeps each planned file's chapter list in memory for the run, roughly 60 KB per typical title (about what `--json` already keeps).

Related: [`batch-spec.md`](batch-spec.md) (its engine is reused unchanged), [`download-spec.md`](download-spec.md),
[`skip-existing-spec.md`](skip-existing-spec.md) (re-runs).

**Intent:** fetch a public MangaDex custom list (MDList) without logging in. Then run every title in it through the `batch` engine, in the list's order and with one set of options. That engine handles planning, running, stop rules, reporting, exit-code aggregation and the `--json` document. A list is a batch without a header row: its rows come from `GET /list/{id}` instead of from a file.

**Who runs it:** mostly a person at a terminal who has copied the list's URL from the browser. Next most often a cron job or script that runs the same list again to pick up new chapters and new titles.

**Why now:** reading your follows needs OAuth, because `GET /user/follows/manga` returns 401 to anonymous requests. Step 1 (this spec): put your follows into a public MDList and download it with no credentials. Step 2 (later, not designed here): login, then reading follows directly. "How `follows` fits in later" below shows that step 2 is purely additive.

**Assumptions:**
- One list per run and one output root (DIR). There are no per-title options: a list carries only titles, so every title gets the command-line options. By default every title goes flat into DIR. **A1:** `--dir-per-title` puts each title into its own subdirectory.
- Titles run one after another. `--jobs` still parallelises the images within each title. No new threads.
- No config file and no env-var options, as today. Precedence is "command line > built-in default" only. CLI11's order (CLI > config > env > default) has nothing to arbitrate. The later `follows` spec will need a precedence statement for credentials; this one does not.
- `GET /list/{id}` returns every title in one response. This is confirmed for lists of up to 121 titles (the largest found). There is no pagination parameter (`limit`/`offset` are silently ignored) and no documented cap (V2).
- Remaining API assumptions are marked `[VERIFY]`; V1 and the new V11 are still open.

## Decision 1: a new subcommand `mdlist LIST [DIR]`

`mangadex-dl mdlist [OPTIONS] LIST [DIR]` is a top-level subcommand.

1. **`batch FILE` keeps its meaning.** FILE is a path or `-`. Letting it also accept a list URL would overload a positional that scripts already depend on. It also can't work for the most common input, a bare UUID. That is ambiguous between a title and a list, and `batch` already rejects it (exit 2 "FILE looks like a MangaDex title").
2. **No `batch --list <id>` flag.** FILE would have to become optional. Then `batch --list X ~/Manga` binds `~/Manga` to FILE and DIR falls back to `.`. That is the same positional trap `batch-spec.md` gave as the reason for rejecting `download --from-csv`. `--list` and FILE would also need `excludes`, and the batch synopsis and help golden would change.
3. **Discoverability.** A subcommand gets its own one-liner in the root help, its own `--help` footer (the LIST forms, the "must be public" rule, the follows tip) and its own hint line on usage errors.
4. **The name is `mdlist`, not `list`.** In CLI convention `list` reads as a verb ("enumerate", as in `pip list` or `gh repo list`). People would expect `mangadex-dl list` to print something, and it would take a name a future read-only command may want. `MDList` is MangaDex's own name for these lists (`[VERIFY]` V5) and has nothing to collide with. The positional is still called `LIST`. The name is gate question Q1.
5. **Room for `follows`.** A sibling subcommand `follows [DIR]` takes the same engine path, and its auth options live only on it (details below).

Rejected alternatives: `batch FILE` with a URL (see 1), `batch --list` (see 2), a nested group such as `from list <id>` / `from follows` (deeper help, longer to type, and today it would be a group with one member), `list` (see 4).

`download` and `batch` change only in help-footer pointers and in two exit-2 messages that teach the new command (see "Command surface").

## Command surface (CLI11)

- **Files:**
  - App: `src/app/cmd_mdlist.{hpp,cpp}` (`register_mdlist`), plus one line in the `cli.cpp` registration list (a serialization point).
  - Core: `include/mangadex_dl/mdlist.hpp` and `src/core/mdlist.cpp`, covering `ListId`, the list parser, the request URL and `plan_list`. Details are under "Engine reuse".
- **Positionals:**
  - `LIST` (required). A list UUID or URL. It is not a `CLI::ExistingFile`. It is validated after parse by `ListId::parse` (see "LIST forms").
  - `DIR` (optional). Default `.`. Created if missing, except with `--dry-run`, as in `batch`.
- **Options.** These are the run-level values for every title. They are bound by the same helpers `batch` uses (`bind_selection_options`, `bind_run_options` in `src/app/cmd_download_options.cpp`), so the help strings are byte-identical:
  - `--volumes`, `--loose-chapters`, `--layout`, `-l,--lang`, `--group` (repeatable), `--direction`, `--data-saver`, `--overwrite`, `-n,--dry-run`, `-j,--jobs`, `--no-report`, and the hidden `--api-base`;
  - `--fail-fast` ("Stop at the first title that fails"), as in `batch`;
  - **A1:** `--dir-per-title`, a flag (default off): "Put each title in a subdirectory of DIR" (39 columns). It is bound to a member of mdlist's own raw args, not to `RawDownloadArgs`, so `download` and `batch` never see it. There is no short form, no `envname` and no value. See "Per-title subdirectories".
- **No `--title`.** One title for every list item would make the file names collide. `mdlist LIST --title X` (also `--title=X`) exits 2 with `mdlist has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title ...'` plus the hint line. This generalises `batch_got_title` in `cli.cpp` to `batch` or `mdlist`.
- **Global flags:** `--json`, `-q`, `-v`, `--color` work anywhere, also after LIST and DIR. They reach the subcommand through `fallthrough()`, which `run()` already sets before any `add_subcommand`. `--version` has no `-v` alias, as today.
- **No prompts.** Nothing is destructive beyond what `download` already does, and stdin is never read.
- **Semantic usage checks after parse.** Each exits 2, prints nothing on stdout, opens nothing and sends no request, and ends with `Run 'mangadex-dl mdlist --help' for more information.`. LIST is never echoed, like MANGA.
  - `LIST must not be empty`.
  - LIST is a title URL: `LIST is a MangaDex title, not a list; use 'mangadex-dl download <title>' for one title`.
  - LIST is anything else that is not a list form (a CSV path, `-`, junk): `LIST must be a MangaDex list UUID or a https://mangadex.org/list/<uuid> URL; for a CSV file of titles use 'mangadex-dl batch <file>'`.
  - `DIR must not be empty`.
  - DIR looks like a title or list id: `DIR looks like a MangaDex id; give one list per run`.
  - Every CLI-level option is validated exactly as `batch` validates it (`detail::validate_common` with a nil MangaId).
- **Root help and errors (stderr only):** `unknown command 'x' (commands: download, batch, mdlist)`. Root `--help` lists `mdlist` with its one-liner.
- **Small changes elsewhere** (exit code unchanged or 1→2; the text teaches the new command):
  - `download <list URL>` still exits 2, but the message becomes `MANGA is a MangaDex list, not a title; use 'mangadex-dl mdlist <list>' to download every title in it`. This applies only when MANGA parses as a list URL. Every other bad MANGA keeps the fixed text.
  - `batch <list URL>` exits 2 (today it is exit 1 "cannot read"), with `FILE looks like a MangaDex list; use 'mangadex-dl mdlist <list>'`. This is a small contract change for a nonsensical input, the same treatment `batch` already gives a title URL. A bare UUID keeps today's "looks like a MangaDex title" message.
  - `download --help` footer: `For many titles at once, see 'mangadex-dl batch --help' (a CSV file)` / `or 'mangadex-dl mdlist --help' (a MangaDex list).`
  - `batch --help` footer gains one line before "Examples:": `For a MangaDex list (MDList) instead of a file, see 'mangadex-dl mdlist --help'.`

### Help (`mangadex-dl mdlist --help`), a draft for cli-ux

Lines fit 80 columns and right-column texts fit 44. The options block is `batch`'s, byte for byte, plus one line.

```
Download every title in a public MangaDex list (MDList)


mangadex-dl mdlist [OPTIONS] LIST [DIR]


POSITIONALS:
  LIST REQUIRED                     Public MDList: its UUID or list URL
  DIR [.]                           Output directory (created if missing)

OPTIONS:
  ... (exactly as batch, including --fail-fast) ...
      --dir-per-title               Put each title in a subdirectory of DIR

LIST:
  A public MangaDex custom list (MDList): its UUID, or its address on the
  website, https://mangadex.org/list/<uuid>/<name> (http://, www., no
  scheme or a trailing / are fine). No login is used, so the list must be
  public. Every title in it is downloaded into DIR, one after another in
  the list's order, with the options above. With --dir-per-title, each
  title goes into its own subdirectory, named like its files:
  DIR/<title>/<title> - Vol. 01.epub.

  To download the titles you follow, add them to a public MDList.

Examples:
  Every title in a list, into ~/Manga:
    mangadex-dl mdlist https://mangadex.org/list/<uuid> ~/Manga
  The same, one folder per title:
    mangadex-dl mdlist <uuid> ~/Manga --dir-per-title
  Show what would be written, without downloading anything:
    mangadex-dl mdlist <uuid> ~/Manga --dry-run
  Volumes 1 to 3 of each title, in French, as one JSON document:
    mangadex-dl mdlist <uuid> ~/Manga --volumes 1-3 --lang fr --json

Running the same command again skips files that already hold every chapter
and rebuilds a file when MangaDex has added chapters to it, downloading only
the new ones; titles added to the list since the last run are downloaded too.

Symlinks inside DIR are followed: don't point DIR at a directory that others
can write to. Each title costs several API requests: a long list is a long run.

Exit status:
  ... (exit_status_footer(), rows 4 and 7 reworded, see "Exit codes") ...
```

## LIST forms

`ListId` is a strong type: a canonical lowercase 8-4-4-4-12 UUID, built only by `[[nodiscard]] static Result<ListId> parse(std::string_view)`. Its rules mirror `MangaId::parse` exactly, with `/list/` in place of `/title/`:

- Bare UUID in any hex case, lowercased.
- `https://`, `http://` or no scheme; optional `www.`; host `mangadex.org` (case-insensitive); then `/list/<uuid>`. After that, nothing, `/`, `/<slug>`, `/<slug>/`, each optionally followed by `?…` or `#…`. A second path segment after the slug is rejected.
- **What the website produces (V5, confirmed):**
  - `https://mangadex.org/list/<id>/<slugify(name)>`.
  - The share controls add no `?` or `#`.
  - A list whose name has no Latin characters gets an **empty slug, so the URL ends in `/list/<id>/`**. That trailing-slash form must be accepted, and is tested (AC1, AC17).
  - `?`/`#` stay accepted for leniency (hand-edited URLs) but are no longer mentioned in `--help`.
  - The UI calls these "MDList(s)".
- **The id is always sent lowercased.** V6: an upper-case id in the request URL is a hard 404 from the router ("No route found"), not the CustomList not-found message. `ListId`'s canonical form guarantees the lowercase id, so `LIST` typed in upper case still works (AC5, AC17).
- Rejected:
  - another host (`mangadex.org.evil`), a port, userinfo-like tails;
  - space, C0 control or DEL bytes;
  - `/list//`, a wrong UUID length;
  - `-` and the empty string.
  - `/title/<uuid>` is rejected with its own "is a title" message.
- **The bare-UUID ambiguity is unavoidable.** A bare title UUID is a valid `ListId` spelling. The API then answers 404, and the not-found message carries the `download` hint (see "Exit codes").
- **Implementation suggestion** (the planner decides): move `extract_uuid`'s URL walk from `src/core/manga_id.cpp` into a private core helper, `src/core/mangadex_url.hpp`, parameterised by the path (`/title/` or `/list/`). Both `parse` functions call it. `MangaId`'s behaviour and its existing unit tests stay unchanged. `looks_like_list_id(text)` is used for the DIR check and for the new `batch`/`download` hints.

## Input & output

- **Precedence:** command line > built-in default. There is no config file and no `envname`, so CLI11's CLI > config > env > default has nothing to arbitrate.
- **stdin:** never read. A closed stdin or `/dev/null` changes nothing.
- **stdout (human):** exactly `download`'s data. One path per written (or, with `--dry-run`, planned) file, concatenated across titles in list order. **A1:** with `--dir-per-title` the paths include the title directory (`out/Clean Title/Clean Title - Vol. 01.epub`). A full success prints nothing on stderr.
- **stderr:**
  - **Row-scoped lines** keep `batch`'s `<source>:<n>: ` shape after `error:`/`warning:`. The source is `list`, `<n>` is the 1-based position in the list, and the item's validated manga UUID follows. For example:
    `mangadex-dl: error: list:3: 66666666-6666-4666-8666-666666666666: title 66666666-… not found on MangaDex (HTTP 404); check the UUID or URL`.
    The UUID is in the prefix because the user wrote no file to look the position up in, and many engine messages (exit 5, exit 6, local errors) don't name the title. It is canonical and validated, so echoing it is safe. Engine messages stay byte-identical to `download`'s, redundancy included.
  - **List-level lines** have no item prefix: `mangadex-dl: error: list <list-uuid> not found on MangaDex …`.
  - **Wording that names a row** uses "item" where `batch` says "line". Every `batch` string stays byte-identical.
    - Duplicate: `list:5: <uuid>: same as item 2; skipped`.
    - Cross-item collision (flat mode): `list:4: <uuid>: <path> was already written by item 2 (another title with the same name); pass --dir-per-title to give each title its own directory, or download one of them on its own with 'mangadex-dl download <title> --title ...'` (rev2: it replaces the engine's "left alone: holds a different book" line for that file). rev4: in a dry run it reads `list:<n>: <uuid>: <path> would already be written by item <k> (another title with the same name); pass --dir-per-title to give each title its own directory, or download one of them on its own with 'mangadex-dl download <title> --title ...'`.
    - **A1**, directory name taken (with `--dir-per-title`; a warning, not shown with `-q`): `list:4: <uuid>: directory '<name>' already holds item 2 (a title with the same name); using '<name> [1a2b3c4d]'`.
    - **A1**, the title's own directory is unusable (an error; the item fails alone): `list:4: <uuid>: cannot open directory '<DIR>/<name>': Not a directory; rename or remove it, or run without --dir-per-title`. This is `check_row_dir`'s existing wording plus the rev2 remedy.
    - Closing line, one only, even with `-q`/`--json`; the item list is capped at 10 as in `batch`:
      - `2 of 15 titles did not complete (items 4, 9); run the same command again to retry them`, or `…; see the errors above`, with the same retry rule as `batch`;
      - dry run: `2 of 15 titles would fail (items 4, 9)`;
      - stopped early: `stopped at item 9: <reason>; 6 titles not run`, with `batch`'s reasons.
  - **The up-to-date note** is printed once at the end, as in `batch`.
  - **Empty list:** `mangadex-dl: warning: list <list-uuid> has no titles`. Not shown with `-q`.
- **`-v`:**
  - After the fetch: `mangadex-dl: list <list-uuid> "<name>": <n> titles`. Without a name: `list <list-uuid>: <n> titles`. The name is capped at 64 bytes on a UTF-8 boundary, then `...`; then `\` and `"` are escaped (rev2); then the line passes through `terminal_safe`.
  - At each item: `list:<n>: <manga-uuid>`, then the engine's own lines.
  - **A1:** with `--dir-per-title`, after the title's plan: `mangadex-dl: output directory: <DIR>/<name>`. It is an engine verbose line, so it has no prefix and passes through `terminal_safe`.
  - `-vv` adds the request trace, including `GET <api>/list/<id> -> 200 (… ms)`.
- **Progress:** TTY stderr only, through the existing `output_policy`: `(2/15) [1/2] Vol. 1: 3/5 pages`. There is no progress line for the list fetch, which is one request.
- **`--json`:** one document at the end. It is printed only when the items ran and were not interrupted, including exits 1-stopped-early and 3–7. It is never printed for usage errors, list-level failures or interrupts, the same rule as `batch`'s phase 1. It is `ordered_json`, indent 2, `error_handler_t::replace`, never ANSI.
  - **It is `batch`'s document**, the same keys in the same order with the same types, **plus one additive key `list`** after `source`. `format_version` stays **1**: no key is renamed, removed or given a new type. `batch` documents are byte-identical to today's and never carry `list`. A consumer written for `batch` reads an `mdlist` document unchanged.
    ```
    format_version: 1
    dry_run: bool
    source: string                 LIST as typed (like batch's FILE as typed)
    list: {                        mdlist only
        id: string                 canonical lowercase list UUID
        name: string|null          as MangaDex returns it (cut to 1024 bytes on a UTF-8 boundary)
        url: string                "https://mangadex.org/list/<id>" (the website, also in fixture mode)
    }
    titles: [ {
        line: int                  for mdlist: the 1-based position in the list (API order)
        manga_id, status, exit_code, error, duplicate_of, report   as batch
                                   (duplicate_of is a position)
        dir: string                the directory the title was (or would be) written to, as typed:
                                   DIR; with --dir-per-title DIR/<name> (A1), or DIR when the
                                   title's name was never learned (exit 3/4 before its plan,
                                   duplicate, not_run)
    } ]
    summary: { titles: {...}, files: {...} }   as batch
    ```
  - **On `line`:** for `mdlist` it holds the list position, and the README documents it as "the title's place in its source: the CSV line for `batch`, the list position for `mdlist`". Rejected alternative: `line: null` plus a new `position` key. That would change `line`'s type to `int|null` inside the shared shape, so a consumer written for `batch` would break on an `mdlist` document, and every row would carry two keys for one fact.
  - **A1:** no key is added for the flag. `dir` already has `batch`'s meaning ("effective output directory, as typed"), and `report.epubs[].path` already carries the full path.
  - The empty list prints `titles: []`, with the `list` object filled in.
- **Color:** stderr prefixes only, through the existing `resolve_color`. stdout is never colored.
- **Locale:** output is locale-independent; positions are formatted with `std::format` without `L`.

## Exit codes

**No new code** (gate decision Q2). List-level outcomes reuse codes whose meaning fits. Item outcomes aggregate exactly as in `batch`.

| Outcome | Code |
|---|---|
| every item ok or duplicate, or a list with no titles | 0 |
| backend unusable (no usable libcurl); a global local error in an item (DIR unusable, ENOSPC, EIO, EROFS) stops the run, rest `not_run`; a failed write to stdout | 1 |
| any `ParseError`; LIST empty, a title URL or not a list form; DIR empty or looks like an id; bad option values; `--title` | 2 |
| the list fetch failed: connection, DNS, TLS, timeout, 5xx/429 retries exhausted, a 403 rate-limit ban; or the response is malformed, over 16 MiB, nested too deep, over the separator cap, has the wrong `data.id`/`data.type`, or holds a manga relationship whose id is not a UUID; or the list has more than 10 000 titles | 3 |
| the list does not exist, or is private (`GET /list/{id}` answered 404) | 4 |
| items: every non-ok item failed the same way | that code (1, 3, 4, 5 or 6) |
| items: non-ok items failed in more than one way | 7 |
| SIGINT / SIGTERM (also during the list fetch) | 130 / 143 (re-raised by `main`) |

- **A1:** an item whose own `--dir-per-title` directory is unusable is `failed` (`exit_code` 1) and the run continues, like `batch`'s row-scoped `dir` failure. It counts as 1 in the aggregate (failed + 4 → 7). It stops nothing. A problem with DIR itself still stops the run (exit 1).
- **Exit 4 message** (rev2: the parenthesis shows the status received, `(HTTP 404)`, or `(HTTP 401)`/`(HTTP 403)` for a CustomList error body): `list <list-uuid> not found on MangaDex (HTTP 404): it does not exist or is private; check the id, make the list public on MangaDex, or use 'mangadex-dl download' if it is a title`.
  - A non-existent id is confirmed (live) to answer `404 {"result":"error","errors":[{"id":…,"status":404,"title":"not_found_http_exception","detail":"CustomList `<id>` not found.","context":null}]}`.
  - The mapping uses only the status, never the body text.
- **Over the title cap:** `list <list-uuid> has more than 10000 titles; refusing to truncate it; split it into smaller lists`. Exit 3 follows the precedent of `download`'s "more than 10000 chapters; refusing to truncate" (an oversized API response). `with_remedy` must **not** append the network-connection remedy here (`[VERIFY]` V8).
- **Telling list-level from aggregate 3/4 apart:** list-level failures print no JSON and their stderr line has no `list:<n>:` prefix. An aggregate 4 ("every item's title is gone") prints the JSON document.
- **Private lists (V1, unresolved, strong indirect evidence):**
  - An anonymous request for a private list is **expected to answer 404**. The API spec documents only 200 and 404 for `GET /list/{id}`, and the website's frontend retries with auth only after a 404. So the message says "does not exist or is private".
  - Fallback, kept in case that is wrong: a 401 or 403 whose JSON error body names the CustomList maps to 4 with the same message.
  - Any other 403 stays 3: a rate-limit ban is a generic 403.
  - AC32 (live, human) is what closes V1.
- **Rejected:** a new code 8 `list_unavailable`. Scripts can already tell the cases apart (above). "Check the id / make it public" is the same remedy class as a title's 4. The 3–63 space is better kept for the auth failures step 2 may need.
- **Help footer rows** (every help golden changes; 80 columns):
  - `4   title or list not found on MangaDex (a private list counts)`
  - `7   batch/mdlist: titles failed in more than one way (see per-title results)`
  - Update the `exit_codes.hpp` doc comments for `not_found` and `mixed_failures`, and the README rows 1, 3, 4 and 7.

## Behaviour

- **Phases:**
  1. Validate CLI arguments (pure, exit 2). No fd is opened and no request is sent.
  2. Install `ScopedInterruptHandlers`, then `make_backend` (exit 1 on failure, e.g. no libcurl).
  3. Fetch the list: one `ApiClient` over the backend with `endpoints_for(api_base, report = false)`. Its stop token comes from a `std::stop_source` wired to an `InterruptBridge`. The client and the bridge are destroyed before item 1 starts.
  4. `plan_list` (pure).
  5. Empty list → warning, JSON if asked, exit 0. Nothing else is touched, and DIR is not created.
  6. Run the items through the shared batch runner (unchanged `run_batch` and the extracted `ConsoleTitleRunner`), over the same `Backend`.
  7. Note, closing line, JSON, exit code, as in `batch`.
- **List request:**
  - `GET <api>/list/<id>` with the canonical lowercase id (V6: upper case is a router 404), no query, no includes.
  - V6 confirmed: `includes[]` is accepted but only bloats the response; `contentRating`/`limit`/`offset` are ignored.
  - One API request through the normal limiter and retry policy, with a 16 MiB body cap.
  - Each item's own `ApiClient` (one per title, as in `batch`) has its own limiter, so the first title's request can follow the list request within 250 ms. That is acceptable for a single extra request (V10).
  - **Caching (V6):** the response carries `cache-control: max-age=15, public, s-maxage=2592000, stale-while-revalidate=15`. Whether editing a list purges the CDN copy is untested (V11), so a run soon after an edit may see the old list. The tool adds no cache-busting parameter; that would defeat MangaDex's CDN. The README says so; AC35 checks it.
- **Parsing (`parse_custom_list(json, expected)`, pure, untrusted input):**
  - `json_util::parse_untrusted` with the API defaults (16 MiB, depth 64, BOM/NUL rejected), **plus a separator cap of 200 000**, about 20 per title at the cap (the V16 allocation-amplification guard; the planner may tune it).
  - `data.type` must equal `custom_list`. `data.id` must be a UUID equal to `expected` (case-insensitive).
  - `relationships` is a required array. Entries whose `type` is `manga` are kept **in array order**. Each `id` must be a string that passes `is_uuid` and is turned into a `MangaId` (canonical lowercase) **before** it reaches any URL or path; otherwise the whole document is malformed (exit 3, consistent with `parse_feed_page`'s rule for chapter ids).
  - V3 confirmed:
    - entries are exactly `{id, type}`;
    - only the types `manga` and `user` occur;
    - there is exactly one `user` (the owner), observed last;
    - an empty list holds only the `user` entry.
  - The parser still ignores any non-`manga` type and any unknown field (robust to additions). It neither requires nor counts the `user` entry.
  - More than 10 000 manga relationships (`max_list_titles`, shared with `batch`) → Errc::network, cap message.
  - `attributes.name` is optional (absent or null → null), cut to 1024 bytes on a UTF-8 boundary. It is never used in a path, a URL or a decision, only printed (`terminal_safe`) and put in the JSON (replace handler).
  - Messages are fixed text naming fields, never response content.
- **Ordering:** API order, never re-sorted.
  - V4 confirmed from the site's source: the website shows the list in `relationships` order.
  - Caveat: the website hides titles outside the viewer's content-rating and tag filters ("N hidden"). The anonymous API returns every title whatever its rating. So `mdlist` may download more titles than the site shows (README line).
- **Dedupe:** the batch engine's `mark_duplicates`. A repeated manga id becomes `duplicate` with `duplicate_of` = the earlier position, plus the warning above. All items share the same options, so equality reduces to the manga id.
- **Pagination / caps (V2):** `relationships` is complete in one response. This is confirmed up to the largest list found (121 titles). There is no pagination parameter (`limit`/`offset` silently ignored) and no documented cap: create/PUT bodies are limited to 8 KB, but titles can be added one at a time, so lists of more than 200 are likely possible. The 10 000 cap and refuse-to-truncate stay. AC31 asks for a list of more than 121 titles if the user has one.
- **Execution:** as in `batch`:
  - items run sequentially through unchanged `run_title`, a fresh `DownloadServices` and a prefixed `ConsoleObserver` per item, one `Backend` overall;
  - continue on error by default; `--fail-fast` stops at the first non-ok item;
  - 3 consecutive exit-3 items stop the run; a global local error stops it; a bad `io.out` stops it;
  - the runner's read-only pre-flight checks DIR itself (empty sub-dir) before each item.
  - **A1:** the title's own directory is checked inside the engine, after the plan (see "Per-title subdirectories").
- **File-name collisions (flat mode):** all titles go flat into DIR, using MangaDex's titles. Two different titles that sanitise to the same name collide. The first item to claim a path owns it (`OutputRegistry`). The later item's file is "foreign" to it and left alone, with the collision warning in place of `download`'s (rev2). The item status is unchanged (as in `batch`). Nothing is overwritten, even with `--overwrite` (rev3: the engine asks the registry before replacing a file; files from earlier runs are still replaced); the registry is keyed on the path. A re-run in the same order behaves the same. rev4: a `--dry-run` without `--overwrite` counts a path an earlier item planned as written, so the later item is left alone (`exists`, `foreign`) as in the real run, with the dry collision line. With `--dir-per-title`, directory disambiguation (below) prevents this case.
- **Interrupt:**
  - during the list fetch: Errc::interrupted, nothing printed on stdout, nothing created, no JSON, then 130/143;
  - inside an item, or between items: exactly as in `batch` (no partial or temp files, no further items, no JSON, no closing line).
- **`--dry-run`:** fetches the list and runs each title's plan requests (manga, feed, covers, as `batch --dry-run` does). It downloads no images, prints the planned paths, creates nothing (not DIR, not a title directory), and aggregates the same way.
- **Re-runs / idempotency:**
  - running the same command again downloads titles added to the list since the last run (subject to the CDN caveat above), and updates grown files as `download` does;
  - titles removed from the list are not touched (their files stay);
  - no state is kept between runs.
- **Large input:** at most one 16 MiB response, at most 10 000 titles. Memory per item is as in `download`; rev4: a `--dry-run` without `--overwrite` also keeps each planned file's chapter list until the run ends.

## Per-title subdirectories (`--dir-per-title`, A1)

**Recommendation: one flag, mdlist-only for now, reused unchanged by `follows`.**

- **Name: `--dir-per-title`.** It reads as what it does ("a directory per title") and is kebab-case like the other long flags. It takes no value and has no short form.
  - Rejected names: `--subdirs` (doesn't say what decides the subdirectory), `--per-title-dirs` (same meaning, clumsier), `--folders` (not the term used anywhere else in the CLI).
- **Scope:**
  - `mdlist` only in this change.
  - `follows` (step 2) binds the same flag with the same meaning; the core seam below is source-independent.
  - Not added to `batch` now. `batch` already has a per-row `dir` column, and a batch flag would need a precedence rule (a row's `dir` cell wins over the derived name) that nobody has asked for. Adding it later is purely additive (gate question QA2).

**Directory name.**
- The title is **the one `download` uses for file names**: `resolve_title(manga, --lang)`, i.e. `title.en` → the first `altTitles` entry in `--lang` → the first `title` value. `mdlist` has no `--title`.
- The name is **exactly that title's file stem**, `sanitize_file_stem(title, manga_id)` from `src/core/filename.cpp`, so a title's files are always `DIR/<stem>/<stem> - Vol. 01.epub`. The rules are reused, not copied:
  - controls (C0, DEL, C1), invalid UTF-8 bytes and `/ \ < > : " | ? *` → `_`;
  - bidi, invisible and non-character code points dropped; whitespace runs collapsed to one space;
  - leading spaces, dots and dashes and trailing spaces and dots trimmed, so the name is never hidden and never looks like an option;
  - cut at **150 bytes** on a UTF-8 boundary (one path component, well below 255).
- **Fallback:** an empty, `.` or `..` result falls back to the **full manga UUID**, exactly as file names already do. Rejected: the 8-hex prefix. The file stems inside would still be the full UUID, and the directory and its files should agree.
- The result is a single component that passes `validate_list_dir` (V12), so it is safe for `check_row_dir` and for joining onto DIR.
- **A consequence to document:** when a title has no `en` title, the name follows `--lang` (an altTitle in that language). Changing `--lang` between runs then changes both the directory and the file names, exactly as it changes file names in flat mode today.

**Re-runs when MangaDex renames a title.** With no suffix, a renamed title gets a new directory on the next run. Its files are downloaded again there (skip-existing looks only at the same path), and the old directory is left alone.
- **This is no worse than today:** in flat mode and in `download`, a rename already changes every file name and causes the same full re-download, because the file stem is the title.
- **A stable ` [<8 hex>]` suffix on the directory would not prevent the re-download.** The file names inside still change, so old and new files would pile up in one folder instead of two.
- **The cost of a suffix is constant, the benefit rare.** Library servers and readers (Komga, Kavita, Calibre imports, KOReader folder browsing) name a series after its folder. `Clean Title [1a2b3c4d]` would show up as the series name for every title, every day.
- **Recommendation: plain `<stem>`, no suffix** (gate question QA1). Documented in the README: "a renamed title, or another `--lang`, gives a new directory; the old one is left alone; delete it yourself".

**Two titles with the same name in one run.**
- Mixing two series in one folder would defeat the flag, so the later title is moved aside rather than merged.
- The batch runner owns a `TitleDirRegistry` for the run, mapping each name to the item that claimed it.
- **First claim wins, in list order.** A later item with a *different* manga id whose stem is taken gets `<stem> [<first 8 hex of its manga id>]`, plus the warning in "Input & output". If that name is also taken by another id (two ids sharing an 8-hex prefix *and* a title), it gets `<stem> [<full manga uuid>]`. The same manga id never conflicts with itself; duplicates are already skipped.
- `OutputRegistry` (the file-path check) keeps running unchanged. Because directories are disambiguated, it can't fire across titles in this mode; it stays as the backstop.
- **Limitation, documented, accepted as rare:** ownership is per run and by list order, not recorded on disk. Suppose run 1 put title A in `X/`, and later the list holds a title B with the same name *before* A. Then B claims `X/` and A moves to `X [aaaaaaaa]/` and is downloaded again there. Where B's file names collide with A's files in `X/`, A's files are "foreign" to B and are left alone with `download`'s warning; nothing is overwritten unless `--overwrite` is given, which replaces files left by an earlier run as usual (rev3 protects only files written earlier in the same run). Detecting directory ownership from the EPUB manifests on disk is out of scope.

**When the name is known, and how the engine stays reusable.**
- The title comes from `GET /manga/{id}`, inside `run_download`'s plan phase. DIR is first touched only in the execute phase (`OutputDir::open_if_exists(options.dir)` for a dry run, `OutputDir::open(options.dir, create)` otherwise). So the engine resolves the directory **between plan and execute**.
- No extra request is needed and the batch loop is unchanged. Additive seams:
  1. `DownloadOptions::dir_per_title` (bool, default false). `plan_list` sets it on every job when the flag is given. `download` and `batch` never set it, so their behaviour is byte-identical (AC38).
  2. `Plan` exposes the title's file stem (today a local of `make_epubs`).
  3. `DownloadServices` gains a borrowed, nullable `TitleDirResolver*`. It is a small polymorphic interface, called only on the `run_download` thread, that takes the stem and the manga id and returns the directory name. The app's runner implements it over `TitleDirRegistry`, knows the current item, and prints the disambiguation warning with the item prefix. Null means the plain stem.
  4. After the plan, if `dir_per_title`, the engine:
     - resolves the name;
     - prints the `-v` line;
     - runs the existing read-only `check_row_dir(options.dir, name)`: `global_error` → `Errc::failure` (stops the run); `row_error` → a new **`Errc::row_dir`** ("this title's own directory below DIR is unusable"; `exit_code_for` → 1); `ok` → continue;
     - uses `options.dir / name` as the effective directory for the dry run, the real run and every reported path (`execute_phase.cpp`'s `options.dir / epub.file_name`).
  5. The batch runner's `is_fatal` treats `Errc::row_dir` as not fatal. The item becomes `failed` (`exit_code` 1) and the next item runs.
  6. The runner reports the claimed name as `titles[].dir`.
- Why a new `Errc` rather than reclassifying after the fact: the engine is the only place that knows the name before anything is created. A distinct category keeps `is_fatal` a pure switch, with no re-check race in the runner. It adds no exit code.
- **Exit 5 and exit 4 leave no directory behind** (the plan is empty, or there is no plan). That matches `batch`'s "a row that ends 4 or 5 leaves no directory".
- **`--dry-run`:** the name is resolved and the read-only pre-flight runs (a `row_error` fails the item in a dry run too, as `batch`'s pre-flight does). `open_if_exists(DIR/<name>)` finds nothing, every file is `planned`, paths are printed under `DIR/<name>/`, and nothing is created.

**Mode switches.** A `--dir-per-title` run neither reads nor moves files from an earlier flat run, and the reverse is also true: files are matched only at their exact path. Switching modes downloads everything again in the new layout and leaves the old files alone (README line).

## Data & state

- **Reads:** the list over HTTPS (fixture mode: `<root>/api/list/<id>.json`), and existing `.epub` files in DIR (with **A1**, in `DIR/<name>/`) as `download` does.
- **Writes:**
  - only `download`'s outputs, through the existing atomic `OutputDir` publish (temp → fsync → link/rename);
  - in DIR, or with **A1** in `DIR/<name>/`, which is created (with DIR if needed) only when a title has a file to write, with `OutputDir::open`'s existing mode (V13);
  - no state file, cache, config, XDG or TMPDIR use; no secrets.
- **Threat model.** The list is third-party data: anyone can publish a list, and the response is untrusted.
  - A list can only name titles. It cannot set options, directories or file titles, so it has strictly less power than a `batch` CSV.
  - Its worst effects are load (up to 10 000 titles, documented like `batch`'s "a long list is a long run") and file names, which come from MangaDex titles exactly as in `download`.
  - **A1:** directory names also come from MangaDex titles. They go through the same sanitiser as file names, are always one component (no `/`, never `.`/`..`, never empty, at most 150 bytes) and pass `validate_list_dir`'s rules (V12). So a hostile title can't leave DIR on its own. An existing symlink inside DIR whose name matches a title is followed (`check_row_dir` and `OutputDir` follow symlinks), the same accepted OD1 rule as `batch`.
  - Every id is checked with `is_uuid` before use. The list name is display-only.
  - Symlinks inside DIR are followed (batch's OD1); the help and README say so.
  - There is no process spawning, no env read and no new file-descriptor path. The fixture transport is unchanged.
  - `security-reviewer` still reviews it, because it adds untrusted JSON input and server-derived directory names.

## Engine reuse (for the planner; names are suggestions)

- **Core, new** (`include/mangadex_dl/mdlist.hpp`, `src/core/mdlist.cpp`):
  - `ListId`;
  - `struct CustomList { ListId id{}; std::optional<std::string> name{}; std::vector<MangaId> titles{}; }`;
  - `parse_custom_list`;
  - `list_request_url(api, id)`;
  - `plan_list(const CustomList&, const BatchSettings&, bool dir_per_title) -> std::vector<BatchJob>`, with `line` = position, `dir` = DIR, `options` = base plus the manga (plus `dir_per_title`), `raw` = defaults plus the manga string, and duplicates marked;
  - **A1:** `TitleDirRegistry` (pure: `claim(stem, MangaId, item) -> {name, std::optional<std::size_t> taken_by}`) and the `TitleDirResolver` interface. Both could live in `mdlist.hpp` or a new `title_dir.hpp`.
- **Core, changed (all serialization points unless noted):**
  - `ApiClient::fetch_custom_list(const ListId&, std::stop_token)` in `include/mangadex_dl/api_client.hpp`. 404 → Errc::not_found with the message above; the 401/403 fallback per "Exit codes"; everything else goes through the existing status mapping.
  - `include/mangadex_dl/batch.hpp`:
    - `BatchSettings` gains `enum class BatchSource { csv, mdlist }` (default `csv`);
    - the closing message, the duplicate warning and the collision advice take their noun from it ("line" or "item");
    - `batch_to_json` gains an optional list summary, so it emits `list` only for `mdlist`;
    - `RowOutcome` can carry the item's effective dir (A1).
  - **A1**, `include/mangadex_dl/options.hpp`: `DownloadOptions::dir_per_title`. `RawDownloadArgs` is unchanged; mdlist's raw args own the flag.
  - **A1**, `include/mangadex_dl/model.hpp`: `Plan` gains the file stem. `src/core/planner.cpp` fills it.
  - **A1**, `include/mangadex_dl/download.hpp`: `DownloadServices` gains `TitleDirResolver*` (nullable, borrowed).
  - **A1**, `src/core/download/execute_phase.cpp` (not a shared header): the post-plan resolution and pre-flight, and the effective dir.
  - **A1**, `include/mangadex_dl/error.hpp`: `Errc::row_dir`, mapped to exit 1 (V14).
  - `validate_batch_args` gains the list-URL check. The `download` MANGA hint lives in `options.cpp`.
- **App:**
  - Move phase 2 of `run_batch_command` and `ConsoleTitleRunner` out of `cmd_batch.cpp` into `src/app/cmd_batch_jobs.{hpp,cpp}`. The `cmd_` prefix keeps it inside the `CONFIGURE_DEPENDS` glob, so there is no CMake edit. This is a pure move; `batch`'s output stays byte-identical, and every existing batch test passes unchanged.
  - `ConsoleTitleRunner::row_prefix` depends on the source: `<label>:<line>: ` for `csv`, `list:<n>: <uuid>: ` for `mdlist`.
  - **A1:** the runner implements `TitleDirResolver` over its `TitleDirRegistry` (one per run, declared before the runner), passes it to `run_title` (which puts it in `DownloadServices`), and treats `Errc::row_dir` as not fatal.
  - `cmd_mdlist.cpp` = validate → handlers → backend → fetch → plan → the shared phase 2.
  - `cli.cpp` (a **serialization point**): one registration line, the `(commands: …)` list, and the `--title` ExtrasError check for `batch`/`mdlist`.
  - `exit_help.hpp` rows 4 and 7; `exit_codes.hpp` comments (a **serialization point**); the `cmd_download.cpp` and `cmd_batch.cpp` footer lines.

## How `follows` fits in later (step 2, not designed here)

- **A sibling subcommand `mangadex-dl follows [DIR] [OPTIONS]`.** Same options, same engine path. The source changes from `fetch_custom_list` to a paginated `/user/follows/manga` fetch. The JSON is the same, with `source: "follows"` and an additive `follows` object; prefixes are `follows:<n>: <uuid>: `.
- **`--dir-per-title`** (A1) is bound on `follows` with the same meaning and the same core seam. Follows-sized collections are the main reason the flag exists.
- **Auth options and credential precedence** (config/env, which will then need the CLI11-order statement) live on `follows`. They can later be added to `mdlist` too, to read your own *private* lists. That is additive: anonymous `mdlist` behaviour, including "private → 4", stays as specified here.
- **Nothing in this spec reserves or blocks anything.** It doesn't take the word `follows`, reads no credentials, env vars or config, and adds no exit code (codes 8 and up stay free for auth failures).
- **Rejected slots:** a magic `mdlist follows` value (it would mix auth options into an anonymous command) and `batch --follows` (the FILE positional trap).

## Acceptance criteria

AC1–AC35 keep their numbers; changes are marked **rev1**. A1 adds AC36–AC48.

1. `[unit]` `ListId::parse` accepts:
   - a bare UUID in upper or mixed case, lowercased;
   - `https`/`http`/no scheme, `www.`, an upper-case host;
   - `/list/<uuid>`, `/<slug>`, a trailing `/` (**rev1:** the empty-slug form the site generates for non-Latin names), `/<slug>/`, `?q`, `#f`.

   It rejects: empty, `-`, `titles.csv`, `/list/<uuid>/a/b`, `//`, `mangadex.org.evil`, a port, a space or control byte, 35 or 37 characters. `/title/<uuid>` gets the distinct "is a title" error. No message contains the input.
2. `[unit]` `MangaId::parse`'s existing tests pass unchanged after any shared-helper refactor. `looks_like_list_id` agrees with `ListId::parse`.
3. `[unit]` `parse_custom_list` accepts the live-sample shape (`user` last):
   - manga in array order; `user` and unknown relationship types ignored; extra fields ignored;
   - name absent or null → null; a 2000-byte name is cut to ≤1024 bytes on a UTF-8 boundary;
   - an upper-case manga id is canonicalised; only a `user` entry → empty.
4. `[unit]` `parse_custom_list` rejects, with Errc::network and a fixed message containing no response bytes:
   - not JSON, a BOM, a NUL, depth 65, over the separator cap, over 16 MiB;
   - missing `data`/`id`/`type`/`relationships`; `type` other than `custom_list`; `data.id` other than the requested id;
   - a manga id `../x`, 35 characters long, or not a string;
   - 10 001 manga relationships (the message names 10000).
5. `[unit]` `ApiClient::fetch_custom_list` over FakeTransport:
   - the URL is exactly `<api>/list/<id>` with the **lowercase** id, also when LIST was typed in upper case (**rev1**, V6);
   - 200 → the list; 404 → Errc::not_found with the message; 5xx retried, then Errc::network; 429 per policy;
   - 401/403 with a CustomList JSON error body → Errc::not_found; a bare 403 → Errc::network (**rev1**);
   - a stop → Errc::interrupted; the 16 MiB body cap is enforced.
6. `[unit]` `plan_list`: positions 1..n in API order; `dir` = DIR for all; options = base plus the manga; a repeated id → `duplicate_of` = the earlier position; empty → no jobs.
7. `[unit]` With `BatchSource::mdlist`: "items 4, 9", "stopped at item 9: …", "same as item 2", the collision advice. With `csv`, every existing batch unit test passes byte for byte.
8. `[unit]` The JSON has exactly batch's keys plus `list{id,name,url}` after `source`; `line` = position. A batch document is byte-identical to before (no `list` key). A name with ESC and invalid UTF-8 → valid JSON, `\u001b` escaped, no raw ESC byte.
9. `[cli]` Fixture `clean`, list L1 → exit 0. stdout = `download`'s paths, stderr empty, files exist. `--json` has `list.name` "Clean List", `source` as typed, `titles[0].line` 1.
10. `[cli]` Usage errors → exit 2, empty stdout, the `mdlist --help` hint, no request (the fixture root is never opened), DIR not created. Covers:
    - LIST empty, a title URL, `-`, a CSV path, junk;
    - DIR empty or id-like; a bad `--lang`;
    - `--title X` and `--title=X`, with their message.
11. `[cli]` Unknown list → exit 4 with the list message. No JSON (also with `--json`), DIR not created.
12. `[cli]` A simulated interrupt before item 2 → item 2 never starts; no JSON, no closing line, no temp file. An interrupt during the list fetch → nothing written, no JSON, the interrupted path.
13. `[cli]` `-v` prints the list line (a name with ESC shows `?`) and `list:1: <uuid>`. `-q` hides the empty-list and duplicate warnings but keeps the closing error line.
14. `[cli]` L2 (clean plus unknown) → exit 4. Item 2's line is `error: list:2: 6666…: title 6666… not found …`. The closing line is `1 of 2 titles did not complete (item 2); see the errors above`.
15. `[cli]` Root `unknown command` lists `download, batch, mdlist`. `batch <list URL>` → 2 with the mdlist hint. `download <list URL>` → 2 with the mdlist hint. Update `tests/cli/cli_test.cpp` and `tests/cli/download_cli_test.cpp`.
16. `[e2e]` The `mdlist --help` golden covers the synopsis `mangadex-dl mdlist [OPTIONS] LIST [DIR]`, the LIST footer, the examples, the `--dir-per-title` line (**rev1**) and the exit table, and has no `--api-base`. Re-bless requested: `download.help`, `batch.help`, `edge.help`, new `mdlist.help`.
17. `[e2e]` L1 on `clean` → 0, the paths, sound EPUBs, no `.part-`. These forms all give identical stdout: the bare UUID, the upper-case UUID, the full URL with slug, **rev1:** the empty-slug `https://mangadex.org/list/<id>/`, the `www.` form and the no-scheme form.
18. `[e2e]` L8 on `nochapters` → exit 7, the prefixed item lines plus the closing line; `--json` valid with `no_chapters`/`not_found`. `--fail-fast` → exit 5, item 2 `not_run`.
19. `[e2e]` L9 on `incomplete` → 6, failed file absent, no `.part-`. L2 on `clean` → 4, with the clean files written.
20. `[e2e]` The unknown list → 4, stdout empty, `out` absent. L5 (bad manga id) and L6 (id mismatch) → 3, `out` absent; with `--json`, stdout empty.
21. `[e2e]` L3 (empty) → 0, warning `list a0000000-…-000000000003 has no titles`; `--json` has `titles: []` and the `list` object. L4 (same title twice) → 0, the duplicate warning, one set of files.
22. `[e2e]` `--dry-run` writes nothing and creates no `out`. Goldens `mdlist.dry-run` and `mdlist.json-dry-run` requested.
23. `[e2e]` `--api-base http://127.0.0.1:1` → 3 with the network remedy, empty stdout, `out` absent (label `needs-libcurl`).
24. `[e2e]` L2 with DIR a regular file (`{{INPUTS}}/README`) → exit 1, `stopped at item 1: the error above affects every title; 1 title not run`.
25. `[edge]` `STDIN CLOSED` and `STDIN NULL` with L1 → exit 0, the same stdout (stdin never read), never 70.
26. `[edge]` L1 `--dry-run > /dev/full` → exit 1 plus `mangadex-dl: write error`.
27. `[edge]` L10 on `bulk`, `--dry-run --layout chapters | head -1` → silent SIGPIPE death (141); also with the parent ignoring SIGPIPE.
28. `[edge]` Invalid-UTF-8 LIST under `LC_ALL=C` → exit 2 with the fixed message (not echoed, not 70). Invalid-UTF-8 DIR with `--json --dry-run` → valid JSON. `Zoë` DIR verbatim under `C.UTF-8` and `C`.
29. `[edge]` `--json` with `CLICOLOR_FORCE=1` → no ANSI. Piped stderr with a failing item (L8) → no ANSI.
30. `[tty-pty]` L2 under a pty → progress `(1/2) [1/2] Vol. 1:`. None with `--json`, `-q` or `TERM=dumb`. `NO_COLOR` → uncolored `error:`.
31. `[human]` The user's follows copied into a real public MDList (≥3 titles), run live: correct files, order as on the website, no 429 storm, readable `list:<n>:` lines. **rev1:**
    - If the user has a list of **more than 121 titles**, run it too and compare the count with the website (closes V2's "largest observed").
    - Expect possibly more titles than the site shows when its content filters hide some (V4).
32. `[human]` A private list, run live, without login: confirm the 404 and the exit-4 message (closes V1).
33. `[human]` The list URL copied from the browser's address bar and from the site's share control is accepted as-is, including a list with a non-Latin name (URL ending `/list/<id>/`).
34. `[human]` Ctrl-C during the list fetch and in the middle of the list, in a real terminal → 130, no partial or temp files, a re-run resumes.
35. `[human]` Add a title to the list on the website, run again → only the new title's files appear. Remove a title → its files are left alone. **rev1:** note how long the change took to show up (V11: the response has `s-maxage=2592000, stale-while-revalidate=15`). If an edit isn't visible within a minute, record it; the README wording on caching depends on it.

**A1, `--dir-per-title`:**

36. `[unit]` Directory name = the title's file stem:
    - `A/B: C?` → `A_B_ C_`; a title of `...`, only spaces, or only U+202E → the full manga UUID;
    - a 300-byte CJK title → ≤150 bytes, cut on a UTF-8 boundary; leading `-`/`.` and trailing `.`/space trimmed;
    - with no `en` title, the name follows `--lang` (altTitle).
    - Property: every output of `sanitize_file_stem` over the existing filename test corpus plus random byte strings passes `validate_list_dir` and is one component (V12).
37. `[unit]` `TitleDirRegistry`:
    - the same manga twice → the same name, no `taken_by`;
    - another manga with the same stem → `<stem> [<8 hex>]` with `taken_by` = the first item;
    - a third manga with the same stem and the same 8-hex prefix → `<stem> [<full uuid>]`.
38. `[unit]` Engine with FakeTransport and a temp DIR:
    - `dir_per_title` off → paths and reports byte-identical to today (every existing download and batch test unchanged);
    - on → files in `DIR/<stem>/`, report paths include it;
    - `DIR/<stem>` a regular file → `Errc::row_dir`, nothing created;
    - DIR a regular file → `Errc::failure`;
    - dry run → neither DIR nor `DIR/<stem>` created, every file `planned` under `DIR/<stem>/`;
    - exit 5 (empty plan) → no directory created.
39. `[unit]` Batch runner: an `Errc::row_dir` item is `failed`/`exit_code` 1, not fatal, and the next item runs; with a later 4 the aggregate is 7.
40. `[cli]` L1 `--dir-per-title` on `clean`:
    - exit 0; stdout `out/Clean Title/Clean Title - Vol. 01.epub` then `…Vol. 02.epub`; stderr empty;
    - `--json` `titles[0].dir` = `out/Clean Title`, report paths under it.
    - Running the same command again → exit 0, empty stdout, only the "already up to date" note.
41. `[cli]` Collision, using a test-built tree `clean-twin`: clean plus manga `77777777-7777-4777-8777-777777777777`, also titled "Clean Title", served. It is not in `all_fixtures()`, so the E2E tree count is unchanged. A list of both:
    - item 2 goes to `out/Clean Title [77777777]/` with the "already holds item 2" warning, wording as specified but naming item 1;
    - both sets of files exist; `-q` hides the warning;
    - `titles[1].dir` = `out/Clean Title [77777777]`.

    Other warnings from the shared cover document are tolerated (V16).
42. `[cli]` `-v --dir-per-title` prints `output directory: out/Clean Title` after `list:1: <uuid>`.
43. `[cli]` A flat run, then a `--dir-per-title` run into the same DIR: the flat files are neither read, moved nor changed; the new files are written under `out/Clean Title/`.
44. `[e2e]` L1 `--dir-per-title` → exit 0, exact stdout regex, `EXPECT_FILES` and `EXPECT_EPUB` under `out/Clean Title/`, no `.part-`.
45. `[e2e]` L1 `--dry-run --dir-per-title` → the planned paths under `out/Clean Title/`, empty stderr, `out` absent.
46. `[e2e]` L2 `--dir-per-title` with DIR `{{INPUTS}}/mdlist`, which holds a committed regular file named `Clean Title`:
    - item 1 fails alone with `list:1: 11111111-…: cannot open directory '…/Clean Title': Not a directory; rename or remove it, or run without --dir-per-title`;
    - item 2 runs and is not found;
    - exit 7, no `stopped at`, nothing created.
47. `[edge]` `--json --dry-run --dir-per-title` with `CLICOLOR_FORCE=1` → valid JSON, no ANSI, `dir` = `out/Clean Title`.
48. `[human]` `--dir-per-title` on the user's real list, opened in their library or reader app (Komga/Kavita/Calibre/KOReader, whichever they use): one series per folder, readable names. Check one non-Latin title, which falls back to an altTitle or the first title.

**E2E fixtures needed.**
- **List documents** are generated, not committed. Add a `FixtureList` (id, name, visibility, ordered manga ids) to `tests/support/mdx_fixture.hpp`, and have `write_fixture_tree` write `<root>/api/list/<id>.json`.
- **Shape:** the live sample (V3/V6): `{"result":"ok","response":"entity","data":{"id","type":"custom_list","attributes":{"name","visibility":"public","version":1},"relationships":[{"id","type":"manga"},…,{"id","type":"user"}]}}`. Entries are exactly `{id, type}`, with **exactly one `user` entry, last**.
- **Existing cases are unaffected:** the documents are additive files, and the twelve-tree count is unchanged.
- **One real title per tree:** covers stay one title per tree (`api/cover/offset-*.json` is not keyed by manga), so every list mixes one real title with unknown ids.

| Id | Tree | Name | Manga relationships (then one `user`) |
|---|---|---|---|
| L1 `a0000000-0000-4000-8000-000000000001` | `clean` | "Clean List" | 1111… |
| L2 `…000000000002` | `clean` | "Clean And Unknown" | 1111…, 6666… |
| L3 `…000000000003` | `clean` | "Empty" | none |
| L4 `…000000000004` | `clean` | "Twice" | 1111…, 1111… |
| L5 `…000000000005` | `clean` | "Bad Id" | `../x` |
| L6 `…000000000006` | `clean` | (file L6, `data.id` = L1) | 1111… |
| L8 `…000000000008` | `nochapters` | | 5555…, 6666… |
| L9 `…000000000009` | `incomplete` | | 4444… |
| L10 `…00000000000a` | `bulk` | | 3333… |
| unknown `…0000000000ff` | `clean` | | no file (404) |

- **A1, committed input:** `tests/e2e/inputs/mdlist/Clean Title`, a regular file for AC46.
- **A1, test-only tree:** the `clean-twin` tree for AC41, built in the CLI test only.
- **Docs and cases:** document both in `tests/e2e/inputs/README`. The new `tests/e2e/cases/mdlist.cmake` is globbed, and `_edge.cmake` gains `mdlist-*` cases.
- **What fixture mode can't express:** private-list and 401/403 behaviour, since it only gives 200 or 404. These are covered by FakeTransport at `[unit]` (AC5) and by a person (AC32).

## README and docs

- **Intro:** add "`mangadex-dl mdlist` does the same for every title in a public MangaDex list."
- **Contents:** add a link to the new section.
- **New section "A MangaDex list: `mdlist`",** after "Many titles: `batch`":
  - the synopsis and a table: LIST forms (incl. the trailing-slash form), DIR, "the options are `batch`'s, with no `--title` and no per-title settings", and `--dir-per-title`;
  - "No login: the list must be public. To download the titles you follow, add them to a public MDList; reading follows directly needs a login, which `mangadex-dl` doesn't support yet";
  - order (the website's order), dedupe and the 10 000 cap;
  - "The website hides titles outside your content filters ('N hidden'); `mdlist` downloads every title in the list, so it may download more than the site shows";
  - "MangaDex caches lists: a title you just added may take a while to appear" (wording settled by AC35/V11);
  - layout:
    - flat DIR and the collision warning;
    - `--dir-per-title`: `DIR/<title>/<title> - Vol. 01.epub`, named and sanitised like the files, with the same-name rule (`<title> [<8 hex>]`);
    - "a renamed title, another `--lang`, or switching between flat and per-title gives new paths; the old files are left alone";
  - stderr: `list:<n>: <uuid>:` and the "items" wording;
  - `--json`: batch's document plus `list`, with `line` = position and `dir` the title's directory;
  - exit status: 4 and 3 at list level vs aggregate, and how to tell them apart;
  - re-runs pick up new titles; a short examples block, including `--dir-per-title`.
- **Exit status table:** rows 1 ("For `batch`, … for `mdlist`, no usable libcurl …" is unchanged in substance), 3 ("… or the list response"), 4 ("The title, or for `mdlist` the list, does not exist on MangaDex or the list is private") and 7 ("`batch` and `mdlist`").
- **Output:** "Usage errors … (or `mangadex-dl mdlist --help` …)". "Only `batch -` reads stdin" stays true.
- **Interruption and file safety:** "… with `mdlist --dir-per-title`, also `DIR/<title>/<name>.epub`".
- **Limits:** "`mdlist` downloads one list per run, at most 10 000 titles."
- **Network behaviour:** "`mdlist` makes one extra API request, for the list."

## Out of scope

- Login and reading follows (step 2); private lists; several lists per run; a list from a file or stdin; per-title options.
- `--dir-per-title` on `batch` (additive later; QA2); a stable id suffix on every title directory (QA1); detecting directory ownership on disk across runs.
- `/list/{id}/feed` and chapter-level selection across a list; creating or editing lists.
- Exporting a list's ids to CSV. A later read-only command could do it; `--dry-run --json` works today but makes every title's plan requests.
- Bypassing MangaDex's CDN cache; legacy numeric list ids; parallel titles; caching the list locally; completions and man page; PDF.

## Decisions at the design gate (user, 2026-10-26)

- **Q1. Command name:** `mdlist`.
- **Q2. List not found or private:** reuse exit 4 and reword its row; no new code.
- **Q3. Layout of DIR:** flat by default, as `batch` does, **plus the opt-in `--dir-per-title` in this same change.** Specified by amendment A1 ("Per-title subdirectories").
- **QA1. Title directory names:** plain `<stem>/`, no id suffix; only a same-name clash within one run gets ` [<8 hex>]`.
- **QA2. `--dir-per-title` on `batch`:** not in this change. The user wants it added later as a separate change (a row's `dir` cell would win over the derived name). Done in `batch-dir-per-title-spec.md` (the cell replaces the derived name).

## [VERIFY] items

- **V1 (API, live): UNRESOLVED, strong indirect evidence for 404.**
  - The API spec documents only 200/404 for `GET /list/{id}`, and the website frontend retries with auth only after a 404, so a private list asked for anonymously is inferred to answer 404.
  - A non-existent id answers `404 {"result":"error","errors":[{"id":…,"status":404,"title":"not_found_http_exception","detail":"CustomList `<id>` not found.","context":null}]}`. A rate-limit ban is a generic 403.
  - Design kept: exit 4 and its wording, with the 401/403-with-CustomList-body → 4 fallback and any other 403 → 3. AC32 closes it live.
- **V2 (API): CONFIRMED up to ≥121 titles.**
  - The largest list found (121 titles) came back complete in one response. `limit`/`offset` are silently ignored; there is no pagination parameter and no documented cap.
  - Create/PUT bodies are limited to 8 KB, but titles can be added one at a time, so lists of more than 200 are likely possible.
  - Design kept: the 10 000 cap and refuse-to-truncate. AC31 asks for a list of more than 121 titles.
- **V3 (API): CONFIRMED.** Only `manga` and `user` types occur. Entries are exactly `{id, type}`. There is exactly one `user`, observed last. An empty list holds only the `user` entry. The fixtures put `user` last.
- **V4 (API/website): CONFIRMED from the site's source.** The displayed order is the `relationships` order. Caveat: the site hides titles outside the viewer's content-rating and tag filters, while the anonymous API returns them all. That earns one README line.
- **V5 (website): CONFIRMED.** The URLs are `https://mangadex.org/list/<id>/<slugify(name)>`. Share controls add no `?`/`#`. Non-Latin names give an empty slug and a trailing slash (`/list/<id>/`), which is accepted and tested. The UI says "MDList(s)".
- **V6 (API): CONFIRMED.**
  - `data.type` is `"custom_list"` and `data.id` is lowercase.
  - **An upper-case id in the URL is a router 404 ("No route found")**, so the request always uses the canonical lowercase id (AC5, AC17).
  - `includes[]` is accepted but only bloats the response; `contentRating`/`limit`/`offset` are ignored.
  - The response carries `cache-control: max-age=15, public, s-maxage=2592000, stale-while-revalidate=15`.
  - The fixture shape follows the live sample (see "E2E fixtures needed").
- **V7 (CLI11, installed 2.7.x):**
  - the shared option helpers register on a third subcommand without `ConstructionError`;
  - globals after LIST and DIR work through `fallthrough()`;
  - `App::remaining(true)` shows a leftover `--title`/`--title=X` once `mdlist` was reached (for the generalised ExtrasError message);
  - help ordering matches the batch golden, with `--dir-per-title` after `--fail-fast`.
- **V8 (repo):**
  - the phase-2 extraction keeps `batch` byte-identical (every batch case in tests/cli, tests/e2e and `_edge.cmake`, plus the goldens);
  - `with_remedy` leaves the over-cap and malformed-list messages without the network remedy, or the plan says otherwise;
  - `check_row_dir` with an empty sub-dir behaves as for a `batch` row without a `dir`.
- **V9 (repo):** the fixture transport maps `file://<root>/api/list/<id>` (no query) to `<root>/api/list/<id>.json`. Reading `map_url` says yes: it appends `.json` to the last `api/` component when there is no `offset`. Confirm with a test.
- **V10 (repo):** the list fetch's `ApiClient` (separate limiter, destroyed before item 1) plus a fresh client per item is acceptable for the "≤4 req/s" claim in the README. It is one extra request, the same per-row-client pattern `batch` already has.
- **V11 (API, live, new): OPEN.** Does editing a list purge the CDN copy (`s-maxage=2592000`, `stale-while-revalidate=15`)? It decides the README's caching sentence and AC35's expectation. It is closed by AC35 (human) or by a live edit-then-GET probe.
- **V12 (repo, new, A1):** every output of `sanitize_file_stem` (≤150 bytes, controls and separators replaced, bidi and invisible characters dropped, never `.`/`..`/empty) passes `validate_list_dir` and is accepted by `check_row_dir` as one segment. Proven by AC36's property test. One gap to check: `validate_list_dir` rejects U+0085 and other C1 controls, and `sanitize` replaces C1 with `_`; confirm that no code point `terminal_safe` considers unsafe survives `classify`.
- **V13 (repo, new, A1):** `OutputDir::open(DIR/<name>, create=true)` creates DIR and the title directory when both are missing (mode as today, subject to umask), and the post-plan pre-flight followed by open behaves like `batch`'s pre-flight followed by `run_title` (ENOENT → ok, created later).
- **V14 (repo, new, A1):** adding `Errc::row_dir` is covered in every `switch (Errc)` (`exit_code_for`, `is_fatal`, any others): `-Wswitch` under `-Werror` finds them. `exit_code_for(row_dir) == 1`.
- **V15 (repo, new, A1):** the additive members on frozen headers (`DownloadOptions::dir_per_title`, the stem on `Plan`, `TitleDirResolver*` on `DownloadServices`) leave `download` and `batch` byte-identical: every existing golden, CLI and E2E case unchanged. `DownloadOptions`' defaulted `operator==` (batch dedupe) is unaffected, because every job of one run has the same flag.
- **V16 (repo, new, A1):** in the test-built `clean-twin` tree the cover document is shared. Check whether the planner ignores covers whose manga relationship is another title, or warns when their files are missing. AC41 then asserts only its own lines.
