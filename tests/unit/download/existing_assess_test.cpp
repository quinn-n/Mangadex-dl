// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/download/existing_assess_test.cpp: the core-private assessment of an existing file
// and the update pre-pass (src/core/download/existing.{hpp,cpp}; skip-existing plan T10).
// - assess / assess_handle on real files in a temporary DIR: absent, not regular, unreadable,
//   foreign, complete, incomplete and diverged, and which assessments keep the descriptor open
//   (only a real run's incomplete file, V5); a moved-from handle is refused, never read.
// - prepass over a fake source: page infos, cover reuse, and every fault falling back (D9).
// - the warning and -v line texts, and announce.
// Books come from tests/support/held_book_builder.hpp (ZipWriter + manifest_to_json).

#include "download/existing.hpp" // core-private (src/core is on the test include path)
#include "fs/errno_text.hpp"
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/update_plan.hpp"
#include "support/fake_read_source.hpp"
#include "support/held_book_builder.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_forge.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
namespace detail = mangadex_dl::detail;
namespace held = mangadex_dl::test::held;
namespace forge = mangadex_dl::test::forge;
using md::test::FakeFault;
using md::test::FakeReadSource;

constexpr std::string_view book_name = "book.epub";

/// Records warnings and -v lines.
class Recorder final : public md::DownloadObserver {
public:
    void on_plan(const md::Plan& /*plan*/) override {}
    void on_epub_begin(const md::PlannedEpub& /*epub*/, std::size_t /*index*/,
                       std::size_t /*total*/) override {}
    void on_pages(std::size_t /*done*/, std::size_t /*total*/) override {}
    void on_epub_end(const md::EpubOutcome& /*outcome*/) override {}
    void on_warning(std::string_view message) override { warnings.emplace_back(message); }
    void on_verbose(int level, std::string_view message) override {
        verbose.emplace_back(level, std::string{message});
    }

    std::vector<std::string> warnings{};
    std::vector<std::pair<int, std::string>> verbose{};
};

/// The planned file a sample book was "written from": its chapters (with a language, as the
/// planner's have), its cover and identifier.
[[nodiscard]] md::PlannedEpub planned_for(const md::BookManifest& manifest) {
    md::PlannedEpub planned{
        .kind = md::EpubKind::volume,
        .file_name = std::string{book_name},
        .display_title = "Sample Vol. 1",
        .volume = "1",
        .identifier = manifest.identifier,
    };
    for (const md::ManifestChapter& record : manifest.chapters) {
        md::Chapter chapter = md::chapter_from(record);
        chapter.language = "en";
        planned.planned_pages += static_cast<std::size_t>(chapter.pages);
        planned.chapters.push_back(std::move(chapter));
    }
    if (manifest.cover) {
        planned.cover = md::Cover{.volume = "1", .file_name = manifest.cover->file_name};
    }
    return planned;
}

/// A chapter the sample book does not hold: vol 1 ch 3, two pages.
[[nodiscard]] md::Chapter new_chapter() {
    return md::Chapter{
        .id = held::sample_uuid(3),
        .volume = "1",
        .chapter = "3",
        .title = "Chapter 3",
        .language = "en",
        .groups = {md::ScanGroup{.id = held::sample_uuid(100), .name = "Group A"}},
        .pages = 2,
    };
}

[[nodiscard]] forge::Bytes sample_bytes(const held::BookParts& parts = held::sample_book()) {
    auto bytes = held::assemble(parts);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// A temporary DIR, opened as an OutputDir.
class Dir {
public:
    Dir() : tmp_(make()), dir_(open(tmp_.path())) {}

    [[nodiscard]] std::filesystem::path path(std::string_view name = book_name) const {
        return tmp_.path() / name;
    }
    [[nodiscard]] const md::OutputDir& dir() const noexcept { return dir_; }

    void write(std::span<const std::byte> bytes, std::string_view name = book_name) const {
        REQUIRE(forge::write_file(path(name), bytes));
    }

private:
    [[nodiscard]] static md::test::TempDir make() {
        auto tmp = md::test::TempDir::create("mangadex-dl-assess");
        REQUIRE(tmp.has_value());
        return std::move(*tmp);
    }
    [[nodiscard]] static md::OutputDir open(const std::filesystem::path& path) {
        auto dir = md::OutputDir::open(path, /*create=*/false);
        REQUIRE(dir.has_value());
        return std::move(*dir);
    }

    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    md::OutputDir dir_;     ///< ditto
};

[[nodiscard]] detail::Assessment assess_ok(const Dir& dir, const md::PlannedEpub& planned,
                                           bool real_run, std::vector<std::string> groups = {}) {
    md::Result<detail::Assessment> assessed =
        detail::assess(dir.dir(), planned, groups, md::ImageQuality::data, real_run);
    INFO((assessed ? std::string{} : assessed.error().message));
    REQUIRE(assessed.has_value());
    return std::move(*assessed);
}

/// The sample book inspected through a fake source (no descriptor), then classified.
[[nodiscard]] detail::Assessment classified(const forge::Bytes& bytes,
                                            const md::PlannedEpub& planned) {
    FakeReadSource source{bytes};
    std::expected<md::HeldBook, md::InspectFailure> book =
        md::inspect_existing(source, planned.identifier);
    REQUIRE(book.has_value());
    return detail::classify_held(std::move(*book), planned, {}, md::ImageQuality::data);
}

/// The ImageInfo of the fixture image `kind` at the sample book's page size.
[[nodiscard]] md::ImageInfo info_of(held::ImageKind kind) {
    const forge::Bytes bytes = held::image_data(kind);
    const md::Result<md::ImageInfo> info = md::sniff_image(bytes);
    REQUIRE(info.has_value());
    return *info;
}

/// The value of `optional`, which the caller has just REQUIREd to be engaged. An empty one (a
/// test bug) throws, which Catch2 reports; unlike REQUIRE, the check is visible to
/// bugprone-unchecked-optional-access.
template <class T> [[nodiscard]] T& engaged(std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

template <class T> [[nodiscard]] const T& engaged(const std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

} // namespace

// ---- assess: what is under the name -------------------------------------------------------------

TEST_CASE("download existing: assess: an absent name is none and holds nothing",
          "[download][existing]") {
    const Dir dir;
    const md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    const detail::Assessment assessment = assess_ok(dir, planned, true);
    CHECK(assessment.kind == md::ExistingKind::none);
    CHECK_FALSE(assessment.handle.fd.valid());
    CHECK_FALSE(assessment.held.has_value());
    CHECK_FALSE(assessment.decision.has_value());
    CHECK_FALSE(assessment.foreign.has_value());
}

TEST_CASE("download existing: assess: a directory, a FIFO and a symlink are foreign, not regular",
          "[download][existing]") {
    const Dir dir;
    const md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    SECTION("directory") { REQUIRE(std::filesystem::create_directory(dir.path())); }
    SECTION("FIFO") { REQUIRE(::mkfifo(dir.path().c_str(), 0600) == 0); }
    SECTION("symlink to a valid book") {
        dir.write(sample_bytes(), "real.epub");
        std::filesystem::create_symlink("real.epub", dir.path());
    }
    const detail::Assessment assessment = assess_ok(dir, planned, true);
    CHECK(assessment.kind == md::ExistingKind::foreign);
    REQUIRE(assessment.foreign.has_value());
    CHECK(engaged(assessment.foreign).reason == md::ForeignReason::not_regular);
    CHECK_FALSE(assessment.handle.fd.valid());
}

TEST_CASE("download existing: assess: a file that cannot be read is foreign with its errno",
          "[download][existing]") {
    if (::geteuid() == 0) {
        SKIP("root reads a mode-000 file");
    }
    const Dir dir;
    const md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    dir.write(sample_bytes());
    std::filesystem::permissions(dir.path(), std::filesystem::perms::none);
    const detail::Assessment assessment = assess_ok(dir, planned, true);
    CHECK(assessment.kind == md::ExistingKind::foreign);
    REQUIRE(assessment.foreign.has_value());
    CHECK(engaged(assessment.foreign) ==
          md::InspectFailure{.reason = md::ForeignReason::unreadable, .error = EACCES});
}

TEST_CASE("download existing: assess: foreign files keep their reason and close the descriptor",
          "[download][existing]") {
    const Dir dir;
    const held::BookParts parts = held::sample_book();
    const md::PlannedEpub planned = planned_for(parts.manifest);
    md::ForeignReason expected = md::ForeignReason::not_ours;
    SECTION("random bytes") {
        dir.write(forge::text_bytes("this is not a zip archive at all, just text"));
    }
    SECTION("no manifest") {
        held::BookParts bare = parts;
        bare.with_manifest = false;
        dir.write(sample_bytes(bare));
    }
    SECTION("another book's identifier") {
        held::BookParts other = parts;
        other.manifest.identifier = "urn:mangadex:11111111-0001-4000-8000-000000000001:fr:v1";
        dir.write(sample_bytes(other));
        expected = md::ForeignReason::other_book;
    }
    SECTION("content.opf changed by another program") {
        held::BookParts edited = parts;
        held::member(edited, md::opf_entry_name).data = forge::text_bytes("<package/>");
        dir.write(sample_bytes(edited));
        expected = md::ForeignReason::changed_by_other_program;
    }
    for (const bool real_run : {true, false}) {
        INFO("real run: " << real_run);
        const detail::Assessment assessment = assess_ok(dir, planned, real_run);
        CHECK(assessment.kind == md::ExistingKind::foreign);
        REQUIRE(assessment.foreign.has_value());
        CHECK(engaged(assessment.foreign).reason == expected);
        CHECK(engaged(assessment.foreign).error == 0);
        CHECK_FALSE(assessment.held.has_value());
        CHECK_FALSE(assessment.handle.fd.valid());
    }
}

TEST_CASE("download existing: assess: a complete file is left alone and its descriptor closed",
          "[download][existing]") {
    const Dir dir;
    const held::BookParts parts = held::sample_book();
    dir.write(sample_bytes(parts));
    const md::PlannedEpub planned = planned_for(parts.manifest);
    for (const bool real_run : {true, false}) {
        INFO("real run: " << real_run);
        const detail::Assessment assessment = assess_ok(dir, planned, real_run);
        CHECK(assessment.kind == md::ExistingKind::complete);
        CHECK_FALSE(assessment.handle.fd.valid());
        REQUIRE(assessment.held.has_value());
        CHECK(engaged(assessment.held).manifest == parts.manifest);
        REQUIRE(assessment.decision.has_value());
        CHECK(engaged(assessment.decision).state == md::UpdateState::complete);
        CHECK(assessment.final_plan == md::final_epub(planned, engaged(assessment.decision)));
        CHECK(detail::held_page_count(engaged(assessment.held)) == 4);
    }
}

TEST_CASE("download existing: assess: only a real run's incomplete file keeps its descriptor open",
          "[download][existing]") {
    const Dir dir;
    const held::BookParts parts = held::sample_book();
    dir.write(sample_bytes(parts));
    md::PlannedEpub planned = planned_for(parts.manifest);
    planned.chapters.push_back(new_chapter());

    const detail::Assessment real = assess_ok(dir, planned, true);
    CHECK(real.kind == md::ExistingKind::incomplete);
    REQUIRE(real.handle.fd.valid());
    CHECK(real.handle.status == md::ExistingOpen::opened);
    struct stat st{};
    REQUIRE(::fstat(real.handle.fd.get(), &st) == 0);
    CHECK(st.st_ino == real.handle.identity.inode);
    REQUIRE(real.decision.has_value());
    CHECK(md::new_chapter_ids(engaged(real.decision)) ==
          std::vector<std::string>{held::sample_uuid(3)});
    CHECK(real.final_plan.chapters.size() == 3);
    CHECK(real.final_plan.planned_pages == 6); // 4 held + 2 planned

    const detail::Assessment dry = assess_ok(dir, planned, false);
    CHECK(dry.kind == md::ExistingKind::incomplete);
    CHECK_FALSE(dry.handle.fd.valid());
}

TEST_CASE("download existing: assess: missing and extra chapters make a file diverged",
          "[download][existing]") {
    const Dir dir;
    const held::BookParts parts = held::sample_book();
    dir.write(sample_bytes(parts));
    md::PlannedEpub planned = planned_for(parts.manifest);
    planned.chapters.pop_back(); // the file holds ch 2, which the plan no longer lists
    planned.chapters.push_back(new_chapter());
    const detail::Assessment assessment = assess_ok(dir, planned, true);
    CHECK(assessment.kind == md::ExistingKind::diverged);
    CHECK_FALSE(assessment.handle.fd.valid());
    REQUIRE(assessment.held.has_value());
    const std::vector<md::Chapter> chapters = detail::held_chapters(engaged(assessment.held));
    REQUIRE(chapters.size() == 2);
    CHECK(chapters[1].id == held::sample_uuid(2));
    CHECK(chapters[1].pages == 2);
}

TEST_CASE("download existing: assess: --group re-picks a better-ranked release matched by number",
          "[download][existing]") {
    const Dir dir;
    const held::BookParts parts = held::sample_book();
    dir.write(sample_bytes(parts));
    md::PlannedEpub planned = planned_for(parts.manifest);
    planned.chapters[0].id = held::sample_uuid(41); // another release of ch 1 ...
    planned.chapters[0].groups = {md::ScanGroup{.id = held::sample_uuid(101), .name = "Group B"}};

    const detail::Assessment kept = assess_ok(dir, planned, true);
    CHECK(kept.kind == md::ExistingKind::complete); // ... kept without --group
    REQUIRE(kept.decision.has_value());
    REQUIRE(engaged(kept.decision).kept.size() == 1);

    const detail::Assessment repicked = assess_ok(dir, planned, true, {"Group B"});
    CHECK(repicked.kind == md::ExistingKind::incomplete);
    CHECK(repicked.handle.fd.valid());
    REQUIRE(repicked.decision.has_value());
    REQUIRE(engaged(repicked.decision).repicked.size() == 1);
    CHECK(md::new_chapter_ids(engaged(repicked.decision)) ==
          std::vector<std::string>{held::sample_uuid(41)});
}

TEST_CASE(
    "download existing: assess_handle refuses a moved-from handle instead of reading descriptor -1",
    "[download][existing]") {
    const Dir dir;
    const held::BookParts parts = held::sample_book();
    dir.write(sample_bytes(parts));
    md::PlannedEpub planned = planned_for(parts.manifest);
    planned.chapters.push_back(new_chapter());

    md::Result<md::ExistingHandle> opened = dir.dir().open_existing(book_name);
    REQUIRE(opened.has_value());
    REQUIRE(opened->status == md::ExistingOpen::opened);
    md::ExistingHandle owner = std::move(*opened);
    REQUIRE(owner.fd.valid());
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what this test checks
    CHECK(opened->status == md::ExistingOpen::opened); // still says opened ...
    CHECK_FALSE(opened->fd.valid());                   // ... with descriptor -1

    const md::Result<detail::Assessment> refused = detail::assess_handle(
        std::move(*opened), planned, {}, md::ImageQuality::data, /*real_run=*/true);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == md::Errc::failure);
    CHECK(refused.error().message.contains("internal error"));

    // The handle that owns the descriptor is assessed normally, and keeps it open.
    const md::Result<detail::Assessment> assessed = detail::assess_handle(
        std::move(owner), planned, {}, md::ImageQuality::data, /*real_run=*/true);
    REQUIRE(assessed.has_value());
    CHECK(assessed->kind == md::ExistingKind::incomplete);
    CHECK(assessed->handle.fd.valid());
}

TEST_CASE("download existing: readable_size rejects a negative size", "[download][existing]") {
    CHECK(detail::readable_size(md::FileIdentity{.size = 42}) == std::optional<std::uint64_t>{42});
    CHECK(detail::readable_size(md::FileIdentity{.size = 0}) == std::optional<std::uint64_t>{0});
    CHECK_FALSE(detail::readable_size(md::FileIdentity{.size = -1}).has_value());
}

// ---- prepass ------------------------------------------------------------------------------------
//
// The sample book: ch 1 = PNG, GIF; ch 2 = JPEG, WebP; a PNG cover. The plan adds ch 3, so the
// final list is [reuse ch 1, reuse ch 2, download ch 3] and the pre-pass reads, in order:
// #0 ch 1 p1, #1 ch 1 p2, #2 ch 2 p1, #3 ch 2 p2, #4 the cover.

TEST_CASE("download existing: prepass records every reused page and the cover",
          "[download][existing][prepass]") {
    const forge::Bytes bytes = sample_bytes();
    md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    planned.chapters.push_back(new_chapter());
    detail::Assessment assessment = classified(bytes, planned);
    REQUIRE(assessment.kind == md::ExistingKind::incomplete);
    const md::PlannedEpub before = assessment.final_plan;

    FakeReadSource source{bytes};
    Recorder observer;
    std::stop_source stop{};
    const md::Result<void> ok = detail::prepass(source, assessment, planned, md::ImageQuality::data,
                                                "out/b", observer, stop);
    REQUIRE(ok.has_value());
    CHECK(source.reads() == 5);
    REQUIRE(assessment.page_info.size() == 3);
    CHECK(assessment.page_info[0] ==
          std::vector{info_of(held::ImageKind::png), info_of(held::ImageKind::gif)});
    CHECK(assessment.page_info[1] == std::vector{info_of(held::ImageKind::jpeg_header),
                                                 info_of(held::ImageKind::webp_header)});
    CHECK(assessment.page_info[2].empty()); // downloaded
    CHECK(assessment.reuse_cover);
    REQUIRE(assessment.cover_info.has_value());
    CHECK(engaged(assessment.cover_info).type == md::MediaType::png);
    CHECK(assessment.final_plan == before);
    CHECK(observer.warnings.empty());
}

TEST_CASE("download existing: prepass falls back to downloading a chapter with any page fault (D9)",
          "[download][existing][prepass]") {
    const forge::Bytes bytes = sample_bytes();
    md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    planned.chapters.push_back(new_chapter());
    detail::Assessment assessment = classified(bytes, planned);
    REQUIRE(assessment.kind == md::ExistingKind::incomplete);

    FakeReadSource source{bytes};
    // Every fault hits ch 1 page 2 (read #1): a read fault or a content fault.
    const std::string_view fault = GENERATE("io", "short read", "crc");
    INFO("fault: " << fault);
    if (fault == "io") {
        source.inject({.at_read = 1, .kind = FakeFault::io, .error = EIO});
    } else if (fault == "short read") {
        source.inject({.at_read = 1, .kind = FakeFault::zero});
    } else {
        // Flip the last byte of ch 1 page 2 once read #0 has returned.
        const md::HeldEntry& page = engaged(assessment.held).pages[0][1];
        source.mutate({.after_read = 0,
                       .offset = page.data_offset + page.size - 1,
                       .value = std::byte{0x5A}});
    }
    Recorder observer;
    std::stop_source stop{};
    const md::Result<void> ok = detail::prepass(source, assessment, planned, md::ImageQuality::data,
                                                "out/b", observer, stop);
    REQUIRE(ok.has_value());
    REQUIRE(observer.warnings.size() == 1);
    CHECK(observer.warnings[0] ==
          "out/b: Ch. 1 in the existing file is damaged; downloading it again");
    REQUIRE(assessment.decision.has_value());
    CHECK(engaged(assessment.decision).chapters[0].source == md::ChapterSource::download);
    CHECK(engaged(assessment.decision).chapters[0].quality == md::ImageQuality::data);
    CHECK(md::new_chapter_ids(engaged(assessment.decision)) ==
          std::vector<std::string>{held::sample_uuid(1), held::sample_uuid(3)});
    CHECK(md::reused_chapter_ids(engaged(assessment.decision)) ==
          std::vector<std::string>{held::sample_uuid(2)});
    CHECK(assessment.page_info[0].empty());
    CHECK(assessment.page_info[1].size() == 2);
    CHECK(assessment.final_plan == md::final_epub(planned, engaged(assessment.decision)));
    CHECK(assessment.final_plan.planned_pages == 2 + 2 + 2); // ch 1 planned + ch 2 held + ch 3
    CHECK(assessment.reuse_cover);
}

TEST_CASE("download existing: prepass drops a kept release that is damaged",
          "[download][existing][prepass]") {
    const forge::Bytes bytes = sample_bytes();
    md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    planned.chapters[1].id = held::sample_uuid(42); // the plan picked another release of ch 2
    planned.chapters.push_back(new_chapter());
    detail::Assessment assessment = classified(bytes, planned);
    REQUIRE(assessment.kind == md::ExistingKind::incomplete);
    REQUIRE(engaged(assessment.decision).kept.size() == 1);

    FakeReadSource source{bytes};
    source.inject({.at_read = 3, .kind = FakeFault::io, .error = EIO}); // ch 2 page 2
    Recorder observer;
    std::stop_source stop{};
    REQUIRE(detail::prepass(source, assessment, planned, md::ImageQuality::data_saver, "p",
                            observer, stop)
                .has_value());
    CHECK(engaged(assessment.decision).kept.empty());
    CHECK(engaged(assessment.decision).chapters[1].chapter.id == held::sample_uuid(42));
    CHECK(engaged(assessment.decision).chapters[1].quality == md::ImageQuality::data_saver);
    CHECK(observer.warnings ==
          std::vector<std::string>{"p: Ch. 2 in the existing file is damaged; downloading it "
                                   "again"});
}

TEST_CASE(
    "download existing: prepass reuses the cover only when it is the planned one and reads back",
    "[download][existing][prepass]") {
    const forge::Bytes bytes = sample_bytes();
    md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    planned.chapters.push_back(new_chapter());
    detail::Assessment assessment = classified(bytes, planned);
    FakeReadSource source{bytes};
    const std::string_view variant = GENERATE("another cover file", "no cover", "damaged cover");
    INFO("variant: " << variant);
    std::size_t expected_reads = 4; // the held cover is not even read
    if (variant == "another cover file") {
        engaged(planned.cover).file_name = "cover-new.png";
    } else if (variant == "no cover") {
        planned.cover.reset();
    } else {
        source.inject({.at_read = 4, .kind = FakeFault::zero});
        expected_reads = 5;
    }
    Recorder observer;
    std::stop_source stop{};
    REQUIRE(
        detail::prepass(source, assessment, planned, md::ImageQuality::data, "p", observer, stop)
            .has_value());
    CHECK(source.reads() == expected_reads);
    CHECK_FALSE(assessment.reuse_cover);
    CHECK_FALSE(assessment.cover_info.has_value());
    CHECK(observer.warnings.empty()); // a cover is simply downloaded instead
}

TEST_CASE("download existing: prepass stops at a page when the run is stopped",
          "[download][existing][prepass]") {
    const forge::Bytes bytes = sample_bytes();
    md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    planned.chapters.push_back(new_chapter());
    detail::Assessment assessment = classified(bytes, planned);
    FakeReadSource source{bytes};
    Recorder observer;
    std::stop_source stop{};
    static_cast<void>(stop.request_stop());
    const md::Result<void> ok =
        detail::prepass(source, assessment, planned, md::ImageQuality::data, "p", observer, stop);
    REQUIRE_FALSE(ok.has_value());
    CHECK(ok.error().code == md::Errc::interrupted);
    CHECK(source.reads() == 0);
}

TEST_CASE("download existing: prepass refuses an assessment that is not an update",
          "[download][existing][prepass]") {
    const forge::Bytes bytes = sample_bytes();
    const md::PlannedEpub planned = planned_for(held::sample_book().manifest);
    detail::Assessment complete = classified(bytes, planned);
    REQUIRE(complete.kind == md::ExistingKind::complete);
    detail::Assessment empty{};
    FakeReadSource source{bytes};
    Recorder observer;
    std::stop_source stop{};
    for (detail::Assessment* assessment : {&complete, &empty}) {
        const md::Result<void> ok = detail::prepass(source, *assessment, planned,
                                                    md::ImageQuality::data, "p", observer, stop);
        REQUIRE_FALSE(ok.has_value());
        CHECK(ok.error().code == md::Errc::failure);
    }
    CHECK(source.reads() == 0);
}

// ---- messages -----------------------------------------------------------------------------------

TEST_CASE("download existing: foreign warnings give the fixed reason and remedy, errno text only "
          "when unreadable",
          "[download][existing][messages]") {
    using md::ForeignReason;
    CHECK(detail::foreign_warning("out/a.epub",
                                  {.reason = ForeignReason::not_regular, .error = 40}) ==
          "out/a.epub: left alone: not a regular file; rename or remove it, or use another DIR");
    CHECK(detail::foreign_warning("out/a.epub",
                                  {.reason = ForeignReason::unreadable, .error = EACCES}) ==
          "out/a.epub: left alone: cannot be read (" + md::errno_text(EACCES) +
              "); rename or remove it, or use another DIR");
    CHECK(detail::foreign_warning("a", {.reason = ForeignReason::not_ours}) ==
          "a: left alone: not an EPUB made by mangadex-dl; use another DIR or --title");
    CHECK(detail::foreign_warning("a", {.reason = ForeignReason::other_book}) ==
          "a: left alone: holds a different book; use another DIR or --title");
    CHECK(detail::foreign_warning("a", {.reason = ForeignReason::changed_by_other_program}) ==
          "a: left alone: changed by another program; pass --overwrite to replace it");
    CHECK(detail::foreign_warning("a", {.reason = ForeignReason::newer_format}) ==
          "a: left alone: made by a newer mangadex-dl; pass --overwrite to replace it");
    CHECK(detail::foreign_warning("a", {.reason = ForeignReason::changed_during_update}) ==
          "a: left alone: changed while it was being updated; pass --overwrite to replace it");
}

namespace {

/// A final-list entry for the planned chapter `planned` (Vol. 2 Ch. `chapter`).
[[nodiscard]] md::FinalChapter final_entry(std::string chapter, md::ChapterSource source,
                                           std::size_t planned = 0) {
    return md::FinalChapter{
        .chapter = md::Chapter{.id = "x", .volume = "2", .chapter = std::move(chapter)},
        .source = source,
        .planned = planned,
    };
}

/// A held manifest record for Vol. 2 Ch. `chapter`.
[[nodiscard]] md::ManifestChapter held_record(std::string chapter) {
    return md::ManifestChapter{.id = "h", .volume = "2", .chapter = std::move(chapter)};
}

} // namespace

TEST_CASE("download existing: the diverged warning counts and names chapters, singular and plural",
          "[download][existing][messages]") {
    md::UpdateDecision decision{.state = md::UpdateState::diverged,
                                .chapters = {final_entry("4", md::ChapterSource::download, 3)},
                                .missing = {3},
                                .extra = {1}};
    const std::vector<md::ManifestChapter> held{
        held_record("1"), held_record("3"), held_record("5"), held_record("6"),
        held_record("7"), held_record("8"), held_record("9"), held_record("10")};
    // One unlisted chapter: "without it".
    CHECK(detail::diverged_warning("out/v.epub", decision, held) ==
          "out/v.epub: MangaDex lists 1 chapter this file lacks (Ch. 4), but the file holds 1 "
          "chapter MangaDex no longer lists (Ch. 3); left it alone (pass --overwrite to rebuild "
          "it without it)");
    // More than one: "without them".
    decision.chapters = {final_entry("2", md::ChapterSource::download, 1),
                         final_entry("2.5", md::ChapterSource::download, 2)};
    decision.missing = {1, 2};
    decision.extra = {0, 1, 2};
    CHECK(detail::diverged_warning("v", decision, held) ==
          "v: MangaDex lists 2 chapters this file lacks (Ch. 2, Ch. 2.5), but the file holds 3 "
          "chapters MangaDex no longer lists (Ch. 1, Ch. 3, Ch. 5); left it alone (pass "
          "--overwrite to rebuild it without them)");
}

TEST_CASE("download existing: the diverged warning shows at most 5 labels per side",
          "[download][existing][messages]") {
    md::UpdateDecision decision{.state = md::UpdateState::diverged};
    for (std::size_t n = 0; n < 7; ++n) {
        decision.chapters.push_back(
            final_entry(std::to_string(n + 11), md::ChapterSource::download, n));
        decision.missing.push_back(n);
    }
    std::vector<md::ManifestChapter> held{};
    for (int n = 1; n <= 8; ++n) {
        held.push_back(held_record(std::to_string(n)));
        decision.extra.push_back(held.size() - 1);
    }
    CHECK(detail::diverged_warning("v", decision, held) ==
          "v: MangaDex lists 7 chapters this file lacks (Ch. 11, Ch. 12, Ch. 13, Ch. 14, Ch. 15 "
          "and 2 more), but the file holds 8 chapters MangaDex no longer lists (Ch. 1, Ch. 2, Ch. "
          "3, Ch. 4, Ch. 5 and 3 more); left it alone (pass --overwrite to rebuild it without "
          "them)");
}

TEST_CASE("download existing: the diverged warning only counts what it cannot name",
          "[download][existing][messages]") {
    // A side whose chapters do not all resolve (no final-list entry, a held index out of range)
    // is counted without a list; the other side keeps its labels.
    md::UpdateDecision decision{.state = md::UpdateState::diverged,
                                .chapters = {final_entry("4", md::ChapterSource::download, 0)},
                                .missing = {0, 5},
                                .extra = {0}};
    const std::vector<md::ManifestChapter> held{held_record("3")};
    CHECK(detail::diverged_warning("v", decision, held) ==
          "v: MangaDex lists 2 chapters this file lacks, but the file holds 1 chapter MangaDex no "
          "longer lists (Ch. 3); left it alone (pass --overwrite to rebuild it without it)");
    decision.missing = {0};
    decision.extra = {0, 1};
    CHECK(detail::diverged_warning("v", decision, held) ==
          "v: MangaDex lists 1 chapter this file lacks (Ch. 4), but the file holds 2 chapters "
          "MangaDex no longer lists; left it alone (pass --overwrite to rebuild it without them)");
    CHECK(detail::diverged_warning("v", decision, {}) ==
          "v: MangaDex lists 1 chapter this file lacks (Ch. 4), but the file holds 2 chapters "
          "MangaDex no longer lists; left it alone (pass --overwrite to rebuild it without them)");
}

TEST_CASE("download existing: kept, re-pick and updating lines", "[download][existing][messages]") {
    CHECK(detail::kept_line("out/v.epub",
                            {.held_id = "h", .planned_id = "p", .key_label = "Vol. 1 Ch. 1"}) ==
          "out/v.epub: Vol. 1 Ch. 1: kept the release already in the file (h) instead of p");
    CHECK(detail::kept_line("v", {.held_id = "h", .planned_id = "p", .key_label = ""}) ==
          "v: kept the release already in the file (h) instead of p");

    CHECK(detail::repick_line("v", {.held_id = "h",
                                    .planned_id = "p",
                                    .held_group = "Clean Scans",
                                    .planned_group = "Bravo Scans",
                                    .key_label = "Vol. 1 Ch. 2"}) ==
          "v: Vol. 1 Ch. 2: replacing the release in the file (Clean Scans) with the release by "
          "Bravo Scans (--group)");
    // Group names may be empty (a release without groups).
    CHECK(detail::repick_line("v", {.held_id = "h", .planned_id = "p", .key_label = "Ch. 7"}) ==
          "v: Ch. 7: replacing the release in the file (no group) with the planned release "
          "(--group)");

    md::UpdateDecision decision{.state = md::UpdateState::incomplete};
    decision.chapters = {final_entry("3", md::ChapterSource::reuse),
                         final_entry("4", md::ChapterSource::download)};
    CHECK(detail::updating_line("out/v.epub", decision) ==
          "updating out/v.epub: downloading 1 chapter (Ch. 4), reusing 1");
    decision.chapters = {final_entry("1", md::ChapterSource::reuse),
                         final_entry("2", md::ChapterSource::reuse),
                         final_entry("3", md::ChapterSource::download),
                         final_entry("4", md::ChapterSource::download)};
    CHECK(detail::updating_line("v", decision) ==
          "updating v: downloading 2 chapters (Ch. 3, Ch. 4), reusing 2");
    decision.chapters.clear();
    for (int n = 1; n <= 7; ++n) {
        decision.chapters.push_back(final_entry(std::to_string(n), md::ChapterSource::download));
    }
    CHECK(detail::updating_line("v", decision) ==
          "updating v: downloading 7 chapters (Ch. 1, Ch. 2, Ch. 3, Ch. 4, Ch. 5 and 2 more), "
          "reusing 0");
    // Nothing to download (never for an incomplete file): counts only.
    decision.chapters = {final_entry("1", md::ChapterSource::reuse)};
    CHECK(detail::updating_line("v", decision) == "updating v: downloading 0 chapters, reusing 1");
}

TEST_CASE("download existing: announce reports what each assessment found",
          "[download][existing][messages]") {
    Recorder observer;
    detail::Assessment assessment{};
    SECTION("none says nothing") {
        detail::announce(observer, "p", assessment);
        CHECK(observer.warnings.empty());
        CHECK(observer.verbose.empty());
    }
    SECTION("foreign warns") {
        assessment.kind = md::ExistingKind::foreign;
        assessment.foreign = md::InspectFailure{.reason = md::ForeignReason::not_ours};
        detail::announce(observer, "p", assessment);
        CHECK(observer.warnings ==
              std::vector<std::string>{
                  "p: left alone: not an EPUB made by mangadex-dl; use another DIR or --title"});
        CHECK(observer.verbose.empty());
    }
    SECTION("diverged warns, naming the held chapters") {
        assessment.kind = md::ExistingKind::diverged;
        assessment.decision =
            md::UpdateDecision{.chapters = {final_entry("4", md::ChapterSource::download, 0)},
                               .missing = {0},
                               .extra = {1, 2}};
        assessment.held = md::HeldBook{};
        assessment.held->manifest.chapters = {held_record("1"), held_record("2"), held_record("3")};
        detail::announce(observer, "p", assessment);
        CHECK(observer.warnings ==
              std::vector<std::string>{
                  "p: MangaDex lists 1 chapter this file lacks (Ch. 4), but the file holds 2 "
                  "chapters MangaDex no longer lists (Ch. 2, Ch. 3); left it alone (pass "
                  "--overwrite to rebuild it without them)"});
        CHECK(observer.verbose.empty());
    }
    SECTION("diverged without the held book still warns, with counts only for that side") {
        assessment.kind = md::ExistingKind::diverged;
        assessment.decision = md::UpdateDecision{.missing = {0}, .extra = {1, 2}};
        detail::announce(observer, "p", assessment);
        CHECK(observer.warnings ==
              std::vector<std::string>{
                  "p: MangaDex lists 1 chapter this file lacks, but the file holds 2 chapters "
                  "MangaDex no longer lists; left it alone (pass --overwrite to rebuild it "
                  "without them)"});
        CHECK(observer.verbose.empty());
    }
    SECTION("complete and incomplete list kept and re-picked releases at -v") {
        assessment.kind = GENERATE(md::ExistingKind::complete, md::ExistingKind::incomplete);
        assessment.decision = md::UpdateDecision{
            .kept = {md::KeptRelease{.held_id = "h", .planned_id = "p", .key_label = "Ch. 1"}},
            .repicked = {md::Repick{.held_id = "h2",
                                    .planned_id = "p2",
                                    .held_group = "A",
                                    .planned_group = "B",
                                    .key_label = "Ch. 2"}},
        };
        detail::announce(observer, "f", assessment);
        CHECK(observer.warnings.empty());
        CHECK(observer.verbose ==
              std::vector<std::pair<int, std::string>>{
                  {1, "f: Ch. 1: kept the release already in the file (h) instead of p"},
                  {1, "f: Ch. 2: replacing the release in the file (A) with the release by B "
                      "(--group)"}});
    }
}
