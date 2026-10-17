#!/usr/bin/env bash
# PreToolUse (matcher: Bash): a BEST-EFFORT LEXICAL SCAN of the command text
# for the shapes this studio never lets an agent run. Exit 2 blocks the call;
# stderr goes back to Claude and names the allowed alternative.
#
# Be precise about what this is. It is a scan, not a sandbox.
#   * A small awk lexer splits the command into segments on unquoted
#     ; && || | & ( ) ` and newlines, and into words with shell quoting
#     removed. Single-quoted text and heredoc BODIES are data and never
#     scanned (so a PR body or commit message may mention `makepkg -si`).
#     `$(...)` and backticks are commands, even inside double quotes.
#     Comments and redirections (`> f`, `2>&1`) are dropped.
#   * Per segment it strips leading VAR=value assignments and the wrappers
#     nice [-n N], timeout [opts] N, time, nohup, stdbuf, command, builtin,
#     exec, noglob, env [opts], xargs [opts], ionice, taskpolicy, setsid,
#     flock, watch and `pixi run|exec` [opts], then checks the real program.
#   * It re-scans the string argument of `bash|sh|zsh|dash|ksh -c`, `eval`,
#     `script -c`, `env -S`, `flock -c` and a single-string `pixi run "..."`.
#
# It CANNOT see: commands assembled at runtime (`eval "$X"`, `${!V}`), a
# script FILE run by an interpreter, commands fed to an interpreter on stdin
# (`bash <<EOF`, `cat f | sh`), aliases/functions, or wrappers it does not
# know. A determined indirection gets past it; that is expected. The other
# layers are the permission rules in .claude/settings.json (deny -> ask ->
# allow; interpreters and unknown commands prompt) and the human.
#
# FAILS OPEN: if neither jq nor python3 can parse the hook payload (only the
# grep fallback is left, which mangles shell metacharacters), awk is missing,
# or the command is larger than GUARD_MAX_BYTES, it exits 0 with a JSON
# systemMessage telling the user the command went unchecked. A
# PreToolUse hook that times out also lets the call through, so the lexer
# is awk (linear time) rather than a bash character loop.
#
# Blocks (see .claude/skills/cpp-security and CLAUDE.md "Hard rules"):
#   privilege escalation (sudo, doas, su, pkexec, run0); sysctl writes;
#   package installs/removals (pacman/yay/paru, apt/apt-get/aptitude, dnf,
#   yum, zypper, brew, pip/pip3/pipx/`python -m pip`/`uv pip`, conda/mamba/
#   micromamba, `pixi global`, global npm/pnpm/yarn, cargo install);
#   `pixi clean cache`; docker/podman pull/push/rmi/build/login/prune, `run`
#   without --rm or with --privileged/--cap-add/--security-opt/--pid=host or a
#   mount that is not a read-only bind of the repo's build/ tree; un-prefixed
#   `cmake --install` (prefix must be under build/ or .platform-runs/),
#   `--target install`, `ninja|make install`; turning warnings-as-errors off
#   (--compile-no-warning-as-error, -D...WARNINGS_AS_ERRORS=OFF,
#   -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF); makepkg -i/-s/-r (and clusters);
#   git tag creation/deletion (listing is fine), push of tags (--tags,
#   --follow-tags, --mirror, refs/tags/), force-push or +refspec or delete of
#   main/master/HEAD/the remote's default branch, `git branch -d/-D/-m/-M/
#   -c/-C/-f` on main/master/the default branch, `commit -n/--no-verify`,
#   `push --no-verify`, `-c core.hooksPath=` overrides; `gh release` (all);
#   any assignment/export of CMAKE_BUILD_PARALLEL_LEVEL, CTEST_PARALLEL_LEVEL,
#   UPDATE_GOLDEN, RATTLER_CACHE_DIR, PIXI_CACHE_DIR; any *SAN_OPTIONS with
#   exitcode=0 or halt_on_error=0; setarch running anything outside
#   .platform-runs/; reading/copying/encoding a secret file (cat, less, more,
#   head, tail, grep, rg, sed, awk, cp sources, base64, xxd, od, strings,
#   source/., or `< file`) matching protect-files' globs - existence checks
#   (ls, test -f, git check-ignore) are not inspected.
set -u
set -f
export LC_ALL=C

HOOKS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd) || exit 0
# shellcheck source=./lib.sh
. "$HOOKS_DIR/lib.sh" 2>/dev/null || exit 0

GUARD_MAX_BYTES=262144

HOOK_INPUT=$(cat 2>/dev/null) || HOOK_INPUT=""
hook_json_field tool_input.command
cmd="$HOOK_JSON_VALUE"
[ -n "$cmd" ] || exit 0

if [ "$HOOK_JSON_PARSER" = "grep" ]; then
  emit_system_message "guard-bash: neither jq nor python3 is available to parse the hook payload; this command was NOT checked (fail-open). The studio's hard rules still apply - review it yourself."
  exit 0
fi
if [ "${#cmd}" -gt "$GUARD_MAX_BYTES" ]; then
  emit_system_message "guard-bash: the command is larger than ${GUARD_MAX_BYTES} bytes; it was NOT checked (fail-open). Review it yourself."
  exit 0
fi
command -v awk >/dev/null 2>&1 || { emit_system_message "guard-bash: awk not found; this command was NOT checked (fail-open). Review it yourself."; exit 0; }

hook_json_field cwd
CWD="$HOOK_JSON_VALUE"
[ -n "$CWD" ] && [ -d "$CWD" ] || CWD="${PWD:-.}"

block() {
  printf 'BLOCKED by guard-bash: %s\n' "$1" >&2
  printf '(guard-bash is a best-effort scan; if this is a false positive, say so and ask the user to run the command themselves.)\n' >&2
  exit 2
}

GB_TOK=$'\036'   # token terminator
GB_SEG=$'\035'   # segment-end marker (a token of its own)
GB_OP=$'\037'    # prefix of a redirection-operator token

# ---------------------------------------------------------------------------
# Lexer (awk, linear time). Output: tokens terminated by \036; a token that is
# exactly \035 ends a segment; redirection operators are emitted as \037<op>
# (the walker drops them together with their target word).
# Each `$(...)` / backtick body is lexed as its own segment(s) at depth+1,
# while the enclosing command keeps its words intact: the substitution is
# replaced by the literal marker `$(...)` inside the word that contained it
# (so `"su$(:)do" x` is still the word `su$(...)do`, and `UPDATE_GOLDEN=$(x)`
# is still an assignment). Arithmetic `$((...))` / `((...))` is plain text.
# ---------------------------------------------------------------------------
# shellcheck disable=SC2016
GB_AWK='
function emit_tok() { if (have) buf[d] = buf[d] tok TS; tok = ""; have = 0 }
function emit_seg() { emit_tok(); if (buf[d] != "") printf "%s%s%s", buf[d], SG, TS; buf[d] = "" }
function emit_op(op) { emit_tok(); buf[d] = buf[d] OPP op TS }
function open_sub(k) {
  savetok[d] = tok; savehave[d] = have; tok = ""; have = 0
  d++; buf[d] = ""; kind[d] = k; ret[d] = mode; mode = "N"
}
function close_sub() {
  emit_seg(); mode = ret[d]; d--
  tok = savetok[d] "$(...)"; have = 1
}
function arith(   depth, ch) {
  # at "((" (i points to the first "("): copy through the matching "))"
  depth = 0
  while (i <= n) {
    ch = substr(s, i, 1); tok = tok ch
    if (ch == "(") depth++
    else if (ch == ")") { depth--; if (depth <= 0) break }
    i++
  }
  have = 1
}
function brace(   depth, ch) {
  depth = 0
  while (i <= n) {
    ch = substr(s, i, 1); tok = tok ch
    if (ch == "{") depth++
    else if (ch == "}") { depth--; if (depth <= 0) break }
    i++
  }
}
function redir(   op, ch) {
  op = ""
  if (have && tok ~ /^[0-9]+$/) { op = tok; tok = ""; have = 0 } else emit_tok()
  while (i <= n) {
    ch = substr(s, i, 1)
    if (ch == "<" || ch == ">" || ch == "&") { op = op ch; i++ }
    else if (ch == "|" && substr(op, length(op), 1) == ">") { op = op ch; i++; break }
    else break
  }
  emit_op(op)
}
function heredoc_op(   strip, dl, ch) {
  emit_tok(); i += 2; strip = 0
  if (substr(s, i, 1) == "-") { strip = 1; i++ }
  while (substr(s, i, 1) == " " || substr(s, i, 1) == "\t") i++
  dl = ""
  while (i <= n) {
    ch = substr(s, i, 1)
    if (ch == " " || ch == "\t" || ch == "\n" || ch == ";" || ch == "&" || ch == "|" || ch == "(" || ch == ")" || ch == "<" || ch == ">") break
    if (ch == "\047") { i++; while (i <= n && substr(s, i, 1) != "\047") { dl = dl substr(s, i, 1); i++ }; i++ }
    else if (ch == "\"") { i++; while (i <= n && substr(s, i, 1) != "\"") { dl = dl substr(s, i, 1); i++ }; i++ }
    else if (ch == "\\") { dl = dl substr(s, i + 1, 1); i += 2 }
    else { dl = dl ch; i++ }
  }
  if (dl != "") { hd_n++; hd_d[hd_n] = dl; hd_s[hd_n] = strip }
}
function heredocs(   k, p, rest, nl, L, j, line, adv, found) {
  if (hd_first > hd_n) return
  p = i + 1
  for (k = hd_first; k <= hd_n; k++) {
    rest = substr(s, p); nl = split(rest, L, "\n"); adv = 0; found = 0
    for (j = 1; j <= nl; j++) {
      line = L[j]
      if (hd_s[k]) sub(/^\t+/, "", line)
      adv += length(L[j]) + 1
      if (line == hd_d[k]) { found = 1; break }
    }
    if (!found) { hd_first = hd_n + 1; i = n; return }
    p += adv
  }
  hd_first = hd_n + 1
  i = p - 2
}
{ s = (NR == 1) ? $0 : s "\n" $0 }
END {
  n = length(s); i = 1; tok = ""; have = 0; mode = "N"; d = 0; buf[0] = ""; kind[0] = ""
  hd_n = 0; hd_first = 1
  while (i <= n) {
    c = substr(s, i, 1)
    if (mode == "S") {
      if (c == "\047") mode = "N"; else tok = tok c
    } else if (mode == "A") {
      if (c == "\\") { tok = tok substr(s, i, 2); i++ }
      else if (c == "\047") mode = "N"
      else tok = tok c
    } else if (mode == "D") {
      if (c == "\"") mode = "N"
      else if (c == "\\") {
        nx = substr(s, i + 1, 1)
        if (nx == "\"" || nx == "\\" || nx == "$" || nx == "`") { tok = tok nx; i++ }
        else if (nx == "\n") i++
        else tok = tok c
      } else if (c == "$" && substr(s, i + 1, 2) == "((") { tok = tok c; i++; arith() }
      else if (c == "$" && substr(s, i + 1, 1) == "(") { open_sub("$"); i++ }
      else if (c == "$" && substr(s, i + 1, 1) == "{") brace()
      else if (c == "`") { if (d > 0 && kind[d] == "`") close_sub(); else open_sub("`") }
      else tok = tok c
    } else {
      if (c == " " || c == "\t" || c == "\r") emit_tok()
      else if (c == "\047") { mode = "S"; have = 1 }
      else if (c == "\"") { mode = "D"; have = 1 }
      else if (c == "\\") { nx = substr(s, i + 1, 1); if (nx != "\n") { tok = tok nx; have = 1 }; i++ }
      else if (c == "#" && !have) {
        nx = index(substr(s, i), "\n")
        if (nx == 0) { i = n + 1; continue }
        i += nx - 1; continue
      } else if (c == "$") {
        nx = substr(s, i + 1, 1)
        if (nx == "(" && substr(s, i + 2, 1) == "(") { tok = tok c; i++; arith() }
        else if (nx == "(") { open_sub("$"); i++ }
        else if (nx == "\047") { mode = "A"; have = 1; i++ }
        else if (nx == "{") { brace(); have = 1 }
        else { tok = tok c; have = 1 }
      } else if (c == "`") {
        if (d > 0 && kind[d] == "`") close_sub(); else open_sub("`")
      } else if (c == "(") {
        if (substr(s, i + 1, 1) == "(" && !have) { emit_seg(); arith(); emit_seg() }
        else emit_seg()
      } else if (c == ")") {
        if (d > 0 && kind[d] == "$") close_sub(); else emit_seg()
      } else if (c == "\n") { emit_seg(); heredocs() }
      else if (c == ";" || c == "|") emit_seg()
      else if (c == "&") {
        if (substr(s, i + 1, 1) == ">") { redir(); continue }
        emit_seg()
      } else if (c == "<" || c == ">") {
        if (substr(s, i, 3) == "<<<") { emit_op("<<<"); i += 3; continue }
        else if (substr(s, i, 2) == "<<") { heredoc_op(); continue }
        else { redir(); continue }
      } else { tok = tok c; have = 1 }
    }
    i++
  }
  while (d > 0) { emit_seg(); d-- }
  emit_seg()
}
'

gb_lex() {
  printf '%s\n' "${1:-}" | tr -d '\035\036\037' \
    | awk -v TS="$GB_TOK" -v SG="$GB_SEG" -v OPP="$GB_OP" "$GB_AWK" 2>/dev/null
}

# ---------------------------------------------------------------------------
# Lazy repo facts (read from the hook input's cwd, never the hook's own pwd)
# ---------------------------------------------------------------------------
_TOP_DONE=0; TOP=""
repo_top() {
  if [ "$_TOP_DONE" = 0 ]; then
    TOP=$(git -C "$CWD" rev-parse --show-toplevel 2>/dev/null </dev/null) || TOP=""
    _TOP_DONE=1
  fi
  printf '%s' "$TOP"
}

# path_under <path> <subdir>: true if <path> (relative to CWD, or absolute)
# lies under <subdir>/ of the current repo/worktree, with no `..`.
path_under() {
  local p="${1:-}" sub="${2:-}" top=""
  [ -n "$p" ] && [ -n "$sub" ] || return 1
  case "$p" in *..*) return 1 ;; esac
  case "$p" in
    '$PWD/'*) p="$CWD/${p#'$PWD/'}" ;;
    '${PWD}/'*) p="$CWD/${p#'${PWD}/'}" ;;
  esac
  case "$p" in
    ./*) p="${p#./}" ;;
  esac
  case "$p" in
    *'$'*|*'`'*|'~'*) return 1 ;;
    /*)
      top=$(repo_top)
      [ -n "$top" ] || top="$CWD"
      case "$p" in "$top/$sub"/*|"$top/$sub") return 0 ;; esac
      case "$p" in "${CWD%/}/$sub"/*) return 0 ;; esac
      return 1 ;;
    "$sub"/*|"$sub") return 0 ;;
  esac
  return 1
}

# block_if_computed <what> <value> <fix>: a path built by $(...) or backticks
# (the lexer leaves the marker `$(...)` in the word) cannot be verified here.
block_if_computed() {
  case "${2:-}" in
    *'$('*|*'`'*)
      block "$1 '${2}' is computed at runtime by a command substitution, so it cannot be verified. Spell it out: a relative build/... path, \$PWD/build/..., or the literal absolute path. $3" ;;
  esac
  return 0
}

# ---------------------------------------------------------------------------
# Rule helpers
# ---------------------------------------------------------------------------
check_assignment() {
  local name="${1%+}" value="${2:-}"
  case "$name" in
    CMAKE_BUILD_PARALLEL_LEVEL|CTEST_PARALLEL_LEVEL)
      block "setting or exporting $name is forbidden: it silently overrides the per-worktree job cap (precedence: CLI -j > env > preset jobs). Job caps live in CMakeUserPresets.json (<preset>-local + the cpp-cli-studio vendor key, written by /features). Run the task as typed instead, e.g. 'nice pixi run check'." ;;
    UPDATE_GOLDEN)
      block "UPDATE_GOLDEN is never set by an agent: goldens are human-blessed. If the output change is intended, stop and request a re-bless; the user approves 'pixi run bless <ctest-regex>' (only scripts/bless.sh sets UPDATE_GOLDEN)." ;;
    RATTLER_CACHE_DIR|PIXI_CACHE_DIR)
      block "agents never set $name inline. If the pixi cache must move (e.g. an ecryptfs/NFS home: 'File name too long'), the user exports $name to a path on another filesystem in their shell profile BEFORE launching claude (README 'Before you start'). Report the problem and stop." ;;
    *SAN_OPTIONS)
      case ",:$value:," in
        *[,:\ ]exitcode=0[,:\ ]*|*[,:\ ]halt_on_error=0[,:\ ]*)
          block "$name with exitcode=0 / halt_on_error=0 hides sanitizer findings. Sanitizer options come from the build (generated sanitizer_options.cpp and the test env, exitcode=86). Fix or report the finding; see the memory-safety skill." ;;
      esac ;;
  esac
  return 0
}

# Token-level checks that apply anywhere in any segment.
check_global_tokens() {
  local t="" prev="" v="" name="" val=""
  for t in "$@"; do
    case "$t" in
      --compile-no-warning-as-error)
        block "--compile-no-warning-as-error turns -Werror off. Fix the warning (or, with a reason, a targeted NOLINTNEXTLINE / pragma); see the cpp-quality skill." ;;
    esac
    v=""
    case "$t" in
      -D?*) v="${t#-D}" ;;
      *) [ "$prev" = "-D" ] && v="$t" ;;
    esac
    case "$v" in *=*) : ;; *) v="" ;; esac
    if [ -n "$v" ]; then
      name="${v%%=*}"; name="${name%%:*}"
      val=""
      case "$v" in *=*) val="${v#*=}" ;; esac
      case "$name" in
        *WARNINGS_AS_ERRORS|CMAKE_COMPILE_WARNING_AS_ERROR)
          case "$val" in
            OFF|off|Off|0|FALSE|false|False|NO|no|No|N|n|IGNORE|NOTFOUND|*-NOTFOUND|'')
              block "-D$name=$val turns warnings-as-errors off. Keep it on and fix the warning; see the cpp-quality skill. (Only a distro PKGBUILD's build() turns it off, and that file is written by a human-reviewed scaffold, not on a command line.)" ;;
          esac ;;
        UPDATE_GOLDEN)
          check_assignment UPDATE_GOLDEN "$val" ;;
      esac
    fi
    prev="$t"
  done
  return 0
}

check_sysctl() {
  local t=""
  for t in "$@"; do
    case "$t" in
      -w|--write|-p*|--load|--load=*|--system|*=*)
        block "sysctl writes change kernel state for the whole machine. Read values with 'sysctl -n <key>' or 'cat /proc/sys/...'; if a sanitizer needs e.g. vm.mmap_rnd_bits=28, report it as a platform-testing degradation reason and let the user decide." ;;
    esac
  done
  return 0
}

PKG_FIX="The studio never installs or removes system/global packages. Report what is missing and the command a human would run; project dependencies change only via 'pixi add'/'pixi remove' (which ask the user)."

check_pacman_like() {
  local prog="$1" t=""
  shift
  if [ $# -eq 0 ] && [ "$prog" != "pacman" ]; then
    block "'$prog' with no arguments upgrades the whole system. $PKG_FIX"
  fi
  for t in "$@"; do
    case "$t" in
      -S[silgqp]*)
        case "$t" in *[!Ssilgqp-]*|*y*|*u*|*w*) block "'$prog $t' installs or syncs packages. $PKG_FIX" ;; esac ;;
      -S|-S*|-U*|-R*|--sync|--upgrade|--remove|--sysupgrade)
        block "'$prog $t' installs, upgrades or removes packages. $PKG_FIX" ;;
    esac
  done
  return 0
}

check_subcommand_pkg() {
  local prog="$1" sub="" t=""
  shift
  for t in "$@"; do
    case "$t" in
      -*) continue ;;
      *) sub="$t"; break ;;
    esac
  done
  case "$sub" in
    install|reinstall|remove|purge|autoremove|autopurge|erase|upgrade|dist-upgrade|full-upgrade|update|downgrade|build-dep|localinstall|groupinstall|groupremove|swap|in|rm|up|dup|uninstall|tap|untap|link|unlink|create|inject|upgrade-all|reinstall-all)
      block "'$prog $sub' installs, upgrades or removes packages or environments. $PKG_FIX" ;;
    env)
      case " $* " in
        *" create "*|*" remove "*|*" update "*)
          block "'$prog env create/remove/update' mutates environments outside the project. $PKG_FIX" ;;
      esac ;;
  esac
  return 0
}

check_pip() {
  local prog="$1" t=""
  shift
  for t in "$@"; do
    case "$t" in
      -*) continue ;;
      install|uninstall|download)
        block "'$prog $t' changes a Python environment outside the project. $PKG_FIX" ;;
      *) return 0 ;;
    esac
  done
  return 0
}

check_python() {
  local prog="$1"
  shift
  while [ $# -gt 0 ]; do
    case "$1" in
      -m)
        case "${2:-}" in
          pip|pip3) shift 2; check_pip "$prog -m pip" "$@"; return 0 ;;
          ensurepip) block "'$prog -m ensurepip' installs pip into an interpreter. $PKG_FIX" ;;
        esac
        return 0 ;;
      -c) return 0 ;;
      -*) shift ;;
      *) return 0 ;;
    esac
  done
  return 0
}

check_uv() {
  case "${1:-} ${2:-}" in
    "pip install"*|"pip uninstall"*|"tool install"*|"tool uninstall"*|"python install"*)
      block "'uv ${1:-} ${2:-}' installs into an environment outside the project. $PKG_FIX" ;;
  esac
  return 0
}

check_js_pkg() {
  local prog="$1" t="" sub="" global=0
  shift
  for t in "$@"; do
    case "$t" in
      -g|--global|--location=global) global=1 ;;
      -*) : ;;
      *) [ -z "$sub" ] && sub="$t" ;;
    esac
  done
  [ "$prog" = "yarn" ] && [ "$sub" = "global" ] && global=1
  if [ "$global" = 1 ]; then
    case "$sub" in
      install|i|add|update|up|upgrade|uninstall|remove|rm|un|r|link|global|'')
        block "a global '$prog' install/remove changes the user's machine. $PKG_FIX" ;;
    esac
  fi
  return 0
}

check_cargo() {
  local t=""
  for t in "$@"; do
    case "$t" in
      -*) continue ;;
      install|uninstall) block "'cargo $t' installs into ~/.cargo/bin. $PKG_FIX" ;;
      *) return 0 ;;
    esac
  done
  return 0
}

# pixi: `global` and `clean cache` are blocked; `run`/`exec`/`x` are wrappers
# and are handled in the walker.
check_pixi() {
  local t="" sub=""
  for t in "$@"; do
    case "$t" in
      -*) [ -z "$sub" ] && continue ;;
      *) if [ -z "$sub" ]; then sub="$t"; continue; fi ;;
    esac
    if [ "$sub" = "clean" ] && [ "$t" = "cache" ]; then
      block "'pixi clean cache' wipes the shared package cache that every worktree's env hard-links from. To reset a build, remove only this worktree's build/<env>/<preset> directory; anything else, ask the user."
    fi
  done
  case "$sub" in
    global)
      block "'pixi global' installs tools onto the user's machine. Tools belong in pixi.toml (pixi add, which asks the user); report what is missing." ;;
  esac
  return 0
}

# docker / podman
DOCKER_FIX="Containers are probe-only: 'docker info', 'docker image inspect <img>', and 'docker run --rm --network none -v <repo>/build/<env>/<preset>/e2e-prefix:/opt/t:ro <img> /opt/t/bin/<tool> --version'. Never pull/build/push; report a missing image as a degradation reason."
check_docker_mount_src() {
  local src="$1" opts="$2" kind="$3"
  block_if_computed "$kind mount source" "$src" "$DOCKER_FIX"
  case "$src" in
    *docker.sock*|*podman.sock*)
      block "mounting the container-engine socket gives the container root on the host. $DOCKER_FIX" ;;
  esac
  if ! path_under "$src" build; then
    block "$kind mount source '$src' is not under the repo's build/ tree. Only read-only binds of build/... are allowed (never /, \$HOME, ~ or named volumes). $DOCKER_FIX"
  fi
  case ",$opts," in
    *,ro,*|*,readonly,*|*,readonly=true,*|*,ro=true,*|*,readonly=1,*|*,ro=1,*) : ;;
    *) block "$kind mount of '$src' must be read-only (append ':ro'). $DOCKER_FIX" ;;
  esac
  return 0
}
check_docker_volume() {
  local v="$1" src="" rest="" opts=""
  block_if_computed "-v mount" "$v" "$DOCKER_FIX"
  src="${v%%:*}"
  rest="${v#*:}"
  if [ "$rest" = "$v" ]; then
    block "anonymous volume '-v $v' is not allowed. $DOCKER_FIX"
  fi
  case "$rest" in
    *:*) opts="${rest#*:}" ;;
    *) opts="" ;;
  esac
  check_docker_mount_src "$src" "$opts" "-v"
}
check_docker_mount() {
  local m="$1" kv="" type="bind" src="" ro="" IFS=','
  for kv in $m; do
    case "$kv" in
      type=*) type="${kv#type=}" ;;
      source=*|src=*) src="${kv#*=}" ;;
      readonly|ro|readonly=true|ro=true|readonly=1|ro=1) ro="ro" ;;
    esac
  done
  IFS=$' \t\n'
  [ "$type" = "tmpfs" ] && return 0
  [ -n "$src" ] || block "--mount without an explicit build/ source is not allowed. $DOCKER_FIX"
  check_docker_mount_src "$src" "$ro" "--mount"
}
check_docker_run() {
  local t="" rm=0
  while [ $# -gt 0 ]; do
    t="$1"
    case "$t" in
      --help) return 0 ;;
      --rm|--rm=true) rm=1 ;;
      --privileged|--privileged=true|--cap-add|--cap-add=*|--security-opt|--security-opt=*|--pid=host|--userns=host|--device|--device=*)
        block "'docker run $t' weakens container isolation. $DOCKER_FIX" ;;
      --pid)
        [ "${2:-}" = "host" ] && block "'docker run --pid host' shares the host PID namespace. $DOCKER_FIX"
        shift ;;
      -v|--volume) check_docker_volume "${2:-}"; shift ;;
      -v?*) check_docker_volume "${t#-v}" ;;
      --volume=*) check_docker_volume "${t#--volume=}" ;;
      --mount) check_docker_mount "${2:-}"; shift ;;
      --mount=*) check_docker_mount "${t#--mount=}" ;;
      --volumes-from|--volumes-from=*)
        block "'--volumes-from' mounts another container's volumes. $DOCKER_FIX" ;;
      --*=*) : ;;
      --init|--read-only|--tty|--interactive|--detach|--no-healthcheck|--oom-kill-disable|--publish-all|--quiet|--help|--disable-content-trust) : ;;
      --*) shift ;;
      -[aceghlmpuw]) shift ;;
      -*) : ;;
      *) break ;;
    esac
    shift
  done
  if [ "$rm" = 0 ]; then
    block "'docker run' without --rm leaves containers behind. $DOCKER_FIX"
  fi
  return 0
}
check_docker() {
  local prog="$1" t="" sub="" sub2=""
  shift
  while [ $# -gt 0 ]; do
    case "$1" in
      -H|--host|-c|--context|--config|-l|--log-level|--tlscacert|--tlscert|--tlskey|--url|--connection|--root|--runroot)
        shift 2 2>/dev/null || return 0; continue ;;
      -*) shift; continue ;;
      *) sub="$1"; shift; break ;;
    esac
  done
  case "$sub" in
    image|container|system|builder|buildx|volume|network|manifest)
      sub2="${1:-}"
      [ $# -gt 0 ] && shift ;;
  esac
  case "$sub:$sub2" in
    pull:*|push:*|rmi:*|build:*|login:*|import:*|load:*|commit:*|image:pull|image:push|image:rm|image:build|image:import|image:load|buildx:build|buildx:bake|builder:build|manifest:push)
      block "'$prog $sub${sub2:+ $sub2}' pulls, builds, pushes, deletes or imports images. $DOCKER_FIX" ;;
    *:prune|prune:*)
      block "'$prog $sub${sub2:+ $sub2}' deletes the user's containers/images/volumes. $DOCKER_FIX" ;;
    volume:rm|volume:create)
      block "'$prog volume $sub2' changes the user's volumes. $DOCKER_FIX" ;;
    run:*|create:*|container:run|container:create)
      [ "$sub" = "container" ] || { [ -n "$sub2" ] && set -- "$sub2" "$@"; }
      check_docker_run "$@" ;;
  esac
  return 0
}

# cmake
check_cmake() {
  local mode="" t="" prefix="" have_prefix=0 in_target=0 tgt_install=0
  local -a envargs=()
  while [ $# -gt 0 ]; do
    t="$1"
    case "$t" in
      --install) mode=install ;;
      --build) mode=build ;;
      --prefix) have_prefix=1; prefix="${2:-}"; shift ;;
      --prefix=*) have_prefix=1; prefix="${t#--prefix=}" ;;
      --target|-t) in_target=1; shift; continue ;;
      --target=*) case "${t#--target=}" in install|install/*) tgt_install=1 ;; esac ;;
      -E)
        if [ "${2:-}" = "env" ]; then
          shift 2
          while [ $# -gt 0 ]; do
            case "$1" in
              --unset=*|--modify=*|-) shift; continue ;;
              --) shift; break ;;
              [A-Za-z_]*=*) check_assignment "${1%%=*}" "${1#*=}"; shift; continue ;;
              *) break ;;
            esac
          done
          [ $# -gt 0 ] && scan_words "$@"
          return 0
        fi ;;
    esac
    if [ "$in_target" = 1 ]; then
      case "$t" in
        -*) in_target=0 ;;
        install|install/*) tgt_install=1 ;;
      esac
    fi
    shift
  done
  if [ "$tgt_install" = 1 ]; then
    block "building the 'install' target installs into CMAKE_INSTALL_PREFIX (system paths). Install only into the build tree: 'cmake --install build/<env>/<preset> --prefix build/<env>/<preset>/e2e-prefix' (the e2e.install CTest fixture already does this)."
  fi
  if [ "$mode" = install ]; then
    if [ "$have_prefix" = 0 ]; then
      block "'cmake --install' without --prefix installs into the configured prefix (system paths). Use '--prefix build/<env>/<preset>/e2e-prefix' or a prefix under .platform-runs/."
    fi
    block_if_computed "the --prefix" "$prefix" "Use '--prefix build/<env>/<preset>/e2e-prefix'."
    if ! path_under "$prefix" build && ! path_under "$prefix" .platform-runs; then
      block "'cmake --install --prefix $prefix' is outside build/ and .platform-runs/. Use '--prefix build/<env>/<preset>/e2e-prefix' or '.platform-runs/<run>/prefix'."
    fi
  fi
  return 0
}

check_build_tool() {
  local prog="$1" t=""
  shift
  while [ $# -gt 0 ]; do
    t="$1"
    if [ "$prog" = "ninja" ]; then
      case "$t" in
        -t) return 0 ;;   # tool mode, not a build
        -C|-f|-j|-k|-l|-d|-w) shift 2 2>/dev/null || return 0; continue ;;
      esac
    else
      case "$t" in
        -C|-f|-o|-W|-I|--directory|--file|--makefile|--assume-old|--assume-new|--include-dir|--what-if|--old-file|--new-file)
          shift 2 2>/dev/null || return 0; continue ;;
        -j|-l|--jobs|--load-average)
          case "${2:-}" in ''|*[!0-9.]*) : ;; *) shift ;; esac ;;
      esac
    fi
    case "$t" in
      -*) : ;;
      *=*) : ;;
      install|install/*)
        block "'$prog $t' installs into CMAKE_INSTALL_PREFIX (system paths). Install only into the build tree: 'cmake --install build/<env>/<preset> --prefix build/<env>/<preset>/e2e-prefix'." ;;
    esac
    shift
  done
  return 0
}

check_makepkg() {
  local t="" c="" rest=""
  for t in "$@"; do
    case "$t" in
      --install|--syncdeps|--rmdeps|--install=*)
        block "'makepkg $t' installs or removes system packages via pacman. Build only ('makepkg -f', 'makepkg --printsrcinfo'); installing the package is a human step." ;;
      --*) : ;;
      -?*)
        rest="${t#-}"
        while [ -n "$rest" ]; do
          c="${rest:0:1}"; rest="${rest:1}"
          case "$c" in
            p|D) break ;;
            i|s|r) block "'makepkg $t' (-i/-s/-r) installs or removes system packages via pacman. Build only ('makepkg -f', 'makepkg --printsrcinfo'); installing the package is a human step." ;;
          esac
        done ;;
    esac
  done
  return 0
}

# git
GIT_DIR_CWD=""
git_q() { git -C "$GIT_DIR_CWD" "$@" 2>/dev/null </dev/null; }
check_git_tag() {
  local t="" list=0 mutate=0 positional=0 skip=0
  [ $# -eq 0 ] && return 0
  for t in "$@"; do
    if [ "$skip" = 1 ]; then skip=0; continue; fi
    case "$t" in
      -l|--list|-n|-n[0-9]*|--column|--column=*|--no-column|-i|--ignore-case|--sort=*|--format=*|--color|--color=*|--omit-empty|--contains=*|--no-contains=*|--points-at=*|--merged=*|--no-merged=*|-v|--verify)
        list=1 ;;
      --contains|--no-contains|--points-at|--merged|--no-merged|--sort|--format)
        list=1; skip=1 ;;
      -a|-s|-u|-f|-d|-m|-F|-e|--annotate|--sign|--local-user|--local-user=*|--force|--delete|--message|--message=*|--file|--file=*|--edit|--create-reflog|--trailer|--trailer=*|--cleanup=*|-m?*|-F?*|-u?*)
        mutate=1 ;;
      -*) mutate=1 ;;
      *) positional=$((positional + 1)) ;;
    esac
  done
  if [ "$mutate" = 1 ] || { [ "$positional" -gt 0 ] && [ "$list" = 0 ]; }; then
    block "creating or deleting tags is a release action the user performs (CI's release workflow is tag-driven). List tags with 'git tag -l' / 'git tag --list <pattern>'."
  fi
  return 0
}
check_git_commit() {
  local t="" c="" rest="" skip=0
  for t in "$@"; do
    if [ "$skip" = 1 ]; then skip=0; continue; fi
    case "$t" in
      --) break ;;
      --no-verify)
        block "'git commit --no-verify' skips the pre-commit checks. Fix what they report instead." ;;
      --message|--file|--author|--date|--template|--reuse-message|--reedit-message|--fixup|--squash|--trailer|--cleanup|--pathspec-from-file)
        skip=1 ;;
      --*) : ;;
      -?*)
        rest="${t#-}"
        while [ -n "$rest" ]; do
          c="${rest:0:1}"; rest="${rest:1}"
          case "$c" in
            n) block "'git commit -n' is --no-verify: it skips the pre-commit checks. Fix what they report instead." ;;
            m|F|C|c|t) [ -z "$rest" ] && skip=1; break ;;
            S|u) break ;;
          esac
        done ;;
    esac
  done
  return 0
}
_PROTECTED=""
protected_branches() {
  local remote="${1:-origin}" b="" cur=""
  if [ -z "$_PROTECTED" ]; then
    _PROTECTED=" main master HEAD "
    b=$(git_q symbolic-ref --short "refs/remotes/$remote/HEAD") && [ -n "$b" ] && _PROTECTED="$_PROTECTED${b#"$remote"/} "
    b=$(git_q config --get init.defaultBranch) && [ -n "$b" ] && _PROTECTED="$_PROTECTED$b "
  fi
  printf '%s' "$_PROTECTED"
}
check_git_push() {
  local t="" c="" rest="" force=0 delete=0 all=0 skip=0 remote="" cur="" dst="" plus=0 prot=""
  local -a refs=()
  for t in "$@"; do
    if [ "$skip" = 1 ]; then skip=0; continue; fi
    case "$t" in
      --tags|--follow-tags|--mirror)
        block "'git push $t' publishes tags (release triggers). Tags are pushed by the user. Push the feature branch only: 'git push -u origin feature/<slug>'." ;;
      --no-verify)
        block "'git push --no-verify' skips the pre-push checks. Fix what they report instead." ;;
      -f|--force|--force-with-lease|--force-with-lease=*) force=1 ;;
      -d|--delete) delete=1 ;;
      --all|--branches) all=1 ;;
      --repo|--receive-pack|--exec|-o|--push-option) skip=1 ;;
      --*) : ;;
      -?*)
        rest="${t#-}"
        while [ -n "$rest" ]; do
          c="${rest:0:1}"; rest="${rest:1}"
          case "$c" in
            f) force=1 ;;
            d) delete=1 ;;
            o) [ -z "$rest" ] && skip=1; break ;;
          esac
        done ;;
      *)
        case "$t" in *refs/tags/*)
          block "pushing refs/tags/... publishes a tag (a release trigger). Tags are pushed by the user." ;;
        esac
        if [ -z "$remote" ]; then remote="$t"; else refs+=("$t"); fi ;;
    esac
  done
  [ "$force" = 1 ] || [ "$delete" = 1 ] || [ "$all" = 1 ] || {
    for t in ${refs[@]+"${refs[@]}"}; do
      case "$t" in +*|:*) plus=1 ;; esac
    done
    [ "$plus" = 1 ] || return 0
  }
  if [ "$all" = 1 ] && { [ "$force" = 1 ] || [ "$delete" = 1 ]; }; then
    block "a forced/deleting 'git push --all' rewrites the default branch too. Push only the feature branch."
  fi
  prot=$(protected_branches "${remote:-origin}")
  cur=$(git_q symbolic-ref --short HEAD) || cur=""
  [ -n "$cur" ] || cur="HEAD"
  if [ ${#refs[@]} -eq 0 ]; then
    if [ "$force" = 1 ] || [ "$delete" = 1 ]; then
      case "$prot" in *" $cur "*)
        block "force-pushing the current branch '$cur' rewrites shared history on a protected branch (main/master/HEAD/the default branch). Push a feature branch and open a PR; a force-push to the default branch is a human action." ;;
      esac
    fi
    return 0
  fi
  for t in "${refs[@]}"; do
    plus=0
    case "$t" in +*) plus=1; t="${t#+}" ;; esac
    case "$t" in
      :*) dst="${t#:}"; plus=1 ;;
      *:*) dst="${t##*:}" ;;
      *) dst="$t" ;;
    esac
    [ "$dst" = "HEAD" ] && dst="$cur"
    dst="${dst#refs/heads/}"
    if [ "$force" = 1 ] || [ "$delete" = 1 ] || [ "$plus" = 1 ]; then
      case "$dst" in
        *'$'*|*'`'*|'')
          block "a forced/deleting push whose target ref is computed at runtime cannot be verified. Spell out the feature branch: 'git push --force-with-lease origin feature/<slug>'." ;;
      esac
      case "$prot" in *" $dst "*)
        block "force-pushing, '+refspec'-pushing or deleting '$dst' rewrites the protected default branch. Push a feature branch and open a PR; anything else on '$dst' is a human action." ;;
      esac
    fi
  done
  return 0
}
check_git_branch() {
  local t="" c="" rest="" mut=0 move=0 skip=0 prot="" cur="" name=""
  local -a names=()
  for t in "$@"; do
    if [ "$skip" = 1 ]; then skip=0; continue; fi
    case "$t" in
      -d|-D|--delete|-c|-C|--copy|-f|--force) mut=1 ;;
      -m|-M|--move) mut=1; move=1 ;;
      -u|--set-upstream-to|--contains|--no-contains|--merged|--no-merged|--points-at|--sort|--format|--color|--column) skip=1 ;;
      --) : ;;
      --*) : ;;
      -?*)
        rest="${t#-}"
        while [ -n "$rest" ]; do
          c="${rest:0:1}"; rest="${rest:1}"
          case "$c" in
            d|D|c|C|f) mut=1 ;;
            m|M) mut=1; move=1 ;;
            u) [ -z "$rest" ] && skip=1; break ;;
          esac
        done ;;
      *) names+=("$t") ;;
    esac
  done
  [ "$mut" = 1 ] || return 0
  prot=$(protected_branches origin)
  if [ "$move" = 1 ] && [ ${#names[@]} -le 1 ]; then
    cur=$(git_q symbolic-ref --short HEAD) || cur=""
    [ -n "$cur" ] && names+=("$cur")
  fi
  for name in ${names[@]+"${names[@]}"}; do
    name="${name#refs/heads/}"
    [ "$name" = "HEAD" ] && continue
    case "$prot" in *" $name "*)
      block "deleting, renaming, copying over or force-resetting the protected branch '$name' (main/master/the default branch) is a human action. Work on a feature branch; list branches with 'git branch --list' / 'git branch -v'." ;;
    esac
  done
  return 0
}
check_git() {
  local sub="" t="" hooks_off=0 dir="$CWD"
  while [ $# -gt 0 ]; do
    case "$1" in
      -C) dir=$(abs_path "${2:-.}" "$dir"); shift 2 2>/dev/null || return 0; continue ;;
      -c)
        case "${2:-}" in [Cc][Oo][Rr][Ee].[Hh][Oo][Oo][Kk][Ss][Pp][Aa][Tt][Hh]=*) hooks_off=1 ;; esac
        shift 2 2>/dev/null || return 0; continue ;;
      --git-dir|--work-tree|--namespace|--exec-path|--config-env|--super-prefix|--list-cmds|--attr-source)
        shift 2 2>/dev/null || return 0; continue ;;
      -*) shift; continue ;;
      *) sub="$1"; shift; break ;;
    esac
  done
  GIT_DIR_CWD="$dir"
  _PROTECTED=""
  if [ "$hooks_off" = 1 ]; then
    case "$sub" in commit|push|merge|rebase|am|cherry-pick|revert)
      block "overriding core.hooksPath on the command line disables the repo's git hooks. Run 'git $sub' plainly and fix what the hooks report." ;;
    esac
  fi
  case "$sub" in
    tag) check_git_tag "$@" ;;
    branch) check_git_branch "$@" ;;
    push) check_git_push "$@" ;;
    commit) check_git_commit "$@" ;;
  esac
  return 0
}

check_gh() {
  local sub=""
  while [ $# -gt 0 ]; do
    case "$1" in
      -R|--repo|--hostname) shift 2 2>/dev/null || return 0; continue ;;
      -*) shift; continue ;;
      *) sub="$1"; break ;;
    esac
  done
  if [ "$sub" = "release" ]; then
    block "'gh release' (any subcommand) is a human release action; releases come from the tag-driven CI workflow. Read CI with 'gh run list' / 'gh run view <id>'."
  fi
  return 0
}

check_setarch() {
  local t="" arch_seen=0 prog=""
  while [ $# -gt 0 ]; do
    t="$1"
    case "$t" in
      --list|-h|--help|-V|--version) return 0 ;;
      --) shift; prog="${1:-}"; break ;;
      -*) : ;;
      */*) prog="$t"; break ;;   # arch-less form: the first path is the program
      *)
        if [ "$arch_seen" = 0 ]; then arch_seen=1; else prog="$t"; break; fi ;;
    esac
    shift
  done
  block_if_computed "the setarch program" "$prog" "Run e.g. 'setarch x86_64 -R .platform-runs/<run>/probe_clean'."
  if [ -z "$prog" ] || ! path_under "$prog" .platform-runs; then
    block "setarch may only run a probe binary under .platform-runs/ (e.g. 'setarch x86_64 -R .platform-runs/<run>/probe_clean'); it never wraps a shell or another program."
  fi
  return 0
}

check_export_like() {
  local t=""
  for t in "$@"; do
    case "$t" in
      -*) continue ;;
      [A-Za-z_]*=*) check_assignment "${t%%=*}" "${t#*=}" ;;
      [A-Za-z_]*) check_assignment "$t" "" ;;
    esac
  done
  return 0
}

# Interpreter wrappers: re-scan the string they will run.
check_shell_c() {
  local t="" c="" rest="" want=0
  while [ $# -gt 0 ]; do
    t="$1"
    if [ "$want" = 1 ]; then scan_command "$t"; return 0; fi
    case "$t" in
      -c|--command) want=1 ;;
      -o|+o|-O|+O|--rcfile|--init-file) shift ;;
      --*) : ;;
      -?*|+?*)
        rest="${t#?}"
        case "$rest" in
          *c*) want=1 ;;
          *o|*O) shift ;;   # e.g. -euxo pipefail
        esac ;;
      *) return 0 ;;
    esac
    shift
  done
  return 0
}
check_script_c() {
  local t="" c="" rest=""
  while [ $# -gt 0 ]; do
    t="$1"
    case "$t" in
      -c|--command) scan_command "${2:-}"; return 0 ;;
      --command=*) scan_command "${t#--command=}"; return 0 ;;
      -E|--echo|-I|--log-in|-O|--log-out|-B|--log-io|-T|--log-timing|-m|--logging-format|-o|--output-limit) shift ;;
      --*) : ;;
      -?*)
        rest="${t#-}"
        while [ -n "$rest" ]; do
          c="${rest:0:1}"; rest="${rest:1}"
          if [ "$c" = "c" ]; then
            if [ -n "$rest" ]; then scan_command "$rest"; else scan_command "${2:-}"; fi
            return 0
          fi
        done ;;
    esac
    shift
  done
  return 0
}

# ---------------------------------------------------------------------------
# Secret-file reads (Read() deny rules do not apply to Bash)
# ---------------------------------------------------------------------------
# is_secret_path <operand>: the same globs protect-files.sh uses.
is_secret_path() {
  local p="${1:-}" b="" top=""
  [ -n "$p" ] || return 1
  case "$p" in
    /*)
      top=$(repo_top)
      [ -n "$top" ] && case "$p" in "$top"/*) p="${p#"$top"/}" ;; esac ;;
  esac
  p="${p#./}"
  b="${p##*/}"
  case "$b" in
    .env.example|settings.local.json.example) return 1 ;;
    .env|.env.*|.env[*?[]*) return 0 ;;
    credentials|.credentials|credentials.json|credentials.yaml|credentials.yml|credentials.toml|credentials.ini|credentials.env|credentials.txt|*.credentials) return 0 ;;
    *id_rsa*|*id_ed25519*|*.pem|*.key|*.p12|*.pfx) return 0 ;;
  esac
  case "/$p" in
    */secrets/*|*/credentials/*|*/.claude/settings.local.json) return 0 ;;
  esac
  return 1
}

SECRET_READ_FIX="don't read secret files; ask the human. The studio never reads, prints, copies or encodes .env files, keys, certificates, credentials or .claude/settings.local.json; if a value is needed, the user exports it in their own shell. (.env.example and settings.local.json.example are fine.)"

# check_secret_read <prog> <args>...: best-effort operand scan for common
# read/copy/encode programs. Existence checks (ls, test -f, [ -f ],
# git check-ignore) are not inspected at all.
check_secret_read() {
  local prog="$1" t="" vopts=" " patfirst=0 seen_pat=0 first=1 last="" tdir=0
  local -a ops=()
  shift
  case "$prog" in
    head|tail) vopts=" -n -c " ;;
    grep|egrep|fgrep) vopts=" -e -f -m -A -B -C -d -D "; patfirst=1 ;;
    rg) vopts=" -e -f -g -t -T -m -A -B -C -M -j -E --type --glob --max-count --context --engine "; patfirst=1 ;;
    sed) vopts=" -e -f -l "; patfirst=1 ;;
    awk|gawk|mawk) vopts=" -f -v -F "; patfirst=1 ;;
    cp) vopts=" -t -S --target-directory --suffix " ;;
    base64) vopts=" -w " ;;
    xxd) vopts=" -s -l -c -g -o -n " ;;
    od) vopts=" -A -t -j -N -w " ;;
    strings) vopts=" -n -t -e -T " ;;
    less|more) vopts=" -b -h -j -k -o -O -p -P -t -T -x -y -z " ;;
  esac
  while [ $# -gt 0 ]; do
    t="$1"
    case "$t" in
      --) shift; while [ $# -gt 0 ]; do ops+=("$1"); shift; done; break ;;
      -e|-f)
        case "$prog" in
          grep|egrep|fgrep|rg|sed|awk|gawk|mawk)
            [ "$t" = "-f" ] && is_secret_path "${2:-}" && block "'$prog -f ${2:-}' reads a secret file. $SECRET_READ_FIX"
            seen_pat=1; shift 2 2>/dev/null || break; continue ;;
        esac ;;
      -t|--target-directory) [ "$prog" = "cp" ] && tdir=1 ;;
      --target-directory=*) [ "$prog" = "cp" ] && tdir=1 ;;
    esac
    case "$t" in
      --*=*) : ;;
      -?*)
        case "$vopts" in *" $t "*) shift ;; esac ;;
      *) ops+=("$t") ;;
    esac
    shift
  done
  # Drop the leading pattern/script/program operand where there is one.
  if [ "$patfirst" = 1 ] && [ "$seen_pat" = 0 ] && [ ${#ops[@]} -gt 0 ]; then
    ops=("${ops[@]:1}")
  fi
  # cp: the destination is a write, not a read (cp .env.example .env is fine).
  if [ "$prog" = "cp" ] && [ "$tdir" = 0 ] && [ ${#ops[@]} -gt 0 ]; then
    ops=("${ops[@]:0:${#ops[@]}-1}")
  fi
  # source / . : only the sourced file.
  case "$prog" in source|.) [ ${#ops[@]} -gt 1 ] && ops=("${ops[0]}") ;; esac
  for t in ${ops[@]+"${ops[@]}"}; do
    if is_secret_path "$t"; then
      block "'$prog $t' reads or copies a secret file. $SECRET_READ_FIX"
    fi
  done
  return 0
}

# ---------------------------------------------------------------------------
# Segment walker
# ---------------------------------------------------------------------------
GB_DEPTH=0

# scan_words <word>...: check one already-split simple command.
scan_words() {
  local t="" prog="" n=0 joined=""
  while [ $# -gt 0 ]; do
    t="$1"
    case "$t" in
      [A-Za-z_]*=*|[A-Za-z_]*+=*)
        n="${t%%=*}"; n="${n%+}"
        case "$n" in
          *[!A-Za-z0-9_]*) break ;;
          *) check_assignment "$n" "${t#*=}"; shift; continue ;;
        esac ;;
      '!'|'{'|'}'|if|then|else|elif|do|while|until|fi|done|esac|coproc|builtin|nohup|noglob|time|-p)
        shift; continue ;;
      exec)
        shift
        while [ $# -gt 0 ]; do case "$1" in -a) shift 2 2>/dev/null || return 0 ;; -*) shift ;; *) break ;; esac; done
        continue ;;
      command)
        shift
        case "${1:-}" in -v|-V) return 0 ;; -p) shift ;; esac
        continue ;;
      env)
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            -S|--split-string) scan_command "${2:-}"; return 0 ;;
            -S?*) scan_command "${1#-S}"; return 0 ;;
            --split-string=*) scan_command "${1#--split-string=}"; return 0 ;;
            -u|--unset|-C|--chdir) shift 2 2>/dev/null || return 0 ;;
            --) shift; break ;;
            -*) shift ;;
            *) break ;;
          esac
        done
        continue ;;
      nice)
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            -n|--adjustment) shift 2 2>/dev/null || return 0 ;;
            --) shift; break ;;
            -*) shift ;;
            *) break ;;
          esac
        done
        continue ;;
      timeout)
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            -s|--signal|-k|--kill-after) shift 2 2>/dev/null || return 0 ;;
            --) shift; break ;;
            -*) shift ;;
            *) break ;;
          esac
        done
        [ $# -gt 0 ] && shift   # DURATION
        continue ;;
      stdbuf|ionice|taskpolicy|setsid|chrt)
        case "$t" in
          stdbuf) GB_VOPTS=" -i -o -e " ;;
          ionice) GB_VOPTS=" -c -n -p -P -u --class --classdata --pid --pgid --uid " ;;
          taskpolicy) GB_VOPTS=" -c -t -l -d -g -p " ;;
          chrt) GB_VOPTS=" -T -P -D --sched-runtime --sched-period --sched-deadline " ;;
          *) GB_VOPTS=" " ;;
        esac
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            --) shift; break ;;
            -*)
              case "$GB_VOPTS" in
                *" $1 "*) shift 2 2>/dev/null || return 0 ;;
                *) shift ;;
              esac ;;
            *) break ;;
          esac
        done
        [ "$t" = "chrt" ] && [ $# -gt 0 ] && shift   # priority
        continue ;;
      xargs)
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            -a|-d|-E|-I|-L|-n|-P|-s|--arg-file|--delimiter|--max-args|--max-procs|--max-lines|--max-chars|--process-slot-var) shift 2 2>/dev/null || return 0 ;;
            --) shift; break ;;
            -*) shift ;;
            *) break ;;
          esac
        done
        continue ;;
      flock)
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            -c|--command) scan_command "${2:-}"; return 0 ;;
            -w|--wait|--timeout|-E|--conflict-exit-code) shift 2 2>/dev/null || return 0 ;;
            -*) shift ;;
            *) shift; break ;;   # the lock file
          esac
        done
        continue ;;
      watch)
        shift
        while [ $# -gt 0 ]; do
          case "$1" in
            -n|--interval|-q|--equexit) shift 2 2>/dev/null || return 0 ;;
            -*) shift ;;
            *) break ;;
          esac
        done
        [ $# -gt 0 ] && scan_command "$*"
        return 0 ;;
      pixi)
        GB_PIXI_SUB=""
        GB_I=2
        while [ "$GB_I" -le $# ]; do
          case "${!GB_I}" in
            --color|--manifest-path) GB_I=$((GB_I + 2)) ;;
            -*) GB_I=$((GB_I + 1)) ;;
            *) GB_PIXI_SUB="${!GB_I}"; break ;;
          esac
        done
        case "$GB_PIXI_SUB" in
          run|exec|x)
            shift "$GB_I"
            while [ $# -gt 0 ]; do
              case "$1" in
                -e|--environment|--manifest-path|-m|--color|--concurrent-solves|--concurrent-downloads|--tls-root-certs|--auth-file|--pypi-keyring-provider|-s|--spec|-c|--channel|-p|--platform|-w|--with|--with-dependency|--workspace|--feature)
                  shift 2 2>/dev/null || return 0 ;;
                --) shift; break ;;
                -*) shift ;;
                *) break ;;
              esac
            done
            if [ $# -eq 1 ]; then
              case "$1" in *[[:space:]]*) scan_command "$1"; return 0 ;; esac
            fi
            continue ;;
        esac
        break ;;
      *) break ;;
    esac
  done
  [ $# -gt 0 ] || return 0

  # A command substitution inside the program word is assumed to expand to
  # nothing (the lexer leaves the marker `$(...)`), so `"$(:)sudo"` and
  # `su$(:)do` are still checked as `sudo`.
  prog="${1//'$(...)'/}"
  prog="${prog##*/}"
  shift
  case "$prog" in
    sudo|doas|su|pkexec|run0|sudoedit)
      block "'$prog' (privilege escalation) is never run by the studio. Report what needs root and the exact command; the user runs it." ;;
    sysctl) check_sysctl "$@" ;;
    pacman|yay|paru) check_pacman_like "$prog" "$@" ;;
    apt|apt-get|aptitude|dnf|yum|zypper|brew|port|snap|flatpak|conda|mamba|micromamba|pipx)
      check_subcommand_pkg "$prog" "$@" ;;
    pip|pip3|pip3.*) check_pip "$prog" "$@" ;;
    python|python3|python3.*) check_python "$prog" "$@" ;;
    uv) check_uv "$@" ;;
    npm|pnpm|yarn) check_js_pkg "$prog" "$@" ;;
    cargo) check_cargo "$@" ;;
    pixi) check_pixi "$@" ;;
    docker|podman) check_docker "$prog" "$@" ;;
    cmake) check_cmake "$@" ;;
    ninja|make|gmake) check_build_tool "$prog" "$@" ;;
    makepkg) check_makepkg "$@" ;;
    git) check_git "$@" ;;
    gh) check_gh "$@" ;;
    setarch) check_setarch "$@" ;;
    export|declare|typeset|readonly|local) check_export_like "$@" ;;
    bash|sh|zsh|dash|ksh|mksh|ash) check_shell_c "$@" ;;
    eval) [ $# -gt 0 ] && scan_command "$*" ;;
    script) check_script_c "$@" ;;
    cat|tac|nl|bat|less|more|head|tail|grep|egrep|fgrep|rg|sed|awk|gawk|mawk|cp|base64|xxd|od|strings|source|.)
      check_secret_read "$prog" "$@" ;;
  esac
  return 0
}

# check_segment <token>...: drop redirections, run the anywhere-checks, walk.
check_segment() {
  local t="" skip=0
  local -a w=()
  local op=""
  for t in "$@"; do
    if [ "$skip" = 1 ]; then
      skip=0
      case "$op" in
        "$GB_OP<<<"*) : ;;
        "$GB_OP"*'<'*)
          is_secret_path "$t" && block "input redirection from '$t' reads a secret file. $SECRET_READ_FIX" ;;
      esac
      continue
    fi
    case "$t" in
      "$GB_OP"*) skip=1; op="$t"; continue ;;
    esac
    w+=("$t")
  done
  [ ${#w[@]} -gt 0 ] || return 0
  check_global_tokens "${w[@]}"
  scan_words "${w[@]}"
  return 0
}

# scan_command <string>: lex and check every segment (recursion-limited).
scan_command() {
  local str="${1:-}" t=""
  local -a toks=()
  [ -n "$str" ] || return 0
  GB_DEPTH=$((GB_DEPTH + 1))
  if [ "$GB_DEPTH" -gt 4 ]; then
    GB_DEPTH=$((GB_DEPTH - 1))
    return 0
  fi
  while IFS= read -r -d "$GB_TOK" t; do
    if [ "$t" = "$GB_SEG" ]; then
      [ ${#toks[@]} -gt 0 ] && check_segment "${toks[@]}"
      toks=()
    else
      toks+=("$t")
    fi
  done < <(gb_lex "$str")
  [ ${#toks[@]} -gt 0 ] && check_segment "${toks[@]}"
  GB_DEPTH=$((GB_DEPTH - 1))
  return 0
}

scan_command "$cmd"
exit 0
