// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-process seam: tests call run() with ostringstream-backed Io and assert exit code, stdout
// and stderr separately. No CLI11 types cross this header.
#pragma once

#include "io.hpp"

#include <span>

namespace mangadex_dl::app {

/// Builds the CLI11 app, parses `args` (argv-style: args[0] is the program name; may be empty),
/// dispatches the selected command and returns its exit code. Parse errors return 2; --help and
/// --version return 0 with their text on io.out. Throws only for bugs (main maps those to 70).
[[nodiscard]] int run(std::span<const char* const> args, Io& io);

} // namespace mangadex_dl::app
