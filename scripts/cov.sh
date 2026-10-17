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
