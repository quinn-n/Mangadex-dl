// SPDX-License-Identifier: GPL-3.0-or-later
//
// --dir-per-title inside run_download (list plan T6): after a non-empty plan, pick the title's
// directory below DIR, say where it is (-v), and run the read-only pre-flight on it. The execute
// phase then runs unchanged with DIR/<name> as its options.dir (src/core/download.cpp), so every
// path it opens, writes or reports is below that directory.
//
// Runs on run_download's calling thread only; the resolver is never reached by a FetchPool worker.
// Nothing here creates, modifies or removes anything on disk.
#include "download/internal.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/row_dir.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/title_dir.hpp"

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl::detail {
namespace {

/// Appended to check_row_dir's message when the title's own directory is unusable.
constexpr std::string_view row_dir_remedy = "; rename or remove it, or run without --dir-per-title";

/// Defence in depth: the name joins onto DIR, so it must be exactly one path component that the
/// batch `dir` rules accept unchanged (no '/', no '.'/'..', no controls, no invisible or bidi
/// code points, valid UTF-8, at most one segment's worth of bytes, no surrounding list space).
[[nodiscard]] bool is_single_safe_component(const std::string& name) {
    if (name.contains('/')) {
        return false;
    }
    const Result<std::string> validated = validate_list_dir(name);
    return validated.has_value() && *validated == name;
}

} // namespace

Result<std::filesystem::path> resolve_title_dir(RunContext& ctx, const Plan& plan) {
    const DownloadOptions& options = *ctx.options;
    DownloadObserver& observer = *ctx.observer;

    if (stop_now(ctx.stop)) {
        return std::unexpected(interrupted_error());
    }
    // `plan.file_stem` is borrowed by resolve() for the call only; the choice owns its strings.
    const TitleDirChoice choice = ctx.title_dir != nullptr
                                      ? ctx.title_dir->resolve(plan.file_stem, options.manga)
                                      : TitleDirChoice{.name = plan.file_stem};
    if (!is_single_safe_component(choice.name)) {
        return std::unexpected(
            internal_error("the title directory name is not one safe path component"));
    }
    if (choice.warning) {
        observer.on_warning(*choice.warning);
    }

    std::filesystem::path dir = options.dir / choice.name;
    observer.on_verbose(1, std::format("output directory: {}", dir.string()));

    // Read-only: DIR missing is ok (the execute phase creates both levels; a dry run creates
    // nothing and finds no existing file).
    RowDirVerdict verdict = check_row_dir(options.dir, choice.name);
    switch (verdict.kind) {
    case RowDirKind::ok:
        return dir;
    case RowDirKind::row_error:
        // Only --dir-per-title makes this directory, so the way out names the flag.
        verdict.message += row_dir_remedy;
        return std::unexpected(Error{.code = Errc::row_dir, .message = std::move(verdict.message)});
    case RowDirKind::global_error:
        return std::unexpected(Error{.code = Errc::failure, .message = std::move(verdict.message)});
    }
    // Only reachable through an out-of-range enum value.
    return std::unexpected(internal_error("unknown title directory pre-flight verdict"));
}

} // namespace mangadex_dl::detail
