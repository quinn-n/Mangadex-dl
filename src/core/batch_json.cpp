// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `batch --json` document (batch plan T3b; batch-spec.md "Input & output"). Its keys and the
// status spellings are API: renaming or removing one is a breaking change that needs a
// format_version bump. Each row's `report` is the row's `download --json` document, built by
// detail::report_json, so it is byte-identical to what `download --json` prints for that title.
// The list plan's T5 adds the additive `list` object (mdlist only) right after `source`; a batch
// document never carries it. The follows plan's T14 adds the additive `follows` object (follows
// only) in the same place; neither a batch nor an mdlist document ever carries it, and it never
// holds a token, secret, password or client id (FollowsSummary has no field for one).
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

/// The `follows` object: {username, titles, statuses (null without --status), selected}, in that
/// order; all four keys always present.
[[nodiscard]] Json follows_json(const FollowsSummary& follows) {
    Json out = Json::object();
    out["username"] = follows.username;
    out["titles"] = follows.titles;
    out["statuses"] = nullable(follows.statuses);
    out["selected"] = follows.selected;
    return out;
}

/// The one additive key a document may carry right after `source` (mdlist's `list`, follows'
/// `follows`). `name` points at a string literal.
struct ExtraKey {
    std::string_view name{};
    Json value{};
};

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

/// The document, with `extra` right after `source` when it has a value.
[[nodiscard]] std::string document(const BatchResult& result, std::string_view source, bool dry_run,
                                   std::optional<ExtraKey> extra) {
    Json titles = Json::array();
    for (const TitleResult& row : result.titles) {
        titles.push_back(title_json(row));
    }

    Json doc = Json::object();
    doc["format_version"] = format_version;
    doc["dry_run"] = dry_run;
    doc["source"] = source;
    if (extra) {
        doc[std::string{extra->name}] = std::move(extra->value);
    }
    doc["titles"] = std::move(titles);
    doc["summary"] = summary_json(result);

    // replace: FILE (or LIST), DIR, the dir cells, the list name, the follows username and every
    // report string are untrusted bytes; the strict handler would throw on invalid UTF-8, and
    // replace applies at every depth. ensure_ascii=false keeps valid UTF-8 readable; C0 control
    // characters (ESC included) are always escaped as \u00XX, so the document never carries a 7-bit
    // escape sequence. DEL and the C1 controls pass through unescaped (list plan gate decision: a
    // separate change escapes them in every --json document).
    std::string text = doc.dump(2, ' ', false, Json::error_handler_t::replace);
    text += '\n';
    return text;
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
    if (!list) {
        return document(result, source, dry_run, std::nullopt);
    }
    return document(result, source, dry_run, ExtraKey{.name = "list", .value = list_json(*list)});
}

std::string batch_to_json(const BatchResult& result, std::string_view source, bool dry_run,
                          const FollowsSummary& follows) {
    return document(result, source, dry_run,
                    ExtraKey{.name = "follows", .value = follows_json(follows)});
}

} // namespace mangadex_dl
