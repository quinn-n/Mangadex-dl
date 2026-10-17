# tests/CMakeLists.txt: test registration for every tier. [serialization point]
#
# Added by the top-level CMakeLists.txt under `if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)`, after
# find_package(Catch2 3 CONFIG REQUIRED). Tiers and labels (cpp-testing skill):
#   unit.*                    label unit               Catch2 against mangadex_dl::core        (tests/unit/)
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

file(GLOB _mangadex_dl_e2e_case_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cases/*.cmake")
list(SORT _mangadex_dl_e2e_case_files)
foreach(_case_file IN LISTS _mangadex_dl_e2e_case_files)
  get_filename_component(_suite "${_case_file}" NAME_WE)
  string(REGEX REPLACE "^_" "" _suite "${_suite}")   # cases/_edge.cmake -> suite "edge"
  set(CLI_E2E_SUITE "${_suite}")
  include("${_case_file}")
endforeach()
unset(CLI_E2E_SUITE)

# ---- sanitizer positive controls ------------------------------------------------------------------
# A control that does not fire means "sanitizer-clean" is NOT licensed for this build.
if(NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "")
  add_subdirectory(sanitizer-controls)
endif()

# ---- release checks (hardened release build only) -------------------------------------------------
if(MANGADEX_DL_HARDENING)
  # Binary gates: PIE/RELRO/BIND_NOW/NX markers, no RPATH/RUNPATH (default env), NEEDED allowlist,
  # glibc floor. The logic lives in scripts/release-check.sh (build-workflow ci-release scaffold).
  add_test(NAME release-check.binary
           COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/release-check.sh"
                   "${CLI_E2E_INSTALLED_TOOL}" "${MANGADEX_DL_GLIBC_FLOOR}")
  set_tests_properties(release-check.binary PROPERTIES LABELS release-check FIXTURES_REQUIRED e2e_installed)

  # What a full, component-less install (= what CPack packages) stages, into a scratch prefix (never
  # e2e-prefix, which holds only the runtime + docs components). CPACK_COMPONENTS_ALL does NOT filter a monolithic
  # TGZ, so this test is the guard against stray headers, static libs or CMake configs from dependencies.
  # Allowed: bin/mangadex-dl, its man page, the three completion files and files directly under the doc dir.
  # Installing something new on purpose? Add an anchored regex here (relative to the prefix).
  set(mangadex_dl_STAGED_FILES_EXTRA_REGEX "")
  add_test(NAME release-check.staged-files
           COMMAND "${CMAKE_COMMAND}"
                   -DCLI_E2E_MODE=staged-files
                   "-DCLI_E2E_BUILD_DIR=${PROJECT_BINARY_DIR}"
                   "-DCLI_E2E_STAGE_DIR=${PROJECT_BINARY_DIR}/staged-check"
                   "-DCLI_E2E_TOOL_NAME=mangadex-dl"
                   "-DCLI_E2E_BINDIR=${CMAKE_INSTALL_BINDIR}"
                   "-DCLI_E2E_MANDIR=${CMAKE_INSTALL_MANDIR}"
                   "-DCLI_E2E_DATADIR=${CMAKE_INSTALL_DATADIR}"
                   "-DCLI_E2E_DOCDIR=${CMAKE_INSTALL_DOCDIR}"
                   "-DCLI_E2E_EXTRA_REGEX=${mangadex_dl_STAGED_FILES_EXTRA_REGEX}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/CliE2E.cmake")
  set_tests_properties(release-check.staged-files PROPERTIES LABELS release-check)
endif()
