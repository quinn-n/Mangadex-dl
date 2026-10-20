# Feature spec: `mangadex-dl batch`, downloading many titles listed in a CSV file

Status: APPROVED 2026-10-18 (⏸ design gate): Q1 `batch` subcommand, Q3 uniform-else-7, Q4 stop after 3 network failures, Q2 and Q5–Q8 recommended defaults. Produced by `cli-designer`.
rev1 (fact-check) 2026-10-18: every [VERIFY] item CONFIRMED (plan clusters A–D). Amendments are marked
**rev1 (fact-check)** inline: the bounded read, the security-reviewer's OD1 tightening of `dir`, and one
**behaviour change for the plan gate**: a row-scoped `dir` failure fails only that row, and the batch continues.
rev2 (board polish) 2026-10-19: message and help wording only, approved after the feature board; marked
**rev2 (board polish)** inline and summarised at the end. No exit code, flag or JSON field changes.
**Re-runs (2026-10-20, merge with skip-existing):** a batch row now re-runs exactly as `download`
does under [`skip-existing-spec.md`](skip-existing-spec.md): files that hold every chapter are
skipped, grown files are rebuilt with page reuse, diverged or foreign files are left alone with a
warning. This supersedes the help footer's re-run paragraph (line "Running the same command
again ..."), the `--overwrite` help (now "Re-download files even if up to date", shared with
`download`), and the aggregated "N files already exist" warning of "Input & output" and AC13: the
end-of-batch line is now `download`'s note `N files are already up to date; pass --overwrite to
download them again` (no `warning:`, never colored, not with `-q`), counting only files left alone
as up to date (`existing == complete`). JSON per-row reports gain `existing`, `new_chapters` and
`reused_chapters` (additive, `format_version` 1); `files.exists` counts every file left alone,
diverged and foreign ones included. No exit code or flag changes.

**Intent:** run the existing `download` engine once for each title listed in a CSV file (or on
stdin), in file order. The CSV can override a few per-title options on each row. The run ends with
one aggregate exit code and, with `--json`, one document.

**Assumptions:**
- "Works" means MangaDex titles, the same input as `download`'s MANGA.
- One output root (DIR). Rows may name a subdirectory under it.
- Titles run one after another. `--jobs` still parallelises the images inside each title.
- The tool has no config file and no env-var options today, and this feature adds neither.

## Decision: a new subcommand, not `download --from-csv`

`mangadex-dl batch FILE [DIR]`:
1. `download`'s contract stays byte-stable (MANGA is required; `download --from-csv x.csv ~/Manga`
   would bind `~/Manga` to MANGA and trip the "DIR looks like a manga id" check).
2. The `--json` document has a different shape (a list of per-title results).
3. Exit codes aggregate across titles and need one new code.
4. The CSV format needs its own help footer.

`download` changes only in one optional footer line pointing to `batch` (Q2) and in the shared
exit table gaining row 7.

## Command surface (CLI11)

- `mangadex-dl batch [OPTIONS] FILE [DIR]`, a top-level subcommand.
  - `src/app/cmd_batch.{hpp,cpp}` (`register_batch`) + one line in `cli.cpp`.
  - Options shared with `download` bound by one helper so help strings stay identical
    (suggested `src/app/cmd_download_options.cpp`).
  - Core: `include/mangadex_dl/batch_csv.hpp` (CSV parser, pure, bytes → `Result`) and
    `include/mangadex_dl/batch.hpp` (merge, dedupe, aggregation, JSON document).
- Positionals: `FILE` (required; path or `-` for stdin; not `CLI::ExistingFile`), `DIR`
  (optional, default `.`, created if missing except with `--dry-run`).
- Options = run-level defaults, same name/short/type/default/validator/help as `download`:
  `--volumes`, `--loose-chapters`, `--layout`, `-l,--lang`, `--group` (repeatable),
  `--direction`, `--data-saver`, `--overwrite`, `-n,--dry-run`, `-j,--jobs`, `--no-report`,
  hidden `--api-base`.
- New: `--fail-fast` — "Stop at the first title that fails" (**rev2 (board polish)**: the help
  text already said this; the spec now matches it).
- Not on the command line: `--title` (per-row column only; one title for all rows collides).
  **rev2 (board polish):** `batch FILE [DIR] --title X` (also `--title=X`) is exit 2 with
  `batch has no --title option; give each row a 'title' column instead` plus the batch hint,
  instead of CLI11's "The following argument was not expected: --title".
- CLI-only (no CSV column): `--api-base`, `--overwrite`, `--dry-run`, `--jobs`, `--data-saver`,
  `--no-report`, `--fail-fast`. A third-party CSV can never redirect API traffic, replace files or
  change load.
- Global flags (`--json`, `-q`, `-v`, `--color`) work after the subcommand via `fallthrough()`.
- No prompts.
- Semantic usage checks after parse (exit 2 + `Run 'mangadex-dl batch --help' for more
  information.`): CLI-level option validators; FILE looks like a manga id (`FILE looks like a
  MangaDex title; use 'mangadex-dl download <title>' for one title`); DIR looks like a manga id;
  empty FILE or DIR.
- Root text changes (stderr only): `unknown command 'x' (commands: download, batch)`;
  `missing command; try 'mangadex-dl --help'`. Update `tests/cli/cli_test.cpp:77` and
  `tests/cli/download_cli_test.cpp:588`.

Help:
```
FILE REQUIRED   CSV file listing the titles ('-' reads stdin)
DIR [.]         Output directory (created if missing)
--fail-fast     Stop at the first title that fails

CSV file:
  UTF-8, comma-separated, "double quotes" around fields that contain commas.
  Blank lines and lines starting with '#' are ignored. Without a header row,
  each line is one title (UUID or https://mangadex.org/title/<uuid>). With a
  header row, these columns are recognised (any order; an empty cell means
  "use the command-line value"):
    manga            the title, as MANGA in 'download' (required)
    title            --title for this row
    dir              subdirectory of DIR for this row
    volumes, loose-chapters, layout, lang, group, direction
                     as the options; group lists NAME|NAME in priority order
    note             ignored
Examples:
  Everything in the list, into ~/Manga:
    mangadex-dl batch titles.csv ~/Manga
  Show what would be written, without downloading anything:
    mangadex-dl batch titles.csv ~/Manga --dry-run
  A list from another command:
    grep -v '^#done' titles.csv | mangadex-dl batch - ~/Manga

Running the same command again skips the files that already exist, so an
interrupted or incomplete batch can simply be restarted.

Symlinks inside DIR are followed: don't point DIR at a directory that others
can write to. Each title costs several API requests: a long list is a long run.
```
**rev1 (fact-check):** the last footer paragraph is new (security review). The wording is a draft for cli-ux.

## CSV format

- **Encoding:** UTF-8 only. Leading UTF-8 BOM skipped. UTF-16 BOM → exit 2 `<file>: is UTF-16;
  save it as UTF-8`. Whole file valid UTF-8, no NUL, else exit 2 `<file>:<line>: not valid UTF-8`.
- **Line endings:** LF and CRLF; last line may lack a newline. Bare CR outside quotes → exit 2.
- **Quoting:** RFC 4180, `,` only. `""` inside quotes is `"`; `,`/CR/LF literal inside quotes.
  `"` in an unquoted field, or junk after a closing quote → error. Unterminated quote names the
  record's start line.
- **Trimming:** ASCII spaces/tabs trimmed from every field after unquoting.
- **Comments/blank lines:** record starting with `#` (outside quotes) is a comment; empty or
  whitespace-only lines skipped.
- **Header detection:** first non-comment record is a header iff its first field is not a valid
  MangaDex UUID/URL. Names match case-insensitively (ASCII): `manga, title, dir, volumes,
  loose-chapters, layout, lang, group, direction, note`. `manga` required. Unknown/empty/duplicate
  names → exit 2 listing known columns (echo `terminal_safe`, capped at 32 bytes). `;` without `,`
  in the header adds `; the separator is ','`.
- **Headerless:** one meaningful field per record; empty extras ignored; non-empty extra → exit 2
  `more than one column; add a header row (manga,title,...)`.
- **Ragged rows (header):** short rows → missing cells empty; long rows allowed only if extras are
  empty, else exit 2 `<n> fields but the header has <m>`.
- **Per-cell validation** through existing core validators. Diagnostic `<file>:<line>: <column>:
  <message>`. `manga` cell never echoed. `group` split on `|`, trimmed, empty entry is an error;
  replaces the CLI `--group` list. **rev1 (fact-check):**
  - A group name containing a control character is an error.
  - Group names never reach a URL; they are only matched locally.
- `dir` (**rev1 (fact-check)**, validated on the trimmed cell):
  - relative, and valid UTF-8;
  - no NUL/C0/DEL and no C1 (U+0080–U+009F);
  - none of `terminal_safe`'s unsafe code points: U+061C, U+200B–U+200F, U+202A–U+202E, U+2028/2029,
    U+2060–U+206F (incl. the U+2066–U+2069 isolates), U+FEFF. `dir` is printed raw to stdout;
  - no `\`; not absolute; no empty/`.`/`..` segments;
  - ≤ 32 segments, ≤255 B per segment, ≤1024 B total.
  - It resolves to `DIR/<dir>`.
- **Limits:** file ≤ 1 MiB (**rev1 (fact-check):** opened `O_NONBLOCK`, so a FIFO without a writer never blocks;
  the type is checked with `fstat`: regular files, FIFOs/pipes and terminals (and a socket on stdin) are read, while a directory
  or another type is exit 1; `O_NONBLOCK` is cleared for FIFOs/terminals; `st_size` is only a size hint;
  the cap is enforced on the bytes actually read, at most cap+1, for every source);
  ≤ 10 000 title rows; ≤ 4 KiB per field (`title` keeps 1024). Exceeded → exit 2 naming the limit.
- **Duplicates:** a row whose effective (merged) options equal an earlier row's → status
  `duplicate`, warning `<file>:<line>: same as line <k>; skipped`.
- **Diagnostics:** `<file>` is FILE via `terminal_safe` or `<stdin>`; `<line>` is the 1-based
  physical start line. Phase 1 reports every error up to 20, then `... and <n> more errors`; exit
  2; nothing written, no request made.
- **Empty list:** exit 0 + warning `<file>: lists no titles` (suppressed by `-q`); `--json` prints
  `titles: []`.

## Input & output

- **Precedence per field:** non-empty CSV cell > CLI flag > built-in default. No config/env, so
  CLI11's CLI > config > env > default has nothing to arbitrate. CLI-level values are validated
  even when every row overrides them.
- **stdin:** only for `-`. If stdin and stderr are TTYs (and not `-q`): `mangadex-dl: reading
  titles from the terminal; end with Ctrl-D`. Closed stdin → exit 1 `cannot read <stdin>: …`. **rev1
  (fact-check):** nothing may open a file descriptor before stdin is read, so slot 0 stays free.
- **stdout (human):** exactly `download`'s data — one path per written (or planned) file,
  concatenated across rows in file order.
- **stderr:** row-scoped lines carry `<file>:<line>: ` after `error:`/`warning:`. Per-row "N files
  already exist" aggregated into one end-of-batch warning. Cross-row collision warning:
  `<file>:<line>: <path> was already written by line <k>; give one of them a 'title' column`.
  (T5 impl: the first row to claim a path — written *or* exists — owns it, so a re-run of a
  colliding list still warns.)
  Empty on full success. Otherwise one closing `error:` line (even with `-q`/`--json`), line list
  capped at 10:
  - `<n> of <m> titles did not complete (lines 4, 9, 12); run the same command again to retry them`
    (**rev2 (board polish):** only when at least one counted non-ok row is `network` or
    `incomplete`; otherwise `<n> of <m> titles did not complete (lines …); see the errors above`)
  - dry run: `<n> of <m> titles would fail (lines …)`
  - stopped early: `stopped at line <k>: <reason>; <r> titles not run`
- **`-v`:** `<file>:<line>: <uuid>` at each row start, then the engine's lines.
- **Progress:** TTY stderr only, via existing `output_policy`: `(2/15) [1/2] Vol. 1: 3/5 pages`.
- **`--json`:** one document at the end, when phase 2 ran and was not interrupted (incl. exits 1
  stopped-early, 3–7). Not for phase-1 errors or interrupts. `ordered_json`, indent 2,
  `error_handler_t::replace`, never ANSI.
  ```
  format_version: 1
  dry_run: bool
  source: string                 FILE as typed ("-" for stdin)
  titles: [ {
      line: int
      manga_id: string           canonical lowercase UUID
      dir: string                effective output directory, as typed (DIR[/dir])
      status: "ok"|"not_found"|"no_chapters"|"incomplete"|"network"|"failed"|"duplicate"|"not_run"
      exit_code: int|null        what `download` would return (0,1,3,4,5,6); null for duplicate/not_run
      error: string|null
      duplicate_of: int|null
      report: object|null        the `download --json` document for this row, when a RunReport exists
  } ],
  summary: {
      titles: {total, ok, not_found, no_chapters, incomplete, network, failed, duplicate, not_run}
      files:  {planned, written, exists, failed}
  }
  ```
- **Color:** stderr prefixes only, via existing `resolve_color`. stdout never colored.
- **Locale:** locale-independent output.

## Exit codes

New: **7 `mixed_failures`** — "batch: titles did not complete for different reasons (more than
one of 3, 4, 5, 6); the per-title results say which". Plus `Errc::mixed_failures`, a help-footer
row and README rows.

| Outcome | Code |
|---|---|
| every row ok/duplicate, or empty list | 0 |
| **rev1 (fact-check):** a global local error in a row (DIR itself unusable, ENOSPC, EIO, EROFS, anything unclassified): batch stops, rest `not_run` | 1 |
| **rev1 (fact-check):** a row-scoped `dir` failure (DIR/<dir> runs into a regular file or a dangling symlink, EACCES/ENOTDIR/ELOOP below DIR): that row is `failed` (exit_code 1) and the batch continues | per the rows below |
| FILE unreadable | 1 |
| stdout write error | 1 via `finish_stdout` (a nonzero aggregate stays) |
| CSV parse/validation, CLI usage, any `ParseError` | 2 |
| every non-ok row failed the same way (all 3 / 4 / 5 / 6; **rev1 (fact-check):** a row-scoped `failed` row counts as 1, so all failed → 1) | that code |
| non-ok rows failed in more than one way (**rev1 (fact-check):** e.g. failed + 4 → 7) | 7 |
| SIGINT / SIGTERM | 130 / 143 (re-raised by `main`) |

`duplicate`/`not_run` never count. A one-row CSV exits exactly as `download` would.

## Behaviour

- **Phases:** validate CLI options → read FILE (bounded) → parse/validate/merge every row (pure
  core) → dedupe. Only then interrupt handlers, backend, network, filesystem.
- **Execution:** rows sequential in file order via unchanged `run_download`; one
  `ScopedInterruptHandlers` and one `Backend` for the batch; a fresh `DownloadServices` and a
  row-prefixed `ConsoleObserver` per row.
- **Continue on error** by default (3/4/5/6 recorded, next row starts). `--fail-fast`: stop at the
  first non-ok row. **rev1 (fact-check):**
  - A global local error in a row always stops the batch.
  - A row-scoped `dir` failure, found by a read-only pre-flight before that row downloads anything, fails only
    that row. Three consecutive exit-3 rows stop the batch
  (`stopped at line k: 3 titles in a row failed with network errors; …`). Stop if `io.out` is bad.
- **Interrupt:** in-row → `Errc::interrupted` (no partial/temp files), no further rows; checked
  before each row. No JSON, no summary; printed paths stay; exit 130/143. During phase 1 the
  default disposition applies.
- **`--dry-run`:** plans every row, prints planned paths, creates nothing; same aggregation.
- **Data/state:** writes only `download`'s outputs under DIR or DIR/<dir> via the existing atomic
  `OutputDir` publish. No state file. Threat model: CSV is untrusted; limited to titles,
  selection/lang/layout/direction, file title, and a lexically-contained subdirectory. Existing
  symlinks inside DIR are followed (OD1). **rev1 (fact-check):**
  - OD1 is confirmed by security-reviewer with the tightened `dir` rules.
  - Help and README say "symlinks inside DIR are followed; don't point DIR at a directory others can write to".
  - They also say the API load of a long list (up to 10 000 titles) is the user's responsibility.
  - A row that ends 4, 5 or with a row-scoped failure leaves no directory behind.

## Acceptance criteria

1. `[unit]` Parser accepts: BOM; CRLF/LF; no final newline; quoted `,` `""` CR LF; trimming;
   comments; blank lines; headerless one-column; header any order, case-insensitive; ragged short
   rows; empty extra fields.
2. `[unit]` Parser rejects with the right (start) line: unterminated quote, quote in unquoted field,
   junk after closing quote, bare CR, invalid UTF-8, NUL, UTF-16 BOM, unknown/empty/duplicate
   columns, no `manga`, non-empty extra field, headerless two columns, `;` hint.
3. `[unit]` Limits: >1 MiB, >10 000 rows, field >4 KiB → `Errc::usage` naming the limit; bounded
   read never reads more than cap+1 bytes from a non-regular source. **rev1 (fact-check):** also a
   directory, a FIFO without a writer (returns at once, empty), a FIFO with a writer, `/proc/self/status`
   (`st_size` 0) and a file over the cap.
4. `[unit]` Merge: cell overrides CLI, empty inherits; `group` replaces, `|` split/trim; both
   selections `none` after merge is an error; `dir` containment accept/reject set. **rev1
   (fact-check):** the rejects include U+009B, U+0085, U+202E, U+2028, `\`, NUL and 33 segments. Group
   `A&B|C#D` is accepted; a group name with a newline is rejected.
5. `[unit]` Diagnostics `<file>:<line>: <column>: <message>`; `manga` never echoed; other cells
   `terminal_safe` + 32-byte cap; list capped at 20 + `... and N more errors`.
6. `[unit]` Dedupe: identical effective rows → `duplicate` + `duplicate_of`; rows differing only in
   `lang`/`dir` both run.
7. `[unit]` Aggregation with a fake per-row runner: all ok → 0; uniform → that code; mixed → 7;
   a fatal row exit 1 stops (rest `not_run`) → 1; duplicate/not_run ignored; 3 consecutive exit-3 rows
   stop; `--fail-fast` stops at first non-ok. **rev1 (fact-check):** a row-scoped `failed` row does not
   stop; all failed → 1; failed + 4 → 7.
8. `[unit]` Batch JSON has exactly the listed keys/types; `report` byte-equal to the `download`
   document; valid JSON for invalid-UTF-8 `source`/`dir`; no ESC byte.
9. `[cli]` Fixture `clean`, CSV with the clean id → exit 0, stdout = `download`'s paths, stderr
   empty, files exist.
10. `[cli]` Simulated interrupt before row 2 → row 2 never starts, no JSON, no summary, no temp
    file; before row 1 → nothing written.
11. `[cli]` Phase-1 errors (CSV error, bad `--lang`, FILE/DIR look like a manga id, empty FILE) →
    exit 2, empty stdout, hint line, no transport created, no DIR created.
12. `[cli]` Cross-row collision (same `title` into same dir) → second row `exists`, warning names
    both lines; still warns with `--overwrite`.
13. `[cli]` Row-scoped stderr lines carry `<file>:<line>: `; aggregated "already exist" warning
    once at end; `-q` suppresses warnings but keeps the closing error line.
14. `[e2e]` `batch --help` golden (synopsis, CSV footer, exit table incl. 7, no `--api-base`).
    Re-bless: `download.help`, `edge.help`, new `batch.help`.
15. `[e2e]` Headerless CSV (clean id) → exit 0 with paths; same file on STDIN with `-` identical.
16. `[e2e]` `nochapters` tree, rows 5555… and 6666… → exit 7, empty stdout, row-prefixed 5 and 4
    lines + closing summary; `--json` valid with `no_chapters`/`not_found`, total 2.
17. `[e2e]` Uniform: clean tree rows 1111… and 6666…6 → exit 4, clean files written. Incomplete
    tree single row → exit 6, failed file absent, no `.part-` left.
18. `[e2e]` `--fail-fast` nochapters [5555, 6666] → exit 5, second row `not_run`.
19. `[e2e]` `--dry-run` writes nothing, creates neither `out` nor row `dir`. Goldens
    `batch.dry-run`, `batch.json-dry-run` requested.
20. `[e2e]` Row `dir` → files under `out/<dir>/`; `../x` or `/abs` → exit 2 `:<line>: dir:`.
21. `[e2e]` BOM + CRLF + quoted title with comma (`inputs/batch/excel.csv`, byte-exact) → exit 0,
    title used in file name. Unterminated quote → exit 2 `…excel-bad.csv:3: `.
22. `[e2e]` Exit 1: FILE missing; FILE is a directory; DIR is a regular file (`stopped at line`,
    rest `not_run`). **rev1 (fact-check):**
    - A row whose `dir` runs into a regular file fails alone: exit 1, later rows still run, no
      `stopped at line`.
    - A not-found row with a `dir` leaves no directory.
23. `[e2e]` `--api-base http://127.0.0.1:1` with 4 rows → exit 3 after 3 rows, 4th `not_run`
    (label `needs-libcurl`).
24. `[edge]` `STDIN NULL` with `-` → exit 0, `lists no titles`, empty stdout; `--json` `titles: []`.
25. `[edge]` `STDIN CLOSED` with `-` → exit 1, `cannot read <stdin>`, never 70.
26. `[edge]` `--dry-run > /dev/full` → exit 1, `mangadex-dl: write error`.
27. `[edge]` Bulk-tree CSV `--layout chapters --dry-run | head -1` → silent SIGPIPE; also with
    parent ignoring SIGPIPE.
28. `[edge]` Invalid-UTF-8 FILE under `LC_ALL=C` → exit 1, `?`, not 70. CSV with `0xFF` → exit 2.
    Invalid-UTF-8 DIR with `--json --dry-run` → valid JSON. `Zoë` DIR verbatim under C.UTF-8 and C.
29. `[edge]` `--json` with `CLICOLOR_FORCE=1` → no ANSI; piped stderr with failing row → no ANSI.
30. `[tty-pty]` Progress `(1/2) [1/2] Vol. 1:` under a pty; none with `--json`/`-q`/`TERM=dumb`;
    `NO_COLOR` uncolored `error:`.
31. `[human]` Real batch of ≥3 live titles: correct files, no 429 storm, readable progress/prefixes.
32. `[human]` Ctrl-C mid-batch in a real terminal: 130, no partial/temp files, re-run resumes.
33. `[human]` CSVs exported from LibreOffice Calc and Excel accepted as-is.
34. `[human]` `mangadex-dl batch - ~/Manga` at a terminal: hint appears, pasted URLs + Ctrl-D run.

## Out of scope

Parallel titles; per-row data-saver/overwrite/jobs/dry-run/no-report/api-base; TSV/`;`;
non-UTF-8; chapter URLs or search by name; state file / write-back; CSV from URL;
`download --from-csv` alias; config/env defaults; `--check` mode; JSON Lines; completions/man;
PDF.

## Open questions (recommended default first)

- Q1. Shape/name: subcommand `batch FILE [DIR]` (alt: `download --from-csv`, `download-list`,
  `import`).
- Q2. Footer line in `download --help` pointing to `batch`: yes.
- Q3. Aggregate exit: uniform code if all failures alike, else 7 (alt: always 7).
- Q4. Stop after 3 consecutive network-failed titles: yes.
- Q5. `--json`: one document at the end (alt: JSON Lines).
- Q6. `--check` mode: defer.
- Q7. `group` separator `|`; keep the ignored `note` column.
- Q8. Empty list: exit 0 + warning (alt: exit 5).

## [VERIFY] items

- V1 (CLI11): lone `-` reaches required positional FILE; global flags after positionals work via
  `fallthrough()`.
- V2 (CLI11): shared bind helper registers identical options on two subcommands without
  `ConstructionError`; help ordering as goldens expect.
- V3 (repo): `exit_status_footer()` is the single table, so row 7 changes `download.help` and
  `edge.help`.
- V4 (repo): `.gitattributes` would normalise CRLF/strip BOM in `tests/e2e/inputs/batch/*.csv`?
- V5 (repo): E2E `STDIN batch/x.csv` resolves subdirs; `{{INPUTS}}` works in ARGS; no
  signal-sending keyword.
- V6 (repo): `run_download` drivable per row with fresh `DownloadServices` and shared `Backend`;
  MD@H reporter starts/stops per call without leaking threads; `report_to_json` JSON-value form.
- V7 (repo): `ConsoleObserver` can take a row prefix/counter and suppress its per-row "already
  exist" notice; `download` output unchanged.
- OD1 (security-reviewer): lexical containment of `dir` + path-based `OutputDir::open` acceptable
  vs an `openat`/`O_NOFOLLOW` walk.

**rev1 (fact-check):**
- V1, V2: confirmed by probe.
- V3–V7: resolved in-repo (batch-plan.md).
- OD1: confirmed with the tightening marked above.
- The bounded-read steps are confirmed and adopted.

**rev2 (board polish), 2026-10-19:** wording changes only, from the feature board's review.
- M1 (row directory pre-flight): `cannot open directory '<path>': <strerror>` for open/openat
  failures; `cannot open directory '<path>': it is a dangling symbolic link`; `cannot write to
  directory '<path>': <strerror>` for the access check. The local-failure stop reason is
  `the error above affects every title` (`stopped at line 1: the error above affects every
  title; 3 titles not run`, without the tail when nothing is left to run).
- M2 (closing line): "run the same command again to retry them" only when a retry can help (a
  `network` or `incomplete` row); otherwise "see the errors above". Dry-run wording unchanged.
- M3 (header detection): a would-be header with exactly one non-empty field that is not a known
  column (and holds no `;`) reports `<file>:<line>: '<echo>' is not a MangaDex title UUID or
  https://mangadex.org/title/<uuid> URL, nor a known column (manga, title, dir, volumes,
  loose-chapters, layout, lang, group, direction, note)`, echoed like a header name (32-byte cap,
  `terminal_safe` when printed).
- M4: `--title` on `batch` (above).
- L1: the `--help` CSV footer gains a three-line `For example:` list after the `note` row.
- L3: `batch … > /dev/full` reports the errno text like `download`
  (`mangadex-dl: write error: No space left on device`); the flush after each row records it.
- L5: lexical errors carry a remedy: `unterminated quote; the field that opens with '"' on this
  line never closes`; `unexpected text after a closing quote; quote the whole field and write
  '""' for a quote`; `a carriage return (CR) outside quotes; save the file with LF or CRLF line
  endings`.
