// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/image/image_test.cpp: sniff_image (download plan T5, acceptance 7). Every format and
// its dimensions, every truncation length of every header, structural corruption, garbage and
// the side limits. Inputs are copied into exactly sized heap buffers, so a read one byte past the
// end lands in an ASan redzone and fails the test run.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/image.hpp"
#include "support/mdx_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;

/// An exactly sized copy of `text` (no slack capacity for an overread to hide in).
[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] std::string from_octets(std::initializer_list<unsigned> octets) {
    std::string out;
    for (const unsigned o : octets) {
        mdx::detail::put_u8(out, o);
    }
    return out;
}

[[nodiscard]] md::Result<md::ImageInfo> sniff(std::string_view text) {
    const std::vector<std::byte> bytes = to_bytes(text);
    return md::sniff_image(bytes);
}

[[nodiscard]] std::string riff_webp(std::string_view fourcc, std::string_view payload) {
    std::string out{"RIFF"};
    mdx::detail::put_u32_le(out, static_cast<std::uint32_t>(4U + 8U + payload.size()));
    out.append("WEBP");
    out.append(fourcc);
    mdx::detail::put_u32_le(out, static_cast<std::uint32_t>(payload.size()));
    out.append(payload);
    return out;
}

/// Lossy WebP: key-frame tag, start code, 14-bit sides (plus `scale` in the top two bits).
[[nodiscard]] std::string vp8_bytes(std::uint32_t width, std::uint32_t height,
                                    std::uint32_t frame_tag = 0x00012A10U,
                                    std::uint32_t scale = 0) {
    std::string payload;
    mdx::detail::put_u24_le(payload, frame_tag);
    payload.append(from_octets({0x9D, 0x01, 0x2A}));
    mdx::detail::put_u16_le(payload, width | (scale << 14U));
    mdx::detail::put_u16_le(payload, height | (scale << 14U));
    return riff_webp("VP8 ", payload);
}

/// Lossless WebP: signature 0x2F, then (width - 1) | (height - 1) << 14 | version << 29.
[[nodiscard]] std::string vp8l_bytes(std::uint32_t width, std::uint32_t height,
                                     std::uint32_t version = 0) {
    std::string payload = from_octets({0x2F});
    mdx::detail::put_u32_le(payload, (width - 1U) | ((height - 1U) << 14U) | (version << 29U));
    return riff_webp("VP8L", payload);
}

/// VP8X with raw stored (side - 1) values, so out-of-range canvases can be written.
[[nodiscard]] std::string vp8x_raw(std::uint32_t width_minus_one, std::uint32_t height_minus_one) {
    std::string payload;
    mdx::detail::put_u32_le(payload, 0U);
    mdx::detail::put_u24_le(payload, width_minus_one);
    mdx::detail::put_u24_le(payload, height_minus_one);
    return riff_webp("VP8X", payload);
}

/// A PNG signature + IHDR with arbitrary 32-bit sides (only the header).
[[nodiscard]] std::string png_header(std::uint32_t width, std::uint32_t height) {
    std::string ihdr;
    mdx::detail::put_u32_be(ihdr, width);
    mdx::detail::put_u32_be(ihdr, height);
    ihdr.append(from_octets({8, 0, 0, 0, 0}));
    std::string out{"\x89PNG\r\n\x1a\n"};
    mdx::detail::put_png_chunk(out, "IHDR", ihdr);
    return out;
}

[[nodiscard]] std::string gif_header(std::uint32_t width, std::uint32_t height) {
    std::string out{"GIF87a"};
    mdx::detail::put_u16_le(out, width);
    mdx::detail::put_u16_le(out, height);
    out.append(from_octets({0, 0, 0}));
    return out;
}

/// SOI, then `middle` (marker segments), then a frame header with marker `sof`.
[[nodiscard]] std::string jpeg_with(std::string_view middle, std::uint32_t width,
                                    std::uint32_t height, unsigned sof = 0xC0) {
    std::string out = from_octets({0xFF, 0xD8});
    out.append(middle);
    out.append(from_octets({0xFF, sof}));
    mdx::detail::put_u16_be(out, 11U); // one component
    mdx::detail::put_u8(out, 8U);
    mdx::detail::put_u16_be(out, height);
    mdx::detail::put_u16_be(out, width);
    out.append(from_octets({1, 1, 0x11, 0}));
    out.append(from_octets({0xFF, 0xD9}));
    return out;
}

void require_info(const md::Result<md::ImageInfo>& result, md::MediaType type, std::uint32_t width,
                  std::uint32_t height) {
    REQUIRE(result.has_value());
    CHECK(*result == md::ImageInfo{.type = type, .width = width, .height = height});
}

void require_failure(const md::Result<md::ImageInfo>& result) {
    REQUIRE(!result.has_value());
    CHECK(result.error().code == md::Errc::failure);
    CHECK(!result.error().message.empty());
}

struct Sample {
    std::string label{};
    std::string bytes{};
    std::size_t header_size = 0; ///< shortest prefix that sniffs successfully
    md::ImageInfo info{};
};

[[nodiscard]] std::vector<Sample> samples() {
    using md::MediaType;
    return {
        {.label = "png",
         .bytes = mdx::detail::png_bytes(6, 9),
         .header_size = 24,
         .info = {.type = MediaType::png, .width = 6, .height = 9}},
        {.label = "gif",
         .bytes = mdx::detail::gif_bytes(7, 3),
         .header_size = 10,
         .info = {.type = MediaType::gif, .width = 7, .height = 3}},
        {.label = "jpeg",
         .bytes = mdx::detail::jpeg_header_bytes(640, 480),
         .header_size = 29,
         .info = {.type = MediaType::jpeg, .width = 640, .height = 480}},
        {.label = "webp-vp8x",
         .bytes = mdx::detail::webp_header_bytes(800, 1200),
         .header_size = 30,
         .info = {.type = MediaType::webp, .width = 800, .height = 1200}},
        {.label = "webp-vp8",
         .bytes = vp8_bytes(320, 200),
         .header_size = 30,
         .info = {.type = MediaType::webp, .width = 320, .height = 200}},
        {.label = "webp-vp8l",
         .bytes = vp8l_bytes(16384, 1),
         .header_size = 25,
         .info = {.type = MediaType::webp, .width = 16384, .height = 1}},
    };
}

} // namespace

TEST_CASE("sniff_image identifies each format and its dimensions", "[image]") {
    for (const Sample& sample : samples()) {
        INFO(sample.label);
        require_info(sniff(sample.bytes), sample.info.type, sample.info.width, sample.info.height);
    }
    // The fixture builder's page images, through its own dispatcher.
    require_info(sniff(mdx::image_bytes({.kind = mdx::ImageKind::png, .width = 8, .height = 12})),
                 md::MediaType::png, 8, 12);
    require_info(sniff(mdx::image_bytes({.kind = mdx::ImageKind::jpeg_header, .width = 1})),
                 md::MediaType::jpeg, 1, 12);
}

TEST_CASE("sniff_image fails on every truncation of every header without reading past the end",
          "[image][truncation]") {
    for (const Sample& sample : samples()) {
        INFO(sample.label);
        REQUIRE(sample.header_size <= sample.bytes.size());
        for (std::size_t length = 0; length <= sample.bytes.size(); ++length) {
            INFO("prefix length " << length);
            // A fresh, exactly sized heap buffer per length: ASan sees any overread.
            const std::vector<std::byte> prefix =
                to_bytes(std::string_view{sample.bytes}.substr(0, length));
            const md::Result<md::ImageInfo> result = md::sniff_image(prefix);
            if (length < sample.header_size) {
                require_failure(result);
            } else {
                REQUIRE(result.has_value());
                CHECK(*result == sample.info);
            }
        }
    }
}

TEST_CASE("sniff_image names truncation and unknown types in its message", "[image]") {
    const md::Result<md::ImageInfo> cut =
        sniff(mdx::image_bytes({.kind = mdx::ImageKind::truncated_png}));
    require_failure(cut);
    CHECK(cut.error().message == "truncated image/png header");

    const md::Result<md::ImageInfo> garbage =
        sniff(mdx::image_bytes({.kind = mdx::ImageKind::garbage}));
    require_failure(garbage);
    CHECK(garbage.error().message == "unsupported image type");

    const md::Result<md::ImageInfo> empty = md::sniff_image({});
    require_failure(empty);
    CHECK(empty.error().message == "unsupported image type");

    using namespace std::string_view_literals;
    require_failure(sniff("RIFF\x10\0\0\0WAVEfmt "sv)); // a RIFF that is not WebP
    require_failure(sniff("RIFF\x10\0\0"sv));           // RIFF, form type cut off
    require_failure(sniff("\x89PNG\r\n\x1a"sv));        // partial signature
    require_failure(sniff("GIF88a\x01\0\x01\0"sv));     // not a GIF version
}

TEST_CASE("sniff_image enforces the side limits", "[image][limits]") {
    using md::MediaType;
    constexpr std::uint32_t max = md::max_image_side;
    require_info(sniff(png_header(max, max)), MediaType::png, max, max);
    require_info(sniff(png_header(1, 1)), MediaType::png, 1, 1);
    require_failure(sniff(png_header(max + 1, 1)));
    require_failure(sniff(png_header(1, max + 1)));
    require_failure(sniff(png_header(0, 5)));
    require_failure(sniff(png_header(5, 0)));
    require_failure(sniff(png_header(0xFFFFFFFFU, 0xFFFFFFFFU)));

    require_info(sniff(gif_header(max, max)), MediaType::gif, max, max);
    require_failure(sniff(gif_header(0, 5)));
    require_failure(sniff(gif_header(5, 0)));

    require_info(sniff(jpeg_with({}, max, max)), MediaType::jpeg, max, max);
    require_failure(sniff(jpeg_with({}, 5, 0))); // height 0: defined later by DNL, unsupported
    require_failure(sniff(jpeg_with({}, 0, 5)));

    require_info(sniff(vp8x_raw(max - 1, max - 1)), MediaType::webp, max, max);
    require_info(sniff(vp8x_raw(0, 0)), MediaType::webp, 1, 1);
    require_failure(sniff(vp8x_raw(max, 0)));       // 65536 wide
    require_failure(sniff(vp8x_raw(0, max)));       // 65536 high
    require_failure(sniff(vp8x_raw(0xFFFFFFU, 0))); // the 24-bit maximum
    require_failure(sniff(vp8_bytes(0, 10)));       // VP8 can encode a zero side
    require_info(sniff(vp8_bytes(0x3FFF, 7, 0x00012A10U, 3)), MediaType::webp, 0x3FFF, 7);
}

TEST_CASE("sniff_image walks JPEG marker segments", "[image][jpeg]") {
    using md::MediaType;
    SECTION("fill bytes before a marker") {
        // FF FF FF C0: a marker lead byte, two fill bytes, then the SOF0 code.
        require_info(sniff(jpeg_with(from_octets({0xFF, 0xFF}), 3, 4)), MediaType::jpeg, 3, 4);
    }
    SECTION("standalone RSTn and TEM markers are skipped") {
        require_info(sniff(jpeg_with(from_octets({0xFF, 0xD0, 0xFF, 0x01, 0xFF, 0xD7}), 3, 4)),
                     MediaType::jpeg, 3, 4);
    }
    SECTION("DHT, JPG and DAC are not frame headers; SOF2 and SOF15 are") {
        const std::string tables =
            from_octets({0xFF, 0xC4, 0x00, 0x02, 0xFF, 0xC8, 0x00, 0x02, 0xFF, 0xCC, 0x00, 0x02});
        require_info(sniff(jpeg_with(tables, 5, 6)), MediaType::jpeg, 5, 6);
        require_info(sniff(jpeg_with({}, 5, 6, 0xC2)), MediaType::jpeg, 5, 6);
        require_info(sniff(jpeg_with({}, 5, 6, 0xCF)), MediaType::jpeg, 5, 6);
    }
    SECTION("many segments before the frame header") {
        std::string app;
        for (int i = 0; i < 1000; ++i) {
            app.append(from_octets({0xFF, 0xE1, 0x00, 0x05, 'a', 'b', 'c'}));
        }
        require_info(sniff(jpeg_with(app, 9, 10)), MediaType::jpeg, 9, 10);
    }
    SECTION("structural errors") {
        require_failure(sniff(from_octets({0xFF, 0xD8, 0x00})));       // no marker
        require_failure(sniff(from_octets({0xFF, 0xD8, 0xFF, 0x00}))); // stuffed zero
        require_failure(sniff(from_octets({0xFF, 0xD8, 0xFF, 0xD8}))); // second SOI
        require_failure(sniff(from_octets({0xFF, 0xD8, 0xFF, 0xD9}))); // EOI before a frame
        require_failure(sniff(jpeg_with(from_octets({0xFF, 0xDA, 0x00, 0x02}), 1, 1))); // SOS first
        require_failure(sniff(jpeg_with(from_octets({0xFF, 0xE0, 0x00, 0x01}), 1, 1))); // length 1
        require_failure(sniff(jpeg_with(from_octets({0xFF, 0xE0, 0x00, 0x00}), 1, 1))); // length 0
        // A frame header whose length is too short for the dimensions.
        require_failure(sniff(from_octets({0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x07, 8, 0, 1, 0, 1, 1})));
        // A segment that claims to run past the end of the input.
        require_failure(sniff(from_octets({0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xFF, 0x00})));
        require_failure(sniff(from_octets({0xFF, 0xD8, 0xFF, 0xFF, 0xFF}))); // fill to the end
    }
}

TEST_CASE("sniff_image validates WebP chunks", "[image][webp]") {
    require_failure(sniff(vp8_bytes(10, 10, 0x00012A11U))); // not a key frame
    std::string bad_start = vp8_bytes(10, 10);
    bad_start[23] = '\x9C';
    require_failure(sniff(bad_start));
    std::string bad_signature = vp8l_bytes(10, 10);
    bad_signature[20] = '\x2E';
    require_failure(sniff(bad_signature));
    require_failure(sniff(vp8l_bytes(10, 10, 1))); // version must be 0
    require_failure(sniff(riff_webp("VP9 ", std::string(10, '\0'))));
    // Chunk sizes too small for the fields read, even when the bytes are there.
    std::string short_vp8x = mdx::detail::webp_header_bytes(5, 5);
    short_vp8x[16] = '\x09';
    require_failure(sniff(short_vp8x));
    std::string short_vp8 = vp8_bytes(5, 5);
    short_vp8[16] = '\x09';
    require_failure(sniff(short_vp8));
    std::string short_vp8l = vp8l_bytes(5, 5);
    short_vp8l[16] = '\x04';
    require_failure(sniff(short_vp8l));
}

TEST_CASE("sniff_image validates the PNG IHDR chunk", "[image][png]") {
    std::string wrong_type = png_header(4, 4);
    wrong_type[12] = 'X';
    require_failure(sniff(wrong_type));
    std::string wrong_length = png_header(4, 4);
    wrong_length[11] = '\x0E';
    require_failure(sniff(wrong_length));
}

TEST_CASE("sniff_image survives garbage and corrupted headers", "[image][garbage]") {
    // A fixed xorshift32 stream: the inputs are reproducible without a seeded std engine.
    std::uint32_t state = 20261017U;
    const auto next_byte = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return static_cast<char>(static_cast<unsigned char>(state & 0xFFU));
    };

    SECTION("random buffers, raw and behind each magic") {
        using namespace std::string_view_literals;
        const std::vector<std::string> magics{
            std::string{},
            std::string{"\x89PNG\r\n\x1a\n"sv},
            std::string{"GIF89a"sv},
            from_octets({0xFF, 0xD8}),
            std::string{"RIFF\0\0\0\0WEBPVP8 "sv},
            std::string{"RIFF\0\0\0\0WEBPVP8L"sv},
            std::string{"RIFF\0\0\0\0WEBPVP8X"sv},
        };
        for (const std::string& magic : magics) {
            for (std::size_t length = 0; length < 64; ++length) {
                std::string input = magic;
                for (std::size_t i = 0; i < length; ++i) {
                    input.push_back(next_byte());
                }
                const md::Result<md::ImageInfo> result = sniff(input);
                if (result.has_value()) {
                    CHECK(result->width >= 1);
                    CHECK(result->width <= md::max_image_side);
                    CHECK(result->height >= 1);
                    CHECK(result->height <= md::max_image_side);
                } else {
                    CHECK(result.error().code == md::Errc::failure);
                }
            }
        }
    }
    SECTION("every byte of every valid header overwritten") {
        for (const Sample& sample : samples()) {
            INFO(sample.label);
            const std::size_t span = std::min<std::size_t>(sample.bytes.size(), 40);
            for (std::size_t at = 0; at < span; ++at) {
                for (const unsigned value : {0x00U, 0x01U, 0x7FU, 0x80U, 0xFEU, 0xFFU}) {
                    std::string corrupted = sample.bytes;
                    corrupted[at] = static_cast<char>(static_cast<unsigned char>(value));
                    const md::Result<md::ImageInfo> result = sniff(corrupted);
                    if (result.has_value()) {
                        CHECK(result->width >= 1);
                        CHECK(result->width <= md::max_image_side);
                        CHECK(result->height >= 1);
                        CHECK(result->height <= md::max_image_side);
                    }
                }
            }
        }
    }
}

TEST_CASE("media_type_name and file_extension cover every type", "[image]") {
    using md::MediaType;
    CHECK(md::media_type_name(MediaType::jpeg) == "image/jpeg");
    CHECK(md::media_type_name(MediaType::png) == "image/png");
    CHECK(md::media_type_name(MediaType::gif) == "image/gif");
    CHECK(md::media_type_name(MediaType::webp) == "image/webp");
    CHECK(md::file_extension(MediaType::jpeg) == "jpg");
    CHECK(md::file_extension(MediaType::png) == "png");
    CHECK(md::file_extension(MediaType::gif) == "gif");
    CHECK(md::file_extension(MediaType::webp) == "webp");
}
