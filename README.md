# Mangadex-dl

Downloads volumes and chapters from MangaDex and converts them into .epub or .pdf files.

`mangadex-dl download` takes a MangaDex title and writes fixed-layout EPUB 3 files: by default
one `.epub` per volume, plus one per chapter that has no volume ("loose" chapters).
`mangadex-dl batch` does the same for every title listed in a CSV file.

> **Status:** EPUB output only. PDF output is planned but not available yet.

## Contents

- [Requirements](#requirements)
- [Building](#building)
- [Usage](#usage)
- [Examples](#examples)
- [Many titles: `batch`](#many-titles-batch)
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
| `--overwrite` | off | Replace existing files instead of skipping them. |
| `-n, --dry-run` | off | Show what would be written; download and write nothing. |
| `-j, --jobs N` | `4` | Parallel image downloads, 1 to 8. |
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

Running the same command again skips the files that already exist (exit 0), so an interrupted
run can simply be restarted.

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
  title's position: `(2/15) [1/2] Vol. 1: 3/5 pages`. The "N files already exist" notice is
  printed once, at the end. Two rows that would write the same file get a warning naming both
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
- **Restarting:** running the same command again skips the files that already exist, so an
  interrupted or incomplete batch can simply be restarted.
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

## Output

- **stdout is data:** the path of each file as it is written, one per line. With `--dry-run`,
  the files that would be written. Files that already exist are left out.
- **stderr is diagnostics:** errors and warnings prefixed `mangadex-dl:`, the
  "N files already exist; pass --overwrite to replace them" notice, and, when stderr is a
  terminal, a single progress line such as `[3/12] Vol. 03: 45/180 pages`. There is no progress
  line with `-q`, with `--json`, or when `TERM=dumb`. `-q` hides everything but errors.
- **`--json`** replaces the path list with one JSON document printed at the end (never colored):

  ```
  { "format_version": 1, "dry_run", "layout",
    "manga":   { "id", "title", "url", "language", "direction" },
    "epubs":   [ { "path", "kind", "volume", "chapter",
                   "status": "planned" | "written" | "exists" | "failed",
                   "replaced", "pages", "bytes", "error",
                   "chapters": [ { "id", "volume", "chapter", "title",
                                   "groups": [ { "id", "name" } ], "pages" } ] } ],
    "skipped_chapters": [ { "id", "volume", "chapter",
                            "reason": "external" | "no_pages" | "unavailable" | "duplicate",
                            "kept" } ],
    "summary": { "planned", "written", "exists", "failed" } }
  ```

  On exit 5 the document has an empty `epubs` list; on exit 6 it includes the failed entries.
  On exits 1 to 4 nothing is printed on stdout.
- **Exit 5** also explains itself on stderr, for example
  `mangadex-dl: error: no downloadable chapters matched --lang en with --volumes all --loose-chapters 10-`,
  followed by ` (N chapters skipped: external, no pages or unavailable)` (or `1 chapter skipped`)
  when chapters were skipped, and then by `; try another --lang or a wider --volumes/--loose-chapters`.
  With `--json` this message still goes to stderr, and the `epubs: []` document to stdout.
- **Exit 6** ends with one closing line after the per-file errors, for example
  `mangadex-dl: error: 2 of 12 files failed; run the same command again to retry them`.
- **Exit 3** errors end with `; check your network connection or proxy (https_proxy) and try again`.
- **Usage errors** (exit 2) read `mangadex-dl: error: <what is wrong>`, followed by
  `Run 'mangadex-dl download --help' for more information.` (or `mangadex-dl batch --help`, or
  `mangadex-dl --help` before a command is given).

`mangadex-dl` never prompts. Only `batch -` reads stdin.

## Exit status

| Code | Meaning |
|---|---|
| 0 | Success: every selected file was written, already existed, or (with `--dry-run`) is planned. |
| 1 | Local failure, which stops the run: `DIR` is unusable, permission denied, read-only file system, disk full, a failed write to stdout, or no usable libcurl. For `batch`, also a list file that can't be read. |
| 2 | Usage error: unknown option, bad value, malformed `SPEC`, both selections `none`, or a `DIR` that looks like a second manga id. For `batch`, also any problem in the CSV list. |
| 3 | Network or API problem before anything could be planned: connection, DNS, TLS, timeout, server errors or rate limiting that did not clear, or a malformed or oversized API response. |
| 4 | The title does not exist on MangaDex. |
| 5 | The title exists, but nothing downloadable matched the language and selection. |
| 6 | Incomplete: at least one file failed; the others may have been written. |
| 7 | `batch` only: the titles that did not complete failed in more than one way (for example one not found and one with no matching chapters); the per-title results say which. See [Exit status of a batch](#what-a-batch-prints-and-when-it-stops). |
| 70 | Internal error (a bug; please report it). |

A run stopped by Ctrl-C or `SIGTERM` ends by that signal (the shell reports 130 or 143). When
stdout is a pipe that closes early (`mangadex-dl download … | head -1`), the process ends
quietly by `SIGPIPE` (141).

## Network behaviour

All production traffic is HTTPS: the MangaDex API (`api.mangadex.org`), cover images
(`uploads.mangadex.org`), the image servers the API hands out, and MangaDex@Home reports
(`api.mangadex.network`). Requests carry the User-Agent `mangadex-dl/<version>`. No login is
used; only public data is downloaded.

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

- MangaDex API requests are limited to at most 4 per second, shared by the whole run.
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

No cache, config or state directories are used, and `TMPDIR` is ignored.

## Interruption and file safety

- Only `DIR/<name>.epub` files (mode 0644) and their temporary files are ever written (with
  `batch`, also `DIR/<dir>/<name>.epub` for a row with a `dir`).
- Each file is written to a temporary file in `DIR`, flushed to disk, and only then given its
  final name. A finished `.epub` is never a partial one.
- Existing files are never overwritten unless you pass `--overwrite`, including a file that
  appears while the download is running.
- **Ctrl-C / `SIGINT` and `SIGTERM`:** the download stops, the temporary file is removed, files
  already finished are kept, and no partial `.epub` is left behind. `DIR` (or a row's `dir`) may
  be left empty. Run the same command again to continue; finished files are skipped.
- **`SIGKILL`, and death by `SIGPIPE`:** on file systems that support `O_TMPFILE` (for example
  tmpfs and ext4), and where `/proc/self/fd` is usable, the temporary file is anonymous and
  nothing is left behind. Otherwise a hidden temporary file named
  `.<name>.part-<12 hex digits>` may remain in `DIR`. With `--overwrite`, even an anonymous
  temporary file is briefly linked under such a hidden `.part-` name before it replaces the
  final file, so a `SIGKILL` in that short window can leave one even on tmpfs or ext4. Either
  way it is safe to delete; `mangadex-dl` does not clean up stale ones.
- With `--dry-run` nothing is written and `DIR` is not created.

## Limits

- One title and one language per `download` run; `batch` runs a list of titles one after
  another (never in parallel).
- No resuming inside a file: an interrupted file is downloaded again from the start.
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
