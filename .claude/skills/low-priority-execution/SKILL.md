---
name: low-priority-execution
description: How to run this project's heavy commands at lowered OS priority locally. Covers every pixi gate/build/test/lint/release/cov/audit/package task, cxx-probe, gcc-env checks, docker smoke runs and valgrind. Agents type bare `nice <cmd>`, never in CI, never swallowing exit codes, with long Bash timeouts for slow gates. Also explains how niceness differs from the per-worktree parallelism caps (`-local` presets, the vendor jobs key, never CMAKE_BUILD_PARALLEL_LEVEL). Use when invoking or wiring any heavy command, choosing a Bash timeout for one, or editing a hook that runs one.
---

# Low-priority execution

This project's heavy commands are:
- the sanitized dev build plus a hardened release build, inside every `check`;
- the same chain again in the `gcc` env;
- clang-tidy over every TU, the E2E suite under ASan, coverage builds, `cxx-probe` compiles, docker smoke runs and valgrind.

A Ninja build saturates every core. Locally, run these at **lowered OS priority** so the machine stays usable. Lowering priority never changes a result, never applies in CI, and never hides an exit code.

## The convention

**Agent-typed commands use bare `nice <cmd>`**, for example `nice pixi run check`.
- GNU `nice` with no `-n` uses increment **10**, so it equals the canonical `nice -n 10` (check with `nice nice`, which prints your current niceness + 10). On macOS, confirm the default in `man nice` before relying on it.
- **Why bare, not `nice -n 10`:** Claude Code strips `nice` before permission matching, so `nice pixi run check` matches the existing `Bash(pixi run check)` allow rule. Whether the **flagged** form `nice -n 10 …` is also stripped is unconfirmed (fact-check I1). Typing it could cost a permission prompt on every gate run. This is a deliberate deviation from the meta-studio canon's `nice -n 10`.
- Niceness inherits across fork/exec. Prefixing the runner once (`nice pixi run …`) covers CMake, Ninja, the compilers, the linker, ctest, the tests, run-clang-tidy and its clang-tidy children.

| OS | Agent-typed form | Notes |
|---|---|---|
| **Linux** (primary) | `nice <cmd>` | Matched through wrapper stripping. **Never type `ionice`**: it is an exec wrapper no `Bash(...)` rule can approve, so it prompts every time. It is also opportunistic (BFQ scheduler only; a no-op on `mq-deadline`/`none`). |
| **macOS** (CI-only in this studio) | `nice <cmd>` | CPU-only. `taskpolicy -b` throttles CPU and I/O, but it is **not** stripped before matching, and a `Bash(taskpolicy …)` rule would approve anything behind it. Keep it inside the hooks' helper. |
| **Windows** | Not a local target | Windows exists only as optional CI. If it ever becomes local, lower a `BelowNormal` shell and run the command normally, so the child inherits and `$LASTEXITCODE` flows. Verify the exit code propagates before trusting it in a gate. |

**Where the flagged forms live:** the canonical `nice -n 10`, `ionice -c3` and `taskpolicy -b` appear **only inside hook scripts**, through the `low_prio` helper below. Hooks run outside permission matching.

The project's own `scripts/*.sh` (`format.sh`, `lint.sh`, `preset.sh`, …) **never re-nice themselves**:
- they inherit the niceness of the typed `nice pixi run …`;
- they also run in CI, where nothing should be lowered;
- a second `nice` inside would stack the increments.

## Rules

1. **Never swallow the exit code.** `nice` execs the command and returns its status unchanged, and pixi returns the task's status. Don't pipe a gate into `tail`/`head`/`grep` without `set -o pipefail`; the gate depends on seeing a nonzero exit. If you need the log, read the tool's output or the CTest log afterwards.
2. **Invocation-level only, never in CI.**
   - Never put `nice` (or `ionice`/`taskpolicy`) into a `pixi.toml` task, `CMakeLists.txt`, a preset, a script or a CI workflow. CI has nothing to yield to.
   - The hook helper is a no-op when `$CI`/`$GITHUB_ACTIONS`/`$GITLAB_CI` is set.
3. **It only matters under contention.** On an idle machine it changes nothing. It is a good-neighbour tool, not a speed knob.
4. **`ionice` is opportunistic.** Don't promise I/O relief on Linux.
5. **Timing-fragile tests:** a niced ASan build is slower than a plain one, and contention slows it further.
   - Give E2E `TIMEOUT` properties generous margins, sized for sanitized and niced runs.
   - A test that only times out under `nice` has a timeout that's too tight. Raise the timeout; don't drop the wrapper.

## Long runs vs the Bash tool timeout

A cold `nice pixi run check` routinely exceeds the Bash tool's **120 s default**. So do `-e gcc check`, `release`, `release-test`, `cov`, `package` and often `test` and `lint`.
- Pass an explicit `timeout` (up to `600000` ms), or start the command with `run_in_background` and wait for it to finish before reading the result.
- **A run killed by the tool's timeout is not a failure of the code.** Re-run it with a longer timeout before you diagnose anything.
- A killed `preset.sh`/`lint.sh` can leave `build/<env>/<preset>.lock.d/` behind. The next run detects that the owner PID is gone, takes the lock over and says so (`cpp-quality`, "Build-dir lock"). Don't delete the lock by hand while another track may be building.

## Niceness is not a parallelism cap

`nice` doesn't limit how many cores or how much RAM a build uses; it only yields CPU under contention. Several worktrees each running `check` can still exhaust memory, especially on ASan links. The caps are separate:

- **Build jobs:**
  - `/features` writes each worktree's `CMakeUserPresets.json` with `dev-local`/`release-local` build presets (`jobs: <cap>`, `nativeToolOptions: ["-l<load>"]`).
  - `scripts/preset.sh` selects a `-local` preset automatically when one exists.
  - A user preset can't shadow a project preset name, which is why the `-local` suffix exists.
- **Test jobs:** the `-local` test presets set `execution.jobs: <cap>`. How that interacts with `CTEST_PARALLEL_LEVEL` is verified at use-time.
- **clang-tidy jobs:** `"vendor": {"cpp-cli-studio": {"jobs": <cap>, "load": <load>}}` in the same file. `lint.sh` reads it via `scripts/read-jobs.cmake` and always passes an explicit `-j`. `run-clang-tidy` and `clang-tidy-diff.py` would otherwise use every CPU.
- **RAM-heavy links:** the Ninja link pool is 2 when sanitizing (`build-workflow`).
- **Never set `CMAKE_BUILD_PARALLEL_LEVEL` or `CTEST_PARALLEL_LEVEL`**: not in a shell, a pixi task `env`, `settings.json`, or a CI env.
  - Parallelism precedence is: CLI `-j` > `CMAKE_BUILD_PARALLEL_LEVEL` > build-preset `jobs`. That env var would silently override the worktree cap.
  - For `CTEST_PARALLEL_LEVEL`, `0` means **unbounded**. An empty value falls back to a CPU-count default.
  - `preset.sh` unsets both, with a notice, when it uses a `-local` preset. The `guard-bash` hook blocks assigning them, and the `cpp-rule-linter` hook flags them in `pixi.toml` task `env` (and `CMAKE_BUILD_PARALLEL_LEVEL` in CMake files).
- **Inline `VAR=value cmd` assignments** defeat permission allow rules anyway. Caps travel in preset files, never in the command line.

## This project's commands, wrapped

Prefix the runner once. In a `/features` worktree, use `cd <worktree> && nice pixi run <task>`.

```sh
nice pixi run check                  # the gate: fmt-check → test (dev, ASan+UBSan) → lint → release → binary-packages
nice pixi run -e gcc check           # the same chain with GCC (lint skips by design in this env)
nice pixi run test                   # dev build + full CTest suite (unit, cli, e2e, sanitizer controls)
nice pixi run test -R unit           # filtered: -R <regex> / -L <label> / -E <regex>
nice pixi run -e gcc test -R e2e
nice pixi run build                  # dev build only
nice pixi run lint                   # run-clang-tidy over own TUs (explicit -j from the worktree cap)
nice pixi run lint-changed           # changed lines vs merge-base
nice pixi run lint-fix               # fix-its + fmt; review the diff
nice pixi run fmt                    # clang-format in place
nice pixi run fmt-check
nice pixi run release                # hardened, -Werror release build
nice pixi run release-test           # release build + tests + release-check
nice pixi run -e gcc release-test
nice pixi run cov                    # coverage build + report
nice pixi run audit                  # syft + grype over the pixi env
nice pixi run package                # CPack from the release build
nice pixi run cxx-probe .claude/skills/platform-testing/probes/probe_asan.cpp
nice pixi run -e gcc cxx-probe .claude/skills/platform-testing/probes/probe_ubsan.cpp
nice valgrind --error-exitcode=99 <non-sanitized binary> <args>   # platform-testing; never on a sanitized build
```

`pixi run bless <regex>` is the one heavy-ish task an agent never runs itself. It is in `ask`, and the human approves it.

**Docker smoke runs** (`platform-testing`): `nice docker run …` lowers only the docker *client*. The container's processes are children of the daemon, so they don't inherit it. If a container's CPU use matters, use docker's own resource flags, after checking them against `docker run --help`. `docker run` is in `ask` regardless.

## The hook helper: `low_prio` (lives in `.claude/hooks/lib.sh`)

Hooks that run a heavy tool (the format-check hook's clang-format/clang-tidy calls) wrap it as `low_prio <tool> <args…>`. This is the canonical form. It is POSIX-sh and `set -u`-safe, preserves the tool's exit code, and degrades to a plain run when no wrapper exists:

```sh
# Run a compute-heavy command at low OS priority locally; full speed in CI.
# Preserves the command's exit code. Degrades to a plain run if no tool is available.
low_prio() {
  if [ -n "${CI:-}${GITHUB_ACTIONS:-}${GITLAB_CI:-}" ]; then "$@"; return; fi
  if [ "$(uname -s)" = "Darwin" ] && command -v taskpolicy >/dev/null 2>&1; then
    taskpolicy -b "$@"; return
  fi
  if command -v nice >/dev/null 2>&1; then
    if command -v ionice >/dev/null 2>&1; then ionice -c3 nice -n 10 "$@"; else nice -n 10 "$@"; fi
    return
  fi
  "$@"
}
```

**Compose it as `low_prio timeout 60 <clang-tidy> …`, never `timeout 60 low_prio …`.** `timeout` execs an external program, so it can't call a shell function: the backwards form fails with 127 ("failed to run command") on every run. In the correct order, `nice` execs `timeout`, which execs the tool, and an expiry still returns 124, which the hook treats as a note rather than a block. Don't change a hook's blocking or exit-code logic when adding the helper.

## Permissions

- **No permission rule for `nice`.** It is stripped before matching, so `Bash(nice *)` is noise. The underlying rule (`Bash(pixi run check)`) is what matches.
- **Never add `Bash(ionice …)`** (it never matches) **or `Bash(taskpolicy …)`** (it would approve anything behind it).

## Sibling skills

- `cpp-quality`: the gate whose exit codes this must never swallow, and the build-dir lock.
- `build-workflow`: pixi tasks, presets, the `-local` per-worktree caps and the link pool.
- `platform-testing`: `cxx-probe`, docker and valgrind runs, and how they are reported.
