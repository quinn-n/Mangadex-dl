# TODO

Follow-ups noted during reviews and plan gates. Each item needs its own plan before code.

## Terminal safety

- **Raw DIR and paths on human-mode stdout.** `download`, `batch` and `mdlist` print each
  `outcome.path` exactly as built (`src/app/cmd_download_output.cpp:204,212`; batch and mdlist reuse
  `ConsoleObserver`). A DIR containing U+009B (bytes `C2 9B`, the 8-bit CSI), DEL or C0 controls
  therefore reaches the terminal raw. Stdout is data and DIR is the user's own argument, so changing
  it is a CLI contract decision (escape? `?`-substitute? only on a TTY?). Found by the json-c1
  fact-check (`docs/design/json-c1-plan.md`, Residuals). The json-c1 cli-ux review suggests the
  GNU `ls` model: escape C0, DEL and C1 only when stdout is a TTY, and keep piped stdout
  byte-exact so `| xargs` / `| while read` keep working. Untrusted sources are already closed (a
  CSV `dir` cell with a control is exit 2; titles are sanitized into file names), so only the
  user's own argv reaches this path.
- **Raw exception text on the last-resort stderr path.** `last_resort(e.what())`
  (`src/app/io.cpp:212-217`, called from `main.cpp`) prints exception text without `terminal_safe`;
  a `std::filesystem_error` can embed a path. Not reachable by any current test case.
- **DEL and C1 in the EPUB manifest.** `manifest_to_json` (`src/core/book_manifest.cpp:356`) still
  writes DEL and U+0080–U+009F raw. It is stored inside the EPUB and only read back by
  `parse_manifest`, never shown on a terminal; escaping it would change new EPUBs' manifest bytes
  and the exact-bytes test in `tests/unit/book_manifest/book_manifest_test.cpp`. The at-home report
  body (`at_home_report_json`, sent to MangaDex) is in the same position.
- **U+2028 / U+2029 in `--json`.** Valid JSON and emitted raw; they are line separators for
  pre-ES2019 JavaScript and some line-oriented viewers. Decide whether `--json` should escape them.

## Docs

- **README: state the `--json` string guarantee.** The `--json` section (around README.md:624-641)
  says only "never colored". Suggested sentence (json-c1 cli-ux review): "Strings are UTF-8: bytes
  that are not valid UTF-8 become U+FFFD, and control characters (U+0000–U+001F, U+007F and
  U+0080–U+009F) are written as `\u00xx` escapes, so the document never carries a terminal escape
  sequence. A JSON parser decodes them back to the original characters." Left out of json-c1 by
  the plan-gate decision (no README change).

## Tests

- **More installed-binary coverage for DEL/C1 in `--json`** (gaps noted by the json-c1 cli-tester;
  all three behave correctly when run by hand, and the unit tier covers the `error` field):
  an E2E case for the JSON `error` field holding a DEL/C1 path (`batch --json` with DIR an
  existing file), `mdlist --json --dir-per-title` with a C1 DIR, and `json-tty` with a C1 DIR.

## Studio docs

- `.claude/skills/cpp-testing/SKILL.md` and its `scaffold.md` list the E2E placeholders and
  `EXPECT_NO_ANSI` without the json-c1 additions (`{{DEL}}`, `{{C1_CSI}}`, the 8-bit CSI check and
  the raw DEL/C1 check on JSON stdout).
