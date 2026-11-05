# Design + plan: batch's own remedy for an unusable `--dir-per-title` directory

Follow-up to `batch-dir-per-title-spec.md` / `batch-dir-per-title-plan.md` (cli-ux board note on AC16). Small change: one message, one seam field, no new flag, exit code or dependency.

## Design

**Today.** When a derived title directory is unusable (say a regular file `DIR/Clean Title`), the engine (`src/core/download/dir_per_title.cpp`) appends a remedy that does not depend on the source:
`; rename or remove it, or run without --dir-per-title`

**Change.** batch has a narrower way out that mdlist lacks: a row's `dir` cell replaces the derived directory. So batch's line becomes:

```
mangadex-dl: error: titles.csv:1: cannot open directory 'out/Clean Title': Not a directory; rename or remove it, give that line a 'dir' cell, or run without --dir-per-title
```

The same text (without the `mangadex-dl: error: <label>:<line>: ` prefix) is the row's `--json` `error`.

**Unchanged.**
- mdlist's text, byte for byte: `list:1: <uuid>: cannot open directory '…': Not a directory; rename or remove it, or run without --dir-per-title`.
- An explicit-`dir` row that runs into a file keeps today's message, with no remedy (AC31). That is the runner's own pre-flight, not the engine's.
- Exit codes (row `failed`, exit_code 1, the batch continues), stream split, `--fail-fast`, `-q`.

## The seam

The remedy stays in the engine's message (one `Errc::row_dir` error, one place that joins it), but the resolver may name it. The resolver is already the source-aware side: it words its warnings with `row_noun(BatchSource)`.

```
cmd_batch_jobs.cpp                          core: resolve_title_dir
ConsoleTitleDirResolver::resolve()  ──►  TitleDirChoice{name, warning, remedy}
   remedy = row_dir_remedy(source)           │
     csv    -> "rename or remove it,         ▼
               give that line a 'dir'     check_row_dir(DIR, name) == row_error ?
               cell, or run without            message + "; " + (remedy ? *remedy
               --dir-per-title"                                  : default_row_dir_remedy)
     mdlist -> nullopt (engine default)
```

- `TitleDirChoice` (`include/mangadex_dl/title_dir.hpp`) gains `std::optional<std::string> remedy{}`. It stays an aggregate with a defaulted `operator==`. nullopt means the engine's default, so mdlist and any resolver that names none (the unit fakes, the headers test) are unchanged by construction.
- `std::optional<std::string>`, not a view: the choice owns its strings, as it already does for `name` and `warning`.
- Alternatives considered: a new virtual on `TitleDirResolver` (it widens the frozen interface for one constant), or a `DownloadServices` field (it has to be threaded through `run_title`, away from the source-aware code). Both are bigger for the same result.

## Plan (one track, no parallel batches)

| Task | Owns |
|---|---|
| Seam field + engine join | `include/mangadex_dl/title_dir.hpp`, `src/core/download/dir_per_title.cpp`, `src/core/download/internal.hpp` (doc) |
| batch remedy | `src/app/cmd_batch_jobs.cpp` (`row_dir_remedy(BatchSource)`, set in `resolve()`) |
| Tests | `tests/unit/download/download_dir_per_title_test.cpp` (remedy replaces the default; none keeps it), `tests/unit/interfaces/headers_test.cpp` (default nullopt, equality), `tests/cli/batch_dir_per_title_test.cpp` AC16 (+ the `--json` `error`), `tests/e2e/cases/batch.cmake` `dir-per-title-unusable` (AC30) |
| Docs | `README.md` "Where the batch files go", spec AC16/AC30 and the stderr bullet, plan V-P5 |

The shared public header `title_dir.hpp` is a serialization point, so this is one track.

mdlist byte-identity is pinned by the tests left untouched: `tests/cli/mdlist_cli_test.cpp`, `tests/cli/batch_jobs_list_test.cpp` (AC39) and `tests/e2e/cases/mdlist.cmake` (`dir-per-title-row-dir-file`).

`[VERIFY]` items: none. No new API, library feature, flag or config key is used.

Memory-safety watch-list: `row_dir_remedy` returns views of string literals (static storage), copied into the owned `choice.remedy` before `resolve()` returns; the engine reads `choice` (a local) only inside `resolve_title_dir`.

Goldens: none affected. AC30's stderr is an `EXPECT_STDERR_REGEX` in the case file, not a golden, so no re-bless is needed.
