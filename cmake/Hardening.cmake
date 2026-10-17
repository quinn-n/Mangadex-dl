# cmake/Hardening.cmake: release hardening per the OpenSSF Compiler Options Hardening Guide for C/C++.
#
# Included from the top-level CMakeLists.txt right after Sanitizers.cmake, before any target, so the
# flags apply project-wide (and to any dependency built from source in this build).
#
# Reads:  MANGADEX_DL_HARDENING        ON in the release preset, OFF in dev/cov/tsan
#         MANGADEX_DL_HARDENING_EXTRA  opt-in: the three "production code" semantics flags
#         MANGADEX_DL_SANITIZE         must be empty when hardening is ON
#
# Rules (memory-safety skill):
# - Hardening and sanitizers are never combined in one build. That is a FATAL_ERROR here.
# - Every flag is probed for the active compiler/linker. Never assume support from a version number.
# - -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 are compile OPTIONS, in that order. CMake emits definitions
#   before options, so a -U in the options would undo a -D from add_compile_definitions().
#   _FORTIFY_SOURCE needs optimization, so it is skipped for Debug builds.
# - PIE comes from CMAKE_POSITION_INDEPENDENT_CODE + check_pie_supported(), not hand-written -fPIE/-pie.
# - The readelf markers that release-check.sh asserts (Type: DYN, GNU_STACK RW, GNU_RELRO,
#   BIND_NOW / FLAGS_1 NOW PIE) are binutils conventions. Confirm them against this build's own output.
# - -fhardened (GCC 14+) is NOT a substitute: it lacks strict-flex-arrays, noexecstack and nodlopen,
#   and it has no Clang equivalent.

include_guard(GLOBAL)

if(NOT DEFINED MANGADEX_DL_HARDENING)
  option(MANGADEX_DL_HARDENING "Release hardening flags (OpenSSF)" OFF)
endif()
if(NOT DEFINED MANGADEX_DL_HARDENING_EXTRA)
  option(MANGADEX_DL_HARDENING_EXTRA "Also -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing" OFF)
endif()

if(MANGADEX_DL_HARDENING AND NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "")
  message(FATAL_ERROR "MANGADEX_DL_HARDENING=ON together with MANGADEX_DL_SANITIZE='${MANGADEX_DL_SANITIZE}'. "
                      "Hardening and sanitizer flags are never combined (fortify vs ASan, auto-var-init hides "
                      "bugs). Use the dev preset (sanitized) or the release preset (hardened).")
endif()

if(NOT MANGADEX_DL_HARDENING)
  return()
endif()

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)

# ---- PIE -------------------------------------------------------------------------------------
check_pie_supported(OUTPUT_VARIABLE _mangadex_dl_pie_out LANGUAGES CXX)
if(CMAKE_CXX_LINK_PIE_SUPPORTED)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
  message(WARNING "Hardening: the toolchain cannot link PIE executables: ${_mangadex_dl_pie_out}")
endif()

# ---- compile flags (probed) ------------------------------------------------------------------
function(_mangadex_dl_add_hardening_cflags)
  foreach(_flag IN LISTS ARGN)
    string(MAKE_C_IDENTIFIER "MANGADEX_DL_HAVE${_flag}" _var)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_compiler_flag("${_flag}" ${_var})
    if(${_var})
      add_compile_options("${_flag}")
    else()
      message(STATUS "Hardening: ${_flag} not supported by ${CMAKE_CXX_COMPILER_ID}; skipped")
    endif()
  endforeach()
endfunction()

# Fortify (needs -O1+): undefine first, because the toolchain or distro flags may predefine a level.
add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>")

# Std-lib hardening. Each library ignores the other's macro. libc++ FAST mode is the OpenSSF choice.
add_compile_definitions(_GLIBCXX_ASSERTIONS _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)

_mangadex_dl_add_hardening_cflags(
  -fstrict-flex-arrays=3
  -fstack-clash-protection
  -fstack-protector-strong
  -ftrivial-auto-var-init=zero)   # OpenSSF "production" row; studio default ON (it does not hide -Wuninitialized)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  _mangadex_dl_add_hardening_cflags(-fcf-protection=full -fzero-call-used-regs=used-gpr)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  _mangadex_dl_add_hardening_cflags(-mbranch-protection=standard)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  _mangadex_dl_add_hardening_cflags(-fzero-init-padding-bits=all)
endif()

if(MANGADEX_DL_HARDENING_EXTRA)
  # Trade-off (memory-safety skill): these make some UB benign and cost some optimization. The dev
  # preset's UBSan is what finds the underlying bugs, so they are opt-in.
  _mangadex_dl_add_hardening_cflags(-fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing)
endif()

# ---- link flags (probed, LINKER: form) -------------------------------------------------------
foreach(_lflag IN ITEMS -z,nodlopen -z,noexecstack -z,relro -z,now --as-needed --no-copy-dt-needed-entries)
  string(MAKE_C_IDENTIFIER "MANGADEX_DL_HAVE_LD${_lflag}" _var)
  set(CMAKE_REQUIRED_QUIET ON)
  check_linker_flag(CXX "LINKER:${_lflag}" ${_var})
  if(${_var})
    add_link_options("LINKER:${_lflag}")
  else()
    message(STATUS "Hardening: linker flag ${_lflag} not supported; skipped")
  endif()
endforeach()

message(STATUS "Hardening: ON (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}; extra=${MANGADEX_DL_HARDENING_EXTRA})")
