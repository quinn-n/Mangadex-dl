# mangadex-dl -- Downloads volumes and chapters from Mangadex and converts them into .epub or .pdf files.
# Manager-agnostic: only find_package(<Pkg> CONFIG REQUIRED) + imported targets, so the
# same file builds under pixi (presets), vcpkg, and a distro package (AUR, -DCMAKE_BUILD_TYPE=None).
# Serialization point: never edited by two parallel tracks in one batch.
cmake_minimum_required(VERSION 3.31...4.4)

project(mangadex_dl
  VERSION 0.1.0 # the single version source: --version, CPack and the release tag guard read it
  DESCRIPTION "Downloads volumes and chapters from Mangadex and converts them into .epub or .pdf files."
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest) # defines BUILD_TESTING (ON by default)

# ---- options (presets set these; defaults suit a plain distro build) ----------------
set(MANGADEX_DL_SANITIZE "" CACHE STRING
  "Sanitizers for the whole build: \"\" | \"address;undefined\" | \"thread\" (never shipped)")
set_property(CACHE MANGADEX_DL_SANITIZE PROPERTY STRINGS "" "address;undefined" "thread")
option(MANGADEX_DL_HARDENING "Release hardening flags (OpenSSF set, probed per compiler)" OFF)
option(MANGADEX_DL_HARDENING_EXTRA "Opt-in -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing" OFF)
option(MANGADEX_DL_WARNINGS_AS_ERRORS "Per-target COMPILE_WARNING_AS_ERROR (presets/CI: ON; packagers: OFF)" OFF)
# The release binary currently needs exactly GLIBC_2.28 (renameat2, used for no-clobber publish),
# so there is no headroom: any newer glibc symbol fails release-check.binary.
set(MANGADEX_DL_GLIBC_FLOOR "2.28" CACHE STRING "Highest GLIBC_x.y symbol version the release binary may require")
option(MANGADEX_DL_LIFETIME_SAFETY "Clang -Wlifetime-safety-permissive (advisory, probed)" OFF)
option(MANGADEX_DL_FUZZ "Build libFuzzer targets + corpus-replay tests (clang only)" OFF)
option(MANGADEX_DL_LINK_DEPFILE "Linker-written list of every file linked into the binary (release preset: ON; read by scripts/binary-packages.sh)" OFF)

if(MANGADEX_DL_HARDENING AND NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "")
  message(FATAL_ERROR
    "MANGADEX_DL_HARDENING and MANGADEX_DL_SANITIZE are mutually exclusive "
    "(release hardening and sanitizers are never combined in one build).")
endif()

# Defaults for builds that don't use the presets (the presets set them too).
if(NOT DEFINED CMAKE_CXX_EXTENSIONS)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()
if(NOT DEFINED CMAKE_CXX_SCAN_FOR_MODULES)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++ modules in this project
endif()

# ---- project-wide instrumentation (before any target) -------------------------------
# Sanitizer and hardening flags are applied project-wide by design (source-built deps get
# instrumented too); warnings / -Werror stay per-target (ProjectWarnings.cmake).
include("${PROJECT_SOURCE_DIR}/cmake/Sanitizers.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Hardening.cmake")

# Sanitized links are memory-hungry: cap concurrent links (Ninja only) unless a module
# already set a link pool.
if(NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "" AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_JOB_POOL_LINK)
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS mangadex_dl_link_pool=2)
  set(CMAKE_JOB_POOL_LINK mangadex_dl_link_pool)
endif()

# ---- dependencies --------------------------------------------------------------------
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# CMake's own FindThreads (not a package): the app calls pthread_sigmask, which glibc < 2.34 (the
# 2.28 floor) provides only in libpthread. The tool is also threaded by design.
find_package(Threads REQUIRED)

# libcurl: HEADERS ONLY, never linked. The core dlopen()s libcurl.so.4 (then
# libcurl-gnutls.so.4) at runtime and binds the few entry points it needs with dlsym, so the
# release binary keeps NEEDED within glibc (no static libcurl: there is no static OpenSSL/zlib
# to go with it) and works against whatever libcurl the host ships.
# Documented exception to find_package(<Pkg> CONFIG REQUIRED): conda-forge ships no
# CURLConfig.cmake, and FindCURL's CURL::libcurl would link the library, which is exactly what
# we avoid. find_path stays manager-agnostic: any system with curl/curl.h installed works.
# An IMPORTED target's include directories are consumed as SYSTEM (-isystem).
find_path(MANGADEX_DL_CURL_INCLUDE_DIR NAMES curl/curl.h REQUIRED
  DOC "Directory containing curl/curl.h (headers only; libcurl is dlopen()ed at runtime)")
add_library(mangadex_dl_curl_headers INTERFACE IMPORTED)
set_target_properties(mangadex_dl_curl_headers PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${MANGADEX_DL_CURL_INCLUDE_DIR}")
# dlopen/dlsym live in libdl below glibc 2.34 (the 2.28 floor); CMAKE_DL_LIBS is "dl" there.
message(STATUS "mangadex-dl: curl headers: ${MANGADEX_DL_CURL_INCLUDE_DIR}; CMAKE_DL_LIBS: ${CMAKE_DL_LIBS}")

include("${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake")

# ---- version: project(VERSION) -> one generated TU (a bump recompiles one file) ------
configure_file(
  "${PROJECT_SOURCE_DIR}/src/core/version.cpp.in"
  "${PROJECT_BINARY_DIR}/generated/version.cpp"
  @ONLY)

# ---- core library: all logic lives here (unit-testable, no CLI11) --------------------
# CONFIGURE_DEPENDS globbing: a new module/subcommand doesn't touch this file (trade-off
# documented in the build-workflow skill; the alternative is explicit lists).
file(GLOB_RECURSE mangadex_dl_core_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/core/*.cpp")
add_library(mangadex_dl_core STATIC ${mangadex_dl_core_sources} "${PROJECT_BINARY_DIR}/generated/version.cpp")
add_library(mangadex_dl::core ALIAS mangadex_dl_core)
target_compile_features(mangadex_dl_core PUBLIC cxx_std_23)
target_include_directories(mangadex_dl_core
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  PRIVATE "${PROJECT_SOURCE_DIR}/src/core")
target_link_libraries(mangadex_dl_core PUBLIC nlohmann_json::nlohmann_json)
# curl headers + libdl stay PRIVATE (the transport is an implementation detail of core; no
# curl type appears in include/mangadex_dl/). Threads is PUBLIC: the core's fetch pool and
# interrupt handling use std::thread / pthread_sigmask, so every consumer links pthread.
# A STATIC library still forwards its PRIVATE link items (libdl) to the final link.
target_link_libraries(mangadex_dl_core
  PRIVATE mangadex_dl_curl_headers ${CMAKE_DL_LIBS}
  PUBLIC Threads::Threads)
mangadex_dl_apply_warnings(mangadex_dl_core)

# ---- the executable: a thin CLI layer over the core ----------------------------------
file(GLOB mangadex_dl_cmd_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/app/cmd_*.cpp")
# Everything but main.cpp, for the in-process CLI tests (tests/CMakeLists.txt).
set(mangadex_dl_app_sources
  "${PROJECT_SOURCE_DIR}/src/app/io.cpp"
  "${PROJECT_SOURCE_DIR}/src/app/cli.cpp"
  ${mangadex_dl_cmd_sources})
add_executable(mangadex_dl_app "${PROJECT_SOURCE_DIR}/src/app/main.cpp" ${mangadex_dl_app_sources})
set_target_properties(mangadex_dl_app PROPERTIES OUTPUT_NAME mangadex-dl)
target_include_directories(mangadex_dl_app PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
target_link_libraries(mangadex_dl_app PRIVATE mangadex_dl::core CLI11::CLI11 Threads::Threads)
mangadex_dl_apply_warnings(mangadex_dl_app)
# The shipped binary carries libstdc++/libgcc statically (portable: needs only glibc). Scoped
# to this target: test executables keep the dynamic libstdc++ that conda's shared-only
# libCatch2.so uses. Distro builds (hardening OFF) link dynamically as packagers expect.
if(MANGADEX_DL_HARDENING)
  target_link_options(mangadex_dl_app PRIVATE -static-libstdc++ -static-libgcc)
endif()
mangadex_dl_add_sanitizer_options(mangadex_dl_app) # no-op unless sanitizing
# Every file the linker pulls into the binary (crt objects, static libstdc++/libgcc, ...), written
# by the linker itself as a make-style depfile. scripts/binary-packages.sh maps these files (plus
# the compiler's header deps) to the conda packages that end up in the shipped artifact. CMake's
# own CMAKE_LINK_DEPENDS_USE_LINKER cannot identify the conda compiler wrapper's linker, so the
# flag is passed explicitly. It changes no byte of the binary.
if(MANGADEX_DL_LINK_DEPFILE)
  include(CheckLinkerFlag)
  set(CMAKE_REQUIRED_QUIET ON)
  check_linker_flag(CXX "LINKER:--dependency-file=${CMAKE_CURRENT_BINARY_DIR}/linker-flag-probe.d"
                    MANGADEX_DL_HAVE_LD_DEPENDENCY_FILE)
  if(NOT MANGADEX_DL_HAVE_LD_DEPENDENCY_FILE)
    message(FATAL_ERROR "MANGADEX_DL_LINK_DEPFILE=ON but the linker does not accept --dependency-file "
                        "(GNU ld >= 2.35, lld or mold do).")
  endif()
  target_link_options(mangadex_dl_app PRIVATE
    "LINKER:--dependency-file=${CMAKE_CURRENT_BINARY_DIR}/mangadex-dl.link.d")
endif()

# ---- install (components: runtime, docs) ---------------------------------------------
include("${PROJECT_SOURCE_DIR}/cmake/Install.cmake")

# ---- tests (only when this is the top-level project) ---------------------------------
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
  add_subdirectory(tests)
endif()

# ---- packaging -------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
  include("${PROJECT_SOURCE_DIR}/cmake/Packaging.cmake")
endif()
