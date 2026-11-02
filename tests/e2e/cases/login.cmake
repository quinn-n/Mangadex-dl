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
