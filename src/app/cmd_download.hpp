// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `download` subcommand (download plan T15): binds RawDownloadArgs, validates them, wires the
// transport, clock, jitter and interrupt handling, and runs the download.
#pragma once

#include "command.hpp"

#include <memory>

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// Adds `download` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `download` is chosen.
[[nodiscard]] std::unique_ptr<Command> register_download(CLI::App& parent, Command*& selected);

} // namespace mangadex_dl::app
