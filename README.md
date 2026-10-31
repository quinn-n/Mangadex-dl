# Mangadex-dl

Downloads volumes and chapters from MangaDex and converts them into .epub or .pdf files.

`mangadex-dl download` takes a MangaDex title and writes fixed-layout EPUB 3 files: by default
one `.epub` per volume, plus one per chapter that has no volume ("loose" chapters).
`mangadex-dl batch` does the same for every title listed in a CSV file.
`mangadex-dl mdlist` does the same for every title in a public MangaDex list.

> **Status:** EPUB output only. PDF output is planned but not available yet.

## Contents

- [Requirements](#requirements)
- [Building](#building)
- [Usage](#usage)
- [Examples](#examples)
- [Running again](#running-again)
- [Many titles: `batch`](#many-titles-batch)
- [A MangaDex list: `mdlist`](#a-mangadex-list-mdlist)
- [Output](#output)
- [Exit status](#exit-status)
- [Network behaviour](#network-behaviour)
- [Environment and configuration](#environment-and-configuration)
- [Interruption and file safety](#interruption-and-file-safety)
- [Limits](#limits)
- [Security notes](#security-notes)
- [Development](#development)
- [License](#license)

## Requirements

- Linux on x86-64, glibc 2.28 or newer.
- **A TLS-capable libcurl, version 7.85.0 or newer**, installed as `libcurl.so.4` or
  `libcurl-gnutls.so.4` (on most distributions this is the `libcurl` / `libcurl4` package).
  - libcurl is not linked into the binary. It is loaded at runtime with `dlopen`, so
    `mangadex-dl --help` and `--version` work without it.
  - `libcurl.so.4` is tried first, then `libcurl-gnutls.so.4`. If neither is found, or the one
    found is too old or lacks TLS, `download` fails with exit 1 and a message that names the
    libraries it tried.
- CA certificates (the `ca-certificates` package). See [TLS and CA certificates](#tls-and-ca-certificates).

## Building

Every tool and dependency comes from [pixi](https://pixi.sh); nothing else is needed on the host.

```sh
pixi run release     # optimized, hardened build: build/default/release/mangadex-dl
pixi run package     # also runs the release tests; writes
                     # build/default/release/mangadex-dl-<version>-linux-x86_64.tar.gz (+ .sha256)
```

The release binary links libstdc++ statically and needs only glibc at startup; libcurl is
loaded at runtime (see [Requirements](#requirements)).

## Usage

```
mangadex-dl download <MANGA> [DIR] [OPTIONS]
```

| Argument / option | Default | Meaning |
|---|---|---|
| `MANGA` | required | Title UUID, or a title URL such as `https://mangadex.org/title/<uuid>/<slug>` (`http://`, `www.`, no scheme, a trailing `/`, `?query` and `#fragment` are accepted). One title per run. |
| `DIR` | `.` | Output directory. Created (mode 0755, subject to your umask) if missing. |
| `--volumes SPEC` | `all` | Volumes to download. |
| `--loose-chapters SPEC` | `all` | Chapters without a volume to download. |
| `--layout WORD` | `volumes` | `volumes`: one file per volume plus one per loose chapter. `chapters`: one file per chapter. `single`: everything in one file. |
| `-l, --lang CODE` | `en` | Translated language as a lowercase MangaDex code, e.g. `en`, `fr`, `pt-br`, `es-la`. |
| `--group NAME\|UUID` | none | Preferred scanlation group. Repeat it to give several, in priority order. |
| `--data-saver` | off | Download MangaDex's compressed images instead of the originals. |
| `--direction WORD` | `auto` | Page progression: `auto` (right-to-left for Japanese originals, left-to-right otherwise), `rtl` or `ltr`. |
| `--title TEXT` | from MangaDex | Title used in file names and EPUB metadata. |
| `--overwrite` | off | Re-download files even if up to date (see [Running again](#running-again)). |
| `-n, --dry-run` | off | Show what would be written; download and write nothing. |
| `-j, --jobs N` | `8` | Parallel image downloads, 1 to 8. |
| `--no-report` | off | Don't send image-server reliability reports to MangaDex@Home (see [below](#mangadexhome-reports)). |

Global options, accepted before or after `download`:

| Option | Meaning |
|---|---|
| `--json` | Write one machine-readable JSON document to stdout at the end. |
| `-q, --quiet` | Print only errors on stderr. |
| `-v, --verbose` | More detail on stderr: `-v` per file and the group choices, `-vv` per request. |
| `--color WORD` | Color stderr diagnostics: `auto` (default), `always` or `never`. stdout is never colored. |
| `--help`, `--version` | Print help or the version and exit 0. |

`mangadex-dl download --help` lists the same options with examples.

### Selecting volumes and chapters

`SPEC` is `all`, `none`, or a comma-separated list of items with no spaces:

| Item | Selects |
|---|---|
| `3` | volume (or chapter) 3 |
| `3-5` | 3 to 5, inclusive |
| `8-` | 8 and everything after it |
| `-4` | everything up to 4 |

Numbers may have a fraction (`10.5`). Labels that are not numbers (MangaDex allows any text) are
selected only by `all`. `--volumes` and `--loose-chapters` cannot both be `none`. There is no
chapter filter inside a volume.

### Which chapters are used

- Only chapters translated into `--lang` are considered.
- Chapters that are hosted on an external site, have no pages, or are marked unavailable are
  skipped with a warning.
- When several groups translated the same chapter, one is kept: the first `--group` that has
  it; otherwise the group that covers the most of your selection; then the release with more
  pages; then the earlier publish date; then the smallest chapter id.

### File names

| Layout | Example |
|---|---|
| `volumes` | `Title - Vol. 03.epub`, `Title - Ch. 012.epub` (a loose chapter), `Title - Ch. 012.5.epub` |
| `chapters` | `Title - Vol. 03 Ch. 021.epub` |
| `single` | `Title.epub` |
| any | `Title - Oneshot [1a2b3c4d].epub` for a chapter without a number |

Titles are made safe for file names: `/ \ < > : " | ? *` and control characters become `_`,
invisible and bidirectional-control characters are dropped, and the title part is cut to 150
bytes.

## Examples

```sh
# Every volume and loose chapter in English, into the current directory
mangadex-dl download https://mangadex.org/title/<uuid>/some-title

# Volumes 1 and 3 to 5 only, into ~/Manga
mangadex-dl download <uuid> ~/Manga --volumes 1,3-5 --loose-chapters none

# Only the loose chapters from 100 on
mangadex-dl download <uuid> --volumes none --loose-chapters 100-

# A title without volume information: one book instead of one file per chapter
mangadex-dl download <uuid> --layout single

# One file per chapter, in Brazilian Portuguese, preferring a given group
mangadex-dl download <uuid> --layout chapters --lang pt-br --group "Some Scans"

# See what would be written, as JSON, without downloading anything
mangadex-dl download <uuid> --dry-run --json
```

Running the same command again skips files that already hold every chapter and rebuilds a file
when MangaDex has added chapters to it, downloading only the new ones, so an interrupted run can
be restarted and a series kept up to date. See [Running again](#running-again).

## Running again

Before it writes a file, `mangadex-dl` looks at the file of that name already in `DIR`, if any:

| Existing file | What happens | JSON `status` / `existing` |
|---|---|---|
| none | written | `written` / `null` |
| made by `mangadex-dl`, holds every chapter this run would put in it (and maybe more) | skipped ("up to date") | `exists` / `"complete"` |
| made by `mangadex-dl`, only lacks some chapters | rebuilt and replaced: the chapters it holds are copied from it, only the missing ones are downloaded | `written`, `replaced: true` / `"incomplete"` |
| made by `mangadex-dl`, lacks chapters **and** holds chapters MangaDex no longer lists | left alone, with a warning | `exists` / `"diverged"` |
| anything else (see [Files left alone](#files-left-alone)) | left alone, with a warning | `exists` / `"foreign"` |

With **`--overwrite`** none of this applies: every existing file is downloaded again in full
and replaced, without being looked at (`existing: "unchecked"`). A symlink is replaced, never
followed. The one exception is a **directory** with the file's name: a file never replaces a
directory, so that file fails before anything is downloaded for it
(`mangadex-dl: error: <path>: cannot replace a directory; rename or remove it, or use another DIR`;
`failed` / `"foreign"`), the directory is left as it was, the other files are still written,
and the run ends with exit 6. `--dry-run` reports it the same way (`... would fail`).

The run still exits 0 when files are skipped or left alone. Files that are up to date are
counted in one closing note on stderr (not shown with `-q`, never colored):

```
mangadex-dl: 3 files are already up to date; pass --overwrite to download them again
```

With exactly one file the note reads
`mangadex-dl: 1 file is already up to date; pass --overwrite to download it again`.

Like every stderr line, the `-v` lines quoted below start with `mangadex-dl: `.

### How chapters are matched

- A chapter counts as held when the file has the same MangaDex chapter (the same id), or else a
  chapter with the same volume and chapter number (`7`, `07` and `7.0` are the same number). A
  chapter without a number is matched by its id only. A chapter that had no volume and was
  later given one still counts as held.
- **The release in the file is kept.** When the file holds a chapter in another release than
  the one this run picks (for example, because another group's release now comes first), the
  release in the file is kept and nothing held is downloaded again. `epubs[].chapters` in
  `--json` lists the release the file holds, and `skipped_chapters` names it as the `kept`
  release. `-v` prints
  `<path>: Vol. 1 Ch. 1: kept the release already in the file (<held id>) instead of <plan id>`.
- **`--group` re-picks a chapter** only when you pass `--group`, and only when the release this
  run picks ranks **strictly better** under the `--group` list than the release in the file. A
  release ranks by the first `--group` entry that matches one of its groups; a release that
  matches none ranks last. Equal ranks, including two releases that match no `--group`, keep the
  release in the file, so a file never flips between two equally ranked releases. A re-picked
  chapter counts as missing: the file is rebuilt, the other chapters are reused and the new
  release is downloaded. `-v` prints
  `<path>: Vol. 1 Ch. 1: replacing the release in the file (<group>) with the release by <group> (--group)`.
- **Image quality is not compared.** A rebuilt file can mix original (`data`) and `--data-saver`
  chapters; each chapter's quality is recorded in the file. Switching `--data-saver` on or off
  does not rebuild anything by itself; use `--overwrite` for that.
- Only the chapter list is compared. The pages of a file that is up to date are not checked, and
  a chapter whose pages changed on MangaDex under the same id or number is not downloaded again.
- Files are found by name. A different `--title` gives different names, so the files are
  downloaded afresh and the old ones stay in `DIR`; `mangadex-dl` does not clean up the old
  ones.

### Rebuilding a file

- A rebuilt file is byte for byte the file a fresh download of the same chapter releases and
  image quality into an empty `DIR` would give, whatever `--jobs` is. It replaces the old file
  atomically (see [Interruption and file safety](#interruption-and-file-safety)).
- Before the rebuild, every page to be reused is read and checked (its CRC-32 and its image
  header). A chapter with a damaged page is downloaded again instead, with
  `mangadex-dl: warning: <path>: Ch. 3 in the existing file is damaged; downloading it again`.
- The cover is copied from the old file when MangaDex still lists the same cover image;
  otherwise it is downloaded (a failure to do so is only a warning, as for new files).
- If the update fails (a new chapter cannot be downloaded, or the old file changes or cannot be
  read while it is being copied), the file counts as failed (exit 6), the old file is kept
  unchanged, and the error ends with ` (the existing file was kept)`, for example
  `mangadex-dl: error: <path>: the existing file changed or could not be read during the update (the existing file was kept)`.
  Ctrl-C and `SIGTERM` during an update also leave the old file unchanged.
- With `-v`, each file gets one line: `<path>: up to date (N chapters)` (`(1 chapter)` for one), or
  `updating <path>: downloading 1 chapter (Ch. 4), reusing 2`. The labels name the chapters to
  download: at most five, then `and K more`.
- With `--dry-run`, existing files are looked at the same way, but only the zip directory and
  the manifest are read, never the pages, so the real run may still find a damaged chapter. A file
  that would be updated is listed on stdout, with `-v` as
  `would update <path> (download 1 chapter: Ch. 4; reuse 2)`, and in `--json` as `planned` with
  `replaced: true`, where `reused_chapters` lists the chapters that matched.

### Files left alone

A file that is left alone is never modified. Its warning names the reason and what to do:

```
mangadex-dl: warning: <path>: left alone: <reason>; <remedy>
```

| Reason | Remedy |
|---|---|
| `not a regular file` (a directory, a symbolic link, a FIFO, …; never followed) | `rename or remove it, or use another DIR` |
| `cannot be read (<system error>)` | `rename or remove it, or use another DIR` |
| `not an EPUB made by mangadex-dl` (including a file without the [manifest](#the-manifest)) | `use another DIR or --title` |
| `holds a different book` (for example the same title in another `--lang`) | `use another DIR or --title` |
| `changed by another program` (its `OEBPS/content.opf` was edited) | `pass --overwrite to replace it` |
| `made by a newer mangadex-dl` | `pass --overwrite to replace it` |
| `changed while it was being updated` | `pass --overwrite to replace it` |

Nothing read from the file itself is ever printed. A file that holds chapters MangaDex no longer
lists, while also lacking some, is left alone too, because merging would keep the same content
twice when MangaDex re-uploads or renumbers a chapter:

```
mangadex-dl: warning: <path>: MangaDex lists N chapters this file lacks (Ch. 4, …), but the file holds M chapters MangaDex no longer lists (Ch. 3, …); left it alone (pass --overwrite to rebuild it without them)
```

Each list names at most five chapters, then `and K more`. With one chapter on a side the warning
says `1 chapter`, and with one chapter MangaDex no longer lists it ends `without it)`.

### The manifest

Every EPUB that `mangadex-dl` writes ends with one extra zip entry, `META-INF/mangadex-dl.json`
(stored uncompressed). It is a compact JSON record of what the file holds:

| Key | Contents |
|---|---|
| `format` | `1` |
| `generator` | `"mangadex-dl"` |
| `identifier` | the book's `dc:identifier`, `urn:mangadex:<title id>:<lang>:<unit>` |
| `opf_crc32` | the CRC-32 of `OEBPS/content.opf`, as 8 lowercase hex digits |
| `cover` | `null`, or `{ "file_name", "entry" }`: the MangaDex cover file name and its image in the file |
| `chapters` | in reading order: `{ "id", "volume", "chapter", "title", "groups": [ { "id", "name" } ], "publish_at", "updated_at", "quality", "images" }`, where `quality` is `"data"` or `"data-saver"` and `images` names the chapter's page images in order |

It holds no clock time and no version string. A re-run reads it to decide whether the file is up
to date; nothing else is kept between runs. Editing the book with another program (for example
changing its metadata in an EPUB editor) changes `content.opf` or drops the record, so the file
is then left alone. A changed `content.opf` gives `changed by another program` with the remedy
`pass --overwrite to replace it`; a dropped record gives `not an EPUB made by mangadex-dl` with
the remedy `use another DIR or --title`. EPUB 3.3 reserves `META-INF` for
configuration files without forbidding additional ones, and the record is not listed in the
book's package manifest, as EPUB requires for `META-INF` files.

## Many titles: `batch`

```
mangadex-dl batch FILE [DIR] [OPTIONS]
```

`batch` runs `download` once for each title listed in `FILE`, one title after another in file
order, into `DIR` (default `.`). `FILE` is a CSV file, or `-` to read the list from stdin.

| Argument / option | Meaning |
|---|---|
| `FILE` | The CSV list (see below), or `-` for stdin. Required. |
| `DIR` | Output directory, as in `download`. A row's `dir` column puts that title in a subdirectory of it. |
| `--volumes`, `--loose-chapters`, `--layout`, `-l, --lang`, `--group`, `--direction` | As in `download`: the value for every row whose CSV cell is empty. |
| `--data-saver`, `--overwrite`, `-n, --dry-run`, `-j, --jobs`, `--no-report` | As in `download`, for every row. These have no CSV column, so a list from someone else can never replace your files or change the load. |
| `--fail-fast` | Stop at the first title that fails. |

There is no `--title` option: one title for every row would make the file names collide. Use the
`title` column instead (`batch FILE --title X` exits 2 with
`batch has no --title option; give each row a 'title' column instead`). The global options (`--json`, `-q`, `-v`, `--color`) work before or after
`batch`, also after `FILE` and `DIR`. `mangadex-dl batch --help` describes the format with
examples.

`FILE` is never a MangaDex list: a list's address there is a usage error (exit 2) with
`FILE looks like a MangaDex list; use 'mangadex-dl mdlist <list>'`. To download a list, see
[A MangaDex list: `mdlist`](#a-mangadex-list-mdlist).

### The CSV file

- UTF-8 (a leading byte-order mark is skipped), comma-separated, LF or CRLF line endings.
  Fields that contain a comma, a quote or a line break go in `"double quotes"`, with `""` for a
  literal quote. Spaces and tabs around a field are trimmed.
- Blank lines and lines starting with `#` are ignored.
- **Without a header row**, each line is one title: a UUID or a title URL, as `MANGA` in
  `download`.
- **With a header row** (any first line that is not a title), these columns are recognised, in
  any order and case-insensitively:

  | Column | Meaning |
  |---|---|
  | `manga` | The title, as `MANGA` in `download`. Required. |
  | `title` | `--title` for this row. |
  | `dir` | A subdirectory of `DIR` for this row, e.g. `series/one`: relative, no `.` or `..` parts, no `\`. |
  | `volumes`, `loose-chapters`, `layout`, `lang`, `direction` | As the options. |
  | `group` | Preferred groups in priority order, separated by `\|`: `Group A\|Group B`. Replaces `--group` for this row. |
  | `note` | Ignored: free text for yourself. |

  Unknown column names are an error. A first line holding one word that is neither a title nor
  a column name (a chapter URL instead of a title URL, say) is reported as both:
  `titles.csv:1: '<word>' is not a MangaDex title UUID or https://mangadex.org/title/<uuid> URL,
  nor a known column (manga, title, ...)`. Rows may be shorter than the header (missing cells are
  empty).
- **Precedence, per field:** a non-empty CSV cell, then the command-line option, then the
  built-in default. An empty cell means "use the command-line value". Command-line values are
  checked even when every row overrides them.
- A row whose effective options are the same as an earlier row's is skipped with a warning
  (`titles.csv:7: same as line 3; skipped`).
- **Limits:** the file may be at most 1 MiB and list at most 10 000 titles; a field may be at
  most 4 KiB (a `title` 1024 bytes).

The whole list is read and checked before anything is downloaded. Any problem in it is a usage
error (exit 2) that names the line, for example `titles.csv:4: layout: ...`; up to 20 problems are
listed, nothing is downloaded and no directory is created.

```csv
# my list
manga,title,dir,lang,note
https://mangadex.org/title/<uuid>,,,,
<uuid>,My Title,series/my-title,fr,read later
```

### Examples

```sh
# Everything in the list, into ~/Manga
mangadex-dl batch titles.csv ~/Manga

# Show what would be written, without downloading anything
mangadex-dl batch titles.csv ~/Manga --dry-run

# A list from another command
grep -v '^#done' titles.csv | mangadex-dl batch - ~/Manga

# One JSON document for the whole batch, stopping at the first title that fails
mangadex-dl batch titles.csv ~/Manga --json --fail-fast
```

### What a batch prints and when it stops

- **stdout** is exactly what `download` would print, title after title: one path per file.
  With `--json`, one document at the end instead (below).
- **stderr:** every warning and error about a row starts with the list's name and the line,
  e.g. `mangadex-dl: error: titles.csv:4: title <uuid> not found on MangaDex (HTTP 404); ...`. With `-v`, each title
  begins with `titles.csv:4: <uuid>`. When stderr is a terminal, the progress line gains the
  title's position: `(2/15) [1/2] Vol. 1: 3/5 pages`. The "N files are already up to date" note
  is printed once, at the end. Two rows that would write the same file get a warning naming both
  lines.
- A full success prints nothing on stderr. Otherwise the batch ends with one closing line, even
  with `-q` or `--json`, for example
  `mangadex-dl: error: 2 of 15 titles did not complete (lines 4, 9); run the same command again to retry them`
  when a retry can help (a title failed with a network error or is incomplete), or
  `...; see the errors above` when it cannot (not found, nothing to download, a local error), or
  `stopped at line 9: ...; 6 titles not run` when it stopped early.
- **A failed title doesn't stop the batch:** the next one starts. It stops early only
  - with `--fail-fast`, at the first title that fails;
  - after 3 titles in a row failed with network errors (exit 3);
  - on a local problem that affects all of `DIR`: `DIR` itself unusable, disk full, a read-only
    file system, an I/O error (exit 1; `stopped at line 9: the error above affects every title`),
    or a failed write to stdout (exit 1).

  A row whose own `dir` can't be used (it runs into a regular file, permission denied below
  `DIR`) fails alone, and the batch continues with the next row. A title that ends with exit 4 or
  5 leaves no directory behind.
- **Restarting:** running the same command again treats each title as a `download` re-run does
  (see [Running again](#running-again)): files that already hold every chapter are skipped and
  files MangaDex has added chapters to are rebuilt, so an interrupted or incomplete batch can be
  restarted and a list of series kept up to date.
- **Ctrl-C / `SIGTERM`** stop the batch as they stop `download`: the current file is cleaned up,
  no further title starts, and nothing more is printed (no closing line, no JSON).
- An empty list (only comments or blank lines) prints the warning `titles.csv: lists no titles`
  and exits 0.
- **stdin:** with `-`, the list is read up to the end of input. When both stdin and stderr are a
  terminal, `batch` first prints `reading titles from the terminal; end with Ctrl-D`. `batch`
  never prompts.

**`--json`** prints one document when the titles ran and were not interrupted (also on exits 1
and 3 to 7, never on exit 2):

```
{ "format_version": 1, "dry_run", "source",
  "titles": [ { "line", "manga_id", "dir",
                "status": "ok" | "not_found" | "no_chapters" | "incomplete" | "network"
                          | "failed" | "duplicate" | "not_run",
                "exit_code", "error", "duplicate_of",
                "report" } ],
  "summary": { "titles": { "total", "ok", "not_found", "no_chapters", "incomplete", "network",
                           "failed", "duplicate", "not_run" },
               "files":  { "planned", "written", "exists", "failed" } } }
```

`exit_code` is what `download` would have returned for that title (`null` for `duplicate` and
`not_run`), and `report` is that title's `download --json` document (`null` when there is none).

**Exit status of a batch:** 0 when every title succeeded (or was a skipped duplicate), and for an
empty list. When titles failed and they all failed the same way, the batch exits with that code
(3, 4, 5 or 6, or 1 for rows whose own `dir` failed); when they failed in more than one way, 7,
and the per-title results (on stderr, or in `--json`) say which. A one-row list exits exactly as
`download` would.

### Things to know before running a long list

- **Symlinks inside `DIR` are followed.** A row's `dir` can't leave `DIR` by itself (no `..`, no
  absolute paths), but a symlink that already exists inside `DIR` is followed. Don't point `DIR`
  at a directory that others can write to.
- **A long list is a long, request-heavy run.** Each title costs several API requests (the title,
  its chapter list, covers, one image-server lookup per chapter), and the rate limits below apply
  to each title in turn. Running a list of thousands of titles is your responsibility towards
  MangaDex: split long lists, and stop if you see rate-limit errors (`HTTP 429` or `403`).

## A MangaDex list: `mdlist`

```
mangadex-dl mdlist LIST [DIR] [OPTIONS]
```

`mdlist` downloads every title in a public MangaDex custom list (an "MDList", as the website
calls it), one title after another in the list's order, into `DIR` (default `.`). Each title is
run as `batch` runs a row, with the same options for every title, and the output, the stop rules,
the exit status and the `--json` document follow `batch`'s (the differences are below).

| Argument / option | Meaning |
|---|---|
| `LIST` | The list's UUID, or its address on the website, `https://mangadex.org/list/<uuid>/<name>` (`http://`, `www.`, no scheme and a trailing `/` are accepted). A list whose name has no Latin letters has an address that ends in `/list/<uuid>/`; copy it as it is. Required. |
| `DIR` | Output directory, as in `download`. |
| `--volumes`, `--loose-chapters`, `--layout`, `-l, --lang`, `--group`, `--direction`, `--data-saver`, `--overwrite`, `-n, --dry-run`, `-j, --jobs`, `--no-report` | As in `download`, for every title. A list names only titles, so there are no per-title settings. |
| `--fail-fast` | Stop at the first title that fails. |
| `--dir-per-title` | Put each title in a subdirectory of `DIR` (see [Where the files go](#where-the-files-go)). |

The options are `batch`'s, with no `--title` and no per-title settings: one title for every
item would make the file names collide (`mdlist LIST --title X` exits 2 with
`mdlist has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title ...'`).
The global options (`--json`, `-q`, `-v`, `--color`) work before or after `mdlist`, also after
`LIST` and `DIR`. `mangadex-dl mdlist --help` lists the options with examples.

`LIST` must be a list. A title's address there is a usage error (exit 2) that points to
`download`; anything else that is not a list (a CSV file, `-`) is one that points to `batch`. The
other way round, `download <list address>` and `batch <list address>` exit 2 with a pointer to
`mdlist`.

### Which titles are downloaded

- **No login:** the list must be public. To download the titles you follow, add them to a public
  MDList; reading follows directly needs a login, which `mangadex-dl` doesn't support yet. A
  private list counts as not found: MangaDex answers it with HTTP 404 (exit 4).
- **Order:** the website's order. A title listed twice is downloaded once; the later entry is
  skipped with a warning (`list:5: <uuid>: same as item 2; skipped`).
- **At most 10 000 titles.** A longer list is refused (exit 3), never cut short.
- **Content filters:** the website hides titles outside your content filters ("N hidden");
  `mdlist` downloads every title in the list, so it may download more than the site shows.
- **Caching:** MangaDex caches lists: a title you just added may take a while to appear.

### Where the files go

- **By default, flat:** every title's files go straight into `DIR`, named as in `download`
  (see [File names](#file-names)). Two different titles with the same name would write the same
  file: the earlier item keeps it, and the later one gets one warning per file (instead of the
  usual `left alone: holds a different book` warning):
  `list:4: <uuid>: <path> was already written by item 2 (another title with the same name); pass --dir-per-title to give each title its own directory, or download one of them on its own with 'mangadex-dl download <title> --title ...'`.
  Nothing is overwritten, even with `--overwrite`: in one run, a later item never replaces a
  file an earlier item wrote (`--overwrite` still replaces files left by earlier runs). The fix
  is `--dir-per-title`, which gives the two titles different directories (below).
- **`--dir-per-title`** puts each title in its own subdirectory, named exactly like its files and
  made safe the same way: `DIR/<title>/<title> - Vol. 01.epub`. The directory is created only
  when the title has a file to write, and never with `--dry-run`.
  - The name is the plain title, with no id. Only when two different titles in one run have the
    same name does the later one get ` [<first 8 hex digits of its UUID>]`, with a warning (not
    shown with `-q`):
    `list:4: <uuid>: directory '<name>' already holds item 2 (a title with the same name); using '<name> [1a2b3c4d]'`.
    The first title in list order keeps the plain name. This is decided afresh on every run and
    not recorded, so a same-name title added earlier in the list takes the plain name over.
  - A title whose own directory can't be used (a regular file with its name, say) fails alone,
    with exit code 1 for that title, and the next title runs:
    `list:1: <uuid>: cannot open directory '<DIR>/<name>': Not a directory; rename or remove it, or run without --dir-per-title`.
- **New paths:** a renamed title, another `--lang` (a title without an English title is named
  after its `--lang` title, when it has one), or switching between flat and `--dir-per-title`
  gives new paths. The files are downloaded again there, and the old ones are left alone; delete
  them yourself.
- **Symlinks inside `DIR` are followed**, as in `batch`; don't point `DIR` at a directory that
  others can write to. A long list is a long, request-heavy run (see
  [Things to know before running a long list](#things-to-know-before-running-a-long-list)).

### What `mdlist` prints

As `batch` prints (see [What a batch prints and when it stops](#what-a-batch-prints-and-when-it-stops)),
with these differences:

- **stdout:** one path per file, in list order; with `--dir-per-title` the paths include the
  title's directory.
- **stderr:** every warning and error about a title starts with `list:<n>: <uuid>: `, its
  position in the list (from 1) and its UUID, for example
  `mangadex-dl: error: list:2: <uuid>: title <uuid> not found on MangaDex (HTTP 404); check the UUID or URL`.
  Where `batch` says "line", `mdlist` says "item":
  `mangadex-dl: error: 1 of 2 titles did not complete (item 2); see the errors above`,
  `...; run the same command again to retry them`, `2 of 15 titles would fail (items 4, 9)` with
  `--dry-run`, or `stopped at item 9: ...; 6 titles not run`.
- **`-v`** first prints the list, `mangadex-dl: list <list-uuid> "<name>": 2 titles` (`1 title`
  for one; `list <list-uuid>: 2 titles` for a list without a name; a long name is cut after 64
  bytes and ends `...`, and a `"` or `\` in it is shown as `\"` or `\\`), then begins each
  title with `list:1: <uuid>`; with `--dir-per-title` it also prints
  `output directory: <DIR>/<name>`. `-vv` adds the request trace, including the list request.
- **An empty list** prints the warning `mangadex-dl: warning: list <list-uuid> has no titles`
  (not shown with `-q`), creates nothing and exits 0.
- **Ctrl-C / `SIGTERM`** during the list request: nothing is printed and nothing is created.
  Later, they stop the run as they stop a batch.

**`--json`** prints `batch`'s document, under the same rules, with one more key, `list`, after
`source`:

```
{ "format_version": 1, "dry_run", "source",
  "list": { "id", "name", "url" },
  "titles": [ { "line", "manga_id", "dir", "status", "exit_code", "error", "duplicate_of",
                "report" } ],
  "summary": { "titles": { ... }, "files": { ... } } }
```

- `source` is `LIST` as typed. `list.id` is the list's UUID in lower case, `list.name` its name as
  MangaDex returns it (or `null`), and `list.url` is `https://mangadex.org/list/<uuid>`.
- `line` is the title's place in its source: the CSV line for `batch`, the list position (from 1)
  for `mdlist`. `duplicate_of` is a position too.
- `dir` is the title's directory, as typed: `DIR`, or with `--dir-per-title` `DIR/<name>`. It is
  `DIR` when the title's name was never learned (a duplicate, a title not run, or one that ended
  with exit 3 or 4 before it was planned).
- An empty list prints `"titles": []`, with the `list` object filled in.

A program written for `batch`'s document reads an `mdlist` document unchanged (`format_version`
is still 1).

### Exit status of a list

The titles' results combine exactly as in a batch (see
[Exit status of a batch](#what-a-batch-prints-and-when-it-stops)). Before any title runs, the
list itself can fail:

- **4:** the list does not exist, or is private:
  `mangadex-dl: error: list <list-uuid> not found on MangaDex (HTTP 404): it does not exist or is private; check the id, make the list public on MangaDex, or use 'mangadex-dl download' if it is a title`.
  MangaDex answers a private list with HTTP 404 (the code also accepts a 401/403 that names the
  list, with the same message).
  A title's bare UUID given as `LIST` ends this way too.
- **3:** the list request failed (connection, DNS, TLS, timeout, server errors or rate limiting
  that did not clear, a response over 16 MiB); the error ends with the network remedy, as for a
  title. Or the list can't be used, and retrying won't help, so there is no remedy:
  `mangadex-dl: error: list <list-uuid>: malformed MangaDex API response (custom list): ...`, or
  `mangadex-dl: error: list <list-uuid> has more than 10000 titles; refusing to truncate it; split it into smaller lists`.

**Telling them apart:** a failure of the list prints no JSON (stdout stays empty, even with
`--json`), creates nothing, and its error has no `list:<n>:` prefix. An exit 3 or 4 because every
title failed that way prints the JSON document, and each title's error starts with
`list:<n>: <uuid>:`.

### Running a list again

Running the same command again treats each title as a `download` re-run does (see
[Running again](#running-again)), and also downloads the titles added to the list since the last
run (see **Caching** above). Titles removed from the list are not touched: their files stay.
Nothing is kept between runs.

### Examples of `mdlist`

```sh
# Every title in a list, into ~/Manga
mangadex-dl mdlist https://mangadex.org/list/<uuid>/my-list ~/Manga

# The same, one folder per title
mangadex-dl mdlist <uuid> ~/Manga --dir-per-title

# Show what would be written, without downloading anything
mangadex-dl mdlist <uuid> ~/Manga --dry-run

# Volumes 1 to 3 of each title, in French, as one JSON document
mangadex-dl mdlist <uuid> ~/Manga --volumes 1-3 --lang fr --json
```

## Output

- **stdout is data:** the path of each file as it is written or updated, one per line. With
  `--dry-run`, the files that would be written or updated. Files that are up to date or left
  alone are left out.
- **stderr is diagnostics:** errors and warnings prefixed `mangadex-dl:`, the closing
  "N files are already up to date; pass --overwrite to download them again" note (see
  [Running again](#running-again)), and, when stderr is a terminal, a single progress line such
  as `[3/12] Vol. 03: 45/180 pages`. There is no progress line with `-q`, with `--json`, or when
  `TERM=dumb`. `-q` hides everything but errors.
- **`--json`** replaces the path list with one JSON document printed at the end (never colored):

  ```
  { "format_version": 1, "dry_run", "layout",
    "manga":   { "id", "title", "url", "language", "direction" },
    "epubs":   [ { "path", "kind", "volume", "chapter",
                   "status": "planned" | "written" | "exists" | "failed",
                   "replaced",
                   "existing": null | "complete" | "incomplete" | "diverged"
                             | "foreign" | "unchecked",
                   "new_chapters", "reused_chapters",
                   "pages", "bytes", "error",
                   "chapters": [ { "id", "volume", "chapter", "title",
                                   "groups": [ { "id", "name" } ], "pages" } ] } ],
    "skipped_chapters": [ { "id", "volume", "chapter",
                            "reason": "external" | "no_pages" | "unavailable" | "duplicate",
                            "kept" } ],
    "summary": { "planned", "written", "exists", "failed" } }
  ```

  - `status: "exists"` means the file was **left alone**: up to date (`existing: "complete"`),
    or skipped with a warning (`"diverged"`, `"foreign"`). `summary.exists` counts them all.
  - `status: "failed"` with `existing: "foreign"` is a `--overwrite` run that met a directory
    with the file's name (it is never replaced).
  - `replaced: true` means an existing file is (or, with `--dry-run`, would be) replaced: with
    `--overwrite` (`existing: "unchecked"`), or, without it, by an update
    (`existing: "incomplete"`).
  - `existing` is `null` when there was no file of that name. `new_chapters` and
    `reused_chapters` list chapter ids, and are non-empty only for `"incomplete"`: the chapters
    to download and the chapters copied from the old file. In a real run `reused_chapters`
    leaves out chapters that were downloaded again because they were damaged.
  - `chapters` lists what the file holds, with the releases kept from an existing file in place
    of the ones this run picked, and `pages` counts the pages it holds. For a file left alone
    that is the old file's content (`complete`, `diverged`); for a `foreign` file it is the
    planned chapters, with `pages` 0.
  - `skipped_chapters` agrees with the files: when a file keeps its own release of a chapter,
    the release this run would have picked is listed as a `duplicate`, and `kept` always names a
    release a file holds.

  On exit 5 the document has an empty `epubs` list; on exit 6 it includes the failed entries.
  On exits 1 to 4 nothing is printed on stdout.
- **Exit 5** also explains itself on stderr, for example
  `mangadex-dl: error: no downloadable chapters matched --lang en with --volumes all --loose-chapters 10-`,
  followed by ` (N chapters skipped: external, no pages or unavailable)` (or `1 chapter skipped`)
  when chapters were skipped, and then by `; try another --lang or a wider --volumes/--loose-chapters`.
  With `--json` this message still goes to stderr, and the `epubs: []` document to stdout.
- **Exit 6** ends with one closing line after the per-file errors, for example
  `mangadex-dl: error: 2 of 12 files failed; run the same command again to retry them`.
- **Exit 3** errors end with `; check your network connection or proxy (https_proxy) and try again`,
  except an `mdlist` list that is malformed or too long, where retrying can't help (see
  [Exit status of a list](#exit-status-of-a-list)).
- **Usage errors** (exit 2) read `mangadex-dl: error: <what is wrong>`, followed by
  `Run 'mangadex-dl download --help' for more information.` (or `mangadex-dl batch --help`, or
  `mangadex-dl mdlist --help`, or `mangadex-dl --help` before a command is given).

`mangadex-dl` never prompts. Only `batch -` reads stdin.

## Exit status

| Code | Meaning |
|---|---|
| 0 | Success: every selected file was written or updated, was already up to date, was left alone with a warning, or (with `--dry-run`) is planned. |
| 1 | Local failure, which stops the run: `DIR` is unusable, permission denied, read-only file system, disk full, a failed write to stdout, or no usable libcurl. For `batch`, also a list file that can't be read. For `batch` and `mdlist`, also the code of a title whose own directory can't be used (that title fails alone). |
| 2 | Usage error: unknown option, bad value, malformed `SPEC`, both selections `none`, or a `DIR` that looks like a second manga id. For `batch`, also any problem in the CSV list. |
| 3 | Network or API problem before anything could be planned: connection, DNS, TLS, timeout, server errors or rate limiting that did not clear, or a malformed or oversized API response. For `mdlist`, also the same problems with the list response, or a list of more than 10 000 titles (see [Exit status of a list](#exit-status-of-a-list)). |
| 4 | The title, or for `mdlist` the list, does not exist on MangaDex, or the list is private. |
| 5 | The title exists, but nothing downloadable matched the language and selection. |
| 6 | Incomplete: at least one file failed, either a new file or the update of an existing one (which is then kept unchanged); the others may have been written. |
| 7 | `batch` and `mdlist` only: the titles that did not complete failed in more than one way (for example one not found and one with no matching chapters); the per-title results say which. See [Exit status of a batch](#what-a-batch-prints-and-when-it-stops). |
| 70 | Internal error (a bug; please report it). |

A run stopped by Ctrl-C or `SIGTERM` ends by that signal (the shell reports 130 or 143). When
stdout is a pipe that closes early (`mangadex-dl download … | head -1`), the process ends
quietly by `SIGPIPE` (141).

## Network behaviour

All production traffic is HTTPS: the MangaDex API (`api.mangadex.org`), cover images
(`uploads.mangadex.org`), the image servers the API hands out, and MangaDex@Home reports
(`api.mangadex.network`). Requests carry the User-Agent `mangadex-dl/<version>`. No login is
used; only public data is downloaded. `mdlist` makes one extra API request, for the list.

### TLS and CA certificates

TLS certificate and host-name verification are **always on** and cannot be turned off. The CA
bundle is chosen once per run:

1. libcurl's own compiled-in CA location, when libcurl reports one and it is readable. This is
   the normal case with a distribution libcurl.
2. Otherwise the first readable file of:
   `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`,
   `/etc/ssl/ca-bundle.pem`, `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`,
   `/etc/ssl/cert.pem`, `/etc/pki/tls/cacert.pem`.
3. Otherwise TLS typically fails with exit 3. When libcurl reports an unreadable CA file, the
   error carries the hint "no CA certificates found; install ca-certificates".

`SSL_CERT_FILE` and `CURL_CA_BUNDLE` are **not** honoured (libcurl itself does not read them;
only the `curl` command-line tool does).

### Timeouts and retries

- Connecting times out after 15 s.
- A transfer that receives no data for 30 s is aborted. libcurl measures this as an average over
  a few seconds, so after a burst of data a stall can be detected up to about 5 s late.
- Every request also has an overall deadline of 10 minutes.
- Name resolution (DNS) has no timeout of its own: it normally counts toward the 15 s connect
  timeout, but depending on how your libcurl was built a slow lookup may not be bounded
  separately.
- Timeouts, dropped connections and server errors (5xx) are retried up to 4 attempts in total,
  waiting about 1, 2 and 4 s (plus a little random jitter). Connection refused, DNS failures and
  TLS verification failures are not retried.

### Rate limiting

- MangaDex API requests are limited to at most 4 per second, shared by the whole of a
  `download` run; with `batch` and `mdlist`, the limits apply to each title in turn.
- Image-server lookups (`/at-home/server`, one per chapter plus any refreshes) are additionally
  limited to 40 per minute.
- Page images from image servers outside `mangadex.org` are not throttled by the API limit;
  their concurrency is bounded by `--jobs`. Images and covers served from `mangadex.org` hosts
  (including `uploads.mangadex.org`) do count toward the API limit.
- If the API answers `429 Too Many Requests`, `mangadex-dl` waits for the time the server asks
  for (60 s if it gives none) and retries **once**. If the wait would exceed 60 s, or the retry
  is rate limited too, it gives up. For the title or chapter list this ends the run with exit 3
  and the time the limit lifts; during downloads it fails the file being downloaded (exit 6).
- MangaDex may temporarily ban clients that keep hitting the rate limit; that shows up as
  `HTTP 403` ("access denied — possibly a temporary rate-limit ban"). Wait before retrying, and
  avoid running several copies at once.

### Covers

A failure to list the cover images or to download one (including rate limiting) is only a
warning: the affected file uses its first page as the cover.

### Image servers

Each chapter's image server address is fetched right before the chapter is downloaded. When a
page download fails, a fresh server is requested (up to 3 times per chapter) before that file is
marked failed.

### MangaDex@Home reports

MangaDex asks clients to report how its volunteer image servers (MangaDex@Home) perform. By
default, after every page-image request to a server outside `mangadex.org`, `mangadex-dl` sends
a small best-effort report to `https://api.mangadex.network/report`: the image URL, whether the
request succeeded, whether it was served from cache, the number of bytes, and the duration.
Reports are never retried and never affect the result of the run.

Reports are sent in the background and never slow the download down: each one is queued and
sent by a separate thread, and gives up after 3 seconds without a connection or without data.
If the report endpoint fails 3 times in a row, reports stop for the rest of the run (`-vv`
shows `MangaDex@Home reports disabled: endpoint not responding`). When many reports are waiting,
new ones are dropped, and reports still waiting when the download ends are discarded.

`--no-report` turns these reports off. Nothing else is sent anywhere.

## Environment and configuration

`mangadex-dl` has **no configuration file** and reads **no environment variables of its own**
for its behaviour; everything is set on the command line. The only exceptions:

- Color of stderr diagnostics: `NO_COLOR`, `CLICOLOR_FORCE` and `TERM` (`TERM=dumb` also turns
  the progress line off). Precedence: `--color` > `NO_COLOR` > `CLICOLOR_FORCE` > `TERM=dumb` >
  whether stderr is a terminal.
- Proxies, read by libcurl itself:
  - `https_proxy` / `HTTPS_PROXY`: the one that applies, since all traffic is HTTPS;
  - `all_proxy` / `ALL_PROXY`: used when no scheme-specific proxy is set;
  - `no_proxy` / `NO_PROXY`: hosts that bypass the proxy;
  - `http_proxy`: lowercase only. `HTTP_PROXY` is deliberately ignored by libcurl.
- The dynamic loader's search path (`LD_LIBRARY_PATH`, `/etc/ld.so.conf`) decides which libcurl
  is loaded; see [Security notes](#security-notes).

No cache, config or state directories are used, and `TMPDIR` is ignored. What a file holds is
recorded inside the file itself (see [The manifest](#the-manifest)).

## Interruption and file safety

- Only `DIR/<name>.epub` files (mode 0644 subject to your umask, or an updated file's previous
  permissions) and their temporary files are ever written (with `batch`, also
  `DIR/<dir>/<name>.epub` for a row with a `dir`; with `mdlist --dir-per-title`, also
  `DIR/<title>/<name>.epub`). Existing `.epub` files are read, never modified in place.
- Each file is written to a temporary file in `DIR`, flushed to disk, and only then given its
  final name. A finished `.epub` is never a partial one.
- An existing file is replaced only with `--overwrite` (never a directory, which fails that
  file instead), or when it is a file made by
  `mangadex-dl` that only lacks chapters (see [Running again](#running-again)). Any other
  existing file is left alone, including a file that appears while the download is running,
  which gets its own warning:
  `mangadex-dl: warning: <path>: left alone: another file took this name while ours was being written, so ours was discarded; run the same command again to check it`.
- An update replaces the old file only if it is still the file that was inspected (the same
  inode, size, modification time and change time). If it changed in the meantime, the new
  version is discarded and the file is left alone with `changed while it was being updated`.
  Changing only the file's permissions (`chmod`) or its number of hard links during an update
  also counts as a change. A window of microseconds remains between that last check and the
  replacement; a program that replaces the file within it loses its version (the last rename
  wins). This is accepted because `DIR` is yours.
- A replaced file is a new file, so its owner, group, ACLs, extended attributes and hard links are
  not kept. An updated file keeps the old file's permission bits (read/write/execute for owner,
  group and others, whatever your umask is; never set-user-ID, set-group-ID or sticky), so a file
  you `chmod 600`'d stays 600. If they cannot be set, the update fails and the old file is kept.
  A file replaced with `--overwrite` gets mode 0644 subject to your umask, like a new one.
- **Ctrl-C / `SIGINT` and `SIGTERM`:** the download stops, the temporary file is removed, files
  already finished are kept, a file that was being updated is left as it was, and no partial
  `.epub` is left behind. `DIR` (or a row's `dir`) may be left empty. Run the same command again
  to continue; finished files are skipped.
- **`SIGKILL`, and death by `SIGPIPE`:** on file systems that support `O_TMPFILE` (for example
  tmpfs and ext4), and where `/proc/self/fd` is usable, the temporary file is anonymous and
  nothing is left behind. Otherwise a hidden temporary file named
  `.<name>.part-<12 hex digits>` may remain in `DIR`. With `--overwrite`, and whenever an
  existing file is updated, even an anonymous temporary file is briefly linked under such a
  hidden `.part-` name before it replaces the final file, so a `SIGKILL` in that short window
  can leave one even on tmpfs or ext4.
  Either way it is safe to delete; `mangadex-dl` does not clean up stale ones.
- With `--dry-run` nothing is written and `DIR` is not created.

## Limits

- One title and one language per `download` run; `batch` runs a list of titles one after
  another (never in parallel).
- `mdlist` downloads one list per run, at most 10 000 titles.
- No resuming inside a file: an interrupted file is downloaded again from the start. (An
  interrupted update starts again from the old file, so its chapters are still reused.)
- API responses over 16 MiB, images over 32 MiB, chapters over 5000 pages and images wider or
  taller than 65535 pixels are rejected. Memory use is roughly `--jobs` × 32 MiB at worst.
- No ZIP64: an EPUB that would exceed 65533 entries or 4 GiB fails (split it with `--layout`
  or a narrower selection).
- Images are stored as served (JPEG, PNG, GIF or WebP; WebP gives a warning, as some older
  readers can't show it). Any other image type fails that chapter.

## Security notes

- **libcurl comes from the host.** Because libcurl is loaded by its bare name
  (`libcurl.so.4`), the dynamic loader's normal search rules apply, including `LD_LIBRARY_PATH`.
  Anyone who can set your environment can make `mangadex-dl` load a different library; this is
  the same trust every dynamically linked program places in its environment. `mangadex-dl` must
  never be installed setuid or setgid.
- **Host libraries are outside this project's audit.** The project's dependency audit covers
  only its own build environment. Vulnerabilities in your system's libcurl and TLS library
  (OpenSSL, GnuTLS) are fixed by your distribution's updates; keep them current.
- Server-provided text (titles, group names) is sanitised before it is used in a file name or
  printed on stderr, and image-server addresses and image names from the API are validated
  (HTTPS only, plain names) before use. Your `MANGA` and `--title` values are never echoed in
  error messages.
- Existing files in `DIR` are treated as untrusted input. They are opened without following
  symbolic links, read with bounded sizes, and checked (zip structure, manifest, CRC-32 of every
  reused page and its image header) before anything is copied from them. A file that fails a
  check is left alone with a warning or has the affected chapter downloaded again, and nothing
  read from it is printed.

### Test-only fixture mode

There is a hidden `--api-base` option used by the test suite (for example
`--api-base file:///abs/fixture/dir` reads canned API responses and images from disk). It is for
testing only, is not shown in `--help`, and is not a supported interface. Any non-default
`--api-base` also turns MangaDex@Home reports off.

## Development

The toolchain (Clang and GCC, CMake, Ninja, clang-format, clang-tidy) and every dependency come
from pixi. Common tasks:

```sh
pixi run test        # sanitized (ASan + UBSan) debug build plus all tests
pixi run lint        # clang-tidy
pixi run check       # the full gate: format check, tests, lint, release build
pixi run -e gcc check
```

Design notes live under [`docs/design/`](docs/design/).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
