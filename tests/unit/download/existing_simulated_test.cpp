// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/download/existing_simulated_test.cpp: simulated_held (core-private,
// src/core/download/existing.{hpp,cpp}), the HeldBook a batch/mdlist dry run assumes for a file
// an earlier row of the same run planned: the manifest a real build would record, page entries
// named as the writer names them, no cover, the allocation caps, and classify_held deciding the
// recorded book itself as complete.

#include "download/existing.hpp" // core-private (src/core is on the test include path)
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/zip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace detail = mangadex_dl::detail;

/// The value of `optional`, which the caller has just REQUIREd to be engaged. An empty one (a
/// test bug) throws, which Catch2 reports; unlike REQUIRE, the check is visible to
/// bugprone-unchecked-optional-access.
template <class T> [[nodiscard]] const T& engaged(const std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

/// A planned chapter as the planner leaves it (no language or flags, so chapter_from round-trips).
[[nodiscard]] md::Chapter chapter(std::string id, std::string number, int pages) {
    return md::Chapter{
        .id = std::move(id),
        .volume = "1",
        .chapter = std::move(number),
        .title = "Title",
        .groups = {md::ScanGroup{.id = "33333333-3333-4333-8333-333333333333", .name = "Alpha"}},
        .pages = pages,
        .publish_at = "2024-01-01T00:00:00+00:00",
        .updated_at = "2024-01-02T00:00:00+00:00",
    };
}

/// A two-chapter planned book (3 and 4 pages) with a cover.
[[nodiscard]] md::PlannedEpub planned_book() {
    md::PlannedEpub book{
        .kind = md::EpubKind::volume,
        .file_name = "Clean Title - Vol. 01.epub",
        .display_title = "Clean Title Vol. 1",
        .volume = "1",
        .chapters = {chapter("11111111-0000-4000-8000-000000000001", "1", 3),
                     chapter("11111111-0000-4000-8000-000000000002", "2", 4)},
        .cover = md::Cover{.volume = "1", .file_name = "cover.jpg", .locale = "ja"},
        .identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:vol-1",
        .series_position = "1",
        .planned_pages = 7,
    };
    return book;
}

} // namespace

TEST_CASE("simulated_held: the manifest a real build of the planned book records",
          "[download][existing][simulated]") {
    const md::PlannedEpub book = planned_book();
    const md::HeldBook held = detail::simulated_held(book, md::ImageQuality::data_saver);

    CHECK(held.manifest.identifier == book.identifier);
    REQUIRE(held.manifest.chapters.size() == book.chapters.size());
    REQUIRE(held.pages.size() == book.chapters.size());
    for (std::size_t i = 0; i < book.chapters.size(); ++i) {
        const md::ManifestChapter& record = held.manifest.chapters[i];
        md::ManifestChapter expected =
            md::manifest_chapter_from(book.chapters[i], md::ImageQuality::data_saver);
        expected.images = record.images; // compared on their own below
        CHECK(record == expected);
        CHECK(record.images.size() == static_cast<std::size_t>(book.chapters[i].pages));
        REQUIRE(held.pages[i].size() == record.images.size());
        for (std::size_t page = 0; page < record.images.size(); ++page) {
            CHECK(held.pages[i][page].name == record.images[page]);
            // Only the name is set: the entry is never read.
            CHECK(held.pages[i][page].size == 0);
            CHECK(held.pages[i][page].data_offset == 0);
        }
    }
    // No cover, whatever the plan has.
    CHECK_FALSE(held.manifest.cover.has_value());
    CHECK_FALSE(held.cover.has_value());
    CHECK(held.manifest.opf_crc32 == 0);
}

TEST_CASE("simulated_held: page entries are named as the writer names them, across the book",
          "[download][existing][simulated]") {
    const md::HeldBook held = detail::simulated_held(planned_book(), md::ImageQuality::data);
    std::vector<std::string> names{};
    for (const md::ManifestChapter& record : held.manifest.chapters) {
        names.insert(names.end(), record.images.begin(), record.images.end());
    }
    REQUIRE(names.size() == 7);
    for (std::size_t index = 0; index < names.size(); ++index) {
        CHECK(names[index] == md::page_image_entry(index, md::MediaType::jpeg));
        CHECK(md::is_held_image_entry_name(names[index]));
    }
    CHECK(names.front() == "OEBPS/images/p00001.jpg");
    CHECK(names.back() == "OEBPS/images/p00007.jpg");
}

TEST_CASE("simulated_held: page counts, the held chapters and a negative count",
          "[download][existing][simulated]") {
    md::PlannedEpub book = planned_book();
    const md::HeldBook held = detail::simulated_held(book, md::ImageQuality::data);
    CHECK(detail::held_page_count(held) == 7);
    // held_chapters gives the planned chapters back (pages = the image count).
    CHECK(detail::held_chapters(held) == book.chapters);

    // A negative count holds no image (as the planner counts it: max(pages, 0)).
    book.chapters.push_back(chapter("11111111-0000-4000-8000-000000000003", "3", -2));
    const md::HeldBook negative = detail::simulated_held(book, md::ImageQuality::data);
    REQUIRE(negative.manifest.chapters.size() == 3);
    CHECK(negative.manifest.chapters[2].images.empty());
    CHECK(negative.pages[2].empty());
    CHECK(detail::held_page_count(negative) == 7);

    // An empty book holds nothing.
    const md::HeldBook empty = detail::simulated_held(md::PlannedEpub{}, md::ImageQuality::data);
    CHECK(empty.manifest.chapters.empty());
    CHECK(empty.pages.empty());
    CHECK(detail::held_page_count(empty) == 0);
}

TEST_CASE("simulated_held: no more images than a real file can hold",
          "[download][existing][simulated]") {
    // A planned book never exceeds either cap (the planner gives it a plan error first); the
    // caps only bound what is allocated for any other input.
    md::PlannedEpub book{};
    book.chapters.push_back(chapter("11111111-0000-4000-8000-000000000001", "1",
                                    static_cast<int>(md::max_pages_per_chapter) + 1));
    const md::HeldBook one = detail::simulated_held(book, md::ImageQuality::data);
    REQUIRE(one.pages.size() == 1);
    CHECK(one.manifest.chapters[0].images.size() == md::max_pages_per_chapter);
    CHECK(one.pages[0].size() == md::max_pages_per_chapter);

    // 14 full chapters ask for 70000 images; the book stops at zip_max_entries.
    book.chapters.assign(14, chapter("11111111-0000-4000-8000-000000000001", "1",
                                     static_cast<int>(md::max_pages_per_chapter)));
    const md::HeldBook many = detail::simulated_held(book, md::ImageQuality::data);
    CHECK(detail::held_page_count(many) == md::zip_max_entries);
    REQUIRE(many.pages.size() == 14);
    CHECK(many.pages[12].size() == md::max_pages_per_chapter);
    CHECK(many.pages[13].size() == md::zip_max_entries - (13 * md::max_pages_per_chapter));
}

TEST_CASE("simulated_held: the recorded book itself is complete",
          "[download][existing][simulated]") {
    const md::PlannedEpub book = planned_book();
    const detail::Assessment assessment = detail::classify_held(
        detail::simulated_held(book, md::ImageQuality::data), book, {}, md::ImageQuality::data);
    CHECK(assessment.kind == md::ExistingKind::complete);
    CHECK(assessment.final_plan == book);
    REQUIRE(assessment.decision.has_value());
    CHECK(engaged(assessment.decision).missing.empty());
    CHECK(engaged(assessment.decision).extra.empty());
    CHECK_FALSE(assessment.handle.fd.valid());
    REQUIRE(assessment.held.has_value());
    CHECK(detail::held_page_count(engaged(assessment.held)) == book.planned_pages);
}
