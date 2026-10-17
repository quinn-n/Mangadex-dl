---
name: cpp-testing
description: Testing tiers for this C++ CLI - Catch2 v3 unit and in-process CLI tests, the installed-binary E2E driver (cli_e2e_case + the cmake -P run_case.cmake driver), the mandatory edge-case suite (stdin, /dev/full, SIGPIPE, TTY/color, UTF-8, --json), human-blessed goldens, sanitizer positive controls, release checks, coverage and opt-in fuzzing. Use when writing, reviewing, running or debugging tests, adding an E2E case, requesting a golden bless, or deciding which tier proves a behaviour.
---

# Testing tiers (C++ CLI)

Every behaviour is proven at the **lowest tier that can prove it**, and the CLI contract is always proven against the **installed binary**. The gate (`nice pixi run check`, see `cpp-quality`) runs tiers 1–4 under ASan+UBSan in the dev preset. Tier 5 (release) runs in CI and `/harden`.

| Tier | What | Where | CTest name / label |
|---|---|---|---|
| 1 Unit | Catch2 against `@PROJ@::core` | `tests/unit/<module>/*_test.cpp` | `unit.<test>` / `unit` |
| 2 In-process CLI | Catch2 through `run(args, Io&)` | `tests/cli/*_test.cpp` | `cli.<test>` / `cli` |
| 3 Installed-binary E2E | `cli_e2e_case()` → `cmake -P run_case.cmake` | `tests/e2e/cases/<sub>.cmake` | `e2e.<sub>.<case>` / `e2e` (+`golden`) |
| 3 Edge suite | mandatory contract edge cases | `tests/e2e/cases/_edge.cmake` | `e2e.edge.<case>` / `e2e;edge` |
| 4 Sanitized run + controls | all of the above under ASan+UBSan, plus positive controls | `tests/sanitizer-controls/` | `sanitizer-control.<n>` / `sanitizer-control` |
| 5 Release | unit + E2E + binary/staging gates on the hardened release install | same tests, release preset | `release-check.binary`, `release-check.staged-files` / `release-check` |

The installer fixture is `e2e.install` (`FIXTURES_SETUP e2e_installed`). It installs the `runtime` and then the `docs` component into `build/<env>/<preset>/e2e-prefix/` (two calls: multi-component `--install` is CMake 4.4+ only). CTest adds it automatically whenever you filter with `-R` or `-L`.

## Running tests
- `nice pixi run test` is the whole dev tier (configure → build → ctest). Narrow it with `nice pixi run test -R '<regex>'`, `-L <label>` or `-E '<regex>'`; `scripts/preset.sh` forwards only these filters (plus `--rerun-failed`, `--output-on-failure`, `-V`).
- These runs are long. Pass an explicit long Bash `timeout` (up to 600000 ms) or use `run_in_background` and wait. A run the tool killed is **not** a test failure. Re-run it; don't diagnose it.
- Release tier: `nice pixi run release-test`. gcc env: `nice pixi run -e gcc test`.
- Low priority is invocation-level only (`nice …`), never inside tasks, CMake or CI. See `low-priority-execution`.

## Catch2 v3 rules
- Link `Catch2::Catch2WithMain`. Include per-subsystem headers (`<catch2/catch_test_macros.hpp>`, `<catch2/matchers/…>`), **never** `catch_all.hpp`.
- Discovery: `catch_discover_tests(<t> TEST_PREFIX "unit." DISCOVERY_MODE PRE_TEST EXTRA_ARGS --warn=NoAssertions ADD_TAGS_AS_LABELS)`.
  - **`DISCOVERY_MODE PRE_TEST`**, written explicitly in the call, so builds without the presets (plain cmake, an AUR `check()`) get it too. The presets also set `CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE`. In POST_BUILD mode a sanitizer crash while listing tests would surface as a *build* failure and get misrouted to `build-fixer`.
  - `--warn=NoAssertions` fails a test that asserts nothing. Keep it ONE token (`=` form): Catch2 3.16.1's CatchAddTests.cmake quotes a multi-element EXTRA_ARGS list into the single argv token `--warn;NoAssertions` (upstream commit 6534dbc3ef).
  - The tier label comes from the **directory `LABELS` property** (`tests/unit/`, `tests/cli/`). `ADD_TAGS_AS_LABELS` overwrites any `PROPERTIES LABELS` argument, but it merges with directory labels.
  - Sanitizer runtime options reach Catch2 binaries **compiled in** (`@PROJ@_add_sanitizer_options(<test target>)`), not through `PROPERTIES ENVIRONMENT`. A `;`-separated list passed through `catch_discover_tests(PROPERTIES …)` gets split into separate properties.
- **Test names never start with `-`.** CTest passes the name as a test spec, and a leading `--` is parsed as an option.
- Order is random by default. Record `--rng-seed <n>` from the log in every bug report, and reproduce with it.
- Conda's Catch2 is shared-only (`libCatch2.so`). That is fine: the dev preset's build RPATH points into the env.
- Fakes behind seams (an injected interface or a function parameter) beat mocking frameworks. trompeloeil is opt-in. Use GoogleTest only if you need gmock, death tests or FuzzTest.

## Tier 1: unit (`@PROJ@::core`)
- All logic lives in the core library, so it is unit-testable without CLI11 or a process.
- One test file per module, under `tests/unit/<module>/`. The sources are globbed (`CONFIGURE_DEPENDS`), so adding one needs no CMake edit.
- Test `Result<T>` both ways: the value path and every `Error` path. Never dereference an `expected` in a test without checking it first.

## Tier 2: in-process CLI (`run(args, Io&)`)
- The seam: `@PROJ@::app::run(std::span<const char* const>, Io&)`, with `Io` holding the out/err streams, TTY flags and an `EnvSnapshot`. `tests/support/io_capture.hpp` → `run_cli({"--json", "x"}, Tty{.stdout_tty = true}, env)` returns `{exit_code, out, err}`, with `std::ostringstream` sinks.
- Use it for help/version text, validation and usage errors (exit 2, empty stdout), exit-code mapping, the `--json` shape, and color decisions with injected TTY flags and env.
- **Every `run()` constructs the whole CLI11 app**, so a `CLI::ConstructionError` (duplicate names, bad wiring, `fallthrough()` set after subcommands) fails here first.
- Write errors in-process: give `Io.out` an `std::ofstream("/dev/full")` and assert the exit code and the diagnostic. The authoritative write-error test is the E2E `/dev/full` case, because buffering defers the error to exit.
- The test target compiles `@PROJ@_app_sources` (everything except `main.cpp`), so a new `cmd_<sub>.cpp` needs no CMake edit.

## Tier 3: installed-binary E2E
`tests/e2e/CliE2E.cmake` defines `cli_e2e_case()`. Each case is a `cmake -P tests/e2e/run_case.cmake` run against `build/<env>/<preset>/e2e-prefix/bin/@TOOL@`.

```
cli_e2e_case(NAME <case> ARGS <arg>...
             [STDIN <file>|NULL|CLOSED]                          default NULL (/dev/null); never ctest's stdin
             [EXPECT_EXIT <n>]                                    default 0; exact
             [EXPECT_STDOUT_REGEX <re> | EXPECT_STDOUT_EMPTY]
             [EXPECT_STDERR_REGEX <re> | EXPECT_STDERR_EMPTY]
             [EXPECT_JSON | EXPECT_JSON_LINES]                    exactly one document / one per line, valid UTF-8
             [EXPECT_NO_ANSI | EXPECT_ANSI]
             [GOLDEN]                                             golden/<suite>.<case>.{stdout,stderr}
             [ENV <NAME=value | -NAME>...]                        set / unset on top of the hermetic env
             [TTY]                                                pseudo-terminal via util-linux script; 77 = skipped if absent
             [STDOUT_FILE /dev/full]
             [PIPE_HEAD <n> [EXPECT_PIPE_RESULT <SIGPIPE|n>] [PARENT_IGNORES_SIGPIPE]]
             [TIMEOUT <s>]                                        default 60 (generous: ASan + nice)
             [LABELS <label>...])
```

- **One file per subcommand:** `tests/e2e/cases/<sub>.cmake` produces tests `e2e.<sub>.<case>`. Files are globbed, so a track that adds a subcommand adds its own file and never touches shared CMake.
- **Placeholders** in ARGS, ENV values and STDIN: `{{INPUTS}}` (`tests/e2e/inputs/`), `{{WORK}}` (the per-case temp dir, also `$HOME`), `{{INVALID_UTF8}}` (bytes `0xFF 0xFE`).
- **Limits:** an ARGS item can't be empty or contain `;`, because CMake list semantics drop or split it. Under `TTY`, stdout and stderr share the terminal: assert on the merged stdout, and `EXPECT_STDERR_*` is rejected.
- **Never** `PASS_REGULAR_EXPRESSION` or `WILL_FAIL` for a contract case. The first ignores the exit code, and the second only inverts it.

### `run_case.cmake` rules (keep them if you touch the driver; it is a serialization point)
1. **Hermetic env via `set(ENV{})`/`unset(ENV{})`.** Never `execute_process(ENVIRONMENT)`, which is CMake 4.4+ only.
   - Fresh `HOME`, `XDG_{CONFIG,CACHE,DATA,STATE}_HOME` and `TMPDIR` under `build/<env>/<preset>/e2e-work/<suite>.<case>/`.
   - `LC_ALL=LANG=C.UTF-8` and `TERM=xterm-256color` unless the case overrides them.
   - **Removed:** `CONDA_*`, `PIXI_*`, `CMAKE_*`, `CTEST_*`, `LC_*`, `LANG*`, `XDG_*`, `CLI_E2E_*`, the tool's own `@PROJ_UPPER@_*`, and `NO_COLOR`/`CLICOLOR*`/`COLORTERM`/`FORCE_COLOR`/`TERM`/`COLUMNS`/`LINES`/`PAGER`/`EDITOR`/`VISUAL`.
   - **Kept:** `*SAN_OPTIONS`, `ASAN_SYMBOLIZER_PATH`, `LLVM_PROFILE_FILE` and `PATH` (which finds `llvm-symbolizer`).
2. **Exact exit code**; stdout and stderr asserted **separately**. Failure messages explain the special codes: 86 = sanitizer finding, 70 = internal error, 134/139/signal strings = crash, 100–114 = a leaked raw CLI11 code.
3. **Sanitizer markers fail the case whatever the exit code.** The regex, identical everywhere in this studio:
   ```
   ERROR: (Address|Leak|Memory)Sanitizer|WARNING: ThreadSanitizer|runtime error:|AddressSanitizer:DEADLYSIGNAL
   ```
   It is applied to stderr, and to the merged output under `TTY`. GCC's UBSan prints no `SUMMARY:` line, so the regex keys on `runtime error:`.
4. **Signal deaths are strings.** `execute_process` reports them per command in `RESULTS_VARIABLE` (`SIGPIPE`, `Segmentation fault`, `Subprocess aborted`, `Process terminated due to timeout`). A `sh -c` wrapper always `exec`s, so it never masks the tool's status.
5. **JSON:** the driver wraps stdout as `[<stdout>]` and requires `string(JSON … LENGTH) == 1`. That rejects empty output, trailing junk and concatenated documents. CMake's parser doesn't check UTF-8, so the driver also runs `iconv -f UTF-8 -t UTF-8` when iconv is present.
6. **`CLI_E2E_TOOL`** (env) overrides the binary under test: a release install, an extracted package, `/usr/bin/@TOOL@` after an AUR install.
7. **TTY** runs `script -qec "<quoted cmd>" /dev/null </dev/null` (util-linux; `\r` stripped). If it is absent, the case exits **77**, which CTest reports as *Skipped*, never as passed.
8. **SIGPIPE** runs `<tool> | head -n N` as a two-command `execute_process`, so both results land in `RESULTS_VARIABLE`.
   - The writer must die with `SIGPIPE` under policy A (see `cli-design`), and `head` must exit 0.
   - The payload must be **well over 64 KiB and newline-separated**. Otherwise the pipe never fills, or `head -n 1` reads everything, and the case proves nothing.
   - `PARENT_IGNORES_SIGPIPE` runs the tool via `sh -c 'trap "" PIPE` + newline + `exec "$0" "$@"'`, so the tool inherits an ignored SIGPIPE and must restore `SIG_DFL` itself.
   - CTest and `cmake -P` already hand children `SIG_DFL`. The `trap` reproduces an ignoring parent.
9. **Goldens are compared byte for byte** after normalizing `{{WORK}}`, `{{INPUTS}}` and `{{TOOL}}` paths. They are written **only** when `UPDATE_GOLDEN=1` (only `scripts/bless.sh` sets it), and only if every other assertion of the case passed.

## The mandatory edge suite (`tests/e2e/cases/_edge.cmake`)
Every CLI built with this studio passes these against its installed binary. Removing one is a contract change and needs `cli-ux-reviewer` + `quality-reviewer` sign-off.

| Case(s) | Asserts |
|---|---|
| `help`, `version` (GOLDEN) | stdout, exit 0, stderr empty, no ANSI when piped; version line `@TOOL@ X.Y.Z` |
| `usage-unknown-option`, `usage-invalid-utf8`, `usage-invalid-utf8-lc-c` | exit 2, empty stdout, a diagnostic on stderr (never 100–114) |
| `stdin-null`, `stdin-closed` | runs with stdin `/dev/null` and with fd 0 closed (`exec … <&-`) |
| `dev-full-version`, `dev-full-help` | stdout on `/dev/full` → exit 1 + `write error` (never a silent 0) |
| `sigpipe`, `sigpipe-parent-ignores` | writer killed by `SIGPIPE`, exactly one line out, stderr empty |
| `tty-help`, `tty-help-no-color`, `tty-help-term-dumb` | pseudo-TTY; `NO_COLOR` and `TERM=dumb` suppress ANSI |
| `tty-color*`, `pipe-clicolor-force`, `no-color-beats-force`, `pipe-no-ansi` | only when the tool colors stdout (`EDGE_COLOR_ARGS`); precedence `--color` > `NO_COLOR` > `CLICOLOR_FORCE` > `TERM=dumb` > isatty |
| `json-valid`, `json-clicolor-force`, `json-tty` | one valid JSON document, never ANSI, even with color forced or on a TTY |
| `utf8-echo`, `utf8-echo-lc-c`, `invalid-utf8-echo`, `invalid-utf8-json` | UTF-8 round trip under `C.UTF-8` and `C`; invalid-UTF-8 argv never crashes; `--json` stays valid |

The `EDGE_*` knobs at the top of `_edge.cmake` point the subcommand-dependent cases at real invocations. Keep them in step with the CLI surface.
- `EDGE_BULK_ARGS` is **required**.
- `EDGE_JSON_ARGS` / `EDGE_ECHO_ARGS` / `EDGE_COLOR_ARGS` may be empty when the tool has no such surface. Say so in the design doc.

Never 70, 134 or 139 anywhere: the exact `EXPECT_EXIT` catches them.

## Goldens
- Files: `tests/e2e/golden/<suite>.<case>.stdout` (always) and `.stderr` (only when non-empty; a missing `.stderr` golden means "stderr must be empty").
- **Human-blessed only.** The `bless` task is in `permissions.ask`. The `guard-bash` hook blocks any inline `UPDATE_GOLDEN`, and `protect-files` blocks Edit/Write on existing goldens.
  - An agent that needs a golden changed **requests a bless**: the case regex plus the reason (an intended `--help` change, a new GOLDEN case).
  - The orchestrator shows the diff, and the human runs `pixi run bless '<regex>'` (`/setup` runs it bare for the initial goldens).
- A missing golden fails with `golden missing ... request a bless`. The driver never auto-creates one.
- Never bless to make a red test green. A golden diff you didn't intend is a bug.

## Tier 4: sanitizers and positive controls
- The dev preset is ASan+UBSan for every tier (option C, see `memory-safety`). Any finding is a failing test: exit 86 plus the marker regex.
- `tests/sanitizer-controls/` holds deliberately buggy programs, registered **only when sanitizers are on**. Each runs through `run_control.cmake`, which asserts nonzero (exactly 86 where given) **and** its marker:

  | Control | Marker |
  |---|---|
  | `heap-overflow` | `ERROR: AddressSanitizer: heap-buffer-overflow` |
  | `heap-overflow-defaults` (env options unset, so the compiled-in defaults alone must give exit 86) | `ERROR: AddressSanitizer: heap-buffer-overflow` |
  | `leak` (built at `-O0`) | `ERROR: LeakSanitizer: detected memory leaks` |
  | `leak-defaults` (same binary, env options unset) | `ERROR: LeakSanitizer: detected memory leaks` |
  | `catch2-segv` (null deref inside a `TEST_CASE`; `handle_segv=2` keeps ASan's report accurate) | `ERROR: AddressSanitizer: SEGV` |
  | `signed-overflow` | `runtime error: signed integer overflow` |
  | `signed-overflow-defaults` (same binary, env options unset; GCC's UBSan runtime reads only its own options) | `runtime error: signed integer overflow` |
  | `data-race` (tsan preset only) | `WARNING: ThreadSanitizer: data race` |

- **If a control does not fire, "sanitizer-clean" is not licensed** for that build. That is a `build-fixer` matter, not something to skip.
- The controls are excluded from lint by their own `.clang-tidy`.
- A new risk class gets a new control.

## Tier 5: release tests
- `nice pixi run release-test` runs the same unit + E2E + edge suite against the **hardened release install**, plus:
  - `release-check.binary`: `scripts/release-check.sh <installed-binary> <glibc-floor>`. It checks PIE, RELRO, BIND_NOW and NX markers, no RPATH/RUNPATH in the default env, the NEEDED allowlist and the glibc floor.
  - `release-check.staged-files`: a component-less `cmake --install` into a scratch prefix (what CPack ships). It fails on any file outside the allowlist: the binary, its man page, completions and doc files. `CPACK_COMPONENTS_ALL` does not filter a monolithic TGZ, so this test is the guard. Extend it deliberately via `@PROJ@_STAGED_FILES_EXTRA_REGEX`.
- These run in CI and `/harden`. Optimizer-only bugs (UB that only shows at `-O2`) surface here, not in the `-O1` sanitized run.

## Coverage (`nice pixi run cov`, report-only)
- Unsanitized clang `cov` preset (`-fprofile-instr-generate -fcoverage-mapping`), with `LLVM_PROFILE_FILE=…/%p-%m.profraw`, then `llvm-profdata merge` + `llvm-cov report` / `export -format=lcov`.
- The LLVM tools must match the clang major (same pixi env).
- It never gates. Use it to find unexercised risky paths for `memory-safety-reviewer` and `test-engineer`.

## Fuzzing (opt-in, default off)
- libFuzzer targets behind `@PROJ_UPPER@_FUZZ` (clang only). Each target also builds as a corpus-replay executable registered as `fuzz-regression.<t>` (`-runs=0`), so the gcc env and CI exercise it too.
- Time-boxed fuzzing runs locally at low priority and **never in CI**.

## Alternative: pytest for E2E (documented, not default)
pytest in the pixi env is more expressive and mirrors the Python studio. It costs a Python dependency in the env and breaks AUR `check()` parity, which the `cmake -P` driver keeps (CMake is already a build dependency). Switch only by an explicit design decision, and keep the same rules: exact exit codes, separate streams, the marker regex, hermetic env, human-blessed goldens.

## When not to
- Don't E2E-test pure logic. That belongs in unit tests.
- Don't unit-test CLI11 wiring. That belongs in tier 2.
- Don't add a GOLDEN for output containing timestamps, PIDs, absolute paths outside `{{WORK}}`/`{{INPUTS}}`, or hash-ordered data. Use a regex, or make the output deterministic.
- Don't loosen an assertion (a regex instead of an exact exit code, or an EMPTY dropped) to get green. Fix the code, or record a design decision.

## Sibling skills
- `cli-design`: the contract these tests assert.
- `memory-safety`: sanitizer options, controls and the evidence standard.
- `platform-testing`: tiers per target.
- `cpp-quality`: the gate and definition of done.
- `build-workflow`: presets, `preset.sh` and the install components.
- `low-priority-execution`: `nice` and timeouts.
