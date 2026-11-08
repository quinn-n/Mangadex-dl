---
name: stacked-prs
description: How this studio splits a large C++ CLI feature into a stack of small PRs instead of one big one. Covers the slicing rules the implementation-planner follows for its PR slices section, the size threshold, building the stack from the approved worktree tree by path, gating every lower slice tip with nice pixi run check, the bottom-up gh pr create chain and its stack-map PR bodies, the restack after a squash merge, and use-time detection of gh stack, Graphite and git-spice. Use when planning a feature's PR slices, opening PRs in /features, or restacking a stack after its bottom PR merged.
---

# Stacked PRs

A big feature reviewed as one PR gets a worse review. Google's small-CL guide says "100 lines is usually a reasonable size for a CL, and 1000 lines is usually too large". SmartBear reports that reviews of 200–400 lines find most defects (SmartBear/Cisco 2006, vendor-reported). So a feature over the threshold ships as a **stack**: an ordered chain of small PRs. Each PR is based on the one below it, is reviewable alone, and is safe to merge in order.

Stacking happens **when possible, never by force**. A small feature, or one that won't split cleanly, ships as one PR exactly as before.

The `/features` command (`.claude/skills/features/SKILL.md`, §4) is where PRs are opened. `/feature` and `/plan` don't open PRs, but their final-plan ⏸ gate shows the slices, so the slicing is approved before any code is written.

## 1. The planner proposes slices
The `implementation-planner` ends its plan with a `### PR slices` section. Each slice in it lists:
- a number;
- a one-line purpose, such as "core parser + unit tests";
- its tasks (T-ids);
- the exact paths it changes.

If the feature shouldn't stack, the section is the single line `Single PR — <reason>`. Valid reasons include "small", "one tightly-coupled change", and "no slice is safe alone". You approve the slicing at the final-plan ⏸ gate, alongside the batches.

Slice rules:
- **Path-disjoint.** Each file's whole change lives in exactly one slice, because slices are committed by path. If two slices need the same file, merge them.
  - A rename or deletion lists **both** the old and the new path in the same slice, or `git add -A -- <paths>` stages only half of it.
  - A file lower code compiles against (a new header under `include/<proj>/`, a new exit code in `include/<proj>/exit_codes.hpp`) goes in the **lowest** slice that needs it.
  - A registration list (`src/app/cli.cpp`, `tests/e2e/cases/_edge.cmake`) goes in the **highest** slice that touches it, with the surface it registers.
- **Ordered by dependency.** Slice k may depend only on slices below it. Land plumbing before the surface that uses it. A typical order for this stack:
  1. build-surface changes the feature needs (CMake options, presets, `pixi.toml` + `pixi.lock`), if any;
  2. core logic in `<proj>_core` (`include/<proj>/`, `src/core/`) with its unit tests;
  3. the subcommand surface: `src/app/cmd_<sub>.cpp`, its registration in `src/app/cli.cpp`, in-process CLI tests, E2E cases and the goldens they need;
  4. docs, completions, man page.
- **Green and safe alone.** Every slice tip must pass the gate and leave the tool working:
  - no `#include` of a header that doesn't exist yet;
  - no registered subcommand or flag whose handler isn't there;
  - no half-built option, `--json` key or exit code reachable from the command line.

  A slice that can't stand alone merges with its neighbour.
- **Goldens travel with their output.** A golden under `tests/e2e/golden/` goes in the same slice as the code that changes that output, or that tip's `check` fails. `--help` goldens usually ride with the `cli.cpp` registration.
- **Dependencies travel together.** A `pixi.toml` change and its `pixi.lock` change share a slice, along with the `find_package` call that consumes it. That slice is the lowest one that needs the dependency.
- **Review slices are not execution batches.** Batches group file-disjoint tasks for concurrency. Slices group reviewable units for merging. The two needn't match.

## 2. Decide at PR time
Work in the feature's worktree, on `feature/<slug>`.

**First pin the start sha.** Run `git merge-base <base-branch> HEAD` and write the printed sha down as a literal `<start-sha>`; shell variables don't persist between Bash calls. Use it, never the live base ref, for the measurement and for every rebuild reset. If the base advances mid-run, a diff or reset against `<base-branch>` would silently count or revert the base's new commits.

The pipeline leaves the finished work uncommitted, so stage it all and measure the staged diff against `<start-sha>`, excluding paths that inflate size without adding review load:
```bash
git add -A
git diff --cached --numstat <start-sha> -- . ':(exclude)pixi.lock' ':(exclude)tests/e2e/golden/**' ':(exclude)third_party/**'
```
Sum the added + deleted columns; binary files show `-`, so count them as 0. Also exclude any other generated or vendored path this repo has.

**Stack only if** the total is over the **stacked-PR threshold** in `CLAUDE.md` → Project specifics (read it at use-time) **and** the approved plan has ≥ 2 slices. The threshold defaults to 400 changed lines; `never` turns stacking off. Otherwise commit and open one PR, as `/features` §4 describes.

## 3. Build the stack from the approved tree
The cli-tester, platform-tester and review board approved one finished tree. The stack must be that tree, cut into commits; nothing is re-implemented. Start by unstaging, which leaves the working tree untouched: `git reset --quiet`.

Expect permission prompts: `git add`, `git commit`, `git switch`, `git reset` and `git branch` aren't allowlisted, so they prompt per slice. That's intended; don't add allow rules for them.

1. **Commit slice by slice, by path.** For k = 1 … n:
   ```bash
   git add -A -- <slice k paths>
   git commit -m "<feature> (k/n): <purpose>"
   ```
   Never use `--no-verify`. After each lower slice (k < n), create its branch: `git branch feature/<slug>-pt<k>-<slice-slug>`. The top slice stays on `feature/<slug>`.
2. **Prove nothing was left behind.** `git status --porcelain` must print nothing. If it prints anything, a path is missing from the slices. Add it to the slice that needs it, then rebuild from step 5's reset. Ignored files (`build/`, `.pixi/`, `CMakeUserPresets.json`, `.platform-runs/`, `compile_commands.json`, all in `info/exclude`) don't show, which is correct.
3. **Gate every lower tip** in the feature's own worktree, where the pixi env and the `-local` presets with this worktree's job cap already exist:
   ```bash
   git switch --detach feature/<slug>-pt<k>-<slice-slug>
   nice pixi run check
   git switch feature/<slug>
   ```
   - **Always switch back**, with `git switch feature/<slug>`, even when the gate fails, so the worktree is never left on a detached HEAD.
   - Add `nice pixi run -e gcc check` when the gcc env is installed in this worktree, exactly as the pipeline's gate does.
   - If that tip's `pixi.lock` differs from the top's, run `pixi install --locked` (plus `-e gcc` if installed) after switching to the tip and again after switching back.
   - Each run is a full `check` that rebuilds the changed sources: give it a long Bash `timeout` (up to 600000 ms) or `run_in_background` and wait. A run killed by the tool timeout is not a failure, but it isn't a pass either: rerun it with a longer timeout until it finishes, and never treat that tip as gated until it has. Never pipe it through `tail`/`head`. See `low-priority-execution`.
   - This is the full gate (`fmt-check` → `test` → `lint` → `release`), which is what catches a lower slice including a header that only exists higher up.
   - The top tip is the tree the pipeline already gated.
4. **A failing tip folds upward.** Merge that slice into the one above it, then rebuild.
5. **Rebuild** from scratch with `git reset --mixed <start-sha>`, which uncommits everything and leaves the working tree untouched. Then delete only the `feature/<slug>-pt<k>-<slice-slug>` branches this run created, one `git branch -D` per branch by full name, never any other branch, and redo step 1. If only one slice remains, open one PR.
6. **Evidence is per tip.** Lower tips carry **gate evidence only**: `check` (and `-e gcc check` when run). The `cli-tester`, `platform-tester` and the review board (`security-reviewer`, `cli-ux-reviewer`, `quality-reviewer`, `memory-safety-reviewer`) ran on the top tip. Never claim more for a lower PR.

## 4. Open the stack
- **Push every branch first.** `gh pr create` won't push in non-interactive mode:
  ```bash
  git push -u origin feature/<slug>-pt1-<slice-slug> … feature/<slug>
  ```
- **Open the PRs bottom-up.** PR 1 targets the base branch, and PR k targets slice k−1's branch:
  ```bash
  gh pr create --base <parent-branch> --head <slice-branch> --title "<feature> (k/n): <purpose>" --body-file <file>
  ```
- **Each body opens with a stack map.** List all n slices by **branch name**, mark this one, and add one line: "Merge bottom-up; review only this PR's diff." Use branch names, not URLs, because lower PRs are opened before upper ones exist. After the map come what this slice does, any pixi deps or CMake/preset changes it carries, and its tip's gate result.
- **The top PR carries the feature-level body.** Review rounds for both track reviewers, board verdicts, the platform tier grid and goldens blessed all describe the top tip, so they go on the top PR only.
- **The `## User Test Plan` lives on the top PR.** The top tip is the only one where the whole feature runs, so it carries the full plan. Each lower PR ends with the explicit empty case: "Nothing required for this slice alone — `nice pixi run check` green at its tip; the feature's hands-on checks are on `<feature> (n/n)`."
- **Without `gh`,** print one compare URL per slice:
  ```text
  https://github.com/<owner>/<repo>/compare/<parent-branch>...<slice-branch>?expand=1
  ```
- **Stacks never cross features.** Each feature in a `/features` run gets its own stack, and no PR names a sibling feature.

## 5. Stack tooling: detect it, never install it
`/features` probes once at preflight, from the base checkout, and reports what it found at its first ⏸ gate:
- **GitHub native stacks.** `gh stack --help` succeeds when the extension is installed.
  - They're in public preview (since 2026-07-30, so they may change) and need `gh` ≥ 2.90.
  - They make merging stack-aware: after a merge, the next PR retargets and rebases automatically, and merge queues take the stack in order.
  - If present, offer to submit the same slices with it, as the recommended option. Read `gh stack --help` at use-time for how it takes titles and bodies. Hardcode nothing beyond `init`, `add`, `push`, `submit`, `view` and `rebase`.
- **Graphite.** The repo is set up when `test -e "$(git rev-parse --git-path .graphite_repo_config)"` succeeds. If you use it, never run plain `git rebase` or `git branch -d` on its branches.
- **git-spice.** It's set up when `git show-ref --verify --quiet refs/spice/data` succeeds. A missing ref doesn't prove the repo doesn't use it.
- **None found.** Use plain git + `gh` as above. It always works. Never install a stack tool.

## 6. Merging and restacking (yours, with help on request)
The studio **never merges**. Merge bottom-up. With native `gh stack`, its merge handles the rest. With plain git, after the bottom PR merges:
- **Merge commit.** Retarget the next PR to the base: `gh pr edit <next-pr> --base <base-branch>`. Don't rely on GitHub's auto-retarget, which fires when the merged branch is deleted.
- **Squash or rebase merge.** The next branch still carries the original commits. Note the old bottom tip's sha **before** merging. Then:
  ```bash
  git fetch origin
  git rebase --onto origin/<base-branch> <old-bottom-tip-sha> <next-branch>
  git push --force-with-lease origin <next-branch>
  gh pr edit <next-pr> --base <base-branch>
  ```
  Repeat up the stack, each time using the just-rebased branch's old tip. With git ≥ 2.38, `git rebase --update-refs` moves the intermediate branches in one pass. Git won't rebase a branch checked out in another worktree, and `--update-refs` **skips any branch checked out in another worktree**, so restack from inside the feature's worktree, or after it's removed.

A restack force-pushes your branches and retargets your PRs. Ask for one, and the studio does it only after you confirm each push. `guard-bash` still blocks any force-push to main/master or the default branch, and `git push` and `gh pr edit` stay in `ask`.
