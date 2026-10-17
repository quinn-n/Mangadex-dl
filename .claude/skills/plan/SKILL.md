---
name: plan
description: "User-invoked orchestration command (/plan) that designs, plans and fact-checks a C++ CLI feature, then stops before any code is written so you can review it. It runs cli-designer, then implementation-planner, then a fan-out of fact-checkers against the installed pixi env with a refute loop. It returns a tier-tagged spec, a batched plan with Owns sets and serialization points, and a verified fact-check report. Invoke it explicitly with a feature description. Claude never auto-runs it."
argument-hint: "<feature / command / flag description>"
disable-model-invocation: true
---

Produce a verified, ready-to-build plan for: **$ARGUMENTS**

## 1. Design
Delegate to `cli-designer`. It produces the command spec:
- the CLI11 surface;
- streams;
- `--json` keys;
- exit codes (new 3–63 codes allocated explicitly);
- edge cases;
- **acceptance criteria tagged with their verifying tier** (`unit | cli | e2e | edge | tty-pty | human`).

It must state the config/env precedence: CLI11's real order is CLI > config > env > default.

⏸ Present the spec and wait for approval or edits.

## 2. Plan
Delegate to `implementation-planner` with the approved spec. The plan includes:
- the approach;
- **dependency and build changes:** conda-forge via `pixi add`, `find_package(... CONFIG REQUIRED)` plus imported targets, and FetchContent only as a documented exception;
- all `[VERIFY]` assumptions, **grouped into clusters**;
- per-task `Goal` / `Files` / **`Owns`** / `Done when` / tests per tier;
- a **Parallelization plan** of ordered batches;
- the per-tier test plan: unit, in-process CLI, E2E cases, edge-suite and sanitizer-control additions, release-test;
- the memory-safety watch-list;
- rollout and risk.

**Serialization points never go in two tracks of one batch:**
- top-level `CMakeLists.txt`;
- `CMakePresets.json`;
- `pixi.toml` + `pixi.lock`;
- `src/app/cli.cpp`;
- shared headers under `include/<proj>/`;
- `tests/CMakeLists.txt`;
- `scripts/*`;
- `.github/workflows/*`.

Registration in `cli.cpp` is its own late task.

## 3. Fact-check (fan out)
In a single turn, launch **one `fact-checker` per verification cluster** (multiple Task calls), each given only its cluster's items. Each resolves its items in this evidence order:
1. **The installed pixi env:**
   - `pixi list` / `pixi tree` / `pixi search`;
   - headers under `.pixi/envs/<env>/include`;
   - `pixi run cmake --help*`;
   - `pixi run clang-tidy --list-checks*`;
   - `pixi run clang-format --dump-config*`.
2. **Probes:** `nice pixi run cxx-probe <file>`, for probe files under `.platform-runs/` or the `platform-testing` skill's `probes/` dir.
3. **Current docs.**

Refuted items go back to `implementation-planner` for revision. Then re-check only the affected items, until the plan rests only on confirmed facts.

## 4. Present
Present:
- the final design;
- the plan;
- the per-tier test plan;
- the parallelization plan;
- the fact-check report (Claim / Verdict / Evidence / Fix).

Do **not** implement. Stop here for my review; `/implement` takes it from here.

Give a one-paragraph status after each phase.

**Execution rules**
- **Low priority.** Run heavy commands, including `cxx-probe` and any build a fact-checker triggers, at low OS priority locally as `nice pixi run …`.
  - Use bare `nice`, never in CI, never swallowing the exit code.
  - See the `low-priority-execution` skill.
- **Long runs.** A probe that configures a cold build dir can exceed the 120 s Bash default. Give it an explicit long `timeout` (up to 600000 ms) or `run_in_background` and wait for completion.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** on a blocker, or if the refute loop doesn't converge within ~3 rounds.
