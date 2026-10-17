# tests/cli/CMakeLists.txt: in-process CLI tests (tier 2) through the run(args, Io&) seam.
#
# The test binary compiles the app's own sources except main.cpp: the top-level CMakeLists.txt exports
# them as mangadex_dl_app_sources (io.cpp, cli.cpp and the globbed cmd_*.cpp), so adding a subcommand needs no
# edit here. Link libraries and include dirs are read back from mangadex_dl_app, so they stay in step with it.
# Tests include app headers as "app/<header>.hpp".

if(NOT DEFINED mangadex_dl_app_sources)
  message(FATAL_ERROR "tests/cli: mangadex_dl_app_sources is not defined (set it in the top-level CMakeLists.txt)")
endif()

file(GLOB _mangadex_dl_cli_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(mangadex_dl_cli_tests ${_mangadex_dl_cli_test_sources} ${mangadex_dl_app_sources})
target_include_directories(mangadex_dl_cli_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/src"                 # "app/cli.hpp", "app/io.hpp"
  "${PROJECT_SOURCE_DIR}/src/app"             # what the app sources themselves include
  "${CMAKE_CURRENT_SOURCE_DIR}/..")           # "support/io_capture.hpp"
get_target_property(_mangadex_dl_app_libs mangadex_dl_app LINK_LIBRARIES)
if(_mangadex_dl_app_libs)
  target_link_libraries(mangadex_dl_cli_tests PRIVATE ${_mangadex_dl_app_libs})
endif()
get_target_property(_mangadex_dl_app_incs mangadex_dl_app INCLUDE_DIRECTORIES)
if(_mangadex_dl_app_incs)
  target_include_directories(mangadex_dl_cli_tests PRIVATE ${_mangadex_dl_app_incs})
endif()
target_link_libraries(mangadex_dl_cli_tests PRIVATE Catch2::Catch2WithMain)
mangadex_dl_apply_warnings(mangadex_dl_cli_tests)
mangadex_dl_add_sanitizer_options(mangadex_dl_cli_tests)

set_property(DIRECTORY PROPERTY LABELS cli)

# PRE_TEST, and EXTRA_ARGS as one `--warn=NoAssertions` element (Catch2 3.16.1 workaround): see
# tests/unit/CMakeLists.txt.
catch_discover_tests(mangadex_dl_cli_tests
  TEST_PREFIX "cli."
  DISCOVERY_MODE PRE_TEST
  EXTRA_ARGS --warn=NoAssertions
  ADD_TAGS_AS_LABELS)
