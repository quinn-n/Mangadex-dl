---
name: build-workflow-scaffold
description: Supporting file of the build-workflow skill (scaffold templates read by /setup). Not a standalone skill.
---

# build-workflow scaffold

`/setup` writes each block below to the **target path in its heading**, substituting the placeholders `@TOOL@` (binary name), `@PROJ@` (`[a-z][a-z0-9_]*`: namespace and CMake prefix), `@PROJ_UPPER@`, `@DESCRIPTION@` and `@SPDX@`. It never overwrites an existing file; for an existing project it shows a per-file diff and asks. `/features` writes the `CMakeUserPresets.json` template (§ at the end), per worktree.

Rules for anyone editing these files later: see the `build-workflow` skill. `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml` and `scripts/*` are serialization points: never edited by two parallel tracks in one batch.

Cross-file names (from the shared contract; other skills' scaffolds rely on them):
- `cmake/ProjectWarnings.cmake` (`cpp-quality`) defines `@PROJ@_warnings` and `@PROJ@_apply_warnings(<tgt>)`, which links the warnings target privately and sets `COMPILE_WARNING_AS_ERROR` when `@PROJ_UPPER@_WARNINGS_AS_ERRORS` is ON.
- `cmake/Sanitizers.cmake` (`memory-safety`) applies the sanitizer flags project-wide, defines `@PROJ@_add_sanitizer_options(<tgt>)` and `@PROJ_UPPER@_SANITIZER_TEST_ENV`, and writes `<binaryDir>/sanitizer-flags.txt` (compile+link flags, whitespace- or newline-separated) and `<binaryDir>/sanitizer-env.txt` (`KEY=VALUE` lines). `scripts/cxx-probe.sh` reads both.
- `cmake/Hardening.cmake` (`memory-safety`) applies the probed release hardening when `@PROJ_UPPER@_HARDENING` is ON.
- `src/core/version.cpp.in` (`cli-design`) is configured with `@ONLY`; it uses `@PROJECT_VERSION@` (substituted by CMake, not by `/setup`).
- `tests/CMakeLists.txt` (`cpp-testing`) may use `@PROJ@_app_sources` (the app sources without `main.cpp`) to build the in-process CLI tests.
- `scripts/lint.sh` (`cpp-quality`) takes the same build-dir lock as `scripts/preset.sh` (protocol in the skill, §5).

## Deriving `pixi.toml` and `CMakePresets.json` from the root examples
1. Copy `pixi.example.toml` → `pixi.toml`. Substitute `@TOOL@`, `@DESCRIPTION@` and `@PROJ_UPPER@` (the last two appear only in the commented `man` and `fuzz` tasks). For each `"<major>.*"` pin, run `pixi search <pkg>` and use the current major. `clangxx_linux-64`, `clang-tools`, `llvm-tools` and `clang-format` must share one LLVM major; `gxx_linux-64` takes the current GCC major. Keep `cmake = ">=3.31"`.
2. `pixi install` (asks) creates `pixi.lock`; offer `pixi install --locked -e gcc`. If extraction fails with `File name too long`, stop and advise `PIXI_CACHE_DIR`/`RATTLER_CACHE_DIR` (skill §2).
3. Copy `CMakePresets.example.json` → `CMakePresets.json`. Substitute `@PROJ_UPPER@`. Keep `version: 10`; add a `$schema` only if the user's editor wants one, pinned to the installed CMake.
4. Check: `pixi run cmake --list-presets=all` lists `dev`, `release` and `cov`; after `pixi run configure`, `build/default/dev/compile_commands.json` exists. After `pixi run test`, `readelf -d build/default/dev/e2e-prefix/bin/@TOOL@` shows the env RPATH.
5. Merge the root `.gitignore` into the project's `.gitignore` (append missing lines only).

## `CMakeLists.txt`
The top-level build. Order is fixed by the shared contract (see the skill, §6).

```cmake
# @TOOL@ -- @DESCRIPTION@
# Manager-agnostic: only find_package(<Pkg> CONFIG REQUIRED) + imported targets, so the
# same file builds under pixi (presets), vcpkg, and a distro package (AUR, -DCMAKE_BUILD_TYPE=None).
# Serialization point: never edited by two parallel tracks in one batch.
cmake_minimum_required(VERSION 3.31...4.4)

project(@PROJ@
  VERSION 0.1.0 # the single version source: --version, CPack and the release tag guard read it
  DESCRIPTION "@DESCRIPTION@"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest) # defines BUILD_TESTING (ON by default)

# ---- options (presets set these; defaults suit a plain distro build) ----------------
set(@PROJ_UPPER@_SANITIZE "" CACHE STRING
  "Sanitizers for the whole build: \"\" | \"address;undefined\" | \"thread\" (never shipped)")
set_property(CACHE @PROJ_UPPER@_SANITIZE PROPERTY STRINGS "" "address;undefined" "thread")
option(@PROJ_UPPER@_HARDENING "Release hardening flags (OpenSSF set, probed per compiler)" OFF)
option(@PROJ_UPPER@_HARDENING_EXTRA "Opt-in -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing" OFF)
option(@PROJ_UPPER@_WARNINGS_AS_ERRORS "Per-target COMPILE_WARNING_AS_ERROR (presets/CI: ON; packagers: OFF)" OFF)
set(@PROJ_UPPER@_GLIBC_FLOOR "2.28" CACHE STRING "Highest GLIBC_x.y symbol version the release binary may require")
option(@PROJ_UPPER@_LIFETIME_SAFETY "Clang -Wlifetime-safety-permissive (advisory, probed)" OFF)
option(@PROJ_UPPER@_FUZZ "Build libFuzzer targets + corpus-replay tests (clang only)" OFF)

if(@PROJ_UPPER@_HARDENING AND NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "")
  message(FATAL_ERROR
    "@PROJ_UPPER@_HARDENING and @PROJ_UPPER@_SANITIZE are mutually exclusive "
    "(release hardening and sanitizers are never combined in one build).")
endif()

# Defaults for builds that don't use the presets (the presets set them too).
if(NOT DEFINED CMAKE_CXX_EXTENSIONS)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()
if(NOT DEFINED CMAKE_CXX_SCAN_FOR_MODULES)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++ modules in this project
endif()

# ---- project-wide instrumentation (before any target) -------------------------------
# Sanitizer and hardening flags are applied project-wide by design (source-built deps get
# instrumented too); warnings / -Werror stay per-target (ProjectWarnings.cmake).
include("${PROJECT_SOURCE_DIR}/cmake/Sanitizers.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Hardening.cmake")

# Sanitized links are memory-hungry: cap concurrent links (Ninja only) unless a module
# already set a link pool.
if(NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "" AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_JOB_POOL_LINK)
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS @PROJ@_link_pool=2)
  set(CMAKE_JOB_POOL_LINK @PROJ@_link_pool)
endif()

# ---- dependencies --------------------------------------------------------------------
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

include("${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake")

# ---- version: project(VERSION) -> one generated TU (a bump recompiles one file) ------
configure_file(
  "${PROJECT_SOURCE_DIR}/src/core/version.cpp.in"
  "${PROJECT_BINARY_DIR}/generated/version.cpp"
  @ONLY)

# ---- core library: all logic lives here (unit-testable, no CLI11) --------------------
# CONFIGURE_DEPENDS globbing: a new module/subcommand doesn't touch this file (trade-off
# documented in the build-workflow skill; the alternative is explicit lists).
file(GLOB_RECURSE @PROJ@_core_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/core/*.cpp")
add_library(@PROJ@_core STATIC ${@PROJ@_core_sources} "${PROJECT_BINARY_DIR}/generated/version.cpp")
add_library(@PROJ@::core ALIAS @PROJ@_core)
target_compile_features(@PROJ@_core PUBLIC cxx_std_23)
target_include_directories(@PROJ@_core
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  PRIVATE "${PROJECT_SOURCE_DIR}/src/core")
target_link_libraries(@PROJ@_core PUBLIC nlohmann_json::nlohmann_json)
@PROJ@_apply_warnings(@PROJ@_core)

# ---- the executable: a thin CLI layer over the core ----------------------------------
file(GLOB @PROJ@_cmd_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/app/cmd_*.cpp")
# Everything but main.cpp, for the in-process CLI tests (tests/CMakeLists.txt).
set(@PROJ@_app_sources
  "${PROJECT_SOURCE_DIR}/src/app/io.cpp"
  "${PROJECT_SOURCE_DIR}/src/app/cli.cpp"
  ${@PROJ@_cmd_sources})
add_executable(@PROJ@_app "${PROJECT_SOURCE_DIR}/src/app/main.cpp" ${@PROJ@_app_sources})
set_target_properties(@PROJ@_app PROPERTIES OUTPUT_NAME @TOOL@)
target_include_directories(@PROJ@_app PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
target_link_libraries(@PROJ@_app PRIVATE @PROJ@::core CLI11::CLI11)
@PROJ@_apply_warnings(@PROJ@_app)
# The shipped binary carries libstdc++/libgcc statically (portable: needs only glibc). Scoped
# to this target: test executables keep the dynamic libstdc++ that conda's shared-only
# libCatch2.so uses. Distro builds (hardening OFF) link dynamically as packagers expect.
if(@PROJ_UPPER@_HARDENING)
  target_link_options(@PROJ@_app PRIVATE -static-libstdc++ -static-libgcc)
endif()
@PROJ@_add_sanitizer_options(@PROJ@_app) # no-op unless sanitizing

# ---- install (components: runtime, docs) ---------------------------------------------
include("${PROJECT_SOURCE_DIR}/cmake/Install.cmake")

# ---- tests (only when this is the top-level project) ---------------------------------
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
  add_subdirectory(tests)
endif()

# ---- packaging -------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
  include("${PROJECT_SOURCE_DIR}/cmake/Packaging.cmake")
endif()
```

## `cmake/Install.cmake`
Install rules for the `runtime` and `docs` components.

```cmake
# Install rules. Relative destinations only (GNUInstallDirs), so `--prefix` staging, the
# E2E fixture (build/<env>/<preset>/e2e-prefix), CPack and DESTDIR packaging all work.
# Components: `runtime` (the binary) and `docs` (man page, completions, license/readme).
# The E2E fixture installs them in two calls (multi-component --component is 4.4-only).
# Optional files are picked up at configure time: after adding one, reconfigure.

install(TARGETS @PROJ@_app
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)

# Man page: an explicit man1/ destination -- install(FILES ... TYPE MAN) omits the man1/
# section dir.
if(EXISTS "${PROJECT_SOURCE_DIR}/docs/@TOOL@.1")
  install(FILES "${PROJECT_SOURCE_DIR}/docs/@TOOL@.1"
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT docs)
endif()

# Shell completions (hand-written, drift-tested). bash-completion looks up <tool>.bash
# (the unsuffixed name is deprecated); zsh wants _<tool>; fish <tool>.fish.
if(EXISTS "${PROJECT_SOURCE_DIR}/completions/@TOOL@.bash")
  install(FILES "${PROJECT_SOURCE_DIR}/completions/@TOOL@.bash"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions" COMPONENT docs)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/completions/_@TOOL@")
  install(FILES "${PROJECT_SOURCE_DIR}/completions/_@TOOL@"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/zsh/site-functions" COMPONENT docs)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/completions/@TOOL@.fish")
  install(FILES "${PROJECT_SOURCE_DIR}/completions/@TOOL@.fish"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d" COMPONENT docs)
endif()

# Docs under share/doc/@TOOL@ (GNUInstallDirs defaults to the project name, @PROJ@). Only
# the default is replaced, so an explicit -DCMAKE_INSTALL_DOCDIR=... still wins.
if(CMAKE_INSTALL_DOCDIR STREQUAL "${CMAKE_INSTALL_DATAROOTDIR}/doc/${PROJECT_NAME}")
  set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/@TOOL@")
endif()

foreach(doc IN ITEMS LICENSE README.md)
  if(EXISTS "${PROJECT_SOURCE_DIR}/${doc}")
    install(FILES "${PROJECT_SOURCE_DIR}/${doc}"
      DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT docs)
  endif()
endforeach()

# Never CMAKE_INSTALL_RPATH_USE_LINK_PATH (it would bake .pixi/ paths into installs). The
# dev/cov presets set CMAKE_INSTALL_RPATH to the env on purpose; release sets none.
```

## `cmake/Packaging.cmake`
CPack TGZ. `CPACK_COMPONENTS_ALL` does not filter a TGZ; the staged-file-list test is the guard.

```cmake
# CPack: a portable release tarball (TGZ). Included only when top-level.
# Version is inherited from project(VERSION) -- never set it twice.

set(CPACK_PACKAGE_NAME "@TOOL@")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_GENERATOR TGZ) # DEB/RPM are opt-in; verify their default prefixes at use-time
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_CHECKSUM SHA256) # writes <file>.sha256 next to the archive
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _@PROJ@_arch)
set(CPACK_PACKAGE_FILE_NAME "@TOOL@-${PROJECT_VERSION}-linux-${_@PROJ@_arch}")
if(EXISTS "${PROJECT_SOURCE_DIR}/LICENSE")
  set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
endif()
# No CPACK_THREADS: it only affects xz/zstd, not .tar.gz.

# Informational for component-aware generators. It does NOT filter a monolithic TGZ:
# everything installed by any component -- including the Unspecified component of a fetched
# dependency's install rules -- lands in the archive. The guard is the
# `release-check.staged-files` test, which must list what CPack actually ships (the tarball,
# or a component-less `cmake --install` into a scratch prefix), never e2e-prefix (that only
# installs `runtime` + `docs`); plus EXCLUDE_FROM_ALL SYSTEM on any FetchContent dependency.
set(CPACK_COMPONENTS_ALL runtime docs)

# `make package_source` must never sweep up build trees, envs or local artifacts.
set(CPACK_SOURCE_GENERATOR TGZ)
set(CPACK_SOURCE_IGNORE_FILES
  "/\\.git/" "/build/" "/\\.pixi/" "/\\.platform-runs/" "/\\.cache/"
  "/CMakeUserPresets\\.json$" "/compile_commands\\.json$" "/\\.claude/settings\\.local\\.json$")

# A sanitized or coverage-instrumented build is never packaged: cpack itself fails.
# (scripts/package.sh refuses earlier with a clearer message.)
if(NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "" OR CMAKE_CXX_FLAGS MATCHES "-fprofile-instr-generate")
  set(_@PROJ@_refuse "${PROJECT_BINARY_DIR}/CPackRefuse.cmake")
  file(WRITE "${_@PROJ@_refuse}"
    "message(FATAL_ERROR \"refusing to package: this build dir is sanitized or coverage-instrumented; use the release preset (pixi run package)\")\n")
  set(CPACK_PROJECT_CONFIG_FILE "${_@PROJ@_refuse}")
endif()

include(CPack)
```

## `scripts/preset.sh`
Drives every preset-based task. The lock block between the `build-dir lock` markers must stay byte-identical to the one in `scripts/lint.sh` (`cpp-quality` scaffold); `preset.sh`'s child-process extension follows the block.

```bash
#!/usr/bin/env bash
# preset.sh <configure|build|test|package> <preset> [args]
#
# The one way pixi tasks drive CMake presets. It:
#   - refuses to run outside pixi (the binaryDir is build/$PIXI_ENVIRONMENT_NAME/<preset>);
#   - selects `<preset>-local` when the untracked CMakeUserPresets.json defines one for this
#     kind (per-worktree job caps written by /features), unsetting CMAKE_BUILD_PARALLEL_LEVEL
#     and CTEST_PARALLEL_LEVEL so they cannot override the cap (CLI -j > env > preset jobs);
#   - holds the atomic per-build-dir lock (build/<env>/<preset>.lock.d/, same protocol as
#     scripts/lint.sh) that records its owner and is taken over only when the owner and the
#     tool process it launched are provably gone; a killed run therefore never wedges the dir;
#   - forwards only whitelisted ctest arguments (exit 2 otherwise);
#   - after configure, fails if the conda activation flags leaked into the cache.
# Exit status is the underlying cmake/ctest/cpack status; 2 = usage; 75 = lock wait timed out.
set -uo pipefail

usage() {
  echo "usage: preset.sh configure <preset> [--fresh]" >&2
  echo "       preset.sh build <preset>" >&2
  echo "       preset.sh test <preset> [-R re] [-L re] [-LE re] [-E re] [--rerun-failed] [--output-on-failure] [-V]" >&2
  echo "       preset.sh package <preset>" >&2
  exit 2
}

[[ $# -ge 2 ]] || usage
kind=$1
base=$2
shift 2

case "$kind" in
  configure | build | test | package) ;;
  *) usage ;;
esac
[[ $base =~ ^[A-Za-z0-9_-]+$ ]] || { echo "preset.sh: bad preset name '$base'" >&2; exit 2; }

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

env_name=${PIXI_ENVIRONMENT_NAME:-}
if [[ -z $env_name ]]; then
  echo "preset.sh: PIXI_ENVIRONMENT_NAME is not set -- run this through pixi (e.g. 'pixi run test')." >&2
  exit 2
fi

# ---- argument whitelist -------------------------------------------------------------
extra=()
case "$kind" in
  configure)
    while [[ $# -gt 0 ]]; do
      case "$1" in
        --fresh) extra+=("--fresh"); shift ;;
        *) echo "preset.sh: configure accepts only --fresh (got '$1')" >&2; exit 2 ;;
      esac
    done
    ;;
  build | package)
    if [[ $# -gt 0 ]]; then
      echo "preset.sh: $kind takes no extra arguments (got '$*')" >&2
      exit 2
    fi
    ;;
  test)
    while [[ $# -gt 0 ]]; do
      case "$1" in
        -R | -L | -LE | -E)
          [[ $# -ge 2 ]] || { echo "preset.sh: $1 needs a value" >&2; exit 2; }
          extra+=("$1" "$2")
          shift 2
          ;;
        --rerun-failed | --output-on-failure | -V)
          extra+=("$1")
          shift
          ;;
        *)
          echo "preset.sh: ctest argument '$1' is not whitelisted (allowed: -R -L -LE -E --rerun-failed --output-on-failure -V)" >&2
          exit 2
          ;;
      esac
    done
    ;;
esac

# ---- pick <preset>-local when the user presets define it ----------------------------
preset=$base
if [[ -f CMakeUserPresets.json ]]; then
  listing=$(cmake --list-presets="$kind" 2>&1) || {
    echo "preset.sh: 'cmake --list-presets=$kind' failed (is CMakeUserPresets.json valid?):" >&2
    echo "$listing" >&2
    exit 1
  }
  if grep -Eq "^[[:space:]]+\"${base}-local\"" <<<"$listing"; then
    preset="${base}-local"
    for v in CMAKE_BUILD_PARALLEL_LEVEL CTEST_PARALLEL_LEVEL; do
      if [[ -n ${!v+x} ]]; then
        echo "preset.sh: unsetting $v=${!v} so the per-worktree cap in '$preset' applies" >&2
        unset "$v"
      fi
    done
    echo "preset.sh: using $kind preset '$preset' (per-worktree cap from CMakeUserPresets.json)" >&2
  fi
fi

bindir="build/$env_name/$base"
PROG=preset.sh

# ---- build-dir lock: this block is identical in scripts/preset.sh and scripts/lint.sh ----
# <binaryDir>.lock.d/ is taken with an atomic mkdir. <lock>/owner records the holder, one key per line:
#   pid=<pid>  host=<uname -n>  pstart=<field 22 of /proc/<pid>/stat>  started=<UTC ISO-8601>  cmd=<label>
# A waiter takes over a lock whose owner is gone (no such PID on this host, or the PID was reused,
# i.e. pstart differs), printing a note. Otherwise it waits at most LOCK_WAIT_S seconds, then gives up.
LOCK_WAIT_S=1800
LOCK_DIR=""
lock_proc_start() {
  local s
  s="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
  s="${s##*') '}"
  # shellcheck disable=SC2086  # deliberate word splitting of the stat fields
  set -- $s
  [ "$#" -ge 20 ] || return 1
  printf '%s\n' "${20}"
}
lock_field() { sed -n "s/^$2=//p" "$1/owner" 2>/dev/null | head -n 1; }
lock_owner_desc() {
  if [ ! -f "$1/owner" ]; then
    printf 'an owner that never recorded itself'
    return
  fi
  printf 'pid %s on %s, "%s", since %s' "$(lock_field "$1" pid)" "$(lock_field "$1" host)" \
    "$(lock_field "$1" cmd)" "$(lock_field "$1" started)"
}
lock_token() { printf '%s:%s' "$(lock_field "$1" pid)" "$(lock_field "$1" pstart)"; }
lock_is_stale() {
  local d="$1" pid host pstart now mtime cur
  if [ ! -f "$d/owner" ]; then
    # The holder dies between mkdir and writing owner, or is just about to write it: stale after 60 s.
    now="$(date +%s)"
    mtime="$(stat -c %Y "$d" 2>/dev/null)" || return 1
    [ $((now - mtime)) -gt 60 ]
    return
  fi
  pid="$(lock_field "$d" pid)"
  host="$(lock_field "$d" host)"
  pstart="$(lock_field "$d" pstart)"
  case "$pid" in '' | *[!0-9]*) return 0 ;; esac
  [ "$host" = "$(uname -n)" ] || return 1 # another host on a shared filesystem: cannot judge, keep waiting
  if ! kill -0 "$pid" 2>/dev/null && [ ! -d "/proc/$pid" ]; then return 0; fi
  if [ -n "$pstart" ] && cur="$(lock_proc_start "$pid")" && [ "$cur" != "$pstart" ]; then return 0; fi
  return 1
}
lock_release() {
  [ -n "$LOCK_DIR" ] || return 0
  if [ "$(lock_field "$LOCK_DIR" pid)" = "$$" ]; then rm -rf "$LOCK_DIR"; fi
  LOCK_DIR=""
}
lock_acquire() { # lock_acquire <lockdir> <label>; returns 75 when the bounded wait runs out
  local d="$1" label="$2" waited=0 noted=0 grave token
  mkdir -p "$(dirname "$d")" || return 2
  while ! mkdir "$d" 2>/dev/null; do
    if lock_is_stale "$d"; then
      token="$(lock_token "$d")"
      grave="$d.stale.$$"
      if mv -T "$d" "$grave" 2>/dev/null; then
        if [ "$(lock_token "$grave")" = "$token" ]; then
          echo "$PROG: took over stale lock $d; its owner ($(lock_owner_desc "$grave")) is no longer running" >&2
          rm -rf "$grave"
          continue
        fi
        # Lost a race: the lock was re-taken between the check and the move. Put it back.
        mv -T "$grave" "$d" 2>/dev/null ||
          echo "$PROG: warning: could not restore $d from $grave; delete $grave once nothing builds" >&2
      fi
    fi
    if [ "$noted" -eq 0 ]; then
      echo "$PROG: waiting for $d (held by $(lock_owner_desc "$d")); giving up after ${LOCK_WAIT_S}s" >&2
      noted=1
    fi
    if [ "$waited" -ge "$LOCK_WAIT_S" ]; then
      echo "$PROG: gave up after ${LOCK_WAIT_S}s waiting for $d (held by $(lock_owner_desc "$d")). If that process is no longer running, delete $d and retry." >&2
      return 75
    fi
    sleep 2
    waited=$((waited + 2))
  done
  LOCK_DIR="$d"
  trap 'lock_release' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  trap 'exit 129' HUP
  {
    printf 'pid=%s\n' "$$"
    printf 'host=%s\n' "$(uname -n)"
    printf 'pstart=%s\n' "$(lock_proc_start "$$" || true)"
    printf 'started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$label"
  } >"$d/owner.tmp" && mv -f "$d/owner.tmp" "$d/owner"
}
# ---- end of the build-dir lock block ----

# preset.sh extension of the shared block: record the tool process it launches (child=,
# cstart=) and treat a lock whose owner died but whose tool still runs (an orphaned
# `cmake --build`) as live, so nobody builds into the same dir concurrently.
eval "$(declare -f lock_is_stale | sed '1s/^lock_is_stale/shared_lock_is_stale/')"
lock_is_stale() {
  local d="$1" cpid cstart cur
  if [ -f "$d/owner" ]; then
    cpid="$(lock_field "$d" child)"
    cstart="$(lock_field "$d" cstart)"
    case "$cpid" in
      '' | *[!0-9]*) ;;
      *)
        if [ -d "/proc/$cpid" ]; then
          cur="$(lock_proc_start "$cpid" || true)"
          if [ -z "$cstart" ] || [ -z "$cur" ] || [ "$cur" = "$cstart" ]; then
            return 1
          fi
        fi
        ;;
    esac
  fi
  shared_lock_is_stale "$d"
}

lock_acquire "$bindir.lock.d" "scripts/preset.sh $kind $base"
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

# On INT/TERM/HUP stop the running tool's whole process tree first, then release the lock
# (the EXIT trap set by lock_acquire), so nothing outlives the lock.
child=""
descendants() { # $1 = pid -> it and all its descendants (pgrep -P; else just $1)
  local c
  echo "$1"
  command -v pgrep >/dev/null 2>&1 || return 0
  for c in $(pgrep -P "$1" 2>/dev/null); do
    descendants "$c"
  done
}
on_signal() { # $1 = exit status
  if [ -n "$child" ]; then
    # shellcheck disable=SC2046 # word splitting of the pid list is intended
    kill -TERM $(descendants "$child") 2>/dev/null
    wait "$child" 2>/dev/null
  fi
  exit "$1"
}
trap 'on_signal 130' INT
trap 'on_signal 143' TERM
trap 'on_signal 129' HUP

run_tracked() { # run "$@" as a recorded child of the lock owner; returns its status
  local rc
  # Non-interactive bash starts async commands with SIGINT/SIGQUIT ignored, and that would be
  # inherited by everything under test (breaking the `timeout -s INT` -> 130 E2E case).
  # Reset them in the subshell; exec keeps $! = the tool's pid.
  ( trap - INT QUIT; exec "$@" ) </dev/null &
  child=$!
  {
    printf 'child=%s\n' "$child"
    printf 'cstart=%s\n' "$(lock_proc_start "$child" || true)"
  } >>"$bindir.lock.d/owner"
  wait "$child"
  rc=$?
  child=""
  return "$rc"
}

# ---- run ----------------------------------------------------------------------------
status=0
case "$kind" in
  configure)
    run_tracked cmake --preset "$preset" "${extra[@]}"
    status=$?
    if [[ $status -eq 0 && -f $bindir/CMakeCache.txt ]]; then
      # Conda activation leak markers (build-workflow skill, flag-leak rule): -march=nocona,
      # -isystem or -fno-plt in the C++ flags; an rpath or --disable-new-dtags in the linker flags.
      if grep -Eq '^CMAKE_CXX_FLAGS:[A-Z]*=.*(-march=nocona|-isystem|-fno-plt)' "$bindir/CMakeCache.txt" ||
        grep -Eq '^CMAKE_(EXE|SHARED)_LINKER_FLAGS:[A-Z]*=.*(-Wl,-rpath|--disable-new-dtags)' "$bindir/CMakeCache.txt"; then
        echo "preset.sh: conda activation flags leaked into $bindir/CMakeCache.txt (CMAKE_CXX_FLAGS / linker flags)." >&2
        echo "preset.sh: reconfigure from scratch through the task env: 'pixi run configure --fresh' (or '_release-configure --fresh')." >&2
        status=1
      fi
    fi
    ;;
  build)
    run_tracked cmake --build --preset "$preset"
    status=$?
    ;;
  test)
    if [[ $preset == "$base" ]]; then
      # no per-worktree cap: use every core (ctest's own default is nproc but env can
      # override it; an explicit -j wins over CTEST_PARALLEL_LEVEL)
      extra=(-j "$(nproc)" "${extra[@]}")
    fi
    run_tracked ctest --preset "$preset" "${extra[@]}"
    status=$?
    ;;
  package)
    run_tracked cpack --preset "$preset"
    status=$?
    ;;
esac
exit "$status"
```

## `scripts/cxx-probe.sh`
Compiles and runs one probe with the dev preset's sanitizer flags. Exit: the probe's own; 125 compile failure; 2 rejected path/usage.

```bash
#!/usr/bin/env bash
# cxx-probe.sh <file.cpp> [--preset dev|tsan] [--no-sanitize]
#
# Compile one probe with the dev (or tsan) preset's exact sanitizer flags and run it with
# the same *_OPTIONS CTest uses. Only files under .claude/skills/platform-testing/probes/
# or .platform-runs/ are accepted (it compiles AND runs code).
# Exit: the probe's own exit status (after "cxx-probe: exit=<n>"); 125 = compile failure;
#       2 = rejected path / usage / missing preset outputs.
# Run via `nice pixi run cxx-probe <file>`; the task depends on `configure`, which writes
# build/<env>/dev/sanitizer-{flags,env}.txt.
set -uo pipefail

usage() {
  echo "usage: cxx-probe.sh <file.cpp> [--preset dev|tsan] [--no-sanitize]" >&2
  exit 2
}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 2
cd "$root" || exit 2

src=""
preset=dev
sanitize=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      [[ $# -ge 2 ]] || usage
      preset=$2
      shift 2
      ;;
    --no-sanitize)
      sanitize=0
      shift
      ;;
    -*) usage ;;
    *)
      [[ -z $src ]] || usage
      src=$1
      shift
      ;;
  esac
done
[[ -n $src ]] || usage
case "$preset" in dev | tsan) ;; *) echo "cxx-probe: --preset must be dev or tsan" >&2; exit 2 ;; esac

env_name=${PIXI_ENVIRONMENT_NAME:-}
if [[ -z $env_name || -z ${CXX:-} || -z ${CONDA_PREFIX:-} ]]; then
  echo "cxx-probe: run through pixi ('nice pixi run cxx-probe <file>'): PIXI_ENVIRONMENT_NAME/CXX/CONDA_PREFIX unset" >&2
  exit 2
fi

# ---- path allowlist (resolved: no '..' or symlink escapes) -------------------------
real=$(realpath -e -- "$src" 2>/dev/null) || { echo "cxx-probe: no such file: $src" >&2; exit 2; }
[[ $real == *.cpp ]] || { echo "cxx-probe: not a .cpp file: $src" >&2; exit 2; }
allowed=0
for r in ".claude/skills/platform-testing/probes" ".platform-runs"; do
  rr=$(realpath -m -- "$root/$r")
  if [[ $real == "$rr"/* ]]; then
    allowed=1
  fi
done
if [[ $allowed -ne 1 ]]; then
  echo "cxx-probe: rejected '$src': probes must live under .claude/skills/platform-testing/probes/ or .platform-runs/" >&2
  exit 2
fi

# ---- flags and runtime options from the configured preset --------------------------
bindir="build/$env_name/$preset"
flags=()
if [[ $sanitize -eq 1 ]]; then
  for f in "$bindir/sanitizer-flags.txt" "$bindir/sanitizer-env.txt"; do
    if [[ ! -f $f ]]; then
      if [[ $preset == dev ]]; then
        echo "cxx-probe: $f is missing -- run 'pixi run configure' first" >&2
      else
        echo "cxx-probe: $f is missing -- configure the tsan preset first (the optional tsan tasks in pixi.toml)" >&2
      fi
      exit 2
    fi
  done
  # whitespace- or newline-separated flags; '#' lines are comments
  while read -r line; do
    [[ -z $line || $line == \#* ]] && continue
    read -r -a parts <<<"$line"
    flags+=("${parts[@]}")
  done <"$bindir/sanitizer-flags.txt"
  if [[ ${#flags[@]} -eq 0 ]]; then
    echo "cxx-probe: $bindir/sanitizer-flags.txt is empty -- is the '$preset' preset sanitized?" >&2
    exit 2
  fi
fi

name=$(basename -- "$real" .cpp)
outdir=".platform-runs/cxx-probe/$env_name"
[[ $sanitize -eq 1 ]] || name="$name.nosan"
mkdir -p "$outdir" || exit 2
out="$outdir/$name"

# -O1 like the dev preset (the probes are written to survive -O1); the rpath matches the
# dev preset's CMAKE_BUILD_RPATH so the env's libstdc++ is used, not the host's; -pthread
# because the conda sysroot's glibc predates the libpthread merge.
echo "cxx-probe: $CXX -std=c++23 -O1 -g ${flags[*]} -pthread $src -o $out" >&2
if ! "$CXX" -std=c++23 -O1 -g "${flags[@]}" -pthread "$real" -o "$out" -Wl,-rpath,"$CONDA_PREFIX/lib"; then
  echo "cxx-probe: compile failed"
  exit 125
fi

# ---- run with the preset's runtime options (KEY=VALUE lines) -----------------------
if [[ $sanitize -eq 1 ]]; then
  while IFS= read -r line; do
    [[ -z $line || $line == \#* ]] && continue
    key=${line%%=*}
    if [[ ! $key =~ ^[A-Z_][A-Z0-9_]*$ || $line != *=* ]]; then
      echo "cxx-probe: ignoring malformed line in sanitizer-env.txt: $line" >&2
      continue
    fi
    export "${key}=${line#*=}"
  done <"$bindir/sanitizer-env.txt"
fi

"./$out"
rc=$?
echo "cxx-probe: exit=$rc"
exit "$rc"
```

## `scripts/cov.sh`
Report-only coverage (clang `cov` preset).

```bash
#!/usr/bin/env bash
# cov.sh -- report-only source-based coverage (clang `cov` preset, unsanitized).
#
# configure -> build -> ctest on build/<env>/cov with LLVM_PROFILE_FILE pointing into the
# build dir, then llvm-profdata merge + llvm-cov report (terminal) + llvm-cov export
# (lcov file). No threshold: coverage informs review, it never gates `check`.
# Exit: nonzero if configure/build/tests fail or the LLVM tools are missing; 0 otherwise.
# In the gcc env it prints a stated skip and exits 0 (llvm-cov needs clang instrumentation).
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

env_name=${PIXI_ENVIRONMENT_NAME:-}
if [[ -z $env_name ]]; then
  echo "cov: run through pixi ('nice pixi run cov')" >&2
  exit 2
fi
if [[ $env_name != default ]]; then
  echo "cov: skipped in env '$env_name' -- source-based coverage needs the clang toolchain; run 'pixi run cov' in the default env"
  exit 0
fi
for t in llvm-profdata llvm-cov; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "cov: '$t' not found -- it comes from the llvm-tools package in the clang feature (pixi.toml)" >&2
    exit 1
  }
done

bindir="build/$env_name/cov"
profdir="$bindir/profraw"

bash scripts/preset.sh configure cov || exit $?
bash scripts/preset.sh build cov || exit $?

rm -rf "$profdir"
mkdir -p "$profdir" || exit 1
# %p = pid, %m = binary signature: one file per process, merged below. The E2E driver
# must pass LLVM_PROFILE_FILE through to the installed binary (it is not a CONDA_/PIXI_ var).
export LLVM_PROFILE_FILE="$root/$profdir/%p-%m.profraw"
bash scripts/preset.sh test cov
test_status=$?

shopt -s nullglob
raws=("$profdir"/*.profraw)
if [[ ${#raws[@]} -eq 0 ]]; then
  echo "cov: no .profraw files were written -- were any tests run?" >&2
  exit 1
fi
llvm-profdata merge -sparse "${raws[@]}" -o "$bindir/coverage.profdata" || exit 1

# Every instrumented ELF executable in the build tree (app + test binaries), excluding
# CMake's scratch dirs and the E2E install copy (same code as the build-tree binary).
objects=()
while IFS= read -r -d '' f; do
  if [[ "$(head -c 4 "$f" 2>/dev/null | od -An -c | tr -d ' ')" == '177ELF' ]]; then
    objects+=("$f")
  fi
done < <(find "$bindir" -type f -perm -u+x -not -path '*/CMakeFiles/*' -not -path '*/e2e-prefix/*' -not -path '*/_CPack_Packages/*' -print0)
if [[ ${#objects[@]} -eq 0 ]]; then
  echo "cov: no instrumented executables found under $bindir" >&2
  exit 1
fi
obj_args=("${objects[0]}")
for o in "${objects[@]:1}"; do
  obj_args+=(-object "$o")
done

ignore='(^|/)(tests|_deps|third_party|\.pixi|build)/'
echo "cov: report (own sources only; tests, deps and generated files excluded)"
llvm-cov report "${obj_args[@]}" -instr-profile="$bindir/coverage.profdata" -ignore-filename-regex="$ignore" || exit 1
llvm-cov export "${obj_args[@]}" -instr-profile="$bindir/coverage.profdata" -ignore-filename-regex="$ignore" -format=lcov >"$bindir/coverage.lcov" || exit 1
echo "cov: lcov written to $bindir/coverage.lcov (report-only; no threshold)"

if [[ $test_status -ne 0 ]]; then
  echo "cov: NOTE: tests failed (status $test_status); coverage above is from a failing run" >&2
fi
exit "$test_status"
```

## `scripts/audit.sh`
Dependency CVE scan with a coverage statement; a stated no-op when the scanners are absent.

```bash
#!/usr/bin/env bash
# audit.sh [--fail-on <negligible|low|medium|high|critical>] -- dependency CVE scan.
#
# pixi has no built-in scanner; this follows pixi's documented two-step: syft catalogs the
# installed env with its conda-meta cataloger, grype matches the SBOM against vulnerability
# data. Conda packages carry CPEs, not PURLs, so matching is heuristic.
# Always prints a coverage statement. Exit: nonzero on findings at/above the threshold (or
# a tool error); 0 with a stated no-op when syft/grype are absent. Never installs anything.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

fail_on=high # conda CPE matching is noisy; tighten with `pixi run audit --fail-on medium`
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fail-on)
      [[ $# -ge 2 ]] || { echo "audit: --fail-on needs a severity" >&2; exit 2; }
      fail_on=$2
      shift 2
      ;;
    *) echo "usage: audit.sh [--fail-on <negligible|low|medium|high|critical>]" >&2; exit 2 ;;
  esac
done
case "$fail_on" in negligible | low | medium | high | critical) ;; *) echo "audit: bad severity '$fail_on'" >&2; exit 2 ;; esac

target=".pixi/envs/default"
out_dir=".platform-runs/audit"
sbom="$out_dir/sbom.json"

coverage() { # $1 = tool line
  echo "audit: coverage statement"
  echo "  tools:        $1"
  echo "  scanned:      conda packages installed in $target (the env the release artifact is built from)"
  echo "  matching:     CPE-heuristic for conda packages (false positives and misses are possible)"
  echo "  NOT covered:  host/system libraries (glibc on the target machine), vendored or FetchContent code,"
  echo "                the gcc env, and pixi.lock itself (Dependabot / the GitHub dependency graph do not read it)"
}

have_syft=0
have_grype=0
command -v syft >/dev/null 2>&1 && have_syft=1
command -v grype >/dev/null 2>&1 && have_grype=1
if [[ $have_syft -eq 0 || $have_grype -eq 0 ]]; then
  missing=()
  [[ $have_syft -eq 1 ]] || missing+=(syft)
  [[ $have_grype -eq 1 ]] || missing+=(grype)
  coverage "none (missing: ${missing[*]})"
  echo "audit: NO-OP -- ${missing[*]} not on PATH; nothing was scanned. Install them yourself (they are not installed by this task) or rely on the scheduled CI audit job."
  exit 0
fi
if [[ ! -d $target ]]; then
  echo "audit: $target does not exist -- run 'pixi install' first" >&2
  exit 1
fi

syft_v=$(syft version 2>/dev/null | sed -n 's/^Version:[[:space:]]*//p' | head -n 1)
grype_v=$(grype version 2>/dev/null | sed -n 's/^Version:[[:space:]]*//p' | head -n 1)
coverage "syft ${syft_v:-?}, grype ${grype_v:-?} (fail on >= $fail_on)"

mkdir -p "$out_dir" || exit 1
syft "$target" --select-catalogers=+conda-meta-cataloger --output "syft-json=$sbom" || {
  echo "audit: syft failed" >&2
  exit 1
}
# --fail-on makes grype exit nonzero on findings; without it grype exits 0 regardless.
# Verify the flag against the installed grype (`grype --help`) -- it is checked here too.
# Capture first: under pipefail, `grype --help | grep -q` can report grype's SIGPIPE when grep exits early.
grype_help=$(grype --help 2>&1 || true)
if grep -q -- '--fail-on' <<<"$grype_help"; then
  grype "$sbom" --fail-on "$fail_on"
  status=$?
else
  echo "audit: WARNING: this grype has no --fail-on; findings are listed but cannot fail the task" >&2
  grype "$sbom"
  status=$?
fi
echo "audit: SBOM kept at $sbom"
exit "$status"
```

## `scripts/package.sh`
Release tarball, default env only.

```bash
#!/usr/bin/env bash
# package.sh -- build the release tarball with `cpack --preset release`.
#
# The `package` task depends on release-test, so the release build is configured, built
# and tested first. Refuses: any env other than `default` (the gcc wrapper always adds an
# rpath into the env, so only the clang env's release binary is shippable) and any
# sanitized or coverage-instrumented build dir. Output: build/default/release/
# @TOOL@-<version>-linux-<arch>.tar.gz plus its .sha256 (CPACK_PACKAGE_CHECKSUM).
# Publishing is CI's job (tag-triggered release workflow), never an agent's.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

env_name=${PIXI_ENVIRONMENT_NAME:-}
if [[ $env_name != default ]]; then
  echo "package: refused in env '${env_name:-<none>}' -- release artifacts are built in the default (clang) env only: 'pixi run package'" >&2
  exit 1
fi
cache="build/$env_name/release/CMakeCache.txt"
if [[ ! -f $cache ]]; then
  echo "package: $cache is missing -- 'pixi run package' configures and tests the release build first" >&2
  exit 1
fi
if grep -Eq '^@PROJ_UPPER@_SANITIZE:[A-Z]*=.+' "$cache"; then
  echo "package: refused -- $cache has @PROJ_UPPER@_SANITIZE set; a sanitized build is never shipped" >&2
  exit 1
fi
if grep -Eq '^CMAKE_CXX_FLAGS:[A-Z]*=.*-f(profile-instr-generate|coverage-mapping)' "$cache"; then
  echo "package: refused -- $cache is coverage-instrumented" >&2
  exit 1
fi
if ! grep -Eq '^@PROJ_UPPER@_HARDENING:[A-Z]*=(ON|1|TRUE|YES)$' "$cache"; then
  echo "package: refused -- $cache was configured without @PROJ_UPPER@_HARDENING=ON (not the release preset?)" >&2
  exit 1
fi

# Drop archives from earlier versions/runs so only this run's output is reported/uploaded.
rm -f build/"$env_name"/release/@TOOL@-*.tar.gz build/"$env_name"/release/@TOOL@-*.tar.gz.sha256
bash scripts/preset.sh package release || exit $?

shopt -s nullglob
made=(build/"$env_name"/release/@TOOL@-*.tar.gz)
if [[ ${#made[@]} -eq 0 ]]; then
  echo "package: cpack reported success but no .tar.gz is in build/$env_name/release/" >&2
  exit 1
fi
echo "package: artifacts:"
for f in "${made[@]}" build/"$env_name"/release/@TOOL@-*.tar.gz.sha256; do
  echo "  $f"
done
```

## `.clangd`
Editor integration; points clangd at the dev compile DB.

```yaml
# clangd: use the compile DB of the sanitized dev build (default/clang env).
# Use the env's clangd (.pixi/envs/default/bin/clangd, same LLVM major as the compiler).
# The DB names the conda driver (x86_64-conda-linux-gnu-clang++), so editors must let clangd
# query it: add --query-driver=<absolute repo path>/.pixi/envs/*/bin/* to your editor's clangd
# arguments (verified at /setup; it is per-machine, so it is not set here).
CompileFlags:
  CompilationDatabase: build/default/dev
```

## `CMakeUserPresets.json` (template, written by `/features` per worktree; `/setup` skips it)
Never committed (it is in `.gitignore` and `/features` also adds it to `info/exclude`). Fill `<CAP>` = max(1, nproc / N worktrees), `<LOAD>` = nproc, and `<COMMON_PARENT>` = the common parent directory of the repo and `../<repo>-worktrees`. Write numbers unquoted. Verify with `cmake --list-presets=build` (must list `dev-local`).

Invariants (the skill, §5): a `-local` **configure** preset keeps the project preset's exact `binaryDir` (never `build/<env>/dev-local`); when local configure presets exist, the local build/test presets point at them with `"configurePreset"` so the `CCACHE_*` environment applies to the build too; otherwise they only `inherits` the project build/test preset. No `CMAKE_BUILD_PARALLEL_LEVEL`/`CTEST_PARALLEL_LEVEL` anywhere.

Without ccache (`command -v ccache` found nothing):
```json
{
  "version": 10,
  "$comment": "Written by /features for this worktree only; never committed. cap=<CAP> load=<LOAD>. scripts/preset.sh selects the -local presets; scripts/read-jobs.cmake reads vendor.cpp-cli-studio.jobs for run-clang-tidy -j.",
  "buildPresets": [
    { "name": "dev-local", "inherits": "dev", "jobs": <CAP>, "nativeToolOptions": ["-l<LOAD>"] },
    { "name": "release-local", "inherits": "release", "jobs": <CAP>, "nativeToolOptions": ["-l<LOAD>"] }
  ],
  "testPresets": [
    { "name": "dev-local", "inherits": "dev", "execution": { "jobs": <CAP> } },
    { "name": "release-local", "inherits": "release", "execution": { "jobs": <CAP> } }
  ],
  "vendor": { "cpp-cli-studio": { "jobs": <CAP>, "load": <LOAD> } }
}
```

With ccache:
```json
{
  "version": 10,
  "$comment": "Written by /features for this worktree only; never committed. cap=<CAP> load=<LOAD>, ccache found. The -local configure presets keep the project presets' binaryDir; the -local build/test presets point at them so they inherit the CCACHE_* environment.",
  "configurePresets": [
    {
      "name": "dev-local",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/$env{PIXI_ENVIRONMENT_NAME}/dev",
      "cacheVariables": { "CMAKE_CXX_COMPILER_LAUNCHER": "ccache" },
      "environment": { "CCACHE_BASEDIR": "<COMMON_PARENT>", "CCACHE_NOHASHDIR": "true" }
    },
    {
      "name": "release-local",
      "inherits": "release",
      "binaryDir": "${sourceDir}/build/$env{PIXI_ENVIRONMENT_NAME}/release",
      "cacheVariables": { "CMAKE_CXX_COMPILER_LAUNCHER": "ccache" },
      "environment": { "CCACHE_BASEDIR": "<COMMON_PARENT>", "CCACHE_NOHASHDIR": "true" }
    }
  ],
  "buildPresets": [
    { "name": "dev-local", "inherits": "dev", "configurePreset": "dev-local", "jobs": <CAP>, "nativeToolOptions": ["-l<LOAD>"] },
    { "name": "release-local", "inherits": "release", "configurePreset": "release-local", "jobs": <CAP>, "nativeToolOptions": ["-l<LOAD>"] }
  ],
  "testPresets": [
    { "name": "dev-local", "inherits": "dev", "configurePreset": "dev-local", "execution": { "jobs": <CAP> } },
    { "name": "release-local", "inherits": "release", "configurePreset": "release-local", "execution": { "jobs": <CAP> } }
  ],
  "vendor": { "cpp-cli-studio": { "jobs": <CAP>, "load": <LOAD> } }
}
```
