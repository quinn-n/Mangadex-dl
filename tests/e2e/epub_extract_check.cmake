# tests/e2e/epub_extract_check.cmake: extract one EPUB and check its required entries.
#
#   cmake -DARCHIVE=<file.epub> -DDEST=<empty-or-scratch dir> -P tests/e2e/epub_extract_check.cmake
#
# Run ONLY as a child process of run_case.cmake (EXPECT_EPUB), after the driver's byte-level checks:
# file(ARCHIVE_EXTRACT) on a corrupt archive is a fatal error, and in a child that is just a nonzero
# exit code the driver reports. Exit 0 = extracted, and `mimetype`, `META-INF/container.xml` and
# `OEBPS/content.opf` are present as files. Every other outcome is a FATAL_ERROR (nonzero exit).
# Entry names are listed first (`cmake -E tar tf`) and an absolute or `..` name fails the check
# before anything is written, so a hostile archive cannot write outside DEST.

cmake_minimum_required(VERSION 3.31...4.4)

if(NOT DEFINED ARCHIVE OR NOT DEFINED DEST OR ARCHIVE STREQUAL "" OR DEST STREQUAL "")
  message(FATAL_ERROR "epub_extract_check: pass -DARCHIVE=<file> -DDEST=<dir>")
endif()
if(NOT EXISTS "${ARCHIVE}" OR IS_DIRECTORY "${ARCHIVE}")
  message(FATAL_ERROR "epub_extract_check: ${ARCHIVE} is not a file")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E tar tf "${ARCHIVE}"
                OUTPUT_VARIABLE _listing ERROR_VARIABLE _list_err RESULT_VARIABLE _list_rc)
if(NOT (_list_rc MATCHES "^[0-9]+$" AND _list_rc EQUAL 0))
  message(FATAL_ERROR "epub_extract_check: cannot list ${ARCHIVE} (${_list_rc}): ${_list_err}")
endif()
string(REPLACE "\n" ";" _names "${_listing}")
foreach(_name IN LISTS _names)
  if(_name MATCHES "^/" OR _name MATCHES "(^|/)\\.\\.(/|$)" OR _name MATCHES "\\\\")
    message(FATAL_ERROR "epub_extract_check: unsafe entry name '${_name}' in ${ARCHIVE}")
  endif()
endforeach()

file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${DEST}")

file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${DEST}" "${DEST}/*")
if(NOT _files)
  message(FATAL_ERROR "epub_extract_check: ${ARCHIVE} extracted to nothing")
endif()
set(_missing "")
foreach(_required IN ITEMS mimetype META-INF/container.xml OEBPS/content.opf)
  if(NOT _required IN_LIST _files)
    string(APPEND _missing " ${_required}")
  endif()
endforeach()
if(NOT _missing STREQUAL "")
  list(JOIN _files ", " _shown)
  message(FATAL_ERROR "epub_extract_check: ${ARCHIVE} lacks:${_missing} (has: ${_shown})")
endif()
list(LENGTH _files _count)
message(STATUS "epub_extract_check: ${ARCHIVE}: ${_count} entries, required ones present")
