# `batch --dir-per-title`: one directory per title for a CSV list

Status: GATE DECIDED 2026-10-28 — **QB1 replace**, **QB2 reserve up front (first segment)**, **QB3 stay with the manga's first directory** (QB4 moot: `TitleDirRegistry::claim` and its tests are unchanged). Sections below are amended to match; where the original recommendation for QB3 differs, the "rev1 (gate)" notes win. Produced by `cli-designer`. This is the additive change foreseen by list-spec QA2: "`batch` gets the flag later as its own change; a non-empty CSV `dir` cell for a row wins over the derived name for that row". That rule is settled and not reopened here. This spec defines what "wins" means precisely (QB1) and how explicit cells interact with derived names (QB2, QB3).

Reused unchanged: the engine step `src/core/download/dir_per_title.cpp` (resolve → `-v` line → read-only `check_row_dir` → `DIR/<name>`), `Errc::row_dir`, `Plan::file_stem`, `ConsoleTitleRunner`/`ConsoleTitleDirResolver` and `run_batch`.

## Summary

`mangadex-dl batch FILE DIR --dir-per-title` puts each title of a CSV list into its own subdirectory of DIR. The subdirectory is named exactly like the title's files: `DIR/<stem>/<stem> - Vol. 01.epub`, where `<stem>` is `Plan::file_stem`, so a row's `title` cell names its directory too.
- A row with a non-empty `dir` cell keeps going to `DIR/<dir>/`, exactly as today. The cell replaces the derived name.
- Explicit `dir` names are reserved for the whole run, so a derived title never moves into a directory that a row named explicitly.

Typical user: a human with a long CSV of series who wants one folder per series for a library app (Komga, Kavita, Calibre, KOReader), without typing a `dir` cell on every row. Scripts and CI get the same stdout/`--json` contract as today.

**Friction audit.** Today, one folder per title takes N `dir` cells for N rows. With the flag it takes zero, plus one cell for each row whose folder name you want to choose. The flag is a single, value-less switch with no short form, matching mdlist.

## Command surface (CLI11)

- **Invocation:** `mangadex-dl batch [OPTIONS] FILE [DIR]`. It is unchanged except for one new flag in `src/app/cmd_batch.cpp` (`BatchCommand::bind`). There is no change to `cli.cpp`, because the subcommand is already registered.
- **New option:** `--dir-per-title`, a flag (bool, default false). Help: `Put each title in a subdirectory of DIR` (39 columns, the same string as mdlist's, so the two commands read the same).
  - It is bound to a new member of batch's own raw args (`RawBatchArgs::dir_per_title`), never to `RawDownloadArgs`. `download` never sees it.
  - No short form, no `envname`, no config key, no value.
  - It goes after `--fail-fast` in the option list, as in mdlist's help.
- **Not a CSV column (decided, not a gate question).**
  - Batch has no boolean columns. The "empty cell = command-line value" rule has no natural meaning for a flag.
  - A per-row opt-out already exists: a `dir` cell.
  - The column set is API: an unknown column is exit 2 today, so adding a column is a format change for every CSV writer.
  - It joins the "CLI-only (no CSV column)" list in batch-spec and the README.
- **Global flags** (`--json`, `-q`, `-v`, `--color`) are unchanged and reach batch through the existing `fallthrough()`.
- **No prompts.** Nothing is destructive: the flag only changes where new files go.
- **Help text changes** (the batch help golden must be re-blessed, see AC24):
  - Options: one new line after `--fail-fast`:
    ```
          --dir-per-title               Put each title in a subdirectory of DIR
    ```
  - Footer, at the end of the "CSV file:" section, after the `For example:` block (every line ≤ 80 columns, aligned by hand because footer formatting is off). **rev2 (board):** four lines, adding that later rows of the same title reuse the directory (was three lines):
    ```
      With --dir-per-title, each title goes into its own subdirectory, named like
      its files: DIR/<title>/<title> - Vol. 01.epub. Later rows of the same title
      (another title cell or lang) reuse that directory. A row's dir cell replaces
      the name, so the row goes into DIR/<dir> exactly as without the flag.
    ```
  - Examples, after "Everything in the list, into ~/Manga:" (same wording as mdlist):
    ```
      The same, one folder per title:
        mangadex-dl batch titles.csv ~/Manga --dir-per-title
    ```
  - The `dir` column line (`subdirectory of DIR for this row`) stays as it is. The new paragraph covers the interplay, and a longer column text would break the column alignment.
  - The exit-status table is unchanged.

## Input & output

- **Precedence.** batch reads no config file and no env var, so CLI11's order (CLI > config > env > default) has nothing to arbitrate here.
  - The flag is CLI-only and run-wide.
  - Per row, a **non-empty `dir` cell wins over the derived name** (QB1: it replaces it).
  - Every other field keeps "non-empty cell > CLI > default".
- **stdin:** unchanged (only for `FILE` = `-`).
- **stdout (human):** exactly `download`'s data, one path per written (or, with `--dry-run`, planned) file, in file order.
  - With the flag, a derived row's paths include its directory: `out/Clean Title/Clean Title - Vol. 01.epub`.
  - An explicit-`dir` row's paths are unchanged: `out/series/clean/Clean Title - Vol. 01.epub`.
  - A full success prints nothing on stderr.
- **stderr:** row-scoped lines keep batch's prefix `<label>:<line>: `, with no uuid (that form is mdlist's).
  - **Directory taken by an earlier derived row of another manga** (warning, not shown with `-q`):
    `mangadex-dl: warning: titles.csv:4: directory 'Clean Title' already holds line 2 (a title with the same name); using 'Clean Title [77777777]'`
    This is the resolver's existing sentence with batch's noun ("line"; mdlist keeps "item").
  - **Directory reserved by an explicit `dir` cell of another manga** (new; warning, not shown with `-q`):
    `mangadex-dl: warning: titles.csv:2: directory 'Clean Title' is used by the 'dir' of line 5; using 'Clean Title [11111111]'`
    `<k>` is the first line whose cell reserved the name. A cell can reserve a name through its first segment (`dir=Clean Title/extras`), so the line says "is used by", not "is".
  - **The title's own directory is unusable** (error; the row fails alone; this is the engine's existing text):
    `mangadex-dl: error: titles.csv:3: cannot open directory 'out/Clean Title': Not a directory; rename or remove it, or run without --dir-per-title`
    The remedy is source-independent and correct for batch, so it stays unchanged. An explicit-`dir` row that runs into a file keeps today's message, with no remedy suffix.
  - **`-v`:** after the existing `mangadex-dl: titles.csv:3: <uuid>` start line, a derived row prints the engine's `mangadex-dl: output directory: out/Clean Title`. It is unprefixed and passes through `terminal_safe`, as in mdlist.
    - The warning, if any, comes before it, which is the existing engine order.
    - An explicit-`dir` row prints no `output directory:` line, as today.
  - **Cross-row file collision line: unchanged.**
    `titles.csv:<line>: <path> was already written by line <k>; give one of them a 'title' column`, and batch keeps the engine's "holds a different book" warning as today.
    - Why unchanged: with the flag, derived directories of different mangas never coincide, so the line fires only when two rows land in the same directory with the same stem. That happens with the same manga and the same stem (overlapping selections), with explicit rows that share a `dir` (today's case), or with an explicit cell that equals the same manga's derived name.
    - In each of those cases a different `title` (hence a different stem, file name and, for derived rows, directory) is the right fix, so the remedy holds.
  - Duplicate warning, up-to-date note and closing line: unchanged.
- **`--json`:** no new keys, and `format_version` stays 1. `titles[].dir`, "the effective output directory, as typed":
  - derived row whose name was claimed (ok, 6, 3 after the plan, or `failed` through `row_dir`): `join_list_dir(DIR, <name>)`, e.g. `out/Clean Title` or `out/Clean Title [77777777]`;
  - derived row that never reached a claim (exit 3/4 before the plan, exit 5 with an empty plan, `duplicate`, `not_run`): `DIR`. This is the same rule as mdlist and the README's "DIR when the title's name was never learned";
  - explicit-`dir` row: `DIR/<dir>` as today, flag or not;
  - `report` paths are the engine's, so they already sit under `DIR/<name>/`.
- **Color:** unchanged (stderr prefixes only, through `resolve_color`). `--json` never carries ANSI, even with `CLICOLOR_FORCE=1`. The JSON path never calls the color resolver.
- **Progress:** unchanged (TTY stderr only; never with `-q`/`--json`).
- **Locale:** locale-independent. Directory names come from `sanitize_file_stem`, with no locale-dependent calls.
- **Invalid UTF-8** in DIR or FILE (argv): passed through as bytes; JSON uses the `replace` handler (existing).

## Exit codes

There are no new codes and no new `Errc`. The table in `include/mangadex_dl/exit_codes.hpp` is unchanged.

| Outcome (with the flag) | Code |
|---|---|
| every row ok/duplicate | 0 |
| a derived row's own directory is unusable (`Errc::row_dir`): that row `failed` (exit_code 1), the batch continues | aggregates as 1 (failed + 4 → 7) |
| DIR itself unusable, ENOSPC, EIO, EROFS: the batch stops | 1 |
| stdout write error | 1 (`finish_stdout`) |
| any `ParseError` (e.g. an unknown option) / CSV error | 2 |
| uniform 3/4/5/6; mixed → 7 | as today |
| SIGINT / SIGTERM | 130 / 143 (re-raised by `main`) |

`is_fatal(Errc::row_dir)` already returns false. Only its comment changes: "batch never produces it" becomes "`--dir-per-title` (batch and mdlist): the row fails alone". Code 70 must never be seen, and 86 is reserved for sanitizers.

## Decisions (the eight questions)

### 1. A row's `dir` cell with the flag: replace (gate QB1)

**Recommendation: replace.** A row with a non-empty `dir` cell goes exactly into `DIR/<dir>/<stem> - Vol. 01.epub`, as today.

**"The cell wins over the derived name for that row" means precisely:**
- `plan_batch` sets `options.dir_per_title = settings.dir_per_title && sub_dir.empty()`. So an explicit row never enters the engine's per-title step:
  - no resolve, no claim, no `-v` `output directory:` line;
  - no title-dir pre-flight, and none of its remedy text.
- The runner's existing row pre-flight `check_row_dir(DIR, sub_dir)` runs for it as today, with today's message.
- The row's stdout, stderr, files and JSON are byte-identical to a run without the flag. The only exception is the case where another row's warning mentions its reservation.

**Reasons:**
- **The derived name *is* the per-title directory component.** The cell is the user's own name for it. "Wins over" therefore means "replaces"; nesting would combine the two names.
- **Adding the flag to an existing CSV never moves a file that already has a home.** People who already wrote `dir=series/one-piece` keep their layout. Nesting would re-download everything into `series/one-piece/One Piece/` and orphan the old files.
- **Nesting stays expressible:** `dir=shonen/One Piece`.
- **One rule is easier to document and to test** (AC9 compares the two runs byte for byte).

### 2. Explicit `dir` cells and the TitleDirRegistry: reserve up front (gate QB2)

**Recommendation: explicit cells claim names before the first row runs. A derived row never takes a reserved name for another manga.**

- **Reservation.** Before `run_batch`, the runner reserves the **first segment** of each non-duplicate job's non-empty `sub_dir`, in file order, recording that row's line and manga.
  - It does this only when the run has the flag. For mdlist it is a no-op, because `sub_dir` is always empty there.
  - Derived names are always one component (V12), so only a first segment can ever equal one:
    - `dir=Clean Title` reserves `Clean Title`;
    - `dir=Clean Title/extras` reserves `Clean Title`;
    - `dir=series/clean` reserves `series`.
  - Reserving the first segment rather than only exact single-segment cells keeps a derived title from moving into a folder the user named for something else, whether as a series directory or as a grouping folder.
- **A later derived claim** of the stem:
  1. The name is reserved by **this same manga** (one or more reserving rows): the plain stem, no warning. The title's derived files and its explicit rows share a folder of the same series.
  2. The name is reserved only by other mangas: `<stem> [<8 hex>]`, or the full uuid if that is taken too (the existing fallback). The new "is used by the 'dir' of line k" warning names the first reserving line.
  3. Otherwise, the existing first-claim-wins rule among derived rows applies.
- **Reservations never feed the per-manga lookup.** A manga with `dir=Foo` on one row and no cell on another still gets its own stem on the derived row, not `Foo`, because the rule is per row.
- **Order-independent and result-independent on purpose.**
  - A derived row is moved aside even if it comes *before* the reserving row, and even if the reserving row later fails (exit 4, network).
  - So the directory a derived row gets depends only on the CSV and the titles' names, never on which rows happened to succeed. A network blip on run 1 can't put a title in `X/` and run 2 in `X [aaaaaaaa]/`.
  - Doing it "in order" would let file order decide whether two series mix.
- **Rejected alternatives:**
  - **Ignore explicit cells** (OutputRegistry stays the only backstop): two series' files would mix in one folder, which defeats the flag. OutputRegistry only stops same-*path* overwrites.
  - **Warn only:** same mixing, plus noise.
- **Multiple rows may share one explicit `dir`**, as they can today. The cell wins, so they mix by the user's own choice and the registry does not police explicit rows.
- **Exact byte comparison.** Names are compared byte-exactly, as `validate_list_dir` returns them (trimmed). Case-folding and Unicode normalisation collisions are out of scope, as they already are for file names.

### 3. Same manga, different stem (gates QB3, QB4)

**rev1 (gate): the user chose "stay with the manga".** `TitleDirRegistry::claim` is unchanged: a manga's first derived claim in the run fixes its directory, and every later derived row of the same manga (another `title` cell, another `lang`) goes into that same directory with its own file names, e.g. `DIR/Clean Title/Other Title - Vol. 01.epub`. No registry rule change, no test change, so the AC6 property test and the AC7 (manga, stem) rules are dropped. Reservations (QB2) still never feed the per-manga lookup. To get separate folders, give the rows different `dir` cells. The original recommendation is kept below for the record.

**Original recommendation (not adopted): key derived claims on (manga, stem).** Files and directory then always agree (`DIR/<stem>/<stem> - …`), as the mdlist spec promises.

- **Today's `TitleDirRegistry::claim` returns the earlier name for a manga it has seen, whatever the stem.** In batch, a `title` cell, or another `lang` for a title with no `en` title, gives the same manga a different stem. Then `DIR/Clean Title/Other Title - Vol. 01.epub` would happen.
  - That mixes two deliberately differently-named sets in one folder.
  - It contradicts the README ("named exactly like its files").
- **A row's `title` cell is a deliberate per-row name, so the folder should follow it.**
  - Rows `1111,,` and `1111,Clean Title (colour),` become two folders.
  - To merge them, give both rows the same `dir` cell.
- **New rules:**
  - the same (manga, stem) again → its earlier name (possibly suffixed), no warning;
  - a new (manga, stem) → treated as a fresh claim; a name already held by the *same* manga is never a conflict;
  - another manga holding the stem → suffix + the existing warning (unchanged).
- **Why mdlist stays byte-identical:**
  - In one mdlist run every job has the same options except the manga (`plan_list`).
  - `mark_duplicates` therefore marks any repeated manga as a duplicate, and duplicates never run.
  - So mdlist calls `claim` at most once per manga id, and "same manga, other stem" is unreachable from mdlist.
  - Pinned by a unit property test (AC6) plus every existing mdlist test unchanged.
- **Implementation (QB4).** The recommendation is to change the one rule inside `claim()` and keep its signature, which `headers_test.cpp:446` pins.
  - Two assertions in `tests/unit/title_dir/title_dir_test.cpp` encode the old rule:
    - lines 241–244, "Even with another stem … its earlier name";
    - line 313, `claim("other", id_a, 3).name == "Short Lived Title"` in the lifetime test.
  - Changing them is an approved contract change, not a weakened test. The lifetime test keeps its purpose by re-claiming the same stem.
  - **Alternative if the test change is not approved:** a separate stem-keyed method for batch (e.g. `claim_stem`), keeping `claim()` and its tests as they are. That means two rules in one class, which costs clarity.

### 4. Messages

All of these are specified in "Input & output":
- the resolver's warning gains batch's noun ("line") and the reserved variant;
- the unusable-title-dir error is the engine's unchanged text, remedy included;
- `-v` shows the engine's `output directory:` line for derived rows only;
- the cross-row collision line is unchanged, for the reasons given there.

Final wording is for `cli-ux-reviewer` to polish. Any change after this gate needs an AC update.

### 5. `--json`

Specified above: no new keys. `dir` is `DIR/<name>` for a derived row that claimed a name, `DIR` for one that never did, and `DIR/<dir>` for an explicit row. A separate key such as `dir_source: "derived"|"cell"` was considered and rejected: a consumer can tell the two apart from the CSV, and keys are API.

### 6. Help, footer, README

Help and footer are specified above. README changes, all in the batch section unless noted:
- Options table: a new row `--dir-per-title` | "Put each title in its own subdirectory of `DIR`, named like its files: `DIR/<title>/<title> - Vol. 01.epub`. A row with a `dir` goes into `DIR/<dir>` as without the flag. See [Where the files go](#where-the-batch-files-go)." It is a separate row, not part of the "no CSV column" row, whose reason (a third-party list can't replace files or change the load) doesn't apply to it.
- CSV table, `dir` row: append "With `--dir-per-title`, it replaces the title's own directory."
- A new subsection **"Where the files go"** in the batch section, mirroring mdlist's:
  - flat by default;
  - the flag; the name is the file stem, so a row's `title` cell names its directory too;
  - a `dir` cell replaces the name;
  - explicit `dir` names (their first part) are kept for their rows: another title with that name gets ` [<8 hex>]` with the "is used by the 'dir' of line k" warning, decided from the list alone, whatever the order and whatever the results;
  - two titles with the same name: the earlier line keeps it;
  - rev1 (gate QB3): the same title under two different `title` cells (or `lang`s) stays in the directory its first derived line got; each row's files keep their own names (give the rows different `dir` cells to separate them);
  - the unusable-directory error;
  - "a renamed title, another `--lang` or switching modes gives new paths; the old files are left alone; delete them yourself";
  - the directory is created only when the title has a file to write, never with `--dry-run`.
- `--json` paragraph: `dir` is `DIR`, `DIR/<dir>`, or with `--dir-per-title` `DIR/<name>` (`DIR` when the name was never learned).
- Examples: `mangadex-dl batch titles.csv ~/Manga --dir-per-title`.
- Interruption and file-safety line (README ≈ line 802): "with `mdlist --dir-per-title`" becomes "with `--dir-per-title` (`batch` and `mdlist`)".
- The exit-1 row (≈ line 686) already covers "a title whose own directory can't be used": no change.
- `tests/e2e/inputs/README`: list the new CSVs, and note that `mdlist/Clean Title` is also used by `e2e.batch.dir-per-title-unusable`.
- Optional, docs only: a one-line amendment to `docs/design/list-spec.md` QA2, "done in batch-dir-per-title-spec".

### 7. Behaviour that matches mdlist (confirmed against the code)

- **`--dry-run`:** the name is resolved, the -v line printed and the read-only pre-flight run. A `row_error` fails the row in a dry run too. `open_if_exists(DIR/<name>)` finds nothing, so every file is `planned`. Paths are printed under `DIR/<name>/`. Nothing is created, neither DIR nor `DIR/<name>`.
- **Exit 4 / exit 5 rows** leave no directory: no plan, or an empty plan (`download.cpp` resolves only when `!plan.epubs.empty()`). Their JSON `dir` is DIR (derived) or `DIR/<dir>` (explicit).
- **`--fail-fast`:** a `row_dir` failure is a non-ok row, so it stops the batch with `stopped at line k: --fail-fast; n titles not run`.
- **Duplicates** (identical merged options, including `dir_per_title`) are skipped before running: they claim nothing and reserve nothing new.
  - Two identical derived rows → duplicate.
  - A derived row and an explicit row of the same manga are not duplicates, because `options.dir` differs. If the explicit cell equals the derived name, both land in one folder and the second row's files are `exists`, with today's collision line. This is documented, not deduplicated: the derived name is unknown at plan time.
- **Interrupt:** `resolve_title_dir` checks the stop token first. In-row and between-row behaviour is exactly batch's: no partial or temp files, no further rows, no JSON, no closing line, 130/143. As with DIR and a row's `dir` today, `OutputDir::open(create)` makes the directory when the execute phase starts, so an interrupt during the first file can leave `DIR/<name>/` empty. That is the same as mdlist and is not a regression.
- **Network streak** (3 in a row), a global local error, or a bad `io.out` stops the run: unchanged.
- **Re-runs:** decided afresh every run and not recorded (list-spec QA1 limitation). Same CSV, same layout. Moving rows changes only which same-named *derived* title keeps the plain name; reserved names don't depend on order.
- **Mode switches:** a flag run neither reads nor moves files from a flat run, and the reverse is also true. Files are matched only at their exact path.

### 8. Anything else

- **Env/config:** none in batch; nothing to arbitrate (stated above).
- **CSV column:** no (above).
- **A flag that does nothing** (every row has a `dir`): no warning. It is harmless, and a warning would be noise in scripts that always pass the flag. Out of scope.
- **Threat model:** the `title` cell, untrusted CSV data, now also names a directory.
  - It passes `validate_title_override` and then `sanitize_file_stem`, and the engine re-checks `is_single_safe_component`. So it is always one component inside DIR: no `/`, never `.`/`..`/empty, ≤ 150 bytes.
  - A CSV can already pick any contained `dir`, so it gains no power.
  - Symlinks inside DIR are followed, as before (OD1).
  - Registry memory is bounded by the 10 000-row cap.
  - `security-reviewer` still reviews the change, because it adds CSV-derived directory names.
- **Same layout across commands:** `batch` and `mdlist` now give the same layout for the same titles. That makes a CSV and an MDList of the same titles interchangeable, as long as the CSV has no `title`/`dir` cells.

## Data & state

- **Reads:** the CSV (bounded, unchanged), the fixture/API as today, and existing `.epub` files at their exact paths: `DIR/<name>/` for derived rows, `DIR/<dir>/` for explicit rows.
- **Writes:** only `download`'s outputs, through the existing atomic `OutputDir` publish (temp → fsync → link/rename), with the existing modes.
  - `DIR/<name>/` is created (with DIR if needed) only when the row has a file to write, never in a dry run.
  - Nothing secret. No state file, cache, XDG or TMPDIR use.
- **Mutation:** none outside new or updated EPUBs; no deletes, moves or renames of existing files. Nothing is destructive, so there is no confirmation; `--dry-run` previews the layout.

## Engine reuse (for the planner; names are suggestions)

- **Core, shared headers (serialization points):**
  - `include/mangadex_dl/batch.hpp`: `RawBatchArgs::dir_per_title` and `BatchSettings::dir_per_title` (bool, default false, added last; mdlist leaves `BatchSettings::dir_per_title` false and keeps its own `MdlistSettings::dir_per_title`). `settings.base.dir_per_title` stays false, as in mdlist (`mdlist_plan_test.cpp:130`).
  - `include/mangadex_dl/title_dir.hpp`:
    - `TitleDirRegistry::reserve(std::string_view first_segment, const MangaId&, std::size_t line)`;
    - `TitleDirClaim::reserved` (bool, default false; true when `taken_by` is a reserving line);
    - rev1 (gate QB3): `claim()`'s same-manga rule is unchanged.
- **Core, sources:**
  - `src/core/batch_plan.cpp`: `validate_batch_args` copies the flag; `plan_record` sets `options.dir_per_title = settings.dir_per_title && sub_dir.empty()`.
  - `src/core/title_dir.cpp`: the new rules.
- **App:**
  - `src/app/cmd_batch.cpp`: `add_flag` after `--fail-fast`, plus the footer text.
  - `src/app/cmd_batch_jobs.{hpp,cpp}`:
    - the reservation loop in `run_batch_jobs` before `run_batch`;
    - `ConsoleTitleDirResolver` gets the source's noun and the reserved wording;
    - the `is_fatal` comment and the header doc.
  - `cli.cpp`, `exit_codes.hpp`, `exit_help.hpp`, `error.hpp`, the engine (`dir_per_title.cpp`, `download.cpp`, `execute_phase.cpp`), `planner.cpp` and `options.hpp`: **unchanged.**
- No CMake change; new tests and E2E cases are globbed.
- Byte-identity argument for flag off: `options.dir_per_title` stays false on every batch job, so the engine never resolves and the runner never passes a resolver. The reservation loop doesn't run, and no reservation happens in mdlist either.

## Committed E2E inputs

Reuse existing inputs where possible: `clean.csv`, `row-dir.csv`, `row-dir-file.csv`, `clean-unknown.csv`, `nochapters-mixed.csv`, `two-clean.csv`, `bulk.csv`, and the committed regular file `tests/e2e/inputs/mdlist/Clean Title`.

New, written with `printf` (`batch/*.csv -text`) and listed in `tests/e2e/inputs/README`:
- `batch/per-title-mixed.csv`:
  ```
  manga,title,dir
  11111111-1111-4111-8111-111111111111,,
  11111111-1111-4111-8111-111111111111,Other Title,
  11111111-1111-4111-8111-111111111111,,series/clean
  ```
  Lines 2–4: derived `Clean Title`; same manga with another stem, which stays in `Clean Title/` as `Other Title - …` (rev1, QB3); explicit (Q1).
- `batch/dir-reserved.csv`:
  ```
  manga,dir
  66666666-6666-4666-8666-666666666666,Clean Title
  11111111-1111-4111-8111-111111111111,
  ```
  Line 2 reserves `Clean Title` for an unknown title (404). Line 3, the clean title, is moved aside to `Clean Title [11111111]` (Q2: up front, whatever the result).

The fixture trees serve one real title each, so a cross-manga derived-vs-derived collision can't be shown E2E. It is covered at the `[cli]` tier with the existing in-process `clean-twin` tree (`tests/support/mdx_twin.hpp`, twin id `77777777-…`). Bracketed directory names can't go in `EXPECT_FILES`/`EXPECT_ABSENT` (inputs README rule), so those cases are dry runs asserted through the stdout regex, where `\\[` already works (`tty-progress`).

## Acceptance criteria

**Flag off, byte-identical**
1. `[unit]` `plan_batch` with `settings.dir_per_title == false`: every job equals (`==`) today's job for every committed CSV shape (headerless, `dir`, `title`, ragged). `options.dir_per_title` is false everywhere.
2. `[cli]` Every existing `batch_cli_test`, `batch_parse_test`, `batch_jobs_list_test`, `mdlist_*`, `download_*` and `title_dir_test` test passes unchanged (rev1: no exceptions; QB4 moot).
3. `[e2e]` Every existing `e2e.batch.*`, `e2e.mdlist.*`, `e2e.download.*` and edge case passes unchanged, including the goldens `batch.dry-run`, `batch.json-dry-run`, `mdlist.*`, `download.*`. Only `batch.help` is re-blessed (AC24).
4. `[e2e]` `per-title-mixed.csv` **without** the flag (real run, clean tree) → exit 0, empty stderr, stdout exactly the flat layout: `out/Clean Title - Vol. 01/02.epub`, `out/Other Title - Vol. 01/02.epub`, `out/series/clean/Clean Title - Vol. 01/02.epub`. This is a regression lock for the flat path.

**Core**
5. `[unit]` `validate_batch_args` copies `dir_per_title` (default false) and keeps `base.dir_per_title` false. With the flag, `plan_batch` sets `options.dir_per_title` only on rows with an empty or absent `dir` cell. Explicit rows keep `dir_per_title` false, `job.dir == DIR/<dir>` and `sub_dir == <dir>`. Two identical derived rows are duplicates; a derived and an explicit row of the same manga are not.
6. ~~`[unit]` plan_list property test~~ — dropped (rev1, QB3 = stay with manga: no registry rule change to prove unobservable).
7. `[unit]` rev1: `TitleDirRegistry::claim`'s existing tests pass unchanged; add one test that a manga whose first claim was suffixed because of a reservation keeps that suffixed name on a later claim with another stem.
8. `[unit]` `reserve`:
   - another manga's derived claim of a reserved name → suffixed, `taken_by` = the first reserving line, `reserved == true`;
   - the same manga (any of several reserving rows) → the plain name, no `taken_by`;
   - a reserved suffixed name pushes to the full uuid;
   - reserving never affects the per-manga lookup;
   - the registry owns its keys (a temporary `first_segment` dies before the next claim; ASan-checked).

**In-process CLI** (fixture trees in a temp dir, `run()`)
9. `[cli]` `row-dir.csv` with the flag vs without: stdout, stderr, files and the JSON document are byte-identical (QB1).
10. `[cli]` Headerless clean + flag → exit 0, stdout `out/Clean Title/Clean Title - Vol. 01.epub\nout/Clean Title/Clean Title - Vol. 02.epub\n`, empty stderr, files there, nothing flat in `out/`.
11. `[cli]` `per-title-mixed.csv` + flag → exit 0, empty stderr, files `out/Clean Title/Clean Title - …`, `out/Clean Title/Other Title - …` (rev1, QB3: the same manga stays in its first directory; no `out/Other Title/`) and `out/series/clean/`. `--json` `dir` values: `out/Clean Title`, `out/Clean Title`, `out/series/clean`.
12. `[cli]` clean-twin, rows clean then twin, + flag → twin files under `out/Clean Title [77777777]/`. Stderr is exactly `mangadex-dl: warning: titles.csv:2: directory 'Clean Title' already holds line 1 (a title with the same name); using 'Clean Title [77777777]'\n`, with no collision line and no "holds a different book" line. With `-q`, stderr is empty.
13. `[cli]` Reservation, both orders: rows `twin,dir=Clean Title` and `clean,` (and reversed) + flag → clean goes to `out/Clean Title [11111111]/` with exactly `…:<clean line>: directory 'Clean Title' is used by the 'dir' of line <twin line>; using 'Clean Title [11111111]'`, and twin goes to `out/Clean Title/`. No directory holds files of both mangas. Also passes with `dir=Clean Title/extras` (first segment) and when the reserving row is a 404.
14. `[cli]` Same manga explicit + derived with the same name (`clean,dir=Clean Title` then `clean,`) + flag → both in `out/Clean Title/`, no directory warning, second row's files `exists`, today's collision line `… was already written by line 2; give one of them a 'title' column`.
15. `[cli]` `-v` + flag: `mangadex-dl: titles.csv:1: 11111111-…` then (after any directory warning) `mangadex-dl: output directory: out/Clean Title`. An explicit row prints no `output directory:` line.
16. `[cli]` A regular file `DIR/Clean Title`, `clean, unknown` + flag (real run and dry run) → row 1 fails alone with exactly `mangadex-dl: error: titles.csv:1: cannot open directory '<DIR>/Clean Title': Not a directory; rename or remove it, or run without --dir-per-title`, row 2 runs (404), exit 7, no `stopped at`. JSON row 1: `failed`, `exit_code` 1, `dir` `<DIR>/Clean Title`. With `--fail-fast`: `stopped at line 1: --fail-fast; 1 title not run`, exit 1.
17. `[cli]` DIR itself a regular file + flag → as today: exit 1, `stopped at line 1: the error above affects every title; …`.
18. `[cli]` Exit-4 and exit-5 rows + flag leave no directory (neither DIR nor `DIR/<name>`). Their JSON `dir` is DIR.
19. `[cli]` A dry run + flag creates nothing. The planned paths are under `DIR/<name>/`, and a `row_dir` problem fails the row in the dry run too.
20. `[cli]` Interrupt (the existing batch interrupt seam) + flag, during a row and between rows → `Errc::interrupted`, no `.part-`/temp files, no JSON, no closing line, 130.
21. `[cli]` A flat run, then a flag run into the same DIR: flat files are neither read, changed nor moved; the new files are written under `DIR/Clean Title/`.
22. `[cli]` `batch --help`: the `--dir-per-title` line comes right after `--fail-fast` and is byte-identical to mdlist's line. The footer has the four-line paragraph (rev2 (board); was three lines) and the example, and no line exceeds 80 columns. `download --help` has no `--dir-per-title`, and `download … --dir-per-title` → exit 2 with download's hint.
23. `[cli]` `batch --dir-per-title=<value>` parses exactly as `mdlist --dir-per-title=<value>` (same CLI11 flag semantics; see V-B1). Unknown or invalid forms → exit 2 with the batch hint, never 100–114.

**Installed binary**
24. `[e2e]` The `batch.help` golden shows the new option line, footer paragraph and example, with the exit table unchanged. **Re-bless requested:** `pixi run bless '^e2e\.batch\.help$'`. Reason: the approved additive `--dir-per-title` help line, footer paragraph and example. The user runs it.
25. `[e2e]` `batch.dir-per-title`: `clean.csv` + flag, real run → exit 0, exact stdout `^out/Clean Title/Clean Title - Vol\. 01\.epub\nout/Clean Title/Clean Title - Vol\. 02\.epub\n$`, `EXPECT_STDERR_EMPTY`, `EXPECT_NO_ANSI`, `EXPECT_FILES`/`EXPECT_EPUB` under `out/Clean Title/`, `EXPECT_ABSENT` the flat files, `EXPECT_NO_GLOB "out/Clean Title/.*.part-*"`.
26. `[e2e]` `batch.dir-per-title-dry-run`: the same with `--dry-run` → same stdout, empty stderr, `EXPECT_ABSENT out`.
27. `[e2e]` `batch.dir-per-title-row-dir`: `row-dir.csv` + flag → the exact `row-dir` stdout regex, empty stderr, files under `out/series/clean/`, `EXPECT_ABSENT "out/Clean Title"`.
28. `[e2e]` `batch.dir-per-title-mixed`: `per-title-mixed.csv` + flag, real run → exit 0, empty stderr, exact six-line stdout: four under `out/Clean Title/` (`Clean Title - …` and `Other Title - …`, rev1 QB3) and two under `out/series/clean/`, `EXPECT_FILES` for all six, `EXPECT_ABSENT "out/Other Title"`.
29. `[e2e]` `batch.dir-per-title-reserved`: `dir-reserved.csv` + flag `--dry-run` → exit 4, stdout `^out/Clean Title \[11111111\]/Clean Title - Vol\. 01\.epub\nout/Clean Title \[11111111\]/Clean Title - Vol\. 02\.epub\n$`, `EXPECT_ABSENT out`, and stderr exactly:
    ```
    mangadex-dl: error: …/dir-reserved.csv:2: title 66666666-… not found on MangaDex (HTTP 404); check the UUID or URL
    mangadex-dl: warning: …/dir-reserved.csv:3: directory 'Clean Title' is used by the 'dir' of line 2; using 'Clean Title [11111111]'
    mangadex-dl: error: 1 of 2 titles would fail (line 2)
    ```
30. `[e2e]` `batch.dir-per-title-unusable`: `clean.csv` + flag `--dry-run` with DIR `{{INPUTS}}/mdlist` → exit 1, empty stdout, stderr exactly `…: error: …/clean.csv:1: cannot open directory '…/inputs/mdlist/Clean Title': Not a directory; rename or remove it, or run without --dir-per-title\nmangadex-dl: error: 1 of 1 title would fail \(line 1\)\n`.
31. `[e2e]` `batch.dir-per-title-row-dir-file`: `row-dir-file.csv` + flag `--dry-run` with DIR `{{INPUTS}}` → exit 1.
    - Line 2 has today's error with **no** remedy suffix: `cannot open directory '…/README': Not a directory`.
    - Line 3 is planned under `…/inputs/Clean Title/`.
    - The closing line is `1 of 2 titles would fail (line 2)`.
    - Nothing is created in the source tree.
32. `[e2e]` `batch.dir-per-title-json`: `clean-unknown.csv` + flag `--json` → exit 4, `EXPECT_JSON`, line 1 `"dir": "out/Clean Title"`, line 2 `"dir": "out"`. `batch.dir-per-title-no-chapters`: `nochapters-mixed.csv` + flag on the nochapters tree → exit 7, `EXPECT_ABSENT out`.
33. `[e2e]` `batch.dir-per-title-duplicate`: `two-clean.csv` + flag → exit 0, stderr exactly the existing `same as line 1; skipped` warning, files under `out/Clean Title/`.

**Edge suite** (`_edge.cmake`)
34. `[edge]` `batch-json-dir-per-title-clicolor-force`: `--json --dry-run --dir-per-title`, `CLICOLOR_FORCE=1` → valid JSON, no ANSI, `"dir": "out/Clean Title"` and `"path": "out/Clean Title/Clean Title - Vol. 01.epub"`, `EXPECT_ABSENT out`.
35. `[edge]` `batch-dir-per-title-dev-full`: dry run + flag, stdout `/dev/full` → exit 1, stderr exactly `mangadex-dl: write error: No space left on device` (hermetic glibc C.UTF-8 only; the prefix elsewhere).
36. `[edge]` `batch-dir-per-title-sigpipe` and `…-parent-ignores`: `bulk.csv --dry-run --layout chapters --dir-per-title | head -n 1` → SIGPIPE death (141), one line, empty stderr.
37. `[edge]` `batch-dir-per-title-invalid-utf8-dir-json`: invalid-UTF-8 DIR under `LC_ALL=C` with `--json --dry-run --dir-per-title` → exit 0, valid JSON. `batch-dir-per-title-utf8-dir` and `…-lc-c`: DIR `Zoë` → stdout `^Zoë/Clean Title/Clean Title - Vol\. 01\.epub\n…$` under C.UTF-8 and C, `EXPECT_ABSENT "Zoë"`.
38. `[edge]` `batch-dir-per-title-stdin-null`: `batch --dir-per-title - out` with stdin `/dev/null` → exit 0, `<stdin>: lists no titles`, `EXPECT_ABSENT out`.

**Terminal and human**
39. `[tty-pty]` `batch.dir-per-title-tty-progress`: `clean.csv` + flag on a pty → `(1/1) [1/2] Vol. 1: …` progress, then `out/Clean Title/Clean Title - Vol. 01.epub`. With `--json` it is one JSON document, with no progress.
40. `[human]` The flag on the user's real CSV, mixing rows with and without `dir` (and one `title` cell), opened in their library or reader app: one series per folder; explicit-`dir` rows exactly where they were before the flag; one non-Latin title named readably. `[human: real data, network and a real library app · terminal]`
41. `[human]` Read `batch --help` in a real terminal at 80 columns: the footer paragraph aligns and reads clearly next to the `dir` column line. `[human: terminal-emulator rendering · retryable]`

## Out of scope

- A CSV column for the flag; a per-row opt-out other than `dir`.
- `download --dir-per-title`; any change to mdlist behaviour or wording.
- Recording directory ownership on disk; moving files between flat and per-title layouts; cleaning up old directories after renames.
- Deduplicating a derived row against an explicit row whose `dir` equals the derived name (the name is unknown at plan time).
- Case-insensitive or Unicode-normalisation collisions between names (as for file names today).
- A warning when the flag has no effect (every row has a `dir`).
- Policing explicit rows that share a `dir` (allowed today; the cell wins).

## Gate questions

**Decided 2026-10-28 by the user:** QB1 = replace; QB2 = reserve the first segment of every explicit cell up front; QB3 = stay with the manga's first directory; QB4 = moot (no change to `claim()` or its tests).

- **QB1. Row `dir` cell + flag:** replace (recommended: `DIR/<dir>/`, the row byte-identical to a no-flag run) or nest (`DIR/<dir>/<stem>/`)?
- **QB2. Explicit `dir` names vs derived names:** reserve the first segment of every explicit cell up front, so a derived title of another manga gets ` [8 hex]` plus the "is used by the 'dir' of line k" warning, independent of row order and row results (recommended)?
  Alternatives: reserve only exact single-segment cells; reserve in file order only; ignore explicit cells (OutputRegistry as the only backstop); warn only.
- **QB3. Same manga, different stem** (a `title` cell, or a `lang` that changes the resolved title): the directory follows the stem, key (manga, stem) (recommended), or stays with the manga id's first name?
- **QB4. Test change (only if QB3 = stem):** approve changing `TitleDirRegistry::claim`'s same-manga rule (signature unchanged) and updating the assertions at `tests/unit/title_dir/title_dir_test.cpp` lines 241–244 and 313 to the new rule (recommended)? The alternative is a separate stem-keyed method for batch, leaving `claim()` and its tests untouched.

## [VERIFY] items for the planner / fact-checker

- **V-B1 (installed CLI11):** how `add_flag(name, bool&)` treats `--dir-per-title=<value>` (`true`/`false`/`0`/`1`/other), and that an invalid value is a `ParseError` (remapped to 2). AC23 asserts parity with mdlist, not a specific CLI11 behaviour. Read the installed header (`.pixi/envs/default/include/CLI/`).
- **V-B2 (repo):** `plan_list` plus `mark_duplicates` guarantee one claim per manga per mdlist run (the basis of QB3's byte-identity). Confirm against `src/core/mdlist_plan.cpp` and `src/core/batch_plan.cpp::mark_duplicates`, and pin it with AC6.
- **V-B3 (repo):** the E2E driver accepts `\\[`/`\\]` in `EXPECT_STDOUT_REGEX` (used by `tty-progress`) but not `[`/`]` in `EXPECT_FILES`/`EXPECT_ABSENT` (inputs README rule). AC29 is designed around that.
- **V-B4 (repo):** `headers_test.cpp` (static_asserts at 122, 410, 446; checks at 876–886, 1002–1007): adding `RawBatchArgs::dir_per_title`, `BatchSettings::dir_per_title` and `TitleDirClaim::reserved` as last members with defaults keeps every existing designated initializer and `operator==` check valid. Add the matching shape checks.
- **V-B5 (repo):** the column widths of the new help line and footer paragraph in the rendered `batch --help` (≤ 80; option text ≤ 44), confirmed on the built binary before the bless request.
- **V-B6 (repo):** the stderr order inside a derived row (directory warning, then `-v` `output directory:`, then any pre-flight error) follows `resolve_title_dir`'s call order. Confirm before AC12/AC15/AC29 pin it.
