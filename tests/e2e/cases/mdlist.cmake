# tests/e2e/cases/mdlist.cmake: installed-binary E2E cases for `mangadex-dl mdlist`
# (tests e2e.mdlist.<case>, label e2e; GOLDEN cases also carry golden).
#
# Exit codes are exact, stdout and stderr are asserted separately, and the tool's working
# directory is {{WORK}}. The list plan's T11 adds the help case; T12 appends the rest (fixture-mode
# runs against the generated list documents of the fixture trees, see tests/e2e/inputs/README).

# ---- help ------------------------------------------------------------------------------------------
# The golden pins the synopsis, the LIST footer, the examples, the --dir-per-title line and the exit
# table with rows 4 and 7, and proves the hidden test option --api-base is absent (AC16).
cli_e2e_case(NAME help ARGS mdlist --help
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\nmangadex-dl mdlist \\[OPTIONS\\] LIST \\[DIR\\]\n"
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)

# ---- T12: every other case runs a generated list document ------------------------------------------
# Fixture mode (`--api-base file://{{FIXTURES}}/<tree>`; trees and their lists in
# tests/e2e/inputs/README) unless noted. The ids are `a0000000-0000-4000-8000-0000000000NN`: L1-L6 on
# clean, L8 on nochapters, L9 on incomplete, L10 (…0a) on bulk; no tree serves …ff (a 404).
# Item-scoped diagnostics start `list:<n>: <manga uuid>: `; list-level ones name the list id instead.
#
# No case writes outside its own {{WORK}}: every case whose DIR lies in the source checkout
# (dir-is-file, dir-per-title-row-dir-file) fails before anything is created, or is a dry run.
#
# network carries LABELS needs-libcurl (it loads the host's libcurl). It fails (never skips) wherever
# it runs; on a host without libcurl, exclude it explicitly: -E '^e2e\.mdlist\.network$'.

string(ASCII 27 _mdlist_esc)

set(_mdlist_clean_id 11111111-1111-4111-8111-111111111111)
set(_mdlist_nochapters_id 55555555-5555-4555-8555-555555555555)
set(_mdlist_incomplete_id 44444444-4444-4444-8444-444444444444)
set(_mdlist_unknown_id 66666666-6666-4666-8666-666666666666)
set(_mdlist_id_prefix a0000000-0000-4000-8000-0000000000)
set(_mdlist_l1 ${_mdlist_id_prefix}01)       # "Clean List": the clean title
set(_mdlist_l2 ${_mdlist_id_prefix}02)       # "Clean And Unknown": clean, then 6666... (a 404)
set(_mdlist_l3 ${_mdlist_id_prefix}03)       # "Empty": only the owner's `user` entry
set(_mdlist_l4 ${_mdlist_id_prefix}04)       # "Twice": clean twice
set(_mdlist_l5 ${_mdlist_id_prefix}05)       # "Bad Id": a manga relationship with id `../x`
set(_mdlist_l6 ${_mdlist_id_prefix}06)       # its document's data.id is L1's
set(_mdlist_l8 ${_mdlist_id_prefix}08)       # nochapters: 5555... (exit 5), then 6666... (exit 4)
set(_mdlist_l9 ${_mdlist_id_prefix}09)       # incomplete: 4444... (exit 6)
set(_mdlist_unknown_list ${_mdlist_id_prefix}ff)

set(_mdlist_clean_api --api-base file://{{FIXTURES}}/clean)
set(_mdlist_nochapters_api --api-base file://{{FIXTURES}}/nochapters)

# Exact stdout of the clean title into DIR `out` (written or planned): download's paths, unchanged.
set(_mdlist_clean_paths_re
    "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n$")
set(_mdlist_clean_files "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub")
# The same with --dir-per-title: the title's own directory DIR/<file stem> (AC44, AC45).
set(_mdlist_title_paths_re
    "^out/Clean Title/Clean Title - Vol\\. 01\\.epub\nout/Clean Title/Clean Title - Vol\\. 02\\.epub\n$")
set(_mdlist_title_files
    "out/Clean Title/Clean Title - Vol. 01.epub" "out/Clean Title/Clean Title - Vol. 02.epub")
# The hint every usage error (exit 2) ends with.
set(_mdlist_hint_re "Run 'mangadex-dl mdlist --help' for more information\\.\n")
# download's exit-5 and exit-4 messages, after the item prefix (byte-identical to download's).
set(_mdlist_no_chapters_msg_re
    "no downloadable chapters matched --lang en with --volumes all --loose-chapters all; try another --lang or a wider --volumes/--loose-chapters")
set(_mdlist_not_found_msg_re
    "title ${_mdlist_unknown_id} not found on MangaDex \\(HTTP 404\\); check the UUID or URL")
# L2's item 2 (not found), then the closing line.
set(_mdlist_l2_item2_re
    "mangadex-dl: error: list:2: ${_mdlist_unknown_id}: ${_mdlist_not_found_msg_re}\n")
set(_mdlist_l2_closing_re
    "mangadex-dl: error: 1 of 2 titles did not complete \\(item 2\\); see the errors above\n")
# L8 on the nochapters tree: item 1 exits 5, item 2 exits 4, then the closing line.
set(_mdlist_l8_items_re
    "mangadex-dl: error: list:1: ${_mdlist_nochapters_id}: ${_mdlist_no_chapters_msg_re}\nmangadex-dl: error: list:2: ${_mdlist_unknown_id}: ${_mdlist_not_found_msg_re}\n")
set(_mdlist_mixed_lines_re
    "${_mdlist_l8_items_re}mangadex-dl: error: 2 of 2 titles did not complete \\(items 1, 2\\); see the errors above\n")
set(_mdlist_mixed_stderr_re "^${_mdlist_mixed_lines_re}$")
# A malformed list document (exit 3): fixed text naming the list id, and no network remedy (the
# anchored regex leaves no room for `check your network ...`).
set(_mdlist_malformed_re "malformed MangaDex API response \\(custom list\\): ")

# ---- success (AC17) ----------------------------------------------------------------------------------
# L1: stdout is exactly download's paths, stderr is empty, the files are sound EPUBs and no temp file
# is left behind.
cli_e2e_case(NAME clean
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l1} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI
             EXPECT_FILES ${_mdlist_clean_files}
             EXPECT_EPUB ${_mdlist_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*")
# Every LIST form names the same list, so stdout is identical. Dry runs: the list and the title's plan
# requests are still made (the planned paths prove L1 was fetched), and nothing is created.
# The upper-case id is lowercased before the request: fixture mode serves only the lowercase file
# name, so a request with the id as typed would be a 404 (exit 4).
cli_e2e_case(NAME form-upper
             ARGS mdlist --dry-run ${_mdlist_clean_api} A0000000-0000-4000-8000-000000000001 out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
cli_e2e_case(NAME form-url-slug
             ARGS mdlist --dry-run ${_mdlist_clean_api}
                  https://mangadex.org/list/${_mdlist_l1}/clean-list out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
# The empty-slug form the site generates for a list with a non-Latin name (a trailing '/').
cli_e2e_case(NAME form-url-empty-slug
             ARGS mdlist --dry-run ${_mdlist_clean_api} https://mangadex.org/list/${_mdlist_l1}/ out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
cli_e2e_case(NAME form-www
             ARGS mdlist --dry-run ${_mdlist_clean_api} https://www.mangadex.org/list/${_mdlist_l1}
                  out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
cli_e2e_case(NAME form-no-scheme
             ARGS mdlist --dry-run ${_mdlist_clean_api} mangadex.org/list/${_mdlist_l1} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)

# ---- aggregate exit codes (AC18, AC19) -----------------------------------------------------------------
# 7: item 1 exits 5 and item 2 exits 4. Neither leaves a directory behind.
cli_e2e_case(NAME mixed-exit-7
             ARGS mdlist ${_mdlist_nochapters_api} ${_mdlist_l8} out
             EXPECT_EXIT 7 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_mixed_stderr_re}"
             EXPECT_ABSENT out)
# ...and --json still prints one document (after the stderr lines): batch's document plus `list`
# after `source`, `line` = the list position.
cli_e2e_case(NAME mixed-exit-7-json
             ARGS mdlist --json ${_mdlist_nochapters_api} ${_mdlist_l8} out
             EXPECT_EXIT 7 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"source\": \"${_mdlist_l8}\",\n  \"list\": {\n    \"id\": \"${_mdlist_l8}\",\n    \"name\": null,\n    \"url\": \"https://mangadex\\.org/list/${_mdlist_l8}\"\n  },\n  \"titles\": \\[.*\"line\": 1,[^}]*\"status\": \"no_chapters\",[^}]*\"exit_code\": 5,.*\"line\": 2,[^}]*\"status\": \"not_found\",[^}]*\"exit_code\": 4,.*\"total\": 2,"
             EXPECT_STDERR_REGEX "${_mdlist_mixed_stderr_re}"
             EXPECT_ABSENT out)
# --fail-fast: the first non-ok item (exit 5) stops the run; item 2 never runs.
set(_mdlist_fail_fast_stderr_re
    "^mangadex-dl: error: list:1: ${_mdlist_nochapters_id}: ${_mdlist_no_chapters_msg_re}\nmangadex-dl: error: stopped at item 1: --fail-fast; 1 title not run\n$")
cli_e2e_case(NAME fail-fast
             ARGS mdlist --fail-fast ${_mdlist_nochapters_api} ${_mdlist_l8} out
             EXPECT_EXIT 5 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_fail_fast_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME fail-fast-json
             ARGS mdlist --fail-fast --json ${_mdlist_nochapters_api} ${_mdlist_l8} out
             EXPECT_EXIT 5 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"line\": 2,[^}]*\"status\": \"not_run\",[^}]*\"exit_code\": null,.*\"not_run\": 1"
             EXPECT_STDERR_REGEX "${_mdlist_fail_fast_stderr_re}"
             EXPECT_ABSENT out)
# 6: one item, incomplete. Vol. 1 is written; Vol. 2 and its temp file are not left behind.
cli_e2e_case(NAME incomplete
             ARGS mdlist --api-base file://{{FIXTURES}}/incomplete ${_mdlist_l9} out
             EXPECT_EXIT 6
             EXPECT_STDOUT_REGEX "^out/Incomplete Title - Vol\\. 01\\.epub\n$"
             EXPECT_STDERR_REGEX "^mangadex-dl: error: list:1: ${_mdlist_incomplete_id}: out/Incomplete Title - Vol\\. 02\\.epub: [^\n]*HTTP 404[^\n]*\nmangadex-dl: error: list:1: ${_mdlist_incomplete_id}: 1 of 2 files failed; run the same command again to retry them\nmangadex-dl: error: 1 of 1 title did not complete \\(item 1\\); run the same command again to retry them\n$"
             EXPECT_FILES "out/Incomplete Title - Vol. 01.epub"
             EXPECT_EPUB "out/Incomplete Title - Vol. 01.epub"
             EXPECT_ABSENT "out/Incomplete Title - Vol. 02.epub"
             EXPECT_NO_GLOB "out/.*.part-*")
# 4: every non-ok item failed the same way; the clean item is still written.
cli_e2e_case(NAME uniform-not-found
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l2} out
             EXPECT_EXIT 4 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}"
             EXPECT_STDERR_REGEX "^${_mdlist_l2_item2_re}${_mdlist_l2_closing_re}$"
             EXPECT_FILES ${_mdlist_clean_files})

# ---- list-level failures: no item prefix, no JSON, nothing created (AC20) ---------------------------
# An unknown (or private) list is a 404: exit 4.
set(_mdlist_unknown_list_stderr_re
    "^mangadex-dl: error: list ${_mdlist_unknown_list} not found on MangaDex \\(HTTP 404\\): it does not exist or is private; check the id, make the list public on MangaDex, or use 'mangadex-dl download' if it is a title\n$")
cli_e2e_case(NAME unknown-list
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_unknown_list} out
             EXPECT_EXIT 4 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_unknown_list_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME unknown-list-json
             ARGS mdlist --json ${_mdlist_clean_api} ${_mdlist_unknown_list} out
             EXPECT_EXIT 4 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_unknown_list_stderr_re}"
             EXPECT_ABSENT out)
# A malformed document is exit 3: a manga relationship whose id is not a UUID (L5), or a data.id that
# is not the requested list (L6).
set(_mdlist_bad_id_stderr_re
    "^mangadex-dl: error: list ${_mdlist_l5}: ${_mdlist_malformed_re}manga relationship 'id' is not a UUID\n$")
cli_e2e_case(NAME bad-id
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l5} out
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_bad_id_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME bad-id-json
             ARGS mdlist --json ${_mdlist_clean_api} ${_mdlist_l5} out
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_bad_id_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME id-mismatch
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l6} out
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: list ${_mdlist_l6}: ${_mdlist_malformed_re}'id' is not the requested list\n$"
             EXPECT_ABSENT out)

# ---- warnings (AC21) ----------------------------------------------------------------------------------
# A list with no titles: exit 0, a warning, nothing on stdout and nothing created.
set(_mdlist_empty_stderr_re "^mangadex-dl: warning: list ${_mdlist_l3} has no titles\n$")
cli_e2e_case(NAME empty
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l3} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_empty_stderr_re}"
             EXPECT_ABSENT out)
# ...and --json prints `titles: []` with the `list` object filled in.
cli_e2e_case(NAME empty-json
             ARGS mdlist --json ${_mdlist_clean_api} ${_mdlist_l3} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"source\": \"${_mdlist_l3}\",\n  \"list\": {\n    \"id\": \"${_mdlist_l3}\",\n    \"name\": \"Empty\",\n    \"url\": \"https://mangadex\\.org/list/${_mdlist_l3}\"\n  },\n  \"titles\": \\[\\],"
             EXPECT_STDERR_REGEX "${_mdlist_empty_stderr_re}"
             EXPECT_ABSENT out)
# The same title twice: item 2 warns and does not run; one set of files; the run still succeeds.
cli_e2e_case(NAME twice
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l4} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: list:2: ${_mdlist_clean_id}: same as item 1; skipped\n$"
             EXPECT_FILES ${_mdlist_clean_files})

# ---- dry run (AC22): plans every item, creates nothing --------------------------------------------------
cli_e2e_case(NAME dry-run
             ARGS mdlist --dry-run ${_mdlist_clean_api} ${_mdlist_l1} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI GOLDEN
             EXPECT_ABSENT out)
cli_e2e_case(NAME json-dry-run
             ARGS mdlist --json --dry-run ${_mdlist_clean_api} ${_mdlist_l1} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "^{\n  \"format_version\": 1,\n  \"dry_run\": true,\n  \"source\": \"${_mdlist_l1}\",\n  \"list\": {\n    \"id\": \"${_mdlist_l1}\",\n    \"name\": \"Clean List\",\n    \"url\": \"https://mangadex\\.org/list/${_mdlist_l1}\"\n  },\n  \"titles\": \\[.*\"line\": 1,.*\"dir\": \"out\",.*\"status\": \"ok\",.*\"status\": \"planned\""
             GOLDEN
             EXPECT_ABSENT out)

# ---- 3: the list fetch cannot connect (AC23). Loads the host's libcurl. ---------------------------------
# Port 1 on loopback refuses the connection: a request error, so the network remedy follows; no item
# runs, no JSON, nothing created.
cli_e2e_case(NAME network
             ARGS mdlist --api-base http://127.0.0.1:1 ${_mdlist_l1} out
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: http://127\\.0\\.0\\.1:1/list/${_mdlist_l1}: [^\n]*; check your network connection or proxy \\(https_proxy\\) and try again\n$"
             EXPECT_ABSENT out
             TIMEOUT 300
             LABELS needs-libcurl)

# ---- exit 1: DIR itself is a regular file (AC24) --------------------------------------------------------
# A global error: the run stops at item 1 and item 2 is not run. Nothing can be created under the
# committed README, so the source tree stays untouched.
cli_e2e_case(NAME dir-is-file
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l2} {{INPUTS}}/README
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: list:1: ${_mdlist_clean_id}: cannot open directory '[^\n]*/inputs/README': Not a directory\nmangadex-dl: error: stopped at item 1: the error above affects every title; 1 title not run\n$")

# ---- usage errors: exit 2, nothing on stdout, the hint, nothing written (AC10 at e2e) -----------------
cli_e2e_case(NAME usage-title-url
             ARGS mdlist ${_mdlist_clean_api} https://mangadex.org/title/${_mdlist_clean_id} out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: LIST is a MangaDex title, not a list; use 'mangadex-dl download <title>' for one title\n${_mdlist_hint_re}$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME usage-csv-path
             ARGS mdlist ${_mdlist_clean_api} {{INPUTS}}/batch/clean.csv out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: LIST must be a MangaDex list UUID or a https://mangadex\\.org/list/<uuid> URL; for a CSV file of titles use 'mangadex-dl batch <file>'\n${_mdlist_hint_re}$"
             EXPECT_ABSENT out)
# mdlist has no --title (in either spelling), and X is never taken as DIR.
set(_mdlist_title_option_re
    "^mangadex-dl: error: mdlist has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title \\.\\.\\.'\n${_mdlist_hint_re}$")
cli_e2e_case(NAME usage-title-option
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l1} --title X
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_title_option_re}"
             EXPECT_ABSENT X)
cli_e2e_case(NAME usage-title-option-equals
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l1} out --title=X
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_mdlist_title_option_re}"
             EXPECT_ABSENT out X)

# ---- a terminal (pseudo-TTY: stdout and stderr merged; skipped with 77 without script) (AC30) --------
# The progress line carries the item counter `(1/2)` before download's `[i/n] Vol. v:`. Item 2 is not
# found at its first request, so it shows no progress. On a terminal the `error:` prefix is colored.
cli_e2e_case(NAME tty-progress
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l2} out
             TTY EXPECT_EXIT 4
             EXPECT_STDOUT_REGEX "\\(1/2\\) \\[1/2\\] Vol\\. 1: [0-5]/5 pages.*out/Clean Title - Vol\\. 01\\.epub\n.*\\(1/2\\) \\[2/2\\] Vol\\. 2: [0-2]/2 pages.*out/Clean Title - Vol\\. 02\\.epub\nmangadex-dl: [^\n]*error:[^\n]* list:2: ${_mdlist_unknown_id}: ${_mdlist_not_found_msg_re}\nmangadex-dl: [^\n]*error:[^\n]* 1 of 2 titles did not complete \\(item 2\\); see the errors above\n$"
             EXPECT_FILES ${_mdlist_clean_files})
# --json: no progress. The merged output must be exactly one JSON document, so any `(1/1)` text (or
# anything else) makes it invalid. L1, so stderr stays empty: with L2 the colored `error:` lines of
# item 2 would share the terminal with the document.
cli_e2e_case(NAME tty-json-no-progress
             ARGS mdlist --json ${_mdlist_clean_api} ${_mdlist_l1} out
             TTY EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI)
# -q: no progress. The anchored regex leaves no room for `(1/1)` text. L1, for the same reason: -q
# keeps the (colored) error lines.
cli_e2e_case(NAME tty-quiet-no-progress
             ARGS mdlist -q ${_mdlist_clean_api} ${_mdlist_l1} out
             TTY EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_clean_paths_re}" EXPECT_NO_ANSI)
# TERM=dumb: no progress and no color, with L2's two items. The anchored regex leaves no room for
# `(1/2)` text.
cli_e2e_case(NAME tty-term-dumb
             ARGS mdlist ${_mdlist_clean_api} ${_mdlist_l2} out
             TTY ENV TERM=dumb EXPECT_EXIT 4
             EXPECT_STDOUT_REGEX "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n${_mdlist_l2_item2_re}${_mdlist_l2_closing_re}$"
             EXPECT_NO_ANSI)
# Positive control: on a terminal the `error:` prefix is colored, so the NO_COLOR case means something.
cli_e2e_case(NAME tty-color-error
             ARGS mdlist ${_mdlist_nochapters_api} ${_mdlist_l8} out
             TTY EXPECT_EXIT 7 EXPECT_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: ${_mdlist_esc}\\[[0-9;]*merror:${_mdlist_esc}\\[0m list:1: ${_mdlist_nochapters_id}: ")
cli_e2e_case(NAME tty-no-color-error
             ARGS mdlist ${_mdlist_nochapters_api} ${_mdlist_l8} out
             TTY ENV NO_COLOR=1 EXPECT_EXIT 7 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "^${_mdlist_mixed_lines_re}$")

# ---- --dir-per-title (AC44, AC45, AC46) -----------------------------------------------------------------
# Every file goes under DIR/<the title's file stem>/.
cli_e2e_case(NAME dir-per-title
             ARGS mdlist --dir-per-title ${_mdlist_clean_api} ${_mdlist_l1} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_title_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI
             EXPECT_FILES ${_mdlist_title_files}
             EXPECT_EPUB ${_mdlist_title_files}
             EXPECT_ABSENT ${_mdlist_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*" "out/Clean Title/.*.part-*")
cli_e2e_case(NAME dir-per-title-dry-run
             ARGS mdlist --dry-run --dir-per-title ${_mdlist_clean_api} ${_mdlist_l1} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_mdlist_title_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
# The title's own directory is a committed regular file ({{INPUTS}}/mdlist/Clean Title): item 1 fails
# alone (exit 1, not fatal), item 2 still runs and is not found, so the aggregate is 7 and there is no
# `stopped at` (the anchored regex leaves no room for one). A dry run (plan spec issue 4), so nothing
# can be created under the source tree even if item 1 were let through.
cli_e2e_case(NAME dir-per-title-row-dir-file
             ARGS mdlist --dry-run --dir-per-title ${_mdlist_clean_api} ${_mdlist_l2} {{INPUTS}}/mdlist
             EXPECT_EXIT 7 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: list:1: ${_mdlist_clean_id}: cannot open directory '[^\n]*/inputs/mdlist/Clean Title': Not a directory\n${_mdlist_l2_item2_re}mangadex-dl: error: 2 of 2 titles would fail \\(items 1, 2\\)\n$")
