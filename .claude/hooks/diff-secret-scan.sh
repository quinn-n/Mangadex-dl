#!/usr/bin/env bash
# Stop + SubagentStop: scan the uncommitted changes for likely secrets before
# work is handed back. Exit 2 prevents stopping and returns stderr to Claude.
#
# Scope (orchestrator amendment #4):
#   * SubagentStop: ONLY the repo/worktree containing the hook input's `cwd`
#     (`git -C "$cwd" rev-parse --show-toplevel`). A subagent is never blocked
#     by another worktree's finding.
#   * Stop (main session): the main checkout plus every worktree in
#     `git worktree list --porcelain`, findings accumulated across all of them.
# What is scanned per repo: the ADDED lines of `git diff <HEAD|empty tree>`
# (staged + unstaged) plus every untracked, non-ignored text file (<= 1 MiB).
# Skipped: .claude/ (the studio documents the patterns it looks for),
# CLAUDE.md, pixi.lock, .gitleaks.toml, .gitleaksignore, build/, .pixi/,
# .platform-runs/.
#
# Detection: gitleaks when installed (its CLI is VERIFIED AT USE-TIME: the
# `dir` subcommand, then the legacy `detect --no-git` form; an unexpected exit
# code or report means "gitleaks unusable" and a note), PLUS the built-in
# patterns, which always run: private-key headers, AWS access key ids, GitHub
# tokens, Slack tokens, Google API keys, sk- style keys, bearer headers, and
# credential-named assignments with a quoted (or whole-line config-style)
# value. The generic credential rule requires a quoted/config-style value so
# that ordinary C++ like `token = next_token(...)` does not trip it.
#
# Output names path:line and the RULE only. A matched value is never printed
# (gitleaks runs with --redact and only RuleID/StartLine are read from its
# JSON report).
#
# Loop guard: the finding set (repo, path, rule; no line numbers, so shifting
# lines does not re-block) is hashed into
#   $(git rev-parse --git-common-dir)/claude-hooks/secret-scan.<scope>.last
# with one state file per scope (`all` for Stop, one per worktree for
# SubagentStop) so parallel worktrees never overwrite each other's state.
# First occurrence -> exit 2. An identical repeat -> exit 0 plus a JSON
# systemMessage to the user. A clean scan deletes the scope's state file.
# `stop_hook_active` is honoured only if the input carries it (it is not in the
# current hook docs); the dedupe is the primary guard.
set -u

HOOKS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd) || exit 0
# shellcheck source=./lib.sh
. "$HOOKS_DIR/lib.sh" 2>/dev/null || exit 0

command -v git >/dev/null 2>&1 || exit 0

HOOK_INPUT=$(cat 2>/dev/null) || HOOK_INPUT=""
hook_json_field hook_event_name
EVENT="$HOOK_JSON_VALUE"
hook_json_field cwd
CWD="$HOOK_JSON_VALUE"
hook_json_field stop_hook_active
STOP_ACTIVE="$HOOK_JSON_VALUE"

start_top=""
for d in "$CWD" "${CLAUDE_PROJECT_DIR:-}" "${PWD:-}"; do
  [ -n "$d" ] && [ -d "$d" ] || continue
  start_top=$(git -C "$d" rev-parse --show-toplevel 2>/dev/null) && [ -n "$start_top" ] && break
  start_top=""
done
[ -n "$start_top" ] || exit 0

# --- which repos ------------------------------------------------------------------
TOPS=()
if [ "$EVENT" = "Stop" ]; then
  SCOPE="all"
  while IFS= read -r line; do
    case "$line" in
      "worktree "*)
        wt="${line#worktree }"
        [ -d "$wt" ] || continue
        [ "$(git -C "$wt" rev-parse --is-inside-work-tree 2>/dev/null)" = "true" ] || continue
        TOPS+=("$wt") ;;
    esac
  done < <(git -C "$start_top" worktree list --porcelain 2>/dev/null)
  [ ${#TOPS[@]} -gt 0 ] || TOPS=("$start_top")
else
  # SubagentStop, or an unknown/missing event name: the narrow scope.
  SCOPE="wt-$(short_hash "$start_top")"
  TOPS=("$start_top")
fi

tmp=$(mktemp -d 2>/dev/null) || exit 0
trap 'rm -rf "$tmp"' EXIT
TSV="$tmp/added.tsv"     # top \t path \t line \t content
: > "$TSV"

is_skipped() {
  case "$1" in
    .claude/*|CLAUDE.md|pixi.lock|.gitleaks.toml|.gitleaksignore|build/*|.pixi/*|.platform-runs/*) return 0 ;;
    *"$(printf '\t')"*) return 0 ;;
  esac
  return 1
}

collect_repo() {
  local top="$1" base="" p="" size=0
  if git -C "$top" rev-parse --verify -q HEAD >/dev/null 2>&1; then
    base="HEAD"
  else
    base=$(git -C "$top" hash-object -t tree /dev/null 2>/dev/null) || return 0
  fi
  # Added lines of tracked changes, with their new-file line numbers.
  git -C "$top" -c core.quotePath=false diff --no-color --no-ext-diff --no-renames \
      -U0 --diff-filter=ACMRT "$base" 2>/dev/null \
    | TOP="$top" awk '
      function skipped(p) {
        return (p ~ /^\.claude\// || p == "CLAUDE.md" || p == "pixi.lock" || p == ".gitleaks.toml" ||
                p == ".gitleaksignore" || p ~ /^build\// || p ~ /^\.pixi\// || p ~ /^\.platform-runs\//)
      }
      /^diff --git / { hdr = 1; path = ""; next }
      hdr && /^\+\+\+ / {
        p = substr($0, 5); sub(/\t$/, "", p)
        if (p == "/dev/null") path = ""; else { sub(/^b\//, "", p); path = p }
        if (skipped(path) || index(path, "\t")) path = ""
        next
      }
      hdr && /^@@ / {
        hdr = 0
        if (match($0, /\+[0-9]+/)) ln = substr($0, RSTART + 1, RLENGTH - 1) + 0
        next
      }
      /^@@ / { if (match($0, /\+[0-9]+/)) ln = substr($0, RSTART + 1, RLENGTH - 1) + 0; next }
      hdr { next }
      /^\+/ { if (path != "") print ENVIRON["TOP"] "\t" path "\t" ln "\t" substr($0, 2); ln++; next }
    ' >> "$TSV"
  # Untracked, non-ignored text files: every line is "added".
  while IFS= read -r -d '' p; do
    [ -n "$p" ] || continue
    is_skipped "$p" && continue
    [ -f "$top/$p" ] || continue
    size=$(wc -c < "$top/$p" 2>/dev/null) || size=0
    case "$size" in ''|*[!0-9]*) size=0 ;; esac
    [ "$size" -le 1048576 ] || continue
    grep -Iq . "$top/$p" 2>/dev/null || continue   # binary or empty
    TOP="$top" P="$p" awk '{ print ENVIRON["TOP"] "\t" ENVIRON["P"] "\t" NR "\t" $0 }' "$top/$p" >> "$TSV" 2>/dev/null
  done < <(git -C "$top" ls-files --others --exclude-standard -z 2>/dev/null)
}

for top in "${TOPS[@]}"; do
  collect_repo "$top"
done

[ -s "$TSV" ] || { rm -f -- "$(hook_state_dir "$start_top")/secret-scan.$SCOPE.last" 2>/dev/null; exit 0; }

ADDED="$tmp/added.txt"
IDX="$tmp/added.idx"
cut -f4- "$TSV" > "$ADDED"
cut -f1-3 "$TSV" > "$IDX"
HITS="$tmp/hits"        # added.txt line number \t rule
: > "$HITS"

# --- built-in patterns (always run) -------------------------------------------------
pat() {  # pat <rule> <grep flags> <ERE>
  grep -n $2 -- "$3" "$ADDED" 2>/dev/null | cut -d: -f1 | awk -v r="$1" '{ print $0 "\t" r }' >> "$HITS"
}
pat private-key-block       -E  '-----BEGIN ([A-Z0-9]+ )*PRIVATE KEY( BLOCK)?-----'
pat aws-access-key-id       -E  '(^|[^A-Z0-9])(AKIA|ASIA)[0-9A-Z]{16}([^A-Z0-9]|$)'
pat github-token            -E  'gh[pousr]_[A-Za-z0-9]{36}'
pat github-fine-grained-pat -E  'github_pat_[A-Za-z0-9_]{22,}'
pat slack-token             -E  'xox[baprs]-[A-Za-z0-9-]{8,}'
pat google-api-key          -E  'AIza[0-9A-Za-z_-]{35}'
pat sk-style-secret-key     -E  '(^|[^A-Za-z0-9_-])sk-(live_|test_|proj-|ant-)?[A-Za-z0-9_-]{20,}'
pat bearer-token            -Ei 'authorization[" '\'']*[:=][" '\'']*bearer[[:space:]]+[A-Za-z0-9._~+/=-]{16,}'
pat credential-assignment   -Ei '(api[_-]?key|secret|passw(or)?d|token|bearer|auth[_-]?key)[A-Za-z0-9_-]*["'\'']?[[:space:]]*[:=]+[[:space:]]*["'\''][A-Za-z0-9_+/=.~-]{16,}["'\'']'
pat credential-config-line  -Ei '^[[:space:]]*(export[[:space:]]+)?[A-Za-z0-9_.-]*(api[_-]?key|secret|passw(or)?d|token)[A-Za-z0-9_.-]*[[:space:]]*[=:][[:space:]]*[A-Za-z0-9_+/.~-]{16,}[[:space:]]*$'

# --- gitleaks (optional; CLI verified at use-time) ------------------------------------
GL_NOTE=""
if command -v gitleaks >/dev/null 2>&1; then
  GLJ="$tmp/gitleaks.json"
  gl_ok=0
  rm -f "$GLJ"
  low_prio gitleaks dir --no-banner --redact --exit-code 3 \
    --report-format json --report-path "$GLJ" "$ADDED" >/dev/null 2>&1
  rc=$?
  if { [ "$rc" -eq 0 ] || [ "$rc" -eq 3 ]; } && [ -s "$GLJ" ]; then
    gl_ok=1
  else
    rm -f "$GLJ"
    low_prio gitleaks detect --no-git --source "$ADDED" --no-banner --redact --exit-code 3 \
      --report-format json --report-path "$GLJ" >/dev/null 2>&1
    rc=$?
    if { [ "$rc" -eq 0 ] || [ "$rc" -eq 3 ]; } && [ -s "$GLJ" ]; then gl_ok=1; fi
  fi
  if [ "$gl_ok" = 1 ]; then
    gl_rows=""
    if command -v jq >/dev/null 2>&1; then
      gl_rows=$(jq -r '.[] | "\(.StartLine)\tgitleaks:\(.RuleID)"' "$GLJ" 2>/dev/null) || gl_ok=0
    elif command -v python3 >/dev/null 2>&1; then
      gl_rows=$(python3 -c '
import json, sys
for f in json.load(open(sys.argv[1])):
    print("%s\tgitleaks:%s" % (f.get("StartLine"), f.get("RuleID")))
' "$GLJ" 2>/dev/null) || gl_ok=0
    else
      gl_ok=0
    fi
    if [ "$gl_ok" = 1 ]; then
      [ -n "$gl_rows" ] && printf '%s\n' "$gl_rows" >> "$HITS"
    else
      GL_NOTE="gitleaks ran but its JSON report could not be read; built-in patterns only."
    fi
  else
    GL_NOTE="gitleaks is installed but unusable here (exit $rc; CLI syntax changed?); built-in patterns only. Verify its CLI ('gitleaks --help')."
  fi
fi

[ -s "$HITS" ] || { rm -f -- "$(hook_state_dir "$start_top")/secret-scan.$SCOPE.last" 2>/dev/null; exit 0; }

# --- map hits back to repo/path/line (never the content) ----------------------------
FIND="$tmp/findings"    # top \t path \t line \t rule
awk -F'\t' 'NR == FNR { idx[FNR] = $1 "\t" $2 "\t" $3; next }
            ($1 in idx) { print idx[$1] "\t" $2 }' "$IDX" "$HITS" | sort -u > "$FIND"
[ -s "$FIND" ] || { rm -f -- "$(hook_state_dir "$start_top")/secret-scan.$SCOPE.last" 2>/dev/null; exit 0; }

nfind=$(grep -c '' "$FIND")
key=$(cut -f1,2,4 "$FIND" | sort -u)
H=$(short_hash "$key")
STATE=$(hook_state_dir "$start_top")
LAST=""
[ -n "$STATE" ] && LAST="$STATE/secret-scan.$SCOPE.last"

multi=0
[ ${#TOPS[@]} -gt 1 ] && multi=1
REPORT=$(awk -F'\t' -v multi="$multi" '{
  top = $1; n = split(top, parts, "/"); label = parts[n]
  printf "  %s%s:%s  %s\n", (multi ? "[" label "] " : ""), $2, $3, $4
}' "$FIND" | head -n 30)

if [ -n "$LAST" ] && [ -f "$LAST" ] && [ "$(cat "$LAST" 2>/dev/null)" = "$H" ]; then
  emit_system_message "diff-secret-scan: the same $nfind possible secret finding(s) are still in the uncommitted changes (not blocking again). Review them before committing:
$REPORT"
  exit 0
fi
if [ -n "$LAST" ]; then
  printf '%s\n' "$H" > "$LAST.tmp.$$" 2>/dev/null && mv -f "$LAST.tmp.$$" "$LAST" 2>/dev/null
fi

if [ "$STOP_ACTIVE" = "true" ]; then
  emit_system_message "diff-secret-scan: $nfind possible secret finding(s) in the uncommitted changes (stop_hook_active set, so not blocking):
$REPORT"
  exit 0
fi

{
  printf 'diff-secret-scan: %s possible secret(s) in the uncommitted changes. Do not stop yet.\n' "$nfind"
  printf 'path:line  rule   (values are deliberately never printed)\n'
  printf '%s\n' "$REPORT"
  [ "$nfind" -gt 30 ] && printf '  ... and %s more\n' "$((nfind - 30))"
  [ -n "$GL_NOTE" ] && printf 'Note: %s\n' "$GL_NOTE"
  printf 'Remove the value from the working tree (read it from an environment variable or a gitignored file the user manages), and tell the user to rotate it if it was ever real. If a finding is a false positive (e.g. a test fixture), say so explicitly in your summary; an identical finding set will not block again.\n'
} >&2
exit 2
