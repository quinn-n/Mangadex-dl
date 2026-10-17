#!/usr/bin/env bash
# lib.sh: shared helpers for the hooks in this directory.
# Sourced by the hook scripts; it is never wired to a hook event itself.
#
# House contract for every hook here:
#   * Wired as `bash "$CLAUDE_PROJECT_DIR/.claude/hooks/<x>.sh"`, so a missing
#     exec bit can never silently disable a hook.
#   * bash, `set -u`, NEVER `set -e`: a hook must never break the session.
#   * Unknown or degraded state (no parser, no git, env not installed, bad
#     input) means exit 0 with a note. It never means a hard failure.
#   * Feedback Claude must act on = exit 2 + a short stderr message that names
#     what to do instead:
#       - PreToolUse  (protect-files, guard-bash): exit 2 blocks the tool call.
#       - PostToolUse (format-check): exit 2 feeds stderr back to Claude; the
#         edit has already happened, so the message says what to fix.
#       - Stop / SubagentStop (diff-secret-scan): exit 2 prevents stopping.
#   * Advisory PostToolUse output (cpp-rule-linter, format-check's notes) goes
#     through emit_context(): `hookSpecificOutput.additionalContext` JSON on
#     stdout with exit 0. Stdout JSON is ignored when a hook exits 2, so a hook
#     that blocks puts every note into its stderr instead.
#   * Repo root is resolved from the EDITED FILE (repo_root_for), never from
#     $CLAUDE_PROJECT_DIR alone, so edits inside a `/features` worktree are
#     checked against that worktree's config, env and build tree.

set -u

# ---------------------------------------------------------------------------
# Hook payload
# ---------------------------------------------------------------------------
# stdin can only be read once, so every hook does `HOOK_INPUT=$(cat)` once at
# the top and hook_json_field parses that variable rather than stdin.
: "${HOOK_INPUT:=}"
# Extractor that produced the last value: jq | python3 | grep | none.
: "${HOOK_JSON_PARSER:=none}"
# The last extracted value. hook_json_field sets globals instead of printing,
# because a command substitution would run it in a subshell and lose the
# parser provenance that guard-bash.sh depends on.
: "${HOOK_JSON_VALUE:=}"

# jq: a missing path or JSON null yields nothing; a string is printed raw; any
# other type (bool, number, object) is printed as compact JSON, so `false`
# reads as the literal text "false", never as an empty/truthy value.
_HOOK_JQ_EXTRACT='try getpath($f | split(".")) catch null
  | if . == null then empty elif type == "string" then . else tojson end'

_HOOK_PY_EXTRACT='
import json, sys
try:
    data = json.loads(sys.stdin.read())
except Exception:
    sys.exit(1)
cur = data
for part in sys.argv[1].split("."):
    if isinstance(cur, dict) and part in cur:
        cur = cur[part]
    else:
        sys.exit(0)
if cur is None:
    sys.exit(0)
sys.stdout.write(cur if isinstance(cur, str) else json.dumps(cur, separators=(",", ":")))
'

# hook_json_field <dotted.field.path>
# Sets HOOK_JSON_VALUE (empty if absent or null) and HOOK_JSON_PARSER.
# Order: jq -> python3 -> grep. The grep fallback mangles values containing
# JSON escapes or newlines (notably tool_input.command), so a caller that needs
# an exact value checks HOOK_JSON_PARSER and degrades safely (fails open).
hook_json_field() {
  local field="${1:-}" out=""
  HOOK_JSON_VALUE=""
  HOOK_JSON_PARSER="none"
  [ -n "$field" ] || return 0
  [ -n "$HOOK_INPUT" ] || return 0

  if command -v jq >/dev/null 2>&1; then
    if out=$(printf '%s' "$HOOK_INPUT" | jq -r --arg f "$field" "$_HOOK_JQ_EXTRACT" 2>/dev/null); then
      HOOK_JSON_PARSER="jq"
      HOOK_JSON_VALUE="$out"
      return 0
    fi
  fi

  if command -v python3 >/dev/null 2>&1; then
    if out=$(printf '%s' "$HOOK_INPUT" | python3 -c "$_HOOK_PY_EXTRACT" "$field" 2>/dev/null); then
      HOOK_JSON_PARSER="python3"
      HOOK_JSON_VALUE="$out"
      return 0
    fi
  fi

  # Last resort: first `"key": "string"` pair anywhere in the payload. Only
  # string values are recovered; escapes are left as-is.
  local key="${field##*.}"
  out=$(printf '%s' "$HOOK_INPUT" \
        | grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*\"([^\"\\\\]|\\\\.)*\"" 2>/dev/null \
        | head -n 1 \
        | sed -e "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"//" -e 's/"$//')
  HOOK_JSON_PARSER="grep"
  HOOK_JSON_VALUE="$out"
  return 0
}

# ---------------------------------------------------------------------------
# Low-priority execution (see the low-priority-execution skill)
# ---------------------------------------------------------------------------
# low_prio <cmd> [args...]: run a compute-heavy command at low OS priority
# locally, at full speed in CI. Preserves the command's exit code (nice,
# ionice and taskpolicy all exec/wait and return the child's status), and
# degrades to a plain run when no tool is available. Hooks run outside the
# permission system, so the canonical `nice -n 10` / `ionice -c3` /
# `taskpolicy -b` forms are fine here (agent-typed commands use bare `nice`).
# <cmd> must be an executable, not a shell function: compose as
# `low_prio timeout 60 <tool> ...`, never `timeout 60 low_prio ...` (timeout
# cannot exec a function; expiry still returns 124 in the correct order).
# The helper body below is identical to the canonical one in the
# low-priority-execution skill; keep the two in sync.
# Run a compute-heavy command at low OS priority locally; full speed in CI.
# Preserves the command's exit code. Degrades to a plain run if no tool is available.
low_prio() {
  if [ -n "${CI:-}${GITHUB_ACTIONS:-}${GITLAB_CI:-}" ]; then "$@"; return; fi
  if [ "$(uname -s)" = "Darwin" ] && command -v taskpolicy >/dev/null 2>&1; then
    taskpolicy -b "$@"; return
  fi
  if command -v nice >/dev/null 2>&1; then
    if command -v ionice >/dev/null 2>&1; then ionice -c3 nice -n 10 "$@"; else nice -n 10 "$@"; fi
    return
  fi
  "$@"
}

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
# nearest_existing_dir <path>: the path itself if it is a directory, else the
# closest existing ancestor directory. A Write that creates a file in a new
# directory has no existing dirname, so git must be asked from an ancestor.
nearest_existing_dir() {
  local d="${1:-}"
  [ -n "$d" ] || { printf '%s' "${PWD:-/}"; return 0; }
  case "$d" in
    /*) : ;;
    *) d="${PWD:-.}/$d" ;;
  esac
  [ -d "$d" ] || d=$(dirname -- "$d")
  while [ -n "$d" ] && [ "$d" != "/" ] && [ ! -d "$d" ]; do
    d=$(dirname -- "$d")
  done
  [ -n "$d" ] || d="/"
  printf '%s' "$d"
}

# repo_root_for <path>: the git toplevel of the repo or worktree that contains
# <path> (resolved from its nearest existing ancestor), else
# $CLAUDE_PROJECT_DIR, else $PWD. No trailing slash.
repo_root_for() {
  local p="${1:-}" d="" root=""
  if [ -n "$p" ] && command -v git >/dev/null 2>&1; then
    d=$(nearest_existing_dir "$p")
    root=$(git -C "$d" rev-parse --show-toplevel 2>/dev/null) || root=""
  fi
  [ -n "$root" ] || root="${CLAUDE_PROJECT_DIR:-}"
  [ -n "$root" ] || root="${PWD:-.}"
  [ "$root" = "/" ] || root="${root%/}"
  printf '%s' "$root"
}

# abs_path <path> [base]: make <path> absolute against [base] (default $PWD).
# Lexical only (no symlink resolution); the file may not exist yet.
abs_path() {
  local p="${1:-}" base="${2:-${PWD:-.}}"
  case "$p" in
    /*) printf '%s' "$p" ;;
    *) printf '%s/%s' "${base%/}" "${p#./}" ;;
  esac
}

# rel_to <root> <abs-path>: <abs-path> relative to <root>; unchanged (still
# absolute) when it lies outside <root>.
rel_to() {
  local root="${1%/}" p="${2:-}"
  case "$p" in
    "$root"/*) printf '%s' "${p#"$root"/}" ;;
    "$root") printf '.' ;;
    *) printf '%s' "$p" ;;
  esac
}

# git_common_dir <dir>: absolute path of the repo's common git dir (shared by
# the main checkout and every worktree), or nothing outside a repo.
git_common_dir() {
  local d="${1:-.}" g=""
  command -v git >/dev/null 2>&1 || return 0
  g=$(git -C "$d" rev-parse --git-common-dir 2>/dev/null) || return 0
  [ -n "$g" ] || return 0
  case "$g" in
    /*) : ;;
    *) g="$d/$g" ;;
  esac
  (cd -- "$g" 2>/dev/null && pwd) || true
}

# hook_state_dir <dir>: a writable directory for hook state (loop guards,
# one-time notes). Lives in the shared git common dir, so it is never part of
# a diff and is shared across worktrees; falls back to a per-user temp dir.
hook_state_dir() {
  local base="" s=""
  base=$(git_common_dir "${1:-.}")
  if [ -n "$base" ]; then
    s="$base/claude-hooks"
  else
    s="${TMPDIR:-/tmp}/claude-hooks-$(id -u 2>/dev/null || echo 0)"
  fi
  mkdir -p -- "$s" 2>/dev/null || return 0
  printf '%s' "$s"
}

# short_hash <string>: a short stable hash (sha256sum, shasum or cksum).
short_hash() {
  local s="${1:-}" h=""
  if command -v sha256sum >/dev/null 2>&1; then
    h=$(printf '%s' "$s" | sha256sum 2>/dev/null)
  elif command -v shasum >/dev/null 2>&1; then
    h=$(printf '%s' "$s" | shasum -a 256 2>/dev/null)
  else
    h=$(printf '%s' "$s" | cksum 2>/dev/null)
  fi
  h="${h%% *}"
  printf '%s' "${h:0:16}"
}

# env_bin <root> <tool>: the pixi `default` env's copy of <tool> for the repo at
# <root>, if installed and executable. Hooks use the env's pinned
# clang-format / clang-tidy (never a host copy of a different major).
env_bin() {
  local root="${1:-}" tool="${2:-}" b=""
  [ -n "$root" ] && [ -n "$tool" ] || return 1
  b="$root/.pixi/envs/default/bin/$tool"
  [ -x "$b" ] || return 1
  printf '%s' "$b"
}

# is_cpp <path>: true for C++ sources and headers.
is_cpp() {
  case "${1:-}" in
    *.cpp|*.cc|*.cxx|*.hpp|*.h|*.hh|*.ipp|*.tpp) return 0 ;;
  esac
  return 1
}

# is_cpp_header <path>
is_cpp_header() {
  case "${1:-}" in
    *.hpp|*.h|*.hh|*.ipp|*.tpp) return 0 ;;
  esac
  return 1
}

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
# json_escape <string>: emit a JSON string literal (quotes included).
json_escape() {
  local s="${1:-}" esc=""
  if command -v jq >/dev/null 2>&1; then
    esc=$(printf '%s' "$s" | jq -Rs . 2>/dev/null) || esc=""
    if [ -n "$esc" ]; then printf '%s' "$esc"; return 0; fi
  fi
  if command -v python3 >/dev/null 2>&1; then
    esc=$(printf '%s' "$s" | python3 -c 'import json,sys; sys.stdout.write(json.dumps(sys.stdin.read()))' 2>/dev/null) || esc=""
    if [ -n "$esc" ]; then printf '%s' "$esc"; return 0; fi
  fi
  # Hand-escape. Other control characters are flattened to spaces.
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  s=${s//$'\t'/ }
  s=${s//$'\r'/ }
  s=${s//$'\n'/\\n}
  s=$(printf '%s' "$s" | tr '\000-\010\013\014\016-\037' ' ')
  printf '"%s"' "$s"
}

# emit_context <message>: the advisory channel of a PostToolUse hook
# (`hookSpecificOutput.additionalContext`). Only meaningful with exit 0.
emit_context() {
  local msg="${1:-}"
  [ -n "$msg" ] || return 0
  printf '{"hookSpecificOutput":{"hookEventName":"PostToolUse","additionalContext":%s}}\n' "$(json_escape "$msg")"
  return 0
}

# emit_system_message <message>: a JSON systemMessage, shown to the user only
# (used by the Stop/SubagentStop hook with exit 0).
emit_system_message() {
  local msg="${1:-}"
  [ -n "$msg" ] || return 0
  printf '{"systemMessage":%s}\n' "$(json_escape "$msg")"
  return 0
}

# hook_note <message>: one line on stderr (debug log / transcript only when
# the hook exits 0).
hook_note() {
  printf '%s\n' "${1:-}" >&2
  return 0
}
