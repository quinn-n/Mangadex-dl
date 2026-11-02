# tests/e2e/cases/follows.cmake: installed-binary E2E cases for `mangadex-dl follows`
# (tests e2e.follows.<case>, label e2e; GOLDEN cases also carry golden).
#
# Exit codes are exact, stdout and stderr are asserted separately, and the tool's working
# directory is {{WORK}} (also HOME's parent and the XDG directories', so a saved login stays in the
# case's own directory). The follows plan's T19 adds the help and dry-run cases; T20 appends the
# rest. Fixture mode: `--api-base file://{{FIXTURES}}/<tree>` with the generated private
# credentials file `{{FIXTURES}}/<tree>/creds/credentials` (tests/e2e/inputs/README); no case
# contacts a real host, and every credential is a fixture fake.

# ---- help ------------------------------------------------------------------------------------------
# The golden pins the synopsis, every option (--status, the auth options), the LOGIN and READING
# STATUS footer, the examples and the exit table with rows 7-9, and proves the hidden test option
# --api-base is absent (AC27).
cli_e2e_case(NAME help ARGS follows --help
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\nmangadex-dl follows \\[OPTIONS\\] \\[DIR\\]\n"
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)

# ---- dry runs (AC33) ---------------------------------------------------------------------------------
# follows-clean follows the clean title only: a dry run logs in with the credentials file, reads
# the follows and plans download's two files under `out`, which is never created.
set(_follows_clean_args
    --credentials {{FIXTURES}}/follows-clean/creds/credentials
    --api-base file://{{FIXTURES}}/follows-clean)

cli_e2e_case(NAME dry-run
             ARGS follows out --dry-run ${_follows_clean_args}
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n$"
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN
             EXPECT_ABSENT out)
cli_e2e_case(NAME json-dry-run
             ARGS follows out --json --dry-run ${_follows_clean_args}
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "^{\n  \"format_version\": 1,\n  \"dry_run\": true,\n  \"source\": \"follows\",\n  \"follows\": {\n    \"username\": \"fixture-user\",\n    \"titles\": 1,\n    \"statuses\": null,\n    \"selected\": 1\n  },\n  \"titles\": \\[.*\"line\": 1,.*\"dir\": \"out\",.*\"status\": \"ok\",.*\"status\": \"planned\""
             GOLDEN
             EXPECT_ABSENT out)

# ---- T20: every other case ----------------------------------------------------------------------------
# Fixture mode throughout (`--api-base file://{{FIXTURES}}/<tree>`; the trees, accounts and creds/
# files are in tests/e2e/inputs/README). DIR is `out`. Item-scoped diagnostics start
# `follows:<n>: <manga uuid>: `. Auth failures are one fixed line with no item prefix, no JSON and no
# DIR. Every stderr regex is anchored at both ends, so it also proves that nothing else is printed:
# no network remedy where none belongs, no prompt, and no value from a credentials file or the
# command line.
#
# Not expressible here (covered at the cli tier instead):
# - `--credentials ''`: an empty ARGS item is dropped by CMake (T14, T16: auth_phase_test.cpp).
# - An interrupt (the driver cannot deliver a signal) and a session lock held by another process
#   (the driver runs one command at a time): follows_cli_test.cpp, login_cli_test.cpp,
#   auth_phase_test.cpp (AC21, AC26). With InstantClock every lock wait times out at once, so
#   "wait, then succeed" is unit-tier only (plan V32).
# - Typing at a real prompt: under TTY the driver gives script(1) stdin /dev/null, so a prompt would
#   wait until TIMEOUT. The TTY cases below therefore never reach a prompt (a complete credentials
#   file, or --no-input); AC39/AC40 stay with cli-tester and the human.

string(ASCII 27 _follows_esc)
# The fixtures root as a regex (exact paths in the messages that name a credentials file).
string(REGEX REPLACE "([][.+*?^$()|\\\\])" "\\\\\\1" _follows_fx_re "${CLI_E2E_FIXTURES_DIR}")

set(_follows_clean_id 11111111-1111-4111-8111-111111111111)
set(_follows_nochapters_id 55555555-5555-4555-8555-555555555555)
set(_follows_unknown_id 66666666-6666-4666-8666-666666666666)

# `C` (a complete credentials file) and the API base for <tree>, as two lists per tree.
foreach(_follows_tree IN ITEMS clean unknown empty twice bad-id nochapters status status-empty
                               status-bad-value status-bad-key)
  string(REPLACE "-" "_" _follows_var "${_follows_tree}")
  set(_follows_${_follows_var}_api --api-base file://{{FIXTURES}}/follows-${_follows_tree})
  set(_follows_${_follows_var}_c
      --credentials {{FIXTURES}}/follows-${_follows_tree}/creds/credentials
      ${_follows_${_follows_var}_api})
endforeach()
set(_follows_creds {{FIXTURES}}/follows-clean/creds)

# Where a fixture login is saved (relative to {{WORK}}; the driver's XDG_STATE_HOME is
# {{WORK}}/state). A fixture login never touches session.json (AC23).
set(_follows_session state/mangadex-dl/fixture-session.json)
set(_follows_real_session state/mangadex-dl/session.json)
set(_follows_seed_args login --credentials ${_follows_creds}/credentials ${_follows_clean_api})

# Exact stdout of the clean title into DIR `out` (written or planned): download's paths.
set(_follows_clean_paths_re
    "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n$")
set(_follows_clean_files "out/Clean Title - Vol. 01.epub" "out/Clean Title - Vol. 02.epub")
set(_follows_title_paths_re
    "^out/Clean Title/Clean Title - Vol\\. 01\\.epub\nout/Clean Title/Clean Title - Vol\\. 02\\.epub\n$")
set(_follows_title_files
    "out/Clean Title/Clean Title - Vol. 01.epub" "out/Clean Title/Clean Title - Vol. 02.epub")
# The hint every usage error (exit 2) ends with.
set(_follows_hint_re "Run 'mangadex-dl follows --help' for more information\\.\n")
# The auth messages (spec "Exit codes" texts; rev3 S5).
set(_follows_not_logged_in_re
    "you are not logged in to MangaDex; run 'mangadex-dl login' in a terminal \\(or pass --password-stdin\\)")
set(_follows_wrong_password_tail_re
    " \\(invalid_grant\\); check them, or log in on the website to see whether the account is locked\n")
set(_follows_not_found_msg_re
    "title ${_follows_unknown_id} not found on MangaDex \\(HTTP 404\\); check the UUID or URL")
set(_follows_no_chapters_msg_re
    "no downloadable chapters matched --lang en with --volumes all --loose-chapters all; try another --lang or a wider --volumes/--loose-chapters")
set(_follows_status_words_re
    "use reading, on_hold, plan_to_read, dropped, re_reading or completed\n")
# A secret-shaped extra: one fixed line, whatever the word held (rev3 S7).
set(_follows_secret_extra_re
    "^mangadex-dl: error: follows has no --password option: passwords and secrets never go on the command line \\(other users can see it\\); use the prompt, --password-stdin or the credentials file\n${_follows_hint_re}$")

# ---- no usable login: exit 8, nothing on stdout (also with --json), DIR not created (AC28) ---------
# A client but no password, no saved login and stdin /dev/null (no TTY): the login message.
cli_e2e_case(NAME no-session
             ARGS follows out --credentials ${_follows_creds}/credentials-client ${_follows_clean_api}
             STDIN NULL
             EXPECT_EXIT 8 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: ${_follows_not_logged_in_re}\n$"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME no-session-json
             ARGS follows out --json --credentials ${_follows_creds}/credentials-client
                  ${_follows_clean_api}
             STDIN NULL
             EXPECT_EXIT 8 EXPECT_STDOUT_EMPTY EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^mangadex-dl: error: ${_follows_not_logged_in_re}\n$"
             EXPECT_ABSENT out ${_follows_session})
# Nothing configured at all (no --credentials, an empty XDG_CONFIG_HOME): the no-client message
# names the default credentials file.
cli_e2e_case(NAME no-client
             ARGS follows out ${_follows_clean_api}
             EXPECT_EXIT 8 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: no MangaDex API client is set up; run 'mangadex-dl login' in a terminal, or put client_id and client_secret in [^\n]*/config/mangadex-dl/credentials\n$"
             EXPECT_ABSENT out ${_follows_session})

# ---- credentials file problems: exit 1, the path and a fixed reason, never a value (AC29, AC16) ----
cli_e2e_case(NAME creds-0644
             ARGS follows out --credentials ${_follows_creds}/credentials-0644 ${_follows_clean_api}
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: credentials file '${_follows_fx_re}/follows-clean/creds/credentials-0644' is readable or writable by others \\(mode 0644\\); run 'chmod 600 ${_follows_fx_re}/follows-clean/creds/credentials-0644'\n$"
             EXPECT_ABSENT out ${_follows_session})
# An explicit --credentials path that does not exist is an error, never a fallback to prompts.
cli_e2e_case(NAME creds-missing
             ARGS follows out --credentials ${_follows_creds}/no-such-file ${_follows_clean_api}
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: credentials file '${_follows_fx_re}/follows-clean/creds/no-such-file' does not exist\n$"
             EXPECT_ABSENT out ${_follows_session})

# ---- success (AC30) ------------------------------------------------------------------------------------
# F1 with the credentials file: download's paths, sound EPUBs, no temp file, stderr empty. The login
# is saved (fixture-session.json only, AC23) for the next run.
cli_e2e_case(NAME clean
             ARGS follows out ${_follows_clean_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_NO_ANSI
             EXPECT_FILES ${_follows_clean_files} ${_follows_session}
             EXPECT_EPUB ${_follows_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*"
             EXPECT_ABSENT ${_follows_real_session})
# --json: batch's document with source "follows" and the `follows` object after it.
cli_e2e_case(NAME json
             ARGS follows out --json ${_follows_clean_c}
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "^{\n  \"format_version\": 1,\n  \"dry_run\": false,\n  \"source\": \"follows\",\n  \"follows\": {\n    \"username\": \"fixture-user\",\n    \"titles\": 1,\n    \"statuses\": null,\n    \"selected\": 1\n  },\n  \"titles\": \\[\n    {\n      \"line\": 1,\n      \"manga_id\": \"${_follows_clean_id}\",\n      \"dir\": \"out\",\n      \"status\": \"ok\",.*\"status\": \"written\",.*\"total\": 1,\n      \"ok\": 1,"
             EXPECT_FILES ${_follows_clean_files}
             EXPECT_EPUB ${_follows_clean_files})
# --dir-per-title: every file under DIR/<the title's file stem>/.
cli_e2e_case(NAME dir-per-title
             ARGS follows out --dir-per-title ${_follows_clean_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_title_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_FILES ${_follows_title_files}
             EXPECT_EPUB ${_follows_title_files}
             EXPECT_ABSENT ${_follows_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*" "out/Clean Title/.*.part-*")
# A login saved by `login` (PRE_RUN, stdin /dev/null), then `follows` with no credentials at all and
# no TTY: the saved access token is still valid, so no token request is made and the session file
# is not rewritten (EXPECT_UNCHANGED), and the run downloads as above.
cli_e2e_case(NAME seeded
             PRE_RUN ${_follows_seed_args}
             ARGS follows out ${_follows_clean_api}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_FILES ${_follows_clean_files}
             EXPECT_EPUB ${_follows_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*"
             EXPECT_UNCHANGED ${_follows_session})
# -v names the saved login as the source, then the follows count and the item (AC20's lines).
cli_e2e_case(NAME seeded-verbose
             PRE_RUN ${_follows_seed_args}
             ARGS follows out --dry-run -v ${_follows_clean_api}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: logged in to MangaDex as fixture-user \\(saved login\\)\nmangadex-dl: follows: 1 title\nmangadex-dl: follows:1: ${_follows_clean_id}\nmangadex-dl: planned 2 files; 0 chapters skipped\n"
             EXPECT_ABSENT out
             EXPECT_UNCHANGED ${_follows_session})
# The same with the credentials file: a password login.
cli_e2e_case(NAME verbose
             ARGS follows out --dry-run -v ${_follows_clean_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: logged in to MangaDex as fixture-user \\(password\\)\nmangadex-dl: follows: 1 title\nmangadex-dl: follows:1: ${_follows_clean_id}\nmangadex-dl: planned 2 files; 0 chapters skipped\n"
             EXPECT_ABSENT out)

# ---- aggregates and empties (AC31) ---------------------------------------------------------------------
# F2: item 2 is not found (4); the clean item is still written.
cli_e2e_case(NAME unknown
             ARGS follows out ${_follows_unknown_c}
             EXPECT_EXIT 4 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: error: follows:2: ${_follows_unknown_id}: ${_follows_not_found_msg_re}\nmangadex-dl: error: 1 of 2 titles did not complete \\(item 2\\); see the errors above\n$"
             EXPECT_FILES ${_follows_clean_files})
# F7: item 1 exits 5 and item 2 exits 4, so the run exits 7. Neither leaves a directory behind.
cli_e2e_case(NAME nochapters
             ARGS follows out ${_follows_nochapters_c}
             EXPECT_EXIT 7 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: follows:1: ${_follows_nochapters_id}: ${_follows_no_chapters_msg_re}\nmangadex-dl: error: follows:2: ${_follows_unknown_id}: ${_follows_not_found_msg_re}\nmangadex-dl: error: 2 of 2 titles did not complete \\(items 1, 2\\); see the errors above\n$"
             EXPECT_ABSENT out)
# F3: no follows. Exit 0, one warning, nothing on stdout, DIR not created.
set(_follows_empty_stderr_re "^mangadex-dl: warning: you follow no titles on MangaDex\n$")
cli_e2e_case(NAME empty
             ARGS follows out ${_follows_empty_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_empty_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME empty-json
             ARGS follows out --json ${_follows_empty_c}
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"source\": \"follows\",\n  \"follows\": {\n    \"username\": \"fixture-user\",\n    \"titles\": 0,\n    \"statuses\": null,\n    \"selected\": 0\n  },\n  \"titles\": \\[\\],"
             EXPECT_STDERR_REGEX "${_follows_empty_stderr_re}"
             EXPECT_ABSENT out)
# F4: the same title twice. Item 2 warns and does not run; one set of files; exit 0.
cli_e2e_case(NAME twice
             ARGS follows out ${_follows_twice_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: follows:2: ${_follows_clean_id}: same as item 1; skipped\n$"
             EXPECT_FILES ${_follows_clean_files})
# F5: a follows page with a non-UUID id is malformed (3): fixed text naming the page URL, and no
# network remedy (the anchored regex leaves no room for `check your network ...`). No JSON, no DIR.
set(_follows_bad_id_stderr_re
    "^mangadex-dl: error: file://${_follows_fx_re}/follows-bad-id/api/user/follows/manga\\?limit=100&offset=0: malformed MangaDex API response \\(follows page\\): a 'data' entry's 'id' is not a UUID\n$")
cli_e2e_case(NAME bad-id
             ARGS follows out ${_follows_bad_id_c}
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_bad_id_stderr_re}"
             EXPECT_ABSENT out)
cli_e2e_case(NAME bad-id-json
             ARGS follows out --json ${_follows_bad_id_c}
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_bad_id_stderr_re}"
             EXPECT_ABSENT out)

# ---- --password-stdin (AC32) -------------------------------------------------------------------------
# The client and username from the file, the password from stdin (one line).
cli_e2e_case(NAME password-stdin
             ARGS follows out --dry-run --password-stdin
                  --credentials ${_follows_creds}/credentials-client ${_follows_clean_api}
             STDIN {{FIXTURES}}/follows-clean/creds/password.txt
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_FILES ${_follows_session}
             EXPECT_ABSENT out)
# An empty stdin is a usage error (2), checked before any request.
cli_e2e_case(NAME password-stdin-null
             ARGS follows out --password-stdin
                  --credentials ${_follows_creds}/credentials-client ${_follows_clean_api}
             STDIN NULL
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: --password-stdin was given, but stdin held no password\n${_follows_hint_re}$"
             EXPECT_ABSENT out ${_follows_session})
# A password typed after --password-stdin becomes DIR (rev3 gate: no guard). It is never repeated
# in the login error: the anchored, fixed-text regex leaves no room for `hunter2`, and the failed
# login creates no directory.
cli_e2e_case(NAME password-stdin-dir-not-echoed
             ARGS follows --password-stdin hunter2
                  --credentials ${_follows_creds}/credentials-client ${_follows_clean_api}
             STDIN {{FIXTURES}}/follows-clean/creds/password-wrong.txt
             EXPECT_EXIT 9 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: MangaDex rejected the username or password for fixture-user${_follows_wrong_password_tail_re}$"
             EXPECT_ABSENT hunter2 ${_follows_session})

# ---- the auth server refuses the details: exit 9, one fixed line, nothing saved (AC17, rev3 S5/S12) ----
# A wrong password answered 401 invalid_grant (fixture-user) and 400 invalid_grant
# (fixture-user-400): the same text, classified by the error code, never by the status.
cli_e2e_case(NAME wrong-password
             ARGS follows out --credentials ${_follows_creds}/credentials-wrong-password
                  ${_follows_clean_api}
             EXPECT_EXIT 9 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: MangaDex rejected the username or password for fixture-user${_follows_wrong_password_tail_re}$"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME wrong-password-400
             ARGS follows out --credentials ${_follows_creds}/credentials-wrong-password-400
                  ${_follows_clean_api}
             EXPECT_EXIT 9 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: MangaDex rejected the username or password for fixture-user-400${_follows_wrong_password_tail_re}$"
             EXPECT_ABSENT out ${_follows_session})
# An unapproved client (unauthorized_client): the approval wording.
cli_e2e_case(NAME unapproved-client
             ARGS follows out --credentials ${_follows_creds}/credentials-unapproved
                  ${_follows_clean_api}
             EXPECT_EXIT 9 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: MangaDex did not let the API client log in \\(unauthorized_client\\); a new personal client may need approval by MangaDex first, see https://mangadex\\.org/settings\n$"
             EXPECT_ABSENT out ${_follows_session})

# ---- the session cannot be saved: a warning, and the run still succeeds (AC22) --------------------------
# XDG_STATE_HOME is a committed regular file, so the state directory cannot exist; nothing can be
# created under it, and the dry run creates nothing else.
cli_e2e_case(NAME state-dir-file
             ARGS follows out --dry-run ${_follows_clean_c}
             ENV XDG_STATE_HOME={{INPUTS}}/README
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: could not save the MangaDex login to [^\n]*/inputs/README/mangadex-dl/fixture-session\\.json: [^\n]+; the next run will need the password again\n$"
             EXPECT_ABSENT out)

# ---- --status (AC60, AC55-AC59) ----------------------------------------------------------------------
# FS1 follows 6666..., 1111..., 7777..., 8888...; only 1111... is `reading`.
cli_e2e_case(NAME status-reading
             ARGS follows out --status reading ${_follows_status_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_FILES ${_follows_clean_files}
             EXPECT_EPUB ${_follows_clean_files}
             EXPECT_NO_GLOB "out/.*.part-*")
# reading,completed keeps 6666... (completed; not found: 4) and 1111..., in follows order.
cli_e2e_case(NAME status-json
             ARGS follows out --status reading,completed --json ${_follows_status_c}
             EXPECT_EXIT 4 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"source\": \"follows\",\n  \"follows\": {\n    \"username\": \"fixture-user\",\n    \"titles\": 4,\n    \"statuses\": \\[\n      \"reading\",\n      \"completed\"\n    \\],\n    \"selected\": 2\n  },\n  \"titles\": \\[\n    {\n      \"line\": 1,\n      \"manga_id\": \"${_follows_unknown_id}\",[^}]*\"status\": \"not_found\",[^}]*\"exit_code\": 4,.*\"line\": 2,\n      \"manga_id\": \"${_follows_clean_id}\",[^}]*\"status\": \"ok\","
             EXPECT_STDERR_REGEX "^mangadex-dl: error: follows:1: ${_follows_unknown_id}: ${_follows_not_found_msg_re}\nmangadex-dl: error: 1 of 2 titles did not complete \\(item 1\\); see the errors above\n$"
             EXPECT_FILES ${_follows_clean_files})
cli_e2e_case(NAME status-dry-run
             ARGS follows out --dry-run --status reading ${_follows_status_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
# -v: the counts, then ONE unknown-status note in the singular (rev3 S17a/b), then the item.
cli_e2e_case(NAME status-verbose
             ARGS follows out --dry-run -v --status reading ${_follows_status_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_STDERR_REGEX "^mangadex-dl: logged in to MangaDex as fixture-user \\(password\\)\nmangadex-dl: follows: 4 titles\nmangadex-dl: follows: 1 of 4 have the reading status reading\nmangadex-dl: follows: 1 title has a reading status mangadex-dl does not know \\('paused'\\) and is left out\nmangadex-dl: follows:1: ${_follows_clean_id}\nmangadex-dl: planned 2 files; 0 chapters skipped\n"
             EXPECT_ABSENT out)
# FS2: an empty status map. Exit 0, the warning, DIR not created.
cli_e2e_case(NAME status-none-selected
             ARGS follows out --status reading ${_follows_status_empty_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: none of the titles you follow have the reading status reading\n$"
             EXPECT_ABSENT out)
# FS3a/FS3b: a malformed status map is 3, fixed text, no network remedy, no JSON, no DIR (AC58).
cli_e2e_case(NAME status-bad-value
             ARGS follows out --json --status reading ${_follows_status_bad_value_c}
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: file://${_follows_fx_re}/follows-status-bad-value/api/manga/status: malformed MangaDex API response \\(reading statuses\\): a 'statuses' value is not a string\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME status-bad-key
             ARGS follows out --status reading ${_follows_status_bad_key_c}
             EXPECT_EXIT 3 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: file://${_follows_fx_re}/follows-status-bad-key/api/manga/status: malformed MangaDex API response \\(reading statuses\\): a 'statuses' key is not a UUID\n$"
             EXPECT_ABSENT out)
# Unknown words (`paused` is a value MangaDex may send, but no --status word; `all` is gone, rev2):
# exit 2 before any request.
cli_e2e_case(NAME status-bad
             ARGS follows out --status paused ${_follows_status_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: --status: unknown reading status 'paused'; ${_follows_status_words_re}${_follows_hint_re}$"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME status-all
             ARGS follows out --status all ${_follows_status_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: --status: unknown reading status 'all'; ${_follows_status_words_re}${_follows_hint_re}$"
             EXPECT_ABSENT out ${_follows_session})

# ---- usage errors: exit 2, nothing on stdout, the hint, nothing written (AC19 at e2e) -------------------
cli_e2e_case(NAME usage-api-base
             ARGS follows out --credentials ${_follows_creds}/credentials --api-base https://example.org
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: follows logs in only to MangaDex itself; --api-base must be the official API\n${_follows_hint_re}$"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME usage-uuid-dir
             ARGS follows ${_follows_clean_id} ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: DIR looks like a MangaDex id; follows takes no title or list \\(use 'mangadex-dl mdlist <list>' or 'download <title>'\\)\n${_follows_hint_re}$"
             EXPECT_ABSENT ${_follows_clean_id} ${_follows_session})
cli_e2e_case(NAME usage-title
             ARGS follows out --title X ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: follows has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title \\.\\.\\.'\n${_follows_hint_re}$"
             EXPECT_ABSENT out X ${_follows_session})
# Secret-shaped words: the fixed password message, and `hunter2` is never echoed (the anchored
# regexes leave no room for it) (rev3 S7).
cli_e2e_case(NAME usage-password
             ARGS follows out --password hunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_secret_extra_re}"
             EXPECT_ABSENT out hunter2 ${_follows_session})
cli_e2e_case(NAME usage-password-equals
             ARGS follows out --password=hunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_secret_extra_re}"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME usage-p-attached
             ARGS follows out -phunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_secret_extra_re}"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME usage-qp-attached
             ARGS follows out -qphunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_secret_extra_re}"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME usage-client-secret
             ARGS follows out --client-secret=hunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_follows_secret_extra_re}"
             EXPECT_ABSENT out ${_follows_session})
# A flag given a value: the flag's name only.
cli_e2e_case(NAME usage-no-input-value
             ARGS follows out --no-input=hunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: --no-input takes no value\n${_follows_hint_re}$"
             EXPECT_ABSENT out ${_follows_session})
# A bare extra word is counted, never shown.
cli_e2e_case(NAME usage-extra-word
             ARGS follows out hunter2 ${_follows_clean_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: follows does not take 1 more argument \\(not shown in case it holds a password\\)\n${_follows_hint_re}$"
             EXPECT_ABSENT out hunter2 ${_follows_session})

# ---- a terminal (pseudo-TTY: stdout and stderr merged; skipped with 77 without script) ---------------
# None of these reaches a prompt (see the note at the top of the T20 cases).
# The progress line carries the item counter `(1/1)` before download's `[i/n] Vol. v:`.
cli_e2e_case(NAME tty-progress
             ARGS follows out ${_follows_clean_c}
             TTY EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\\(1/1\\) \\[1/2\\] Vol\\. 1: [0-5]/5 pages.*out/Clean Title - Vol\\. 01\\.epub\n.*\\(1/1\\) \\[2/2\\] Vol\\. 2: [0-2]/2 pages.*out/Clean Title - Vol\\. 02\\.epub\n$"
             EXPECT_FILES ${_follows_clean_files})
# --json: no progress; the merged output must be exactly one JSON document.
cli_e2e_case(NAME tty-json-no-progress
             ARGS follows out --json ${_follows_clean_c}
             TTY EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI)
# TERM=dumb: no progress and no color; the anchored regex leaves no room for `(1/1)` text.
cli_e2e_case(NAME tty-term-dumb
             ARGS follows out ${_follows_clean_c}
             TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_follows_clean_paths_re}"
             EXPECT_NO_ANSI)
# --no-input on a terminal with no saved login and no password: 8 without a prompt (the anchored
# regex leaves no room for one), with the colored `error:` prefix of a terminal.
cli_e2e_case(NAME tty-no-input
             ARGS follows out --no-input --credentials ${_follows_creds}/credentials-client
                  ${_follows_clean_api}
             TTY EXPECT_EXIT 8 EXPECT_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: ${_follows_esc}\\[[0-9;]*merror:${_follows_esc}\\[0m ${_follows_not_logged_in_re}\n$"
             EXPECT_ABSENT out ${_follows_session})
cli_e2e_case(NAME tty-no-input-no-color
             ARGS follows out --no-input --credentials ${_follows_creds}/credentials-client
                  ${_follows_clean_api}
             TTY ENV NO_COLOR=1 EXPECT_EXIT 8 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: error: ${_follows_not_logged_in_re}\n$"
             EXPECT_ABSENT out ${_follows_session})
