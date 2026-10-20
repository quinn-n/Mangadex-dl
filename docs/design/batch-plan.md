# Implementation plan: `mangadex-dl batch FILE [DIR]` (many titles from a CSV list)

Status: APPROVED 2026-10-18 at the ⏸ plan gate (rev1 incl. row-scoped dir failure → continue).

Design: `docs/design/batch-spec.md` (APPROVED 2026-10-18). Its gate decisions are binding: Q1 `batch` subcommand; Q2 a `download --help` footer line pointing to `batch`; Q3 a uniform code when every failure is alike, else 7; Q4 stop after 3 consecutive network-failed titles; Q5 one JSON document at the end; Q6 no `--check`; Q7 `group` separator `|`, ignored `note` column; Q8 empty list = exit 0 plus a warning.

## rev1 (fact-check), 2026-10-18

All four [VERIFY] clusters were **CONFIRMED** (none refuted). This revision adds the fact-checker's required steps and the security-reviewer's OD1 tightening. Changed sections are marked **rev1 (fact-check)**. The batches and the disjointness of the Owns sets are unchanged.

| # | Change | Tasks |
|---|---|---|
| 1 | `fs/bounded_read`: open with `O_NONBLOCK`; an explicit `fstat` file-type policy; `O_NONBLOCK` cleared for FIFOs/char devices; `st_size` only a reserve hint; the cap enforced on bytes read (≤ cap+1); new tests (directory, FIFO with and without a writer, `/proc/self/status`, file over the cap) | T1, T2 |
| 2 | New header-only `include/mangadex_dl/text_safety.hpp`: `terminal_safe`'s UTF-8 decoder and unsafe-code-point predicate, copied verbatim. Used by `validate_list_dir` and the CSV lexer; `terminal_safe` switches to it in T5, which now also owns `cmd_download_output.cpp` | T1, T2, T3a, T5 |
| 3 | `validate_list_dir` tightened: C1 and the bidi/invisible/separator set, `\`, NUL, trimmed input, ≤ 32 segments. New AC4 rejects: U+009B, U+0085, U+202E, U+2028 | T3a |
| 4 | **Behaviour change (for the plan gate):** a row-scoped `dir` failure marks that row `failed` (exit_code 1) and the batch continues. Only global conditions stop it. Pieces: a new read-only pre-flight `check_row_dir` (`include/mangadex_dl/fs/row_dir.hpp`); `RowOutcome::fatal` and `BatchJob::sub_dir`; a new stop and aggregation rule (`failed` counts as 1: all failed → 1, failed + 4 → 7, a fatal stop → 1); runner wiring; AC7/AC22 updated | T1, T2, T3b, T5, T7 |
| 5 | `OutputRegistry` keyed on the lexically normalized joined path | T1, T5 |
| 6 | `lang`/`group` URL safety confirmed in-repo (groups never reach a URL); tests for `&`, `#` and a newline in `group`; a group name with a control character is rejected | T3a, T5 |
| 7 | Docs: "symlinks inside DIR are followed; don't point DIR at a directory others can write to", plus the API load of long lists, in `batch --help` (T5, part of the `batch.help` golden) and in the README (T6) | T5, T6 |
| 8 | Implementer notes: `cxx-probe` adds no `-I` for the env include dir (probes use an absolute include path or `CPATH`). T4 diffs the real installed `download --help` before and after. The cluster C doubles caveat does not apply (no floats in the report JSON), but T3b adds a guard test | T4, T3b |
| 9 | Spec `docs/design/batch-spec.md` amended in place, marked "rev1 (fact-check)" | — |

What I checked in the tree on 2026-10-18 (orientation only; the fact-checker re-confirmed the [VERIFY] items):
- `pixi list`: cli11 2.7.2, catch2 3.16.1, cmake 4.4.4, libstdcxx 16.2.0. nlohmann_json is the one `download` already uses. **No new dependency is needed.**
- `CMakeLists.txt` globs `src/core/**/*.cpp` (recursive) and `src/app/cmd_*.cpp` (flat, **only the `cmd_` prefix**). So every new app source in this plan is named `src/app/cmd_*.cpp`.
- `tests/unit/**/*.cpp`, `tests/cli/*.cpp` and `tests/e2e/cases/*.cmake` are globbed. No task edits `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake` or `tests/e2e/run_case.cmake`.

---

## Approach

The CSV list is turned into fully validated per-row `DownloadOptions` before anything runs: core reads it (capped) → parses it (pure) → merges each row with the CLI defaults, validates and dedupes it (pure). Then a core loop (`run_batch`) drives an injected `TitleRunner` row by row. It owns stopping, aggregation, the closing message and the JSON document. The app's runner reuses `download`'s own per-title path, which T4 extracts unchanged from `cmd_download.cpp`, against one shared `Backend`. A row-aware `ConsoleObserver` adds the `<file>:<line>: ` prefix and the `(i/n)` progress counter.

**The key trade-off:** a behaviour-preserving refactor of `download`'s run path (T4), with regression risk on a shipped command. The alternative was a second copy of `download`'s backend/report/printing code that could drift. Byte-stable goldens and the full `cli.download*` suite guard the refactor.

---

## Dependencies & pixi/CMake changes

| Change | Where | Task |
|---|---|---|
| None. No `pixi add`, no `find_package`, no FetchContent | — | — |
| No preset, pixi task, CMake option, CI or `scripts/*` change | — | — |
| New core sources picked up by the existing recursive glob: `src/core/batch_csv.cpp`, `src/core/fs/bounded_read.cpp`, `src/core/batch_plan.cpp`, `src/core/batch_run.cpp`, `src/core/batch_json.cpp` | glob | T2, T3a, T3b |
| New app sources picked up by the `cmd_*.cpp` glob: `cmd_download_run.cpp`, `cmd_download_options.cpp`, `cmd_batch.cpp` | glob | T4, T5 |
| `.gitattributes`: add `tests/e2e/inputs/batch/*.csv -text`. Not a CMake file, but shared: only T7 touches it | `.gitattributes` | T7 |

**Build files for build-fixer, all tasks:** none beyond each task's Owns. If a task seems to need a shared CMake edit, that is a coordination event for the orchestrator.

---

## Resolved in-repo (no fact-check needed)

The spec's V3–V7 were answered by reading the code. Each entry gives the evidence.
- **V3:** `exit_status_footer()` lives in `src/app/exit_help.hpp` (an app header, not under `include/`).
  - It is the single table used by the root help (`cli.cpp:134`) and by `download --help` (`cmd_download.cpp:219`).
  - Adding row 7 changes `edge.help` and `download.help`, and `batch.help` gets it too. T6 owns `exit_help.hpp`, so T1–T5 leave every existing golden byte-stable.
- **V4:** `.gitattributes` is `* text=auto`. Git normalizes CRLF to LF when it adds new text files. A BOM is not stripped. So `excel.csv` (BOM + CRLF) needs `-text`.
  - T7 adds the attribute.
  - T7's Done-when checks `git check-attr text -- tests/e2e/inputs/batch/excel.csv` reports `unset`.
  - `batch.cmake` also gets a configure-time byte guard (below).
- **V5:** `run_case.cmake:109-121`:
  - A relative `STDIN` path resolves under `tests/e2e/inputs/`, so `STDIN batch/clean.csv` works with subdirectories.
  - `{{INPUTS}}`, `{{WORK}}`, `{{FIXTURES}}` and `{{INVALID_UTF8}}` expand in ARGS, ENV and STDIN.
  - `PIPE_HEAD` passes `INPUT_FILE` too.
  - There is **no signal-sending keyword**, so interrupt E2E is impossible and stays `[human]` (AC32).
  - `TTY` rejects `STDIN` and runs the tool with the pty as stdin and script's own stdin on `/dev/null`. **Never write a TTY case with FILE `-`**: it would wait on the pty until the timeout. AC34 stays human.
- **V6:**
  - `run_download` (`src/core/download.cpp:63-97`) builds its `ApiClient` (which owns the MD@H reporter thread), `FetchPool` and `InterruptBridge` per call. Destruction order joins all three before it returns. So a fresh `DownloadServices` per row is clean, and no thread outlives a row.
  - `Backend` (transport + clock) is borrowed through raw pointers and may be shared across sequential rows.
  - `make_fixture_transport` takes its root by value (copies it).
  - `report_to_json` exposes only a string. T3b adds a core-private `src/core/report_json.hpp` with the `ordered_json` value form. `report_to_json` becomes `dump(report_json(r)) + '\n'`, which is byte-identical.
  - Note: the rate limiters live in each `ApiClient`, so they reset per row. At a row boundary at most one API request and one at-home request can come sooner than the 250 ms / 1500 ms interval. This is accepted and is AC31 `[human]`; see Rollout & risk.
- **V7:**
  - `ConsoleObserver` (`cmd_download_output.{hpp,cpp}`) can take an owned `RowContext` (prefix, position/total, whether `finish()` prints the exists notice).
  - With a default `RowContext` its output is byte-identical: `prefix` is empty, there is no `(i/n) ` counter and the notice is on.
  - The per-row exists count comes from `summarize(report).exists`, so the observer needs no new accessor.
- **Io has no stdin.** `src/app/io.hpp`'s `Io` holds only out/err and the TTY flags. T4 adds `int stdin_fd = -1;` as the **last** member, so existing designated initializers still compile. `process_io()` sets 0. Tests pass an fd they opened, and the default `-1` keeps every in-process test hermetic and gives the "closed stdin" path for free.
- **Root error texts:** these live in `cli.cpp` `failure_text` (`unknown command '…' (the only command is 'download')`, `missing command; try 'mangadex-dl download --help'`). Tests assert them at `tests/cli/cli_test.cpp:77` and `tests/cli/download_cli_test.cpp:578-600`. T6 changes all three together.
- **Fixture trees:** each tree serves one title. Any other UUID in a tree is a 404 (exit 4).
  - `nochapters` with rows 5555… and 6666… gives 5 and 4, so exit 7.
  - `clean` with rows 1111… and 6666… gives 0 and 4, so exit 4.
  - `incomplete` with 4444… gives 6.
  - `progress_label` prints `Vol. 1` (unpadded).
- **The filename sanitizer keeps `,`.** An Excel title `"Excel, Title"` gives `out/Excel, Title - Vol. 01.epub`, which is passable in `EXPECT_FILES` because it contains no `;`, `[` or `]`.
- **rev1 (fact-check): `lang` and `group` never inject into URLs.**
  - `validate_language` allows only `^[a-z]{2,3}(-[a-z]{2,4})?$`, and the feed URL still passes it through `encode_component` (`mangadex_api.cpp:389,538`).
  - `group` values never go into any URL. They only feed the planner's local preference matching (`plan_phase.cpp:105` → `preferred_groups`).
  - Tests pin both (T3a, T5).
- **rev1 (fact-check): the `download` JSON has no floating-point values.** `report.cpp`/`model.hpp` hold only strings, integers, booleans, arrays, objects and null, so the cluster C caveat about doubles in the byte-equal round trip does not apply. T3b still adds a guard test that the parsed document contains no `number_float` node.
- **rev1 (fact-check): one unsafe-code-point set.** `terminal_safe`'s `is_unsafe_code_point` (`cmd_download_output.cpp:31-39`: C1, U+061C, U+200B–U+200F, U+202A–U+202E, U+2028/9, U+2060–U+206F, U+FEFF) already covers every code point the security review lists: bidi U+202A–202E and U+2066–2069, LRM/RLM/ALM, U+2028/2029, U+FEFF, C1. T1 copies it verbatim into `text_safety.hpp`. The filename sanitizer's slightly different set is unchanged (follow-up, out of scope).

---

## Assumptions to verify — rev1 (fact-check): all CONFIRMED

**Cluster A — CLI11 (installed 2.7.2).** Probe note: `scripts/cxx-probe.sh` adds no `-I` for the env include dir (download-plan-rev1 T0 note). A CLI11 probe may need the include path passed explicitly, or run as a scratch tier-2 test.
- **V-A1** A lone `-` argument (`batch - out`) is assigned to the first positional (`FILE`, `->required()`, no type name). It is not rejected as an option and not dropped. `batch -- - out` binds `-` the same way.
- **V-A2** With root `fallthrough()` set before `add_subcommand`, root flags written **after both positionals** (`batch f.csv out --json -q -v --color never`) are parsed by the root, and FILE/DIR still bind. Today's tests put the globals before the subcommand.
- **V-A3** Two sibling subcommands each get the same option names from one shared bind helper: `--volumes`, `--loose-chapters`, `--layout`, `-l,--lang`, `--group` (`expected(1)` + `allow_extra_args(false)` + `TakeAll`), `--data-saver`, `--direction`, `--overwrite`, `-n,--dry-run`, `-j,--jobs` (`Range(1,8)`), `--no-report`, hidden `--api-base` (`group("")`).
  - This raises no `CLI::ConstructionError`.
  - Each subcommand's help lists its options in add order (positionals first), so `download --help` stays byte-identical when the binding moves into helpers.
- **V-A4** `batch f.csv --title X` (an option `batch` does not define, while the parent has fallthrough) ends in a `CLI::ParseError` that `run()` maps to exit 2. It must never succeed with `X` silently bound to DIR.

**Cluster B — POSIX / glibc (the bounded read)**
- **V-B1** When the parent closed fd 0 (`exec … <&-`, the E2E `STDIN CLOSED` mode), `read(0, …)` fails with `EBADF`.
  - glibc/ld.so do not reopen fds 0–2 for a non-setuid process.
  - Nothing that runs before batch's read opens a descriptor that could take slot 0: `main`, `process_io` (isatty), CLI11 parsing, `validate_batch_args`. No interrupt handlers or backend exist yet in phase 1.
- **V-B2** `open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY)` succeeds on a directory, and `fstat` then reports `S_ISDIR` (the plan rejects it before reading). `st_size` is trusted only for `S_ISREG`; procfs/sysfs regular files report 0 and are bounded by the cap+1 read anyway. `read` on a pipe returns short counts and needs a loop with EINTR retry.

**Cluster C — nlohmann_json (installed 3.12.0)**
- **V-C1** An `ordered_json` subtree moved into another `ordered_json` (`titles[i]["report"] = std::move(report_json(r))`) keeps its key order. `dump(2, ' ', false, ordered_json::error_handler_t::replace)` on the outer document applies `replace` to strings nested at any depth: invalid UTF-8 in `source`, `dir` or report titles becomes U+FFFD, with no throw. `ordered_json(nullptr)` dumps as `null`.
- **V-C2** The AC8 test technique: `ordered_json::parse(batch_doc)` preserves key order. Re-dumping `titles[i]["report"]` with `dump(2, ' ', false, replace)` + `'\n'` reproduces `report_to_json(report)` byte for byte, including for a report that held invalid UTF-8 (U+FFFD round-trips).

**Cluster D — security (security-reviewer judgment, design OD1)**
- **OD1** For the per-row `dir` column, lexical containment is enough: relative; valid UTF-8; no C0/DEL controls; no leading `/`; no empty, `.` or `..` segment; ≤ 255 B per segment; ≤ 1024 B total. After it, the existing path-based `OutputDir::open(DIR/<dir>, create=true)` runs, which creates missing components with 0755 and follows symlinks that already exist inside DIR. The alternative is an `openat(…, O_NOFOLLOW | O_DIRECTORY)` walk.

**Clusters:** Cluster A: V-A1, V-A2, V-A3, V-A4 · Cluster B: V-B1, V-B2 · Cluster C: V-C1, V-C2 · Cluster D: OD1.

**rev1 (fact-check) results:**
- **Cluster A** is confirmed by probe. Keep `--title` between `bind_selection_options` and `bind_run_options` (done), and T4 diffs the real `download --help` before and after. Probe note: `cxx-probe` adds no `-I` for the env include dir, so use an absolute include path or `CPATH`.
- **Cluster B** is confirmed, with required steps (open `O_NONBLOCK`, the `fstat` type policy, clearing `O_NONBLOCK` for FIFOs and terminals, `st_size` as a hint only, the cap on bytes read, and the fd-0 guard). They are folded into T1/T2/T5.
- **Cluster C** is confirmed; the doubles caveat is resolved in-repo.
- **Cluster D (OD1)** is confirmed with the required tightening (T3a) and the adopted should-fixes (T2, T3a, T5, T6). The row-scoped-continue behaviour change goes to the plan gate.

---

## Task breakdown

Conventions for every task:
- Namespace `mangadex_dl` (core) and `mangadex_dl::app` (app).
- Aggregates with default member initializers everywhere, built with designated initializers. One exception: fill `DownloadServices` member by member, because clang-analyzer 23 reports a NewDeleteLeaks false positive with designated init (T15 deviation).
- `enum class` switches list every enumerator, have no `default:`, and end with a fallback return.
- No `<CLI/CLI.hpp>` outside `src/app/`. No nlohmann in public headers.
- Core never prints and never sanitizes. Every stderr line goes through `print_error` or the observer, which apply `terminal_safe` to the whole message, prefix included.
- rev1 (fact-check) probe note: `scripts/cxx-probe.sh` adds no `-I` for the env include dir. A probe that includes CLI11 or nlohmann needs an absolute include path (`$CONDA_PREFIX/include`) or `CPATH`.
- Every "Done when" is `nice pixi run check` green (long `timeout`) **plus** `quality-reviewer` and `memory-safety-reviewer` both `VERDICT: APPROVED`.
- All unit tests link into one executable. A track's tests may call only symbols defined by itself or by an already-integrated batch.
- Public headers from T1 are **frozen**. Changing one is a coordination event.

### T1 — Frozen interfaces (public headers, exit code 7)
- **Goal:** every cross-module declaration lands once, so batch 2 can run three core tracks in parallel. No behaviour change, no golden change.
- **Files:**
  - edit `include/mangadex_dl/exit_codes.hpp`, `include/mangadex_dl/error.hpp`, `include/mangadex_dl/options.hpp`;
  - create `include/mangadex_dl/batch_csv.hpp`, `include/mangadex_dl/batch.hpp`, `include/mangadex_dl/fs/bounded_read.hpp`;
  - rev1 (fact-check): create `include/mangadex_dl/fs/row_dir.hpp`, `include/mangadex_dl/text_safety.hpp` and `tests/unit/text_safety/text_safety_test.cpp`;
  - edit `tests/unit/interfaces/headers_test.cpp`.
- **Owns:** exactly the ten files above.
- **Build files for build-fixer:** none.
- **What changes:**
  - `exit_codes.hpp`: `inline constexpr int mixed_failures = 7;`, documented "batch: non-ok titles failed in more than one way (more than one of 3, 4, 5, 6); the per-title results say which".
  - `error.hpp`: `Errc::mixed_failures`, plus `case Errc::mixed_failures: return exit_code::mixed_failures;` in `exit_code_for`.
  - `options.hpp`: `friend bool operator==(const DownloadOptions&, const DownloadOptions&) = default;`, used by dedupe. Every member already has `==`: `Selection`, `ApiBase`, `MangaId`, `std::filesystem::path`, optional, vector.
  - `fs/bounded_read.hpp`:
    ```cpp
    /// rev1 (fact-check). Opens `path` with O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK (a FIFO
    /// without a writer never blocks the open), then fstat: S_ISREG, S_ISFIFO and S_ISCHR are
    /// read; S_ISDIR -> Errc::failure "cannot read <label>: is a directory"; any other type ->
    /// Errc::failure "cannot read <label>: not a regular file, pipe or terminal". For S_ISFIFO
    /// and S_ISCHR, O_NONBLOCK is cleared (fcntl F_GETFL/F_SETFL) before reading. st_size is
    /// only a reserve hint (procfs reports 0). Then the read_bounded_fd loop. Closed before
    /// return.
    [[nodiscard]] Result<std::string> read_bounded_file(const std::filesystem::path& path,
                                                        std::string_view label, std::size_t cap);
    /// The loop on an fd the caller owns (never closed, flags never changed). fstat type policy:
    /// S_ISREG/S_ISFIFO/S_ISCHR/S_ISSOCK read, S_ISDIR -> "is a directory", others -> "not a
    /// regular file, pipe or terminal". Then read until EOF, EINTR retried, every request at most
    /// cap + 1 - got bytes, so at most cap+1 bytes are ever consumed. More than `cap` bytes ->
    /// Errc::usage "<label>: is larger than <cap> bytes". fd < 0, EBADF or any other read error
    /// (EAGAIN on an inherited non-blocking fd included) -> Errc::failure
    /// "cannot read <label>: <strerror>". Precondition: cap < SIZE_MAX.
    [[nodiscard]] Result<std::string> read_bounded_fd(int fd, std::string_view label, std::size_t cap);
    ```
  - `fs/row_dir.hpp` (rev1 (fact-check)):
    ```cpp
    enum class RowDirKind { ok, row_error, global_error };
    struct RowDirVerdict {
        RowDirKind kind = RowDirKind::ok;
        std::string message{};   ///< one line, raw bytes ("<path>: <strerror>"); empty when ok
        friend bool operator==(const RowDirVerdict&, const RowDirVerdict&) = default;
    };
    /// Read-only pre-flight for one row's output directory root/sub (`sub` already passed
    /// validate_list_dir; empty = the row has no `dir` cell). Creates and modifies NOTHING.
    /// 1. open(root, O_RDONLY|O_DIRECTORY|O_CLOEXEC), symlinks followed: ENOENT -> ok (download
    ///    creates it later; nothing below it can exist); any other failure -> global_error.
    /// 2. For each component of `sub`: openat(parent, c, O_RDONLY|O_DIRECTORY|O_CLOEXEC),
    ///    symlinks followed (OD1). ENOENT where fstatat(AT_SYMLINK_NOFOLLOW) also says ENOENT ->
    ///    the walk ends, ok. A dangling symlink, or classify_dir_errno(err, below_root = true).
    /// 3. faccessat(deepest existing dir fd, ".", W_OK|X_OK, AT_EACCESS) ->
    ///    classify_dir_errno(err, below_root = the deepest directory is not root).
    [[nodiscard]] RowDirVerdict check_row_dir(const std::filesystem::path& root, std::string_view sub);
    /// The pure errno table: ENOTDIR, ELOOP, ENAMETOOLONG, EACCES, EPERM -> row_error when
    /// below_root, else global_error; everything else (ENOSPC, EIO, EROFS, EDQUOT, EMFILE,
    /// ENFILE, ENOMEM, unknown) -> global_error.
    [[nodiscard]] RowDirKind classify_dir_errno(int err, bool below_root) noexcept;
    ```
  - `text_safety.hpp` (rev1 (fact-check); header-only, everything `constexpr`, a verbatim copy of today's `cmd_download_output.cpp` code so T5's switch-over is behaviour-preserving):
    ```cpp
    struct Utf8Decoded {
        std::size_t length = 0;      ///< 0 = not well-formed (overlong, surrogate, > U+10FFFF, truncated)
        std::uint32_t code_point = 0;
    };
    /// The well-formed UTF-8 sequence (1 to 4 bytes) at text[pos]; pos < text.size().
    [[nodiscard]] constexpr Utf8Decoded decode_utf8_at(std::string_view text, std::size_t pos) noexcept;
    /// C1 controls U+0080-U+009F, U+061C, U+200B-U+200F, U+202A-U+202E, U+2028/9,
    /// U+2060-U+206F, U+FEFF. (C0 and DEL are byte-level checks left to the callers.)
    [[nodiscard]] constexpr bool is_unsafe_code_point(std::uint32_t cp) noexcept;
    ```
  - `batch_csv.hpp`:
    ```cpp
    inline constexpr std::size_t max_list_bytes = 1024 * 1024;
    inline constexpr std::size_t max_list_titles = 10000;
    inline constexpr std::size_t max_list_field_bytes = 4096;
    inline constexpr std::size_t max_list_errors = 20;
    inline constexpr std::size_t max_list_echo_bytes = 32;   ///< cap for echoed header names

    /// One title record after unquoting and trimming. Absent or empty cells are nullopt.
    /// Every string is owned (no view into the input buffer).
    struct ListRecord {
        std::size_t line = 0;                          ///< 1-based physical start line
        std::string manga{};                           ///< never empty
        std::optional<std::string> title{};
        std::optional<std::string> dir{};
        std::optional<std::string> volumes{};
        std::optional<std::string> loose_chapters{};
        std::optional<std::string> layout{};
        std::optional<std::string> lang{};
        std::optional<std::string> group{};            ///< raw "A|B" text; split by plan_batch
        std::optional<std::string> direction{};
        friend bool operator==(const ListRecord&, const ListRecord&) = default;
    };
    struct TitleList {
        bool has_header = false;
        std::vector<ListRecord> records{};
        friend bool operator==(const TitleList&, const TitleList&) = default;
    };
    /// Exit-2 diagnostics: at most max_list_errors one-line messages ("<label>:<line>: ..." or
    /// "<label>: ..."), raw bytes (the app sanitizes when printing), plus how many more were found.
    struct ListErrors {
        std::vector<std::string> messages{};
        std::size_t more = 0;
        friend bool operator==(const ListErrors&, const ListErrors&) = default;
    };
    template <class T> using ListResult = std::expected<T, ListErrors>;

    /// Pure: bytes -> records. `label` is FILE as typed or "<stdin>", copied into messages.
    [[nodiscard]] ListResult<TitleList> parse_title_list(std::string_view bytes, std::string_view label);
    ```
  - `batch.hpp`. It includes `batch_csv.hpp`, `options.hpp`, `report.hpp`, `error.hpp`, `<span>`, `<map>`, `<optional>`:
    ```cpp
    /// What CLI11 binds for `batch` (owned by the batch Command). `defaults.manga` and
    /// `defaults.title` stay empty; `defaults.dir` is DIR.
    struct RawBatchArgs {
        std::string file{};                 ///< FILE as typed; "-" = stdin
        RawDownloadArgs defaults{};
        bool fail_fast = false;
    };
    struct BatchSettings {
        std::string file{};                 ///< as typed
        std::string label{};                ///< FILE as typed, or "<stdin>" for "-"
        RawDownloadArgs defaults{};         ///< validated CLI-level strings
        DownloadOptions base{};             ///< validated CLI-level options (manga = nil placeholder)
        bool fail_fast = false;
    };
    /// Errc::usage: FILE empty; FILE looks like a manga id ("FILE looks like a MangaDex title; use
    /// 'mangadex-dl download <title>' for one title"); DIR empty; DIR looks like a manga id; then
    /// every CLI-level option exactly as `download` validates it (manga excluded).
    [[nodiscard]] Result<BatchSettings> validate_batch_args(const RawBatchArgs& raw);

    struct BatchJob {
        std::size_t line = 0;
        RawDownloadArgs raw{};              ///< the merged strings (for exit-5 messages)
        DownloadOptions options{};          ///< validated
        std::string dir{};                  ///< effective DIR or DIR/<dir>, as typed
        std::optional<std::size_t> duplicate_of{}; ///< line of the first identical row
        std::string sub_dir{};              ///< rev1: the validated `dir` cell, empty when none
    };
    /// Merge (non-empty cell > CLI value), per-cell validation, dedupe. Errors accumulate.
    [[nodiscard]] ListResult<std::vector<BatchJob>> plan_batch(const TitleList& list,
                                                               const BatchSettings& settings);
    /// The `dir` cell rules (lexical containment); returns the cell unchanged; Errc::usage
    /// with a message that does not start with the column name.
    [[nodiscard]] Result<std::string> validate_list_dir(std::string_view cell);
    /// base + "/" + sub, without doubling a trailing '/' of base.
    [[nodiscard]] std::string join_list_dir(std::string_view base, std::string_view sub);

    enum class TitleStatus { ok, not_found, no_chapters, incomplete, network, failed, duplicate, not_run };
    /// 0 ok, 3 network, 4 not_found, 5 no_chapters, 6 incomplete, anything else failed.
    [[nodiscard]] TitleStatus title_status_for(int exit_code) noexcept;
    [[nodiscard]] std::string_view title_status_name(TitleStatus status) noexcept; ///< JSON spelling

    struct RowOutcome {                     ///< what the runner reports for one row
        int exit_code = 0;
        std::string error{};                ///< the row's closing message, empty when none
        RunSummary files{};
        std::optional<RunReport> report{};  ///< only when the caller keeps it (--json)
        bool interrupted = false;
        /// rev1 (fact-check): true = a global local condition (DIR itself unusable, ENOSPC, EIO,
        /// EROFS, anything unclassified): the batch stops. false with exit_code 1 = a row-scoped
        /// failure (check_row_dir row_error): the row is `failed` and the batch continues.
        bool fatal = false;
    };
    struct TitleResult {
        std::size_t line = 0;
        std::string manga_id{};
        std::string dir{};
        TitleStatus status = TitleStatus::not_run;
        std::optional<int> exit_code{};
        std::optional<std::string> error{};
        std::optional<std::size_t> duplicate_of{};
        RunSummary files{};
        std::optional<RunReport> report{};
    };
    enum class BatchStop { none, fail_fast, local_failure, network_streak, output_error, interrupted };
    struct BatchResult {
        std::vector<TitleResult> titles{};
        BatchStop stop = BatchStop::none;
        std::size_t stopped_at_line = 0;
    };
    inline constexpr std::size_t network_streak_limit = 3;

    /// The seam between the loop (core) and the download (app). Polymorphic base per house rules.
    class TitleRunner {
    public:
        TitleRunner(const TitleRunner&) = delete;  /* + the other three deleted */
        virtual ~TitleRunner() = default;
        /// `position` 1-based among non-duplicate rows; `total` = their count.
        [[nodiscard]] virtual RowOutcome run(const BatchJob& job, std::size_t position, std::size_t total) = 0;
        virtual void on_duplicate(const BatchJob& job) = 0;
        /// Checked after each row: true stops the batch (stdout is broken).
        [[nodiscard]] virtual bool output_failed() = 0;
    protected:
        TitleRunner() = default;
    };
    [[nodiscard]] BatchResult run_batch(std::span<const BatchJob> jobs, TitleRunner& runner, bool fail_fast);
    /// rev1 (fact-check): interrupted or stop == local_failure -> 1; otherwise the distinct codes
    /// of the non-ok rows (`failed` counts as 1): none -> 0, one -> it, several -> 7.
    [[nodiscard]] int batch_exit_code(const BatchResult& result) noexcept;
    /// The single closing `error:` line, or nullopt when every row is ok/duplicate.
    [[nodiscard]] std::optional<std::string> batch_closing_message(const BatchResult& result, bool dry_run);
    /// "1 file already exists; ..." / "N files already exist; ..." (download's exact text), or nullopt.
    [[nodiscard]] std::optional<std::string> exists_notice(std::size_t exists);
    /// The --json document, ordered_json, indent 2, replace handler, '\n'-terminated, never ANSI.
    [[nodiscard]] std::string batch_to_json(const BatchResult& result, std::string_view source, bool dry_run);

    /// Cross-row collision detection. Owns its keys.
    class OutputRegistry {
    public:
        /// Records `path` for `line`; returns the earlier line that already claimed it. rev1: keyed
        /// on std::filesystem::path(path).lexically_normal().generic_string() of the joined path.
        [[nodiscard]] std::optional<std::size_t> claim(std::string path, std::size_t line);
    private:
        std::map<std::string, std::size_t, std::less<>> by_path_{};
    };
    ```
  - `headers_test.cpp`:
    - `static_assert(md::exit_code::mixed_failures == 7)` and `exit_code_for(Errc::mixed_failures) == 7`;
    - limit constants;
    - `std::is_abstract_v<md::TitleRunner>`;
    - every new aggregate default-constructs and builds with partial designated init (warning-free under `-Werror` in both compilers);
    - `DownloadOptions{} == DownloadOptions{}`.
  - rev1 (fact-check) `text_safety_test.cpp`:
    - `static_assert`s that U+009B, U+0085, U+202E, U+2028, U+2066, U+200E, U+061C and U+FEFF are unsafe, and that `é`, `Z`, U+3042 and U+1F600 are not;
    - `decode_utf8_at` on ASCII, 2/3/4-byte sequences, overlong forms, a surrogate, > U+10FFFF and a truncated sequence.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` **and** `nice pixi run -e gcc check` are green (a new `Errc` enumerator is compiler-sensitive through `-Wswitch`). `git diff --stat tests/e2e/golden` is empty. Both reviewers approve.
- **Tests:** unit: `headers_test.cpp` and `text_safety_test.cpp` as above. Other tiers: none new; the existing suite proves no regression.

### T4 — App refactor: shared binding, shared per-title run, row-aware observer, `Io::stdin_fd`
- **Goal:** `download` becomes byte-identical in every output, and its pieces become reusable by `batch`.
- **Files:**
  - create `src/app/cmd_download_options.{hpp,cpp}`, `src/app/cmd_download_run.{hpp,cpp}`, `tests/cli/download_row_output_test.cpp`;
  - edit `src/app/cmd_download.cpp`, `src/app/cmd_download_output.{hpp,cpp}`, `src/app/io.{hpp,cpp}`, `tests/support/io_capture.hpp`.
- **Owns:** `src/app/cmd_download.cpp`, `src/app/cmd_download_options.hpp`, `src/app/cmd_download_options.cpp`, `src/app/cmd_download_run.hpp`, `src/app/cmd_download_run.cpp`, `src/app/cmd_download_output.hpp`, `src/app/cmd_download_output.cpp`, `src/app/io.hpp`, `src/app/io.cpp`, `tests/support/io_capture.hpp`, `tests/cli/download_row_output_test.cpp`.
- **Build files for build-fixer:** none.
- **What changes:**
  - `cmd_download_options.{hpp,cpp}`:
    - `void bind_selection_options(CLI::App& sub, RawDownloadArgs& args);` binds `--volumes`, `--loose-chapters`, `--layout`, `-l,--lang`, `--group`, `--data-saver`, `--direction`.
    - `void bind_run_options(CLI::App& sub, RawDownloadArgs& args);` binds `--overwrite`, `-n,--dry-run`, `-j,--jobs`, `--no-report`, hidden `--api-base`.
    - Help strings, type names, validators and `capture_default_str` are moved verbatim, with the `--group` comment.
    - The header forward-declares `CLI::App` (as `cmd_download.hpp` does) and documents: "binds into `args`, which must be a member of the Command that outlives parse() and dispatch".
  - `DownloadCommand::bind` becomes: MANGA, DIR, `bind_selection_options`, `--title` (`add_option_function`, unchanged), `bind_run_options`, footer. Same add order, so `download --help` is unchanged (V-A3).
  - `cmd_download_run.{hpp,cpp}` moves code out of `cmd_download.cpp`'s anonymous namespace:
    - `Backend`, `user_agent()`, `make_jitter()` (with `jitter_seed`), `with_remedy`, `no_chapters_message`, `incomplete_message`;
    - `make_backend`, with its signature narrowed to `make_backend(const ApiBase& api_base, const std::string& agent)`;
    - plus:
    ```cpp
    struct TitleRun {
        std::optional<RunReport> report{};   ///< set when run_download returned a report
        std::optional<Error> error{};        ///< set when it returned an Error (incl. interrupted)
        int exit_code = 0;
        std::string closing_error{};         ///< the message printed for this run (no prefix/remedy), empty if none
    };
    /// One title, exactly as `download` runs it: fresh DownloadServices over `backend`
    /// (transport/clock borrowed, make_jitter(), default stop, `agent`), a ConsoleObserver with
    /// `row`, run_download, observer.finish(), then the closing lines `download` prints, each as
    /// print_error(io, policy, row.prefix + message). Never prints JSON. `backend` and `io` must
    /// outlive the call.
    [[nodiscard]] TitleRun run_title(const DownloadOptions& options, const RawDownloadArgs& raw,
                                     const Backend& backend, const std::string& agent, Io& io,
                                     const OutputPolicy& policy, const RowContext& row);
    ```
  - `DownloadCommand::run` = validate → policy → handlers → `make_backend(options->api_base, agent)` → `run_title(…, RowContext{})` → if `policy.json && report` → `print_report_json` → return. The order of every print is unchanged.
  - `cmd_download_output.hpp`:
    ```cpp
    struct RowContext {
        std::string prefix{};        ///< "<file>:<line>: " (raw); empty for `download`
        std::size_t position = 0;    ///< 1-based; 0 = no "(i/n) " progress counter
        std::size_t total = 0;
        bool exists_notice = true;   ///< false: finish() leaves the notice to the caller (batch)
        friend bool operator==(const RowContext&, const RowContext&) = default;
    };
    ```
    - `ConsoleObserver(Io& io, const OutputPolicy& policy, RowContext row = {})` takes `row` by value (sink) and stores an owned `row_`. It drops `noexcept`.
    - Warnings become `warning: <prefix><message>`. A failed file becomes `error: <prefix><path>: <error>`.
    - The progress line becomes `(<position>/<total>) [i/n] <label>: d/t pages` when `position > 0`.
    - `finish()` skips the notice when `!exists_notice`.
    - Verbose/trace lines are unprefixed.
  - `io.hpp`/`io.cpp`: `int stdin_fd = -1;` as the last `Io` member (doc: "fd to read stdin from, borrowed, never closed; -1 = closed"). `process_io()` sets `.stdin_fd = 0`.
  - `io_capture.hpp`: `run_cli(args, Tty tty = {}, EnvSnapshot env = {}, int stdin_fd = -1)`.
- **Dependencies:** none (it uses no T1 symbol). It runs in parallel with T1.
- **Done when:**
  - `nice pixi run check` and `nice pixi run -e gcc check` are green;
  - `git diff --stat tests/e2e/golden` is empty (**no bless may be requested in T4**);
  - rev1 (fact-check), V-A3: the real installed `download --help` (`build/default/dev/e2e-prefix/bin/mangadex-dl download --help`), captured before the change and after it, is byte-identical (`cmp`). The same holds for root `--help`;
  - every existing `cli.*`, `e2e.download.*` and `e2e.edge.*` test passes unmodified (no edit under `tests/cli/download_*` except the new file);
  - both reviewers approve.
- **Tests:**
  - **unit:** none.
  - **cli** (`download_row_output_test.cpp`): drive `ConsoleObserver` directly with an `ostringstream` Io.
    - Default `RowContext` output equals today's for a warning, a failed file and the exists notice.
    - With a prefix, `warning:`/`error:` lines carry it after the label, and verbose lines don't.
    - `(2/5) [1/2] Vol. 1: 0/3 pages` appears with `stderr_tty`.
    - `exists_notice=false` suppresses the notice.
    - A prefix holding ESC/invalid UTF-8 prints as `?`.
    - `run_cli(..., stdin_fd)` compiles and passes the fd through, checked by `Io` construction in a trivial test.
  - **e2e:** none new. The existing goldens (`download.help`, `download.dry-run-clean`, `download.json-dry-run`, `edge.help`) are the byte-stability proof.

### T2 — Core: CSV parser, bounded reader and the row-dir pre-flight (rev1 (fact-check))
- **Goal:** bytes → `TitleList` per the spec's "CSV format", a capped reader for files and fds, and (rev1) the read-only per-row directory pre-flight.
- **Files:** create `src/core/batch_csv.cpp`, `src/core/fs/bounded_read.cpp`, `src/core/fs/row_dir.cpp`, `tests/unit/batch_csv/batch_csv_test.cpp`, `tests/unit/fs/bounded_read_test.cpp`, `tests/unit/fs/row_dir_test.cpp`.
- **Owns:** exactly those six files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`read_bounded_*` (rev1 (fact-check), cluster B required steps):**
    - `read_bounded_file` opens with `O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK` into a `UniqueFd` (from `fs/output_dir.hpp`);
    - `fstat` decides the type, exactly as the header says. Never rely on `read`'s `EISDIR`;
    - for `S_ISFIFO`/`S_ISCHR` opened from a path, clear `O_NONBLOCK` with `fcntl(F_GETFL)`/`fcntl(F_SETFL)` before reading. The fd form never changes the caller's flags;
    - `st_size` is used only for regular files, range-checked with `std::in_range<std::size_t>`, and only as a `reserve` hint of `min(st_size, cap + 1)`. Procfs files report 0 and still read correctly;
    - one shared read loop to EOF: request size `min(64 KiB, cap + 1 - got)`, `EINTR` retried, the cap enforced on the bytes actually read (more than `cap` → usage);
    - messages use `std::system_category().message(errno)` the way existing core code does.
  - **`check_row_dir` / `classify_dir_errno` (rev1 (fact-check)):** exactly the `fs/row_dir.hpp` contract.
    - Every directory fd is a `UniqueFd`.
    - Nothing calls `mkdir*`, `open(O_CREAT)` or anything else that writes.
    - Each message is `<path as joined>: <strerror>`, or `<path>: is a dangling symbolic link`.
  - **The CSV lexer's UTF-8 check** uses `decode_utf8_at` from `text_safety.hpp` (rev1 (fact-check)), so it shares one decoder with `terminal_safe`.
  - **`parse_title_list`, lexing:**
    - UTF-16 BOM (`FF FE` / `FE FF`) → `<label>: is UTF-16; save it as UTF-8`;
    - skip a UTF-8 BOM;
    - whole-input UTF-8 + NUL check with the line of the first bad byte → `<label>:<line>: not valid UTF-8`;
    - an RFC 4180 lexer over `,`: LF and CRLF, a final line without newline, `""` → `"`, CR/LF/`,` literal inside quotes;
    - a bare CR outside quotes, a `"` inside an unquoted field, junk after a closing quote, or an unterminated quote (record start line) is an error;
    - fields are ASCII space/tab-trimmed after unquoting;
    - a record starting with `#` outside quotes is a comment, and empty, whitespace-only or all-empty-field records are skipped.
  - **Lexical/encoding errors are fatal at the first one** (one diagnostic): the stream cannot be resynchronized.
  - **Header:** the first non-comment record is a header iff `!looks_like_manga_id(first field)`.
    - Names match ASCII case-insensitively against `manga, title, dir, volumes, loose-chapters, layout, lang, group, direction, note`.
    - `manga` is required.
    - An unknown, empty or duplicate name gives `<label>:<line>: unknown column '<name>' (known: manga, title, dir, volumes, loose-chapters, layout, lang, group, direction, note)`. The echo is capped at `max_list_echo_bytes`, backed off to a UTF-8 boundary.
    - If the header record contains `;` and no `,`, append `; the separator is ','`.
  - **Records:**
    - In headerless mode, field 1 is `manga`, empty extras are ignored, and a non-empty extra gives `<label>:<line>: more than one column; add a header row (manga,title,...)`.
    - In header mode, short rows → missing cells are nullopt. A long row is OK only if the extras are empty, else `<label>:<line>: <n> fields but the header has <m>`.
    - An empty `manga` with other non-empty cells gives `<label>:<line>: manga: the title is missing`.
    - `note` is dropped.
    - A field over `max_list_field_bytes` gives `<label>:<line>: a field is longer than 4096 bytes`.
    - More than `max_list_titles` records gives `<label>: lists more than 10000 titles`.
    - **These record-level errors accumulate**, up to `max_list_errors` messages, with `more` counting the rest.
  - **Line numbers** are 1-based physical start lines. A BOM does not count; a quoted LF advances the line.
  - Internally the lexer may hold `string_view`s into `bytes`. **Every stored cell is copied into a `std::string` before `parse_title_list` returns.**
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green; both reviewers approve.
- **Tests (unit):**
  - **AC1 accept set:** BOM; LF/CRLF/mixed; no final newline; quoted `,`, `""`, CR and LF; trimming inside and outside quotes; `#` comments (and `"#x"` quoted is *not* a comment); blank and whitespace-only lines; headerless one column with empty extras; header in any order and any case; ragged short rows; empty extra fields; a URL in field 1 means headerless.
  - **AC2 reject set:** each with the exact message and the right start line: unterminated quote (multi-line record → start line); quote in an unquoted field; junk after a closing quote; bare CR; invalid UTF-8 (line of the bad byte); NUL; UTF-16 LE and BE BOM; unknown, empty and duplicate column; no `manga` column; non-empty extra field; headerless two columns; the `;` hint; a 40-byte unknown name echoed as ≤ 32 bytes on a UTF-8 boundary; the `manga` cell never echoed.
  - **AC3 limits:** a field of 4097 B; 10001 rows; 25 record errors → 20 messages + `more == 5`.
  - **bounded_read (rev1 (fact-check)):**
    - a regular file of cap+1 bytes → usage; exactly cap bytes → OK; an empty file → `""`;
    - a pipe with 100 bytes and cap 16 → usage, and the remaining 83 bytes are still readable from the pipe (≤ cap+1 consumed);
    - `/proc/self/status` (`st_size` 0, content non-empty), with a cap above its size → its full content;
    - a FIFO (`mkfifo` in a `TempDir`) **without a writer** → `read_bounded_file` returns `""` promptly. It never blocks: `O_NONBLOCK` on open, then EOF because no writer exists;
    - a FIFO **with a writer**: a `std::jthread` opens it for writing (retrying on `ENXIO` until the reader has opened), writes a 3-line CSV and closes → exactly those bytes;
    - a directory → failure `is a directory`, in both forms;
    - a missing file → failure with strerror text;
    - fd −1 and a closed fd → failure `cannot read <label>: Bad file descriptor`;
    - `read_bounded_fd` never closes the caller's fd and never changes its flags (`fcntl(F_GETFD)`/`F_GETFL` unchanged).
  - **row_dir (rev1 (fact-check)):**
    - the `classify_dir_errno` table: ENOTDIR, ELOOP, ENAMETOOLONG, EACCES and EPERM → `row_error` below root and `global_error` at root; ENOSPC, EIO, EROFS, EDQUOT, EMFILE, ENFILE, ENOMEM and an unknown errno → `global_error` either way. The EACCES rows need no chmod-based test, which running as root would defeat;
    - in a `TempDir`:
      - root missing → ok;
      - root is a regular file → `global_error`;
      - `sub = a/b` with `a` a regular file → `row_error`;
      - `a` a dangling symlink → `row_error`;
      - `a` a symlink to a directory → ok (followed, OD1);
      - `a` existing and `b` missing → ok;
    - nothing was created: a recursive listing of the `TempDir` is identical before and after every call.

### T3a — Core: merge, per-cell validation, `dir` containment, dedupe
- **Goal:** `TitleList` + CLI defaults → `std::vector<BatchJob>`, with column-attributed diagnostics.
- **Files:** create `src/core/batch_plan.cpp`, `src/core/options_internal.hpp`, `tests/unit/batch/batch_plan_test.cpp`; edit `src/core/options.cpp`.
- **Owns:** `src/core/batch_plan.cpp`, `src/core/options_internal.hpp`, `src/core/options.cpp`, `tests/unit/batch/batch_plan_test.cpp`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`options_internal.hpp`** (core-private, `mangadex_dl::detail`) declares `parse_layout`, `parse_direction` and `parse_selection(flag, spec)`, moved out of `options.cpp`'s anonymous namespace unchanged. It also declares `Result<DownloadOptions> validate_common(const RawDownloadArgs& raw, MangaId manga)`, which is everything `validate_download_args` checks after the DIR checks, in today's order.
  - `validate_download_args` = `MangaId::parse` → the DIR-empty and DIR-looks-like-id checks → `validate_common`. Its messages and their order are unchanged, which the existing `options_test.cpp` proves.
  - **`validate_batch_args`:**
    - FILE empty → `FILE must not be empty`;
    - FILE looks like an id → the spec message;
    - DIR empty;
    - DIR looks like an id → `DIR looks like a MangaDex title; list the titles in FILE`;
    - then `validate_common(defaults, MangaId{})` for the CLI-level values;
    - `label = file == "-" ? "<stdin>" : file`.
  - **`plan_batch`**, per record:
    1. `MangaId::parse` fails → `<label>:<line>: manga: ` + the fixed invalid-manga text. The cell is never echoed.
    2. Each non-empty cell goes through its validator: `title` → `validate_title_override`; `volumes`/`loose-chapters` → `parse_selection`; `layout` → `parse_layout`; `lang` → `validate_language`; `direction` → `parse_direction`; `dir` → `validate_list_dir`; `group` → split on `|`, trim each part, an empty entry → `empty name in the list`.
    3. A failure prints `<label>:<line>: <column>: <message>`. A leading `--<column>: ` or `--<column> ` is stripped from the validator's message (column names equal the long option names). Example: `lang: must be a lowercase language code such as en, pt-br or es-la`.
    4. The merged `RawDownloadArgs` (a copy of `settings.defaults` with the cells applied; `group` *replaces* the list; `dir` → `join_list_dir(defaults.dir, cell)`) goes through `validate_common(merged, manga)` for cross-field errors (`both none`) → `<label>:<line>: <message>`.
  - **Errors accumulate** across records (`max_list_errors` + `more`).
  - **Dedupe:** group job indices by `MangaId` (`std::map<MangaId, std::vector<std::size_t>>`, indices and never pointers into the growing vector). Within a group, a job whose `options ==` an earlier job's gets `duplicate_of = earlier.line`. O(n log n + Σk²).
  - **`validate_list_dir` (rev1 (fact-check), security-reviewer OD1 tightening).** It runs on the trimmed cell: it trims ASCII space/tab itself because it is public, although the parser already trims. It rejects, in this order:
    - empty → `must not be empty` (defensive: `plan_batch` never passes an empty cell);
    - NUL, any other C0 control, or DEL → `must not contain control characters`;
    - invalid UTF-8 (`decode_utf8_at`) → `must be valid UTF-8`;
    - a code point with `is_unsafe_code_point` → `must not contain invisible, bidirectional or terminal control characters`. That covers C1 U+0080–U+009F, U+061C, U+200B–U+200F, U+202A–U+202E, U+2028/2029, U+2060–U+206F and U+FEFF. `dir` reaches stdout raw in every path, so this is required;
    - `\` → `must not contain '\'`;
    - a leading `/` → `must be a relative path inside DIR`;
    - `//` or a trailing `/` → `must not contain empty segments ('//' or a trailing '/')`;
    - a `.` or `..` segment → `must not contain '.' or '..' segments`;
    - more than 32 segments → `has more than 32 levels`;
    - a segment over 255 bytes → `a segment is longer than 255 bytes`;
    - over 1024 bytes in total → `is longer than 1024 bytes`.
  - **`BatchJob::sub_dir`** (rev1) = the validated cell, empty when the row has none.
  - **`group` cells (rev1 (fact-check)):**
    - Each `|`-separated name is trimmed.
    - A name containing a C0/DEL control (a quoted LF, for example) → `group: must not contain control characters`.
    - `&`, `#` and every other printable character are kept as they are. Group names never reach a URL (see "Resolved in-repo").
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green, including the unchanged `unit.options*`; both reviewers approve.
- **Tests (unit):**
  - **AC4:** a cell overrides the CLI value; an empty cell inherits; `group` replaces, `A | B ` → `{"A","B"}`, `A||B` → error; `volumes=none` + `loose-chapters=none` after merge → a row error; `dir` accept set: `a`, `a/b`, `Zoë/x`, a 255-B segment, 32 segments, ` a ` → `a` (trimmed). Reject set: `/abs`, `../x`, `a/..`, `.`, `a//b`, `a/`, a 256-B segment, 1025 B total, a control character, invalid UTF-8, and rev1 (fact-check): U+009B, U+0085, U+202E, U+2028, U+FEFF, `a\b`, an embedded NUL, 33 segments. Also rev1: `group` `A&B|C#D` → `{"A&B","C#D"}`, and `"A\nB"` (a quoted LF) is rejected; `join_list_dir` with `out`, `out/` and `.`.
  - **AC5:** exact `<label>:<line>: <column>: <message>` strings; the `manga` text never contains the cell; 25 bad rows → 20 + `more == 5`.
  - **AC6:** identical effective rows → `duplicate_of`; rows differing only in `lang`, or only in `dir`, both run; the same title written as a URL and as a bare UUID counts as a duplicate (canonical id).
  - `validate_batch_args`: every message above; CLI-level `--lang bogus` rejected even when every row sets `lang`.

### T3b — Core: the batch loop, aggregation, messages, collisions and JSON
- **Goal:** `run_batch` + `batch_exit_code` + `batch_closing_message` + `exists_notice` + `OutputRegistry` + `batch_to_json`.
- **Files:** create `src/core/batch_run.cpp`, `src/core/batch_json.cpp`, `src/core/report_json.hpp`, `tests/unit/batch/batch_run_test.cpp`, `tests/unit/batch/batch_json_test.cpp`; edit `src/core/report.cpp`.
- **Owns:** `src/core/batch_run.cpp`, `src/core/batch_json.cpp`, `src/core/report_json.hpp`, `src/core/report.cpp`, `tests/unit/batch/batch_run_test.cpp`, `tests/unit/batch/batch_json_test.cpp`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`report_json.hpp`** (core-private): `namespace mangadex_dl::detail { [[nodiscard]] nlohmann::ordered_json report_json(const RunReport&); }`. `report_to_json` = `report_json(r).dump(2, ' ', false, replace) + '\n'`, byte-identical.
  - **`run_batch`:**
    - For each job in order:
      1. If `interrupted` is already recorded or `interrupt_requested()` is true → `stop = interrupted`, stop.
      2. A duplicate → `on_duplicate` and status `duplicate`, without calling `run`.
      3. Otherwise call `runner.run(job, position, total)` → `TitleResult`, with the status from `title_status_for` and the strings copied from the job.
    - After each run, check in this order:
      1. `interrupted` → `stop = interrupted`, and the row is `not_run`;
      2. `fatal` → `local_failure` (rev1 (fact-check): a non-fatal `failed` row, which is a row-scoped `dir` failure, does not stop the batch);
      3. `runner.output_failed()` → `output_error`;
      4. `fail_fast && status != ok` → `fail_fast`;
      5. a streak of `network_streak_limit` consecutive `network` rows → `network_streak`. Duplicates neither count nor reset the streak.
    - A non-interrupt stop is recorded **only if a runnable row remains**. Then `stopped_at_line` = this row's line, and the remaining non-duplicate rows are `not_run` (duplicates keep `duplicate`).
  - **`batch_exit_code` (rev1 (fact-check)):**
    - `interrupted` → `exit_code::failure` (fallback: `main` re-raises).
    - `stop == local_failure` (a fatal row) → 1, whatever the other rows did.
    - Otherwise, take the distinct codes of the non-ok rows: `failed` counts as 1, and `network`/`not_found`/`no_chapters`/`incomplete` as 3/4/5/6. None → 0, one → that code, more than one → 7. So all failed → 1, and failed + 4 → 7.
    - `duplicate`/`not_run` never count.
  - **`batch_closing_message`** (draft wording; cli-ux board signs off):
    - `stopped at line <k>: 3 titles in a row failed with network errors; <r> titles not run`
    - `stopped at line <k>: --fail-fast; <r> titles not run`
    - `stopped at line <k>: a local error (see above); <r> titles not run`
    - `stopped at line <k>: cannot write to stdout; <r> titles not run`
    - otherwise `<n> of <m> titles did not complete (lines 4, 9, 12); run the same command again to retry them`, or for a dry run `<n> of <m> titles would fail (lines …)`.
    - `m` counts non-duplicate rows. Singular forms (`1 title`, `line 4`). At most 10 line numbers, then ` and <k> more`.
    - nullopt when all rows are ok/duplicate, and when interrupted.
  - **`exists_notice`:** `download`'s exact two strings.
  - **`OutputRegistry::claim`:** the first claim wins; a claim by the same line returns nullopt.
  - **`batch_to_json`:**
    - `ordered_json`: `format_version: 1`, `dry_run`, `source`, `titles[]` (keys in the spec's order, with `null` for absent optionals; `report` = `report_json(*report)` or `null`);
    - `summary.titles` with all 9 keys, where total = all rows;
    - `summary.files` summed from `TitleResult::files`;
    - `dump(2, ' ', false, replace) + '\n'`.
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green, including the unchanged `unit.report*` and the `e2e.download.json-dry-run` golden; both reviewers approve.
- **Tests (unit; `FakeRunner` returns scripted `RowOutcome`s and records calls):**
  - **AC7:**
    - all ok → 0; uniform 4 → 4; {5,4} → 7; {3,6} → 7;
    - a **fatal** exit 1 at row 2 of 4 → 1, rows 3–4 `not_run`, `local_failure`, line 2. The same after an earlier 4 → still 1;
    - a fatal exit 1 on the last row → 1, `stop == none`;
    - rev1 (fact-check), **non-fatal** `failed` rows (row-scoped `dir` failures):
      - at row 1 of 3 → rows 2 and 3 still run;
      - {failed, ok, ok} → 1; {failed, 4} → 7; {failed, failed} → 1;
      - non-fatal `failed` with `fail_fast` → a `fail_fast` stop, exit 1;
      - the closing line lists the failed row in `did not complete (line …)`;
    - a duplicate in the middle is not run and doesn't break the streak;
    - 3,3,3,3 → `network_streak` after row 3, row 4 `not_run`, exit 3;
    - 3,4,3,3,3 (5 rows) → the streak is rows 3–5 and row 5 is the last row, so `stop == none`, exit 7; 4,3,3,3,0 → `network_streak` after row 4, row 5 `not_run`, exit 7;
    - `fail_fast` with a 5 at row 1 of 2 → exit 5, row 2 `not_run`;
    - `output_failed()` true after row 1 → `output_error`.
  - **AC10 (unit tier):** the fake runner calls `simulate_interrupt_for_testing(SIGINT)` during row 1 → row 2 is never run, `stop == interrupted`, closing message nullopt, exit 1 fallback. A flag set before the call → no row runs. Use an RAII flag reset like `download_test.cpp`'s `InterruptFlagReset`.
  - **Closing messages:** every variant, singular/plural, the 10-line cap, dry-run wording.
  - **AC8:**
    - parse the output with `nlohmann::ordered_json::parse`: exactly the listed keys and their types, `null`s;
    - `report` re-dumped + `'\n'` == `report_to_json(report)` byte for byte (V-C2);
    - invalid-UTF-8 `source`/`dir` → valid JSON with U+FFFD;
    - no `0x1B` byte in the output for a title containing ESC;
    - empty result → `"titles": []` and zeroed summary;
    - rev1 (fact-check), the cluster C caveat guard: walking the parsed document of a full fixture-like report finds no `number_float` node, so the byte-equal round trip covers only strings, integers, booleans, arrays, objects and null.
  - `exists_notice(0/1/2)`; `OutputRegistry`.

### T5 — App: `cmd_batch` (not yet registered)
- **Goal:** the whole `batch` command, as a free function that is testable without CLI11, plus `register_batch` for T6.
- **Files:** create `src/app/cmd_batch.hpp`, `src/app/cmd_batch.cpp`, `tests/cli/batch_cli_test.cpp`. rev1 (fact-check): edit `src/app/cmd_download_output.cpp` so that `terminal_safe` uses `decode_utf8_at`/`is_unsafe_code_point` from `text_safety.hpp`, deleting its local copies. Its output is unchanged.
- **Owns:** exactly those four files. `cmd_download_output.cpp` passes from T4 (batch 1) to T5 (batch 3); no other task touches it in between.
- **Build files for build-fixer:** none.
- **What changes:**
  - `cmd_batch.hpp`: `[[nodiscard]] int run_batch_command(const RawBatchArgs& args, const GlobalOptions& globals, Io& io);` and `[[nodiscard]] std::unique_ptr<Command> register_batch(CLI::App& parent, Command*& selected);`, with the same lifetime doc as `register_download`.
  - `BatchCommand final : Command` owns `RawBatchArgs args_{}`. Its `bind()` does:
    - `FILE` (required, `type_name("")`, "CSV file listing the titles ('-' reads stdin)");
    - `DIR` → `args_.defaults.dir` (`capture_default_str`);
    - `bind_selection_options(sub, args_.defaults)`, `bind_run_options(sub, args_.defaults)`;
    - `--fail-fast` → `args_.fail_fast` ("Stop at the first title that does not succeed");
    - `sub.footer(batch_footer + exit_status_footer())`, with the spec's CSV/Examples/restart text, pre-aligned, ≤ 80 columns. rev1 (fact-check): it ends with the spec's new paragraph, a draft for cli-ux: `Symlinks inside DIR are followed: don't point DIR at a directory that others` / `can write to. Each title costs several API requests: a long list is a long run.`
  - `run()` → `run_batch_command(args_, globals, io)`. The selecting callback captures `[slot = &selected, self = command.get()]`.
  - **`run_batch_command`, phases:**
    1. `policy = output_policy(globals, io)`.
    2. `validate_batch_args` → on error, `print_error` + `Run 'mangadex-dl batch --help' for more information.` → 2.
    3. **Read.** rev1 (fact-check) fd guard: nothing in phases 1–3 opens a file descriptor before this read (no handlers, no backend, no `OutputDir`, no fixture root, no `check_row_dir`), so a closed fd 0 can't be taken over. Keep it that way; both reviewers check it. For `-`: print the hint `mangadex-dl: reading titles from the terminal; end with Ctrl-D` first when `stdin_tty && stderr_tty && !quiet`, then `read_bounded_fd(io.stdin_fd, "<stdin>", max_list_bytes)`. Otherwise `read_bounded_file(file, label, max_list_bytes)`. Usage errors → message + hint → 2. Failure → `print_error` → 1, with no hint.
    4. `parse_title_list` and then `plan_batch`. On errors, print each message, then `... and <n> more errors` when `more > 0`, then the hint → 2.
    5. Empty job list → warning `<label>: lists no titles` (not with `-q`); with `--json`, `batch_to_json({}, file, dry_run)`; return 0. No handlers, backend or filesystem access.
    6. `ScopedInterruptHandlers::install()` → `user_agent()` → `make_backend(settings.base.api_base, agent)`. A failure → `print_error(with_remedy)` → its code.
    7. Declare `OutputRegistry registry`, then `ConsoleTitleRunner runner{…}`, which holds pointers to `backend`, `agent`, `io`, `policy`, `registry`, `label`, `keep_reports = policy.json`. Then `result = run_batch(jobs, runner, fail_fast)`.
    8. Interrupted → return `batch_exit_code(result)` at once (no JSON, no exists notice, no closing line).
    9. Otherwise: `exists_notice(Σ files.exists)` as a warning (not with `-q`) → `batch_closing_message` via `print_error` → with `--json`, `batch_to_json` on `io.out` → return `batch_exit_code(result)`.
  - **`ConsoleTitleRunner`:**
    - `run()`: with `-v` (and not `-q`), `mangadex-dl: <label>:<line>: <uuid>`.
    - rev1 (fact-check), pre-flight: `check_row_dir(settings.defaults.dir, job.sub_dir)`.
      - `row_error` → no download; print `error: <label>:<line>: <message>`; return `RowOutcome{.exit_code = 1, .error = message, .fatal = false}`.
      - `global_error` → the same with `.fatal = true`.
    - Then `run_title(job.options, job.raw, backend, agent, io, policy, RowContext{.prefix = "<label>:<line>: ", .position, .total, .exists_notice = false})`.
    - For every `written`/`planned` outcome, `registry.claim(path, line)` (rev1: the registry normalizes the joined path) → on a hit, the warning `<label>:<line>: <path> was already written by line <k>; give one of them a 'title' column`. An `exists` outcome whose path an earlier row claimed warns too, so the second row of AC12 warns with or without `--overwrite`.
    - It maps `TitleRun` → `RowOutcome`: `files = summarize(*report)`, `report` moved only if `keep_reports`, `interrupted = error && error->code == Errc::interrupted`. rev1 (fact-check): `fatal = true` for any `Errc::failure`/`Errc::io`/`Errc::usage` from `run_title`, because the pre-flight has already cleared the row-scoped cases; a later failure is treated as global, which is the conservative choice.
    - `on_duplicate`: warning `<label>:<line>: same as line <k>; skipped`.
    - `output_failed()`: `io.out.flush(); return !io.out;`.
- **Dependencies:** T2, T3a, T3b, T4.
- **Done when:**
  - `nice pixi run check` is green;
  - `nice pixi run tsan -R 'cli\.batch'` is green: per-row thread start/join through `run_download`, with one Backend shared across rows;
  - both reviewers approve. `memory-safety-reviewer` needs evidence that the multi-row fixture cases exercised the Backend-across-rows path under ASan+UBSan and TSan.
- **Tests (cli, `batch_cli_test.cpp`):** call `run_batch_command` with a filled `RawBatchArgs`, `GlobalOptions` and an `Io` around `ostringstream`. Fixture trees come from `tests/support/mdx_fixture.hpp` in a `TempDir`; the CSV is a temp file, or a temp file opened as `stdin_fd`.
  - **AC9:** a clean id → exit 0; stdout == `download`'s paths; stderr empty; the files exist.
  - **AC10:** the flag is simulated before the call → nothing written, empty stdout, no JSON, no closing line, rc 1. A second `run_batch_command` afterwards succeeds (handlers restored, the one-live flag released).
  - **AC11:**
    - a CSV error, CLI `lang=bogus`, FILE looks like an id, DIR looks like an id, FILE `""` → exit 2, empty stdout, the hint line, DIR not created;
    - with `api_base = "file:///nonexistent"` plus a CSV error, still exit 2 (the backend is never built).
  - **AC12:** `manga,volumes` rows `1111…,1` and `1111…,1-2` → row 2's Vol. 01 is `exists`, with a collision warning naming lines 2 and 3. With `overwrite` the warning is still printed.
  - **AC13:** row prefixes on `warning:`/`error:` lines; the aggregated "already exist" line appears once at the end on a re-run; `-q` suppresses warnings but keeps the closing error.
  - **stdin:**
    - an fd to a temp CSV with `file = "-"` → same stdout as the FILE form;
    - `stdin_fd = -1` → exit 1 `cannot read <stdin>: …`;
    - the hint appears only with `stdin_tty && stderr_tty` and not with `quiet`.
  - **JSON:**
    - nochapters tree rows 5555/6666 → exit 7; the document parses, with statuses `no_chapters`/`not_found` and `summary.titles.total == 2`;
    - empty list → `titles: []`, exit 0;
    - `--json` with exit 1 (DIR is a regular file) still prints the document with `not_run` rows.
  - `--dry-run` creates neither DIR nor DIR/<dir>.
  - A row `dir` lands under `out/<dir>/`.
  - A duplicate row warns and doesn't run.
  - **`io.out` = `std::ofstream("/dev/full")`** with a 2-row CSV → the batch stops after row 1 (`output_error`), and the closing line says so.
  - **Progress:** `stderr_tty` → `(1/2) [1/2] Vol. 1:` on err; none with `json` or `quiet`.
  - **Color:** `--color=always` colors `error:` on stderr; stdout/JSON never contain ESC.
  - **rev1 (fact-check), row-scoped `dir` failures.** In a `TempDir`, `out/blocked` is a regular file, with `manga,dir` rows `1111…,blocked/x` and `1111…,ok`:
    - row 1 is `failed` (exit_code 1, `error` names `out/blocked/x`), and row 2 still runs and writes;
    - the batch exits 1, with no `stopped at line` line;
    - the same holds with `--dry-run`;
    - `--json` shows `failed`, then `ok`;
    - with row 2 an unknown id instead → exit 7.
  - **rev1, global failure:** DIR itself a regular file → the batch stops at row 1 (`stopped at line …: a local error`), the rest is `not_run`, exit 1.
  - **rev1, nothing left behind:** an unknown-id row (exit 4) and a nochapters row (exit 5), each with `dir` = `sub/deeper` → neither `out/sub` nor `out` exists afterwards. If `run_download` turns out to create DIR before planning, that is a coordination event, not a test change.
  - **rev1, group values never reach a URL:** a fixture run with the `group` cell `A&B|C#D` and `-vv` (the per-request trace lines) → exit 0, and no trace line contains `A&B`, `C#D`, `%26` or `%23`.
  - **rev1, one predicate:** for every code point in U+0000–U+2FFF and U+FE00–U+FFFF (surrogates skipped) and a sample of each higher plane, `terminal_safe(utf8(cp)) == "?"` exactly when the code point is C0/DEL or `is_unsafe_code_point(cp)`. This pins the switch-over.

### T6 — Registration, exit row 7, root texts, `download` footer line, README, `batch.help` golden
- **Goal:** make `batch` reachable and finish the help surface, with every help golden blessed in one round.
- **Files:**
  - edit `src/app/cli.cpp`, `src/app/exit_help.hpp`, `src/app/cmd_download.cpp`, `tests/cli/cli_test.cpp`, `tests/cli/download_cli_test.cpp`, `README.md`;
  - create `tests/cli/batch_parse_test.cpp`, `tests/e2e/cases/batch.cmake` (the `help` case only).
- **Owns:** `src/app/cli.cpp`, `src/app/exit_help.hpp`, `src/app/cmd_download.cpp`, `tests/cli/cli_test.cpp`, `tests/cli/download_cli_test.cpp`, `tests/cli/batch_parse_test.cpp`, `tests/e2e/cases/batch.cmake`, `README.md`.
- **Build files for build-fixer:** none.
- **What changes:**
  - `cli.cpp`:
    - `commands.push_back(register_batch(app, inv.selected));` after `download`. The subcommand description is "Download every title listed in a CSV file" (`register_batch` sets it; T5 already wrote it).
    - `failure_text`: `unknown command '<x>' (commands: download, batch)` and `missing command; try 'mangadex-dl --help'`. These are literal strings next to the registration list; no CLI11 API is used to list subcommands.
  - `exit_help.hpp`: a row `  7   batch: titles failed in more than one way (see the per-title results)`, ≤ 80 columns, between 6 and 70. The draft wording goes to the cli-ux board.
  - `cmd_download.cpp`: `download_footer` gains `For many titles at once, see 'mangadex-dl batch --help'.\n\n` after the restart paragraph (Q2).
  - `README.md`: a `batch` section (CSV format, columns, precedence, examples, the restart note) and exit-table row 7. rev1 (fact-check): the section also says:
    - symlinks inside DIR are followed, so don't point DIR at a directory others can write to;
    - a row whose `dir` is unusable fails alone, while a DIR-wide problem (DIR itself unusable, disk full, read-only, I/O error) stops the batch;
    - the API load of a long list (up to 10 000 titles, several requests each) is the user's responsibility.
  - `batch.cmake` (T6 part): `cli_e2e_case(NAME help ARGS batch --help EXPECT_EXIT 0 EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)`.
- **Dependencies:** T5.
- **Done when:** the human has blessed `edge.help`, `download.help` and `batch.help`; then `nice pixi run check` is green and both reviewers approve.
- **Tests:**
  - **cli** (`batch_parse_test.cpp`, through `run_cli`):
    - `batch --help` contains the CSV footer and the exit row 7, and has no `--api-base`;
    - `batch - out` with `stdin_fd` → `-` reaches FILE (V-A1);
    - `batch f.csv out --json -q` → the globals apply (V-A2);
    - `batch f.csv --title X` → exit 2 with the root failure text and `Run 'mangadex-dl batch --help'` (V-A4);
    - `batch` with no FILE → 2, plus the hint;
    - `-j 9`, `--layout bogus`, `--group` with no value → 2;
    - `--fail-fast` parses;
    - a `download` smoke still works.
  - **cli** (edits): the root unknown/missing-command expectations in `cli_test.cpp:77` and `download_cli_test.cpp:578-600`.
  - **e2e:** `batch.help` (GOLDEN). The existing `edge.help` and `download.help` cases get re-blessed.

### T7 — E2E cases, edge-suite additions and committed CSV inputs
- **Goal:** the spec's `[e2e]` and `[edge]` criteria against the installed binary.
- **Files:**
  - edit `tests/e2e/cases/batch.cmake` (append), `tests/e2e/cases/_edge.cmake` (append a `batch-*` block; the `EDGE_*` knobs are unchanged), `tests/e2e/inputs/README` (document `batch/`), `.gitattributes` (`tests/e2e/inputs/batch/*.csv -text`);
  - create `tests/e2e/inputs/batch/*.csv`. Write the byte-exact ones with `printf` via Bash, not the Write tool: `excel.csv` (BOM + CRLF), `excel-bad.csv` (CRLF, unterminated quote starting on line 3) and `invalid-utf8.csv` (`0xFF`).
- **Owns:** `tests/e2e/cases/batch.cmake`, `tests/e2e/cases/_edge.cmake`, `tests/e2e/inputs/README`, `tests/e2e/inputs/batch/` (every file in it), `.gitattributes`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **Inputs:**
    - `clean.csv` (headerless 1111…);
    - `nochapters-mixed.csv` (5555…, 6666…);
    - `clean-unknown.csv` (1111…, 6666…6);
    - `incomplete.csv` (4444…);
    - `nochapters-fail-fast.csv` (5555…, 6666…);
    - `row-dir.csv` (`manga,dir` / `1111…,series/clean`);
    - `dir-escape.csv` (`../x`), `dir-absolute.csv` (`/abs`);
    - `excel.csv` (BOM, CRLF, `manga,title` / `1111…,"Excel, Title"`), `excel-bad.csv`;
    - `invalid-utf8.csv`;
    - `bulk.csv` (3333…);
    - `two-clean.csv` (1111…, 1111…: a duplicate);
    - `comments-only.csv`;
    - `four-clean.csv` (four distinct-looking rows for the network streak; any four UUIDs);
    - rev1 (fact-check): `row-dir-file.csv` (`manga,dir` / `1111…,README/x` / `1111…,`): row 1's `dir` runs into the committed regular file `inputs/README`;
    - rev1: `unknown-dir.csv` (`manga,dir` / `6666…,sub`).
  - **Configure-time guard in `batch.cmake`:** `file(READ "${CMAKE_CURRENT_LIST_DIR}/../inputs/batch/excel.csv" _h HEX LIMIT 64)` must start with `efbbbf` and contain `0d0a`, else `message(FATAL_ERROR …)` naming `.gitattributes`.
- **Dependencies:** T6.
- **Done when:**
  - the human has blessed `batch.dry-run` and `batch.json-dry-run`;
  - `nice pixi run check` is green;
  - `git check-attr text -- tests/e2e/inputs/batch/excel.csv` prints `unset`;
  - `nice pixi run release-test` is green (the new E2E cases on the hardened install);
  - both reviewers approve.
- **Tests:** see the Test plan, E2E and edge sections.

---

## Parallelization plan

**Batch 1 (parallel): T1, T4 · Batch 2 (parallel): T2, T3a, T3b · Batch 3: T5 · Batch 4: T6 (cli.cpp registration) · Batch 5: T7**

- **Batch 1:**
  - T1 owns every touched `include/mangadex_dl/*` header (a serialization point: only T1 in this batch).
  - T4 owns only `src/app/*` (not `cli.cpp`) and `tests/support/io_capture.hpp`.
  - The Owns sets are disjoint, and T4 uses no T1 symbol.
- **Batch 2:** three core tracks with disjoint Owns.
  - T2: `batch_csv.cpp`, `fs/bounded_read.cpp`, `fs/row_dir.cpp` (rev1).
  - T3a: `batch_plan.cpp`, `options.cpp`, `options_internal.hpp`.
  - T3b: `batch_run.cpp`, `batch_json.cpp`, `report.cpp`, `report_json.hpp`.
  - None touches a public header; the private core headers are owned by one track each.
  - Their tests are separate files and call only their own or integrated symbols. T3a/T3b build `TitleList`/`BatchJob` by hand, never through T2's parser.
  - The three tracks share `build/default/dev` under the lock. A half-written file in one can break another's build; that is a coordination event, not a fix.
- **Batch 3:** T5 alone. It needs all of batch 2 plus T4 at link and test time.
- **Batch 4:** T6 is the late registration task, the sole owner of `src/app/cli.cpp` and `exit_help.hpp`. It also takes the second edit of `cmd_download.cpp` (T4's edit landed in batch 1). It owns the first bless round.
- **Batch 5:** T7. It needs the registered, installed binary, and it appends to `batch.cmake`, which T6 created.

**Serialization points and who touches them:**

| Serialization point | Task |
|---|---|
| shared public headers `include/mangadex_dl/*` (incl. `exit_codes.hpp`, `error.hpp`, `options.hpp`) | T1 only |
| `src/app/cli.cpp` | T6 only |
| `tests/e2e/cases/_edge.cmake` (a shared file) | T7 only |
| top-level `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake`, `scripts/*`, `.github/workflows/*` | none |

Also shared but not on the list: `src/app/exit_help.hpp` (T6), `src/app/io.hpp` (T4), `tests/support/io_capture.hpp` (T4), `.gitattributes` (T7), and rev1 `src/app/cmd_download_output.cpp` (T4 in batch 1, then T5 in batch 3; never in two tracks of one batch). Each has exactly one owner per batch. rev1 adds `fs/row_dir.hpp` and `text_safety.hpp` to T1's public headers, still in batch 1 alone.

---

## Test plan (per tier)

### Unit (Catch2 against `mangadex_dl::core`)

| File | Task | Covers |
|---|---|---|
| `tests/unit/interfaces/headers_test.cpp` | T1 | exit 7, the `Errc` map, constants, abstract `TitleRunner`, designated-init smoke |
| `tests/unit/batch_csv/batch_csv_test.cpp` | T2 | AC1, AC2, AC3 (row/field limits, 20-error cap), echo cap, line numbers |
| `tests/unit/fs/bounded_read_test.cpp` | T2 | AC3 (file > cap, pipe ≤ cap+1 consumed), rev1: FIFO with and without a writer, `/proc/self/status`, directory, missing, bad fd, fd and flags untouched |
| `tests/unit/fs/row_dir_test.cpp` | T2 | rev1: the `classify_dir_errno` table, and the `check_row_dir` filesystem cases; nothing created |
| `tests/unit/text_safety/text_safety_test.cpp` | T1 | rev1: the unsafe set (U+009B, U+0085, U+202E, U+2028 …) and the decoder |
| `tests/unit/batch/batch_plan_test.cpp` | T3a | AC4 (rev1 rejects included), AC5, AC6, `validate_batch_args`, the `dir` accept/reject table, `group` control characters, message stripping |
| `tests/unit/batch/batch_run_test.cpp` | T3b | AC7 (fake runner; rev1 fatal vs row-scoped `failed`), AC10 at the unit tier (interrupt between rows), closing messages, `exists_notice`, `OutputRegistry` |
| `tests/unit/batch/batch_json_test.cpp` | T3b | AC8 (keys/types/nulls, `report` byte-equal, invalid UTF-8, no ESC, empty) |

Existing `unit.options*` and `unit.report*` must stay green unmodified: that proves the T3a and T3b refactors are behaviour-preserving.

### In-process CLI (`tests/cli/`)
- **`download_row_output_test.cpp` (T4):** `ConsoleObserver` with and without `RowContext`.
- **`batch_cli_test.cpp` (T5):** AC9, AC10 (before row 1 + a second run), AC11, AC12, AC13; rev1: row-scoped `dir` failure continues, global failure stops, nothing left behind for 4/5 rows, `group` never in a URL, one unsafe-code-point predicate; stdin via fd / closed / the TTY hint; JSON exits 0, 1, 7 and the empty list; dry-run; row dir; duplicate; the `/dev/full` stop; progress counter; color/ESC.
- **`batch_parse_test.cpp` (T6):** help content; V-A1, V-A2 and V-A4 behaviour through `run()`; usage errors including `--title`; a download smoke.
- **Edited (T6):** the root unknown/missing-command texts in `cli_test.cpp` and `download_cli_test.cpp`.
- **TSan:** T5 `nice pixi run tsan -R 'cli\.batch'`; the board runs `nice pixi run tsan -R 'cli\.(batch|download)'`.

### E2E cases — `tests/e2e/cases/batch.cmake` (tests `e2e.batch.<case>`)
Every case uses `--api-base file://{{FIXTURES}}/<tree>` unless noted, `out` as DIR, and exact `EXPECT_EXIT`.

| Case | Task | Asserts |
|---|---|---|
| `help` | T6 | exit 0, stderr empty, no ANSI, GOLDEN (AC14) |
| `headerless-clean` | T7 | `{{INPUTS}}/batch/clean.csv` → 0, stdout = the two clean paths, stderr empty, `EXPECT_FILES` + `EXPECT_EPUB` (AC15) |
| `stdin-clean` | T7 | `batch - out`, `STDIN batch/clean.csv` → identical stdout regex (AC15) |
| `mixed-exit-7` | T7 | nochapters tree, `nochapters-mixed.csv` → 7, stdout empty, stderr has row-prefixed exit-5 and exit-4 lines and the closing line (AC16) |
| `mixed-exit-7-json` | T7 | same + `--json` → 7, `EXPECT_JSON`, stdout regex `"no_chapters"` and `"not_found"` and `"total": 2` (AC16) |
| `uniform-not-found` | T7 | clean tree, `clean-unknown.csv` → 4, the clean files written (AC17) |
| `incomplete` | T7 | incomplete tree → 6, the failed file `EXPECT_ABSENT`, `EXPECT_NO_GLOB "out/.*.part-*"` (AC17) |
| `fail-fast` | T7 | `--fail-fast`, nochapters tree → 5, stderr names `--fail-fast` and `1 title not run` (AC18) |
| `dry-run` | T7 | `--dry-run` → 0, `EXPECT_ABSENT out`, GOLDEN (AC19) |
| `json-dry-run` | T7 | `--json --dry-run` → 0, `EXPECT_JSON`, `EXPECT_ABSENT out`, GOLDEN (AC19) |
| `row-dir` | T7 | → 0, `EXPECT_FILES "out/series/clean/Clean Title - Vol. 01.epub"` (AC20) |
| `row-dir-dry-run` | T7 | `--dry-run` → `EXPECT_ABSENT out` (AC19) |
| `dir-escape`, `dir-absolute` | T7 | → 2, stderr `…dir-escape\.csv:2: dir: `, stdout empty, `EXPECT_ABSENT out` (AC20) |
| `excel` | T7 | → 0, `EXPECT_FILES "out/Excel, Title - Vol. 01.epub"` (AC21) |
| `excel-bad` | T7 | → 2, stderr `excel-bad\.csv:3: ` (AC21) |
| `file-missing` | T7 | `{{WORK}}/nope.csv` → 1, `cannot read`, no hint (AC22) |
| `file-is-dir` | T7 | FILE `{{INPUTS}}/batch` → 1, `is a directory` (AC22) |
| `dir-is-file` | T7 | DIR `{{INPUTS}}/README` (global), a 2-row CSV → 1, `stopped at line 1:` … `1 title not run` (AC22) |
| `row-dir-is-file` | T7 | rev1 (fact-check): `--dry-run`, DIR `{{INPUTS}}`, `row-dir-file.csv` → 1. Row 2 still plans (stdout matches `Clean Title - Vol\. 01\.epub`). stderr `row-dir-file\.csv:2: ` and no `stopped at line` (AC22) |
| `not-found-leaves-no-dir` | T7 | rev1: clean tree, `unknown-dir.csv` → 4, `EXPECT_ABSENT out` (AC22) |
| `network-streak` | T7 | `--api-base http://127.0.0.1:1`, `four-clean.csv` → 3, `stopped at line 3: 3 titles in a row failed with network errors; 1 title not run`, `LABELS needs-libcurl`, `TIMEOUT 300` (AC23) |
| `usage-file-looks-like-id` | T7 | `batch 1111… out` → 2, stdout empty, hint (AC11) |
| `usage-title-option` | T7 | `batch f.csv --title X` → 2 |
| `duplicate` | T7 | `two-clean.csv` → 0, stderr `two-clean\.csv:2: same as line 1; skipped` |
| `comments-only` | T7 | → 0, stdout empty, stderr `lists no titles` |
| `tty-progress` | T7 | `TTY`, clean.csv → 0, merged output matches `\(1/1\) \[1/2\] Vol\. 1:` (AC30) |
| `tty-json-no-progress`, `tty-term-dumb` | T7 | `TTY` + `--json` / `TERM=dumb` → no `\(1/1\)` and `EXPECT_NO_ANSI` (AC30) |
| `tty-no-color-error` | T7 | `TTY ENV NO_COLOR=1`, nochapters-mixed → 7, `EXPECT_NO_ANSI` (AC30) |

### Edge-suite additions (`tests/e2e/cases/_edge.cmake`, T7 only; tests `e2e.edge.batch-*`)

| Case | Asserts |
|---|---|
| `batch-stdin-null` | `batch - out STDIN NULL` → 0, stdout empty, stderr `<stdin>: lists no titles` (AC24) |
| `batch-stdin-null-json` | + `--json` → 0, `EXPECT_JSON`, `"titles": \[\]` (AC24) |
| `batch-stdin-closed` | `STDIN CLOSED` → 1, `^mangadex-dl: error: cannot read <stdin>: `, never 70 (AC25) |
| `batch-dev-full` | `--dry-run` clean, `STDOUT_FILE /dev/full` → 1, `write error` (AC26) |
| `batch-sigpipe`, `batch-sigpipe-parent-ignores` | `bulk.csv --layout chapters --dry-run --api-base …/bulk`, `PIPE_HEAD 1` (± `PARENT_IGNORES_SIGPIPE`), stderr empty (AC27) |
| `batch-invalid-utf8-file-lc-c` | FILE `{{INVALID_UTF8}}`, `ENV LC_ALL=C LANG=C` → 1, stderr contains `?`, not 70 (AC28) |
| `batch-invalid-utf8-csv` | `invalid-utf8.csv` → 2, `:1: not valid UTF-8` (AC28) |
| `batch-invalid-utf8-dir-json` | `--json --dry-run`, DIR `{{INVALID_UTF8}}` → 0, `EXPECT_JSON` (AC28) |
| `batch-utf8-dir`, `batch-utf8-dir-lc-c` | DIR `Zoë`, `--dry-run` → stdout contains `Zoë/` under C.UTF-8 and C (AC28) |
| `batch-json-clicolor-force` | `--json --dry-run`, `CLICOLOR_FORCE=1` → `EXPECT_JSON EXPECT_NO_ANSI` (AC29) |
| `batch-pipe-stderr-no-ansi` | nochapters-mixed, piped → 7, `EXPECT_NO_ANSI` (both streams) (AC29) |

The `EDGE_BULK_ARGS`/`EDGE_JSON_ARGS`/`EDGE_ECHO_ARGS` knobs stay on `download`.

### Sanitizer-control additions
None. The feature adds no new risk class: no new threads, no C-string interop, no raw buffers beyond `read(2)` into a `std::string`. `string_view`-into-buffer dangling is already covered by the controls `dangling_view_heap` and `dangling_view_scope`, which must still fire. Evidence for `memory-safety-reviewer`: those controls fired in the T2/T5 gate runs, and the multi-row cases exercised the per-row DownloadServices.

### Release-test
- No release-surface change: no deps, no link change, NEEDED and the glibc floor are untouched.
- T7's Done-when and the board run `nice pixi run release-test`, so every new `e2e.batch.*`/`e2e.edge.batch-*` case runs on the hardened static-libstdc++ install.
- `network-streak` loads the host libcurl (label `needs-libcurl`; on a host without it, exclude it with `-E '^e2e\.batch\.network-streak$'`).
- The board also runs `nice pixi run -e gcc check` once (GCC release has not compiled the new TUs).

### Goldens that will change (human-blessed: `pixi run bless '<regex>'`)

| Golden | Why | Triggered by |
|---|---|---|
| `edge.help` | root `SUBCOMMANDS:` gains `batch`; the exit table gains row 7 | T6 |
| `download.help` | the exit table gains row 7; the Q2 footer line | T6 |
| `batch.help` (new) | the new `batch --help` | T6 |
| `batch.dry-run` (new) | planned paths for clean.csv | T7 |
| `batch.json-dry-run` (new) | the batch JSON document for a dry run | T7 |

Bless requests: T6 `'^e2e\.(edge|download|batch)\.help$'`; T7 `'^e2e\.batch\.(json-)?dry-run$'`.

**Must not change:** `edge.version`, `download.dry-run-clean`, `download.json-dry-run`. If any of these diffs in T1–T5, it is a regression, never a bless.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**Views into the input buffer and their owners**
1. **The CSV buffer.** `run_batch_command` owns the `std::string bytes` returned by `read_bounded_*` for the whole of phase 1.
   - `parse_title_list` may lex with `string_view`s into it, but every `ListRecord` field, every `ListErrors` message and the echo slices must be **owned copies** before it returns.
   - No `string_view` member anywhere in `batch_csv.hpp`/`batch.hpp`.
   - The `label` view (FILE as typed, or the literal `"<stdin>"`) is copied into messages. `BatchSettings::label` owns its copy.
2. **`read_bounded_*` arithmetic.**
   - `cap + 1` (precondition `cap < SIZE_MAX`);
   - `off_t st_size` vs `size_t` via `std::cmp_greater`;
   - `ssize_t` read results checked before any `static_cast<std::size_t>`;
   - the buffer is resized before `read(fd, buf.data() + got, n)` with `n ≤ cap + 1 - got`;
   - `read_bounded_fd` never closes or takes ownership of `io.stdin_fd`, and never changes its flags.
   - rev1 (fact-check): `O_NONBLOCK` is cleared only on the fd `read_bounded_file` opened itself. The `fcntl` results are checked.
   - rev1, fd-0 guard: no descriptor is opened before the stdin read in `run_batch_command`.
2b. **rev1 (fact-check) `check_row_dir`.** One `UniqueFd` per component, released before the next component is opened; `openat` parents are only ever borrowed from the live `UniqueFd`. The `sub` view is split into owned `std::string` components, because `openat` needs NUL-terminated names and `.data()` of a `string_view` must never be passed. Nothing is created.
2c. **rev1 `text_safety.hpp`.** `decode_utf8_at` reads `text[pos + i]` only after the length check; `pos < text.size()` is a precondition that every caller checks.

**CLI11 bindings**

3. **Bound variables.** `BatchCommand` owns `RawBatchArgs args_`. FILE binds `args_.file`. DIR and every shared option bind into `args_.defaults` through `bind_selection_options`/`bind_run_options(CLI::App&, RawDownloadArgs&)`.
   - These must receive the **member**, never a local or a copy.
   - `--group` binds the vector; `--fail-fast` binds a bool.
   - The `Command` lives in `run()`'s `commands` vector, declared before the `CLI::App`, so it outlives `parse()` and dispatch.
   - `run_batch_command` takes `const RawBatchArgs&` and copies what it keeps into `BatchSettings`.
4. **Callbacks.**
   - `register_batch`'s selecting callback captures `[slot = &selected, self = command.get()]` by value.
   - `download`'s `--title` `add_option_function` lambda keeps capturing `&args_.title` after T4's move, with the pointer into the Command, never into a helper's frame.
   - No lambda in `bind_*` may capture by reference.

**Per-row objects and the shared Backend**

5. **Backend shared across rows; DownloadServices fresh per row.**
   - Declaration order in `run_batch_command`: `handlers` → `agent` → `backend` → `registry` → `runner`, then `run_batch`. The runner holds pointers to the backend, agent, io, policy, registry and label, all declared before it.
   - Each row's `DownloadServices` holds raw `transport`/`clock` pointers into the Backend, a fresh `make_jitter()` (an engine captured by value), a default `stop_source` and a copy of the agent. All of it lives only inside `run_title`.
   - Confirm `run_download` joins the reporter thread, the FetchPool and the InterruptBridge before it returns, so nothing from row k runs during row k+1.
   - Confirm the curl transport's handle pool is used sequentially across rows (TSan run).
   - `make_backend(const ApiBase&)`: `fixture_root()` is a reference into `settings.base.api_base`. `make_fixture_transport` copies the path by value, but `settings` still outlives the backend.
   - `DownloadServices` is filled member by member (the analyzer false positive).
6. **ConsoleObserver with a prefix.** `RowContext` is taken by value and stored as an owned `row_` (`std::string prefix`). It must **not** borrow `"<label>:<line>: "` built in a temporary. The observer keeps holding `Io*`, and the `Io` outlives it. T4 drops `noexcept` from the constructor because it now copies a string.

**Containers and data flow between core and app**

7. **`run_batch(std::span<const BatchJob>)`.**
   - The jobs vector must not be mutated while the span is live.
   - `TitleResult` copies `line`, `manga_id` (`std::string{job.options.manga.str()}`, never a stored `str()` view) and `dir`.
   - `RowOutcome::report` is moved into `TitleResult`.
   - The `TitleRunner` reference is borrowed only for the call.
8. **Dedupe.** `plan_batch` groups by `std::map<MangaId, std::vector<std::size_t>>`: indices, never iterators or pointers into the `std::vector<BatchJob>` it is still growing.
9. **`OutputRegistry`.** It owns its `std::string` keys (`claim` takes the path by value and stores the `lexically_normal()` generic string, rev1). There is no `string_view` key into an `EpubOutcome::path` of a `RunReport` that is about to be destroyed or moved.
10. **JSON.** `report_json` returns by value and is moved into the batch document. `batch_to_json` builds and dumps locally; nothing escapes but the string.

**Process-wide state**

11. **Interrupt state.** One `ScopedInterruptHandlers` for the whole batch, plus a per-row `InterruptBridge` inside `run_download`. The global flag is read before each row. Tests that simulate it reset it with RAII. The in-process second-run test proves the one-live-install flag is released.
12. **New threads:** none in batch code itself. Every thread comes from `run_download` and is per row.

---

## Rollout & risk

**CLI contract**
- `download`'s flags, exit codes, stdout and `--json` are unchanged. The byte-stable goldens and the unmodified `cli.download*` suite guard this.
- Changed text on stderr only:
  - root `unknown command '<x>' (commands: download, batch)`;
  - `missing command; try 'mangadex-dl --help'`.
- Changed help text (not a script contract): `download --help` gains row 7 and one footer line; the root help lists `batch`.
- New exit code 7. Only `batch` returns it. It is declared in `exit_codes.hpp` before any code uses it (T1).
- No migration and no state file.

**Riskiest task: T4** (refactoring a shipped command). De-risked by:
- moving code verbatim into `cmd_download_run.cpp`/`cmd_download_options.cpp` in the same add order;
- forbidding any golden change or edit of existing download tests in T4;
- running both env gates;
- `memory-safety-reviewer` checking the moved lambda and binding lifetimes.

**Second risk: T5** (lifetime across rows plus interrupt handling). De-risked by:
- the TSan run;
- the second-run-after-interrupt test;
- the fake-runner unit coverage of every stop path in T3b, before T5 starts.

**Third risk: T2** (an untrusted-input parser).
- Table-driven accept/reject tests, hard caps and no stored views.
- At `/harden`, consider a libFuzzer target for `parse_title_list` (fuzz is off in Project specifics, so this is a follow-up, not this plan).

**rev1 (fact-check) behaviour change for the plan gate.** A row-scoped `dir` failure is now `failed` + continue instead of stop.
- **Where it lives:** the classification is a read-only pre-flight (`check_row_dir`) run before each row.
- **Conservative fallback:** anything `run_download` reports later is treated as global (fatal).
- **Risk:** a filesystem state that changes between the pre-flight and the download. It only ever makes the batch stop sooner, never write outside DIR.
- **Aggregation:** `failed` counts as 1 in the uniform/mixed rule.

**Smaller risks**
- **Rate limiting across rows:** limiters reset per `ApiClient`, so each row boundary may send one API request up to 250 ms early and one at-home request up to 1.5 s early. The [human] AC31 live batch watches for 429s. If it bites, a follow-up shares the limiters in `Backend`; that is a core API change.
- **Security board items:** OD1 (symlinks inside DIR followed); FILE may be a FIFO (blocks until a writer, which is the user's choice); every CSV byte reaches the terminal only through `terminal_safe`; JSON uses the `replace` handler; size caps.
- **Draft wording for the cli-ux board:** the exit row 7, the Q2 footer line, the four `stopped at line` reasons, the stdin hint and the cell-message stripping.

---

## Refactor triggers

rev1 (fact-check): no [VERIFY] item was refuted, so none of these triggers fired. The table is kept for reference.

| [VERIFY] | If refuted, these tasks change |
|---|---|
| V-A1 (lone `-` → FILE) | The surface contract changes: back to `cli-designer`. T5's `bind()` and T6's tests: e.g. accept `/dev/stdin`, or pre-check argv for a bare `-` before parse. The README and help text (T6). |
| V-A2 (globals after positionals) | T6: tests/README/help examples put the globals before `batch`. If the designer insists on the spec's wording, the spec changes. No core change. |
| V-A3 (shared binders, no ConstructionError, add order) | T4: drop the shared helpers and duplicate the `add_option` calls per subcommand, with the help strings hoisted into shared `constexpr std::string_view`s so they can't drift. T5's `bind()` follows. |
| V-A4 (`--title` on batch → ParseError) | T5: add a hidden `--title` on `batch` that fails with exit 2 `--title is a per-row column in FILE`. T6: test it. |
| V-B1 (closed fd 0 → EBADF; slot 0 stays free) | T4: `process_io` records `stdin_fd = -1` when `fcntl(0, F_GETFD)` fails at startup. T2: `read_bounded_fd` unchanged. T7: the `batch-stdin-closed` expectation stays. |
| V-B2 (directory open/fstat, `st_size`) | T2: reject directories via `open(O_DIRECTORY)` probing or a read-time `EISDIR` instead; adjust its tests. |
| V-C1 (nested order + replace handler) | T3b: build the report subtree from the dumped `report_to_json` string (reparse with `ordered_json::parse`, a trusted self-produced document). The spec's "never dump unvalidated JSON" rule is unaffected. |
| V-C2 (byte-equal technique) | T3b's tests only: compare `report` semantically (`ordered_json ==`) plus key order. The AC8 wording goes back to the designer for sign-off. |
| OD1 (lexical containment + path-based open) | **Re-plan:** a new core task adds an `openat(O_NOFOLLOW \| O_DIRECTORY)` walk to `fs/output_dir` (a frozen public header → a T1-style interface task); T3a's `validate_list_dir` is unchanged; `run_download`'s DIR handling or `DownloadOptions` gains a pre-opened dir fd (a public header). That pushes T5 one batch later. |
