#!/usr/bin/env bash
# scripts/bless.sh [<ctest -R regex>]: (re)generate E2E golden files from the CURRENT dev build.
#
#   pixi run bless                      # every GOLDEN case (initial goldens, at /setup)
#   pixi run bless '^e2e\.edge\.help$'  # one case
#
# This script is the ONLY place UPDATE_GOLDEN is set (the guard-bash hook blocks setting it anywhere
# else). The `bless` task is in permissions.ask: a human approves every bless, because a golden is the
# CLI contract. Agents request a bless (case regex + reason); they never bless to make a test pass.
# The driver writes a golden only when every other assertion of that case passed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || exit 2

if [[ -n "${CI:-}" ]]; then
    echo "bless: refusing to run in CI (goldens are blessed by a human, locally)" >&2
    exit 2
fi
if [[ $# -gt 1 ]]; then
    echo "usage: bash scripts/bless.sh [<ctest -R regex>]" >&2
    exit 2
fi

args=(-L golden)
if [[ $# -eq 1 && -n "$1" ]]; then
    args+=(-R "$1")
fi

export UPDATE_GOLDEN=1
# preset.sh takes the build-dir lock and picks the per-worktree dev-local test preset if one exists.
# CTest adds the e2e.install fixture automatically under -L/-R.
bash scripts/preset.sh test dev "${args[@]}"
rc=$?
unset UPDATE_GOLDEN

echo
echo "bless: golden changes (review them; commit them together with the change that caused them):"
git diff --stat -- tests/e2e/golden || true
git status --short --untracked-files=all -- tests/e2e/golden || true
if [[ $rc -ne 0 ]]; then
    echo "bless: ctest exited $rc. A failing case's golden is NOT written; fix the failure first." >&2
fi
exit "$rc"
