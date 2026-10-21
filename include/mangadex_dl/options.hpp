// SPDX-License-Identifier: GPL-3.0-or-later
//
// `download` options: the raw strings CLI11 binds to, and the validated form core code consumes.
// Frozen interface (download plan T1); implemented in src/core/options.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/selection.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {

inline constexpr std::string_view default_api_base{"https://api.mangadex.org"};

/// The (hidden) --api-base: where API requests go. A default-constructed ApiBase is the official
/// API.
class ApiBase {
public:
    enum class Kind {
        official,      ///< exactly default_api_base
        https,         ///< another https:// base
        http_loopback, ///< http:// to 127.0.0.1, localhost or [::1] only
        fixture,       ///< file://<absolute dir>: the hermetic fixture tree (tests)
    };

    ApiBase() = default;

    /// `https://host[:port][/path]` without userinfo, query or fragment; `http://` only for a
    /// loopback host; `file://` plus an absolute path with no `.`, `..` or empty segments and no
    /// `?`/`#`. Errc::usage with a one-line message otherwise.
    [[nodiscard]] static Result<ApiBase> parse(std::string_view text);

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    /// The base URL without a trailing '/'. Borrows from *this: never bind it to a temporary
    /// (`ApiBase::parse(x)->url()` dangles once the full-expression ends).
    [[nodiscard]] std::string_view url() const noexcept MANGADEX_DL_LIFETIMEBOUND { return url_; }

    /// The fixture tree root; empty unless kind() == Kind::fixture. Borrows from *this.
    [[nodiscard]] const std::filesystem::path&
    fixture_root() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return fixture_root_;
    }

    friend bool operator==(const ApiBase&, const ApiBase&) = default;

private:
    /// For parse() only: the parts are already validated.
    ApiBase(Kind kind, std::string url, std::filesystem::path fixture_root) noexcept
        : kind_(kind), url_(std::move(url)), fixture_root_(std::move(fixture_root)) {}

    Kind kind_ = Kind::official;
    std::string url_{default_api_base};
    std::filesystem::path fixture_root_{};
};

/// Exactly what CLI11 binds to (owned by the download Command). Nothing here is validated yet.
struct RawDownloadArgs {
    std::string manga{};
    std::string dir = ".";
    std::string volumes = "all";
    std::string loose_chapters = "all";
    std::string layout = "volumes";
    std::string lang = "en";
    std::vector<std::string> groups{}; ///< --group, repeatable, in priority order
    bool data_saver = false;
    std::string direction = "auto";
    std::optional<std::string> title{};
    bool overwrite = false;
    bool dry_run = false;
    int jobs = 8;
    std::string api_base = std::string{default_api_base};
    bool no_report = false;
};

/// Validated options. Every default here is header-inline (no out-of-line definition needed).
struct DownloadOptions {
    MangaId manga{};
    std::filesystem::path dir{"."};
    Selection volumes = Selection::all();
    Selection loose_chapters = Selection::all();
    Layout layout = Layout::volumes;
    std::string lang = "en";
    std::vector<std::string> groups{};
    ImageQuality quality = ImageQuality::data;
    DirectionChoice direction = DirectionChoice::automatic;
    std::optional<std::string> title{};
    bool overwrite = false;
    bool dry_run = false;
    unsigned jobs = 8;
    ApiBase api_base{};
    bool report = true; ///< send MD@H reliability reports (official API only)

    /// Member-wise; batch uses it to spot rows that would repeat an earlier row's download.
    friend bool operator==(const DownloadOptions&, const DownloadOptions&) = default;
};

/// Turns raw strings into DownloadOptions. Errc::usage with a one-line message (plus a hint) for:
/// a bad MANGA, SPEC, --lang, --title or --api-base; both selections `none`; a DIR that looks
/// like a manga id ("only one manga per run"); jobs outside 1-8. `report` is false when
/// --no-report is set, the API base is not the official one, or in fixture mode.
[[nodiscard]] Result<DownloadOptions> validate_download_args(const RawDownloadArgs& raw);

/// `^[a-z]{2,3}(-[a-z]{2,4})?$`. Returns the code unchanged; Errc::usage otherwise.
[[nodiscard]] Result<std::string> validate_language(std::string_view code);

/// Non-empty after trimming, valid UTF-8, no C0 controls or DEL, at most 1024 bytes. Returns the
/// trimmed title; Errc::usage otherwise.
[[nodiscard]] Result<std::string> validate_title_override(std::string_view title);

} // namespace mangadex_dl
