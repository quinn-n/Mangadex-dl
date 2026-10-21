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
#                    contains '[' and ']': the literal-path keywords must cope), update-reuse,
#                    update-json, regroup-group-b
#   PRE_RUN          rerun-unchanged (the same command twice: the pre-run writes, the main run re-runs)
#                    and every other case in the "re-runs" section
#   EXPECT_UNCHANGED rerun-unchanged (both volumes keep their bytes across the re-run), and the update,
#                    dry-run, regroup, failure, diverged and TTY re-run cases (each file the run must
#                    leave alone)
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
# docs/design/skip-existing-spec.md. Every case below first downloads a tree into `out` (the pre-run),
# then runs against a clean-* variant (tests/e2e/inputs/README) that shares the clean title's manga id.
# A variant that does not serve a chapter proves that chapter came from the existing EPUB: downloading
# it would be a 404 and exit 6. Notes and warnings go to stderr; stdout lists only the files written
# (or, in a dry run, the files that would be).
set(_download_vol1 "out/Clean Title - Vol. 01.epub")
set(_download_vol2 "out/Clean Title - Vol. 02.epub")
set(_download_vol1_re "out/Clean Title - Vol\\. 01\\.epub")
set(_download_vol2_re "out/Clean Title - Vol\\. 02\\.epub")
set(_download_ch3_id 11111111-0001-4000-8000-000000000003)
set(_download_ch4_id 11111111-0001-4000-8000-000000000004)
# The closing up-to-date note: unprefixed, uncolored, the tool name only (never `warning:`).
set(_download_note_one_re
    "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n")
set(_download_note_two_re
    "mangadex-dl: 2 files are already up to date; pass --overwrite to download them again\n")
# The diverged warning for clean-diverged over clean: Vol. 2 lost ch 3 and gained ch 4 on MangaDex.
set(_download_diverged_tail_re
    " ${_download_vol2_re}: MangaDex lists 1 chapter this file lacks \\(Ch\\. 4\\), but the file holds 1 chapter MangaDex no longer lists \\(Ch\\. 3\\); left it alone \\(pass --overwrite to rebuild it without it\\)\n")

# Up to date (criterion 7): the same command twice. The second run leaves both volumes byte-unchanged,
# lists nothing on stdout, leaves no temp file, and says exactly the plural note.
cli_e2e_case(NAME rerun-unchanged
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^${_download_note_two_re}$"
             EXPECT_NO_ANSI
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")
# ...--json reports both as `exists` + `existing: "complete"`, with `pages` = the held count.
cli_e2e_case(NAME up-to-date-json
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --json ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"path\": \"${_download_vol1_re}\",[^}]*\"status\": \"exists\",\n *\"replaced\": false,\n *\"existing\": \"complete\",\n *\"new_chapters\": \\[\\],\n *\"reused_chapters\": \\[\\],\n *\"pages\": 5,.*\"path\": \"${_download_vol2_re}\",[^}]*\"status\": \"exists\",\n *\"replaced\": false,\n *\"existing\": \"complete\",\n *\"new_chapters\": \\[\\],\n *\"reused_chapters\": \\[\\],\n *\"pages\": 2,.*\"summary\": {\n *\"planned\": 0,\n *\"written\": 0,\n *\"exists\": 2,\n *\"failed\": 0\n"
             EXPECT_STDERR_REGEX "^${_download_note_two_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}")
# ...-q hides the note.
cli_e2e_case(NAME up-to-date-quiet
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download -q ${_download_clean_api} ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_EMPTY
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}")

# Update with page reuse (criterion 17): clean, then clean-grown-reuse-only, which adds ch 4 to Vol. 2
# and serves ONLY ch 4 (and no cover images). Vol. 2 is rebuilt from the held ch 3 plus the new ch 4,
# Vol. 1 is up to date, so stdout is exactly the Vol. 02 path and stderr exactly the singular note.
cli_e2e_case(NAME update-reuse
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-grown-reuse-only ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^${_download_vol2_re}\n$"
             EXPECT_STDERR_REGEX "^${_download_note_one_re}$" EXPECT_NO_ANSI
             EXPECT_EPUB "${_download_vol2}"
             EXPECT_UNCHANGED "${_download_vol1}"
             EXPECT_NO_GLOB "out/.*.part-*")
# ...--json: Vol. 2 is `written` + `replaced: true` + `existing: "incomplete"` with ch 4 new and ch 3
# reused (the keys right after `replaced`, in this order); Vol. 1 is `exists` + `complete`.
cli_e2e_case(NAME update-json
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --json --api-base file://{{FIXTURES}}/clean-grown-reuse-only ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"dry_run\": false,.*\"path\": \"${_download_vol1_re}\",[^}]*\"status\": \"exists\",\n *\"replaced\": false,\n *\"existing\": \"complete\",\n *\"new_chapters\": \\[\\],\n *\"reused_chapters\": \\[\\],.*\"path\": \"${_download_vol2_re}\",[^}]*\"status\": \"written\",\n *\"replaced\": true,\n *\"existing\": \"incomplete\",\n *\"new_chapters\": \\[\n *\"${_download_ch4_id}\"\n *\\],\n *\"reused_chapters\": \\[\n *\"${_download_ch3_id}\"\n *\\],\n *\"pages\": 4,.*\"summary\": {\n *\"planned\": 0,\n *\"written\": 1,\n *\"exists\": 1,\n *\"failed\": 0\n"
             EXPECT_STDERR_REGEX "^${_download_note_one_re}$"
             EXPECT_EPUB "${_download_vol2}"
             EXPECT_UNCHANGED "${_download_vol1}"
             EXPECT_NO_GLOB "out/.*.part-*")

# Dry run over an existing DIR (criterion 15): clean-grown serves everything, but a dry run reads only
# the existing files' central directories and manifests. stdout is the file that would be written,
# -v says why, and nothing in DIR changes.
cli_e2e_case(NAME update-dry-run
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --dry-run -v --api-base file://{{FIXTURES}}/clean-grown ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^${_download_vol2_re}\n$" EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^mangadex-dl: planned 2 files; 0 chapters skipped\nmangadex-dl: ${_download_vol1_re}: up to date \\(2 chapters\\)\nmangadex-dl: would update ${_download_vol2_re} \\(download 1 chapter: Ch\\. 4; reuse 1\\)\n${_download_note_one_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")
# ...--json: `planned` + `replaced: true` + `incomplete` with the same lists.
cli_e2e_case(NAME update-dry-run-json
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --dry-run --json --api-base file://{{FIXTURES}}/clean-grown ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"dry_run\": true,.*\"path\": \"${_download_vol2_re}\",[^}]*\"status\": \"planned\",\n *\"replaced\": true,\n *\"existing\": \"incomplete\",\n *\"new_chapters\": \\[\n *\"${_download_ch4_id}\"\n *\\],\n *\"reused_chapters\": \\[\n *\"${_download_ch3_id}\"\n *\\],.*\"summary\": {\n *\"planned\": 1,\n *\"written\": 0,\n *\"exists\": 1,\n *\"failed\": 0\n"
             EXPECT_STDERR_REGEX "^${_download_note_one_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}")

# Regrouped (criterion 10, plan decision 5): MangaDex now lists ch 1-3 only by group B. Without
# --group the held releases are kept and nothing is rebuilt (clean-regrouped serves nothing, so any
# download would fail).
cli_e2e_case(NAME regroup-kept
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-regrouped ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^${_download_note_two_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")
# ...--group "Bravo Scans" re-picks B's releases, so both volumes are rebuilt and listed.
cli_e2e_case(NAME regroup-group-b
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --group "Bravo Scans" --api-base file://{{FIXTURES}}/clean-regrouped-served
                  ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^${_download_vol1_re}\n${_download_vol2_re}\n$"
             EXPECT_STDERR_EMPTY
             EXPECT_EPUB "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")
# ...--group "Charlie Scans" matches no group, so it re-picks nothing: both files stay up to date.
cli_e2e_case(NAME regroup-group-c
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --group "Charlie Scans" --api-base file://{{FIXTURES}}/clean-regrouped-served
                  ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^${_download_note_two_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")

# Failed update (criterion 16): clean-diverged writes Vol. 2 with ch 4 only. clean-grown-reuse-only
# then lists ch 3 + ch 4 for Vol. 2, so Vol. 2 is incomplete and ch 3 must be downloaded, but that tree
# does not serve it: the update fails (exit 6), the error line ends "(the existing file was kept)", the
# old bytes survive and no temp file is left behind. Vol. 1 is up to date.
cli_e2e_case(NAME update-failed
             PRE_RUN download --api-base file://{{FIXTURES}}/clean-diverged ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-grown-reuse-only ${_download_clean_id} out
             EXPECT_EXIT 6 EXPECT_STDOUT_EMPTY EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^mangadex-dl: error: ${_download_vol2_re}: chapter ${_download_ch3_id}: [^\n]*HTTP 404[^\n]* \\(the existing file was kept\\)\n${_download_note_one_re}mangadex-dl: error: 1 of 2 files failed; run the same command again to retry them\n$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")

# Diverged (criterion 13): clean, then clean-diverged (Vol. 2: ch 3 gone, ch 4 new). Vol. 2 is left
# alone with the diverged warning; piped stderr is plain.
cli_e2e_case(NAME diverged-warning
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-diverged ${_download_clean_id} out
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^mangadex-dl: warning:${_download_diverged_tail_re}${_download_note_one_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")

# On a terminal (criterion 20, pseudo-TTY: stdout and stderr merged; skipped with 77 without script).
# Progress is drawn only for the file being updated ([2/2], Vol. 2 now has 4 pages), then its path,
# then the note.
cli_e2e_case(NAME tty-update-progress
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-grown-reuse-only ${_download_clean_id} out
             TTY EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "^\\[2/2\\] Vol\\. 2: [0-4]/4 pages.*${_download_vol2_re}\n${_download_note_one_re}$"
             EXPECT_UNCHANGED "${_download_vol1}")
# The note is never colored, even on a terminal.
cli_e2e_case(NAME tty-up-to-date
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download ${_download_clean_api} ${_download_clean_id} out
             TTY EXPECT_EXIT 0 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "^${_download_note_two_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}")
# `warning:` is colored on a terminal (positive control), while the note on the next line stays plain
# (the anchored regex leaves no room for an escape sequence in it).
cli_e2e_case(NAME tty-diverged-warning
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-diverged ${_download_clean_id} out
             TTY EXPECT_EXIT 0 EXPECT_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: ${_download_esc}\\[[0-9]*mwarning:${_download_esc}\\[0m${_download_diverged_tail_re}${_download_note_one_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}")
# ...and plain under NO_COLOR.
cli_e2e_case(NAME tty-diverged-warning-no-color
             PRE_RUN download ${_download_clean_api} ${_download_clean_id} out
             ARGS download --api-base file://{{FIXTURES}}/clean-diverged ${_download_clean_id} out
             TTY ENV NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: warning:${_download_diverged_tail_re}${_download_note_one_re}$"
             EXPECT_UNCHANGED "${_download_vol1}" "${_download_vol2}")

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
