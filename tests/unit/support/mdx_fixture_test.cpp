// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/support/mdx_fixture_test.cpp: the test-support fixture builder itself
// (tests/support/mdx_fixture.hpp) and the RAII temp dir (tests/support/temp_dir.hpp). Every other
// tier trusts these, so they get their own checks: JSON shapes, real decodable PNG/GIF bytes
// (verified by small independent decoders here), sniffable JPEG/WebP headers and the on-disk tree.

#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace mdx = mangadex_dl::test::mdx;
using mangadex_dl::test::TempDir;

[[nodiscard]] std::uint32_t u8(std::string_view s, std::size_t at) {
    return static_cast<std::uint8_t>(s.at(at));
}

[[nodiscard]] std::uint32_t le16(std::string_view s, std::size_t at) {
    return u8(s, at) | (u8(s, at + 1) << 8U);
}

[[nodiscard]] std::uint32_t le24(std::string_view s, std::size_t at) {
    return le16(s, at) | (u8(s, at + 2) << 16U);
}

[[nodiscard]] std::uint32_t be16(std::string_view s, std::size_t at) {
    return (u8(s, at) << 8U) | u8(s, at + 1);
}

[[nodiscard]] std::uint32_t be32(std::string_view s, std::size_t at) {
    return (be16(s, at) << 16U) | be16(s, at + 2);
}

/// Round-trips a document through text and the non-throwing parser.
[[nodiscard]] bool reparses(const nlohmann::json& doc) {
    const std::string text = doc.dump(2);
    const nlohmann::json back = nlohmann::json::parse(text, nullptr, false);
    return !back.is_discarded() && back == doc;
}

struct PngChunk {
    std::string type{};
    std::string data{};
    bool crc_ok = false;
};

/// Splits a PNG into chunks; nullopt on a bad signature or a chunk that runs past the end.
[[nodiscard]] std::optional<std::vector<PngChunk>> png_chunks(std::string_view png) {
    constexpr std::string_view signature{"\x89PNG\r\n\x1a\n"};
    if (!png.starts_with(signature)) {
        return std::nullopt;
    }
    std::vector<PngChunk> chunks;
    std::size_t pos = signature.size();
    while (pos < png.size()) {
        if (png.size() - pos < 12) {
            return std::nullopt;
        }
        const std::size_t len = be32(png, pos);
        if (png.size() - pos - 12 < len) {
            return std::nullopt;
        }
        const std::string_view body = png.substr(pos + 4, 4 + len);
        chunks.push_back(PngChunk{.type = std::string{body.substr(0, 4)},
                                  .data = std::string{body.substr(4)},
                                  .crc_ok = mdx::crc32(body) == be32(png, pos + 8 + len)});
        pos += 12 + len;
    }
    return chunks;
}

/// Inflates a zlib stream made only of stored blocks and checks its Adler-32 trailer.
[[nodiscard]] std::optional<std::string> inflate_stored(std::string_view z) {
    if (z.size() < 6 || (u8(z, 0) & 0x0FU) != 8U || ((u8(z, 0) << 8U) | u8(z, 1)) % 31U != 0U) {
        return std::nullopt;
    }
    std::string out;
    std::size_t pos = 2;
    bool final_block = false;
    while (!final_block) {
        if (z.size() - pos < 5) {
            return std::nullopt;
        }
        const std::uint32_t header = u8(z, pos);
        if (((header >> 1U) & 3U) != 0U) {
            return std::nullopt; // not a stored block
        }
        final_block = (header & 1U) != 0U;
        const std::size_t len = le16(z, pos + 1);
        if ((len ^ 0xFFFFU) != le16(z, pos + 3) || z.size() - pos - 5 < len) {
            return std::nullopt;
        }
        out.append(z.substr(pos + 5, len));
        pos += 5 + len;
    }
    if (z.size() - pos != 4 || be32(z, pos) != mdx::adler32(out)) {
        return std::nullopt;
    }
    return out;
}

struct GifDecoded {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pixels = 0;
};

/// Reads LZW codes LSB-first from GIF image data. Borrows `data`: the caller keeps the string alive
/// for the reader's whole (block-local) lifetime.
class BitReader {
public:
    explicit BitReader(std::string_view data) : data_(data) {}

    /// Reads `bits` (1..32) bits; nullopt past the end of the data or for an out-of-range width,
    /// so a bad stream fails the test instead of shifting by >= 32 (undefined behaviour).
    [[nodiscard]] std::optional<std::uint32_t> read(std::uint32_t bits) {
        if (bits == 0U || bits > 32U) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < bits; ++i) {
            if (bit_ / 8U >= data_.size()) {
                return std::nullopt;
            }
            const std::uint32_t byte = static_cast<std::uint8_t>(data_.at(bit_ / 8U));
            value |= ((byte >> (bit_ % 8U)) & 1U) << i;
            ++bit_;
        }
        return value;
    }

private:
    std::string_view data_{};
    std::size_t bit_ = 0;
};

/// A standard GIF LZW decoder (variable code width, up to 12 bits) that counts output pixels.
[[nodiscard]] std::optional<std::size_t> lzw_pixel_count(std::string_view data,
                                                         std::uint32_t min_code_size) {
    // GIF allows minimum code sizes 2..8 (codes up to 12 bits); anything else is a broken
    // generator, and rejecting it here keeps every shift below well-defined.
    if (min_code_size < 2U || min_code_size > 8U) {
        return std::nullopt;
    }
    const std::uint32_t clear = 1U << min_code_size;
    const std::uint32_t end = clear + 1U;
    std::vector<std::size_t> length(4096, 1);
    std::uint32_t width = min_code_size + 1U;
    std::uint32_t next = clear + 2U;
    std::optional<std::uint32_t> prev;
    std::size_t pixels = 0;
    BitReader reader{data};
    while (true) {
        const std::optional<std::uint32_t> code = reader.read(width);
        if (!code) {
            return std::nullopt; // ran out of data before the end code
        }
        if (*code == clear) {
            width = min_code_size + 1U;
            next = clear + 2U;
            prev.reset();
            continue;
        }
        if (*code == end) {
            return pixels;
        }
        if (!prev) {
            if (*code >= clear) {
                return std::nullopt;
            }
            pixels += 1;
            prev = code;
            continue;
        }
        if (*code > next || next >= length.size()) {
            return std::nullopt;
        }
        const std::size_t emitted = *code < next ? length.at(*code) : length.at(*prev) + 1;
        length.at(next) = length.at(*prev) + 1;
        ++next;
        if (next == (1U << width) && width < 12U) {
            ++width;
        }
        pixels += emitted;
        prev = code;
    }
}

[[nodiscard]] std::optional<GifDecoded> decode_gif(std::string_view gif) {
    if (!gif.starts_with("GIF89a") || gif.size() < 13) {
        return std::nullopt;
    }
    std::size_t pos = 13;
    const std::uint32_t packed = u8(gif, 10);
    if ((packed & 0x80U) != 0U) {
        pos += 3U * (std::size_t{1} << ((packed & 7U) + 1U));
    }
    if (u8(gif, pos) != 0x2CU) {
        return std::nullopt;
    }
    GifDecoded result{.width = le16(gif, pos + 5), .height = le16(gif, pos + 7)};
    if ((u8(gif, pos + 9) & 0x80U) != 0U) {
        return std::nullopt; // the fixture never writes a local colour table
    }
    pos += 10;
    const std::uint32_t min_code_size = u8(gif, pos++);
    std::string data;
    while (true) {
        const std::size_t block = u8(gif, pos++);
        if (block == 0) {
            break;
        }
        data.append(gif.substr(pos, block));
        pos += block;
    }
    if (u8(gif, pos) != 0x3BU || pos + 1 != gif.size()) {
        return std::nullopt;
    }
    const std::optional<std::size_t> pixels = lzw_pixel_count(data, min_code_size);
    if (!pixels) {
        return std::nullopt;
    }
    result.pixels = *pixels;
    return result;
}

[[nodiscard]] std::vector<mdx::FixtureManga> every_fixture() {
    std::vector<mdx::FixtureManga> out;
    for (const mdx::NamedFixture& fixture : mdx::all_fixtures()) {
        out.push_back(fixture.make());
    }
    return out;
}

[[nodiscard]] bool regular_nonempty(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0;
}

} // namespace

TEST_CASE("fixture checksums match the published check values", "[mdx_fixture]") {
    CHECK(mdx::crc32("123456789") == 0xCBF43926U);
    CHECK(mdx::crc32("") == 0U);
    CHECK(mdx::adler32("Wikipedia") == 0x11E60398U);
    CHECK(mdx::adler32("") == 1U);
}

TEST_CASE("every fixture JSON document round-trips through the parser", "[mdx_fixture]") {
    for (const mdx::FixtureManga& manga : every_fixture()) {
        INFO(manga.id);
        CHECK(reparses(mdx::manga_json(manga)));
        for (const nlohmann::json& page : mdx::feed_pages_json(manga)) {
            CHECK(reparses(page));
        }
        for (const nlohmann::json& page : mdx::covers_json(manga)) {
            CHECK(reparses(page));
        }
        for (const mdx::FixtureChapter& chapter : manga.chapters) {
            CHECK(reparses(mdx::at_home_json(chapter)));
        }
    }
}

TEST_CASE("manga JSON has the MangaDex entity shape", "[mdx_fixture]") {
    const nlohmann::json doc = mdx::manga_json(mdx::clean_fixture());
    CHECK(doc.at("result") == "ok");
    const nlohmann::json& data = doc.at("data");
    CHECK(data.at("id") == "11111111-1111-4111-8111-111111111111");
    CHECK(data.at("type") == "manga");
    const nlohmann::json& attributes = data.at("attributes");
    CHECK(attributes.at("title").at("en") == "Clean Title");
    CHECK(attributes.at("originalLanguage") == "ja");
    REQUIRE(attributes.at("altTitles").is_array());
    CHECK(attributes.at("altTitles").at(0).at("ja") == "クリーン・タイトル");
    std::vector<std::string> types;
    for (const nlohmann::json& rel : data.at("relationships")) {
        types.push_back(rel.at("type").get<std::string>());
        CHECK(rel.at("attributes").is_object());
    }
    CHECK(types == std::vector<std::string>{"author", "artist", "cover_art"});
}

TEST_CASE("feed pages have the collection shape and chapter attributes", "[mdx_fixture]") {
    const mdx::FixtureManga manga = mdx::basic_fixture();
    const std::vector<nlohmann::json> pages = mdx::feed_pages_json(manga);
    REQUIRE(pages.size() == 1);
    const nlohmann::json& page = pages.front();
    CHECK(page.at("limit") == 500);
    CHECK(page.at("offset") == 0);
    CHECK(page.at("total") == manga.chapters.size());
    REQUIRE(page.at("data").size() == manga.chapters.size());

    bool saw_external = false;
    bool saw_unavailable = false;
    bool saw_no_pages = false;
    bool saw_oneshot = false;
    bool saw_fr = false;
    for (const nlohmann::json& chapter : page.at("data")) {
        CHECK(chapter.at("type") == "chapter");
        const nlohmann::json& a = chapter.at("attributes");
        for (const char* key : {"volume", "chapter", "title", "translatedLanguage", "externalUrl",
                                "publishAt", "readableAt", "updatedAt", "pages", "isUnavailable"}) {
            INFO(key);
            CHECK(a.contains(key));
        }
        CHECK(chapter.at("relationships").at(0).at("type") == "scanlation_group");
        CHECK(chapter.at("relationships").at(0).at("attributes").at("name").is_string());
        saw_external = saw_external || a.at("externalUrl").is_string();
        saw_unavailable = saw_unavailable || a.at("isUnavailable") == true;
        saw_no_pages = saw_no_pages || (a.at("pages") == 0 && a.at("externalUrl").is_null());
        saw_oneshot = saw_oneshot || (a.at("volume").is_null() && a.at("chapter").is_null());
        saw_fr = saw_fr || a.at("translatedLanguage") == "fr";
    }
    CHECK(saw_external);
    CHECK(saw_unavailable);
    CHECK(saw_no_pages);
    CHECK(saw_oneshot);
    CHECK(saw_fr);
}

TEST_CASE("the bulk feed spans eight pages of 500", "[mdx_fixture]") {
    const mdx::FixtureManga manga = mdx::bulk_fixture();
    REQUIRE(manga.chapters.size() == mdx::bulk_chapter_count);
    const std::vector<nlohmann::json> pages = mdx::feed_pages_json(manga);
    REQUIRE(pages.size() == 8);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        CHECK(pages.at(i).at("offset") == i * 500);
        CHECK(pages.at(i).at("total") == 4000);
        CHECK(pages.at(i).at("data").size() == 500);
    }
    CHECK(manga.title_en.find_first_of("[];") == std::string::npos);
}

TEST_CASE("an empty feed or cover list is still one empty page", "[mdx_fixture]") {
    const mdx::FixtureManga manga{.id = "66666666-6666-4666-8666-666666666666", .title_en = "None"};
    const std::vector<nlohmann::json> feed = mdx::feed_pages_json(manga);
    REQUIRE(feed.size() == 1);
    CHECK(feed.front().at("data").empty());
    CHECK(feed.front().at("total") == 0);
    const std::vector<nlohmann::json> covers = mdx::covers_json(manga);
    REQUIRE(covers.size() == 1);
    CHECK(covers.front().at("limit") == 100);
    CHECK(covers.front().at("data").empty());
}

TEST_CASE("cover pages carry nullable volume and locale", "[mdx_fixture]") {
    const std::vector<nlohmann::json> pages = mdx::covers_json(mdx::basic_fixture());
    REQUIRE(pages.size() == 1);
    bool null_volume = false;
    bool null_locale = false;
    for (const nlohmann::json& cover : pages.front().at("data")) {
        CHECK(cover.at("type") == "cover_art");
        const nlohmann::json& a = cover.at("attributes");
        CHECK(a.at("fileName").is_string());
        null_volume = null_volume || a.at("volume").is_null();
        null_locale = null_locale || a.at("locale").is_null();
    }
    CHECK(null_volume);
    CHECK(null_locale);
}

TEST_CASE("at-home JSON lists data and data-saver names under the fixture base URL",
          "[mdx_fixture]") {
    const mdx::FixtureChapter chapter = mdx::clean_fixture().chapters.front();
    const nlohmann::json doc = mdx::at_home_json(chapter);
    CHECK(doc.at("result") == "ok");
    CHECK(doc.at("baseUrl") == "https://fixture.invalid");
    CHECK(doc.at("chapter").at("hash") == chapter.hash);
    CHECK(chapter.hash.size() == 32);
    const std::vector<std::string> data = doc.at("chapter").at("data");
    const std::vector<std::string> saver = doc.at("chapter").at("dataSaver");
    CHECK(data == std::vector<std::string>{"p001.png", "p002.gif", "p003.png"});
    CHECK(saver == std::vector<std::string>{"s001.png", "s002.gif", "s003.png"});
}

TEST_CASE("PNG fixture images are valid and decodable", "[mdx_fixture]") {
    // 300 x 300 needs two stored deflate blocks (90300 raw bytes > 65535).
    for (const auto& [width, height] : {std::pair<std::uint16_t, std::uint16_t>{8, 12},
                                        std::pair<std::uint16_t, std::uint16_t>{1, 1},
                                        std::pair<std::uint16_t, std::uint16_t>{0, 3},
                                        std::pair<std::uint16_t, std::uint16_t>{300, 300}}) {
        INFO(width << "x" << height);
        const std::string png =
            mdx::image_bytes({.kind = mdx::ImageKind::png, .width = width, .height = height});
        const std::optional<std::vector<PngChunk>> parsed = png_chunks(png);
        REQUIRE(parsed.has_value()); // value_or below: clang-tidy cannot see REQUIRE's early exit
        const std::vector<PngChunk> chunks = parsed.value_or(std::vector<PngChunk>{});
        REQUIRE(chunks.size() == 3);
        for (const PngChunk& chunk : chunks) {
            INFO(chunk.type);
            CHECK(chunk.crc_ok);
        }
        const PngChunk& ihdr = chunks.at(0);
        REQUIRE(ihdr.type == "IHDR");
        REQUIRE(ihdr.data.size() == 13);
        CHECK(be32(ihdr.data, 0) == width);
        CHECK(be32(ihdr.data, 4) == height);
        CHECK(u8(ihdr.data, 8) == 8);  // bit depth
        CHECK(u8(ihdr.data, 9) == 0);  // grayscale
        CHECK(u8(ihdr.data, 12) == 0); // not interlaced
        REQUIRE(chunks.at(1).type == "IDAT");
        const std::optional<std::string> inflated = inflate_stored(chunks.at(1).data);
        REQUIRE(inflated.has_value());
        const std::string raw = inflated.value_or(std::string{});
        const std::size_t stride = std::size_t{width} + 1;
        REQUIRE(raw.size() == stride * height);
        for (std::size_t row = 0; row < height; ++row) {
            CHECK(u8(raw, row * stride) == 0); // filter type None on every scanline
        }
        CHECK(chunks.at(2).type == "IEND");
        CHECK(chunks.at(2).data.empty());
    }
}

TEST_CASE("GIF fixture images are valid and decodable", "[mdx_fixture]") {
    for (const auto& [width, height] : {std::pair<std::uint16_t, std::uint16_t>{8, 12},
                                        std::pair<std::uint16_t, std::uint16_t>{1, 1},
                                        std::pair<std::uint16_t, std::uint16_t>{97, 41}}) {
        INFO(width << "x" << height);
        const std::string gif =
            mdx::image_bytes({.kind = mdx::ImageKind::gif, .width = width, .height = height});
        CHECK(le16(gif, 6) == width); // logical screen
        CHECK(le16(gif, 8) == height);
        const std::optional<GifDecoded> decoded = decode_gif(gif);
        REQUIRE(decoded.has_value());
        const GifDecoded image = decoded.value_or(GifDecoded{});
        CHECK(image.width == width);
        CHECK(image.height == height);
        CHECK(image.pixels == std::size_t{width} * height);
    }
}

TEST_CASE("header-only JPEG and WebP fixtures are sniffable", "[mdx_fixture]") {
    const std::string jpeg =
        mdx::image_bytes({.kind = mdx::ImageKind::jpeg_header, .width = 640, .height = 480});
    CHECK(be16(jpeg, 0) == 0xFFD8U);
    CHECK(be16(jpeg, 2) == 0xFFE0U);
    CHECK(jpeg.substr(6, 5) == std::string_view{"JFIF\0", 5});
    CHECK(be16(jpeg, 20) == 0xFFC0U); // SOF0 right after the 18-byte APP0 segment
    CHECK(be16(jpeg, 22) == 17U);
    CHECK(be16(jpeg, 25) == 480U); // height first
    CHECK(be16(jpeg, 27) == 640U);
    CHECK(be16(jpeg, jpeg.size() - 2) == 0xFFD9U);

    const std::string webp =
        mdx::image_bytes({.kind = mdx::ImageKind::webp_header, .width = 640, .height = 480});
    CHECK(webp.substr(0, 4) == "RIFF");
    CHECK(le16(webp, 4) + 8U == webp.size()); // RIFF size covers everything after it
    CHECK(webp.substr(8, 8) == "WEBPVP8X");
    CHECK(le24(webp, 24) + 1U == 640U);
    CHECK(le24(webp, 27) + 1U == 480U);
    CHECK(webp.size() == 30);
}

TEST_CASE("garbage and truncated fixtures carry no complete header", "[mdx_fixture]") {
    const std::string garbage = mdx::image_bytes({.kind = mdx::ImageKind::garbage});
    CHECK_FALSE(garbage.starts_with("\x89PNG"));
    CHECK_FALSE(garbage.starts_with("GIF8"));
    CHECK_FALSE(garbage.starts_with("RIFF"));
    CHECK_FALSE(garbage.starts_with("\xFF\xD8"));
    const std::string truncated = mdx::image_bytes({.kind = mdx::ImageKind::truncated_png});
    CHECK(truncated.size() == 20);
    CHECK(truncated.starts_with("\x89PNG\r\n\x1a\n"));
    CHECK(truncated.substr(12, 4) == "IHDR");
}

TEST_CASE("canned fixtures follow the naming rules", "[mdx_fixture]") {
    for (const mdx::FixtureManga& manga : every_fixture()) {
        INFO(manga.id);
        CHECK(manga.title_en.find_first_of("[];") == std::string::npos);
        for (const mdx::FixtureChapter& chapter : manga.chapters) {
            CHECK(chapter.id.size() == 36);
            CHECK(chapter.publish_at.ends_with("+00:00"));
            CHECK(chapter.images.empty() == chapter.hash.empty());
        }
    }
    CHECK(mdx::clean_fixture().id == "11111111-1111-4111-8111-111111111111");
    CHECK(mdx::basic_fixture().id == "22222222-2222-4222-8222-222222222222");
    CHECK(mdx::bulk_fixture().id == "33333333-3333-4333-8333-333333333333");
    CHECK(mdx::incomplete_fixture().id == "44444444-4444-4444-8444-444444444444");
    CHECK(mdx::nochapters_fixture().id == "55555555-5555-4555-8555-555555555555");
}

TEST_CASE("write_fixture_tree lays out the fixture-mode tree", "[mdx_fixture]") {
    auto dir = TempDir::create();
    REQUIRE(dir.has_value());
    const std::filesystem::path root = dir->path() / "clean";
    const mdx::FixtureManga manga = mdx::clean_fixture();
    const auto written = mdx::write_fixture_tree(root, manga);
    REQUIRE(written.has_value());

    CHECK(regular_nonempty(root / "api" / "manga" / (manga.id + ".json")));
    CHECK(regular_nonempty(root / "api" / "manga" / manga.id / "feed" / "offset-0.json"));
    CHECK_FALSE(
        std::filesystem::exists(root / "api" / "manga" / manga.id / "feed" / "offset-500.json"));
    CHECK(regular_nonempty(root / "api" / "cover" / "offset-0.json"));
    for (const mdx::FixtureChapter& chapter : manga.chapters) {
        INFO(chapter.id);
        CHECK(regular_nonempty(root / "api" / "at-home" / "server" / (chapter.id + ".json")));
        for (std::size_t i = 0; i < chapter.images.size(); ++i) {
            const mdx::PageImage& image = chapter.images.at(i);
            CHECK(regular_nonempty(root / "at-home" / "data" / chapter.hash /
                                   mdx::page_file_name(i, image)));
            CHECK(regular_nonempty(root / "at-home" / "data-saver" / chapter.hash /
                                   mdx::data_saver_file_name(i, image)));
        }
    }
    for (const mdx::FixtureCover& cover : manga.covers) {
        CHECK(regular_nonempty(root / "uploads" / "covers" / manga.id / cover.file));
    }
}

TEST_CASE("write_fixture_tree leaves missing pages out and skips hashless chapters",
          "[mdx_fixture]") {
    auto dir = TempDir::create();
    REQUIRE(dir.has_value());

    const mdx::FixtureManga incomplete = mdx::incomplete_fixture();
    REQUIRE(mdx::write_fixture_tree(dir->path() / "incomplete", incomplete).has_value());
    const mdx::FixtureChapter& holey = incomplete.chapters.at(1);
    const std::filesystem::path pages =
        dir->path() / "incomplete" / "at-home" / "data" / holey.hash;
    CHECK(regular_nonempty(pages / "p001.png"));
    CHECK_FALSE(std::filesystem::exists(pages / "p002.png"));
    const nlohmann::json listed = mdx::at_home_json(holey);
    CHECK(listed.at("chapter").at("data").size() == 2); // still listed: a 404 at fetch time

    const mdx::FixtureManga bulk = mdx::bulk_fixture();
    const std::filesystem::path bulk_root = dir->path() / "bulk";
    REQUIRE(mdx::write_fixture_tree(bulk_root, bulk).has_value());
    const std::filesystem::path feed = bulk_root / "api" / "manga" / bulk.id / "feed";
    CHECK(regular_nonempty(feed / "offset-3500.json"));
    CHECK_FALSE(std::filesystem::exists(feed / "offset-4000.json"));
    CHECK_FALSE(std::filesystem::exists(bulk_root / "api" / "at-home"));
    CHECK_FALSE(std::filesystem::exists(bulk_root / "at-home"));
}

TEST_CASE("write_fixture_tree refuses unsafe path components", "[mdx_fixture]") {
    auto dir = TempDir::create();
    REQUIRE(dir.has_value());
    mdx::FixtureManga manga = mdx::clean_fixture();
    manga.covers.at(0).file = "../escape.png";
    const auto written = mdx::write_fixture_tree(dir->path() / "t", manga);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().find("unsafe cover file") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir->path() / "escape.png"));

    manga = mdx::clean_fixture();
    manga.chapters.at(0).hash = ".hidden";
    CHECK_FALSE(mdx::write_fixture_tree(dir->path() / "t2", manga).has_value());
}

TEST_CASE("TempDir creates a private directory and removes it", "[temp_dir]") {
    std::filesystem::path kept;
    {
        std::optional<TempDir> target;
        {
            auto dir = TempDir::create("mdx-temp-test");
            REQUIRE(dir.has_value());
            kept = dir->path();
            CHECK(std::filesystem::is_directory(kept));
            CHECK(kept.filename().string().starts_with("mdx-temp-test-"));
            const auto perms = std::filesystem::status(kept).permissions();
            CHECK((perms & std::filesystem::perms::all) == std::filesystem::perms::owner_all);
            target.emplace(std::move(*dir));
        } // the moved-from TempDir dies here and must not remove the directory
        REQUIRE(target.has_value());
        CHECK(std::filesystem::is_directory(kept));
        CHECK(target->path() == kept);
        std::filesystem::create_directories(kept / "a" / "b");

        auto other = TempDir::create("mdx-temp-test");
        REQUIRE(other.has_value());
        const std::filesystem::path replaced = other->path();
        *other = std::move(*target); // move assignment removes the directory it replaces
        CHECK_FALSE(std::filesystem::exists(replaced));
        CHECK(other->path() == kept);
        CHECK(std::filesystem::exists(kept / "a" / "b"));
    } // removed recursively, subdirectories included
    CHECK_FALSE(std::filesystem::exists(kept));
}

TEST_CASE("TempDir rejects a prefix with a slash", "[temp_dir]") {
    CHECK_FALSE(TempDir::create("a/b").has_value());
    CHECK_FALSE(TempDir::create("").has_value());
}
