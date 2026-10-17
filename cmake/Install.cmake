# Install rules. Relative destinations only (GNUInstallDirs), so `--prefix` staging, the
# E2E fixture (build/<env>/<preset>/e2e-prefix), CPack and DESTDIR packaging all work.
# Components: `runtime` (the binary) and `docs` (man page, completions, license/readme).
# The E2E fixture installs them in two calls (multi-component --component is 4.4-only).
# Optional files are picked up at configure time: after adding one, reconfigure.

install(TARGETS mangadex_dl_app
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)

# Man page: an explicit man1/ destination -- install(FILES ... TYPE MAN) omits the man1/
# section dir.
if(EXISTS "${PROJECT_SOURCE_DIR}/docs/mangadex-dl.1")
  install(FILES "${PROJECT_SOURCE_DIR}/docs/mangadex-dl.1"
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT docs)
endif()

# Shell completions (hand-written, drift-tested). bash-completion looks up <tool>.bash
# (the unsuffixed name is deprecated); zsh wants _<tool>; fish <tool>.fish.
if(EXISTS "${PROJECT_SOURCE_DIR}/completions/mangadex-dl.bash")
  install(FILES "${PROJECT_SOURCE_DIR}/completions/mangadex-dl.bash"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions" COMPONENT docs)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/completions/_mangadex-dl")
  install(FILES "${PROJECT_SOURCE_DIR}/completions/_mangadex-dl"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/zsh/site-functions" COMPONENT docs)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/completions/mangadex-dl.fish")
  install(FILES "${PROJECT_SOURCE_DIR}/completions/mangadex-dl.fish"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d" COMPONENT docs)
endif()

# Docs under share/doc/mangadex-dl (GNUInstallDirs defaults to the project name, mangadex_dl). Only
# the default is replaced, so an explicit -DCMAKE_INSTALL_DOCDIR=... still wins.
if(CMAKE_INSTALL_DOCDIR STREQUAL "${CMAKE_INSTALL_DATAROOTDIR}/doc/${PROJECT_NAME}")
  set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/mangadex-dl")
endif()

foreach(doc IN ITEMS LICENSE README.md)
  if(EXISTS "${PROJECT_SOURCE_DIR}/${doc}")
    install(FILES "${PROJECT_SOURCE_DIR}/${doc}"
      DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT docs)
  endif()
endforeach()

# Never CMAKE_INSTALL_RPATH_USE_LINK_PATH (it would bake .pixi/ paths into installs). The
# dev/cov presets set CMAKE_INSTALL_RPATH to the env on purpose; release sets none.
