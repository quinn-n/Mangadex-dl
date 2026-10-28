// SPDX-License-Identifier: GPL-3.0-or-later
//
// run_download (download.hpp; download plan T14): sets up the run's services, then runs the plan
// phase (src/core/download/plan_phase.cpp) and the execute phase
// (src/core/download/execute_phase.cpp) on the calling thread.
//
// --dir-per-title (list plan T6): between the phases, a non-empty plan's title directory is
// resolved and checked (src/core/download/dir_per_title.cpp), and the unchanged execute phase
// runs with DIR/<name> as its options.dir. Without the flag the two phases run as before.
//
// Stop requests: the caller's stop_source (DownloadServices::stop), an InterruptBridge that turns
// SIGINT/SIGTERM into a request on it within 50 ms, and stop_now() checks at every phase step
// that turn a pending signal into a request at once. Every stop unwinds as Errc::interrupted.
#include "mangadex_dl/download.hpp"

#include "download/internal.hpp"
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fetch_pool.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"

#include <expected>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace detail {

void LineQueue::trace(std::string line) { note(2, std::move(line)); }

void LineQueue::note(int level, std::string line) {
    const std::scoped_lock lock{mutex_};
    lines_.push_back(Line{.level = level, .text = std::move(line)});
}

void LineQueue::drain(DownloadObserver& observer) {
    std::vector<Line> taken{};
    {
        const std::scoped_lock lock{mutex_};
        taken.swap(lines_);
    }
    // Outside the lock: the observer may take its time, and workers keep queueing meanwhile.
    for (const Line& line : taken) {
        observer.on_verbose(line.level, line.text);
    }
}

// The standard declares std::stop_source::request_stop non-const ([stopsource.mem]); only
// libstdc++ declares it const, so the reference stays non-const for portability.
// NOLINTNEXTLINE(misc-const-correctness): request_stop is a non-const member in the standard
bool stop_now(std::stop_source& stop) noexcept {
    if (interrupt_requested()) {
        static_cast<void>(stop.request_stop());
    }
    return stop.stop_requested();
}

Error interrupted_error() { return Error{.code = Errc::interrupted, .message = "interrupted"}; }

} // namespace detail

Result<RunReport> run_download(const DownloadOptions& options, const DownloadServices& services,
                               DownloadObserver& observer) {
    if (services.transport == nullptr || services.clock == nullptr) {
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = "internal error: run_download needs a transport and a clock",
        });
    }
    // Declaration order is the borrow order; everything is destroyed in reverse. The line queue
    // comes before the client that traces into it. The client and the pool come before the
    // bridge, so its poller (which requests stops that cancel their work) is joined first. The
    // transport and the clock are the caller's and outlive the call.
    const std::stop_source stop = services.stop; // a copy shares the caller's stop state
    detail::LineQueue lines{};
    ApiClient client{*services.transport, *services.clock,
                     ClientConfig{.endpoints = endpoints_for(options.api_base, options.report)},
                     services.jitter, &lines};
    FetchPool pool{options.jobs};
    const InterruptBridge bridge{stop};

    detail::RunContext ctx{
        .options = &options,
        .client = &client,
        .pool = &pool,
        .observer = &observer,
        .lines = &lines,
        .stop = stop,
        .title_dir = services.title_dir,
        .claimed = &services.claimed,
    };
    Result<detail::PlanResult> planned = detail::plan_phase(ctx);
    if (!planned) {
        return std::unexpected(std::move(planned.error()));
    }
    // An empty plan (exit 5) claims no directory and leaves DIR alone, so it takes the old path.
    if (options.dir_per_title && !planned->plan.epubs.empty()) {
        Result<std::filesystem::path> title_dir = detail::resolve_title_dir(ctx, planned->plan);
        if (!title_dir) {
            return std::unexpected(std::move(title_dir.error()));
        }
        // Both locals live in this frame until execute_phase returns, after ctx (whose borrowed
        // pointers they share); nothing keeps &effective past the call. The copied stop_source
        // shares its state, so the bridge's stop requests still reach the execute phase.
        DownloadOptions effective = options;
        effective.dir = std::move(*title_dir);
        detail::RunContext title_ctx = ctx;
        title_ctx.options = &effective;
        return detail::execute_phase(title_ctx, *planned);
    }
    return detail::execute_phase(ctx, *planned);
}

} // namespace mangadex_dl
