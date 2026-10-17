# tests/e2e/run_case.cmake: the E2E case driver. Runs ONE case against the INSTALLED binary.
#
#   cmake -DCASE_SPEC=<build>/e2e-cases/<suite>.<case>.cmake -P tests/e2e/run_case.cmake
#
# Registered by cli_e2e_case() (CliE2E.cmake); never invoked by hand except to debug one case. The
# spec file holds the case's arguments and expectations. Rules (cpp-testing skill):
# - Hermetic env via set(ENV{}) / unset(ENV{}). execute_process(ENVIRONMENT) is CMake 4.4+ only.
#   Fresh HOME/XDG_*/TMPDIR under <build>/e2e-work/<suite>.<case>/, LC_ALL=C.UTF-8,
#   TERM=xterm-256color, and CONDA_*/PIXI_*/CMAKE_*/CTEST_*/LC_*/LANG*/XDG_*/color variables and the
#   tool's own MANGADEX_DL_* variables removed. *SAN_OPTIONS, ASAN_SYMBOLIZER_PATH, LLVM_PROFILE_FILE
#   (the cov task) and PATH (finds llvm-symbolizer) are KEPT.
# - The exit code is asserted exactly; stdout and stderr are asserted separately.
# - Any sanitizer marker on stderr fails the case WHATEVER the exit code.
# - Signal deaths come back as strings ("SIGPIPE", "Segmentation fault", "Subprocess aborted"), per
#   command in RESULTS_VARIABLE. A `sh -c` wrapper always `exec`s, so it never masks them.
# - Goldens are compared byte for byte. They are written ONLY when UPDATE_GOLDEN=1, which only
#   scripts/bless.sh sets, and only after every other assertion passed.
# - CLI_E2E_TOOL (env) overrides the binary under test (a release install, an extracted package, /usr/bin).
# - TTY cases run under util-linux `script`. Without it the case exits 77, which CTest reports as skipped.

cmake_minimum_required(VERSION 3.31...4.4)

if(NOT DEFINED CASE_SPEC OR NOT EXISTS "${CASE_SPEC}")
  message(FATAL_ERROR "run_case.cmake: pass -DCASE_SPEC=<generated spec file> (got '${CASE_SPEC}')")
endif()
include("${CASE_SPEC}")

set(_marker_re "ERROR: (Address|Leak|Memory)Sanitizer|WARNING: ThreadSanitizer|runtime error:|AddressSanitizer:DEADLYSIGNAL")
string(ASCII 27 _esc)
string(ASCII 255 254 _invalid_utf8)

# ---- 0. read the controls BEFORE the env is scrubbed --------------------------------------------
set(_update OFF)
if("$ENV{UPDATE_GOLDEN}" STREQUAL "1")
  set(_update ON)
endif()
set(_tool "${CASE_DEFAULT_TOOL}")
if(DEFINED ENV{CLI_E2E_TOOL} AND NOT "$ENV{CLI_E2E_TOOL}" STREQUAL "")
  set(_tool "$ENV{CLI_E2E_TOOL}")
endif()
if(NOT EXISTS "${_tool}")
  message(FATAL_ERROR "${CASE_TEST}: binary under test not found: ${_tool}\n"
                      "The e2e.install fixture should have installed it; run the whole `pixi run test`.")
endif()

function(_expand out value)
  string(REPLACE "{{INPUTS}}" "${CASE_INPUTS_DIR}" value "${value}")
  string(REPLACE "{{WORK}}" "${CASE_WORK_DIR}" value "${value}")
  string(REPLACE "{{INVALID_UTF8}}" "${_invalid_utf8}" value "${value}")
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

# ---- 1. hermetic environment -------------------------------------------------------------------
file(REMOVE_RECURSE "${CASE_WORK_DIR}")
foreach(_d IN ITEMS home tmp config cache data state)
  file(MAKE_DIRECTORY "${CASE_WORK_DIR}/${_d}")
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E environment OUTPUT_VARIABLE _env_dump)
string(REGEX MATCHALL "(^|\n)[A-Za-z_][A-Za-z0-9_]*=" _env_names "${_env_dump}")
foreach(_n IN LISTS _env_names)
  string(REGEX REPLACE "^\n?([A-Za-z_][A-Za-z0-9_]*)=$" "\\1" _n "${_n}")
  if(_n MATCHES "^(CONDA_|PIXI_|CMAKE_|CTEST_|LC_|LANG|XDG_|CLI_E2E_|${CASE_PROJ_UPPER}_)"
     OR _n MATCHES "^(UPDATE_GOLDEN|NO_COLOR|CLICOLOR|CLICOLOR_FORCE|COLORTERM|FORCE_COLOR|TERM|COLUMNS|LINES|PAGER|EDITOR|VISUAL)$")
    unset(ENV{${_n}})
  endif()
endforeach()

set(ENV{HOME} "${CASE_WORK_DIR}/home")
set(ENV{TMPDIR} "${CASE_WORK_DIR}/tmp")
set(ENV{XDG_CONFIG_HOME} "${CASE_WORK_DIR}/config")
set(ENV{XDG_CACHE_HOME} "${CASE_WORK_DIR}/cache")
set(ENV{XDG_DATA_HOME} "${CASE_WORK_DIR}/data")
set(ENV{XDG_STATE_HOME} "${CASE_WORK_DIR}/state")
set(ENV{LC_ALL} "C.UTF-8")
set(ENV{LANG} "C.UTF-8")
set(ENV{TERM} "xterm-256color")

foreach(_item IN LISTS CASE_ENV)
  _expand(_item "${_item}")
  if(_item MATCHES "^-([A-Za-z_][A-Za-z0-9_]*)$")
    unset(ENV{${CMAKE_MATCH_1}})
  elseif(_item MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
    set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
  else()
    message(FATAL_ERROR "${CASE_TEST}: ENV item '${_item}' must be NAME=value or -NAME")
  endif()
endforeach()

# ---- 2. the command ------------------------------------------------------------------------------
set(_argv "${_tool}")
foreach(_a IN LISTS CASE_ARGS)
  _expand(_a "${_a}")
  list(APPEND _argv "${_a}")
endforeach()

set(_input_args INPUT_FILE /dev/null)
set(_wrap_pre "")       # shell prologue for the `sh -c '<pre>exec "$0" "$@"<post>'` wrapper
                        # (no ';' anywhere in it: a ';' would split the CMake list that holds the command)
set(_wrap_post "")
if(CASE_STDIN STREQUAL "NULL")
  # default: /dev/null
elseif(CASE_STDIN STREQUAL "CLOSED")
  set(_input_args "")
  set(_wrap_post " <&-")
else()
  _expand(_stdin_file "${CASE_STDIN}")
  if(NOT IS_ABSOLUTE "${_stdin_file}")
    set(_stdin_file "${CASE_INPUTS_DIR}/${_stdin_file}")
  endif()
  if(NOT EXISTS "${_stdin_file}")
    message(FATAL_ERROR "${CASE_TEST}: STDIN file not found: ${_stdin_file}")
  endif()
  set(_input_args INPUT_FILE "${_stdin_file}")
endif()
if(CASE_PARENT_IGNORES_SIGPIPE)
  set(_wrap_pre "trap '' PIPE\n")
endif()

set(_cmd ${_argv})
if(NOT _wrap_pre STREQUAL "" OR NOT _wrap_post STREQUAL "")
  # `exec` replaces the shell, so the tool's own exit status or signal reaches RESULTS_VARIABLE.
  set(_cmd sh -c "${_wrap_pre}exec \"$0\" \"$@\"${_wrap_post}" ${_argv})
endif()

set(_out "")
set(_err "")
set(_rc "")
set(_head_rc "0")
if(CASE_TTY)
  find_program(_script NAMES script)
  set(_script_ok FALSE)
  if(_script)
    execute_process(COMMAND "${_script}" --version OUTPUT_VARIABLE _sv ERROR_VARIABLE _sv RESULT_VARIABLE _svrc)
    if(_svrc STREQUAL "0" AND _sv MATCHES "util-linux")
      set(_script_ok TRUE)
    endif()
  endif()
  if(NOT _script_ok)
    message(STATUS "${CASE_TEST}: SKIPPED: util-linux script(1) not found; the pseudo-TTY case cannot run here")
    cmake_language(EXIT 77)
  endif()
  # Build a shell-quoted command line for `script -c`.
  set(_line "")
  foreach(_a IN LISTS _cmd)
    string(REPLACE "'" "'\\''" _q "${_a}")
    string(APPEND _line " '${_q}'")
  endforeach()
  execute_process(COMMAND "${_script}" -qec "${_line}" /dev/null
                  INPUT_FILE /dev/null
                  OUTPUT_VARIABLE _out ERROR_VARIABLE _script_err RESULT_VARIABLE _rc
                  TIMEOUT ${CASE_TIMEOUT})
  string(REPLACE "\r" "" _out "${_out}")
  # Under a pseudo-terminal, stdout and stderr share the terminal: both land in _out, and _err holds
  # only script(1)'s own diagnostics. EXPECT_STDERR_* is rejected for TTY cases at registration.
  set(_err "${_script_err}")
elseif(DEFINED CASE_PIPE_HEAD)
  execute_process(COMMAND ${_cmd}
                  COMMAND head -n "${CASE_PIPE_HEAD}"
                  ${_input_args}
                  OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULTS_VARIABLE _results
                  TIMEOUT ${CASE_TIMEOUT})
  list(GET _results 0 _rc)
  list(GET _results 1 _head_rc)
elseif(DEFINED CASE_STDOUT_FILE)
  execute_process(COMMAND ${_cmd} ${_input_args}
                  OUTPUT_FILE "${CASE_STDOUT_FILE}" ERROR_VARIABLE _err RESULT_VARIABLE _rc
                  TIMEOUT ${CASE_TIMEOUT})
else()
  execute_process(COMMAND ${_cmd} ${_input_args}
                  OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
                  TIMEOUT ${CASE_TIMEOUT})
endif()

# ---- 3. assertions (collect all failures, report once) -------------------------------------------
set(_fail "")   # a plain string (not a list), so messages may contain ';'
macro(_fail_with msg)
  string(APPEND _fail "\n  - ${msg}")
endmacro()

function(_explain_exit out code)
  set(_why "")
  if(code STREQUAL "86")
    set(_why " (86 = the reserved sanitizer exit code: a sanitizer finding)")
  elseif(code STREQUAL "70")
    set(_why " (70 = internal error: an exception reached main)")
  elseif(code MATCHES "^(134|139)$" OR code MATCHES "Segmentation fault|Subprocess aborted|SIG")
    set(_why " (a crash or signal death)")
  elseif(code MATCHES "timeout")
    set(_why " (the case exceeded TIMEOUT ${CASE_TIMEOUT}s)")
  elseif(code MATCHES "^(10[0-9]|11[0-4])$")
    set(_why " (100-114: a raw CLI11 parse code leaked; map ParseError to 2)")
  endif()
  set(${out} "${_why}" PARENT_SCOPE)
endfunction()

if(DEFINED CASE_PIPE_HEAD)
  if(NOT "${_rc}" STREQUAL "${CASE_EXPECT_PIPE_RESULT}")
    _explain_exit(_why "${_rc}")
    _fail_with("writer result in the pipe: expected '${CASE_EXPECT_PIPE_RESULT}', got '${_rc}'${_why}")
  endif()
  if(NOT "${_head_rc}" STREQUAL "0")
    _fail_with("`head -n ${CASE_PIPE_HEAD}` exited '${_head_rc}' (expected 0)")
  endif()
else()
  if(NOT "${_rc}" STREQUAL "${CASE_EXPECT_EXIT}")
    _explain_exit(_why "${_rc}")
    _fail_with("exit: expected ${CASE_EXPECT_EXIT}, got '${_rc}'${_why}")
  endif()
endif()

set(_scan "${_err}")
if(CASE_TTY)
  set(_scan "${_out}\n${_err}")
endif()
if(_scan MATCHES "${_marker_re}")
  _fail_with("sanitizer marker on stderr ('${CMAKE_MATCH_0}'): a sanitizer finding fails the case whatever the exit code")
endif()

if(CASE_EXPECT_STDOUT_EMPTY AND NOT _out STREQUAL "")
  _fail_with("stdout: expected empty")
endif()
if(DEFINED CASE_EXPECT_STDOUT_REGEX AND NOT _out MATCHES "${CASE_EXPECT_STDOUT_REGEX}")
  _fail_with("stdout: does not match regex '${CASE_EXPECT_STDOUT_REGEX}'")
endif()
if(CASE_EXPECT_STDERR_EMPTY AND NOT _err STREQUAL "")
  _fail_with("stderr: expected empty")
endif()
if(DEFINED CASE_EXPECT_STDERR_REGEX AND NOT _err MATCHES "${CASE_EXPECT_STDERR_REGEX}")
  _fail_with("stderr: does not match regex '${CASE_EXPECT_STDERR_REGEX}'")
endif()

if(CASE_EXPECT_JSON)
  # Wrapping in [...] and requiring length 1 rejects empty output, trailing junk and concatenated docs.
  string(JSON _n ERROR_VARIABLE _jerr LENGTH "[${_out}]")
  if(_jerr OR NOT _n EQUAL 1)
    _fail_with("stdout: not exactly one valid JSON document (${_jerr})")
  endif()
endif()
if(CASE_EXPECT_JSON_LINES)
  set(_rest "${_out}")
  set(_lineno 0)
  set(_docs 0)
  while(NOT _rest STREQUAL "")
    string(FIND "${_rest}" "\n" _nl)
    if(_nl EQUAL -1)
      set(_line "${_rest}")
      set(_rest "")
    else()
      string(SUBSTRING "${_rest}" 0 ${_nl} _line)
      math(EXPR _next "${_nl} + 1")
      string(SUBSTRING "${_rest}" ${_next} -1 _rest)
    endif()
    math(EXPR _lineno "${_lineno} + 1")
    if(_line STREQUAL "")
      continue()
    endif()
    string(JSON _n ERROR_VARIABLE _jerr LENGTH "[${_line}]")
    if(_jerr OR NOT _n EQUAL 1)
      _fail_with("stdout line ${_lineno}: not exactly one valid JSON document")
      break()
    endif()
    math(EXPR _docs "${_docs} + 1")
  endwhile()
  if(_docs EQUAL 0)
    _fail_with("stdout: expected JSON Lines, got no documents")
  endif()
endif()

if(CASE_EXPECT_JSON OR CASE_EXPECT_JSON_LINES)
  # JSON text must be UTF-8 (RFC 8259) and CMake's JSON parser does not check that. Linux argv and paths
  # are bytes, so this catches a dump() without error_handler_t::replace.
  find_program(_iconv NAMES iconv)
  if(_iconv)
    file(WRITE "${CASE_WORK_DIR}/stdout.json" "${_out}")
    execute_process(COMMAND "${_iconv}" -f UTF-8 -t UTF-8 "${CASE_WORK_DIR}/stdout.json"
                    OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE _iconv_rc)
    if(NOT _iconv_rc STREQUAL "0")
      _fail_with("stdout: the JSON is not valid UTF-8 (iconv). Dump with nlohmann::json::error_handler_t::replace")
    endif()
  else()
    message(STATUS "${CASE_TEST}: note: iconv not found, UTF-8 validity of the JSON not checked")
  endif()
endif()

string(FIND "${_out}" "${_esc}[" _ansi_out)
string(FIND "${_err}" "${_esc}[" _ansi_err)
if(CASE_EXPECT_NO_ANSI AND (NOT _ansi_out EQUAL -1 OR NOT _ansi_err EQUAL -1))
  _fail_with("ANSI escape sequence found in the output (expected none)")
endif()
if(CASE_EXPECT_ANSI AND _ansi_out EQUAL -1)
  _fail_with("no ANSI escape sequence on stdout (expected color)")
endif()

# ---- 4. goldens (last: never bless a failing case) ------------------------------------------------
if(CASE_GOLDEN)
  set(_gbase "${CASE_GOLDEN_DIR}/${CASE_SUITE}.${CASE_NAME}")
  string(REPLACE "." "\\." _bless_re "^${CASE_TEST}$")
  set(_norm_out "${_out}")
  set(_norm_err "${_err}")
  foreach(_v IN ITEMS _norm_out _norm_err)
    string(REPLACE "${CASE_WORK_DIR}" "{{WORK}}" ${_v} "${${_v}}")
    string(REPLACE "${CASE_INPUTS_DIR}" "{{INPUTS}}" ${_v} "${${_v}}")
    string(REPLACE "${_tool}" "{{TOOL}}" ${_v} "${${_v}}")
  endforeach()

  if(_update)
    if(NOT _fail STREQUAL "")
      _fail_with("UPDATE_GOLDEN=1 but the case fails other assertions: goldens NOT written")
    else()
      file(WRITE "${_gbase}.stdout" "${_norm_out}")
      if(_norm_err STREQUAL "")
        file(REMOVE "${_gbase}.stderr")
      else()
        file(WRITE "${_gbase}.stderr" "${_norm_err}")
      endif()
      message(STATUS "${CASE_TEST}: golden written: ${_gbase}.stdout")
    endif()
  else()
    find_program(_diff NAMES diff)
    foreach(_stream IN ITEMS stdout stderr)
      if(_stream STREQUAL "stdout")
        set(_actual "${_norm_out}")
      else()
        set(_actual "${_norm_err}")
      endif()
      if(NOT EXISTS "${_gbase}.${_stream}")
        if(_stream STREQUAL "stdout")
          _fail_with("golden missing: ${_gbase}.stdout. Request a bless (never hand-write a golden): pixi run bless '${_bless_re}'")
        elseif(NOT _actual STREQUAL "")
          _fail_with("stderr is not empty and no golden ${_gbase}.stderr exists (request a bless if intended)")
        endif()
        continue()
      endif()
      file(READ "${_gbase}.${_stream}" _expected)
      if(NOT _expected STREQUAL _actual)
        file(WRITE "${CASE_WORK_DIR}/actual.${_stream}" "${_actual}")
        set(_d "")
        if(_diff)
          execute_process(COMMAND "${_diff}" -u "${_gbase}.${_stream}" "${CASE_WORK_DIR}/actual.${_stream}"
                          OUTPUT_VARIABLE _d ERROR_QUIET)
          string(SUBSTRING "${_d}" 0 4000 _d)
        endif()
        _fail_with("${_stream} differs from golden ${_gbase}.${_stream}\n${_d}\nIf the change is intended, request a bless: pixi run bless '${_bless_re}'")
      endif()
    endforeach()
  endif()
endif()

# ---- 5. verdict --------------------------------------------------------------------------------------
if(NOT _fail STREQUAL "")
  string(SUBSTRING "${_out}" 0 4000 _out_head)
  string(SUBSTRING "${_err}" 0 4000 _err_head)
  list(JOIN _cmd "' '" _shown)
  message(FATAL_ERROR "${CASE_TEST} FAILED${_fail}\n"
                      "command: '${_shown}'\n"
                      "exit: ${_rc}\n"
                      "--- stdout (first 4000 bytes) ---\n${_out_head}\n"
                      "--- stderr (first 4000 bytes) ---\n${_err_head}\n")
endif()
message(STATUS "${CASE_TEST}: ok")
