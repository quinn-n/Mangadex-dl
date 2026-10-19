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
# The reason after "write error: " is not pinned: the note on stderr is written first, and std::cerr is
# tied to std::cout, so that write already flushes stdout, fails, and consumes the errno.
cli_e2e_case(NAME dev-full-update
             PRE_RUN ${_edge_update_pre_args}
             ARGS ${_edge_update_args}
             STDOUT_FILE /dev/full
             EXPECT_EXIT 1
             EXPECT_STDERR_REGEX "^${_edge_note_one_re}mangadex-dl: write error: [^\n]+\n$"
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
