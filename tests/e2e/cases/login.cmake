# tests/e2e/cases/login.cmake: installed-binary E2E cases for `mangadex-dl login`
# (tests e2e.login.<case>, label e2e; GOLDEN cases also carry golden).
#
# Exit codes are exact, stdout and stderr are asserted separately, and the tool's working
# directory is {{WORK}}. The follows plan's T19 adds the help case; T20 appends the rest.

# ---- help ------------------------------------------------------------------------------------------
# The golden pins the synopsis, the auth options, the footer (the client, the credentials file, the
# examples) and the exit table with rows 7-9, and proves the hidden --api-base is absent (AC27).
cli_e2e_case(NAME help ARGS login --help
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\nmangadex-dl login \\[OPTIONS\\]\n"
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)

# ---- T20: every other case ----------------------------------------------------------------------------
# Fixture mode (`--api-base file://{{FIXTURES}}/follows-clean`; the accounts and the creds/ files
# are in tests/e2e/inputs/README). login writes nothing to stdout, also with --json. Every stderr
# regex is anchored at both ends, so it also proves that no prompt and no value from a credentials
# file or the command line is printed. An interrupt at a prompt (login_cli_test.cpp) and a session
# lock held elsewhere (auth_phase_test.cpp, "auth phase: a session lock held elsewhere ...") are
# covered at the cli tier, because the driver can neither signal nor hold a lock. Typing at a real
# prompt is left to cli-tester and the human.

string(ASCII 27 _login_esc)
set(_login_api --api-base file://{{FIXTURES}}/follows-clean)
set(_login_creds {{FIXTURES}}/follows-clean/creds)
set(_login_c --credentials ${_login_creds}/credentials ${_login_api})
# Relative to {{WORK}} (the driver's XDG_STATE_HOME is {{WORK}}/state): a fixture login is saved in
# fixture-session.json and never touches session.json (AC23).
set(_login_session state/mangadex-dl/fixture-session.json)
set(_login_real_session state/mangadex-dl/session.json)
set(_login_ok_stderr_re
    "^mangadex-dl: logged in to MangaDex as fixture-user; the login is saved in [^\n]*/state/mangadex-dl/fixture-session\\.json\n$")
set(_login_hint_re "Run 'mangadex-dl login --help' for more information\\.\n")
set(_login_not_logged_in_re
    "you are not logged in to MangaDex; run 'mangadex-dl login' in a terminal \\(or pass --password-stdin\\)")

# ---- success (AC24): 0, stdout empty, the "logged in ... saved in" line, the file saved ----------------
# With a complete credentials file and stdin /dev/null, so no prompt is possible or needed.
cli_e2e_case(NAME ok
             ARGS login ${_login_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_login_ok_stderr_re}"
             EXPECT_NO_ANSI
             EXPECT_FILES ${_login_session}
             EXPECT_ABSENT ${_login_real_session})
cli_e2e_case(NAME json
             ARGS login --json ${_login_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_login_ok_stderr_re}"
             EXPECT_NO_ANSI
             EXPECT_FILES ${_login_session})
cli_e2e_case(NAME quiet
             ARGS login -q ${_login_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_EMPTY
             EXPECT_FILES ${_login_session})
# login always logs in afresh: a second login over a saved one succeeds and saves again.
cli_e2e_case(NAME again
             PRE_RUN login ${_login_c}
             ARGS login ${_login_c}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_login_ok_stderr_re}"
             EXPECT_FILES ${_login_session})
# The client and username from the file, the password from stdin.
cli_e2e_case(NAME password-stdin
             ARGS login --password-stdin --credentials ${_login_creds}/credentials-client ${_login_api}
             STDIN {{FIXTURES}}/follows-clean/creds/password.txt
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "${_login_ok_stderr_re}"
             EXPECT_FILES ${_login_session})

# ---- no login possible: exit 8, nothing saved ----------------------------------------------------------
# `login` alone, stdin /dev/null, nothing configured: the no-client message names the default file.
cli_e2e_case(NAME no-tty
             ARGS login ${_login_api}
             STDIN NULL
             EXPECT_EXIT 8 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: no MangaDex API client is set up; run 'mangadex-dl login' in a terminal, or put client_id and client_secret in [^\n]*/config/mangadex-dl/credentials\n$"
             EXPECT_ABSENT ${_login_session})
# A client but no password, no TTY: the login message.
cli_e2e_case(NAME no-tty-client
             ARGS login --credentials ${_login_creds}/credentials-client ${_login_api}
             STDIN NULL
             EXPECT_EXIT 8 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: ${_login_not_logged_in_re}\n$"
             EXPECT_ABSENT ${_login_session})
cli_e2e_case(NAME password-stdin-null
             ARGS login --password-stdin --credentials ${_login_creds}/credentials-client ${_login_api}
             STDIN NULL
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: --password-stdin was given, but stdin held no password\n${_login_hint_re}$"
             EXPECT_ABSENT ${_login_session})

# ---- refused: exit 9, nothing saved (AC17) -------------------------------------------------------------
cli_e2e_case(NAME wrong-password
             ARGS login --credentials ${_login_creds}/credentials-wrong-password ${_login_api}
             EXPECT_EXIT 9 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: MangaDex rejected the username or password for fixture-user \\(invalid_grant\\); check them, or log in on the website to see whether the account is locked\n$"
             EXPECT_ABSENT ${_login_session})
cli_e2e_case(NAME unapproved-client
             ARGS login --credentials ${_login_creds}/credentials-unapproved ${_login_api}
             EXPECT_EXIT 9 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: MangaDex did not let the API client log in \\(unauthorized_client\\); a new personal client may need approval by MangaDex first, see https://mangadex\\.org/settings\n$"
             EXPECT_ABSENT ${_login_session})

# ---- the session cannot be saved: login exits 1 (AC22) --------------------------------------------------
# XDG_STATE_HOME is a committed regular file, so nothing can be created under it.
cli_e2e_case(NAME state-dir-file
             ARGS login ${_login_c}
             ENV XDG_STATE_HOME={{INPUTS}}/README
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: could not save the MangaDex login to [^\n]*/inputs/README/mangadex-dl/fixture-session\\.json: [^\n]+\n$")

# ---- usage errors: exit 2, the hint, nothing saved; a value is never echoed (rev3 S7) ------------------
cli_e2e_case(NAME usage-extra-word
             ARGS login hunter2 ${_login_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: login does not take 1 more argument \\(not shown in case it holds a password\\)\n${_login_hint_re}$"
             EXPECT_ABSENT ${_login_session})
# login has no --status (AC59): the option name is echoed, its value only counted.
cli_e2e_case(NAME usage-status
             ARGS login --status reading ${_login_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: login does not take these arguments: --status \\(and 1 other word, not shown in case it holds a password\\)\n${_login_hint_re}$"
             EXPECT_ABSENT ${_login_session})
cli_e2e_case(NAME usage-password
             ARGS login --password hunter2 ${_login_c}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: login has no --password option: passwords and secrets never go on the command line \\(other users can see it\\); use the prompt, --password-stdin or the credentials file\n${_login_hint_re}$"
             EXPECT_ABSENT ${_login_session})
cli_e2e_case(NAME usage-api-base
             ARGS login --credentials ${_login_creds}/credentials --api-base https://example.org
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: login logs in only to MangaDex itself; --api-base must be the official API\n${_login_hint_re}$"
             EXPECT_ABSENT ${_login_session})

# ---- a terminal (pseudo-TTY; skipped with 77 without script) ------------------------------------------
# A complete credentials file: no prompt, the same line as a pipe.
cli_e2e_case(NAME tty-ok
             ARGS login ${_login_c}
             TTY EXPECT_EXIT 0 EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "${_login_ok_stderr_re}"
             EXPECT_FILES ${_login_session})
# --no-input on a terminal with no password: 8 without a prompt (the anchored regex leaves no room
# for one).
cli_e2e_case(NAME tty-no-input
             ARGS login --no-input --credentials ${_login_creds}/credentials-client ${_login_api}
             TTY EXPECT_EXIT 8 EXPECT_ANSI
             EXPECT_STDOUT_REGEX "^mangadex-dl: ${_login_esc}\\[[0-9;]*merror:${_login_esc}\\[0m ${_login_not_logged_in_re}\n$"
             EXPECT_ABSENT ${_login_session})
