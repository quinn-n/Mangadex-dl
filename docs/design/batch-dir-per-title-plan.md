# Implementation plan: `mangadex-dl batch --dir-per-title`

Status: DRAFT for the ⏸ plan gate (fact-check pending). Design: `/home/user/Mangadex-dl/docs/design/batch-dir-per-title-spec.md` (gate decided 2026-10-28: QB1 replace, QB2 reserve first segments up front, QB3 stay with the manga, QB4 moot). Wherever the spec body and its "rev1 (gate)" notes disagree, the rev1 notes win: AC6 is dropped, and AC7, AC11 and AC28 are amended. Prior plan used for format and depth: `docs/design/list-plan.md`.

What I checked in the tree on 2026-10-28 (orientation only; the fact-checkers re-confirm the [VERIFY] items):
- `pixi list`: cli11 2.7.2, catch2 3.16.1, cmake 4.4.4, libstdcxx 16.2.0. **No new dependency.**
- `git status`: clean except the untracked spec.

---

## Approach

The engine step (`resolve_title_dir`), `Errc::row_dir`, the per-row `ConsoleTitleRunner` and `run_batch` are reused unchanged. The runner already hands a resolver to any job with `options.dir_per_title` set, whatever its source (`cmd_batch_jobs.cpp:151-156`). So batch gets the flag in three pieces:
1. **Core:** `plan_batch` sets `options.dir_per_title = settings.dir_per_title && sub_dir.empty()`, and `TitleDirRegistry` gains up-front first-segment reservations (`reserve()` plus `TitleDirClaim::reserved`).
2. **App:** `run_batch_jobs` reserves before `run_batch`, and the resolver's warning becomes source-aware ("line" for batch, "item" for mdlist) with the new "is used by the 'dir' of line k" variant.
3. **Surface:** the flag, help text, E2E cases and README.

**Key trade-off.** The tasks run almost entirely in sequence (core, then app, then E2E ∥ README). Splitting the app work into a runner track and a help track would have let two tracks run at once, but the help edit turns `e2e.batch.help` red until the human re-blesses it. That red golden would sit inside the other track's review gate. This change is small, so a clean gate beats the concurrency.

---

## Spec issues for the plan gate (read first)

- **S1 (needs your OK): AC2 "every existing test passes unchanged" cannot hold for two help-shape assertions.** Both encode the old batch help, so the approved help change makes them false:
  - `tests/cli/mdlist_parse_test.cpp:235-272`, "mdlist --help: the options block is batch's plus --dir-per-title…". Its last check, `without == options_block(batch.out)`, breaks once batch has the same line. **Plan:** rename the test to "…the options block is batch's, byte for byte, with --dir-per-title after --fail-fast" and change the check to `own == options_block(batch.out)`. Every other check in the test stays. This makes the test stricter, not weaker.
  - `tests/cli/batch_parse_test.cpp:214-224` asserts that the text from `note ignored` through `For example:` and its rows is followed directly by `\nFor a MangaDex list…\n\nExamples:\n`. The spec puts the new paragraph in exactly that gap. **Plan:** extend the contiguous expected string with the three new lines. Alternative: put the paragraph after the Examples block, which leaves the assertion alone but is not the spec's placement.
  - No other existing assertion changes. `title_dir_test.cpp` stays untouched: the new registry tests go in a new file.
- **S2: README anchor.** The spec names the new subsection "Where the files go", but links it as `#where-the-batch-files-go`. The batch section (README:291) comes before mdlist's `### Where the files go` (README:492). A duplicate heading would take the plain slug and turn mdlist's into `-1`, breaking README:467 and the other mdlist links. **Plan:** the heading is `### Where the batch files go` (V-P8).
- **S3: a registry edge case the spec does not cover.** A stem can be reserved by both manga A and manga B (two explicit rows `dir=T`), with derived rows of A and then B. **Plan:** the reservation never moves B aside (B is a reserver), but derived first-claim-wins still applies, so B gets `T [bbbbbbbb]` with the existing "already holds line k" warning. This keeps the spec's own invariant (spec line 76): "derived directories of different mangas never coincide". Pinned by a unit test.
- **S4: AC14's exact stderr is longer than the spec says.** Today's behaviour (`batch_cli_test.cpp:765-783`) gives one collision line per file the second row finds, then the end note `mangadex-dl: 2 files are already up to date; pass --overwrite to download them again`. The test asserts exactly that. Stdout is the first row's two paths only, because files that already exist are not printed.
- **S5: AC20 at the cli tier.** The in-process `run_batch_command` seam only supports an interrupt that is pending before the call. Interrupts during a row and between rows, with a resolver, are already covered by unit tests of the unchanged engine and loop: `download_dir_per_title_test.cpp:555-620` and `batch_source_test.cpp:82`. A real SIGINT on the installed binary goes on the `cli-tester` checklist.
- **S6: `reserve()` takes the whole validated `dir` cell, not a pre-cut first segment.** Core then owns the segment rule and can unit-test it (`Clean Title/extras` → `Clean Title`). The spec says names are suggestions.
- **S7: comment-only edits** to files the spec lists as unchanged, because their comments would become false: `include/mangadex_dl/options.hpp:102-103` ("download and batch never set it"), `src/app/cmd_download_run.hpp:81` ("null for `download` and `batch`"), and the `title_dir.hpp`/`.cpp` file headers ("`mdlist --dir-per-title`"). No behaviour changes.

## Resolved in-repo (evidence; fact-checkers only re-read these)

- **R1:** `ConsoleTitleRunner::run` passes `&resolver_` for every job with `options.dir_per_title`, whatever the source (`src/app/cmd_batch_jobs.cpp:151-156`). It sets `outcome.dir = join_list_dir(job.dir, *claimed)` (`:169-176`). So JSON `dir` and the engine path need no change.
- **R2:** the resolver's warning hard-codes "item" (`cmd_batch_jobs.cpp:88-91`) and must become source-aware. `row_noun(BatchSource)` already exists above it (`:53-62`).
- **R3:** `plan_record` builds `options` through `detail::validate_common`, so `dir_per_title` comes out false. It has to be set afterwards, before `std::move(*options)` and before `sub_dir` is moved (`src/core/batch_plan.cpp:216-231`).
- **R4:** `mark_duplicates` compares the whole `DownloadOptions` with the defaulted `operator==`, which includes `dir` and `dir_per_title` (`batch_plan.cpp:241-253`, `options.hpp:106`). So two identical derived rows are duplicates, and a derived row and an explicit row of the same manga are not (their `options.dir` differs).
- **R5:** engine order in `resolve_title_dir`: stop check, then `resolve`, then `on_warning`, then `on_verbose("output directory: …")`, then `check_row_dir` plus the remedy (`src/core/download/dir_per_title.cpp:46-80`). The runner's `-v` start line is printed before `run_title` (`cmd_batch_jobs.cpp:132-136`).
- **R6:** grep found no positional aggregate init (`RawBatchArgs{x…}`, `BatchSettings{x…}`, `TitleDirClaim{x…}`) and no structured bindings of these types.
- **R7:** the footer widths are 77, 74 and 72 columns for the paragraph, and 33 and 56 for the example: all ASCII, all ≤ 80.
- **R8:** `.gitattributes` already marks `tests/e2e/inputs/batch/*.csv -text`, so the new CSVs need no `.gitattributes` edit.
- **R9:** `tests/e2e/CliE2E.cmake:42-44` says `EXPECT_FILES`/`EXPECT_ABSENT` are literal paths and that balanced `[`/`]` work. Only unbalanced brackets and `;` fail. This weakens the spec's premise for AC29 (V-B3).

---

## Dependencies & pixi/CMake changes

| Change | Where | Task |
|---|---|---|
| None: no `pixi add`, `find_package`, FetchContent, preset, pixi task, CMake option, `scripts/*` or CI change | — | — |
| New unit test sources (recursive `tests/unit/**` glob): `tests/unit/title_dir/title_dir_reserve_test.cpp`, `tests/unit/batch/batch_plan_dir_per_title_test.cpp` | glob | T1 |
| New in-process CLI tests (flat `tests/cli/*.cpp` glob): `tests/cli/batch_dir_per_title_test.cpp`, `tests/cli/batch_dir_per_title_parse_test.cpp` | glob | T2 |
| E2E cases appended to the existing `tests/e2e/cases/batch.cmake` and `_edge.cmake` | no CMake edit | T3 |
| Committed inputs `tests/e2e/inputs/batch/per-title-mixed.csv` and `dir-reserved.csv` | not CMake | T3 |

- **Build files for build-fixer, every task:** none beyond the task's Owns. If a task seems to need a shared CMake edit, that is a coordination event for the orchestrator.
- `tests/CMakeLists.txt`, `CliE2E.cmake`, `run_case.cmake`, `fixture_gen.cpp`, `cli.cpp`, `exit_codes.hpp`, `exit_help.hpp`, `error.hpp` and the engine sources are **not edited**.

---

## Assumptions to verify

V-B2 is moot (QB3: `claim()`'s same-manga rule is unchanged).

**Cluster A: CLI11 2.7.2 (installed headers under `.pixi/envs/default/include/CLI/`).** Blocks T2.
- **V-B1** How `add_flag("--dir-per-title", bool&)` treats `--dir-per-title=<value>` for `true`, `false`, `1`, `0`, `yes`/`no`/`on`/`off` and a bogus value. An invalid value must be a `CLI::ParseError` subclass, which `cli.cpp` already maps to exit 2 with the subcommand hint, never 100–114. AC23 asserts parity with mdlist only, so the exact set of accepted values does not matter.
- **V-P1** With the root's `fallthrough()`, all of these must work: `batch FILE DIR --dir-per-title`, `batch FILE --dir-per-title` (DIR stays `.`; the flag is never taken as DIR), and `batch --dir-per-title FILE DIR --json`. The new option renders right after `--fail-fast`.
- **V-B5** The rendered option line is byte-identical to mdlist's, `      --dir-per-title               Put each title in a subdirectory of DIR` (already in `tests/e2e/golden/mdlist.help.stdout`), with the description within the 44-column right column. The footer is hand-aligned: widths per R7. T2 re-confirms on the built binary before the bless request.

**Cluster B: repo, core shapes and compile.** Blocks T1.
- **V-B4** `headers_test.cpp` (static_asserts at 122, 410, 422, 445-447; runtime checks at 876-892 and 997-1007): adding `RawBatchArgs::dir_per_title`, `BatchSettings::dir_per_title` (after `source`) and `TitleDirClaim::reserved` (after `taken_by`) as last members with DMIs keeps every existing designated initializer and `operator==` check valid on clang 23 and gcc 16 under `-Werror`.
- **V-P2** `TitleDirRegistry` stays `std::is_nothrow_default_constructible` (`headers_test.cpp:422`) with a third private member `std::map<std::string, Reservation, std::less<>>`, where `Reservation` holds a `std::size_t` and an owned container of `MangaId` (`std::set<MangaId>` or `std::vector<MangaId>`), on libstdc++ 16 in both envs.
- **V-P3** `MangaId` provides the ordering `std::set<MangaId>` needs. It is already a `std::map` key in `title_dir.hpp`. Re-read R3 and R4.

**Cluster C: repo, runner/engine order and message text.** Blocks the exact assertions in T2 (cli) and T3 (e2e).
- **V-B6** The stderr order inside a derived row: the runner's `-v` start line, then the directory warning, then `output directory: …`, then the pre-flight `row_dir` error (R5). It holds for csv rows exactly as for mdlist.
- **V-P5** For a csv row, the warning and error lines the engine sends through the row observer carry the `<label>:<line>: ` prefix and pass through `terminal_safe`. The `-v` `output directory:` line carries no row prefix. A `row_dir` row error prints as `mangadex-dl: error: <label>:<line>: cannot open directory '<DIR>/<name>': Not a directory; rename or remove it, or run without --dir-per-title`.
- **V-P6** The closing and stop wording in `batch_run.cpp` (`:343` and the stop messages) for the asserted cases:
  - a dry run with one exit-4 row of two: `1 of 2 titles would fail (line 2)`;
  - one `row_dir` row of one: `1 of 1 title would fail (line 1)`;
  - with `--fail-fast`: `stopped at line 1: --fail-fast; 1 title not run`;
  - JSON `status: "failed"`, `exit_code: 1` for the `row_dir` row.

**Cluster D: the E2E driver (repo).** Blocks T3.
- **V-B3** `\\[`/`\\]` work in `EXPECT_STDOUT_REGEX` (precedent: `tty-progress`). Also confirm R9: does `EXPECT_FILES "out/Clean Title [11111111]/Clean Title - Vol. 01.epub"` work (balanced brackets, a space)?
- **V-P7** Sharing `{{INPUTS}}/mdlist/Clean Title` between `e2e.mdlist.dir-per-title-row-dir-file` and `e2e.batch.dir-per-title-unusable` is race-free under `ctest -j`: both are read-only dry runs. A derived dry-run row with DIR `{{INPUTS}}` creates nothing, because `open_if_exists` finds nothing. `EXPECT_NO_GLOB "out/Clean Title/.*.part-*"` works with a space in the path.

**Cluster E: docs (GitHub rendering).** Blocks T4.
- **V-P8** GitHub's heading slug for `### Where the batch files go` is `where-the-batch-files-go`. A second identical heading gets a `-1` suffix, and the first occurrence keeps the plain slug, which is the basis of S2.

**Clusters:** Cluster A: V-B1, V-P1, V-B5 · Cluster B: V-B4, V-P2, V-P3 · Cluster C: V-B6, V-P5, V-P6 · Cluster D: V-B3, V-P7 · Cluster E: V-P8.

| Cluster | Must be confirmed before |
|---|---|
| B | T1 (batch 1) |
| A, C | T2 (batch 2) |
| C, D | T3 (batch 3) |
| E | T4 (batch 3) |

Fan out all five before batch 1; only B gates batch 1 itself. **Language/library:** nothing beyond what the tree already uses (heterogeneous `std::map<…, std::less<>>`, `std::format`, `std::string_view::find`/`substr`, `std::set`).

---

## Task breakdown

These conventions apply to every task. They are inherited from list-plan.
- Aggregates have DMIs and are built with designated initializers.
- `enum class` switches list every enumerator, have no `default:`, and end with a fallback return.
- No `<CLI/CLI.hpp>` outside `src/app/`.
- Core never prints. Every stderr line goes through `print_*` or the observer, which `terminal_safe` the whole line.
- Test names start with the module (`"title dir: reserve …"`, `"batch plan --dir-per-title: …"`, `"batch --dir-per-title: …"`), so `-R 'unit\.title dir'` and `-R 'cli\.batch'` select them.
- GCC: never `static_cast` to a type the expression already has (`-Wuseless-cast`).
- **Done when (every task):** `nice pixi run check` is green (long `timeout`, or `run_in_background`) **and** `quality-reviewer` + `memory-safety-reviewer` both return `VERDICT: APPROVED`.

### T1: Core: the flag through `plan_batch`, and up-front reservations in `TitleDirRegistry`
- **Goal:** AC1, AC5, AC7 and AC8, plus the S3 rule, at the unit tier. No behaviour change anywhere while the flag is off.
- **Files:**
  - edit `include/mangadex_dl/batch.hpp`, `include/mangadex_dl/title_dir.hpp`, `include/mangadex_dl/options.hpp` (comment only, S7), `src/core/batch_plan.cpp`, `src/core/title_dir.cpp`, and `tests/unit/interfaces/headers_test.cpp` (append only);
  - create `tests/unit/title_dir/title_dir_reserve_test.cpp` and `tests/unit/batch/batch_plan_dir_per_title_test.cpp`.
- **Owns:** exactly those 8 files. These are the only shared public headers that change (serialization points), and they stay alone in batch 1.
- **Build files for build-fixer:** none.
- **What changes:**
  - `RawBatchArgs`: `bool dir_per_title = false;` as the last member.
  - `BatchSettings`: `bool dir_per_title = false;` last, after `source`. Doc: "batch `--dir-per-title`; mdlist leaves it false and keeps `MdlistSettings::dir_per_title`".
  - `validate_batch_args`: add `.dir_per_title = raw.dir_per_title` (designators in declaration order). `base.dir_per_title` stays false.
  - `plan_record`: `options->dir_per_title = settings.dir_per_title && sub_dir.empty();`, set before `sub_dir` and `*options` are moved into the `BatchJob` (R3). Nothing else changes, so `mark_duplicates` sees the flag (R4).
  - `TitleDirClaim`: `bool reserved = false; ///< true when taken_by is the first line whose 'dir' cell reserved the name`, as the last member.
  - `TitleDirRegistry`:
    - new `void reserve(std::string_view row_dir, const MangaId& manga, std::size_t line);`
    - documented contract: it reserves the first `/`-segment of `row_dir` (a validated `dir` cell); an empty `row_dir` is a no-op; the first line to reserve a segment is the one reported; it is called before the first `claim()`; it never touches the per-manga lookup; it copies everything (owns its keys).
    - new private `reserved_` map (V-P2, V-P3).
  - `claim()` keeps its signature. Its rules are documented and implemented in this order:
    1. The manga already claimed in this run → its earlier name, no `taken_by`, `reserved` false (unchanged, QB3).
    2. The stem is reserved and none of its reserving rows is `manga` → moved aside, `taken_by` = first reserving line, `reserved = true`.
    3. The stem is held by an earlier claim (necessarily another manga) → moved aside, `taken_by` = holder's line (unchanged; S3).
    4. Otherwise → the stem, also when only `manga` reserved it.
    - **Moved aside** = `<stem> [<8 hex>]`, or `<stem> [<full uuid>]` when the 8-hex name is held by a claim **or reserved by another manga**. The full-uuid name is returned even if held (unchanged backstop).
    - With no `reserve()` call, every branch behaves byte-for-byte as today.
  - File-header comments say `--dir-per-title (batch and mdlist)`. `options.hpp:102-103` drops "download and batch never set it" and says "batch and mdlist `--dir-per-title`; download never sets it".
- **Dependencies:** none (Cluster B confirmed).
- **Done when:** the common Done-when above, plus:
  - `title_dir_test.cpp`, `batch_plan_test.cpp`, `mdlist_plan_test.cpp` and every `mdlist_*`/`download_*` test pass with no edit;
  - once, at integration (shared headers changed): `nice pixi run -e gcc test -R '^unit\.'`.
- **Tests:** unit only (see Test plan).

### T2: App: the flag, reservations in the runner, batch wording, help text, cli tests, bless request
- **Goal:** AC9–AC24 at the cli tier, plus the help surface.
- **Files:**
  - edit `src/app/cmd_batch.cpp`, `src/app/cmd_batch_jobs.hpp`, `src/app/cmd_batch_jobs.cpp`, `src/app/cmd_download_run.hpp` (comment only, S7), `tests/cli/batch_parse_test.cpp` (S1: the `:214-224` assertion only) and `tests/cli/mdlist_parse_test.cpp` (S1: the `:235-272` test only);
  - create `tests/cli/batch_dir_per_title_test.cpp` and `tests/cli/batch_dir_per_title_parse_test.cpp`.
- **Owns:** exactly those 8 files. No serialization point: `cli.cpp` already registers `batch`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cmd_batch.cpp`:**
    - in `BatchCommand::bind`, right after `--fail-fast`: `sub.add_flag("--dir-per-title", args_.dir_per_title, "Put each title in a subdirectory of DIR");` (the same string as mdlist);
    - in `batch_footer`, insert these lines directly after `"    <uuid>,My Title,series/my-title\n"`. There is no blank line between them, because the CSV section has none (`cli-ux-reviewer` may polish):
      ```
      "  With --dir-per-title, each title goes into its own subdirectory, named like\n"
      "  its files: DIR/<title>/<title> - Vol. 01.epub. A row's dir cell replaces\n"
      "  that name, so the row goes into DIR/<dir> exactly as without the flag.\n"
      ```
    - after `"    mangadex-dl batch titles.csv ~/Manga\n"`, add `"  The same, one folder per title:\n"` and `"    mangadex-dl batch titles.csv ~/Manga --dir-per-title\n"`.
  - **`cmd_batch_jobs.cpp`:**
    - `ConsoleTitleDirResolver(TitleDirRegistry&, BatchSource source) noexcept` stores the enum by value (no view);
    - `resolve()` words its warning with `row_noun(source_)`:
      - when `claim.reserved`: `directory '<stem>' is used by the 'dir' of <noun> <k>; using '<name>'`;
      - otherwise the existing sentence with the noun: `directory '<stem>' already holds <noun> <k> (a title with the same name); using '<name>'`;
      - mdlist's bytes are unchanged ("item"), and its reserved branch is unreachable;
    - `ConsoleTitleRunner` builds `resolver_(dirs, settings.source)`;
    - in `run_batch_jobs`, between `TitleDirRegistry dirs{};` and the runner, a reservation loop with a lifetime comment: `if (settings.dir_per_title) for (const BatchJob& job : jobs) if (!job.duplicate_of && !job.sub_dir.empty()) dirs.reserve(job.sub_dir, job.options.manga, job.line);`;
    - the `is_fatal` comment becomes "`--dir-per-title` (batch and mdlist): the row fails alone".
  - **`cmd_batch_jobs.hpp`:** the `run_batch_jobs` doc covers the reservation rule and the "line"/"item" resolver wording.
  - **`cmd_download_run.hpp:81`:** comment only.
  - **S1 test edits** exactly as described in Spec issues.
- **Dependencies:** T1. Cluster A and C confirmed.
- **Done when:**
  - T2 hands off with the confirmed V-B5 widths and a **bless request**: `pixi run bless '^e2e\.batch\.help$'`, reason "approved additive `--dir-per-title` help line, footer paragraph and example (spec AC24)";
  - the human has run that bless;
  - then the common Done-when, with every golden other than `batch.help` unchanged.
- **Tests:** cli and the re-blessed golden (see Test plan).

### T3: E2E cases, edge-suite additions and committed inputs
- **Goal:** AC3, AC4, AC25–AC39 on the installed binary.
- **Files:**
  - edit `tests/e2e/cases/batch.cmake` (append a `---- --dir-per-title` block), `tests/e2e/cases/_edge.cmake` (append a `batch-dir-per-title-*` block; the `EDGE_*` knobs are unchanged) and `tests/e2e/inputs/README` (list the two CSVs; note that `mdlist/Clean Title` is also used by `e2e.batch.dir-per-title-unusable`);
  - create `tests/e2e/inputs/batch/per-title-mixed.csv` and `tests/e2e/inputs/batch/dir-reserved.csv`, written with `printf` (LF, exactly the spec's bytes) and checked with `od -c`.
- **Owns:** exactly those 5 files. `_edge.cmake` is owned only here.
- **Build files for build-fixer:** none.
- **Dependencies:** T2 (flag registered, final wording). Clusters C and D confirmed.
- **Done when:** the common Done-when (no new GOLDEN case, so no bless), plus `nice pixi run release-test` green (the new cases on the hardened install).
- **Tests:** see the E2E and edge tables.

### T4: README (and the optional list-spec note)
- **Goal:** spec §6.
- **Files:** edit `README.md`; optionally `docs/design/list-spec.md` (the one-line QA2 amendment).
- **Owns:** those 2 files.
- **Build files for build-fixer:** none.
- **What changes** (README):
  - the batch options table gets a `--dir-per-title` row linking `#where-the-batch-files-go`;
  - the CSV `dir` row gets the sentence "With `--dir-per-title`, it replaces the title's own directory.";
  - a new `### Where the batch files go` subsection (S2) with every bullet of spec §6, including the rev1 QB3 bullet;
  - the `--json` `dir` sentence;
  - the example `mangadex-dl batch titles.csv ~/Manga --dir-per-title`;
  - the Interruption line (README:803) "with `mdlist --dir-per-title`" becomes "with `--dir-per-title` (`batch` and `mdlist`)".
  - Message wording is copied from T2's merged code, not from the spec.
- **Dependencies:** T2. Cluster E confirmed.
- **Done when:** `nice pixi run check` green and `quality-reviewer` approves. `memory-safety-reviewer` confirms that no C++ changed.
- **Tests:** none (docs only).

---

## Parallelization plan

**Batch 1: T1 · Batch 2: T2 · Batch 3 (parallel): T3, T4**

- **Batch 1:** T1 alone. It is the only owner of the shared headers `batch.hpp`, `title_dir.hpp` and `options.hpp`.
- **Batch 2:** T2 alone, for the reason given in Approach: a split would leave `e2e.batch.help` red inside a parallel track's gate. There is no `cli.cpp` registration task, because `batch` is already registered and the flag is bound in `cmd_batch.cpp`.
- **Batch 3:** T3 (test files and inputs) and T4 (README). Their Owns are disjoint, and T4 builds nothing, so they cannot break each other's build.
- **Serialization points:** `batch.hpp`, `title_dir.hpp` and `options.hpp` are touched only by T1. `tests/CMakeLists.txt`, the top-level `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `cli.cpp`, `scripts/*` and `.github/*` are not touched at all.

---

## Test plan (per tier)

### AC → task → tier
| AC | Tier | Task | Where |
|---|---|---|---|
| 1, 5 | unit | T1 | `batch_plan_dir_per_title_test.cpp`, plus the unchanged `batch_plan_test.cpp` |
| 2 | all | — | every existing suite unchanged, except S1's two assertions |
| 3 | e2e | — | every existing case and golden unchanged; only `batch.help` is re-blessed |
| 4 | e2e | T3 | `e2e.batch.dir-per-title-mixed-flat` |
| 6 | — | — | dropped (rev1) |
| 7, 8, S3 | unit | T1 | `title_dir_reserve_test.cpp` |
| 9–21 | cli | T2 | `batch_dir_per_title_test.cpp` (AC20 per S5) |
| 22, 23 | cli | T2 | `batch_dir_per_title_parse_test.cpp`, plus the S1 edits |
| 24 | e2e golden | T2 | `e2e.batch.help` re-bless |
| 25–33 | e2e | T3 | `batch.cmake` |
| 34–38 | edge | T3 | `_edge.cmake` |
| 39 | tty-pty | T3 | `batch.cmake` TTY cases |
| 40, 41 | human | — | User Test Plan |

### Unit (Catch2 against `mangadex_dl::core`, T1)
- **`headers_test.cpp` (append):**
  - static_asserts: `RawBatchArgs::dir_per_title`, `BatchSettings::dir_per_title` and `TitleDirClaim::reserved` are `bool`; `&TitleDirRegistry::reserve` is `void (TitleDirRegistry::*)(std::string_view, const MangaId&, std::size_t)`;
  - runtime: all defaults are false; partial designated init works (`RawBatchArgs{.file = "-", .dir_per_title = true}`, `BatchSettings{.label = "x", .dir_per_title = true}`); `TitleDirClaim{.name = "T", .taken_by = 1, .reserved = true} != TitleDirClaim{.name = "T", .taken_by = 1}`.
- **`batch_plan_dir_per_title_test.cpp`** (`"batch plan --dir-per-title: …"`):
  - `validate_batch_args` copies the flag (false by default, true when set). `base.dir_per_title` is false in both cases.
  - Flag off: for headerless, `dir`, `title` and ragged CSVs, every `options.dir_per_title` is false, and the jobs `==` those planned with default-constructed settings fields (AC1).
  - Flag on:
    - an empty, absent or all-spaces `dir` cell gives `options.dir_per_title == true`, `job.dir == DIR` and `sub_dir == ""`;
    - an explicit cell gives false, `job.dir == join_list_dir(DIR, cell)` and `sub_dir == cell`, including `a/b`.
  - Dedupe: two identical derived rows → `duplicate_of`; a derived and an explicit row of the same manga → not duplicates; two identical explicit rows → duplicates, as today (AC5).
- **`title_dir_reserve_test.cpp`** (`"title dir: reserve …"`; ids as in `title_dir_test.cpp`):
  - A stem reserved by another manga → `{.name = "Clean Title [11111111]", .taken_by = <first reserving line>, .reserved = true}`. With reservers at lines 2 and 5, the claim reports 2.
  - The same manga among several reservers → plain, no `taken_by`, `reserved` false.
  - The first segment: `Clean Title/extras` reserves `Clean Title`; `series/clean` reserves `series` (so `clean` stays free); an empty `row_dir` reserves nothing.
  - A reserved suffixed name pushes to the full uuid, with `taken_by` and `reserved` from the stem's reservation.
  - Reservations never feed the per-manga lookup: `reserve("Foo", a)` then `claim("Clean Title", a)` → `Clean Title`.
  - AC7: a manga first suffixed because of a reservation keeps that name on a later claim with another stem (no `taken_by`, `reserved` false).
  - S3: stem reserved by both a and b; `claim(a)` → plain; `claim(b)` → `T [<b 8 hex>]`, `taken_by` = a's line, `reserved` false.
  - Byte-exact: reserving `clean title` does not block `Clean Title`.
  - Ownership: `reserve()` with a `std::string` temporary that dies before the next `claim()` (ASan-checked).
- **Unchanged and must pass as-is:** `title_dir_test.cpp`, `batch_plan_test.cpp`, `batch_source_test.cpp`, `batch_run_test.cpp`, `mdlist_plan_test.cpp`, `download_dir_per_title_test.cpp`.

### In-process CLI (T2; `tests/cli/`, flat)
**`batch_dir_per_title_test.cpp`** (`"batch --dir-per-title: …"`) runs through `run_batch_command` with `RawBatchArgs{… .dir_per_title = true}`. Each test uses its own fixture tree in a `TempDir`, as `batch_cli_test.cpp` does: `clean`, `nochapters`, and `clean-twin` via `mdx::write_clean_twin_tree`.

- **AC9:** a `manga,dir` row `series/clean` with and without the flag. stdout, stderr, the file listing and the `--json` document are byte-identical (run, capture, remove `out`, run again).
- **AC10:** a headerless clean row → exit 0, stdout `out/Clean Title/Clean Title - Vol. 0{1,2}.epub` lines, empty stderr, files there, nothing flat in `out/`.
- **AC11:** the `per-title-mixed` rows → exit 0, empty stderr, files `out/Clean Title/{Clean,Other} Title - …` and `out/series/clean/…`, no `out/Other Title`. JSON `dir`s are `out/Clean Title`, `out/Clean Title`, `out/series/clean`.
- **AC12:** clean then twin → the twin is under `out/Clean Title [77777777]/`. stderr is exactly the one `titles.csv:2: directory 'Clean Title' already holds line 1 …` warning (the noun is "line"). With `-q`, stderr is empty.
- **AC13:**
  - Rows twin `dir=Clean Title` and clean (empty dir), in both orders → clean is in `out/Clean Title [11111111]/` with exactly `…:<clean line>: directory 'Clean Title' is used by the 'dir' of line <twin line>; using 'Clean Title [11111111]'`. The twin is in `out/Clean Title/`, and no directory holds files of both mangas.
  - Variants: `dir=Clean Title/extras`, and the reserving row is unknown (404) with exit 4.
- **AC14:** clean `dir=Clean Title` then clean (empty dir) → both rows' files are in `out/Clean Title/`, with no directory warning. stderr is exactly the two collision lines (`… was already written by line 2; give one of them a 'title' column`) plus the up-to-date note (S4). stdout is the first row's two paths.
- **AC15:** `-v` → the start line `mangadex-dl: <csv>:1: 1111…` comes before `mangadex-dl: output directory: <out>/Clean Title`, and in the twin case the warning sits between them. An explicit row has no `output directory:` line. Assert presence and relative order, as `mdlist_cli_test.cpp` does.
- **AC16:** a regular file at `out/Clean Title`, rows clean and unknown, real run and dry run:
  - row 1 fails with the exact remedy line, row 2 gets its 404, exit 7, no `stopped at`;
  - JSON row 1 is `failed`, `exit_code` 1, `dir` `<out>/Clean Title`;
  - with `--fail-fast`: `stopped at line 1: --fail-fast; 1 title not run`, exit 1.
- **AC17:** DIR is a regular file, flag on → exit 1, `stopped at line 1: the error above affects every title; …`.
- **AC18:** exit-4 and exit-5 rows → neither `out` nor `out/<name>` exists, and JSON `dir == out`.
- **AC19:** a dry run creates nothing, plans its paths under `out/Clean Title/`, and a `row_dir` file still fails the row.
- **AC20 (S5):** an interrupt pending before the call, with the flag → no output, no JSON, nothing created, dispositions restored.
- **AC21:** a flat run, then a flag run into the same DIR → the flat files are byte-unchanged (SHA or bytes) and the new files are under `out/Clean Title/`.

**`batch_dir_per_title_parse_test.cpp`** runs through `run_cli` (the full CLI11 app):
- **AC22:**
  - `batch --help` has `dir_per_title_line` right after the `--fail-fast` line, identical to mdlist's line;
  - the footer has the three-line paragraph and the example;
  - every line is ≤ 80 columns;
  - `download --help` has no `--dir-per-title`, and `download <id> --dir-per-title` → exit 2 with download's hint.
- **AC23:** a parity table over `--dir-per-title=<v>` for each V-B1 value, run as `batch --dry-run --api-base file://<clean tree> list.csv out` and as `mdlist --dry-run … <L1> out`:
  - both commands give the same exit code;
  - on exit 0, both either have or lack the `Clean Title/` path component;
  - errors are exit 2 with each command's hint, never 100–114.
  - Also V-P1's flag positions, with `--json` after the positionals.
- **S1 edits:** `batch_parse_test.cpp:214-224` and `mdlist_parse_test.cpp:235-272`.

**Unchanged and must pass as-is:** `batch_cli_test.cpp` (its footer test at `:1040` uses `contains` and still holds), `batch_jobs_list_test.cpp` (pins mdlist's "item" wording at `:465-500`), `mdlist_cli_test.cpp`, `download_*`.

### E2E cases (T3; `tests/e2e/cases/batch.cmake`, tests `e2e.batch.<case>`)
`<csv>` is `${_batch_in}/<name>.csv`. Every case carries `EXPECT_NO_ANSI` unless noted, and exact exit codes.

| Case | Args | Asserts |
|---|---|---|
| `dir-per-title-mixed-flat` (AC4) | `per-title-mixed.csv out` (no flag) | exit 0, the exact six-line flat stdout, `EXPECT_STDERR_EMPTY`, `EXPECT_FILES` all six |
| `dir-per-title` (AC25) | `--dir-per-title clean.csv out` | exit 0, stdout `^out/Clean Title/Clean Title - Vol\\. 01\\.epub\nout/Clean Title/Clean Title - Vol\\. 02\\.epub\n$`, empty stderr, `EXPECT_FILES`/`EXPECT_EPUB` under `out/Clean Title/`, `EXPECT_ABSENT` the flat files, `EXPECT_NO_GLOB "out/Clean Title/.*.part-*"` |
| `dir-per-title-dry-run` (AC26) | `+ --dry-run` | same stdout, empty stderr, `EXPECT_ABSENT out` |
| `dir-per-title-row-dir` (AC27) | `row-dir.csv` | today's `row-dir` stdout regex, empty stderr, files under `out/series/clean/`, `EXPECT_ABSENT "out/Clean Title"` |
| `dir-per-title-mixed` (AC28) | `per-title-mixed.csv` | exit 0, empty stderr, exact six-line stdout (four under `out/Clean Title/`, two under `out/series/clean/`), `EXPECT_FILES` all six, `EXPECT_ABSENT "out/Other Title"` |
| `dir-per-title-reserved` (AC29) | `dir-reserved.csv --dry-run` | exit 4, the bracketed stdout regex, `EXPECT_ABSENT out`, stderr exactly the 404 line, the "is used by the 'dir' of line 2" warning and `1 of 2 titles would fail \\(line 2\\)`. If V-B3 confirms R9, add `dir-per-title-reserved-real` (real run, `EXPECT_FILES` under `out/Clean Title [11111111]/`, exit 4) |
| `dir-per-title-unusable` (AC30) | `clean.csv --dry-run {{INPUTS}}/mdlist` | exit 1, empty stdout, exact remedy line plus `1 of 1 title would fail \\(line 1\\)` |
| `dir-per-title-row-dir-file` (AC31) | `row-dir-file.csv --dry-run {{INPUTS}}` | exit 1; line 2 is today's error with no suffix; line 3 is planned under `[^\n]*/inputs/Clean Title/`; closing `1 of 2 titles would fail \\(line 2\\)` |
| `dir-per-title-json` (AC32) | `--json clean-unknown.csv` | exit 4, `EXPECT_JSON`, line 1 `"dir": "out/Clean Title"`, line 2 `"dir": "out"` |
| `dir-per-title-no-chapters` (AC32) | nochapters tree, `nochapters-mixed.csv` | exit 7, today's mixed stderr regex, `EXPECT_ABSENT out` |
| `dir-per-title-duplicate` (AC33) | `two-clean.csv` | exit 0, stderr exactly the `same as line 1; skipped` warning, files under `out/Clean Title/` |
| `dir-per-title-tty-progress` (AC39) | `clean.csv`, `TTY` | `\\(1/1\\) \\[1/2\\] Vol\\. 1: …out/Clean Title/Clean Title - Vol\\. 01\\.epub` as in `tty-progress` |
| `dir-per-title-tty-json` (AC39) | `--json`, `TTY` | one JSON document, no progress |

### Edge-suite additions (T3 only; `_edge.cmake`, tests `e2e.edge.<case>`)
| Case | Asserts |
|---|---|
| `batch-json-dir-per-title-clicolor-force` (AC34) | `--json --dry-run --dir-per-title`, `ENV CLICOLOR_FORCE=1`: `EXPECT_JSON`, `EXPECT_NO_ANSI`, the `dir` and `path` values, `EXPECT_ABSENT out` |
| `batch-dir-per-title-dev-full` (AC35) | dry run, `STDOUT_FILE /dev/full`: exit 1, stderr exactly as in today's `batch-dev-full` |
| `batch-dir-per-title-sigpipe`, `…-parent-ignores` (AC36) | `bulk.csv --dry-run --layout chapters --dir-per-title`, `PIPE_HEAD 1` (with `PARENT_IGNORES_SIGPIPE` for the second): SIGPIPE, one line, empty stderr |
| `batch-dir-per-title-invalid-utf8-dir-json` (AC37) | as today's `batch-invalid-utf8-dir-json`, plus the flag: exit 0, `EXPECT_JSON` |
| `batch-dir-per-title-utf8-dir`, `…-lc-c` (AC37) | DIR `Zoë`: `^Zoë/Clean Title/Clean Title - Vol\\. 01\\.epub\n…$`, `EXPECT_ABSENT "Zoë"` |
| `batch-dir-per-title-stdin-null` (AC38) | `batch --dir-per-title - out`, `STDIN NULL`: exit 0, the `<stdin>: lists no titles` warning, `EXPECT_ABSENT out` |

### Sanitizer controls
No new risk class: no new threads, C APIs or raw buffers. The existing controls must fire in the run that reviews T1 and T2. Optional, because the project is threaded and rows run a FetchPool: `nice pixi run tsan -R 'cli\.batch --dir-per-title'` once, from `memory-safety-reviewer`.

### Release-test
`nice pixi run release-test` in T3's Done-when, so every new case also runs on the hardened install. No release-surface change (no CMake, preset or link change).

### Goldens that will change
| Golden | Why | Task | Bless request |
|---|---|---|---|
| `batch.help` | the new option line, footer paragraph and example | T2 | `pixi run bless '^e2e\.batch\.help$'` |

**Must not change:** `batch.dry-run`, `batch.json-dry-run`, `mdlist.help`, `mdlist.dry-run`, `mdlist.json-dry-run`, `download.*`, `edge.help` and `edge.version`. Any diff there is a regression, never a bless. A second bless round happens only if `cli-ux-reviewer` rewords the help at the board.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

- **`TitleDirRegistry::reserve(std::string_view row_dir, …)`:**
  - `row_dir` borrows `job.sub_dir`, owned by the caller's `std::vector<BatchJob>`, which outlives `run_batch_jobs`;
  - the first segment is a sub-view of `row_dir`, used only for the call;
  - the key must be copied into an owned `std::string`, and `MangaId` stored **by value**, never as a `const MangaId*` or a view into `jobs`;
  - the AC8 ownership test feeds a temporary.
- **String-view keys in the registry:** `find(string_view)` on `std::map<std::string, …, std::less<>>` maps is heterogeneous lookup only; no `string_view` is ever a key or a value. `suffixed()` borrows `manga.str()` (the parameter) for one `std::format`, as today.
- **Iterators:** inside `claim()`, an iterator from `reserved_` or `by_name_` must not be used after an insertion it depends on. `std::map` insertion keeps iterators valid, but read `holder->second.line`/`item` before the `try_emplace` calls, as today's code does. `reserve()` is never called while a `claim()` is running: everything is on one thread.
- **Reservation lifetime vs the runner:** `dirs` is declared before `runner` in `run_batch_jobs`'s frame, so it is destroyed after it. The reservation loop runs after `dirs` is constructed and before `run_batch`. There is exactly one registry per run, never copied, moved, or captured by an escaping lambda. `ConsoleTitleDirResolver` keeps its borrowed `TitleDirRegistry*` (never null) and now stores `BatchSource` **by value**. No `string_view` noun is stored; `row_noun` returns static storage and is called per message.
- **`claimed()`** still returns `const std::optional<std::string>&` with `MANGADEX_DL_LIFETIMEBOUND`, read in the same row before the next `begin()` (unchanged).
- **CLI11 bound variable:** `args_.dir_per_title` is a member of `BatchCommand`, owned by `run()`'s command list, so it outlives `parse()` and dispatch, like `fail_fast`.
- **Threads:** the registry, resolver and reservation loop run only on the calling (main) thread. FetchPool workers never reach them, as `dir_per_title.cpp`'s header comment documents. No new thread.
- **No C APIs:** names reach the filesystem only through the existing `check_row_dir` and `OutputDir` paths (`std::filesystem::path`).

## Security note (for `security-reviewer`)

- **New untrusted input to a name:** a CSV `dir` cell's first segment becomes a reservation key and appears in a warning. It is never used as a path by the new code, and it was already validated by `validate_list_dir` (no controls, invisible or bidi characters, `..`, absolute paths or `\`; ≤ 255 bytes per segment).
- **Directory names from CSV:** a `title` cell now also names a directory, through `validate_title_override` → `sanitize_file_stem` → the engine's `is_single_safe_component` re-check. It is one component inside DIR, ≤ 189 bytes with a suffix. The CSV could already choose any contained `dir`, so it gains no new reach.
- **Output safety:** every new warning goes through the observer or `print_warning`, which apply `terminal_safe`. Nothing is formatted with a runtime format string.
- **Unchanged:** symlinks inside DIR are followed (OD1). No new file, process, env, network or secret surface.
- **Memory:** bounded by the 10 000-row cap, at most one reservation per row.
- `security-reviewer` runs at the feature board because CSV-derived directory names are involved. `nice pixi run audit` has no new dependency to scan.

---

## Rollout & risk

- **CLI contract:** additive only. No renamed flag, no new or changed exit code, no new `--json` key, and `format_version` stays 1. Without the flag, every batch output is byte-identical (enforced by AC1–AC4, AC9 and the unchanged goldens). mdlist's bytes are unchanged (pinned by `batch_jobs_list_test.cpp` and every mdlist E2E case). A script that passed `batch --dir-per-title` used to get exit 2 and now runs; that is the intended addition. No migration is needed.
- **Riskiest task: T1's edit inside `claim()`**, the shared registry that mdlist depends on. De-risked three ways: the new branches are reachable only once `reserved_` is non-empty; the unchanged `title_dir_test.cpp` and every mdlist test are a byte-for-byte lock on the old rules; and the full-uuid change only adds "or reserved by another manga" to the existing condition.
- **Second risk: the help golden and the S1 assertions.** De-risked by doing them in one task (T2) with one bless request, and by your explicit OK on S1 at this gate.
- **User Test Plan seeds:** AC40 `[human: real data, network and a real library app · terminal]`; AC41 `[human: terminal-emulator rendering · retryable]`. `cli-tester` covers a real SIGINT and SIGTERM during a `--dir-per-title` row and between rows (S5), and confirms that `git status` stays clean after the `{{INPUTS}}` dry-run cases.

## Refactor triggers

| Item refuted | Tasks that change |
|---|---|
| V-B1 (a flag value form errors differently) | T2: AC23's parity table only. Parity with mdlist is the contract, so no code changes |
| V-P1 (flag position or DIR binding) | T2 `bind` (e.g. ordering) and AC23; ask the planner if it needs `cli.cpp`, a serialization point that becomes its own task |
| V-B5 (rendered width or alignment) | T2 footer text; cli-ux rewording; the bless request text |
| V-B4 (an existing initializer or check breaks) | T1 member placement or headers_test additions; never edit existing checks without a new gate |
| V-P2 (registry no longer nothrow default-constructible) | T1: change the `Reservation` container (e.g. `std::vector<MangaId>`) or the map shape; the static_assert stays |
| V-P3 (no `MangaId` ordering for `std::set`) | T1: `std::vector<MangaId>` with a linear `contains` (≤ 10 000 rows) |
| V-B6 / V-P5 (order or prefix differs) | T2/T3 exact stderr assertions (AC12, AC13, AC15, AC16, AC29, AC30); if the engine order differs from the spec, back to the designer (no engine edit is planned) |
| V-P6 (closing or stop wording differs) | T2/T3 regexes for AC16, AC29, AC30, AC31 |
| V-B3 (balanced brackets fail in `EXPECT_FILES`) | T3: drop the optional `dir-per-title-reserved-real` case; AC29 stays a dry run |
| V-B3 (balanced brackets work) | T3: add `dir-per-title-reserved-real`; T3 updates the inputs README note |
| V-P7 (shared `{{INPUTS}}/mdlist` input races or creates files) | T3: a new committed `tests/e2e/inputs/batch-dirs/Clean Title` regular file for the batch case, documented in the inputs README |
| V-P8 (anchor rules differ) | T4: heading and link text only |

## Relevant paths

- Spec: `/home/user/Mangadex-dl/docs/design/batch-dir-per-title-spec.md`
- T1:
  - `/home/user/Mangadex-dl/include/mangadex_dl/batch.hpp`
  - `/home/user/Mangadex-dl/include/mangadex_dl/title_dir.hpp`
  - `/home/user/Mangadex-dl/include/mangadex_dl/options.hpp`
  - `/home/user/Mangadex-dl/src/core/batch_plan.cpp`
  - `/home/user/Mangadex-dl/src/core/title_dir.cpp`
  - `/home/user/Mangadex-dl/tests/unit/interfaces/headers_test.cpp`
  - new `/home/user/Mangadex-dl/tests/unit/title_dir/title_dir_reserve_test.cpp`
  - new `/home/user/Mangadex-dl/tests/unit/batch/batch_plan_dir_per_title_test.cpp`
- T2:
  - `/home/user/Mangadex-dl/src/app/cmd_batch.cpp`
  - `/home/user/Mangadex-dl/src/app/cmd_batch_jobs.hpp`
  - `/home/user/Mangadex-dl/src/app/cmd_batch_jobs.cpp`
  - `/home/user/Mangadex-dl/src/app/cmd_download_run.hpp`
  - `/home/user/Mangadex-dl/tests/cli/batch_parse_test.cpp`
  - `/home/user/Mangadex-dl/tests/cli/mdlist_parse_test.cpp`
  - new `/home/user/Mangadex-dl/tests/cli/batch_dir_per_title_test.cpp`
  - new `/home/user/Mangadex-dl/tests/cli/batch_dir_per_title_parse_test.cpp`
  - golden `/home/user/Mangadex-dl/tests/e2e/golden/batch.help.stdout` (bless only)
- T3:
  - `/home/user/Mangadex-dl/tests/e2e/cases/batch.cmake`
  - `/home/user/Mangadex-dl/tests/e2e/cases/_edge.cmake`
  - `/home/user/Mangadex-dl/tests/e2e/inputs/README`
  - new `/home/user/Mangadex-dl/tests/e2e/inputs/batch/per-title-mixed.csv`
  - new `/home/user/Mangadex-dl/tests/e2e/inputs/batch/dir-reserved.csv`
- T4:
  - `/home/user/Mangadex-dl/README.md`
  - optional `/home/user/Mangadex-dl/docs/design/list-spec.md`
- Reused, unchanged:
  - `/home/user/Mangadex-dl/src/core/download/dir_per_title.cpp`
  - `/home/user/Mangadex-dl/src/core/mdlist_plan.cpp`
  - `/home/user/Mangadex-dl/src/app/cmd_mdlist.cpp`
  - `/home/user/Mangadex-dl/tests/support/mdx_twin.hpp`
  - `/home/user/Mangadex-dl/tests/unit/title_dir/title_dir_test.cpp`
  - `/home/user/Mangadex-dl/tests/cli/batch_jobs_list_test.cpp`
