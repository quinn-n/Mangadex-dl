// SPDX-License-Identifier: GPL-3.0-or-later
//
// `batch`: the CLI-level settings, the per-row plan (merge, validation, dedupe), the row loop and
// its aggregation, the closing message and the --json document. Frozen interface (batch plan T1);
// implemented in src/core/batch_plan.cpp (T3a) and src/core/batch_run.cpp, batch_json.cpp (T3b).
// The list plan's T1 adds BatchSource, ListSummary, RowOutcome::dir and the source/list
// overloads (defined in list plan T5).
#pragma once

#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// What CLI11 binds for `batch` (owned by the batch Command). `defaults.manga` and
/// `defaults.title` stay empty; `defaults.dir` is DIR.
struct RawBatchArgs {
    std::string file{}; ///< FILE as typed; "-" = stdin
    RawDownloadArgs defaults{};
    bool fail_fast = false;
};

/// Where a multi-title run's rows come from: the wording of row-scoped messages ("line"/"lines"
/// for csv, "item"/"items" for mdlist).
enum class BatchSource {
    csv,
    mdlist,
};

struct BatchSettings {
    std::string file{};         ///< as typed
    std::string label{};        ///< FILE as typed, or "<stdin>" for "-"
    RawDownloadArgs defaults{}; ///< validated CLI-level strings
    DownloadOptions base{};     ///< validated CLI-level options (manga = nil placeholder)
    bool fail_fast = false;
    BatchSource source = BatchSource::csv;
};

/// Errc::usage: FILE empty; FILE looks like a manga id ("FILE looks like a MangaDex title; use
/// 'mangadex-dl download <title>' for one title"); DIR empty; DIR looks like a manga id; then
/// every CLI-level option exactly as `download` validates it (manga excluded).
[[nodiscard]] Result<BatchSettings> validate_batch_args(const RawBatchArgs& raw);

struct BatchJob {
    std::size_t line = 0;
    RawDownloadArgs raw{};                     ///< the merged strings (for exit-5 messages)
    DownloadOptions options{};                 ///< validated
    std::string dir{};                         ///< effective DIR or DIR/<dir>, as typed
    std::optional<std::size_t> duplicate_of{}; ///< line of the first identical row
    std::string sub_dir{};                     ///< the validated `dir` cell, empty when none
};

/// Merge (non-empty cell > CLI value), per-cell validation, dedupe. Errors accumulate.
[[nodiscard]] ListResult<std::vector<BatchJob>> plan_batch(const TitleList& list,
                                                           const BatchSettings& settings);

/// The `dir` cell rules (lexical containment); returns the cell unchanged; Errc::usage with a
/// message that does not start with the column name.
[[nodiscard]] Result<std::string> validate_list_dir(std::string_view cell);

/// base + "/" + sub, without doubling a trailing '/' of base.
[[nodiscard]] std::string join_list_dir(std::string_view base, std::string_view sub);

enum class TitleStatus {
    ok,
    not_found,
    no_chapters,
    incomplete,
    network,
    failed,
    duplicate,
    not_run,
};

/// 0 ok, 3 network, 4 not_found, 5 no_chapters, 6 incomplete, anything else failed.
[[nodiscard]] TitleStatus title_status_for(int exit_code) noexcept;

/// The JSON spelling of `status`. Points at static storage.
[[nodiscard]] std::string_view title_status_name(TitleStatus status) noexcept;

/// What the runner reports for one row.
struct RowOutcome {
    int exit_code = 0;
    std::string error{}; ///< the row's closing message, empty when none
    RunSummary files{};
    std::optional<RunReport> report{}; ///< only when the caller keeps it (--json)
    bool interrupted = false;
    /// true = a global local condition (DIR itself unusable, ENOSPC, EIO, EROFS, anything
    /// unclassified): the batch stops. false with exit_code 1 = a row-scoped failure
    /// (check_row_dir row_error): the row is `failed` and the batch continues.
    bool fatal = false;
    std::string dir{}; ///< effective dir when it differs from job.dir (--dir-per-title:
                       ///< DIR/<name>); empty = job.dir

    friend bool operator==(const RowOutcome&, const RowOutcome&) = default;
};

struct TitleResult {
    std::size_t line = 0;
    std::string manga_id{};
    std::string dir{};
    TitleStatus status = TitleStatus::not_run;
    std::optional<int> exit_code{};
    std::optional<std::string> error{};
    std::optional<std::size_t> duplicate_of{};
    RunSummary files{};
    std::optional<RunReport> report{};

    friend bool operator==(const TitleResult&, const TitleResult&) = default;
};

enum class BatchStop {
    none,
    fail_fast,
    local_failure,
    network_streak,
    output_error,
    interrupted,
};

struct BatchResult {
    std::vector<TitleResult> titles{};
    BatchStop stop = BatchStop::none;
    std::size_t stopped_at_line = 0;

    friend bool operator==(const BatchResult&, const BatchResult&) = default;
};

/// Consecutive network-failed titles after which the batch stops.
inline constexpr std::size_t network_streak_limit = 3;

/// The seam between the loop (core) and the download (app). Polymorphic base per house rules:
/// neither copyable nor movable, so it can't be sliced.
class TitleRunner {
public:
    TitleRunner(const TitleRunner&) = delete;
    TitleRunner& operator=(const TitleRunner&) = delete;
    TitleRunner(TitleRunner&&) = delete;
    TitleRunner& operator=(TitleRunner&&) = delete;
    virtual ~TitleRunner() = default;

    /// `position` 1-based among non-duplicate rows; `total` = their count.
    [[nodiscard]] virtual RowOutcome run(const BatchJob& job, std::size_t position,
                                         std::size_t total) = 0;
    virtual void on_duplicate(const BatchJob& job) = 0;
    /// Checked after each row: true stops the batch (stdout is broken).
    [[nodiscard]] virtual bool output_failed() = 0;

protected:
    TitleRunner() = default;
};

/// Drives `runner` over `jobs` in order: stopping, the network streak and aggregation.
[[nodiscard]] BatchResult run_batch(std::span<const BatchJob> jobs, TitleRunner& runner,
                                    bool fail_fast);

/// Interrupted or stop == local_failure -> 1; otherwise the distinct codes of the non-ok rows
/// (`failed` counts as 1): none -> 0, one -> it, several -> 7.
[[nodiscard]] int batch_exit_code(const BatchResult& result) noexcept;

/// The single closing `error:` line, or nullopt when every row is ok/duplicate.
[[nodiscard]] std::optional<std::string> batch_closing_message(const BatchResult& result,
                                                               bool dry_run);

/// The same, worded for `source`: "line"/"lines" (csv, byte-identical to the two-argument
/// overload) or "item"/"items" (mdlist).
[[nodiscard]] std::optional<std::string> batch_closing_message(const BatchResult& result,
                                                               bool dry_run, BatchSource source);

/// The --json `list` object (mdlist only).
struct ListSummary {
    std::string id{}; ///< canonical list UUID
    std::optional<std::string> name{};
    std::string url{}; ///< https://mangadex.org/list/<id>

    friend bool operator==(const ListSummary&, const ListSummary&) = default;
};

/// The --json document, ordered_json, indent 2, replace handler, '\n'-terminated, never ANSI.
[[nodiscard]] std::string batch_to_json(const BatchResult& result, std::string_view source,
                                        bool dry_run);

/// The same, plus `"list": {"id", "name" (null when absent), "url"}` right after `"source"` when
/// `list` has a value. With nullopt, byte-identical to the three-argument overload.
[[nodiscard]] std::string batch_to_json(const BatchResult& result, std::string_view source,
                                        bool dry_run, const std::optional<ListSummary>& list);

/// Cross-row collision detection. Owns its keys.
class OutputRegistry {
public:
    /// Records `path` for `line`; returns the earlier line that already claimed it. Keyed on
    /// std::filesystem::path(path).lexically_normal().generic_string() of the joined path.
    [[nodiscard]] std::optional<std::size_t> claim(std::string path, std::size_t line);

    /// The line that claimed `path` so far (keyed as claim() keys it), or nullopt. Records
    /// nothing.
    [[nodiscard]] std::optional<std::size_t> claimed_by(std::string_view path) const;

private:
    std::map<std::string, std::size_t, std::less<>> by_path_{};
};

} // namespace mangadex_dl
