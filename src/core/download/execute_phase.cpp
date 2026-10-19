// SPDX-License-Identifier: GPL-3.0-or-later
//
// The execute phase of run_download (download plan T14 + rev1, skip-existing plan T10). Files are
// produced one at a time on the calling thread:
//   assessment of an existing file (unless --overwrite; download/existing.hpp) -> plan_error
//   check -> [update: pre-pass over the pages to reuse] -> temporary -> EPUB begin -> cover ->
//   per chapter: either its held pages copied from the existing file, or fetch_at_home and its
//   pages through the FetchPool -> EPUB finish (with the book manifest) -> publish.
// Workers only fetch and sniff pages (ChapterPages::fetch); the pool hands the results back to
// the calling thread in page order (EpubBuild::consume), so the bytes written do not depend on
// --jobs and every observer call happens on the calling thread. Held pages are copied on the
// calling thread, between pool runs, and the EpubWriter call sequence of an update is exactly
// that of a fresh build of the same final chapter list, so the two are byte-identical.
//
// What an existing file gets (spec "Summary"): absent -> written; complete, diverged or foreign ->
// left alone (exists); incomplete -> rebuilt into a temporary and published with
// publish_replacing, which replaces it only while it is the file that was inspected. The
// inspection descriptor stays open until that publish has returned (V5): in produce() the
// Assessment owning it is declared before the FdReadSource that borrows it, the TempFile and the
// EpubBuild, so every early return or unwind discards the temporary first and closes the
// descriptor last.
//
// Error classes, decided by Error::code:
// - Errc::failure / Errc::io (a local file-system problem, or an exception on a worker) and
//   Errc::interrupted end the run. The temporaries are discarded by their destructors.
// - Anything else is a failure of one file (Errc::incomplete here, or a network error): that
//   file is reported failed, nothing is published for it (an existing file is kept), and the run
//   continues.
// ZipWriter and EpubWriter report every problem as Errc::failure; TrackingSink tells a failing
// temporary file (local, abort) apart from a file that cannot be built (that file fails).
#include "download/existing.hpp"
#include "download/internal.hpp"
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/fetch_pool.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/update_plan.hpp"
#include "mangadex_dl/zip.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::detail {
namespace {

/// At-home server refreshes per chapter after failed page fetches (spec: "refresh ≤3×").
inline constexpr int max_at_home_refreshes = 3;

/// dcterms:modified when no chapter of a file has a usable timestamp (deterministic output).
inline constexpr std::string_view epoch_modified = "1970-01-01T00:00:00Z";

/// One file failed; the run goes on.
[[nodiscard]] Error file_failure(std::string message) {
    return Error{.code = Errc::incomplete, .message = std::move(message)};
}

/// Errors that end the whole run rather than one file: a local problem or a stop.
[[nodiscard]] bool aborts_run(const Error& error) noexcept {
    return error.code == Errc::failure || error.code == Errc::io || error.code == Errc::interrupted;
}

/// Views the characters of `text` as bytes. The span borrows `text`.
[[nodiscard]] std::span<const std::byte>
bytes_of(const std::string& text MANGADEX_DL_LIFETIMEBOUND) noexcept {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

/// The page list for `quality`. Borrows from `server`.
[[nodiscard]] const std::vector<std::string>&
files_of(const AtHomeServer& server MANGADEX_DL_LIFETIMEBOUND, ImageQuality quality) noexcept {
    return quality == ImageQuality::data_saver ? server.data_saver : server.data;
}

/// "DIR/<file name>" with DIR as typed (the default "." gives "./<name>").
[[nodiscard]] std::string output_path(const DownloadOptions& options, const PlannedEpub& epub) {
    return (options.dir / epub.file_name).string();
}

// ---- EPUB metadata and navigation -----------------------------------------------------------

void append_unique(std::vector<std::string>& out, const std::string& value) {
    if (!value.empty() && std::ranges::find(out, value) == out.end()) {
        out.push_back(value);
    }
}

/// Authors, then artists, each name once.
[[nodiscard]] std::vector<std::string> creators_of(const MangaInfo& manga) {
    std::vector<std::string> out{};
    for (const std::string& name : manga.authors) {
        append_unique(out, name);
    }
    for (const std::string& name : manga.artists) {
        append_unique(out, name);
    }
    return out;
}

/// The scanlation groups of the file's chapters, each name once, in reading order.
[[nodiscard]] std::vector<std::string> contributors_of(const PlannedEpub& epub) {
    std::vector<std::string> out{};
    for (const Chapter& chapter : epub.chapters) {
        for (const ScanGroup& group : chapter.groups) {
            append_unique(out, group.name);
        }
    }
    return out;
}

/// The latest chapter updatedAt (else publishAt) as UTC "CCYY-MM-DDThh:mm:ssZ". Those strings
/// compare chronologically, so the maximum is the latest.
[[nodiscard]] std::string modified_of(const PlannedEpub& epub) {
    std::string latest{};
    for (const Chapter& chapter : epub.chapters) {
        Result<std::string> stamp = format_modified(chapter.updated_at);
        if (!stamp) {
            stamp = format_modified(chapter.publish_at);
        }
        if (stamp && *stamp > latest) {
            latest = std::move(*stamp);
        }
    }
    return latest.empty() ? std::string{epoch_modified} : latest;
}

[[nodiscard]] EpubMetadata metadata_for(const PlanResult& planned, const PlannedEpub& epub) {
    const MangaSummary& manga = planned.plan.manga;
    return EpubMetadata{
        .identifier = epub.identifier,
        .title = epub.display_title,
        .language = manga.language,
        .creators = creators_of(planned.manga),
        .contributors = contributors_of(epub),
        .source = manga.url,
        .modified = modified_of(epub),
        .series = epub.series_position ? manga.title : std::string{},
        .series_position = epub.series_position,
        .direction = manga.direction,
    };
}

/// "Ch. 12: Title", "Ch. 12", the chapter title, or "Oneshot".
[[nodiscard]] std::string nav_label(const Chapter& chapter) {
    if (chapter.chapter) {
        return chapter.title.empty() ? std::format("Ch. {}", *chapter.chapter)
                                     : std::format("Ch. {}: {}", *chapter.chapter, chapter.title);
    }
    if (!chapter.title.empty()) {
        return chapter.title;
    }
    return chapter.volume ? std::string{"Chapter"} : std::string{"Oneshot"};
}

/// Layout single nests chapters under their volume. The planner orders a single file by volume
/// (loose chapters last), so the entries of one group are consecutive, as EpubWriter requires.
[[nodiscard]] std::optional<std::string> nav_group(const PlannedEpub& epub,
                                                   const Chapter& chapter) {
    if (epub.kind != EpubKind::single || !chapter.volume) {
        return std::nullopt;
    }
    return std::format("Vol. {}", *chapter.volume);
}

// ---- sinks and pages ------------------------------------------------------------------------

/// Forwards to a borrowed sink (the temporary file) and remembers its first failure.
class TrackingSink final : public ByteSink {
public:
    /// Borrows `target`, which must outlive this sink.
    explicit TrackingSink(ByteSink& target) noexcept : target_(&target) {}

    [[nodiscard]] Result<void> write(std::span<const std::byte> data) override {
        Result<void> written = target_->write(data);
        if (!written && !error_) {
            error_ = written.error();
        }
        return written;
    }

    /// The target's first failure, if any.
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

private:
    ByteSink* target_ = nullptr; ///< borrowed; never null
    std::optional<Error> error_{};
};

/// The at-home server of one chapter, shared by the pool workers that fetch its pages. A failed
/// page fetch refreshes it (fetch_at_home again), at most max_at_home_refreshes times for the
/// chapter, whatever the failure (an image-host 403 included). Lives on the calling thread's
/// stack for the duration of one FetchPool::run.
class ChapterPages {
public:
    /// Borrows `client` and `lines`, which outlive the object (run_download's locals).
    ChapterPages(ApiClient& client, LineQueue& lines, std::stop_source run_stop,
                 std::string chapter_id, AtHomeServer server, ImageQuality quality)
        : client_(&client), lines_(&lines), run_stop_(std::move(run_stop)),
          chapter_id_(std::move(chapter_id)), quality_(quality),
          count_(files_of(server, quality).size()),
          server_(std::make_shared<const AtHomeServer>(std::move(server))) {}

    /// The number of pages, from the first at-home answer.
    [[nodiscard]] std::size_t count() const noexcept { return count_; }

    /// Downloads and sniffs page `index`. Called concurrently on pool workers.
    [[nodiscard]] Result<FetchedPage> fetch(std::size_t index, const std::stop_token& stop) {
        while (true) {
            if (stop_now(run_stop_) || stop.stop_requested()) {
                return std::unexpected(interrupted_error());
            }
            const Snapshot current = snapshot();
            const std::vector<std::string>& files = files_of(*current.server, quality_);
            if (files.size() != count_ || index >= count_) {
                return std::unexpected(file_failure(
                    std::format("chapter {}: the at-home server changed the chapter's page list",
                                chapter_id_)));
            }
            Result<std::string> body =
                client_->fetch_page(*current.server, quality_, files[index], stop);
            if (body) {
                const Result<ImageInfo> info = sniff_image(bytes_of(*body));
                if (!info) {
                    return std::unexpected(
                        file_failure(std::format("chapter {} page {}: unsupported image ({})",
                                                 chapter_id_, index + 1, info.error().message)));
                }
                return FetchedPage{.bytes = std::move(*body), .info = *info};
            }
            if (body.error().code == Errc::interrupted) {
                return std::unexpected(std::move(body.error()));
            }
            // Any failure refreshes (spec: refresh up to 3 times), so a page that fails for good
            // spends the chapter's whole refresh budget before the file is given up.
            if (Result<void> refreshed = refresh(current.generation, body.error(), stop);
                !refreshed) {
                return std::unexpected(std::move(refreshed.error()));
            }
        }
    }

private:
    struct Snapshot {
        std::shared_ptr<const AtHomeServer> server{};
        std::uint64_t generation = 0;
    };

    [[nodiscard]] Snapshot snapshot() const {
        const std::scoped_lock lock{mutex_};
        return Snapshot{.server = server_, .generation = generation_};
    }

    /// Replaces the server after a page fetch failed with `cause` on `failed_generation`. A
    /// worker that finds another worker already refreshed simply retries with the new server.
    /// The refreshes are serialized, so the per-chapter budget is exact.
    [[nodiscard]] Result<void> refresh(std::uint64_t failed_generation, const Error& cause,
                                       const std::stop_token& stop) {
        const std::scoped_lock refresh_lock{refresh_mutex_};
        if (snapshot().generation != failed_generation) {
            return {};
        }
        if (refreshes_ >= max_at_home_refreshes) {
            return std::unexpected(file_failure(std::format(
                "{} (after {} at-home server refreshes)", cause.message, max_at_home_refreshes)));
        }
        ++refreshes_;
        lines_->note(1, std::format("chapter {}: {}; refreshing its at-home server ({} of {})",
                                    chapter_id_, cause.message, refreshes_, max_at_home_refreshes));
        Result<AtHomeServer> fresh = client_->fetch_at_home(chapter_id_, stop);
        if (!fresh) {
            if (fresh.error().code == Errc::interrupted) {
                return std::unexpected(std::move(fresh.error()));
            }
            return std::unexpected(
                file_failure(std::format("chapter {}: {}", chapter_id_, fresh.error().message)));
        }
        auto next = std::make_shared<const AtHomeServer>(std::move(*fresh));
        const std::scoped_lock lock{mutex_};
        server_ = std::move(next);
        ++generation_;
        return {};
    }

    ApiClient* client_ = nullptr; ///< borrowed; never null; thread-safe
    LineQueue* lines_ = nullptr;  ///< borrowed; never null; thread-safe
    std::stop_source run_stop_{}; ///< the run's stop state (request_stop is thread-safe)
    std::string chapter_id_{};
    ImageQuality quality_ = ImageQuality::data;
    std::size_t count_ = 0;

    mutable std::mutex mutex_{};
    /// Shared: a worker keeps its snapshot alive while a refresh replaces it.
    std::shared_ptr<const AtHomeServer> server_{}; ///< guarded by mutex_
    std::uint64_t generation_ = 0;                 ///< guarded by mutex_

    std::mutex refresh_mutex_{}; ///< taken before mutex_, never after it
    int refreshes_ = 0;          ///< guarded by refresh_mutex_
};

/// What a finished file holds.
struct BuiltFile {
    std::size_t pages = 0;
    std::uint64_t bytes = 0;
};

/// Where an update copies held pages from. Every pointer is borrowed from produce()'s locals (the
/// Assessment and the FdReadSource over its descriptor), which outlive the build; never null.
struct HeldCopy {
    ReadSource* source = nullptr;   ///< reads the file `book` was inspected from
    const HeldBook* book = nullptr; ///< Assessment::held
    /// Assessment::page_info: parallel to the final list, the pre-pass ImageInfo of every page.
    const std::vector<std::vector<ImageInfo>>* page_info = nullptr;
    std::optional<ImageInfo> cover{}; ///< set when the held cover is copied (its pre-pass info)
};

/// A held page or cover that no longer reads or validates as it did in the pre-pass. There is no
/// fallback mid-build: the file fails and the existing file is kept.
[[nodiscard]] Error changed_during_copy() {
    return file_failure("the existing file changed or could not be read during the update");
}

/// An invariant of this module broke (never expected): ends the run with exit 1.
[[nodiscard]] Error internal_error(std::string_view what) {
    return Error{.code = Errc::failure, .message = std::format("internal error: {}", what)};
}

/// A fresh build's final list: every planned chapter downloaded at the run's quality.
[[nodiscard]] std::vector<FinalChapter> fresh_chapters(const PlannedEpub& epub,
                                                       ImageQuality quality) {
    std::vector<FinalChapter> out{};
    out.reserve(epub.chapters.size());
    for (std::size_t index = 0; index < epub.chapters.size(); ++index) {
        out.push_back(FinalChapter{.chapter = epub.chapters[index],
                                   .source = ChapterSource::download,
                                   .quality = quality,
                                   .held = std::nullopt,
                                   .planned = index});
    }
    return out;
}

/// Builds one file into a temporary, entirely on the calling thread (the pool's consumer runs
/// here too). Neither copyable nor movable: the pool's functions point at it.
///
/// The final list decides each chapter: a reused one is copied from the existing file (held
/// pages, read and re-validated against the pre-pass), a downloaded one goes through the pool.
/// Either way the EpubWriter sees add_nav_entry then add_page per page, in list order, so an
/// update writes the bytes a fresh build of the same list would.
class EpubBuild {
public:
    /// Borrows everything: `ctx` and `planned` outlive the build; so do `epub` and `chapters`
    /// (the file and its final list), `held` (null for a fresh build) and `temp` (owned by the
    /// caller, which publishes it afterwards).
    EpubBuild(RunContext& ctx, const PlanResult& planned, const PlannedEpub& epub,
              const std::vector<FinalChapter>& chapters, const HeldCopy* held, TempFile& temp)
        : ctx_(&ctx), epub_(&epub), chapters_(&chapters), held_(held), sink_(temp), zip_(sink_),
          writer_(zip_, metadata_for(planned, epub)) {}
    EpubBuild(const EpubBuild&) = delete;
    EpubBuild& operator=(const EpubBuild&) = delete;
    EpubBuild(EpubBuild&&) = delete;
    EpubBuild& operator=(EpubBuild&&) = delete;
    ~EpubBuild() = default;

    [[nodiscard]] Result<BuiltFile> run() {
        if (Result<void> ok = writer_.begin(); !ok) {
            return std::unexpected(writer_error(ok.error()));
        }
        if (Result<void> ok = add_cover(); !ok) {
            return std::unexpected(std::move(ok.error()));
        }
        std::vector<EpubBookChapter> book{};
        book.reserve(chapters_->size());
        for (std::size_t index = 0; index < chapters_->size(); ++index) {
            const FinalChapter& entry = (*chapters_)[index];
            const std::size_t first = pages_;
            Result<void> ok = entry.source == ChapterSource::reuse
                                  ? copy_chapter(index, entry)
                                  : add_chapter(entry.chapter, entry.quality);
            if (!ok) {
                return std::unexpected(std::move(ok.error()));
            }
            book.push_back(EpubBookChapter{
                .record = manifest_chapter_from(entry.chapter, entry.quality),
                .first_page = first,
                .page_count = pages_ - first,
            });
        }
        std::optional<std::string> cover_name{};
        if (epub_->cover) {
            cover_name = epub_->cover->file_name;
        }
        if (Result<void> ok = writer_.finish(std::move(cover_name), std::move(book)); !ok) {
            return std::unexpected(writer_error(ok.error()));
        }
        if (Result<void> ok = zip_.finish(); !ok) {
            return std::unexpected(writer_error(ok.error()));
        }
        return BuiltFile{.pages = pages_, .bytes = zip_.bytes_written()};
    }

private:
    /// A writer error: the temporary's own failure (local, ends the run) when the sink failed,
    /// else this file cannot be built.
    [[nodiscard]] Error writer_error(const Error& error) const {
        if (sink_.error()) {
            return *sink_.error();
        }
        return file_failure(std::format("{}: {}", epub_->file_name, error.message));
    }

    void warn(std::string_view message) const {
        ctx_->observer->on_warning(std::format("{}: {}", epub_->file_name, message));
    }

    /// The WebP warning, once per file.
    void note_media(MediaType type) {
        if (type == MediaType::webp && !webp_warned_) {
            webp_warned_ = true;
            warn("contains WebP images, which some EPUB readers cannot display");
        }
    }

    /// Adds the planned cover: copied from the existing file when the pre-pass validated it,
    /// else downloaded. Any problem with a downloaded cover is a warning, and the first page
    /// becomes the cover.
    [[nodiscard]] Result<void> add_cover() {
        if (!epub_->cover) {
            return {};
        }
        if (stop_now(ctx_->stop)) {
            return std::unexpected(interrupted_error());
        }
        if (held_ != nullptr && held_->cover) {
            return copy_cover(*held_->cover);
        }
        const Result<std::string> image = ctx_->client->fetch_cover(
            ctx_->options->manga, epub_->cover->file_name, ctx_->stop.get_token());
        ctx_->lines->drain(*ctx_->observer);
        if (!image) {
            if (image.error().code == Errc::interrupted) {
                return std::unexpected(image.error());
            }
            warn(std::format("could not download the cover image ({}); the first page is the "
                             "cover instead",
                             image.error().message));
            return {};
        }
        const Result<ImageInfo> info = sniff_image(bytes_of(*image));
        if (!info) {
            warn(std::format("unsupported cover image ({}); the first page is the cover instead",
                             info.error().message));
            return {};
        }
        note_media(info->type);
        if (Result<void> ok = writer_.add_cover(bytes_of(*image), *info); !ok) {
            return std::unexpected(writer_error(ok.error()));
        }
        return {};
    }

    /// Copies the held cover, which must read and validate exactly as in the pre-pass.
    [[nodiscard]] Result<void> copy_cover(ImageInfo expected) {
        if (!held_->book->cover) {
            return std::unexpected(internal_error("a reused cover has no held entry"));
        }
        const std::expected<HeldPage, PageFault> image =
            read_held_page(*held_->source, *held_->book->cover);
        if (!image || image->info != expected) {
            return std::unexpected(changed_during_copy());
        }
        note_media(image->info.type);
        if (Result<void> ok = writer_.add_cover(image->bytes, image->info); !ok) {
            return std::unexpected(writer_error(ok.error()));
        }
        return {};
    }

    /// The navigation entry of a chapter starting at the next page.
    void add_nav(const Chapter& chapter) {
        writer_.add_nav_entry(EpubNavEntry{
            .label = nav_label(chapter),
            .first_page = pages_,
            .group = nav_group(*epub_, chapter),
        });
    }

    /// Copies the held pages of the reused entry at `index` of the final list, on the calling
    /// thread (no pool, no network). Each page must read and validate exactly as it did in the
    /// pre-pass; anything else fails the file (D9: no fallback mid-build).
    [[nodiscard]] Result<void> copy_chapter(std::size_t index, const FinalChapter& entry) {
        if (held_ == nullptr || !entry.held || *entry.held >= held_->book->pages.size() ||
            index >= held_->page_info->size()) {
            return std::unexpected(internal_error("a reused chapter has no held pages"));
        }
        const std::vector<HeldEntry>& pages = held_->book->pages[*entry.held];
        const std::vector<ImageInfo>& infos = (*held_->page_info)[index];
        if (pages.empty() || infos.size() != pages.size()) {
            return std::unexpected(internal_error("a reused chapter was not pre-checked"));
        }
        if (stop_now(ctx_->stop)) {
            return std::unexpected(interrupted_error());
        }
        add_nav(entry.chapter);
        for (std::size_t page = 0; page < pages.size(); ++page) {
            if (stop_now(ctx_->stop)) {
                return std::unexpected(interrupted_error());
            }
            // One held image in memory at a time; its bytes are freed after this iteration.
            const std::expected<HeldPage, PageFault> image =
                read_held_page(*held_->source, pages[page]);
            if (!image || image->info != infos[page]) {
                return std::unexpected(changed_during_copy());
            }
            if (Result<void> ok = append_page(image->bytes, image->info); !ok) {
                return ok;
            }
        }
        return {};
    }

    /// Looks up the chapter's at-home server, then downloads its pages in parallel and adds them
    /// in order.
    [[nodiscard]] Result<void> add_chapter(const Chapter& chapter, ImageQuality quality) {
        if (stop_now(ctx_->stop)) {
            return std::unexpected(interrupted_error());
        }
        Result<AtHomeServer> server =
            ctx_->client->fetch_at_home(chapter.id, ctx_->stop.get_token());
        ctx_->lines->drain(*ctx_->observer);
        if (!server) {
            if (server.error().code == Errc::interrupted) {
                return std::unexpected(std::move(server.error()));
            }
            return std::unexpected(
                file_failure(std::format("chapter {}: {}", chapter.id, server.error().message)));
        }
        if (files_of(*server, quality).empty()) {
            return std::unexpected(file_failure(
                std::format("chapter {}: the at-home server lists no pages", chapter.id)));
        }
        add_nav(chapter);

        ChapterPages pages{*ctx_->client, *ctx_->lines,       ctx_->stop,
                           chapter.id,    std::move(*server), quality};
        // Both functions are borrowed by FetchPool::run only until it returns, and it never
        // returns while a worker still runs one. They point at `pages` and at *this, which
        // outlive that call; nothing is captured by reference. No held data (the source, the
        // HeldBook, the descriptor) is reachable from them.
        const PageFetcher fetch = [source = &pages](std::size_t index,
                                                    const std::stop_token& stop) {
            return source->fetch(index, stop);
        };
        const PageConsumer consume = [build = this](std::size_t /*index*/,
                                                    const FetchedPage& page) {
            return build->consume(page);
        };
        Result<void> ran = ctx_->pool->run(pages.count(), fetch, consume, ctx_->stop.get_token());
        ctx_->lines->drain(*ctx_->observer);
        if (!ran) {
            if (aborts_run(ran.error())) {
                return std::unexpected(std::move(ran.error()));
            }
            return std::unexpected(file_failure(std::move(ran.error().message)));
        }
        return {};
    }

    /// The pool's consumer: adds the next page (pages arrive in order), on the calling thread.
    [[nodiscard]] Result<void> consume(const FetchedPage& page) {
        if (stop_now(ctx_->stop)) {
            return std::unexpected(interrupted_error());
        }
        return append_page(bytes_of(page.bytes), page.info);
    }

    /// Adds the next page image and reports progress (downloaded and copied pages alike).
    [[nodiscard]] Result<void> append_page(std::span<const std::byte> image, ImageInfo info) {
        note_media(info.type);
        if (Result<void> ok = writer_.add_page(image, info); !ok) {
            return std::unexpected(writer_error(ok.error()));
        }
        ++pages_;
        ctx_->lines->drain(*ctx_->observer);
        ctx_->observer->on_pages(pages_, std::max(pages_, epub_->planned_pages));
        return {};
    }

    RunContext* ctx_ = nullptr;                           ///< borrowed; never null
    const PlannedEpub* epub_ = nullptr;                   ///< borrowed; never null
    const std::vector<FinalChapter>* chapters_ = nullptr; ///< borrowed; never null
    const HeldCopy* held_ = nullptr;                      ///< borrowed; null for a fresh build
    // Declaration order is the borrow order: the sink borrows the caller's TempFile, the zip
    // writer borrows the sink and the EPUB writer borrows the zip writer, so the writers are
    // destroyed first. None of the three has a default constructor: they are initialized in the
    // constructor's member-initializer list.
    TrackingSink sink_;
    ZipWriter zip_;
    EpubWriter writer_;
    std::size_t pages_ = 0; ///< pages added so far
    bool webp_warned_ = false;
};

/// Builds `epub` (its final list `chapters`, copying reused chapters through `held`) into
/// `temp`. The build (and its writers, which borrow `temp`) is gone when this returns, so the
/// caller may publish `temp` right away.
[[nodiscard]] Result<BuiltFile> build_epub(RunContext& ctx, const PlanResult& planned,
                                           const PlannedEpub& epub,
                                           const std::vector<FinalChapter>& chapters,
                                           const HeldCopy* held, TempFile& temp) {
    EpubBuild build{ctx, planned, epub, chapters, held, temp};
    return build.run();
}

// ---- one file -------------------------------------------------------------------------------

/// One file's outcome, plus the held releases the file holds in place of planned ones (for
/// reconcile_skipped; empty unless the file holds the final list after this run).
struct FileResult {
    EpubOutcome outcome{};
    std::vector<KeptRelease> kept{};
};

/// A file left alone (complete, diverged or foreign) is reported as exists. Ours: `pages` is
/// what the file holds; a complete file lists its final chapters (substitutions applied), a
/// diverged one its held chapters (D2). Foreign: the planned chapters with 0 pages.
void report_left_alone(const Assessment& assessment, FileResult& result) {
    EpubOutcome& outcome = result.outcome;
    outcome.status = EpubStatus::exists;
    if (!assessment.held) {
        return;
    }
    outcome.pages = held_page_count(*assessment.held);
    if (assessment.kind == ExistingKind::complete) {
        outcome.epub = assessment.final_plan;
        if (assessment.decision) {
            result.kept = assessment.decision->kept;
        }
        return;
    }
    outcome.epub.chapters = held_chapters(*assessment.held);
}

/// An incomplete file's lists (D1): its final chapters, and which are downloaded or reused.
void report_update(const Assessment& assessment, EpubOutcome& outcome) {
    outcome.epub = assessment.final_plan;
    if (assessment.decision) {
        outcome.new_chapters = new_chapter_ids(*assessment.decision);
        outcome.reused_chapters = reused_chapter_ids(*assessment.decision);
    }
}

/// Downloads and builds the planned file from scratch, then publishes it (no-clobber unless
/// --overwrite).
[[nodiscard]] Result<FileResult> produce_fresh(RunContext& ctx, const PlanResult& planned,
                                               OutputDir& dir, std::size_t index,
                                               FileResult result) {
    const DownloadOptions& options = *ctx.options;
    const PlannedEpub& epub = planned.plan.epubs[index];
    EpubOutcome& outcome = result.outcome;
    if (epub.plan_error) {
        outcome.error = *epub.plan_error;
        return result;
    }

    ctx.observer->on_epub_begin(epub, index, planned.plan.epubs.size());
    Result<TempFile> temp = dir.create_temp(epub.file_name);
    if (!temp) {
        return std::unexpected(std::move(temp.error()));
    }
    const std::vector<FinalChapter> chapters = fresh_chapters(epub, options.quality);
    Result<BuiltFile> built = build_epub(ctx, planned, epub, chapters, nullptr, *temp);
    if (!built) {
        if (aborts_run(built.error())) {
            return std::unexpected(std::move(built.error()));
        }
        outcome.error = std::move(built.error().message);
        return result; // `temp` is discarded on the way out
    }
    const Result<PublishOutcome> published =
        dir.publish(std::move(*temp), epub.file_name, options.overwrite);
    if (!published) {
        return std::unexpected(published.error());
    }
    switch (*published) {
    case PublishOutcome::written:
    case PublishOutcome::replaced:
        outcome.status = EpubStatus::written;
        outcome.replaced = *published == PublishOutcome::replaced;
        outcome.pages = built->pages;
        outcome.bytes = built->bytes;
        return result;
    case PublishOutcome::exists: // created by someone else since the assessment
        outcome.status = EpubStatus::exists;
        return result;
    }
    // Only reachable through an out-of-range enum value.
    return std::unexpected(internal_error("unknown publish outcome"));
}

/// Rebuilds an incomplete file: the pre-pass, the build with page reuse, then
/// publish_replacing. `assessment` is the caller's and outlives this call, so its descriptor is
/// still open when publish_replacing runs (V5); the FdReadSource and the TempFile here are
/// declared after it and destroyed first, the temporary before the source.
[[nodiscard]] Result<FileResult> produce_update(RunContext& ctx, const PlanResult& planned,
                                                OutputDir& dir, std::size_t index,
                                                Assessment& assessment, FileResult result) {
    const DownloadOptions& options = *ctx.options;
    const PlannedEpub& epub = planned.plan.epubs[index];
    EpubOutcome& outcome = result.outcome;
    if (epub.plan_error) {
        // The plan cannot be built, so neither can the update: failed, the existing file kept.
        announce(*ctx.observer, outcome.path, assessment);
        report_update(assessment, outcome);
        outcome.error = *epub.plan_error;
        return result;
    }
    const std::optional<std::uint64_t> size = readable_size(assessment.handle.identity);
    if (!assessment.held || !assessment.decision || !size || !assessment.handle.fd.valid()) {
        return std::unexpected(internal_error("an update without its inspected file"));
    }
    // Borrows the Assessment's named descriptor (never a temporary's).
    FdReadSource source{assessment.handle.fd, *size};
    if (Result<void> checked = prepass(source, assessment, epub, options.quality, outcome.path,
                                       *ctx.observer, ctx.stop);
        !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    // The decision is final from here on (the pre-pass applied every fallback).
    const UpdateDecision& decision = *assessment.decision;
    announce(*ctx.observer, outcome.path, assessment);
    ctx.observer->on_verbose(1, updating_line(outcome.path, decision));
    report_update(assessment, outcome);

    ctx.observer->on_epub_begin(assessment.final_plan, index, planned.plan.epubs.size());
    Result<TempFile> temp = dir.create_temp(epub.file_name);
    if (!temp) {
        return std::unexpected(std::move(temp.error()));
    }
    const HeldCopy copy{
        .source = &source,
        .book = &*assessment.held,
        .page_info = &assessment.page_info,
        .cover = assessment.reuse_cover ? assessment.cover_info : std::nullopt,
    };
    Result<BuiltFile> built =
        build_epub(ctx, planned, assessment.final_plan, decision.chapters, &copy, *temp);
    if (!built) {
        if (aborts_run(built.error())) {
            return std::unexpected(std::move(built.error()));
        }
        outcome.error = std::move(built.error().message);
        return result; // `temp` is discarded, then the caller closes the descriptor
    }
    // The inspection descriptor is still open: the identity check is ABA-safe (V5).
    const Result<ReplaceOutcome> published =
        dir.publish_replacing(std::move(*temp), epub.file_name, assessment.handle.identity);
    if (!published) {
        return std::unexpected(published.error());
    }
    switch (*published) {
    case ReplaceOutcome::replaced:
    case ReplaceOutcome::written: // the name vanished meanwhile: published without clobbering
        outcome.status = EpubStatus::written;
        outcome.replaced = *published == ReplaceOutcome::replaced;
        outcome.pages = built->pages;
        outcome.bytes = built->bytes;
        result.kept = decision.kept;
        return result;
    case ReplaceOutcome::changed: {
        // Something else is under the name now; the new file was discarded.
        const InspectFailure changed{.reason = ForeignReason::changed_during_update, .error = 0};
        ctx.observer->on_warning(foreign_warning(outcome.path, changed));
        outcome.status = EpubStatus::exists;
        outcome.existing = ExistingKind::foreign;
        outcome.epub = epub;
        outcome.new_chapters.clear();
        outcome.reused_chapters.clear();
        return result;
    }
    }
    // Only reachable through an out-of-range enum value.
    return std::unexpected(internal_error("unknown publish outcome"));
}

/// Produces one file: assessment of what is under its name (unless --overwrite), then a fresh
/// build, an update, or nothing. The value is the file's outcome (written, exists or failed);
/// an Error ends the run.
[[nodiscard]] Result<FileResult> produce(RunContext& ctx, const PlanResult& planned, OutputDir& dir,
                                         std::size_t index) {
    const DownloadOptions& options = *ctx.options;
    const PlannedEpub& epub = planned.plan.epubs[index];
    FileResult result{
        .outcome =
            EpubOutcome{
                .epub = epub,
                .path = output_path(options, epub),
                .status = EpubStatus::failed,
            },
        .kept = {},
    };
    if (options.overwrite) {
        const Result<bool> exists = dir.exists(epub.file_name);
        if (!exists) {
            return std::unexpected(exists.error());
        }
        result.outcome.existing = *exists ? ExistingKind::unchecked : ExistingKind::none;
        return produce_fresh(ctx, planned, dir, index, std::move(result));
    }

    // Owns the inspection descriptor. Declared before everything that borrows it, never moved,
    // and destroyed last: for an update the descriptor stays open through publish (V5).
    Result<Assessment> assessed =
        assess(dir, epub, options.groups, options.quality, /*real_run=*/true);
    if (!assessed) {
        return std::unexpected(std::move(assessed.error()));
    }
    Assessment& assessment = *assessed;
    result.outcome.existing = assessment.kind;
    switch (assessment.kind) {
    case ExistingKind::none:
        return produce_fresh(ctx, planned, dir, index, std::move(result));
    case ExistingKind::incomplete:
        return produce_update(ctx, planned, dir, index, assessment, std::move(result));
    case ExistingKind::complete:
    case ExistingKind::diverged:
    case ExistingKind::foreign:
        announce(*ctx.observer, result.outcome.path, assessment);
        report_left_alone(assessment, result);
        return result;
    case ExistingKind::unchecked: // assess never classifies a file as unchecked
        break;
    }
    return std::unexpected(internal_error("unexpected assessment of an existing file"));
}

/// --dry-run for one file: what a real run would do, decided from the central directory and the
/// manifest only (D11). Nothing is downloaded, written or created, and the inspection
/// descriptor is closed before this returns. `dir` is null when DIR does not exist.
[[nodiscard]] Result<FileResult> plan_file(const RunContext& ctx, const OutputDir* dir,
                                           const PlannedEpub& epub) {
    const DownloadOptions& options = *ctx.options;
    FileResult result{
        .outcome =
            EpubOutcome{
                .epub = epub,
                .path = output_path(options, epub),
                .status = EpubStatus::planned,
            },
        .kept = {},
    };
    EpubOutcome& outcome = result.outcome;
    bool replaces = false;
    if (dir != nullptr && options.overwrite) {
        const Result<bool> found = dir->exists(epub.file_name);
        if (!found) {
            return std::unexpected(found.error());
        }
        replaces = *found;
        outcome.existing = *found ? ExistingKind::unchecked : ExistingKind::none;
    } else if (dir != nullptr) {
        const Result<Assessment> assessed =
            assess(*dir, epub, options.groups, options.quality, /*real_run=*/false);
        if (!assessed) {
            return std::unexpected(assessed.error());
        }
        const Assessment& assessment = *assessed;
        outcome.existing = assessment.kind;
        announce(*ctx.observer, outcome.path, assessment);
        switch (assessment.kind) {
        case ExistingKind::none:
            break;
        case ExistingKind::complete:
        case ExistingKind::diverged:
        case ExistingKind::foreign:
            report_left_alone(assessment, result);
            return result;
        case ExistingKind::incomplete:
            report_update(assessment, outcome);
            if (epub.plan_error) {
                outcome.status = EpubStatus::failed;
                outcome.error = *epub.plan_error;
                return result;
            }
            outcome.pages = assessment.final_plan.planned_pages;
            outcome.replaced = true; // the update would replace the existing file
            if (assessment.decision) {
                result.kept = assessment.decision->kept;
            }
            return result;
        case ExistingKind::unchecked: // assess never classifies a file as unchecked
            return std::unexpected(internal_error("unexpected assessment of an existing file"));
        }
    }
    if (epub.plan_error) {
        outcome.status = EpubStatus::failed;
        outcome.error = *epub.plan_error;
    } else {
        outcome.pages = epub.planned_pages;
        // With --overwrite an existing target stays planned; `replaced` says the real run would
        // write over it (as `replaced` on a written outcome does).
        outcome.replaced = replaces;
    }
    return result;
}

/// --dry-run: every file is planned (or exists, or failed for a plan error). Nothing is
/// downloaded and nothing is created, not even DIR. With --overwrite an existing target is
/// planned with `replaced` set; without it an existing file is assessed as a real run would.
[[nodiscard]] Result<RunReport> dry_run(RunContext& ctx, const PlanResult& planned,
                                        RunReport report) {
    const DownloadOptions& options = *ctx.options;
    const Result<std::optional<OutputDir>> dir = OutputDir::open_if_exists(options.dir);
    if (!dir) {
        return std::unexpected(dir.error());
    }
    const OutputDir* const existing_dir = dir->has_value() ? &**dir : nullptr;
    for (const PlannedEpub& epub : planned.plan.epubs) {
        if (stop_now(ctx.stop)) {
            return std::unexpected(interrupted_error());
        }
        Result<FileResult> file = plan_file(ctx, existing_dir, epub);
        if (!file) {
            return std::unexpected(std::move(file.error()));
        }
        ctx.observer->on_epub_end(file->outcome);
        report.skipped = reconcile_skipped(std::move(report.skipped), file->kept, epub);
        report.epubs.push_back(std::move(file->outcome));
    }
    return report;
}

} // namespace

Result<RunReport> execute_phase(RunContext& ctx, const PlanResult& planned) {
    const DownloadOptions& options = *ctx.options;
    const Plan& plan = planned.plan;
    RunReport report{
        .dry_run = options.dry_run,
        .layout = plan.layout,
        .manga = plan.manga,
        .epubs = {},
        .skipped = plan.skipped,
    };
    if (plan.epubs.empty()) {
        return report; // exit 5 at the caller; DIR is left alone
    }
    report.epubs.reserve(plan.epubs.size());
    if (options.dry_run) {
        return dry_run(ctx, planned, std::move(report));
    }

    if (stop_now(ctx.stop)) {
        return std::unexpected(interrupted_error());
    }
    Result<OutputDir> dir = OutputDir::open(options.dir, /*create=*/true);
    if (!dir) {
        return std::unexpected(std::move(dir.error()));
    }
    for (std::size_t index = 0; index < plan.epubs.size(); ++index) {
        if (stop_now(ctx.stop)) {
            return std::unexpected(interrupted_error());
        }
        Result<FileResult> produced = produce(ctx, planned, *dir, index);
        if (!produced) {
            return std::unexpected(std::move(produced.error()));
        }
        ctx.observer->on_epub_end(produced->outcome);
        // `kept` names held releases the file now holds instead of planned ones.
        report.skipped =
            reconcile_skipped(std::move(report.skipped), produced->kept, plan.epubs[index]);
        report.epubs.push_back(std::move(produced->outcome));
    }
    return report;
}

} // namespace mangadex_dl::detail
