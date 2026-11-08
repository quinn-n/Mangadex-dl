// SPDX-License-Identifier: GPL-3.0-or-later
//
// `follows` phase 1 (follows plan T14): the CLI-level checks of DIR, the raw auth arguments, the
// shared options and --status, and the step that turns the (filtered) follows into ordinary
// BatchJobs for run_batch, exactly as plan_list does for mdlist. Pure: no I/O, no locale, no
// printing. DIR is never echoed in a message (it may hold a mistyped password, follows plan rev1
// gate), and nothing here ever sees a secret: RawAuthArgs holds only flags and a file name.
#include "batch_internal.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/options.hpp"
#include "options_internal.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// `source` in the --json document and BatchSettings::file: the literal command word (follows
/// has no positional to echo).
constexpr std::string_view follows_source = "follows";

[[nodiscard]] Error usage_error(std::string_view message) {
    return Error{.code = Errc::usage, .message = std::string{message}};
}

} // namespace

Result<FollowsSettings> validate_follows_args(const RawFollowsArgs& raw) {
    if (raw.defaults.dir.empty()) {
        return std::unexpected(usage_error("DIR must not be empty"));
    }
    if (looks_like_manga_id(raw.defaults.dir) || looks_like_list_id(raw.defaults.dir)) {
        return std::unexpected(
            usage_error("DIR looks like a MangaDex id; follows takes no title or list (use "
                        "'mangadex-dl mdlist <list>' or 'download <title>')"));
    }
    if (Result<void> auth = check_raw_auth_args(raw.auth); !auth) {
        return std::unexpected(std::move(auth).error());
    }
    // Backstop for cli.cpp's --title check (as in mdlist): one title for every follow would
    // collide every file.
    if (raw.defaults.title.has_value()) {
        return std::unexpected(
            usage_error("follows has no --title option; to name a title yourself, download it on "
                        "its own with 'mangadex-dl download <title> --title ...'"));
    }
    Result<DownloadOptions> base = detail::validate_common(raw.defaults, MangaId{});
    if (!base) {
        return std::unexpected(std::move(base).error());
    }
    std::optional<StatusFilter> filter{};
    if (!raw.status.empty()) {
        Result<StatusFilter> parsed = StatusFilter::parse(raw.status);
        if (!parsed) {
            return std::unexpected(std::move(parsed).error());
        }
        filter = std::move(*parsed);
    }
    return FollowsSettings{
        .batch =
            BatchSettings{
                .file = std::string{follows_source},
                .label = std::string{follows_row_label},
                .defaults = raw.defaults,
                .base = std::move(*base),
                .fail_fast = raw.fail_fast,
                .source = BatchSource::follows,
            },
        .filter = std::move(filter),
        .dir_per_title = raw.dir_per_title,
        .auth = raw.auth,
    };
}

std::vector<BatchJob> plan_follows(std::span<const MangaId> titles,
                                   const FollowsSettings& settings) {
    const BatchSettings& batch = settings.batch;
    std::vector<BatchJob> jobs{};
    jobs.reserve(titles.size());
    for (std::size_t i = 0; i < titles.size(); ++i) {
        const MangaId& manga = titles[i];
        RawDownloadArgs raw = batch.defaults;
        raw.manga = std::string{manga.str()};
        DownloadOptions options = batch.base;
        options.manga = manga;
        options.dir_per_title = settings.dir_per_title;
        jobs.push_back(BatchJob{
            .line = i + 1, // the position in the filtered sequence: 1..k, no gaps
            .raw = std::move(raw),
            .options = std::move(options),
            .dir = batch.defaults.dir,
            .duplicate_of = std::nullopt,
            .sub_dir = std::string{},
        });
    }
    // Every job shares the run-level options, so equality reduces to the manga id.
    detail::mark_duplicates(jobs);
    return jobs;
}

} // namespace mangadex_dl
