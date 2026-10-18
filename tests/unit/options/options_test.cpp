// tests/unit/options/options_test.cpp: --api-base, --lang and --title validation, and
// validate_download_args (every semantic usage error and the `report` derivation).
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/selection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view manga_uuid = "a1b2c3d4-e5f6-4789-8abc-def012345678";

[[nodiscard]] md::RawDownloadArgs valid_raw() {
    return md::RawDownloadArgs{.manga = std::string{manga_uuid}};
}

/// The usage-error message of `result`, or "<accepted>" when it holds a value.
template <class T> [[nodiscard]] std::string usage_message(const md::Result<T>& result) {
    if (result.has_value()) {
        return "<accepted>";
    }
    CHECK(result.error().code == md::Errc::usage);
    return result.error().message;
}

[[nodiscard]] std::string args_error(const md::RawDownloadArgs& raw) {
    return usage_message(md::validate_download_args(raw));
}

[[nodiscard]] md::ApiBase api_base(std::string_view text) {
    md::Result<md::ApiBase> parsed = md::ApiBase::parse(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] bool report_for(const md::RawDownloadArgs& raw) {
    const md::Result<md::DownloadOptions> options = md::validate_download_args(raw);
    REQUIRE(options.has_value());
    return options->report;
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

// --- --api-base -------------------------------------------------------------------------------

TEST_CASE("ApiBase recognises the official API in any letter case and with a trailing slash",
          "[options][api_base]") {
    const std::string text = GENERATE(as<std::string>{}, "https://api.mangadex.org",
                                      "https://api.mangadex.org/", "HTTPS://API.MangaDex.ORG//");
    CAPTURE(text);
    const md::ApiBase base = api_base(text);
    CHECK(base.kind() == md::ApiBase::Kind::official);
    CHECK(base.url() == md::default_api_base);
    CHECK(base.fixture_root().empty());
    CHECK(base == md::ApiBase{});
}

TEST_CASE("ApiBase accepts other https bases and loopback http", "[options][api_base]") {
    const md::ApiBase other = api_base("https://Example.COM:8443/api/v5/");
    CHECK(other.kind() == md::ApiBase::Kind::https);
    CHECK(other.url() == "https://example.com:8443/api/v5");
    CHECK(other.fixture_root().empty());

    CHECK(api_base("https://api.mangadex.org:443").kind() == md::ApiBase::Kind::https);
    CHECK(api_base("https://api.mangadex.dev").kind() == md::ApiBase::Kind::https);
    CHECK(api_base("https://[2001:db8::1]/x").url() == "https://[2001:db8::1]/x");

    CHECK(api_base("http://127.0.0.1:1").kind() == md::ApiBase::Kind::http_loopback);
    CHECK(api_base("http://127.0.0.1:1").url() == "http://127.0.0.1:1");
    CHECK(api_base("http://LOCALHOST:8080/").url() == "http://localhost:8080");
    CHECK(api_base("http://[::1]:65535/a").kind() == md::ApiBase::Kind::http_loopback);
}

TEST_CASE("ApiBase accepts a file:// fixture tree with an absolute path", "[options][api_base]") {
    const md::ApiBase fixture = api_base("file:///tmp/fixtures/basic");
    CHECK(fixture.kind() == md::ApiBase::Kind::fixture);
    CHECK(fixture.url() == "file:///tmp/fixtures/basic");
    CHECK(fixture.fixture_root() == std::filesystem::path{"/tmp/fixtures/basic"});

    const md::ApiBase trailing = api_base("FILE:///srv/tree/");
    CHECK(trailing.url() == "file:///srv/tree");
    CHECK(trailing.fixture_root() == std::filesystem::path{"/srv/tree"});
    CHECK(api_base("file:///a.b/c..d/.e").fixture_root() == std::filesystem::path{"/a.b/c..d/.e"});
}

TEST_CASE("ApiBase rejects malformed or unsafe bases with a specific message",
          "[options][api_base]") {
    const auto message = [](std::string_view text) {
        return usage_message(md::ApiBase::parse(text));
    };
    constexpr std::string_view bad_scheme =
        "--api-base must start with https://, http:// (loopback only) or file://";
    constexpr std::string_view bad_file =
        "--api-base file:// needs an absolute directory path without '.', '..' or empty segments";
    constexpr std::string_view not_loopback =
        "--api-base http:// is allowed only for 127.0.0.1, localhost or [::1]";
    constexpr std::string_view bad_host = "--api-base has a missing or invalid host";
    constexpr std::string_view bad_port = "--api-base has an invalid port (expected 1-65535)";

    CHECK(message("") == bad_scheme);
    CHECK(message("api.mangadex.org") == bad_scheme);
    CHECK(message("ftp://example.com") == bad_scheme);
    CHECK(message("https:/example.com") == bad_scheme);
    CHECK(message("http://example.com") == not_loopback);
    CHECK(message("http://127.0.0.2") == not_loopback);
    CHECK(message("http://localhost.example.com") == not_loopback);
    CHECK(message("http://[::2]") == not_loopback);
    CHECK(message("https://user:pw@example.com") == "--api-base must not contain user information");
    CHECK(message("https://example.com/?x=1") == "--api-base must not have a query or fragment");
    CHECK(message("https://example.com#f") == "--api-base must not have a query or fragment");
    CHECK(message("https://exa mple.com") ==
          "--api-base must not contain spaces or control characters");
    CHECK(message("https://example.com\n") ==
          "--api-base must not contain spaces or control characters");
    CHECK(message("https://") == bad_host);
    CHECK(message("https:///path") == bad_host);
    CHECK(message("https://:443") == bad_host);
    CHECK(message("https://-example.com") == bad_host);
    CHECK(message("https://exa_mple.com") == bad_host);
    CHECK(message("https://[::1") == bad_host);
    CHECK(message("https://[]") == bad_host);
    CHECK(message("https://example.com:") == bad_port);
    CHECK(message("https://example.com:0") == bad_port);
    CHECK(message("https://example.com:65536") == bad_port);
    CHECK(message("https://example.com:123456") == bad_port);
    CHECK(message("https://example.com:8a") == bad_port);
    CHECK(message("https://[::1]x") == bad_port);
    CHECK(message("https://example.com/a\\b") == "--api-base has an invalid character in its path");
    CHECK(message("https://example.com/\xc3\xa9") ==
          "--api-base has an invalid character in its path");
    CHECK(message("file://") == bad_file);
    CHECK(message("file:///") == bad_file);
    CHECK(message("file://relative/dir") == bad_file);
    CHECK(message("file://localhost/srv") == bad_file);
    CHECK(message("file:///a/../b") == bad_file);
    CHECK(message("file:///a/./b") == bad_file);
    CHECK(message("file:///a/..") == bad_file);
    CHECK(message("file:///a//b") == bad_file);
    CHECK(message("file:///a//") == bad_file);
    CHECK(message("file:///a?x") == "--api-base must not have a query or fragment");
}

// --- --lang -----------------------------------------------------------------------------------

TEST_CASE("validate_language accepts ^[a-z]{2,3}(-[a-z]{2,4})?$", "[options][lang]") {
    const std::string code = GENERATE(as<std::string>{}, "en", "ja", "fil", "pt-br", "es-la",
                                      "zh-hk", "zh-hant", "ja-ro");
    CAPTURE(code);
    CHECK(md::validate_language(code) == code);
}

TEST_CASE("validate_language rejects everything else", "[options][lang]") {
    const std::string code =
        GENERATE(as<std::string>{}, "", "e", "engl", "EN", "En", "en-", "-br", "en-b", "en-abcde",
                 "en_us", "en-br-x", "e1", "en-B1", " en", "en\n", "\xc3\xa9n");
    CAPTURE(code);
    CHECK(usage_message(md::validate_language(code)) ==
          "--lang must be a lowercase language code such as en, pt-br or es-la");
}

// --- --title ----------------------------------------------------------------------------------

TEST_CASE("validate_title_override trims ASCII whitespace and keeps UTF-8", "[options][title]") {
    CHECK(md::validate_title_override("Title") == std::string{"Title"});
    CHECK(md::validate_title_override("  Two  Words \t\r\n") == std::string{"Two  Words"});
    CHECK(md::validate_title_override("Zo\xc3\xab \xe6\xbc\xab\xe7\x94\xbb") ==
          std::string{"Zo\xc3\xab \xe6\xbc\xab\xe7\x94\xbb"});
    CHECK(md::validate_title_override("\xf0\x9f\x93\x96 \xf4\x8f\xbf\xbf") ==
          std::string{"\xf0\x9f\x93\x96 \xf4\x8f\xbf\xbf"}); // U+1F4D6 and U+10FFFF
    const std::string max(1024, 'x');
    CHECK(md::validate_title_override(max) == max);
    CHECK(md::validate_title_override("  " + max + "  ") ==
          max); // the limit is on the trimmed text
}

TEST_CASE("validate_title_override rejects empty, long, invalid UTF-8 and control characters",
          "[options][title]") {
    const auto message = [](std::string_view title) {
        return usage_message(md::validate_title_override(title));
    };
    CHECK(message("") == "--title must not be empty");
    CHECK(message(" \t\n ") == "--title must not be empty");
    CHECK(message(std::string(1025, 'x')) == "--title is longer than 1024 bytes");
    for (const std::string_view bad : {
             "\xff",             // never valid
             "a\x80",            // lone continuation byte
             "\xc0\xaf",         // overlong '/'
             "\xe0\x80\xaf",     // overlong '/'
             "\xed\xa0\x80",     // UTF-16 surrogate U+D800
             "\xf4\x90\x80\x80", // above U+10FFFF
             "\xe6\xbc",         // truncated sequence
             "\xe6\xbcx",        // bad continuation
         }) {
        CAPTURE(bad);
        CHECK(message(bad) == "--title must be valid UTF-8");
    }
    CHECK(message("a\x01z") == "--title must not contain control characters");
    CHECK(message("a\tb") == "--title must not contain control characters");
    CHECK(message("a\x7f") == "--title must not contain control characters");
    CHECK(message("\x1b[31mred") == "--title must not contain control characters");
}

// --- validate_download_args -------------------------------------------------------------------

TEST_CASE("validate_download_args maps the defaults", "[options][download_args]") {
    const md::Result<md::DownloadOptions> options = md::validate_download_args(valid_raw());
    REQUIRE(options.has_value());
    CHECK(options->manga.str() == manga_uuid);
    CHECK(options->dir == std::filesystem::path{"."});
    CHECK(options->volumes == md::Selection::all());
    CHECK(options->loose_chapters == md::Selection::all());
    CHECK(options->layout == md::Layout::volumes);
    CHECK(options->lang == "en");
    CHECK(options->groups.empty());
    CHECK(options->quality == md::ImageQuality::data);
    CHECK(options->direction == md::DirectionChoice::automatic);
    CHECK_FALSE(options->title.has_value());
    CHECK_FALSE(options->overwrite);
    CHECK_FALSE(options->dry_run);
    CHECK(options->jobs == 4U);
    CHECK(options->api_base == md::ApiBase{});
    CHECK(options->report);
}

TEST_CASE("validate_download_args maps every option", "[options][download_args]") {
    const md::RawDownloadArgs raw{
        .manga = "https://mangadex.org/title/A1B2C3D4-E5F6-4789-8ABC-DEF012345678/slug",
        .dir = "out/manga",
        .volumes = "1,3-5",
        .loose_chapters = "none",
        .layout = "chapters",
        .lang = "pt-br",
        .groups = {"Group B", "c0ffee00-0000-4000-8000-000000000000", "Group A"},
        .data_saver = true,
        .direction = "rtl",
        .title = "  My Title ",
        .overwrite = true,
        .dry_run = true,
        .jobs = 8,
        .api_base = "file:///srv/fixtures",
        .no_report = false,
    };
    const md::Result<md::DownloadOptions> options = md::validate_download_args(raw);
    REQUIRE(options.has_value());
    CHECK(options->manga.str() == manga_uuid);
    CHECK(options->dir == std::filesystem::path{"out/manga"});
    CHECK(options->volumes == md::Selection::parse("1,3-5"));
    CHECK(options->loose_chapters.is_none());
    CHECK(options->layout == md::Layout::chapters);
    CHECK(options->lang == "pt-br");
    CHECK(options->groups ==
          std::vector<std::string>{"Group B", "c0ffee00-0000-4000-8000-000000000000", "Group A"});
    CHECK(options->quality == md::ImageQuality::data_saver);
    CHECK(options->direction == md::DirectionChoice::rtl);
    CHECK(options->title == std::optional<std::string>{"My Title"});
    CHECK(options->overwrite);
    CHECK(options->dry_run);
    CHECK(options->jobs == 8U);
    CHECK(options->api_base.kind() == md::ApiBase::Kind::fixture);
    CHECK(options->api_base.fixture_root() == std::filesystem::path{"/srv/fixtures"});
    CHECK_FALSE(options->report);
}

TEST_CASE("validate_download_args maps each layout and direction word",
          "[options][download_args]") {
    md::RawDownloadArgs raw = valid_raw();
    raw.layout = "single";
    raw.direction = "ltr";
    raw.jobs = 1;
    const md::Result<md::DownloadOptions> options = md::validate_download_args(raw);
    REQUIRE(options.has_value());
    CHECK(options->layout == md::Layout::single);
    CHECK(options->direction == md::DirectionChoice::ltr);
    CHECK(options->jobs == 1U);

    raw.layout = "volumes";
    raw.direction = "auto";
    const md::Result<md::DownloadOptions> defaults = md::validate_download_args(raw);
    REQUIRE(defaults.has_value());
    CHECK(defaults->layout == md::Layout::volumes);
    CHECK(defaults->direction == md::DirectionChoice::automatic);
}

TEST_CASE("validate_download_args reports each semantic usage error", "[options][download_args]") {
    md::RawDownloadArgs raw = valid_raw();

    SECTION("a bad MANGA") {
        raw.manga = "https://example.org/title/a1b2c3d4-e5f6-4789-8abc-def012345678";
        CHECK(args_error(raw) ==
              "MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL");
    }
    SECTION("an empty MANGA") {
        raw.manga.clear();
        CHECK(args_error(raw) ==
              "MANGA must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL");
    }
    SECTION("an empty DIR") {
        raw.dir.clear();
        CHECK(args_error(raw) == "DIR must not be empty");
    }
    SECTION("a DIR that is a second manga id") {
        raw.dir = GENERATE(as<std::string>{}, "b1b2c3d4-e5f6-4789-8abc-def012345678",
                           "https://mangadex.org/title/b1b2c3d4-e5f6-4789-8abc-def012345678");
        CAPTURE(raw.dir);
        CHECK(args_error(raw) == "DIR looks like a manga id: only one manga per run");
    }
    SECTION("a malformed --volumes") {
        raw.volumes = "5-3";
        CHECK(args_error(raw) ==
              "--volumes: '5-3' is an empty range: the low bound is greater than the high bound");
    }
    SECTION("a malformed --loose-chapters") {
        raw.loose_chapters = "1, 2";
        CHECK(args_error(raw) == "--loose-chapters: the selection must not contain whitespace");
    }
    SECTION("both selections none") {
        raw.volumes = "none";
        raw.loose_chapters = "none";
        CHECK(args_error(raw) ==
              "--volumes and --loose-chapters are both none: nothing to download");
    }
    SECTION("an unknown --layout") {
        raw.layout = "bogus";
        CHECK(args_error(raw) == "--layout must be volumes, chapters or single");
    }
    SECTION("a bad --lang") {
        raw.lang = "EN";
        CHECK(args_error(raw) ==
              "--lang must be a lowercase language code such as en, pt-br or es-la");
    }
    SECTION("an unknown --direction") {
        raw.direction = "up";
        CHECK(args_error(raw) == "--direction must be auto, ltr or rtl");
    }
    SECTION("a bad --title") {
        raw.title = "   ";
        CHECK(args_error(raw) == "--title must not be empty");
    }
    SECTION("--jobs outside 1-8") {
        raw.jobs = GENERATE(0, 9, -1, 1000);
        CAPTURE(raw.jobs);
        CHECK(args_error(raw) == "--jobs must be between 1 and 8");
    }
    SECTION("a bad --api-base") {
        raw.api_base = "http://example.com";
        CHECK(args_error(raw) ==
              "--api-base http:// is allowed only for 127.0.0.1, localhost or [::1]");
    }
}

TEST_CASE("One none selection is fine", "[options][download_args]") {
    md::RawDownloadArgs raw = valid_raw();
    raw.volumes = "none";
    CHECK(args_error(raw) == "<accepted>");
    raw.volumes = "all";
    raw.loose_chapters = "none";
    CHECK(args_error(raw) == "<accepted>");
}

TEST_CASE("report is on only for the official API without --no-report",
          "[options][download_args]") {
    md::RawDownloadArgs raw = valid_raw();
    CHECK(report_for(raw));
    raw.api_base = "https://api.mangadex.org/";
    CHECK(report_for(raw));

    raw.no_report = true;
    CHECK_FALSE(report_for(raw));
}

TEST_CASE("report is off for any base other than the official API", "[options][download_args]") {
    md::RawDownloadArgs raw = valid_raw();
    raw.api_base =
        GENERATE(as<std::string>{}, "https://api.mangadex.dev", "https://api.mangadex.org:443",
                 "https://api.mangadex.org/v2", "http://127.0.0.1:8080", "file:///srv/fixtures");
    CAPTURE(raw.api_base);
    CHECK_FALSE(report_for(raw));
}

TEST_CASE("validate_title_override never reads past the end of truncated UTF-8",
          "[options][title][bounds]") {
    for (const std::string_view truncated :
         {"\xc3", "\xe6\xbc", "\xf0\x9f\x93", "ab\xf0\x9f", "  \xf0\x9f\x93", "\t\xe6\xbc"}) {
        CAPTURE(truncated);
        const ExactBuffer buffer{truncated};
        CHECK(usage_message(md::validate_title_override(buffer.view())) ==
              "--title must be valid UTF-8");
    }
    // Every prefix of a multi-byte title: complete sequences pass, cut ones fail, and the
    // trimmed view (here ending at the allocation's last byte) is never over-read.
    constexpr std::string_view title = " Zo\xc3\xab \xe6\xbc\xab\xf0\x9f\x93\x96";
    // Byte offsets: 3-4 "\xc3\xab", 6-8 "\xe6\xbc\xab", 9-12 "\xf0\x9f\x93\x96".
    constexpr std::array<std::size_t, 6> cut_lengths{4, 7, 8, 10, 11, 12};
    for (std::size_t length = 0; length <= title.size(); ++length) {
        CAPTURE(length);
        const ExactBuffer buffer{title.substr(0, length)};
        const md::Result<std::string> result = md::validate_title_override(buffer.view());
        if (length <= 1) {
            CHECK(usage_message(result) == "--title must not be empty");
        } else if (std::ranges::contains(cut_lengths, length)) {
            CHECK(usage_message(result) == "--title must be valid UTF-8");
        } else {
            std::string_view expected = title.substr(1, length - 1);
            if (expected.ends_with(' ')) {
                expected.remove_suffix(1); // length 6 ends with the space before the CJK text
            }
            CHECK(result == std::string{expected});
        }
    }
}

TEST_CASE("validate_language never reads past the end of its view", "[options][lang][bounds]") {
    constexpr std::string_view code = "zh-hant";
    for (std::size_t length = 0; length <= code.size(); ++length) {
        const std::string_view prefix = code.substr(0, length);
        CAPTURE(prefix);
        const ExactBuffer buffer{prefix};
        // Valid prefixes: "zh" and "zh-ha" through "zh-hant".
        const bool valid = length == 2 || length >= 5;
        CHECK(md::validate_language(buffer.view()).has_value() == valid);
    }
}

TEST_CASE("ApiBase::parse never reads past the end of its view", "[options][api_base][bounds]") {
    for (const std::string_view base :
         {std::string_view{"https://[::1]:8443/api/v5/"}, std::string_view{"http://127.0.0.1:1"},
          std::string_view{"file:///srv/fixtures/x/"},
          std::string_view{"https://api.mangadex.org"}}) {
        for (std::size_t length = 0; length <= base.size(); ++length) {
            const std::string_view prefix = base.substr(0, length);
            CAPTURE(prefix);
            const ExactBuffer buffer{prefix};
            const md::Result<md::ApiBase> parsed = md::ApiBase::parse(buffer.view());
            if (!parsed.has_value()) {
                CHECK(parsed.error().code == md::Errc::usage);
            }
        }
        const ExactBuffer whole{base};
        CHECK(md::ApiBase::parse(whole.view()).has_value());
    }
}
