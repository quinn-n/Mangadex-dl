#pragma once
// tests/support/mdx_fixture.hpp: hermetic MangaDex fixtures for every test tier.
//
// Header-only and self-contained: it depends on nlohmann/json and the standard library only, never
// on mangadex_dl::core, so unit tests can feed its JSON to the core parsers and E2E cases can point
// `--api-base file://<root>` at a tree written by write_fixture_tree() (see tests/e2e/inputs/README
// for the on-disk layout, which mirrors the fixture transport's URL mapping).
//
// What is real and what is not:
// - The JSON follows the fact-checked MangaDex shapes (docs/design/download-factcheck.md, cluster
// F):
//   manga entity, feed collection pages ({data, limit, offset, total}), cover collection pages and
//   the at-home server document. Unknown extra fields are included on purpose: parsers must ignore
//   them.
// - PNG and GIF pages are real, decodable minimal images (grayscale PNG with stored zlib blocks; a
//   two-colour GIF89a whose LZW stream re-clears before every pixel). JPEG and WebP pages are
//   HEADER-ONLY: sniffable (JPEG SOF0 marker, WebP RIFF/VP8X canvas) but not decodable. `garbage`
//   has no known magic; `truncated_png` stops inside the IHDR width field.
// - Every at-home baseUrl is https://fixture.invalid (an https URL that can never resolve).
// - Titles never contain '[', ']' or ';', which the CMake E2E driver cannot pass safely.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mangadex_dl::test::mdx {

// ---- model -------------------------------------------------------------------------------------

enum class ImageKind { png, gif, jpeg_header, webp_header, garbage, truncated_png };

struct PageImage {
    ImageKind kind = ImageKind::png;
    std::uint16_t width = 8;
    std::uint16_t height = 12;
    /// Listed in the at-home document but never written to disk (a 404 in fixture mode).
    bool missing = false;
};

struct FixtureGroup {
    std::string id{};
    std::string name{};
};

struct FixtureCreator {
    std::string id{};
    std::string name{};
};

struct LocalizedTitle {
    std::string lang{};
    std::string title{};
};

struct FixtureChapter {
    std::string id{};
    std::optional<std::string> volume{};
    std::optional<std::string> chapter{};
    std::optional<std::string> title{};
    std::string lang = "en";
    std::vector<std::string> group_ids{};
    /// The `pages` attribute; nullopt means "the number of images".
    std::optional<int> pages{};
    std::optional<std::string> external_url{};
    bool unavailable = false;
    std::string publish_at{};
    std::string updated_at{};
    /// The at-home hash. Empty: no at-home document is written (a 404 in fixture mode).
    std::string hash{};
    std::vector<PageImage> images{};
};

struct FixtureCover {
    std::string id{};
    std::optional<std::string> volume{};
    std::string file{};
    std::optional<std::string> locale{};
    PageImage image{.kind = ImageKind::png, .width = 6, .height = 9};
};

struct FixtureManga {
    std::string id{};
    std::string title_en{};
    std::vector<LocalizedTitle> alt_titles{};
    std::string original_language = "ja";
    std::vector<FixtureCreator> authors{};
    std::vector<FixtureCreator> artists{};
    std::vector<FixtureGroup> groups{};
    std::vector<FixtureChapter> chapters{};
    std::vector<FixtureCover> covers{};
};

inline constexpr std::string_view at_home_base_url = "https://fixture.invalid";
inline constexpr std::size_t default_feed_limit = 500;
inline constexpr std::size_t default_cover_limit = 100;

// ---- checksums (independent of core's CRC-32, so the two cross-check each other)
// -------------------

/// CRC-32 (ISO-HDLC: reflected polynomial 0xEDB88320, init and xorout 0xFFFFFFFF), bitwise.
[[nodiscard]] inline std::uint32_t crc32(std::string_view bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char c : bytes) {
        crc ^= static_cast<std::uint8_t>(c);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/// Adler-32 (RFC 1950).
[[nodiscard]] inline std::uint32_t adler32(std::string_view bytes) noexcept {
    constexpr std::uint32_t mod = 65521U;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const char c : bytes) {
        a = (a + static_cast<std::uint8_t>(c)) % mod;
        b = (b + a) % mod;
    }
    return (b << 16U) | a;
}

// ---- image bytes --------------------------------------------------------------------------------

namespace detail {

inline void put_u8(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(value & 0xFFU)));
}

inline void put_u16_le(std::string& out, std::uint32_t value) {
    put_u8(out, value);
    put_u8(out, value >> 8U);
}

inline void put_u24_le(std::string& out, std::uint32_t value) {
    put_u16_le(out, value);
    put_u8(out, value >> 16U);
}

inline void put_u32_le(std::string& out, std::uint32_t value) {
    put_u16_le(out, value);
    put_u16_le(out, value >> 16U);
}

inline void put_u16_be(std::string& out, std::uint32_t value) {
    put_u8(out, value >> 8U);
    put_u8(out, value);
}

inline void put_u32_be(std::string& out, std::uint32_t value) {
    put_u16_be(out, value >> 16U);
    put_u16_be(out, value);
}

/// One PNG chunk: length, type, data, CRC over type + data.
inline void put_png_chunk(std::string& out, std::string_view type, std::string_view data) {
    put_u32_be(out, static_cast<std::uint32_t>(data.size()));
    std::string body{type};
    body.append(data);
    out.append(body);
    put_u32_be(out, crc32(body));
}

/// A zlib stream (RFC 1950) holding `raw` in stored (uncompressed) deflate blocks. At least one
/// block is always written, so an empty input is still a valid stream.
[[nodiscard]] inline std::string zlib_stored(std::string_view raw) {
    constexpr std::size_t max_block = 65535;
    std::string out;
    put_u8(out, 0x78U); // CMF: deflate, 32 KiB window
    put_u8(out, 0x01U); // FLG: (0x78 * 256 + 0x01) % 31 == 0, no dictionary
    std::size_t offset = 0;
    do {
        const std::size_t len = std::min(max_block, raw.size() - offset);
        const bool final_block = offset + len == raw.size();
        put_u8(out, final_block ? 1U : 0U); // BFINAL, BTYPE = 00 (stored)
        put_u16_le(out, static_cast<std::uint32_t>(len));
        put_u16_le(out, static_cast<std::uint32_t>(~len & 0xFFFFU));
        out.append(raw.substr(offset, len));
        offset += len;
    } while (offset < raw.size());
    put_u32_be(out, adler32(raw));
    return out;
}

/// A decodable 8-bit grayscale PNG with a deterministic gradient.
[[nodiscard]] inline std::string png_bytes(std::uint16_t width, std::uint16_t height) {
    std::string out{"\x89PNG\r\n\x1a\n"};
    std::string ihdr;
    put_u32_be(ihdr, width);
    put_u32_be(ihdr, height);
    put_u8(ihdr, 8U); // bit depth
    put_u8(ihdr, 0U); // colour type: grayscale
    put_u8(ihdr, 0U); // compression: deflate
    put_u8(ihdr, 0U); // filter method 0
    put_u8(ihdr, 0U); // no interlace
    put_png_chunk(out, "IHDR", ihdr);

    const std::uint32_t w = width;
    const std::uint32_t h = height;
    std::string raw;
    raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) + 1U));
    for (std::uint32_t y = 0; y < h; ++y) {
        put_u8(raw, 0U); // filter type None
        for (std::uint32_t x = 0; x < w; ++x) {
            put_u8(raw, (x + y) * 16U);
        }
    }
    put_png_chunk(out, "IDAT", zlib_stored(raw));
    put_png_chunk(out, "IEND", {});
    return out;
}

/// A decodable two-colour GIF89a. LZW minimum code size 2 (clear = 4, end = 5, 3-bit codes); a
/// clear code precedes every pixel, so the decoder's table never grows and the code width never
/// changes.
[[nodiscard]] inline std::string gif_bytes(std::uint16_t width, std::uint16_t height) {
    std::string out{"GIF89a"};
    put_u16_le(out, width);
    put_u16_le(out, height);
    put_u8(out, 0x80U); // global colour table present, 2 entries
    put_u8(out, 0U);    // background colour index
    put_u8(out, 0U);    // pixel aspect ratio
    for (const std::uint32_t rgb : {0x000000U, 0xFFFFFFU}) {
        put_u8(out, rgb >> 16U);
        put_u8(out, rgb >> 8U);
        put_u8(out, rgb);
    }
    put_u8(out, 0x2CU); // image descriptor
    put_u16_le(out, 0U);
    put_u16_le(out, 0U);
    put_u16_le(out, width);
    put_u16_le(out, height);
    put_u8(out, 0U); // no local colour table, not interlaced

    constexpr std::uint32_t clear_code = 4U;
    constexpr std::uint32_t end_code = 5U;
    constexpr std::uint32_t code_bits = 3U;
    std::string lzw;
    std::uint32_t acc = 0U;
    std::uint32_t nbits = 0U;
    const auto emit = [&lzw, &acc, &nbits](std::uint32_t code) {
        acc |= code << nbits;
        nbits += code_bits;
        while (nbits >= 8U) {
            put_u8(lzw, acc);
            acc >>= 8U;
            nbits -= 8U;
        }
    };
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < pixels; ++i) {
        emit(clear_code);
        emit(static_cast<std::uint32_t>(i % 2U));
    }
    emit(end_code);
    if (nbits > 0U) {
        put_u8(lzw, acc);
    }

    put_u8(out, 2U); // LZW minimum code size
    constexpr std::size_t max_sub_block = 255;
    for (std::size_t offset = 0; offset < lzw.size(); offset += max_sub_block) {
        const std::string_view block = std::string_view{lzw}.substr(offset, max_sub_block);
        put_u8(out, static_cast<std::uint32_t>(block.size()));
        out.append(block);
    }
    put_u8(out, 0U);    // block terminator
    put_u8(out, 0x3BU); // trailer
    return out;
}

/// SOI, a JFIF APP0 segment, a baseline SOF0 frame header (3 components) and EOI. No tables and no
/// scan: a sniffer finds the dimensions, a decoder fails.
[[nodiscard]] inline std::string jpeg_header_bytes(std::uint16_t width, std::uint16_t height) {
    std::string out;
    put_u16_be(out, 0xFFD8U); // SOI
    put_u16_be(out, 0xFFE0U); // APP0
    put_u16_be(out, 16U);
    out.append(std::string_view{"JFIF\0", 5});
    for (const std::uint32_t b : {1U, 1U, 0U, 0U, 1U, 0U, 1U, 0U, 0U}) {
        put_u8(out, b); // version 1.1, no units, density 1x1, no thumbnail
    }
    put_u16_be(out, 0xFFC0U); // SOF0
    put_u16_be(out, 17U);
    put_u8(out, 8U); // sample precision
    put_u16_be(out, height);
    put_u16_be(out, width);
    put_u8(out, 3U);
    for (const std::uint32_t b : {1U, 0x22U, 0U, 2U, 0x11U, 1U, 3U, 0x11U, 1U}) {
        put_u8(out, b); // component id, sampling factors, quantisation table
    }
    put_u16_be(out, 0xFFD9U); // EOI
    return out;
}

/// RIFF/WEBP with only a VP8X chunk (canvas size stored minus one, 24-bit little-endian). Not
/// decodable. VP8X cannot express a side of 0, so 0 is written as 1.
[[nodiscard]] inline std::string webp_header_bytes(std::uint16_t width, std::uint16_t height) {
    const auto minus_one = [](std::uint16_t side) -> std::uint32_t {
        return side == 0U ? 0U : static_cast<std::uint32_t>(side) - 1U;
    };
    std::string out{"RIFF"};
    put_u32_le(out, 4U + 8U + 10U); // "WEBP" + chunk header + VP8X payload
    out.append("WEBPVP8X");
    put_u32_le(out, 10U);
    put_u32_le(out, 0U); // flags + 3 reserved bytes
    put_u24_le(out, minus_one(width));
    put_u24_le(out, minus_one(height));
    return out;
}

} // namespace detail

/// The file bytes for one page or cover image (binary data in a std::string).
[[nodiscard]] inline std::string image_bytes(const PageImage& image) {
    switch (image.kind) {
    case ImageKind::png:
        return detail::png_bytes(image.width, image.height);
    case ImageKind::gif:
        return detail::gif_bytes(image.width, image.height);
    case ImageKind::jpeg_header:
        return detail::jpeg_header_bytes(image.width, image.height);
    case ImageKind::webp_header:
        return detail::webp_header_bytes(image.width, image.height);
    case ImageKind::garbage:
        return "not an image: fixture garbage bytes\n";
    case ImageKind::truncated_png:
        // Signature, IHDR length and type, and the width: the height is cut off.
        return detail::png_bytes(image.width, image.height).substr(0, 20);
    }
    return {};
}

/// The file extension used for an image kind (no dot).
[[nodiscard]] inline std::string_view image_extension(ImageKind kind) noexcept {
    switch (kind) {
    case ImageKind::png:
    case ImageKind::garbage:
    case ImageKind::truncated_png:
        return "png";
    case ImageKind::gif:
        return "gif";
    case ImageKind::jpeg_header:
        return "jpg";
    case ImageKind::webp_header:
        return "webp";
    }
    return "bin";
}

/// The at-home `data[]` file name of page `index` (0-based): `p001.png`, ...
[[nodiscard]] inline std::string page_file_name(std::size_t index, const PageImage& image) {
    return std::format("p{:03}.{}", index + 1U, image_extension(image.kind));
}

/// The at-home `dataSaver[]` file name of page `index` (0-based): `s001.png`, ...
[[nodiscard]] inline std::string data_saver_file_name(std::size_t index, const PageImage& image) {
    return std::format("s{:03}.{}", index + 1U, image_extension(image.kind));
}

// ---- JSON documents -----------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline nlohmann::json optional_string(const std::optional<std::string>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

[[nodiscard]] inline int page_count(const FixtureChapter& chapter) {
    return chapter.pages.value_or(static_cast<int>(chapter.images.size()));
}

[[nodiscard]] inline nlohmann::json creator_relationship(const FixtureCreator& creator,
                                                         std::string_view type) {
    return {{"id", creator.id}, {"type", type}, {"attributes", {{"name", creator.name}}}};
}

[[nodiscard]] inline nlohmann::json cover_attributes(const FixtureCover& cover) {
    return {{"volume", optional_string(cover.volume)},
            {"fileName", cover.file},
            {"locale", optional_string(cover.locale)},
            {"description", ""},
            {"version", 1},
            {"createdAt", "2020-01-01T00:00:00+00:00"},
            {"updatedAt", "2020-01-01T00:00:00+00:00"}};
}

[[nodiscard]] inline nlohmann::json chapter_json(const FixtureManga& manga,
                                                 const FixtureChapter& chapter) {
    nlohmann::json relationships = nlohmann::json::array();
    for (const std::string& group_id : chapter.group_ids) {
        nlohmann::json rel = {{"id", group_id}, {"type", "scanlation_group"}};
        for (const FixtureGroup& group : manga.groups) {
            if (group.id == group_id) {
                rel["attributes"] = {{"name", group.name}};
                break;
            }
        }
        relationships.push_back(std::move(rel));
    }
    relationships.push_back({{"id", manga.id}, {"type", "manga"}});
    return {{"id", chapter.id},
            {"type", "chapter"},
            {"attributes",
             {{"volume", optional_string(chapter.volume)},
              {"chapter", optional_string(chapter.chapter)},
              {"title", optional_string(chapter.title)},
              {"translatedLanguage", chapter.lang},
              {"externalUrl", optional_string(chapter.external_url)},
              {"publishAt", chapter.publish_at},
              {"readableAt", chapter.publish_at},
              {"createdAt", chapter.publish_at},
              {"updatedAt", chapter.updated_at},
              {"pages", page_count(chapter)},
              {"version", 1},
              {"isUnavailable", chapter.unavailable}}},
            {"relationships", std::move(relationships)}};
}

/// Splits `count` items into collection pages of `limit`; always at least one (possibly empty)
/// page.
template <typename MakeItem>
[[nodiscard]] std::vector<nlohmann::json> collection_pages(std::size_t count, std::size_t limit,
                                                           MakeItem make_item) {
    if (limit == 0) {
        limit = 1;
    }
    std::vector<nlohmann::json> pages;
    std::size_t offset = 0;
    do {
        nlohmann::json data = nlohmann::json::array();
        for (std::size_t i = offset; i < count && i < offset + limit; ++i) {
            data.push_back(make_item(i));
        }
        pages.push_back({{"result", "ok"},
                         {"response", "collection"},
                         {"data", std::move(data)},
                         {"limit", limit},
                         {"offset", offset},
                         {"total", count}});
        offset += limit;
    } while (offset < count);
    return pages;
}

} // namespace detail

/// GET /manga/{id}?includes[]=cover_art&includes[]=author&includes[]=artist
[[nodiscard]] inline nlohmann::json manga_json(const FixtureManga& manga) {
    nlohmann::json alt_titles = nlohmann::json::array();
    for (const LocalizedTitle& alt : manga.alt_titles) {
        alt_titles.push_back(nlohmann::json::object({{alt.lang, alt.title}}));
    }
    nlohmann::json relationships = nlohmann::json::array();
    for (const FixtureCreator& author : manga.authors) {
        relationships.push_back(detail::creator_relationship(author, "author"));
    }
    for (const FixtureCreator& artist : manga.artists) {
        relationships.push_back(detail::creator_relationship(artist, "artist"));
    }
    if (!manga.covers.empty()) {
        const FixtureCover& main_cover = manga.covers.front();
        relationships.push_back({{"id", main_cover.id},
                                 {"type", "cover_art"},
                                 {"attributes", detail::cover_attributes(main_cover)}});
    }
    return {{"result", "ok"},
            {"response", "entity"},
            {"data",
             {{"id", manga.id},
              {"type", "manga"},
              {"attributes",
               {{"title", {{"en", manga.title_en}}},
                {"altTitles", std::move(alt_titles)},
                {"description", {{"en", "A hermetic test fixture."}}},
                {"originalLanguage", manga.original_language},
                {"status", "completed"},
                {"contentRating", "safe"},
                {"tags", nlohmann::json::array()},
                {"version", 1}}},
              {"relationships", std::move(relationships)}}}};
}

/// GET /manga/{id}/feed?...&limit=<limit>&offset=<n>: one document per page, offsets 0, limit, ...
/// Every language is included: fixture mode ignores the query except `offset`, so the client must
/// filter by language itself.
[[nodiscard]] inline std::vector<nlohmann::json>
feed_pages_json(const FixtureManga& manga, std::size_t limit = default_feed_limit) {
    return detail::collection_pages(manga.chapters.size(), limit, [&manga](std::size_t i) {
        return detail::chapter_json(manga, manga.chapters[i]);
    });
}

/// GET /cover?manga[]=<id>&limit=<limit>&offset=<n>: one document per page.
[[nodiscard]] inline std::vector<nlohmann::json>
covers_json(const FixtureManga& manga, std::size_t limit = default_cover_limit) {
    return detail::collection_pages(manga.covers.size(), limit, [&manga](std::size_t i) {
        const FixtureCover& cover = manga.covers[i];
        return nlohmann::json{
            {"id", cover.id},
            {"type", "cover_art"},
            {"attributes", detail::cover_attributes(cover)},
            {"relationships", nlohmann::json::array({{{"id", manga.id}, {"type", "manga"}}})}};
    });
}

/// GET /at-home/server/{chapterId}
[[nodiscard]] inline nlohmann::json at_home_json(const FixtureChapter& chapter) {
    nlohmann::json data = nlohmann::json::array();
    nlohmann::json data_saver = nlohmann::json::array();
    for (std::size_t i = 0; i < chapter.images.size(); ++i) {
        data.push_back(page_file_name(i, chapter.images[i]));
        data_saver.push_back(data_saver_file_name(i, chapter.images[i]));
    }
    return {{"result", "ok"},
            {"baseUrl", at_home_base_url},
            {"chapter", {{"hash", chapter.hash}, {"data", data}, {"dataSaver", data_saver}}}};
}

// ---- the on-disk tree ---------------------------------------------------------------------------

namespace detail {

/// The path-component rule the core enforces on at-home hashes and file names
/// (^[A-Za-z0-9][A-Za-z0-9._-]*$). UUIDs satisfy it too.
[[nodiscard]] inline bool safe_component(std::string_view name) noexcept {
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    return !name.empty() && alnum(name.front()) && std::ranges::all_of(name, [&alnum](char c) {
        return alnum(c) || c == '.' || c == '_' || c == '-';
    });
}

[[nodiscard]] inline std::expected<void, std::string> write_file(const std::filesystem::path& path,
                                                                 std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected(std::format("{}: {}", path.parent_path().string(), ec.message()));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(std::format("{}: cannot open for writing", path.string()));
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        return std::unexpected(std::format("{}: write failed", path.string()));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> write_json(const std::filesystem::path& path,
                                                                 const nlohmann::json& doc) {
    return write_file(path, doc.dump(2) + "\n");
}

[[nodiscard]] inline std::expected<void, std::string> check_names(const FixtureManga& manga) {
    const auto bad = [](std::string_view what, std::string_view name) {
        return std::unexpected(
            std::format("unsafe {} '{}' (must match [A-Za-z0-9][A-Za-z0-9._-]*)", what, name));
    };
    if (!safe_component(manga.id)) {
        return bad("manga id", manga.id);
    }
    for (const FixtureChapter& chapter : manga.chapters) {
        if (!chapter.hash.empty() && !safe_component(chapter.id)) {
            return bad("chapter id", chapter.id);
        }
        if (!chapter.hash.empty() && !safe_component(chapter.hash)) {
            return bad("hash", chapter.hash);
        }
    }
    for (const FixtureCover& cover : manga.covers) {
        if (!safe_component(cover.file)) {
            return bad("cover file", cover.file);
        }
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string>
write_chapter_files(const std::filesystem::path& root, const FixtureChapter& chapter) {
    if (chapter.hash.empty()) {
        return {};
    }
    if (const auto ok = write_json(root / "api" / "at-home" / "server" / (chapter.id + ".json"),
                                   at_home_json(chapter));
        !ok) {
        return ok;
    }
    for (std::size_t i = 0; i < chapter.images.size(); ++i) {
        const PageImage& image = chapter.images[i];
        if (image.missing) {
            continue;
        }
        const std::string bytes = image_bytes(image);
        if (const auto ok = write_file(
                root / "at-home" / "data" / chapter.hash / page_file_name(i, image), bytes);
            !ok) {
            return ok;
        }
        if (const auto ok = write_file(root / "at-home" / "data-saver" / chapter.hash /
                                           data_saver_file_name(i, image),
                                       bytes);
            !ok) {
            return ok;
        }
    }
    return {};
}

} // namespace detail

/// Writes the fixture-mode tree for `manga` under `root` (created if needed; existing files are
/// overwritten, stale ones are NOT removed):
///   api/manga/<id>.json                         manga_json
///   api/manga/<id>/feed/offset-<n>.json         feed_pages_json (n = 0, 500, ...; always offset-0)
///   api/cover/offset-<n>.json                   covers_json (n = 0, 100, ...; always offset-0)
///   api/at-home/server/<chapterId>.json         at_home_json, for chapters with a non-empty hash
///   at-home/data/<hash>/<p###.ext>              page images (not for PageImage::missing)
///   at-home/data-saver/<hash>/<s###.ext>        the same bytes under the data-saver names
///   uploads/covers/<mangaId>/<file>             cover images
[[nodiscard]] inline std::expected<void, std::string>
write_fixture_tree(const std::filesystem::path& root, const FixtureManga& manga) {
    if (const auto ok = detail::check_names(manga); !ok) {
        return ok;
    }
    const std::filesystem::path api = root / "api";
    if (const auto ok = detail::write_json(api / "manga" / (manga.id + ".json"), manga_json(manga));
        !ok) {
        return ok;
    }
    const std::vector<nlohmann::json> feed = feed_pages_json(manga);
    for (std::size_t i = 0; i < feed.size(); ++i) {
        const std::string name = std::format("offset-{}.json", i * default_feed_limit);
        if (const auto ok = detail::write_json(api / "manga" / manga.id / "feed" / name, feed[i]);
            !ok) {
            return ok;
        }
    }
    const std::vector<nlohmann::json> covers = covers_json(manga);
    for (std::size_t i = 0; i < covers.size(); ++i) {
        const std::string name = std::format("offset-{}.json", i * default_cover_limit);
        if (const auto ok = detail::write_json(api / "cover" / name, covers[i]); !ok) {
            return ok;
        }
    }
    for (const FixtureChapter& chapter : manga.chapters) {
        if (const auto ok = detail::write_chapter_files(root, chapter); !ok) {
            return ok;
        }
    }
    for (const FixtureCover& cover : manga.covers) {
        if (const auto ok = detail::write_file(root / "uploads" / "covers" / manga.id / cover.file,
                                               image_bytes(cover.image));
            !ok) {
            return ok;
        }
    }
    return {};
}

// ---- canned fixtures ----------------------------------------------------------------------------

namespace detail {

enum class IdKind : std::uint16_t { chapter = 1, group = 2, author = 3, artist = 4, cover = 5 };

/// A deterministic v4-shaped UUID: `dddddddd-000k-4000-8000-<n as 12 hex digits>`.
[[nodiscard]] inline std::string fixture_uuid(char digit, IdKind kind, std::uint64_t n) {
    return std::format("{0}{0}{0}{0}{0}{0}{0}{0}-{1:04x}-4000-8000-{2:012x}", digit,
                       std::to_underlying(kind), n);
}

/// A deterministic 32-hex-digit at-home hash.
[[nodiscard]] inline std::string fixture_hash(char digit, std::uint64_t n) {
    return std::format("{0}{0}{0}{0}{0}{0}{0}{0}{1:024x}", digit, n);
}

/// 2020-01-01T00:00:00+00:00 plus `days`, in MangaDex's timestamp form.
[[nodiscard]] inline std::string fixture_timestamp(int days) {
    const std::chrono::sys_days day =
        std::chrono::sys_days{std::chrono::year{2020} / std::chrono::January / 1} +
        std::chrono::days{days};
    return std::format("{:%F}T00:00:00+00:00", day);
}

/// Fills a chapter's id, hash (only when it has images), publishAt and updatedAt from its index.
[[nodiscard]] inline FixtureChapter numbered(char digit, std::uint64_t n, FixtureChapter chapter) {
    chapter.id = fixture_uuid(digit, IdKind::chapter, n);
    if (chapter.hash.empty() && !chapter.images.empty()) {
        chapter.hash = fixture_hash(digit, n);
    }
    const int day = static_cast<int>(n % 3000U);
    chapter.publish_at = fixture_timestamp(day);
    chapter.updated_at = fixture_timestamp(day + 30);
    return chapter;
}

[[nodiscard]] inline PageImage img(ImageKind kind, std::uint16_t width = 8,
                                   std::uint16_t height = 12) {
    return PageImage{.kind = kind, .width = width, .height = height};
}

} // namespace detail

/// 11111111-...: "Clean Title" (original ja). Vol 1 = ch 1-2, vol 2 = ch 3, all en, one group,
/// PNG and GIF pages only, a cover per volume. Nothing is skipped and nothing warns.
[[nodiscard]] inline FixtureManga clean_fixture() {
    using detail::IdKind;
    using detail::img;
    constexpr char d = '1';
    const std::string group = detail::fixture_uuid(d, IdKind::group, 1);
    FixtureManga m{
        .id = "11111111-1111-4111-8111-111111111111",
        .title_en = "Clean Title",
        .alt_titles = {{.lang = "ja", .title = "クリーン・タイトル"},
                       {.lang = "ja-ro", .title = "Kuriin Taitoru"}},
        .original_language = "ja",
        .authors = {{.id = detail::fixture_uuid(d, IdKind::author, 1), .name = "Clean Author"}},
        .artists = {{.id = detail::fixture_uuid(d, IdKind::artist, 1), .name = "Clean Artist"}},
        .groups = {{.id = group, .name = "Clean Scans"}},
    };
    m.chapters = {
        detail::numbered(
            d, 1,
            {.volume = "1",
             .chapter = "1",
             .title = "Beginnings",
             .group_ids = {group},
             .images = {img(ImageKind::png), img(ImageKind::gif), img(ImageKind::png, 12, 8)}}),
        detail::numbered(d, 2,
                         {.volume = "1",
                          .chapter = "2",
                          .title = "Middles",
                          .group_ids = {group},
                          .images = {img(ImageKind::gif), img(ImageKind::png)}}),
        detail::numbered(d, 3,
                         {.volume = "2",
                          .chapter = "3",
                          .title = "Endings",
                          .group_ids = {group},
                          .images = {img(ImageKind::png), img(ImageKind::png)}}),
    };
    m.covers = {
        {.id = detail::fixture_uuid(d, IdKind::cover, 1),
         .volume = "1",
         .file = "cover-v1.png",
         .locale = "ja"},
        {.id = detail::fixture_uuid(d, IdKind::cover, 2),
         .volume = "2",
         .file = "cover-v2.png",
         .locale = "ja"},
    };
    return m;
}

/// 22222222-...: "Basic Title", two groups. Exercises every planner rule:
/// - vol 1 ch 1 by both groups (dedupe: Alpha covers more keys), plus an `fr` copy;
/// - vol 1 ch 2 with a WebP page (warning) and a JPEG page in ch 1;
/// - vol 2: ch 3 external, ch 4 `pages == 0`, ch 5 `isUnavailable`, ch 6 downloadable;
/// - loose chapters 10 and 10.5, and a oneshot (no volume, no chapter);
/// - covers: vol 1 (ja and en), vol 2 with `locale: null`, and one with `volume: null`.
[[nodiscard]] inline FixtureManga basic_fixture() {
    using detail::IdKind;
    using detail::img;
    constexpr char d = '2';
    const std::string alpha = detail::fixture_uuid(d, IdKind::group, 1);
    const std::string beta = detail::fixture_uuid(d, IdKind::group, 2);
    const FixtureCreator creator{.id = detail::fixture_uuid(d, IdKind::author, 1),
                                 .name = "Basic Mangaka"};
    FixtureManga m{
        .id = "22222222-2222-4222-8222-222222222222",
        .title_en = "Basic Title",
        .alt_titles = {{.lang = "ja-ro", .title = "Kihon Taitoru"}},
        .original_language = "ja",
        .authors = {creator},
        .artists = {creator},
        .groups = {{.id = alpha, .name = "Alpha Scans"}, {.id = beta, .name = "Beta Scans"}},
    };
    m.chapters = {
        detail::numbered(
            d, 1,
            {.volume = "1",
             .chapter = "1",
             .title = "Alpha One",
             .group_ids = {alpha},
             .images = {img(ImageKind::jpeg_header), img(ImageKind::png), img(ImageKind::gif)}}),
        detail::numbered(d, 2,
                         {.volume = "1",
                          .chapter = "1",
                          .title = "Beta One",
                          .group_ids = {beta},
                          .images = {img(ImageKind::png), img(ImageKind::png)}}),
        detail::numbered(d, 3,
                         {.volume = "1",
                          .chapter = "2",
                          .title = "Alpha Two",
                          .group_ids = {alpha},
                          .images = {img(ImageKind::webp_header), img(ImageKind::png)}}),
        detail::numbered(d, 4,
                         {.volume = "2",
                          .chapter = "3",
                          .title = "Elsewhere",
                          .group_ids = {alpha},
                          .pages = 0,
                          .external_url = "https://example.invalid/chapter/3"}),
        detail::numbered(
            d, 5,
            {.volume = "2", .chapter = "4", .title = "Empty", .group_ids = {alpha}, .pages = 0}),
        detail::numbered(d, 6,
                         {.volume = "2",
                          .chapter = "5",
                          .title = "Withdrawn",
                          .group_ids = {alpha},
                          .unavailable = true,
                          .images = {img(ImageKind::png), img(ImageKind::png)}}),
        detail::numbered(d, 7,
                         {.volume = "2",
                          .chapter = "6",
                          .title = "Alpha Six",
                          .group_ids = {alpha},
                          .images = {img(ImageKind::png), img(ImageKind::gif)}}),
        detail::numbered(d, 8,
                         {.chapter = "10",
                          .title = "Loose Ten",
                          .group_ids = {alpha},
                          .images = {img(ImageKind::png)}}),
        detail::numbered(d, 9,
                         {.chapter = "10.5",
                          .title = "Loose Ten and a Half",
                          .group_ids = {alpha},
                          .images = {img(ImageKind::gif)}}),
        detail::numbered(d, 10,
                         {.title = "Special Oneshot",
                          .group_ids = {beta},
                          .images = {img(ImageKind::png), img(ImageKind::png)}}),
        detail::numbered(d, 11,
                         {.volume = "1",
                          .chapter = "1",
                          .title = "Alpha Un",
                          .lang = "fr",
                          .group_ids = {alpha},
                          .images = {img(ImageKind::png)}}),
    };
    m.covers = {
        {.id = detail::fixture_uuid(d, IdKind::cover, 1),
         .volume = "1",
         .file = "cover-v1-ja.png",
         .locale = "ja"},
        {.id = detail::fixture_uuid(d, IdKind::cover, 2),
         .volume = "1",
         .file = "cover-v1-en.png",
         .locale = "en"},
        {.id = detail::fixture_uuid(d, IdKind::cover, 3),
         .volume = "2",
         .file = "cover-v2-nolocale.png",
         .locale = std::nullopt},
        {.id = detail::fixture_uuid(d, IdKind::cover, 4),
         .volume = std::nullopt,
         .file = "cover-novolume.png",
         .locale = "ja"},
    };
    return m;
}

inline constexpr std::size_t bulk_chapter_count = 4000;

/// 33333333-...: a long title and 4000 loose en chapters (1 .. 4000), one page each but no images
/// and no at-home documents. For dry runs only: the planned file list is well over 128 KiB.
[[nodiscard]] inline FixtureManga bulk_fixture() {
    using detail::IdKind;
    constexpr char d = '3';
    const std::string group = detail::fixture_uuid(d, IdKind::group, 1);
    FixtureManga m{
        .id = "33333333-3333-4333-8333-333333333333",
        .title_en = "Bulk Title That Is Deliberately Long So That A Dry Run Of Four Thousand "
                    "Chapters Fills Any Pipe",
        .original_language = "ja",
        .groups = {{.id = group, .name = "Bulk Scans"}},
    };
    m.chapters.reserve(bulk_chapter_count);
    for (std::size_t i = 1; i <= bulk_chapter_count; ++i) {
        m.chapters.push_back(detail::numbered(
            d, i, {.chapter = std::to_string(i), .group_ids = {group}, .pages = 1}));
    }
    return m;
}

/// 44444444-...: vol 1 (ch 1) is complete; vol 2 (ch 2) lists a page whose file is missing, so its
/// EPUB fails while vol 1's is written.
[[nodiscard]] inline FixtureManga incomplete_fixture() {
    using detail::IdKind;
    using detail::img;
    constexpr char d = '4';
    const std::string group = detail::fixture_uuid(d, IdKind::group, 1);
    PageImage missing = img(ImageKind::png);
    missing.missing = true;
    FixtureManga m{
        .id = "44444444-4444-4444-8444-444444444444",
        .title_en = "Incomplete Title",
        .original_language = "ja",
        .groups = {{.id = group, .name = "Incomplete Scans"}},
    };
    m.chapters = {
        detail::numbered(d, 1,
                         {.volume = "1",
                          .chapter = "1",
                          .title = "Whole",
                          .group_ids = {group},
                          .images = {img(ImageKind::png), img(ImageKind::png)}}),
        detail::numbered(d, 2,
                         {.volume = "2",
                          .chapter = "2",
                          .title = "Holey",
                          .group_ids = {group},
                          .images = {img(ImageKind::png), missing}}),
    };
    m.covers = {{.id = detail::fixture_uuid(d, IdKind::cover, 1),
                 .volume = "1",
                 .file = "cover-v1.png",
                 .locale = "ja"}};
    return m;
}

/// 55555555-...: only `de` chapters, so an `en` download finds nothing to do.
[[nodiscard]] inline FixtureManga nochapters_fixture() {
    using detail::IdKind;
    using detail::img;
    constexpr char d = '5';
    const std::string group = detail::fixture_uuid(d, IdKind::group, 1);
    FixtureManga m{
        .id = "55555555-5555-4555-8555-555555555555",
        .title_en = "No Chapters Title",
        .original_language = "ko",
        .groups = {{.id = group, .name = "Deutsche Scans"}},
    };
    m.chapters = {
        detail::numbered(d, 1,
                         {.volume = "1",
                          .chapter = "1",
                          .title = "Eins",
                          .lang = "de",
                          .group_ids = {group},
                          .images = {img(ImageKind::png)}}),
        detail::numbered(d, 2,
                         {.volume = "1",
                          .chapter = "2",
                          .title = "Zwei",
                          .lang = "de",
                          .group_ids = {group},
                          .images = {img(ImageKind::png)}}),
    };
    return m;
}

struct NamedFixture {
    /// Always a string literal (static storage), so the view never dangles.
    std::string_view name{};
    FixtureManga (*make)() = nullptr;
};

/// The five trees fixture_gen writes, by directory name.
[[nodiscard]] inline std::vector<NamedFixture> all_fixtures() {
    return {{.name = "clean", .make = &clean_fixture},
            {.name = "basic", .make = &basic_fixture},
            {.name = "bulk", .make = &bulk_fixture},
            {.name = "incomplete", .make = &incomplete_fixture},
            {.name = "nochapters", .make = &nochapters_fixture}};
}

} // namespace mangadex_dl::test::mdx
