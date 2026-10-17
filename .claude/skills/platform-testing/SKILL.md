---
name: platform-testing
description: Knowledge skill for conditional platform, compiler, sanitizer and tool verification - probe what is actually present at use-time (pixi envs, clang/gcc, ASan/UBSan/LSan/TSan usability, symbolizer, docker images, cross toolchains, valgrind, CI results), climb tiers per target (configured -> built -> smoke-ran -> contract-verified -> sanitizer-clean), report a literal tier grid, and degrade with closed retryable/terminal reasons. This is the reference the platform-tester agent follows for the /platform-test command, the platform pass in /feature and /harden, and the sanitizer-usability probe in /setup. It is not the /platform-test command itself.
---

# Platform testing (conditional capability)

Some verification is possible **only when something happens to be present**: a second compiler env, a usable sanitizer runtime, a docker image, a cross toolchain, valgrind, finished CI runs. This skill turns that into evidence without ever over-claiming:
- **Probe** at use-time, cheapest and non-mutating first.
- **Climb tiers per target.** There is never a global tier.
- **Report** what each tier licenses.
- **Degrade** with a stated, closed-list reason.

A missing capability makes the pass a **no-op report, and that report is a success**. It never fails, blocks or stalls the pipeline.

Owners:
- this skill;
- the `platform-tester` agent (read-only Bash, no edits, no verdict);
- the `/platform-test [probe|local|ci|all] [<target filter>]` command.

It also runs:
- in `/feature` (scope `local`, after `cli-tester`);
- in `/harden` (scope `local`);
- in `/setup` (scope `probe`). If the default env's ASan+UBSan state is not `usable`, `/setup` stops, because option C can't hold.

## Safety rules (non-negotiable)
- **Never install, pull, `sysctl`, `sudo`, or change container or kernel settings.** Detect, report what's missing plus the fix the *user* can run, and degrade.
- **Docker:** only images already present (`docker image inspect`). Always run with:
  - `--rm`;
  - `--network none`;
  - a **read-only** mount of the release install only;
  - the name `@TOOL@-platform-<run>-<n>`.

  Never `--privileged`, `--cap-add`, `--security-opt`, `--pid=host`, or a mount of `/`, `$HOME` or the docker socket. The `guard-bash` hook enforces most of this, best-effort.
- **No lease.** There is no exclusive resource. Docker is shared, and CPU/RAM are governed by the per-worktree caps (`build-workflow`). Parallel worktrees may run passes concurrently.
- **No global state is touched**, so there is nothing to snapshot or restore. Teardown runs from a `trap`: remove any `@TOOL@-platform-<run>-*` container still present. The report says "nothing to restore".
- **Artifacts** go to `.platform-runs/<UTC-timestamp>-<scope>/` (e.g. `.platform-runs/20261002T141500Z-local/`).
  - `cxx-probe` leaves its binaries under `.platform-runs/cxx-probe/<env>/<name>` (`<name>.nosan` for `--no-sanitize`).
  - The directory is gitignored, and the command adds it to `$(git rev-parse --git-common-dir)/info/exclude` at preflight, because new worktrees only see committed `.gitignore` lines.
  - **Artifacts never go into a PR body or a commit.** Logs can contain paths and environment details.
- Heavy commands are typed as `nice pixi run …` (bare `nice`), never in CI, and never swallowing the exit code (`low-priority-execution`).
  - `cxx-probe` (which configures first), `test` and `release-test` exceed the Bash tool's 120 s default. Pass an explicit `timeout` (up to 600000 ms) or use `run_in_background` and wait.
  - A run killed by the tool timeout is **not** a result. Re-run it longer, or report R11.
- Never prefix a command with an inline `VAR=…` assignment. Never `gh run watch`; polling is bounded. Never `gh pr edit`.
- **Parse fields; never substring-match.** Take exit codes from `cxx-probe: exit=<n>`, states from exact markers, `ptrace_scope` from the number. A pattern that also matches a header or a "0 found" line is a bug.

## Scopes
| Scope | Runs |
|---|---|
| `probe` | P0–P4 (host, envs, compilers, sanitizer usability). No tier climb (R10 for the rest) |
| `local` | P0–P8: + host tier climb in each installed env, containers already present, configured cross toolchains, valgrind |
| `ci` | P0, P9 (read-only GitHub Actions results for HEAD) |
| `all` | `local` + `ci` |

A target filter (`gcc`, `docker`, `aarch64`, …) narrows what is attempted. Everything filtered out is reported as R10.

## Probe ladder (cheapest and non-mutating first)
| # | Probe | Commands | Outcome |
|---|---|---|---|
| P0 | host | `uname -m`, `nproc`, `cat /proc/sys/kernel/yama/ptrace_scope`, `/.dockerenv` / `/run/.containerenv` (Read/Glob), `stat -f -c %T .` and `stat -f -c %T "$HOME"` | Facts for the Host line. `/proc/sys/vm/mmap_rnd_bits` is root-only, so report `unreadable (root-only)` and infer from symptoms. `ecryptfs` → note it (pixi cache advice is in `build-workflow`) |
| P1 | envs | Glob `.pixi/envs/default`, `.pixi/envs/gcc` | missing → R1 (`pixi install --locked -e <env>`) |
| P2 | compiler | `nice pixi run [-e gcc] cxx-probe .claude/skills/platform-testing/probes/probe_clean.cpp --no-sanitize` | exit 0 + `probe-clean-ok 3 42` → `configured`. Record compiler id + version (`nice pixi run [-e gcc] cxx-probe …/probe_features.cpp` prints `probe-features toolchain …` lines). 125 → compiler unusable (quote the error) |
| P3 | sanitizers, per env | `cxx-probe` (sanitized) on `probe_clean`, `probe_asan`, `probe_ubsan`, `probe_lsan` | state per the table below. Also record linkage (`ldd .platform-runs/cxx-probe/<env>/probe_asan`, field-match `lib(a\|ub\|t)san`: clang links its runtime statically, gcc dynamically) and `command -v llvm-symbolizer` under `pixi run` |
| P4 | TSan | only if **Project specifics** says the tool is threaded **and** the `tsan` preset is configured: `nice pixi run cxx-probe …/probe_tsan.cpp --preset tsan` | not threaded → T3. No preset → R9 |
| P5 | host tier climb | `nice pixi run [-e gcc] test`, `nice pixi run [-e gcc] release-test` | tiers per the tier table. Scope `probe` → R10 |
| P6 | containers | `docker info` (daemon reachable?), then `docker image inspect <img>` for each image in Project specifics | no docker / permission denied → R4. Image missing → R5 (never pull). Present → `docker run --rm --network none --name @TOOL@-platform-<run>-<n> -v <abs repo>/build/default/release/e2e-prefix:/opt/t:ro <img> /opt/t/bin/@TOOL@ --version` (and `--help`) → `smoke-ran`. **Container ceiling = `smoke-ran`.** It also proves the glibc floor on that image |
| P7 | cross | `x86_64-w64-mingw32-g++ --version`, `wine --version`, `aarch64-linux-gnu-g++ --version`, `qemu-aarch64 --version` | tiers only if the user added a toolchain preset (else R9). MinGW is never MSVC (T3) |
| P8 | valgrind | `valgrind --version`; if present, run on the **unsanitized** `cov` install with `--error-exitcode=99` | evidence for hazard #8 (uninitialized reads), not a tier. No `cov` build → R9 |
| P9 | CI results | `gh --version`, `gh auth status`, `gh run list --commit <HEAD sha> --json databaseId,status,conclusion,headSha,name`, `gh run view <id> --json jobs` | see "CI results" below |
| P10 | perf/audit tools | `ccache --version`, `sccache --version`, `syft version`, `grype version` | ccache/sccache are performance only and license nothing. syft/grype feed the `audit` coverage statement |

**Categorically absent on Linux:** Apple tooling (AppleClang, Xcode, macOS) and MSVC. List them **once** as T1. Don't probe them, don't warn about them, and never claim them.

## Sanitizer state per env (P3)
Run the probes with `cxx-probe`, which compiles with the env's **real** dev-preset flags (`sanitizer-flags.txt`) and runs with the real options (`sanitizer-env.txt`). That is condition 8 of `memory-safety`.

| State | Condition |
|---|---|
| `absent` (R2) | sanitized `probe_clean` fails to compile/link (exit 125), e.g. a missing runtime |
| `present-but-unusable` | `probe_clean` exits nonzero, or prints any startup marker (`Shadow memory range interleaves…`, repeated `AddressSanitizer:DEADLYSIGNAL`, `ThreadSanitizer: unexpected memory mapping`, `memory layout is incompatible…`, `CHECK failed: … personality`, `LeakSanitizer has encountered a fatal error`, `AddressSanitizer: out of memory`, `ASan runtime does not come first in initial library list` (an `LD_PRELOAD` interferes: report the variable's presence, never its value)). Quote the text. Add a **diagnosis line**: `setarch x86_64 -R .platform-runs/cxx-probe/<env>/probe_clean` passes → high-entropy ASLR (R6); it fails with a personality/permission error → ptrace/seccomp (R7) |
| `detection-broken` | a positive control exits 0, or lacks its marker: `probe_asan` must exit 86 with `ERROR: AddressSanitizer: heap-buffer-overflow`; `probe_ubsan` must exit 86 with `runtime error: signed integer overflow`. Treated as unusable |
| `degraded` (R3) | markers present, but the ASan report has no `probe_asan.cpp:` frame (unsymbolized: `llvm-symbolizer` missing). Tests still gate; the tier is reduced (see below) |
| `usable` | clean probe silent with exit 0, and every control fired with its marker and exit 86 |

- **`probe_lsan` is informational.** Stack residue at `-O1` can hide its leak. The authoritative leak control is `sanitizer-control.leak` (built at `-O0`) in `nice pixi run test -L sanitizer-control`.
- **`probe_tsan` can miss its race intermittently.** Re-run it up to 3 times before calling TSan `detection-broken`. The authoritative TSan control is `sanitizer-control.data-race` in the `tsan` preset.
- **Never "fix" a probe** to make it pass. The probes are deliberately minimal and buggy. `probe_features.cpp` failing to compile (125) means the toolchain is below the C++23 floor.

## Tiers (per target; never global)
A **target** is `os/arch · compiler + version · config`, e.g. `linux/x86_64 · clang 23.1.2 · dev` or `linux/x86_64 · gcc 16.2.0 · release`. Read versions from the probe output; never assume them.

| Tier | Licenses | Evidence required |
|---|---|---|
| `configured` | nothing (toolchain found) | P2 |
| `built` | compiles and links `-Werror`-clean with that compiler/config | the build step of `test` (dev) / `release` (release) succeeded |
| `smoke-ran` | the installed binary loads and starts; **not** behaviour | `--version` and `--help` exit 0 on that target (host install, or a container) |
| `contract-verified` | the exercised unit + E2E + edge-suite cases hold on that target's installed binary | `test` (dev) / `release-test` (release) passed. TTY cases that *skipped* (77) are listed as not covered (R2: util-linux `script`) |
| `sanitizer-clean` | "no ASan/UBSan finding on the exercised paths", for that compiler and config | contract-verified on the sanitized `dev` config **and** sanitizer state `usable` **and** `sanitizer-control.*` passed in the same run. With state `degraded`, cap at `contract-verified` and say why (R3). Not applicable to `release` |

**Evidence that a run happened is not evidence of correctness.** A zero exit, a log line or a container that started shows that something executed, not that it behaved. Never write "verified", "works on X" or "memory-safe" without a target and a tier.

## CI results (P9)
- Read `.github/workflows/*.yml` to learn what each job actually runs (matrix env → compiler, `check` vs `release-test`). Never infer from job names.
- Only runs whose `headSha` equals the local HEAD count. Anything else is stale (R8). `in_progress`/`queued` → R8 (pending). There is no run, or the branch isn't pushed → R8.
- A successful job that ran `pixi run check` licenses that matrix target's `dev` config up to `sanitizer-clean`, because the sanitizer controls run inside `check`. One that ran `release-test` licenses `contract-verified` for its `release` config. Write the target as `ubuntu-<image> · <compiler> <version if the log shows it, else "version unread"> · <config>`.
- macOS/Windows jobs (if the workflow enables them) are the **only** way those targets get any tier. Locally they are T1.
- Polling is bounded: at most a few `gh run list`/`view` calls. `gh` absent or unauthenticated → R2.

## Degradation reasons (closed lists; use the code and the stated fix)
**Retryable** (re-running `/platform-test` after the change shrinks the User Test Plan):
- **R1** env not installed → `pixi install --locked -e <env>`
- **R2** tool absent → install it yourself (name the tool)
- **R3** symbolizer missing → `llvm-symbolizer` comes from the env's `llvm-tools`; check `pixi list`
- **R4** docker daemon down, or socket permission denied
- **R5** image not present → `docker pull <img>` yourself
- **R6** high-entropy ASLR → `sudo sysctl -w vm.mmap_rnd_bits=28` yourself (not persistent)
- **R7** ptrace/seccomp restricted → container flags, your decision
- **R8** not pushed / CI not run / CI pending / CI stale
- **R9** preset not configured (tsan, cov, a toolchain preset)
- **R10** skipped by scope or filter → re-run with `local`/`all`
- **R11** resource limit (ASan OOM under the caps, tool timeout) → re-run with fewer parallel worktrees

**Terminal** (re-running changes nothing):
- **T1** host OS can't (MSVC, AppleClang/macOS, Windows native). Only CI licenses these.
- **T2** MSan impractical (needs a fully instrumented std library)
- **T3** not applicable (TSan on a single-threaded tool, HWASan on x86_64, MinGW ≠ MSVC)
- **T4** human judgment

## Report (written literally; `/platform-test` prints it verbatim)
```
## Platform verification (<ISO date>)  scope: <probe|local|ci|all>  ref: <branch>@<short sha>
**Host** — arch · nproc · ptrace_scope · container: y/n · mmap_rnd_bits: unreadable (root-only) · fs: <type>
**Capabilities** — | capability | state | evidence |   (clang ASan+UBSan, gcc ASan+UBSan, TSan, symbolizer, docker+images, mingw/wine, aarch64/qemu, valgrind, CI results, ccache)
**Tier grid** — no global tier
| target (os/arch · compiler ver · config) | configured | built | smoke-ran | contract-verified | sanitizer-clean |
**What each tier licensed** — one line per User Test Plan subtraction: item · target · tier
**Not covered, with reasons** — <item> — <reason> — [retryable: <what changes it>] | [terminal: <why rerun changes nothing>]
**Still human, unconditionally** — real terminal-emulator rendering & a human typing into prompts · real data/credentials/network · distro package install (makepkg -i) · Windows/macOS behaviour beyond what CI ran
**Commands NOT run (and why)** — installs, pulls, sysctl, privileged/unpinned docker, anything needing sudo
```
- **Capability states:** sanitizers use `absent / present-but-unusable / detection-broken / degraded / usable`. Everything else uses `absent / present-but-unusable / ready`.
- **Evidence column:** the exact command and the parsed field (`cxx-probe: exit=86` + marker), never a paraphrase.
- **Grid cells:** `✅` reached, `—` not reached, or the reason code (`R5`, `T1`, …) for the first tier not reached. Never leave a cell blank.
- Append "Teardown: nothing to restore" (and any container removed).

## Narrowing rule (how the pass changes the User Test Plan)
- The pass **removes only** the User Test Plan items its tiers license, **for that target**: `removed: "<item>" — licensed by <target> @ <tier>`.
  - `smoke-ran` licenses only "loads and starts on <target>".
  - `contract-verified` licenses the exercised contract cases there.
  - `sanitizer-clean` licenses "no ASan/UBSan finding on exercised paths" for that compiler and config.
- Everything else **stays**, tagged `[human: <reason> · retryable|terminal]`. Items that need a person's judgment rather than a run (whether help text reads well, whether output looks right in a real terminal, a reviewer's judgment call) are tagged **T4**; no tier licenses them.
- Real-terminal rendering, a human typing into prompts, real data/credentials/network, distro package installs, and Windows/macOS behaviour beyond what CI ran are **never** licensed away.
- The boundary narrows; it never disappears, and it is never assumed away.

## Sibling skills
- `memory-safety`: sanitizer flags, runtime options, ASLR markers, controls.
- `cpp-testing`: the tiers that `test`/`release-test` run, and the TTY skip code 77.
- `build-workflow`: envs, presets, `cxx-probe.sh`, release builder, CI.
- `low-priority-execution`: `nice` and timeouts.
