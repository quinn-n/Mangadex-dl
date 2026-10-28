// SPDX-License-Identifier: GPL-3.0-or-later
//
// `mdlist` phase 1 (list plan T8): the CLI-level checks of LIST, DIR and the shared options, and
// the step that turns a validated CustomList into ordinary BatchJobs for run_batch. Pure: no I/O,
// no locale, no printing. LIST is never echoed in a message, like MANGA and FILE.
#include "batch_internal.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "mangadex_dl/options.hpp"
#include "options_internal.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

[[nodiscard]] Error usage_error(std::string_view message) {
    return Error{.code = Errc::usage, .message = std::string{message}};
}

} // namespace

Result<MdlistSettings> validate_mdlist_args(const RawMdlistArgs& raw) {
    // First: ListId::parse("") answers with the generic "LIST must be ..." text.
    if (raw.list.empty()) {
        return std::unexpected(usage_error("LIST must not be empty"));
    }
    Result<ListId> list = ListId::parse(raw.list);
    if (!list) {
        return std::unexpected(std::move(list).error());
    }
    if (raw.defaults.dir.empty()) {
        return std::unexpected(usage_error("DIR must not be empty"));
    }
    if (looks_like_manga_id(raw.defaults.dir) || looks_like_list_id(raw.defaults.dir)) {
        return std::unexpected(usage_error("DIR looks like a MangaDex id; give one list per run"));
    }
    // Backstop for cli.cpp's --title check: one title for every item would collide every file.
    if (raw.defaults.title.has_value()) {
        return std::unexpected(
            usage_error("mdlist has no --title option; to name a title yourself, download it on "
                        "its own with 'mangadex-dl download <title> --title ...'"));
    }
    Result<DownloadOptions> base = detail::validate_common(raw.defaults, MangaId{});
    if (!base) {
        return std::unexpected(std::move(base).error());
    }
    return MdlistSettings{
        .batch =
            BatchSettings{
                .file = raw.list,
                .label = std::string{list_row_label},
                .defaults = raw.defaults,
                .base = std::move(*base),
                .fail_fast = raw.fail_fast,
                .source = BatchSource::mdlist,
            },
        .list = std::move(*list),
        .dir_per_title = raw.dir_per_title,
    };
}

std::vector<BatchJob> plan_list(const CustomList& list, const MdlistSettings& settings) {
    const BatchSettings& batch = settings.batch;
    std::vector<BatchJob> jobs{};
    jobs.reserve(list.titles.size());
    for (std::size_t i = 0; i < list.titles.size(); ++i) {
        const MangaId& manga = list.titles[i];
        RawDownloadArgs raw = batch.defaults;
        raw.manga = std::string{manga.str()};
        DownloadOptions options = batch.base;
        options.manga = manga;
        options.dir_per_title = settings.dir_per_title;
        jobs.push_back(BatchJob{
            .line = i + 1,
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
