# tests/sanitizer-controls/run_control.cmake: run one positive control; PASS only if the sanitizer FIRED.
#
#   cmake -DCONTROL_NAME=<n> -DCONTROL_EXE=<path> -DCONTROL_MARKER=<regex> [-DCONTROL_EXIT=<n>]
#         [-DCONTROL_DEFAULTS_ONLY=ON] -P run_control.cmake
#
# Pass = exit nonzero (exactly CONTROL_EXIT when given) AND stderr matches CONTROL_MARKER.
# Fail = the sanitizer is silent or misconfigured in this env, so "sanitizer-clean" is not licensed.

cmake_minimum_required(VERSION 3.31...4.4)

if(CONTROL_DEFAULTS_ONLY)
  foreach(_v IN ITEMS ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS TSAN_OPTIONS)
    unset(ENV{${_v}})
  endforeach()
endif()

execute_process(COMMAND "${CONTROL_EXE}"
                INPUT_FILE /dev/null
                OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
                TIMEOUT 90)

set(_fail "")
if("${_rc}" STREQUAL "0")
  list(APPEND _fail "exited 0: the sanitizer did not fire (detection broken)")
elseif(NOT "${CONTROL_EXIT}" STREQUAL "" AND NOT "${_rc}" STREQUAL "${CONTROL_EXIT}")
  list(APPEND _fail "exit '${_rc}', expected ${CONTROL_EXIT} (the reserved sanitizer exit code is not honoured)")
endif()
if(NOT _err MATCHES "${CONTROL_MARKER}")
  list(APPEND _fail "stderr lacks the marker '${CONTROL_MARKER}'")
endif()

if(_fail)
  string(SUBSTRING "${_err}" 0 4000 _err_head)
  list(JOIN _fail "\n  - " _msg)
  message(FATAL_ERROR "${CONTROL_NAME}: positive control FAILED\n  - ${_msg}\n"
                      "\"sanitizer-clean\" is NOT licensed for this build.\n"
                      "--- stderr (first 4000 bytes) ---\n${_err_head}\n")
endif()
message(STATUS "${CONTROL_NAME}: fired as expected (exit ${_rc})")
