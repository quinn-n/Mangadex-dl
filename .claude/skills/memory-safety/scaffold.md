---
name: memory-safety-scaffold
description: Supporting file of the memory-safety skill (scaffold templates read by /setup). Not a standalone skill.
---

# memory-safety scaffold

`/setup` writes these files into the user's repository. The policy behind them is in `SKILL.md` next to this file (option C, the runtime options, suppressions, release hardening). This file holds only the templates.

**How `/setup` uses it:**
- Each `###` heading below is a **target path**, relative to the repo root. The fenced block under it is the complete file content.
- Substitute `@PROJ@` and `@PROJ_UPPER@`. No other placeholders appear.
  - `@SAN_ASAN_OPTIONS@`, `@SAN_UBSAN_OPTIONS@`, `@SAN_LSAN_OPTIONS@` and `@SAN_TSAN_OPTIONS@` in `sanitizer_options.cpp.in` are **not** `/setup` placeholders. CMake's `configure_file()` fills them at configure time. Leave them as they are.
- **Never overwrite an existing file.** For an existing project, show a per-file diff and wait for the human (⏸).
- The three `.supp` files must exist even though they hold only comments. A missing suppressions file is a fatal sanitizer runtime error, and `Sanitizers.cmake` refuses to configure without them.

**Interfaces other scaffolds rely on (shared contract):**

| Name | Defined in | Used by |
|---|---|---|
| `@PROJ_UPPER@_SANITIZE` (`""` / `"address;undefined"` / `"thread"`) | `CMakeLists.txt` (build-workflow) | `Sanitizers.cmake`, `Hardening.cmake`, `ProjectWarnings.cmake`, `Packaging.cmake` (`cpack` refuses at package time when non-empty), `tests/CMakeLists.txt` |
| `@PROJ_UPPER@_HARDENING`, `@PROJ_UPPER@_HARDENING_EXTRA` | `CMakeLists.txt` | `Hardening.cmake`, `tests/CMakeLists.txt` (release checks) |
| `@PROJ@_add_sanitizer_options(<target>)` | `Sanitizers.cmake` | `CMakeLists.txt` (app), test targets, sanitizer controls |
| `@PROJ_UPPER@_SANITIZER_TEST_ENV` (list of `KEY=VALUE`) | `Sanitizers.cmake` | CTest `ENVIRONMENT` of every plain `add_test()` (E2E cases, controls) |
| `<binaryDir>/sanitizer-flags.txt` (one flag per line) | `Sanitizers.cmake` | `scripts/cxx-probe.sh` |
| `<binaryDir>/sanitizer-env.txt` (`KEY=VALUE` lines) | `Sanitizers.cmake` | `scripts/cxx-probe.sh`, humans running a sanitized binary by hand |
| `<binaryDir>/generated/sanitizer_options.cpp` | `Sanitizers.cmake` from `src/app/sanitizer_options.cpp.in` | `@PROJ@_add_sanitizer_options()` |

**Placement notes:**
- The six GCC-sanitized `-Wno-error=` demotions are warnings, so they are target-scoped. They live in `cmake/ProjectWarnings.cmake` (cpp-quality scaffold), not here.
- The Ninja link pool for sanitized links is set in `CMakeLists.txt` (build-workflow scaffold), not here.

**Verify after writing** (the gate does most of it):
1. `nice pixi run configure`, then read `build/default/dev/sanitizer-flags.txt` and `sanitizer-env.txt`. Every `*_OPTIONS` line carries `exitcode=86`, and every `suppressions=` path is absolute and exists.
2. `nice pixi run test -L sanitizer-control` passes. Each control fired with its marker, and the `*-defaults` controls (heap-overflow, leak, signed-overflow) prove that the compiled-in defaults alone give exit 86 for each runtime.
3. `nice pixi run release`, then check the configure log. It lists which hardening flags the toolchain accepted (`Hardening: <flag> ... skipped` lines name the rest).

### `cmake/Sanitizers.cmake`

```cmake
# cmake/Sanitizers.cmake: project-wide sanitizer flags and the SINGLE source of the runtime options.
#
# Included from the top-level CMakeLists.txt after the options and before Hardening.cmake, any
# find_package() and any target, so every target in this build (and any dependency built from source
# in it) is instrumented. Warnings stay target-scoped in ProjectWarnings.cmake, and that includes the
# six GCC-sanitized -Wno-error= demotions.
#
# Reads:    @PROJ_UPPER@_SANITIZE   "" (off) | "address;undefined" (dev preset) | "thread" (opt-in tsan preset)
# Defines:  @PROJ@_add_sanitizer_options(<target>)  compiles the default-options TU into a sanitized
#             executable; a no-op when sanitizers are off (CMakeLists calls it unconditionally).
#           @PROJ_UPPER@_SANITIZER_TEST_ENV  list of KEY=VALUE for the CTest ENVIRONMENT property
#             (empty when off). Test registration passes it to every plain add_test().
#           @PROJ_UPPER@_SANITIZER_EXITCODE  86, the reserved "sanitizer finding" exit code.
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
# - A sanitized build is never packaged: Packaging.cmake FATALs when @PROJ_UPPER@_SANITIZE is non-empty.
# - Hardening and sanitizers are never combined (Hardening.cmake FATALs).

include_guard(GLOBAL)

if(NOT DEFINED @PROJ_UPPER@_SANITIZE)
  set(@PROJ_UPPER@_SANITIZE "" CACHE STRING "Sanitizers: empty, \"address;undefined\" or \"thread\"")
endif()

set(@PROJ_UPPER@_SANITIZER_EXITCODE 86)
set(@PROJ_UPPER@_SANITIZER_TEST_ENV "")
set(_@PROJ@_san_flags_file "${PROJECT_BINARY_DIR}/sanitizer-flags.txt")
set(_@PROJ@_san_env_file "${PROJECT_BINARY_DIR}/sanitizer-env.txt")
set(_@PROJ@_san_options_cpp "${PROJECT_BINARY_DIR}/generated/sanitizer_options.cpp")

function(@PROJ@_add_sanitizer_options target)
  if(NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "")
    target_sources(${target} PRIVATE "${_@PROJ@_san_options_cpp}")
  endif()
endfunction()

if("${@PROJ_UPPER@_SANITIZE}" STREQUAL "")
  file(WRITE "${_@PROJ@_san_flags_file}" "")
  file(WRITE "${_@PROJ@_san_env_file}" "")
  return()
endif()

# ---- validate the request ---------------------------------------------------------------------
set(_@PROJ@_san_known address undefined thread)
foreach(_san IN LISTS @PROJ_UPPER@_SANITIZE)
  if(NOT _san IN_LIST _@PROJ@_san_known)
    message(FATAL_ERROR "@PROJ_UPPER@_SANITIZE: unknown sanitizer '${_san}'. "
                        "Use \"address;undefined\" (dev) or \"thread\" (tsan preset).")
  endif()
endforeach()
if("thread" IN_LIST @PROJ_UPPER@_SANITIZE AND "address" IN_LIST @PROJ_UPPER@_SANITIZE)
  message(FATAL_ERROR "@PROJ_UPPER@_SANITIZE: thread cannot be combined with address (or leak). "
                      "Use the separate tsan preset.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "@PROJ_UPPER@_SANITIZE is supported with Clang and GCC only (got ${CMAKE_CXX_COMPILER_ID}).")
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
function(_@PROJ@_probe_san_flag flag out_var)
  string(MAKE_C_IDENTIFIER "@PROJ_UPPER@_HAVE${flag}" _cache_var)
  set(CMAKE_REQUIRED_LINK_OPTIONS ${_@PROJ@_san_link_base} "${flag}")
  set(CMAKE_REQUIRED_FLAGS "${_@PROJ@_san_compile_base_str}")
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_compiler_flag("${flag}" ${_cache_var})
  set(${out_var} ${${_cache_var}} PARENT_SCOPE)
endfunction()

list(JOIN @PROJ_UPPER@_SANITIZE "," _@PROJ@_san_joined)
set(_@PROJ@_san_main "-fsanitize=${_@PROJ@_san_joined}")

set(_@PROJ@_san_link_base "")
set(_@PROJ@_san_compile_base_str "")
_@PROJ@_probe_san_flag("${_@PROJ@_san_main}" _@PROJ@_have_main)
if(NOT _@PROJ@_have_main)
  message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} cannot compile and link "
                      "${_@PROJ@_san_main}: the sanitizer runtime is absent from this toolchain/env. "
                      "Run the platform-testing probe (scope probe) for the diagnosis; do not turn the "
                      "sanitizers off to get a green build.")
endif()

set(_@PROJ@_san_compile "${_@PROJ@_san_main}")
set(_@PROJ@_san_link "${_@PROJ@_san_main}")
set(_@PROJ@_san_link_base "${_@PROJ@_san_main}")
set(_@PROJ@_san_compile_base_str "${_@PROJ@_san_main}")

if("undefined" IN_LIST @PROJ_UPPER@_SANITIZE)
  # float-cast-overflow: in Clang's 'undefined' group, NOT in GCC's -> must be explicit for parity.
  # vptr: Clang's 'undefined' no longer includes it; GCC's does (repeating it is harmless).
  foreach(_extra IN ITEMS -fsanitize=float-cast-overflow -fsanitize=vptr)
    _@PROJ@_probe_san_flag("${_extra}" _ok)
    if(_ok)
      list(APPEND _@PROJ@_san_compile "${_extra}")
      list(APPEND _@PROJ@_san_link "${_extra}")
    else()
      message(STATUS "Sanitizers: ${_extra} not supported by ${CMAKE_CXX_COMPILER_ID}; skipped")
    endif()
  endforeach()
endif()

if(NOT "thread" IN_LIST @PROJ_UPPER@_SANITIZE)
  # Any finding terminates with the reserved exit code (and UBSan runtime suppressions become inert).
  list(APPEND _@PROJ@_san_compile -fno-sanitize-recover=all)
  list(APPEND _@PROJ@_san_link -fno-sanitize-recover=all)
endif()

list(APPEND _@PROJ@_san_compile -fno-omit-frame-pointer -fno-optimize-sibling-calls)

# Clang-only (GCC rejects it): keep distinct check sites so every report points at its own source line.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  _@PROJ@_probe_san_flag(-fno-sanitize-merge _ok)
  if(_ok)
    list(APPEND _@PROJ@_san_compile -fno-sanitize-merge)
  endif()
endif()

# Std-lib checks: libstdc++ assertions explicitly (the preset is -O1); libc++ debug hardening (ignored
# by libstdc++). Never _GLIBCXX_DEBUG: it changes container ABI against the prebuilt Catch2.
set(_@PROJ@_san_defines -D_GLIBCXX_ASSERTIONS -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)

# -U_FORTIFY_SOURCE as a compile OPTION: CMake emits definitions before options on the command line,
# and the toolchain may predefine fortify.
add_compile_options(${_@PROJ@_san_compile} -U_FORTIFY_SOURCE ${_@PROJ@_san_defines})
add_link_options(${_@PROJ@_san_link})

# ---- runtime options: the single source -------------------------------------------------------
set(_@PROJ@_supp_dir "${PROJECT_SOURCE_DIR}/tools/sanitizers")
foreach(_supp IN ITEMS asan ubsan lsan)
  # A missing suppressions file is a fatal runtime error (raised lazily), so it must exist up front.
  if(NOT EXISTS "${_@PROJ@_supp_dir}/${_supp}.supp")
    message(FATAL_ERROR "Missing ${_@PROJ@_supp_dir}/${_supp}.supp. The file may hold only comments, "
                        "but it must exist (memory-safety skill).")
  endif()
endforeach()

set(_@PROJ@_ec "${@PROJ_UPPER@_SANITIZER_EXITCODE}")
set(SAN_ASAN_OPTIONS "")
set(SAN_UBSAN_OPTIONS "")
set(SAN_LSAN_OPTIONS "")
set(SAN_TSAN_OPTIONS "")
if("address" IN_LIST @PROJ_UPPER@_SANITIZE)
  string(JOIN ":" SAN_ASAN_OPTIONS
    halt_on_error=1 abort_on_error=0 exitcode=${_@PROJ@_ec} detect_leaks=1
    detect_stack_use_after_return=1 check_initialization_order=1 strict_init_order=1
    strict_string_checks=1 handle_segv=2 print_summary=1
    suppressions=${_@PROJ@_supp_dir}/asan.supp)
  string(JOIN ":" SAN_LSAN_OPTIONS
    exitcode=${_@PROJ@_ec} print_suppressions=0 suppressions=${_@PROJ@_supp_dir}/lsan.supp)
endif()
if("undefined" IN_LIST @PROJ_UPPER@_SANITIZE)
  string(JOIN ":" SAN_UBSAN_OPTIONS
    halt_on_error=1 print_stacktrace=1 exitcode=${_@PROJ@_ec}
    suppressions=${_@PROJ@_supp_dir}/ubsan.supp)
endif()
if("thread" IN_LIST @PROJ_UPPER@_SANITIZE)
  string(JOIN ":" SAN_TSAN_OPTIONS halt_on_error=1 second_deadlock_stack=1 exitcode=${_@PROJ@_ec})
endif()

set(_@PROJ@_env_text "")
foreach(_name IN ITEMS ASAN UBSAN LSAN TSAN)
  if(NOT "${SAN_${_name}_OPTIONS}" STREQUAL "")
    list(APPEND @PROJ_UPPER@_SANITIZER_TEST_ENV "${_name}_OPTIONS=${SAN_${_name}_OPTIONS}")
    string(APPEND _@PROJ@_env_text "${_name}_OPTIONS=${SAN_${_name}_OPTIONS}\n")
  endif()
endforeach()
file(WRITE "${_@PROJ@_san_env_file}" "${_@PROJ@_env_text}")

# Flags for a one-shot compile-and-link that matches this preset (build-type flags included).
string(TOUPPER "${CMAKE_BUILD_TYPE}" _@PROJ@_bt)
separate_arguments(_@PROJ@_bt_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_@PROJ@_bt}}")
set(_@PROJ@_flag_lines ${_@PROJ@_bt_flags} ${_@PROJ@_san_compile} -U_FORTIFY_SOURCE ${_@PROJ@_san_defines})
foreach(_f IN LISTS _@PROJ@_san_link)
  if(NOT _f IN_LIST _@PROJ@_flag_lines)
    list(APPEND _@PROJ@_flag_lines "${_f}")
  endif()
endforeach()
list(JOIN _@PROJ@_flag_lines "\n" _@PROJ@_flag_text)
file(WRITE "${_@PROJ@_san_flags_file}" "${_@PROJ@_flag_text}\n")

configure_file("${PROJECT_SOURCE_DIR}/src/app/sanitizer_options.cpp.in" "${_@PROJ@_san_options_cpp}" @ONLY)

message(STATUS "Sanitizers: ${_@PROJ@_san_joined} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}); "
               "reserved exit code ${_@PROJ@_ec}")
```

### `cmake/Hardening.cmake`

```cmake
# cmake/Hardening.cmake: release hardening per the OpenSSF Compiler Options Hardening Guide for C/C++.
#
# Included from the top-level CMakeLists.txt right after Sanitizers.cmake, before any target, so the
# flags apply project-wide (and to any dependency built from source in this build).
#
# Reads:  @PROJ_UPPER@_HARDENING        ON in the release preset, OFF in dev/cov/tsan
#         @PROJ_UPPER@_HARDENING_EXTRA  opt-in: the three "production code" semantics flags
#         @PROJ_UPPER@_SANITIZE         must be empty when hardening is ON
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

if(NOT DEFINED @PROJ_UPPER@_HARDENING)
  option(@PROJ_UPPER@_HARDENING "Release hardening flags (OpenSSF)" OFF)
endif()
if(NOT DEFINED @PROJ_UPPER@_HARDENING_EXTRA)
  option(@PROJ_UPPER@_HARDENING_EXTRA "Also -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing" OFF)
endif()

if(@PROJ_UPPER@_HARDENING AND NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "")
  message(FATAL_ERROR "@PROJ_UPPER@_HARDENING=ON together with @PROJ_UPPER@_SANITIZE='${@PROJ_UPPER@_SANITIZE}'. "
                      "Hardening and sanitizer flags are never combined (fortify vs ASan, auto-var-init hides "
                      "bugs). Use the dev preset (sanitized) or the release preset (hardened).")
endif()

if(NOT @PROJ_UPPER@_HARDENING)
  return()
endif()

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)

# ---- PIE -------------------------------------------------------------------------------------
check_pie_supported(OUTPUT_VARIABLE _@PROJ@_pie_out LANGUAGES CXX)
if(CMAKE_CXX_LINK_PIE_SUPPORTED)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
  message(WARNING "Hardening: the toolchain cannot link PIE executables: ${_@PROJ@_pie_out}")
endif()

# ---- compile flags (probed) ------------------------------------------------------------------
function(_@PROJ@_add_hardening_cflags)
  foreach(_flag IN LISTS ARGN)
    string(MAKE_C_IDENTIFIER "@PROJ_UPPER@_HAVE${_flag}" _var)
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

_@PROJ@_add_hardening_cflags(
  -fstrict-flex-arrays=3
  -fstack-clash-protection
  -fstack-protector-strong
  -ftrivial-auto-var-init=zero)   # OpenSSF "production" row; studio default ON (it does not hide -Wuninitialized)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  _@PROJ@_add_hardening_cflags(-fcf-protection=full -fzero-call-used-regs=used-gpr)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  _@PROJ@_add_hardening_cflags(-mbranch-protection=standard)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  _@PROJ@_add_hardening_cflags(-fzero-init-padding-bits=all)
endif()

if(@PROJ_UPPER@_HARDENING_EXTRA)
  # Trade-off (memory-safety skill): these make some UB benign and cost some optimization. The dev
  # preset's UBSan is what finds the underlying bugs, so they are opt-in.
  _@PROJ@_add_hardening_cflags(-fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing)
endif()

# ---- link flags (probed, LINKER: form) -------------------------------------------------------
foreach(_lflag IN ITEMS -z,nodlopen -z,noexecstack -z,relro -z,now --as-needed --no-copy-dt-needed-entries)
  string(MAKE_C_IDENTIFIER "@PROJ_UPPER@_HAVE_LD${_lflag}" _var)
  set(CMAKE_REQUIRED_QUIET ON)
  check_linker_flag(CXX "LINKER:${_lflag}" ${_var})
  if(${_var})
    add_link_options("LINKER:${_lflag}")
  else()
    message(STATUS "Hardening: linker flag ${_lflag} not supported; skipped")
  endif()
endforeach()

message(STATUS "Hardening: ON (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}; extra=${@PROJ_UPPER@_HARDENING_EXTRA})")
```

### `src/app/sanitizer_options.cpp.in`

```cpp
// sanitizer_options.cpp: compiled-in sanitizer runtime defaults.
//
// GENERATED into <build>/generated/sanitizer_options.cpp by cmake/Sanitizers.cmake from
// src/app/sanitizer_options.cpp.in. Edit the .in template or Sanitizers.cmake, never the generated copy.
// The option strings have ONE source (Sanitizers.cmake); the same strings reach CTest's ENVIRONMENT
// and sanitizer-env.txt.
//
// Linked only into sanitized executables, via @PROJ@_add_sanitizer_options(<target>). The runtimes call
// these hooks at startup. No attributes are needed on either compiler. Keys set in ASAN_OPTIONS /
// UBSAN_OPTIONS / LSAN_OPTIONS / TSAN_OPTIONS in the environment override these defaults key by key.
// The effect: an installed sanitized binary run by hand still exits with the reserved code 86 on a
// finding, prints stacks, and uses the committed suppression files.
//
// An empty string means that runtime is not part of this build's sanitizer set.

extern "C" {
const char* __asan_default_options();
const char* __ubsan_default_options();
const char* __lsan_default_options();
const char* __tsan_default_options();

const char* __asan_default_options() { return "@SAN_ASAN_OPTIONS@"; }
const char* __ubsan_default_options() { return "@SAN_UBSAN_OPTIONS@"; }
const char* __lsan_default_options() { return "@SAN_LSAN_OPTIONS@"; }
const char* __tsan_default_options() { return "@SAN_TSAN_OPTIONS@"; }
}
```

### `tools/sanitizers/asan.supp`

```text
# AddressSanitizer runtime suppressions. Wired in by cmake/Sanitizers.cmake (ASAN_OPTIONS suppressions=<abs path>).
#
# This file MUST exist, even if it holds only comments: a missing suppressions file is a fatal runtime error.
#
# Format: one `<type>:<pattern>` per line, where <pattern> matches a function, source file or module name
#   interceptor_via_fun:<function>
#   interceptor_via_lib:<library>
#   odr_violation:<global>
#
# Policy (memory-safety skill):
# - A suppression is a last resort for a finding in code you do not control (a prebuilt dependency).
#   A finding in our own code is a bug: fix it.
# - Every entry carries a reason and an issue link on the comment line directly above it.
# - A new entry is a review finding for memory-safety-reviewer.
#
# Example (do not uncomment without a reason + link):
# # reason: false positive in prebuilt libfoo's custom allocator; upstream https://example.invalid/issue/123
# interceptor_via_lib:libfoo.so
```

### `tools/sanitizers/ubsan.supp`

```text
# UndefinedBehaviorSanitizer runtime suppressions. Wired in by cmake/Sanitizers.cmake (UBSAN_OPTIONS suppressions=<abs path>).
#
# This file MUST exist, even if it holds only comments: a missing suppressions file is a fatal runtime error.
#
# Format: one `<check>:<pattern>` per line, where <pattern> matches a source file, function or module name,
#   for example `signed-integer-overflow:third_party/foo/*.c`.
#
# IMPORTANT: this project compiles with -fno-sanitize-recover=all, and UBSan runtime suppressions apply only
# to RECOVERABLE checks. Entries here therefore have NO effect in the dev preset. Fix the code instead.
# If the behaviour is intended and defined by contract (rare), use a narrowly scoped
#   __attribute__((no_sanitize("<check>")))
# on that one function, with a comment that gives the reason and an issue link.
#
# Policy (memory-safety skill): every entry carries a reason and an issue link on the comment line above it;
# a new entry is a review finding for memory-safety-reviewer.
```

### `tools/sanitizers/lsan.supp`

```text
# LeakSanitizer runtime suppressions. Wired in by cmake/Sanitizers.cmake (LSAN_OPTIONS suppressions=<abs path>).
#
# This file MUST exist, even if it holds only comments: a missing suppressions file is a fatal runtime error.
#
# Format: one `leak:<pattern>` per line; <pattern> is a substring match on a function, source file or
# module name anywhere in the leak's allocation stack.
#
# Policy (memory-safety skill):
# - A leak in our own code is a bug: fix the ownership (Rule of Zero, unique_ptr).
# - Suppress only leaks inside prebuilt dependencies or deliberate process-lifetime singletons that you
#   cannot change. Every entry carries a reason and an issue link on the comment line directly above it.
# - A new entry is a review finding for memory-safety-reviewer.
#
# Example (do not uncomment without a reason + link):
# # reason: libfoo caches a process-lifetime table it never frees; upstream https://example.invalid/issue/456
# leak:libfoo.so
```
