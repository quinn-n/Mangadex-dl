---
name: implement
description: "User-invoked orchestration command (/implement) that executes an already-approved C++ CLI plan. It runs parallel file-disjoint implementer batches, each track looping with quality-reviewer and memory-safety-reviewer until both APPROVE, with build-fixer routing for build/link failures. Then it runs the installed-binary cli-tester, an optional platform pass, the review board and fixes, and ends with a summary and a User Test Plan. Invoke it explicitly with the plan. Claude never auto-runs it."
argument-hint: "<plan file path or description of the approved plan>"
disable-model-invocation: true
---

Execute the approved plan: **$ARGUMENTS**

## 1. Read the plan's Parallelization plan
Read the plan's execution batches and per-task `Owns` sets.
- **If the plan has none,** derive batches from dependencies and file ownership.
- **Two tasks may share a batch only if** they are independent, file-disjoint, and neither touches a **serialization point**:
  - top-level `CMakeLists.txt`;
  - `CMakePresets.json`;
  - `pixi.toml` + `pixi.lock`;
  - `src/app/cli.cpp`;
  - shared headers under `include/<proj>/`;
  - `tests/CMakeLists.txt`;
  - `scripts/*`;
  - `.github/workflows/*`.
- **If everything is interdependent,** run a single sequential batch.

## 2. Per batch, in order
Launch **one `implementer` per independent task, in parallel** (multiple Task calls in one turn). Scope each to its `Owns` files and this repo's absolute path.

**Dual review loop per track**
1. The `implementer` writes the C++23 code plus the tests per tier, iterates with `nice pixi run test` / `nice pixi run lint`, and gets `nice pixi run check` green.
2. In parallel, `quality-reviewer` and `memory-safety-reviewer` review the track.
   - `quality-reviewer` runs the full gate.
   - `memory-safety-reviewer` runs targeted tests (`nice pixi run test -R <affected>`) and reads the latest results.
   - Each returns **VERDICT: APPROVED** or **VERDICT: CHANGES REQUESTED**.
3. If either requests changes, the **same** implementer addresses **both** Must-fix lists in one round, with one change log, and re-runs `nice pixi run check`.
4. Both reviewers re-review.
5. **Both must APPROVE.** The cap is ~3 **combined** rounds.
   - ⏸ Surface a disagreement or a 4th round to me.

**Routing and coordination**
- **Build/link/toolchain failures** inside a track (pixi solve, configure, compile flags, link, lint infrastructure, CTest discovery) go to `build-fixer`.
  - It is scoped to the track's `Owns` plus the build files the plan designates.
  - `quality-reviewer` reviews it in build-review mode.
  - Anything else is a coordination point for you.
- **Shared build dir.** Tracks share one build dir per worktree, serialized by the `preset.sh` lock. A compile error in a file **another track owns** is a coordination event: re-sequence the tracks rather than "fixing" someone else's file.
- **Goldens are human-blessed.** Implementers never set `UPDATE_GOLDEN` and never edit an existing golden. For a **bless request** run the golden re-bless gate before review:
  1. Show the expected-vs-actual output with `nice pixi run test -R <case regex>`.
  2. Say why the change is intended.
  3. ⏸ Wait for my approval.
  4. Run `nice pixi run bless <regex>`. It is in `ask`.
  5. Show `git diff -- tests/e2e/golden/`.
  6. Re-run the affected tests.

**Integration.** When all tracks in the batch are APPROVED:
1. Integrate the shared touch-points serially: registration in `cli.cpp`, `pixi add` by one track only, `tests/CMakeLists.txt`.
2. Run `nice pixi run check`.
3. Only then start the next batch.

After the last batch, also run `nice pixi run -e gcc check` if `.pixi/envs/gcc` exists.
- A failure there goes to `build-fixer` if it is compiler-specific, or to the owning implementer otherwise.
- If the gcc env isn't installed, say so. CI covers it.

## 3. Installed-binary test
Delegate to `cli-tester` on the integrated feature. It returns:
- ✅/❌ per acceptance criterion;
- defects, including any sanitizer marker on stderr and any exit of 70/134/139;
- its "Could not verify from here" line.

## 4. Optional platform pass
Delegate to `platform-tester` with scope `local`, unless I said to skip it.
- It is non-blocking and emits no verdict. A missing capability is a no-op with a stated reason.
- A failure on a present target is a defect for step 6.

## 5. Review board
Run these in parallel:
- `security-reviewer` (runs `nice pixi run audit`);
- `cli-ux-reviewer` (on the installed binary);
- a **final** `quality-reviewer` (full gate; build-review mode if the build surface changed);
- a **final** `memory-safety-reviewer`.

For any CHANGES REQUESTED, loop until APPROVED:
- **Routing:**
  - logic → `implementer`;
  - runtime → `bug-fixer`;
  - build → `build-fixer`;
  - help-text, message and format-only fixes → `cli-ux-reviewer`.
- Every C++ change goes back through `quality-reviewer` + `memory-safety-reviewer`.
- Golden moves go through the golden gate.
- Re-run the relevant cli-tester checks afterwards.

## 6. Fix
Route the remaining defects:
- runtime, test, crash and sanitizer defects → `bug-fixer`;
- build, link and gcc-only defects → `build-fixer`.

Loop each through `quality-reviewer` + `memory-safety-reviewer` until both APPROVE (cap ~3). Then confirm `nice pixi run check` is green.

## 7. Summary
Summarize:
- the parallel tracks;
- review rounds per track (both reviewers);
- gate results (`check`, plus `-e gcc check` or why not);
- cli-tester results;
- the platform tier grid;
- goldens blessed;
- new pixi deps and CMake/preset changes;
- follow-ups.

Close with a `## User Test Plan`, using the same derivation, five-part shape, tier tags (`[human: <reason> · retryable|terminal]`), `removed: "<item>" — licensed by <target> @ <tier>` lines, and rules as the feature pipeline's step 10 (`.claude/skills/feature/SKILL.md`). Never omit it. If nothing is needed, write one sentence naming what covered it.

Give a one-paragraph status at each batch and loop.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `nice pixi run …`: `check`, `-e gcc check`, `test`, `lint`, `release`, `release-test`, `bless`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and no piping gate output through `tail`/`head`.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Gate runs get an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` with a wait for completion. A cold `check` routinely exceeds the 120 s default.
  - A run killed by the tool timeout is not a test failure.
  - The build lock self-recovers.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** if any agent reports a blocker or a loop fails to converge.
