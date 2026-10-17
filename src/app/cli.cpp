// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.hpp"

#include "cmd_hello.hpp"
#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/version.hpp"

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

namespace mangadex_dl::app {
namespace {

constexpr std::string_view app_description{
    "Downloads volumes and chapters from Mangadex and converts them into .epub or .pdf files."};

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

} // namespace mangadex_dl::app
