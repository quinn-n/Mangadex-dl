// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/epub/epub_manifest_test.cpp: EpubWriter::finish(cover, chapters) writes the book
// manifest (skip-existing plan T8, acceptance 3). Books are built into a MemorySink and read back
// with the independent tests/support/zip_reader.hpp; the manifest is parsed with parse_manifest
// and checked against the archive it describes.

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"
#include "support/mdx_fixture.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace zip = mangadex_dl::test::zip;
namespace mdx = mangadex_dl::test::mdx;

// ---- helpers -----------------------------------------------------------------------------------

[[nodiscard]] md::EpubMetadata sample_meta() {
    return md::EpubMetadata{
        .identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:vol-2",
        .title = "Clean Title Vol. 2",
        .language = "en",
        .creators = {"Author One"},
        .contributors = {"Group A"},
        .source = "https://mangadex.org/title/11111111-1111-4111-8111-111111111111",
        .modified = "2024-01-02T03:04:05Z",
        .series = "Clean Title",
        .series_position = "2",
        .direction = md::Direction::rtl,
    };
}

/// One page or cover image: the bytes and what the writer is told about them.
struct Image {
    std::string bytes{};
    md::ImageInfo info{};
};

[[nodiscard]] Image make_image(mdx::ImageKind kind, md::MediaType type, std::uint16_t width,
                               std::uint16_t height) {
    return Image{
        .bytes = mdx::image_bytes(mdx::PageImage{.kind = kind, .width = width, .height = height}),
        .info = md::ImageInfo{.type = type, .width = width, .height = height},
    };
}

[[nodiscard]] std::vector<Image> four_pages() {
    return {
        make_image(mdx::ImageKind::png, md::MediaType::png, 8, 12),
        make_image(mdx::ImageKind::jpeg_header, md::MediaType::jpeg, 600, 800),
        make_image(mdx::ImageKind::gif, md::MediaType::gif, 30, 40),
        make_image(mdx::ImageKind::webp_header, md::MediaType::webp, 1200, 900),
    };
}

[[nodiscard]] Image cover_image() {
    return make_image(mdx::ImageKind::jpeg_header, md::MediaType::jpeg, 700, 1000);
}

/// The two chapter records of the sample book (images left empty, as the caller passes them).
[[nodiscard]] md::ManifestChapter first_record() {
    return md::ManifestChapter{
        .id = "33333333-3333-4333-8333-333333333333",
        .volume = "2",
        .chapter = "3",
        .title = "Third",
        .groups = {{.id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", .name = "Group A"},
                   {.id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", .name = "Group B"}},
        .publish_at = "2024-01-01T00:00:00+00:00",
        .updated_at = "2024-01-02T00:00:00+00:00",
        .quality = md::ImageQuality::data,
        .images = {},
    };
}

[[nodiscard]] md::ManifestChapter second_record() {
    return md::ManifestChapter{
        .id = "44444444-4444-4444-8444-444444444444",
        .volume = std::nullopt,
        .chapter = std::nullopt,
        .title = "Oneshot \"extra\" & <more>",
        .groups = {},
        .publish_at = "2024-02-01T00:00:00+00:00",
        .updated_at = "2024-02-02T00:00:00+00:00",
        .quality = md::ImageQuality::data_saver,
        .images = {},
    };
}

/// The sample book's chapters over four_pages(): pages 1-3, then page 4.
[[nodiscard]] std::vector<md::EpubBookChapter> two_chapters() {
    return {
        {.record = first_record(), .first_page = 0, .page_count = 3},
        {.record = second_record(), .first_page = 3, .page_count = 1},
    };
}

/// What to put in a test book.
struct BookSpec {
    std::optional<Image> cover{};
    std::vector<Image> pages = four_pages();
};

/// How the book is finished: plain finish(), or finish(cover_file_name, chapters).
struct Finish {
    bool with_manifest = true;
    std::optional<std::string> cover_file_name{};
    std::vector<md::EpubBookChapter> chapters = two_chapters();
};

/// Begins the book and adds its cover and pages.
void add_content(md::EpubWriter& writer, const BookSpec& spec) {
    REQUIRE(writer.begin().has_value());
    if (spec.cover) {
        REQUIRE(writer.add_cover(zip::bytes_of(spec.cover->bytes), spec.cover->info).has_value());
    }
    writer.add_nav_entry({.label = "Ch. 3", .first_page = 0, .group = "Vol. 2"});
    writer.add_nav_entry({.label = "Oneshot", .first_page = 3, .group = std::nullopt});
    for (const Image& page : spec.pages) {
        REQUIRE(writer.add_page(zip::bytes_of(page.bytes), page.info).has_value());
    }
}

/// Builds the book into a fresh sink and returns the finished archive's bytes.
[[nodiscard]] std::vector<std::byte> build(const BookSpec& spec, Finish finish) {
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};
    add_content(writer, spec);
    // The call is made once, outside the assertion macro.
    const md::Result<void> finished =
        finish.with_manifest
            ? writer.finish(std::move(finish.cover_file_name), std::move(finish.chapters))
            : writer.finish();
    REQUIRE(finished.has_value());
    REQUIRE(archive.finish().has_value());
    return sink.bytes();
}

[[nodiscard]] zip::Archive read_back(const std::vector<std::byte>& bytes) {
    auto archive = zip::read_archive(bytes);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

/// The archive's last entry, which must be the manifest, parsed.
[[nodiscard]] md::BookManifest manifest_of(const zip::Archive& archive) {
    REQUIRE(!archive.entries.empty());
    const zip::Entry& last = archive.entries.back();
    REQUIRE(last.name == md::manifest_entry_name);
    auto parsed = md::parse_manifest(last.text());
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// A sink that accepts `budget` bytes and then fails every write.
class FailingSink final : public md::ByteSink {
public:
    explicit FailingSink(std::uint64_t budget) : budget_(budget) {}

    [[nodiscard]] md::Result<void> write(std::span<const std::byte> data) override {
        if (data.size() > budget_) {
            return std::unexpected(md::Error{.code = md::Errc::io, .message = "disk full"});
        }
        budget_ -= data.size();
        return {};
    }

private:
    std::uint64_t budget_ = 0;
};

} // namespace

// ---- entry names -------------------------------------------------------------------------------

TEST_CASE("epub manifest: page and cover entry names are 0-based held image names",
          "[epub][manifest]") {
    CHECK(md::page_image_entry(0, md::MediaType::jpeg) == "OEBPS/images/p00001.jpg");
    CHECK(md::page_image_entry(1, md::MediaType::png) == "OEBPS/images/p00002.png");
    CHECK(md::page_image_entry(41, md::MediaType::gif) == "OEBPS/images/p00042.gif");
    CHECK(md::page_image_entry(99998, md::MediaType::webp) == "OEBPS/images/p99999.webp");
    CHECK(md::cover_image_entry(md::MediaType::jpeg) == "OEBPS/images/cover.jpg");
    CHECK(md::cover_image_entry(md::MediaType::png) == "OEBPS/images/cover.png");
    CHECK(md::cover_image_entry(md::MediaType::gif) == "OEBPS/images/cover.gif");
    CHECK(md::cover_image_entry(md::MediaType::webp) == "OEBPS/images/cover.webp");
    for (const md::MediaType type :
         {md::MediaType::jpeg, md::MediaType::png, md::MediaType::gif, md::MediaType::webp}) {
        CHECK(md::is_held_image_entry_name(md::page_image_entry(0, type)));
        CHECK(md::is_held_image_entry_name(md::page_image_entry(99998, type)));
        CHECK(md::is_held_image_entry_name(md::cover_image_entry(type)));
    }
    CHECK(md::opf_entry_name == "OEBPS/content.opf");
}

// ---- the manifest (acceptance 3) ---------------------------------------------------------------

TEST_CASE("epub manifest: the last STORED entry, consistent with the archive", "[epub][manifest]") {
    const BookSpec spec{.cover = cover_image()};
    std::vector<md::EpubBookChapter> chapters = two_chapters();
    // Images handed in are replaced by the pages actually added.
    chapters[1].record.images = {"OEBPS/images/p00009.png"};
    const zip::Archive archive = read_back(
        build(spec, Finish{.cover_file_name = "cover-file.jpg", .chapters = std::move(chapters)}));

    // The manifest comes last, after toc.ncx, STORED with no extra field and flags 0.
    REQUIRE(archive.entries.size() >= 2);
    const zip::Entry& last = archive.entries.back();
    CHECK(last.name == md::manifest_entry_name);
    CHECK(last.method == 0);
    CHECK(last.flags == 0);
    CHECK(last.local_extra_length == 0);
    CHECK(last.central_extra_length == 0);
    CHECK(archive.entries[archive.entries.size() - 2].name == "OEBPS/toc.ncx");

    const md::BookManifest manifest = manifest_of(archive);

    // identifier == dc:identifier.
    const zip::Entry* opf = archive.find(md::opf_entry_name);
    REQUIRE(opf != nullptr);
    CHECK(manifest.identifier == sample_meta().identifier);
    CHECK(opf->text().contains(
        std::format(R"(<dc:identifier id="book-id">{}</dc:identifier>)", manifest.identifier)));

    // opf_crc32 is the CRC of the exact content.opf bytes (independent CRC and the CD's).
    CHECK(manifest.opf_crc32 == zip::reference_crc32(opf->data));
    CHECK(manifest.opf_crc32 == opf->crc32);

    // Every image and the cover name an entry in the archive, with the pages' own types.
    REQUIRE(manifest.cover.has_value());
    const md::ManifestCover cover = manifest.cover.value_or(md::ManifestCover{});
    CHECK(cover.file_name == "cover-file.jpg");
    CHECK(cover.entry == "OEBPS/images/cover.jpg");
    CHECK(archive.find(cover.entry) != nullptr);
    md::ManifestChapter first = first_record();
    first.images = {"OEBPS/images/p00001.png", "OEBPS/images/p00002.jpg",
                    "OEBPS/images/p00003.gif"};
    md::ManifestChapter second = second_record();
    second.images = {"OEBPS/images/p00004.webp"};
    CHECK(manifest.chapters == std::vector<md::ManifestChapter>{first, second});
    const std::vector<Image> pages = four_pages();
    std::size_t page = 0;
    for (const md::ManifestChapter& chapter : manifest.chapters) {
        for (const std::string& image : chapter.images) {
            INFO(image);
            const zip::Entry* entry = archive.find(image);
            REQUIRE(entry != nullptr);
            REQUIRE(page < pages.size());
            CHECK(entry->text() == pages[page].bytes);
            ++page;
        }
    }
    CHECK(page == pages.size());

    // The full record, field for field.
    const md::BookManifest expected{
        .identifier = sample_meta().identifier,
        .opf_crc32 = opf->crc32,
        .cover =
            md::ManifestCover{.file_name = "cover-file.jpg", .entry = "OEBPS/images/cover.jpg"},
        .chapters = {first, second},
    };
    CHECK(manifest == expected);
    // The stored text is exactly manifest_to_json of that record (compact, deterministic).
    CHECK(last.text() == md::manifest_to_json(expected));
}

TEST_CASE("epub manifest: the entries before it equal the plain finish() output",
          "[epub][manifest]") {
    // A prefix check: same entries (names, offsets, data), plus one trailing manifest entry.
    const BookSpec spec{.cover = cover_image()};
    const zip::Archive plain = read_back(build(spec, Finish{.with_manifest = false}));
    const zip::Archive with = read_back(build(spec, Finish{.cover_file_name = "c.jpg"}));
    REQUIRE(with.entries.size() == plain.entries.size() + 1);
    for (std::size_t i = 0; i < plain.entries.size(); ++i) {
        INFO(plain.entries[i].name);
        CHECK(with.entries[i].name == plain.entries[i].name);
        CHECK(with.entries[i].local_header_offset == plain.entries[i].local_header_offset);
        CHECK(with.entries[i].crc32 == plain.entries[i].crc32);
        const bool same_data = with.entries[i].data == plain.entries[i].data;
        CHECK(same_data);
    }
    CHECK(plain.find(md::manifest_entry_name) == nullptr);
}

TEST_CASE("epub manifest: cover is null without a cover or without a name", "[epub][manifest]") {
    SECTION("cover added, no name") {
        const zip::Archive archive =
            read_back(build(BookSpec{.cover = cover_image()}, Finish{.cover_file_name = {}}));
        CHECK(archive.find("OEBPS/images/cover.jpg") != nullptr);
        CHECK(!manifest_of(archive).cover.has_value());
        CHECK(archive.entries.back().text().contains(R"("cover":null)"));
    }
    SECTION("name given, no cover added") {
        const zip::Archive archive =
            read_back(build(BookSpec{}, Finish{.cover_file_name = "cover-file.jpg"}));
        CHECK(archive.find("OEBPS/images/cover.jpg") == nullptr);
        CHECK(!manifest_of(archive).cover.has_value());
        CHECK(archive.entries.back().text().contains(R"("cover":null)"));
    }
    SECTION("neither") {
        const zip::Archive archive = read_back(build(BookSpec{}, Finish{}));
        const md::BookManifest manifest = manifest_of(archive);
        CHECK(!manifest.cover.has_value());
        const zip::Entry* opf = archive.find(md::opf_entry_name);
        REQUIRE(opf != nullptr);
        CHECK(manifest.opf_crc32 == zip::reference_crc32(opf->data));
        CHECK(manifest.chapters.size() == 2);
    }
}

TEST_CASE("epub manifest: a single chapter covering every page", "[epub][manifest]") {
    const zip::Archive archive = read_back(
        build(BookSpec{},
              Finish{.chapters = {{.record = first_record(), .first_page = 0, .page_count = 4}}}));
    const md::BookManifest manifest = manifest_of(archive);
    REQUIRE(manifest.chapters.size() == 1);
    CHECK(manifest.chapters[0].images ==
          std::vector<std::string>{"OEBPS/images/p00001.png", "OEBPS/images/p00002.jpg",
                                   "OEBPS/images/p00003.gif", "OEBPS/images/p00004.webp"});
}

TEST_CASE("epub manifest: two builds of the same book are byte-identical", "[epub][manifest]") {
    const BookSpec spec{.cover = cover_image()};
    const std::vector<std::byte> first = build(spec, Finish{.cover_file_name = "c.jpg"});
    const std::vector<std::byte> second = build(spec, Finish{.cover_file_name = "c.jpg"});
    const bool identical = first == second;
    CHECK(identical);
    CHECK(!first.empty());
}

// ---- validation and call order -----------------------------------------------------------------

TEST_CASE("epub manifest: page ranges that do not tile the pages are rejected before writing",
          "[epub][manifest]") {
    constexpr std::size_t huge = std::numeric_limits<std::size_t>::max();
    const auto chapter = [](std::size_t first_page, std::size_t page_count) {
        return md::EpubBookChapter{
            .record = first_record(), .first_page = first_page, .page_count = page_count};
    };
    struct Case {
        std::string_view name{};
        std::vector<md::EpubBookChapter> chapters{};
        bool past_end = false; ///< must be rejected by the bounds check itself
    };
    const std::vector<Case> cases{
        {.name = "no chapters", .chapters = {}},
        {.name = "gap at the start", .chapters = {chapter(1, 3)}},
        {.name = "gap between chapters", .chapters = {chapter(0, 2), chapter(3, 1)}},
        {.name = "overlap", .chapters = {chapter(0, 3), chapter(2, 2)}},
        {.name = "out of order", .chapters = {chapter(3, 1), chapter(0, 3)}},
        {.name = "last page not covered", .chapters = {chapter(0, 3)}},
        {.name = "past the last page", .chapters = {chapter(0, 5)}, .past_end = true},
        {.name = "second chapter past the end",
         .chapters = {chapter(0, 3), chapter(3, 2)},
         .past_end = true},
        {.name = "starts past the end",
         .chapters = {chapter(0, 4), chapter(5, 1)},
         .past_end = true},
        {.name = "empty chapter", .chapters = {chapter(0, 4), chapter(4, 0)}},
        {.name = "empty first chapter", .chapters = {chapter(0, 0), chapter(0, 4)}},
        {.name = "first_page + page_count wraps",
         .chapters = {chapter(0, 3), chapter(3, huge)},
         .past_end = true},
        {.name = "huge first_page", .chapters = {chapter(huge, 1)}, .past_end = true},
        {.name = "huge both", .chapters = {chapter(huge, huge)}, .past_end = true},
        {.name = "page_count wraps back to the start",
         .chapters = {chapter(1, huge)},
         .past_end = true},
        // A naive `first_page + page_count > total` passes these (4 + MAX wraps to 3, 2 + MAX to
        // 1), the wrapped `next` is then re-tiled to 4, and pages past the end would be read.
        {.name = "wrap lands back on a valid tiling",
         .chapters = {chapter(0, 4), chapter(4, huge), chapter(3, 1)},
         .past_end = true},
        {.name = "wrap mid-book lands back on a valid tiling",
         .chapters = {chapter(0, 2), chapter(2, huge), chapter(1, 3)},
         .past_end = true},
    };

    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};
    add_content(writer, BookSpec{});
    const std::size_t entries = archive.entry_count();
    const std::uint64_t written = archive.bytes_written();
    for (const Case& tiling : cases) {
        INFO(tiling.name);
        const md::Result<void> result = writer.finish("cover-file.jpg", tiling.chapters);
        REQUIRE(!result.has_value());
        CHECK(result.error().code == md::Errc::failure);
        CHECK(result.error().message.starts_with("EPUB manifest chapter"));
        if (tiling.past_end) {
            CHECK(result.error().message.contains("covers pages past"));
        }
        CHECK(archive.entry_count() == entries);
        CHECK(archive.bytes_written() == written);
    }
    // Nothing was written, so the writer is still open and a valid tiling finishes the book.
    REQUIRE(writer.finish(std::nullopt, two_chapters()).has_value());
    REQUIRE(archive.finish().has_value());
    const zip::Archive book = read_back(sink.bytes());
    CHECK(manifest_of(book).chapters.size() == 2);
}

TEST_CASE("epub manifest: finish(cover, chapters) follows the call-order rules",
          "[epub][manifest]") {
    const Image page = make_image(mdx::ImageKind::png, md::MediaType::png, 8, 12);
    const auto bytes = zip::bytes_of(page.bytes);
    const auto failure = [](const md::Result<void>& result) {
        return !result.has_value() && result.error().code == md::Errc::failure;
    };
    const std::vector<md::EpubBookChapter> one{
        {.record = first_record(), .first_page = 0, .page_count = 1}};
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};

    CHECK(failure(writer.finish(std::nullopt, one))); // before begin()
    CHECK(archive.entry_count() == 0);
    REQUIRE(writer.begin().has_value());
    const md::Result<void> no_pages = writer.finish(std::nullopt, {});
    REQUIRE(!no_pages.has_value());
    CHECK(no_pages.error().code == md::Errc::failure);
    CHECK(no_pages.error().message == "EPUB has no pages");
    CHECK(archive.entry_count() == 2);

    REQUIRE(writer.add_page(bytes, page.info).has_value());
    REQUIRE(writer.finish(std::nullopt, one).has_value());
    const std::size_t entries = archive.entry_count();
    CHECK(entries == 2 + 2 + 3 + 1);
    CHECK(failure(writer.finish(std::nullopt, one))); // after finish
    CHECK(failure(writer.finish()));
    CHECK(archive.entry_count() == entries);

    {
        // After the plain finish(), the manifest overload is refused too.
        md::MemorySink other_sink;
        md::ZipWriter other{other_sink};
        md::EpubWriter plain{other, sample_meta()};
        REQUIRE(plain.begin().has_value());
        REQUIRE(plain.add_page(bytes, page.info).has_value());
        REQUIRE(plain.finish().has_value());
        CHECK(failure(plain.finish(std::nullopt, one)));
        CHECK(other.entry_count() == 2 + 2 + 3);
    }
}

TEST_CASE("epub manifest: a sink failure on the manifest poisons the writer", "[epub][manifest]") {
    // The exact size of everything before the manifest: the same book through the plain finish().
    std::uint64_t before_manifest = 0;
    {
        md::MemorySink sink;
        md::ZipWriter archive{sink};
        md::EpubWriter writer{archive, sample_meta()};
        add_content(writer, BookSpec{});
        REQUIRE(writer.finish().has_value());
        before_manifest = archive.bytes_written();
    }
    FailingSink sink{before_manifest};
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};
    add_content(writer, BookSpec{});
    const md::Result<void> result = writer.finish(std::nullopt, two_chapters());
    REQUIRE(!result.has_value());
    CHECK(result.error().code == md::Errc::io); // the sink's own error, unchanged
    CHECK(archive.bytes_written() == before_manifest);
    const md::Result<void> again = writer.finish(std::nullopt, two_chapters());
    REQUIRE(!again.has_value());
    CHECK(again.error().code == md::Errc::failure);
    CHECK(again.error().message.contains("earlier write failure"));
}
