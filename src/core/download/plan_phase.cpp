// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plan phase of run_download (download plan T14): title, feed and covers from the API, then
// build_plan, then the observer hears about the plan and its skips. Runs on the calling thread.
#include "download/internal.hpp"
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/planner.hpp"

#include <expected>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace mangadex_dl::detail {
namespace {

/// "Vol. 2 Ch. 3", "Ch. 10", "Vol. 2 (unnumbered chapter)" or "Oneshot".
[[nodiscard]] std::string chapter_label(const SkippedChapter& skipped) {
    if (skipped.volume && skipped.chapter) {
        return std::format("Vol. {} Ch. {}", *skipped.volume, *skipped.chapter);
    }
    if (skipped.chapter) {
        return std::format("Ch. {}", *skipped.chapter);
    }
    if (skipped.volume) {
        return std::format("Vol. {} (unnumbered chapter)", *skipped.volume);
    }
    return "Oneshot";
}

/// The warning for a skipped chapter, or nullopt for a duplicate (those are -v dedupe notes).
[[nodiscard]] std::optional<std::string> skip_warning(const SkippedChapter& skipped) {
    switch (skipped.reason) {
    case SkipReason::external:
        return std::format("skipped {}: hosted outside MangaDex", chapter_label(skipped));
    case SkipReason::no_pages:
        return std::format("skipped {}: it has no pages", chapter_label(skipped));
    case SkipReason::unavailable:
        return std::format("skipped {}: unavailable on MangaDex", chapter_label(skipped));
    case SkipReason::duplicate:
        return std::nullopt;
    }
    return std::nullopt; // only reachable through an out-of-range enum value
}

} // namespace

Result<PlanResult> plan_phase(RunContext& ctx) {
    const DownloadOptions& options = *ctx.options;
    ApiClient& client = *ctx.client;
    DownloadObserver& observer = *ctx.observer;

    if (stop_now(ctx.stop)) {
        return std::unexpected(interrupted_error());
    }
    Result<MangaInfo> manga = client.fetch_manga(options.manga, ctx.stop.get_token());
    ctx.lines->drain(observer);
    if (!manga) {
        return std::unexpected(std::move(manga.error()));
    }

    if (stop_now(ctx.stop)) {
        return std::unexpected(interrupted_error());
    }
    Result<std::vector<Chapter>> feed =
        client.fetch_feed(options.manga, options.lang, ctx.stop.get_token());
    ctx.lines->drain(observer);
    if (!feed) {
        return std::unexpected(std::move(feed.error()));
    }

    if (stop_now(ctx.stop)) {
        return std::unexpected(interrupted_error());
    }
    Result<std::vector<Cover>> covers = client.fetch_covers(options.manga, ctx.stop.get_token());
    ctx.lines->drain(observer);
    std::vector<Cover> cover_list{};
    if (covers) {
        cover_list = std::move(*covers);
    } else if (covers.error().code == Errc::interrupted) {
        return std::unexpected(std::move(covers.error()));
    } else {
        observer.on_warning(std::format(
            "could not list the cover images ({}); each file uses its first page as the cover",
            covers.error().message));
    }

    PlanInput input{
        .manga = std::move(*manga),
        .chapters = std::move(*feed),
        .covers = std::move(cover_list),
    };
    const PlanOptions plan_options{
        .volumes = options.volumes,
        .loose_chapters = options.loose_chapters,
        .layout = options.layout,
        .lang = options.lang,
        .preferred_groups = options.groups,
        .direction = options.direction,
        .title_override = options.title,
    };
    Result<Plan> plan = build_plan(input, plan_options);
    if (!plan) {
        return std::unexpected(std::move(plan.error()));
    }

    observer.on_plan(*plan);
    for (const SkippedChapter& skipped : plan->skipped) {
        if (const std::optional<std::string> warning = skip_warning(skipped)) {
            observer.on_warning(*warning);
        }
    }
    return PlanResult{.manga = std::move(input.manga), .plan = std::move(*plan)};
}

} // namespace mangadex_dl::detail
