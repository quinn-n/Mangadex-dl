---
name: cli-design
description: CLI11 idioms and the CLI contract for this C++ tool - exit codes, the main() firewall, stdout/stderr, flush-and-check, SIGPIPE, TTY/color, JSON, locale, prompts, help/version/man/completions. Use when designing, implementing or reviewing any command, writing help text, choosing options/flags, shaping output, or wiring a new subcommand into CLI11.
---

# CLI design and the CLI contract (C++23 + CLI11)

Apply this when you design, implement or review any command. Every command has two users to satisfy at once:
- the **newcomer**, who learns the tool from `--help`;
- the **scripter**, who automates it and depends on exact exit codes and stream contents.

The C++ that implements this contract is in `scaffold.md`, a supporting file that `/setup` writes into the repo. Follow its shape for every new subcommand.

## Core principles
- **Discoverable from `--help`.** Every subcommand and option has a clear, present-tense help string. Non-trivial commands show an example in their description or footer. A user should rarely need external docs.
- **Sensible defaults.** The common case needs few flags. Default whatever can be defaulted.
- **Consistency.** Option names, ordering and behaviour are uniform across subcommands (`--output` everywhere, not `--out` here and `--dest` there). Long flags are kebab-case (`--dry-run`).
- **Errors that teach.** Say what went wrong *and* the fix: `<tool>: no config at X; run '<tool> init' or pass --config`. Never let an expected failure surface as an exception message or an exit code of 70.
- **Composability.** **Data goes to stdout; diagnostics, progress and prompts go to stderr.** Then `tool ... | other` works. Offer `--json`.
- **Forgiving and safe.** Destructive actions confirm, support `--dry-run`, and can be scripted with `--yes`. Be idempotent where possible.

### The five things every command handles
1. Success.
2. **Empty / no result.** Exit 0 with empty stdout, or a documented code. Never an error message on stdout.
3. **Invalid input or usage:** exit 2.
4. **Expected runtime failure** (missing file, permission, network, a full disk): exit 1 or a documented 3–63 code.
5. **Interruption.** Ctrl-C leaves no half-written state, thanks to atomic writes.

A command isn't done until all five behave well, and the cli-tester has seen them on the installed binary.

### Friction audit
Count the flags and steps the primary task takes. For each required flag or prompt, ask whether it can be defaulted, inferred, or read from config/env. The best flag is the one the user never has to type.

## Exit codes are an API
The single source is `include/<proj>/exit_codes.hpp` (`<proj>::exit_code::*`). It is mirrored in the `--help` footer (built from the constants), the README and the man page's EXIT STATUS.

| Code | Name | Meaning |
|---|---|---|
| 0 | `success` | did what was asked (also `--help`, `--version`) |
| 1 | `failure` | expected runtime failure, **including a failed write to stdout** |
| 2 | `usage` | invalid invocation. **Every** `CLI::ParseError` is remapped here, as are semantic usage errors found after parsing (`Errc::usage`) |
| 3–63 | tool-specific | add to `exit_codes.hpp` with a doc comment **before** any code returns it; mirror it in README/man |
| 70 | `internal` | an exception reached `main`: a bug. Testers assert it never appears |
| 86 | `sanitizer_reserved` | the ASan/UBSan/LSan `exitcode` in sanitized dev builds. Code never returns it (see `memory-safety`) |
| 130 / 141 | (signals) | SIGINT / SIGPIPE deaths, produced by the default dispositions. Never returned by hand |

Never return 126, 127, 128+n, or anything above 255. Don't adopt `sysexits.h` wholesale; FreeBSD has deprecated it.

## Architecture: firewall → parse → dispatch
```
main()                       src/app/main.cpp   restore SIGPIPE, catch ladder, finish_stdout, return
 └─ run(args, io)            src/app/cli.cpp    build CLI::App, register subcommands, parse, dispatch
     ├─ register_<sub>()     src/app/cmd_<sub>.cpp  binds options to a Command's members; callback selects it
     └─ Command::run(io, g)  src/app/cmd_<sub>.cpp  the work: calls core, prints, returns an exit code
core                         include/<proj>/, src/core/   pure logic returning Result<T>; no CLI11, no I/O to std streams
```
- `Io` (`src/app/io.hpp`) is the process seam. It holds `out`/`err` streams, `stdin_tty`/`stdout_tty`/`stderr_tty` and an `EnvSnapshot` read once at startup. `main` builds the real one; tests build one around `std::ostringstream` and call `run()` in-process.
- **`#include <CLI/CLI.hpp>` only in `src/app/*.cpp`.** Headers forward-declare `namespace CLI { class App; }`. Headers that need JSON types include `<nlohmann/json_fwd.hpp>`; only `.cpp` files include `<nlohmann/json.hpp>`. Both rules are about compile time.
- `main.cpp` contains `main()` only. The CLI test binary compiles every other `src/app/` file.

## CLI11 rules
Pin to the installed CLI11 (read the version from the pixi env, never from memory). Have the `fact-checker` confirm an API before relying on anything not shown here. The floor is 2.6, since the version flag must beat requirements. 2.7.x is preferred.

- **Parse-then-dispatch. Never `CLI11_PARSE`.** That macro returns CLI11's own exit codes (100–114), which leak to scripts. In `run()`:
  ```cpp
  try {
      app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError& e) {
      const int rc = app.exit(e, io.out, io.err);  // help/version -> io.out, rc 0; errors -> io.err
      return rc == 0 ? exit_code::success : exit_code::usage;
  }
  return inv.selected->run(io, inv.globals);
  ```
  - Use the stream-taking `App::exit(e, out, err)` so tests capture help and errors. This call form works with the single defaulted function before 2.7.2 and with the three overloads since.
  - `CallForHelp` and `CallForVersion` are `Success` (a `ParseError`) and return 0.
  - `ConstructionError` is **not** a `ParseError`. It escapes `run()` and would be a 70. The in-process CLI test catches it first, because every `run()` constructs the whole app.
- **Callbacks only select.** A subcommand's `callback` stores its `Command*` and does nothing else. Work happens after `parse()` returns, so a later parse error can't leave half-done work.
- **Bound variables must outlive `parse()` and dispatch.**
  - Bind options to **members of the `Command` object** (`cmd_<sub>.cpp`).
  - Bind global flags to the `Invocation` struct that `run()` owns.
  - Declare both **before** the `CLI::App`, so the app (which holds pointers into them) is destroyed first.
  - Never bind to a local of `register_<sub>()`. That dangling reference is a top memory-safety finding.
- **Callbacks capture pointers by value, never by reference:** `[slot = &selected, self = command.get()] { *slot = self; }`, with a lifetime comment (`modern-cpp-conventions` rule 8).
- **`app.fallthrough()` before adding any subcommand.** It is copied into each subcommand when the subcommand is created. With it, global flags work after the subcommand (`tool hello --json`). Every group also gets `require_subcommand(1)`.
- **Name the app explicitly:** `CLI::App app{description, "<tool>"}`. Otherwise CLI11 takes the name from `argv[0]`.
- **`app.set_version_flag("--version", version_line())`**, with no `-v` alias, because `-v` is `--verbose`. The first line of `--version` output is `<tool> X.Y.Z` (help2man parses it).
- **Precedence is command line > config file > environment (`->envname(...)`) > default.** This is CLI11's real order: the config file is processed before env, and an env value only applies when the option wasn't set.
  - A config-file value therefore **beats** an env var. If a feature wants env to beat config, the tool must apply that itself after parsing.
  - The cli-designer states the precedence on every feature that has both a config key and an env var.
- **Validators are UX, not safety.** `CLI::ExistingFile`, `CLI::Range` and friends give good messages, but the file can change before you open it (TOCTOU). Open it and handle the error anyway (`cpp-security`).
- **Enum-like options:** bind a `std::string` with `->check(CLI::IsMember({...}))` and convert after parse, as the scaffold does for `--color`. The help and error text then show the words, not the integers.
- **Never `capture_default_str()` on a secret.** It prints the default in `--help`. Secrets don't belong in argv at all (`cpp-security`).
- `ensure_utf8(argv)` is a no-op on POSIX. It matters only if a Windows build is ever added.
- **Tests:** the `parse(std::vector<std::string>&)` overload expects a **reversed** vector. Tests go through `run()` with an argv-style span (the `parse(argc, argv)` path), never the vector overload.
- **`argc` may be 0** (`execve` with an empty argv). `run()` substitutes `{"<tool>"}` before calling CLI11. CLI11 2.7.2 guards this itself, but don't depend on that. Never index `argv[0]` unchecked anywhere.
- The help footer is built from `exit_codes.hpp`. `enable_footer_formatting(false)` keeps it aligned. If an older CLI11 lacks that call, drop it; the text just reflows.

### Adding a subcommand
1. `cli-designer` specifies it first: synopsis, flags, the streams, exit codes, the `--json` shape, precedence, and the acceptance criteria.
2. Put the logic in core (`include/<proj>/*.hpp` + `src/core/*.cpp`), returning `Result<T>`, unit-tested without CLI11.
3. Create `src/app/cmd_<sub>.hpp/.cpp`:
   - declare `[[nodiscard]] std::unique_ptr<Command> register_<sub>(CLI::App& parent, Command*& selected)`;
   - write a `final` `Command` subclass that owns the bound variables, with `bind(CLI::App&)` and `run(Io&, const GlobalOptions&)`.
4. Add **one line** to the registration list in `cli.cpp`. This is a serialization point: one track edits it at a time.
5. Add tests (`cpp-testing`): unit tests for core, in-process CLI tests through `run()`, and `tests/e2e/cases/<sub>.cmake` for the installed binary. Goldens come from `pixi run bless` (a human approves) and are never hand-written.
6. Update the completions and the man page. Update the README/man EXIT STATUS if a new code was added.

## Errors
- Core returns `Result<T> = std::expected<T, Error>` and marks such functions `[[nodiscard]]`. Compose with `and_then` / `transform` / `or_else`. **Never dereference an unchecked `expected`**, because that is UB. Check `if (!r)` first or use `value_or`.
- `Error{.code = Errc::..., .message = "..."}` holds a one-line message with no `<tool>:` prefix and no newline. `Error::exit_code()` maps `Errc` to the table. Add an `Errc` value when a new exit code is introduced.
- **Only `main`/`run` (and a `Command::run` acting for it) map errors to exit codes and print them.** Library code never calls `std::exit`, `abort`, or prints to stdout.
- Exceptions are for truly exceptional conditions or logic errors. `Failure` (a `std::runtime_error` carrying an `Errc`) covers the rare place that can't return a `Result`.
- **Never throw** from destructors, `noexcept` functions, `atexit` handlers or thread bodies.
- **Return from `main`. Never call `std::exit`**, which skips automatic destructors (and so flushes and RAII cleanup).

### The main() firewall (exact order)
```cpp
restore_default_sigpipe();                 // policy A, first thing
int rc = exit_code::internal;
try        { auto io = process_io(); rc = run(args, io); }
catch (const Failure& e)            { last_resort(e.what()); rc = e.exit_code(); }
catch (const std::system_error& e)  { last_resort(e.what()); rc = exit_code::failure; }   // BEFORE std::exception
catch (const std::exception& e)     { last_resort(e.what()); rc = exit_code::internal; }
catch (...)                         { last_resort("unknown exception"); rc = exit_code::internal; }
return finish_stdout(rc);                  // never std::exit
```
- **`std::system_error` comes before `std::exception`.** FILE*-based `std::print` and `std::filesystem` throw it on I/O failure, carrying the **real errno** (`ENOSPC` on `/dev/full`, `EPIPE` on a closed pipe, `EACCES`, …). It is an expected runtime failure (1), not a bug (70). Tests assert "exit 1" or "throws `std::system_error`", **never a specific errno such as EIO**.
- **`last_resort` uses `fputs`/`fwrite` only, never `std::print`**, which can throw inside a catch handler. Its results are discarded via `static_cast<void>`.
- `Failure` stores only its `Errc`, plus the message in `runtime_error`. Reading `exit_code()` in the handler therefore can't allocate or throw.

## Output and streams
- Write data with `std::print(io.out, "...{}\n", x)` and diagnostics with `std::print(io.err, "<tool>: ...\n")`. Include `<print>` **and** `<ostream>`: the ostream overload is declared in `<ostream>`, so mark that include `// IWYU pragma: keep`. Use one API consistently.
- Use `'\n'`, never `std::endl`, which flushes per line. **Never `std::ios::sync_with_stdio(false)`**: CLI11 writes help through `std::cout`, and the flush-and-check relies on cout sharing stdout's buffer.
- **Format strings are compile-time only.** User data goes in arguments (`"{}"`), never into `std::vformat` / `std::runtime_format` (`cpp-security`).
- **Flush and check stdout on every exit path** with `finish_stdout(rc)`, the gnulib `close_stdout` analog:
  1. reset `errno`;
  2. `std::cout.flush()`;
  3. fail if `std::cout.bad()`, `std::fflush(stdout) != 0` or `std::ferror(stdout)`;
  4. on failure print `<tool>: write error: <reason>` and turn rc 0 into 1. The reason is the `strerror_r` text of the errno of the **first** failed stdout write or flush, and `output error` only when none was observed.

  In this project, `process_io()` installs `RecordingStdioBuf` (`src/app/recording_stdio_buf.hpp`) as `std::cout`'s buffer. It passes calls straight through to the `stdout` FILE* and records that first errno. Without it the errno is lost whenever the first failure happens before `finish_stdout`: a write to the tied `std::cerr` flushes stdout first, and an overflow of the stdio buffer in the middle of a run fails the same way. glibc drops the buffer after a failed write, so the final flush has nothing to write and sets no errno.

  Tests may pin the reason text (`No space left on device`) only under the E2E driver's hermetic `LC_ALL=C.UTF-8` on glibc. On any other libc, tests assert only **exit 1** plus the **`<tool>: write error` prefix**.

  Stdio buffering defers a `/dev/full` error to exit, where it would otherwise be silently lost: plain printing to `/dev/full` exits 0.
  - `std::print(std::ostream&, …)` **does not throw** on a failed write. It sets `badbit`, so this check is the real guard; stop long output loops early with `if (!io.out)`.
  - `std::print(FILE*, …)` **throws** `std::system_error`.
  - Reset `errno` before flushing, because `isatty` leaves a stale `ENOTTY`. Report the first recorded errno (above); print the generic `output error` only when there is none.
- **Never `fclose(stdout)`. Never close stderr:** sanitizers report there after `main` returns.

## Signals and partial state
- **SIGPIPE policy A (default):** first thing in `main`, call `sigaction(SIGPIPE)` with `SIG_DFL` and `sigprocmask(SIG_UNBLOCK, {SIGPIPE})`, as git's `restore_sigpipe_to_default` does.
  - `tool | head -1` then dies silently with 141, even when the parent ignored or blocked SIGPIPE.
  - The E2E edge suite checks both the plain case and the parent-ignores case.
- **Policy B** (`SIG_IGN` plus explicit `EPIPE` handling) is allowed only with a written reason, such as cleanup that must run. It is more code and less deterministic. Sockets always send with `MSG_NOSIGNAL`.
- **SIGINT** keeps its default disposition (130). Rather than catching it, make writes atomic: `mkstemp` in the destination directory → write → `fsync` → `rename` (`cpp-security`). An interrupt then leaves no partial file. If cleanup is unavoidable, use a flag-only handler that re-raises.

## TTY, color, progress, prompts
- **Decide color per stream** with `resolve_color(mode, io.env, is_tty)`. Precedence:
  1. `--color=always|never`;
  2. `NO_COLOR` set and non-empty (disables);
  3. `CLICOLOR_FORCE` set, non-empty and not `0` (forces);
  4. `TERM=dumb` (disables);
  5. `isatty` of that stream.
- **`--json` output never contains ANSI**, not even under `--color=always` or `CLICOLOR_FORCE`. The JSON path never calls `resolve_color`.
- Color is never the only signal. Use the scaffold's ~30-line SGR helper (`paint`), not a color library.
- **Progress** goes only to a TTY stderr, and never with `--quiet` or `--json`.
- **Prompts** need **both stdin and stderr to be TTYs** (`io.stdin_tty && io.stderr_tty`). Otherwise the action requires `--yes`. With `--no-input`, a needed prompt becomes exit 2 plus a hint (`pass --yes to confirm`).
- `-q/--quiet` means errors only. `-v/--verbose` (repeatable) adds detail on stderr. `-h/--help` and `--version` are built in.

## JSON
- Build the whole document, then write it **once**: `std::print(io.out, "{}\n", doc.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace))`.
  - The **`replace`** handler matters. Linux argv and paths are bytes, and the default strict mode throws `type_error.316` on invalid UTF-8.
  - Long streams use JSON Lines: one compact `dump()` per line.
- **Keys are API.** Renaming or removing a key is a breaking change. Pick stable, snake_case names.
- Diagnostics stay on stderr even in `--json` mode. Exit codes don't change with `--json`.

## Locale and argv
- **Never `setlocale(LC_ALL, "")` or `std::locale::global(...)`.** Output must not change with the user's locale. If a feature truly needs it, set `LC_CTYPE` alone, with a documented reason.
- Use `std::format`/`std::print` **without the `L` specifier**. Parse and format numbers with `std::from_chars`/`std::to_chars`. Hand-roll ASCII case mapping and character classes; never `toupper`/`isalpha`, which are locale-dependent and UB for negative `char`.
- argv is bytes, not guaranteed UTF-8. Pass it through verbatim. The E2E suite runs under `LC_ALL=C.UTF-8` and also `LC_ALL=C`.

## Help, version, man page, completions
- `--help` and `--version` go to **stdout** and exit 0. help2man depends on this.
- **Man page:** generated with `help2man`. Probe for it at use-time (`command -v help2man`); if it is absent, say so.
  - Command: `help2man --no-info --name '<one-line description>' --output docs/<tool>.1 <installed binary>`.
  - The page is committed under `docs/<tool>.1` and installed under `man1/` (component `docs`).
  - CI regenerates it and fails on drift. CI lints it with `out=$(groff -ww -z -man docs/<tool>.1 2>&1); test -z "$out"`, because groff exits 0 even on warnings.
  - Have the fact-checker confirm help2man's exit-status expectations against the installed version.
- **Completions are hand-written:** bash `<tool>.bash`, zsh `_<tool>`, fish `<tool>.fish`. Released CLI11 has no completion generator; have the fact-checker check again before writing one by hand.
  - A drift test asserts that every long flag and subcommand in `--help` appears in each completion file.

## When NOT to apply this
- A one-off developer script under `scripts/` isn't the product CLI and doesn't need `Io` plumbing.
- Don't add `--json`, color or prompts to a command that has nothing to structure, nothing to emphasise and nothing destructive. Add them when the design calls for it, not by reflex.
- Don't invent 3–63 codes for failure classes that scripts don't need to distinguish. Exit 1 plus a clear message is fine.

## Sibling skills
- `modern-cpp-conventions`: how the code itself is written.
- `cpp-security`: argv/env/paths/secrets as untrusted input; atomic writes.
- `memory-safety`: CLI11 bound-variable and `Command` lifetime, sanitizer exit code 86.
- `cpp-testing`: the in-process seam, the E2E driver and the mandatory edge suite.
- `cpp-quality`: the gate.
- `build-workflow`: the targets, install of the man page and completions.
