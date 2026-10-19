# Feature spec: `download` skips files that already hold every chapter (chapter-aware skipping)

Status: DRAFT, awaiting the design gate (2026-10-19). Produced by `cli-designer`.

**Intent:** re-running `mangadex-dl download` leaves an existing EPUB alone if it already holds every chapter the plan would put in it. It rebuilds and atomically replaces a volume, chapter or single EPUB only when MangaDex has added chapters to it since it was written. `--overwrite` stays the way to force a re-download.

**Assumptions** (from the repo survey):
- EPUBs written today do **not** record chapter UUIDs.
  - The OPF has `dc:identifier` `urn:mangadex:<id>:<lang>:<unit>`, contributors (group names) and `dcterms:modified`.
  - nav/NCX hold human labels.
  - Nothing there is a reliable record of which chapters a file holds, so this feature adds a small manifest.
- File names don't include the language. The identifier does.
- Today `produce()` checks `dir.exists(name)` and reports `exists`. `--overwrite` publishes through `renameat`. New files go through `RENAME_NOREPLACE`/`linkat`.

## Summary
Each planned file is checked against the file of the same name in DIR:

| Existing file | What happens |
|---|---|
| absent | written, as today |
| **complete** | skipped (`exists`) |
| **incomplete** (only missing chapters) | rebuilt and atomically replaced |
| anything else | left alone with a warning saying why |

`--overwrite` means "re-download and replace every planned file, uninspected". There are no new exit codes and no new flags (see the gate decision on an alias).

## Command surface (CLI11)
- The invocation is unchanged. `cli.cpp` is not touched.
- Only the help text changes:
  - `--overwrite` help: `"Re-download files even if up to date"`.
  - The `download --help` footer paragraph becomes:
    ```
    Running the same command again skips files that already hold every chapter
    and rebuilds a file when MangaDex has added chapters to it, so an
    interrupted run can be restarted and a series kept up to date.
    ```
  - Both changes need a re-bless of `e2e.download.help`.
- `--redownload-existing`: the recommendation is **not** to add it.
  - The alternative is one option with two long names, `"--overwrite,--redownload-existing"`. That is additive and needs a golden re-bless.
  - `--overwrite` is never renamed (that would be a breaking change).
- No prompts.

## Input & output
- Precedence: command line > default. There is no config file and no env var. stdin is never read.
- New input: the existing `DIR/<name>.epub` files, treated as untrusted.

### The manifest
- A new STORED zip entry `META-INF/mangadex-dl.json`, written **last**.
- Compact, deterministic JSON with no clock or version string, so output stays byte-identical for any `--jobs`.

| Key | Meaning |
|---|---|
| `format` | int, 1 |
| `generator` | `"mangadex-dl"` |
| `identifier` | equals `dc:identifier` |
| `quality` | `"data"` or `"data-saver"`; informational |
| `opf_crc32` | 8 lowercase hex digits: the CRC-32 of the written `OEBPS/content.opf` |
| `chapters` | `[{id, volume\|null, chapter\|null, updated_at}]`, raw API labels, re-normalised at read time |

- EPUB 3.3: META-INF files must not appear in the OPF manifest, so the OPF doesn't change.
- `[VERIFY]` that epubcheck accepts the extra file. If it doesn't, fall back to an OPF `<meta>`.

### Reading an existing file (bounded, no symlinks, never clobbers)
1. Open and check what the name is.
   - `openat(dirfd, name, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`, then `fstat`.
   - Not `S_ISREG` → **foreign** ("not a regular file").
   - `ENOENT` → absent.
   - `EACCES`/`EPERM`/`EIO` → **foreign** ("cannot be read").
   - Any other errno → `Errc::failure` (exit 1).
2. Record `(st_dev, st_ino, st_size, st_mtim)` for the replace step.
3. Parse the zip; anything outside these rules is **foreign**.
   - `pread` loops retry `EINTR`. A short read → foreign.
   - Size is between 22 and `zip_max_u32`.
   - The EOCD sits exactly at `size-22` with a zero-length comment. It is single-disk, the entry counts agree, and there are no zip64 sentinels.
   - Bounds use overflow builtins, and `cd_size ≤ 8 MiB`.
   - Every central-directory field is bounds-checked.
   - Entry 0 is `mimetype`. Duplicate names → foreign.
4. Check the manifest entry.
   - It uses method 0, with no encryption bit and no data-descriptor bit.
   - `csize == usize ≤ 16 MiB`.
   - The local header matches the central directory, the data lies in bounds, and the CRC matches.
5. Parse and validate the manifest.
   - The JSON parse doesn't throw, the depth is pre-scanned (`json_util`), and the cap and error class are chosen by the caller.
   - Types are validated.
   - `format > 1` → foreign ("made by a newer mangadex-dl").
6. Check that it's the same book.
   - identifier ≠ planned → foreign ("holds a different book").
   - `opf_crc32` ≠ the central-directory CRC of `content.opf` → foreign ("changed by another program").
7. With no manifest, read at most the first 4 KiB of the STORED `OEBPS/content.opf`.
   - It contains the exact `<dc:identifier id="book-id">` + id + `</dc:identifier>` → **legacy**.
   - Otherwise → **foreign**.

### Decision rules (pure core function)
A chapter's identity is its **number** (normalised volume and chapter), not its UUID. A group change gives a new UUID for the same chapter.

Each planned chapter is matched against the file's chapters in this order:
1. the same id;
2. the same (volume, chapter) key (a null or empty chapter label is keyed by id only);
3. a planned (v, c) against a leftover file chapter (none, c), for a chapter that was loose and has since been given a volume.

| missing | extra | `existing` | action |
|---|---|---|---|
| ∅ | any | `complete` | skip, `exists` |
| ≠∅ | ∅ | `incomplete` | rebuild and replace |
| ≠∅ | ≠∅ | `diverged` | leave alone and warn (never shrink a file without `--overwrite`) |

- A **legacy** file (no manifest) is treated as complete and skipped, with a closing hint.
- With `--layout chapters`, the same rules apply to a one-chapter set.
- `--title` changes the name, so the old file isn't found and the new name is downloaded.
- Quality is not compared.

### Statuses, JSON, human output
- **No new status values.** `format_version` stays 1 (the changes are additive).
  - `incomplete` shows as `written`+`replaced:true` (`planned`+`replaced:true` in a dry run, or `failed`).
- New per-`epubs[]` keys, after `replaced`:
  - `existing`: `null`, `"complete"`, `"incomplete"`, `"diverged"`, `"legacy"`, `"foreign"`, or `"unchecked"` (under `--overwrite`);
  - `new_chapters`: an array of chapter ids.
- stdout: one path per written file, updated files included.
- stderr (all of it hidden by `-q`):
  - The closing note replaces "N files already exist…": `mangadex-dl: N files are already up to date; pass --overwrite to download them again`.
  - Legacy: `warning: N files were made by an older mangadex-dl and can't be checked for new chapters; pass --overwrite to download them again`.
  - Foreign: `warning: <path>: left alone: <fixed reason>; <remedy>`.
  - Diverged: `warning: <path>: MangaDex lists N chapters this file lacks, but the file holds M chapters MangaDex no longer lists; left it alone (pass --overwrite to rebuild it without them)`.
  - Failed update: `error: <path>: <error> (the existing file was kept)`.
- `-v`:
  - `<path>: up to date (N chapters)`
  - `updating <path>: N new chapters (Ch. 25, Ch. 26)`
  - in a dry run: `would update <path> (…)`

## Exit codes
Unchanged.
- **0:** every file was written, complete, legacy, diverged or foreign.
- **1:** a local fatal error.
- **6:** an update or a new file failed. The old file is kept.

## Behaviour & edge cases
- **Replace:**
  1. Build into a temp file.
  2. Re-`fstatat(AT_SYMLINK_NOFOLLOW)` the target.
  3. Then:
     - identity unchanged → `renameat` over it;
     - gone → publish no-clobber;
     - changed → discard the temp, report `exists` ("changed while it was being updated").
- Interruption: the temp is discarded and the old file stays intact.
- `--overwrite` still clobbers as it does today.
- A file with a `plan_error`: complete → `exists`; incomplete → `failed`, old file kept.
- Crafted archives classify as `foreign` and never crash.
- Idempotent: run → update → run again reports everything up to date.

## Data & state
- Reads `DIR/<name>.epub` read-only.
- Writes only the `.epub` files and their temps, as today.
- A replace creates a new inode: mode, owner and hard links are not preserved.
- Inspection runs on the calling thread.
- Docs: README (Output, JSON schema, the no-clobber line, the `.part-` note) and `download-spec.md`.

## Acceptance criteria
1. `[unit]` The matching table: same ids; same numbers with different ids; added chapter; extra only; extra + missing; a loose chapter moved into a volume; id-keyed chapters; relabel with the same id; normalisation.
2. `[unit]` Manifest round-trip: deterministic, written last, STORED, `opf_crc32` correct.
3. `[unit]` Every crafted-archive case is foreign, never an error or crash. Valid `ZipWriter` output parses.
4. `[unit]` Classification: a different identifier, `format: 2` and an OPF CRC mismatch are foreign. Legacy and foreign are told apart.
5. `[unit]` OutputDir:
   - open-existing on a symlink, a FIFO (it doesn't hang) and a directory;
   - replace-if-unchanged when the target is unchanged, missing or changed, on both the `O_TMPFILE` and the fallback paths.
6. `[cli]` A re-run: exit 0, empty stdout, the up-to-date note, JSON `existing:"complete"`.
7. `[cli]` `clean`, then `clean-grown`:
   - only the Vol. 02 path is printed;
   - Vol. 01 is byte-unchanged;
   - JSON shows `incomplete`, `replaced:true` and `new_chapters`;
   - a third run reports everything up to date.
8. `[cli]` `clean`, then `clean-regrouped`: nothing is rebuilt.
9. `[cli]` `clean`, then `clean-diverged`: the file is untouched, the warning is printed, exit 0.
10. `[cli]` A legacy file is skipped with the closing warning. A foreign file is untouched with a per-file warning. `--overwrite` replaces all of them (`unchecked`).
11. `[cli]` `--dry-run` on `clean-grown`: lists Vol. 02, prints `would update …`, and modifies nothing.
12. `[cli]` A failed update exits 6, the old file is byte-unchanged, and the message says "(the existing file was kept)".
13. `[cli]` `-q` hides the new notes. `--json --color=always` contains no ANSI. Updates are byte-identical with `-j1` and `-j8`.
14. `[e2e]` The installed binary with `PRE_RUN` on `clean`, then `clean-grown`. An up-to-date re-run case. This needs a new driver keyword.
15. `[e2e]` Golden re-bless: `e2e.download.help` and `e2e.download.json-dry-run`.
16. `[edge]` After an update: `> /dev/full` exits 1 and the file is complete. `| head -1` gives 141, and every `.epub` is a valid zip.
17. `[tty-pty]` The progress line shows during an update. The up-to-date note is uncolored. `warning:` is yellow, and plain with `NO_COLOR`.
18. `[human]` Updated and fresh EPUBs open in at least 2 readers. epubcheck accepts `META-INF/mangadex-dl.json`.
19. `[human]` Live: a newly published chapter re-downloads only the affected volume.
20. `[human]` Ctrl-C during an update exits 130, the old file is intact, and no temp is left.

New fixtures: `clean-grown`, `clean-regrouped` and `clean-diverged` in `mdx::all_fixtures()`.

## Out of scope
- reusing pages from the old EPUB (an update re-downloads the whole file);
- re-uploads of the same chapter number;
- orphan cleanup;
- automatic upgrade of legacy files;
- rebuilding on a quality change;
- preserving mode, owner or links;
- resuming within a file;
- config/env.

## `[VERIFY]`
- V1: epubcheck and the META-INF extra file.
- V2: CLI11 help rendering of a two-name flag (only if the alias is chosen).
- V3: `O_NONBLOCK` open of a FIFO with no writer.
- V4: nlohmann 3.12 parse limits for the manifest.
- V5: `renameat` over a file that a reader has open.
