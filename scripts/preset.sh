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

# ---- build-dir lock: this block is identical in scripts/preset.sh, scripts/lint.sh and scripts/binary-packages.sh ----
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
