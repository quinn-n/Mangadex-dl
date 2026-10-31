# tests/e2e/cases/_edge.cmake: the MANDATORY edge-case suite (tests e2e.edge.<case>, labels e2e;edge).
#
# Every CLI built with this studio passes these against its installed binary. Generic cases use only
# --help/--version and a usage error. The knobs below point the rest at real subcommands; keep them in
# step with the CLI surface when it changes. Deleting a case here is a CLI-contract change and needs
# cli-ux-reviewer + quality-reviewer sign-off.

# ---- per-project knobs ----------------------------------------------------------------------------
# REQUIRED: a subcommand invocation that writes well over 64 KiB of NEWLINE-separated lines to stdout.
# The SIGPIPE cases need the pipe to fill. Without newlines, `head -n 1` would read everything and the
# case would prove nothing. The bulk fixture (4000 loose chapters with long titles, no images) makes
# a dry run with one file per chapter print well over 128 KiB of planned paths, one per line.
set(EDGE_BULK_ARGS download --dry-run --layout chapters --api-base file://{{FIXTURES}}/bulk
    33333333-3333-4333-8333-333333333333 out)
# An invocation that writes exactly one JSON document to stdout (empty = the tool has no --json surface).
set(EDGE_JSON_ARGS download --json --dry-run --api-base file://{{FIXTURES}}/clean
    11111111-1111-4111-8111-111111111111)
# An invocation whose stdout contains its LAST argument verbatim (for UTF-8 and locale round trips; empty = none).
# The appended argument becomes DIR, and a dry run prints the planned paths under it.
set(EDGE_ECHO_ARGS download --dry-run --api-base file://{{FIXTURES}}/clean
    11111111-1111-4111-8111-111111111111)
# An invocation that colors stdout when stdout is a TTY (empty = the tool never colors stdout).
# Empty: stdout is data and is never colored (design "Color"; plan gate decision 7). Only stderr's
# `warning:`/`error:` prefixes are colored; tests/e2e/cases/download.cmake covers that.
set(EDGE_COLOR_ARGS "")

if(NOT EDGE_BULK_ARGS)
  message(FATAL_ERROR "tests/e2e/cases/_edge.cmake: EDGE_BULK_ARGS is required (the SIGPIPE cases are mandatory)")
endif()

# ---- help / version (goldens are human-blessed: pixi run bless) ------------------------------------
cli_e2e_case(NAME help ARGS --help
             EXPECT_EXIT 0 EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)
cli_e2e_case(NAME version ARGS --version
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^mangadex-dl [0-9]+\\.[0-9]+\\.[0-9]+" EXPECT_STDERR_EMPTY GOLDEN)

# ---- usage errors: exit 2, nothing on stdout, never a raw CLI11 code (100-114) ----------------------
cli_e2e_case(NAME usage-unknown-option ARGS --definitely-not-an-option
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")
cli_e2e_case(NAME usage-invalid-utf8 ARGS "{{INVALID_UTF8}}"
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")
cli_e2e_case(NAME usage-invalid-utf8-lc-c ARGS "{{INVALID_UTF8}}" ENV LC_ALL=C LANG=C
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")

# ---- stdin -----------------------------------------------------------------------------------------
cli_e2e_case(NAME stdin-null ARGS --version STDIN NULL
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^mangadex-dl " EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME stdin-closed ARGS --version STDIN CLOSED
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^mangadex-dl " EXPECT_STDERR_EMPTY)

# ---- write errors: stdout on a full device -> exit 1 and a "write error" diagnostic -----------------
cli_e2e_case(NAME dev-full-version ARGS --version STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "write error")
cli_e2e_case(NAME dev-full-help ARGS --help STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "write error")
# The reason is the errno text (strerror_r), exactly one diagnostic line. The hermetic LC_ALL=C.UTF-8
# keeps the text English.
cli_e2e_case(NAME dev-full-reason ARGS --version STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "^mangadex-dl: write error: No space left on device\n$")
# The same reason when the failure happens mid-run: the bulk output (about 450 KiB) overflows stdout's
# 4 KiB stdio buffer long before exit, that write fails, glibc drops the buffer, and later output is
# discarded, so the final flush has nothing to write. The errno comes from the first failed write.
cli_e2e_case(NAME dev-full-bulk ARGS ${EDGE_BULK_ARGS} STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "^mangadex-dl: write error: No space left on device\n$"
             EXPECT_ABSENT out)

# ---- SIGPIPE policy A: `tool | head -n 1` -> the writer dies by SIGPIPE, silently -------------------
cli_e2e_case(NAME sigpipe ARGS ${EDGE_BULK_ARGS} PIPE_HEAD 1
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)
# The parent ignores SIGPIPE (an inherited SIG_IGN, as some shells and services do): the tool must
# restore SIG_DFL itself.
cli_e2e_case(NAME sigpipe-parent-ignores ARGS ${EDGE_BULK_ARGS} PIPE_HEAD 1 PARENT_IGNORES_SIGPIPE
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)

# ---- TTY and color (pseudo-terminal via util-linux script; skipped with 77 where it is absent) -------
cli_e2e_case(NAME tty-help ARGS --help TTY EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "mangadex-dl")
cli_e2e_case(NAME tty-help-no-color ARGS --help TTY ENV NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI)
cli_e2e_case(NAME tty-help-term-dumb ARGS --help TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_NO_ANSI)
if(EDGE_COLOR_ARGS)
  cli_e2e_case(NAME tty-color ARGS ${EDGE_COLOR_ARGS} TTY EXPECT_EXIT 0 EXPECT_ANSI)
  cli_e2e_case(NAME tty-color-no-color ARGS ${EDGE_COLOR_ARGS} TTY ENV NO_COLOR=1 EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME tty-color-term-dumb ARGS ${EDGE_COLOR_ARGS} TTY ENV TERM=dumb EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME pipe-clicolor-force ARGS ${EDGE_COLOR_ARGS} ENV CLICOLOR_FORCE=1 EXPECT_EXIT 0 EXPECT_ANSI)
  # CLICOLOR_FORCE=0 does not force (clicolor spec): a pipe stays plain.
  cli_e2e_case(NAME pipe-clicolor-force-zero ARGS ${EDGE_COLOR_ARGS} ENV CLICOLOR_FORCE=0
               EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME no-color-beats-force ARGS ${EDGE_COLOR_ARGS} ENV CLICOLOR_FORCE=1 NO_COLOR=1
               EXPECT_EXIT 0 EXPECT_NO_ANSI)
  cli_e2e_case(NAME pipe-no-ansi ARGS ${EDGE_COLOR_ARGS} EXPECT_EXIT 0 EXPECT_NO_ANSI)
endif()

# ---- --json: one valid document, never ANSI, even when color is forced or stdout is a TTY ----------
if(EDGE_JSON_ARGS)
  cli_e2e_case(NAME json-valid ARGS ${EDGE_JSON_ARGS}
               EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY)
  cli_e2e_case(NAME json-clicolor-force ARGS ${EDGE_JSON_ARGS} ENV CLICOLOR_FORCE=1
               EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI)
  cli_e2e_case(NAME json-tty ARGS ${EDGE_JSON_ARGS} TTY EXPECT_EXIT 0 EXPECT_NO_ANSI)
  # DEL and the C1 controls (here U+009B, the 8-bit CSI) in DIR are \u-escaped, never raw bytes.
  cli_e2e_case(NAME json-del-c1-dir ARGS ${EDGE_JSON_ARGS} "out{{DEL}}{{C1_CSI}}x"
               EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
               EXPECT_STDOUT_REGEX "\"path\": \"out\\\\u007f\\\\u009bx/")
endif()

# ---- UTF-8 and locale independence ------------------------------------------------------------------
if(EDGE_ECHO_ARGS)
  cli_e2e_case(NAME utf8-echo ARGS ${EDGE_ECHO_ARGS} "Zoë"
               EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "Zoë" EXPECT_STDERR_EMPTY)
  cli_e2e_case(NAME utf8-echo-lc-c ARGS ${EDGE_ECHO_ARGS} "Zoë" ENV LC_ALL=C LANG=C
               EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "Zoë" EXPECT_STDERR_EMPTY)
  # Linux argv is bytes: invalid UTF-8 must not crash (never 70/134/139).
  cli_e2e_case(NAME invalid-utf8-echo ARGS ${EDGE_ECHO_ARGS} "{{INVALID_UTF8}}" EXPECT_EXIT 0)
  if(EDGE_JSON_ARGS)
    # ...and --json must still be valid JSON (nlohmann dump with error_handler_t::replace).
    cli_e2e_case(NAME invalid-utf8-json ARGS ${EDGE_JSON_ARGS} "{{INVALID_UTF8}}"
                 EXPECT_EXIT 0 EXPECT_JSON)
  endif()
endif()

# ---- re-runs that UPDATE an existing DIR (docs/design/skip-existing-spec.md, criterion 19) ----------
# Each case first downloads the clean fixture into `out` (PRE_RUN), then runs against a variant that
# makes the main run rebuild files that already exist. clean-grown-reuse-only adds ch 4 to Vol. 2 and
# serves only ch 4, so Vol. 2 is updated (held ch 3 copied, ch 4 downloaded) and Vol. 1 is up to date.
set(_edge_clean_id 11111111-1111-4111-8111-111111111111)
set(_edge_update_pre_args download --api-base file://{{FIXTURES}}/clean ${_edge_clean_id} out)
set(_edge_update_args download --api-base file://{{FIXTURES}}/clean-grown-reuse-only ${_edge_clean_id} out)
set(_edge_vol1 "out/Clean Title - Vol. 01.epub")
set(_edge_vol2 "out/Clean Title - Vol. 02.epub")
set(_edge_note_one_re
    "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n")

# stdout on a full device during an update: exit 1 with the `write error` diagnostic (never a silent 0),
# and the update itself still lands: Vol. 2 is a sound EPUB, Vol. 1 is untouched, no temp file is left.
# The reason is pinned to the FIRST failure's errno. std::cerr is tied to std::cout, so writing the note
# flushes stdout first: that flush is the one that fails (ENOSPC), and glibc drops stdout's buffer, so
# the final flush in finish_stdout has nothing to write and sets no errno. std::cout's recording buffer
# (app/io.cpp, RecordingStdioBuf) keeps the errno from that first failed flush for finish_stdout.
cli_e2e_case(NAME dev-full-update
             PRE_RUN ${_edge_update_pre_args}
             ARGS ${_edge_update_args}
             STDOUT_FILE /dev/full
             EXPECT_EXIT 1
             EXPECT_STDERR_REGEX "^${_edge_note_one_re}mangadex-dl: write error: No space left on device\n$"
             EXPECT_EPUB "${_edge_vol2}"
             EXPECT_UNCHANGED "${_edge_vol1}"
             EXPECT_NO_GLOB "out/.*.part-*")

# `| head -n 1` across two updates: --group "Bravo Scans" re-picks group B's releases, so BOTH volumes
# are rebuilt and listed. Plan decision D7: the writer exits 0 here, not 141.
# CONSTRAINT (fact-check V18): this is deterministic ONLY because stdout stays fully buffered (nothing
# flushes it per path) and this run prints well under 4 KiB, so both lines reach the pipe in one write
# at exit, before `head` can close it. If stdout is ever flushed per path (or this output grows past a
# pipe buffer), the writer may die by SIGPIPE instead: revisit this case then. SIGPIPE policy A itself
# is proven by the bulk `sigpipe` cases above.
cli_e2e_case(NAME pipe-head-update
             PRE_RUN ${_edge_update_pre_args}
             ARGS download --group "Bravo Scans" --api-base file://{{FIXTURES}}/clean-regrouped-served
                  ${_edge_clean_id} out
             PIPE_HEAD 1 EXPECT_PIPE_RESULT 0
             EXPECT_STDOUT_REGEX "^out/Clean Title - Vol\\. 01\\.epub\n$" EXPECT_STDERR_EMPTY
             EXPECT_EPUB "${_edge_vol1}" "${_edge_vol2}"
             EXPECT_NO_GLOB "out/.*.part-*")

# An update with fd 0 closed: stdin is never read, and a closed fd 0 must not be reused by mistake.
cli_e2e_case(NAME stdin-closed-update
             PRE_RUN ${_edge_update_pre_args}
             ARGS ${_edge_update_args}
             STDIN CLOSED
             EXPECT_EXIT 0
             EXPECT_STDOUT_REGEX "^out/Clean Title - Vol\\. 02\\.epub\n$"
             EXPECT_STDERR_REGEX "^${_edge_note_one_re}$"
             EXPECT_EPUB "${_edge_vol2}"
             EXPECT_UNCHANGED "${_edge_vol1}"
             EXPECT_NO_GLOB "out/.*.part-*")

# ---- batch: the same contract for `mangadex-dl batch FILE [DIR]` (batch spec AC24-AC29) -------------
# The EDGE_* knobs above stay on `download`. These cases point the stdin, write-error, SIGPIPE, UTF-8
# and color edges at `batch`, whose input is a list (FILE or `-` for stdin) instead of argv.
set(_edge_batch_in "{{INPUTS}}/batch")
set(_edge_batch_clean_api --api-base file://{{FIXTURES}}/clean)
# Exact stdout of a dry run of the clean title into DIR <dir>.
set(_edge_batch_zoe_paths_re
    "^Zoë/Clean Title - Vol\\. 01\\.epub\nZoë/Clean Title - Vol\\. 02\\.epub\n$")

# stdin: /dev/null is an empty list (exit 0 + a warning); a closed fd 0 is unreadable (exit 1, never 70).
cli_e2e_case(NAME batch-stdin-null ARGS batch - out STDIN NULL
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: <stdin>: lists no titles\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME batch-stdin-null-json ARGS batch --json - out STDIN NULL
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI
             EXPECT_STDOUT_REGEX "\"source\": \"-\",\n  \"titles\": \\[\\],"
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: <stdin>: lists no titles\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME batch-stdin-closed ARGS batch - out STDIN CLOSED
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: cannot read <stdin>: [^\n]+\n$"
             EXPECT_ABSENT out)

# Write errors: a dry run writes only to stdout, so /dev/full is exit 1 and one `write error` line
# with the errno text, as for `download` (the flush after each row must not lose the reason).
cli_e2e_case(NAME batch-dev-full
             ARGS batch --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv out
             STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "^mangadex-dl: write error: No space left on device\n$"
             EXPECT_ABSENT out)

# SIGPIPE policy A: `batch ... | head -n 1` -> the writer dies by SIGPIPE, silently. The bulk title
# (4000 loose chapters) planned one file per chapter prints well over 64 KiB of lines.
set(_edge_batch_bulk_args batch --dry-run --layout chapters --api-base file://{{FIXTURES}}/bulk
    ${_edge_batch_in}/bulk.csv out)
cli_e2e_case(NAME batch-sigpipe ARGS ${_edge_batch_bulk_args} PIPE_HEAD 1
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME batch-sigpipe-parent-ignores ARGS ${_edge_batch_bulk_args} PIPE_HEAD 1
             PARENT_IGNORES_SIGPIPE
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)

# UTF-8: an invalid-UTF-8 FILE name is echoed as `?` (exit 1, never 70) even under LC_ALL=C; a list
# with the byte 0xFF is a list error (exit 2); an invalid-UTF-8 DIR still gives valid --json.
cli_e2e_case(NAME batch-invalid-utf8-file-lc-c
             ARGS batch ${_edge_batch_clean_api} "{{INVALID_UTF8}}" out
             ENV LC_ALL=C LANG=C
             EXPECT_EXIT 1 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: cannot read \\?\\?: [^\n]+\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME batch-invalid-utf8-csv
             ARGS batch ${_edge_batch_clean_api} ${_edge_batch_in}/invalid-utf8.csv out
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/invalid-utf8\\.csv:1: not valid UTF-8\nRun 'mangadex-dl batch --help' for more information\\.\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME batch-invalid-utf8-dir-json
             ARGS batch --json --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv
                  "{{INVALID_UTF8}}"
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY)
# DEL and U+009B (the 8-bit CSI) in DIR are \u-escaped in the row's dir and the embedded report.
cli_e2e_case(NAME batch-del-c1-dir-json
             ARGS batch --json --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv
                  "out{{DEL}}{{C1_CSI}}x"
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"dir\": \"out\\\\u007f\\\\u009bx\",.*\"path\": \"out\\\\u007f\\\\u009bx/")
# A UTF-8 DIR reaches stdout verbatim under C.UTF-8 and under C (locale-independent output).
cli_e2e_case(NAME batch-utf8-dir
             ARGS batch --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv "Zoë"
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_batch_zoe_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT "Zoë")
cli_e2e_case(NAME batch-utf8-dir-lc-c
             ARGS batch --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv "Zoë"
             ENV LC_ALL=C LANG=C
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_batch_zoe_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT "Zoë")

# Color: --json never carries ANSI, even with CLICOLOR_FORCE=1; piped stderr with failing rows is plain.
cli_e2e_case(NAME batch-json-clicolor-force
             ARGS batch --json --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv out
             ENV CLICOLOR_FORCE=1
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME batch-pipe-stderr-no-ansi
             ARGS batch --api-base file://{{FIXTURES}}/nochapters ${_edge_batch_in}/nochapters-mixed.csv out
             EXPECT_EXIT 7 EXPECT_STDOUT_EMPTY EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^mangadex-dl: error: [^\n]*/batch/nochapters-mixed\\.csv:1: ")

# ---- mdlist: the same contract for `mangadex-dl mdlist LIST [DIR]` (list spec AC25-AC29, AC47) ------
# The EDGE_* knobs above stay on `download`. These cases point the stdin, write-error, SIGPIPE, UTF-8
# and color edges at `mdlist`, whose input is a MangaDex list (fixture mode: a generated list document,
# tests/e2e/inputs/README). L1 a0000000-...-000000000001 on clean holds the clean title; L8 (...08) on
# nochapters holds an exit-5 and an exit-4 title; L10 (...0a) on bulk holds the bulk title.
set(_edge_mdlist_clean_api --api-base file://{{FIXTURES}}/clean)
set(_edge_mdlist_l1 a0000000-0000-4000-8000-000000000001)
set(_edge_mdlist_l8 a0000000-0000-4000-8000-000000000008)
set(_edge_mdlist_l10 a0000000-0000-4000-8000-00000000000a)
set(_edge_mdlist_dry_l1_args mdlist --dry-run ${_edge_mdlist_clean_api} ${_edge_mdlist_l1})
# Exact stdout of a dry run of L1 into DIR `out`.
set(_edge_mdlist_paths_re
    "^out/Clean Title - Vol\\. 01\\.epub\nout/Clean Title - Vol\\. 02\\.epub\n$")

# stdin is never read: /dev/null and a closed fd 0 give the same stdout (never 70).
cli_e2e_case(NAME mdlist-stdin-null ARGS ${_edge_mdlist_dry_l1_args} out STDIN NULL
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_mdlist_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)
cli_e2e_case(NAME mdlist-stdin-closed ARGS ${_edge_mdlist_dry_l1_args} out STDIN CLOSED
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_mdlist_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT out)

# Write errors: a dry run writes only to stdout, so /dev/full is exit 1 and one `write error` line
# with the errno text.
cli_e2e_case(NAME mdlist-dev-full ARGS ${_edge_mdlist_dry_l1_args} out
             STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "^mangadex-dl: write error: No space left on device\n$"
             EXPECT_ABSENT out)

# SIGPIPE policy A: `mdlist ... | head -n 1` -> the writer dies by SIGPIPE, silently. L10's one title
# (4000 loose chapters) planned one file per chapter prints well over 64 KiB of lines.
set(_edge_mdlist_bulk_args mdlist --dry-run --layout chapters --api-base file://{{FIXTURES}}/bulk
    ${_edge_mdlist_l10} out)
cli_e2e_case(NAME mdlist-sigpipe ARGS ${_edge_mdlist_bulk_args} PIPE_HEAD 1
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME mdlist-sigpipe-parent-ignores ARGS ${_edge_mdlist_bulk_args} PIPE_HEAD 1
             PARENT_IGNORES_SIGPIPE
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "^[^\n]+\n$" EXPECT_STDERR_EMPTY)

# UTF-8: an invalid-UTF-8 LIST is a usage error with a fixed message (never echoed, never 70) even
# under LC_ALL=C; an invalid-UTF-8 DIR still gives valid --json.
cli_e2e_case(NAME mdlist-invalid-utf8-list-lc-c
             ARGS mdlist ${_edge_mdlist_clean_api} "{{INVALID_UTF8}}" out
             ENV LC_ALL=C LANG=C
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: error: LIST must be a MangaDex list UUID or a https://mangadex\\.org/list/<uuid> URL; for a CSV file of titles use 'mangadex-dl batch <file>'\nRun 'mangadex-dl mdlist --help' for more information\\.\n$"
             EXPECT_ABSENT out)
cli_e2e_case(NAME mdlist-invalid-utf8-dir-json
             ARGS mdlist --json --dry-run ${_edge_mdlist_clean_api} ${_edge_mdlist_l1}
                  "{{INVALID_UTF8}}"
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY)
# DEL and U+009B (the 8-bit CSI) in DIR are \u-escaped in the item's dir and the embedded report.
cli_e2e_case(NAME mdlist-del-c1-dir-json
             ARGS mdlist --json --dry-run ${_edge_mdlist_clean_api} ${_edge_mdlist_l1}
                  "out{{DEL}}{{C1_CSI}}x"
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"dir\": \"out\\\\u007f\\\\u009bx\",.*\"path\": \"out\\\\u007f\\\\u009bx/")
# A UTF-8 DIR reaches stdout verbatim under C.UTF-8 and under C (locale-independent output).
set(_edge_mdlist_zoe_paths_re
    "^Zoë/Clean Title - Vol\\. 01\\.epub\nZoë/Clean Title - Vol\\. 02\\.epub\n$")
cli_e2e_case(NAME mdlist-utf8-dir ARGS ${_edge_mdlist_dry_l1_args} "Zoë"
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_mdlist_zoe_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT "Zoë")
cli_e2e_case(NAME mdlist-utf8-dir-lc-c ARGS ${_edge_mdlist_dry_l1_args} "Zoë"
             ENV LC_ALL=C LANG=C
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_mdlist_zoe_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT "Zoë")

# Color: --json never carries ANSI, even with CLICOLOR_FORCE=1; piped stderr with failing items is plain.
cli_e2e_case(NAME mdlist-json-clicolor-force
             ARGS mdlist --json --dry-run ${_edge_mdlist_clean_api} ${_edge_mdlist_l1} out
             ENV CLICOLOR_FORCE=1
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME mdlist-pipe-stderr-no-ansi
             ARGS mdlist --api-base file://{{FIXTURES}}/nochapters ${_edge_mdlist_l8} out
             EXPECT_EXIT 7 EXPECT_STDOUT_EMPTY EXPECT_NO_ANSI
             EXPECT_STDERR_REGEX "^mangadex-dl: error: list:1: 55555555-5555-4555-8555-555555555555: ")
# --dir-per-title in the document: `dir` is the title's own directory DIR/<name> (AC47).
cli_e2e_case(NAME mdlist-json-dir-per-title-clicolor-force
             ARGS mdlist --json --dry-run --dir-per-title ${_edge_mdlist_clean_api} ${_edge_mdlist_l1} out
             ENV CLICOLOR_FORCE=1
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"dir\": \"out/Clean Title\",.*\"path\": \"out/Clean Title/Clean Title - Vol\\. 01\\.epub\","
             EXPECT_ABSENT out)

# ---- batch --dir-per-title: the same edges with each title in DIR/<its name>/ (AC34-AC38) -----------
# The batch cases above, plus the flag. clean.csv's one title lands in DIR/Clean Title/.
set(_edge_batch_dpt_dry_clean_args batch --dry-run --dir-per-title ${_edge_batch_clean_api}
    ${_edge_batch_in}/clean.csv)

# --json never carries ANSI, even with CLICOLOR_FORCE=1; `dir` is the title's own directory and the
# report's paths sit under it. A dry run creates nothing.
cli_e2e_case(NAME batch-json-dir-per-title-clicolor-force
             ARGS batch --json --dry-run --dir-per-title ${_edge_batch_clean_api}
                  ${_edge_batch_in}/clean.csv out
             ENV CLICOLOR_FORCE=1
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"dir\": \"out/Clean Title\",.*\"path\": \"out/Clean Title/Clean Title - Vol\\. 01\\.epub\","
             EXPECT_ABSENT out)

# Write errors: stdout on /dev/full is exit 1 and one `write error` line with the errno text.
cli_e2e_case(NAME batch-dir-per-title-dev-full
             ARGS ${_edge_batch_dpt_dry_clean_args} out
             STDOUT_FILE /dev/full
             EXPECT_EXIT 1 EXPECT_STDERR_REGEX "^mangadex-dl: write error: No space left on device\n$"
             EXPECT_ABSENT out)

# SIGPIPE policy A: the bulk title's planned paths, now under DIR/<name>/, die quietly by SIGPIPE.
set(_edge_batch_dpt_bulk_args ${_edge_batch_bulk_args} --dir-per-title)
set(_edge_batch_dpt_bulk_line_re "^out/Bulk Title[^\n/]*/Bulk Title[^\n/]* - Ch\\. 001\\.epub\n$")
cli_e2e_case(NAME batch-dir-per-title-sigpipe ARGS ${_edge_batch_dpt_bulk_args} PIPE_HEAD 1
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "${_edge_batch_dpt_bulk_line_re}"
             EXPECT_STDERR_EMPTY EXPECT_ABSENT out)
cli_e2e_case(NAME batch-dir-per-title-sigpipe-parent-ignores ARGS ${_edge_batch_dpt_bulk_args}
             PIPE_HEAD 1 PARENT_IGNORES_SIGPIPE
             EXPECT_PIPE_RESULT SIGPIPE EXPECT_STDOUT_REGEX "${_edge_batch_dpt_bulk_line_re}"
             EXPECT_STDERR_EMPTY EXPECT_ABSENT out)

# UTF-8: an invalid-UTF-8 DIR still gives valid --json (its bytes replaced, the title's name appended);
# a UTF-8 DIR reaches stdout verbatim under C.UTF-8 and under C.
cli_e2e_case(NAME batch-dir-per-title-invalid-utf8-dir-json
             ARGS batch --json --dry-run --dir-per-title ${_edge_batch_clean_api}
                  ${_edge_batch_in}/clean.csv "{{INVALID_UTF8}}"
             ENV LC_ALL=C LANG=C
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY
             EXPECT_STDOUT_REGEX "\"dir\": \"[^\"\n/]+/Clean Title\",")
set(_edge_batch_dpt_zoe_paths_re
    "^Zoë/Clean Title/Clean Title - Vol\\. 01\\.epub\nZoë/Clean Title/Clean Title - Vol\\. 02\\.epub\n$")
cli_e2e_case(NAME batch-dir-per-title-utf8-dir
             ARGS ${_edge_batch_dpt_dry_clean_args} "Zoë"
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_batch_dpt_zoe_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT "Zoë")
cli_e2e_case(NAME batch-dir-per-title-utf8-dir-lc-c
             ARGS ${_edge_batch_dpt_dry_clean_args} "Zoë"
             ENV LC_ALL=C LANG=C
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "${_edge_batch_dpt_zoe_paths_re}" EXPECT_STDERR_EMPTY
             EXPECT_ABSENT "Zoë")

# stdin: /dev/null is an empty list, flag or not (exit 0 + the warning, nothing created).
cli_e2e_case(NAME batch-dir-per-title-stdin-null ARGS batch --dir-per-title - out STDIN NULL
             EXPECT_EXIT 0 EXPECT_STDOUT_EMPTY
             EXPECT_STDERR_REGEX "^mangadex-dl: warning: <stdin>: lists no titles\n$"
             EXPECT_ABSENT out)
