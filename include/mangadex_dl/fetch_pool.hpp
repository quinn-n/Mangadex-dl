// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parallel page downloads with in-order delivery. Frozen interface (download plan T1);
// implemented in src/core/fetch_pool.cpp.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/image.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace mangadex_dl {

/// One downloaded and sniffed page. Owns its bytes; moves between threads only through the
/// pool's mutex-guarded window.
struct FetchedPage {
    std::string bytes{};
    ImageInfo info{};

    friend bool operator==(const FetchedPage&, const FetchedPage&) = default;
};

/// Called concurrently on worker threads, once per index. It must be thread-safe, capture
/// nothing by reference that could die before run() returns, and honour the stop token. An
/// exception thrown by it, or by anything it calls on the worker (Transport::fetch,
/// TraceSink::trace, ...), is caught on that worker and becomes the run's Errc::failure Error;
/// no exception ever escapes a worker thread.
using PageFetcher = std::function<Result<FetchedPage>(std::size_t index, std::stop_token stop)>;

/// Called on the thread that called run(), in index order 0, 1, 2, ... An exception thrown by
/// it is caught by run() and returned as Errc::failure, after the workers have drained.
using PageConsumer = std::function<Result<void>(std::size_t index, FetchedPage&& page)>;

/// A fixed set of worker threads (std::jthread). Workers block SIGPIPE, SIGINT and SIGTERM in
/// their own signal masks, so signals land on the main thread. Neither copyable nor movable;
/// the destructor requests stop and joins every worker. State lives in fetch_pool.cpp (pimpl).
class FetchPool {
public:
    /// `workers` should be in 1..8 (the --jobs range). Anything outside is a precondition
    /// violation that the constructor clamps into 1..8 (so 0 becomes 1); it never fails.
    explicit FetchPool(unsigned workers);
    FetchPool(const FetchPool&) = delete;
    FetchPool& operator=(const FetchPool&) = delete;
    FetchPool(FetchPool&&) = delete;
    FetchPool& operator=(FetchPool&&) = delete;
    ~FetchPool();

    /// Fetches indices 0..count-1 on the workers and hands each result to `consume` in order.
    /// At most `workers` results are buffered. The first error (from either function) stops new
    /// work, waits for every in-flight fetch to finish, and is returned. A stop request on
    /// `stop` does the same and returns Errc::interrupted. `fetch` and `consume` are borrowed
    /// only for the duration of the call: run() never returns while a worker still uses them.
    /// An empty `fetch` or `consume` (with count > 0) is Errc::failure before any work starts.
    /// Exceptions from either function are returned as Errc::failure (see PageFetcher and
    /// PageConsumer). Not reentrant: one run() at a time.
    [[nodiscard]] Result<void> run(std::size_t count, const PageFetcher& fetch,
                                   const PageConsumer& consume, std::stop_token stop);

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace mangadex_dl
