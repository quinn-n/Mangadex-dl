# Feature spec (rev 2): `download` skips files that already hold every chapter, and rebuilds by reusing pages

Status: APPROVED at the design gate (2026-10-19), with amendment 5. Produced by `cli-designer`.

**Gate decisions (user, 2026-10-19):**
1. No `--redownload-existing`. `--overwrite` is the only force flag, with reworded help.
2. Page reuse now. Held chapters are copied from the old EPUB, and only new ones are downloaded.
3. No legacy class. The user has no prior files, so a file without a manifest is `foreign`.
4. Diverged and foreign files give exit 0 plus a warning. The OPF-CRC "changed by another program" guard stays.
5. **Re-pick on `--group` (this amends "keep the held release" below).**
   - **When it applies:** only when `--group` was passed, and only to a held release H matched by **number** (not by id) to a planned release P.
   - **The rule:** P replaces H only when P ranks **strictly better** than H under the `--group` priority list.
     - A release's rank is the index of the first `--group` entry that matches any of its groups.
     - A release matching no entry ranks worst.
   - **Replacing:** H's pages are dropped and P is downloaded.
   - **Keeping:** without `--group`, or when the ranks are equal (including both unmatched), H is kept, as described below. This means a file never flaps between two releases that `--group` ranks equally.
   - **Effect on classification:** a re-picked chapter counts as "missing". So a file whose chapter numbers are all held, but where some release ranks worse under `--group`, is `incomplete` and is rebuilt. The other chapters are reused and the re-picked ones downloaded. Diverged rules are unchanged.
   - **Report:**
     - Re-picked ids go in `new_chapters`.
     - `skipped_chapters` reconciliation follows the normal plan choice: H is not held after the rebuild, so no "kept" substitution is needed for it.
     - `-v` line: `<path>: Vol. 1 Ch. 1: replacing the release in the file (<group>) with <group>'s (--group)`.
   - **Acceptance:**
     - `[unit]` rank comparison: better replaces; equal or worse keeps; no `--group` keeps.
     - `[cli]` `clean`, then `clean-regrouped` **with `--group <B>`** → both volumes are rebuilt with B's releases (B's images must be served by that fixture variant), and the files are byte-identical to a fresh B download.
     - `[cli]` the same run **without** `--group` → nothing is rebuilt (criterion 10).
     - `[cli]` `--group <C>`, where C matches neither release → nothing is rebuilt.

Designer decision to confirm at plan approval: a rebuild may mix `data` and `data-saver` chapters, because quality is not compared.

---

**Intent:** a re-run of `mangadex-dl download` does three things:
- It leaves alone every existing EPUB that already holds every chapter the plan would put in it.
- When MangaDex has added chapters to a file, it rebuilds that file atomically. Chapters already held are copied from it, and only the new ones are downloaded.
- `--overwrite` forces a full re-download.

**Assumptions (from the repo):**
- Today's EPUBs don't record chapter UUIDs. The only per-book record is the OPF `dc:identifier` `urn:mangadex:<id>:<lang>:<unit>`.
- Every zip entry is STORED, including page images, because `ZipWriter::add_stored` is the only writer. No inflate is needed.
- Page entries are `OEBPS/images/pNNNNN.<ext>`, numbered per file. Pages are renumbered on a rebuild, and their XHTML is regenerated. Only image bytes are copied.
- Limits reused: `max_image_bytes` (32 MiB) and `max_pages_per_chapter` (5000) in `mangadex_api.hpp`, plus `sniff_image` and `max_image_side` in `image.hpp`.

## Summary

| Existing file | What happens |
|---|---|
| absent | written |
| complete | skipped (`exists`) |
| incomplete (missing chapters only) | rebuilt and replaced atomically: held chapters copied, missing ones downloaded |
| anything else | left alone, with a warning |

There are no new flags and no new exit codes.

## Command surface (CLI11)
- Unchanged. `cli.cpp` is not touched, and there are no prompts.
- `--overwrite` help becomes `"Re-download files even if up to date"`.
- The footer paragraph becomes:
  ```
  Running the same command again skips files that already hold every chapter
  and rebuilds a file when MangaDex has added chapters to it, downloading only
  the new ones, so an interrupted run can be restarted and a series kept up to
  date.
  ```
- Re-bless `e2e.download.help`.

## Input & output
- Precedence is command line > default only. There is no config file, no env var and no `envname()`. stdin is never read.
- New untrusted input: the existing `DIR/<name>.epub` files.

### Manifest
- A STORED entry, `META-INF/mangadex-dl.json`, written **last**.
- Compact, deterministic JSON, with no clock and no version string. It doesn't depend on `--jobs`.

| Key | Type | Meaning |
|---|---|---|
| `format` | int `1` | |
| `generator` | `"mangadex-dl"` | |
| `identifier` | string | equals `dc:identifier` |
| `opf_crc32` | 8 lowercase hex | CRC-32 of the `OEBPS/content.opf` bytes |
| `cover` | null \| `{file_name, entry}` | MangaDex cover file name and its zip entry (`OEBPS/images/cover.<ext>`) |
| `chapters` | array, in reading order | `{id, volume\|null, chapter\|null, title, groups:[{id,name}], publish_at, updated_at, quality, images:[entry names in page order]}` |

- The record holds everything the EPUB builder and the report need. Media type and W×H are **not** stored: they are re-derived with `sniff_image` when the page is copied, and that sniff is part of validation.
- Labels are re-normalised at read time with the current `normalize_label`.
- Rejected alternatives: OPF `<meta>` would need an XML scanner over untrusted bytes. A sidecar state dir drifts from the files, and the spec rules out state.
- EPUB 3.3: META-INF files "MUST NOT be listed in the manifest". META-INF is reserved for configuration files, which puts this entry in a grey zone (`com.apple.ibooks.display-options.xml` is precedent). See V1.

### Inspection (every planned file whose name exists, unless `--overwrite`)
1. **Open.**
   - `openat(dirfd, name, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`, then `fstat`.
   - Not `S_ISREG` → **foreign** ("not a regular file").
   - `ENOENT` → absent.
   - `EACCES`/`EPERM`/`EIO` → **foreign** ("cannot be read").
   - Any other errno → `Errc::failure`, exit 1.
   - Record the identity `(st_dev, st_ino, st_size, st_mtim)`.
   - In a real run, **keep the fd open until publish**. Every later read goes through it.
2. **Zip structure.**
   - `pread` loops retry `EINTR`; a short read means foreign.
   - 22 ≤ size ≤ `zip_max_u32`.
   - The EOCD sits exactly at `size-22` with comment length 0.
   - Single disk; the two entry counts are equal and ≤ `zip_max_entries`; no zip64 sentinels.
   - `cd_offset + cd_size ≤ size-22`, computed with overflow builtins. `cd_size ≤ 8 MiB`.
   - Walk the central directory with bounds checks. Entry 0 is `mimetype`. A duplicate name means foreign.
   - Index name → `{method, flags, crc, csize, usize, local offset}`.
3. **Manifest entry.**
   - Method 0, flags 0, `csize == usize ≤ 16 MiB`.
   - The local header agrees with the CD.
   - The data lies in `[0, cd_offset)`.
   - The CRC matches.
4. **Parse.**
   - Depth pre-scan, a non-throwing parse and `FieldReader` (`json_util`), with a caller-chosen cap and error class.
   - Every type is checked:
     - strings ≤ 4 KiB;
     - ids are UUIDs;
     - ≤ `max_pages_per_chapter` images per chapter;
     - ≤ `zip_max_entries` images in total.
   - `format > 1` → foreign ("made by a newer mangadex-dl"). Unknown keys are ignored.
5. **Identity.**
   - No manifest → **foreign** ("not an EPUB made by mangadex-dl").
   - A different identifier → **foreign** ("holds a different book").
   - `opf_crc32` ≠ the CD CRC of `OEBPS/content.opf` → **foreign** ("changed by another program").
6. **References** (central directory only):
   - Every `images` name and `cover.entry` matches `^OEBPS/images/(p[0-9]{5}|cover)\.(jpg|png|gif|webp)$`.
   - Each exists in the CD and is referenced exactly once.
   - Each is method 0 with flags 0, and `0 < csize == usize ≤ max_image_bytes`.
   - Its local header agrees with the CD, and its data lies in `[0, cd_offset)`.
   - Any failure → **foreign** ("not an EPUB made by mangadex-dl").

Buffers: CD ≤ 8 MiB, plus manifest text ≤ 16 MiB (freed after parsing), plus the parsed record. Inspection runs one file at a time, on the calling thread.

### Decision rules (pure core function, table-tested)

**Matching.** Each planned chapter matches at most one held chapter, and the first rule that applies wins. The rules run as three passes over the whole plan (all id matches, then all number matches, then loose-into-volume), so a number match never takes a held chapter that another planned chapter matches by id:
1. The same id, case-insensitive.
2. The same key: (normalised volume or none, normalised chapter). A null or empty chapter label is keyed by id only, as dedupe does.
3. A planned (v≠none, c) against a leftover held (none, c): a loose chapter that was later given a volume.

Number identity comes first because a `--group` change gives a different UUID for the same chapter.

| missing | extra | `existing` | action |
|---|---|---|---|
| ∅ | any | `complete` | skip, `exists` |
| ≠∅ | ∅ | `incomplete` | rebuild with reuse, replace |
| ≠∅ | ≠∅ | `diverged` | leave alone + warning |

`diverged` stays "leave alone". When MangaDex re-uploads or renumbers a chapter under a new id, a union rebuild would silently hold the same content twice.

**Rebuild rules:**
- **Which release goes in the file:**
  - matched by id → the planned `Chapter` (fresh metadata);
  - matched by number with a different UUID → **keep the held release**: id, title, groups, publish_at, updated_at and quality come from the manifest, and the volume and chapter labels come from the plan.
  - Nothing held is downloaded again.
- **Final chapter list:**
  - It follows the plan's reading order, with substitutions, and unmatched planned chapters are downloaded.
  - Metadata comes from that list through the existing functions: `contributors_of`, `modified_of`, `nav_label`/`nav_group`, and the manifest.
  - `display_title`, `identifier` and `series_position` come from the plan.
  - `planned_pages` = held image count + planned pages.
- **Cover:** taken from the plan. If the cover `file_name` equals the manifest's `cover.file_name` and the entry validates, it is copied. Otherwise it is downloaded, and a failure is only a warning.
- **Quality:** not compared. A rebuild may mix qualities; each chapter is recorded with its own (document this).
- **Layouts:** the same rules apply to `--layout chapters` and `--layout single`.
- **`--title`:** a new name is treated as absent, and the old file is left as an orphan.

### Copying pages
- **Pre-pass**, before the temp file exists. Reused chapters are read page by page, one image in memory at a time:
  - `pread`;
  - CRC check against the CD;
  - `sniff_image` (sides non-zero and ≤ `max_image_side`);
  - the sniffed type matches the extension;
  - each page's `ImageInfo` is stored.
  - **Any failed page makes that chapter fall back to downloading** the planned release for its key, with a warning. Interruption is checked once per page.
- **Build.** `EpubBuild` processes chapters in order, as today.
  - A reused chapter is copied on the calling thread: no FetchPool, no at-home lookup, no network. Each page is `pread`, its CRC re-checked, then `add_nav_entry` and `add_page` in page order, and `on_pages` progress is updated.
  - A downloaded chapter goes through the pool, unchanged.
  - The `EpubWriter` call sequence is identical to a fresh build, which is what gives byte identity.
- **Failure during the copy** (a CRC change since the pre-pass, an `EIO` or a short read):
  - the file is `failed`, with "the existing file changed or could not be read during the update";
  - the temp is discarded and the old file is kept; exit 6;
  - there is no fallback mid-build.
- **Memory:** about `max(jobs × 32 MiB, 32 MiB)` + the CD index (≤ 8 MiB) + the parsed manifest. Copied and downloaded chapters never overlap. One extra fd per file being built.

### Publish
1. Re-`fstatat(AT_SYMLINK_NOFOLLOW)` the name.
2. Then act on the result:
   - **identity matches** → `renameat` over the target (with `O_TMPFILE`: link it to `.part-` first, as `--overwrite` does);
   - **name vanished** → publish without clobbering: `written`, `replaced:false`;
   - **name changed** → discard the temp: `exists`/`foreign` ("changed while it was being updated").

The check-to-rename window is microseconds, which is accepted for a user-owned DIR (document it, `cpp-security` §4). `--overwrite` is unchanged.

### Report and output
- **Statuses:**
  - No new statuses. `complete`/`diverged`/`foreign` → `exists`. `incomplete` → `written`+`replaced:true`, or `planned`+`replaced:true`, or `failed`.
  - `format_version` stays 1.
  - Document that `exists` now means "left alone", and that `replaced:true` can appear without `--overwrite`.
- **New `epubs[]` keys**, after `replaced`:
  - `existing`: null \| `"complete"` \| `"incomplete"` \| `"diverged"` \| `"foreign"` \| `"unchecked"` (`--overwrite`).
  - `new_chapters`: [ids].
  - `reused_chapters`: [ids]. In a real run it excludes fallbacks; in a dry run it lists matched chapters, structurally checked only.
- **Existing keys:**
  - `epubs[].chapters` lists what the file holds, with substitutions applied; `pages` is the held count.
  - `skipped_chapters` is reconciled. When held release H is kept instead of the planned release P, H's `duplicate` entry is removed and P is added as `{reason:"duplicate", kept:H}`. Any other release whose entry said `kept:P` is rewritten to `kept:H`, so `kept` always names a release the file holds. This is a pure function, and a dry run applies it to matched chapters.
  - `summary` is unchanged.
- **stdout:** one path per written file, now including updates.
- **stderr** (all hidden by `-q`; passed through `terminal_safe`):
  - Closing note, with no prefix and no color: `mangadex-dl: N files are already up to date; pass --overwrite to download them again` (`1 file is …`).
  - Foreign: `warning: <path>: left alone: <fixed reason>; <remedy>`.
    - Reasons: not a regular file · cannot be read (`<strerror>`) · not an EPUB made by mangadex-dl · holds a different book · changed by another program · made by a newer mangadex-dl · changed while it was being updated.
    - Remedies: "rename or remove it, or use another DIR" / "use another DIR or --title" / "pass --overwrite to replace it".
    - Nothing read from the file is ever echoed.
  - Diverged: `warning: <path>: MangaDex lists N chapters this file lacks, but the file holds M chapters MangaDex no longer lists; left it alone (pass --overwrite to rebuild it without them)`.
  - Damaged held chapter: `warning: <path>: Ch. 3 in the existing file is damaged; downloading it again`.
  - Failed update: `error: <path>: <error> (the existing file was kept)`.
- **`-v`:**
  - `<path>: up to date (N chapters)`
  - `updating <path>: 1 new chapter, 2 reused (Ch. 4)` (≤ 5 labels, then `and K more`)
  - in a dry run: `would update <path> (1 new chapter, 2 reused: Ch. 4)`
  - `<path>: Vol. 1 Ch. 1: kept the release already in the file (<held id>) instead of <plan id>`
- **Display:** progress and colour are unchanged, and the up-to-date note is never coloured.

## Exit codes
Unchanged; there are no new codes.
- **0:** every file was written, complete, diverged or foreign.
- **1:** a local fatal error, including an `EMFILE`-class error during inspection.
- **2:** usage.
- **3/4/5:** unchanged.
- **6:** a new file or an update failed, including a change or read error during the copy. The old file is kept.
- **70** is never expected; **86/130/141** as specified.

## Behaviour & edge cases
- **Outcomes:**
  - Success: updates are atomic, and only new chapters are downloaded.
  - Empty: everything is up to date → exit 0, empty stdout, the note.
  - Runtime failure:
    - an unreadable or corrupt file → warning, left alone;
    - damaged held pages → re-downloaded;
    - a failure during the copy or download → `failed`, exit 6, the old file intact.
  - SIGINT or SIGTERM during the pre-pass, the copy or the download: the temp is discarded and the old file stays intact.
- **`plan_error`:** a complete file → `exists`; an incomplete one → `failed`, the old file kept.
- **Edge suite:**
  - `> /dev/full` during an update → exit 1 + `write error`, and the file is complete.
  - `| head -1` across two updates → 141, and every `.epub` is a valid zip.
  - stdin is never read, and there is no ANSI when output is not a TTY.
- **Hostile archives**, each of which must classify as foreign or fall back, never crash, and stay sanitizer-clean:
  - truncation;
  - a zip comment;
  - zip64 sentinels;
  - overflowing offsets or lengths;
  - duplicate names;
  - deflated entries;
  - an oversized or deeply nested manifest;
  - references to non-image entries.
- **Idempotent:** a run, an update, then another run reports up to date.
- **`--dry-run`:** reads the CD and the manifest only, never writes, and never creates DIR.

## Data & state
- **Reads:** `DIR/<name>.epub` read-only, through the inspection fd during a rebuild.
- **Writes:** the `.epub` (0644) and its temp only, plus the manifest inside new EPUBs.
- **Replacing a file:**
  - It applies only to our own `incomplete` files with an unchanged identity, and is never lossy.
  - It creates a new inode, so mode, owner and hard links are not kept.
  - The last rename wins.
- **Threads:** inspection and copying run on the calling thread. The FetchPool and tsan scope are unchanged.
- **Docs to update:** README Output and the JSON schema (around lines 145–169), the "never overwritten unless --overwrite" line (308), the `.part-` note (316), and `download-spec.md`.
- **Likely touch points:**
  - `epub.hpp`/`.cpp`: the manifest entry, OPF CRC, cover entry name;
  - `fs/output_dir.hpp`/`.cpp`: open-existing returning identity + fd; publish replace-if-unchanged;
  - a new core module for the bounded CD reader over a fakeable pread source, manifest read/write, reference validation and page copy;
  - a pure matching and final-list function;
  - `report.hpp`/`.cpp`;
  - `execute_phase.cpp`;
  - `cmd_download_output.cpp`, `cmd_download.cpp`;
  - `tests/support/mdx_fixture.hpp`;
  - the E2E driver.

## Acceptance criteria

**Fixtures** (in `mdx::all_fixtures()`, manga id `11111111-…`; update `tests/e2e/inputs/README`):

| Fixture | Contents | At-home docs + images served |
|---|---|---|
| `clean` | today: vol 1 = ch 1, 2 (group A); vol 2 = ch 3 (A) | all |
| `clean-grown` | `clean` + vol 2 ch 4 (A) | all |
| `clean-grown-reuse-only` | same API docs as `clean-grown` | **only ch 4** |
| `clean-regrouped` | ch 1–3 by group B (new UUIDs, A gone) | none |
| `clean-regrouped-grown` | B's ch 1–3 + vol 1 ch 2.5 (B) | **only ch 2.5** |
| `clean-mixed-reference` | A's ch 1–2 + B's ch 2.5 | all |
| `clean-diverged` | vol 2: ch 3 gone, ch 4 new | all |

1. `[unit]` Matching table:
   - same ids;
   - same numbers with different ids → complete, held release kept;
   - added chapter → incomplete with correct new/reused lists;
   - extras only → complete;
   - extras + missing → diverged;
   - a loose chapter moved into a volume;
   - id-keyed null labels;
   - a relabel with the same id;
   - normalisation (`"07"` = `"7.0"`).
2. `[unit]` Final list:
   - held-release substitution (plan labels, manifest metadata);
   - same-id chapters take plan metadata;
   - contributors and modified come from the final list;
   - `skipped_chapters` reconciliation.
3. `[unit]` Manifest round trip: deterministic, written last, STORED; `opf_crc32` and the images/cover names match the archive.
4. `[unit]` CD and manifest reader on the hostile cases → foreign, never an error or crash, sanitizer-clean.
5. `[unit]` Copy-path validation:
   - **Structural faults → foreign:**
     - a name not in the image pattern;
     - a duplicate reference;
     - an entry missing from the CD;
     - method 8;
     - csize ≠ usize;
     - size 0 or > 32 MiB;
     - data overlapping the CD or past the end of the file;
     - a local header that disagrees with the CD.
   - **Content faults → that chapter falls back:**
     - CRC mismatch;
     - non-image bytes;
     - a side of 0 or > 65535;
     - an extension/type mismatch.
   - **During the copy → `failed`, old file kept:** the CRC changes between the pre-pass and the copy, or a pread error or short read (fake source).
6. `[unit]` OutputDir:
   - a symlink, a FIFO (it doesn't hang) and a directory are each "not regular";
   - replace-if-unchanged replaces, writes (name vanished) or doesn't publish (changed), on both the `O_TMPFILE` and the fallback paths.
7. `[cli]` `clean` twice → exit 0, empty stdout, stderr exactly the note, JSON `existing:"complete"`. The existing re-run and `download_output_test` expectations are updated.
8. `[cli]` **Reuse proof:** `clean`, then `clean-grown-reuse-only`:
   - exit 0, and stdout is only the Vol. 02 path. Ch 3 isn't served, so downloading it would mean exit 6;
   - JSON: `incomplete`, `replaced:true`, `new_chapters:[ch4]`, `reused_chapters:[ch3]`;
   - Vol. 01 is byte-unchanged.
9. `[cli]` **Byte identity:** that Vol. 02 is byte-identical to a fresh `clean-grown` download into an empty DIR, at `-j1` and at `-j8`.
10. `[cli]` `clean`, then `clean-regrouped` → nothing rebuilt, exit 0.
11. `[cli]` **Regrouped + grown:** `clean`, then `clean-regrouped-grown`:
    - Vol. 01 is rebuilt with A's ch 1–2 (reused, not served) + B's ch 2.5 (downloaded);
    - contributors are A then B;
    - `skipped_chapters` lists B's ch 1–3 as duplicates with `kept` = A's ids (ch 3 via the complete Vol. 02);
    - `-v` prints the kept-release lines;
    - the file is byte-identical to a fresh `clean-mixed-reference` download;
    - Vol. 02 is complete.
12. `[cli]` **Damaged held page:** corrupt one image byte in the reused chapter of the old Vol. 02, then run `clean-grown`:
    - the damage warning appears, ch 3 is downloaded again, exit 0;
    - the file is byte-identical to a fresh download;
    - `reused_chapters:[]`.
13. `[cli]` `clean`, then `clean-diverged` → Vol. 02 untouched, the diverged warning, exit 0, `existing:"diverged"`.
14. `[cli]` **Foreign files:** no manifest, random bytes, another language's identifier, an OPF edited without updating its CRC:
    - each is left untouched with its reason, exit 0;
    - `--overwrite` replaces all of them (`unchecked`).
15. `[cli]` `--dry-run` on `clean-grown` over an existing DIR:
    - stdout is the Vol. 02 path;
    - `-v`: `would update … (1 new chapter, 1 reused: Ch. 4)`;
    - JSON `planned` + `replaced:true` + the lists;
    - no file is modified.
16. `[cli]` **Failed update:** a failed download of the new chapter → exit 6, the old file byte-unchanged, and the error ends "(the existing file was kept)". `-q` hides every new note. `--json --color=always` produces no ANSI.
17. `[e2e]` **Installed binary:** `PRE_RUN` on `clean`, then `clean-grown-reuse-only`:
    - exact stdout, exit 0;
    - `EXPECT_EPUB` Vol. 02;
    - no `.part-` file.
    - An up-to-date re-run case.
    - This needs a new driver keyword (`PRE_RUN`).
18. `[e2e]` Bless requests: `e2e.download.help` and `e2e.download.json-dry-run`.
19. `[edge]` After an update: `> /dev/full` → exit 1 + `write error`, and the file is complete. `| head -1` across two updates → 141, and every `.epub` is a valid zip.
20. `[tty-pty]` Progress appears while updating. The note isn't coloured. `warning:` is yellow, and plain under `NO_COLOR`.
21. `[human]` Rebuilt and fresh EPUBs open in ≥ 2 readers, and epubcheck accepts `META-INF/mangadex-dl.json` (V1).
22. `[human]` Live: on an ongoing series with a new chapter, only that chapter is fetched (`-vv`), with no 429 storm.
23. `[human]` Ctrl-C during the copy and during the download → 130, the old file intact, no temp left.

## Out of scope
- re-downloading held chapters whose pages changed under the same number;
- verifying the images of complete files;
- union rebuilds of diverged files;
- orphan cleanup;
- rebuilding on a quality change;
- keeping edits, mode, owner or links on replace;
- resuming within a file;
- a read-only DIR where everything is up to date (still exit 1);
- config/env, completions, man page.

## `[VERIFY]`
- **V1:** epubcheck on the extra `META-INF/mangadex-dl.json` reports no error or warning. Otherwise use an OPF `<meta>`.
- **V3:** an `openat(O_RDONLY|O_NONBLOCK)` of a FIFO with no writer returns at once, and `fstat` reports `S_ISFIFO`.
- **V4:** nlohmann_json 3.12 parses and destroys iteratively, and `depth_ok` plus a non-throwing parse with a 16 MiB cap covers the manifest.
- **V5:** `renameat` over a file a reader holds open is safe on Linux.
- **V6:** `pread` on our fd reads the inspected inode after the name is renamed over, with no `O_TMPFILE`/overlayfs surprise.
- **V7:** `sniff_image` gives the same `ImageInfo` on the copy path as on the download path for the fixture formats.
- **V8:** whether `tests/support/zip_reader.hpp` can be reused to read and modify entries in tests.
