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
