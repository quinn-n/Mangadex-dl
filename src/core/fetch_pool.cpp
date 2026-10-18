// SPDX-License-Identifier: GPL-3.0-or-later
//
// FetchPool (download plan T8 + rev1): a fixed set of std::jthread workers, one mutex, two
// condition_variable_any, and a ring of `workers` result slots. Index i lives in slot
// i % workers; a worker may start index i only while i < consumed + workers, so a slot is always
// free when its index is handed out, and at most `workers` pages (counting the one being
// consumed) are held at once. Results reach the consumer strictly in index order, whatever the
// completion order, so the output does not depend on the number of workers.
#include "mangadex_dl/fetch_pool.hpp"

#include "mangadex_dl/error.hpp"
#include "signal_block.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

inline constexpr unsigned min_workers = 1;
inline constexpr unsigned max_workers = 8;

/// Errc::failure for an exception that escaped `where`. Never throws: if even the message cannot
/// be built, the Error has an empty message.
[[nodiscard]] Error exception_error(std::string_view where, const char* what) noexcept {
    try {
        return Error{.code = Errc::failure,
                     .message = std::format("{} failed: {}", where,
                                            what != nullptr ? what : "unknown exception")};
    } catch (...) {
        return Error{.code = Errc::failure};
    }
}

/// Runs the fetcher; any exception becomes an Error, so nothing escapes a worker thread.
[[nodiscard]] Result<FetchedPage> invoke_fetch(const PageFetcher& fetch, std::size_t index,
                                               std::stop_token stop) noexcept {
    try {
        return fetch(index, std::move(stop));
    } catch (const std::exception& e) {
        return std::unexpected(exception_error("page fetch", e.what()));
    } catch (...) {
        return std::unexpected(exception_error("page fetch", nullptr));
    }
}

/// Runs the consumer; any exception becomes an Error.
[[nodiscard]] Result<void> invoke_consume(const PageConsumer& consume, std::size_t index,
                                          FetchedPage&& page) noexcept {
    try {
        return consume(index, std::move(page));
    } catch (const std::exception& e) {
        return std::unexpected(exception_error("page consumer", e.what()));
    } catch (...) {
        return std::unexpected(exception_error("page consumer", nullptr));
    }
}

} // namespace

struct FetchPool::State {
    explicit State(std::size_t window) : ring(window) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State() = default;

    std::mutex mutex{};
    std::condition_variable_any work_ready{}; ///< workers: an index may be available
    std::condition_variable_any progress{};   ///< run(): a result arrived or a fetch finished

    // The current run; every field below is guarded by `mutex`.
    const PageFetcher* fetch = nullptr; ///< borrowed for one run(); null between runs
    std::stop_token run_token{};        ///< handed to every fetch of the run
    std::size_t count = 0;
    std::size_t next_index = 0;    ///< the next index to hand out
    std::size_t consumed = 0;      ///< indices fully consumed (their slots are free again)
    std::size_t in_flight = 0;     ///< fetches running outside the lock
    bool stop_handing_out = false; ///< an error or the end of the run: start nothing new
    std::vector<std::optional<Result<FetchedPage>>> ring{}; ///< size == workers; never resized

    // Last member: destroyed first, so every worker is stopped and joined while the members
    // above (which the workers use) still exist.
    std::vector<std::jthread> workers{};

    [[nodiscard]] std::size_t window() const noexcept { return ring.size(); }

    [[nodiscard]] bool has_task() const noexcept {
        return fetch != nullptr && !stop_handing_out && next_index < count &&
               next_index < consumed + window();
    }

    /// The body of every worker thread, until the pool is destroyed.
    void worker_loop(const std::stop_token& token) {
        std::unique_lock lock{mutex};
        while (true) {
            static_cast<void>(work_ready.wait(lock, token, [this] { return has_task(); }));
            if (token.stop_requested()) {
                return;
            }
            const std::size_t index = next_index++;
            ++in_flight;
            const PageFetcher& fetcher = *fetch; // valid until in_flight drops back to 0
            std::stop_token stop = run_token;
            lock.unlock();

            Result<FetchedPage> result = invoke_fetch(fetcher, index, std::move(stop));

            lock.lock();
            const bool failed = !result.has_value();
            ring[index % window()].emplace(std::move(result));
            if (failed) {
                stop_handing_out = true; // later indices are useless; earlier ones still finish
            }
            --in_flight;
            progress.notify_all();
        }
    }

    /// Hands results 0..count-1 to `consume` in order. Returns at the first error or stop
    /// request; the caller drains the workers afterwards.
    [[nodiscard]] Result<void> consume_in_order(const PageConsumer& consume,
                                                const std::stop_token& stop) {
        std::unique_lock lock{mutex};
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t slot = index % window();
            static_cast<void>(
                progress.wait(lock, stop, [this, slot] { return ring[slot].has_value(); }));
            // The wait ends with the slot filled or with a stop request; a stop wins even when
            // the page is ready.
            std::optional<Result<FetchedPage>> taken = std::exchange(ring[slot], std::nullopt);
            if (stop.stop_requested() || !taken.has_value()) {
                return std::unexpected(Error{.code = Errc::interrupted, .message = "interrupted"});
            }
            Result<FetchedPage> result = std::move(*taken);
            if (!result) {
                return std::unexpected(std::move(result).error());
            }
            lock.unlock();

            Result<void> consumed_ok = invoke_consume(consume, index, std::move(*result));

            lock.lock();
            if (!consumed_ok) {
                return consumed_ok;
            }
            // Only now is the slot free for index + window: the page being consumed counts
            // towards the window.
            ++consumed;
            work_ready.notify_all();
        }
        return {};
    }

    /// Ends the current run on every path: start nothing new, cancel the in-flight fetches
    /// through `cancel`, wait until none is running, and forget the borrowed fetcher.
    void drain(std::stop_source& cancel) noexcept {
        {
            const std::scoped_lock lock{mutex};
            stop_handing_out = true;
        }
        // Outside the lock: stop_callbacks registered by the fetchers run synchronously here.
        static_cast<void>(cancel.request_stop());
        std::unique_lock lock{mutex};
        progress.wait(lock, [this] { return in_flight == 0; });
        fetch = nullptr;
        run_token = std::stop_token{};
        count = 0;
        for (std::optional<Result<FetchedPage>>& slot : ring) {
            slot.reset();
        }
    }

    /// Calls drain() when the run scope ends, on every path (including an exception). Borrows
    /// both arguments, which are locals of run() declared before it.
    class DrainOnExit {
    public:
        DrainOnExit(State& state, std::stop_source& cancel) noexcept
            : state_(&state), cancel_(&cancel) {}
        DrainOnExit(const DrainOnExit&) = delete;
        DrainOnExit& operator=(const DrainOnExit&) = delete;
        DrainOnExit(DrainOnExit&&) = delete;
        DrainOnExit& operator=(DrainOnExit&&) = delete;
        ~DrainOnExit() { state_->drain(*cancel_); }

    private:
        State* state_ = nullptr;
        std::stop_source* cancel_ = nullptr;
    };
};

FetchPool::FetchPool(unsigned workers)
    : state_(std::make_unique<State>(std::clamp(workers, min_workers, max_workers))) {
    State* const state = state_.get();
    state->workers.reserve(state->window());
    const detail::ScopedAsyncSignalBlock blocked{}; // the workers inherit the blocked mask
    for (std::size_t i = 0; i < state->window(); ++i) {
        // The worker borrows the State: `workers` is its last member, so the thread is stopped
        // and joined before anything else in the State is destroyed.
        state->workers.emplace_back(
            [state](const std::stop_token& token) { state->worker_loop(token); });
    }
}

FetchPool::~FetchPool() = default;

Result<void> FetchPool::run(std::size_t count, const PageFetcher& fetch,
                            const PageConsumer& consume, std::stop_token stop) {
    if (count == 0) {
        return {};
    }
    if (!fetch || !consume) {
        return std::unexpected(
            Error{.code = Errc::failure, .message = "fetch pool: empty fetch or consume function"});
    }
    State& state = *state_;

    // The run's own stop state: requested by the caller's stop (forwarded below) and, for the
    // fetchers only, by the end of the run (drain). The consumer waits on it too, so until the
    // drain it is stopped exactly when the caller's token is.
    std::stop_source cancel{};
    // `cancel` outlives the callback (declared first; ~stop_callback waits for a running one).
    const std::stop_callback forward{std::move(stop), [source = &cancel] noexcept {
                                         static_cast<void>(source->request_stop());
                                     }};
    {
        const std::scoped_lock lock{state.mutex};
        if (state.fetch != nullptr) {
            return std::unexpected(
                Error{.code = Errc::failure, .message = "fetch pool: run() is not reentrant"});
        }
        state.fetch = &fetch;
        state.run_token = cancel.get_token();
        state.count = count;
        state.next_index = 0;
        state.consumed = 0;
        state.stop_handing_out = false;
    }
    const State::DrainOnExit drain{state, cancel};
    state.work_ready.notify_all();
    return state.consume_in_order(consume, cancel.get_token());
}

} // namespace mangadex_dl
