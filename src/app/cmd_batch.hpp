// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `batch` subcommand (batch plan T5): `mangadex-dl batch FILE [DIR]` runs `download`'s
// per-title path once for each title listed in a CSV file (or on stdin), in file order, over one
// shared Backend, and ends with one aggregate exit code and, with --json, one document.
//
// Phases (batch-spec.md "Behaviour"): validate the CLI options -> read FILE (bounded) -> parse,
// merge, validate and dedupe every row (pure core) -> only then the interrupt handlers, the
// backend, the network and the filesystem.
#pragma once

#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/batch.hpp"

#include <memory>
#include <string>

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// The whole `batch` command after parsing, testable without CLI11: validates `args`, reads the
/// list (FILE, or io.stdin_fd for "-"), plans every row and runs them. Returns the exit code.
///
/// File descriptors: nothing is opened before the stdin read (no handlers, backend, output
/// directory or row-dir check), so a closed fd 0 can never be taken over by one of ours.
/// `args`, `globals` and `io` are only borrowed for the call.
[[nodiscard]] int run_batch_command(const RawBatchArgs& args, const GlobalOptions& globals, Io& io);

/// The `batch --help` footer: the CSV format, examples, the restart note, the symlink and API-load
/// note, then the shared exit-status table. Pre-aligned, at most 80 columns, no trailing newline.
[[nodiscard]] std::string batch_help_footer();

/// Adds `batch` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `batch` is chosen.
[[nodiscard]] std::unique_ptr<Command> register_batch(CLI::App& parent, Command*& selected);

} // namespace mangadex_dl::app
