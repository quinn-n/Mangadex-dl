// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_download.hpp"

#include "cmd_download_output.hpp"
#include "command.hpp"
#include "exit_help.hpp"
#include "io.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/curl_transport.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <sys/random.h>
#include <sys/types.h>

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
    "\n"};

/// The User-Agent MangaDex asks for: identifies the tool, never spoofed, no Via header.
[[nodiscard]] std::string user_agent() { return std::format("{}/{}", tool_name, version()); }

/// A seed for the retry jitter. getrandom(2) without blocking (an 8-byte request is never cut
/// short once the pool is ready); the steady clock when it is unavailable. Jitter only spreads
/// retries apart, so a predictable fallback seed is harmless.
[[nodiscard]] std::uint64_t jitter_seed() noexcept {
    std::uint64_t seed = 0;
    const ssize_t got = ::getrandom(&seed, sizeof seed, GRND_NONBLOCK);
    if (std::cmp_equal(got, sizeof seed)) {
        return seed;
    }
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

/// `jitter(max)` returns a value in [0, max]. Seeded once per run. The engine lives inside the
/// callable (captured by value), and ApiClient serializes its calls, so it needs no lock.
[[nodiscard]] std::function<std::uint32_t(std::uint32_t)> make_jitter() {
    return [engine = std::mt19937_64{jitter_seed()}](std::uint32_t max) mutable {
        return std::uniform_int_distribution<std::uint32_t>{0, max}(engine);
    };
}

/// The transport and clock behind one run. Declared before the DownloadServices that borrow them,
/// so it outlives run_download.
struct Backend {
    std::unique_ptr<Transport> transport{};
    std::unique_ptr<Clock> owned_clock{}; ///< the instant clock in fixture mode; null otherwise
    Clock* clock = nullptr;               ///< owned_clock.get() or the process-wide system_clock()
};

/// Fixture mode (`--api-base file://...`): the hermetic file transport and an instant clock, so no
/// retry ever sleeps and libcurl is never loaded. Otherwise: libcurl and the real clock.
[[nodiscard]] Result<Backend> make_backend(const DownloadOptions& options,
                                           const std::string& agent) {
    if (options.api_base.kind() == ApiBase::Kind::fixture) {
        Result<std::unique_ptr<Transport>> transport =
            make_fixture_transport(options.api_base.fixture_root());
        if (!transport) {
            return std::unexpected(std::move(transport).error());
        }
        std::unique_ptr<Clock> clock = make_instant_clock();
        Clock* const borrowed = clock.get(); // heap object: stable across moves of Backend
        return Backend{
            .transport = std::move(*transport), .owned_clock = std::move(clock), .clock = borrowed};
    }
    Result<std::unique_ptr<Transport>> transport =
        make_curl_transport(CurlTransportConfig{.user_agent = agent});
    if (!transport) {
        return std::unexpected(std::move(transport).error());
    }
    return Backend{
        .transport = std::move(*transport), .owned_clock = nullptr, .clock = &system_clock()};
}

/// A semantic usage error: `mangadex-dl: error: <message>` plus the same hint line as a CLI11 parse
/// error (cli.cpp), exit 2, nothing on stdout.
[[nodiscard]] int usage_failure(Io& io, const Error& error) {
    std::print(io.err, "{}: error: {}\nRun '{} download --help' for more information.\n", tool_name,
               terminal_safe(error.message), tool_name);
    return error.exit_code();
}

/// The exit-5 diagnostic: what was asked for (as typed; print_error makes it terminal-safe) and
/// how many chapters were skipped for a reason other than dedupe.
[[nodiscard]] std::string no_chapters_message(const RawDownloadArgs& args,
                                              const RunReport& report) {
    std::string message =
        std::format("no downloadable chapters matched --lang {} with --volumes {} "
                    "--loose-chapters {}",
                    args.lang, args.volumes, args.loose_chapters);
    const auto skipped = static_cast<std::size_t>(
        std::ranges::count_if(report.skipped, [](const SkippedChapter& chapter) {
            return chapter.reason != SkipReason::duplicate;
        }));
    if (skipped > 0) {
        message += std::format(" ({} {} skipped: external, no pages or unavailable)", skipped,
                               skipped == 1 ? "chapter" : "chapters");
    }
    message += "; try another --lang or a wider --volumes/--loose-chapters";
    return message;
}

/// An Error's message, plus what to do about it where the user can act (exit 3: the network).
[[nodiscard]] std::string with_remedy(const Error& error) {
    if (error.exit_code() == exit_code::network) {
        return error.message +
               "; check your network connection or proxy (https_proxy) and try again";
    }
    return error.message;
}

/// The exit-6 closing line: how many files failed, and that a rerun retries only those.
/// A dry run only plans, so its failures (a file that can't be planned) won't clear on a rerun.
[[nodiscard]] std::string incomplete_message(const RunSummary& summary, std::size_t total,
                                             bool dry_run) {
    const std::string_view noun = total == 1 ? "file" : "files";
    if (dry_run) {
        return std::format("{} of {} {} would fail", summary.failed, total, noun);
    }
    return std::format("{} of {} {} failed; run the same command again to retry them",
                       summary.failed, total, noun);
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
        sub.add_option("--volumes", args_.volumes, "Volumes: all, none or a list like 1,3-5,8-")
            ->type_name("SPEC")
            ->capture_default_str();
        sub.add_option("--loose-chapters", args_.loose_chapters,
                       "Chapters with no volume (SPEC as --volumes)")
            ->type_name("SPEC")
            ->capture_default_str();
        sub.add_option("--layout", args_.layout, "Split into: volumes, chapters or single file")
            ->type_name("WORD")
            ->check(CLI::IsMember({"volumes", "chapters", "single"}).description(""))
            ->capture_default_str();
        sub.add_option("-l,--lang", args_.lang, "Language to download (e.g. en, pt-br)")
            ->type_name("CODE")
            ->capture_default_str();
        // expected(1) alone rejects a repeated --group; TakeAll alone swallows the next positional.
        sub.add_option("--group", args_.groups, "Preferred group; repeat in priority order")
            ->type_name("NAME|UUID")
            ->expected(1)
            ->allow_extra_args(false)
            ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
        sub.add_flag("--data-saver", args_.data_saver,
                     "Download compressed images (smaller files)");
        sub.add_option("--direction", args_.direction, "Page order: auto (by origin), rtl or ltr")
            ->type_name("WORD")
            ->check(CLI::IsMember({"auto", "rtl", "ltr"}).description(""))
            ->capture_default_str();
        // Not bound to the std::optional directly: CLI11 turns an empty value into nullopt, which
        // would silently ignore `--title ""`. The callback only stores the value as given (like a
        // binding), so validation rejects an empty title. It captures a pointer into this Command,
        // which outlives the app.
        sub.add_option_function<std::string>(
               "--title", [title = &args_.title](const std::string& value) { *title = value; },
               "Title for file names and metadata")
            ->type_name("TEXT");
        sub.add_flag("--overwrite", args_.overwrite, "Re-download files even if up to date");
        sub.add_flag("-n,--dry-run", args_.dry_run, "List the files to write; download nothing");
        sub.add_option("-j,--jobs", args_.jobs, "Parallel image downloads, 1 to 8")
            ->type_name("N")
            ->check(CLI::Range(1, 8).description(""))
            ->capture_default_str();
        sub.add_flag("--no-report", args_.no_report, "Skip MangaDex@Home reliability reports");
        // Hidden (empty group): a test seam for the hermetic fixture tree and loopback servers.
        sub.add_option("--api-base", args_.api_base, "API base URL")->group("");
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

        // Lifetime: `backend` owns what `services` borrows; both outlive run_download below.
        const std::string agent = user_agent();
        const Result<Backend> backend = make_backend(*options, agent);
        if (!backend) {
            print_error(io, policy, with_remedy(backend.error()));
            return backend.error().exit_code();
        }
        // Assigned member by member: with designated initializers clang-analyzer 23 loses track
        // of the members' destructors and reports the stop_source and std::function as leaked.
        // `stop` keeps its default: a fresh stop state that run_download's InterruptBridge drives.
        DownloadServices services{};
        services.transport = backend->transport.get();
        services.clock = backend->clock;
        services.jitter = make_jitter();
        services.user_agent = agent;

        // Borrows `io` (a parameter, so it outlives the observer).
        ConsoleObserver observer{io, policy};
        const Result<RunReport> report = run_download(*options, services, observer);
        // On every path, before anything else is printed: clears the progress line and prints
        // the "N files are already up to date" note.
        observer.finish();

        if (!report) {
            // Interrupted: no message. main() re-raises the signal, so the shell sees 130/143;
            // the exit code is only the fallback when re-raising is impossible.
            if (report.error().code != Errc::interrupted) {
                print_error(io, policy, with_remedy(report.error()));
            }
            return report.error().exit_code(); // --json: no document on exits 1, 3 and 4
        }
        if (report->epubs.empty()) {
            // Exit 5: always explained on stderr; with --json the document still follows on stdout.
            print_error(io, policy, no_chapters_message(args_, *report));
        }
        if (const RunSummary summary = summarize(*report); summary.failed > 0) {
            // Exit 6: one closing line after the per-file errors (also with --json).
            print_error(io, policy,
                        incomplete_message(summary, report->epubs.size(), report->dry_run));
        }
        if (policy.json) {
            print_report_json(io, *report);
        }
        return exit_code_for(*report);
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
