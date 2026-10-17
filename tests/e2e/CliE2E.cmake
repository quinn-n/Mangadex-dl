# tests/e2e/CliE2E.cmake: the installed-binary E2E harness. [serialization point]
#
# Two roles in one file:
#   1. Project mode (include()d by tests/CMakeLists.txt). Defines cli_e2e_add_install_fixture() and
#      cli_e2e_case(). Each case is registered as `cmake -P run_case.cmake` against the INSTALLED binary.
#   2. Script mode (`cmake -DCLI_E2E_MODE=<install|staged-files> ... -P CliE2E.cmake`). This is the
#      e2e.install fixture body and the release-check.staged-files body.
#
# cli_e2e_case(NAME <case> ARGS <arg>...
#              [STDIN <file>|NULL|CLOSED]                       default NULL (/dev/null); never ctest's stdin
#              [EXPECT_EXIT <n>]                                 default 0; always exact
#              [EXPECT_STDOUT_REGEX <re> | EXPECT_STDOUT_EMPTY]
#              [EXPECT_STDERR_REGEX <re> | EXPECT_STDERR_EMPTY]
#              [EXPECT_JSON | EXPECT_JSON_LINES]                 stdout is exactly one JSON document / one per line
#              [EXPECT_NO_ANSI | EXPECT_ANSI]                    ESC[ absent from / present in the output
#              [GOLDEN]                                          compare golden/<suite>.<case>.{stdout,stderr}
#              [ENV <NAME=value | -NAME>...]                     set / unset on top of the hermetic env
#              [TTY]                                             run under `script` (pseudo-terminal); skips (77) if absent
#              [STDOUT_FILE /dev/full]                           stdout to a file (write-error cases)
#              [PIPE_HEAD <n> [EXPECT_PIPE_RESULT <SIGPIPE|n>] [PARENT_IGNORES_SIGPIPE]]
#              [TIMEOUT <seconds>]                               default 60 (generous: ASan + nice)
#              [LABELS <label>...])
#
# Placeholders usable in ARGS, ENV values and STDIN: {{INPUTS}} (tests/e2e/inputs), {{WORK}} (the
# per-case temp dir, also $HOME), {{INVALID_UTF8}} (the bytes 0xFF 0xFE).
#
# The test name is e2e.<suite>.<case>, where <suite> is the case file's name (cases/hello.cmake ->
# "hello"; cases/_edge.cmake -> "edge"). Labels: e2e, plus edge for the edge suite, golden for GOLDEN,
# plus LABELS. Never use PASS_REGULAR_EXPRESSION or WILL_FAIL for contract cases: the first ignores
# the exit code, and the second only inverts it.

# =====================================================================================================
# Script mode
# =====================================================================================================
if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(CLI_E2E_MODE STREQUAL "install")
    # Fresh prefix every run, so a file removed from the install rules can't linger and pass.
    file(REMOVE_RECURSE "${CLI_E2E_PREFIX}")
    # Two calls: multi-component `--component a b` exists only in CMake 4.4+.
    foreach(_component IN ITEMS runtime docs)
      execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${CLI_E2E_BUILD_DIR}" --prefix "${CLI_E2E_PREFIX}"
                --component ${_component}
        RESULT_VARIABLE _rc)
      if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR "e2e.install: `cmake --install --component ${_component}` failed (${_rc})")
      endif()
    endforeach()
    if(NOT EXISTS "${CLI_E2E_INSTALLED_TOOL}")
      message(FATAL_ERROR "e2e.install: ${CLI_E2E_INSTALLED_TOOL} was not installed. Check that the app "
                          "target is installed with COMPONENT runtime (cmake/Install.cmake).")
    endif()
    message(STATUS "e2e.install: installed into ${CLI_E2E_PREFIX}")
    return()
  elseif(CLI_E2E_MODE STREQUAL "staged-files")
    file(REMOVE_RECURSE "${CLI_E2E_STAGE_DIR}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --install "${CLI_E2E_BUILD_DIR}" --prefix "${CLI_E2E_STAGE_DIR}"
      RESULT_VARIABLE _rc)
    if(NOT _rc STREQUAL "0")
      message(FATAL_ERROR "release-check.staged-files: full `cmake --install` failed (${_rc})")
    endif()
    string(REGEX REPLACE "([][.+*?^$()|\\\\])" "\\\\\\1" _t "${CLI_E2E_TOOL_NAME}")
    set(_allowed
      "^${CLI_E2E_BINDIR}/${_t}$"
      "^${CLI_E2E_MANDIR}/man1/${_t}\\.1$"
      "^${CLI_E2E_DATADIR}/bash-completion/completions/${_t}(\\.bash)?$"
      "^${CLI_E2E_DATADIR}/zsh/site-functions/_${_t}$"
      "^${CLI_E2E_DATADIR}/fish/vendor_completions\\.d/${_t}\\.fish$"
      "^${CLI_E2E_DOCDIR}/[^/]+$")
    if(NOT "${CLI_E2E_EXTRA_REGEX}" STREQUAL "")
      list(APPEND _allowed "${CLI_E2E_EXTRA_REGEX}")
    endif()
    file(GLOB_RECURSE _staged LIST_DIRECTORIES false RELATIVE "${CLI_E2E_STAGE_DIR}" "${CLI_E2E_STAGE_DIR}/*")
    list(SORT _staged)
    set(_unexpected "")
    foreach(_f IN LISTS _staged)
      set(_ok FALSE)
      foreach(_re IN LISTS _allowed)
        if(_f MATCHES "${_re}")
          set(_ok TRUE)
          break()
        endif()
      endforeach()
      if(NOT _ok)
        list(APPEND _unexpected "${_f}")
      endif()
    endforeach()
    if(NOT "${CLI_E2E_BINDIR}/${CLI_E2E_TOOL_NAME}" IN_LIST _staged)
      message(FATAL_ERROR "release-check.staged-files: ${CLI_E2E_BINDIR}/${CLI_E2E_TOOL_NAME} is not staged")
    endif()
    if(_unexpected)
      list(JOIN _unexpected "\n  " _list)
      message(FATAL_ERROR "release-check.staged-files: unexpected files in a full install (they would ship "
                          "in the package):\n  ${_list}\nIf a file is intentional, add a regex to "
                          "mangadex_dl_STAGED_FILES_EXTRA_REGEX in tests/CMakeLists.txt; otherwise mark the "
                          "dependency EXCLUDE_FROM_ALL or fix the install() rule.")
    endif()
    list(LENGTH _staged _n)
    message(STATUS "release-check.staged-files: ${_n} staged file(s), all expected")
    return()
  else()
    message(FATAL_ERROR "CliE2E.cmake: unknown CLI_E2E_MODE '${CLI_E2E_MODE}'")
  endif()
endif()

# =====================================================================================================
# Project mode
# =====================================================================================================
include_guard(GLOBAL)

set(CLI_E2E_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(CLI_E2E_PREFIX "${PROJECT_BINARY_DIR}/e2e-prefix")
set(CLI_E2E_INSTALLED_TOOL "${CLI_E2E_PREFIX}/${CMAKE_INSTALL_BINDIR}/mangadex-dl")
set(CLI_E2E_SPEC_DIR "${PROJECT_BINARY_DIR}/e2e-cases")
set(CLI_E2E_WORK_ROOT "${PROJECT_BINARY_DIR}/e2e-work")

function(cli_e2e_add_install_fixture)
  add_test(NAME e2e.install
           COMMAND "${CMAKE_COMMAND}"
                   -DCLI_E2E_MODE=install
                   "-DCLI_E2E_BUILD_DIR=${PROJECT_BINARY_DIR}"
                   "-DCLI_E2E_PREFIX=${CLI_E2E_PREFIX}"
                   "-DCLI_E2E_INSTALLED_TOOL=${CLI_E2E_INSTALLED_TOOL}"
                   -P "${CLI_E2E_DIR}/CliE2E.cmake")
  set_tests_properties(e2e.install PROPERTIES FIXTURES_SETUP e2e_installed LABELS e2e)
endfunction()

# Bracket-quote one value for the generated spec file.
function(_cli_e2e_quote out value)
  if(value MATCHES "]==]")
    message(FATAL_ERROR "cli_e2e_case: a value contains ']==]', which the spec file cannot quote")
  endif()
  set(${out} "[==[${value}]==]" PARENT_SCOPE)
endfunction()

function(cli_e2e_case)
  set(_flags EXPECT_STDOUT_EMPTY EXPECT_STDERR_EMPTY EXPECT_JSON EXPECT_JSON_LINES EXPECT_NO_ANSI
             EXPECT_ANSI GOLDEN TTY PARENT_IGNORES_SIGPIPE)
  set(_one NAME STDIN EXPECT_EXIT EXPECT_STDOUT_REGEX EXPECT_STDERR_REGEX STDOUT_FILE PIPE_HEAD
           EXPECT_PIPE_RESULT TIMEOUT)
  set(_multi ARGS ENV LABELS)
  cmake_parse_arguments(PARSE_ARGV 0 C "${_flags}" "${_one}" "${_multi}")

  if(C_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): unknown arguments: ${C_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT C_NAME MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "cli_e2e_case: NAME '${C_NAME}' must match [A-Za-z0-9_-]+")
  endif()
  if(NOT DEFINED CLI_E2E_SUITE OR CLI_E2E_SUITE STREQUAL "")
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): call it from a tests/e2e/cases/<suite>.cmake file")
  endif()
  if(C_EXPECT_STDOUT_EMPTY AND DEFINED C_EXPECT_STDOUT_REGEX)
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): EXPECT_STDOUT_EMPTY and EXPECT_STDOUT_REGEX conflict")
  endif()
  if(C_EXPECT_STDERR_EMPTY AND DEFINED C_EXPECT_STDERR_REGEX)
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): EXPECT_STDERR_EMPTY and EXPECT_STDERR_REGEX conflict")
  endif()
  if(C_EXPECT_ANSI AND C_EXPECT_NO_ANSI)
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): EXPECT_ANSI and EXPECT_NO_ANSI conflict")
  endif()
  set(_modes 0)
  foreach(_m IN ITEMS C_TTY C_STDOUT_FILE C_PIPE_HEAD)
    if(${_m})
      math(EXPR _modes "${_modes} + 1")
    endif()
  endforeach()
  if(_modes GREATER 1)
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): TTY, STDOUT_FILE and PIPE_HEAD are mutually exclusive")
  endif()
  if((DEFINED C_EXPECT_PIPE_RESULT OR C_PARENT_IGNORES_SIGPIPE) AND NOT C_PIPE_HEAD)
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): EXPECT_PIPE_RESULT/PARENT_IGNORES_SIGPIPE need PIPE_HEAD")
  endif()
  if(C_TTY AND (DEFINED C_STDIN))
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): TTY cases read stdin from the pseudo-terminal; drop STDIN")
  endif()
  if(C_TTY AND (C_EXPECT_STDERR_EMPTY OR DEFINED C_EXPECT_STDERR_REGEX))
    message(FATAL_ERROR "cli_e2e_case(${C_NAME}): under TTY both streams share the terminal; "
                        "assert on stdout (the merged output) instead of EXPECT_STDERR_*")
  endif()
  foreach(_a IN LISTS C_ARGS)
    if(_a STREQUAL "")
      message(FATAL_ERROR "cli_e2e_case(${C_NAME}): empty ARGS item. CMake drops empty list elements, "
                          "and a ';' inside an argument splits it, so neither can be passed")
    endif()
  endforeach()

  if(NOT DEFINED C_EXPECT_EXIT)
    set(C_EXPECT_EXIT 0)
  endif()
  if(NOT DEFINED C_TIMEOUT)
    set(C_TIMEOUT 60)
  endif()
  if(NOT DEFINED C_STDIN)
    set(C_STDIN NULL)
  endif()
  if(C_PIPE_HEAD AND NOT DEFINED C_EXPECT_PIPE_RESULT)
    set(C_EXPECT_PIPE_RESULT SIGPIPE)   # SIGPIPE policy A (cli-design): the writer dies silently by signal
  endif()

  set(_test "e2e.${CLI_E2E_SUITE}.${C_NAME}")
  set(_spec "${CLI_E2E_SPEC_DIR}/${CLI_E2E_SUITE}.${C_NAME}.cmake")

  # Everything the driver needs goes into a generated spec file. That keeps args with ';', quotes,
  # spaces or regex metacharacters intact (no -D quoting games).
  set(_text "# Generated by cli_e2e_case() from ${CMAKE_CURRENT_LIST_FILE}. Do not edit.\n")
  foreach(_var IN ITEMS NAME STDIN EXPECT_EXIT EXPECT_STDOUT_REGEX EXPECT_STDERR_REGEX STDOUT_FILE
                        PIPE_HEAD EXPECT_PIPE_RESULT TIMEOUT)
    if(DEFINED C_${_var})
      _cli_e2e_quote(_q "${C_${_var}}")
      string(APPEND _text "set(CASE_${_var} ${_q})\n")
    endif()
  endforeach()
  foreach(_var IN LISTS _flags)
    if(C_${_var})
      string(APPEND _text "set(CASE_${_var} ON)\n")
    endif()
  endforeach()
  foreach(_var IN ITEMS ARGS ENV)
    string(APPEND _text "set(CASE_${_var})\n")
    foreach(_item IN LISTS C_${_var})
      _cli_e2e_quote(_q "${_item}")
      string(APPEND _text "list(APPEND CASE_${_var} ${_q})\n")
    endforeach()
  endforeach()
  foreach(_pair IN ITEMS "SUITE;${CLI_E2E_SUITE}" "TEST;${_test}"
                         "DEFAULT_TOOL;${CLI_E2E_INSTALLED_TOOL}"
                         "GOLDEN_DIR;${CLI_E2E_DIR}/golden" "INPUTS_DIR;${CLI_E2E_DIR}/inputs"
                         "WORK_DIR;${CLI_E2E_WORK_ROOT}/${CLI_E2E_SUITE}.${C_NAME}"
                         "PROJ_UPPER;MANGADEX_DL")
    list(GET _pair 0 _k)
    list(GET _pair 1 _v)
    _cli_e2e_quote(_q "${_v}")
    string(APPEND _text "set(CASE_${_k} ${_q})\n")
  endforeach()
  file(WRITE "${_spec}" "${_text}")

  add_test(NAME "${_test}"
           COMMAND "${CMAKE_COMMAND}" "-DCASE_SPEC=${_spec}" -P "${CLI_E2E_DIR}/run_case.cmake")

  set(_labels e2e)
  if(CLI_E2E_SUITE STREQUAL "edge")
    list(APPEND _labels edge)
  endif()
  if(C_GOLDEN)
    list(APPEND _labels golden)
  endif()
  list(APPEND _labels ${C_LABELS})
  math(EXPR _ctest_timeout "${C_TIMEOUT} + 30")
  set_tests_properties("${_test}" PROPERTIES
    FIXTURES_REQUIRED e2e_installed
    LABELS "${_labels}"
    SKIP_RETURN_CODE 77
    TIMEOUT ${_ctest_timeout})
  if(MANGADEX_DL_SANITIZER_TEST_ENV)
    set_tests_properties("${_test}" PROPERTIES ENVIRONMENT "${MANGADEX_DL_SANITIZER_TEST_ENV}")
  endif()
endfunction()
