// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `follows` subcommand (follows plan T18): `mangadex-dl follows [DIR]` logs in to MangaDex,
// reads the titles the user follows (and, with --status, their reading statuses), and runs
// `download`'s per-title path once for each title kept, in follows order, over one shared
// Backend, through the runner `batch` and `mdlist` use (cmd_batch_jobs.hpp). It ends with one
// aggregate exit code and, with --json, one document.
//
// Phases (follows-spec.md "Behaviour"): validate the CLI options and the API base (pure, exit 2)
// -> the interrupt handlers -> the agent, the backend and the trace -> the login phase
// (cmd_auth.hpp, LoginMode::reuse) -> the collection fetch (the only authorized requests) ->
// close_login_phase (the late warnings, then the secrets wiped and the lock released) before any
// download -> the --status filter, the -v lines and the empty warnings -> run every item -> the
// note, the closing line and the document.
#pragma once

#include "cmd_download_run.hpp"
#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"

#include <memory>
#include <span>
#include <string>

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// The whole `follows` command after parsing, testable without CLI11: validates `args`, logs in,
/// fetches the collection, plans every item and runs them over a fresh Backend for the API base.
/// Returns the exit code.
///
/// Nothing is opened and no request is sent before the arguments (the API base included) are
/// valid: a usage error is exit 2 with an empty stdout. A failure of the login or of the
/// collection (8, 9, 3, 1, an interrupt) prints no JSON and touches no directory. DIR is never
/// echoed by any of these messages. `args`, `globals` and `io` are only borrowed for the call.
[[nodiscard]] int run_follows_command(const RawFollowsArgs& args, const GlobalOptions& globals,
                                      Io& io);

/// The same over `backend` instead of a Backend built from the API base: the seam the in-process
/// tests use to record (and interrupt) the requests. `backend` must be the one make_backend
/// would build for `args.defaults.api_base` (a fixture backend for a file:// base); it is
/// borrowed for the call and shared by the login, the collection fetch and every item.
[[nodiscard]] int run_follows_command(const RawFollowsArgs& args, const GlobalOptions& globals,
                                      Io& io, const Backend& backend);

/// The `follows --help` footer: the login, the reading-status filter, examples, the re-run note,
/// the symlink and API-load note, then the shared exit-status table. Pre-aligned, at most 80
/// columns, no trailing newline.
[[nodiscard]] std::string follows_help_footer();

/// Tests only: a throwaway CLI::App with only `follows`'s options, bound as register_follows
/// binds them, parsing `args` (no argv[0]) and recording credentials_given as the Command does.
/// A CLI11 parse error -> Errc::usage with CLI11's what() verbatim, which may echo an argument's
/// value: never show that message to a user (cli.cpp maps parse errors without values, T19).
[[nodiscard]] Result<RawFollowsArgs>
parse_follows_args_for_testing(std::span<const std::string> args);

/// Adds `follows` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `follows` is chosen.
/// Not registered in cli.cpp yet (follows plan T19).
[[nodiscard]] std::unique_ptr<Command> register_follows(CLI::App& parent, Command*& selected);

} // namespace mangadex_dl::app
