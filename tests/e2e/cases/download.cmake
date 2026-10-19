# tests/e2e/cases/download.cmake: installed-binary E2E cases for `mangadex-dl download`
# (tests e2e.download.<case>, label e2e; GOLDEN cases also carry golden).
#
# Every case runs in fixture mode (`--api-base file://{{FIXTURES}}/<tree>`; the trees and their
# contents are described in tests/e2e/inputs/README), except network-unreachable, which loads the
# host's libcurl. Exit codes are exact, stdout and stderr are asserted separately, and the tool's
# working directory is {{WORK}}, so the relative DIR `out` lands at {{WORK}}/out.
#
# Permanent E2E driver self-test (deferred from T2): these cases exercise every file keyword and the
# {{FIXTURES}} placeholder against real output.
#   {{FIXTURES}}     every fixture-mode case below
#   EXPECT_FILES     success-clean, json-success, layout-chapters, incomplete, webp-warning
#   EXPECT_ABSENT    dry-run-clean, json-dry-run, not-found and the usage errors (the whole DIR),
#                    layout-single (the per-volume names), incomplete (the failed file)
#   EXPECT_NO_GLOB   success-clean, layout-chapters, incomplete, webp-warning (no `.<final>.part-<hex>`
#                    temp file left behind; never with a name that contains '[')
#   EXPECT_EPUB      success-clean (both volumes), layout-single, webp-warning (the oneshot, whose name
#                    contains '[' and ']': the literal-path keywords must cope)
#   PRE_RUN          rerun-unchanged (the same command twice: the pre-run writes, the main run re-runs)
#   EXPECT_UNCHANGED rerun-unchanged (both volumes keep their bytes across the re-run)
#
# Stderr color (plan gate decision 7: stdout is data and never colored, so _edge.cmake's
# EDGE_COLOR_ARGS is empty and the color precedence is proven here, on stderr's `warning:` prefix):
#   tty-color-stderr (positive control), tty-no-color-stderr, pipe-clicolor-force-stderr,
#   no-color-beats-force-stderr, and the piped default (webp-warning, EXPECT_NO_ANSI).
#
# network-unreachable carries LABELS needs-libcurl. It fails (never skips) wherever it runs; on a host
# without libcurl, exclude it explicitly: -E '^e2e\.download\.network-unreachable$'.

string(ASCII 27 _download_esc)

set(_download_clean_id 11111111-1111-4111-8111-111111111111)
set(_download_basic_id 22222222-2222-4222-8222-222222222222)
set(_download_incomplete_id 44444444-4444-4444-8444-444444444444)
set(_download_nochapters_id 55555555-5555-4555-8555-555555555555)
set(_download_unknown_id 66666666-6666-4666-8666-666666666666)

set(_download_clean_api --api-base file://{{FIXTURES}}/clean)
set(_download_basic_api --api-base file://{{FIXTURES}}/basic)

# Exact stdout of a successful (or dry) default-layout run on the clean fixture: one path per line.
set(_download_clean_paths_re
    "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n$")
# Exact stdout of a successful default-layout run on the basic fixture.
set(_download_basic_paths_re
    "^out/Basic Title - Vol\\. 01\\.epub\nout/Basic Title - Vol\\. 02\\.epub\nout/Basic Title - Ch\\. 010\\.epub\nout/Basic Title - Ch\\. 010\\.5\\.epub\nout/Basic Title - Oneshot \\[22222222\\]\\.epub\n$")
set(_download_basic_files
    "out/Basic Title - Vol. 01.epub"
    "out/Basic Title - Vol. 02.epub"
    "out/Basic Title - Ch. 010.epub"
    "out/Basic Title - Ch. 010.5.epub"
    "out/Basic Title - Oneshot [22222222].epub")
# The skip warning for the basic fixture's external chapter (rev1, from T14).
set(_download_external_warning_re "mangadex-dl: warning: skipped Vol\\. 2 Ch\\. 3: hosted outside MangaDex\n")

# ---- help ------------------------------------------------------------------------------------------
# The golden also proves the hidden test option --api-base is absent from the help text.
cli_e2e_case(NAME help ARGS download --help
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\nmangadex-dl download \\[OPTIONS\\] MANGA \\[DIR\\]\n"
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)

# ---- success ---------------------------------------------------------------------------------------
cli_e2e_case(NAME success-clean
             ARGS download ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_download_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI
             EXPECT_FILES "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub"
             EXPECT_EPUB "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub"
             EXPECT_NO_GLOB "out/.*.part-*")

cli_e2e_case(NAME dry-run-clean
             ARGS download --dry-run ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_download_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI GOLDEN
             EXPECT_ABSENT out)

cli_e2e_case(NAME json-dry-run
             ARGS download --json --dry-run ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"dry_run\": true,.*\"status\": \"planned\""
             GOLDEN
             EXPECT_ABSENT out)

cli_e2e_case(NAME json-success
             ARGS download --json ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"path\": \"out/Clean Title - Vol\\. 01\\.epub\",[^}]*\"status\": \"written\".*\"path\": \"out/Clean Title - Vol\\. 02\\.epub\",[^}]*\"status\": \"written\".*\"written\": 2,"
             EXPECT_FILES "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub")

cli_e2e_case(NAME layout-single
             ARGS download --layout single ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^out/Clean Title\\.epub\n$" EXPECT_STDERR_EMPTY
             EXPECT_EPUB "out/Clean Title.epub"
             EXPECT_ABSENT "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub")

cli_e2e_case(NAME layout-chapters
             ARGS download --layout chapters ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "^out/Clean Title - Vol\\. 01 Ch\\. 001\\.epub\nout/Clean Title - Vol\\. 01 Ch\\. 002\\.epub\nout/Clean Title - Vol\\. 02 Ch\\. 003\\.epub\n$"
             EXPECT_STDERR_EMPTY
             EXPECT_FILES "out/Clean Title - Vol. 01 Ch. 001.epub" "out/Clean Title - Vol. 01 Ch. 002.epub"
                          "out/Clean Title - Vol. 02 Ch. 003.epub"
             EXPECT_NO_GLOB "out/.*.part-*")

# ---- re-runs (PRE_RUN prepares {{WORK}} with one earlier run) ----------------------------------------
# The same command twice: the second run leaves both volumes byte-unchanged, lists nothing on stdout and
# leaves no temp file. Stderr is deliberately not asserted yet (its note changes with skip-existing).
cli_e2e_case(NAME rerun-unchanged
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_UNCHANGED "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub"
             EXPECT_NO_GLOB "out/.*.part-*")

# ---- tool-specific exit codes ------------------------------------------------------------------------
# 4: no such title (the fixture tree has no manga document for this id).
cli_e2e_case(NAME not-found
             ARGS download ${_download_clean_api} ${_download_unknown_id} out
             EXPECT_EXIT 4 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: title ${_download_unknown_id} not found on MangaDex \\(HTTP 404\\); check the UUID or URL\n$"
             EXPECT_ABSENT out)

# 5: nothing matched the selection (only de chapters exist). The message names language and selection.
set(_download_no_chapters_re
    "^mangadex-dl: error: no downloadable chapters matched --lang en with --volumes all --loose-chapters all; try another --lang or a wider --volumes/--loose-chapters\n$")
cli_e2e_case(NAME no-chapters
             ARGS download --api-base file://{{FIXTURES}}/nochapters ${_download_nochapters_id} out
             EXPECT_EXIT 5 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_download_no_chapters_re}")
# ...and --json still prints the report document on stdout, with an empty epubs array.
cli_e2e_case(NAME no-chapters-json
             ARGS download --json --api-base file://{{FIXTURES}}/nochapters ${_download_nochapters_id} out
             EXPECT_EXIT 5 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"epubs\": \\[\\],"
             EXPECT_STDERR_REGEX "${_download_no_chapters_re}")

# 6: incomplete. Vol. 1 is written and listed; Vol. 2 references a missing page, so it fails with one
# `mangadex-dl: error: <path>: <error>` line, and neither it nor a temp file is left behind.
cli_e2e_case(NAME incomplete
             ARGS download --api-base file://{{FIXTURES}}/incomplete ${_download_incomplete_id} out
             EXPECT_EXIT 6
             EXPECT_STDOUT_REGEX "^out/Incomplete Title - Vol\\. 01\\.epub\n$"
             EXPECT_STDERR_REGEX "^mangadex-dl: error: out/Incomplete Title - Vol\\. 02\\.epub: [^\n]*HTTP 404[^\n]*\nmangadex-dl: error: 1 of 2 files failed; run the same command again to retry them\n$"
             EXPECT_FILES "out/Incomplete Title - Vol. 01.epub"
             EXPECT_ABSENT "out/Incomplete Title - Vol. 02.epub"
             EXPECT_NO_GLOB "out/.*.part-*")

# 3: network failure. Port 1 on loopback refuses the connection. This loads the host's libcurl.
cli_e2e_case(NAME network-unreachable
             ARGS download --api-base http://127.0.0.1:1 ${_download_clean_id} out
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: http://127\\.0\\.0\\.1:1/manga/${_download_clean_id}[^\n]*; check your network connection or proxy \\(https_proxy\\) and try again\n$"
             EXPECT_ABSENT out
             LABELS needs-libcurl)

# 1: DIR exists but is a regular file.
cli_e2e_case(NAME dir-is-file
             ARGS download ${_download_clean_api} ${_download_clean_id} {{INPUTS}}/README
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: cannot open directory '[^\n]*/README': Not a directory\n$")

# 1: stdout on a full device (a dry run writes only to stdout).
cli_e2e_case(NAME dev-full-dry-run
             ARGS download --dry-run ${_download_clean_api} ${_download_clean_id} out
             STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "^mangadex-dl: write error: No space left on device\n$"
             EXPECT_ABSENT out)

# ---- usage errors: exit 2, nothing on stdout, a diagnostic on stderr, nothing written ---------------
cli_e2e_case(NAME both-none
             ARGS download --volumes none --loose-chapters none ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: --volumes and --loose-chapters are both none: nothing to download\nRun 'mangadex-dl download --help' for more information.\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME bad-spec
             ARGS download --volumes 3-1 ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^mangadex-dl: error: --volumes: '3-1' [^\n]*\nRun 'mangadex-dl download --help' for more information.\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME bad-lang
             ARGS download --lang EN ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^mangadex-dl: error: --lang must be [^\n]*\nRun 'mangadex-dl download --help' for more information.\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME dir-looks-like-id
             ARGS download ${_download_clean_api} ${_download_clean_id} ${_download_basic_id}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^mangadex-dl: error: DIR looks like a manga id: only one manga per run\nRun 'mangadex-dl download --help' for more information.\n$"
             EXPECT_ABSENT ${_download_basic_id})
cli_e2e_case(NAME bad-api-base
             ARGS download --api-base ftp://x ${_download_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^mangadex-dl: error: --api-base must start with [^\n]*\nRun 'mangadex-dl download --help' for more information.\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME jobs-9
             ARGS download -j 9 ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^mangadex-dl: error: --jobs: [^\n]*9[^\n]*\nRun 'mangadex-dl download --help' for more information.\n$"
             EXPECT_ABSENT out)

# ---- warnings and -q ---------------------------------------------------------------------------------
# The basic fixture skips an external chapter and holds WebP pages in Vol. 1. Piped stderr is plain.
cli_e2e_case(NAME webp-warning
             ARGS download ${_download_basic_api} ${_download_basic_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_download_basic_paths_re}" EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^${_download_external_warning_re}.*mangadex-dl: warning: Basic Title - Vol\\. 01\\.epub: contains WebP images"
             EXPECT_FILES ${_download_basic_files}
             EXPECT_EPUB "out/Basic Title - Oneshot [22222222].epub"
             EXPECT_NO_GLOB "out/.*.part-*")
cli_e2e_case(NAME quiet
             ARGS download -q ${_download_basic_api} ${_download_basic_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_download_basic_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_FILES ${_download_basic_files})

# ---- progress on a terminal (pseudo-TTY: stdout and stderr merged; skipped with 77 without script) ---
cli_e2e_case(NAME tty-progress
             ARGS download ${_download_clean_api} ${_download_clean_id} out
             TTY EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\\[1/2\\] Vol\\. 1: [0-5]/5 pages.*out/Clean Title - Vol\\. 01\\.epub\n.*\\[2/2\\] Vol\\. 2: [0-2]/2 pages.*out/Clean Title - Vol\\. 02\\.epub\n$")
# --json: no progress line. The merged terminal output must be exactly one JSON document, so any
# `[1/2]` progress text (or anything else) makes it invalid.
cli_e2e_case(NAME tty-progress-json
             ARGS download --json ${_download_clean_api} ${_download_clean_id} out
             TTY EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI)
# TERM=dumb: no progress line. The anchored regex leaves no room for `[1/2]` text.
cli_e2e_case(NAME tty-progress-term-dumb
             ARGS download ${_download_clean_api} ${_download_clean_id} out
             TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_download_clean_paths_re}" EXPECT_NO_ANSI)

# ---- stderr color (precedence --color > NO_COLOR > CLICOLOR_FORCE > TERM=dumb > isatty) -------------
# Positive control: on a terminal the `warning:` prefix is colored, so the NO_ANSI cases mean something.
cli_e2e_case(NAME tty-color-stderr
             ARGS download ${_download_basic_api} ${_download_basic_id} out
             TTY EXPECT_EXIT 0 EXPECT_ANSI
             EXPECT_STDOUT_REGEX "mangadex-dl: ${_download_esc}\\[[0-9]*mwarning:${_download_esc}\\[0m skipped Vol\\. 2 Ch\\. 3")
cli_e2e_case(NAME tty-no-color-stderr
             ARGS download ${_download_basic_api} ${_download_basic_id} out
             TTY ENV NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "${_download_external_warning_re}")
# Piped with CLICOLOR_FORCE=1: stderr is colored; stdout (data) stays exactly the plain paths.
cli_e2e_case(NAME pipe-clicolor-force-stderr
             ARGS download ${_download_basic_api} ${_download_basic_id} out
             ENV CLICOLOR_FORCE=1 EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "${_download_basic_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: ${_download_esc}\\[[0-9]*mwarning:${_download_esc}\\[0m skipped Vol\\. 2 Ch\\. 3: hosted outside MangaDex\n")
cli_e2e_case(NAME no-color-beats-force-stderr
             ARGS download ${_download_basic_api} ${_download_basic_id} out
             ENV CLICOLOR_FORCE=1 NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "${_download_basic_paths_re}"
             EXPECT_STDERR_REGEX "^${_download_external_warning_re}")
