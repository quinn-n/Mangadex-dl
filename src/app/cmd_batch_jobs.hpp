// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 2 of a multi-title run (list plan T3), shared by `batch` and (later) `mdlist`: the
// ConsoleTitleRunner over one Backend, run_batch, then the up-to-date note, the closing line and
// the --json document. Also the stderr helpers every multi-title command prints with. Moved
// verbatim out of cmd_batch.cpp: `batch` prints the same bytes as before.
#pragma once

#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "io.hpp"
#include "mangadex_dl/batch.hpp"

#include <span>
#include <string>
#include <string_view>

namespace mangadex_dl::app {

/// `mangadex-dl: error: <message>` for a usage error: uncolored, like CLI11's parse errors and
/// `download`'s semantic usage errors, so every exit-2 diagnostic looks the same. The message
/// passes through terminal_safe.
void print_usage_line(Io& io, std::string_view message);

/// "Run 'mangadex-dl <command> --help' for more information.", the hint line every usage error
/// ends with; returns exit 2. `command` is a subcommand name from the code, never user input.
[[nodiscard]] int usage_hint(Io& io, std::string_view command);

/// `mangadex-dl: warning: <message>` (not with -q), the same line ConsoleObserver prints.
void print_warning(Io& io, const OutputPolicy& policy, std::string_view message);

/// `mangadex-dl: <message>` (not with -q, never colored), the same note line ConsoleObserver
/// prints.
void print_note(Io& io, const OutputPolicy& policy, std::string_view message);

/// Phase 2 of a multi-title run over non-empty `jobs`: the runner (the read-only row-dir
/// pre-flight, then run_title per row), run_batch, then (unless interrupted) the up-to-date note,
/// the closing line and, with --json, the document. Returns the aggregate exit code.
///
/// Lifetime: every argument is borrowed for the call and nothing is kept after it returns.
/// `backend` and `agent` are shared by every row; the caller keeps its interrupt handlers
/// installed around the call.
[[nodiscard]] int run_batch_jobs(std::span<const BatchJob> jobs, const BatchSettings& settings,
                                 const Backend& backend, const std::string& agent, Io& io,
                                 const OutputPolicy& policy);

} // namespace mangadex_dl::app
