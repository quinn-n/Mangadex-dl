// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_trace.hpp"

#include "cmd_download_output.hpp"
#include "mangadex_dl/version.hpp"

#include <mutex>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>

namespace mangadex_dl::app {

void StderrTrace::trace(std::string line) {
    if (!enabled_) {
        return;
    }
    const std::scoped_lock lock{mutex_};
    std::print(io_->err, "{}: {}\n", tool_name, terminal_safe(line));
}

} // namespace mangadex_dl::app
