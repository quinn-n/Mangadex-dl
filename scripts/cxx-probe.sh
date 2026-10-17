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
