---
name: review
description: "User-invoked orchestration command (/review) that runs the full C++ CLI review board on the current diff, or on a path or git range you give it. It runs `nice pixi run check` once, then quality-reviewer, security-reviewer, memory-safety-reviewer and cli-ux-reviewer in parallel, and consolidates their verdicts into Must / Should / Nits with file:line and the flagging agent. It does not change code. Invoke it explicitly. Claude never auto-runs it."
argument-hint: "[<path | git range> — omit for the current diff]"
disable-model-invocation: true
---

Review: **$ARGUMENTS**. If no argument was given, review the current changes (`git diff` plus staged).

## 1. Scope
Resolve the scope to a file list and a diff:
- a path → that path's changes;
- a git range → `git diff <range>`;
- empty → the working-tree + staged diff.

Note two things about the scope:
- **Does it touch the build surface?** That means CMake files, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `scripts/` or `.github/`. If so, `quality-reviewer` uses **build-review mode**.
- **Does it touch C++?**

## 2. Run the gate once
Run `nice pixi run check` **yourself, once**:
- default env;
- `fmt-check` → `test` → `lint` → `release` → `binary-packages`;
- with an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` and wait.

Capture:
- the exit status;
- which step failed;
- the failing tests;
- any sanitizer markers;
- the lint file count.

If `.pixi/envs/gcc` exists and the scope touches C++ or the build surface, also run `nice pixi run -e gcc check`. Otherwise note that CI covers it.

## 3. Review board in parallel
Pass the gate results to all four reviewers, and tell them the full gate has **already run**: they re-run only **targeted** tasks, so four heavy gates never run at once.
- **`quality-reviewer`** reviews correctness, the CLI contract, tests (right tier, exact exit codes, no `PASS_REGULAR_EXPRESSION`/`WILL_FAIL` for contract cases), modern-C++ conventions, compile-time hygiene and NOLINT hygiene.
  - In build-review mode it also reviews target-scoped flags, `find_package`-only, no global `-Werror`, explicit preset flags, no committed `jobs`, no `*_PARALLEL_LEVEL`, install destinations and CI pinning.
  - Any gate failure from step 2 is a Must-fix.
- **`security-reviewer`** runs `nice pixi run audit` and quotes its coverage statement. It reviews process spawning, fds, temp files, paths/TOCTOU, format strings, size math, env, secrets and supply chain.
- **`memory-safety-reviewer`** reviews ownership, lifetime and UB against the hazard table:
  - CLI11 bound variables;
  - `string_view`s to temporaries;
  - escaping `[&]`;
  - suppression diffs.

  It cites sanitizer evidence: whether the `sanitizer-control` tests fired, and which changed paths were exercised (targeted `nice pixi run test -R <affected>`).
- **`cli-ux-reviewer`** reviews help, errors, defaults, output/stream design and scriptability on the **installed binary** (`build/default/dev/e2e-prefix/bin/<tool>`).

Each returns **VERDICT: APPROVED** or **VERDICT: CHANGES REQUESTED**.

## 4. Consolidate
Produce one prioritized list:
- **Must fix**
- **Should fix**
- **Nits**

Give each item file:line and a concrete fix, and note which agent flagged it. Put the four verdicts and the gate results at the top.

**Don't change code** unless I ask. That includes `cli-ux-reviewer`, which may edit text only when I ask for fixes.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `nice pixi run check` / `nice pixi run -e gcc check` / `nice pixi run test -R …` / `nice pixi run audit`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and no piping gate output through `tail`/`head`.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Gate runs need the long `timeout` or `run_in_background`. A run killed by the tool timeout is not a failure.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** on a blocker, for example the gate can't run because the env isn't installed.
