// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.hpp"

#include "cmd_batch.hpp"
#include "cmd_download.hpp"
#include "cmd_download_output.hpp"
#include "cmd_follows.hpp"
#include "cmd_login.hpp"
#include "cmd_mdlist.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/text_safety.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
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

/// "Run 'mangadex-dl --help' ..." or, once a subcommand was reached, "Run 'mangadex-dl <command>
/// --help' ...": the one hint line every usage error ends with.
[[nodiscard]] std::string help_hint(const CLI::App& app) {
    const std::vector<CLI::App*> reached = app.get_subcommands();
    if (reached.empty()) {
        return std::format("Run '{} --help' for more information.\n", tool_name);
    }
    return std::format("Run '{} {} --help' for more information.\n", tool_name,
                       reached.front()->get_name());
}

/// The reached subcommand's name, if it is one of `names` (string literals, static storage).
[[nodiscard]] std::optional<std::string_view>
reached_one_of(const CLI::App& app, std::initializer_list<std::string_view> names) {
    const std::vector<CLI::App*> reached = app.get_subcommands();
    if (reached.empty()) {
        return std::nullopt;
    }
    const std::string& name = reached.front()->get_name();
    for (const std::string_view candidate : names) {
        if (name == candidate) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// `batch ... --title X`, `mdlist ... --title X` or `follows ... --title X` (or `--title=X`):
/// CLI11 sends the unknown option to the root, which rejects it as "not expected". None of them
/// has a --title (a batch row names its own title; one title for every list item or follow would
/// make the file names collide), so failure_text says that instead. Only for ExtrasError once one
/// of the three was reached. Returns the command's name (a string literal, static storage), or
/// nullopt.
[[nodiscard]] std::optional<std::string_view> title_option_owner(const CLI::App& app,
                                                                 const CLI::Error& error) {
    if (error.get_name() != "ExtrasError") {
        return std::nullopt;
    }
    const std::optional<std::string_view> owner =
        reached_one_of(app, {"batch", "mdlist", "follows"});
    if (!owner.has_value()) {
        return std::nullopt;
    }
    const bool got_title = std::ranges::any_of(app.remaining(true), [](const std::string& word) {
        return word == "--title" || word.starts_with("--title=");
    });
    return got_title ? owner : std::nullopt;
}

// ---- the echo policy of follows, login and logout ----------------------------------------------
//
// Their arguments may hold a password (`follows --password hunter2`, `login hunter2`, a password
// typed after a flag that takes no value), so no parse error of theirs repeats a value or a bare
// word: only option names are echoed, cut at '='. CLI11's stock texts for ExtrasError and
// ConversionError list the words verbatim, so failure_text rebuilds both; every other ParseError
// (a ValidationError of --jobs, --layout or --color, a missing value) keeps CLI11's text, which
// names only the option and the user's value for a non-secret option.

/// The longest option name echoed in an extras message, in bytes (cut on a UTF-8 boundary).
constexpr std::size_t max_echoed_name_bytes = 64;

/// The commands whose parse errors never echo a value. Returns the name (static storage).
[[nodiscard]] std::optional<std::string_view> value_hiding_command(const CLI::App& app) {
    return reached_one_of(app, {"follows", "login", "logout"});
}

/// Whether `word` starts with `prefix`, ignoring ASCII case.
[[nodiscard]] bool starts_with_ignoring_case(std::string_view word,
                                             std::string_view prefix) noexcept {
    if (word.size() < prefix.size()) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    return std::ranges::equal(word.substr(0, prefix.size()), prefix,
                              [&lower](char a, char b) { return lower(a) == lower(b); });
}

/// An attempt to pass a secret on the command line: a long option starting, in any ASCII case,
/// with `--pass` (`--password`, `--passwd`, ...), `--pw`, `--secret`, `--client-secret` or
/// `--client_secret`, or a short `-p...` (`-p`, `-phunter2`, the rest of `-qphunter2` once
/// CLI11 has taken the `-q`).
[[nodiscard]] bool is_secret_shaped(std::string_view word) noexcept {
    constexpr std::array<std::string_view, 5> long_prefixes{"--pass", "--pw", "--secret",
                                                            "--client-secret", "--client_secret"};
    return word.starts_with("-p") ||
           std::ranges::any_of(long_prefixes, [word](std::string_view prefix) {
               return starts_with_ignoring_case(word, prefix);
           });
}

/// `--name`, `--name=...` or exactly `-x`: echoed (the part before '='). A single-dash word longer
/// than 2 characters (`-xyz`, `-qhunter2`) may be a short option glued to its value, so it is
/// only counted, as are bare words, empty words and `--`.
[[nodiscard]] bool is_echoable_option(std::string_view word) noexcept {
    if (word.starts_with("--")) {
        return word.size() > 2;
    }
    return word.size() == 2 && word.front() == '-';
}

/// `text` cut to at most `limit` bytes, on a UTF-8 boundary (the input is valid UTF-8: it comes
/// from terminal_safe).
[[nodiscard]] std::string cut_utf8(std::string text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t length = limit;
    while (length > 0 && detail::is_utf8_continuation(static_cast<unsigned char>(text[length]))) {
        --length;
    }
    text.resize(length);
    return text;
}

/// The option name of an echoable word: the part before '=', terminal_safe, at most
/// max_echoed_name_bytes.
[[nodiscard]] std::string echoed_name(std::string_view word) {
    const std::string_view name = word.substr(0, word.find('='));
    return cut_utf8(terminal_safe(name), max_echoed_name_bytes);
}

/// `follows has no --password option: ...`: the fixed text for a secret-shaped extra.
[[nodiscard]] std::string secret_option_text(const CLI::App& app, std::string_view command) {
    return std::format("{}: error: {} has no --password option: passwords and secrets never go on "
                       "the command line (other users can see it); use the prompt, "
                       "--password-stdin or the credentials file\n{}",
                       tool_name, command, help_hint(app));
}

/// An ExtrasError of a value-hiding command: the option names (cut at '='), and a count of the
/// other words. `<cmd> does not take these arguments: <names> (and N other words, not shown in
/// case they hold a password)`, or, without a name, `<cmd> does not take N more arguments (not
/// shown in case they hold a password)`.
[[nodiscard]] std::string hidden_extras_text(const CLI::App& app, std::string_view command) {
    std::string names{};
    std::size_t hidden = 0;
    for (const std::string& word : app.remaining(true)) {
        if (!is_echoable_option(word)) {
            ++hidden;
            continue;
        }
        if (!names.empty()) {
            names += ", ";
        }
        names += echoed_name(word);
    }
    if (names.empty()) {
        return std::format("{}: error: {} does not take {} more {} (not shown in case {} a "
                           "password)\n{}",
                           tool_name, command, hidden, hidden == 1 ? "argument" : "arguments",
                           hidden == 1 ? "it holds" : "they hold", help_hint(app));
    }
    std::string others{};
    if (hidden > 0) {
        others =
            std::format(" (and {} other {}, not shown in case {} a password)", hidden,
                        hidden == 1 ? "word" : "words", hidden == 1 ? "it holds" : "they hold");
    }
    return std::format("{}: error: {} does not take these arguments: {}{}\n{}", tool_name, command,
                       names, others, help_hint(app));
}

/// The option a ConversionError names: `Could not convert: <name> = <values>` (V35), with a
/// name like `--dry-run` or `-n`; nullopt for any other text.
[[nodiscard]] std::optional<std::string> converted_option_name(std::string_view message) {
    constexpr std::string_view prefix = "Could not convert: ";
    if (!message.starts_with(prefix)) {
        return std::nullopt;
    }
    message.remove_prefix(prefix.size());
    const std::size_t end = message.find(" = ");
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = message.substr(0, end);
    // ^--?[A-Za-z0-9][A-Za-z0-9-]*$
    std::string_view rest = name;
    if (!rest.starts_with('-')) {
        return std::nullopt;
    }
    rest.remove_prefix(rest.starts_with("--") ? 2 : 1);
    const auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (rest.empty() || !alnum(rest.front()) ||
        !std::ranges::all_of(rest, [&alnum](char c) { return alnum(c) || c == '-'; })) {
        return std::nullopt;
    }
    return std::string{name};
}

/// The option called `name` on the reached subcommand, else on the root; null if neither has it.
/// Borrowed from the app (valid while it lives).
[[nodiscard]] const CLI::Option* find_option(const CLI::App& app, const std::string& name) {
    const std::vector<CLI::App*> reached = app.get_subcommands();
    if (!reached.empty()) {
        if (const CLI::Option* option = reached.front()->get_option_no_throw(name)) {
            return option;
        }
    }
    return app.get_option_no_throw(name);
}

/// `<name> takes no value` for a flag given `=value`, else `<name>: invalid value (not shown)`.
/// `name` passed converted_option_name's check; `option` is the one it names.
[[nodiscard]] std::string value_free_conversion_text(const CLI::App& app, std::string_view name,
                                                     const CLI::Option& option) {
    if (option.get_expected_max() == 0) {
        return std::format("{}: error: {} takes no value\n{}", tool_name, name, help_hint(app));
    }
    // Defensive: unreachable with the current options (the value options --jobs, --layout and
    // --color reject a bad value with a ValidationError, which keeps CLI11's text).
    return std::format("{}: error: {}: invalid value (not shown)\n{}", tool_name, name,
                       help_hint(app));
}

/// A ConversionError of a value-hiding command, without the value: `<flag> takes no value` for a
/// flag given `=value`, `<option>: invalid value (not shown)` for an option, or a generic line
/// when the option can't be identified (on the reached subcommand, then on the root).
[[nodiscard]] std::string hidden_conversion_text(const CLI::App& app, const CLI::Error& error,
                                                 std::string_view command) {
    const std::optional<std::string> name = converted_option_name(error.what());
    const CLI::Option* const option = name.has_value() ? find_option(app, *name) : nullptr;
    if (!name.has_value() || option == nullptr) {
        // Defensive: unreachable with the current options (every ConversionError of theirs reads
        // `Could not convert: <option> = <value>` for an option they have).
        return std::format("{}: error: {}: an option was given a value it can't take (the value "
                           "is not shown)\n{}",
                           tool_name, command, help_hint(app));
    }
    return value_free_conversion_text(app, *name, *option);
}

/// A ConversionError of a root (global) option, such as `--version=<value>` or `--json=<value>`,
/// on any command or none, without the value: value_free_conversion_text. nullopt when the error
/// names no root option (a download, batch or mdlist option keeps CLI11's text).
[[nodiscard]] std::optional<std::string> global_conversion_text(const CLI::App& app,
                                                                const CLI::Error& error) {
    const std::optional<std::string> name = converted_option_name(error.what());
    const CLI::Option* const option = name.has_value() ? app.get_option_no_throw(*name) : nullptr;
    if (!name.has_value() || option == nullptr) {
        return std::nullopt;
    }
    return value_free_conversion_text(app, *name, *option);
}

/// failure_text before any subcommand was reached: the "subcommand required" error (or extras
/// on the root) becomes "unknown command 'x'" or "unknown option '-x'" (the name only: cut at
/// '=', a single-dash word to 2 characters; a secret-shaped option gets the fixed password text
/// instead) for the first non-empty leftover, or "missing command". nullopt for any other error,
/// or once a subcommand was reached.
[[nodiscard]] std::optional<std::string> root_failure_text(const CLI::App& app,
                                                           const CLI::Error& error) {
    if (!app.get_subcommands().empty()) {
        return std::nullopt;
    }
    const std::string name = error.get_name();
    if (name != "RequiredError" && name != "ExtrasError") {
        return std::nullopt;
    }
    // The first non-empty leftover decides: an option the root doesn't know, or a word that
    // isn't a command. Empty words ('') say nothing, so they count as no command.
    for (const std::string& word : app.remaining()) {
        if (word.empty()) {
            continue;
        }
        if (is_secret_shaped(word)) {
            return std::format("{}: error: there is no --password option: passwords and secrets "
                               "never go on the command line (other users can see it); use the "
                               "prompt, --password-stdin or the credentials file\n{}",
                               tool_name, help_hint(app));
        }
        if (word.starts_with('-')) {
            // The name only: cut at '=', so `--x=<value>` never shows the value, and a single-dash
            // word to its first two characters (`-xhunter2` may be `-x` glued to its value).
            const std::string_view shown = word.starts_with("--")
                                               ? std::string_view{word}
                                               : std::string_view{word}.substr(0, 2);
            return std::format("{}: error: unknown option '{}'\n{}", tool_name, echoed_name(shown),
                               help_hint(app));
        }
        // The command list is spelled out here, next to the registration list in run(): keep
        // the two in step.
        return std::format("{}: error: unknown command '{}' (commands: download, batch, mdlist, "
                           "follows, login, logout)\n{}",
                           tool_name, terminal_safe(word), help_hint(app));
    }
    return std::format("{}: error: missing command; try '{} --help'\n", tool_name, tool_name);
}

/// The text App::exit prints for a parse error (never for --help or --version):
/// `mangadex-dl: error: <message>` plus the hint line. `app` is the root app (run() calls its
/// exit()). Without a subcommand, root_failure_text decides; `batch`, `mdlist` or `follows` with
/// a leftover --title says that the command has no such option; `follows`, `login` and `logout`
/// follow the echo policy above; a global flag given `=value` (`--version=x`) never shows the
/// value, whatever the command. User text goes through terminal_safe.
[[nodiscard]] std::string failure_text(const CLI::App* app, const CLI::Error& error) {
    if (std::optional<std::string> root = root_failure_text(*app, error)) {
        return std::move(*root);
    }
    const std::optional<std::string_view> hiding = value_hiding_command(*app);
    const bool extras = error.get_name() == "ExtrasError";
    if (hiding.has_value() && extras &&
        std::ranges::any_of(app->remaining(true),
                            [](const auto& w) { return is_secret_shaped(w); })) {
        return secret_option_text(*app, *hiding); // before --title: nothing else is said
    }
    if (const std::optional<std::string_view> owner = title_option_owner(*app, error)) {
        if (*owner == "batch") {
            return std::format("{}: error: batch has no --title option; give each row a 'title' "
                               "column instead\n{}",
                               tool_name, help_hint(*app));
        }
        return std::format("{}: error: {} has no --title option; to name a title yourself, "
                           "download it on its own with '{} download <title> --title ...'\n{}",
                           tool_name, *owner, tool_name, help_hint(*app));
    }
    if (hiding.has_value() && extras) {
        return hidden_extras_text(*app, *hiding);
    }
    if (hiding.has_value() && error.get_name() == "ConversionError") {
        return hidden_conversion_text(*app, error, *hiding);
    }
    if (error.get_name() == "ConversionError") {
        if (std::optional<std::string> global = global_conversion_text(*app, error)) {
            return std::move(*global);
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
    // Keep failure_text's "(commands: ...)" list in step with it.
    commands.push_back(register_download(app, inv.selected));
    commands.push_back(register_batch(app, inv.selected));
    commands.push_back(register_mdlist(app, inv.selected));
    commands.push_back(register_follows(app, inv.selected));
    commands.push_back(register_login(app, inv.selected));
    commands.push_back(register_logout(app, inv.selected));

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
