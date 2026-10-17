---
name: platform-test
description: "User-invoked orchestration command (/platform-test). It re-runs the conditional platform pass on demand: it probes which compilers, sanitizers, containers, cross toolchains, valgrind and CI results are present, has platform-tester climb tiers per target, and prints the grid plus a User Test Plan delta. It never blocks and never installs. Use it after pulling a docker image, installing the gcc env or attaching a toolchain, or once CI has finished (scope `ci`). This is the on-demand command (`/platform-test`). It is distinct from the `platform-testing` knowledge skill, which holds the probe ladder, tiers and reasons this command follows."
argument-hint: "[probe|local|ci|all] [<target filter>]"
disable-model-invocation: true
---

Run a platform pass for: **$ARGUMENTS**. The first word is the scope (default `local`); anything after it is a target filter, for example `gcc`, `docker`, `aarch64`.

| Scope | What it covers |
|---|---|
| `probe` | Host, envs, compilers and sanitizer usability only. No tier climb (R10). |
| `local` | Everything runnable here: both pixi envs, the tier climb through `test`/`release-test`, docker images already present, cross toolchains if configured, valgrind. |
| `ci` | Read-only GitHub Actions results for HEAD. |
| `all` | `local` + `ci`. |

## 1. Preflight
- Resolve the ref with `git rev-parse --abbrev-ref HEAD` and `git rev-parse --short HEAD`.
- For `ci`/`all`, also resolve the full HEAD SHA, so stale CI runs (`head_sha` ≠ HEAD) are reported as R8, not counted.
- Idempotently add `.platform-runs/` to `$(git rev-parse --git-common-dir)/info/exclude` (append only if the line is absent).
  - Artifacts go to `.platform-runs/<UTC>-<scope>/` and never into a commit or PR body.
  - The exclude goes in `info/exclude` because an uncommitted `.gitignore` entry doesn't reach new worktrees.
- **No ⏸ gates. This command never blocks and never installs.**

## 2. Delegate
Delegate to `platform-tester` with:
- the scope;
- the target filter;
- the ref;
- the repo's absolute path;
- the docker images and extra toolchain presets listed in `CLAUDE.md` → **Project specifics**.

It follows the `platform-testing` skill:
- **Probe ladder:** cheap, non-mutating probes first, with fields parsed rather than substring-matched.
- **Tiers per target, never one global tier:** `configured → built → smoke-ran → contract-verified → sanitizer-clean`.
- **States:**
  - sanitizers: `absent` / `present-but-unusable` / `detection-broken` / `degraded` / `usable`;
  - other capabilities: `absent` / `present-but-unusable` / `ready`.
- **Reasons:** R1–R11 (retryable) and T1–T4 (terminal).

Teardown runs from a trap: containers are `--rm` and named `<tool>-platform-<run>-*`. It never installs, pulls, runs `sysctl` or `sudo`, or uses privileged docker.

## 3. Present the report verbatim
Print the agent's `## Platform verification (<date>) scope: … ref: …` block **exactly as returned**:
- Host;
- Capabilities;
- Tier grid;
- What each tier licensed;
- Not covered, with reasons;
- Still human, unconditionally;
- Commands NOT run.

Don't summarize away rows and don't promote a tier. Evidence that a run happened is not evidence of correctness.

## 4. Print a User Test Plan delta
Base the delta on the most recent User Test Plan in context. That is the last `/feature` or `/implement` summary, or a PR body if I name one, which you read with `gh pr view <n>`. If none is in context, print the delta against a generic CLI plan and say so.

**Subtractions.** One line per item this pass's tiers license, for **that target only**: `removed: "<item>" — licensed by <target> @ <tier>`.
- `smoke-ran` licenses only "loads and starts on that target".
- `contract-verified` licenses the exercised contract cases on that target.
- `sanitizer-clean` licenses "no ASan/UBSan finding on exercised paths" for that compiler and config. It never licenses "memory-safe".

**Remaining items.** Each keeps a tag `[human: <reason> · retryable|terminal]`.
- **retryable** says what changes it. Examples:
  - "pull `<image>` yourself, then `/platform-test local`";
  - "`/platform-test ci` after CI finishes";
  - "`pixi install --locked -e gcc`".
- **terminal** says why a re-run changes nothing. Examples:
  - MSVC/macOS on this Linux host, which only CI licenses (T1);
  - MSan (T2);
  - TSan on a single-threaded tool (T3);
  - human judgment (T4).

**Always still human.** These stay listed, whatever ran:
- real terminal-emulator rendering;
- a human typing into prompts;
- real data, credentials and network;
- a distro package install;
- Windows/macOS behaviour beyond what CI ran.

**Never edit a PR body.** `gh pr edit` is in `ask` on purpose. Print the delta so I can apply it myself.

## 5. Status line
End with one line:
- targets probed;
- the highest tier per target;
- the count of retryable vs terminal gaps;
- the single most useful next action (for example, "install the gcc env, then re-run `local`").

A pass where everything was absent is a **successful no-op report**, not a failure.

**Execution rules**
- **Low priority.** Heavy commands the agent runs locally are at low OS priority as `nice pixi run …`: `nice pixi run cxx-probe …`, `nice pixi run -e gcc test`, `nice pixi run release-test`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code.
  - See the `low-priority-execution` skill.
- **Long runs.** The tier climb's `test`/`release-test` runs need an explicit long Bash `timeout` (up to 600000 ms) or `run_in_background`. A run killed by the tool timeout is not a contract failure. Re-run it longer. If it still can't finish, report it as not covered, with the reason stated.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** only on a genuine blocker that isn't a missing capability, for example the repo isn't a git checkout. A missing capability is never a reason to stop.
