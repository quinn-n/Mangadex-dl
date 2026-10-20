// SPDX-License-Identifier: GPL-3.0-or-later
//
// The options `download` shares with other subcommands (batch plan T4): one binder per group, so
// every subcommand that offers them gets the same names, type names, validators, defaults and help
// strings. Help lists options in add order, so callers keep the order of their bind_* calls.
#pragma once

#include "mangadex_dl/options.hpp"

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// Adds --volumes, --loose-chapters, --layout, -l/--lang, --group, --data-saver and --direction to
/// `sub`, in that order. Binds into `args`, which must be a member of the Command that outlives
/// parse() and dispatch (never a local or a copy).
void bind_selection_options(CLI::App& sub, RawDownloadArgs& args);

/// Adds --overwrite, -n/--dry-run, -j/--jobs, --no-report and the hidden --api-base to `sub`, in
/// that order. Binds into `args`, which must be a member of the Command that outlives parse() and
/// dispatch (never a local or a copy).
void bind_run_options(CLI::App& sub, RawDownloadArgs& args);

} // namespace mangadex_dl::app
