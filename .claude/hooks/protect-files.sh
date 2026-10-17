#!/usr/bin/env bash
# PreToolUse (matcher: Edit|Write): refuse edits to files an agent must never
# write by hand. Exit 2 blocks the tool call; stderr goes back to Claude and
# says what to do instead. Anything else exits 0.
#
# Blocked:
#   * secrets: .env / .env.* (except .env.example), *id_rsa*, *id_ed25519*,
#     *.pem, *.key, *.p12, *.pfx, anything under a secrets/ or credentials/
#     directory, and config-style credentials files (credentials,
#     .credentials, credentials.{json,yaml,yml,toml,ini,env,txt},
#     *.credentials) - source files such as credentials.cpp are allowed;
#   * pixi.lock: change dependencies with `pixi add` / `pixi update`;
#   * .claude/settings.local.json: the user's personal permission file;
#   * EXISTING files under tests/e2e/golden/: goldens are human-blessed via
#     `pixi run bless <regex>` (a brand-new golden file may be created).
#
# The globs deliberately do not match this studio's own hook names
# (protect-files.sh, diff-secret-scan.sh, ...), so the hooks stay editable.
set -u

HOOKS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd) || exit 0
# shellcheck source=./lib.sh
. "$HOOKS_DIR/lib.sh" 2>/dev/null || exit 0

HOOK_INPUT=$(cat 2>/dev/null) || HOOK_INPUT=""
hook_json_field tool_input.file_path
path="$HOOK_JSON_VALUE"
if [ -z "$path" ]; then
  hook_json_field tool_input.path
  path="$HOOK_JSON_VALUE"
fi
[ -n "$path" ] || exit 0

hook_json_field cwd
cwd="${HOOK_JSON_VALUE:-${PWD:-.}}"
abs=$(abs_path "$path" "$cwd")
root=$(repo_root_for "$abs")
rel=$(rel_to "$root" "$abs")
base="${abs##*/}"
# Directory globs match the REPO-RELATIVE path, so a checkout that happens to
# live under e.g. ~/credentials-tool/ is not blocked wholesale.
case "$rel" in
  /*) mpath="$abs" ;;
  *) mpath="/$rel" ;;
esac

refuse() {
  printf 'BLOCKED by protect-files: %s\n' "$1" >&2
  printf 'File: %s\n' "$path" >&2
  exit 2
}

SECRET_FIX="This file holds (or may hold) a secret. Agents never write secrets; if the change is genuinely needed, ask the user to make it by hand. Keep real values out of the repo (environment variables, a gitignored file the user manages)."

# --- dependency lockfile ----------------------------------------------------
case "$base" in
  pixi.lock)
    refuse "pixi.lock is generated. Never hand-edit it: change dependencies with 'pixi add <spec>' / 'pixi remove <name>' / 'pixi update [name]' (all ask the user), then 'pixi install --locked' to verify." ;;
esac

# --- personal settings --------------------------------------------------------
case "$abs" in
  */.claude/settings.local.json)
    refuse ".claude/settings.local.json is the user's personal, gitignored permission file. Do not edit it; suggest the change to the user (template: .claude/settings.local.json.example)." ;;
esac

# --- secrets ------------------------------------------------------------------
case "$base" in
  .env.example) : ;;
  .env|.env.*)
    refuse "'$base' is an environment file. $SECRET_FIX Document variable NAMES in .env.example instead." ;;
  *id_rsa*|*id_ed25519*|*.pem|*.key|*.p12|*.pfx)
    refuse "'$base' looks like a private key or certificate bundle. $SECRET_FIX" ;;
esac
case "$mpath" in
  */secrets/*|*/credentials/*)
    refuse "the path lies under a secrets/ or credentials/ directory. $SECRET_FIX" ;;
esac
# Config-style credential files only: C++ sources/headers/tests named
# credentials.* (e.g. src/core/credentials.cpp) stay editable.
case "$base" in
  credentials|.credentials|credentials.json|credentials.yaml|credentials.yml|credentials.toml|credentials.ini|credentials.env|credentials.txt|*.credentials)
    refuse "'$base' is a credentials file. $SECRET_FIX" ;;
esac

# --- human-blessed goldens -------------------------------------------------------
case "$rel" in
  tests/e2e/golden/*)
    if [ -e "$abs" ]; then
      refuse "existing goldens are human-blessed and never hand-edited (they are the CLI's output contract). If the output change is intended, stop and request a re-bless: the orchestrator shows the user the diff and the user approves 'pixi run bless <ctest-regex>'. If it is not intended, fix the code instead."
    fi ;;
esac

exit 0
