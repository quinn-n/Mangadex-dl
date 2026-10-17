# cmake/ProjectWarnings.cmake: compiler warnings for this project's OWN targets.
#
# Included from the top-level CMakeLists.txt after the options, Sanitizers.cmake and Hardening.cmake,
# and before any project target is created. The options it reads are declared in CMakeLists.txt:
#   MANGADEX_DL_WARNINGS_AS_ERRORS  (default OFF; the presets and CI turn it ON)
#   MANGADEX_DL_LIFETIME_SAFETY     (default OFF; Clang-only advisory analysis)
#   MANGADEX_DL_SANITIZE            ("" / "address;undefined" / "thread")
#
# Usage, for every own target (core library, app, test executables):
#   mangadex_dl_apply_warnings(<target>)
#
# Rules (cpp-quality skill):
# - Warnings are target-scoped. Never add_compile_options(-W...), never a global
#   CMAKE_COMPILE_WARNING_AS_ERROR: either would also -Werror source-built dependencies.
# - -Werror is the per-target COMPILE_WARNING_AS_ERROR property, behind the option. Packagers turn it
#   off with `cmake --compile-no-warning-as-error`; agents never do.
# - Flag lists are gated per compiler. Clang only *warns* on an unknown -W flag, and under -Werror that
#   warning becomes an error, so a GCC-only flag must never reach Clang (and vice versa).
include_guard(GLOBAL)
include(CheckCXXCompilerFlag)

add_library(mangadex_dl_warnings INTERFACE)

set(_mangadex_dl_warnings_common
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow                  # friction: constructor params vs members; members use a trailing `_`
    -Wconversion
    -Wsign-conversion         # explicit: GCC's -Wconversion does not imply it in C++
    -Wdouble-promotion
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wsuggest-override
    -Wold-style-cast          # friction: libc macros; wrap them in a small helper instead of disabling
    -Wcast-align
    -Wcast-qual
    -Wnull-dereference        # GCC: only effective with optimization (the release build catches it)
    -Wformat=2
    -Werror=format-security
    -Wimplicit-fallthrough
    -Wextra-semi)

# GCC-only. (-Wdangling-reference is already in GCC's -Wextra; -Wdangling-pointer=2 and
# -Wuse-after-free=2 are in GCC's -Wall.)
set(_mangadex_dl_warnings_gcc
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast            # friction: casts that are no-ops on this platform's typedefs
    -Wtrampolines)

# Clang-only.
set(_mangadex_dl_warnings_clang
    -Wcomma
    -Wunreachable-code
    -Wshadow-uncaptured-local)

# GCC sanitized builds only: sanitizer instrumentation degrades GCC's flow analysis and produces false
# positives (GCC PR 83382, PR 108306). Demote exactly these to warnings; the release build keeps full
# -Werror and catches the real ones. This is a starting list (memory-safety skill): add an entry only
# with a GCC bugzilla link in the comment.
set(_mangadex_dl_gcc_sanitized_no_error
    -Wno-error=maybe-uninitialized
    -Wno-error=array-bounds
    -Wno-error=stringop-overflow
    -Wno-error=stringop-overread
    -Wno-error=stringop-truncation
    -Wno-error=null-dereference)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(mangadex_dl_warnings INTERFACE
                         ${_mangadex_dl_warnings_common} ${_mangadex_dl_warnings_gcc})
  check_cxx_compiler_flag(-Wbidi-chars=any MANGADEX_DL_HAVE_WBIDI_CHARS)
  if(MANGADEX_DL_HAVE_WBIDI_CHARS)
    target_compile_options(mangadex_dl_warnings INTERFACE -Wbidi-chars=any)
  endif()
  if(NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "")
    target_compile_options(mangadex_dl_warnings INTERFACE ${_mangadex_dl_gcc_sanitized_no_error})
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")   # Clang and AppleClang
  target_compile_options(mangadex_dl_warnings INTERFACE
                         ${_mangadex_dl_warnings_common} ${_mangadex_dl_warnings_clang})
  if(MANGADEX_DL_LIFETIME_SAFETY)
    # Advisory: probed, and never an error even under -Werror. Its noise level is verified at use-time.
    check_cxx_compiler_flag(-Wlifetime-safety-permissive MANGADEX_DL_HAVE_WLIFETIME_SAFETY)
    if(MANGADEX_DL_HAVE_WLIFETIME_SAFETY)
      target_compile_options(mangadex_dl_warnings INTERFACE
                             -Wlifetime-safety-permissive -Wno-error=lifetime-safety-permissive)
    else()
      message(WARNING "MANGADEX_DL_LIFETIME_SAFETY=ON but ${CMAKE_CXX_COMPILER} does not accept "
                      "-Wlifetime-safety-permissive; continuing without it.")
    endif()
  endif()
# elseif(MSVC)   # Windows CI only (optional). Verify each flag against the runner's MSVC before enabling.
#   target_compile_options(mangadex_dl_warnings INTERFACE
#                          /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
else()
  message(WARNING "ProjectWarnings.cmake: no warning set for compiler '${CMAKE_CXX_COMPILER_ID}'.")
endif()

# Link the warnings PRIVATE and build-only (they never leak into an install/export interface),
# and turn on per-target -Werror when the option is ON.
function(mangadex_dl_apply_warnings tgt)
  if(NOT TARGET ${tgt})
    message(FATAL_ERROR "mangadex_dl_apply_warnings: '${tgt}' is not a target")
  endif()
  target_link_libraries(${tgt} PRIVATE $<BUILD_INTERFACE:mangadex_dl_warnings>)
  if(MANGADEX_DL_WARNINGS_AS_ERRORS)
    set_target_properties(${tgt} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  endif()
endfunction()
