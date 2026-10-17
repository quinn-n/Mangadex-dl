#!/usr/bin/env bash
# PostToolUse (matcher: Edit|Write): the per-file format/lint gate.
# It REPORTS and never rewrites: exit 2 + a stderr summary (capped at ~20
# lines) feeds the findings back to Claude, which fixes them (for formatting:
# `pixi run fmt`). Advisory notes with nothing to fix go out as
# additionalContext with exit 0.
#
# Per edited file (repo root resolved from the file, so worktrees work):
#   * C++ (.cpp .cc .cxx .hpp .h .hh .ipp .tpp):
#       - clang-format --dry-run --Werror --style=file --fallback-style=none
#         using the pixi `default` env's clang-format (never a host copy);
#       - .cpp/.cc/.cxx only, when build/default/dev/compile_commands.json
#         lists the file: clang-tidy -p build/default/dev --quiet under a
#         60 s timeout. A timeout (124) is a context note ("`pixi run lint`
#         is authoritative"), not a block. When clang-tidy cannot analyse the
#         file at all (compile errors / stale compile DB, i.e. only
#         clang-diagnostic-error output) that is also a note: the build
#         reports compile errors, and blocking every edit on a stale DB helps
#         nobody.
#       - env not installed (no .pixi/envs/default) -> a one-time note per
#         session, exit 0.
#   * CMakePresets.json, CMakeUserPresets.json, .claude/settings.json,
#     .claude/settings.local.json.example: must be valid JSON.
#   * .clang-format: `clang-format --dump-config` must accept it (unknown keys
#     exit 1).
#   * .clang-tidy: `clang-tidy --verify-config` (falls back to --dump-config if
#     the installed clang-tidy lacks it); its output is checked as well as its
#     exit code, because a YAML parse error still exits 0.
#   * scripts/**/*.sh and .claude/hooks/*.sh: bash -n.
#   * pixi.toml: parsed with python3 tomllib when available.
# Skips the rest of .claude/, build/, .pixi/ and .platform-runs/.
# Heavy tools run through low_prio (see the low-priority-execution skill).
set -u

HOOKS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd) || exit 0
# shellcheck source=./lib.sh
. "$HOOKS_DIR/lib.sh" 2>/dev/null || exit 0

HOOK_INPUT=$(cat 2>/dev/null) || HOOK_INPUT=""
hook_json_field tool_input.file_path
path="$HOOK_JSON_VALUE"
[ -n "$path" ] || exit 0
hook_json_field cwd
cwd="${HOOK_JSON_VALUE:-${PWD:-.}}"
hook_json_field session_id
sid="${HOOK_JSON_VALUE:-nosession}"

abs=$(abs_path "$path" "$cwd")
[ -f "$abs" ] || exit 0
root=$(repo_root_for "$abs")
rel=$(rel_to "$root" "$abs")
case "$rel" in
  /*) exit 0 ;;   # outside any project checkout
esac
base="${rel##*/}"

FINDINGS=""
NOTES=""
add_finding() { FINDINGS="${FINDINGS}${1}"$'\n'; }
add_note() { NOTES="${NOTES}${1}"$'\n'; }
indent() { sed -e 's/^/    /'; }

# One-time-per-session note when the pixi env is missing.
env_missing_note() {
  local state="" marker=""
  state=$(hook_state_dir "$root")
  if [ -n "$state" ]; then
    marker="$state/format-check.env-note.$(short_hash "$sid|$root")"
    [ -e "$marker" ] && return 0
    : > "$marker" 2>/dev/null
  fi
  add_note "format-check: the pixi env is not installed in $root (no .pixi/envs/default/bin/$1), so clang-format/clang-tidy checks are skipped this session. Run 'pixi install --locked' (or /setup) to enable them; 'pixi run check' remains the authoritative gate."
}

check_json() {
  local out="" rc=0
  if command -v jq >/dev/null 2>&1; then
    out=$(jq empty "$abs" 2>&1); rc=$?
  elif command -v python3 >/dev/null 2>&1; then
    out=$(python3 -m json.tool "$abs" 2>&1 >/dev/null); rc=$?
  else
    return 0
  fi
  if [ "$rc" -ne 0 ]; then
    add_finding "$rel is not valid JSON:"
    add_finding "$(printf '%s\n' "$out" | head -n 4 | indent)"
  fi
}

check_bash_syntax() {
  local out=""
  if ! out=$(bash -n "$abs" 2>&1); then
    add_finding "bash -n failed for $rel:"
    add_finding "$(printf '%s\n' "$out" | head -n 6 | indent)"
  fi
}

check_toml() {
  local out=""
  command -v python3 >/dev/null 2>&1 || return 0
  python3 -c 'import tomllib' >/dev/null 2>&1 || return 0
  if ! out=$(python3 -c '
import sys, tomllib
try:
    with open(sys.argv[1], "rb") as f:
        tomllib.load(f)
except Exception as e:
    print(e)
    sys.exit(1)
' "$abs" 2>&1); then
    add_finding "$rel is not valid TOML:"
    add_finding "$(printf '%s\n' "$out" | tail -n 2 | indent)"
  fi
}

check_clang_format_config() {
  local cf="" dir="" out=""
  cf=$(env_bin "$root" clang-format) || { env_missing_note clang-format; return 0; }
  dir=$(dirname -- "$abs")
  if ! out=$( (cd -- "$dir" && low_prio "$cf" --dump-config --style=file --assume-filename="$dir/x.cpp") </dev/null 2>&1 >/dev/null); then
    add_finding "clang-format rejects $rel (unknown key or bad value for this clang-format major):"
    add_finding "$(printf '%s\n' "$out" | grep -v '^[[:space:]]*\^' | head -n 6 | indent)"
  fi
}

check_clang_tidy_config() {
  local ct="" dir="" out="" rc=0
  ct=$(env_bin "$root" clang-tidy) || { env_missing_note clang-tidy; return 0; }
  dir=$(dirname -- "$abs")
  if "$ct" --help 2>/dev/null | grep -q -- '--verify-config'; then
    out=$( (cd -- "$dir" && low_prio "$ct" --verify-config) </dev/null 2>&1); rc=$?
  else
    out=$( (cd -- "$dir" && low_prio "$ct" --dump-config) </dev/null 2>&1 >/dev/null); rc=$?
  fi
  if [ "$rc" -ne 0 ] || printf '%s\n' "$out" | grep -qE '(error|warning):|Error parsing'; then
    add_finding "clang-tidy rejects $rel (unknown check/option or YAML error):"
    add_finding "$(printf '%s\n' "$out" | grep -E '(error|warning):|Error parsing' | sort -u | head -n 8 | indent)"
  fi
}

check_cpp() {
  local cf="" ct="" out="" rc=0 db="" dbdir="" tagged="" real=""
  local -a tmo=()
  cf=$(env_bin "$root" clang-format) || { env_missing_note clang-format; return 0; }

  out=$( (cd -- "$root" && low_prio "$cf" --dry-run --Werror --style=file --fallback-style=none "$abs") </dev/null 2>&1); rc=$?
  if [ "$rc" -ne 0 ]; then
    add_finding "clang-format: $rel is not formatted. Run 'pixi run fmt' (the formatter owns layout; never hand-format to match):"
    add_finding "$(printf '%s\n' "$out" | grep -E ':[0-9]+:[0-9]+: (error|warning):' | sed -e "s#^$root/##" | head -n 3 | indent)"
  fi

  case "$rel" in
    *.cpp|*.cc|*.cxx) : ;;
    *) return 0 ;;
  esac
  dbdir="$root/build/default/dev"
  db="$dbdir/compile_commands.json"
  [ -f "$db" ] || return 0
  grep -qF -- "$abs" "$db" 2>/dev/null || return 0
  ct=$(env_bin "$root" clang-tidy) || { env_missing_note clang-tidy; return 0; }
  command -v timeout >/dev/null 2>&1 && tmo=(timeout 60)

  out=$( (cd -- "$root" && low_prio ${tmo[@]+"${tmo[@]}"} "$ct" -p "$dbdir" --quiet "$abs") </dev/null 2>&1); rc=$?
  if [ "$rc" -eq 124 ]; then
    add_note "format-check: clang-tidy timed out after 60 s on $rel and was skipped for this edit. 'pixi run lint' is authoritative; run it before review."
    return 0
  fi
  [ "$rc" -eq 0 ] && return 0

  # Check-tagged diagnostics: "...: warning|error: msg [check-name(,...)]".
  tagged=$(printf '%s\n' "$out" | grep -E ':[0-9]+:[0-9]+: (warning|error): .*\[[A-Za-z0-9_.,-]+\]$')
  real=$(printf '%s\n' "$tagged" | grep -vE '\[([^]]*,)?clang-diagnostic-error(,[^]]*)?\]$' | grep -v '^$')
  if [ -n "$real" ]; then
    add_finding "clang-tidy on $rel (fix the code; suppress only with '// NOLINTNEXTLINE(check): reason', see cpp-quality):"
    add_finding "$(printf '%s\n' "$real" | sed -e "s#^$root/##" | head -n 8 | indent)"
  else
    add_note "format-check: clang-tidy could not analyse $rel (compile errors or a stale compile DB; rc=$rc). Rebuild with 'pixi run build' and fix compile errors first; 'pixi run lint' is authoritative."
  fi
}

case "$rel" in
  .claude/hooks/*.sh) check_bash_syntax ;;
  .claude/settings.json|.claude/settings.local.json.example) check_json ;;
  .claude/*|build/*|.pixi/*|.platform-runs/*) exit 0 ;;
  *)
    case "$base" in
      CMakePresets.json|CMakeUserPresets.json) check_json ;;
      .clang-format) check_clang_format_config ;;
      .clang-tidy) check_clang_tidy_config ;;
      pixi.toml) check_toml ;;
      *.sh)
        case "$rel" in scripts/*) check_bash_syntax ;; esac ;;
      *)
        if is_cpp "$rel"; then check_cpp; fi ;;
    esac ;;
esac

if [ -n "$FINDINGS" ]; then
  msg="format-check on $rel:"$'\n'"$FINDINGS$NOTES"
  msg=$(printf '%s' "$msg" | grep -v '^[[:space:]]*$')
  total=$(printf '%s\n' "$msg" | grep -c '')
  printf '%s\n' "$msg" | head -n 20 >&2
  if [ "$total" -gt 20 ]; then
    printf '... (truncated; run '\''pixi run fmt-check'\'' / '\''pixi run lint'\'' for the full list)\n' >&2
  fi
  exit 2
fi
if [ -n "$NOTES" ]; then
  emit_context "$NOTES"
fi
exit 0
