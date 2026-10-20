// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_download_options.hpp"

#include "mangadex_dl/options.hpp"

#include <CLI/CLI.hpp>

namespace mangadex_dl::app {

// Help text: each description fits the 44-column right column on one line where it can.

void bind_selection_options(CLI::App& sub, RawDownloadArgs& args) {
    sub.add_option("--volumes", args.volumes, "Volumes: all, none or a list like 1,3-5,8-")
        ->type_name("SPEC")
        ->capture_default_str();
    sub.add_option("--loose-chapters", args.loose_chapters,
                   "Chapters with no volume (SPEC as --volumes)")
        ->type_name("SPEC")
        ->capture_default_str();
    sub.add_option("--layout", args.layout, "Split into: volumes, chapters or single file")
        ->type_name("WORD")
        ->check(CLI::IsMember({"volumes", "chapters", "single"}).description(""))
        ->capture_default_str();
    sub.add_option("-l,--lang", args.lang, "Language to download (e.g. en, pt-br)")
        ->type_name("CODE")
        ->capture_default_str();
    // expected(1) alone rejects a repeated --group; TakeAll alone swallows the next positional.
    sub.add_option("--group", args.groups, "Preferred group; repeat in priority order")
        ->type_name("NAME|UUID")
        ->expected(1)
        ->allow_extra_args(false)
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    sub.add_flag("--data-saver", args.data_saver, "Download compressed images (smaller files)");
    sub.add_option("--direction", args.direction, "Page order: auto (by origin), rtl or ltr")
        ->type_name("WORD")
        ->check(CLI::IsMember({"auto", "rtl", "ltr"}).description(""))
        ->capture_default_str();
}

void bind_run_options(CLI::App& sub, RawDownloadArgs& args) {
    sub.add_flag("--overwrite", args.overwrite, "Replace files that already exist");
    sub.add_flag("-n,--dry-run", args.dry_run, "List the files to write; download nothing");
    sub.add_option("-j,--jobs", args.jobs, "Parallel image downloads, 1 to 8")
        ->type_name("N")
        ->check(CLI::Range(1, 8).description(""))
        ->capture_default_str();
    sub.add_flag("--no-report", args.no_report, "Skip MangaDex@Home reliability reports");
    // Hidden (empty group): a test seam for the hermetic fixture tree and loopback servers.
    sub.add_option("--api-base", args.api_base, "API base URL")->group("");
}

} // namespace mangadex_dl::app
