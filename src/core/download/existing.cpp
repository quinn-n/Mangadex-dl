// SPDX-License-Identifier: GPL-3.0-or-later
//
// Assessment of an existing file and the update pre-pass (skip-existing plan T10). See
// download/existing.hpp for the descriptor invariant (V5) and the order of the steps.
//
// Memory-safety notes:
// - The FdReadSource in inspect_descriptor borrows the named `handle.fd` and dies before the
//   handle is moved into the result (the descriptor itself is moved, never closed, on the way).
// - prepass works on indices into the decision and re-reads `assessment.decision` after every
//   fall_back_to_download (which replaces the decision), so no reference into the old final list
//   is used after it changes. FinalChapter::held indexes HeldBook::pages, which is parallel to the
//   manifest the decision was made from; it is range-checked before use.
// - A held page's bytes live for one loop iteration; only its ImageInfo is kept.
#include "download/existing.hpp"

#include "download/internal.hpp"
#include "fs/errno_text.hpp"
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/update_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::detail {
namespace {

/// "1 chapter" / "N chapters".
[[nodiscard]] std::string chapters_count(std::size_t count) {
    return count == 1 ? std::string{"1 chapter"} : std::format("{} chapters", count);
}

/// " (Ch. 3, Ch. 4)": the labels of `chapters` (at most 5, then "and K more"), or "" unless
/// every one of the `count` chapters a message counts was resolved to a label (a counts-only
/// message beats a list that silently names fewer chapters than it counts).
[[nodiscard]] std::string labels_suffix(std::span<const Chapter> chapters, std::size_t count) {
    if (chapters.empty() || chapters.size() != count) {
        return {};
    }
    return std::format(" ({})", chapter_labels(chapters));
}

/// "<path>: <key>: " or "<path>: " when the key label is empty.
[[nodiscard]] std::string key_prefix(std::string_view path, std::string_view key_label) {
    return key_label.empty() ? std::format("{}: ", path) : std::format("{}: {}: ", path, key_label);
}

[[nodiscard]] Assessment foreign_assessment(ForeignReason reason, int error) {
    Assessment out{};
    out.kind = ExistingKind::foreign;
    out.foreign = InspectFailure{.reason = reason, .error = error};
    return out;
}

[[nodiscard]] ExistingKind kind_of(UpdateState state) noexcept {
    switch (state) {
    case UpdateState::complete:
        return ExistingKind::complete;
    case UpdateState::incomplete:
        return ExistingKind::incomplete;
    case UpdateState::diverged:
        return ExistingKind::diverged;
    }
    // Only reachable through an out-of-range enum value: leave such a file alone.
    return ExistingKind::diverged;
}

/// Reads every page of `entries`; their ImageInfos in order, or nullopt at the first fault.
/// Errc::interrupted when `stop` is requested before a page.
[[nodiscard]] Result<std::optional<std::vector<ImageInfo>>>
validate_pages(ReadSource& source, std::span<const HeldEntry> entries, std::stop_source& stop) {
    std::vector<ImageInfo> infos{};
    infos.reserve(entries.size());
    for (const HeldEntry& entry : entries) {
        if (stop_now(stop)) {
            return std::unexpected(interrupted_error());
        }
        // The bytes are freed at the end of this iteration: one image in memory at a time.
        const std::expected<HeldPage, PageFault> page = read_held_page(source, entry);
        if (!page) {
            return std::optional<std::vector<ImageInfo>>{}; // D9: either fault falls back
        }
        infos.push_back(page->info);
    }
    return std::optional<std::vector<ImageInfo>>{std::move(infos)};
}

/// inspect_existing through an FdReadSource that borrows `fd` (a named UniqueFd of the caller)
/// for this call only: the source is gone before the caller may move the descriptor.
[[nodiscard]] std::expected<HeldBook, InspectFailure>
inspect_descriptor(const UniqueFd& fd, std::uint64_t size, std::string_view identifier) {
    FdReadSource source{fd, size};
    return inspect_existing(source, identifier);
}

} // namespace

std::optional<std::uint64_t> readable_size(const FileIdentity& identity) noexcept {
    if (!std::in_range<std::uint64_t>(identity.size)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(identity.size);
}

Assessment classify_held(HeldBook held, const PlannedEpub& planned,
                         std::span<const std::string> preferred_groups, ImageQuality quality) {
    Assessment out{};
    UpdateDecision decision = decide_update(planned, held.manifest, preferred_groups, quality);
    out.kind = kind_of(decision.state);
    out.final_plan = final_epub(planned, decision);
    out.decision = std::move(decision);
    out.held = std::move(held);
    return out;
}

Result<Assessment> assess_handle(ExistingHandle handle, const PlannedEpub& planned,
                                 std::span<const std::string> preferred_groups,
                                 ImageQuality quality, bool real_run) {
    switch (handle.status) {
    case ExistingOpen::absent:
        return Assessment{};
    case ExistingOpen::not_regular:
        return foreign_assessment(ForeignReason::not_regular, handle.error);
    case ExistingOpen::unreadable:
        return foreign_assessment(ForeignReason::unreadable, handle.error);
    case ExistingOpen::opened:
        break;
    }
    if (handle.status != ExistingOpen::opened) {
        // Only reachable through an out-of-range enum value.
        return std::unexpected(internal_error("unknown result of opening an existing file"));
    }
    // A moved-from handle keeps `opened` with descriptor -1: never read through it.
    if (!handle.fd.valid()) {
        return std::unexpected(internal_error(
            std::format("{}: the existing file's descriptor is not open", planned.file_name)));
    }
    const std::optional<std::uint64_t> size = readable_size(handle.identity);
    if (!size) {
        return foreign_assessment(ForeignReason::not_ours, 0); // closes the descriptor
    }

    std::expected<HeldBook, InspectFailure> inspected =
        inspect_descriptor(handle.fd, *size, planned.identifier);
    if (!inspected) {
        // `handle` (and its descriptor) is closed on return.
        return foreign_assessment(inspected.error().reason, inspected.error().error);
    }
    Assessment out = classify_held(std::move(*inspected), planned, preferred_groups, quality);
    const bool keep_open = real_run && out.kind == ExistingKind::incomplete;
    out.handle = std::move(handle); // the descriptor moves; it is not closed
    if (!keep_open) {
        out.handle.fd.reset(); // nothing reads or publishes through it any more
    }
    return out;
}

Result<Assessment> assess(const OutputDir& dir, const PlannedEpub& planned,
                          std::span<const std::string> preferred_groups, ImageQuality quality,
                          bool real_run) {
    Result<ExistingHandle> opened = dir.open_existing(planned.file_name);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return assess_handle(std::move(*opened), planned, preferred_groups, quality, real_run);
}

Result<void> prepass(ReadSource& source, Assessment& assessment, const PlannedEpub& planned,
                     ImageQuality quality, std::string_view path, DownloadObserver& observer,
                     std::stop_source& stop) {
    if (assessment.kind != ExistingKind::incomplete || !assessment.held || !assessment.decision) {
        return std::unexpected(internal_error("the pre-pass needs an incomplete file"));
    }
    const HeldBook& held = *assessment.held;
    const std::size_t count = assessment.decision->chapters.size();
    assessment.page_info.assign(count, {});
    assessment.reuse_cover = false;
    assessment.cover_info.reset();

    // By index: fall_back_to_download replaces the decision, so nothing from the old final list
    // is held across it.
    for (std::size_t index = 0; index < count; ++index) {
        const FinalChapter& entry = assessment.decision->chapters[index];
        if (entry.source != ChapterSource::reuse) {
            continue;
        }
        if (!entry.held || *entry.held >= held.pages.size()) {
            return std::unexpected(internal_error("a reused chapter has no held pages"));
        }
        Result<std::optional<std::vector<ImageInfo>>> checked =
            validate_pages(source, held.pages[*entry.held], stop);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (checked->has_value()) {
            assessment.page_info[index] = std::move(**checked);
            continue;
        }
        // The warning is built before the decision changes (`entry` points into it).
        observer.on_warning(damaged_warning(path, entry.chapter));
        assessment.decision =
            fall_back_to_download(std::move(*assessment.decision), index, planned, quality);
    }

    if (planned.cover && held.cover && held.manifest.cover &&
        held.manifest.cover->file_name == planned.cover->file_name) {
        if (stop_now(stop)) {
            return std::unexpected(interrupted_error());
        }
        const std::expected<HeldPage, PageFault> cover = read_held_page(source, *held.cover);
        if (cover) {
            assessment.reuse_cover = true;
            assessment.cover_info = cover->info;
        }
    }
    assessment.final_plan = final_epub(planned, *assessment.decision);
    return {};
}

std::size_t held_page_count(const HeldBook& held) noexcept {
    std::size_t total = 0;
    for (const std::vector<HeldEntry>& pages : held.pages) {
        total += pages.size(); // at most zip_max_entries in all (inspect_existing)
    }
    return total;
}

std::vector<Chapter> held_chapters(const HeldBook& held) {
    std::vector<Chapter> out{};
    out.reserve(held.manifest.chapters.size());
    for (const ManifestChapter& record : held.manifest.chapters) {
        out.push_back(chapter_from(record));
    }
    return out;
}

std::string foreign_warning(std::string_view path, const InspectFailure& failure) {
    const std::string_view reason = foreign_reason_text(failure.reason);
    const std::string_view remedy = foreign_remedy(failure.reason);
    if (failure.reason == ForeignReason::unreadable && failure.error != 0) {
        return std::format("{}: left alone: {} ({}); {}", path, reason, errno_text(failure.error),
                           remedy);
    }
    return std::format("{}: left alone: {}; {}", path, reason, remedy);
}

std::string diverged_warning(std::string_view path, const UpdateDecision& decision,
                             std::span<const ManifestChapter> held) {
    // The chapters MangaDex lists that the file lacks: the final list's entries for `missing`.
    std::vector<Chapter> lacking{};
    lacking.reserve(decision.missing.size());
    for (const std::size_t planned : decision.missing) {
        const auto entry = std::ranges::find(decision.chapters, planned, &FinalChapter::planned);
        if (entry != decision.chapters.end()) {
            lacking.push_back(entry->chapter);
        }
    }
    // The chapters the file holds that MangaDex no longer lists: `extra` indexes the manifest.
    std::vector<Chapter> unlisted{};
    unlisted.reserve(decision.extra.size());
    for (const std::size_t index : decision.extra) {
        if (index < held.size()) {
            unlisted.push_back(chapter_from(held[index]));
        }
    }
    const std::size_t extra = decision.extra.size();
    return std::format("{}: MangaDex lists {} this file lacks{}, but the file holds {} MangaDex "
                       "no longer lists{}; left it alone (pass --overwrite to rebuild it without "
                       "{})",
                       path, chapters_count(decision.missing.size()),
                       labels_suffix(lacking, decision.missing.size()), chapters_count(extra),
                       labels_suffix(unlisted, extra), extra == 1 ? "it" : "them");
}

std::string damaged_warning(std::string_view path, const Chapter& chapter) {
    return std::format("{}: {} in the existing file is damaged; downloading it again", path,
                       chapter_labels(std::span<const Chapter>{&chapter, 1}));
}

std::string kept_line(std::string_view path, const KeptRelease& kept) {
    return std::format("{}kept the release already in the file ({}) instead of {}",
                       key_prefix(path, kept.key_label), kept.held_id, kept.planned_id);
}

std::string repick_line(std::string_view path, const Repick& repick) {
    const std::string held =
        repick.held_group.empty() ? std::string{"no group"} : repick.held_group;
    const std::string planned = repick.planned_group.empty()
                                    ? std::string{"the planned release"}
                                    : std::format("the release by {}", repick.planned_group);
    return std::format("{}replacing the release in the file ({}) with {} (--group)",
                       key_prefix(path, repick.key_label), held, planned);
}

std::string updating_line(std::string_view path, const UpdateDecision& decision) {
    std::vector<Chapter> fresh{};
    std::size_t reused = 0;
    for (const FinalChapter& entry : decision.chapters) {
        if (entry.source == ChapterSource::download) {
            fresh.push_back(entry.chapter);
        } else {
            ++reused;
        }
    }
    return std::format("updating {}: downloading {}{}, reusing {}", path,
                       chapters_count(fresh.size()), labels_suffix(fresh, fresh.size()), reused);
}

void announce(DownloadObserver& observer, std::string_view path, const Assessment& assessment) {
    switch (assessment.kind) {
    case ExistingKind::none:
    case ExistingKind::unchecked:
        return;
    case ExistingKind::foreign:
        if (assessment.foreign) {
            observer.on_warning(foreign_warning(path, *assessment.foreign));
        }
        return;
    case ExistingKind::diverged:
        if (assessment.decision) {
            // Without the held book (never, for an assessed file) the warning only counts.
            const std::span<const ManifestChapter> held =
                assessment.held
                    ? std::span<const ManifestChapter>{assessment.held->manifest.chapters}
                    : std::span<const ManifestChapter>{};
            observer.on_warning(diverged_warning(path, *assessment.decision, held));
        }
        return;
    case ExistingKind::complete:
    case ExistingKind::incomplete:
        if (assessment.decision) {
            for (const KeptRelease& kept : assessment.decision->kept) {
                observer.on_verbose(1, kept_line(path, kept));
            }
            for (const Repick& repick : assessment.decision->repicked) {
                observer.on_verbose(1, repick_line(path, repick));
            }
        }
        return;
    }
}

} // namespace mangadex_dl::detail
