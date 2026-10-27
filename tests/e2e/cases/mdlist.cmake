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
