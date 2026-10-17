---
name: cpp-quality
description: The quality gate and standards for this C++23 project. Covers the pixi task vocabulary, the `check` chain (fmt-check → sanitized dev test → clang-tidy lint → -Werror release build), the clang-format and clang-tidy ownership and config, the per-compiler warnings policy, -Werror scope, the NOLINT policy, the shared build-dir lock, and the definition of done. Use when writing, reviewing, refactoring or testing C++, when running or reasoning about the gate, when editing .clang-format/.clang-tidy/ProjectWarnings.cmake/scripts/format.sh/scripts/lint.sh, or when deciding whether work is done.
---

# C++ quality: the gate and the standards

This is the bar every change clears. Each kind of finding has one owner:

| Owner | Owns | Never |
|---|---|---|
| **clang-format** | Layout, braces (`InsertBraces`), include order and grouping | Hand-format, or argue with its output |
| **Compiler warnings** | Conversions, shadowing, overrides, format strings, and per-compiler extras | Silence globally or `-Wno-error` broadly |
| **clang-tidy** | Bug patterns, modernization, performance, readability, include hygiene, the static analyzer | Duplicate formatter rules (the brace checks are off) |
| **Sanitizers + tests** | Runtime UB, memory errors, leaks, and the CLI contract | See `memory-safety` and `cpp-testing` |
| **Reviewers** | Judgment: `[[nodiscard]]` on factories and `Result` functions, design, naming | Re-litigate anything the tools above already decide |

Placeholders: `<tool>`/`<proj>`/`<PROJ>` mean the values in CLAUDE.md "Project specifics". The files themselves come from `scaffold.md` next to this skill; `/setup` writes them.

## Task vocabulary

Agents type only these pixi tasks. They never type a raw `cmake`/`ctest`/`clang-tidy` pipeline as a substitute. Prefix each heavy one with `nice` (see `low-priority-execution`).

| Task | What it does |
|---|---|
| `fmt` / `fmt-check` | `scripts/format.sh --fix` / `--check`: clang-format on tracked and new C++ files. Check mode validates the config first. |
| `configure` / `build` / `test` | Dev preset (Debug `-O1 -g`, ASan+UBSan, `-Werror`) via `scripts/preset.sh`. `test` runs unit + in-process CLI + E2E against the installed binary + sanitizer controls. Filters: `pixi run test -R <regex>` / `-L <label>` / `-E <regex>`. |
| `lint` | `scripts/lint.sh`: `run-clang-tidy` over own TUs from `build/default/dev`. Depends on `build`. |
| `lint-fix` | Apply clang-tidy fix-its, then `fmt`. Manual only; review the diff. |
| `lint-changed` | `clang-tidy-diff` on lines changed since the merge-base. A fast local signal, not the gate. It runs only changed `.cpp/.cc/.cxx` TUs, so a header-only diff is ignored (and untracked files are not in the diff). |
| `release` / `release-test` | Release preset (`-O2`, hardened, static libstdc++, `-Werror`): build / build + tests + release-check |
| **`check`** | **The gate.** See below. |
| `cxx-probe`, `bless`, `cov`, `audit`, `package` | See `platform-testing`, `cpp-testing`, `build-workflow` and `cpp-security`. `bless` always goes to the human. |

There is **no `typecheck` slot**: the compile is the type check.

## The gate: `check`

```
check = fmt-check → test (dev: configure → build → test, ASan+UBSan, -Werror) → lint → release (configure → build, -Werror)
```

- **Ordered and fail-fast.** pixi `depends-on` runs in list order and stops at the first failure. Whether pixi runs `build` once or twice when both `test` and `lint` depend on it is verified at `/setup`; either way it is correct, because the second build is a no-op.
- **Two environments, same chain.**
  - `nice pixi run check` uses the `default` env (Clang).
  - `nice pixi run -e gcc check` uses the `gcc` env (GCC). There `lint` deliberately skips with a printed reason, because clang-tidy needs the Clang compile DB. That is the only allowed skip, and it prints its reason.
  - CI runs both envs on every PR, so a red GCC job is a red gate.
  - Run `-e gcc check` locally whenever a change touches CMake, presets, warning flags or `pixi.toml`, or is compiler-sensitive (templates, `constexpr`, `<print>`/`<expected>` use), and whenever the plan or `build-fixer` asks for it.
- **What `check` leaves out:** release *tests* (`nice pixi run release-test`) run in `/harden`, in CI, and when the plan touches the release surface.
- **Anything that can't run is red.** A missing compile DB, a missing tool or a lint that finds 0 files are failures, never skips.
- **Long runs and the Bash tool.** A cold `check` routinely exceeds the Bash tool's 120 s default.
  - Pass an explicit long `timeout` (up to `600000` ms), or start it with `run_in_background` and wait for it to finish.
  - **A run killed by a timeout is not a test failure.** Re-run it with a longer timeout before you diagnose anything.
  - A killed run can leave `build/<env>/<preset>.lock.d/` behind. The next run takes it over automatically (see "Build-dir lock"). Don't delete it by hand while another track may be building.

## Definition of done

A change is done only when **all** of these hold:
1. `nice pixi run check` is green in this worktree, and so is `nice pixi run -e gcc check` when the rules above call for it.
2. The acceptance criteria pass in the tests **and** on the real installed binary (`cli-tester`).
3. Both track reviewers return `VERDICT: APPROVED` (`quality-reviewer`, `memory-safety-reviewer`), and the feature board has no open Must-fix.
4. No new `NOLINT` without a reason, no new sanitizer suppression without a reason and issue link, and no weakened test or golden. Goldens change only via a human-approved `pixi run bless <regex>`.
5. New dependencies came in through `pixi add`. `pixi.lock` is committed and was never hand-edited.

## clang-format

- **One pin source:** `pixi.toml` pins `clang-format` to the **same LLVM major** as the env's clang, in both envs. Never use the host's clang-format: `format.sh` refuses a binary outside `$CONDA_PREFIX`, and the hook calls the env's binary directly. Output drifts across majors:
  - 22 made `AlignAfterOpenBracket` a bool;
  - 23 added `AllowShortRecordOnASingleLine` and deprecated `BinPackParameters`/`BinPackArguments`.

  After a major bump, run `fmt` once in a dedicated commit.
- **Invocation:** always `--style=file --fallback-style=none`. Check mode is `--dry-run --Werror` (exits 1 on any diff). Config validity is checked with `--dump-config`, where an unknown key exits 1.
- **Config** (`.clang-format`):
  - `BasedOnStyle: LLVM`, `IndentWidth: 4`, `AccessModifierOffset: -4`, `ColumnLimit: 100`, `PointerAlignment: Left` (`DerivePointerAlignment: false`), `InsertBraces: true`, `InsertNewlineAtEOF`, `LineEnding: LF`;
  - `IncludeBlocks: Regroup`, ordered project `"…"` → third-party `<lib/…>` → C++ std `<x>` → C/POSIX `<x.h>`.
- **Never enable** `QualifierAlignment`, `RemoveParentheses`, `RemoveSemicolon` or `EnumTrailingComma`. They can change meaning or churn diffs.
- **`.clang-format-ignore`** covers `.claude/**`, `build/**` and `third_party/**`. In this file `*` does not cross `/`, so use `**`.
- **pre-commit `mirrors-clang-format`** only if the project ever drops the pixi manifest, never both. If used, override `types_or: [c++, c]`, because its default includes JSON and would reformat `CMakePresets.json`.
- The PostToolUse hook checks every edited C++ file and **reports without rewriting**. Fix the file with `nice pixi run fmt`.

## clang-tidy

**Config** (`.clang-tidy`, verbatim in `scaffold.md`):

- **Checks:** whole modules `bugprone-*`, `clang-analyzer-*` (listed explicitly), `cppcoreguidelines-*`, `misc-*`, `modernize-*`, `performance-*`, `readability-*`, plus `cert-err33-c`. That check covers unchecked `fflush`/`fclose`/`fprintf`/`remove` results, which matches the flush-at-exit contract in `cli-design`; cast to `(void)` only with a comment.
- **Subtracted, by reason:**
  - *Noise:* `bugprone-easily-swappable-parameters`, `readability-magic-numbers`, `cppcoreguidelines-avoid-magic-numbers`, `readability-identifier-length`, `modernize-use-trailing-return-type`, `cppcoreguidelines-avoid-do-while`, `cppcoreguidelines-owning-memory` (assumes GSL `owner<>`), `misc-no-recursion`, `misc-multiple-inheritance`, `performance-enum-size`.
  - *Debate, reviewer-enforced instead:* `modernize-use-nodiscard`.
  - *Formatter-owned:* `readability-braces-around-statements`, `readability-inconsistent-ifelse-braces`, `readability-trailing-comma`.
  - *Runtime-covered* (std-lib assertions + ASan): `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`.
  - *Conflicts with a house rule:* `readability-redundant-member-init`. `modern-cpp-conventions` requires `{}` default member initializers on class-type members, so partial designated initialization stays warning-free under `-Werror` on both compilers. This check flags exactly those (`std::string message{};`), and it has no option to exempt them.
- **Never add** `portability-*` (it contains `avoid-pragma-once`), `llvm-*`, `google-*`, `fuchsia-*`, `llvmlibc-*` or `altera-*`. Add `concurrency-mt-unsafe` if the program becomes multi-threaded.
- **Analyzer checks:** none are pre-disabled. Disable one specific check only after it trips, with a written reason in the config comment.
- **Other keys:**
  - `WarningsAsErrors: '*'` (exit 1 on any finding; a CLI `--warnings-as-errors=''` does not override the file);
  - `HeaderFilterRegex: '.*/(include|src|tests)/.*'` (the default became `.*`) with `ExcludeHeaderFilterRegex` for `_deps`, `third_party`, `external`, `vendor`, `.pixi` and `vcpkg_installed`;
  - `SystemHeaders: false`;
  - `FormatStyle: file`.
- **CheckOptions:**
  - `misc-include-cleaner.IgnoreHeaders` for the CLI11, nlohmann and Catch2 umbrella headers;
  - `IgnoreClassesWithAllMemberVariablesBeingPublic` (aggregates), under both the `misc-` and the `cppcoreguidelines-` alias names.
- **Unknown keys and the verify-config gate.** A normal clang-tidy run **silently ignores an unknown CheckOptions key**, so a typo simply does nothing. `/setup` therefore confirms each key appears in `clang-tidy --dump-config` (unknown keys are dropped from the dump). It also runs `clang-tidy --verify-config`, which on the 23.x build checked while writing this studio warned `unknown check option` and exited 1. Confirm your pinned version still does before relying on it.
- **`tests/.clang-tidy`:** `InheritParentConfig: true` plus four removals, each caused by a Catch2 macro expansion:
  - `readability-function-cognitive-complexity`
  - `cppcoreguidelines-avoid-non-const-global-variables`
  - `bugprone-chained-comparison`
  - `misc-use-anonymous-namespace`

  It uses the YAML list form of `Checks:` so each entry carries its reason. A `#` line inside a folded `Checks: >` block would become part of the check string.

**Running it** (`scripts/lint.sh`; do not improvise a different invocation):
- **Compile DB:**
  - Lint only from the **Clang** compile DB, `build/default/dev/compile_commands.json`. Its clang major must equal clang-tidy's, which the pixi pin guarantees. The sanitized dev DB is fine.
  - PCH and unity builds stay off in dev.
  - If clang-tidy reports standard-header errors (`'print' file not found`), it can't drive the conda compiler named in the DB. That is a `build-fixer` job, not a reason to skip lint.
- **Always pass `-j`.** `run-clang-tidy` and `clang-tidy-diff.py` default to every CPU, which defeats the per-worktree cap. `lint.sh` reads `vendor.cpp-cli-studio.jobs` from `CMakeUserPresets.json` (`scripts/read-jobs.cmake`), falling back to `nproc`.
- **Restrict to own sources:** the regex `^<root>/(src|tests)/`, so `_deps/*.cpp` and generated TUs are never linted.
- **Assert a nonzero file count.** A filter that matches nothing makes `run-clang-tidy` report success "for 0 files". `lint.sh` counts the matching DB entries first and **exits 1 on zero**. It accepts the root spelled physically or through a symlink, whichever the DB uses.
- **Script facts** (resolved at use-time; packaging differs):
  - The conda `clang-tools` ships `run-clang-tidy` **without** `.py`. `lint.sh` falls back to `run-clang-tidy.py`.
  - `run-clang-tidy` has **no** `-timeout` and **no** `-only-check-in-db`; those belong to `clang-tidy-diff.py`.
  - `clang-tidy-diff.py` lives at `$CONDA_PREFIX/share/clang/clang-tidy-diff.py`, not on `PATH`. Its `-p` is the **patch strip level** and its build dir is `-path`. It has **no** `-format` and no `-style`.
- **Mode semantics:** all three modes hold the build-dir lock below. `lint-fix` applies fix-its with `-fix -format -style file`, then runs `format.sh --fix`. Always review a `lint-fix` diff; `misc-const-correctness` fix-its in particular can touch many lines.
- **Troubleshooting `@…modmap: no such file`:** a clang-tidy error mentioning a `.modmap` file means lint ran before `build`, or C++ module scanning is ON. The presets set `CMAKE_CXX_SCAN_FOR_MODULES=OFF`; run `nice pixi run build` (or re-configure a fresh build dir), then lint again.
- **Per-file checks:** the PostToolUse hook runs clang-tidy on the edited `.cpp` under `timeout 60`. A timeout there is a note, not a block. **`pixi run lint` is authoritative.**

## NOLINT policy

- Write suppressions only as `// NOLINTNEXTLINE(<check-name>): <reason>` on the line **before** the finding.
- The check name must be exact: a wrong name does not suppress, and that is intended.
- Never use bare `NOLINT`, wildcard check lists, or a suppression without a reason. The `cpp-rule-linter` hook flags those.
- `NOLINTBEGIN(<check>)`/`NOLINTEND(<check>)` are only for generated or vendored code.
- Reviewers treat every new NOLINT as a finding to justify. Suppressing a real signal to make the gate green is a Must-fix.

## Warnings policy (`cmake/ProjectWarnings.cmake`)

- **Target-scoped.** An INTERFACE target `<proj>_warnings` is linked `PRIVATE $<BUILD_INTERFACE:<proj>_warnings>` through `<proj>_apply_warnings(<tgt>)`. Call that on every own target (core, app, test executables). Never use `add_compile_options(-W…)`.
- **Common (GCC and Clang):** `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion -Wnon-virtual-dtor -Woverloaded-virtual -Wsuggest-override -Wold-style-cast -Wcast-align -Wcast-qual -Wnull-dereference -Wformat=2 -Werror=format-security -Wimplicit-fallthrough -Wextra-semi`.
  - `-Wsign-conversion` is explicit, because GCC's `-Wconversion` doesn't imply it in C++.
- **Gated per compiler.** Clang only *warns* on an unknown `-W` flag, and under `-Werror` that warning becomes an error. A flag for one compiler must never reach the other.
  - *GCC-only:* `-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast -Wtrampolines`, plus `-Wbidi-chars=any` (probed). `-Wdangling-reference` is already in GCC's `-Wextra`; `-Wdangling-pointer=2` and `-Wuse-after-free=2` are in its `-Wall`.
  - *Clang-only:* `-Wcomma -Wunreachable-code -Wshadow-uncaptured-local`. With `<PROJ>_LIFETIME_SAFETY=ON` (default OFF), the probed `-Wlifetime-safety-permissive` is added as **advisory**: `-Wno-error=` keeps it a warning even under `-Werror`. Its noise is judged at use-time.
- **`-Werror` scope:**
  - It is the per-target `COMPILE_WARNING_AS_ERROR` property, set only when `<PROJ>_WARNINGS_AS_ERRORS` is ON (default OFF; the presets and CI turn it ON).
  - **Never** use the global `CMAKE_COMPILE_WARNING_AS_ERROR`, which would `-Werror` source-built dependencies.
  - Packagers (the AUR PKGBUILD) pass `-D<PROJ>_WARNINGS_AS_ERRORS=OFF` or `cmake --compile-no-warning-as-error`. Agents never do; the `guard-bash` hook blocks it.
- **Sanitized GCC builds only:**
  - These six are demoted to warnings: `-Wno-error=maybe-uninitialized`, `array-bounds`, `stringop-overflow`, `stringop-overread`, `stringop-truncation`, `null-dereference`. Sanitizer instrumentation degrades GCC's flow analysis (GCC PR 83382, PR 108306).
  - The release build keeps full `-Werror` and catches the real ones.
  - The list grows only with a GCC bugzilla link (`memory-safety` owns that policy). Clang's sanitized dev build keeps full `-Werror`.
- **A real false positive** gets a scoped `#pragma GCC diagnostic` with a reason and bugzilla link, or a compiler-gated `-Wno-error=<flag>` in ProjectWarnings.cmake. Never a blanket `-Wno-error`.
- **Friction notes:**
  - `-Wshadow`: name members with a trailing `_` so constructor parameters can keep the natural names.
  - `-Wuseless-cast`: casts that are no-ops on one platform's typedefs. Restructure, or use a compiler-gated pragma.
  - `-Wold-style-cast`: fires on libc macros. Wrap them in a small helper instead of disabling the warning.

## Build-dir lock (shared by `scripts/preset.sh` and `scripts/lint.sh`)

Parallel tracks in one worktree share `build/<env>/<preset>`. Both scripts take the same lock: an atomic `mkdir build/<env>/<preset>.lock.d/`.
- **`owner` file:** the holder writes `<lock>/owner` with `pid=`, `host=`, `pstart=` (field 22 of `/proc/<pid>/stat`), `started=` (UTC) and `cmd=`.
- **Stale takeover:** a waiter that finds the lock checks the owner. A dead PID on this host, or a reused PID (different `pstart`), means the owner is gone. The waiter then takes the lock over atomically (rename, verify, delete) and prints a note. An owner dir with no `owner` file for more than 60 s is stale too.
- **Bounded wait:** a live owner is waited for, with a message naming it, for at most 30 minutes. Then the script exits **75** with instructions. It never spins forever.
- **Release:** a trap releases the lock on exit, `INT`, `TERM` and `HUP`. A `SIGKILL` leaves it behind, and the next run recovers it.
- **Waiting is normal:** "waiting for … lock" means another track is building. A compile error in a file another track owns is a coordination event for the orchestrator, not something to fix.

## Other analyzers

- **cppcheck:** advisory only. It is probed, run in `/harden` when present, and never gates.
- **Skipped:** IWYU (`misc-include-cleaner` covers include hygiene), scan-build (the analyzer runs inside clang-tidy) and cmake-format. gersemi is off by default (see the debates table).
- **ctcache:** a conditional speed-up. Probe for it; never install it.

## Style debates: defaults and alternatives

The defaults are the decided values. An alternative is a documented, project-level switch: one `.clang-format`/`.clang-tidy` commit plus a `fmt` commit, never a per-file deviation.

| Topic | Default | Alternative |
|---|---|---|
| Braces | `InsertBraces: true` (formatter-owned) | `readability-braces-around-statements` in tidy, `InsertBraces` off |
| Base style | `BasedOnStyle: LLVM`, `IndentWidth: 4`, `AccessModifierOffset: -4`, `ColumnLimit: 100` | LLVM with indent 2 (`AccessModifierOffset: -2`), or `BasedOnStyle: Google` |
| `misc-const-correctness` | On (via `misc-*`) | Disable it, or run it CI-only if it is too noisy |
| `modernize-use-nodiscard` | Off. `[[nodiscard]]` on factories and `Result` functions is enforced by reviewers | Enable it and accept the churn |
| Where tidy runs | A separate `lint` step after `build` | A `CMAKE_CXX_CLANG_TIDY` preset, which is incremental but slows every build |
| clang-analyzer | In the local gate | CI-only, if local lint becomes too slow |
| CMake formatting | gersemi off | gersemi with a pinned version |

## Verify at use-time (never baked)

- That every `.clang-format` key is valid for the pinned major (`--dump-config`).
- That every `tests/.clang-tidy` name exists (`clang-tidy --list-checks -checks='-*,<name>'`).
- That every CheckOptions key appears in `--dump-config`.
- The script names and locations in the installed `clang-tools`.
- That clang-tidy handles the conda driver name in the compile DB.
- The `-Wlifetime-safety*` spelling and noise.
- The `-Wbidi-chars=any` support (probed in CMake).
- Whether pixi de-duplicates `build` across `depends-on`.
- Tool versions: read them from `pixi list`, never from memory.

## Sibling skills

- `low-priority-execution`: how to type these heavy commands (`nice pixi run …`) and why job caps are separate from niceness.
- `build-workflow`: presets, `scripts/preset.sh` (the other holder of the build-dir lock), pixi envs, and the per-worktree `-local` presets that carry the job cap.
- `memory-safety`: the sanitized dev build, and the GCC `-Wno-error` demotion policy.
- `cpp-testing`: what `test` runs, the test tiers and goldens.
- `modern-cpp-conventions`: the code style that clears these checks naturally.
