// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `batch --json` document (batch plan T3b; batch-spec.md "Input & output"). Its keys and the
// status spellings are API: renaming or removing one is a breaking change that needs a
// format_version bump. Each row's `report` is the row's `download --json` document, built by
// detail::report_json, so it is byte-identical to what `download --json` prints for that title.
// The list plan's T5 adds the additive `list` object (mdlist only) right after `source`; a batch
// document never carries it.
#include "json_output.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/report.hpp"
#include "report_json.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

/// insertion-ordered, so the document lists its keys in the documented order. Every object here is
/// built from fixed keys (never parsed from untrusted input).
using Json = nlohmann::ordered_json;

/// Bump only for a breaking change to the document (a renamed or removed key, a changed meaning).
constexpr int format_version = 1;

/// A JSON value, or null for nullopt.
template <class T> [[nodiscard]] Json nullable(const std::optional<T>& value) {
    if (!value) {
        return nullptr;
    }
    return Json(*value);
}

[[nodiscard]] Json title_json(const TitleResult& row) {
    Json out = Json::object();
    out["line"] = row.line;
    out["manga_id"] = row.manga_id;
    out["dir"] = row.dir;
    out["status"] = title_status_name(row.status);
    out["exit_code"] = nullable(row.exit_code);
    out["error"] = nullable(row.error);
    out["duplicate_of"] = nullable(row.duplicate_of);
    out["report"] = row.report ? detail::report_json(*row.report) : Json(nullptr);
    return out;
}

/// The per-status counts of summary.titles.
struct TitleCounts {
    std::size_t total = 0;
    std::size_t ok = 0;
    std::size_t not_found = 0;
    std::size_t no_chapters = 0;
    std::size_t incomplete = 0;
    std::size_t network = 0;
    std::size_t failed = 0;
    std::size_t duplicate = 0;
    std::size_t not_run = 0;

    void add(TitleStatus status) noexcept {
        ++total;
        switch (status) {
        case TitleStatus::ok:
            ++ok;
            return;
        case TitleStatus::not_found:
            ++not_found;
            return;
        case TitleStatus::no_chapters:
            ++no_chapters;
            return;
        case TitleStatus::incomplete:
            ++incomplete;
            return;
        case TitleStatus::network:
            ++network;
            return;
        case TitleStatus::failed:
            ++failed;
            return;
        case TitleStatus::duplicate:
            ++duplicate;
            return;
        case TitleStatus::not_run:
            ++not_run;
            return;
        }
        ++failed; // only reachable through an out-of-range enum value
    }
};

/// The `list` object: {id, name (null when absent), url}, in that order.
[[nodiscard]] Json list_json(const ListSummary& list) {
    Json out = Json::object();
    out["id"] = list.id;
    out["name"] = nullable(list.name);
    out["url"] = list.url;
    return out;
}

[[nodiscard]] Json summary_json(const BatchResult& result) {
    TitleCounts counts{};
    RunSummary files{};
    for (const TitleResult& row : result.titles) {
        counts.add(row.status);
        files.planned += row.files.planned;
        files.written += row.files.written;
        files.exists += row.files.exists;
        files.failed += row.files.failed;
    }

    Json titles = Json::object();
    titles["total"] = counts.total;
    titles["ok"] = counts.ok;
    titles["not_found"] = counts.not_found;
    titles["no_chapters"] = counts.no_chapters;
    titles["incomplete"] = counts.incomplete;
    titles["network"] = counts.network;
    titles["failed"] = counts.failed;
    titles["duplicate"] = counts.duplicate;
    titles["not_run"] = counts.not_run;

    Json file_counts = Json::object();
    file_counts["planned"] = files.planned;
    file_counts["written"] = files.written;
    file_counts["exists"] = files.exists;
    file_counts["failed"] = files.failed;

    Json out = Json::object();
    out["titles"] = std::move(titles);
    out["files"] = std::move(file_counts);
    return out;
}

} // namespace

std::string_view title_status_name(TitleStatus status) noexcept {
    switch (status) {
    case TitleStatus::ok:
        return "ok";
    case TitleStatus::not_found:
        return "not_found";
    case TitleStatus::no_chapters:
        return "no_chapters";
    case TitleStatus::incomplete:
        return "incomplete";
    case TitleStatus::network:
        return "network";
    case TitleStatus::failed:
        return "failed";
    case TitleStatus::duplicate:
        return "duplicate";
    case TitleStatus::not_run:
        return "not_run";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

std::string batch_to_json(const BatchResult& result, std::string_view source, bool dry_run) {
    return batch_to_json(result, source, dry_run, std::nullopt);
}

std::string batch_to_json(const BatchResult& result, std::string_view source, bool dry_run,
                          const std::optional<ListSummary>& list) {
    Json titles = Json::array();
    for (const TitleResult& row : result.titles) {
        titles.push_back(title_json(row));
    }

    Json doc = Json::object();
    doc["format_version"] = format_version;
    doc["dry_run"] = dry_run;
    doc["source"] = source;
    if (list) {
        doc["list"] = list_json(*list);
    }
    doc["titles"] = std::move(titles);
    doc["summary"] = summary_json(result);

    // FILE (or LIST), DIR, the dir cells, the list name and every report string are untrusted
    // bytes. dump_document dumps with the replace handler, which applies at every depth (invalid
    // UTF-8 becomes U+FFFD); nlohmann escapes the C0 controls (ESC included) and dump_document
    // escapes DEL and the C1 controls (U+009B, the 8-bit CSI, included), so the document carries
    // neither a 7-bit nor an 8-bit escape sequence.
    return json_output::dump_document(doc);
}

} // namespace mangadex_dl
