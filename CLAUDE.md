# Project: C++23 command-line tool(s)

This repo builds native C++23 command-line tools. The toolchain and every dependency come from **pixi** (one lockfile, two environments: `default` = conda-forge **Clang**, `gcc` = conda-forge **GCC**). The build is **CMake + Ninja** driven through `CMakePresets.json`. The command line is **CLI11**, tests are **Catch2 v3** plus an installed-binary E2E driver (`cmake -P`), JSON is **nlohmann/json**, formatting is **clang-format**, linting is **clang-tidy**, and the dev build is **always sanitized (ASan + UBSan)**. This file is your standing instructions: treat it as binding.

## Verify at use-time: read this first
Training data goes stale, and this stack moves fast (compiler majors, CLI11/Catch2 APIs, CMake presets schema, clang-tidy checks, GitHub action majors). **Never write a version, flag, API or config key from memory.**
- Read what is installed: `pixi list`, `pixi tree <pkg>`, `pixi run cmake --version`, `pixi run clang-format --version`, the headers under `.pixi/envs/<env>/include`, and the probes (`nice pixi run cxx-probe .claude/skills/platform-testing/probes/probe_features.cpp`).
- Planners mark every version-, API- or config-specific assumption `[VERIFY]`. The `fact-checker` confirms or refutes each one against the installed env first, then current docs, **before** code is written. A refutation sends the plan back to the planner.
- Things only this machine can answer (compiler/sanitizer usability, the glibc floor of a build, readelf markers, action SHAs) are checked at `/setup` and by the `platform-tester`, never assumed.

**Fact-check snapshot (orientation only, verify at use-time).** These are the versions observed when the studio was generated. They are *not* facts about your machine, and nothing in the studio depends on them:

| Component | Seen at generation |
|---|---|
| conda-forge clang (`clangxx_linux-64`, `clang-tools`, `llvm-tools`, `clang-format`) | 23.1.2 |
| conda-forge gcc (`gxx_linux-64`) | 16.2.0 |
| conda sysroot glibc | 2.28 |
| CMake / Ninja | 4.4.3 / 1.13.2 |
| pixi | 0.81.0 |
| CLI11 / Catch2 / nlohmann_json | 2.7.2 / 3.16.0 (shared-only on conda-forge) / 3.12.0 |
| CI runner image | ubuntu-26.04 |

## Golden workflow
Never jump straight to code for a non-trivial change. Follow:

**design → plan → fact-check → ⏸ plan approval → implement (parallel file-disjoint tracks, each looping with `quality-reviewer` + `memory-safety-reviewer`) → real installed-binary test (`cli-tester`) → conditional platform pass (`platform-tester`) → feature review board → fix → re-check → `## User Test Plan`**

The commands run it (they are user-invoked skills; see "Commands" below). `/feature` runs the whole pipeline in the current tree; `/plan`, `/implement`, `/review`, `/fix`, `/harden` and `/platform-test` run pieces of it; `/setup` bootstraps a project from the studio's scaffolds. `/features <file.md>` fans the whole pipeline out over **many features at once**: one **git worktree per feature** (`feature/<slug>` in `../<repo>-worktrees/<slug>`), each with its own `pixi install --locked` env and per-worktree job caps, running concurrently, then **one PR per feature**. It checks each feature off in the source Markdown file as its PR lands, and forwards every ⏸ gate, subagent question and **golden re-bless request** to you, labeled by feature + worktree + phase.

### The User Test Plan
`/feature`, `/implement` and every `/features` PR body end with a section headed exactly `## User Test Plan`. It is **never omitted**: when nothing needs hands-on checking it says so and names what covered it. It is **derived, not brainstormed**, from:
- acceptance criteria not marked passing (every `human`- and `tty-pty`-tagged criterion);
- `cli-tester`'s "Could not verify from here";
- the `platform-tester` tier grid ("Not covered, with reasons", "Still human, unconditionally");
- decisions settled at a ⏸ gate, and reviewer judgment calls.

Each remaining item carries a tag `[human: <reason> · retryable|terminal]`. Each item a platform pass removed is listed as `removed: "<item>" — licensed by <target> @ <tier>`. `cli-tester` drives the piped/no-TTY path and a pseudo-TTY, so the real interactive terminal (a human typing into prompts, terminal-emulator rendering, real data and credentials) always stays with you.

### Parallel implementation
When the plan's **Parallelization plan** marks tasks independent (no shared dependency, disjoint `Owns` sets), launch one `implementer` per task **in the same batch**. Batches run in order; a batch integrates and its review loops pass before the next starts. Correctness beats concurrency.
- **Serialization points** (never in two tracks of one batch): top-level `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml` + `pixi.lock`, `src/app/cli.cpp` (the subcommand registration list), shared public headers in `include/<proj>/`, `tests/CMakeLists.txt`, `scripts/*`, `.github/workflows/*`.
- Sources and tests are globbed with `CONFIGURE_DEPENDS` (`src/core/**/*.cpp`, `src/app/cmd_*.cpp`, `tests/unit/**/*.cpp`, `tests/cli/*.cpp`, `tests/e2e/cases/*.cmake`), so adding a module, test or E2E case does not touch shared CMake.
- **One build per worktree, guarded by a lock.** Tracks in one worktree share `build/<env>/<preset>`. `scripts/preset.sh` and `scripts/lint.sh` take an atomic `build/<env>/<preset>.lock.d/` lock that records its owner PID; a lock whose owner is dead is taken over automatically, with a note. Never delete a lock by hand while another track may be building.
- **A compile error in a file another track owns is a coordination event, not something to fix.** Report it; the orchestrator resolves it.

### Code must pass review (implement↔review loop, two reviewers)
Writing the code is not "done". Every C++ track loops:
1. The `implementer` gets `nice pixi run check` green and hands off.
2. `quality-reviewer` and `memory-safety-reviewer` review **in parallel**. Only `quality-reviewer` runs the full gate; `memory-safety-reviewer` runs targeted tests (`nice pixi run test -R …`) and reads the latest results, so two heavy gates never run at once.
3. On any **CHANGES REQUESTED**, the **same implementer** addresses both Must-fix lists in one round with a single change log, re-runs the gate, and both re-review.
4. **Both** must return `VERDICT: APPROVED`. The ~3-round cap counts combined rounds; a disagreement or a 4th round goes to ⏸ you.
5. Build/link/toolchain failures inside a track go to `build-fixer`, scoped to the track's `Owns` plus the build files the plan designates.

The feature-level board (`security-reviewer`, `cli-ux-reviewer`, a final `quality-reviewer` and a final `memory-safety-reviewer`) gates the same way. Reviewers approve on "high quality, gate green, no real blockers", not unreachable perfection.

## Everything runs through pixi
Run every tool through a pixi task so it uses the project's pinned toolchain. **Never use a host compiler, host `clang-format`/`clang-tidy`, or a raw `cmake`/`ctest`/`clang-tidy` pipeline as a substitute for a task.** Add dependencies with `pixi add <pkg>` (conda-forge). **Never hand-edit `pixi.lock`**; commit it. `CMakeLists.txt` stays **manager-agnostic** (`find_package(... CONFIG REQUIRED)`, no pixi paths), so the project also builds outside pixi.

| Task | What it does |
|---|---|
| `fmt` / `fmt-check` | clang-format in place / verify |
| `configure` / `build` / `test` | sanitized `dev` preset (ASan+UBSan, `-O1 -g`, `-Werror`): configure → build → ctest (unit, in-process CLI, installed-binary E2E, edge suite, sanitizer positive controls). `test -R/-L/-E` filter |
| `lint` / `lint-fix` / `lint-changed` | `run-clang-tidy` over own TUs (asserts a nonzero file count; a stated skip in the `gcc` env) / fix-its + `fmt` / changed lines vs the merge-base |
| `release` / `release-test` | optimized, hardened, static-libstdc++ `release` build with `-Werror` / + tests + `release-check` on the release install |
| **`check`** | **the gate:** `fmt-check` → `test` → `lint` → `release`, fail-fast |
| `cxx-probe` | compile and run one probe from `.claude/skills/platform-testing/probes/` or `.platform-runs/` with the dev preset's exact flags |
| `bless` | re-bless E2E goldens matching a regex (**human-approved**; in `ask`) |
| `cov` / `audit` / `package` | report-only coverage / syft + grype over the env / CPack tarball from the release build |
| optional: `tsan`, `fuzz`, `man` | commented in `pixi.example.toml`; enable per Project specifics |

- **Two environments, same chain:** `nice pixi run check` (Clang) and `nice pixi run -e gcc check` (GCC). CI runs both on every PR. Run the gcc one locally when a change touches CMake, presets, warning flags or `pixi.toml`, or is compiler-sensitive.
- **Build dirs are keyed by env:** `build/<env>/<preset>` (e.g. `build/default/dev`, `build/gcc/release`); the installed binary is `build/<env>/<preset>/e2e-prefix/bin/<tool>`.
- **The shipped artifact comes from the `default` (Clang) env's release build only.** The gcc env's release build is a `-Werror` + test check, never shipped.
- Release *tests* (`nice pixi run release-test`) run in CI, in `/harden`, and when a plan touches the release surface.

## Specialized subagents (delegate proactively)
- **cli-designer** (opus, read-only) — turns an idea into a precise CLI11 command spec: surface, streams, exit codes, edge cases, data/state; every acceptance criterion tagged with the tier that verifies it. Use before planning.
- **implementation-planner** (opus, read-only) — ordered plan with per-task `Owns` sets, parallel batches, called-out serialization points, pixi/CMake changes, a per-tier test plan, a memory-safety watch-list and a clustered `[VERIFY]` list.
- **fact-checker** (sonnet, read-only on source) — confirms/refutes every `[VERIFY]` item against the installed pixi env (`pixi list`, headers, `cxx-probe`), then current docs. The orchestrator fans these out, one per independent cluster. It writes only probe scratch under `.platform-runs/`.
- **implementer** (opus, edit) — executes its slice of an approved, fact-checked plan with tests, stays inside its `Owns` set, reports cross-track needs as coordination points, and loops until **both** track reviewers APPROVE.
- **cli-tester** (sonnet, read-only) — runs the **real installed binary** (sanitized dev install; the release install on request and always in `/harden`): exit codes, stdout/stderr split, pipes and SIGPIPE, `/dev/full`, stdin, pseudo-TTY, `--json`, env/config precedence. Ends with "Could not verify from here".
- **test-engineer** (sonnet, edit `tests/**` only) — backfills unit, in-process CLI, E2E, edge-suite and sanitizer-control tests by risk; never touches production code or goldens.
- **quality-reviewer** (sonnet, read-only) — runs the gate and reviews correctness, the CLI contract, test tiers, conventions, compile-time and NOLINT hygiene; **build-review mode** for CMake, presets, `pixi.toml`, `scripts/` and `.github/`. Emits a verdict.
- **memory-safety-reviewer** (opus, read-only) — on **every** C++ change: ownership, lifetime and UB against the `memory-safety` hazard table, with sanitizer evidence (positive controls fired, changed paths exercised) and a suppression-diff audit. Emits a verdict.
- **security-reviewer** (sonnet, read-only) — runs `nice pixi run audit` and reviews against the `cpp-security` checklist. Emits a verdict (Critical/High block).
- **cli-ux-reviewer** (opus) — reviews help, naming, errors, defaults, output, color/TTY, `--json` and prompt safety on the real binary. Emits a verdict; when asked, implements help-text, message and format-only fixes, and requests a golden re-bless instead of touching goldens.
- **code-refactorer** (sonnet, edit) — behaviour-preserving cleanup with `check` (including the release `-Werror` build) green before and after; never touches goldens or the CLI contract.
- **bug-fixer** (opus, edit) — runtime bugs, failing tests, crashes and sanitizer findings: reproduce on the installed binary or exact test, root-cause, failing regression test, minimal fix, prove with `check`.
- **build-fixer** (opus, edit) — the build surface: pixi solve/lock, configure, compile/link, conda flag leaks, lint infrastructure, CTest discovery, the E2E install fixture, `release-check`, CI.
- **platform-tester** (sonnet, read-only) — the conditional capability: probes what is present, climbs tiers per target, reports a tier grid with retryable/terminal reasons. Never installs, pulls, blocks or emits a verdict.

"Read-only" means no Edit/Write tool. Several read-only agents still run Bash (gates, probes, the installed binary). Delegate context-heavy or specialized work to keep the main session focused.

## Hard rules
1. **Verify, don't assume.** Every version-, API- or config-specific fact comes from the installed pixi env (`pixi list`, headers, probes) and current docs via the `fact-checker`. If it can't be confirmed, verify it at use-time; never bake a guess.
2. **Green `nice pixi run check` is the definition of done** (plus `nice pixi run -e gcc check` when the change touches the build surface, is compiler-sensitive, or the plan calls for it), together with both track reviewers and the feature board at `VERDICT: APPROVED`. New behaviour ships with tests at the right tier. **Never** skip, weaken or delete tests, turn off `-Werror` (`--compile-no-warning-as-error`, `*_WARNINGS_AS_ERRORS=OFF`), add a blanket `NOLINT`, or add a sanitizer suppression to go green. Anything that can't run is red, not skipped.
3. **Memory safety and UB are the dominant risk.** Follow `modern-cpp-conventions` (no owning raw pointers, Rule of Zero, views are borrowers, no escaping by-reference captures). A sanitizer finding is a bug, never noise. Suppressions need a reason and an issue link, and UBSan runtime suppressions are inert under `-fno-sanitize-recover=all`. **`memory-safety-reviewer` reviews every C++ change**, and approves only on evidence: positive controls fired and the changed paths were exercised. "Sanitizer-clean" means "no finding on exercised paths", never "memory-safe".
4. **The always-sanitized dev preset (option C) holds only under its eight conditions** (`memory-safety` skill):
   1. **exit code 86 is reserved** — `exitcode=86` in `ASAN_OPTIONS`, `UBSAN_OPTIONS` and `LSAN_OPTIONS` (and `TSAN_OPTIONS`); the app never returns 86, and the E2E driver also fails any case whose stderr matches the sanitizer marker regex, whatever the exit code;
   2. in **sanitized GCC builds only**, exactly the six documented `-Wno-error=` demotions, each further one only with a GCC bugzilla link;
   3. **a sanitized build is never shipped** (`package.sh` and CPack refuse it);
   4. explicit `-D_GLIBCXX_ASSERTIONS` (the dev preset is `-O1`);
   5. `-U_FORTIFY_SOURCE` in sanitized builds;
   6. **the release build compiles with `-Werror` inside `check`**, catching what only `-O2` sees;
   7. parallel caps cover ctest `-j` and the link pool;
   8. sanitizer usability is probed in the real context (same env, flags and options as `check`).

   Never use `_GLIBCXX_DEBUG` (it changes the libstdc++ ABI against prebuilt deps). Never set `*SAN_OPTIONS` with `exitcode=0` or `halt_on_error=0`.
5. **The CLI contract is an API.** Flag names, output formats and exit codes are depended on by scripts; change them only through an approved plan. See `cli-design`.
   - **Exit codes** (single source `include/<proj>/exit_codes.hpp`): `0` success · `1` expected failure, including a failed write to stdout · `2` usage (every `CLI::ParseError` and every semantic usage error) · `3–63` tool-specific, declared before use · `70` internal (an exception reached `main`) · `86` reserved for sanitizers · `130`/`141` signal deaths only. Never 126, 127, 128+n or >255.
   - **stdout is data, stderr is diagnostics** (`<tool>: …`). `--json` output never contains ANSI.
   - **SIGPIPE policy A:** `main` restores the default disposition first thing, so `tool | head -1` dies quietly with 141. Policy B only with a written reason (Project specifics).
   - **Flush and check stdout on every exit path** (`finish_stdout`), so `tool > /dev/full` exits 1 with `<tool>: write error`.
   - **Return from `main`; never `std::exit`.** `main` is a catch ladder: `Failure` → its code, `std::system_error` → 1 (before `std::exception`), `std::exception`/`...` → 70.
   - **Parse-then-dispatch, never `CLI11_PARSE`**; callbacks only select; bound variables outlive `parse()` and dispatch.
   - **CLI11 precedence is command line > config file > env (`envname`) > default.** Config beats env; the designer states the precedence on every feature with both.
6. **Modern C++ conventions are the house style** (`modern-cpp-conventions`, the analog of an attrs-style rule): value types are aggregates with a default member initializer on every member and are built with designated initializers; a type with an invariant becomes a class with private members and a `[[nodiscard]]` validating factory returning `Result<T>` (`std::expected<T, Error>`); `enum class` only; `#pragma once` and self-contained headers; no C-style casts; strong types over bare primitives. Plain functions are fine where a type isn't the right fit.
7. **Keep the CLI layer thin.** Logic lives in the core library (`<proj>_core`: `include/<proj>/`, `src/core/`) as pure functions returning `Result<T>`, unit-tested without CLI11 and without touching std streams. `src/app/` only parses, dispatches, prints and maps errors to exit codes. `<CLI/CLI.hpp>` is included only from `src/app/*.cpp`.
8. **Test the installed artifact.** E2E cases run the binary installed by the `e2e.install` fixture, hermetically, asserting **exact** exit codes and separate streams; never `PASS_REGULAR_EXPRESSION` (it ignores the exit code). **Goldens are human-blessed only:** an agent requests a bless (case regex + reason); you run `pixi run bless '<regex>'` (it asks). Never set `UPDATE_GOLDEN`, never hand-edit an existing golden, never bless to turn a red test green.
9. **Build hygiene.** Warnings and `-Werror` are target-scoped (`<proj>_apply_warnings`), never global; presets set compiler/linker flags explicitly so conda activation flags can't leak; no `jobs` in committed presets. **Never set `CMAKE_BUILD_PARALLEL_LEVEL` or `CTEST_PARALLEL_LEVEL`** anywhere: they override the per-worktree caps, which travel only in `CMakeUserPresets.json` `-local` presets and its `vendor` key. New sources go in the globbed directories; serialization points stay serialized.
10. **Lint and format ownership.** clang-format owns formatting (never hand-format against it); `clang-format`, `clang-tidy` and the Clang compiler share one LLVM major. Every `NOLINT` is `// NOLINTNEXTLINE(check-name): reason` on the line before. A lint run that analysed 0 files is a failure.
11. **Security gates.** Run `security-reviewer` before committing anything that touches process spawning, files/paths/temp files, file descriptors, env vars, secrets, untrusted input (argv, env, files, JSON), format strings, size math, network, dependencies or CI (`cpp-security`). No shell interpolation of input, no runtime format strings with user data, no secrets in argv or logs, atomic writes via temp + rename.
12. **Releases come only from tag-push CI.** Agents never run `gh release`, `git tag`, `git push --tags`, un-prefixed `cmake --install`, `makepkg -i`, `sudo`, or any package install; `guard-bash` and the deny rules back this up.
13. **Low-priority execution, locally only.** Type heavy commands as bare **`nice <cmd>`** (`nice pixi run check`, `nice pixi run -e gcc test -R e2e`, `cd <worktree> && nice pixi run …`). Bare `nice` is GNU's +10 and is stripped before permission matching, so it matches the exact `pixi run` allow rules. Using bare `nice` instead of the canonical `nice -n 10` is a deliberate deviation; the flagged form (with `ionice`/`taskpolicy`) survives only inside the hooks' `low_prio` helper. Never put `nice` into a task, preset, script or CI; never swallow the exit code (no unguarded pipes into `tail`/`grep`). **Never set `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR` inline** (`guard-bash` blocks it). Gate runs (`check`, `-e gcc check`, `test`, `lint`, `release`, `release-test`, `cov`) routinely exceed the Bash tool's 120 s default: pass an explicit long `timeout` (up to 600000 ms) or use `run_in_background` and wait. **A run killed by the tool timeout is not a failure**; re-run it with a longer timeout before diagnosing. See `low-priority-execution`.
14. **Platform testing is conditional and additive** (`platform-testing`, `platform-tester`, `/platform-test`).
    - Probe at use-time, cheapest and non-mutating first; report a **tier per target** (`configured` → `built` → `smoke-ran` → `contract-verified` → `sanitizer-clean`), never a global one.
    - Degrade with a stated reason, **retryable** (R1–R11: what would change it) or **terminal** (T1–T4: why a rerun changes nothing). A missing capability is a successful no-op report, never a failure, block or stall.
    - Never install, `docker pull`, `sysctl` or run anything privileged to raise a tier.
    - A platform pass **narrows** the User Test Plan, removing only the items its tier licenses for that target, and **never deletes** it. Never write "verified" without a target and a tier.
    - macOS and Windows/MSVC are terminal locally and licensed only by CI results. Real-terminal and interactive human judgment is never licensed away.
15. **pixi cache on ecryptfs/NFS: warn, never block or set.** Detecting ecryptfs (on the cache dir, `$HOME` or the repo) is a warning, and work continues. Only an actual `File name too long` from `pixi install` stops; then the user exports `PIXI_CACHE_DIR` (which wins over `RATTLER_CACHE_DIR`) to another filesystem in their shell profile and restarts `claude`.
16. **Stop and ask** on a blocker, a loop past ~3 rounds, a surprise (the installed reality contradicts the plan or a skill), or anything destructive or irreversible.

## Commands (user-invoked skills)
The 9 orchestration commands are written as skills (`.claude/skills/<name>/SKILL.md` with `disable-model-invocation: true`), so you invoke them as `/<name>` and Claude never auto-runs them. There is no `.claude/commands/` directory.
- `/setup <tool name> — <purpose>` — bootstrap from the scaffolds: ecryptfs warning, names, `pixi.toml` pinned from `pixi search`, scaffolds, C++23 feature probe, sanitizer usability probe, green `check`, initial goldens, verify-at-use-time checklist, optional CI.
- `/feature <description>` — the full golden workflow in the current tree, ending with the User Test Plan.
- `/features <file.md>` — the full workflow per feature in parallel worktrees, one PR each.
- `/plan <description>` — design + plan + fact-check, then stop before code.
- `/implement <approved plan>` — parallel batches with dual review loops, installed-binary test, platform pass, board, fixes.
- `/review [path | range]` — run `check` once and the four-reviewer board on a diff; consolidated verdicts, no code changes.
- `/fix <symptom>` — route to `build-fixer` or `bug-fixer`, regression test, prove with `check`.
- `/harden [area]` — tests backfill, extended verification (release-test, extended UB pass, TSan if threaded, cov, audit, platform pass), parallel review, refactor, re-check; behaviour unchanged.
- `/platform-test [probe|local|ci|all]` — re-run the conditional platform pass on demand (e.g. after pulling a docker image or once CI finishes).

## Skills available (knowledge)
`cli-design` (CLI11 idioms and the CLI contract), `modern-cpp-conventions` (C++23 house conventions), `cpp-security` (security checklist + the `audit` task), `cpp-quality` (the gate, clang-format/clang-tidy, warnings, NOLINT, definition of done), `build-workflow` (pixi + CMake presets, targets, caps and lock, release builder, CI), `cpp-testing` (test tiers, the E2E driver, edge suite, goldens, sanitizer controls), `memory-safety` (the hazard table, option C, sanitizer flags/options, hardening, evidence standard), `platform-testing` (the probe ladder, tiers and reasons that `/platform-test` and `platform-tester` follow; not the command itself), `low-priority-execution` (bare `nice`, timeouts, caps vs niceness). Consult them when relevant. Several carry supporting `scaffold.md` / `ci-release.md` files that `/setup` reads.

## Review routing
| Change | Reviewers |
|---|---|
| Any C++ in a track | `quality-reviewer` + `memory-safety-reviewer` (both APPROVE) |
| Feature board / `/review` | `quality-reviewer` + `security-reviewer` + `memory-safety-reviewer` + `cli-ux-reviewer` |
| CMake, presets, `pixi.toml`, `scripts/`, `.github/` | `quality-reviewer` in **build-review mode** (+ `security-reviewer` for dependencies and CI) |
| Process, files/paths, env, secrets, untrusted input, network, deps | `security-reviewer` |
| Help text, messages, output format | `cli-ux-reviewer` |

`platform-tester` is **not** a board member: it reports tiers and never emits a verdict.

## Hooks (`.claude/hooks/`, wired in `.claude/settings.json`)
- **protect-files.sh** (PreToolUse `Edit|Write`) — blocks edits to secrets (`.env*`, keys, `*.pem`, `secrets/`, `credentials`), `pixi.lock`, `.claude/settings.local.json`, and **existing** goldens under `tests/e2e/golden/`.
- **guard-bash.sh** (PreToolUse `Bash`) — a **best-effort lexical scan** that blocks privilege escalation, package installs, unsafe docker, un-prefixed installs, turning `-Werror` off, tag/release/force-push shapes, and inline `CMAKE_BUILD_PARALLEL_LEVEL`/`CTEST_PARALLEL_LEVEL`/`UPDATE_GOLDEN`/`PIXI_CACHE_DIR`/`RATTLER_CACHE_DIR`. It also blocks Bash reads, copies and encodes of secret files (`cat .env`, `< .env`, `source .env`, `base64 *.key`, `cp .env …`), while `.env.example`, `settings.local.json.example` and existence checks (`ls`, `test -f`, `git check-ignore`) stay allowed. It cannot see commands built at runtime, script files run by an interpreter, or commands fed to an interpreter on stdin; it fails open (with a message) if it cannot parse the payload. It is one layer, not a sandbox.
- **format-check.sh** (PostToolUse `Edit|Write`) — clang-format (and clang-tidy when the file is in the compile DB) on edited C++, JSON validity for presets/settings, `.clang-format`/`.clang-tidy` validity, `bash -n` on scripts and hooks; findings come back as exit-2 feedback.
- **cpp-rule-linter.sh** (PostToolUse `Edit|Write`) — warn-only heuristic grep for house rules the compiler and clang-tidy miss; advisory `additionalContext`, always exit 0.
- **diff-secret-scan.sh** (Stop + SubagentStop) — scans uncommitted changes for likely secrets (gitleaks when installed, plus built-in patterns); SubagentStop scans only that subagent's worktree, Stop scans the main checkout and every worktree. Names path:line and rule, never the value.
- **lib.sh** — shared helpers (payload parsing, repo root from the edited file, the `low_prio` wrapper); not wired to an event.

## No MCP (intentional)
Everything here is CLI and exit-coded, so there is no `.mcp.json`. Capabilities (docker, cross toolchains, CI results) are probed from Bash. Never commit a capability MCP server.

## Conventions
- Match existing patterns: read neighbours before writing. Kebab-case long flags; `snake_case` functions and variables; trailing `_` on private data members; `<proj>` namespace (see `modern-cpp-conventions`).
- Small, reviewable diffs. Note out-of-scope improvements for the right agent rather than doing them inline.
- clang-format owns formatting and include order; don't hand-sort or fight it.

## Project specifics
Filled by `/setup` on 2026-10-17; agents read them at use-time.
- Tool name (binary/CLI name, `<tool>` → `@TOOL@`): `mangadex-dl`
- Namespace / CMake project (`<proj>` → `@PROJ@`) and option prefix: `mangadex_dl` / `MANGADEX_DL_`
- Description (one line): Downloads volumes and chapters from Mangadex and converts them into .epub or .pdf files.
- License (SPDX identifier): `GPL-3.0-or-later`
- Subcommands, and the exit codes 3–63 in use (`include/mangadex_dl/exit_codes.hpp`): `hello` (scaffold example); no tool-specific exit codes yet
- Threaded? (enables the `tsan` preset/task and `concurrency-mt-unsafe`): **yes** (planned parallel downloads). `tsan` preset + pixi task enabled, `concurrency-mt-unsafe` on in `.clang-tidy`, CI runs `tsan` in both envs.
- SIGPIPE policy: A (default)
- Optional tiers / tasks enabled (cov, tsan, fuzz, man, completions, audit tools): `cov` (scaffold default), `tsan`; `audit` runs in CI via `pixi exec` (syft/grype absent locally). fuzz, man, completions: off.
- Compilers and minimums confirmed at `/setup` (from `pixi list` + the feature probe): clang 23.1.3 (`default`), gcc 16.2.0 (`gcc`), both on libstdc++ 16; all required C++23 `__cpp_lib_*` macros present in both
- Supported platforms beyond Linux (CI-only; licensed only by CI results): none
- Packaging (TGZ; AUR yes/no; release builder pixi or manylinux): TGZ, no AUR, pixi
- glibc floor (`MANGADEX_DL_GLIBC_FLOOR`): 2.28 (release binary needed GLIBC_2.26 max at /setup)
- pixi cache dir note: none (cache `~/.cache/rattler/cache`, `$HOME` and repo are ext4; no ecryptfs/NFS)
- clang-format style (default LLVM, indent 4, column 100, InsertBraces): default
- Docker images for platform smoke runs (you pull them yourself): none (docker not installed)
- Extra toolchain presets (mingw / aarch64), if any: none
- GitHub remote + default (base) branch: no remote configured yet; default branch `main`

Claude Code formats evolve: if your version differs, run `/agents` to confirm every subagent loaded and `/help` to confirm the commands.
