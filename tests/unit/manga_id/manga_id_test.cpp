// tests/unit/manga_id/manga_id_test.cpp: MANGA parsing (download spec acceptance criterion 1).
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view lower_uuid = "a1b2c3d4-e5f6-4789-8abc-def012345678";
constexpr std::string_view upper_uuid = "A1B2C3D4-E5F6-4789-8ABC-DEF012345678";
constexpr std::string_view invalid_manga_message =
    "MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL";

[[nodiscard]] std::string with_uuid(std::string_view prefix, std::string_view suffix = "",
                                    std::string_view uuid = lower_uuid) {
    std::string text{prefix};
    text += uuid;
    text += suffix;
    return text;
}

/// A heap copy of `text` with no terminator: the last byte of the view is the last byte of the
/// allocation, so ASan reports any read one past the end of the view.
class ExactBuffer {
    // An exact-size heap array is the point: std::string adds a NUL and std::vector may
    // over-allocate, either of which would hide a one-past-end read from ASan.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): exact-size alloc
    using Bytes = char[];

public:
    explicit ExactBuffer(std::string_view text)
        : size_(text.size()), bytes_(std::make_unique<Bytes>(text.size())) {
        std::ranges::copy(text, bytes_.get());
    }

    [[nodiscard]] std::string_view view() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return {bytes_.get(), size_};
    }

private:
    std::size_t size_ = 0;
    std::unique_ptr<Bytes> bytes_{};
};

} // namespace

TEST_CASE("MangaId accepts a bare UUID in either case and lowercases it", "[manga_id]") {
    const std::string text{
        GENERATE(lower_uuid, upper_uuid, "A1b2C3d4-E5f6-4789-8aBc-DeF012345678")};
    CAPTURE(text);
    const md::Result<md::MangaId> id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    CHECK(id->str() == lower_uuid);
}

TEST_CASE("MangaId accepts every documented title URL spelling", "[manga_id]") {
    const std::string text = GENERATE(
        with_uuid("https://mangadex.org/title/"), with_uuid("http://mangadex.org/title/"),
        with_uuid("https://www.mangadex.org/title/"), with_uuid("http://www.mangadex.org/title/"),
        with_uuid("mangadex.org/title/"), with_uuid("www.mangadex.org/title/"),
        with_uuid("https://mangadex.org/title/", "/"),
        with_uuid("https://mangadex.org/title/", "/some-manga-slug"),
        with_uuid("https://mangadex.org/title/", "/some-manga-slug/"),
        with_uuid("https://mangadex.org/title/", "?tab=chapters"),
        with_uuid("https://mangadex.org/title/", "#top"),
        with_uuid("https://mangadex.org/title/", "/slug?tab=art#x"),
        with_uuid("https://mangadex.org/title/", "/slug/?q"),
        with_uuid("https://mangadex.org/title/", "/?q=a/b"),
        with_uuid("https://mangadex.org/title/", "#frag/with/slashes"),
        with_uuid("HTTPS://WWW.MangaDex.ORG/title/", "", upper_uuid));
    CAPTURE(text);
    const md::Result<md::MangaId> id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    CHECK(id->str() == lower_uuid);
    CHECK(md::looks_like_manga_id(text));
}

TEST_CASE("MangaId rejects anything else as a usage error", "[manga_id]") {
    const std::string text = GENERATE(
        std::string{}, std::string{"not-a-uuid"},
        std::string{"a1b2c3d4-e5f6-4789-8abc-def01234567"},   // short (35 bytes)
        std::string{"a1b2c3d4-e5f6-4789-8abc-def0123456789"}, // long (37 bytes)
        std::string{"a1b2c3d4e5f6-4789-8abc-def012345678-"},  // dashes in the wrong places
        std::string{"g1b2c3d4-e5f6-4789-8abc-def012345678"},  // not hex
        with_uuid(" "), with_uuid("", "\n"), with_uuid("", "/"),
        with_uuid("https://example.org/title/"), with_uuid("https://mangadex.com/title/"),
        with_uuid("https://evilmangadex.org/title/"),
        with_uuid("https://mangadex.org.evil.example/title/"),
        with_uuid("https://mangadex.org:443/title/"), with_uuid("https://user@mangadex.org/title/"),
        with_uuid("ftp://mangadex.org/title/"), with_uuid("//mangadex.org/title/"),
        with_uuid("https://mangadex.org/chapter/"), with_uuid("https://mangadex.org/title//"),
        with_uuid("https://mangadex.org/"), with_uuid("https://mangadex.org/title/", "/slug/extra"),
        with_uuid("https://mangadex.org/title/", "/slug//"),
        with_uuid("https://mangadex.org/title/", "//"),
        with_uuid("https://mangadex.org/title/", "x"),
        with_uuid("https://mangadex.org/title/", "/sl ug"),
        with_uuid("https://mangadex.org/title/", "/slug\x7f"),
        std::string{"https://mangadex.org/title/a1b2c3d4-e5f6-4789-8abc-def01234567"},
        std::string{"https://mangadex.org/title/"});
    CAPTURE(text);
    const md::Result<md::MangaId> id = md::MangaId::parse(text);
    REQUIRE_FALSE(id.has_value());
    CHECK(id.error().code == md::Errc::usage);
    CHECK(id.error().message == invalid_manga_message);
    CHECK_FALSE(md::looks_like_manga_id(text));
}

TEST_CASE("MangaId compares by canonical value", "[manga_id]") {
    const md::Result<md::MangaId> lower = md::MangaId::parse(lower_uuid);
    const md::Result<md::MangaId> upper =
        md::MangaId::parse(with_uuid("mangadex.org/title/", "", upper_uuid));
    const md::Result<md::MangaId> other =
        md::MangaId::parse("b1b2c3d4-e5f6-4789-8abc-def012345678");
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());
    REQUIRE(other.has_value());
    CHECK(*lower == *upper);
    CHECK(*lower != *other);
    CHECK(*lower < *other);
    CHECK(md::MangaId{} < *lower); // the nil placeholder sorts first
}

TEST_CASE("is_uuid checks the 8-4-4-4-12 hex shape only", "[manga_id]") {
    CHECK(md::is_uuid(lower_uuid));
    CHECK(md::is_uuid(upper_uuid));
    CHECK(md::is_uuid("00000000-0000-0000-0000-000000000000"));
    CHECK_FALSE(md::is_uuid(""));
    CHECK_FALSE(md::is_uuid(with_uuid("https://mangadex.org/title/")));
    CHECK_FALSE(md::is_uuid("a1b2c3d4-e5f6-4789-8abc_def012345678"));
    CHECK_FALSE(md::is_uuid("a1b2c3d4-e5f6-4789-8abc-def01234567z"));
    CHECK_FALSE(md::is_uuid("a1b2c3d4-e5f6-4789-8abc-def012345678 "));
    CHECK_FALSE(md::is_uuid("a1b2c3d4-e5f6-4789-8abc-def012345678/"));
}

TEST_CASE("looks_like_manga_id is false for ordinary directory names", "[manga_id]") {
    CHECK_FALSE(md::looks_like_manga_id("."));
    CHECK_FALSE(md::looks_like_manga_id("out"));
    CHECK_FALSE(md::looks_like_manga_id("/home/user/manga"));
    CHECK_FALSE(md::looks_like_manga_id("mangadex.org"));
    CHECK(md::looks_like_manga_id(upper_uuid));
}

TEST_CASE("is_uuid never reads past the end of its view", "[manga_id][bounds]") {
    for (std::size_t length = 0; length <= lower_uuid.size(); ++length) {
        CAPTURE(length);
        const ExactBuffer buffer{lower_uuid.substr(0, length)};
        CHECK(md::is_uuid(buffer.view()) == (length == lower_uuid.size()));
    }
}

TEST_CASE("MangaId::parse never reads past the end of its view", "[manga_id][bounds]") {
    constexpr std::string_view prefix = "https://www.mangadex.org/title/";
    const std::string url = with_uuid(prefix, "/some-slug/?tab=art#top", upper_uuid);
    for (std::size_t length = 0; length <= url.size(); ++length) {
        CAPTURE(length);
        const ExactBuffer buffer{std::string_view{url}.substr(0, length)};
        const md::Result<md::MangaId> id = md::MangaId::parse(buffer.view());
        // Every prefix that contains the whole UUID is an accepted spelling; shorter ones are not.
        const bool complete = length >= prefix.size() + lower_uuid.size();
        REQUIRE(id.has_value() == complete);
        if (id.has_value()) {
            CHECK(id->str() == lower_uuid);
        } else {
            CHECK(id.error().code == md::Errc::usage);
        }
        CHECK(md::looks_like_manga_id(buffer.view()) == complete);
    }
}
