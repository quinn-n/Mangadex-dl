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
