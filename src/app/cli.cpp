// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.hpp"

#include "cmd_download.hpp"
#include "cmd_download_output.hpp"
#include "command.hpp"
#include "exit_help.hpp"
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
    "Downloads volumes and chapters from MangaDex and converts them into .epub or .pdf files."};

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

/// "Run 'mangadex-dl --help' ..." or, once a subcommand was reached, "Run 'mangadex-dl download
/// --help' ...": the one hint line every usage error ends with.
[[nodiscard]] std::string help_hint(const CLI::App& app) {
    const std::vector<CLI::App*> reached = app.get_subcommands();
    if (reached.empty()) {
        return std::format("Run '{} --help' for more information.\n", tool_name);
    }
    return std::format("Run '{} {} --help' for more information.\n", tool_name,
                       reached.front()->get_name());
}

/// The text App::exit prints for a parse error (never for --help or --version):
/// `mangadex-dl: error: <message>` plus the hint line. `app` is the root app (run() calls its
/// exit()). Without a subcommand, the "subcommand required" error becomes "unknown command 'x'"
/// or "unknown option '-x'" for the first non-empty leftover, or "missing command". User text
/// goes through terminal_safe.
[[nodiscard]] std::string failure_text(const CLI::App* app, const CLI::Error& error) {
    if (app->get_subcommands().empty()) {
        const std::string name = error.get_name();
        if (name == "RequiredError" || name == "ExtrasError") {
            // The first non-empty leftover decides: an option the root doesn't know, or a word
            // that isn't a command. Empty words ('') say nothing, so they count as no command.
            for (const std::string& word : app->remaining()) {
                if (word.empty()) {
                    continue;
                }
                if (word.starts_with('-')) {
                    return std::format("{}: error: unknown option '{}'\n{}", tool_name,
                                       terminal_safe(word), help_hint(*app));
                }
                return std::format("{}: error: unknown command '{}' (the only command is "
                                   "'download')\n{}",
                                   tool_name, terminal_safe(word), help_hint(*app));
            }
            return std::format("{}: error: missing command; try '{} download --help'\n", tool_name,
                               tool_name);
        }
    }
    std::string_view message{error.what()};
    const std::string own_prefix = std::format("{}: ", tool_name);
    if (message.starts_with(own_prefix)) { // CLI11 names the app in some messages (ExtrasError)
        message.remove_prefix(own_prefix.size());
    }
    return std::format("{}: error: {}\n{}", tool_name, terminal_safe(message), help_hint(*app));
}

/// Help layout: names in a 36-column left column (long names aligned at column 6), descriptions
/// wrapped at 44, so every line fits 80 columns. Footers are pre-aligned text. The formatter is
/// held by shared pointer, so a const App still configures it (and subcommands share it).
void configure_help(const CLI::App& app) {
    const std::shared_ptr<CLI::FormatterBase> formatter = app.get_formatter();
    formatter->column_width(36);
    formatter->long_option_alignment_ratio(0.17F);
    formatter->right_column_width(44);
    formatter->enable_footer_formatting(false); // keep the tables' alignment
}

void add_global_options(CLI::App& app, Invocation& inv) {
    app.add_flag("--json", inv.globals.json, "Write machine-readable JSON to stdout");
    app.add_flag("-q,--quiet", inv.globals.quiet, "Only print errors");
    // option_text only replaces the help column's " [0]" (the counter's default_str, which CLI11
    // needs for parsing); it changes nothing else.
    app.add_flag("-v,--verbose", inv.globals.verbose, "Print more detail to stderr (repeatable)")
        ->option_text(" ");
    app.add_option("--color", inv.color_word, "Color stderr: auto, always or never")
        ->type_name("WORD")
        ->check(CLI::IsMember({"auto", "always", "never"}).description(""))
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
    // Before any add_subcommand: subcommands copy the formatter and the failure message.
    configure_help(app);
    app.failure_message(failure_text);
    app.set_version_flag("--version", std::string{version_line()},
                         "Print the version and exit"); // no -v: that is --verbose
    app.footer(exit_status_footer());
    add_global_options(app, inv);

    // Registration list, one line per subcommand (a serialization point: one track at a time).
    commands.push_back(register_download(app, inv.selected));

    // argc may be 0 (execve with an empty argv). Never index argv[0] unchecked; give CLI11 a name.
    const std::string argv0{tool_name};
    const std::array<const char*, 1> fallback{argv0.c_str()};
    const std::span<const char* const> argv =
        args.empty() ? std::span<const char* const>{fallback} : args;
    if (!std::in_range<int>(argv.size())) {
        std::print(io.err, "{}: error: too many arguments\n", tool_name);
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
        std::print(io.err, "{}: error: no command was selected (a bug: please report it)\n",
                   tool_name);
        return exit_code::internal;
    }
    return inv.selected->run(io, inv.globals);
}

} // namespace mangadex_dl::app
