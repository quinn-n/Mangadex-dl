---
name: fix
description: "User-invoked orchestration command (/fix) that diagnoses and fixes a bug, failing command, sanitizer report or build error in this C++ CLI. It classifies the problem and routes build/link/toolchain/CTest/E2E-fixture/release-check/CI breakage to build-fixer, and behaviour/test/crash/sanitizer findings to bug-fixer. The fixer adds a regression test, proves the fix with `nice pixi run check`, and memory-safety-reviewer reviews automatically when C++ changed. Invoke it explicitly with the symptom. Claude never auto-runs it."
argument-hint: "<bug description | failing command | sanitizer report | build error>"
disable-model-invocation: true
---

Fix: **$ARGUMENTS**

## 1. Classify
Classify the problem, quoting the evidence (the error text, the failing command and its exit code).

**→ `build-fixer`** for:
- pixi solve or install failures, including a stale lock under `--locked`, or `File name too long` on ecryptfs (advise only; never set the cache variables);
- configure failures and activation flag leaks;
- compile-flag and warning-gating failures;
- link failures and runtime mixing;
- lint infrastructure: 0 files, a DB/tidy major mismatch, script paths;
- CTest discovery;
- the E2E install fixture;
- `release-check` failures: RPATH, glibc floor, readelf markers;
- CI.

**→ `bug-fixer`** for:
- wrong behaviour;
- a failing unit, CLI or E2E test;
- a crash;
- any sanitizer report: ASan, UBSan, LSan, or a sanitizer marker on stderr;
- exits of 70/134/139.

If it's ambiguous (for example, a sanitizer crash during CTest discovery), say which way you're routing and why.

## 2. Fix with a regression test
**`bug-fixer`:**
1. Reproduce with the exact installed-binary command (`build/default/dev/e2e-prefix/bin/<tool> …`), or with `nice pixi run test -R <name>` plus the `--rng-seed` from the log.
2. Root-cause it.
3. Write a **failing regression test at the right tier**: unit, in-process CLI, E2E case, edge-suite or sanitizer control.
4. Make a minimal fix.
5. Prove it with the now-passing test and `nice pixi run check`.

It never weakens a test, suppresses a sanitizer finding, or `NOLINT`s away a signal.

**`build-fixer`:**
1. Make a minimal build-surface change.
2. Prove it with `nice pixi run check`, adding `nice pixi run -e gcc check` when the fix is compiler-specific and `nice pixi run release-test` when it touches the release surface.
3. If the fix needs a version re-pin, stop and send it back for planning and fact-checking.

**Goldens.** If the fix intentionally changes a golden output, run the golden re-bless gate:
1. Show the expected-vs-actual output.
2. ⏸ Wait for my approval.
3. Run `nice pixi run bless <regex>` (in `ask`).
4. Show `git diff -- tests/e2e/golden/`.

Never edit a golden by hand, and never set `UPDATE_GOLDEN`.

## 3. Review
- **If any C++ changed, `memory-safety-reviewer` runs automatically.**
- **If `build-fixer` made the change, `quality-reviewer` runs automatically in build-review mode.**

Then **ask me** whether to also run `quality-reviewer` (if it hasn't run) and `security-reviewer`.

## 4. Loop
On **VERDICT: CHANGES REQUESTED**:
1. The same fixer addresses the Must-fix items with a change log.
2. It re-runs `nice pixi run check`.
3. The reviewer re-reviews.

**Cap: 3 rounds.** Then ⏸ surface the disagreement.

## 5. Report
Report: symptom/repro → classification → root cause → fix → regression test (tier + name) → gate evidence → reviewer verdicts → related risks.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `nice pixi run …`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and no piping gate output through `tail`/`head`.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Gate runs (`check`, `test`, `release-test`, `lint`, `-e gcc check`) get an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` with a wait for completion.
  - A run killed by the tool timeout is not a failure.
  - The build lock self-recovers from a dead owner.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
  - `bug-fixer`'s gdb recipe from the `memory-safety` skill (`gdb -batch -ex 'set environment ASAN_OPTIONS=…' …`) is the only place sanitizer options are set ad hoc. It uses gdb's `set environment`, not a command prefix, and it prompts by design.
  - Never weaken `*SAN_OPTIONS` with `exitcode=0` or `halt_on_error=0`; `guard-bash` blocks those.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** on a blocker or a non-converging loop.
