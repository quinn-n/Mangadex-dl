// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/; skip-existing plan T10): what the execute phase finds under a
// planned file's name, and what it does about it before building anything.
//
//   assess   open_existing -> FdReadSource -> inspect_existing -> decide_update. Reads the central
//            directory, the manifest and local headers only (never page data), so a dry run uses
//            it as it is (D11).
//   prepass  a real run's update only: reads every page the update means to reuse (one image in
//            memory at a time) and records its ImageInfo. A page that fails to read or validate
//            switches its chapter to downloading (D9), with a warning.
//   announce the warnings and -v lines one assessment produces.
//
// The descriptor (V5 ABA invariant): Assessment::handle.fd is the inspection descriptor. For a
// real run's incomplete file it stays open until OutputDir::publish_replacing has returned,
// because the identity comparison there is only meaningful while the inode stays referenced (ext4
// reuses a freed inode number at once). The Assessment is therefore a local of the caller,
// declared before everything that borrows its descriptor, and is never moved, reset or closed
// before the publish. Every other assessment (complete, diverged, foreign, and every dry run)
// closes the descriptor before assess returns.
//
// Threads: calling thread only. Nothing here is shared with the FetchPool.
#pragma once

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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl::detail {

/// Everything one look at an existing file decided. Owns all of it: the descriptor, the held
/// book and the decision (whose FinalChapter indices point into `held`'s manifest and pages,
/// which are parallel). Move-only through `handle`; a moved-from Assessment holds fd -1.
struct Assessment {
    /// none: nothing under the name; complete / incomplete / diverged: ours (held and decision
    /// set); foreign: `foreign` says why. Never unchecked (--overwrite does not assess).
    ExistingKind kind = ExistingKind::none;
    /// The inspection descriptor. Open only for a real run's incomplete file (see the header
    /// comment); closed (fd -1) otherwise.
    ExistingHandle handle{};
    std::optional<HeldBook> held{};           ///< ours: what the file holds
    std::optional<UpdateDecision> decision{}; ///< ours: the update decision
    /// ours: final_epub(planned, *decision), recomputed by prepass after any fallback.
    PlannedEpub final_plan{};
    std::optional<InspectFailure> foreign{}; ///< foreign only
    /// After prepass: parallel to decision->chapters; for a reused entry, the ImageInfo of each
    /// held page in order; empty for a downloaded entry.
    std::vector<std::vector<ImageInfo>> page_info{};
    std::optional<ImageInfo> cover_info{}; ///< after prepass: set exactly when reuse_cover is
    bool reuse_cover = false;              ///< after prepass: copy the held cover image
};

/// The size an FdReadSource over the file reads up to: FileIdentity::size, or nullopt for a
/// negative size (never reported by fstat for a regular file).
[[nodiscard]] std::optional<std::uint64_t> readable_size(const FileIdentity& identity) noexcept;

/// The decision part of an assessment, without I/O: classifies `held` (inspected for `planned`)
/// with decide_update, and fills final_plan. `handle` stays empty.
[[nodiscard]] Assessment classify_held(HeldBook held, const PlannedEpub& planned,
                                       std::span<const std::string> preferred_groups,
                                       ImageQuality quality);

/// Classifies what open_existing returned for `planned.file_name`. An opened handle whose
/// descriptor is not valid (a moved-from handle) is an internal error (Errc::failure), never a
/// read through descriptor -1. The descriptor is moved into the result, never closed on the way;
/// it is closed before returning unless `real_run` and the file is incomplete.
[[nodiscard]] Result<Assessment> assess_handle(ExistingHandle handle, const PlannedEpub& planned,
                                               std::span<const std::string> preferred_groups,
                                               ImageQuality quality, bool real_run);

/// open_existing(planned.file_name) on `dir`, then assess_handle. Errc::failure for an open
/// error other than the ones ExistingOpen classifies (EMFILE, for one): the run ends with exit 1.
[[nodiscard]] Result<Assessment> assess(const OutputDir& dir, const PlannedEpub& planned,
                                        std::span<const std::string> preferred_groups,
                                        ImageQuality quality, bool real_run);

/// For an incomplete assessment: reads and validates every page of every reused chapter, then
/// the held cover when the plan's cover has the manifest's file name. A page fault of either kind
/// switches its chapter to downloading (fall_back_to_download) after a damaged warning to
/// `observer`; a cover fault only means the cover is downloaded. Fills page_info, cover_info and
/// reuse_cover, then final_plan. `stop` is checked once per page (Errc::interrupted).
///
/// `source` reads the file `assessment.held` was inspected from (the caller's FdReadSource over
/// assessment.handle.fd); it is only used during the call. `planned` is the plan's file the
/// decision was made for. Errc::failure when the assessment is not incomplete or is inconsistent.
[[nodiscard]] Result<void> prepass(ReadSource& source, Assessment& assessment,
                                   const PlannedEpub& planned, ImageQuality quality,
                                   std::string_view path, DownloadObserver& observer,
                                   std::stop_source& stop);

/// Every page image the held file has (all of its chapters).
[[nodiscard]] std::size_t held_page_count(const HeldBook& held) noexcept;

/// The held chapters as Chapters, in file order (a diverged outcome lists them, D2).
[[nodiscard]] std::vector<Chapter> held_chapters(const HeldBook& held);

/// The HeldBook a dry run assumes for a file an earlier row of the same run would leave: `book`
/// is that row's planned EpubOutcome::epub (the plan, or an update's final plan). What a real
/// build writes (EpubBuild::run): manifest.identifier = book.identifier; one chapter per
/// book.chapters entry, manifest_chapter_from(chapter, quality), holding max(chapter.pages, 0)
/// image entries named as the writer names them (page_image_entry, one page index across the
/// book; `.jpg` as a placeholder extension); `pages` parallel to the chapters with as many
/// placeholder HeldEntry (the name only); no cover (manifest.cover and cover both nullopt).
///
/// Known differences that decide nothing: every chapter's `quality` is `quality` (a real
/// manifest keeps the held quality of a reused chapter; nothing reads it here), and the image
/// count is the chapter list's `pages`, not the at-home server's count. A chapter holds at most
/// max_pages_per_chapter images and the book at most zip_max_entries, as a real file can (a
/// planned book never exceeds either; this only bounds the allocation).
///
/// For the decision and the dry-run report only: it is never read through a ReadSource (no
/// prepass, no read_held_page; a dry run has neither).
[[nodiscard]] HeldBook simulated_held(const PlannedEpub& book, ImageQuality quality);

// ---- messages (path as built for the report; the observer makes them terminal-safe) ---------

/// `<path>: left alone: <reason>[ (<strerror>)]; <remedy>`. The errno text only for unreadable.
[[nodiscard]] std::string foreign_warning(std::string_view path, const InspectFailure& failure);

/// `<path>: MangaDex lists N chapters this file lacks (Ch. 4, ...), but the file holds M chapters
/// MangaDex no longer lists (Ch. 3, ...); left it alone (pass --overwrite to rebuild it without
/// them)`, with "1 chapter" and "without it" when the count is one. Each list shows at most 5
/// labels, then "and K more". The lacking chapters are the final-list entries for
/// `decision.missing`; the unlisted ones are `held[i]` for each i in `decision.extra`. A list is
/// left out (counts only) unless every chapter it counts resolves to a label.
[[nodiscard]] std::string diverged_warning(std::string_view path, const UpdateDecision& decision,
                                           std::span<const ManifestChapter> held);

/// `<path>: Ch. 3 in the existing file is damaged; downloading it again`.
[[nodiscard]] std::string damaged_warning(std::string_view path, const Chapter& chapter);

/// `<path>: Vol. 1 Ch. 1: kept the release already in the file (<held id>) instead of <id>`.
[[nodiscard]] std::string kept_line(std::string_view path, const KeptRelease& kept);

/// `<path>: Vol. 1 Ch. 1: replacing the release in the file (<groups>) with the release by
/// <groups> (--group)`. Empty group names read "no group" and "the planned release".
[[nodiscard]] std::string repick_line(std::string_view path, const Repick& repick);

/// `updating <path>: downloading 1 chapter (Ch. 4), reusing 2` (at most 5 labels, then "and K
/// more"; no parenthesis when nothing is downloaded).
[[nodiscard]] std::string updating_line(std::string_view path, const UpdateDecision& decision);

/// Tells `observer` what `assessment` found: the foreign or diverged warning, or for a complete
/// or incomplete file one -v line per kept and per re-picked release. Nothing for none.
void announce(DownloadObserver& observer, std::string_view path, const Assessment& assessment);

} // namespace mangadex_dl::detail
