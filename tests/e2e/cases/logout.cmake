# tests/e2e/cases/logout.cmake: installed-binary E2E cases for `mangadex-dl logout`
# (tests e2e.logout.<case>, label e2e; GOLDEN cases also carry golden).
#
# Exit codes are exact, stdout and stderr are asserted separately, and the tool's working
# directory is {{WORK}}. The follows plan's T19 adds the help case; T20 appends the rest.

# ---- help ------------------------------------------------------------------------------------------
# The golden pins the synopsis, the footer and the exit table with rows 7-9. Its OPTIONS block is
# CLI11's stock layout: only the inherited global flags (-h, --version, --json, -q, -v, --color;
# V9c), never the hidden --api-base (AC27).
cli_e2e_case(NAME help ARGS logout --help
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "\nmangadex-dl logout \\[OPTIONS\\]\n\n\nOPTIONS:\n  -h, --help "
             EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)

# ---- T20: every other case ----------------------------------------------------------------------------
# Fixture mode (`--api-base file://{{FIXTURES}}/follows-clean`, hidden on logout). A saved login is
# seeded by `PRE_RUN login --credentials ...` (stdin /dev/null; tests/e2e/inputs/README). logout
# always exits 0 when the saved login is gone afterwards, also when it was never there or the revoke
# failed (AC24). A held session lock and an interrupted revoke are covered at the cli tier
# (login_cli_test.cpp; the driver can neither hold a lock nor signal).

set(_logout_api --api-base file://{{FIXTURES}}/follows-clean)
set(_logout_creds {{FIXTURES}}/follows-clean/creds)
set(_logout_session state/mangadex-dl/fixture-session.json)   # relative to {{WORK}}
set(_logout_removed_re
    "mangadex-dl: logged out; removed [^\n]*/state/mangadex-dl/fixture-session\\.json\n")
set(_logout_hint_re "Run 'mangadex-dl logout --help' for more information\\.\n")

# login, then logout: the revoke is sent, the file removed, 0.
cli_e2e_case(NAME after-login
             PRE_RUN login --credentials ${_logout_creds}/credentials ${_logout_api}
             ARGS logout ${_logout_api}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX "^${_logout_removed_re}$"
             EXPECT_NO_ANSI
             EXPECT_ABSENT ${_logout_session})
cli_e2e_case(NAME after-login-quiet
             PRE_RUN login --credentials ${_logout_creds}/credentials ${_logout_api}
             ARGS logout -q ${_logout_api}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY EXPECT_STDERR_EMPTY
             EXPECT_ABSENT ${_logout_session})
# No saved login (a second logout): 0, "not logged in", nothing sent.
cli_e2e_case(NAME twice
             ARGS logout ${_logout_api}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: not logged in \\(no saved login at [^\n]*/state/mangadex-dl/fixture-session\\.json\\)\n$"
             EXPECT_ABSENT ${_logout_session})
# The account whose revoke answers 503: a warning naming the revoke URL and the status, then the
# file is removed anyway, 0 (rev3 S4).
cli_e2e_case(NAME revoke-fails
             PRE_RUN login --credentials ${_logout_creds}/credentials-revoke-503 ${_logout_api}
             ARGS logout ${_logout_api}
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: could not end the session at MangaDex \\(file://[^\n]*/follows-clean/auth/realms/mangadex/protocol/openid-connect/revoke: HTTP 503 \\(server error\\)\\); the saved login was removed anyway\n${_logout_removed_re}$"
             EXPECT_ABSENT ${_logout_session})

# ---- usage errors: exit 2, the hint; a value is never echoed (rev3 S7) --------------------------------
cli_e2e_case(NAME usage-extra-word
             PRE_RUN login --credentials ${_logout_creds}/credentials ${_logout_api}
             ARGS logout hunter2 ${_logout_api}
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: logout does not take 1 more argument \\(not shown in case it holds a password\\)\n${_logout_hint_re}$"
             EXPECT_UNCHANGED ${_logout_session})
cli_e2e_case(NAME usage-api-base
             PRE_RUN login --credentials ${_logout_creds}/credentials ${_logout_api}
             ARGS logout --api-base https://example.org
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: logout logs in only to MangaDex itself; --api-base must be the official API\n${_logout_hint_re}$"
             EXPECT_UNCHANGED ${_logout_session})
