// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_download.hpp"

#include "cmd_download_options.hpp"
#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>

#include <memory>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>
#include <string_view>

namespace mangadex_dl::app {
namespace {

/// The `download --help` footer, before the exit-status table. Kept aligned (and within 80
/// columns): the root formatter has footer formatting off.
constexpr std::string_view download_footer{
    "Examples:\n"
    "  Every volume and loose chapter in English, one .epub per volume, into ~/Manga:\n"
    "    mangadex-dl download https://mangadex.org/title/<uuid> ~/Manga\n"
    "  Volumes 1 to 3 only, in Brazilian Portuguese:\n"
    "    mangadex-dl download <uuid> --volumes 1-3 --loose-chapters none -l pt-br\n"
    "  Everything in one file, preferring one scanlation group:\n"
    "    mangadex-dl download <uuid> --layout single --group \"Group Name\"\n"
    "  Show which files would be written, without downloading anything:\n"
    "    mangadex-dl download <uuid> --dry-run\n"
    "\n"
    "Running the same command again skips files that already hold every chapter\n"
    "and rebuilds a file when MangaDex has added chapters to it, downloading only\n"
    "the new ones, so an interrupted run can be restarted and a series kept up to\n"
    "date.\n"
    "\n"
    "For many titles at once, see 'mangadex-dl batch --help'.\n"
    "\n"};

/// A semantic usage error: `mangadex-dl: error: <message>` plus the same hint line as a CLI11 parse
/// error (cli.cpp), exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error) {
    std::print(io.err, "{}: error: {}\nRun '{} download --help' for more information.\n", tool_name,
               terminal_safe(error.message), tool_name);
    return error.exit_code();
}

class DownloadCommand final : public Command {
public:
    /// Binds options to members of this object (never to locals of register_download).
    void bind(CLI::App& sub) {
        // Help text: each description fits the 44-column right column on one line where it can.
        // Positionals carry no type name: MANGA and DIR already say what they are.
        sub.add_option("MANGA", args_.manga, "UUID or https://mangadex.org/title/<uuid>")
            ->type_name("")
            ->required();
        sub.add_option("DIR", args_.dir, "Output directory (created if missing)")
            ->type_name("")
            ->capture_default_str();
        bind_selection_options(sub, args_);
        // Not bound to the std::optional directly: CLI11 turns an empty value into nullopt, which
        // would silently ignore `--title ""`. The callback only stores the value as given (like a
        // binding), so validation rejects an empty title. It captures a pointer into this Command,
        // which outlives the app.
        sub.add_option_function<std::string>(
               "--title", [title = &args_.title](const std::string& value) { *title = value; },
               "Title for file names and metadata")
            ->type_name("TEXT");
        bind_run_options(sub, args_);
        sub.footer(std::string{download_footer} + exit_status_footer());
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        const Result<DownloadOptions> options = validate_download_args(args_);
        if (!options) {
            return usage_failure(io, options.error());
        }
        const OutputPolicy policy = output_policy(globals, io);

        // Restores the previous SIGINT/SIGTERM dispositions when run() returns; main() then
        // re-raises a pending signal after finish_stdout().
        const Result<ScopedInterruptHandlers> handlers = ScopedInterruptHandlers::install();
        if (!handlers) {
            print_error(io, policy, with_remedy(handlers.error()));
            return handlers.error().exit_code();
        }

        // Lifetime: `backend` owns what run_title's services borrow; it outlives run_title below.
        const std::string agent = user_agent();
        const Result<Backend> backend = make_backend(options->api_base, agent);
        if (!backend) {
            print_error(io, policy, with_remedy(backend.error()));
            return backend.error().exit_code();
        }
        // Prints the path stream, the per-file lines and the closing line; never the JSON.
        const TitleRun run = run_title(*options, args_, *backend, agent, io, policy, RowContext{});
        if (policy.json && run.report) {
            print_report_json(io, *run.report); // --json: no document on exits 1, 3 and 4
        }
        return run.exit_code;
    }

private:
    RawDownloadArgs args_{};
};

} // namespace

std::unique_ptr<Command> register_download(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<DownloadCommand>();
    CLI::App* sub = parent.add_subcommand("download", "Download a MangaDex title as EPUB files");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in run()'s
    // Invocation and `self` is owned by run()'s command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace mangadex_dl::app
