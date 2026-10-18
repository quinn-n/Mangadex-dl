// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "Exit status:" help footer, shared by the root `--help` and `download --help` (and so by the
// help2man page). Built from exit_codes.hpp so the documented numbers can't drift from the code.
#pragma once

#include "mangadex_dl/exit_codes.hpp"

#include <format>
#include <string>

namespace mangadex_dl::app {

/// The exit-status table, aligned for a formatter with footer formatting off. No trailing newline.
[[nodiscard]] inline std::string exit_status_footer() {
    return std::format("Exit status:\n"
                       "  {:<3} success\n"
                       "  {:<3} runtime failure (including a failed write to stdout)\n"
                       "  {:<3} usage error\n"
                       "  {:<3} network or API failure (connection, TLS, timeout, server error)\n"
                       "  {:<3} title not found on MangaDex\n"
                       "  {:<3} no downloadable chapters matched the language and selection\n"
                       "  {:<3} incomplete: at least one output file failed\n"
                       "  {:<3} internal error (a bug: please report it)",
                       exit_code::success, exit_code::failure, exit_code::usage, exit_code::network,
                       exit_code::not_found, exit_code::no_chapters, exit_code::incomplete,
                       exit_code::internal);
}

} // namespace mangadex_dl::app
