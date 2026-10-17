---
name: feature
description: "User-invoked orchestration command (/feature) that runs the full C++ CLI feature pipeline in the current working tree. Steps are design, plan and fact-check, then parallel implementer tracks, each with a dual quality + memory-safety review loop. After that come an installed-binary test, a conditional platform pass, the feature review board, fixes, a golden re-bless gate, and a closing User Test Plan. Invoke it explicitly with a feature or command description. Claude never auto-runs it."
argument-hint: "<feature / command / flag description>"
disable-model-invocation: true
---

Build the following CLI feature end to end: **$ARGUMENTS**

Orchestrate the specialized subagents in order, and pause for my approval at each ⏸ gate. Do not skip gates.

Everything runs through pixi:
- The quality gate is `nice pixi run check`. It chains `fmt-check` → `test` → `lint` → `release`. `test` covers the sanitized dev build plus unit, in-process CLI, installed-binary E2E, edge-suite and sanitizer-control tests. `release` is optimized, hardened and `-Werror`.
- The gate also includes `nice pixi run -e gcc check` when the gcc env is installed.
- The installed binary under test is `build/default/dev/e2e-prefix/bin/<tool>`.
- `<tool>` and `<proj>` come from `CLAUDE.md` → **Project specifics**. Read them **at use-time**.

## 1. Design
Delegate to `cli-designer`. It returns:
- the CLI11 command surface;
- streams, `--json` keys and exit codes (new 3–63 codes allocated explicitly from `include/<proj>/exit_codes.hpp`);
- the edge cases and applicable edge-suite items;
- **acceptance criteria, each tagged with its verifying tier:** `unit | cli | e2e | edge | tty-pty | human`.

Make sure the spec says how config and env interact. CLI11's real precedence is **CLI > config > env > default**. If the feature wants env to beat config, the tool must apply that itself.

⏸ Present the spec and wait for approval or edits.

## 2. Plan
Delegate to `implementation-planner` with the approved design. The plan must contain:
- **Dependency and build changes.** New conda-forge deps go in via `pixi add`. `find_package(<P> CONFIG REQUIRED)` plus imported targets. FetchContent is only a documented exception.
- **`[VERIFY]` items grouped into clusters.** Typical clusters: CLI11 API, libstdc++/C++23 feature, CMake/CTest/CPack, conda-forge availability, compiler-specific warnings.
- **Per-task details:** `Goal`, `Files`, **`Owns`**, `Done when`, and tests per tier.
- **A per-tier test plan:**
  - unit;
  - in-process CLI;
  - `tests/e2e/cases/<sub>.cmake` cases;
  - edge-suite additions;
  - sanitizer-control additions;
  - release-test.
- **A memory-safety watch-list:** views, CLI11 bound variables, callbacks, iterators.
- **A Parallelization plan** of ordered execution batches with the serialization points called out. Registration in `src/app/cli.cpp` is its own late task.

## 3. Fact-check (fan out)
The `[VERIFY]` items are independent lookups. In a single turn, launch **one `fact-checker` per verification cluster** from the plan (multiple Task calls), giving each only its cluster's items.
- Each checks against the **installed pixi env** first:
  - `pixi list` / `pixi tree` / `pixi search`;
  - headers under `.pixi/envs/<env>/include`;
  - `pixi run cmake --help*`, `pixi run clang-tidy --list-checks*`;
  - `nice pixi run cxx-probe <file>` for code under `.platform-runs/`.
- After the installed env, it checks current docs.
- If the plan lists no clusters, split the items into disjoint groups yourself. Use a single fact-checker if there are only a couple.
- Merge the verdicts into one report.

If anything is **refuted**:
1. Send the plan back to `implementation-planner` to revise the affected steps.
2. Re-run only the relevant `fact-checker`(s).
3. Repeat until the plan rests only on confirmed facts.

⏸ Present the final plan, test plan and parallelization plan for approval.

## 4. Implement in parallel batches, each track gated by a dual review loop
Work through the plan's batches **in order**.

**Launching a batch**
- Launch **one `implementer` per independent task in the batch, in parallel** (multiple Task calls in one turn).
- Give each implementer only its task, its `Owns` set and this repo's absolute path. Tell it to stay strictly inside that scope.
- **Serialization points never appear in two tracks of the same batch:**
  - top-level `CMakeLists.txt`;
  - `CMakePresets.json`;
  - `pixi.toml` + `pixi.lock`;
  - `src/app/cli.cpp` (the subcommand registration list);
  - shared public headers under `include/<proj>/`;
  - `tests/CMakeLists.txt`;
  - `scripts/*`;
  - `.github/workflows/*`.
- New modules, unit tests, CLI tests and E2E cases don't touch shared CMake: sources and tests are globbed with `CONFIGURE_DEPENDS`.
- **One build dir per worktree.** Tracks in a batch share `build/<env>/<preset>`, and `scripts/preset.sh` serializes them with a lock.
- A compile error in a file **another track owns** is a coordination event, not something to fix. The implementer reports it, and you re-sequence the tracks.

**Implement↔review loop per track.** This is the core requirement: code must *pass* review, not just be written once.
1. The `implementer` writes the C++23 code plus the tests the plan specifies, iterates with `nice pixi run test` / `nice pixi run lint`, and gets `nice pixi run check` green. Then it hands off.
2. In parallel, `quality-reviewer` and `memory-safety-reviewer` review that track's diff.
   - Only `quality-reviewer` runs the full gate.
   - `memory-safety-reviewer` runs targeted tests (`nice pixi run test -R <affected>`) and reads the latest results, so two heavy gates never run at once.
   - Each returns **VERDICT: APPROVED** or **VERDICT: CHANGES REQUESTED** with Must-fix items.
3. If either requests changes, send **both** Must-fix lists to the **same** implementer. It addresses them in one round with one change log and re-runs `nice pixi run check`.
4. Both reviewers re-review.
5. **Both must APPROVE.** The ~3-round cap counts **combined** rounds.
   - ⏸ Surface a reviewer disagreement, an implementer-vs-reviewer disagreement on a Must-fix, or a 4th round to me instead of looping.
6. **Build/link/toolchain failures** inside a track go to `build-fixer`. These include pixi solve, configure, compile-flag, link, lint-infrastructure and CTest-discovery failures.
   - Scope the fix to the track's `Owns` plus the build files the plan designates.
   - `quality-reviewer` reviews it in build-review mode.
   - Anything outside that scope is a coordination point for you.
7. **Golden files are human-blessed.** An implementer never sets `UPDATE_GOLDEN` and never edits an existing file under `tests/e2e/golden/`.
   - When a `+golden` case fails because of an *intended* output change, or a new golden case needs its first golden, the implementer reports a **bless request** (regex + why).
   - Run the **golden re-bless gate** (below) before that track goes to review.

Review loops for different tracks run concurrently.

**Integrating a batch.** When every track in the batch is APPROVED:
1. Resolve the shared touch-points serially: registration in `cli.cpp`, `pixi add` by one track only, `tests/CMakeLists.txt`, shared headers.
2. Run `nice pixi run check`. Start the next batch only after it is green.

After the **last** batch:
- If `.pixi/envs/gcc` exists, also run `nice pixi run -e gcc check`. A failure there goes to `build-fixer` (compiler-specific) or the owning implementer.
- If the gcc env isn't installed, say so. CI runs it, and step 6 records it as reason R1.

**Golden re-bless gate (⏸, used whenever a bless is requested).** For each bless request:
1. Show the failing case's expected-vs-actual output (`nice pixi run test -R <case regex>`).
2. Say which acceptance criterion, ⏸ decision or reviewer note makes the change intended.
3. ⏸ Wait for my approval.
4. Run `nice pixi run bless <regex>`. It is in `ask`, so the permission prompt is my second confirmation.
5. Show `git diff -- tests/e2e/golden/`.
6. Re-run the affected tests.

An unintended golden diff is a defect, never a bless.

## 5. Installed-binary test
Delegate to `cli-tester` on the integrated feature.
- It runs `nice pixi run test` so the sanitized dev install exists. Then it drives the real installed binary through its checklist: exit codes, stream split, `/dev/full`, SIGPIPE, stdin, non-TTY/no-ANSI, `--json`, locale, precedence, SIGINT, `--dry-run`/`--yes`, pseudo-TTY via `script`.
- It returns ✅/❌ per acceptance criterion, defects, and its closing **"Could not verify from here"** line.
- Any sanitizer marker on stderr, or an exit of 70/134/139, is a defect.

## 6. Conditional platform pass (non-blocking, no verdict)
Delegate to `platform-tester` with scope `local`. It probes what is actually present:
- both pixi envs;
- sanitizer usability;
- docker images named in Project specifics;
- cross toolchains;
- valgrind.

It climbs tiers **per target** (`configured → built → smoke-ran → contract-verified → sanitizer-clean`) and returns its literal `## Platform verification` report.

- **A missing capability is a no-op with a stated reason** (R1–R11 retryable, T1–T4 terminal). It never blocks this pipeline, and the agent never installs anything.
- **A failure on a present target is a defect** and goes to step 8. Examples: gcc doesn't build `-Werror`-clean, or an E2E case fails under gcc.
- Keep the tier grid. It feeds step 10.

## 7. Feature-level review board
Run these on the integrated feature **in parallel**. Each returns a verdict.
- `security-reviewer` runs `nice pixi run audit` and quotes its coverage statement.
- `cli-ux-reviewer` works on the installed binary.
- A **final** `quality-reviewer` runs the full gate, and uses build-review mode if the feature touched CMake, presets, `pixi.toml`, `scripts/` or `.github/`.
- A **final** `memory-safety-reviewer` works from sanitizer evidence plus targeted tests.

Only `quality-reviewer` runs the full gate.

For any **CHANGES REQUESTED**, run the same loop until that reviewer APPROVES:
- **Routing:**
  - logic → `implementer`;
  - runtime defect → `bug-fixer`;
  - build surface → `build-fixer`;
  - help-text, message and format-only fixes → `cli-ux-reviewer`.
- **Every fix that changes C++ goes back through `quality-reviewer` + `memory-safety-reviewer`**, and both must APPROVE.
- Text changes that move `--help` goldens go through the golden re-bless gate.
- After fixes, re-run the relevant `cli-tester` checks.
- Same ~3-round cap. ⏸ Surface disagreements.

## 8. Fix
Route the remaining defects from steps 5–7:
- runtime, test, crash and sanitizer findings → `bug-fixer`. It reproduces on the installed binary, writes a failing regression test at the right tier, makes a minimal fix and proves it green.
- build/link/toolchain and gcc-only failures → `build-fixer`.

Loop each fix through `quality-reviewer` + `memory-safety-reviewer` until both APPROVE (cap ~3). Then re-run `nice pixi run check`.

## 9. Final golden re-bless gate
If any *intended* golden change is still pending, run the golden re-bless gate ⏸. Typical sources are cli-ux text fixes and bug fixes that changed output.

Then confirm `nice pixi run check` is green.

## 10. Summary and User Test Plan
**Summary.** Report:
- what was built;
- which tracks ran in parallel per batch;
- review rounds per track, for **both** reviewers;
- gate results: `check`, plus `-e gcc check` or why not;
- cli-tester results;
- the platform-tester tier grid;
- new pixi deps;
- CMake/preset changes;
- goldens blessed;
- outstanding Should-fix and Nit items.

**User Test Plan.** Close with a section headed exactly `## User Test Plan`. It is **always present, never omitted**: an absent section is ambiguous between "considered, nothing needed" and "forgotten".

- **Why it matters most for this CLI.** `cli-tester` deliberately drives the piped/no-TTY path, a pseudo-TTY via `script` (isatty/ANSI decisions only) and the `--yes`/`--no-input` bypasses. So **the real interactive path is exactly what stays unverified**:
  - a human typing into prompts and confirmations;
  - progress output;
  - real terminal-emulator rendering: widths and wrapping, unicode in your font, `NO_COLOR`/`TERM=dumb` in a real terminal;
  - destructive flows against real data;
  - real credentials, network and remote services;
  - shell integration: completions, and exit codes your scripts consume;
  - any OS, arch or compiler that no platform pass reached.
- **Derive it; don't brainstorm it.** The sources are:
  - acceptance criteria not marked passing (including every `human`- and `tty-pty`-tagged criterion);
  - cli-tester's "Could not verify from here";
  - the platform-tester grid ("Not covered, with reasons" and "Still human, unconditionally");
  - anything settled at a ⏸ gate;
  - reviewer items left as judgment calls.

  Never restate what the gate already asserted.
- **A platform pass narrows the list; it never deletes it.**
  - Subtract an item only if the tier reached on a named target licenses it:
    - `smoke-ran` licenses only "loads and starts";
    - `contract-verified` licenses the exercised cases on that target;
    - `sanitizer-clean` licenses "no ASan/UBSan finding on exercised paths", never "memory-safe".
  - List each subtraction as `removed: "<item>" — licensed by <target> @ <tier>`.
  - **Every remaining item ends with a tag** `[human: <reason> · retryable|terminal]`.
    - retryable means a re-run with something attached or installed would change it, e.g. "docker image not present → `/platform-test local` after you pull it".
    - terminal means a re-run changes nothing, e.g. "real-terminal rendering", or "MSVC/macOS on this Linux host — only CI licenses it".
- **Shape: five parts, the last optional.**
  1. One line on *why*: what automation couldn't reach, and why.
  2. **Get it running.** A copy-pasteable block in this repo's real vocabulary, with `<tool>` read from `CLAUDE.md` → Project specifics **at use-time**.
     - Never invent a binary name or path. Emit `<fill in: tool name>` if the specific is still a placeholder.
     - There is no checkout step: this ran in the current working tree.
     ```
     pixi install --locked
     nice pixi run test
     build/default/dev/e2e-prefix/bin/<tool from Project specifics> <args>   # real interactive terminal, not piped (sanitized dev build)
     ```
  3. **Check these.** Numbered `do X → expect Y` steps, ≤ ~5, highest-risk and interactive-terminal items first.
     - Each is traceable to an acceptance criterion, a ⏸ decision or a reviewer note.
     - Each carries its `[human: … · retryable|terminal]` tag.
  4. **Already verified — don't redo.**
     - One line naming the gate (`nice pixi run check`, plus `-e gcc check` if run) and what cli-tester covered.
     - The platform targets and tiers reached.
     - The `removed:` lines.
  5. *(optional)* **You may notice:** outstanding Should-fix/Nit items.
- **When nothing is needed, say so in one sentence naming what covered it.** Never write a bare "N/A". For example: *"Nothing required: `nice pixi run check` is green, cli-tester verified all 5 acceptance criteria including the exit-2 usage path and SIGPIPE, and this subcommand has no prompts, color or credentials."*
- **It is not an escape hatch.** Something testable-but-untested is a coverage gap: flag it as a follow-up for `test-engineer`, don't hand it to me. If the honest list runs past ~5 items, say so: the feature needed more automated coverage.
- **No credentials, tokens, real endpoints or `.platform-runs/` artifact paths.** This block may travel into a public PR body, and the diff-secret hook never sees PR bodies.
- **If this work is later committed and pushed, this block is the PR's `## User Test Plan` verbatim.** If the repo has a PR template (`.github/pull_request_template.md` or `.github/PULL_REQUEST_TEMPLATE/`), fold the section into it rather than fighting it.

Give a one-paragraph status at each batch and loop: which tracks are running, current verdicts from both reviewers, and round counts.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `nice pixi run …`: `nice pixi run check`, `nice pixi run -e gcc check`, `test`, `lint`, `release`, `release-test`, `bless`, `cxx-probe`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and don't pipe gate output through `tail`/`head`, which hides the status.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Gate runs (`check`, `test`, `release`, `release-test`, `lint`, `cov`, `-e gcc check`) get an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` with a wait for completion. A cold `check` routinely exceeds the 120 s default.
  - A run killed by the tool timeout is not a test failure. Re-run it longer.
  - The build lock self-recovers from a dead owner.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** if any agent reports a blocker, a loop fails to converge, or something surprising turns up.
