---
name: setup
description: "User-invoked orchestration command (/setup) that bootstraps a pixi-managed C++23 CLI project from this studio's scaffolds. It covers preflight (including ecryptfs detection, warn-only), deriving names, writing pixi.toml with majors pinned from `pixi search`, and writing the scaffold files. It then probes C++23 features and sanitizer usability, requires a green `nice pixi run check`, blesses the initial goldens, and runs a verify-at-use-time checklist. CI with SHA-pinned actions is optional. Invoke it explicitly with the tool name and purpose. Claude never auto-runs it."
argument-hint: "<tool name> — <one-line purpose>"
disable-model-invocation: true
---

Bootstrap a new C++23 CLI project: **$ARGUMENTS**

The stack:
- pixi envs: `default` = conda-forge clang, `gcc` = conda-forge gcc;
- CMake + Ninja via `CMakePresets.json`;
- CLI11, Catch2 v3, nlohmann/json;
- clang-format and clang-tidy;
- ASan+UBSan in the dev preset;
- a hardened static-libstdc++ release build.

**Verify, don't assume.** The fact-check snapshot in `CLAUDE.md` is orientation only. Every version comes from what `pixi search` / `pixi list` report **now**. Anything else version-specific goes to `fact-checker`.

**Never overwrite an existing file.** In an existing project, for every target that already exists:
1. Show a per-file diff (current vs scaffold).
2. ⏸ Ask: keep / replace / merge by hand.

## 0. Preflight
1. **pixi.** `pixi --version`. If pixi is missing, stop and point me at pixi's install docs; never install it yourself.
2. **The repo.** `git rev-parse --show-toplevel`. If this isn't a git repo, ⏸ ask whether to `git init`.
3. **The studio's root examples.** These must exist at the repo root, copied from the studio per the README:
   - `pixi.example.toml`;
   - `CMakePresets.example.json`;
   - `.gitignore`.

   So must the scaffold files under `.claude/skills/*/`. If any is missing, stop and say which.
4. **The ecryptfs check (detect and warn; never set anything):**
   1. Read the package cache dir from `pixi info` (its cache-dir line). If that dir doesn't exist yet (nothing installed so far), `stat` its nearest existing parent instead. The default cache is under `~/.cache/rattler` unless `pixi info` reports otherwise.
   2. Run `stat -f -c %T <path>` on that dir, on `$HOME`, and on the repo root.
   3. **If the cache dir or `$HOME` is `ecryptfs`, warn me and continue.** This is a warning, not a blocker. ecryptfs caps filenames at ~143 bytes, so package extraction there *can* fail with `File name too long (os error 36)` if a package path exceeds the cap; in practice pixi usually works. If that failure does happen (steps 2.2/2.3), the fix is:
      - export `PIXI_CACHE_DIR` (which takes precedence) or `RATTLER_CACHE_DIR` to a path on another filesystem;
      - do it **in your shell profile, then restart `claude`**.

      I never set either variable, and `guard-bash` blocks inline assignments of them. Keep future `/features` worktrees on the same filesystem as that cache, so pixi hard-links instead of copying.
   4. **If the repo itself is on `ecryptfs`, warn me and continue** (also non-blocking): env extraction into `.pixi/` may hit the same filename cap. Point me at pixi's detached-environments setting. Its exact config key is **verified at use-time** via `fact-checker` (pixi docs / `pixi config --help`). Never guess it, and never run `pixi config set` for me.
5. **Optional tools.** `command -v` each of `gh`, `ccache`, `help2man`, `syft`, `grype`, `docker`, `valgrind`. They are optional; record what's present for the report.

## 1. Names
Derive these from `$ARGUMENTS` and ⏸ confirm them with me:
- `@TOOL@`: the binary/CLI name, as typed by users.
- `@PROJ@`: the C++ namespace and CMake prefix. It must match `[a-z][a-z0-9_]*`; derive it from the tool name with `-` → `_`.
- `@PROJ_UPPER@`: `@PROJ@` upper-cased. It is the CMake option prefix, e.g. `@PROJ_UPPER@_SANITIZE`.
- `@DESCRIPTION@`: the one-line purpose. It lands inside a C++ string literal, so **escape `\` as `\\` and `"` as `\"`** when substituting it.
- `@SPDX@`: the license as an SPDX identifier. **Ask me**; don't default it.

## 2. pixi manifest and envs
1. **Write `pixi.toml` from `pixi.example.toml`.**
   - Substitute the placeholders.
   - Pin each major to what `pixi search` shows **now**:
     - `pixi search clangxx_linux-64`, `pixi search clang-tools`, `pixi search llvm-tools`, `pixi search clang-format`;
     - `pixi search gxx_linux-64`;
     - plus `cmake`, `ninja`, `cli11`, `nlohmann_json`, `catch2` for the report.
   - **`clangxx_linux-64`, `clang-tools`, `llvm-tools` and `clang-format` must share one LLVM major.** Use the highest major all four have.
   - Keep the task table, env layout (`default` = feature `clang`, `gcc` = feature `gcc`, solve-group `main`) and the `CXXFLAGS = ""`/`LDFLAGS = ""` task envs **exactly** as in the example.
   - Never add `CMAKE_BUILD_PARALLEL_LEVEL` or `CTEST_PARALLEL_LEVEL` to any task env.
2. **Create the lock** with `pixi install`. It's in `ask`, and it creates `pixi.lock`.
   - The lock is committed and **never hand-edited**. Change it only via `pixi add` / `pixi update`.
   - If it fails with `File name too long`, that is a real failure, not a warning: give me the ecryptfs fix from step 0.4 and stop.
3. **Offer the gcc env.** `pixi install --locked -e gcc`. The gcc env is needed for `nice pixi run -e gcc check` locally; CI runs it either way. If this fails with `File name too long`, same as step 2.2: give me the step 0.4 fix and stop.

## 3. Write the scaffolds
Each scaffold supporting file is a list of blocks, each headed with its **target path**. Read each file, substitute `@TOOL@`/`@PROJ@`/`@PROJ_UPPER@`/`@DESCRIPTION@`/`@SPDX@`, and write every target. `@DESCRIPTION@` uses the escaped form from step 1.

Write them in this order, so the format hook finds `.clang-format` before any C++ lands:
1. **`.claude/skills/cpp-quality/scaffold.md`** → `.clang-format`, `.clang-format-ignore`, `.clang-tidy`, `tests/.clang-tidy`, `cmake/ProjectWarnings.cmake`, `scripts/format.sh`, `scripts/lint.sh`, `scripts/read-jobs.cmake`.
2. **`.claude/skills/build-workflow/scaffold.md`** → `CMakeLists.txt`, `cmake/Install.cmake`, `cmake/Packaging.cmake`, `scripts/preset.sh`, `scripts/cxx-probe.sh`, `scripts/cov.sh`, `scripts/audit.sh`, `scripts/package.sh`, `.clangd`.
   - **Skip** its `CMakeUserPresets.json` template. `/features` writes that file per worktree, and it is never committed.
3. **`.claude/skills/build-workflow/ci-release.md`** → **only** `scripts/release-check.sh` now. `release-test` needs it whether or not CI is set up. The rest of that file is step 9.
4. **`.claude/skills/memory-safety/scaffold.md`** → `cmake/Sanitizers.cmake`, `cmake/Hardening.cmake`, `src/app/sanitizer_options.cpp.in`, `tools/sanitizers/asan.supp`, `ubsan.supp`, `lsan.supp`.
5. **`.claude/skills/cli-design/scaffold.md`** → `src/app/*`, `include/@PROJ@/*`, `src/core/*`.
6. **`.claude/skills/cpp-testing/scaffold.md`** → `tests/**` and `scripts/bless.sh`.
   - **Never write anything under `tests/e2e/golden/`.** Goldens come only from `bless` (step 7).
7. **`CMakePresets.json`** from the root `CMakePresets.example.json`, with placeholders substituted. It has no `jobs` and no hardcoded `$schema`.
8. **`.gitignore`.** Merge the root `.gitignore` lines into the project's `.gitignore` idempotently: add only missing lines and never drop existing ones.

Scripts are run as `bash scripts/<x>.sh` by the pixi tasks, so no exec bit is needed.

**After all scaffold files are written, always run `nice pixi run fmt` once, before the first `check`.** Do this unconditionally.
- Include order and line lengths depend on the substituted names, so the scaffolds aren't guaranteed to be clang-format-clean for every `@TOOL@`/`@PROJ@`.
- Format-hook notices raised while you were writing the files are expected at this point. Don't hand-edit around them; `fmt` fixes them.

## 4. C++23 feature probe
Run `nice pixi run cxx-probe .claude/skills/platform-testing/probes/probe_features.cpp`. If the gcc env is installed, also run `nice pixi run -e gcc cxx-probe .claude/skills/platform-testing/probes/probe_features.cpp`.
- **Exit 0:** the required `__cpp_lib_*` macros are present (`expected`, `print`, `format` with the libc++ 18 exception, `ranges_to_container`, `to_underlying`, `unreachable`). Record the report-only macros it prints.
- **Exit 125 (compile failed / `#error`):** the toolchain is below the C++23 floor. Stop, show which macro failed, and send it to `fact-checker` + `build-fixer`. Don't paper over it by editing the probe.
- **Exit 2:** a rejected path. That is a studio bug, so report it.

## 5. Sanitizer usability probe (option C gate)
Delegate to `platform-tester` with scope `probe`. It reports ASan+UBSan state **per env** (clang `default`, and `gcc` if installed):
- `absent`;
- `present-but-unusable`;
- `detection-broken`;
- `degraded`;
- `usable`.

It also reports the symbolizer and linkage.

**What to do with each state:**
- **`usable`**: continue.
- **`degraded`** (detection works, but stacks are unsymbolized, R3): explain it (the symbolizer comes from `llvm-tools` in the env), then ⏸ ask whether to continue.
- **Anything else, for the default env: STOP.** Option C (sanitized dev preset as the gate) can't hold. Present the agent's workarounds **as reported, not applied**:
  - high-entropy ASLR → you run `sudo sysctl -w vm.mmap_rnd_bits=28` yourself;
  - ptrace/seccomp restrictions in a container;
  - a missing env.

  Never run them yourself.
- **The gcc env not usable:** the same STOP-and-report, because CI's gcc matrix leg would fail. If the gcc env isn't installed, say so (R1) and continue.

## 6. Green gate
Run `nice pixi run check` with an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` and wait. A cold first build is slow. The chain is `fmt-check` → `test` → `lint` → `release` → `binary-packages`.
- **Expected failures before the first bless:** `+golden` E2E cases whose golden file doesn't exist yet. If those are the only failures, go to step 7 and re-run `check` afterwards.
- **Any other failure:**
  - build/configure/link/lint infrastructure → `build-fixer`;
  - scaffold code → `bug-fixer`;
  - then `quality-reviewer` reviews (build-review mode for the build surface).

  If the scaffold trips a specific clang-analyzer check, disable **that one check only**, with a reason comment in `.clang-tidy`.

If the gcc env is installed, also run `nice pixi run -e gcc check`.

## 7. Initial goldens
⏸ Ask, then run `nice pixi run bless`. It is in `ask`; the bless script is the only place `UPDATE_GOLDEN` is ever set. It generates the initial `--help`/`--version` goldens.
1. Show me the generated files under `tests/e2e/golden/` and the `git diff --stat` that bless prints.
2. Re-run `nice pixi run check`. It **must be green.**

## 8. Verify-at-use-time checklist
These are the facts the scaffold depends on that can only be confirmed on this machine. Route a failure to `build-fixer`. Report each item ✅/❌ with its evidence.
1. **The dev binary resolves its runtime from the env.**
   - `readelf -d build/default/dev/e2e-prefix/bin/@TOOL@` shows an RPATH/RUNPATH into `.pixi/envs/default/lib` (via `$env{CONDA_PREFIX}`).
   - `ldd` on it shows `libstdc++` resolved from the env, not `/usr/lib`.
2. **The build dir is `build/default/dev`.** It must not be `build//dev`, which would mean `$env{PIXI_ENVIRONMENT_NAME}` expanded empty.
3. **The release binary passes its gates.** Run `nice pixi run release-test`. `release-check.binary` must pass, which means:
   - no RPATH/RUNPATH;
   - NEEDED within {libc, libm, ld-linux};
   - glibc floor ≤ `@PROJ_UPPER@_GLIBC_FLOOR` (2.28);
   - hardening markers present.

   Also show `readelf -d` on `build/default/release/e2e-prefix/bin/@TOOL@`. The readelf markers are binutils conventions, so confirm them against this build's actual output.
4. **lint is real.** The `lint` step of `check` reported a **nonzero file count** and no standard-header errors. Header errors would mean the conda driver name in `compile_commands.json` isn't resolving its sysroot.
5. **`.clang-format` is valid.** `pixi run clang-format --dump-config --assume-filename=src/app/main.cpp` exits 0 and shows the set values for `Standard`, `IndentWidth`, `AccessModifierOffset`, `InsertBraces` and `IncludeCategories`.
6. **Every check named in `tests/.clang-tidy` exists.** Run `pixi run clang-tidy --list-checks -checks='-*,<name>'` per check. Drop any missing check with a comment saying why.
7. **The `.clang-tidy` CheckOptions keys are real.** Each key appears in `pixi run clang-tidy --dump-config` output.
   - clang-tidy **silently ignores unknown keys** and may echo them back, so also have `fact-checker` confirm each option name against the installed major's check docs.
   - Don't claim validation you didn't do.
8. **Trailing args reach ctest.** `nice pixi run test -R unit` runs only `unit.*` tests. If everything ran, trailing-arg forwarding is broken; send that to `build-fixer`.
9. **`depends-on` de-duplicates.** In the `check` output, the dev `configure`/`build` ran once. A duplicate is a cost note, not a failure.
10. **Editor integration.** `build/default/dev/compile_commands.json` exists and `.clangd` points at `build/default/dev`.
    - Tell me editors need `--query-driver=<abs repo>/.pixi/envs/*/bin/*` for clangd.
    - A root `compile_commands.json` symlink is optional and gitignored: `ln -s build/default/dev/compile_commands.json compile_commands.json` (it prompts).
11. **Toolchain floor.** GCC ≥ 14 / Clang ≥ 19 (from step 4 and `pixi list`). Recorded, not hardcoded.

## 9. Optional: CI and release
⏸ Ask whether to write CI. If yes, write from `.claude/skills/build-workflow/ci-release.md`:
- `.github/workflows/ci.yml`;
- `.github/workflows/release.yml`;
- `.github/dependabot.yml` (github-actions updates).

Separately ⏸ ask whether to write the optional AUR `packaging/aur/PKGBUILD` (uses `@SPDX@`).

**Pin every action by full commit SHA, resolved now** (never from memory). For each `uses: <owner>/<action>@…`:
1. Get the latest release tag: `gh api --method GET repos/<owner>/<action>/releases/latest --jq .tag_name`.
2. Get the tag's ref: `gh api --method GET repos/<owner>/<action>/git/ref/tags/<tag>`. Read `.object.sha` and `.object.type`.
3. If the type is `tag` (an annotated tag), dereference it: `gh api --method GET repos/<owner>/<action>/git/tags/<sha> --jq .object.sha`.
4. Write `uses: <owner>/<action>@<40-hex sha> # <tag>`.
5. If the resolved major differs from what the template assumes, have `fact-checker` confirm the inputs the template uses against that tag's `action.yml`. Examples: `prefix-dev/setup-pixi`'s `locked`/`environments`, attest's required `artifact-metadata: write` permission.

`gh api` is in `ask`. **If `gh` is missing, unauthenticated or the lookup fails,** leave the template's placeholder with a `# TODO: pin SHA` comment and tell me. The workflow won't run until those pins are filled.

Keep these from the template:
- `runs-on: ubuntu-26.04` pinned;
- `on: pull_request` (never `_target`);
- `persist-credentials: false`;
- least `permissions`.

Releases happen only from tag-push CI. Never run `gh release`, `git tag` or `git push --tags` yourself.

**Fill in `CLAUDE.md` → Project specifics:**
- tool name, namespace and option prefix;
- subcommands (initially `hello`) and the exit codes 3–63 in use;
- whether the tool is threaded (ask);
- SIGPIPE policy A;
- optional tiers enabled;
- packaging;
- glibc floor;
- the pixi cache dir note (if ecryptfs was detected or the cache was moved);
- clang-format style;
- docker images for platform smoke runs (ask; you pull them yourself);
- extra toolchain presets;
- GitHub remote + default branch.

## 10. Report
Report:
- **The layout** written, as a tree.
- **Resolved versions:** the `pixi list` majors for clang/gcc/cmake/CLI11/Catch2/nlohmann_json.
- **Filesystem warnings:** any ecryptfs detection from step 0.4 (cache dir, `$HOME`, repo), or `none`.
- **Probe results:** C++23 macros, and sanitizer state per env.
- **Gate results:** `check`, plus `-e gcc check` if run.
- **Goldens blessed.**
- **The step 8 checklist** with ✅/❌.
- **CI pin status.**
- **Remaining manual steps:**
  - copy `.claude/settings.local.json.example` to `.claude/settings.local.json` and edit it yourself (the protect hook blocks me);
  - set the cache-dir export if advised;
  - pull any docker images;
  - run `/agents`.
- **The commands I'll use:**
  - `/feature`, `/features`, `/plan`, `/implement`, `/review`, `/fix`, `/harden`, `/platform-test`;
  - `nice pixi run check`;
  - `nice pixi run -e gcc check`;
  - `nice pixi run test -R <regex>`;
  - `nice pixi run release-test`.

**Execution rules**
- **Low priority.** Run heavy commands at low OS priority locally as `nice pixi run …`: `check`, `-e gcc check`, `test`, `lint`, `release`, `release-test`, `cxx-probe`, `bless`, `fmt`.
  - Use bare `nice`, never in CI.
  - Never swallow the exit code: no `|| true`, and no piping gate output through `tail`/`head`.
  - See the `low-priority-execution` skill.
- **Long gate runs.** Every gate run gets an explicit long Bash `timeout` (up to 600000 ms), or `run_in_background` with a wait for completion. A cold `check` routinely exceeds the 120 s default.
  - A run killed by the tool timeout is not a failure.
  - The build lock self-recovers from a dead owner.
- **No inline assignments.** Never prefix a command with an inline `VAR=…` assignment.
- **Never set these variables:** `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, `UPDATE_GOLDEN`, `PIXI_CACHE_DIR` or `RATTLER_CACHE_DIR`.
- **Stop and ask** on any blocker (missing pixi, a `File name too long` install failure, sanitizers unusable, a red gate that won't converge) or a non-converging fix loop. An ecryptfs detection alone is a warning, not a blocker.
