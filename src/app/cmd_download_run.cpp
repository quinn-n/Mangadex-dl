// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_download_run.hpp"

#include "cmd_download_output.hpp"
#include "io.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/curl_transport.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/title_dir.hpp"
#include "mangadex_dl/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <sys/random.h>
#include <sys/types.h>

namespace mangadex_dl::app {
namespace {

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

/// print_error with the row prefix in front of the message (sanitized together by print_error).
void print_row_error(Io& io, const OutputPolicy& policy, const RowContext& row,
                     std::string_view message) {
    std::string text = row.prefix;
    text += message;
    print_error(io, policy, text);
}

} // namespace

std::string user_agent() { return std::format("{}/{}", tool_name, version()); }

std::function<std::uint32_t(std::uint32_t)> make_jitter() {
    return [engine = std::mt19937_64{jitter_seed()}](std::uint32_t max) mutable {
        return std::uniform_int_distribution<std::uint32_t>{0, max}(engine);
    };
}

Result<Backend> make_backend(const ApiBase& api_base, const std::string& agent) {
    if (api_base.kind() == ApiBase::Kind::fixture) {
        Result<std::unique_ptr<Transport>> transport =
            make_fixture_transport(api_base.fixture_root());
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

std::string with_remedy(const Error& error) {
    if (error.exit_code() == exit_code::network) {
        return error.message +
               "; check your network connection or proxy (https_proxy) and try again";
    }
    return error.message;
}

std::string no_chapters_message(const RawDownloadArgs& args, const RunReport& report) {
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

std::string incomplete_message(const RunSummary& summary, std::size_t total, bool dry_run) {
    const std::string_view noun = total == 1 ? "file" : "files";
    if (dry_run) {
        return std::format("{} of {} {} would fail", summary.failed, total, noun);
    }
    return std::format("{} of {} {} failed; run the same command again to retry them",
                       summary.failed, total, noun);
}

TitleRun run_title(const DownloadOptions& options, const RawDownloadArgs& raw,
                   const Backend& backend, const std::string& agent, Io& io,
                   const OutputPolicy& policy, const RowContext& row, TitleDirResolver* title_dir) {
    // Lifetime: `backend` (the caller's) owns what `services` borrows; both outlive run_download.
    // Assigned member by member: with designated initializers clang-analyzer 23 loses track
    // of the members' destructors and reports the stop_source and std::function as leaked.
    // `stop` keeps its default: a fresh stop state that run_download's InterruptBridge drives.
    DownloadServices services{};
    services.transport = backend.transport.get();
    services.clock = backend.clock;
    services.jitter = make_jitter();
    services.user_agent = agent;
    services.title_dir = title_dir; // borrowed for run_download only; null = the plain stem

    // Borrows `io` (a parameter, so it outlives the observer); owns its copy of `row`.
    ConsoleObserver observer{io, policy, row};
    Result<RunReport> report = run_download(options, services, observer);
    // On every path, before anything else is printed: clears the progress line and prints
    // the "N files are already up to date" note (unless the row leaves it to the caller).
    observer.finish();

    TitleRun run{.up_to_date = observer.up_to_date()};
    if (!report) {
        run.exit_code = report.error().exit_code();
        // Interrupted: no message. main() re-raises the signal, so the shell sees 130/143;
        // the exit code is only the fallback when re-raising is impossible.
        if (report.error().code != Errc::interrupted) {
            print_row_error(io, policy, row, with_remedy(report.error()));
            run.closing_error = report.error().message;
        }
        run.error = std::move(report).error(); // --json: no document on exits 1, 3 and 4
        return run;
    }
    if (report->epubs.empty()) {
        // Exit 5: always explained on stderr; with --json the document still follows on stdout.
        run.closing_error = no_chapters_message(raw, *report);
        print_row_error(io, policy, row, run.closing_error);
    }
    if (const RunSummary summary = summarize(*report); summary.failed > 0) {
        // Exit 6: one closing line after the per-file errors (also with --json).
        run.closing_error = incomplete_message(summary, report->epubs.size(), report->dry_run);
        print_row_error(io, policy, row, run.closing_error);
    }
    run.exit_code = exit_code_for(*report);
    run.report = std::move(*report);
    return run;
}

} // namespace mangadex_dl::app
