// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `mdlist` subcommand (list plan T10): `mangadex-dl mdlist LIST [DIR]` fetches one public
// MangaDex custom list (MDList) and runs `download`'s per-title path once for each title in it,
// in the list's order, over one shared Backend, through the runner `batch` uses
// (cmd_batch_jobs.hpp). It ends with one aggregate exit code and, with --json, one document.
//
// Phases (list-spec.md "Behaviour"): validate the CLI options (pure, exit 2) -> the interrupt
// handlers and the backend -> fetch and parse the list (one request) -> plan the items (pure) ->
// an empty list ends here -> run every item -> the note, the closing line and the document.
#pragma once

#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/mdlist.hpp"

#include <memory>
#include <string>

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// The whole `mdlist` command after parsing, testable without CLI11: validates `args`, fetches the
/// list, plans every item and runs them. Returns the exit code.
///
/// Nothing is opened and no request is sent before the arguments are valid (a usage error is exit
/// 2 with an empty stdout). A failure of the list itself (not found, the network, a malformed
/// document, an interrupt) prints no JSON and touches no directory.
/// `args`, `globals` and `io` are only borrowed for the call.
[[nodiscard]] int run_mdlist_command(const RawMdlistArgs& args, const GlobalOptions& globals,
                                     Io& io);

/// The `mdlist --help` footer: what LIST is, examples, the re-run note, the symlink and API-load
/// note, then the shared exit-status table. Pre-aligned, at most 80 columns, no trailing newline.
[[nodiscard]] std::string mdlist_help_footer();

/// Adds `mdlist` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `mdlist` is chosen.
[[nodiscard]] std::unique_ptr<Command> register_mdlist(CLI::App& parent, Command*& selected);

} // namespace mangadex_dl::app
