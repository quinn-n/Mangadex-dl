// SPDX-License-Identifier: GPL-3.0-or-later
//
// One Command subclass per subcommand (cmd_<sub>.cpp). The object owns the variables CLI11 binds
// to, so it must outlive parse() and dispatch: run() in cli.cpp owns every Command for the whole
// invocation. CLI11 callbacks only record which Command was selected; the work happens in run().
#pragma once

#include "io.hpp"

namespace mangadex_dl::app {

/// Global flags (bound on the root app; fallthrough() lets them appear after the subcommand).
struct GlobalOptions {
    bool json = false;  ///< --json: one JSON document on stdout, no ANSI, no progress
    bool quiet = false; ///< -q/--quiet: errors only on stderr
    int verbose = 0;    ///< -v/--verbose (repeatable): more detail on stderr
    ColorMode color = ColorMode::automatic; ///< --color=auto|always|never
};

class Command {
public:
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;
    virtual ~Command() = default;

    /// Runs after a successful parse. Returns an exit code from exit_codes.hpp.
    [[nodiscard]] virtual int run(Io& io, const GlobalOptions& globals) = 0;

protected:
    Command() = default;
};

} // namespace mangadex_dl::app
