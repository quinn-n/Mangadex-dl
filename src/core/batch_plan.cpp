// SPDX-License-Identifier: GPL-3.0-or-later
//
// `batch` phase 1 after parsing (batch plan T3a): the CLI-level checks, the per-row merge with
// column-attributed diagnostics, the lexical containment of the `dir` column and the dedupe
// (detail::mark_duplicates, shared with plan_list: src/core/batch_internal.hpp). Pure: no I/O,
// no locale, no printing. Every message is raw bytes; the app sanitizes on print.
#include "batch_internal.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/selection.hpp"
#include "mangadex_dl/text_safety.hpp"
#include "options_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

inline constexpr std::string_view stdin_label = "<stdin>";
inline constexpr std::string_view list_space = " \t"; ///< what the CSV lexer trims, too

/// Fixed text for a bad `manga` cell: the cell itself is never echoed.
inline constexpr std::string_view invalid_manga_cell =
    "must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL";

inline constexpr std::size_t max_dir_segments = 32;
inline constexpr std::size_t max_dir_segment_bytes = 255;
inline constexpr std::size_t max_dir_bytes = 1024;

[[nodiscard]] Error usage_error(std::string message) {
    return Error{.code = Errc::usage, .message = std::move(message)};
}

[[nodiscard]] constexpr bool is_control_byte(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7F;
}

/// `text` without leading and trailing ASCII spaces and tabs (a sub-view of `text`).
[[nodiscard]] constexpr std::string_view
trim_list_space(std::string_view text MANGADEX_DL_LIFETIMEBOUND) noexcept {
    const std::size_t first = text.find_first_not_of(list_space);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(list_space);
    return text.substr(first, last - first + 1);
}

/// Splits `text` on `separator`, keeping empty pieces. The pieces are owned copies.
[[nodiscard]] std::vector<std::string> split_owned(std::string_view text, char separator) {
    std::vector<std::string> pieces{};
    while (true) {
        const std::size_t at = text.find(separator);
        pieces.emplace_back(text.substr(0, at));
        if (at == std::string_view::npos) {
            return pieces;
        }
        text.remove_prefix(at + 1);
    }
}

/// A validator's message without its leading "--<column>: " or "--<column> " (the column names
/// equal the long option names, and the column is printed in front of the message instead).
[[nodiscard]] std::string strip_option_prefix(std::string message, std::string_view column) {
    const std::string option = std::format("--{}", column);
    if (message.starts_with(option)) {
        const std::string_view rest = std::string_view{message}.substr(option.size());
        if (rest.starts_with(": ")) {
            return message.substr(option.size() + 2);
        }
        if (rest.starts_with(' ')) {
            return message.substr(option.size() + 1);
        }
    }
    return message;
}

/// plan_batch's diagnostics: "<label>:<line>: <column>: <message>" (or without a column), up to
/// max_list_errors messages, then only a count. Owns its copy of the label.
class PlanErrors {
public:
    explicit PlanErrors(std::string label) : label_(std::move(label)) {}

    /// Starts the diagnostics of the record at `line`.
    void begin_row(std::size_t line) noexcept {
        line_ = line;
        row_failed_ = false;
    }

    void cell(std::string_view column, std::string_view message) {
        add(std::format("{}:{}: {}: {}", label_, line_, column, message));
    }

    void row(std::string_view message) { add(std::format("{}:{}: {}", label_, line_, message)); }

    /// True once the current record has an error.
    [[nodiscard]] bool row_failed() const noexcept { return row_failed_; }

    [[nodiscard]] bool empty() const noexcept {
        return errors_.messages.empty() && errors_.more == 0;
    }

    [[nodiscard]] ListErrors take() && { return std::move(errors_); }

private:
    void add(std::string message) {
        row_failed_ = true;
        if (errors_.messages.size() < max_list_errors) {
            errors_.messages.push_back(std::move(message));
        } else {
            ++errors_.more;
        }
    }

    ListErrors errors_{};
    std::string label_{};
    std::size_t line_ = 0;
    bool row_failed_ = false;
};

/// The validated value of a non-empty cell, or nullopt: absent and empty cells mean "inherit",
/// and a rejected cell records "<column>: <message>" (the validator's "--<column>" prefix is
/// dropped). `validate` maps the cell text to a Result.
template <class Validate>
[[nodiscard]] auto checked_cell(PlanErrors& errors, std::string_view column,
                                const std::optional<std::string>& cell, Validate validate)
    -> std::optional<typename std::invoke_result_t<Validate&, std::string_view>::value_type> {
    if (!cell || cell->empty()) {
        return std::nullopt;
    }
    auto result = validate(std::string_view{*cell});
    if (!result) {
        errors.cell(column, strip_option_prefix(std::move(result).error().message, column));
        return std::nullopt;
    }
    return std::move(*result);
}

/// Adapts a validator to return the accepted cell text (the merged RawDownloadArgs keep strings).
template <class Validate> [[nodiscard]] auto keeping_text(Validate validate) {
    return [validate](std::string_view text) -> Result<std::string> {
        auto result = validate(text);
        if (!result) {
            return std::unexpected(std::move(result).error());
        }
        return std::string{text};
    };
}

[[nodiscard]] Result<Selection> parse_volumes(std::string_view spec) {
    return detail::parse_selection("--volumes", spec);
}

[[nodiscard]] Result<Selection> parse_loose_chapters(std::string_view spec) {
    return detail::parse_selection("--loose-chapters", spec);
}

/// The `group` cell: names separated by '|', each trimmed; never empty, never a control byte.
[[nodiscard]] Result<std::vector<std::string>> parse_group_cell(std::string_view cell) {
    std::vector<std::string> names = split_owned(cell, '|');
    for (std::string& name : names) {
        name = std::string{trim_list_space(name)};
        if (name.empty()) {
            return std::unexpected(usage_error("empty name in the list"));
        }
        if (std::ranges::any_of(name, is_control_byte)) {
            return std::unexpected(usage_error("must not contain control characters"));
        }
    }
    return names;
}

/// One record merged with the CLI-level strings, or nullopt after recording its errors.
[[nodiscard]] std::optional<BatchJob>
plan_record(const ListRecord& record, const BatchSettings& settings, PlanErrors& errors) {
    RawDownloadArgs merged = settings.defaults;
    std::string sub_dir{};

    Result<MangaId> manga = MangaId::parse(record.manga);
    if (manga) {
        merged.manga = std::string{manga->str()};
    } else {
        errors.cell("manga", invalid_manga_cell);
    }
    if (auto title = checked_cell(errors, "title", record.title, validate_title_override)) {
        merged.title = std::move(*title);
    }
    if (auto dir = checked_cell(errors, "dir", record.dir, validate_list_dir)) {
        sub_dir = std::move(*dir);
        merged.dir = join_list_dir(settings.defaults.dir, sub_dir);
    }
    if (auto volumes =
            checked_cell(errors, "volumes", record.volumes, keeping_text(parse_volumes))) {
        merged.volumes = std::move(*volumes);
    }
    if (auto loose = checked_cell(errors, "loose-chapters", record.loose_chapters,
                                  keeping_text(parse_loose_chapters))) {
        merged.loose_chapters = std::move(*loose);
    }
    if (auto layout =
            checked_cell(errors, "layout", record.layout, keeping_text(detail::parse_layout))) {
        merged.layout = std::move(*layout);
    }
    if (auto lang = checked_cell(errors, "lang", record.lang, validate_language)) {
        merged.lang = std::move(*lang);
    }
    if (auto groups = checked_cell(errors, "group", record.group, parse_group_cell)) {
        merged.groups = std::move(*groups); // replaces --group, never appends
    }
    if (auto direction = checked_cell(errors, "direction", record.direction,
                                      keeping_text(detail::parse_direction))) {
        merged.direction = std::move(*direction);
    }
    if (errors.row_failed() || !manga) {
        return std::nullopt;
    }

    // Cross-field rules (both selections `none`) on the merged values, worded as `download` does.
    Result<DownloadOptions> options = detail::validate_common(merged, std::move(*manga));
    if (!options) {
        errors.row(options.error().message);
        return std::nullopt;
    }
    std::string dir = merged.dir;
    return BatchJob{
        .line = record.line,
        .raw = std::move(merged),
        .options = std::move(*options),
        .dir = std::move(dir),
        .duplicate_of = std::nullopt,
        .sub_dir = std::move(sub_dir),
    };
}

} // namespace

namespace detail {

/// Jobs are grouped by title, and the groups hold indices (the vector is not resized here, but
/// indices stay valid regardless).
void mark_duplicates(std::vector<BatchJob>& jobs) {
    std::map<MangaId, std::vector<std::size_t>> firsts_by_manga{};
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        std::vector<std::size_t>& firsts = firsts_by_manga[jobs[i].options.manga];
        const auto same = std::ranges::find_if(
            firsts, [&](std::size_t j) { return jobs[j].options == jobs[i].options; });
        if (same == firsts.end()) {
            firsts.push_back(i); // only first occurrences: a duplicate equals one of them
        } else {
            jobs[i].duplicate_of = jobs[*same].line;
        }
    }
}

} // namespace detail

Result<BatchSettings> validate_batch_args(const RawBatchArgs& raw) {
    if (raw.file.empty()) {
        return std::unexpected(usage_error("FILE must not be empty"));
    }
    if (looks_like_manga_id(raw.file)) {
        return std::unexpected(usage_error("FILE looks like a MangaDex title; use 'mangadex-dl "
                                           "download <title>' for one title"));
    }
    // After the title check: a bare UUID is both, and keeps the title message.
    if (looks_like_list_id(raw.file)) {
        return std::unexpected(
            usage_error("FILE looks like a MangaDex list; use 'mangadex-dl mdlist <list>'"));
    }
    if (raw.defaults.dir.empty()) {
        return std::unexpected(usage_error("DIR must not be empty"));
    }
    if (looks_like_manga_id(raw.defaults.dir)) {
        return std::unexpected(
            usage_error("DIR looks like a MangaDex title; list the titles in FILE"));
    }
    // The CLI-level values are validated even when every row overrides them.
    Result<DownloadOptions> base = detail::validate_common(raw.defaults, MangaId{});
    if (!base) {
        return std::unexpected(std::move(base).error());
    }
    return BatchSettings{
        .file = raw.file,
        .label = raw.file == "-" ? std::string{stdin_label} : raw.file,
        .defaults = raw.defaults,
        .base = std::move(*base),
        .fail_fast = raw.fail_fast,
    };
}

Result<std::string> validate_list_dir(std::string_view cell) {
    const std::string_view dir = trim_list_space(cell);
    if (dir.empty()) {
        return std::unexpected(usage_error("must not be empty"));
    }
    if (std::ranges::any_of(dir, is_control_byte)) { // NUL, the other C0 controls and DEL
        return std::unexpected(usage_error("must not contain control characters"));
    }
    bool unsafe = false;
    for (std::size_t pos = 0; pos < dir.size();) {
        const Utf8Decoded decoded = decode_utf8_at(dir, pos);
        if (decoded.length == 0) {
            return std::unexpected(usage_error("must be valid UTF-8"));
        }
        unsafe = unsafe || is_unsafe_code_point(decoded.code_point);
        pos += decoded.length;
    }
    if (unsafe) {
        return std::unexpected(usage_error(
            "must not contain invisible, bidirectional or terminal control characters"));
    }
    if (dir.contains('\\')) {
        return std::unexpected(usage_error("must not contain '\\'"));
    }
    if (dir.starts_with('/')) {
        return std::unexpected(usage_error("must be a relative path inside DIR"));
    }
    if (dir.contains("//") || dir.ends_with('/')) {
        return std::unexpected(
            usage_error("must not contain empty segments ('//' or a trailing '/')"));
    }
    const std::vector<std::string> segments = split_owned(dir, '/');
    if (std::ranges::any_of(segments, [](const std::string& segment) {
            return segment == "." || segment == "..";
        })) {
        return std::unexpected(usage_error("must not contain '.' or '..' segments"));
    }
    if (segments.size() > max_dir_segments) {
        return std::unexpected(
            usage_error(std::format("has more than {} levels", max_dir_segments)));
    }
    if (std::ranges::any_of(segments, [](const std::string& segment) {
            return segment.size() > max_dir_segment_bytes;
        })) {
        return std::unexpected(
            usage_error(std::format("a segment is longer than {} bytes", max_dir_segment_bytes)));
    }
    if (dir.size() > max_dir_bytes) {
        return std::unexpected(usage_error(std::format("is longer than {} bytes", max_dir_bytes)));
    }
    return std::string{dir};
}

std::string join_list_dir(std::string_view base, std::string_view sub) {
    if (sub.empty()) {
        return std::string{base};
    }
    if (base.empty()) {
        return std::string{sub}; // never "/" + sub: that would turn a relative cell absolute
    }
    return base.ends_with('/') ? std::format("{}{}", base, sub) : std::format("{}/{}", base, sub);
}

ListResult<std::vector<BatchJob>> plan_batch(const TitleList& list, const BatchSettings& settings) {
    PlanErrors errors{settings.label};
    std::vector<BatchJob> jobs{};
    jobs.reserve(list.records.size());
    for (const ListRecord& record : list.records) {
        errors.begin_row(record.line);
        std::optional<BatchJob> job = plan_record(record, settings, errors);
        if (job) {
            jobs.push_back(std::move(*job));
        }
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors).take());
    }
    detail::mark_duplicates(jobs);
    return jobs;
}

} // namespace mangadex_dl
