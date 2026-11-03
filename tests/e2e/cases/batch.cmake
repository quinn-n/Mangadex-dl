# tests/e2e/cases/batch.cmake: installed-binary E2E cases for `mangadex-dl batch`
# (tests e2e.batch.<case>, label e2e; GOLDEN cases also carry golden).
#
# Exit codes are exact, stdout and stderr are asserted separately, and the tool's working
# directory is {{WORK}}. The batch plan's T6 adds the help case; T7 appends the rest (fixture-mode
# runs with committed CSV lists under tests/e2e/inputs/batch/).

# ---- help ------------------------------------------------------------------------------------------
# The golden pins the synopsis, the CSV footer and the exit table with row 7, and proves the hidden
# test option --api-base is absent from the help text (AC14).
cli_e2e_case(NAME help ARGS batch --help
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\nmangadex-dl batch \\[OPTIONS\\] FILE \\[DIR\\]\n"
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)

# ---- T7: every other case runs a committed list from tests/e2e/inputs/batch/ ----------------------
# Fixture mode (`--api-base file://{{FIXTURES}}/<tree>`; trees in tests/e2e/inputs/README) unless
# noted. FILE is passed as {{INPUTS}}/batch/<list>.csv, so row-scoped diagnostics start with that
# absolute path: the regexes match it as `[^\n]*/batch/<list>\.csv:<line>: `.
#
# network-streak and network-streak-json carry LABELS needs-libcurl (they load the host's libcurl).
# They fail (never skip) wherever they run; on a host without libcurl, exclude them explicitly:
# -E '^e2e\.batch\.network-streak'.

# Configure-time guard: excel.csv must keep its UTF-8 BOM and CRLF line ends byte for byte. A checkout
# that normalized it (core.autocrlf, a lost `-text` in .gitattributes) would turn the `excel` case into
# a plain-LF test that proves nothing, so fail the configure instead.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../inputs/batch/excel.csv" _batch_excel_hex HEX LIMIT 64)
string(TOLOWER "${_batch_excel_hex}" _batch_excel_hex)
string(SUBSTRING "${_batch_excel_hex}" 0 6 _batch_excel_bom)
# Bytes 14-15 end the header line `manga,title` (11 bytes after the 3-byte BOM): hex offset 28.
string(SUBSTRING "${_batch_excel_hex}" 28 4 _batch_excel_eol)
if(NOT _batch_excel_bom STREQUAL "efbbbf" OR NOT _batch_excel_eol STREQUAL "0d0a")
  message(FATAL_ERROR "tests/e2e/inputs/batch/excel.csv lost its UTF-8 BOM or its CRLF line ends "
                      "(first bytes: ${_batch_excel_hex}). Check that .gitattributes still says "
                      "`tests/e2e/inputs/batch/*.csv -text`, then restore the file with "
                      "`git checkout -- tests/e2e/inputs/batch/`.")
endif()
unset(_batch_excel_hex)
unset(_batch_excel_bom)
unset(_batch_excel_eol)

string(ASCII 27 _batch_esc)

set(_batch_clean_id 11111111-1111-4111-8111-111111111111)
set(_batch_unknown_id 66666666-6666-4666-8666-666666666666)

set(_batch_in "{{INPUTS}}/batch")
set(_batch_clean_api --api-base file://{{FIXTURES}}/clean)
set(_batch_nochapters_api --api-base file://{{FIXTURES}}/nochapters)

# Exact stdout of the clean title into DIR `out` (written or planned): download's paths, unchanged.
set(_batch_clean_paths_re
    "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n$")
set(_batch_clean_files "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub")
# The hint every usage error (exit 2) ends with.
set(_batch_hint_re "Run 'mangadex-dl batch --help' for more information\\.\n")
# download's exit-5 and exit-4 messages, after the row prefix.
set(_batch_no_chapters_msg_re
    "no downloadable chapters matched --lang en with --volumes all --loose-chapters all; try another --lang or a wider --volumes/--loose-chapters")
set(_batch_not_found_msg_re
    "title ${_batch_unknown_id} not found on MangaDex \\(HTTP 404\\); check the UUID or URL")
# nochapters-mixed.csv on the nochapters tree: row 1 exits 5, row 2 exits 4, then the closing line.
set(_batch_mixed_stderr_re
    "^mangadex-dl: error: [^\n]*/batch/nochapters-mixed\\.csv:1: ${_batch_no_chapters_msg_re}\nmangadex-dl: error: [^\n]*/batch/nochapters-mixed\\.csv:2: ${_batch_not_found_msg_re}\nmangadex-dl: error: 2 of 2 titles did not complete \\(lines 1, 2\\); see the errors above\n$")

# ---- success (AC15) ----------------------------------------------------------------------------------
# A headerless one-column list: stdout is exactly download's paths, stderr is empty, the files are
# sound EPUBs and no temp file is left behind.
cli_e2e_case(NAME headerless-clean
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI
             EXPECT_FILES ${_batch_clean_files}
             EXPECT_EPUB ${_batch_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*")
# The same list on stdin through `-`: identical output.
cli_e2e_case(NAME stdin-clean
             ARGS batch ${_batch_clean_api} - out
             STDIN batch/clean.csv
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI
             EXPECT_FILES ${_batch_clean_files})

# ---- aggregate exit codes (AC16, AC17, AC18) -----------------------------------------------------------
# 7: one row exits 5 and one exits 4. Neither leaves a directory behind.
cli_e2e_case(NAME mixed-exit-7
             ARGS batch ${_batch_nochapters_api} ${_batch_in}/nochapters-mixed.csv out
             EXPECT_EXIT 7 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_mixed_stderr_re}"
             EXPECT_ABSENT out)
# ...and --json still prints one document (after the stderr lines), never colored.
cli_e2e_case(NAME mixed-exit-7-json
             ARGS batch --json ${_batch_nochapters_api} ${_batch_in}/nochapters-mixed.csv out
             EXPECT_EXIT 7 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"line\": 1,[^}]*\"status\": \"no_chapters\",[^}]*\"exit_code\": 5,.*\"line\": 2,[^}]*\"status\": \"not_found\",[^}]*\"exit_code\": 4,.*\"total\": 2,"
             EXPECT_STDERR_REGEX "${_batch_mixed_stderr_re}"
             EXPECT_ABSENT out)
# 4: every non-ok row failed the same way; the clean row is still written.
cli_e2e_case(NAME uniform-not-found
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean-unknown.csv out
             EXPECT_EXIT 4 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/clean-unknown\\.csv:2: ${_batch_not_found_msg_re}\nmangadex-dl: error: 1 of 2 titles did not complete \\(line 2\\); see the errors above\n$"
             EXPECT_FILES ${_batch_clean_files})
# 6: one row, incomplete. Vol. 1 is written; Vol. 2 and its temp file are not left behind.
cli_e2e_case(NAME incomplete
             ARGS batch --api-base file://{{FIXTURES}}/incomplete ${_batch_in}/incomplete.csv out
             EXPECT_EXIT 6
             EXPECT_STDOUT_REGEX "^out/Incomplete Title - Vol\\. 01\\.epub\n$"
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/incomplete\\.csv:1: out/Incomplete Title - Vol\\. 02\\.epub: [^\n]*HTTP 404[^\n]*\nmangadex-dl: error: [^\n]*/batch/incomplete\\.csv:1: 1 of 2 files failed; run the same command again to retry them\nmangadex-dl: error: 1 of 1 title did not complete \\(line 1\\); run the same command again to retry them\n$"
             EXPECT_FILES "out/Incomplete Title - Vol. 01.epub"
             EXPECT_ABSENT "out/Incomplete Title - Vol. 02.epub"
             EXPECT_NO_GLOB "out/.*.part-*")
# --fail-fast: the first non-ok row (exit 5) stops the batch; row 2 never runs.
set(_batch_fail_fast_stderr_re
    "^mangadex-dl: error: [^\n]*/batch/nochapters-fail-fast\\.csv:1: ${_batch_no_chapters_msg_re}\nmangadex-dl: error: stopped at line 1: --fail-fast; 1 title not run\n$")
cli_e2e_case(NAME fail-fast
             ARGS batch --fail-fast ${_batch_nochapters_api} ${_batch_in}/nochapters-fail-fast.csv out
             EXPECT_EXIT 5 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_fail_fast_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME fail-fast-json
             ARGS batch --fail-fast --json ${_batch_nochapters_api} ${_batch_in}/nochapters-fail-fast.csv out
             EXPECT_EXIT 5 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"line\": 2,[^}]*\"status\": \"not_run\",[^}]*\"exit_code\": null,.*\"not_run\": 1"
             EXPECT_STDERR_REGEX "${_batch_fail_fast_stderr_re}")

# ---- dry run (AC19): plans every row, creates nothing --------------------------------------------------
cli_e2e_case(NAME dry-run
             ARGS batch --dry-run ${_batch_clean_api} ${_batch_in}/clean.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI GOLDEN
             EXPECT_ABSENT out)
cli_e2e_case(NAME json-dry-run
             ARGS batch --json --dry-run ${_batch_clean_api} ${_batch_in}/clean.csv out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "^{\n  \"format_version\": 1,\n  \"dry_run\": true,.*\"status\": \"ok\",.*\"status\": \"planned\""
             GOLDEN
             EXPECT_ABSENT out)

# ---- same-path overlap: a dry run reports what the real run does -------------------------------------
# Line 3 writes a path line 2 already wrote. The dry run decides it against line 2's planned file, as
# the real run will find it: one collision line ("would already be written" in a dry run), then the
# real run's outcome. Inline regexes, no golden.
# clean-overlap.csv: line 3's Vol. 01 is line 2's, up to date.
set(_batch_overlap_dry_re
    "^mangadex-dl: warning: [^\n]*/batch/clean-overlap\\.csv:3: out/Clean Title - Vol\\. 01\\.epub would already be written by line 2; give one of them a 'title' column\nmangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n$")
set(_batch_overlap_real_re
    "^mangadex-dl: warning: [^\n]*/batch/clean-overlap\\.csv:3: out/Clean Title - Vol\\. 01\\.epub was already written by line 2; give one of them a 'title' column\nmangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n$")
cli_e2e_case(NAME overlap-dry-run
             ARGS batch --dry-run ${_batch_clean_api} ${_batch_in}/clean-overlap.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}"
             EXPECT_STDERR_REGEX "${_batch_overlap_dry_re}"
             EXPECT_NO_ANSI
             EXPECT_ABSENT out)
cli_e2e_case(NAME overlap
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean-overlap.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}"
             EXPECT_STDERR_REGEX "${_batch_overlap_real_re}"
             EXPECT_NO_ANSI
             EXPECT_FILES ${_batch_clean_files}
             EXPECT_EPUB ${_batch_clean_files})
# clean-grow.csv: line 3 updates line 2's single file, so the path is listed twice in both runs.
set(_batch_grow_dry_re
    "^mangadex-dl: warning: [^\n]*/batch/clean-grow\\.csv:3: out/Clean Title\\.epub would already be written by line 2; give one of them a 'title' column\n$")
set(_batch_grow_real_re
    "^mangadex-dl: warning: [^\n]*/batch/clean-grow\\.csv:3: out/Clean Title\\.epub was already written by line 2; give one of them a 'title' column\n$")
set(_batch_grow_paths_re "^out/Clean Title\\.epub\nout/Clean Title\\.epub\n$")
cli_e2e_case(NAME grow-dry-run
             ARGS batch --dry-run ${_batch_clean_api} ${_batch_in}/clean-grow.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_grow_paths_re}"
             EXPECT_STDERR_REGEX "${_batch_grow_dry_re}"
             EXPECT_NO_ANSI
             EXPECT_ABSENT out)
cli_e2e_case(NAME grow
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean-grow.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_grow_paths_re}"
             EXPECT_STDERR_REGEX "${_batch_grow_real_re}"
             EXPECT_NO_ANSI
             EXPECT_FILES "out/Clean Title.epub"
             EXPECT_EPUB "out/Clean Title.epub"
             EXPECT_NO_GLOB "out/.*.part-*")

# ---- the per-row `dir` column (AC19, AC20) ---------------------------------------------------------------
set(_batch_row_dir_paths_re
    "^out/series/clean/Clean Title - Vol\\. 01\\.epub\nout/series/clean/Clean Title - Vol\\. 02\\.epub\n$")
cli_e2e_case(NAME row-dir
             ARGS batch ${_batch_clean_api} ${_batch_in}/row-dir.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_row_dir_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_FILES "out/series/clean/Clean Title - Vol. 01.epub"
                          "out/series/clean/Clean Title - Vol. 02.epub"
             EXPECT_EPUB "out/series/clean/Clean Title - Vol. 01.epub"
             EXPECT_NO_GLOB "out/series/clean/.*.part-*")
cli_e2e_case(NAME row-dir-dry-run
             ARGS batch --dry-run ${_batch_clean_api} ${_batch_in}/row-dir.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_row_dir_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
# A `dir` that escapes DIR is a list error: exit 2, nothing written (not even DIR), no request made.
cli_e2e_case(NAME dir-escape
             ARGS batch ${_batch_clean_api} ${_batch_in}/dir-escape.csv out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/dir-escape\\.csv:2: dir: [^\n]+\n${_batch_hint_re}$"
             EXPECT_ABSENT out x)
cli_e2e_case(NAME dir-absolute
             ARGS batch ${_batch_clean_api} ${_batch_in}/dir-absolute.csv out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/dir-absolute\\.csv:2: dir: [^\n]+\n${_batch_hint_re}$"
             EXPECT_ABSENT out)

# ---- an Excel export (AC21): UTF-8 BOM, CRLF and a quoted title with a comma ------------------------
cli_e2e_case(NAME excel
             ARGS batch ${_batch_clean_api} ${_batch_in}/excel.csv out
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "^out/Excel, Title - Vol\\. 01\\.epub\nout/Excel, Title - Vol\\. 02\\.epub\n$"
             EXPECT_STDERR_EMPTY
             EXPECT_FILES "out/Excel, Title - Vol. 01.epub" "out/Excel, Title - Vol. 02.epub")
# An unterminated quote names the line its record starts on (3), counted across CRLF line ends.
cli_e2e_case(NAME excel-bad
             ARGS batch ${_batch_clean_api} ${_batch_in}/excel-bad.csv out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/excel-bad\\.csv:3: unterminated quote; the field that opens with '\"' on this line never closes\n${_batch_hint_re}$"
             EXPECT_ABSENT out)

# ---- exit 1: local errors (AC22) -----------------------------------------------------------------------
# An unreadable FILE is not a usage error: exit 1 and no hint line.
cli_e2e_case(NAME file-missing
             ARGS batch ${_batch_clean_api} {{WORK}}/nope.csv out
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: cannot read [^\n]*/nope\\.csv: No such file or directory\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME file-is-dir
             ARGS batch ${_batch_clean_api} ${_batch_in} out
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: cannot read [^\n]*/inputs/batch: is a directory\n$"
             EXPECT_ABSENT out)
# DIR itself is a regular file: a global error, so the batch stops at row 1 and row 2 is not run.
set(_batch_dir_is_file_stderr_re
    "^mangadex-dl: error: [^\n]*/batch/clean-unknown\\.csv:1: cannot open directory '[^\n]*/README': Not a directory\nmangadex-dl: error: stopped at line 1: the error above affects every title; 1 title not run\n$")
cli_e2e_case(NAME dir-is-file
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean-unknown.csv {{INPUTS}}/README
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_dir_is_file_stderr_re}")
cli_e2e_case(NAME dir-is-file-json
             ARGS batch --json ${_batch_clean_api} ${_batch_in}/clean-unknown.csv {{INPUTS}}/README
             EXPECT_EXIT 1 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"line\": 1,[^}]*\"status\": \"failed\",[^}]*\"exit_code\": 1,.*\"line\": 2,[^}]*\"status\": \"not_run\","
             EXPECT_STDERR_REGEX "${_batch_dir_is_file_stderr_re}")
# A row whose `dir` runs into a regular file (row 1: README/x under DIR {{INPUTS}}) fails alone: the
# next row still plans, and there is no `stopped at line` (the anchored regex leaves no room for one).
# A dry run, so nothing is ever created under the source tree.
cli_e2e_case(NAME row-dir-is-file
             ARGS batch --dry-run ${_batch_clean_api} ${_batch_in}/row-dir-file.csv {{INPUTS}}
             EXPECT_EXIT 1
             EXPECT_STDOUT_REGEX "^[^\n]*/inputs/Clean Title - Vol\\. 01\\.epub\n[^\n]*/inputs/Clean Title - Vol\\. 02\\.epub\n$"
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/row-dir-file\\.csv:2: cannot open directory '[^\n]*/README': Not a directory\nmangadex-dl: error: 1 of 2 titles would fail \\(line 2\\)\n$")
# A not-found row with a `dir` leaves no directory behind (neither DIR nor DIR/<dir>).
cli_e2e_case(NAME not-found-leaves-no-dir
             ARGS batch ${_batch_clean_api} ${_batch_in}/unknown-dir.csv out
             EXPECT_EXIT 4 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/unknown-dir\\.csv:2: ${_batch_not_found_msg_re}\nmangadex-dl: error: 1 of 1 title did not complete \\(line 2\\); see the errors above\n$"
             EXPECT_ABSENT out)

# ---- 3: three network failures in a row stop the batch (AC23). Loads the host's libcurl. ----------
# Port 1 on loopback refuses the connection; row 4 is never run.
set(_batch_network_row_re
    "check your network connection or proxy \\(https_proxy\\) and try again\n")
set(_batch_network_stderr_re
    "^mangadex-dl: error: [^\n]*/batch/four-clean\\.csv:1: http://127\\.0\\.0\\.1:1/[^\n]*${_batch_network_row_re}mangadex-dl: error: [^\n]*/batch/four-clean\\.csv:2: http://127\\.0\\.0\\.1:1/[^\n]*${_batch_network_row_re}mangadex-dl: error: [^\n]*/batch/four-clean\\.csv:3: http://127\\.0\\.0\\.1:1/[^\n]*${_batch_network_row_re}mangadex-dl: error: stopped at line 3: 3 titles in a row failed with network errors; 1 title not run\n$")
cli_e2e_case(NAME network-streak
             ARGS batch --api-base http://127.0.0.1:1 ${_batch_in}/four-clean.csv out
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_network_stderr_re}"
             EXPECT_ABSENT out
             TIMEOUT 300
             LABELS needs-libcurl)
cli_e2e_case(NAME network-streak-json
             ARGS batch --json --api-base http://127.0.0.1:1 ${_batch_in}/four-clean.csv out
             EXPECT_EXIT 3 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"line\": 3,[^}]*\"status\": \"network\",[^}]*\"exit_code\": 3,.*\"line\": 4,[^}]*\"status\": \"not_run\","
             EXPECT_STDERR_REGEX "${_batch_network_stderr_re}"
             EXPECT_ABSENT out
             TIMEOUT 300
             LABELS needs-libcurl)

# ---- usage errors: exit 2, nothing on stdout, the hint, nothing written (AC11) -----------------------
cli_e2e_case(NAME usage-file-looks-like-id
             ARGS batch ${_batch_clean_api} ${_batch_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: FILE looks like a MangaDex title; use 'mangadex-dl download <title>' for one title\n${_batch_hint_re}$"
             EXPECT_ABSENT out)
# --title is a per-row column only: on the command line it says so (in every spelling), and X is
# never taken as DIR.
set(_batch_title_option_re
    "^mangadex-dl: error: batch has no --title option; give each row a 'title' column instead\n${_batch_hint_re}$")
cli_e2e_case(NAME usage-title-option
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean.csv --title X
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_title_option_re}"
             EXPECT_ABSENT X)
cli_e2e_case(NAME usage-title-option-after-dir
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean.csv out --title X
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_title_option_re}"
             EXPECT_ABSENT out X)
cli_e2e_case(NAME usage-title-option-equals
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean.csv out --title=X
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_batch_title_option_re}"
             EXPECT_ABSENT out)
# A headerless list whose first line is a chapter URL (not a title URL): one message that names
# both readings (a title, or a header) and lists the known columns. No request, nothing written.
cli_e2e_case(NAME chapter-url-first-line
             ARGS batch ${_batch_clean_api} ${_batch_in}/chapter-url.csv out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/chapter-url\\.csv:1: 'https://mangadex\\.org/chapter/0a1\\.\\.\\.' is not a MangaDex title UUID or https://mangadex\\.org/title/<uuid> URL, nor a known column \\(manga, title, dir, volumes, loose-chapters, layout, lang, group, direction, note\\)\n${_batch_hint_re}$"
             EXPECT_ABSENT out)

# ---- warnings -----------------------------------------------------------------------------------------
# A row identical to an earlier one warns and does not run; the batch still succeeds.
cli_e2e_case(NAME duplicate
             ARGS batch ${_batch_clean_api} ${_batch_in}/two-clean.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: [^\n]*/batch/two-clean\\.csv:2: same as line 1; skipped\n$"
             EXPECT_FILES ${_batch_clean_files})
# Only comments and blank lines: exit 0, a warning, nothing on stdout and nothing created.
cli_e2e_case(NAME comments-only
             ARGS batch ${_batch_clean_api} ${_batch_in}/comments-only.csv out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: [^\n]*/batch/comments-only\\.csv: lists no titles\n$"
             EXPECT_ABSENT out)

# ---- a terminal (pseudo-TTY: stdout and stderr merged; skipped with 77 without script) (AC30) --------
# The progress line carries the row counter `(1/1)` before download's `[i/n] Vol. v:`.
cli_e2e_case(NAME tty-progress
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean.csv out
             TTY EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\\(1/1\\) \\[1/2\\] Vol\\. 1: [0-5]/5 pages.*out/Clean Title - Vol\\. 01\\.epub\n.*\\(1/1\\) \\[2/2\\] Vol\\. 2: [0-2]/2 pages.*out/Clean Title - Vol\\. 02\\.epub\n$"
             EXPECT_FILES ${_batch_clean_files})
# --json: no progress. The merged output must be exactly one JSON document, so any `(1/1)` text
# (or anything else) makes it invalid.
cli_e2e_case(NAME tty-json-no-progress
             ARGS batch --json ${_batch_clean_api} ${_batch_in}/clean.csv out
             TTY EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI)
# -q and TERM=dumb: no progress. The anchored regex leaves no room for `(1/1)` text.
cli_e2e_case(NAME tty-quiet-no-progress
             ARGS batch -q ${_batch_clean_api} ${_batch_in}/clean.csv out
             TTY EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}" EXPECT_NO_ANSI)
cli_e2e_case(NAME tty-term-dumb
             ARGS batch ${_batch_clean_api} ${_batch_in}/clean.csv out
             TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_batch_clean_paths_re}" EXPECT_NO_ANSI)
# Positive control: on a terminal the `error:` prefix is colored, so the NO_COLOR case means something.
cli_e2e_case(NAME tty-color-error
             ARGS batch ${_batch_nochapters_api} ${_batch_in}/nochapters-mixed.csv out
             TTY EXPECT_EXIT 7 EXPECT_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: ${_batch_esc}\\[[0-9;]*merror:${_batch_esc}\\[0m [^\n]*/batch/nochapters-mixed\\.csv:1: ")
cli_e2e_case(NAME tty-no-color-error
             ARGS batch ${_batch_nochapters_api} ${_batch_in}/nochapters-mixed.csv out
             TTY ENV NO_COLOR=1 EXPECT_EXIT 7 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: error: [^\n]*/batch/nochapters-mixed\\.csv:1: ")
