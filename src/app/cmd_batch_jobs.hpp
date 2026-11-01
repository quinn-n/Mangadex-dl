// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 2 of a multi-title run (list plan T3), shared by `batch`, `mdlist` and `follows`: the
// ConsoleTitleRunner over one Backend, run_batch, then the up-to-date note, the closing line and
// the --json document. Also the stderr helpers every multi-title command prints with. Moved
// verbatim out of cmd_batch.cpp (T3), then made source-aware (T9): row wording follows
// BatchSettings::source, and --dir-per-title rows get a directory from one TitleDirRegistry per
// run. `batch` (source csv, no list) prints the same bytes as before. The follows plan's T14 adds
// the overload that carries the --json `follows` object.
#pragma once

#include "cmd_download_output.hpp"
#include "cmd_download_run.hpp"
#include "io.hpp"
#include "mangadex_dl/batch.hpp"

#include <optional>
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
/// Wording follows `settings.source`:
/// - csv: rows are "<label>:<line>: ", "same as line <k>", "was already written by line <k>; give
///   one of them a 'title' column", and the closing line says "line"/"lines";
/// - mdlist and follows: rows are "<label>:<line>: <uuid>: ", "same as item <k>", "was already
///   written by item <k> (another title with the same name); pass --dir-per-title to give each
///   title its own directory, or download one of them on its own with 'mangadex-dl download
///   <title> --title ...'" (the only line for that file: the engine's "holds a different book"
///   warning for it is dropped), and the closing line says "item"/"items". Each row also gets the
///   run's claimed paths, so with --overwrite a later row never replaces a file an earlier row of
///   the same run wrote.
/// Either way the -v start line is "<label>:<line>: <uuid>".
///
/// A job with options.dir_per_title gets its directory below DIR from the run's TitleDirRegistry
/// (first claim wins, in job order; a later title with the same name is moved aside with a
/// warning), and its JSON `dir` is DIR/<name> once a name was claimed.
///
/// `list` (mdlist only) adds the --json `list` object; nullopt keeps the batch document.
///
/// Lifetime: every argument is borrowed for the call and nothing is kept after it returns.
/// `backend` and `agent` are shared by every row; the caller keeps its interrupt handlers
/// installed around the call.
[[nodiscard]] int run_batch_jobs(std::span<const BatchJob> jobs, const BatchSettings& settings,
                                 const Backend& backend, const std::string& agent, Io& io,
                                 const OutputPolicy& policy,
                                 const std::optional<ListSummary>& list = std::nullopt);

/// The same for `follows` (settings.source == follows, settings.label "follows",
/// settings.file "follows"): `follows` is the --json `follows` object, right after `source`.
/// Borrowed for the call, like every other argument.
[[nodiscard]] int run_batch_jobs(std::span<const BatchJob> jobs, const BatchSettings& settings,
                                 const Backend& backend, const std::string& agent, Io& io,
                                 const OutputPolicy& policy, const FollowsSummary& follows);

} // namespace mangadex_dl::app
