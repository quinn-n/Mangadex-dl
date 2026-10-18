// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/epub/epub_test.cpp: fixed-layout EPUB generation (download plan T10, acceptance 6).
// Books are built into a MemorySink and read back with the independent
// tests/support/zip_reader.hpp; the XML is checked with string assertions plus a small
// well-formedness scanner.

#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"
#include "support/mdx_fixture.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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
        .identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:vol-3",
        .title = "Clean Title Vol. 3",
        .language = "en",
        .creators = {"Author One", "Artist Two"},
        .contributors = {"Group A", "Group B"},
        .source = "https://mangadex.org/title/11111111-1111-4111-8111-111111111111",
        .modified = "2024-01-02T03:04:05Z",
        .series = "Clean Title",
        .series_position = "3",
        .direction = md::Direction::ltr,
    };
}

/// One page or cover image: the bytes and what the writer is told about them.
struct Image {
    std::string bytes{};
    md::ImageInfo info{};
};

[[nodiscard]] Image make_image(mdx::ImageKind kind, std::uint16_t width, std::uint16_t height) {
    md::MediaType type = md::MediaType::png;
    switch (kind) {
    case mdx::ImageKind::png:
    case mdx::ImageKind::truncated_png:
    case mdx::ImageKind::garbage:
        type = md::MediaType::png;
        break;
    case mdx::ImageKind::gif:
        type = md::MediaType::gif;
        break;
    case mdx::ImageKind::jpeg_header:
        type = md::MediaType::jpeg;
        break;
    case mdx::ImageKind::webp_header:
        type = md::MediaType::webp;
        break;
    }
    return Image{
        .bytes = mdx::image_bytes(mdx::PageImage{.kind = kind, .width = width, .height = height}),
        .info = md::ImageInfo{.type = type, .width = width, .height = height},
    };
}

/// What to put in a test book.
struct BookSpec {
    md::EpubMetadata meta = sample_meta();
    std::optional<Image> cover{};
    std::vector<Image> pages{};
    std::vector<md::EpubNavEntry> nav{};
};

[[nodiscard]] std::vector<Image> four_pages() {
    return {
        make_image(mdx::ImageKind::png, 8, 12),
        make_image(mdx::ImageKind::jpeg_header, 600, 800),
        make_image(mdx::ImageKind::gif, 30, 40),
        make_image(mdx::ImageKind::webp_header, 1200, 900),
    };
}

/// Builds the book into a fresh sink and returns the finished archive's bytes.
[[nodiscard]] std::vector<std::byte> build(const BookSpec& spec) {
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, spec.meta};
    REQUIRE(writer.begin().has_value());
    if (spec.cover) {
        REQUIRE(writer.add_cover(zip::bytes_of(spec.cover->bytes), spec.cover->info).has_value());
    }
    for (const md::EpubNavEntry& entry : spec.nav) {
        writer.add_nav_entry(entry);
    }
    for (const Image& page : spec.pages) {
        REQUIRE(writer.add_page(zip::bytes_of(page.bytes), page.info).has_value());
    }
    REQUIRE(writer.finish().has_value());
    REQUIRE(archive.finish().has_value());
    return sink.bytes();
}

[[nodiscard]] zip::Archive read_back(const std::vector<std::byte>& bytes) {
    auto archive = zip::read_archive(bytes);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

[[nodiscard]] std::string entry_text(const zip::Archive& archive, std::string_view name) {
    const zip::Entry* entry = archive.find(name);
    INFO(name);
    REQUIRE(entry != nullptr);
    return entry->text();
}

/// UTF-8 for one BMP code point at or above U+0800. Bidi controls are built at run time: GCC's
/// -Wbidi-chars=any and clang-tidy's misc-misleading-bidirectional reject them in literals.
[[nodiscard]] std::string utf8_bmp(std::uint32_t code_point) {
    std::string out{};
    out += static_cast<char>(0xE0U | (code_point >> 12U));
    out += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (code_point & 0x3FU));
    return out;
}

/// Every explicit bidi embedding, override and isolate control, concatenated.
[[nodiscard]] std::string explicit_bidi_controls() {
    std::string out{};
    for (std::uint32_t c = 0x202AU; c <= 0x202EU; ++c) {
        out += utf8_bmp(c);
    }
    for (std::uint32_t c = 0x2066U; c <= 0x2069U; ++c) {
        out += utf8_bmp(c);
    }
    return out;
}

[[nodiscard]] std::size_t count_of(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

/// A deliberately small XML well-formedness scanner for the documents this writer produces:
/// balanced, properly nested tags with exactly one root, no raw '<' or '>' in character data or
/// attribute values, and every '&' starting one of the references xml_escape emits. Returns the
/// first problem, or nullopt.
[[nodiscard]] std::optional<std::string> xml_problem(std::string_view doc) {
    std::vector<std::string> open;
    std::size_t roots = 0;
    std::size_t pos = 0;
    while (pos < doc.size()) {
        const char c = doc[pos];
        if (c == '&') {
            const std::string_view rest = doc.substr(pos);
            const bool known = rest.starts_with("&amp;") || rest.starts_with("&lt;") ||
                               rest.starts_with("&gt;") || rest.starts_with("&quot;") ||
                               rest.starts_with("&#39;");
            if (!known) {
                return std::format("bare '&' at {}", pos);
            }
            ++pos;
            continue;
        }
        if (c == '>') {
            return std::format("raw '>' at {}", pos);
        }
        if (c != '<') {
            ++pos;
            continue;
        }
        const std::size_t close = doc.find('>', pos);
        if (close == std::string_view::npos) {
            return std::format("unterminated tag at {}", pos);
        }
        const std::string_view tag = doc.substr(pos + 1, close - pos - 1);
        pos = close + 1;
        if (tag.contains('<')) {
            return std::format("'<' inside a tag before {}", pos);
        }
        if (tag.starts_with('?') || tag.starts_with('!')) {
            continue; // XML declaration, DOCTYPE
        }
        if (tag.starts_with('/')) {
            const std::string_view name = tag.substr(1);
            if (open.empty() || open.back() != name) {
                return std::format("unexpected </{}>", name);
            }
            open.pop_back();
            continue;
        }
        const std::string_view name = tag.substr(0, tag.find_first_of(" \n/"));
        if (name.empty()) {
            return std::format("empty tag name before {}", pos);
        }
        if (open.empty()) {
            ++roots;
        }
        if (!tag.ends_with('/')) {
            open.emplace_back(name);
        }
    }
    if (!open.empty()) {
        return std::format("<{}> is never closed", open.back());
    }
    if (roots != 1) {
        return std::format("{} root elements", roots);
    }
    return std::nullopt;
}

void check_well_formed(const zip::Archive& archive) {
    for (const zip::Entry& entry : archive.entries) {
        const bool is_xml = entry.name.ends_with(".xml") || entry.name.ends_with(".xhtml") ||
                            entry.name.ends_with(".opf") || entry.name.ends_with(".ncx");
        if (!is_xml) {
            continue;
        }
        INFO(entry.name);
        const std::string text = entry.text();
        CHECK(text.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"));
        CHECK(xml_problem(text).value_or("") == "");
    }
}

/// A sink that accepts `budget` bytes and then fails every write.
class FailingSink final : public md::ByteSink {
public:
    explicit FailingSink(std::size_t budget) : budget_(budget) {}

    [[nodiscard]] md::Result<void> write(std::span<const std::byte> data) override {
        if (data.size() > budget_) {
            return std::unexpected(md::Error{.code = md::Errc::io, .message = "disk full"});
        }
        budget_ -= data.size();
        return {};
    }

private:
    std::size_t budget_ = 0;
};

} // namespace

// ---- archive structure (acceptance 6) ----------------------------------------------------------

TEST_CASE("epub: entry order, mimetype first and stored, container rootfile", "[epub]") {
    const std::vector<Image> pages = four_pages();
    const BookSpec spec{
        .cover = make_image(mdx::ImageKind::jpeg_header, 700, 1000),
        .pages = pages,
        .nav = {{.label = "Ch. 1", .first_page = 0}, {.label = "Ch. 2", .first_page = 2}},
    };
    const zip::Archive archive = read_back(build(spec));

    CHECK(archive.names() == std::vector<std::string>{
                                 "mimetype",
                                 "META-INF/container.xml",
                                 "OEBPS/images/cover.jpg",
                                 "OEBPS/cover.xhtml",
                                 "OEBPS/images/p00001.png",
                                 "OEBPS/pages/p00001.xhtml",
                                 "OEBPS/images/p00002.jpg",
                                 "OEBPS/pages/p00002.xhtml",
                                 "OEBPS/images/p00003.gif",
                                 "OEBPS/pages/p00003.xhtml",
                                 "OEBPS/images/p00004.webp",
                                 "OEBPS/pages/p00004.xhtml",
                                 "OEBPS/content.opf",
                                 "OEBPS/nav.xhtml",
                                 "OEBPS/toc.ncx",
                             });

    const zip::Entry& mimetype = archive.entries.front();
    CHECK(mimetype.name == "mimetype");
    CHECK(mimetype.local_header_offset == 0);
    CHECK(mimetype.method == 0);
    CHECK(mimetype.local_extra_length == 0);
    CHECK(mimetype.central_extra_length == 0);
    CHECK(mimetype.data_offset == 38); // 30-byte header + "mimetype": the OCF magic offset
    CHECK(mimetype.text() == "application/epub+zip");

    const std::string container = entry_text(archive, "META-INF/container.xml");
    CHECK(container.contains(
        R"(<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>)"));
    CHECK(container.contains(R"(xmlns="urn:oasis:names:tc:opendocument:xmlns:container")"));

    // Images are stored byte for byte.
    const zip::Entry* cover = archive.find("OEBPS/images/cover.jpg");
    REQUIRE(cover != nullptr);
    CHECK(cover->text() == spec.cover->bytes);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const zip::Entry& image = archive.entries[4 + (2 * i)];
        CHECK(image.text() == pages[i].bytes);
    }
    check_well_formed(archive);
}

TEST_CASE("epub: OPF metadata, fixed-layout properties and manifest", "[epub]") {
    const BookSpec spec{.pages = four_pages(), .nav = {{.label = "Ch. 1", .first_page = 0}}};
    const zip::Archive archive = read_back(build(spec));
    const std::string opf = entry_text(archive, "OEBPS/content.opf");

    CHECK(opf.contains(R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" )"
                       R"(unique-identifier="book-id" xml:lang="en">)"));
    CHECK(opf.contains(R"(<dc:identifier id="book-id">)"
                       "urn:mangadex:11111111-1111-4111-8111-111111111111:en:vol-3"
                       "</dc:identifier>"));
    CHECK(opf.contains(R"(<dc:title id="title">Clean Title Vol. 3</dc:title>)"));
    CHECK(opf.contains("<dc:language>en</dc:language>"));
    CHECK(opf.contains(R"(<dc:creator id="creator-1">Author One</dc:creator>)"));
    CHECK(opf.contains(R"(<dc:creator id="creator-2">Artist Two</dc:creator>)"));
    CHECK(opf.contains(R"(<dc:contributor id="contributor-1">Group A</dc:contributor>)"));
    CHECK(opf.contains(R"(<dc:contributor id="contributor-2">Group B</dc:contributor>)"));
    CHECK(opf.contains("<dc:source>https://mangadex.org/title/"
                       "11111111-1111-4111-8111-111111111111</dc:source>"));
    CHECK(opf.contains(R"(<meta property="dcterms:modified">2024-01-02T03:04:05Z</meta>)"));

    // Series and position (belongs-to-collection refined by collection-type and group-position).
    CHECK(opf.contains(R"(<meta property="belongs-to-collection" id="series">Clean Title</meta>)"));
    CHECK(opf.contains(R"(<meta refines="#series" property="collection-type">series</meta>)"));
    CHECK(opf.contains(R"(<meta refines="#series" property="group-position">3</meta>)"));

    // Fixed layout, and never the deprecated package-level viewport.
    CHECK(opf.contains(R"(<meta property="rendition:layout">pre-paginated</meta>)"));
    CHECK(opf.contains(R"(<meta property="rendition:orientation">auto</meta>)"));
    CHECK(opf.contains(R"(<meta property="rendition:spread">landscape</meta>)"));
    CHECK(!opf.contains("rendition:viewport"));

    // Manifest: nav, ncx, one image + one page per page, with magic-byte media types. The
    // mimetype file is never listed.
    CHECK(!opf.contains("mimetype"));
    CHECK(opf.contains(R"(<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" )"
                       R"(properties="nav"/>)"));
    CHECK(opf.contains(R"(<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>)"));
    CHECK(opf.contains(
        R"(<item id="img-p00002" href="images/p00002.jpg" media-type="image/jpeg"/>)"));
    CHECK(
        opf.contains(R"(<item id="img-p00003" href="images/p00003.gif" media-type="image/gif"/>)"));
    CHECK(opf.contains(
        R"(<item id="img-p00004" href="images/p00004.webp" media-type="image/webp"/>)"));
    CHECK(opf.contains(
        R"(<item id="p00004" href="pages/p00004.xhtml" media-type="application/xhtml+xml"/>)"));
    CHECK(count_of(opf, "<item ") == 2 + (2 * 4));

    // Spine: every page in order, NCX for EPUB 2 readers.
    CHECK(opf.contains(R"(<spine toc="ncx" page-progression-direction="ltr">)"));
    const std::size_t first = opf.find(R"(<itemref idref="p00001"/>)");
    const std::size_t last = opf.find(R"(<itemref idref="p00004"/>)");
    CHECK(first != std::string::npos);
    CHECK(last != std::string::npos);
    CHECK(first < last);
    CHECK(count_of(opf, "<itemref ") == 4);
}

TEST_CASE("epub: dcterms:modified has the CCYY-MM-DDThh:mm:ssZ shape", "[epub]") {
    const zip::Archive archive =
        read_back(build(BookSpec{.pages = {make_image(mdx::ImageKind::png, 8, 12)}}));
    const std::string opf = entry_text(archive, "OEBPS/content.opf");
    constexpr std::string_view open = R"(<meta property="dcterms:modified">)";
    const std::size_t at = opf.find(open);
    REQUIRE(at != std::string::npos);
    const std::size_t begin = at + open.size();
    const std::size_t end = opf.find('<', begin);
    REQUIRE(end != std::string::npos);
    CHECK_THAT(opf.substr(begin, end - begin),
               Catch::Matchers::Matches(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$)"));
    CHECK(count_of(opf, "dcterms:modified") == 1);
}

TEST_CASE("epub: each page document carries its image's viewport", "[epub]") {
    const std::vector<Image> pages = four_pages();
    const zip::Archive archive = read_back(build(BookSpec{.pages = pages}));
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const md::ImageInfo info = pages[i].info;
        INFO(i);
        const std::string page =
            entry_text(archive, std::format("OEBPS/pages/p{:05}.xhtml", i + 1));
        CHECK(page.contains(std::format(R"(<meta name="viewport" content="width={}, height={}"/>)",
                                        info.width, info.height)));
        CHECK(page.contains(std::format(R"(<img src="../images/p{:05}.{}" alt="" width="{}" )"
                                        R"(height="{}"/>)",
                                        i + 1, md::file_extension(info.type), info.width,
                                        info.height)));
        CHECK(page.contains(std::format("<title>Page {}</title>", i + 1)));
    }
}

TEST_CASE("epub: cover image vs first-page fallback", "[epub]") {
    SECTION("with a cover") {
        const zip::Archive archive = read_back(build(BookSpec{
            .cover = make_image(mdx::ImageKind::png, 50, 70),
            .pages = four_pages(),
        }));
        const std::string opf = entry_text(archive, "OEBPS/content.opf");
        CHECK(opf.contains(R"(<item id="cover-image" href="images/cover.png" )"
                           R"(media-type="image/png" properties="cover-image"/>)"));
        CHECK(opf.contains(
            R"(<item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>)"));
        CHECK(opf.contains(R"(<meta name="cover" content="cover-image"/>)"));
        CHECK(count_of(opf, R"(properties="cover-image")") == 1);
        // The cover page is the first spine item.
        const std::size_t cover_ref = opf.find(R"(<itemref idref="cover"/>)");
        REQUIRE(cover_ref != std::string::npos);
        CHECK(cover_ref < opf.find(R"(<itemref idref="p00001"/>)"));
        CHECK(count_of(opf, "<itemref ") == 5);

        const std::string cover_page = entry_text(archive, "OEBPS/cover.xhtml");
        CHECK(cover_page.contains(R"(<meta name="viewport" content="width=50, height=70"/>)"));
        CHECK(cover_page.contains(R"(<img src="images/cover.png")"));
    }
    SECTION("without a cover the first page image is the cover") {
        const zip::Archive archive = read_back(build(BookSpec{.pages = four_pages()}));
        CHECK(archive.find("OEBPS/cover.xhtml") == nullptr);
        const std::string opf = entry_text(archive, "OEBPS/content.opf");
        CHECK(opf.contains(R"(<item id="img-p00001" href="images/p00001.png" )"
                           R"(media-type="image/png" properties="cover-image"/>)"));
        CHECK(opf.contains(R"(<meta name="cover" content="img-p00001"/>)"));
        CHECK(count_of(opf, R"(properties="cover-image")") == 1);
        CHECK(!opf.contains(R"(idref="cover")"));
    }
}

TEST_CASE("epub: page progression follows the direction", "[epub]") {
    BookSpec spec{.pages = {make_image(mdx::ImageKind::png, 8, 12)}};
    SECTION("rtl") {
        spec.meta.direction = md::Direction::rtl;
        const std::string opf = entry_text(read_back(build(spec)), "OEBPS/content.opf");
        CHECK(opf.contains(R"(<spine toc="ncx" page-progression-direction="rtl">)"));
    }
    SECTION("ltr") {
        spec.meta.direction = md::Direction::ltr;
        const std::string opf = entry_text(read_back(build(spec)), "OEBPS/content.opf");
        CHECK(opf.contains(R"(<spine toc="ncx" page-progression-direction="ltr">)"));
    }
}

TEST_CASE("epub: series is optional, position only with a series", "[epub]") {
    BookSpec spec{.pages = {make_image(mdx::ImageKind::png, 8, 12)}};
    SECTION("no series") {
        spec.meta.series = "";
        const std::string opf = entry_text(read_back(build(spec)), "OEBPS/content.opf");
        CHECK(!opf.contains("belongs-to-collection"));
        CHECK(!opf.contains("group-position"));
    }
    SECTION("series without a position") {
        spec.meta.series_position.reset();
        const std::string opf = entry_text(read_back(build(spec)), "OEBPS/content.opf");
        CHECK(opf.contains(R"(property="belongs-to-collection")"));
        CHECK(opf.contains(R"(property="collection-type">series<)"));
        CHECK(!opf.contains("group-position"));
    }
}

TEST_CASE("epub: metadata text is escaped; blank and repeated people are skipped", "[epub]") {
    BookSpec spec{.pages = {make_image(mdx::ImageKind::png, 8, 12)}};
    spec.meta.title = "Tom & Jerry <\"Vol\"> 'x'\x01";
    spec.meta.creators = {"Same Person", "", "Same Person", " \t", "Other"};
    spec.meta.contributors = {"A&B"};
    spec.meta.series = "S<1>";
    spec.meta.source = "";
    const zip::Archive archive = read_back(build(spec));
    check_well_formed(archive);
    const std::string opf = entry_text(archive, "OEBPS/content.opf");
    CHECK(opf.contains(
        R"(<dc:title id="title">Tom &amp; Jerry &lt;&quot;Vol&quot;&gt; &#39;x&#39;</dc:title>)"));
    CHECK(opf.contains(R"(<dc:creator id="creator-1">Same Person</dc:creator>)"));
    CHECK(opf.contains(R"(<dc:creator id="creator-2">Other</dc:creator>)"));
    CHECK(count_of(opf, "<dc:creator ") == 2);
    CHECK(opf.contains(R"(<dc:contributor id="contributor-1">A&amp;B</dc:contributor>)"));
    CHECK(opf.contains(R"(id="series">S&lt;1&gt;</meta>)"));
    CHECK(!opf.contains("dc:source"));
}

TEST_CASE("epub: metadata and nav text drop explicit bidi controls, keep LRM/RLM", "[epub]") {
    const std::string controls = explicit_bidi_controls();
    const std::string rlm = utf8_bmp(0x200F);
    const std::string lrm = utf8_bmp(0x200E);
    BookSpec spec{.pages = four_pages()};
    spec.meta.title = "Ti" + controls + "tle" + rlm;
    spec.meta.creators = {utf8_bmp(0x202E) + "Author" + utf8_bmp(0x202C), controls};
    spec.meta.series = utf8_bmp(0x2067) + "Series" + utf8_bmp(0x2069);
    spec.nav = {{.label = "Ch." + controls + " 1" + lrm, .first_page = 0},
                {.label = controls, .first_page = 2}};
    const zip::Archive archive = read_back(build(spec));
    check_well_formed(archive);
    const std::string opf = entry_text(archive, "OEBPS/content.opf");
    CHECK(opf.contains("<dc:title id=\"title\">Title" + rlm + "</dc:title>"));
    CHECK(opf.contains(R"(<dc:creator id="creator-1">Author</dc:creator>)"));
    CHECK(count_of(opf, "<dc:creator ") == 1); // a creator of only controls is blank, skipped
    CHECK(opf.contains(R"(id="series">Series</meta>)"));
    const std::string nav = entry_text(archive, "OEBPS/nav.xhtml");
    CHECK(nav.contains("<li><a href=\"pages/p00001.xhtml\">Ch. 1" + lrm + "</a></li>"));
    CHECK(nav.contains(R"(<li><a href="pages/p00003.xhtml">Page 3</a></li>)"));
    const std::string ncx = entry_text(archive, "OEBPS/toc.ncx");
    CHECK(ncx.contains("<navLabel><text>Ch. 1" + lrm + "</text></navLabel>"));
    for (const zip::Entry& entry : archive.entries) {
        if (!entry.name.ends_with(".xhtml") && !entry.name.ends_with(".opf") &&
            !entry.name.ends_with(".ncx")) {
            continue; // images and the fixed container/mimetype entries
        }
        INFO(entry.name);
        const std::string text = entry.text();
        for (std::uint32_t c = 0x202AU; c <= 0x202EU; ++c) {
            CHECK(!text.contains(utf8_bmp(c)));
        }
        for (std::uint32_t c = 0x2066U; c <= 0x2069U; ++c) {
            CHECK(!text.contains(utf8_bmp(c)));
        }
    }
}

// ---- navigation ---------------------------------------------------------------------------------

TEST_CASE("epub: flat nav and NCX list chapters in order", "[epub]") {
    const BookSpec spec{
        .pages = four_pages(),
        .nav = {{.label = "Ch. 1", .first_page = 0},
                {.label = "Ch. 2 <Finale>", .first_page = 1},
                {.label = "Ch. 3", .first_page = 3}},
    };
    const zip::Archive archive = read_back(build(spec));
    check_well_formed(archive);

    const std::string nav = entry_text(archive, "OEBPS/nav.xhtml");
    CHECK(nav.contains(R"(<nav epub:type="toc" id="toc">)"));
    CHECK(nav.contains(R"(xmlns:epub="http://www.idpf.org/2007/ops")"));
    CHECK(nav.contains(R"(xml:lang="en")"));
    CHECK(nav.contains(R"(<li><a href="pages/p00001.xhtml">Ch. 1</a></li>)"));
    CHECK(nav.contains(R"(<li><a href="pages/p00002.xhtml">Ch. 2 &lt;Finale&gt;</a></li>)"));
    CHECK(nav.contains(R"(<li><a href="pages/p00004.xhtml">Ch. 3</a></li>)"));
    CHECK(count_of(nav, "<ol>") == 1);
    CHECK(nav.find("Ch. 1") < nav.find("Ch. 2"));
    CHECK(nav.find("Ch. 2") < nav.find("Ch. 3"));

    const std::string ncx = entry_text(archive, "OEBPS/toc.ncx");
    CHECK(ncx.contains(R"(<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1")"));
    CHECK(ncx.contains(R"(<meta name="dtb:uid" content="urn:mangadex:)"
                       R"(11111111-1111-4111-8111-111111111111:en:vol-3"/>)"));
    CHECK(ncx.contains(R"(<meta name="dtb:depth" content="1"/>)"));
    CHECK(ncx.contains("<docTitle><text>Clean Title Vol. 3</text></docTitle>"));
    CHECK(ncx.contains(R"(<navPoint id="navpoint-1" playOrder="1">)"));
    CHECK(ncx.contains(R"(<navPoint id="navpoint-3" playOrder="3">)"));
    CHECK(ncx.contains("<navLabel><text>Ch. 2 &lt;Finale&gt;</text></navLabel>"));
    CHECK(ncx.contains(R"(<content src="pages/p00004.xhtml"/>)"));
    CHECK(count_of(ncx, "<navPoint ") == 3);
}

TEST_CASE("epub: grouped nav entries nest under volume headings", "[epub]") {
    // Layout single: a loose chapter (no group), two chapters of Vol. 1, one of Vol. 2.
    std::vector<Image> pages = four_pages();
    pages.push_back(make_image(mdx::ImageKind::png, 8, 12));
    const BookSpec spec{
        .pages = pages,
        .nav = {{.label = "Ch. 1", .first_page = 0, .group = "Vol. 1"},
                {.label = "Ch. 2", .first_page = 1, .group = "Vol. 1"},
                {.label = "Ch. 3", .first_page = 2, .group = "Vol. 2"},
                {.label = "Ch. 10", .first_page = 4}},
    };
    const zip::Archive archive = read_back(build(spec));
    check_well_formed(archive);

    const std::string nav = entry_text(archive, "OEBPS/nav.xhtml");
    // Every nested <ol> sits inside an <li> that begins with an <a>, and none is empty.
    CHECK(nav.contains("<li><a href=\"pages/p00001.xhtml\">Vol. 1</a>\n"
                       "    <ol>\n"
                       "      <li><a href=\"pages/p00001.xhtml\">Ch. 1</a></li>\n"
                       "      <li><a href=\"pages/p00002.xhtml\">Ch. 2</a></li>\n"
                       "    </ol>\n"
                       "  </li>\n"));
    CHECK(nav.contains("<li><a href=\"pages/p00003.xhtml\">Vol. 2</a>\n"
                       "    <ol>\n"
                       "      <li><a href=\"pages/p00003.xhtml\">Ch. 3</a></li>\n"
                       "    </ol>\n"
                       "  </li>\n"));
    // An entry without a group is a leaf <li><a> at the top level.
    CHECK(nav.contains("\n  <li><a href=\"pages/p00005.xhtml\">Ch. 10</a></li>\n"));
    CHECK(count_of(nav, "<ol>") == 3);
    CHECK(count_of(nav, "<li>") == 6);
    CHECK(!nav.contains("<ol>\n</ol>"));
    CHECK(!nav.contains("<li>\n"));

    const std::string ncx = entry_text(archive, "OEBPS/toc.ncx");
    CHECK(ncx.contains(R"(<meta name="dtb:depth" content="2"/>)"));
    CHECK(count_of(ncx, "<navPoint ") == 6);
    // A volume heading and its first chapter share a target and so a playOrder.
    CHECK(ncx.contains("<navPoint id=\"navpoint-1\" playOrder=\"1\">\n"
                       "      <navLabel><text>Vol. 1</text></navLabel>\n"
                       "      <content src=\"pages/p00001.xhtml\"/>\n"
                       "      <navPoint id=\"navpoint-2\" playOrder=\"1\">\n"
                       "        <navLabel><text>Ch. 1</text></navLabel>\n"));
    CHECK(ncx.contains(R"(<navPoint id="navpoint-3" playOrder="2">)"));
    CHECK(ncx.contains(R"(<navPoint id="navpoint-4" playOrder="3">)"));
    CHECK(ncx.contains(R"(<navPoint id="navpoint-5" playOrder="3">)"));
    CHECK(ncx.contains(R"(<navPoint id="navpoint-6" playOrder="4">)"));
    CHECK(count_of(ncx, "</navPoint>") == 6);
}

TEST_CASE("epub: nav falls back to the title, labels never blank", "[epub]") {
    SECTION("no entries: one entry with the book title") {
        const zip::Archive archive =
            read_back(build(BookSpec{.pages = {make_image(mdx::ImageKind::png, 8, 12)}}));
        const std::string nav = entry_text(archive, "OEBPS/nav.xhtml");
        CHECK(nav.contains(R"(<li><a href="pages/p00001.xhtml">Clean Title Vol. 3</a></li>)"));
        CHECK(count_of(nav, "<li>") == 1);
        const std::string ncx = entry_text(archive, "OEBPS/toc.ncx");
        CHECK(count_of(ncx, "<navPoint ") == 1);
    }
    SECTION("blank or fully dropped labels become page numbers") {
        const BookSpec spec{
            .pages = four_pages(),
            .nav = {{.label = " ", .first_page = 0}, {.label = "\x01\xFF", .first_page = 2}},
        };
        const zip::Archive archive = read_back(build(spec));
        const std::string nav = entry_text(archive, "OEBPS/nav.xhtml");
        CHECK(nav.contains(R"(<li><a href="pages/p00001.xhtml">Page 1</a></li>)"));
        CHECK(nav.contains(R"(<li><a href="pages/p00003.xhtml">Page 3</a></li>)"));
        const std::string ncx = entry_text(archive, "OEBPS/toc.ncx");
        CHECK(ncx.contains("<navLabel><text>Page 3</text></navLabel>"));
    }
}

// ---- determinism
// ---------------------------------------------------------------------------------

TEST_CASE("epub: two builds of the same book are byte-identical", "[epub]") {
    const BookSpec spec{
        .cover = make_image(mdx::ImageKind::gif, 20, 30),
        .pages = four_pages(),
        .nav = {{.label = "Ch. 1", .first_page = 0, .group = "Vol. 1"},
                {.label = "Ch. 2", .first_page = 2, .group = "Vol. 1"}},
    };
    const std::vector<std::byte> first = build(spec);
    const std::vector<std::byte> second = build(spec);
    const bool identical = first == second;
    CHECK(identical);
    CHECK(!first.empty());
}

// ---- call order and validation -----------------------------------------------------------------

TEST_CASE("epub: calls out of order fail with Errc::failure", "[epub]") {
    const Image page = make_image(mdx::ImageKind::png, 8, 12);
    const auto bytes = zip::bytes_of(page.bytes);
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};

    const auto failure = [](const md::Result<void>& result) {
        return !result.has_value() && result.error().code == md::Errc::failure;
    };

    CHECK(failure(writer.add_page(bytes, page.info)));
    CHECK(failure(writer.add_cover(bytes, page.info)));
    CHECK(failure(writer.finish()));
    CHECK(archive.entry_count() == 0);

    REQUIRE(writer.begin().has_value());
    CHECK(failure(writer.begin()));
    CHECK(failure(writer.finish())); // no pages yet
    REQUIRE(writer.add_page(bytes, page.info).has_value());
    CHECK(failure(writer.add_cover(bytes, page.info))); // the cover must come first

    writer.add_nav_entry({.label = "Too far", .first_page = 1});
    const md::Result<void> past = writer.finish();
    REQUIRE(!past.has_value());
    CHECK(past.error().code == md::Errc::failure);
    CHECK(past.error().message.contains("page 2 of 1"));
}

TEST_CASE("epub: a second cover and post-finish calls are rejected", "[epub]") {
    const Image page = make_image(mdx::ImageKind::png, 8, 12);
    const auto bytes = zip::bytes_of(page.bytes);
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};
    REQUIRE(writer.begin().has_value());
    REQUIRE(writer.add_cover(bytes, page.info).has_value());
    CHECK(!writer.add_cover(bytes, page.info).has_value());
    REQUIRE(writer.add_page(bytes, page.info).has_value());
    REQUIRE(writer.finish().has_value());
    const std::size_t entries = archive.entry_count();
    CHECK(entries == 2 + 2 + 2 + 3);
    CHECK(!writer.finish().has_value());
    CHECK(!writer.add_page(bytes, page.info).has_value());
    CHECK(archive.entry_count() == entries);
    REQUIRE(archive.finish().has_value());
    CHECK(read_back(sink.bytes()).entries.size() == entries);
}

TEST_CASE("epub: begin() validates the archive and the metadata", "[epub]") {
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    SECTION("mimetype must be the archive's first entry") {
        const std::string data = "x";
        REQUIRE(archive.add_stored("first", zip::bytes_of(data)).has_value());
        md::EpubWriter writer{archive, sample_meta()};
        CHECK(!writer.begin().has_value());
    }
    SECTION("required metadata") {
        const auto with = [](void (*change)(md::EpubMetadata&)) {
            md::EpubMetadata meta = sample_meta();
            change(meta);
            return meta;
        };
        const std::vector<std::pair<std::string_view, md::EpubMetadata>> cases{
            {"no identifier", with([](md::EpubMetadata& m) { m.identifier = ""; })},
            {"blank title", with([](md::EpubMetadata& m) { m.title = "\x02 "; })},
            {"no language", with([](md::EpubMetadata& m) { m.language = ""; })},
            {"modified with an offset",
             with([](md::EpubMetadata& m) { m.modified = "2024-01-02T03:04:05+09:00"; })},
            {"modified with a fraction",
             with([](md::EpubMetadata& m) { m.modified = "2024-01-02T03:04:05.5Z"; })},
            {"modified empty", with([](md::EpubMetadata& m) { m.modified = ""; })},
        };
        for (const auto& [name, meta] : cases) {
            INFO(name);
            md::EpubWriter writer{archive, meta};
            const md::Result<void> result = writer.begin();
            REQUIRE(!result.has_value());
            CHECK(result.error().code == md::Errc::failure);
            CHECK(archive.entry_count() == 0);
        }
    }
}

TEST_CASE("epub: image info is validated before anything is written", "[epub]") {
    const Image page = make_image(mdx::ImageKind::png, 8, 12);
    md::MemorySink sink;
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};
    REQUIRE(writer.begin().has_value());
    const std::size_t entries = archive.entry_count();

    const std::uint32_t too_big = md::max_image_side + 1;
    const std::vector<md::ImageInfo> invalid{
        {.type = md::MediaType::png, .width = 0, .height = 12},
        {.type = md::MediaType::png, .width = 8, .height = 0},
        {.type = md::MediaType::png, .width = too_big, .height = 12},
        {.type = md::MediaType::png, .width = 8, .height = too_big},
    };
    for (const md::ImageInfo& info : invalid) {
        INFO(std::format("{}x{}", info.width, info.height));
        CHECK(!writer.add_page(zip::bytes_of(page.bytes), info).has_value());
        CHECK(!writer.add_cover(zip::bytes_of(page.bytes), info).has_value());
    }
    CHECK(!writer.add_page({}, page.info).has_value()); // empty image
    CHECK(archive.entry_count() == entries);
    // Still usable: nothing was written.
    CHECK(writer.add_page(zip::bytes_of(page.bytes), page.info).has_value());
    CHECK(writer.finish().has_value());
}

TEST_CASE("epub: a sink failure poisons the writer", "[epub]") {
    const Image page = make_image(mdx::ImageKind::png, 8, 12);
    FailingSink sink{200}; // mimetype + part of container.xml
    md::ZipWriter archive{sink};
    md::EpubWriter writer{archive, sample_meta()};
    const md::Result<void> begun = writer.begin();
    REQUIRE(!begun.has_value());
    CHECK(begun.error().code == md::Errc::io); // the sink's own error, unchanged
    const md::Result<void> after = writer.add_page(zip::bytes_of(page.bytes), page.info);
    REQUIRE(!after.has_value());
    CHECK(after.error().code == md::Errc::failure);
    CHECK(!writer.finish().has_value());
}

// ---- free functions
// ------------------------------------------------------------------------------

TEST_CASE("epub: container_xml and page_xhtml", "[epub]") {
    const std::string container = md::container_xml();
    CHECK(container.contains(R"(full-path="OEBPS/content.opf")"));
    CHECK(xml_problem(container).value_or("") == "");

    const std::string page = md::page_xhtml(
        "images/a&b.png", md::ImageInfo{.type = md::MediaType::png, .width = 640, .height = 960},
        "Title <1>");
    CHECK(page.contains(R"(<meta name="viewport" content="width=640, height=960"/>)"));
    CHECK(page.contains(R"(src="images/a&amp;b.png")"));
    CHECK(page.contains("<title>Title &lt;1&gt;</title>"));
    CHECK(page.contains("<!DOCTYPE html>"));
    CHECK(xml_problem(page).value_or("") == "");
}

TEST_CASE("epub: xml_escape escapes markup and drops illegal characters", "[epub][xml]") {
    struct Row {
        std::string_view input;
        std::string_view expected;
    };
    const std::vector<Row> rows{
        {.input = "", .expected = ""},
        {.input = "plain text", .expected = "plain text"},
        {.input = "a&b<c>d\"e'f", .expected = "a&amp;b&lt;c&gt;d&quot;e&#39;f"},
        {.input = "]]>", .expected = "]]&gt;"},
        {.input = "&amp;", .expected = "&amp;amp;"},
        // C0 controls: only TAB, LF and CR are XML characters. DEL and C1 controls are legal.
        {.input = "a\x01"
                  "b\tc\nd\re\x1F"
                  "f\x7F",
         .expected = "ab\tc\nd\ref\x7F"},
        {.input = std::string_view{"a\0b", 3}, .expected = "ab"},
        {.input = "\xC2\x85", .expected = "\xC2\x85"},
        // Non-characters U+FFFE and U+FFFF are dropped; U+FFFD and U+E000 are kept.
        {.input = "x\xEF\xBF\xBEy\xEF\xBF\xBFz", .expected = "xyz"},
        {.input = "\xEF\xBF\xBD\xEE\x80\x80", .expected = "\xEF\xBF\xBD\xEE\x80\x80"},
        // Lone surrogates (CESU-style encodings) are invalid UTF-8 and dropped.
        {.input = "a\xED\xA0\x80"
                  "b\xED\xBF\xBF"
                  "c",
         .expected = "abc"},
        {.input = "\xED\x9F\xBF", .expected = "\xED\x9F\xBF"}, // U+D7FF
        // Overlong forms, bytes that never occur, stray continuations, truncation.
        {.input = "\xC0\xAF\xC1\xBF\xE0\x80\xAF\xF0\x80\x80\xAF", .expected = ""},
        {.input = "\xF5\x80\x80\x80\xFE\xFF", .expected = ""},
        {.input = "a\x80\xBF"
                  "b",
         .expected = "ab"},
        {.input = "\xE3\x81"
                  "A",
         .expected = "A"},
        {.input = "end\xE3\x81", .expected = "end"},
        {.input = "\xF0\x9F\x98", .expected = ""},
        // Above U+10FFFF dropped; U+10FFFF and astral characters kept.
        {.input = "\xF4\x90\x80\x80", .expected = ""},
        {.input = "\xF4\x8F\xBF\xBF\xF0\x9F\x98\x80",
         .expected = "\xF4\x8F\xBF\xBF\xF0\x9F\x98\x80"},
        {.input = "進撃の巨人 & 「タイトル」", .expected = "進撃の巨人 &amp; 「タイトル」"},
    };
    for (const Row& row : rows) {
        INFO(std::format("input bytes: {}", row.input.size()));
        CHECK(md::xml_escape(row.input) == row.expected);
    }
}

TEST_CASE("epub: xml_escape drops explicit bidi controls and keeps the marks", "[epub][xml]") {
    for (const std::uint32_t c :
         {0x202AU, 0x202BU, 0x202CU, 0x202DU, 0x202EU, 0x2066U, 0x2067U, 0x2068U, 0x2069U}) {
        INFO(std::format("U+{:04X}", c));
        CHECK(md::xml_escape("a" + utf8_bmp(c) + "&b") == "a&amp;b");
    }
    CHECK(md::xml_escape(explicit_bidi_controls()).empty());
    // LRM/RLM and the neighbours of both ranges are ordinary text.
    for (const std::uint32_t c : {0x200EU, 0x200FU, 0x2029U, 0x202FU, 0x2065U, 0x206AU}) {
        INFO(std::format("U+{:04X}", c));
        const std::string text = "x" + utf8_bmp(c) + "y";
        CHECK(md::xml_escape(text) == text);
    }
}

TEST_CASE("epub: format_modified converts offsets to UTC", "[epub][time]") {
    struct Row {
        std::string_view input;
        std::string_view expected;
    };
    const std::vector<Row> rows{
        // +09:00 crossing back over a day, a month and a year boundary.
        {.input = "2024-01-01T05:00:00+09:00", .expected = "2023-12-31T20:00:00Z"},
        // A negative offset crossing forward.
        {.input = "2023-12-31T22:30:00-02:00", .expected = "2024-01-01T00:30:00Z"},
        {.input = "2024-01-02T03:04:05+00:00", .expected = "2024-01-02T03:04:05Z"},
        {.input = "2024-01-02T03:04:05-00:00", .expected = "2024-01-02T03:04:05Z"},
        {.input = "2024-01-02T03:04:05Z", .expected = "2024-01-02T03:04:05Z"},
        // Leap years.
        {.input = "2024-03-01T01:00:00+02:00", .expected = "2024-02-29T23:00:00Z"},
        {.input = "2023-03-01T01:00:00+02:00", .expected = "2023-02-28T23:00:00Z"},
        {.input = "2000-02-28T23:00:00-01:00", .expected = "2000-02-29T00:00:00Z"},
        {.input = "2020-06-15T12:00:00+05:30", .expected = "2020-06-15T06:30:00Z"},
        // Fractional seconds are truncated.
        {.input = "2021-04-19T21:45:59.999+00:00", .expected = "2021-04-19T21:45:59Z"},
        {.input = "2021-04-19T21:45:59.1Z", .expected = "2021-04-19T21:45:59Z"},
        {.input = "0000-01-01T00:00:00Z", .expected = "0000-01-01T00:00:00Z"},
        {.input = "9999-12-31T23:59:59Z", .expected = "9999-12-31T23:59:59Z"},
    };
    for (const Row& row : rows) {
        INFO(row.input);
        const md::Result<std::string> result = md::format_modified(row.input);
        REQUIRE(result.has_value());
        CHECK(*result == row.expected);
        CHECK_THAT(*result, Catch::Matchers::Matches(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$)"));
    }
}

TEST_CASE("epub: format_modified rejects anything else", "[epub][time]") {
    const std::vector<std::string_view> rejects{
        "",
        "2024-01-02",
        "2024-01-02T03:04:05",            // no offset
        "2024-13-01T00:00:00Z",           // month 13
        "2024-00-01T00:00:00Z",           // month 0
        "2023-02-29T00:00:00Z",           // not a leap year
        "2024-04-31T00:00:00Z",           // April has 30 days
        "2024-01-02T24:00:00Z",           // hour 24
        "2024-01-02T23:60:00Z",           // minute 60
        "2024-01-02T23:59:60Z",           // leap second
        "2024-01-02 03:04:05Z",           // space separator
        "2024-01-02t03:04:05z",           // lowercase
        "2024-01-02T03:04:05+0900",       // basic-format offset
        "2024-01-02T03:04:05+09",         // hour-only offset
        "2024-01-02T03:04:05+24:00",      // offset hour out of range
        "2024-01-02T03:04:05+09:60",      // offset minute out of range
        "2024-01-02T03:04:05+09:00x",     // trailing junk
        "2024-01-02T03:04:05Zx",          // trailing junk
        "2024-01-02T03:04:05.+09:00",     // empty fraction
        "2024-01-02T03:04:05.123",        // fraction without an offset
        "2024-1-02T03:04:05Z",            // one-digit month
        "-024-01-02T03:04:05Z",           // sign in the year
        "2024-01-02T03:04:05\uFF0B09:00", // a full-width plus sign (U+FF0B)
        "9999-12-31T23:00:00-02:00",      // year 10000 in UTC
        "0000-01-01T00:30:00+01:00",      // year -1 in UTC
    };
    for (const std::string_view input : rejects) {
        INFO(input);
        const md::Result<std::string> result = md::format_modified(input);
        REQUIRE(!result.has_value());
        CHECK(result.error().code == md::Errc::failure);
        if (!input.empty()) {
            CHECK(!result.error().message.contains(input)); // untrusted input is never echoed
        }
    }
}
