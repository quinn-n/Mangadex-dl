---
name: build-workflow
description: How this project builds - pixi envs/tasks/deps, CMakePresets (v10), the conda flag-leak rule, manager-agnostic CMakeLists, structure and targets, per-worktree job caps and the build lock, compile-time hygiene, install/CPack/release builder, CI, and the fallbacks. Use when adding deps, tasks, presets or targets, touching CMake, scripts/ or CI, or diagnosing a pixi/configure/compile/link/CTest/release-check failure.
---

# Build workflow (pixi + CMake + Ninja)

One entrypoint: **pixi tasks**. pixi locks the whole toolchain (compiler, cmake, ninja, clang-format, clang-tidy, libraries) in `pixi.lock`; CMake presets describe the configurations; small `scripts/*.sh` hold the logic. Everybody — humans, agents, CI — types the same task names.

**Use this skill** when you add a dependency, task, preset, target or source directory; edit `CMakeLists.txt`, `cmake/*.cmake`, `CMakePresets.json`, `pixi.toml` or `scripts/`; touch `.github/`; or diagnose a build. **Not for:** warning flags and the lint config (`cpp-quality`), sanitizer and hardening flags (`memory-safety`), test registration and the E2E driver (`cpp-testing`), or the probe/tier report (`platform-testing`).

Supporting files (read by `/setup`, not standalone skills): `scaffold.md` (CMakeLists, install/packaging modules, `scripts/{preset,cxx-probe,cov,audit,package}.sh`, `.clangd`, the `CMakeUserPresets.json` template) and `ci-release.md` (workflows, Dependabot, `scripts/release-check.sh`, the AUR PKGBUILD). The root `pixi.example.toml` and `CMakePresets.example.json` are the manifest and preset sources.

## 1. Root decision (and what was rejected)
- **Chosen:** pixi + conda-forge + CMake (Ninja, `CMakePresets.json`) + pixi tasks. It is the only option that locks the compiler too, so local and CI run the same bits; per-worktree envs are hard-linked from the shared package cache.
- **Rejected:** Conan 2 (it generates/overwrites `CMakeUserPresets.json`; an extra Python tool), CPM/FetchContent as the primary dependency path (every worktree × config rebuilds every dep; mutable tags), Meson (no presets analog), pacman/apt (no lock, mutates the host, can't pin clang-format).
- **Fallbacks (documented, not default):**
  - *vcpkg manifest* (`builtin-baseline`) + a `justfile` exposing the same task names. `CMakeLists.txt` is unchanged because it only uses `find_package(<Pkg> CONFIG REQUIRED)`.
  - *System-compiler swap* if a conda compiler ever proves unusable: keep pixi for cmake/ninja/clang tools/libs; presets set `CMAKE_CXX_COMPILER`; CI installs a matching compiler; tier reports name the compiler version.
- **FetchContent is an exception**, never the default: pinned commit SHA or `URL_HASH`, `SYSTEM`, `EXCLUDE_FROM_ALL`, `FIND_PACKAGE_ARGS`. Its install rules would otherwise leak into the release tarball (§9).

## 2. pixi
- **Environments:** `default` = feature `clang` (`clangxx_linux-64`, `clang-tools`, `llvm-tools`); `gcc` = feature `gcc` (`gxx_linux-64`). Both in solve-group `main`, so shared libraries resolve identically. Common deps: `cmake`, `ninja`, `cli11`, `nlohmann_json`, `catch2`, `clang-format`.
- **One LLVM major** across `clangxx_linux-64`, `clang-tools`, `llvm-tools` and `clang-format`. clang-format is in the common deps so the gcc env formats identically. Never use the host's clang-format/clang-tidy.
- `llvm-tools` exists for `llvm-symbolizer` (readable sanitizer stacks) and `llvm-profdata`/`llvm-cov` (`cov`). `clang-tools` ships `run-clang-tidy` (no `.py`), `clangd`, `clang-include-cleaner`; `clang-tidy-diff.py` sits at `$CONDA_PREFIX/share/clang/` and is not on PATH.
- **Versions are read, never remembered:** `pixi list`, `pixi search <pkg>`, `pixi run cmake --version`, `pixi run clang-format --version`. Pins in `pixi.toml` are majors resolved at `/setup`.
- **Adding a dependency:** prefer conda-forge (`pixi search <pkg>`), then `pixi add <pkg>` (asks) — never hand-edit `pixi.lock`. Library → common `[dependencies]`; compiler-specific → its feature. Then `find_package(<Pkg> CONFIG REQUIRED)` + the imported target, nothing manager-specific. Note new deps in the PR body.
- `pixi install --locked` (CI, worktrees) aborts when `pixi.lock` is stale against `pixi.toml`; `--frozen` installs the lock as-is and ignores manifest drift. Use `--locked` unless you deliberately want the latter.
- **Tasks:** `depends-on` runs in list order and **stops at the first failure** (so `check` is fail-fast). Trailing arguments go to the task you named, not its dependencies (`pixi run test -R unit`). Hide internals with a leading `_`. Anything beyond one command lives in `scripts/*.sh` called via `bash` (pixi's task shell is not bash).
- `pixi-build` is preview (verify its status at use-time) — not for the dev loop.
- **ecryptfs / NFS home:** pixi's cache extraction can fail with `File name too long (os error 36)` (ecryptfs caps names at ~143 bytes). Detect with that message or `stat -f -c %T <dir>` = `ecryptfs`. Detection alone is only a warning (the `/setup` and `/features` preflights continue); only the actual error stops. Advise exporting `PIXI_CACHE_DIR` (wins) or `RATTLER_CACHE_DIR` to another filesystem **in the shell profile before launching Claude**. Never set it inline (the guard hook blocks that) and never set it silently. Moving the cache off the repo's filesystem means envs are copied, not hard-linked (more disk).

### Task vocabulary

| task | what it does |
|---|---|
| `fmt` / `fmt-check` | clang-format fix / check (`scripts/format.sh`) |
| `configure` → `build` → `test` | sanitized `dev` preset: configure, build, ctest (`scripts/preset.sh`) |
| `lint` (`lint-fix`, `lint-changed`) | clang-tidy over the Clang compile DB; a stated skip in the gcc env |
| `_release-configure` → `release` → `release-test` | hardened `release` preset; `release-test` runs unit + E2E + release-check on the release install |
| `check` | `fmt-check` → `test` → `lint` → `release` → `binary-packages` (the gate; run in both envs) |
| `cxx-probe <file>` | compile + run a probe with the dev preset's sanitizer flags and options |
| `bless <regex>` | re-bless E2E goldens (human-approved; `cpp-testing`) |
| `cov` | report-only clang coverage (`cov` preset) |
| `binary-packages` (`--update`) | conda packages compiled/linked into the release binary (ninja header deps + the `MANGADEX_DL_LINK_DEPFILE` linker depfile), checked against `audit/binary-packages.txt`; a stated skip in the gcc env |
| `audit` | syft + grype over the installed env, split by `audit/binary-packages.txt` into a binary and a dev scope, with a coverage statement per scope |
| `package` | `cpack --preset release` after `release-test` (default env only) |
| optional `tsan`, `fuzz`, `man` | commented in `pixi.example.toml`; enable when needed |

Run heavy tasks locally as `nice pixi run <task>` (see `low-priority-execution`); the definitions stay unwrapped so CI runs full speed.

**Long-running commands vs the Bash tool timeout.** A cold `check` (sanitized build + release build + clang-tidy) routinely takes longer than the Bash tool's 120 s default. When you run `check`, `-e gcc check`, `test`, `release`, `release-test`, `lint`, `cov` or `package`, pass an explicit long `timeout` (up to 600000 ms) or use `run_in_background` and wait for it to finish. A run killed by a timeout is **not** a failing gate — re-run it with a longer timeout. The build lock recovers from a killed run (§5).

## 3. The conda flag-leak rule (non-negotiable)
Activating a conda compiler exports `CXXFLAGS` (`-O2 -march=nocona -mtune=haswell -fPIC -fno-plt -isystem $ENV/include …`) and `LDFLAGS` (`-Wl,-rpath,$ENV/lib -Wl,--disable-new-dtags …`), plus `CPPFLAGS` and `CMAKE_ARGS`. CMake seeds `CMAKE_CXX_FLAGS` and the linker flags from `CXXFLAGS`/`LDFLAGS` on a binary dir's **first** configure (it ignores `CPPFLAGS`), which silently turns the sanitized Debug build into `-O2` and plants an RPATH into `.pixi/`.

Defence in depth — keep all three:
1. Every configure preset sets `CMAKE_CXX_FLAGS`, `CMAKE_EXE_LINKER_FLAGS` **and** `CMAKE_SHARED_LINKER_FLAGS` explicitly (empty in `base`). The `_<CONFIG>` variants are not env-seeded; presets set the ones they need.
2. Every task that configures or compiles (`configure`, `_release-configure`, `cxx-probe`, `cov`) has `env = { CXXFLAGS = "", LDFLAGS = "" }`.
3. `scripts/preset.sh` fails a configure whose cache still shows leak markers. Fix: `pixi run configure --fresh` (or `pixi run _release-configure --fresh`).

Never configure a project build dir with bare `cmake` outside the tasks.

## 4. Presets (`CMakePresets.json`, schema v10)
- `cmake_minimum_required(VERSION 3.31...4.4)` and presets **version 10** (the first with `$comment`, which documents the file in place). No hardcoded `$schema` URL.
- **Build dir:** `${sourceDir}/build/$env{PIXI_ENVIRONMENT_NAME}/${presetName}` → `build/default/dev`, `build/gcc/release`, … so the two envs never share a cache. Outside pixi the env segment is empty: always go through the tasks.
- Hidden `base`: Ninja, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, `CMAKE_CXX_EXTENSIONS=OFF`, `CMAKE_CXX_SCAN_FOR_MODULES=OFF` (no modules), `CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST` (a sanitizer crash during discovery must show up as a test failure, not a build failure), the explicit empty flag variables, `<PROJ>_WARNINGS_AS_ERRORS=ON`, `BUILD_TESTING=ON`.

| configure preset | type | key settings |
|---|---|---|
| `dev` | Debug | `CMAKE_CXX_FLAGS_DEBUG="-O1 -g"`, `<PROJ>_SANITIZE="address;undefined"`, hardening OFF, `CMAKE_BUILD_RPATH`/`CMAKE_INSTALL_RPATH` = `$env{CONDA_PREFIX}/lib` |
| `release` | Release | `CMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"`, sanitize `""`, `<PROJ>_HARDENING=ON` (which makes `CMakeLists.txt` link the app with `-static-libstdc++ -static-libgcc`), linker flags empty, no RPATH |
| `cov` | Debug | `-O0 -g`, `-fprofile-instr-generate -fcoverage-mapping` (clang only), unsanitized, env RPATH |
| `tsan` (optional) | Debug | template below; only for threaded code |

- Build and test presets `dev`, `release`, `cov` (tests: `outputOnFailure`, `noTestsAction: "error"`). Workflow presets `dev` and `release` (configure → build → test) and package preset `release` (TGZ) are conveniences; the tasks call configure/build/test separately so the per-worktree cap applies.
- **No `jobs` in committed presets.** Caps are per-worktree (§5).
- **Why the dev RPATH points into the env:** the clang env compiles against the env's (newer) libstdc++ headers. Without the RPATH the installed dev binary would resolve the *host's* libstdc++ and fail on older hosts. Release instead links the app's libstdc++/libgcc statically and carries no RPATH. `/setup` verifies both with `readelf -d` + `ldd`.
- **Workflow preset rules:** the first step is the configure preset; every later step must use that same configure preset.
- Adding a preset: inherit `base`, set every flag variable you change explicitly, add matching build/test presets, add a task pair if agents should run it (with the `env` blanking), and update this table.

`tsan` template (add to `CMakePresets.json` and uncomment the `tsan` tasks; TSan and ASan never share a build):
```json
{ "name": "tsan", "inherits": "base", "displayName": "tsan: Debug -O1 -g, ThreadSanitizer",
  "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "CMAKE_CXX_FLAGS_DEBUG": "-O1 -g",
    "<PROJ>_SANITIZE": "thread", "<PROJ>_HARDENING": "OFF",
    "CMAKE_BUILD_RPATH": "$env{CONDA_PREFIX}/lib", "CMAKE_INSTALL_RPATH": "$env{CONDA_PREFIX}/lib" } }
```
plus `{ "name": "tsan", "configurePreset": "tsan" }` in `buildPresets` and `{ "name": "tsan", "inherits": "test-base", "configurePreset": "tsan" }` in `testPresets`.

## 5. Per-worktree job caps and the build lock
**Why `-local` presets:** a `CMakeUserPresets.json` preset can't redefine (shadow) a project preset name, so putting `jobs` on a user preset called `dev` is impossible, and `jobs` in the committed file would apply to everyone. Precedence for build parallelism is **CLI `-j` > env `CMAKE_BUILD_PARALLEL_LEVEL` > preset `jobs`**. For ctest, `CTEST_PARALLEL_LEVEL` unset means ctest's default, and `0` means unbounded. How a test preset's `execution.jobs` interacts with `CTEST_PARALLEL_LEVEL` is checked at use-time.

The mechanism:
- `/features` writes an untracked `CMakeUserPresets.json` per worktree (template in `scaffold.md`): `dev-local`/`release-local` build presets (`jobs = cap`, `nativeToolOptions: ["-l<load>"]`), matching test presets (`execution.jobs = cap`), configure `-local` presets **only when ccache is present**, and `"vendor": {"cpp-cli-studio": {"jobs": <cap>, "load": <load>}}` (read by `scripts/read-jobs.cmake` for `run-clang-tidy -j`). cap = max(1, nproc / N worktrees); load = nproc.
- **Invariant:** a `-local` *configure* preset sets `binaryDir` to exactly the project preset's (`${sourceDir}/build/$env{PIXI_ENVIRONMENT_NAME}/dev` or `/release`). Inheriting alone would give `build/<env>/dev-local`, breaking `lint.sh`, the hooks, `.clangd`, the E2E paths and the lock. When a local configure preset exists, the local build/test presets set `"configurePreset": "<name>-local"`, so they inherit its environment (`CCACHE_*`); otherwise they `inherits` the project build/test preset, which points at the project configure preset.
- `scripts/preset.sh <kind> <preset>` uses `<preset>-local` whenever `cmake --list-presets=<kind>` lists it. In that case it unsets `CMAKE_BUILD_PARALLEL_LEVEL`/`CTEST_PARALLEL_LEVEL` and says so. Without a local test preset it passes `ctest -j "$(nproc)"`.
- **Never set `CMAKE_BUILD_PARALLEL_LEVEL` or `CTEST_PARALLEL_LEVEL`** in a task, in CI, or in a shell profile you control. No inline `VAR=… cmd` either: it defeats Claude Code's allow rules.
- Sanitized links are memory-hungry: `CMakeLists.txt` puts links in a Ninja job pool of 2 when sanitizing (unless a module already set `CMAKE_JOB_POOL_LINK`).
- ccache only when probed (`command -v ccache`), via the `-local` configure preset (`CMAKE_CXX_COMPILER_LAUNCHER=ccache`, `CCACHE_BASEDIR` = the worktrees' common parent, `CCACHE_NOHASHDIR=true`). Never commit a launcher.
- `nice` lowers priority; it caps neither parallelism nor RAM (see `low-priority-execution`).

**The build lock** (one build dir per worktree, shared by that worktree's parallel tracks). Protocol — the lock block is identical in `scripts/preset.sh` and `scripts/lint.sh`:
- Acquire = atomic `mkdir build/<env>/<preset>.lock.d`. The holder writes `<lock>/owner` with `pid=`, `host=` (`uname -n`), `pstart=` (the process start time from `/proc/<pid>/stat`, which guards against PID reuse), `started=` (UTC) and `cmd=`. `preset.sh` also appends `child=`/`cstart=` for the cmake/ctest/cpack process it launches.
- A waiter takes over only a lock whose owner is on this host and gone (no such PID, or the PID was reused), or an owner-less lock older than 60 s. `preset.sh` additionally treats a lock as live while its recorded tool process still runs (an orphaned `cmake --build` after its script was killed). Takeover is an atomic `mv` of the lock dir plus re-verification, and prints a note naming the dead owner.
- Bounded wait: one note naming the holder, then after 30 min exit **75** with the holder's details and the manual remedy (delete the lock dir if that process is gone).
- Release: the `EXIT` trap removes the lock only if `$$` owns it. On INT/TERM/HUP, `preset.sh` first TERMs the running tool's process tree, so nothing outlives the lock.
- A compile error in a file another track owns is a **coordination event**, not something to fix in your track.

## 6. Structure, targets and the CMakeLists order
```
CMakeLists.txt  CMakePresets.json  pixi.toml  pixi.lock          # serialization points
cmake/{ProjectWarnings,Sanitizers,Hardening,Install,Packaging}.cmake
include/<proj>/            # lean public headers of the core
src/core/**.cpp            # <proj>_core (STATIC) + private headers; version.cpp.in
src/app/{main,io,cli}.cpp  src/app/cmd_<sub>.cpp                     # thin CLI layer
tests/{unit,cli,e2e,sanitizer-controls,support}/  tools/sanitizers/*.supp
scripts/*.sh  docs/<tool>.1  completions/{<tool>.bash,_<tool>,<tool>.fish}
```
- **Targets:** `<proj>_core` (STATIC, alias `<proj>::core`, `cxx_std_23` PUBLIC, all logic, no CLI11); `<proj>_app` (`OUTPUT_NAME <tool>`, links core + `CLI11::CLI11` privately); `<proj>_warnings` (INTERFACE, via `<proj>_apply_warnings(tgt)`); `<proj>_unit_tests`, `<proj>_cli_tests` (in `tests/`). `CMakeLists.txt` exports `<proj>_app_sources` (the app sources without `main.cpp`) for the in-process CLI tests.
- **Order** (fixed): `cmake_minimum_required(VERSION 3.31...4.4)` → `project(<proj> VERSION … LANGUAGES CXX)` → `GNUInstallDirs` + `CTest` → options → `cmake/Sanitizers.cmake` → `cmake/Hardening.cmake` (FATAL if both on) → `find_package(CLI11 …)`, `find_package(nlohmann_json …)` → `cmake/ProjectWarnings.cmake` → configure `version.cpp` → core → app → `<proj>_add_sanitizer_options(<proj>_app)` → `cmake/Install.cmake` → `if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)` Catch2 + `tests/` → `if(PROJECT_IS_TOP_LEVEL)` `cmake/Packaging.cmake`.
- Options: `<PROJ>_SANITIZE` (`""` / `"address;undefined"` / `"thread"`), `<PROJ>_HARDENING`, `<PROJ>_HARDENING_EXTRA`, `<PROJ>_WARNINGS_AS_ERRORS` (default OFF; presets and CI turn it ON; packagers pass `--compile-no-warning-as-error` or leave it OFF), `<PROJ>_GLIBC_FLOOR` (2.28), `<PROJ>_LIFETIME_SAFETY`, `<PROJ>_FUZZ`, `<PROJ>_LINK_DEPFILE` (default OFF; the `release` preset turns it ON: the app's link writes `<build>/mangadex-dl.link.d`, which `scripts/binary-packages.sh` reads; fails configure if the linker lacks `--dependency-file`).
- **Scope:** sanitizer and hardening flags are project-wide by design, so source-built deps get instrumented too. Warnings and `-Werror` are per-target. Never `add_compile_options` for warnings or `include_directories`.
- **Version single source:** `project(VERSION)` → `configure_file(src/core/version.cpp.in → <binaryDir>/generated/version.cpp @ONLY)`, so a bump recompiles one TU. `--version`'s first line is `<tool> X.Y.Z`.
- **Globbing:** `src/core/**/*.cpp`, `src/app/cmd_*.cpp` (and the test dirs) use `CONFIGURE_DEPENDS`, so adding a module, subcommand, test or E2E case touches no shared CMake. This is a documented trade-off: the alternative is explicit lists, which turn every addition into a serialization point.
- Avoid CMake features newer than the floor: `execute_process(ENVIRONMENT)` and multi-component `cmake --install --component a b` are 4.4-only. `CMAKE_LINKER_TYPE` (3.29) only when the linker is probed.

## 7. Compile-time hygiene
Lean public headers with forward declarations (`nlohmann/json_fwd.hpp`). `#include <CLI/CLI.hpp>` only in `src/app/*.cpp`. Diagnose slow TUs with `-ftime-trace` (clang) before restructuring. An optional PRIVATE precompiled header is allowed on the test target only; sanitized and release flag sets can't share PCHs. Unity builds are developer-opt-in, never in presets. A faster linker goes through `CMAKE_LINKER_TYPE`, and only when probed.

## 8. Install and CPack (`cmake/Install.cmake`, `cmake/Packaging.cmake`)
- **Relative destinations only** (`CMAKE_INSTALL_BINDIR` etc.). Never absolute paths derived from `find_package`: they break `--prefix` staging and DESTDIR.
- Components: `runtime` (the binary) and `docs` (man page, completions, LICENSE/README). The E2E fixture installs them into `build/<env>/<preset>/e2e-prefix` with two `cmake --install --component` calls.
- The man page goes to `${CMAKE_INSTALL_MANDIR}/man1` explicitly. `install(FILES … TYPE MAN)` omits `man1/`.
- Completions: `share/bash-completion/completions/<tool>.bash` (the unsuffixed name is deprecated), `share/zsh/site-functions/_<tool>`, `share/fish/vendor_completions.d/<tool>.fish`. Optional files are picked up at configure time, so reconfigure after adding one.
- CPack: `TGZ`, version inherited from `project(VERSION)`, `CPACK_VERBATIM_VARIABLES ON`, `CPACK_STRIP_FILES ON`, `CPACK_PACKAGE_CHECKSUM SHA256` (a single value; a list of algorithms needs CMake ≥ 4.2), file name `<tool>-<ver>-linux-<arch>`. Packaging a sanitized or coverage build fails (`package.sh` refuses, and cpack itself aborts via `CPACK_PROJECT_CONFIG_FILE`).
- **`CPACK_COMPONENTS_ALL` does NOT filter a monolithic TGZ.** Everything any component installs lands in the archive, including a fetched dependency's install rules. The guard is the `release-check.staged-files` test (`cpp-testing`), which must inspect what CPack actually ships — `tar tzf` of the tarball or a component-less `cmake --install` into a scratch prefix — **never `e2e-prefix`**: that installs only `runtime` + `docs`, so an Unspecified-component leak would pass it. Plus `EXCLUDE_FROM_ALL SYSTEM` on any FetchContent dep. `CPACK_ARCHIVE_COMPONENT_INSTALL` + `CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE` is an alternative, verified at use-time.
- No `CPACK_THREADS`: it only affects xz/zstd, not `.tar.gz`. Under CMake ≥ 4.3, policy CMP0206 (NEW via the `...4.4` range) gives archive entries UID/GID 0.
- Code must not depend on `NDEBUG` for correctness: distro builds (`CMAKE_BUILD_TYPE=None`) keep asserts.

## 9. Release builder
- The shipped binary is built by the **pixi clang toolchain in the `default` env**, from the `release` preset. The conda sysroot targets glibc 2.28. With `<PROJ>_HARDENING` ON, `CMakeLists.txt` links **the app target only** with `-static-libstdc++ -static-libgcc` (`target_link_options`), so the binary needs only glibc. Never put those flags in the preset's `CMAKE_EXE_LINKER_FLAGS` or in `Hardening.cmake`: test executables must keep the dynamic libstdc++ that conda's shared-only `libCatch2.so` uses. `scripts/release-check.sh` gates it: PIE, non-exec stack, RELRO, BIND_NOW, **no RPATH/RUNPATH**, `NEEDED` ⊆ glibc's libraries, highest `GLIBC_x.y` ≤ `<PROJ>_GLIBC_FLOOR`.
- The **gcc env's release build is a `-Werror` + test check, never shipped**: the conda gcc wrapper always adds `DT_RPATH=$ENV/lib`, so release-check only warns about RPATH there.
- Alternative: a manylinux_2_28 container builder (documented, not default). Never ship a fully-static glibc binary or a sanitized build. Reproducibility is release-only and best effort: `SOURCE_DATE_EPOCH` from git, `-ffile-prefix-map=<src>/=` if you add it, no `__DATE__`/`__TIME__`, no bit-for-bit promise.

## 10. CI and releases (templates in `ci-release.md`)
- `runs-on: ubuntu-26.04` pinned (don't rely on `ubuntu-latest` while it migrates). `prefix-dev/setup-pixi` with an exact `pixi-version`, `locked: true`, cache on.
- Matrix over envs: `pixi run -e <env> check`, then `pixi run -e <env> release-test`. A scheduled `audit` job. Defensive `sudo sysctl -w vm.mmap_rnd_bits=28 || true` (hosted images already set it; it matters on self-hosted/containers). Optional macOS/Windows jobs are commented out; they are the only way to license those tiers.
- Every action is pinned by **full commit SHA + `# <tag>`**, resolved at `/setup` (`gh api`), never typed from memory; Dependabot (`github-actions`) keeps them current. `permissions: contents: read` by default; `on: pull_request` (never `pull_request_target`); `persist-credentials: false`; concurrency cancels superseded PR runs. CI runs at normal priority (no `nice`).
- Release = **tag push only**. Build/test/package job (read-only) → publish job (`contents: write`, `id-token: write`, `attestations: write`, `artifact-metadata: write`): `SHA256SUMS` → attest → `gh release create --verify-tag --draft` → upload → publish.
- **Agents never run** un-prefixed `cmake --install`, `makepkg -i`, `gh release …`, `git tag` / `git push --tags`, or `sudo`. Installs go to `--prefix build/…` via the E2E fixture; releases come from CI.
- Dependabot has no pixi.lock support; refresh the lock deliberately (`pixi update`, asked) and review the diff.

## 11. AUR (optional, `packaging/aur/PKGBUILD`)
Arch's CMake template: `cmake -B build -S … -G Ninja -W no-dev -D CMAKE_BUILD_TYPE=None -D CMAKE_INSTALL_PREFIX=/usr -D <PROJ>_WARNINGS_AS_ERRORS=OFF`, `check()` = `ctest --test-dir build --output-on-failure -E '^release-check\.'` (distro builds are neither hardened nor static), `package()` = `DESTDIR="$pkgdir" cmake --install build` plus the license under `/usr/share/licenses/$pkgname/`. `makedepends` includes `cmake ninja` and the system `cli11 nlohmann-json catch2`. `license=()` takes SPDX identifiers (`LicenseRef-…` for custom). Regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`. namcap is advisory. Verify `-git`/`-bin` package naming conventions at use-time.

## 12. Diagnosing builds (for `build-fixer`)
| symptom | likely cause → fix |
|---|---|
| Debug build is `-O2`, `-march=nocona` in commands, RPATH into `.pixi/` | flag leak → §3; `pixi run configure --fresh` |
| `pixi install --locked` fails "lock file not up-to-date" | `pixi.toml` changed without `pixi install` → run it (asks), commit the lock |
| `File name too long (os error 36)` during install | ecryptfs cache/home → §2 (advise, don't set) |
| `GLIBCXX_3.4.xx not found` running a dev binary | missing env RPATH (dev/cov presets) or a binary run outside the build tree |
| unknown `-W…` flag is an error under clang only | per-compiler gating in `ProjectWarnings.cmake` (`cpp-quality`) |
| `lint` "0 files" / std-header errors | filter or DB path wrong; DB compiler major ≠ clang-tidy major (`cpp-quality`) |
| tests vanish or a crash during discovery | PRE_TEST mode not set / a sanitizer finding at listing time (`cpp-testing`) |
| `noTestsAction` error | the test dir wasn't added or a filter matched nothing |
| E2E fixture installs nothing / wrong paths | missing `COMPONENT`, or an absolute install destination |
| release-check: RPATH / NEEDED libstdc++ / GLIBC too new | wrong env (gcc), static flags lost, or a host library linked in |
| `preset.sh` / `lint.sh` exits 75 | lock wait timed out: another build is genuinely running in this worktree, or delete the lock dir it names if that process is gone |
| `preset.sh` exits 2 | not under pixi, or a non-whitelisted ctest argument |

Prove fixes with `nice pixi run check`; add `nice pixi run -e gcc check` for compiler-specific fixes and `nice pixi run release-test` for the release surface. A version re-pin goes back to the planner/fact-checker.

## 13. Verify at use-time (never baked in)
Tool and library versions and majors; action SHAs/tags and setup-pixi inputs; `$env{CONDA_PREFIX}` RPATH behaviour and the empty-env binaryDir; pixi trailing-argument forwarding and `depends-on` de-duplication; test-preset `execution.jobs` vs `CTEST_PARALLEL_LEVEL`; `CPACK_COMPONENTS_GROUPING`; DEB/RPM default prefixes; `gh release edit --draft=false`; readelf marker spelling against your own build; grype's `--fail-on`; whether `syft`/`grype` are on conda-forge for CI; help2man's exit-0 requirement; AUR naming.

Sibling skills: `cpp-quality` (format/lint/warnings, the gate), `memory-safety` (sanitizer/hardening modules), `cpp-testing` (tests/E2E/goldens), `platform-testing` (probes and tiers), `low-priority-execution` (`nice`), `cpp-security` (`audit`, supply chain).
