---
name: cli-design-scaffold
description: Supporting file of the cli-design skill (scaffold templates read by /setup). Not a standalone skill.
---

# cli-design scaffold: the C++ sources `/setup` writes

`/setup` reads this file and writes each block below to the **target path in its heading**, relative to the repository root. The design behind these files lives in `SKILL.md` next to this file. This file is the code.

## How `/setup` uses it
1. **Substitute the placeholders** in every block:
   - `@TOOL@`: the binary/CLI name.
   - `@PROJ@`: matches `[a-z][a-z0-9_]*`; it is the C++ namespace and the CMake prefix.
   - `@DESCRIPTION@`: a one-line description. It lands inside a C++ string literal, so escape `"` and `\`.
   - `@SPDX@`: the license identifier.

   Leave `@PROJECT_VERSION@` alone. CMake's `configure_file(... @ONLY)` fills it in.
2. **Never overwrite an existing file** without asking. When adopting an existing project, show a diff and let the user choose.
3. **Run `nice pixi run fmt` once all scaffolds are written.** The include order and line lengths depend on the substituted names, so the formatter has the last word.
4. Then run the gate, `nice pixi run check`, with a long Bash `timeout` (up to 600000 ms) or `run_in_background`.

## What the other scaffolds rely on (keep in step if you change these)
| Interface | Used by |
|---|---|
| `@PROJ@::app::run(std::span<const char* const>, Io&)` in `src/app/cli.hpp` | `tests/support/io_capture.hpp` and `tests/cli/` (cpp-testing) |
| `@PROJ@::app::Io{out, err, stdin_tty, stdout_tty, stderr_tty, env}` and the default-constructible `EnvSnapshot` in `src/app/io.hpp` | the cpp-testing in-process tests (designated init `Io{.out = o, .err = e, ...}`) |
| `@PROJ@::exit_code::{success=0, failure=1, usage=2, internal=70, sanitizer_reserved=86}` | everything; the E2E driver asserts these numbers |
| `@PROJ@::greet(std::string_view) -> Result<std::string>` (`"Hello, <name>!"`; `Result == std::string` compares the value) | `tests/unit/core/greet_test.cpp` |
| `hello [NAME] [-n/--count N] [--json]`: default `world`, one line per count (1..1000000), and with `--json` one document `{"count", "greeting"}`. It colors stdout via `resolve_color`, so `EDGE_COLOR_ARGS` may be `hello` | `tests/e2e/cases/hello.cmake`, the `_edge.cmake` knobs |
| `--version` prints `@TOOL@ X.Y.Z` on stdout and exits 0. Usage errors exit 2. A stdout write failure exits 1 with `@TOOL@: write error: <reason>` on stderr (the reason is best-effort; tests assert only the `write error` prefix). SIGPIPE policy A | the E2E edge suite and help2man |
| `src/app/main.cpp` holds only `main()`. The rest of `src/app/*.cpp` is `@PROJ@_app_sources`. `src/core/**/*.cpp` + `generated/version.cpp` make up `@PROJ@_core` (with `include/` PUBLIC) | `CMakeLists.txt` (build-workflow) |

These files are written to pass the project's own gate:
- the full warning set under `-Werror` on both compilers;
- clang-tidy with `WarningsAsErrors: '*'`, given that `.clang-tidy` disables `readability-redundant-member-init` (it conflicts with `modern-cpp-conventions` rule 4: `std::string message{};` is required);
- `clang-format --dry-run --Werror` after `pixi run fmt`.

Each of the five `NOLINTNEXTLINE`s names its check and gives its reason. Each one was confirmed to fire when removed (clang-tidy 23 against the conda glibc 2.28 sysroot). There is deliberately no `cppcoreguidelines-pro-type-union-access` suppression on `action.sa_handler`: glibc spells `sa_handler` as a macro defined in a system header, so the check never fires there. If another libc/clang-tidy combination makes it (or any other check) fire, add a NOLINT naming that check with its reason; never disable the check. clang-tidy has no unused-NOLINT diagnostic, so a clean `lint` proves nothing about a suppression: to re-check one on a new toolchain, strip it, run `pixi run clang-tidy -p build/default/dev --quiet <its .cpp>`, and drop it only if the named check stays silent. The `// IWYU pragma: keep` on `<ostream>` is deliberate: the `std::print(std::ostream&, ...)` overload is declared in `<ostream>`, and libc++ needs it.

## `include/@PROJ@/exit_codes.hpp`
The exit-code table, the single source. `--help`, README and the man page mirror it.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// The exit-code table: the single source of truth. Mirrored in the --help footer (cli.cpp builds it
// from these constants) and in README / the man page EXIT STATUS section. Scripts depend on these
// numbers, so changing one is a breaking change.
#pragma once

namespace @PROJ@::exit_code {

/// The command did what was asked.
inline constexpr int success = 0;
/// Expected runtime failure, including a failed write to stdout.
inline constexpr int failure = 1;
/// Invalid invocation. Every CLI11 ParseError is remapped to this.
inline constexpr int usage = 2;

// 3-63: tool-specific failures. Add them here, documented, before any code returns them.

/// An exception reached main(): a bug. Tests assert it never appears.
inline constexpr int internal = 70;
/// The ASan/UBSan/LSan exitcode in sanitized dev builds. Never returned by code.
inline constexpr int sanitizer_reserved = 86;

// Never return 126, 127, 128+n (130 and 141 come from signals) or anything above 255.

} // namespace @PROJ@::exit_code
```

## `include/@PROJ@/error.hpp`
`Errc`, `Error`, `Result<T>` and the `Failure` exception. Only `main`/`run` turn these into exit codes.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// Error vocabulary for core code. Core functions return Result<T>; only main()/run() turn an Error
// into an exit code. Failure is the exception form, for the rare place that cannot return a Result.
#pragma once

#include "@PROJ@/exit_codes.hpp"

#include <expected>
#include <stdexcept>
#include <string>

namespace @PROJ@ {

/// Error categories. Each maps to exactly one exit code (see exit_code_for).
enum class Errc {
    failure, ///< expected runtime failure (missing file, permission, malformed data)
    usage,   ///< the invocation is invalid in a way the parser cannot see
    io,      ///< a read or write on a file or stream failed
};

[[nodiscard]] constexpr int exit_code_for(Errc code) noexcept {
    switch (code) {
    case Errc::usage:
        return exit_code::usage;
    case Errc::failure:
    case Errc::io:
        return exit_code::failure;
    }
    return exit_code::internal; // only reachable through an out-of-range enum value
}

/// A value type: aggregate + default member initializers + defaulted ==.
struct Error {
    Errc code = Errc::failure;
    std::string message{}; ///< one line, no trailing newline, no "<tool>: " prefix

    [[nodiscard]] int exit_code() const noexcept { return exit_code_for(code); }
    friend bool operator==(const Error&, const Error&) = default;
};

/// Every fallible core function returns this. Never dereference it unchecked.
template <class T> using Result = std::expected<T, Error>;

/// Exception carrying an Error. It stores only the code plus runtime_error's message, so copying it
/// never allocates (safe to catch and inspect inside main()'s catch ladder).
class Failure : public std::runtime_error {
public:
    explicit Failure(const Error& err) : std::runtime_error(err.message), code_(err.code) {}

    [[nodiscard]] Errc code() const noexcept { return code_; }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_for(code_); }
    [[nodiscard]] Error error() const { return Error{.code = code_, .message = what()}; }

private:
    Errc code_ = Errc::failure;
};

} // namespace @PROJ@
```

## `include/@PROJ@/version.hpp`
Program identity: `tool_name`, `version()` and `version_line()`.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// Program identity. The version has one source, project(VERSION) in CMakeLists.txt, which
// configure_file() writes into generated/version.cpp from src/core/version.cpp.in.
#pragma once

#include <string_view>

namespace @PROJ@ {

inline constexpr std::string_view tool_name{"@TOOL@"};

/// "X.Y.Z", from project(VERSION).
[[nodiscard]] std::string_view version() noexcept;

/// "@TOOL@ X.Y.Z": the first line of --version (help2man parses it).
[[nodiscard]] std::string_view version_line() noexcept;

} // namespace @PROJ@
```

## `include/@PROJ@/greet.hpp`
Example core API returning `Result<T>`. Replace it with real modules.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// Example core module: pure logic, no I/O, no CLI11, unit-testable. Replace it with real features.
#pragma once

#include "@PROJ@/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace @PROJ@ {

inline constexpr std::size_t max_name_length = 256;

/// "Hello, <name>!". The name is bytes (argv on Linux is not guaranteed UTF-8) and is kept
/// verbatim. Errc::usage when it is empty, longer than max_name_length bytes, or contains ASCII
/// control characters.
[[nodiscard]] Result<std::string> greet(std::string_view name);

} // namespace @PROJ@
```

## `src/core/greet.cpp`
Example core implementation: pure, locale-independent, no I/O.

```cpp
// SPDX-License-Identifier: @SPDX@
#include "@PROJ@/greet.hpp"

#include "@PROJ@/error.hpp"

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace @PROJ@ {
namespace {

// Byte-wise and locale-independent on purpose: never std::iscntrl or the global locale.
[[nodiscard]] bool has_control_char(std::string_view text) noexcept {
    return std::ranges::any_of(text, [](char c) {
        const int byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F;
    });
}

} // namespace

Result<std::string> greet(std::string_view name) {
    if (name.empty()) {
        return std::unexpected(Error{.code = Errc::usage, .message = "name must not be empty"});
    }
    if (name.size() > max_name_length) {
        return std::unexpected(Error{
            .code = Errc::usage,
            .message = std::format("name is longer than {} bytes", max_name_length),
        });
    }
    if (has_control_char(name)) {
        return std::unexpected(
            Error{.code = Errc::usage, .message = "name must not contain control characters"});
    }
    return std::format("Hello, {}!", name);
}

} // namespace @PROJ@
```

## `src/core/version.cpp.in`
`configure_file(... @ONLY)` template, compiled into `@PROJ@_core` as `<build dir>/generated/version.cpp`. `/setup` substitutes `@TOOL@`/`@PROJ@`/`@SPDX@` and **leaves `@PROJECT_VERSION@` for CMake**.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// Template. CMakeLists.txt runs configure_file() on it in at-only mode, writing
// <build dir>/generated/version.cpp. The at-sign PROJECT_VERSION token below is CMake's, not a
// /setup placeholder. Edit this template, never the generated file.
#include "@PROJ@/version.hpp"

#include <string_view>

namespace @PROJ@ {

std::string_view version() noexcept { return "@PROJECT_VERSION@"; }

std::string_view version_line() noexcept { return "@TOOL@ @PROJECT_VERSION@"; }

} // namespace @PROJ@
```

## `src/app/io.hpp`
The process-I/O seam (`Io`, `EnvSnapshot`), the SIGPIPE/flush/last-resort helpers, color resolution and the SGR helper.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// Process I/O seam. main() builds the real Io once (process_io); tests build one around
// std::ostringstream sinks and call run() in-process. Nothing below src/app/ touches std::cout,
// std::cerr or getenv directly.
#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

namespace @PROJ@::app {

/// Environment read once at startup. getenv results are untrusted input: keep only what is needed,
/// already interpreted.
struct EnvSnapshot {
    bool no_color = false;       ///< NO_COLOR is set and non-empty
    bool clicolor_force = false; ///< CLICOLOR_FORCE is set, non-empty and not "0"
    bool term_dumb = false;      ///< TERM is exactly "dumb"
};

struct Io {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): non-owning view
    std::ostream& out; ///< data only (stdout)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): non-owning view
    std::ostream& err; ///< diagnostics, progress, prompts (stderr)
    bool stdin_tty = false;
    bool stdout_tty = false;
    bool stderr_tty = false;
    EnvSnapshot env{};
};

/// The real process: std::cout/std::cerr, isatty(0/1/2), environment snapshot.
[[nodiscard]] Io process_io();

/// SIGPIPE policy A: default disposition and unblocked, even if the parent ignored or blocked it,
/// so `@TOOL@ ... | head -1` dies quietly with 141 instead of reporting write errors.
void restore_default_sigpipe() noexcept;

/// "@TOOL@: <message>\n" on stderr with stdio only (never std::print: it can throw). For main()'s
/// catch ladder.
void last_resort(const char* message) noexcept;

/// Flush stdout and check it on every exit path (gnulib close_stdout analog). On a write error,
/// prints "@TOOL@: write error: <reason>" and turns rc 0 into 1. The reason is best-effort
/// (strerror, or "output error" when it is unknown). Never closes stdout or stderr.
[[nodiscard]] int finish_stdout(int rc) noexcept;

enum class ColorMode {
    automatic,
    always,
    never,
};

/// Precedence: --color=always|never > NO_COLOR > CLICOLOR_FORCE > TERM=dumb > isatty. Decide per
/// stream (pass io.stdout_tty or io.stderr_tty). --json output never calls this: it has no ANSI.
[[nodiscard]] bool resolve_color(ColorMode mode, const EnvSnapshot& env, bool is_tty) noexcept;

enum class Style {
    bold,
    dim,
    red,
    green,
    yellow,
};

/// Wraps text in an SGR sequence when enabled; returns it unchanged otherwise.
[[nodiscard]] std::string paint(std::string_view text, Style style, bool enabled);

} // namespace @PROJ@::app
```

## `src/app/io.cpp`
The process-I/O seam's implementation. The only file that touches `std::cout`, `std::cerr`, `getenv`, `isatty` and signals.

```cpp
// SPDX-License-Identifier: @SPDX@
#include "io.hpp"

#include "@PROJ@/exit_codes.hpp"
#include "@PROJ@/version.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction and sigprocmask are POSIX <signal.h>
#include <signal.h>
#include <unistd.h>

namespace @PROJ@::app {
namespace {

void write_stderr(std::string_view text) noexcept {
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), stderr));
}

[[nodiscard]] bool env_non_empty(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

[[nodiscard]] EnvSnapshot read_env() noexcept {
    const char* force = std::getenv("CLICOLOR_FORCE");
    const char* term = std::getenv("TERM");
    return EnvSnapshot{
        .no_color = env_non_empty("NO_COLOR"),
        .clicolor_force = force != nullptr && *force != '\0' && std::string_view{force} != "0",
        .term_dumb = term != nullptr && std::string_view{term} == "dumb",
    };
}

[[nodiscard]] constexpr int sgr_code(Style style) noexcept {
    switch (style) {
    case Style::bold:
        return 1;
    case Style::dim:
        return 2;
    case Style::red:
        return 31;
    case Style::green:
        return 32;
    case Style::yellow:
        return 33;
    }
    return 0;
}

} // namespace

Io process_io() {
    return Io{
        .out = std::cout,
        .err = std::cerr,
        .stdin_tty = ::isatty(STDIN_FILENO) == 1,
        .stdout_tty = ::isatty(STDOUT_FILENO) == 1,
        .stderr_tty = ::isatty(STDERR_FILENO) == 1,
        .env = read_env(),
    };
}

void restore_default_sigpipe() noexcept {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    static_cast<void>(sigemptyset(&action.sa_mask));
    static_cast<void>(sigaction(SIGPIPE, &action, nullptr));

    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t pipe_only{};
    static_cast<void>(sigemptyset(&pipe_only));
    static_cast<void>(sigaddset(&pipe_only, SIGPIPE));
    static_cast<void>(sigprocmask(SIG_UNBLOCK, &pipe_only, nullptr));
}

void last_resort(const char* message) noexcept {
    write_stderr(tool_name);
    write_stderr(": ");
    static_cast<void>(std::fputs(message != nullptr ? message : "unknown error", stderr));
    write_stderr("\n");
}

int finish_stdout(int rc) noexcept {
    errno = 0; // isatty() and friends leave stale values (ENOTTY); only report what the flush sets
    bool failed = false;
    try {
        std::cout.flush();
    } catch (...) {
        failed = true; // only if someone enabled exceptions on std::cout
    }
    failed = std::cout.bad() || failed;
    failed = (std::fflush(stdout) != 0) || failed;
    failed = (std::ferror(stdout) != 0) || failed;
    if (!failed) {
        return rc;
    }
    const int saved_errno = errno;
    write_stderr(tool_name);
    write_stderr(": write error: ");
    if (saved_errno != 0) {
        static_cast<void>(std::fputs(std::strerror(saved_errno), stderr));
    } else {
        write_stderr("output error"); // badbit set by an earlier write; the errno is gone
    }
    write_stderr("\n");
    return rc == exit_code::success ? exit_code::failure : rc;
}

bool resolve_color(ColorMode mode, const EnvSnapshot& env, bool is_tty) noexcept {
    switch (mode) {
    case ColorMode::always:
        return true;
    case ColorMode::never:
        return false;
    case ColorMode::automatic:
        break;
    }
    if (env.no_color) {
        return false;
    }
    if (env.clicolor_force) {
        return true;
    }
    if (env.term_dumb) {
        return false;
    }
    return is_tty;
}

std::string paint(std::string_view text, Style style, bool enabled) {
    if (!enabled) {
        return std::string{text};
    }
    return std::format("\x1b[{}m{}\x1b[0m", sgr_code(style), text);
}

} // namespace @PROJ@::app
```

## `src/app/command.hpp`
`GlobalOptions` and the `Command` base: one subclass per subcommand owns that subcommand's bound variables.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// One Command subclass per subcommand (cmd_<sub>.cpp). The object owns the variables CLI11 binds
// to, so it must outlive parse() and dispatch: run() in cli.cpp owns every Command for the whole
// invocation. CLI11 callbacks only record which Command was selected; the work happens in run().
#pragma once

#include "io.hpp"

namespace @PROJ@::app {

/// Global flags (bound on the root app; fallthrough() lets them appear after the subcommand).
struct GlobalOptions {
    bool json = false;  ///< --json: one JSON document on stdout, no ANSI, no progress
    bool quiet = false; ///< -q/--quiet: errors only on stderr
    int verbose = 0;    ///< -v/--verbose (repeatable): more detail on stderr
    ColorMode color = ColorMode::automatic; ///< --color=auto|always|never
};

class Command {
public:
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;
    virtual ~Command() = default;

    /// Runs after a successful parse. Returns an exit code from exit_codes.hpp.
    [[nodiscard]] virtual int run(Io& io, const GlobalOptions& globals) = 0;

protected:
    Command() = default;
};

} // namespace @PROJ@::app
```

## `src/app/cli.hpp`
The in-process seam the CLI tests call. No CLI11 types cross it.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// The in-process seam: tests call run() with ostringstream-backed Io and assert exit code, stdout
// and stderr separately. No CLI11 types cross this header.
#pragma once

#include "io.hpp"

#include <span>

namespace @PROJ@::app {

/// Builds the CLI11 app, parses `args` (argv-style: args[0] is the program name; may be empty),
/// dispatches the selected command and returns its exit code. Parse errors return 2; --help and
/// --version return 0 with their text on io.out. Throws only for bugs (main maps those to 70).
[[nodiscard]] int run(std::span<const char* const> args, Io& io);

} // namespace @PROJ@::app
```

## `src/app/cli.cpp`
Builds the CLI11 app, holds the registration list (a serialization point), parses, then dispatches.

```cpp
// SPDX-License-Identifier: @SPDX@
#include "cli.hpp"

#include "cmd_hello.hpp"
#include "command.hpp"
#include "io.hpp"
#include "@PROJ@/exit_codes.hpp"
#include "@PROJ@/version.hpp"

#include <CLI/CLI.hpp>

#include <array>
#include <format>
#include <memory>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace @PROJ@::app {
namespace {

constexpr std::string_view app_description{"@DESCRIPTION@"};

/// Everything the root app binds to. Owned by run() and declared before the CLI::App.
struct Invocation {
    GlobalOptions globals{};
    std::string color_word = "auto"; ///< raw --color value, converted after parse()
    Command* selected = nullptr;     ///< set by the chosen subcommand's callback; borrowed
};

[[nodiscard]] ColorMode to_color_mode(std::string_view word) noexcept {
    if (word == "always") {
        return ColorMode::always;
    }
    if (word == "never") {
        return ColorMode::never;
    }
    return ColorMode::automatic; // "auto"; IsMember has already rejected anything else
}

/// Mirrors exit_codes.hpp so --help (and the help2man page) document the table.
[[nodiscard]] std::string exit_status_footer() {
    return std::format("Exit status:\n"
                       "  {:<3} success\n"
                       "  {:<3} runtime failure (including a failed write to stdout)\n"
                       "  {:<3} usage error\n"
                       "  {:<3} internal error (a bug: please report it)",
                       exit_code::success, exit_code::failure, exit_code::usage,
                       exit_code::internal);
}

void add_global_options(CLI::App& app, Invocation& inv) {
    app.add_flag("--json", inv.globals.json, "Write machine-readable JSON to stdout");
    app.add_flag("-q,--quiet", inv.globals.quiet, "Only print errors");
    app.add_flag("-v,--verbose", inv.globals.verbose, "Print more detail to stderr (repeatable)");
    app.add_option("--color", inv.color_word, "Colorize output: auto, always or never")
        ->check(CLI::IsMember({"auto", "always", "never"}))
        ->capture_default_str();
}

} // namespace

int run(std::span<const char* const> args, Io& io) {
    // Lifetime: CLI11 holds pointers into `inv` and into every Command. Both are declared before
    // `app`, so they are destroyed after it, and they live until dispatch below has returned.
    Invocation inv{};
    std::vector<std::unique_ptr<Command>> commands;

    CLI::App app{std::string{app_description}, std::string{tool_name}};
    app.fallthrough(); // before any add_subcommand: subcommands copy it when they are created
    app.require_subcommand(1);
    app.set_version_flag("--version", std::string{version_line()}); // no -v: that is --verbose
    app.footer(exit_status_footer());
    app.get_formatter()->enable_footer_formatting(false); // keep the table's alignment
    add_global_options(app, inv);

    // Registration list, one line per subcommand (a serialization point: one track at a time).
    commands.push_back(register_hello(app, inv.selected));

    // argc may be 0 (execve with an empty argv). Never index argv[0] unchecked; give CLI11 a name.
    const std::string argv0{tool_name};
    const std::array<const char*, 1> fallback{argv0.c_str()};
    const std::span<const char* const> argv =
        args.empty() ? std::span<const char* const>{fallback} : args;
    if (!std::in_range<int>(argv.size())) {
        std::print(io.err, "{}: too many arguments\n", tool_name);
        return exit_code::usage;
    }

    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& e) {
        // Help/version print to io.out and return 0; every other parse error prints to io.err.
        // CLI11's own codes (100-114) never leak: anything nonzero becomes usage (2).
        const int rc = app.exit(e, io.out, io.err);
        return rc == 0 ? exit_code::success : exit_code::usage;
    }

    inv.globals.color = to_color_mode(inv.color_word);
    if (inv.selected == nullptr) { // require_subcommand(1) makes this a bug, not a usage error
        std::print(io.err, "{}: no command was selected\n", tool_name);
        return exit_code::internal;
    }
    return inv.selected->run(io, inv.globals);
}

} // namespace @PROJ@::app
```

## `src/app/cmd_hello.hpp`
Registration declaration for one subcommand. It forward-declares `CLI::App` and never includes CLI11.

```cpp
// SPDX-License-Identifier: @SPDX@
#pragma once

#include "command.hpp"

#include <memory>

namespace CLI {
class App;
} // namespace CLI

namespace @PROJ@::app {

/// Adds `hello` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `hello` is chosen.
[[nodiscard]] std::unique_ptr<Command> register_hello(CLI::App& parent, Command*& selected);

} // namespace @PROJ@::app
```

## `src/app/cmd_hello.cpp`
Example subcommand: options bound to `Command` members, the callback only selects, and the work runs after parse. Covers `--json` and color.

```cpp
// SPDX-License-Identifier: @SPDX@
#include "cmd_hello.hpp"

#include "command.hpp"
#include "io.hpp"
#include "@PROJ@/error.hpp"
#include "@PROJ@/exit_codes.hpp"
#include "@PROJ@/greet.hpp"
#include "@PROJ@/version.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>

namespace @PROJ@::app {
namespace {

class HelloCommand final : public Command {
public:
    HelloCommand() = default;

    /// Binds options to members of this object (never to locals of register_hello).
    void bind(CLI::App& sub) {
        sub.add_option("name", name_, "Who to greet")->capture_default_str();
        sub.add_option("-n,--count", count_, "How many times to greet")
            ->check(CLI::Range(1, max_count))
            ->capture_default_str();
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        const Result<std::string> greeting = greet(name_);
        if (!greeting) {
            std::print(io.err, "{}: {}\n", tool_name, greeting.error().message);
            return greeting.error().exit_code();
        }
        if (globals.json) {
            // One document, built whole and written once; no ANSI ever.
            nlohmann::json doc = nlohmann::json::object();
            doc["greeting"] = *greeting;
            doc["count"] = count_;
            // replace: argv and paths are bytes on Linux; strict mode throws on invalid UTF-8.
            std::print(io.out, "{}\n",
                       doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
            return exit_code::success;
        }
        const bool color = resolve_color(globals.color, io.env, io.stdout_tty);
        const std::string line = paint(*greeting, Style::bold, color);
        for (int i = 0; i < count_ && io.out; ++i) { // stop early once a write has failed
            std::print(io.out, "{}\n", line);
        }
        return exit_code::success; // a failed write is reported by finish_stdout in main()
    }

private:
    static constexpr int max_count = 1'000'000;

    std::string name_ = "world";
    int count_ = 1;
};

} // namespace

std::unique_ptr<Command> register_hello(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<HelloCommand>();
    CLI::App* sub = parent.add_subcommand("hello", "Print a greeting");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in run()'s
    // Invocation and `self` is owned by run()'s command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace @PROJ@::app
```

## `src/app/main.cpp`
The firewall. It holds `main()` only, because the CLI tests compile every other `src/app/` file.

```cpp
// SPDX-License-Identifier: @SPDX@
//
// The firewall. main() holds no logic: it restores SIGPIPE, runs the CLI, turns any escaped
// exception into an exit code, and flushes and checks stdout. It returns; it never calls std::exit.
// Keep this file to main() alone: tests compile every other src/app/ file into the CLI test binary.
#include "cli.hpp"
#include "io.hpp"
#include "@PROJ@/error.hpp"
#include "@PROJ@/exit_codes.hpp"

#include <cstddef>
#include <exception>
#include <span>
#include <system_error>

// NOLINTNEXTLINE(misc-const-correctness): the signature of main is fixed by the standard
int main(int argc, char** argv) {
    @PROJ@::app::restore_default_sigpipe(); // SIGPIPE policy A
    int rc = @PROJ@::exit_code::internal;
    try {
        auto io = @PROJ@::app::process_io();
        const char* const* const argv_view = argv; // qualification conversion, no cast
        const std::span<const char* const> args{argv_view, static_cast<std::size_t>(argc)};
        rc = @PROJ@::app::run(args, io);
    } catch (const @PROJ@::Failure& e) {
        @PROJ@::app::last_resort(e.what());
        rc = e.exit_code();
    } catch (const std::system_error& e) {
        // Before std::exception: FILE*-based std::print and std::filesystem throw this with the
        // real errno (ENOSPC, EPIPE, EACCES...). An expected runtime failure, not a bug.
        @PROJ@::app::last_resort(e.what());
        rc = @PROJ@::exit_code::failure;
    } catch (const std::exception& e) {
        @PROJ@::app::last_resort(e.what());
        rc = @PROJ@::exit_code::internal;
    } catch (...) {
        @PROJ@::app::last_resort("unknown exception");
        rc = @PROJ@::exit_code::internal;
    }
    return @PROJ@::app::finish_stdout(rc);
}
```
