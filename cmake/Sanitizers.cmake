# cmake/Sanitizers.cmake: project-wide sanitizer flags and the SINGLE source of the runtime options.
#
# Included from the top-level CMakeLists.txt after the options and before Hardening.cmake, any
# find_package() and any target, so every target in this build (and any dependency built from source
# in it) is instrumented. Warnings stay target-scoped in ProjectWarnings.cmake, and that includes the
# six GCC-sanitized -Wno-error= demotions.
#
# Reads:    MANGADEX_DL_SANITIZE   "" (off) | "address;undefined" (dev preset) | "thread" (opt-in tsan preset)
# Defines:  mangadex_dl_add_sanitizer_options(<target>)  compiles the default-options TU into a sanitized
#             executable; a no-op when sanitizers are off (CMakeLists calls it unconditionally).
#           MANGADEX_DL_SANITIZER_TEST_ENV  list of KEY=VALUE for the CTest ENVIRONMENT property
#             (empty when off). Test registration passes it to every plain add_test().
#           MANGADEX_DL_SANITIZER_EXITCODE  86, the reserved "sanitizer finding" exit code.
# Writes (binary dir):
#           sanitizer-flags.txt   one compiler/linker flag per line, no quoting, no comments; the flags a
#                                 single `$CXX -std=c++23 <flags> file.cpp -o out` needs to match this
#                                 preset (read by scripts/cxx-probe.sh). Empty when sanitizers are off.
#           sanitizer-env.txt     KEY=VALUE lines, the same runtime options (read by cxx-probe.sh and
#                                 by humans running a sanitized binary by hand). Empty when off.
#           generated/sanitizer_options.cpp   from src/app/sanitizer_options.cpp.in.
#
# Rules (memory-safety skill, option C):
# - The same exitcode=86 goes in ASAN_OPTIONS, UBSAN_OPTIONS and LSAN_OPTIONS (+ TSAN_OPTIONS).
#   Clang has one shared exit code; GCC has separate runtimes.
# - -U_FORTIFY_SOURCE (ASan is incompatible with fortify) and an explicit -D_GLIBCXX_ASSERTIONS
#   (libstdc++ enables its assertions by itself only at -O0; this preset is -O1).
# - -O1 -g is owned by the preset (CMAKE_CXX_FLAGS_DEBUG). This file never adds a second -O level.
# - Every optional flag is probed for the active compiler, compiling AND linking. Never hardcode support.
# - A sanitized build is never packaged: Packaging.cmake FATALs when MANGADEX_DL_SANITIZE is non-empty.
# - Hardening and sanitizers are never combined (Hardening.cmake FATALs).

include_guard(GLOBAL)

if(NOT DEFINED MANGADEX_DL_SANITIZE)
  set(MANGADEX_DL_SANITIZE "" CACHE STRING "Sanitizers: empty, \"address;undefined\" or \"thread\"")
endif()

set(MANGADEX_DL_SANITIZER_EXITCODE 86)
set(MANGADEX_DL_SANITIZER_TEST_ENV "")
set(_mangadex_dl_san_flags_file "${PROJECT_BINARY_DIR}/sanitizer-flags.txt")
set(_mangadex_dl_san_env_file "${PROJECT_BINARY_DIR}/sanitizer-env.txt")
set(_mangadex_dl_san_options_cpp "${PROJECT_BINARY_DIR}/generated/sanitizer_options.cpp")

function(mangadex_dl_add_sanitizer_options target)
  if(NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "")
    target_sources(${target} PRIVATE "${_mangadex_dl_san_options_cpp}")
  endif()
endfunction()

if("${MANGADEX_DL_SANITIZE}" STREQUAL "")
  file(WRITE "${_mangadex_dl_san_flags_file}" "")
  file(WRITE "${_mangadex_dl_san_env_file}" "")
  return()
endif()

# ---- validate the request ---------------------------------------------------------------------
set(_mangadex_dl_san_known address undefined thread)
foreach(_san IN LISTS MANGADEX_DL_SANITIZE)
  if(NOT _san IN_LIST _mangadex_dl_san_known)
    message(FATAL_ERROR "MANGADEX_DL_SANITIZE: unknown sanitizer '${_san}'. "
                        "Use \"address;undefined\" (dev) or \"thread\" (tsan preset).")
  endif()
endforeach()
if("thread" IN_LIST MANGADEX_DL_SANITIZE AND "address" IN_LIST MANGADEX_DL_SANITIZE)
  message(FATAL_ERROR "MANGADEX_DL_SANITIZE: thread cannot be combined with address (or leak). "
                      "Use the separate tsan preset.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "MANGADEX_DL_SANITIZE is supported with Clang and GCC only (got ${CMAKE_CXX_COMPILER_ID}).")
endif()

# The runtime option parser splits on ':' and whitespace, so suppression paths must contain neither.
if(PROJECT_SOURCE_DIR MATCHES "[: \t]")
  message(FATAL_ERROR "The source path '${PROJECT_SOURCE_DIR}' contains ':' or whitespace. Sanitizer "
                      "suppression paths would be split by the runtime option parser. Move the checkout.")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O1")
  message(WARNING "Sanitized build without -O1 in CMAKE_CXX_FLAGS_DEBUG ('${CMAKE_CXX_FLAGS_DEBUG}'). "
                  "The dev preset owns \"-O1 -g\"; check CMakePresets.json.")
endif()

# ---- probe and apply flags --------------------------------------------------------------------
include(CheckCXXCompilerFlag)

# Probe a flag for compile AND link (a compile-only probe can pass while the runtime is missing).
function(_mangadex_dl_probe_san_flag flag out_var)
  string(MAKE_C_IDENTIFIER "MANGADEX_DL_HAVE${flag}" _cache_var)
  set(CMAKE_REQUIRED_LINK_OPTIONS ${_mangadex_dl_san_link_base} "${flag}")
  set(CMAKE_REQUIRED_FLAGS "${_mangadex_dl_san_compile_base_str}")
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_compiler_flag("${flag}" ${_cache_var})
  set(${out_var} ${${_cache_var}} PARENT_SCOPE)
endfunction()

list(JOIN MANGADEX_DL_SANITIZE "," _mangadex_dl_san_joined)
set(_mangadex_dl_san_main "-fsanitize=${_mangadex_dl_san_joined}")

set(_mangadex_dl_san_link_base "")
set(_mangadex_dl_san_compile_base_str "")
_mangadex_dl_probe_san_flag("${_mangadex_dl_san_main}" _mangadex_dl_have_main)
if(NOT _mangadex_dl_have_main)
  message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} cannot compile and link "
                      "${_mangadex_dl_san_main}: the sanitizer runtime is absent from this toolchain/env. "
                      "Run the platform-testing probe (scope probe) for the diagnosis; do not turn the "
                      "sanitizers off to get a green build.")
endif()

set(_mangadex_dl_san_compile "${_mangadex_dl_san_main}")
set(_mangadex_dl_san_link "${_mangadex_dl_san_main}")
set(_mangadex_dl_san_link_base "${_mangadex_dl_san_main}")
set(_mangadex_dl_san_compile_base_str "${_mangadex_dl_san_main}")

if("undefined" IN_LIST MANGADEX_DL_SANITIZE)
  # float-cast-overflow: in Clang's 'undefined' group, NOT in GCC's -> must be explicit for parity.
  # vptr: Clang's 'undefined' no longer includes it; GCC's does (repeating it is harmless).
  foreach(_extra IN ITEMS -fsanitize=float-cast-overflow -fsanitize=vptr)
    _mangadex_dl_probe_san_flag("${_extra}" _ok)
    if(_ok)
      list(APPEND _mangadex_dl_san_compile "${_extra}")
      list(APPEND _mangadex_dl_san_link "${_extra}")
    else()
      message(STATUS "Sanitizers: ${_extra} not supported by ${CMAKE_CXX_COMPILER_ID}; skipped")
    endif()
  endforeach()
endif()

if(NOT "thread" IN_LIST MANGADEX_DL_SANITIZE)
  # Any finding terminates with the reserved exit code (and UBSan runtime suppressions become inert).
  list(APPEND _mangadex_dl_san_compile -fno-sanitize-recover=all)
  list(APPEND _mangadex_dl_san_link -fno-sanitize-recover=all)
endif()

list(APPEND _mangadex_dl_san_compile -fno-omit-frame-pointer -fno-optimize-sibling-calls)

# Clang-only (GCC rejects it): keep distinct check sites so every report points at its own source line.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  _mangadex_dl_probe_san_flag(-fno-sanitize-merge _ok)
  if(_ok)
    list(APPEND _mangadex_dl_san_compile -fno-sanitize-merge)
  endif()
endif()

# Std-lib checks: libstdc++ assertions explicitly (the preset is -O1); libc++ debug hardening (ignored
# by libstdc++). Never _GLIBCXX_DEBUG: it changes container ABI against the prebuilt Catch2.
set(_mangadex_dl_san_defines -D_GLIBCXX_ASSERTIONS -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)

# -U_FORTIFY_SOURCE as a compile OPTION: CMake emits definitions before options on the command line,
# and the toolchain may predefine fortify.
add_compile_options(${_mangadex_dl_san_compile} -U_FORTIFY_SOURCE ${_mangadex_dl_san_defines})
add_link_options(${_mangadex_dl_san_link})

# ---- runtime options: the single source -------------------------------------------------------
set(_mangadex_dl_supp_dir "${PROJECT_SOURCE_DIR}/tools/sanitizers")
foreach(_supp IN ITEMS asan ubsan lsan)
  # A missing suppressions file is a fatal runtime error (raised lazily), so it must exist up front.
  if(NOT EXISTS "${_mangadex_dl_supp_dir}/${_supp}.supp")
    message(FATAL_ERROR "Missing ${_mangadex_dl_supp_dir}/${_supp}.supp. The file may hold only comments, "
                        "but it must exist (memory-safety skill).")
  endif()
endforeach()

set(_mangadex_dl_ec "${MANGADEX_DL_SANITIZER_EXITCODE}")
set(SAN_ASAN_OPTIONS "")
set(SAN_UBSAN_OPTIONS "")
set(SAN_LSAN_OPTIONS "")
set(SAN_TSAN_OPTIONS "")
if("address" IN_LIST MANGADEX_DL_SANITIZE)
  string(JOIN ":" SAN_ASAN_OPTIONS
    halt_on_error=1 abort_on_error=0 exitcode=${_mangadex_dl_ec} detect_leaks=1
    detect_stack_use_after_return=1 check_initialization_order=1 strict_init_order=1
    strict_string_checks=1 handle_segv=2 print_summary=1
    suppressions=${_mangadex_dl_supp_dir}/asan.supp)
  string(JOIN ":" SAN_LSAN_OPTIONS
    exitcode=${_mangadex_dl_ec} print_suppressions=0 suppressions=${_mangadex_dl_supp_dir}/lsan.supp)
endif()
if("undefined" IN_LIST MANGADEX_DL_SANITIZE)
  string(JOIN ":" SAN_UBSAN_OPTIONS
    halt_on_error=1 print_stacktrace=1 exitcode=${_mangadex_dl_ec}
    suppressions=${_mangadex_dl_supp_dir}/ubsan.supp)
endif()
if("thread" IN_LIST MANGADEX_DL_SANITIZE)
  string(JOIN ":" SAN_TSAN_OPTIONS halt_on_error=1 second_deadlock_stack=1 exitcode=${_mangadex_dl_ec})
endif()

set(_mangadex_dl_env_text "")
foreach(_name IN ITEMS ASAN UBSAN LSAN TSAN)
  if(NOT "${SAN_${_name}_OPTIONS}" STREQUAL "")
    list(APPEND MANGADEX_DL_SANITIZER_TEST_ENV "${_name}_OPTIONS=${SAN_${_name}_OPTIONS}")
    string(APPEND _mangadex_dl_env_text "${_name}_OPTIONS=${SAN_${_name}_OPTIONS}\n")
  endif()
endforeach()
file(WRITE "${_mangadex_dl_san_env_file}" "${_mangadex_dl_env_text}")

# Flags for a one-shot compile-and-link that matches this preset (build-type flags included).
string(TOUPPER "${CMAKE_BUILD_TYPE}" _mangadex_dl_bt)
separate_arguments(_mangadex_dl_bt_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_mangadex_dl_bt}}")
set(_mangadex_dl_flag_lines ${_mangadex_dl_bt_flags} ${_mangadex_dl_san_compile} -U_FORTIFY_SOURCE ${_mangadex_dl_san_defines})
foreach(_f IN LISTS _mangadex_dl_san_link)
  if(NOT _f IN_LIST _mangadex_dl_flag_lines)
    list(APPEND _mangadex_dl_flag_lines "${_f}")
  endif()
endforeach()
list(JOIN _mangadex_dl_flag_lines "\n" _mangadex_dl_flag_text)
file(WRITE "${_mangadex_dl_san_flags_file}" "${_mangadex_dl_flag_text}\n")

configure_file("${PROJECT_SOURCE_DIR}/src/app/sanitizer_options.cpp.in" "${_mangadex_dl_san_options_cpp}" @ONLY)

message(STATUS "Sanitizers: ${_mangadex_dl_san_joined} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}); "
               "reserved exit code ${_mangadex_dl_ec}")
