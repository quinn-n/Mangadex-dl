# tests/e2e/cases/_edge.cmake: the MANDATORY edge-case suite (tests e2e.edge.<case>, labels e2e;edge).
#
# Every CLI built with this studio passes these against its installed binary. Generic cases use only
# --help/--version and a usage error. The knobs below point the rest at real subcommands; keep them in
# step with the CLI surface when it changes. Deleting a case here is a CLI-contract change and needs
# cli-ux-reviewer + quality-reviewer sign-off.

# ---- per-project knobs ----------------------------------------------------------------------------
# REQUIRED: a subcommand invocation that writes well over 64 KiB of NEWLINE-separated lines to stdout.
# The SIGPIPE cases need the pipe to fill. Without newlines, `head -n 1` would read everything and the
# case would prove nothing.
set(EDGE_BULK_ARGS hello --count 20000)
# An invocation that writes exactly one JSON document to stdout (empty = the tool has no --json surface).
set(EDGE_JSON_ARGS hello --json)
# An invocation whose stdout contains its LAST argument verbatim (for UTF-8 and locale round trips; empty = none).
set(EDGE_ECHO_ARGS hello)
# An invocation that colors stdout when stdout is a TTY (empty = the tool never colors stdout).
set(EDGE_COLOR_ARGS hello)

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
