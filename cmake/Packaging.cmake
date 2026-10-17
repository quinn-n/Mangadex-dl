# CPack: a portable release tarball (TGZ). Included only when top-level.
# Version is inherited from project(VERSION) -- never set it twice.

set(CPACK_PACKAGE_NAME "mangadex-dl")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_GENERATOR TGZ) # DEB/RPM are opt-in; verify their default prefixes at use-time
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_CHECKSUM SHA256) # writes <file>.sha256 next to the archive
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mangadex_dl_arch)
set(CPACK_PACKAGE_FILE_NAME "mangadex-dl-${PROJECT_VERSION}-linux-${_mangadex_dl_arch}")
if(EXISTS "${PROJECT_SOURCE_DIR}/LICENSE")
  set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
endif()
# No CPACK_THREADS: it only affects xz/zstd, not .tar.gz.

# Informational for component-aware generators. It does NOT filter a monolithic TGZ:
# everything installed by any component -- including the Unspecified component of a fetched
# dependency's install rules -- lands in the archive. The guard is the
# `release-check.staged-files` test, which must list what CPack actually ships (the tarball,
# or a component-less `cmake --install` into a scratch prefix), never e2e-prefix (that only
# installs `runtime` + `docs`); plus EXCLUDE_FROM_ALL SYSTEM on any FetchContent dependency.
set(CPACK_COMPONENTS_ALL runtime docs)

# `make package_source` must never sweep up build trees, envs or local artifacts.
set(CPACK_SOURCE_GENERATOR TGZ)
set(CPACK_SOURCE_IGNORE_FILES
  "/\\.git/" "/build/" "/\\.pixi/" "/\\.platform-runs/" "/\\.cache/"
  "/CMakeUserPresets\\.json$" "/compile_commands\\.json$" "/\\.claude/settings\\.local\\.json$")

# A sanitized or coverage-instrumented build is never packaged: cpack itself fails.
# (scripts/package.sh refuses earlier with a clearer message.)
if(NOT "${MANGADEX_DL_SANITIZE}" STREQUAL "" OR CMAKE_CXX_FLAGS MATCHES "-fprofile-instr-generate")
  set(_mangadex_dl_refuse "${PROJECT_BINARY_DIR}/CPackRefuse.cmake")
  file(WRITE "${_mangadex_dl_refuse}"
    "message(FATAL_ERROR \"refusing to package: this build dir is sanitized or coverage-instrumented; use the release preset (pixi run package)\")\n")
  set(CPACK_PROJECT_CONFIG_FILE "${_mangadex_dl_refuse}")
endif()

include(CPack)
