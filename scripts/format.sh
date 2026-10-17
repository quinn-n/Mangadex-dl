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
