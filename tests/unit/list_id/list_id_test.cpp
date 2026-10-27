// tests/unit/list_id/list_id_test.cpp: LIST parsing (list spec "LIST forms", AC1 and AC2; list
// plan T4), plus the `download <list URL>` hint in validate_download_args (AC15, unit side).
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/options.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view lower_uuid = "a1b2c3d4-e5f6-4789-8abc-def012345678";
constexpr std::string_view upper_uuid = "A1B2C3D4-E5F6-4789-8ABC-DEF012345678";
constexpr std::string_view mixed_uuid = "A1b2C3d4-E5f6-4789-8aBc-DeF012345678";

constexpr std::string_view title_message =
    "LIST is a MangaDex title, not a list; use 'mangadex-dl download <title>' for one title";
constexpr std::string_view invalid_message =
    "LIST must be a MangaDex list UUID or a https://mangadex.org/list/<uuid> URL; for a CSV file "
    "of titles use 'mangadex-dl batch <file>'";
constexpr std::string_view download_list_message =
    "MANGA is a MangaDex list, not a title; use 'mangadex-dl mdlist <list>' to download every "
    "title in it";
constexpr std::string_view invalid_manga_message =
    "MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL";

[[nodiscard]] std::string with_uuid(std::string_view prefix, std::string_view suffix = "",
                                    std::string_view uuid = lower_uuid) {
    std::string text{prefix};
    text += uuid;
    text += suffix;
    return text;
}

/// Every spelling ListId::parse accepts (list spec "LIST forms"); each yields lower_uuid.
[[nodiscard]] std::vector<std::string> accepted_spellings() {
    return {
        std::string{lower_uuid},
        std::string{upper_uuid},
        std::string{mixed_uuid},
        with_uuid("https://mangadex.org/list/"),
        with_uuid("http://mangadex.org/list/"),
        with_uuid("mangadex.org/list/"),
        with_uuid("https://www.mangadex.org/list/"),
        with_uuid("http://www.mangadex.org/list/"),
        with_uuid("www.mangadex.org/list/"),
        with_uuid("https://MangaDex.ORG/list/"),
        with_uuid("HTTPS://WWW.MANGADEX.ORG/list/", "", upper_uuid),
        with_uuid("https://mangadex.org/list/", "", mixed_uuid),
        with_uuid("https://mangadex.org/list/", "/"), // V5: a non-Latin name gives an empty slug
        with_uuid("https://mangadex.org/list/", "/my-favourite-titles"),
        with_uuid("https://mangadex.org/list/", "/my-favourite-titles/"),
        with_uuid("https://mangadex.org/list/", "?q"),
        with_uuid("https://mangadex.org/list/", "#f"),
        with_uuid("https://mangadex.org/list/", "/slug?tab=x#y"),
        with_uuid("https://mangadex.org/list/", "/slug/?q"),
        with_uuid("https://mangadex.org/list/", "/?q=a/b"),
        with_uuid("https://mangadex.org/list/", "#frag/with/slashes"),
        with_uuid("https://mangadex.org/list/", "/", upper_uuid),
        with_uuid("https://mangadex.org/list/", "/caf\xC3\xA9"), // a non-ASCII UTF-8 slug
    };
}

/// Spellings that are neither a list nor a title: the generic message.
[[nodiscard]] std::vector<std::string> rejected_spellings() {
    return {
        std::string{},
        std::string{"-"},
        std::string{"titles.csv"},
        std::string{"./titles.csv"},
        std::string{"not-a-list"},
        std::string{"a1b2c3d4-e5f6-4789-8abc-def01234567"},   // 35 characters
        std::string{"a1b2c3d4-e5f6-4789-8abc-def0123456789"}, // 37 characters
        std::string{"g1b2c3d4-e5f6-4789-8abc-def012345678"},  // not hex
        with_uuid(" "),
        with_uuid("", " "),
        with_uuid("", "\n"),
        with_uuid("\t"),
        with_uuid("https://mangadex.org/list/", "/a/b"),
        with_uuid("https://mangadex.org/list/", "//"),
        with_uuid("https://mangadex.org/list/", "/slug//"),
        with_uuid("https://mangadex.org/list/", "x"),
        with_uuid("https://mangadex.org/list/", "/sl ug"),
        with_uuid("https://mangadex.org/list/", "/slug\x7f"),
        with_uuid("https://mangadex.org/list/", "/slug\x01"),
        with_uuid("https://mangadex.org/list/", std::string{"/a\0b", 4}), // an embedded NUL
        with_uuid("https://mangadex.org/list//"),
        std::string{"https://mangadex.org/list//"},
        with_uuid("https://mangadex.org/LIST/"), // the path is case-sensitive
        with_uuid("https://mangadex.org.evil/list/"),
        with_uuid("https://evilmangadex.org/list/"),
        with_uuid("https://mangadex.com/list/"),
        with_uuid("https://mangadex.org:443/list/"),
        with_uuid("https://user@mangadex.org/list/"),
        with_uuid("ftp://mangadex.org/list/"),
        with_uuid("//mangadex.org/list/"),
        with_uuid("https://mangadex.org/chapter/"),
        with_uuid("https://mangadex.org/"),
        std::string{"https://mangadex.org/list/"},
        std::string{"https://mangadex.org/list/a1b2c3d4-e5f6-4789-8abc-def01234567"},
        std::string{"mangadex.org"},
    };
}

/// Title URLs (looks_like_manga_id && !is_uuid): the "is a title" message.
[[nodiscard]] std::vector<std::string> title_spellings() {
    return {
        with_uuid("https://mangadex.org/title/"),
        with_uuid("https://mangadex.org/title/", "/some-manga-slug"),
        with_uuid("https://mangadex.org/title/", "/"),
        with_uuid("mangadex.org/title/"),
        with_uuid("HTTP://WWW.MANGADEX.ORG/title/", "?tab=chapters", upper_uuid),
    };
}

/// True when `message` holds the UUID in any of the spellings the tables use. No fixed message
/// contains a UUID, so this catches a message built from its input on every table row.
[[nodiscard]] bool contains_uuid(std::string_view message) {
    return message.contains(lower_uuid) || message.contains(upper_uuid) ||
           message.contains(mixed_uuid);
}

/// A token no fixed message contains, for the inputs of the "never echoes" test.
constexpr std::string_view echo_marker = "zz-echo-marker";

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

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

TEST_CASE("list id: every documented spelling parses to the lowercase UUID", "[list_id]") {
    for (const std::string& text : accepted_spellings()) {
        CAPTURE(text);
        const md::Result<md::ListId> id = md::ListId::parse(text);
        REQUIRE(id.has_value());
        CHECK(id->str() == lower_uuid);
    }
}

TEST_CASE("list id: anything else is a usage error with the generic message", "[list_id]") {
    for (const std::string& text : rejected_spellings()) {
        CAPTURE(text);
        const md::Result<md::ListId> id = md::ListId::parse(text);
        REQUIRE(!id.has_value());
        CHECK(id.error().code == md::Errc::usage);
        CHECK(id.error().message == invalid_message);
        CHECK(!contains_uuid(id.error().message));
    }
}

TEST_CASE("list id: a title URL gets the distinct 'is a title' message", "[list_id]") {
    for (const std::string& text : title_spellings()) {
        CAPTURE(text);
        const md::Result<md::ListId> id = md::ListId::parse(text);
        REQUIRE(!id.has_value());
        CHECK(id.error().code == md::Errc::usage);
        CHECK(id.error().message == title_message);
        CHECK(!contains_uuid(id.error().message));
    }
}

TEST_CASE("list id: no message echoes its input", "[list_id]") {
    const std::string marker{echo_marker};
    const std::string text =
        GENERATE_COPY(marker, marker + ".csv", "./" + marker,
                      with_uuid("https://mangadex.org/list/", "/slug/" + marker),
                      with_uuid("https://mangadex.org/title/", "/" + marker),
                      with_uuid("https://mangadex.org/title/", "?" + marker));
    CAPTURE(text);
    const md::Result<md::ListId> id = md::ListId::parse(text);
    REQUIRE(!id.has_value());
    CHECK(!id.error().message.contains(echo_marker));
    CHECK(!contains_uuid(id.error().message));
}

TEST_CASE("list id: a bare UUID is a list spelling, never the title message", "[list_id]") {
    // A bare UUID is ambiguous between a title and a list (list spec "LIST forms"); it parses.
    const md::Result<md::ListId> id = md::ListId::parse(upper_uuid);
    REQUIRE(id.has_value());
    CHECK(id->str() == lower_uuid);
}

TEST_CASE("list id: looks_like_list_id agrees with ListId::parse on every row", "[list_id]") {
    std::vector<std::string> rows = accepted_spellings();
    std::ranges::copy(rejected_spellings(), std::back_inserter(rows));
    std::ranges::copy(title_spellings(), std::back_inserter(rows));
    for (const std::string& text : rows) {
        CAPTURE(text);
        CHECK(md::looks_like_list_id(text) == md::ListId::parse(text).has_value());
    }
}

TEST_CASE("list id: compares by canonical value and defaults to the nil UUID", "[list_id]") {
    const md::Result<md::ListId> lower = md::ListId::parse(lower_uuid);
    const md::Result<md::ListId> upper =
        md::ListId::parse(with_uuid("https://mangadex.org/list/", "/", upper_uuid));
    const md::Result<md::ListId> other = md::ListId::parse("b1b2c3d4-e5f6-4789-8abc-def012345678");
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());
    REQUIRE(other.has_value());
    CHECK(*lower == *upper);
    CHECK(*lower != *other);
    CHECK(*lower < *other);
    const md::ListId nil{};
    CHECK(nil.str() == "00000000-0000-0000-0000-000000000000");
    CHECK(nil < *lower);
}

TEST_CASE("list id: parsing never reads past the end of its view", "[list_id][bounds]") {
    constexpr std::string_view prefix = "https://www.mangadex.org/list/";
    const std::string url = with_uuid(prefix, "/some-slug/?tab=x#top", upper_uuid);
    for (std::size_t length = 0; length <= url.size(); ++length) {
        CAPTURE(length);
        const ExactBuffer buffer{std::string_view{url}.substr(0, length)};
        const md::Result<md::ListId> id = md::ListId::parse(buffer.view());
        // Every prefix that holds the whole UUID is an accepted spelling; shorter ones are not.
        const bool complete = length >= prefix.size() + lower_uuid.size();
        REQUIRE(id.has_value() == complete);
        if (id.has_value()) {
            CHECK(id->str() == lower_uuid);
        } else {
            CHECK(id.error().message == invalid_message);
        }
        CHECK(md::looks_like_list_id(buffer.view()) == complete);
    }
}

TEST_CASE("list id: the parsed id owns its bytes, not the input's", "[list_id][lifetime]") {
    md::Result<md::ListId> id = md::ListId{};
    {
        std::string text = with_uuid("https://mangadex.org/list/", "/slug", upper_uuid);
        id = md::ListId::parse(text);
        std::ranges::fill(text, 'x'); // then the buffer dies at the end of the scope
    }
    REQUIRE(id.has_value());
    CHECK(id->str() == lower_uuid);
}

// --- validate_download_args: the `download <list URL>` hint (AC15) ------------------------------

TEST_CASE("list id: download MANGA that is a list URL gets the mdlist hint",
          "[list_id][download_args]") {
    const std::string manga = GENERATE(with_uuid("https://mangadex.org/list/", "/x"),
                                       with_uuid("https://mangadex.org/list/", "/"),
                                       with_uuid("mangadex.org/list/", "", upper_uuid),
                                       with_uuid("https://www.mangadex.org/list/", "?q"),
                                       with_uuid("https://mangadex.org/list/", "/zz-echo-marker"));
    CAPTURE(manga);
    const md::Result<md::DownloadOptions> options =
        md::validate_download_args(md::RawDownloadArgs{.manga = manga});
    REQUIRE(!options.has_value());
    CHECK(options.error().code == md::Errc::usage);
    CHECK(options.error().message == download_list_message);
    CHECK(!contains_uuid(options.error().message));
    CHECK(!options.error().message.contains(echo_marker));
}

TEST_CASE("list id: every other bad download MANGA keeps today's message",
          "[list_id][download_args]") {
    const std::string manga = GENERATE(
        std::string{}, std::string{"not-a-manga"}, std::string{"titles.csv"},
        with_uuid("https://mangadex.org/list/", "/a/b"), with_uuid("https://mangadex.org/list//"),
        with_uuid("https://mangadex.org.evil/list/"), with_uuid("https://mangadex.org/chapter/"));
    CAPTURE(manga);
    const md::Result<md::DownloadOptions> options =
        md::validate_download_args(md::RawDownloadArgs{.manga = manga});
    REQUIRE(!options.has_value());
    CHECK(options.error().code == md::Errc::usage);
    CHECK(options.error().message == invalid_manga_message);
}

TEST_CASE("list id: a bare UUID or a title URL as download MANGA never gets the list hint",
          "[list_id][download_args]") {
    const std::string manga =
        GENERATE(std::string{upper_uuid}, with_uuid("https://mangadex.org/title/", "/slug"));
    CAPTURE(manga);
    const md::Result<md::DownloadOptions> options =
        md::validate_download_args(md::RawDownloadArgs{.manga = manga});
    REQUIRE(options.has_value());
    CHECK(options->manga.str() == lower_uuid);
}
