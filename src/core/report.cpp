// SPDX-License-Identifier: GPL-3.0-or-later
//
// The run report: status counts, the exit-code rule and the --json document (download plan T13;
// `existing`, `new_chapters` and `reused_chapters` from skip-existing plan T11).
// The document's keys and enum spellings are API (download-spec.md "Input & output"); renaming or
// removing one is a breaking change that needs a format_version bump.
#include "mangadex_dl/report.hpp"

#include "json_output.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/model.hpp"
#include "report_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// insertion-ordered, so the document lists its keys in the documented order. Every object here is
/// built by this file from a handful of fixed keys (never parsed from untrusted input), so the
/// linear key lookup of ordered_json costs nothing.
using Json = nlohmann::ordered_json;

/// Bump only for a breaking change to the document (a renamed or removed key, a changed meaning).
constexpr int format_version = 1;

[[nodiscard]] constexpr std::string_view layout_name(Layout layout) noexcept {
    switch (layout) {
    case Layout::volumes:
        return "volumes";
    case Layout::chapters:
        return "chapters";
    case Layout::single:
        return "single";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

[[nodiscard]] constexpr std::string_view direction_name(Direction direction) noexcept {
    switch (direction) {
    case Direction::ltr:
        return "ltr";
    case Direction::rtl:
        return "rtl";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

[[nodiscard]] constexpr std::string_view kind_name(EpubKind kind) noexcept {
    switch (kind) {
    case EpubKind::volume:
        return "volume";
    case EpubKind::chapter:
        return "chapter";
    case EpubKind::oneshot:
        return "oneshot";
    case EpubKind::single:
        return "single";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

[[nodiscard]] constexpr std::string_view status_name(EpubStatus status) noexcept {
    switch (status) {
    case EpubStatus::planned:
        return "planned";
    case EpubStatus::written:
        return "written";
    case EpubStatus::exists:
        return "exists";
    case EpubStatus::failed:
        return "failed";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

[[nodiscard]] constexpr std::string_view reason_name(SkipReason reason) noexcept {
    switch (reason) {
    case SkipReason::external:
        return "external";
    case SkipReason::no_pages:
        return "no_pages";
    case SkipReason::unavailable:
        return "unavailable";
    case SkipReason::duplicate:
        return "duplicate";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

/// The `existing` spelling; null for none (no file of that name).
[[nodiscard]] Json existing_json(ExistingKind existing) {
    switch (existing) {
    case ExistingKind::none:
        return nullptr;
    case ExistingKind::complete:
        return "complete";
    case ExistingKind::incomplete:
        return "incomplete";
    case ExistingKind::diverged:
        return "diverged";
    case ExistingKind::foreign:
        return "foreign";
    case ExistingKind::unchecked:
        return "unchecked";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

/// A JSON array of the strings in `values`, in order.
[[nodiscard]] Json string_array(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const std::string& value : values) {
        out.push_back(value);
    }
    return out;
}

/// A JSON string, or null for nullopt.
[[nodiscard]] Json nullable(const std::optional<std::string>& value) {
    if (!value) {
        return nullptr;
    }
    return *value;
}

[[nodiscard]] Json manga_json(const MangaSummary& manga) {
    Json out = Json::object();
    out["id"] = manga.id;
    out["title"] = manga.title;
    out["url"] = manga.url;
    out["language"] = manga.language;
    out["direction"] = direction_name(manga.direction);
    return out;
}

[[nodiscard]] Json chapter_json(const Chapter& chapter) {
    Json groups = Json::array();
    for (const ScanGroup& group : chapter.groups) {
        Json entry = Json::object();
        entry["id"] = group.id;
        entry["name"] = group.name;
        groups.push_back(std::move(entry));
    }
    Json out = Json::object();
    out["id"] = chapter.id;
    out["volume"] = nullable(chapter.volume);
    out["chapter"] = nullable(chapter.chapter);
    out["title"] = chapter.title;
    out["groups"] = std::move(groups);
    out["pages"] = chapter.pages;
    return out;
}

[[nodiscard]] Json epub_json(const EpubOutcome& outcome) {
    Json chapters = Json::array();
    for (const Chapter& chapter : outcome.epub.chapters) {
        chapters.push_back(chapter_json(chapter));
    }
    Json out = Json::object();
    out["path"] = outcome.path;
    out["kind"] = kind_name(outcome.epub.kind);
    out["volume"] = nullable(outcome.epub.volume);
    out["chapter"] = nullable(outcome.epub.chapter);
    out["status"] = status_name(outcome.status);
    out["replaced"] = outcome.replaced;
    out["existing"] = existing_json(outcome.existing);
    out["new_chapters"] = string_array(outcome.new_chapters);
    out["reused_chapters"] = string_array(outcome.reused_chapters);
    out["pages"] = outcome.pages;
    out["bytes"] = outcome.bytes;
    out["error"] = outcome.error.empty() ? Json(nullptr) : Json(outcome.error);
    out["chapters"] = std::move(chapters);
    return out;
}

[[nodiscard]] Json skipped_json(const SkippedChapter& skipped) {
    Json out = Json::object();
    out["id"] = skipped.id;
    out["volume"] = nullable(skipped.volume);
    out["chapter"] = nullable(skipped.chapter);
    out["reason"] = reason_name(skipped.reason);
    out["kept"] = nullable(skipped.kept);
    return out;
}

[[nodiscard]] Json summary_json(const RunSummary& summary) {
    Json out = Json::object();
    out["planned"] = summary.planned;
    out["written"] = summary.written;
    out["exists"] = summary.exists;
    out["failed"] = summary.failed;
    return out;
}

} // namespace

RunSummary summarize(const RunReport& report) noexcept {
    RunSummary summary{};
    for (const EpubOutcome& outcome : report.epubs) {
        switch (outcome.status) {
        case EpubStatus::planned:
            ++summary.planned;
            break;
        case EpubStatus::written:
            ++summary.written;
            break;
        case EpubStatus::exists:
            ++summary.exists;
            break;
        case EpubStatus::failed:
            ++summary.failed;
            break;
        }
    }
    return summary;
}

int exit_code_for(const RunReport& report) noexcept {
    const bool any_failed = std::ranges::any_of(report.epubs, [](const EpubOutcome& outcome) {
        return outcome.status == EpubStatus::failed;
    });
    if (any_failed) {
        return exit_code::incomplete;
    }
    if (report.epubs.empty()) {
        return exit_code::no_chapters;
    }
    return exit_code::success;
}

std::optional<std::string> up_to_date_notice(std::size_t up_to_date) {
    if (up_to_date == 0) {
        return std::nullopt;
    }
    if (up_to_date == 1) {
        return std::string{"1 file is already up to date; pass --overwrite to download it again"};
    }
    return std::format("{} files are already up to date; pass --overwrite to download them again",
                       up_to_date);
}

namespace detail {

Json report_json(const RunReport& report) {
    Json epubs = Json::array();
    for (const EpubOutcome& outcome : report.epubs) {
        epubs.push_back(epub_json(outcome));
    }
    Json skipped = Json::array();
    for (const SkippedChapter& chapter : report.skipped) {
        skipped.push_back(skipped_json(chapter));
    }

    Json doc = Json::object();
    doc["format_version"] = format_version;
    doc["dry_run"] = report.dry_run;
    doc["layout"] = layout_name(report.layout);
    doc["manga"] = manga_json(report.manga);
    doc["epubs"] = std::move(epubs);
    doc["skipped_chapters"] = std::move(skipped);
    doc["summary"] = summary_json(summarize(report));
    return doc;
}

} // namespace detail

std::string report_to_json(const RunReport& report) {
    // Titles, names and paths are untrusted bytes. dump_document dumps with the replace handler
    // (invalid UTF-8 becomes U+FFFD); nlohmann escapes the C0 controls (ESC included) and
    // dump_document escapes DEL and the C1 controls (U+009B, the 8-bit CSI, included), so the
    // document carries neither a 7-bit nor an 8-bit escape sequence.
    return json_output::dump_document(detail::report_json(report));
}

} // namespace mangadex_dl
