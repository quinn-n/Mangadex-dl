# tests/sanitizer-controls/CMakeLists.txt: sanitizer POSITIVE CONTROLS (added only when sanitizing).
#
# Each control is a deliberately buggy program. It must make the sanitizer fire: a nonzero exit plus its
# marker on stderr. A control that stays silent means the instrumentation, runtime or options are broken
# in this env, so "sanitizer-clean" is NOT licensed (memory-safety and platform-testing skills).
# Label: sanitizer-control. Names: sanitizer-control.<n>.
#
# These binaries are not linted for the bug they contain on purpose: the directory's .clang-tidy replaces
# the parent configs with a single harmless check.

set(_mangadex_dl_ctl_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_control.cmake")

# _mangadex_dl_add_control(<name> <source> MARKER <regex> [EXIT <n>] [LINK_CATCH2] [DEFAULTS_ONLY] [O0])
#   EXIT          exact exit code to require (default: any nonzero)
#   DEFAULTS_ONLY run with *SAN_OPTIONS unset, proving the compiled-in defaults (sanitizer_options.cpp)
#   O0            build at -O0 (stack residue at -O1 can hide a leak; -O0 keeps each scoped local in
#                 its own poisoned stack slot for use-after-scope)
function(_mangadex_dl_add_control name source)
  cmake_parse_arguments(PARSE_ARGV 2 C "LINK_CATCH2;DEFAULTS_ONLY;O0" "MARKER;EXIT;TARGET" "")
  if(NOT C_TARGET)
    set(C_TARGET "mangadex_dl_sanctl_${name}")
    string(REPLACE "-" "_" C_TARGET "${C_TARGET}")
  endif()
  if(NOT TARGET ${C_TARGET})
    add_executable(${C_TARGET} "${source}")
    if(C_LINK_CATCH2)
      target_link_libraries(${C_TARGET} PRIVATE Catch2::Catch2WithMain)
    endif()
    if(C_O0)
      target_compile_options(${C_TARGET} PRIVATE -O0)
    endif()
    mangadex_dl_add_sanitizer_options(${C_TARGET})
  endif()
  if(NOT DEFINED C_EXIT)
    set(C_EXIT "")
  endif()
  add_test(NAME sanitizer-control.${name}
           COMMAND "${CMAKE_COMMAND}"
                   "-DCONTROL_NAME=sanitizer-control.${name}"
                   "-DCONTROL_EXE=$<TARGET_FILE:${C_TARGET}>"
                   "-DCONTROL_MARKER=${C_MARKER}"
                   "-DCONTROL_EXIT=${C_EXIT}"
                   "-DCONTROL_DEFAULTS_ONLY=${C_DEFAULTS_ONLY}"
                   -P "${_mangadex_dl_ctl_driver}")
  set_tests_properties(sanitizer-control.${name} PROPERTIES LABELS sanitizer-control TIMEOUT 120)
  if(MANGADEX_DL_SANITIZER_TEST_ENV)
    set_tests_properties(sanitizer-control.${name} PROPERTIES ENVIRONMENT "${MANGADEX_DL_SANITIZER_TEST_ENV}")
  endif()
endfunction()

if("address" IN_LIST MANGADEX_DL_SANITIZE)
  _mangadex_dl_add_control(heap-overflow "${CMAKE_CURRENT_SOURCE_DIR}/heap_overflow.cpp"
    MARKER "ERROR: AddressSanitizer: heap-buffer-overflow" EXIT 86)
  # Same binary, *SAN_OPTIONS unset: the compiled-in defaults alone must still give exit 86.
  _mangadex_dl_add_control(heap-overflow-defaults "${CMAKE_CURRENT_SOURCE_DIR}/heap_overflow.cpp"
    TARGET mangadex_dl_sanctl_heap_overflow
    MARKER "ERROR: AddressSanitizer: heap-buffer-overflow" EXIT 86 DEFAULTS_ONLY)
  _mangadex_dl_add_control(leak "${CMAKE_CURRENT_SOURCE_DIR}/leak.cpp"
    MARKER "ERROR: LeakSanitizer: detected memory leaks" EXIT 86 O0)
  # ...and the compiled-in __lsan_default_options alone (GCC's leak exit code is governed separately).
  _mangadex_dl_add_control(leak-defaults "${CMAKE_CURRENT_SOURCE_DIR}/leak.cpp"
    TARGET mangadex_dl_sanctl_leak
    MARKER "ERROR: LeakSanitizer: detected memory leaks" EXIT 86 DEFAULTS_ONLY)
  # A null dereference inside a Catch2 TEST_CASE: Catch2 installs its own SIGSEGV handler, and
  # handle_segv=2 keeps ASan's accurate report in charge.
  _mangadex_dl_add_control(catch2-segv "${CMAKE_CURRENT_SOURCE_DIR}/catch2_segv.cpp"
    MARKER "ERROR: AddressSanitizer: SEGV" LINK_CATCH2)
  # The dangling-view class: a std::string_view outliving its owner. First a destroyed long (heap)
  # std::string, then a block-scoped stack buffer (-O0, so the buffer keeps its own poisoned slot).
  _mangadex_dl_add_control(dangling-view-heap "${CMAKE_CURRENT_SOURCE_DIR}/dangling_view_heap.cpp"
    MARKER "ERROR: AddressSanitizer: heap-use-after-free" EXIT 86)
  _mangadex_dl_add_control(dangling-view-scope "${CMAKE_CURRENT_SOURCE_DIR}/dangling_view_scope.cpp"
    MARKER "ERROR: AddressSanitizer: stack-use-after-scope" EXIT 86 O0)
endif()

if("undefined" IN_LIST MANGADEX_DL_SANITIZE)
  # GCC's UBSan prints no SUMMARY line, so the marker keys on "runtime error:".
  _mangadex_dl_add_control(signed-overflow "${CMAKE_CURRENT_SOURCE_DIR}/signed_overflow.cpp"
    MARKER "runtime error: signed integer overflow" EXIT 86)
  # *SAN_OPTIONS unset: the compiled-in __ubsan_default_options alone must still give exit 86
  # (GCC's UBSan runtime reads only its own options, separately from ASan's).
  _mangadex_dl_add_control(signed-overflow-defaults "${CMAKE_CURRENT_SOURCE_DIR}/signed_overflow.cpp"
    TARGET mangadex_dl_sanctl_signed_overflow
    MARKER "runtime error: signed integer overflow" EXIT 86 DEFAULTS_ONLY)
endif()

if("thread" IN_LIST MANGADEX_DL_SANITIZE)
  _mangadex_dl_add_control(data-race "${CMAKE_CURRENT_SOURCE_DIR}/data_race.cpp"
    MARKER "WARNING: ThreadSanitizer: data race" EXIT 86)
endif()
