---
name: features
description: "User-invoked orchestration command (/features). It takes a Markdown file that lists several CLI features and builds each one in its own git worktree, concurrently, with its own pixi env, per-worktree job caps and a full /feature pipeline run. It then opens one PR per feature, each with a tier-tagged, self-contained User Test Plan. Gates, questions and golden re-bless requests are forwarded to you, labeled by feature. Invoke it explicitly with the file path. Claude never auto-runs it."
argument-hint: "<path/to/features.md>"
disable-model-invocation: true
---

Build **every feature listed in the Markdown file at `$ARGUMENTS`**. Each feature gets its **own git worktree** and runs the same pipeline the `/feature` command runs, **concurrently across features**:

design → plan → fact-check → parallel implement↔dual-review loops → installed-binary test → platform pass → review board → fix → golden gate

Then open **one pull request per feature**.

Worktrees give true isolation. Parallel features can't collide on:
- files;
- `pixi.toml`/`pixi.lock`;
- `src/app/cli.cpp`;
- the top-level `CMakeLists.txt`;
- each other's build dirs (`build/<env>/<preset>` lives inside each worktree, with its own lock).

Correctness still beats concurrency:
- Create worktrees serially.
- Never mutate the base checkout mid-run. The only exceptions are the idempotent `info/exclude` append (§0) and checking completed features off in the features file (§4).

## 0. Preflight (once, before any worktree)
**The features file**
- If `$ARGUMENTS` is empty or isn't a readable `.md` file, ask me for the path. Then read the file.

**Repo state**
- Capture the **base branch** (`git rev-parse --abbrev-ref HEAD`).
- Confirm a sane starting state with `git status`: clean, or intentionally dirty, and not a detached HEAD.
- If the tree is dirty, HEAD is detached, or there are multiple remotes, **stop and ask me** before creating worktrees.

**PR tooling (verify at use-time; don't assume it's present)**
- Run `gh --version` and `gh auth status`.
- Run `git remote get-url origin` and confirm it points at `github.com` (or at the host `gh` is configured for).
- If `gh` is missing or unauthenticated, or there's no GitHub remote, we still do all the work but **fall back** to pushing each branch and printing its compare URL. Tell me up front which path we're on.

**Toolchain and host**
- `pixi --version`. Abort and ask if pixi is missing; never install it.
- `nproc`.
- `command -v ccache`. ccache is optional; record whether it is present.

**Filesystem (the ecryptfs check)**
1. Find the pixi package cache dir from `pixi info` (its cache-dir line). If that dir doesn't exist yet (nothing installed so far), `stat` its nearest existing parent instead. The default cache is under `~/.cache/rattler` unless `pixi info` reports otherwise.
2. Run `stat -f -c %T <path>` on that dir, on `$HOME`, and on the repo root (which is also the parent of `../<repo>-worktrees`).
3. **If the cache dir, `$HOME` or the repo is `ecryptfs`, warn me and continue.** This is a warning, not a blocker or a gate. Tell me which paths are on ecryptfs, and:
   - **The filename cap.** ecryptfs caps filenames at ~143 bytes, so package extraction into the cache or a worktree's `.pixi/` *can* fail with `File name too long (os error 36)` if a package path exceeds it. In practice pixi usually works.
   - **The fix, if it happens.** Export `PIXI_CACHE_DIR` (which wins) or `RATTLER_CACHE_DIR` to a path on another filesystem, **in your shell profile, then restart `claude`**. I never set either variable, and `guard-bash` blocks inline assignments of them.
   - **Copies, not hard-links.** If the worktrees and the cache end up on different filesystems, each worktree's `pixi install --locked` copies packages instead of hard-linking them. That means more disk per worktree.
   - **The escape hatch.** Pixi's detached-environments setting moves envs off the repo. Its exact config key is verified at use-time through `fact-checker` (pixi docs / `pixi config --help`); don't guess it, and never change pixi config for me.

   **An actual failure still stops.** If a worktree's `pixi install --locked` (either env, §2 step 3) fails with `File name too long`, stop preparing that feature's worktree and surface it to me, labeled by feature, with the fix above. It is a worktree that can't be prepared (see the execution rules), not a warning.

**Worktree permissions**
- Tell me to run `/add-dir ../<repo>-worktrees`, or to add that path to `.claude/settings.local.json` → `permissions.additionalDirectories` (see `settings.local.json.example`).
- Without that, `cd <wt> && nice pixi run …` prompts on every call.
- Don't edit `settings.local.json` yourself; the protect hook blocks it.

**Local excludes (idempotent)**
- Append each of these lines to `$(git rev-parse --git-common-dir)/info/exclude`, **only if it isn't already present**:
  - `.platform-runs/`
  - `CMakeUserPresets.json`
  - `build/`
  - `.pixi/`
  - `compile_commands.json`
- Why it's needed: `git worktree add` checks out the last *committed* tree, so an uncommitted `.gitignore` entry never reaches a new worktree. Ignored files also keep `git worktree remove` from refusing at cleanup.

**Per-worktree job caps**
- With **N** = the number of features about to run concurrently, compute `cap = max(1, nproc / N)` (integer division) and `load = nproc`.
- If `cap = 1`, warn me that every worktree will build single-job and suggest **waves**: run the features in groups of about `nproc / 2`. ⏸ Ask whether to proceed or use waves.
- **The cap rides on `-local` user presets plus the `vendor` field** (§2), never on environment variables. Never set `CMAKE_BUILD_PARALLEL_LEVEL` or `CTEST_PARALLEL_LEVEL`: they override preset `jobs`, and `guard-bash` blocks them.
- `nice` lowers priority but doesn't cap parallelism or RAM. The caps do that.

## 1. Parse the features file
**One feature per top-level item. Auto-detect the file's structure:**
- **`##` headings:** each heading is a feature. The heading text is the *name*; the prose and bullets beneath it, up to the next `##`, are the *description*.
- **A top-level list** (numbered, bulleted, or a `- [ ]` checklist): each top-level item is a feature. The item text is the *name*; any indented lines beneath it are the *description*.
- A leading `#` title, an intro paragraph, and section headers that merely group the list are context, not features.
- `---`-separated blocks also work.

**State how you parsed the file** at the ⏸ gate below.

**Decide how you'll mark each item done**, from that same detected structure:

| Item type | Done marker |
|---|---|
| checklist `- [ ]` | flip to `- [x]` |
| numbered or plain-bulleted item | append ` — ✅ DONE` to its line |
| `##` heading | append ` — ✅ DONE` to the heading |
| `---` block | mark its title line the same way |

Items already marked done (`- [x]`, or a trailing `DONE`/`COMPLETE`/`✅`) are **already built: skip them** unless I ask otherwise.

**Names.** For each feature, derive a **slug** from its name: lowercase, kebab-case, punctuation stripped, ≤ ~40 chars.
- Branch = `feature/<slug>`.
- Worktree dir = `../<repo>-worktrees/<slug>`.
- De-duplicate collisions with `-2`, `-3`.

⏸ **Present the parsed list and wait for my approval.** For each feature show its name, a one-line description, the branch and the worktree path. Also show:
- how you parsed the file;
- how each item will be marked done;
- which items you're skipping as already done;
- the computed `cap`/`load`, and whether ccache will be used.

## 2. Create and prepare one worktree per feature (serial)
Create the worktrees **one at a time**: git multi-worktree operations aren't guaranteed to be concurrency-safe. For each feature:

1. **Pre-check collisions:**
   - the branch: `git show-ref --verify --quiet refs/heads/<branch>`;
   - the path.

   On either collision, pick a suffixed name.
2. **Create the worktree:** `git worktree add -b <branch> ../<repo>-worktrees/<slug> <base-branch>`.
3. **Install its env:** `cd <wt> && pixi install --locked`.
   - This is a per-worktree env, hard-linked from the shared package cache when they share a filesystem.
   - `--locked` aborts on a stale lock. If that happens, **stop and ask**; never `pixi update` silently.
   - Install the gcc env only if the base checkout has one: `cd <wt> && pixi install --locked -e gcc`.
4. **Write `<wt>/CMakeUserPresets.json`.** Use the template in the `build-workflow` skill's `scaffold.md` (read `.claude/skills/build-workflow/scaffold.md`), filling in `cap`, `load` and the ccache choice.
   - Pick the template's ccache variant only if `command -v ccache` succeeded; otherwise pick the plain variant.
   - **Substitute every placeholder before writing:** `<CAP>` → `cap`, `<LOAD>` → `load`, and, in the ccache variant, `<COMMON_PARENT>` → the absolute common parent of the repo and `../<repo>-worktrees`. Numbers go unquoted.
   - No `<…>` placeholder may remain in the written file. If one does, `cmake --list-presets` fails on invalid JSON.

   The written file must contain:
   - `"version": 10`;
   - build presets `dev-local` and `release-local`: `inherits` `dev`/`release`, `"jobs": <cap>`, `"nativeToolOptions": ["-l<load>"]`;
   - test presets `dev-local` and `release-local`: `inherits` `dev`/`release`, `"execution": {"jobs": <cap>}`;
   - **only if ccache was found:** configure presets `dev-local`/`release-local` that `inherit` `dev`/`release`, with:
     - `"cacheVariables": {"CMAKE_CXX_COMPILER_LAUNCHER": "ccache"}`;
     - `"environment": {"CCACHE_BASEDIR": "<common parent of the repo and ../<repo>-worktrees>", "CCACHE_NOHASHDIR": "true"}`;
   - `"vendor": {"cpp-cli-studio": {"jobs": <cap>, "load": <load>}}`. `scripts/lint.sh` reads this through `scripts/read-jobs.cmake` for `run-clang-tidy -j`.

   **Invariants to keep:**
   - A local configure preset must keep the **same `binaryDir` as its project preset** (`build/<env>/dev`, not `.../dev-local`). `lint`, the hooks and `.clangd` all read `build/default/dev`.
   - When a local configure preset exists, the local build and test presets point at it (`"configurePreset": "dev-local"`) so they inherit its environment.
   - If the template disagrees with these invariants, stop and report it rather than improvising.
5. **Verify** with `cd <wt> && cmake --list-presets=build`. It must list `dev-local`. Also check that `scripts/preset.sh` will pick it up: it selects `<preset>-local` whenever the user file defines it, and says so.

Don't run repo-wide git operations (`git gc`, `git fetch --all`, submodule ops) from more than one worktree at once.

Show me the final mapping: feature → branch → worktree path → envs installed → cap.

## 3. Run the full feature pipeline in each worktree, in parallel
**Read `.claude/skills/feature/SKILL.md` and execute its steps for each feature, scoped to that feature's worktree.**
- Wherever that file refers to its argument (the feature description), use this feature's name + description.
- Do **not** try to invoke the feature command: it is user-invoked only.

**Inside each feature:**
- Every subagent gets the **worktree's absolute path** and works only there:
  - `cli-designer`, `implementation-planner`, `fact-checker`, `implementer`(s);
  - the dual review by `quality-reviewer` + `memory-safety-reviewer`;
  - `build-fixer`, `cli-tester`, `platform-tester` (scope `local`);
  - the board: `security-reviewer`, `cli-ux-reviewer`, final quality and memory-safety;
  - `bug-fixer`, `test-engineer`, `code-refactorer` as needed.
- **Every command is `cd <wt> && nice pixi run <task>`.** The gate (`nice pixi run check`, plus `-e gcc check` if that env is installed) must go green **in that worktree**.
- Hooks resolve the repo root from the edited file, so they are worktree-correct.
- Honor the feature pipeline's rules:
  - file-disjoint implementer batches;
  - serialization points;
  - the dual-review loop until **both** APPROVE (~3 combined rounds);
  - build-fixer routing;
  - the board;
  - capped rounds.
- Launch the current phase for all unblocked features in the same turn, so they progress concurrently.

**Forward gates and questions to me, labeled by feature.** Every pipeline has ⏸ gates: design, final plan, and reviewer disagreements or a 4th round. In addition, any subagent may hit a real question or blocker, and every **golden re-bless request** needs my approval.
- **Collect these across all running features and surface them together.**
- Tag each one with **feature name + worktree + phase + the exact question + the context I need**: the spec/plan excerpt, the options, the agent's recommendation and, for a golden, the expected-vs-actual output.
- Never silently decide a genuine choice for me.
- Don't let one feature's pending gate stall the others. Keep unblocked features moving and batch the questions.
- Route my answers back to the right feature's pipeline. Approved blesses run as `cd <wt> && nice pixi run bless <regex>`.

Give a **one-paragraph status per round** covering every feature: phase, current verdicts (both reviewers per track), round counts, and what's blocked on me.

## 4. Open a PR per feature
A feature is ready when, in its worktree:
- `nice pixi run check` is green;
- cli-tester's criteria pass;
- the board is APPROVED.

For each ready feature:

**Commit and push**
1. Commit its work on its branch with a clear message. Never use `--no-verify`.
2. **Push first.** `gh pr create` won't push for you in this non-interactive context. From inside the worktree: `cd <wt> && git push -u origin <branch>`.

**Open the PR**
- Write a real body file (not `--fill`), then run `gh pr create --base <base-branch> --head <branch> --title "<feature name>" --body-file <body-file>`. Capture the PR URL that `gh pr create` prints and report it.
- **The body contains:**
  - what was built;
  - new pixi deps;
  - CMake/preset changes;
  - tests added per tier;
  - review rounds per track, for **both** `quality-reviewer` and `memory-safety-reviewer`, plus the board verdicts;
  - the platform tier grid, **without artifact paths** (nothing from `.platform-runs/`);
  - goldens blessed (by name);
  - a closing `## User Test Plan`.

**The PR body's `## User Test Plan`** is never omitted. It uses the same derivation, five-part shape, tier tags (`[human: <reason> · retryable|terminal]`), `removed: "<item>" — licensed by <target> @ <tier>` lines and rules as the feature pipeline's step 10. It is not an escape hatch: testable-but-untested is a coverage gap to flag, not to hand to me. Batch mode differs from a single `/feature` run in four ways:

- **It is self-contained.** The worktree is deleted at cleanup, but the PR outlives it. So **Get it running** starts from a fresh checkout, with `<tool>` read from `CLAUDE.md` → Project specifics at use-time (`<fill in: tool name>` if it is still a placeholder):
  ```
  git fetch origin && git switch <branch>
  pixi install --locked
  nice pixi run test
  build/default/dev/e2e-prefix/bin/<tool from Project specifics> <args>   # real interactive terminal, not piped (sanitized dev build)
  ```
  You may add one line saying the worktree at `<path>` is already installed, if it still exists. That is a shortcut, never the only route.
- **macOS and Windows items** are tagged `[human: retryable — run /platform-test ci after this PR's CI finishes]` when the workflow has those jobs enabled. Otherwise they are terminal on this Linux host.
- **Scope every item to this feature only.** Never mention a sibling feature, its branch or its worktree.
- **No credentials, tokens, real endpoints or `.platform-runs/` content in the body.** A PR body is public, and the diff-secret hook scans the working diff, not the PR body. If the repo has a PR template (`.github/pull_request_template.md` or `.github/PULL_REQUEST_TEMPLATE/`), fold the section into it rather than fighting it.

**Fallback (no `gh` or remote from preflight).** After `git push -u origin <branch>`, print the compare URL for manual PR creation: `https://github.com/<owner>/<repo>/compare/<base>...<branch>?expand=1`. The `?expand=1` is best-effort UX.

**One PR per feature.** Never fold two features into one branch or PR.

**Then mark that feature done in the source file.** Once its PR is open (or, in the fallback, its branch is pushed and its compare URL printed):
- Edit `$ARGUMENTS` in place to mark **only that feature's item** complete, using the convention chosen in §1.
- **Re-read the file immediately before each edit**, so features finishing around the same time don't clobber each other's check-offs. Leave every other line unchanged.
- If the file can't be written, report it and move on. Never fail a finished feature over a check-off.

## 5. Summary and cleanup
**Report a table:**
- feature → branch → worktree → PR URL (or compare URL);
- gate results (`check` / `-e gcc check`);
- review rounds (both reviewers);
- platform tiers reached;
- new pixi deps;
- **User Test Plan** (`N items` / `none needed`);
- outstanding Should-fix/Nit items;
- anything still waiting on me.

**Then a consolidated cross-feature digest** of everything needing hands-on checking, grouped by feature, so I can do a **single testing pass**.
- **Lead with the interactive-terminal items** (prompts, real terminal rendering, `NO_COLOR`/`TERM=dumb` in a real terminal). The automated tests drove the piped and pseudo-TTY paths, so that is where the real gaps are.
- Give each feature its own **Get it running** line.
- Name explicitly the features that need nothing.
- This digest doesn't replace the per-PR sections. Each PR keeps its own self-contained copy.

**The check-offs.** Completed features are now checked off in `$ARGUMENTS`; unfinished, failed and still-blocked ones are left unmarked. That edit is an **uncommitted change in the base working tree**, separate from the per-feature branches. Commit or revert it as you prefer.

**Cleanup: ask before removing worktrees.** On my OK, for each worktree:
1. `git worktree remove <path>`, with `-f` only if I confirm it's dirty.
2. Then `git worktree prune`.

Each worktree's `build/` dirs and `.pixi/` env go with it. Leave branches and PRs intact. Never run `pixi clean cache`: other worktrees and projects hard-link from that cache, and `guard-bash` blocks it.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `cd <wt> && nice pixi run …`: `nice pixi run check`, `nice pixi run -e gcc check`, `test`, `lint`, `release`, `release-test`, `bless`, `cxx-probe`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and no piping gate output through `tail`/`head`.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Gate runs get an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` with a wait for completion. A cold `check` routinely exceeds the 120 s default, and N worktrees under caps are slower still.
  - A run killed by the tool timeout is not a test failure.
  - The build lock self-recovers from a dead owner.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** if:
  - a feature's preconditions fail;
  - a pipeline loop won't converge;
  - a worktree can't be prepared (`pixi install --locked` fails, `dev-local` missing);
  - a PR can't be created.

  Surface it; don't paper over it.
