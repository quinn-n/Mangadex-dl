---
name: harden
description: "User-invoked orchestration command (/harden) that improves existing C++ CLI code without changing behaviour. It requires a green baseline gate first, then backfills tests across tiers. It then runs extended verification (release-test, an extended UB pass, TSan if threaded, coverage, audit, a local platform pass) and a parallel security, CLI-UX and memory-safety review. A refactor follows, findings are routed to fixers, and the gate is re-run at the end. Invoke it explicitly with an area, or blank for the current diff. Claude never auto-runs it."
argument-hint: "[<file / area to harden> — omit for the current diff]"
disable-model-invocation: true
---

Harden the target: **$ARGUMENTS**. The default is the current diff and changed files.

Run the steps below in order. The baseline `nice pixi run check` must be green first (step 1). The gate is re-run after every fix loop and once more at the end (step 7).

## 1. Baseline
Run `nice pixi run check`. It **must be green before anything else.**
- If it's red, stop and offer `/fix` instead. Hardening on a red baseline hides regressions.
- If `.pixi/envs/gcc` exists, also run `nice pixi run -e gcc check`.

## 2. Backfill tests
Delegate to `test-engineer`. It adds meaningful tests across tiers, so the refactors and fixes below are safe:
- unit;
- in-process CLI;
- E2E cases;
- edge-suite items;
- sanitizer controls.

It edits `tests/**` only and **never blesses goldens**. New golden cases become a bless request (step 6).

## 3. Extended verification (sequential; each is report-only unless it finds a defect)
Run these one at a time, each at low priority, so heavy builds don't overlap.
1. **Release tests:** `nice pixi run release-test`. This covers unit + E2E + `release-check` against the hardened release install. The release build links static libstdc++ (app target only, via CMakeLists when `<PROJ>_HARDENING` is ON; test executables stay dynamic). The release-check gates are: no RPATH, NEEDED allowlist, glibc floor, readelf markers.
2. **Extended UB pass.** Only if the project configured the opt-in extended sanitizer set (`float-divide-by-zero`, `implicit-conversion`, `unsigned-integer-overflow`, `local-bounds`; see the `memory-safety` skill). Run it exactly as that skill documents, with `nice`. Otherwise note "not configured".
3. **TSan.** Only if `CLAUDE.md` → Project specifics says the tool is threaded **and** a `tsan` task/preset exists: `nice pixi run tsan`. Otherwise record the reason:
   - not threaded → T3;
   - preset absent → R9.
4. **Coverage:** `nice pixi run cov`. It is report-only; use it to point `test-engineer` at untested risky paths.
5. **Audit:** `nice pixi run audit`. Quote its **coverage statement** verbatim, including what it does *not* cover. If syft/grype are absent, it is a stated no-op.
6. **Platform pass:** `platform-tester` with scope `local`. It is non-blocking, emits no verdict, and returns a tier per target.
   - A failure on a present target (for example, the gcc release doesn't build `-Werror`-clean) is a finding for step 6.
   - An absent capability is a reason, not a failure.

`cppcheck` is advisory and runs only if present: `command -v cppcheck`.

## 4. Review (parallel, read-only)
Pass the step 3 results to these reviewers, running in parallel:
- `security-reviewer`;
- `cli-ux-reviewer`, on the installed binary;
- `memory-safety-reviewer`, with sanitizer, coverage and valgrind evidence where present.

Each returns a **VERDICT** with a Must-fix list.

## 5. Refactor
Delegate to `code-refactorer`. It improves structure and readability with **no behaviour change**:
- the gate, including release `-Werror`, is green before and after;
- goldens are untouched.

Its C++ changes go through `quality-reviewer` + `memory-safety-reviewer` until both APPROVE.

## 6. Route the fixes
Route each finding:
- security and logic fixes → `implementer`;
- runtime, sanitizer and crash defects → `bug-fixer`;
- build, link, release-check and gcc-only defects → `build-fixer` (reviewed by `quality-reviewer` in build-review mode);
- help-text, message and format-only fixes → `cli-ux-reviewer`.

**Loops.** Every C++ change loops through `quality-reviewer` + `memory-safety-reviewer` until both APPROVE, and the flagging reviewer re-reviews its own items. The cap is ~3 rounds; ⏸ surface disagreements.

**Goldens.** Intended golden changes go through the golden re-bless gate:
1. Show the expected-vs-actual output.
2. ⏸ Wait for my approval.
3. Run `nice pixi run bless <regex>`.
4. Show `git diff -- tests/e2e/golden/`.

## 7. Re-gate and summarize
Run `nice pixi run check` and `nice pixi run release-test`, plus `nice pixi run -e gcc check` if that env is installed. All must be green.

Summarize:
- tests added per tier;
- the extended-verification results: release-test, extended UB, TSan, the coverage numbers, and the audit coverage statement;
- the platform tier grid;
- reviewer verdicts and rounds;
- refactors by theme;
- fixes, with who made them;
- goldens blessed;
- what was left out of scope, and why.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `nice pixi run …`: `check`, `-e gcc check`, `release-test`, `cov`, `audit`, `tsan`, `bless`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and no piping gate output through `tail`/`head`.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Every gate run (`check`, `release-test`, `cov`, `lint`, `-e gcc check`) gets an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` with a wait for completion. A cold `check` routinely exceeds the 120 s default.
  - A run killed by the tool timeout is not a test failure.
  - The build lock self-recovers from a dead owner.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** on a blocker or a non-converging loop.
