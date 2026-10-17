# tests/unit/CMakeLists.txt: Catch2 unit tests against mangadex_dl::core (tier 1).
#
# One test file per module under tests/unit/<module>/, globbed: adding a test file needs no CMake edit.
# Every discovered test gets the `unit.` name prefix and the `unit` label. The directory LABELS property
# merges with ADD_TAGS_AS_LABELS; a PROPERTIES LABELS argument would be overwritten by the tag labels.

file(GLOB_RECURSE _mangadex_dl_unit_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(mangadex_dl_unit_tests ${_mangadex_dl_unit_sources})
target_include_directories(mangadex_dl_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")   # tests/support/
target_link_libraries(mangadex_dl_unit_tests PRIVATE mangadex_dl::core Catch2::Catch2WithMain)
mangadex_dl_apply_warnings(mangadex_dl_unit_tests)
# Sanitized builds: compile the runtime defaults (exitcode=86, handle_segv=2, suppressions) into the test
# binary itself. CTest ENVIRONMENT can't be passed through catch_discover_tests(PROPERTIES): a ';' list
# would be split into separate properties.
mangadex_dl_add_sanitizer_options(mangadex_dl_unit_tests)

set_property(DIRECTORY PROPERTY LABELS unit)

# PRE_TEST discovery, explicit here so builds without the presets (plain cmake, AUR check()) get it too.
# With POST_BUILD, a sanitizer crash while listing tests would look like a build failure.
# EXTRA_ARGS must stay ONE list element (`--warn=NoAssertions`, not `--warn NoAssertions`): Catch2 3.16.1's
# CatchAddTests.cmake quotes "${extra_args}" when writing add_test(), so a multi-element list reaches the
# test binary as the single argv token `--warn;NoAssertions` and every test fails with "Unrecognised token"
# (upstream catchorg/Catch2 commit 6534dbc3ef; 3.16.0 passed ${extra_args} unquoted).
catch_discover_tests(mangadex_dl_unit_tests
  TEST_PREFIX "unit."
  DISCOVERY_MODE PRE_TEST
  EXTRA_ARGS --warn=NoAssertions
  ADD_TAGS_AS_LABELS)
