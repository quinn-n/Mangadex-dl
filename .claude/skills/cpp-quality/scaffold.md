---
name: cpp-quality-scaffold
description: Supporting file of the cpp-quality skill (scaffold templates read by /setup). Not a standalone skill.
---

# cpp-quality scaffold

`/setup` writes these files into the user's repository. The policy behind them lives in `SKILL.md` next to this file; this file is only the templates.

**How `/setup` uses it:**
- Each `###` heading below is the **target path**, relative to the repo root. The fenced block under it is the complete file content.
- Substitute `@PROJ@` (lowercase CMake prefix and namespace) and `@PROJ_UPPER@`. These templates use no other placeholders.
- **Never overwrite an existing file.** For an existing project, show a per-file diff and wait for the human (⏸).
- Write the scripts as plain files. The pixi tasks invoke them as `bash scripts/<x>.sh`, so no exec bit is needed.
- After writing, run the verification steps at the end of this file.

---

### `.clang-format`

```yaml
---
# clang-format config. The formatter owns layout, braces and include order; never hand-format.
# Use only the pixi-pinned clang-format (same LLVM major as the compiler). Output drifts across majors.
# Keys are checked at /setup with: clang-format --dump-config (an unknown key exits 1).
# Style debates (indent 2 vs 4, LLVM vs Google, InsertBraces vs tidy brace check): see the cpp-quality skill.
Language: Cpp
BasedOnStyle: LLVM
Standard: Latest                 # validated at /setup
IndentWidth: 4                   # debate: LLVM default 2
AccessModifierOffset: -4         # pairs with IndentWidth 4 (public:/private: flush with the class keyword)
ColumnLimit: 100
PointerAlignment: Left
DerivePointerAlignment: false
InsertBraces: true               # formatter-owned braces; readability-braces-around-statements is off in .clang-tidy
InsertNewlineAtEOF: true
LineEnding: LF
IncludeBlocks: Regroup
IncludeCategories:               # first match wins; priority = block order
  - Regex: '^"'                  # project
    Priority: 1
  - Regex: '^<[^>]*\.h>$'        # C / POSIX (incl. <sys/…h>)
    Priority: 4
  - Regex: '^<[[:alnum:]_.-]+/'  # third-party <lib/…>
    Priority: 2
  - Regex: '^<[[:alnum:]_]+>$'   # C++ standard library
    Priority: 3
# Deliberately absent (they can change semantics or churn diffs): QualifierAlignment,
# RemoveParentheses, RemoveSemicolon, EnumTrailingComma.
...
```

### `.clang-format-ignore`

```text
# Paths clang-format skips even when a file is passed explicitly (format.sh, format-check hook).
# Patterns are relative to this file's directory. `*` does not cross `/` here, so `build/*` would only
# match files directly in build/; `**` matches at any depth (checked with clang-format 23).
.claude/**
build/**
third_party/**
```

### `.clang-tidy`

```yaml
---
# clang-tidy config for this project's own sources. Lint runs from the Clang compile DB
# (build/default/dev) via scripts/lint.sh, with the pixi-pinned clang-tidy (same major as clang).
#
# Policy (see the cpp-quality skill):
# - Enable whole modules; subtract noise, formatter-owned checks and runtime-covered checks.
# - Never add portability-*, llvm-*, google-*, fuchsia-*, llvmlibc-*, altera-*.
# - If the program becomes multi-threaded, add: concurrency-mt-unsafe
# - Disable a single clang-analyzer check only with a written reason next to it.
# - clang-tidy SILENTLY IGNORES unknown CheckOptions keys. /setup confirms each key below
#   appears in `clang-tidy --dump-config` output; a typo here does nothing, silently.
#
# -readability-redundant-member-init: modern-cpp-conventions requires `{}` default member initializers on
#   class-type members (so partial designated init stays warning-free under -Werror on both compilers);
#   this check flags exactly those (`std::string message{};`) and has no option to exempt them.
# Do not put comments inside the folded `Checks: >` block: a `#` line there becomes part of the check string.
Checks: >
  -*, bugprone-*, cert-err33-c, clang-analyzer-*, cppcoreguidelines-*, misc-*, modernize-*,
  performance-*, readability-*,
  -bugprone-easily-swappable-parameters, -readability-magic-numbers,
  -cppcoreguidelines-avoid-magic-numbers, -readability-identifier-length,
  -modernize-use-trailing-return-type, -cppcoreguidelines-avoid-do-while,
  -cppcoreguidelines-owning-memory, -misc-no-recursion, -misc-multiple-inheritance,
  -modernize-use-nodiscard, -performance-enum-size,
  -readability-braces-around-statements, -readability-inconsistent-ifelse-braces,
  -readability-trailing-comma,
  -cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
  -readability-redundant-member-init
WarningsAsErrors: '*'
HeaderFilterRegex: '.*/(include|src|tests)/.*'
ExcludeHeaderFilterRegex: '.*/(_deps|third_party|external|vendor|\.pixi|vcpkg_installed)/.*'
SystemHeaders: false
FormatStyle: file
CheckOptions:
  misc-include-cleaner.IgnoreHeaders: 'CLI/.*;nlohmann/.*;catch2/.*'                         # verify at /setup
  misc-non-private-member-variables-in-classes.IgnoreClassesWithAllMemberVariablesBeingPublic: true            # aggregates; verify
  cppcoreguidelines-non-private-member-variables-in-classes.IgnoreClassesWithAllMemberVariablesBeingPublic: true  # alias
...
```

### `tests/.clang-tidy`

```yaml
---
# Test-only relaxations layered on the root .clang-tidy. Each one is caused by a Catch2 macro expansion,
# not by code we write. /setup confirms every name exists (clang-tidy --list-checks) and drops missing ones.
InheritParentConfig: true
Checks:
  - -readability-function-cognitive-complexity              # TEST_CASE + SECTION/REQUIRE expand into deeply branched function bodies
  - -cppcoreguidelines-avoid-non-const-global-variables     # TEST_CASE/TEMPLATE_TEST_CASE register through namespace-scope static AutoReg objects
  - -bugprone-chained-comparison                            # REQUIRE/CHECK(a == b) decompose as `Catch::Decomposer() <= a == b`
  - -misc-use-anonymous-namespace                           # TEST_CASE defines `static void C_A_T_C_H_T_E_S_T_n()` at namespace scope
...
```

### `cmake/ProjectWarnings.cmake`

```cmake
# cmake/ProjectWarnings.cmake: compiler warnings for this project's OWN targets.
#
# Included from the top-level CMakeLists.txt after the options, Sanitizers.cmake and Hardening.cmake,
# and before any project target is created. The options it reads are declared in CMakeLists.txt:
#   @PROJ_UPPER@_WARNINGS_AS_ERRORS  (default OFF; the presets and CI turn it ON)
#   @PROJ_UPPER@_LIFETIME_SAFETY     (default OFF; Clang-only advisory analysis)
#   @PROJ_UPPER@_SANITIZE            ("" / "address;undefined" / "thread")
#
# Usage, for every own target (core library, app, test executables):
#   @PROJ@_apply_warnings(<target>)
#
# Rules (cpp-quality skill):
# - Warnings are target-scoped. Never add_compile_options(-W...), never a global
#   CMAKE_COMPILE_WARNING_AS_ERROR: either would also -Werror source-built dependencies.
# - -Werror is the per-target COMPILE_WARNING_AS_ERROR property, behind the option. Packagers turn it
#   off with `cmake --compile-no-warning-as-error`; agents never do.
# - Flag lists are gated per compiler. Clang only *warns* on an unknown -W flag, and under -Werror that
#   warning becomes an error, so a GCC-only flag must never reach Clang (and vice versa).
include_guard(GLOBAL)
include(CheckCXXCompilerFlag)

add_library(@PROJ@_warnings INTERFACE)

set(_@PROJ@_warnings_common
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow                  # friction: constructor params vs members; members use a trailing `_`
    -Wconversion
    -Wsign-conversion         # explicit: GCC's -Wconversion does not imply it in C++
    -Wdouble-promotion
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wsuggest-override
    -Wold-style-cast          # friction: libc macros; wrap them in a small helper instead of disabling
    -Wcast-align
    -Wcast-qual
    -Wnull-dereference        # GCC: only effective with optimization (the release build catches it)
    -Wformat=2
    -Werror=format-security
    -Wimplicit-fallthrough
    -Wextra-semi)

# GCC-only. (-Wdangling-reference is already in GCC's -Wextra; -Wdangling-pointer=2 and
# -Wuse-after-free=2 are in GCC's -Wall.)
set(_@PROJ@_warnings_gcc
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast            # friction: casts that are no-ops on this platform's typedefs
    -Wtrampolines)

# Clang-only.
set(_@PROJ@_warnings_clang
    -Wcomma
    -Wunreachable-code
    -Wshadow-uncaptured-local)

# GCC sanitized builds only: sanitizer instrumentation degrades GCC's flow analysis and produces false
# positives (GCC PR 83382, PR 108306). Demote exactly these to warnings; the release build keeps full
# -Werror and catches the real ones. This is a starting list (memory-safety skill): add an entry only
# with a GCC bugzilla link in the comment.
set(_@PROJ@_gcc_sanitized_no_error
    -Wno-error=maybe-uninitialized
    -Wno-error=array-bounds
    -Wno-error=stringop-overflow
    -Wno-error=stringop-overread
    -Wno-error=stringop-truncation
    -Wno-error=null-dereference)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(@PROJ@_warnings INTERFACE
                         ${_@PROJ@_warnings_common} ${_@PROJ@_warnings_gcc})
  check_cxx_compiler_flag(-Wbidi-chars=any @PROJ_UPPER@_HAVE_WBIDI_CHARS)
  if(@PROJ_UPPER@_HAVE_WBIDI_CHARS)
    target_compile_options(@PROJ@_warnings INTERFACE -Wbidi-chars=any)
  endif()
  if(NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "")
    target_compile_options(@PROJ@_warnings INTERFACE ${_@PROJ@_gcc_sanitized_no_error})
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")   # Clang and AppleClang
  target_compile_options(@PROJ@_warnings INTERFACE
                         ${_@PROJ@_warnings_common} ${_@PROJ@_warnings_clang})
  if(@PROJ_UPPER@_LIFETIME_SAFETY)
    # Advisory: probed, and never an error even under -Werror. Its noise level is verified at use-time.
    check_cxx_compiler_flag(-Wlifetime-safety-permissive @PROJ_UPPER@_HAVE_WLIFETIME_SAFETY)
    if(@PROJ_UPPER@_HAVE_WLIFETIME_SAFETY)
      target_compile_options(@PROJ@_warnings INTERFACE
                             -Wlifetime-safety-permissive -Wno-error=lifetime-safety-permissive)
    else()
      message(WARNING "@PROJ_UPPER@_LIFETIME_SAFETY=ON but ${CMAKE_CXX_COMPILER} does not accept "
                      "-Wlifetime-safety-permissive; continuing without it.")
    endif()
  endif()
# elseif(MSVC)   # Windows CI only (optional). Verify each flag against the runner's MSVC before enabling.
#   target_compile_options(@PROJ@_warnings INTERFACE
#                          /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
else()
  message(WARNING "ProjectWarnings.cmake: no warning set for compiler '${CMAKE_CXX_COMPILER_ID}'.")
endif()

# Link the warnings PRIVATE and build-only (they never leak into an install/export interface),
# and turn on per-target -Werror when the option is ON.
function(@PROJ@_apply_warnings tgt)
  if(NOT TARGET ${tgt})
    message(FATAL_ERROR "@PROJ@_apply_warnings: '${tgt}' is not a target")
  endif()
  target_link_libraries(${tgt} PRIVATE $<BUILD_INTERFACE:@PROJ@_warnings>)
  if(@PROJ_UPPER@_WARNINGS_AS_ERRORS)
    set_target_properties(${tgt} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  endif()
endfunction()
```

### `scripts/format.sh`

```bash
#!/usr/bin/env bash
# scripts/format.sh: clang-format over this repo's own C++ files.
#   pixi run fmt-check -> bash scripts/format.sh --check   (non-mutating; part of `check`)
#   pixi run fmt       -> bash scripts/format.sh --fix     (rewrites in place)
# Uses the pixi env's clang-format only, --style=file --fallback-style=none.
# Never re-nices itself: the caller types `nice pixi run fmt-check` (low-priority-execution skill).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)" || exit 2
cd "$ROOT" || exit 2

usage() { echo "usage: bash scripts/format.sh --check | --fix" >&2; }
case "$#:${1:-}" in
  1:--check) MODE=check ;;
  1:--fix) MODE=fix ;;
  *) usage; exit 2 ;;
esac

CF="$(command -v clang-format 2>/dev/null)" || {
  echo "format: clang-format not found; run through pixi: nice pixi run fmt-check" >&2
  exit 2
}
if [ -n "${CONDA_PREFIX:-}" ]; then
  case "$CF" in
    "$CONDA_PREFIX"/*) ;;
    *) echo "format: clang-format resolves to $CF, outside the pixi env ($CONDA_PREFIX); refusing to use a host formatter" >&2
       exit 2 ;;
  esac
fi
if [ ! -f "$ROOT/.clang-format" ]; then
  echo "format: no .clang-format at the repo root (written by /setup from the cpp-quality scaffold)" >&2
  exit 1
fi

EXTS=('*.cpp' '*.cc' '*.cxx' '*.hpp' '*.h' '*.hh' '*.ipp' '*.tpp')
files=()
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  # Tracked plus untracked-but-not-ignored files, so new files are checked before their first commit.
  while IFS= read -r -d '' f; do
    [ -f "$f" ] && files+=("$f")
  done < <(git ls-files -z --cached --others --exclude-standard -- "${EXTS[@]}" \
             ':!:.claude/**' ':!:build/**' ':!:.pixi/**' ':!:third_party/**' | sort -zu)
else
  while IFS= read -r -d '' f; do
    files+=("$f")
  done < <(find . \( -path ./.claude -o -path ./build -o -path ./.pixi -o -path ./third_party -o -path ./.git \) -prune \
             -o -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.hpp' -o -name '*.h' \
                           -o -name '*.hh' -o -name '*.ipp' -o -name '*.tpp' \) -print0 | sort -zu)
fi

# Config validity first: an unknown or misspelled key makes --dump-config exit nonzero.
if [ "${#files[@]}" -gt 0 ]; then
  "$CF" --dump-config --style=file "${files[0]}" >/dev/null
else
  "$CF" --dump-config --style=file --assume-filename="$ROOT/format-probe.cpp" </dev/null >/dev/null
fi
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "format: .clang-format is invalid for $("$CF" --version 2>/dev/null) (see the error above)" >&2
  exit 1
fi

if [ "${#files[@]}" -eq 0 ]; then
  echo "format: no C++ files yet; nothing to do"
  exit 0
fi

if [ "$MODE" = check ]; then
  printf '%s\0' "${files[@]}" |
    xargs -0 -n 100 "$CF" --style=file --fallback-style=none --dry-run --Werror
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "format: files above are not clang-formatted; run: nice pixi run fmt" >&2
    exit 1
  fi
  echo "format: ${#files[@]} files clean"
else
  printf '%s\0' "${files[@]}" |
    xargs -0 -n 100 "$CF" --style=file --fallback-style=none -i
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "format: clang-format -i failed (see above)" >&2
    exit 1
  fi
  echo "format: formatted ${#files[@]} files"
fi
exit 0
```

### `scripts/read-jobs.cmake`

```cmake
# scripts/read-jobs.cmake: print this worktree's parallelism cap, or nothing.
#
#   cmake -P scripts/read-jobs.cmake              -> vendor.cpp-cli-studio.jobs
#   cmake -DKEY=load -P scripts/read-jobs.cmake   -> vendor.cpp-cli-studio.load
#
# Reads CMakeUserPresets.json at the repo root (written per worktree by /features):
#   "vendor": {"cpp-cli-studio": {"jobs": <int>, "load": <int>}}
# Prints a positive integer on stdout, or nothing when the file, the key or a sane value is
# missing. Exits 0 for any state of the file (FATAL only on a KEY other than jobs/load, which is
# caller misuse); the caller falls back to nproc.
cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED KEY)
  set(KEY jobs)
endif()
if(NOT KEY MATCHES "^(jobs|load)$")
  message(FATAL_ERROR "read-jobs.cmake: KEY must be 'jobs' or 'load', got '${KEY}'")
endif()

get_filename_component(_rj_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_rj_file "${_rj_root}/CMakeUserPresets.json")

if(EXISTS "${_rj_file}")
  file(READ "${_rj_file}" _rj_json)
  string(JSON _rj_value ERROR_VARIABLE _rj_err GET "${_rj_json}" vendor cpp-cli-studio ${KEY})
  if(_rj_value MATCHES "^[1-9][0-9]*$")
    # message() writes to stderr; echo through a child so the value lands on stdout.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_rj_value}")
  endif()
endif()
```

### `scripts/lint.sh`

```bash
#!/usr/bin/env bash
# scripts/lint.sh: clang-tidy over this project's own sources, from the Clang compile DB.
#   pixi run lint          -> bash scripts/lint.sh                      (whole project; part of `check`)
#   pixi run lint-fix      -> bash scripts/lint.sh --fix                (apply fix-its, then format.sh --fix)
#   pixi run lint-changed  -> bash scripts/lint.sh --changed [<base>]   (changed lines vs merge-base)
#
# - Default pixi env only (Clang DB at build/default/dev). Other envs: skip, exit 0 (CI covers lint).
# - Always passes an explicit -j: vendor.cpp-cli-studio.jobs from CMakeUserPresets.json, else nproc.
# - Lints own translation units only (src/, tests/), and FAILS when that set is empty:
#   run-clang-tidy reports success "for 0 files" when its filter matches nothing.
# - Holds the build-dir lock <binaryDir>.lock.d/ (shared with scripts/preset.sh) while it runs.
# - Never re-nices itself: the caller types `nice pixi run lint` (low-priority-execution skill).
# Exit: 0 clean/skipped, 1 findings or a failed precondition, 2 usage/tooling, 75 lock wait gave up.
set -uo pipefail

PROG=lint
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)" || exit 2
ROOT_L="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -L)" || ROOT_L="$ROOT"
cd "$ROOT" || exit 2

usage() { echo "usage: bash scripts/lint.sh [--fix | --changed [<base-ref>]]" >&2; }
MODE=full
BASE=""
case "$#:${1:-}" in
  0:) MODE=full ;;
  1:--fix) MODE=fix ;;
  1:--changed) MODE=changed ;;
  2:--changed) MODE=changed; BASE="$2" ;;
  *:-h | *:--help) usage; exit 0 ;;
  *) usage; exit 2 ;;
esac

# Linux-only stale-recovery path (stat -c, mv -T, /proc); macOS is CI-only and never contends.
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

# Environment: lint needs the Clang compile DB, which only the default env produces.
ENV_NAME="${PIXI_ENVIRONMENT_NAME:-}"
if [ -z "$ENV_NAME" ]; then
  echo "lint: PIXI_ENVIRONMENT_NAME is unset; run it through pixi: nice pixi run lint" >&2
  exit 2
fi
if [ "$ENV_NAME" != "default" ]; then
  echo "lint: skipped in env '$ENV_NAME' — clang-tidy needs the Clang compile DB; run 'pixi run lint' in the default env (CI covers it)"
  exit 0
fi

# Tools: the env's clang-tidy (same major as the env's clang), never the host's.
TIDY="$(command -v clang-tidy 2>/dev/null)" || {
  echo "lint: clang-tidy not found; the default env needs the clang-tools package" >&2
  exit 2
}
if [ -n "${CONDA_PREFIX:-}" ]; then
  case "$TIDY" in
    "$CONDA_PREFIX"/*) ;;
    *) echo "lint: clang-tidy resolves to $TIDY, outside the pixi env ($CONDA_PREFIX); refusing to use a host clang-tidy" >&2
       exit 2 ;;
  esac
fi
PY="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"

# Explicit parallelism: newer run-clang-tidy/clang-tidy-diff default to every CPU, which defeats
# the per-worktree cap.
JOBS="$(cmake -P "$ROOT/scripts/read-jobs.cmake" 2>/dev/null | tr -d '[:space:]')"
case "$JOBS" in
  '' | *[!0-9]* | 0) JOBS="$(nproc 2>/dev/null || echo 2)"; JOBS_FROM="nproc" ;;
  *) JOBS_FROM="CMakeUserPresets.json vendor.cpp-cli-studio.jobs" ;;
esac

BUILD="$ROOT/build/default/dev"
DB="$BUILD/compile_commands.json"

lock_acquire "$BUILD.lock.d" "scripts/lint.sh${1:+ $1}"
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

if [ ! -f "$DB" ]; then
  echo "lint: no compile DB at build/default/dev/compile_commands.json; run: nice pixi run build" >&2
  exit 1
fi

# Own translation units in the DB, matched as literal path prefixes (no regex escaping needed).
db_own_files() { # db_own_files <root>
  grep -o '"file": *"[^"]*"' "$DB" | sed -e 's/^"file": *"//' -e 's/"$//' |
    R="$1/" awk 'index($0, ENVIRON["R"] "src/") == 1 || index($0, ENVIRON["R"] "tests/") == 1' |
    sort -u
}
# The DB may spell the root physically (pwd -P) or logically (through a symlink); use the one it uses.
LINT_ROOT=""
NFILES=0
for cand in "$ROOT" "$ROOT_L"; do
  n="$(db_own_files "$cand" | wc -l | tr -d '[:space:]')"
  if [ "${n:-0}" -gt 0 ]; then
    LINT_ROOT="$cand"
    NFILES="$n"
    break
  fi
done
if [ -z "$LINT_ROOT" ]; then
  total="$(grep -o '"file":' "$DB" 2>/dev/null | wc -l | tr -d '[:space:]')"
  echo "lint: 0 of $total compile-DB entries are under $ROOT/src/ or $ROOT/tests/." >&2
  echo "lint: refusing to report success for an empty lint (a 0-file filter 'succeeds'). Reconfigure: nice pixi run build" >&2
  exit 1
fi
cd "$LINT_ROOT" || exit 2

re_escape() { printf '%s' "$1" | sed 's/[][\\.^$*+?(){}|]/\\&/g'; }
REGEX="^$(re_escape "$LINT_ROOT")/(src|tests)/"

case "$MODE" in
  full | fix)
    RCT=()
    if command -v run-clang-tidy >/dev/null 2>&1; then
      RCT=(run-clang-tidy)
    elif command -v run-clang-tidy.py >/dev/null 2>&1; then
      RCT=(run-clang-tidy.py)
    elif [ -n "$PY" ] && [ -f "${CONDA_PREFIX:-}/share/clang/run-clang-tidy.py" ]; then
      RCT=("$PY" "$CONDA_PREFIX/share/clang/run-clang-tidy.py")
    else
      echo "lint: run-clang-tidy not found (clang-tools ships it without a .py suffix)" >&2
      exit 2
    fi
    ARGS=(-clang-tidy-binary "$TIDY" -p "$BUILD" -j "$JOBS" -quiet)
    if [ "$MODE" = fix ]; then
      ARGS+=(-fix -format -style file)
      CAR="$(command -v clang-apply-replacements 2>/dev/null || true)"
      [ -n "$CAR" ] && ARGS+=(-clang-apply-replacements-binary "$CAR")
    fi
    echo "lint: clang-tidy on $NFILES own translation units (src/, tests/), -j $JOBS (from $JOBS_FROM)"
    "${RCT[@]}" "${ARGS[@]}" "$REGEX"
    rc=$?
    if [ "$MODE" = fix ]; then
      bash "$ROOT/scripts/format.sh" --fix || rc=1
    fi
    if [ "$rc" -ne 0 ] && [ "$MODE" = fix ]; then
      echo "lint: fix-its were applied where clang-tidy had them; review the diff, then run 'nice pixi run lint' to see what remains" >&2
      exit 1
    fi
    if [ "$rc" -ne 0 ]; then
      echo "lint: clang-tidy reported findings (WarningsAsErrors: '*'). Fix the code; suppress only with" >&2
      echo "lint:   // NOLINTNEXTLINE(<check-name>): <reason>   (cpp-quality skill, NOLINT policy)" >&2
      exit 1
    fi
    echo "lint: clean"
    ;;
  changed)
    TIDY_DIFF=""
    if [ -f "${CONDA_PREFIX:-}/share/clang/clang-tidy-diff.py" ]; then
      TIDY_DIFF="$CONDA_PREFIX/share/clang/clang-tidy-diff.py"
    elif command -v clang-tidy-diff.py >/dev/null 2>&1; then
      TIDY_DIFF="$(command -v clang-tidy-diff.py)"
    fi
    if [ -z "$TIDY_DIFF" ] || [ -z "$PY" ]; then
      echo "lint: lint-changed needs python3 and \$CONDA_PREFIX/share/clang/clang-tidy-diff.py; use: nice pixi run lint" >&2
      exit 2
    fi
    if [ -z "$BASE" ]; then
      for cand in origin/HEAD origin/main origin/master main master; do
        if git rev-parse --verify -q "$cand^{commit}" >/dev/null 2>&1; then
          BASE="$cand"
          break
        fi
      done
    fi
    if [ -z "$BASE" ]; then
      echo "lint: no base ref found; pass one: pixi run lint-changed <base-ref>" >&2
      exit 2
    fi
    MB="$(git merge-base HEAD "$BASE" 2>/dev/null)" || {
      echo "lint: no merge-base between HEAD and '$BASE'" >&2
      exit 2
    }
    echo "lint: changed lines since $(git rev-parse --short "$MB") (merge-base with $BASE), -j $JOBS (from $JOBS_FROM)"
    echo "lint: untracked files are not in the diff; 'pixi run lint' remains the authoritative gate"
    # clang-tidy-diff: -p is the patch strip level; the build dir is -path. Only TUs are run
    # (headers are covered through the TUs that include them and through HeaderFilterRegex).
    git diff -U0 --no-color "$MB" -- src tests |
      "$PY" "$TIDY_DIFF" -clang-tidy-binary "$TIDY" -p1 -path "$BUILD" -j "$JOBS" -quiet \
        -regex '.*\.(cpp|cc|cxx)'
    rc=$?
    if [ "$rc" -ne 0 ]; then
      echo "lint: clang-tidy reported findings on changed lines" >&2
      exit 1
    fi
    ;;
esac
exit 0
```

---

## Verification after writing (part of `/setup` step 8)

Run each step and report its result. A failure goes to `build-fixer`; never work around it silently.

1. **clang-format config:** `pixi run clang-format --dump-config --assume-filename=x.cpp </dev/null`, or rely on `nice pixi run fmt-check`, which validates first. It must exit 0. An unknown key exits 1. Fix it by matching the key to the pinned major (cpp-quality skill, "major drift").
2. **clang-tidy config:** `pixi run clang-tidy --verify-config` and `pixi run clang-tidy --dump-config` must both exit 0. **Every `CheckOptions` key above must appear in the `--dump-config` output.** A normal clang-tidy run silently ignores unknown keys, so a key missing from the dump is a dead key: fix its spelling or remove it. On the 23.x build checked while writing this studio, `--verify-config` also warned `unknown check option` and exited 1. Treat that as a bonus signal, and keep the dump check as the authority.
3. **`tests/.clang-tidy`:** for every disabled name, `pixi run clang-tidy --list-checks -checks='-*,<name>'` must list it. Drop a missing name with a one-line note. Also confirm that `pixi run clang-tidy --dump-config tests/<any>.cpp` shows the inherited root checks plus the four removals.
4. **Lint reality:** `nice pixi run lint` must print a **nonzero** own-file count, and it must not report standard-header errors (for example `'print' file not found`). Those would mean clang-tidy can't drive the conda compiler named in the compile DB. Route that to `build-fixer`.
5. **Scripts:** `bash -n scripts/format.sh scripts/lint.sh`. Also `cmake -P scripts/read-jobs.cmake` must exit 0 and print nothing in a checkout without `CMakeUserPresets.json`.
6. **Warnings:** `nice pixi run check` and `nice pixi run -e gcc check` must both be green. This proves the per-compiler gating: a misplaced GCC-only flag fails the Clang build under `-Werror`, and vice versa.
