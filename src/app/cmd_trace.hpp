// SPDX-License-Identifier: GPL-3.0-or-later
//
// The -vv request trace of the requests a command sends itself, outside run_download (the list
// fetch of `mdlist`; the login and the collection fetch of `follows` and `login`). Moved verbatim
// out of cmd_mdlist.cpp (follows plan T16) so every command prints the same trace lines.
#pragma once

#include "cmd_download_output.hpp"
#include "io.hpp"
#include "mangadex_dl/api_client.hpp"

#include <mutex>
#include <string>

namespace mangadex_dl::app {

/// The -vv request trace: `mangadex-dl: <line>` on stderr, terminal_safe and uncolored, exactly
/// like the engine's own trace lines. Silent unless -vv without -q.
///
/// Threads: TraceSink requires trace() to be thread-safe, so every line is written under the
/// mutex (the list fetch itself calls it on the calling thread only: its client has no reporter).
/// Lifetime: `io` is borrowed (the caller's, it outlives the sink). Whoever is handed the sink
/// borrows it: fetch_custom_list keeps no pointer to it past its return, and a LoginSession keeps
/// one until it is destroyed, so the sink is declared before the session.
class StderrTrace final : public TraceSink {
public:
    StderrTrace(Io& io, const OutputPolicy& policy) noexcept
        : io_(&io), enabled_(policy.verbose >= 2 && !policy.quiet) {}

    void trace(std::string line) override;

private:
    Io* io_ = nullptr; ///< borrowed, never null
    bool enabled_ = false;
    std::mutex mutex_{};
};

} // namespace mangadex_dl::app
