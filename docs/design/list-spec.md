# Feature spec: `mangadex-dl mdlist`: downloading every title in a public MangaDex list

Status: DRAFT 2026-10-26, waiting for the ⏸ design gate. Produced by `cli-designer`.
Related: [`batch-spec.md`](batch-spec.md) (its engine is reused unchanged), [`download-spec.md`](download-spec.md),
[`skip-existing-spec.md`](skip-existing-spec.md) (re-runs).

**Intent:** fetch a public MangaDex custom list (MDList) without logging in. Then run every title in it through the `batch` engine, in the list's order and with one set of options. That engine handles planning, running, stop rules, reporting, exit-code aggregation and the `--json` document. A list is a batch without a header row: its rows come from `GET /list/{id}` instead of from a file.

**Who runs it:** mostly a person at a terminal who has copied the list's URL from the browser. Next most often a cron job or script that runs the same list again to pick up new chapters and new titles.

**Why now:** reading your follows needs OAuth, because `GET /user/follows/manga` returns 401 to anonymous requests. Step 1 (this spec): put your follows into a public MDList and download it with no credentials. Step 2 (later, not designed here): login, then reading follows directly. "How `follows` fits in later" below shows that step 2 is purely additive.

**Assumptions:**
- One list per run, one output root (DIR), and no per-title options. A list carries only titles, so every title gets the command-line options.
- Titles run one after another. `--jobs` still parallelises the images within each title. No new threads.
- No config file and no env-var options, as today. Precedence is "command line > built-in default" only. CLI11's order (CLI > config > env > default) has nothing to arbitrate. The later `follows` spec will need a precedence statement for credentials; this one does not.
- Everything about the API beyond the facts in the brief is marked `[VERIFY]`.

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
  - `--fail-fast` ("Stop at the first title that fails"), as in `batch`.
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

Lines fit 80 columns and right-column texts fit 44. The options block is `batch`'s, byte for byte.

```
Download every title in a public MangaDex list (MDList)


mangadex-dl mdlist [OPTIONS] LIST [DIR]


POSITIONALS:
  LIST REQUIRED                     Public MDList: its UUID or list URL
  DIR [.]                           Output directory (created if missing)

OPTIONS:
  ... (exactly as batch, including --fail-fast) ...

LIST:
  A public MangaDex custom list (MDList): its UUID, or its address on the
  website, https://mangadex.org/list/<uuid>/<name> (http://, www., no
  scheme, a trailing /, ?query or #fragment are fine). No login is used, so
  the list must be public. Every title in it is downloaded into DIR, one
  after another in the list's order, with the options above.

  To download the titles you follow, add them to a public MDList.

Examples:
  Every title in a list, into ~/Manga:
    mangadex-dl mdlist https://mangadex.org/list/<uuid> ~/Manga
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
- **stdout (human):** exactly `download`'s data. One path per written (or, with `--dry-run`, planned) file, concatenated across titles in list order. A full success prints nothing on stderr.
- **stderr:**
  - **Row-scoped lines** keep `batch`'s `<source>:<n>: ` shape after `error:`/`warning:`. The source is `list`, `<n>` is the 1-based position in the list, and the item's validated manga UUID follows. For example:
    `mangadex-dl: error: list:3: 66666666-6666-4666-8666-666666666666: title 66666666-… not found on MangaDex (HTTP 404); check the UUID or URL`.
    The UUID is in the prefix because the user wrote no file to look the position up in, and many engine messages (exit 5, exit 6, local errors) don't name the title. It is canonical and validated, so echoing it is safe. Engine messages stay byte-identical to `download`'s, redundancy included.
  - **List-level lines** have no item prefix: `mangadex-dl: error: list <list-uuid> not found on MangaDex …`.
  - **Wording that names a row** uses "item" where `batch` says "line". Every `batch` string stays byte-identical.
    - Duplicate: `list:5: <uuid>: same as item 2; skipped`.
    - Cross-item collision: `list:4: <uuid>: <path> was already written by item 2; download one of them on its own with 'mangadex-dl download <title> --title ...'`.
    - Closing line, one only, even with `-q`/`--json`; the item list is capped at 10 as in `batch`:
      - `2 of 15 titles did not complete (items 4, 9); run the same command again to retry them`, or `…; see the errors above`, with the same retry rule as `batch`;
      - dry run: `2 of 15 titles would fail (items 4, 9)`;
      - stopped early: `stopped at item 9: <reason>; 6 titles not run`, with `batch`'s reasons.
  - **The up-to-date note** is printed once at the end, as in `batch`.
  - **Empty list:** `mangadex-dl: warning: list <list-uuid> has no titles`. Not shown with `-q`.
- **`-v`:**
  - After the fetch: `mangadex-dl: list <list-uuid> "<name>": <n> titles`. Without a name: `list <list-uuid>: <n> titles`. The name passes through `terminal_safe` and is capped at 64 bytes on a UTF-8 boundary, then `...`.
  - At each item: `list:<n>: <manga-uuid>`, then the engine's own lines.
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
        manga_id, dir, status, exit_code, error, duplicate_of, report   as batch
                                   (dir = DIR as typed for every title; duplicate_of is a position)
    } ]
    summary: { titles: {...}, files: {...} }   as batch
    ```
  - **On `line`:** for `mdlist` it holds the list position, and the README documents it as "the title's place in its source: the CSV line for `batch`, the list position for `mdlist`". Rejected alternative: `line: null` plus a new `position` key. That would change `line`'s type to `int|null` inside the shared shape, so a consumer written for `batch` would break on an `mdlist` document, and every row would carry two keys for one fact.
  - The empty list prints `titles: []`, with the `list` object filled in.
- **Color:** stderr prefixes only, through the existing `resolve_color`. stdout is never colored.
- **Locale:** output is locale-independent; positions are formatted with `std::format` without `L`.

## Exit codes

**No new code is recommended** (gate question Q2). List-level outcomes reuse codes whose meaning fits. Item outcomes aggregate exactly as in `batch`.

| Outcome | Code |
|---|---|
| every item ok or duplicate, or a list with no titles | 0 |
| backend unusable (no usable libcurl); a global local error in an item (DIR unusable, ENOSPC, EIO, EROFS) stops the run, rest `not_run`; a failed write to stdout | 1 |
| any `ParseError`; LIST empty, a title URL or not a list form; DIR empty or looks like an id; bad option values; `--title` | 2 |
| the list fetch failed: connection, DNS, TLS, timeout, 5xx/429 retries exhausted, a 403 rate-limit ban; or the response is malformed, over 16 MiB, nested too deep, over the separator cap, has the wrong `data.id`/`data.type`, or holds a manga relationship whose id is not a UUID; or the list has more than 10 000 titles | 3 |
| the list does not exist, or is private (`GET /list/{id}` answered 404; see V1 for private lists) | 4 |
| items: every non-ok item failed the same way | that code (1, 3, 4, 5 or 6) |
| items: non-ok items failed in more than one way | 7 |
| SIGINT / SIGTERM (also during the list fetch) | 130 / 143 (re-raised by `main`) |

- **Exit 4 message:** `list <list-uuid> not found on MangaDex (HTTP 404): it does not exist or is private; check the id, make the list public on MangaDex, or use 'mangadex-dl download' if it is a title`.
- **Over the title cap:** `list <list-uuid> has more than 10000 titles; refusing to truncate it; split it into smaller lists`. Exit 3 follows the precedent of `download`'s "more than 10000 chapters; refusing to truncate" (an oversized API response). `with_remedy` must **not** append the network-connection remedy here (`[VERIFY]` V8).
- **Telling list-level from aggregate 3/4 apart:** list-level failures print no JSON and their stderr line has no `list:<n>:` prefix. An aggregate 4 ("every item's title is gone") prints the JSON document.
- **Private lists (V1):** if MangaDex answers an anonymous request for a private list with 403 or 401 and a JSON error body naming the list, that status maps to 4 with the same message. A 403 without that body stays 3, the rate-limit ban as in `download`. If V1 shows private lists are indistinguishable from bans, the plan goes back to the designer.
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
- **List request:** `GET <api>/list/<id>`, no query and no includes (`[VERIFY]` V6). It is one API request through the normal limiter and retry policy, with a 16 MiB body cap. Each item's own `ApiClient` (one per title, as in `batch`) has its own limiter, so the first title's request can follow the list request within 250 ms. That is acceptable for a single extra request (V10).
- **Parsing (`parse_custom_list(json, expected)`, pure, untrusted input):**
  - `json_util::parse_untrusted` with the API defaults (16 MiB, depth 64, BOM/NUL rejected), **plus a separator cap of 200 000**, about 20 per title at the cap (the V16 allocation-amplification guard; the planner may tune it).
  - `data.type` must equal `custom_list`. `data.id` must be a UUID equal to `expected` (case-insensitive).
  - `relationships` is a required array. Entries whose `type` is `manga` are kept **in array order**. Each `id` must be a string that passes `is_uuid` and is turned into a `MangaId` (canonical lowercase) **before** it reaches any URL or path; otherwise the whole document is malformed (exit 3, consistent with `parse_feed_page`'s rule for chapter ids).
  - Other relationship types (`user`, owner, unknown) are ignored (`[VERIFY]` V3). Unknown fields are ignored.
  - More than 10 000 manga relationships (`max_list_titles`, shared with `batch`) → Errc::network, cap message.
  - `attributes.name` is optional (absent or null → null), cut to 1024 bytes on a UTF-8 boundary. It is never used in a path, a URL or a decision, only printed (`terminal_safe`) and put in the JSON (replace handler).
  - Messages are fixed text naming fields, never response content.
- **Ordering:** API order, never re-sorted (`[VERIFY]` V4: whether that is the website's order).
- **Dedupe:** the batch engine's `mark_duplicates`. A repeated manga id becomes `duplicate` with `duplicate_of` = the earlier position, plus the warning above. All items share the same options, so equality reduces to the manga id.
- **Pagination / caps:** the design assumes `relationships` is complete (V2). If V2 finds a server cap or pagination, the plan goes back to the designer. Never truncate silently.
- **Execution:** as in `batch`:
  - items run sequentially through unchanged `run_title`, a fresh `DownloadServices` and a prefixed `ConsoleObserver` per item, one `Backend` overall;
  - continue on error by default; `--fail-fast` stops at the first non-ok item;
  - 3 consecutive exit-3 items stop the run; a global local error stops it; a bad `io.out` stops it;
  - the read-only DIR pre-flight runs per item with an empty sub-dir.
- **File-name collisions:** all titles go flat into DIR, using MangaDex's titles. Two different titles that sanitise to the same name collide. The first item to claim a path owns it (`OutputRegistry`). The later item's file is "foreign" to it and left alone with `download`'s warning plus the collision warning. The item status is unchanged (as in `batch`). Nothing is overwritten, even with `--overwrite`; the registry is keyed on the path. A re-run in the same order behaves the same.
- **Interrupt:**
  - during the list fetch: Errc::interrupted, nothing printed on stdout, nothing created, no JSON, then 130/143;
  - inside an item, or between items: exactly as in `batch` (no partial or temp files, no further items, no JSON, no closing line).
- **`--dry-run`:** fetches the list and runs each title's plan requests (manga, feed, covers, as `batch --dry-run` does). It downloads no images, prints the planned paths, creates nothing (not even DIR), and aggregates the same way.
- **Re-runs / idempotency:**
  - running the same command again downloads titles added to the list since the last run, and updates grown files as `download` does;
  - titles removed from the list are not touched (their files stay);
  - no state is kept between runs.
- **Large input:** at most one 16 MiB response, at most 10 000 titles. Memory per item is as in `download`.

## Data & state

- **Reads:** the list over HTTPS (fixture mode: `<root>/api/list/<id>.json`), and existing `.epub` files in DIR as `download` does.
- **Writes:** only `download`'s outputs in DIR, through the existing atomic `OutputDir` publish (temp → fsync → link/rename). No state file, cache, config, XDG or TMPDIR use. No secrets.
- **Threat model.** The list is third-party data: anyone can publish a list, and the response is untrusted.
  - A list can only name titles. It cannot set options, directories or file titles, so it has strictly less power than a `batch` CSV.
  - Its worst effects are load (up to 10 000 titles, documented like `batch`'s "a long list is a long run") and file names, which come from MangaDex titles exactly as in `download`.
  - Every id is checked with `is_uuid` before use. The name is display-only.
  - Symlinks inside DIR are followed (batch's OD1); the help and README say so.
  - There is no process spawning, no env read and no new file-descriptor path. The fixture transport is unchanged.
  - `security-reviewer` still reviews it, because it adds untrusted JSON input.

## Engine reuse (for the planner; names are suggestions)

- **Core, new** (`include/mangadex_dl/mdlist.hpp`, `src/core/mdlist.cpp`):
  - `ListId`;
  - `struct CustomList { ListId id{}; std::optional<std::string> name{}; std::vector<MangaId> titles{}; }`;
  - `parse_custom_list`;
  - `list_request_url(api, id)`;
  - `plan_list(const CustomList&, const BatchSettings&) -> std::vector<BatchJob>`, with `line` = position, `dir` = DIR, `options` = base plus the manga, `raw` = defaults plus the manga string, and duplicates marked.
- **Core, changed:**
  - `ApiClient::fetch_custom_list(const ListId&, std::stop_token)`. 404 → Errc::not_found with the message above; everything else goes through the existing status mapping. This touches `include/mangadex_dl/api_client.hpp`, a **serialization point**.
  - `include/mangadex_dl/batch.hpp` (a **serialization point**):
    - `BatchSettings` gains `enum class BatchSource { csv, mdlist }` (default `csv`);
    - the closing message, the duplicate warning and the collision advice take their noun from it ("line" or "item");
    - `batch_to_json` gains an optional list summary, so it emits `list` only for `mdlist`.
  - `validate_batch_args` gains the list-URL check. The `download` MANGA hint lives in `options.cpp`.
- **App:**
  - Move phase 2 of `run_batch_command` and `ConsoleTitleRunner` out of `cmd_batch.cpp` into `src/app/cmd_batch_jobs.{hpp,cpp}`. The `cmd_` prefix keeps it inside the `CONFIGURE_DEPENDS` glob, so there is no CMake edit. This is a pure move; `batch`'s output stays byte-identical, and every existing batch test passes unchanged.
  - `ConsoleTitleRunner::row_prefix` depends on the source: `<label>:<line>: ` for `csv`, `list:<n>: <uuid>: ` for `mdlist`.
  - `cmd_mdlist.cpp` = validate → handlers → backend → fetch → plan → the shared phase 2.
  - `cli.cpp` (a **serialization point**): one registration line, the `(commands: …)` list, and the `--title` ExtrasError check for `batch`/`mdlist`.
  - `exit_help.hpp` rows 4 and 7; `exit_codes.hpp` comments (a **serialization point**); the `cmd_download.cpp` and `cmd_batch.cpp` footer lines.

## How `follows` fits in later (step 2, not designed here)

- **A sibling subcommand `mangadex-dl follows [DIR] [OPTIONS]`.** Same options, same engine path. The source changes from `fetch_custom_list` to a paginated `/user/follows/manga` fetch. The JSON is the same, with `source: "follows"` and an additive `follows` object; prefixes are `follows:<n>: <uuid>: `.
- **Auth options and credential precedence** (config/env, which will then need the CLI11-order statement) live on `follows`. They can later be added to `mdlist` too, to read your own *private* lists. That is additive: anonymous `mdlist` behaviour, including "private → 4", stays as specified here.
- **Nothing in this spec reserves or blocks anything.** It doesn't take the word `follows`, reads no credentials, env vars or config, and adds no exit code (codes 8 and up stay free for auth failures).
- **Rejected slots:** a magic `mdlist follows` value (it would mix auth options into an anonymous command) and `batch --follows` (the FILE positional trap).

## Acceptance criteria

1. `[unit]` `ListId::parse` accepts:
   - a bare UUID in upper or mixed case, lowercased;
   - `https`/`http`/no scheme, `www.`, an upper-case host;
   - `/list/<uuid>`, `/<slug>`, a trailing `/`, `/<slug>/`, `?q`, `#f`.

   It rejects: empty, `-`, `titles.csv`, `/list/<uuid>/a/b`, `//`, `mangadex.org.evil`, a port, a space or control byte, 35 or 37 characters. `/title/<uuid>` gets the distinct "is a title" error. No message contains the input.
2. `[unit]` `MangaId::parse`'s existing tests pass unchanged after any shared-helper refactor. `looks_like_list_id` agrees with `ListId::parse`.
3. `[unit]` `parse_custom_list` accepts a fixture-shaped document:
   - manga in array order; `user` and unknown relationship types ignored; extra fields ignored;
   - name absent or null → null; a 2000-byte name is cut to ≤1024 bytes on a UTF-8 boundary;
   - an upper-case manga id is canonicalised; zero manga → empty.
4. `[unit]` `parse_custom_list` rejects, with Errc::network and a fixed message containing no response bytes:
   - not JSON, a BOM, a NUL, depth 65, over the separator cap, over 16 MiB;
   - missing `data`/`id`/`type`/`relationships`; `type` other than `custom_list`; `data.id` other than the requested id;
   - a manga id `../x`, 35 characters long, or not a string;
   - 10 001 manga relationships (the message names 10000).
5. `[unit]` `ApiClient::fetch_custom_list` over FakeTransport:
   - the URL is exactly `<api>/list/<id>`;
   - 200 → the list; 404 → Errc::not_found with the message; 5xx retried, then Errc::network; 429 per policy; 403/401 per V1;
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
16. `[e2e]` The `mdlist --help` golden covers the synopsis `mangadex-dl mdlist [OPTIONS] LIST [DIR]`, the LIST footer, the examples and the exit table, and has no `--api-base`. Re-bless requested: `download.help`, `batch.help`, `edge.help`, new `mdlist.help`.
17. `[e2e]` L1 on `clean` → 0, the paths, sound EPUBs, no `.part-`. The bare UUID, the upper-case UUID, the full URL with slug, the `www.` form and the no-scheme form all give identical stdout.
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
31. `[human]` The user's follows copied into a real public MDList (≥3 titles), run live: correct files, order as on the website, no 429 storm, readable `list:<n>:` lines.
32. `[human]` A private list, run live: the status MangaDex gives an anonymous request and the wording of the exit-4 message (closes V1 if the fact-checker could not).
33. `[human]` The list URL copied from the browser's address bar and from the site's share control is accepted as-is.
34. `[human]` Ctrl-C during the list fetch and in the middle of the list, in a real terminal → 130, no partial or temp files, a re-run resumes.
35. `[human]` Add a title to the list on the website, run again → only the new title's files appear. Remove a title → its files are left alone.

**E2E fixtures needed.** These are generated, not committed. Add a `FixtureList` (id, name, visibility, ordered manga ids, extra relationships) to `tests/support/mdx_fixture.hpp`, and have `write_fixture_tree` write `<root>/api/list/<id>.json`. The JSON shape is `{"result":"ok","response":"entity","data":{"id","type":"custom_list","attributes":{"name","visibility":"public","version":1},"relationships":[{"id","type":"manga"},…,{"id","type":"user"}]}}` (shape per V3/V6). The documents are additive files, so existing cases and the twelve-tree count are unchanged. Covers stay one title per tree (`api/cover/offset-*.json` is not keyed by manga), so every list mixes one real title with unknown ids.

| Id | Tree | Name | Manga relationships |
|---|---|---|---|
| L1 `a0000000-0000-4000-8000-000000000001` | `clean` | "Clean List" | 1111…, plus a `user` relationship |
| L2 `…000000000002` | `clean` | "Clean And Unknown" | 1111…, 6666… |
| L3 `…000000000003` | `clean` | "Empty" | none (only `user`) |
| L4 `…000000000004` | `clean` | "Twice" | 1111…, 1111… |
| L5 `…000000000005` | `clean` | "Bad Id" | `../x` |
| L6 `…000000000006` | `clean` | (file L6, `data.id` = L1) | 1111… |
| L8 `…000000000008` | `nochapters` | | 5555…, 6666… |
| L9 `…000000000009` | `incomplete` | | 4444… |
| L10 `…00000000000a` | `bulk` | | 3333… |
| unknown `…0000000000ff` | `clean` | | no file (404) |

Also document the list files in `tests/e2e/inputs/README`. The new `tests/e2e/cases/mdlist.cmake` is globbed, and `_edge.cmake` gains `mdlist-*` cases. Private-list and 403/401 behaviour can't be expressed in fixture mode (it only gives 200 or 404), so it is covered by FakeTransport at `[unit]` (AC5) and by a person (AC32).

## README and docs

- **Intro:** add "`mangadex-dl mdlist` does the same for every title in a public MangaDex list."
- **Contents:** add a link to the new section.
- **New section "A MangaDex list: `mdlist`",** after "Many titles: `batch`":
  - the synopsis and a table: LIST forms, DIR, "the options are `batch`'s, with no `--title` and no per-title settings";
  - "No login: the list must be public. To download the titles you follow, add them to a public MDList; reading follows directly needs a login, which `mangadex-dl` doesn't support yet";
  - order, dedupe and the 10 000 cap; flat DIR and the collision warning;
  - stderr: `list:<n>: <uuid>:` and the "items" wording;
  - `--json`: batch's document plus `list`, with `line` = position;
  - exit status: 4 and 3 at list level vs aggregate, and how to tell them apart;
  - re-runs pick up new titles; a short examples block.
- **Exit status table:** rows 1 ("For `batch`, … for `mdlist`, no usable libcurl …" is unchanged in substance), 3 ("… or the list response"), 4 ("The title, or for `mdlist` the list, does not exist on MangaDex or the list is private") and 7 ("`batch` and `mdlist`").
- **Output:** "Usage errors … (or `mangadex-dl mdlist --help` …)". "Only `batch -` reads stdin" stays true.
- **Limits:** "`mdlist` downloads one list per run, at most 10 000 titles."
- **Network behaviour:** "`mdlist` makes one extra API request, for the list."

## Out of scope

Login and reading follows (step 2); private lists; several lists per run; a list from a file or stdin; per-title options or subdirectories (a possible later `--dir-per-title`); `/list/{id}/feed` and chapter-level selection across a list; creating or editing lists; exporting a list's ids to CSV (a later read-only command could do it; `--dry-run --json` works today but makes every title's plan requests); legacy numeric list ids; parallel titles; caching the list; completions and man page; PDF.

## Decisions at the design gate (user, 2026-10-26)

- **Q1. Command name:** `mdlist`.
- **Q2. List not found or private:** reuse exit 4 and reword its row; no new code.
- **Q3. Layout of DIR:** flat by default, as `batch` does, **plus an opt-in per-title subdirectory flag in this same change** (spec amendment pending).

## [VERIFY] items

- **V1 (API, live):** what an anonymous `GET /list/{id}` returns for a **private** list: 404, 403 or 401, and the body (`result: "error"`, `errors[].status/title/detail`). Can it be told apart from a rate-limit 403? Exit 4 vs 3 depends on this.
- **V2 (API):** is `relationships` on `/list/{id}` complete for large lists (no cap, no pagination, no `limit`/`offset`)? What is the largest list MangaDex allows?
- **V3 (API):** relationship types present: `manga` plus the owner `user`, anything else? Shape without includes: `id` and `type` only, perhaps `related`; no `attributes`.
- **V4 (API/website):** is the `relationships` order the order the website shows (insertion order)?
- **V5 (website):** the list URL forms: `https://mangadex.org/list/<uuid>/<slug>`, with or without slug, `?`/`#` from the share button. Does the site call these "MDList(s)", as the help text assumes?
- **V6 (API):** `/list/{id}` needs and takes no `includes[]` or `contentRating`. `data.type == "custom_list"` and `data.id` is lowercase in live responses. A live sample to base the fixture on. It counts as a normal API request for rate limits.
- **V7 (CLI11, installed 2.7.x):**
  - the shared option helpers register on a third subcommand without `ConstructionError`;
  - globals after LIST and DIR work through `fallthrough()`;
  - `App::remaining(true)` shows a leftover `--title`/`--title=X` once `mdlist` was reached (for the generalised ExtrasError message);
  - help ordering matches the batch golden.
- **V8 (repo):**
  - the phase-2 extraction keeps `batch` byte-identical (every batch case in tests/cli, tests/e2e and `_edge.cmake`, plus the goldens);
  - `with_remedy` leaves the over-cap and malformed-list messages without the network remedy, or the plan says otherwise;
  - `check_row_dir` with an empty sub-dir behaves as for a `batch` row without a `dir`.
- **V9 (repo):** the fixture transport maps `file://<root>/api/list/<id>` (no query) to `<root>/api/list/<id>.json`. Reading `map_url` says yes: it appends `.json` to the last `api/` component when there is no `offset`. Confirm with a test.
- **V10 (repo):** the list fetch's `ApiClient` (separate limiter, destroyed before item 1) plus a fresh client per item is acceptable for the "≤4 req/s" claim in the README. It is one extra request, the same per-row-client pattern `batch` already has.
