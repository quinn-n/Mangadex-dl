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
