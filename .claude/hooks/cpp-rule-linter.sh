#!/usr/bin/env bash
# PostToolUse (matcher: Edit|Write): ADVISORY grep for this studio's C++/CMake
# rules that clang-tidy and the compiler do not catch (or catch too late).
#
# WARN-ONLY: it always exits 0 and never blocks. Findings go back to Claude as
# `hookSpecificOutput.additionalContext`, each naming the rule and the skill
# that holds it. It is a fast heuristic, not a parser: code rules run on a
# copy of the file with comments and string/char literals blanked (line
# numbers preserved); multi-line raw strings are not understood, so a false
# positive now and then is expected. The authoritative checks are the build
# (-Werror), `pixi run lint`, and the reviewers.
#
# Scope: own code only (src/, include/, tests/), CMake files (CMakeLists.txt,
# *.cmake) and pixi.toml. Never .claude/, build/, .pixi/, .platform-runs/.
set -u
trap 'exit 0' EXIT

HOOKS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd) || exit 0
# shellcheck source=./lib.sh
. "$HOOKS_DIR/lib.sh" 2>/dev/null || exit 0

HOOK_INPUT=$(cat 2>/dev/null) || HOOK_INPUT=""
hook_json_field tool_input.file_path
path="$HOOK_JSON_VALUE"
[ -n "$path" ] || exit 0
hook_json_field cwd
cwd="${HOOK_JSON_VALUE:-${PWD:-.}}"
abs=$(abs_path "$path" "$cwd")
[ -f "$abs" ] || exit 0
root=$(repo_root_for "$abs")
rel=$(rel_to "$root" "$abs")
base="${rel##*/}"

case "$rel" in
  /*|.claude/*|build/*|.pixi/*|.platform-runs/*) exit 0 ;;
esac

kind=""
case "$rel" in
  src/*|include/*|tests/*) is_cpp "$rel" && kind="cpp" ;;
esac
case "$base" in
  CMakeLists.txt|*.cmake) kind="cmake" ;;
esac
[ "$rel" = "pixi.toml" ] && kind="pixi"
[ -n "$kind" ] || exit 0

tmp=$(mktemp -d 2>/dev/null) || exit 0
trap 'rm -rf "$tmp"; exit 0' EXIT

FINDINGS=""
NFIND=0
# rule <file-to-grep> <ERE> <message> [grep -v ERE to drop]
rule() {
  local f="$1" re="$2" msg="$3" not="${4:-}" lines=""
  if [ -n "$not" ]; then
    lines=$(grep -nE -- "$re" "$f" 2>/dev/null | grep -vE -- "$not" | cut -d: -f1 | head -n 4 | tr '\n' ',')
  else
    lines=$(grep -nE -- "$re" "$f" 2>/dev/null | cut -d: -f1 | head -n 4 | tr '\n' ',')
  fi
  lines="${lines%,}"
  [ -n "$lines" ] || return 0
  NFIND=$((NFIND + 1))
  FINDINGS="${FINDINGS}- line ${lines//,/, }: ${msg}"$'\n'
}

# Code view: comments and string/char literals blanked, line count preserved.
make_cpp_view() {
  awk '
  {
    line = $0; out = ""; i = 1; n = length(line)
    while (i <= n) {
      c = substr(line, i, 1); c2 = substr(line, i, 2)
      if (inblock) { if (c2 == "*/") { inblock = 0; i += 2; continue }; i++; continue }
      if (c2 == "/*") { inblock = 1; out = out " "; i += 2; continue }
      if (c2 == "//") break
      if (c == "\"") {
        out = out "\"\""; i++
        while (i <= n) { c = substr(line, i, 1); if (c == "\\") { i += 2; continue }; i++; if (c == "\"") break }
        continue
      }
      if (c == "\047") {
        if (i > 1 && substr(line, i - 1, 1) ~ /[0-9A-Fa-f]/) { i++; continue }   # digit separator
        out = out "\047\047"; i++
        while (i <= n) { c = substr(line, i, 1); if (c == "\\") { i += 2; continue }; i++; if (c == "\047") break }
        continue
      }
      out = out c; i++
    }
    print out
  }' "$1" > "$2" 2>/dev/null
}

# CMake / TOML view: `#` comments blanked (outside double quotes).
make_hash_view() {
  awk '
  {
    line = $0; out = ""; q = 0; n = length(line)
    for (i = 1; i <= n; i++) {
      c = substr(line, i, 1)
      if (c == "\"" && substr(line, i - 1, 1) != "\\") q = !q
      if (c == "#" && !q) break
      out = out c
    }
    print out
  }' "$1" > "$2" 2>/dev/null
}

lint_cpp() {
  local v="$tmp/view"
  make_cpp_view "$abs" "$v" || return 0
  [ -s "$v" ] || return 0

  case "$rel" in
    src/app/main.cpp) : ;;
    *)
      rule "$v" '(^|[^A-Za-z0-9_.>:])(::)?(exit|_Exit|quick_exit|abort)[[:space:]]*\(|std::(exit|_Exit|quick_exit|abort)[[:space:]]*\(' \
        "exit()/std::exit/quick_exit/abort outside src/app/main.cpp. Library and command code returns a Result/exit code; only main() returns it (never std::exit, which skips destructors and the stdout flush check). See cli-design." ;;
  esac
  rule "$v" '(^|[^A-Za-z0-9_.>])(std::|::)?(system|popen)[[:space:]]*\(' \
    "system()/popen() runs a shell (injection, PATH and signal hazards). Use posix_spawn/execve with an argv vector, or drop the subprocess. See cpp-security."
  rule "$v" '(^|[^A-Za-z0-9_])(tmpnam|tmpnam_r|tempnam|mktemp)[[:space:]]*\(' \
    "tmpnam/tempnam/mktemp() are racy (TOCTOU). Use mkstemp in the destination directory, or std::filesystem with exclusive create. See cpp-security."
  rule "$v" '(^|[^A-Za-z0-9_])new[[:space:]]+[A-Za-z_:(]' \
    "bare 'new': use std::make_unique / std::make_shared / a container; ownership is never a raw pointer. See modern-cpp-conventions." \
    'operator[[:space:]]*new'
  rule "$v" '(^|[^A-Za-z0-9_=])delete([[:space:]]*\[[[:space:]]*\])?[[:space:]]+[A-Za-z_(*:]' \
    "bare 'delete': ownership belongs to RAII types (unique_ptr, containers). See modern-cpp-conventions." \
    '=[[:space:]]*delete|operator[[:space:]]*delete'
  if is_cpp_header "$rel"; then
    rule "$v" '^[[:space:]]*using[[:space:]]+namespace[[:space:]]' \
      "'using namespace' in a header leaks into every includer. Qualify names, or use a namespace alias inside a function. See modern-cpp-conventions."
  fi
  rule "$v" '(^|[^A-Za-z0-9_])endl([^A-Za-z0-9_]|$)' \
    "std::endl flushes on every line. Write '\\n'; flushing is done once by finish_stdout(). See cli-design."
  rule "$v" 'sync_with_stdio[[:space:]]*\([[:space:]]*(false|0)[[:space:]]*\)' \
    "sync_with_stdio(false) breaks the interleaving of std::print/stdio and iostreams this tool relies on. Never use it. See cli-design."
  rule "$v" 'setlocale[[:space:]]*\([[:space:]]*LC_ALL|locale::global' \
    "setlocale(LC_ALL, ...)/std::locale::global changes number formatting process-wide. Output stays in the C locale; use from_chars/to_chars. See cli-design."
  rule "$v" '_GLIBCXX_DEBUG' \
    "_GLIBCXX_DEBUG changes the libstdc++ ABI (ODR violations against prebuilt deps). Hardened builds use _GLIBCXX_ASSERTIONS. See memory-safety."
  rule "$abs" 'NOLINT' \
    "NOLINT must be '// NOLINTNEXTLINE(check-name): reason' (or NOLINT(check-name): reason) - a named check and a written reason; never a blanket NOLINT. See cpp-quality." \
    'NOLINT(NEXTLINE)?\([^)]+\):[[:space:]]*[^[:space:]]'
  rule "$v" 'fclose[[:space:]]*\([[:space:]]*(stdout|stderr)' \
    "never fclose(stdout/stderr): main's finish_stdout() flushes and checks stdout, and stderr must stay open for last-resort messages. See cli-design."
  rule "$v" '(^|[^A-Za-z0-9_])(vformat|runtime_format)[[:space:]]*\(' \
    "vformat/runtime_format with a runtime format string defeats compile-time format checking (and is a format-injection risk with user data). Use std::format/std::print with a literal format. See cli-design."
  case "$rel" in
    src/app/*.cpp) : ;;
    *)
      rule "$abs" '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]CLI/' \
        "CLI11 headers are included only in src/app/*.cpp. Headers forward-declare 'namespace CLI { class App; }'; core code never sees CLI11. See cli-design." ;;
  esac
  rule "$abs" '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]catch2/catch_all\.hpp' \
    "catch_all.hpp compiles all of Catch2 into every test TU. Include the per-feature headers (catch2/catch_test_macros.hpp, ...). See cpp-testing."
  rule "$v" 'CLI11_PARSE' \
    "CLI11_PARSE returns CLI11's internal codes (100-114) from main. Parse inside run() and map ParseError via app.exit(e, out, err) -> exit_code::usage. See cli-design."
  rule "$v" 'std::string_view[[:space:]]+[A-Za-z_][A-Za-z0-9_]*_[[:space:]]*(;|\{|=)' \
    "a std::string_view data member dangles as soon as the viewed string dies. Store std::string (or document the owner's lifetime explicitly). See memory-safety."
}

lint_cmake() {
  local v="$tmp/view" blocks="" ln="" text=""
  make_hash_view "$abs" "$v" || return 0
  [ -s "$v" ] || return 0
  rule "$v" 'CMAKE_COMPILE_WARNING_AS_ERROR' \
    "CMAKE_COMPILE_WARNING_AS_ERROR is global (it reaches fetched deps and consumers). -Werror scope is owned by the <PROJ>_WARNINGS_AS_ERRORS option in cmake/ProjectWarnings.cmake, set by the presets. See cpp-quality."
  rule "$v" 'add_compile_options[[:space:]]*\([^)]*-W' \
    "warning flags in add_compile_options() are directory-wide. Warnings live on the <proj>_warnings interface target via <proj>_apply_warnings(tgt). See cpp-quality."
  rule "$v" '(^|[^A-Za-z0-9_])include_directories[[:space:]]*\(' \
    "include_directories() is directory-scoped. Use target_include_directories(<target> PUBLIC|PRIVATE ...). See build-workflow."
  rule "$v" 'CMAKE_BUILD_PARALLEL_LEVEL' \
    "CMAKE_BUILD_PARALLEL_LEVEL overrides the per-worktree job cap (CLI -j > env > preset jobs). Job caps live in CMakeUserPresets.json <preset>-local presets. See build-workflow."
  rule "$v" 'TYPE[[:space:]]+MAN([^A-Za-z0-9_]|$)' \
    "install(... TYPE MAN) installs into share/man without the man1/ section dir. Use DESTINATION \${CMAKE_INSTALL_MANDIR}/man1 COMPONENT docs. See build-workflow."
  if grep -inE 'INSTALL_RPATH_USE_LINK_PATH[[:space:]"]+(ON|TRUE|YES|Y|1)([^A-Za-z0-9_]|$)' "$v" >/dev/null 2>&1; then
    ln=$(grep -inE 'INSTALL_RPATH_USE_LINK_PATH[[:space:]"]+(ON|TRUE|YES|Y|1)([^A-Za-z0-9_]|$)' "$v" | cut -d: -f1 | head -n 4 | tr '\n' ',')
    NFIND=$((NFIND + 1))
    FINDINGS="${FINDINGS}- line ${ln%,}: INSTALL_RPATH_USE_LINK_PATH ON bakes the build machine's library paths into installed binaries. Dev installs use the explicit env RPATH from the presets; release ships with no RPATH. See build-workflow."$'\n'
  fi
  case "$rel" in
    tests/e2e/*)
      rule "$v" 'PASS_REGULAR_EXPRESSION|WILL_FAIL' \
        "PASS_REGULAR_EXPRESSION ignores the exit code and WILL_FAIL only inverts it. E2E cases assert the exact exit code and goldens through cli_e2e_case / run_case.cmake. See cpp-testing." ;;
  esac
  # FetchContent_Declare blocks: need URL_HASH or a 40-hex GIT_TAG.
  blocks=$(awk '
    /FetchContent_Declare[[:space:]]*\(/ { inb = 1; start = NR; depth = 0; text = "" }
    inb {
      text = text " " $0
      for (i = 1; i <= length($0); i++) {
        c = substr($0, i, 1)
        if (c == "(") depth++
        else if (c == ")") { depth--; if (depth == 0) { inb = 0; break } }
      }
      if (!inb) { gsub(/\t/, " ", text); print start "\t" text }
    }' "$v" 2>/dev/null)
  if [ -n "$blocks" ]; then
    while IFS=$'\t' read -r ln text; do
      [ -n "$ln" ] || continue
      if ! printf '%s\n' "$text" | grep -qE 'URL_HASH|GIT_TAG[[:space:]]+"?[0-9a-fA-F]{40}"?([^0-9a-fA-F]|$)'; then
        NFIND=$((NFIND + 1))
        FINDINGS="${FINDINGS}- line ${ln}: FetchContent_Declare without URL_HASH or a full 40-hex GIT_TAG commit is not reproducible (tags and branches move). Prefer the pixi package; otherwise pin by hash. See build-workflow."$'\n'
      fi
    done <<< "$blocks"
  fi
}

lint_pixi() {
  local v="$tmp/view"
  make_hash_view "$abs" "$v" || return 0
  rule "$v" '"?(CMAKE_BUILD|CTEST)_PARALLEL_LEVEL"?[[:space:]]*=' \
    "CMAKE_BUILD_PARALLEL_LEVEL/CTEST_PARALLEL_LEVEL in a task env or activation env silently override the per-worktree job caps. Remove them; caps come from CMakeUserPresets.json <preset>-local presets. See build-workflow."
}

case "$kind" in
  cpp) lint_cpp ;;
  cmake) lint_cmake ;;
  pixi) lint_pixi ;;
esac

[ "$NFIND" -gt 0 ] || exit 0
msg="cpp-rule-linter (advisory, not blocking) on $rel - fix these or say why each is a false positive:"$'\n'"$(printf '%s' "$FINDINGS" | head -n 20)"
emit_context "$msg"
exit 0
