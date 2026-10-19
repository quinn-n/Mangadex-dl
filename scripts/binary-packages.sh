#!/usr/bin/env bash
# binary-packages.sh [--update] -- which conda packages end up in the shipped binary, derived from
# the release build rather than written by hand.
#   pixi run binary-packages            -> check audit/binary-packages.txt against the build (part of `check`)
#   pixi run binary-packages --update   -> rewrite audit/binary-packages.txt from the build
#
# The inputs of build/default/release/mangadex-dl are:
#   - compiled in: the header deps ninja recorded for every object linked into it
#     (`ninja -t inputs` -> `ninja -t deps`, system headers included);
#   - linked in: the linker-written depfile mangadex-dl.link.d (MANGADEX_DL_LINK_DEPFILE, ON in the
#     release preset): crt objects, static libstdc++/libgcc, libc_nonshared.a, ...
# Each input under the pixi env prefix is mapped to the conda package(s) whose conda-meta/*.json
# "files" list owns it; inputs under the repo (src/, include/, build/) are the project's own.
# scripts/audit.sh splits the SBOM with this list: these packages go to the strict binary audit,
# everything else to the dev (build toolchain) audit, so no finding is reported twice.
#
# Fails when: the list drifted from the build (prints the diff), an input lies outside the env and
# the repo (a host header or library leaked into the build), an env input has no owning package,
# the deps log or depfile is missing or stale, or any input set comes out empty.
# Default env only: other envs print a stated skip and exit 0 (their builds are never shipped).
# Holds the build-dir lock <binaryDir>.lock.d/ (shared with scripts/preset.sh) while it reads.
# Exit: 0 match/updated/skipped, 1 drift or a failed precondition, 2 usage/tooling, 75 lock wait gave up.
set -uo pipefail

PROG=binary-packages
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)" || exit 2
ROOT_L="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -L)" || ROOT_L="$ROOT"
cd "$ROOT" || exit 2

usage() { echo "usage: bash scripts/binary-packages.sh [--update]" >&2; }
MODE=check
case "$#:${1:-}" in
  0:) MODE=check ;;
  1:--update) MODE=update ;;
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

# Environment: the shipped artifact comes from the default env's release build only.
ENV_NAME="${PIXI_ENVIRONMENT_NAME:-}"
if [ -z "$ENV_NAME" ]; then
  echo "$PROG: PIXI_ENVIRONMENT_NAME is unset; run it through pixi: nice pixi run binary-packages" >&2
  exit 2
fi
if [ "$ENV_NAME" != "default" ]; then
  echo "$PROG: skipped in env '$ENV_NAME' — only the default env's release build is shipped; run 'pixi run binary-packages' in the default env (CI covers it)"
  exit 0
fi
PREFIX="${CONDA_PREFIX:-}"
if [ -z "$PREFIX" ] || [ ! -d "$PREFIX/conda-meta" ]; then
  echo "$PROG: CONDA_PREFIX ('$PREFIX') has no conda-meta/; run it through pixi: nice pixi run binary-packages" >&2
  exit 2
fi
PREFIX_P="$(cd "$PREFIX" && pwd -P)" || exit 2

# Tools: the env's ninja and jq, never the host's.
for tool in ninja jq; do
  path="$(command -v "$tool" 2>/dev/null)" || {
    echo "$PROG: $tool not found in the pixi env" >&2
    exit 2
  }
  case "$path" in
    "$PREFIX"/* | "$PREFIX_P"/*) ;;
    *) echo "$PROG: $tool resolves to $path, outside the pixi env ($PREFIX); refusing to use a host $tool" >&2
       exit 2 ;;
  esac
done

LIST="$ROOT/audit/binary-packages.txt"
BUILD="$ROOT/build/default/release"
BIN_TARGET="mangadex-dl"
DEPFILE="$BUILD/$BIN_TARGET.link.d"

lock_acquire "$BUILD.lock.d" "scripts/binary-packages.sh${1:+ $1}"
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

if [ ! -f "$BUILD/build.ninja" ] || [ ! -f "$BUILD/$BIN_TARGET" ]; then
  echo "$PROG: no release build at build/default/release; run: nice pixi run release" >&2
  exit 1
fi
if [ ! -f "$DEPFILE" ]; then
  echo "$PROG: no linker depfile build/default/release/$BIN_TARGET.link.d; the release build must be configured with MANGADEX_DL_LINK_DEPFILE=ON (the release preset sets it): nice pixi run release" >&2
  exit 1
fi
# The linker writes the depfile during the same link as the binary, so their mtimes match to within
# timestamp granularity; a depfile clearly older than the binary is left over from an earlier link.
dep_mtime=$(stat -c %Y "$DEPFILE") && bin_mtime=$(stat -c %Y "$BUILD/$BIN_TARGET") || exit 2
if [ "$dep_mtime" -lt $((bin_mtime - 2)) ]; then
  echo "$PROG: build/default/release/$BIN_TARGET.link.d is older than the binary (linked without MANGADEX_DL_LINK_DEPFILE?); run: nice pixi run release" >&2
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/binary-packages.XXXXXX")" || exit 2
# Chain onto the lock's EXIT trap so both the lock and the scratch dir are released.
trap 'rm -rf "$WORK"; lock_release' EXIT

# ---- compiled-in inputs: header deps of every object linked into the binary ----------------
(cd "$BUILD" && ninja -t inputs "$BIN_TARGET") >"$WORK/ninja-inputs" || {
  echo "$PROG: 'ninja -t inputs $BIN_TARGET' failed in build/default/release" >&2
  exit 2
}
grep '\.o$' "$WORK/ninja-inputs" >"$WORK/objects" || true
n_obj=$(wc -l <"$WORK/objects")
if [ "$n_obj" -eq 0 ]; then
  echo "$PROG: no object files among the inputs of $BIN_TARGET (unexpected build graph)" >&2
  exit 1
fi
(cd "$BUILD" && xargs -d '\n' ninja -t deps <"$WORK/objects") >"$WORK/deps" || {
  echo "$PROG: 'ninja -t deps' failed in build/default/release" >&2
  exit 2
}
# Header lines look like "<obj>: #deps N, deps mtime T (VALID)"; anything but VALID means the
# recorded deps may not match the object that was linked.
bad_deps=$(grep -E '^[^[:space:]].*(deps not found|\((STALE|INVALID)\))' "$WORK/deps" || true)
if [ -n "$bad_deps" ]; then
  echo "$PROG: ninja has no current header deps for some objects; run: nice pixi run release" >&2
  printf '  %s\n' "$bad_deps" >&2
  exit 1
fi
grep -E '^[[:space:]]+[^[:space:]]' "$WORK/deps" | sed 's/^[[:space:]]*//' >"$WORK/compiled"

# ---- linked-in inputs: the first rule of the linker's make-style depfile ---------------------
# GNU ld writes "<output>: \" and then one input per line ("  <path> \"), followed by phony rules.
sed -n '1,/^$/p' "$DEPFILE" | sed -e '1s/^[^:]*:[[:space:]]*//' -e 's/[[:space:]]*\\$//' \
  -e 's/^[[:space:]]*//' -e '/^$/d' >"$WORK/linked"
if grep -q '[[:space:]]' "$WORK/linked"; then
  echo "$PROG: unexpected format in build/default/release/$BIN_TARGET.link.d (an entry contains whitespace)" >&2
  exit 2
fi
n_link=$(wc -l <"$WORK/linked")
if [ "$n_link" -eq 0 ]; then
  echo "$PROG: build/default/release/$BIN_TARGET.link.d lists no inputs" >&2
  exit 1
fi

# ---- classify: own (repo) / env (map to a package) / anything else (host leak) ---------------
# Lexical normalization only (-s): conda-meta lists symlinks under their own names. Relative depfile
# entries are relative to the build dir.
(cd "$BUILD" && cat "$WORK/compiled" "$WORK/linked" | xargs -d '\n' realpath -s -m --) |
  LC_ALL=C sort -u >"$WORK/all" || {
  echo "$PROG: could not normalize the input paths" >&2
  exit 2
}
: >"$WORK/env-rel"
: >"$WORK/host"
n_own=0
while IFS= read -r f; do
  case "$f" in
    "$PREFIX"/* | "$PREFIX_P"/*) # before ROOT: the pixi env lives inside the repo
      rel="${f#"$PREFIX"/}"
      [ "$rel" != "$f" ] || rel="${f#"$PREFIX_P"/}"
      printf '%s\n' "$rel" >>"$WORK/env-rel" ;;
    "$ROOT"/.pixi/* | "$ROOT_L"/.pixi/*) # another pixi env (e.g. gcc) is not this build's toolchain
      printf '%s\n' "$f" >>"$WORK/host" ;;
    "$ROOT"/* | "$ROOT_L"/*) n_own=$((n_own + 1)) ;;
    *) printf '%s\n' "$f" >>"$WORK/host" ;;
  esac
done <"$WORK/all"
if [ -s "$WORK/host" ]; then
  echo "$PROG: the release binary was built from files outside this pixi env and the repo sources (host or other-env leak):" >&2
  sed 's/^/  /' "$WORK/host" >&2
  exit 1
fi
LC_ALL=C sort -u -o "$WORK/env-rel" "$WORK/env-rel"
n_env=$(wc -l <"$WORK/env-rel")
if [ "$n_env" -eq 0 ]; then
  echo "$PROG: no input of $BIN_TARGET comes from the pixi env (unexpected: the toolchain's crt objects and libstdc++ should)" >&2
  exit 1
fi

jq -r '.name as $n | .files[]? | [., $n] | @tsv' "$PREFIX"/conda-meta/*.json | LC_ALL=C sort >"$WORK/owners" || {
  echo "$PROG: could not read $PREFIX/conda-meta/*.json" >&2
  exit 2
}
LC_ALL=C join -t "$(printf '\t')" -v 1 "$WORK/env-rel" "$WORK/owners" >"$WORK/unowned"
if [ -s "$WORK/unowned" ]; then
  echo "$PROG: env files used by the release build that no conda package owns:" >&2
  sed 's/^/  /' "$WORK/unowned" >&2
  exit 1
fi
LC_ALL=C join -t "$(printf '\t')" "$WORK/env-rel" "$WORK/owners" | cut -f 2 | LC_ALL=C sort -u >"$WORK/derived"

echo "$PROG: $(wc -l <"$WORK/all") inputs ($n_obj objects' header deps + $n_link linked files): $n_own project-owned, $n_env from $(wc -l <"$WORK/derived") conda packages"

if [ "$MODE" = update ]; then
  mkdir -p "$(dirname "$LIST")" || exit 2
  tmp="$LIST.tmp.$$"
  if ! {
    echo "# Conda packages whose files are compiled or linked into the shipped binary (default env,"
    echo "# release build). Generated by: pixi run binary-packages --update -- do not edit by hand;"
    echo "# 'pixi run check' fails when this drifts from the build. scripts/audit.sh scans exactly these"
    echo "# packages in the binary audit and everything else in the dev (build toolchain) audit."
    cat "$WORK/derived"
  } >"$tmp" || ! mv -f "$tmp" "$LIST"; then
    rm -f "$tmp"
    echo "$PROG: could not write $LIST" >&2
    exit 2
  fi
  echo "$PROG: wrote audit/binary-packages.txt ($(wc -l <"$WORK/derived") packages)"
  exit 0
fi

if [ ! -f "$LIST" ]; then
  echo "$PROG: audit/binary-packages.txt is missing; create it with: nice pixi run binary-packages --update" >&2
  exit 1
fi
grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$LIST" | LC_ALL=C sort -u >"$WORK/listed"
if cmp -s "$WORK/listed" "$WORK/derived"; then
  echo "$PROG: audit/binary-packages.txt matches the release build"
  exit 0
fi
echo "$PROG: audit/binary-packages.txt has drifted from the release build:" >&2
LC_ALL=C comm -13 "$WORK/listed" "$WORK/derived" | sed 's/^/  + now in the binary: /' >&2
LC_ALL=C comm -23 "$WORK/listed" "$WORK/derived" | sed 's/^/  - no longer in the binary: /' >&2
echo "$PROG: if this is intended, run 'nice pixi run binary-packages --update' and commit audit/binary-packages.txt (the binary audit scans exactly that list)" >&2
exit 1
