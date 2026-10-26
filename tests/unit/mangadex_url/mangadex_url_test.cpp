// tests/unit/mangadex_url/mangadex_url_test.cpp: the shared mangadex.org URL walk (list plan T2).
// MangaId's full spelling matrix stays in tests/unit/manga_id/; this file proves the path
// parameter, the borrow and lowercase_ascii.
#include "mangadex_dl/attributes.hpp"
#include "mangadex_url.hpp" // core-private (src/core is on the test include path)

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace detail = mangadex_dl::detail;

namespace {

constexpr std::string_view lower_uuid = "a1b2c3d4-e5f6-4789-8abc-def012345678";
constexpr std::string_view upper_uuid = "A1B2C3D4-E5F6-4789-8ABC-DEF012345678";
constexpr std::string_view title_path = "/title/";
constexpr std::string_view list_path = "/list/";

[[nodiscard]] std::string with_uuid(std::string_view prefix, std::string_view suffix = "",
                                    std::string_view uuid = lower_uuid) {
    std::string text{prefix};
    text += uuid;
    text += suffix;
    return text;
}

/// True when `part` lies entirely inside `whole` (pointer order via std::less, which is total).
[[nodiscard]] bool views_into(std::string_view part, std::string_view whole) {
    const std::less<const char*> before{};
    return !before(std::to_address(part.begin()), std::to_address(whole.begin())) &&
           !before(std::to_address(whole.end()), std::to_address(part.end()));
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

TEST_CASE("mangadex url: a bare UUID is accepted for either path, as the input itself",
          "[mangadex_url]") {
    const std::string_view path = GENERATE(title_path, list_path);
    const std::string text{GENERATE(lower_uuid, upper_uuid)};
    CAPTURE(path, text);
    const std::optional<std::string_view> uuid = detail::extract_mangadex_uuid(text, path);
    REQUIRE(uuid.has_value()); // value_or below: clang-tidy cannot see REQUIRE's early exit
    const std::string_view got = uuid.value_or(std::string_view{});
    CHECK(got == text); // original case kept
    CHECK(got.data() == text.data());
    CHECK(got.size() == text.size());
}

TEST_CASE("mangadex url: each path accepts its own URL spellings", "[mangadex_url]") {
    const std::string_view path = GENERATE(title_path, list_path);
    const std::string base = std::string{"https://mangadex.org"} + std::string{path};
    const std::string www = std::string{"www.mangadex.org"} + std::string{path};
    const std::string shouty = std::string{"HTTP://WWW.MangaDex.ORG"} + std::string{path};
    const std::string text =
        GENERATE_COPY(with_uuid(base), with_uuid(base, "/"), with_uuid(base, "/some-slug"),
                      with_uuid(base, "/some-slug/?tab=1#x"), with_uuid(base, "?q=a/b"),
                      with_uuid(base, "#frag/with/slashes"), with_uuid(www, "/slug"),
                      with_uuid(shouty, "", upper_uuid));
    CAPTURE(path, text);
    const std::optional<std::string_view> uuid = detail::extract_mangadex_uuid(text, path);
    REQUIRE(uuid.has_value()); // value_or below: clang-tidy cannot see REQUIRE's early exit
    const std::string_view got = uuid.value_or(std::string_view{});
    CHECK(detail::lowercase_ascii(got) == lower_uuid);
    CHECK(views_into(got, text));
}

TEST_CASE("mangadex url: the title path rejects list URLs and vice versa", "[mangadex_url]") {
    const std::string suffix = GENERATE(std::string{}, std::string{"/slug"}, std::string{"?q"});
    CAPTURE(suffix);
    const std::string title_url = with_uuid("https://mangadex.org/title/", suffix);
    const std::string list_url = with_uuid("https://mangadex.org/list/", suffix);
    CHECK(detail::extract_mangadex_uuid(title_url, title_path).has_value());
    CHECK_FALSE(detail::extract_mangadex_uuid(title_url, list_path).has_value());
    CHECK(detail::extract_mangadex_uuid(list_url, list_path).has_value());
    CHECK_FALSE(detail::extract_mangadex_uuid(list_url, title_path).has_value());
}

TEST_CASE("mangadex url: the path is matched case-sensitively", "[mangadex_url]") {
    CHECK_FALSE(detail::extract_mangadex_uuid(with_uuid("https://mangadex.org/LIST/"), list_path)
                    .has_value());
    CHECK_FALSE(detail::extract_mangadex_uuid(with_uuid("https://mangadex.org/Title/"), title_path)
                    .has_value());
}

TEST_CASE("mangadex url: the list path keeps the title path's rejections", "[mangadex_url]") {
    const std::string text = GENERATE(
        std::string{}, std::string{"not-a-uuid"}, with_uuid(" "), with_uuid("", "\n"),
        with_uuid("", "/"), with_uuid("https://example.org/list/"),
        with_uuid("https://evilmangadex.org/list/"), with_uuid("https://mangadex.org:443/list/"),
        with_uuid("https://user@mangadex.org/list/"), with_uuid("ftp://mangadex.org/list/"),
        with_uuid("//mangadex.org/list/"), with_uuid("https://mangadex.org/list//"),
        with_uuid("https://mangadex.org/list/", "/slug/extra"),
        with_uuid("https://mangadex.org/list/", "/slug//"),
        with_uuid("https://mangadex.org/list/", "x"),
        with_uuid("https://mangadex.org/list/", "/sl ug"),
        with_uuid("https://mangadex.org/list/", "/slug\x7f"),
        std::string{"https://mangadex.org/list/a1b2c3d4-e5f6-4789-8abc-def01234567"},
        std::string{"https://mangadex.org/list/"});
    CAPTURE(text);
    CHECK_FALSE(detail::extract_mangadex_uuid(text, list_path).has_value());
}

TEST_CASE("mangadex url: the walk never reads past the end of its view", "[mangadex_url][bounds]") {
    const std::string_view path = GENERATE(title_path, list_path);
    const std::string prefix = std::string{"https://www.mangadex.org"} + std::string{path};
    const std::string url = with_uuid(prefix, "/some-slug/?tab=art#top", upper_uuid);
    CAPTURE(path);
    for (std::size_t length = 0; length <= url.size(); ++length) {
        CAPTURE(length);
        const ExactBuffer buffer{std::string_view{url}.substr(0, length)};
        const std::optional<std::string_view> uuid =
            detail::extract_mangadex_uuid(buffer.view(), path);
        // Every prefix that contains the whole UUID is an accepted spelling; shorter ones are not.
        const bool complete = length >= prefix.size() + upper_uuid.size();
        REQUIRE(uuid.has_value() == complete);
        if (uuid.has_value()) {
            CHECK(*uuid == upper_uuid);
            CHECK(views_into(*uuid, buffer.view()));
        }
    }
}

TEST_CASE("mangadex url: lowercase_ascii maps only A-Z", "[mangadex_url]") {
    CHECK(detail::lowercase_ascii("").empty());
    CHECK(detail::lowercase_ascii(upper_uuid) == lower_uuid);
    CHECK(detail::lowercase_ascii("AZaz09@[`{-/") == "azaz09@[`{-/");
    CHECK(detail::lowercase_ascii("\xC3\x89T\xC3\xA9") == "\xC3\x89t\xC3\xA9"); // non-ASCII kept
    const std::string with_nul{"A\0B", 3};
    CHECK(detail::lowercase_ascii(with_nul) == std::string{"a\0b", 3});
}
