---
name: cpp-testing-scaffold
description: Supporting file of the cpp-testing skill (scaffold templates read by /setup). Not a standalone skill.
---

# cpp-testing scaffold

`/setup` writes these files into the user's repository. The policy behind them is in `SKILL.md` next to this file (tiers, the `cli_e2e_case` API, the driver rules, the edge suite, goldens and controls). This file holds only the templates.

**How `/setup` uses it:**
- Each `###` heading below is a **target path**, relative to the repo root. The fenced block under it is the complete file content.
- Substitute `@TOOL@`, `@PROJ@` and `@PROJ_UPPER@`. No other placeholders appear.
  - `{{INPUTS}}`, `{{WORK}}` and `{{INVALID_UTF8}}` are E2E **driver** placeholders, expanded by `run_case.cmake` at test time. Leave them as they are.
- **Never overwrite an existing file.** For an existing project, show a per-file diff and wait for the human (⏸).
- **Never write anything under `tests/e2e/golden/`.** Goldens come only from `pixi run bless` (⏸, human-approved). Before the first bless, the `GOLDEN` cases fail with `golden missing ... request a bless`. That failure is expected; `/setup` blesses and then re-runs the gate.
- `scripts/bless.sh` runs as `bash scripts/bless.sh` (the `bless` pixi task), so no exec bit is needed.

**What these templates assume from the other scaffolds** (shared contract; check them if a scaffold changes):

| Assumption | Owner |
|---|---|
| `@PROJ@::app::run(std::span<const char* const>, Io&)` declared in `src/app/cli.hpp`; `@PROJ@::app::Io` (`out`, `err`, `stdin_tty`, `stdout_tty`, `stderr_tty`, `env`) and a default-constructible `@PROJ@::app::EnvSnapshot` in `src/app/io.hpp` | cli-design scaffold |
| `@PROJ@::greet(std::string_view) -> Result<std::string>` in `include/@PROJ@/greet.hpp` (`Hello, <name>!`; `Errc::usage` for an empty name or control characters), `Error`/`Errc` in `include/@PROJ@/error.hpp` | cli-design scaffold |
| A `hello [NAME] [-n/--count N] [--json]` subcommand: default name `world`, one greeting line per count, one JSON document carrying the greeting with the global `--json`, and stdout colored via `resolve_color` | cli-design scaffold (used by `cases/hello.cmake` and the `_edge.cmake` knobs) |
| `--version` first line `@TOOL@ X.Y.Z`, **byte-deterministic** (no git describe, sha or build date: `edge.version` is GOLDEN in dev and release); usage errors exit 2; a stdout write failure exits 1 with `write error` on stderr; SIGPIPE policy A | cli-design skill and scaffold |
| `@PROJ@_app_sources` (app sources without `main.cpp`), `@PROJ@_app`, `@PROJ@::core`, `@PROJ@_apply_warnings()`; `include(CTest)`; `find_package(Catch2 3 CONFIG REQUIRED)` before `add_subdirectory(tests)` | build-workflow scaffold (`CMakeLists.txt`) |
| `install(... COMPONENT runtime)` for the app; `COMPONENT docs` for the man page and completions | build-workflow scaffold (`cmake/Install.cmake`) |
| `@PROJ@_add_sanitizer_options()`, `@PROJ_UPPER@_SANITIZER_TEST_ENV`, `@PROJ_UPPER@_SANITIZE`, `@PROJ_UPPER@_HARDENING`, `@PROJ_UPPER@_GLIBC_FLOOR` | memory-safety scaffold, `CMakeLists.txt` |
| `scripts/release-check.sh <installed-binary> <glibc-floor>` | build-workflow `ci-release.md` |
| `scripts/preset.sh test <preset> [-R re] [-L re] ...` (lock + `-local` preset selection) | build-workflow scaffold |

**Verify after writing** (as part of `/setup`'s gate):
1. `nice pixi run test` (long Bash `timeout` or `run_in_background`). Only the `GOLDEN` cases may fail, with `golden missing`.
2. ⏸ `nice pixi run bless`, then re-run `nice pixi run check`. It must be green.
3. `nice pixi run test -L edge` runs the edge suite alone. Every `e2e.edge.*` case passes; `tty-*` cases report **Skipped** only where util-linux `script` is absent.

## Registration (tests/CMakeLists.txt and the tier-1/2 directories)

### `tests/CMakeLists.txt`

```cmake
# tests/CMakeLists.txt: test registration for every tier. [serialization point]
#
# Added by the top-level CMakeLists.txt under `if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)`, after
# find_package(Catch2 3 CONFIG REQUIRED). Tiers and labels (cpp-testing skill):
#   unit.*                    label unit               Catch2 against @PROJ@::core        (tests/unit/)
#   cli.*                     label cli                Catch2 against the in-process run() seam (tests/cli/)
#   e2e.install               fixture e2e_installed    installs runtime + docs into <build>/e2e-prefix
#   e2e.<sub>.<case>          label e2e (+golden)      installed-binary cases, one file per subcommand
#   e2e.edge.<case>           labels e2e;edge          the mandatory edge-case suite (cases/_edge.cmake)
#   sanitizer-control.<n>     label sanitizer-control  positive controls; only when sanitizers are on
#   release-check.*           label release-check      only in hardened (release) builds
#
# Adding a module test, an in-process test or an E2E case needs NO edit here: the sources and case
# files are globbed with CONFIGURE_DEPENDS. Edit this file only for a new tier or a new kind of check.

include(Catch)

add_subdirectory(unit)
add_subdirectory(cli)

# ---- installed-binary E2E -----------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/e2e/CliE2E.cmake")
cli_e2e_add_install_fixture()

file(GLOB _@PROJ@_e2e_case_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cases/*.cmake")
list(SORT _@PROJ@_e2e_case_files)
foreach(_case_file IN LISTS _@PROJ@_e2e_case_files)
  get_filename_component(_suite "${_case_file}" NAME_WE)
  string(REGEX REPLACE "^_" "" _suite "${_suite}")   # cases/_edge.cmake -> suite "edge"
  set(CLI_E2E_SUITE "${_suite}")
  include("${_case_file}")
endforeach()
unset(CLI_E2E_SUITE)

# ---- sanitizer positive controls ------------------------------------------------------------------
# A control that does not fire means "sanitizer-clean" is NOT licensed for this build.
if(NOT "${@PROJ_UPPER@_SANITIZE}" STREQUAL "")
  add_subdirectory(sanitizer-controls)
endif()

# ---- release checks (hardened release build only) -------------------------------------------------
if(@PROJ_UPPER@_HARDENING)
  # Binary gates: PIE/RELRO/BIND_NOW/NX markers, no RPATH/RUNPATH (default env), NEEDED allowlist,
  # glibc floor. The logic lives in scripts/release-check.sh (build-workflow ci-release scaffold).
  add_test(NAME release-check.binary
           COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/release-check.sh"
                   "${CLI_E2E_INSTALLED_TOOL}" "${@PROJ_UPPER@_GLIBC_FLOOR}")
  set_tests_properties(release-check.binary PROPERTIES LABELS release-check FIXTURES_REQUIRED e2e_installed)

  # What a full, component-less install (= what CPack packages) stages, into a scratch prefix (never
  # e2e-prefix, which holds only the runtime + docs components). CPACK_COMPONENTS_ALL does NOT filter a monolithic
  # TGZ, so this test is the guard against stray headers, static libs or CMake configs from dependencies.
  # Allowed: bin/@TOOL@, its man page, the three completion files and files directly under the doc dir.
  # Installing something new on purpose? Add an anchored regex here (relative to the prefix).
  set(@PROJ@_STAGED_FILES_EXTRA_REGEX "")
  add_test(NAME release-check.staged-files
           COMMAND "${CMAKE_COMMAND}"
                   -DCLI_E2E_MODE=staged-files
                   "-DCLI_E2E_BUILD_DIR=${PROJECT_BINARY_DIR}"
                   "-DCLI_E2E_STAGE_DIR=${PROJECT_BINARY_DIR}/staged-check"
                   "-DCLI_E2E_TOOL_NAME=@TOOL@"
                   "-DCLI_E2E_BINDIR=${CMAKE_INSTALL_BINDIR}"
                   "-DCLI_E2E_MANDIR=${CMAKE_INSTALL_MANDIR}"
                   "-DCLI_E2E_DATADIR=${CMAKE_INSTALL_DATADIR}"
                   "-DCLI_E2E_DOCDIR=${CMAKE_INSTALL_DOCDIR}"
                   "-DCLI_E2E_EXTRA_REGEX=${@PROJ@_STAGED_FILES_EXTRA_REGEX}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/CliE2E.cmake")
  set_tests_properties(release-check.staged-files PROPERTIES LABELS release-check)
endif()
```

### `tests/unit/CMakeLists.txt`

```cmake
# tests/unit/CMakeLists.txt: Catch2 unit tests against @PROJ@::core (tier 1).
#
# One test file per module under tests/unit/<module>/, globbed: adding a test file needs no CMake edit.
# Every discovered test gets the `unit.` name prefix and the `unit` label. The directory LABELS property
# merges with ADD_TAGS_AS_LABELS; a PROPERTIES LABELS argument would be overwritten by the tag labels.

file(GLOB_RECURSE _@PROJ@_unit_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(@PROJ@_unit_tests ${_@PROJ@_unit_sources})
target_include_directories(@PROJ@_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")   # tests/support/
target_link_libraries(@PROJ@_unit_tests PRIVATE @PROJ@::core Catch2::Catch2WithMain)
@PROJ@_apply_warnings(@PROJ@_unit_tests)
# Sanitized builds: compile the runtime defaults (exitcode=86, handle_segv=2, suppressions) into the test
# binary itself. CTest ENVIRONMENT can't be passed through catch_discover_tests(PROPERTIES): a ';' list
# would be split into separate properties.
@PROJ@_add_sanitizer_options(@PROJ@_unit_tests)

set_property(DIRECTORY PROPERTY LABELS unit)

# PRE_TEST discovery, explicit here so builds without the presets (plain cmake, AUR check()) get it too.
# With POST_BUILD, a sanitizer crash while listing tests would look like a build failure.
catch_discover_tests(@PROJ@_unit_tests
  TEST_PREFIX "unit."
  DISCOVERY_MODE PRE_TEST
  EXTRA_ARGS --warn=NoAssertions
  ADD_TAGS_AS_LABELS)
```

### `tests/unit/core/greet_test.cpp`

```cpp
// tests/unit/core/greet_test.cpp: tier-1 example against @PROJ@::core. Replace it with real module
// tests. Test both sides of every Result: the value and each Error path.
#include "@PROJ@/error.hpp"
#include "@PROJ@/greet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("greet formats a greeting for a name", "[core][greet]") {
    CHECK(@PROJ@::greet("Ada") == std::string{"Hello, Ada!"});
}

TEST_CASE("greet keeps UTF-8 bytes intact", "[core][greet]") {
    CHECK(@PROJ@::greet("Zoë") == std::string{"Hello, Zoë!"});
}

TEST_CASE("greet rejects an empty name as a usage error", "[core][greet]") {
    const auto result = @PROJ@::greet("");
    CHECK_FALSE(result.has_value());
    // error_or never dereferences: a value would yield the default Error (failure), and fail here.
    CHECK(result.error_or(@PROJ@::Error{}).code == @PROJ@::Errc::usage);
}

TEST_CASE("greet rejects control characters", "[core][greet]") {
    const auto result = @PROJ@::greet("a\tb");
    CHECK_FALSE(result.has_value());
    CHECK(result.error_or(@PROJ@::Error{}).code == @PROJ@::Errc::usage);
}
```

### `tests/cli/CMakeLists.txt`

```cmake
# tests/cli/CMakeLists.txt: in-process CLI tests (tier 2) through the run(args, Io&) seam.
#
# The test binary compiles the app's own sources except main.cpp: the top-level CMakeLists.txt exports
# them as @PROJ@_app_sources (io.cpp, cli.cpp and the globbed cmd_*.cpp), so adding a subcommand needs no
# edit here. Link libraries and include dirs are read back from @PROJ@_app, so they stay in step with it.
# Tests include app headers as "app/<header>.hpp".

if(NOT DEFINED @PROJ@_app_sources)
  message(FATAL_ERROR "tests/cli: @PROJ@_app_sources is not defined (set it in the top-level CMakeLists.txt)")
endif()

file(GLOB _@PROJ@_cli_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(@PROJ@_cli_tests ${_@PROJ@_cli_test_sources} ${@PROJ@_app_sources})
target_include_directories(@PROJ@_cli_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/src"                 # "app/cli.hpp", "app/io.hpp"
  "${PROJECT_SOURCE_DIR}/src/app"             # what the app sources themselves include
  "${CMAKE_CURRENT_SOURCE_DIR}/..")           # "support/io_capture.hpp"
get_target_property(_@PROJ@_app_libs @PROJ@_app LINK_LIBRARIES)
if(_@PROJ@_app_libs)
  target_link_libraries(@PROJ@_cli_tests PRIVATE ${_@PROJ@_app_libs})
endif()
get_target_property(_@PROJ@_app_incs @PROJ@_app INCLUDE_DIRECTORIES)
if(_@PROJ@_app_incs)
  target_include_directories(@PROJ@_cli_tests PRIVATE ${_@PROJ@_app_incs})
endif()
target_link_libraries(@PROJ@_cli_tests PRIVATE Catch2::Catch2WithMain)
@PROJ@_apply_warnings(@PROJ@_cli_tests)
@PROJ@_add_sanitizer_options(@PROJ@_cli_tests)

set_property(DIRECTORY PROPERTY LABELS cli)

# PRE_TEST: see tests/unit/CMakeLists.txt.
catch_discover_tests(@PROJ@_cli_tests
  TEST_PREFIX "cli."
  DISCOVERY_MODE PRE_TEST
  EXTRA_ARGS --warn=NoAssertions
  ADD_TAGS_AS_LABELS)
```

### `tests/cli/cli_test.cpp`

```cpp
// tests/cli/cli_test.cpp: tier-2 in-process CLI tests through run(args, Io&).
// Catch2 test names must not start with '-' (CTest passes the name as a test spec, and a leading
// "--" is parsed as an option).
// Each run() call constructs the whole CLI11 app, so a CLI::ConstructionError (duplicate option
// name, bad validator wiring, fallthrough ordering) fails here instead of at a user's first
// invocation.
#include "support/io_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using @PROJ@::test::run_cli;

TEST_CASE("the whole app constructs and --help goes to stdout with exit 0", "[cli][help]") {
    const auto r = run_cli({"--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.out.contains("@TOOL@"));
    CHECK(r.err.empty());
}

TEST_CASE("version prints '@TOOL@ X.Y.Z' first, on stdout, with exit 0", "[cli][version]") {
    const auto r = run_cli({"--version"});
    CHECK(r.exit_code == 0);
    CHECK(r.out.starts_with("@TOOL@ "));
    CHECK(r.err.empty());
}

TEST_CASE("an unknown option is a usage error: exit 2, nothing on stdout", "[cli][usage]") {
    const auto r = run_cli({"--definitely-not-an-option"});
    CHECK(r.exit_code == 2); // never a raw CLI11 code (100-114)
    CHECK(r.out.empty());
    CHECK_FALSE(r.err.empty());
}
```

### `tests/support/io_capture.hpp`

```cpp
#pragma once
// tests/support/io_capture.hpp: run the CLI in-process through the run(args, Io&) seam and capture
// both streams. Tier 2 (cpp-testing skill): help/version text, usage errors, exit-code mapping, the
// --json shape and color decisions, with TTY flags and env injected instead of read from the
// process.

#include "app/cli.hpp"
#include "app/io.hpp"

#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace @PROJ@::test {

struct Tty {
    bool stdin_tty = false;
    bool stdout_tty = false;
    bool stderr_tty = false;
};

struct Captured {
    int exit_code = -1;
    std::string out;
    std::string err;
};

// argv[0] is supplied; pass only the arguments. Arguments are copied, so temporaries are safe here.
[[nodiscard]] inline Captured run_cli(std::initializer_list<std::string> args, Tty tty = {},
                                      app::EnvSnapshot env = {}) {
    std::vector<std::string> storage{"@TOOL@"};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    argv.reserve(storage.size());
    for (const auto& arg : storage) {
        argv.push_back(arg.c_str());
    }

    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out,
               .err = err,
               .stdin_tty = tty.stdin_tty,
               .stdout_tty = tty.stdout_tty,
               .stderr_tty = tty.stderr_tty,
               .env = env};
    Captured result;
    result.exit_code = app::run(std::span<const char* const>{argv}, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

} // namespace @PROJ@::test
```

## Installed-binary E2E (tier 3)

### `tests/e2e/CliE2E.cmake`

```cmake
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
                          "@PROJ@_STAGED_FILES_EXTRA_REGEX in tests/CMakeLists.txt; otherwise mark the "
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
set(CLI_E2E_INSTALLED_TOOL "${CLI_E2E_PREFIX}/${CMAKE_INSTALL_BINDIR}/@TOOL@")
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
                         "PROJ_UPPER;@PROJ_UPPER@")
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
  if(@PROJ_UPPER@_SANITIZER_TEST_ENV)
    set_tests_properties("${_test}" PROPERTIES ENVIRONMENT "${@PROJ_UPPER@_SANITIZER_TEST_ENV}")
  endif()
endfunction()
```

### `tests/e2e/run_case.cmake`

```cmake
# tests/e2e/run_case.cmake: the E2E case driver. Runs ONE case against the INSTALLED binary.
#
#   cmake -DCASE_SPEC=<build>/e2e-cases/<suite>.<case>.cmake -P tests/e2e/run_case.cmake
#
# Registered by cli_e2e_case() (CliE2E.cmake); never invoked by hand except to debug one case. The
# spec file holds the case's arguments and expectations. Rules (cpp-testing skill):
# - Hermetic env via set(ENV{}) / unset(ENV{}). execute_process(ENVIRONMENT) is CMake 4.4+ only.
#   Fresh HOME/XDG_*/TMPDIR under <build>/e2e-work/<suite>.<case>/, LC_ALL=C.UTF-8,
#   TERM=xterm-256color, and CONDA_*/PIXI_*/CMAKE_*/CTEST_*/LC_*/LANG*/XDG_*/color variables and the
#   tool's own @PROJ_UPPER@_* variables removed. *SAN_OPTIONS, ASAN_SYMBOLIZER_PATH, LLVM_PROFILE_FILE
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
```

### `tests/e2e/cases/_edge.cmake`

```cmake
# tests/e2e/cases/_edge.cmake: the MANDATORY edge-case suite (tests e2e.edge.<case>, labels e2e;edge).
#
# Every CLI built with this studio passes these against its installed binary. Generic cases use only
# --help/--version and a usage error. The knobs below point the rest at real subcommands; keep them in
# step with the CLI surface when it changes. Deleting a case here is a CLI-contract change and needs
# cli-ux-reviewer + quality-reviewer sign-off.

# ---- per-project knobs ----------------------------------------------------------------------------
# REQUIRED: a subcommand invocation that writes well over 64 KiB of NEWLINE-separated lines to stdout.
# The SIGPIPE cases need the pipe to fill. Without newlines, `head -n 1` would read everything and the
# case would prove nothing.
set(EDGE_BULK_ARGS hello --count 20000)
# An invocation that writes exactly one JSON document to stdout (empty = the tool has no --json surface).
set(EDGE_JSON_ARGS hello --json)
# An invocation whose stdout contains its LAST argument verbatim (for UTF-8 and locale round trips; empty = none).
set(EDGE_ECHO_ARGS hello)
# An invocation that colors stdout when stdout is a TTY (empty = the tool never colors stdout).
set(EDGE_COLOR_ARGS hello)

if(NOT EDGE_BULK_ARGS)
  message(FATAL_ERROR "tests/e2e/cases/_edge.cmake: EDGE_BULK_ARGS is required (the SIGPIPE cases are mandatory)")
endif()

# ---- help / version (goldens are human-blessed: pixi run bless) ------------------------------------
cli_e2e_case(NAME help ARGS --help
             EXPECT_EXIT 0 EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)
cli_e2e_case(NAME version ARGS --version
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^@TOOL@ [0-9]+\\.[0-9]+\\.[0-9]+" EXPECT_STDERR_EMPTY GOLDEN)

# ---- usage errors: exit 2, nothing on stdout, never a raw CLI11 code (100-114) ----------------------
cli_e2e_case(NAME usage-unknown-option ARGS --definitely-not-an-option
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")
cli_e2e_case(NAME usage-invalid-utf8 ARGS "{{INVALID_UTF8}}"
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")
cli_e2e_case(NAME usage-invalid-utf8-lc-c ARGS "{{INVALID_UTF8}}" ENV LC_ALL=C LANG=C
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")

# ---- stdin -----------------------------------------------------------------------------------------
cli_e2e_case(NAME stdin-null ARGS --version STDIN NULL
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^@TOOL@ " EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME stdin-closed ARGS --version STDIN CLOSED
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^@TOOL@ " EXPECT_STDERR_EMPTY)

# ---- write errors: stdout on a full device -> exit 1 and a "write error" diagnostic -----------------
cli_e2e_case(NAME dev-full-version ARGS --version STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "write error")
cli_e2e_case(NAME dev-full-help ARGS --help STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "write error")

# ---- SIGPIPE policy A: `tool | head -n 1` -> the writer dies by SIGPIPE, silently -------------------
cli_e2e_case(NAME sigpipe ARGS ${EDGE_BULK_ARGS} PIPE_HEAD 1
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)
# The parent ignores SIGPIPE (an inherited SIG_IGN, as some shells and services do): the tool must
# restore SIG_DFL itself.
cli_e2e_case(NAME sigpipe-parent-ignores ARGS ${EDGE_BULK_ARGS} PIPE_HEAD 1 PARENT_IGNORES_SIGPIPE
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)

# ---- TTY and color (pseudo-terminal via util-linux script; skipped with 77 where it is absent) -------
cli_e2e_case(NAME tty-help ARGS --help TTY EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "@TOOL@")
cli_e2e_case(NAME tty-help-no-color ARGS --help TTY ENV NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI)
cli_e2e_case(NAME tty-help-term-dumb ARGS --help TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_NO_ANSI)
if(EDGE_COLOR_ARGS)
  cli_e2e_case(NAME tty-color ARGS ${EDGE_COLOR_ARGS} TTY EXPECT_EXIT 0 EXPECT_ANSI)
  cli_e2e_case(NAME tty-color-no-color ARGS ${EDGE_COLOR_ARGS} TTY ENV NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME tty-color-term-dumb ARGS ${EDGE_COLOR_ARGS} TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME pipe-clicolor-force ARGS ${EDGE_COLOR_ARGS} ENV CLICOLOR_FORCE=1 EXPECT_EXIT 0 EXPECT_ANSI)
  cli_e2e_case(NAME no-color-beats-force ARGS ${EDGE_COLOR_ARGS} ENV CLICOLOR_FORCE=1 NO_COLOR=1
               EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME pipe-no-ansi ARGS ${EDGE_COLOR_ARGS} EXPECT_EXIT 0 EXPECT_NO_ANSI)
endif()

# ---- --json: one valid document, never ANSI, even when color is forced or stdout is a TTY ----------
if(EDGE_JSON_ARGS)
  cli_e2e_case(NAME json-valid ARGS ${EDGE_JSON_ARGS}
               EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY)
  cli_e2e_case(NAME json-clicolor-force ARGS ${EDGE_JSON_ARGS} ENV CLICOLOR_FORCE=1
               EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI)
  cli_e2e_case(NAME json-tty ARGS ${EDGE_JSON_ARGS} TTY EXPECT_EXIT 0 EXPECT_NO_ANSI)
endif()

# ---- UTF-8 and locale independence ------------------------------------------------------------------
if(EDGE_ECHO_ARGS)
  cli_e2e_case(NAME utf8-echo ARGS ${EDGE_ECHO_ARGS} "Zoë"
               EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "Zoë" EXPECT_STDERR_EMPTY)
  cli_e2e_case(NAME utf8-echo-lc-c ARGS ${EDGE_ECHO_ARGS} "Zoë" ENV LC_ALL=C LANG=C
               EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "Zoë" EXPECT_STDERR_EMPTY)
  # Linux argv is bytes: invalid UTF-8 must not crash (never 70/134/139).
  cli_e2e_case(NAME invalid-utf8-echo ARGS ${EDGE_ECHO_ARGS} "{{INVALID_UTF8}}" EXPECT_EXIT 0)
  if(EDGE_JSON_ARGS)
    # ...and --json must still be valid JSON (nlohmann dump with error_handler_t::replace).
    cli_e2e_case(NAME invalid-utf8-json ARGS ${EDGE_JSON_ARGS} "{{INVALID_UTF8}}"
                 EXPECT_EXIT 0 EXPECT_JSON)
  endif()
endif()
```

### `tests/e2e/cases/hello.cmake`

```cmake
# tests/e2e/cases/hello.cmake: installed-binary cases for the scaffold's `hello` subcommand
# (tests e2e.hello.<case>, label e2e). One file per subcommand, globbed, so a track that adds a
# subcommand adds its own cases/<sub>.cmake without touching shared CMake.
#
# Always an exact EXPECT_EXIT, and stdout/stderr asserted separately. A GOLDEN case's expected output
# comes only from `pixi run bless` (human-approved); never hand-write tests/e2e/golden/*.

cli_e2e_case(NAME default ARGS hello
             EXPECT_EXIT 0 EXPECT_STDERR_EMPTY GOLDEN)
cli_e2e_case(NAME named ARGS hello Ada
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^Hello, Ada!\n$" EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME json ARGS hello --json Ada
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_STDOUT_REGEX "Hello, Ada!" EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME unknown-option ARGS hello --definitely-not-an-option
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")
```

### `tests/e2e/inputs/README`

```text
Input fixtures for the installed-binary E2E cases (tests/e2e/cases/*.cmake).

Reference them from a case as `STDIN <file>` (resolved relative to this directory) or as
`{{INPUTS}}/<file>` inside ARGS or ENV values. Keep fixtures small, deterministic and free of real
data, credentials or personal paths: they are committed, and goldens may echo them.
```

## Sanitizer positive controls (tier 4)

### `tests/sanitizer-controls/.clang-tidy`

```yaml
---
# The positive controls contain deliberate memory errors and UB, so linting them produces only noise.
# This file replaces (does not inherit) the parent configs for this directory. clang-tidy refuses an
# empty check set ("no checks enabled"), so one harmless check stays on.
Checks: '-*,readability-duplicate-include'
```

### `tests/sanitizer-controls/CMakeLists.txt`

```cmake
# tests/sanitizer-controls/CMakeLists.txt: sanitizer POSITIVE CONTROLS (added only when sanitizing).
#
# Each control is a deliberately buggy program. It must make the sanitizer fire: a nonzero exit plus its
# marker on stderr. A control that stays silent means the instrumentation, runtime or options are broken
# in this env, so "sanitizer-clean" is NOT licensed (memory-safety and platform-testing skills).
# Label: sanitizer-control. Names: sanitizer-control.<n>.
#
# These binaries are never linted for the bug they contain on purpose; each carries a NOLINT with a reason.

set(_@PROJ@_ctl_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_control.cmake")

# _@PROJ@_add_control(<name> <source> MARKER <regex> [EXIT <n>] [LINK_CATCH2] [DEFAULTS_ONLY] [O0])
#   EXIT          exact exit code to require (default: any nonzero)
#   DEFAULTS_ONLY run with *SAN_OPTIONS unset, proving the compiled-in defaults (sanitizer_options.cpp)
#   O0            build at -O0 (stack residue at -O1 can hide a leak)
function(_@PROJ@_add_control name source)
  cmake_parse_arguments(PARSE_ARGV 2 C "LINK_CATCH2;DEFAULTS_ONLY;O0" "MARKER;EXIT;TARGET" "")
  if(NOT C_TARGET)
    set(C_TARGET "@PROJ@_sanctl_${name}")
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
    @PROJ@_add_sanitizer_options(${C_TARGET})
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
                   -P "${_@PROJ@_ctl_driver}")
  set_tests_properties(sanitizer-control.${name} PROPERTIES LABELS sanitizer-control TIMEOUT 120)
  if(@PROJ_UPPER@_SANITIZER_TEST_ENV)
    set_tests_properties(sanitizer-control.${name} PROPERTIES ENVIRONMENT "${@PROJ_UPPER@_SANITIZER_TEST_ENV}")
  endif()
endfunction()

if("address" IN_LIST @PROJ_UPPER@_SANITIZE)
  _@PROJ@_add_control(heap-overflow "${CMAKE_CURRENT_SOURCE_DIR}/heap_overflow.cpp"
    MARKER "ERROR: AddressSanitizer: heap-buffer-overflow" EXIT 86)
  # Same binary, *SAN_OPTIONS unset: the compiled-in defaults alone must still give exit 86.
  _@PROJ@_add_control(heap-overflow-defaults "${CMAKE_CURRENT_SOURCE_DIR}/heap_overflow.cpp"
    TARGET @PROJ@_sanctl_heap_overflow
    MARKER "ERROR: AddressSanitizer: heap-buffer-overflow" EXIT 86 DEFAULTS_ONLY)
  _@PROJ@_add_control(leak "${CMAKE_CURRENT_SOURCE_DIR}/leak.cpp"
    MARKER "ERROR: LeakSanitizer: detected memory leaks" EXIT 86 O0)
  # ...and the compiled-in __lsan_default_options alone (GCC's leak exit code is governed separately).
  _@PROJ@_add_control(leak-defaults "${CMAKE_CURRENT_SOURCE_DIR}/leak.cpp"
    TARGET @PROJ@_sanctl_leak
    MARKER "ERROR: LeakSanitizer: detected memory leaks" EXIT 86 DEFAULTS_ONLY)
  # A null dereference inside a Catch2 TEST_CASE: Catch2 installs its own SIGSEGV handler, and
  # handle_segv=2 keeps ASan's accurate report in charge.
  _@PROJ@_add_control(catch2-segv "${CMAKE_CURRENT_SOURCE_DIR}/catch2_segv.cpp"
    MARKER "ERROR: AddressSanitizer: SEGV" LINK_CATCH2)
endif()

if("undefined" IN_LIST @PROJ_UPPER@_SANITIZE)
  # GCC's UBSan prints no SUMMARY line, so the marker keys on "runtime error:".
  _@PROJ@_add_control(signed-overflow "${CMAKE_CURRENT_SOURCE_DIR}/signed_overflow.cpp"
    MARKER "runtime error: signed integer overflow" EXIT 86)
  # *SAN_OPTIONS unset: the compiled-in __ubsan_default_options alone must still give exit 86
  # (GCC's UBSan runtime reads only its own options, separately from ASan's).
  _@PROJ@_add_control(signed-overflow-defaults "${CMAKE_CURRENT_SOURCE_DIR}/signed_overflow.cpp"
    TARGET @PROJ@_sanctl_signed_overflow
    MARKER "runtime error: signed integer overflow" EXIT 86 DEFAULTS_ONLY)
endif()

if("thread" IN_LIST @PROJ_UPPER@_SANITIZE)
  _@PROJ@_add_control(data-race "${CMAKE_CURRENT_SOURCE_DIR}/data_race.cpp"
    MARKER "WARNING: ThreadSanitizer: data race" EXIT 86)
endif()
```

### `tests/sanitizer-controls/run_control.cmake`

```cmake
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
```

### `tests/sanitizer-controls/heap_overflow.cpp`

```cpp
// Positive control: a 1-byte heap overflow (argc == 1 -> p[8] on an 8-byte block). volatile and
// argc stop the optimizer from folding the bug away at -O1. Deliberately buggy; not linted (see
// .clang-tidy).
#include <cstdlib>

int main(int argc, char** /*argv*/) {
    char* volatile p = static_cast<char*>(std::malloc(8));
    p[argc + 7] = 'x';
    std::free(p);
    return 0;
}
```

### `tests/sanitizer-controls/signed_overflow.cpp`

```cpp
// Positive control: signed integer overflow (INT_MAX + argc), undefined behaviour that UBSan must
// report. Deliberately buggy; not linted (see .clang-tidy).
#include <climits>

int main(int argc, char** /*argv*/) {
    volatile int big = INT_MAX;
    const int r = big + argc;
    return r == 0 ? 1 : 0;
}
```

### `tests/sanitizer-controls/leak.cpp`

```cpp
// Positive control: a direct leak of 1337 bytes. Built at -O0 (see CMakeLists.txt) because stack
// residue at -O1 can keep the pointer reachable and hide the leak. Deliberately buggy; not linted
// (see .clang-tidy).
#include <cstdlib>

namespace {
[[gnu::noinline]] void leak() {
    void* volatile p = std::malloc(1337);
    static_cast<void>(p);
}
} // namespace

int main() {
    leak();
    return 0;
}
```

### `tests/sanitizer-controls/catch2_segv.cpp`

```cpp
// Positive control: a null dereference inside a Catch2 TEST_CASE. Catch2 installs its own POSIX
// signal handlers; with ASAN_OPTIONS=handle_segv=2 ASan's accurate "ERROR: AddressSanitizer: SEGV"
// report must still appear. If it doesn't, Catch2 is hiding crash reports in this env. UBSan's
// `null` check is switched off for the one function, so the fault reaches the signal path instead
// of being reported earlier as "runtime error: load of null pointer". Deliberately buggy; not
// linted (see .clang-tidy).
#include <catch2/catch_test_macros.hpp>

namespace {
__attribute__((no_sanitize("null"), noinline)) int read_through(const int* p) { return *p; }
} // namespace

TEST_CASE("null dereference is reported by ASan, not hidden by Catch2", "[sanitizer-control]") {
    const int* volatile p = nullptr;
    CHECK(read_through(p) == 0);
}
```

### `tests/sanitizer-controls/data_race.cpp`

```cpp
// Positive control (tsan preset only): two unsynchronized writes to one global, which TSan must
// report. The sleep orders the writes in time but is NOT synchronization, so it is still a data
// race. It makes detection reliable (without it, the report was observed to be intermittent). The
// global has external linkage so the optimizer can't delete the unread stores. Deliberately buggy;
// not linted (see .clang-tidy).
#include <chrono>
#include <thread>

int g_sanitizer_control_shared = 0;

int main() {
    std::thread t([] { g_sanitizer_control_shared = 1; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_sanitizer_control_shared = 2;
    t.join();
    return 0;
}
```

## Goldens

### `scripts/bless.sh`

```bash
#!/usr/bin/env bash
# scripts/bless.sh [<ctest -R regex>]: (re)generate E2E golden files from the CURRENT dev build.
#
#   pixi run bless                      # every GOLDEN case (initial goldens, at /setup)
#   pixi run bless '^e2e\.edge\.help$'  # one case
#
# This script is the ONLY place UPDATE_GOLDEN is set (the guard-bash hook blocks setting it anywhere
# else). The `bless` task is in permissions.ask: a human approves every bless, because a golden is the
# CLI contract. Agents request a bless (case regex + reason); they never bless to make a test pass.
# The driver writes a golden only when every other assertion of that case passed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || exit 2

if [[ -n "${CI:-}" ]]; then
    echo "bless: refusing to run in CI (goldens are blessed by a human, locally)" >&2
    exit 2
fi
if [[ $# -gt 1 ]]; then
    echo "usage: bash scripts/bless.sh [<ctest -R regex>]" >&2
    exit 2
fi

args=(-L golden)
if [[ $# -eq 1 && -n "$1" ]]; then
    args+=(-R "$1")
fi

export UPDATE_GOLDEN=1
# preset.sh takes the build-dir lock and picks the per-worktree dev-local test preset if one exists.
# CTest adds the e2e.install fixture automatically under -L/-R.
bash scripts/preset.sh test dev "${args[@]}"
rc=$?
unset UPDATE_GOLDEN

echo
echo "bless: golden changes (review them; commit them together with the change that caused them):"
git diff --stat -- tests/e2e/golden || true
git status --short --untracked-files=all -- tests/e2e/golden || true
if [[ $rc -ne 0 ]]; then
    echo "bless: ctest exited $rc. A failing case's golden is NOT written; fix the failure first." >&2
fi
exit "$rc"
```
