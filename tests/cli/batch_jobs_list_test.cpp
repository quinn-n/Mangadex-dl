// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_jobs_list_test.cpp: the shared multi-title runner (run_batch_jobs) as `mdlist`
// drives it (list plan T9): source-aware row wording, the --dir-per-title directory resolver over
// one TitleDirRegistry per run, the effective `dir` in the JSON document and the `list` object.
//
// No list is fetched here: every test hand-builds the BatchJobs plan_list would produce (position
// = line, raw/options = the CLI defaults plus the manga, dir = DIR, sub_dir empty, duplicates
// marked), with BatchSettings{.label = "list", .source = mdlist}, over a fixture Backend from
// make_backend and a hermetic fixture tree in a fresh temporary directory. A csv case proves that
// `batch` still prints exactly what it printed before.
//
// Covers AC39 (runner half), AC41 (runner level, the clean-twin tree), the item wording, the
// mdlist flat-collision advice and the csv byte-identity.
//
// Catch2 test names must not start with '-'. Every name starts with "mdlist" so
// `pixi run tsan -R 'cli\.mdlist'` selects them all.

#include "app/cmd_batch.hpp"
#include "app/cmd_batch_jobs.hpp"
#include "app/cmd_download_output.hpp"
#include "app/cmd_download_run.hpp"
#include "app/command.hpp"
#include "app/io.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/mdx_twin.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using md::test::Captured;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view nochapters_id = "55555555-5555-4555-8555-555555555555";
constexpr std::string_view unknown_id = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view twin_id = mdx::twin_manga_id;

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view clean_dir = "Clean Title";
constexpr std::string_view twin_dir = "Clean Title [77777777]";

/// LIST as typed (what `source` echoes in the document); never fetched here.
constexpr std::string_view list_source =
    "https://mangadex.org/list/a0000000-0000-4000-8000-000000000001";

/// The exit-4 line of the unknown title, without the row prefix.
[[nodiscard]] std::string not_found_message(std::string_view id) {
    return std::format("title {} not found on MangaDex (HTTP 404); check the UUID or URL", id);
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-batch-jobs");
    INFO((tmp ? std::string{} : tmp.error()));
    REQUIRE(tmp.has_value());
    return std::move(*tmp);
}

void write_file(const fs::path& path, std::string_view content) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << content;
    file.close();
    REQUIRE(file.good());
}

/// Selects the clean-twin tree (mdx_twin.hpp) instead of one canned title.
struct CleanTwin {};

/// A fixture tree under a fresh temporary directory; output directories go next to it. Not
/// copyable or movable (TempDir owns the directory).
class Tree {
public:
    explicit Tree(const mdx::FixtureManga& manga)
        : tmp_(make_temp_dir()), root_(tmp_.path() / "fixture") {
        const auto written = mdx::write_fixture_tree(root_, manga);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
    }
    explicit Tree(CleanTwin /*tag*/) : tmp_(make_temp_dir()), root_(tmp_.path() / "fixture") {
        const auto written = mdx::write_clean_twin_tree(root_);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Tree(Tree&&) = delete;
    Tree& operator=(Tree&&) = delete;
    ~Tree() = default;

    [[nodiscard]] std::string api_base() const { return "file://" + root_.string(); }

    /// `<tmp>/<name>`, not created.
    [[nodiscard]] fs::path path(std::string_view name) const { return tmp_.path() / name; }

    /// What validate_batch_args makes of `DIR` plus the tree's api base (every other option at
    /// its default), as the run's CLI-level settings.
    [[nodiscard]] md::BatchSettings validated(const fs::path& dir, bool dry_run = false) const {
        md::RawBatchArgs raw{.file = "titles.csv"};
        raw.defaults.dir = dir.string();
        raw.defaults.api_base = api_base();
        raw.defaults.dry_run = dry_run;
        md::Result<md::BatchSettings> settings = md::validate_batch_args(raw);
        INFO((settings ? std::string{} : settings.error().message));
        REQUIRE(settings.has_value());
        return std::move(*settings);
    }

    /// mdlist's settings: label "list", source mdlist, `source` = LIST as typed.
    [[nodiscard]] md::BatchSettings list_settings(const fs::path& dir, bool dry_run = false) const {
        md::BatchSettings settings = validated(dir, dry_run);
        settings.file = std::string{list_source};
        settings.label = std::string{md::list_row_label};
        settings.source = md::BatchSource::mdlist;
        return settings;
    }

private:
    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    fs::path root_{};
};

/// The jobs plan_list would make for `ids` (in order, positions from 1): raw/options are the
/// settings' defaults plus the manga, `dir` is DIR, `sub_dir` is empty, and a repeated id is a
/// duplicate of its first position.
[[nodiscard]] std::vector<md::BatchJob> make_jobs(const md::BatchSettings& settings,
                                                  std::initializer_list<std::string_view> ids,
                                                  bool dir_per_title = false) {
    std::vector<md::BatchJob> jobs{};
    for (const std::string_view id : ids) {
        md::Result<md::MangaId> manga = md::MangaId::parse(id);
        REQUIRE(manga.has_value());
        md::BatchJob job{.line = jobs.size() + 1, .raw = settings.defaults};
        job.raw.manga = std::string{id};
        job.options = settings.base;
        job.options.manga = std::move(*manga);
        job.options.dir_per_title = dir_per_title;
        job.dir = settings.defaults.dir;
        const auto first = std::ranges::find_if(jobs, [&job](const md::BatchJob& earlier) {
            return earlier.options.manga == job.options.manga;
        });
        if (first != jobs.end()) {
            job.duplicate_of = first->line;
        }
        jobs.push_back(std::move(job));
    }
    return jobs;
}

// ---- running the runner ------------------------------------------------------------------------

/// run_batch_jobs over two string streams and a fresh fixture Backend, as run_batch_command and
/// run_mdlist_command call it after their phase 1.
[[nodiscard]] Captured run_jobs(const std::vector<md::BatchJob>& jobs,
                                const md::BatchSettings& settings,
                                const app::GlobalOptions& globals = {},
                                const std::optional<md::ListSummary>& list = std::nullopt) {
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out, .err = err};
    const app::OutputPolicy policy = app::output_policy(globals, io);
    // Lifetime: agent and backend are declared before the call and outlive it.
    const std::string agent = app::user_agent();
    const md::Result<app::Backend> backend = app::make_backend(settings.base.api_base, agent);
    INFO((backend ? std::string{} : backend.error().message));
    REQUIRE(backend.has_value());
    Captured result;
    result.exit_code = app::run_batch_jobs(jobs, settings, *backend, agent, io, policy, list);
    result.out = out.str();
    result.err = err.str();
    return result;
}

/// The list object mdlist passes for list_source.
[[nodiscard]] md::ListSummary list_summary() {
    return md::ListSummary{.id = "a0000000-0000-4000-8000-000000000001",
                           .name = "Clean List",
                           .url = "https://mangadex.org/list/a0000000-0000-4000-8000-000000000001"};
}

// ---- output checks ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines{};
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

/// `<dir>/<name>\n` for each name: the stdout path stream.
[[nodiscard]] std::string path_lines(const fs::path& dir,
                                     std::initializer_list<std::string_view> names) {
    std::string out;
    for (const std::string_view name : names) {
        out += (dir / name).string();
        out += '\n';
    }
    return out;
}

/// Exactly one JSON document (plus the final newline), parsed; a discarded value otherwise.
[[nodiscard]] Json parse_document(const std::string& text) {
    if (!text.ends_with('\n') || !Json::accept(text)) {
        return Json::value_t::discarded;
    }
    return Json::parse(text);
}

/// The top-level keys of the document, in document order.
[[nodiscard]] std::vector<std::string> top_level_keys(const std::string& text) {
    std::vector<std::string> keys{};
    if (!OrderedJson::accept(text)) {
        return keys;
    }
    for (const auto& item : OrderedJson::parse(text).items()) {
        keys.push_back(item.key());
    }
    return keys;
}

/// The statuses of the document's titles, in order.
[[nodiscard]] std::vector<std::string> statuses(const Json& doc) {
    std::vector<std::string> out{};
    for (const Json& row : doc.at("titles")) {
        out.push_back(row.at("status").get<std::string>());
    }
    return out;
}

/// The names in `dir`, sorted (empty when it does not exist).
[[nodiscard]] std::vector<std::string> listing(const fs::path& dir) {
    std::vector<std::string> names{};
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator{dir, ec}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

} // namespace

// ---- item wording ----------------------------------------------------------------------------

TEST_CASE("mdlist runner: row lines carry 'list:<n>: <uuid>: '; the closing line counts items",
          "[cli][mdlist][batch-jobs]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path out = tree.path("out");
    const md::BatchSettings settings = tree.list_settings(out);
    const std::vector<md::BatchJob> jobs = make_jobs(settings, {nochapters_id, unknown_id});

    const Captured human = run_jobs(jobs, settings);
    CHECK(human.exit_code == 7);
    CHECK(human.out.empty());
    INFO("stderr: " << human.err);
    const std::vector<std::string> lines = lines_of(human.err);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with(
        std::format("mangadex-dl: error: list:1: {}: no downloadable chapters matched --lang en",
                    nochapters_id)));
    CHECK(lines[1] == std::format("mangadex-dl: error: list:2: {}: {}", unknown_id,
                                  not_found_message(unknown_id)));
    CHECK(lines[2] == "mangadex-dl: error: 2 of 2 titles did not complete (items 1, 2); see the "
                      "errors above");
    CHECK(!fs::exists(out));

    SECTION("--json: the same stderr; batch's document plus `list` right after `source`") {
        const Captured json = run_jobs(jobs, settings, {.json = true}, list_summary());
        CHECK(json.exit_code == 7);
        CHECK(json.err == human.err);
        CHECK(top_level_keys(json.out) == std::vector<std::string>{"format_version", "dry_run",
                                                                   "source", "list", "titles",
                                                                   "summary"});
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("source") == list_source);
        CHECK(doc.at("list") == Json{{"id", "a0000000-0000-4000-8000-000000000001"},
                                     {"name", "Clean List"},
                                     {"url", "https://mangadex.org/list/"
                                             "a0000000-0000-4000-8000-000000000001"}});
        CHECK(statuses(doc) == std::vector<std::string>{"no_chapters", "not_found"});
        CHECK(doc.at("titles").at(0).at("line") == 1);
        CHECK(doc.at("titles").at(1).at("line") == 2);
        CHECK(doc.at("titles").at(1).at("dir") == out.string());
    }
    SECTION("a dry run says 'would fail (items ...)'") {
        const md::BatchSettings dry = tree.list_settings(out, /*dry_run=*/true);
        const Captured r = run_jobs(make_jobs(dry, {nochapters_id, unknown_id}), dry);
        CHECK(r.exit_code == 7);
        CHECK(r.err.ends_with("mangadex-dl: error: 2 of 2 titles would fail (items 1, 2)\n"));
    }
    SECTION("--fail-fast: 'stopped at item 1'") {
        md::BatchSettings fail_fast = settings;
        fail_fast.fail_fast = true;
        const Captured r = run_jobs(jobs, fail_fast);
        CHECK(r.exit_code == 5);
        CHECK(r.err.ends_with("mangadex-dl: error: stopped at item 1: --fail-fast; 1 title not "
                              "run\n"));
    }
}

TEST_CASE("mdlist runner: a repeated title is 'same as item <k>; skipped'; -q hides it",
          "[cli][mdlist][batch-jobs]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const md::BatchSettings settings = tree.list_settings(out, /*dry_run=*/true);
    const std::vector<md::BatchJob> jobs = make_jobs(settings, {clean_id, clean_id});
    REQUIRE(jobs[1].duplicate_of == 1);

    const Captured r = run_jobs(jobs, settings);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err ==
          std::format("mangadex-dl: warning: list:2: {}: same as item 1; skipped\n", clean_id));

    const Captured quiet = run_jobs(jobs, settings, {.quiet = true});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.err.empty());
}

TEST_CASE("mdlist runner: -v starts each item with 'list:<n>: <uuid>' (the uuid once)",
          "[cli][mdlist][batch-jobs]") {
    const Tree tree{mdx::clean_fixture()};
    const md::BatchSettings settings = tree.list_settings(tree.path("out"), /*dry_run=*/true);
    const Captured r = run_jobs(make_jobs(settings, {clean_id}), settings, {.verbose = 1});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    REQUIRE(!lines.empty());
    CHECK(lines.front() == std::format("mangadex-dl: list:1: {}", clean_id));
    CHECK(!r.err.contains(std::format("{}: {}", clean_id, clean_id)));
}

// ---- --dir-per-title (AC39, AC41) --------------------------------------------------------------

TEST_CASE("mdlist runner --dir-per-title: an item whose directory is a file fails alone",
          "[cli][mdlist][batch-jobs][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    fs::create_directories(out);
    write_file(out / clean_dir, "a regular file\n");
    const md::BatchSettings settings = tree.list_settings(out);
    const std::vector<md::BatchJob> jobs =
        make_jobs(settings, {clean_id, unknown_id}, /*dir_per_title=*/true);
    const std::string row_error =
        std::format("mangadex-dl: error: list:1: {}: cannot open directory '{}': Not a directory\n",
                    clean_id, (out / clean_dir).string());
    const std::string not_found = std::format("mangadex-dl: error: list:2: {}: {}\n", unknown_id,
                                              not_found_message(unknown_id));
    // Neither a row-scoped directory failure nor a missing title clears on a retry.
    const std::string closing =
        "mangadex-dl: error: 2 of 2 titles did not complete (items 1, 2); see the errors above\n";

    const Captured human = run_jobs(jobs, settings);
    CHECK(human.exit_code == 7); // failed (1) + not_found (4): the run did not stop at item 1
    CHECK(human.out.empty());
    CHECK(human.err == std::format("{}{}{}", row_error, not_found, closing));
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}}); // nothing created

    const Captured json = run_jobs(jobs, settings, {.json = true}, list_summary());
    CHECK(json.exit_code == 7);
    CHECK(json.err == human.err);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"failed", "not_found"});
    CHECK(doc.at("titles").at(0).at("exit_code") == 1);
    CHECK(doc.at("titles").at(0).at("error") ==
          std::format("cannot open directory '{}': Not a directory", (out / clean_dir).string()));
    // The engine claimed the name before its pre-flight failed the item.
    CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
    // Item 2 ended before its plan (404): no name of its own, not item 1's either.
    CHECK(doc.at("titles").at(1).at("dir") == out.string());
}

TEST_CASE("mdlist runner --dir-per-title: each title in DIR/<title>; JSON dir is DIR/<title>",
          "[cli][mdlist][batch-jobs][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");

    SECTION("a real run, then a re-run that finds every file up to date") {
        const md::BatchSettings settings = tree.list_settings(out);
        const std::vector<md::BatchJob> jobs =
            make_jobs(settings, {clean_id, unknown_id}, /*dir_per_title=*/true);
        const Captured r = run_jobs(jobs, settings, {.json = true}, list_summary());
        CHECK(r.exit_code == 4);
        CHECK(r.err == std::format("mangadex-dl: error: list:2: {}: {}\n"
                                   "mangadex-dl: error: 1 of 2 titles did not complete (item 2); "
                                   "see the errors above\n",
                                   unknown_id, not_found_message(unknown_id)));
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
        CHECK(doc.at("titles").at(0).at("report").at("epubs").at(0).at("path") ==
              (out / clean_dir / clean_vol1).string());
        CHECK(doc.at("titles").at(1).at("dir") == out.string());
        CHECK(fs::is_regular_file(out / clean_dir / clean_vol1));
        CHECK(fs::is_regular_file(out / clean_dir / clean_vol2));
        CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});

        const Captured again = run_jobs(make_jobs(settings, {clean_id}, true), settings);
        CHECK(again.exit_code == 0);
        CHECK(again.out.empty());
        CHECK(again.err == "mangadex-dl: 2 files are already up to date; pass --overwrite to "
                           "download them again\n");
    }
    SECTION("a dry run prints the paths below DIR/<title> and creates nothing") {
        const md::BatchSettings dry = tree.list_settings(out, /*dry_run=*/true);
        const Captured r = run_jobs(make_jobs(dry, {clean_id}, /*dir_per_title=*/true), dry);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}));
        CHECK(r.err.empty());
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("mdlist runner --dir-per-title: a second title with the same name is moved aside",
          "[cli][mdlist][batch-jobs][dir-per-title]") {
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");
    const md::BatchSettings settings = tree.list_settings(out);
    const std::vector<md::BatchJob> jobs =
        make_jobs(settings, {clean_id, twin_id}, /*dir_per_title=*/true);
    const std::string warning = std::format(
        "mangadex-dl: warning: list:2: {}: directory 'Clean Title' already holds item 1 (a title "
        "with the same name); using 'Clean Title [77777777]'\n",
        twin_id);

    SECTION("both titles are downloaded, each into its own directory") {
        const Captured r = run_jobs(jobs, settings);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}) +
                           path_lines(out / twin_dir, {clean_vol1, clean_vol2}));
        CHECK(r.err == warning);
        CHECK(fs::is_regular_file(out / clean_dir / clean_vol1));
        CHECK(fs::is_regular_file(out / clean_dir / clean_vol2));
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol1));
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol2));
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_dir}, std::string{twin_dir}});

        // The registry lives for one run: the same order claims the same names again.
        const Captured again = run_jobs(jobs, settings);
        CHECK(again.exit_code == 0);
        CHECK(again.out.empty());
        CHECK(again.err == warning + "mangadex-dl: 4 files are already up to date; pass "
                                     "--overwrite to download them again\n");
    }
    SECTION("-q hides the warning; the directories are the same") {
        const Captured r = run_jobs(jobs, settings, {.quiet = true});
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol2));
    }
    SECTION("--json: titles[1].dir is the moved-aside directory") {
        const Captured r = run_jobs(jobs, settings, {.json = true}, list_summary());
        CHECK(r.exit_code == 0);
        CHECK(r.err == warning);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
        CHECK(doc.at("titles").at(1).at("dir") == (out / twin_dir).string());
        CHECK(doc.at("titles").at(1).at("report").at("epubs").at(1).at("path") ==
              (out / twin_dir / clean_vol2).string());
    }
    SECTION("-v: the engine's 'output directory:' line follows the item line") {
        const md::BatchSettings dry = tree.list_settings(out, /*dry_run=*/true);
        const Captured r = run_jobs(make_jobs(dry, {clean_id, twin_id}, true), dry, {.verbose = 1});
        CHECK(r.exit_code == 0);
        INFO("stderr: " << r.err);
        const std::vector<std::string> lines = lines_of(r.err);
        const auto item2 =
            std::ranges::find(lines, std::format("mangadex-dl: list:2: {}", twin_id));
        REQUIRE(item2 != lines.end());
        CHECK(std::ranges::find(item2, lines.end(),
                                std::format("mangadex-dl: output directory: {}",
                                            (out / twin_dir).string())) != lines.end());
        CHECK(!fs::exists(out));
    }
}

// ---- flat mode collision ---------------------------------------------------------------------

TEST_CASE("mdlist runner: two titles writing the same file get the mdlist collision advice",
          "[cli][mdlist][batch-jobs][collision]") {
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");
    const md::BatchSettings settings = tree.list_settings(out, /*dry_run=*/true);
    const Captured r = run_jobs(make_jobs(settings, {clean_id, twin_id}), settings);
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    for (const std::string_view file : {clean_vol1, clean_vol2}) {
        CHECK(r.err.contains(std::format(
            "mangadex-dl: warning: list:2: {}: {} was already written by item 1; download one of "
            "them on its own with 'mangadex-dl download <title> --title ...'\n",
            twin_id, (out / file).string())));
    }
    CHECK(!r.err.contains("'title' column"));
    CHECK(!r.err.contains("by line"));
}

// ---- csv: batch is unchanged -----------------------------------------------------------------

TEST_CASE("mdlist runner: source csv prints exactly what `batch` prints, and no `list` key",
          "[cli][mdlist][batch-jobs][csv]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path out = tree.path("out");
    const fs::path csv = tree.path("mixed.csv");
    write_file(csv, std::format("{}\n{}\n{}\n", nochapters_id, unknown_id, nochapters_id));

    // What batch_cli_test.cpp pins, built by hand: label = FILE as typed, source csv.
    md::BatchSettings settings = tree.validated(out);
    settings.file = csv.string();
    settings.label = csv.string();
    REQUIRE(settings.source == md::BatchSource::csv);
    const std::vector<md::BatchJob> jobs =
        make_jobs(settings, {nochapters_id, unknown_id, nochapters_id});

    const Captured human = run_jobs(jobs, settings, {.verbose = 1});
    CHECK(human.exit_code == 7);
    INFO("stderr: " << human.err);
    const std::vector<std::string> lines = lines_of(human.err);
    CHECK(std::ranges::count(
              lines, std::format("mangadex-dl: {}:1: {}", csv.string(), nochapters_id)) == 1);
    CHECK(std::ranges::count(lines, std::format("mangadex-dl: error: {}:2: {}", csv.string(),
                                                not_found_message(unknown_id))) == 1);
    CHECK(std::ranges::count(lines, std::format("mangadex-dl: warning: {}:3: same as line 1; "
                                                "skipped",
                                                csv.string())) == 1);
    REQUIRE(!lines.empty());
    CHECK(lines.back() == "mangadex-dl: error: 2 of 2 titles did not complete (lines 1, 2); see "
                          "the errors above");

    const Captured json = run_jobs(jobs, settings, {.json = true});
    CHECK(top_level_keys(json.out) ==
          std::vector<std::string>{"format_version", "dry_run", "source", "titles", "summary"});

    // Byte for byte what the batch command itself prints for the same CSV.
    for (const bool as_json : {false, true}) {
        INFO((as_json ? "--json" : "human"));
        md::RawBatchArgs raw{.file = csv.string()};
        raw.defaults.dir = out.string();
        raw.defaults.api_base = tree.api_base();
        std::ostringstream cmd_out;
        std::ostringstream cmd_err;
        app::Io io{.out = cmd_out, .err = cmd_err};
        const int code = app::run_batch_command(raw, app::GlobalOptions{.json = as_json}, io);
        const Captured runner = run_jobs(jobs, settings, {.json = as_json});
        CHECK(code == runner.exit_code);
        CHECK(cmd_out.str() == runner.out);
        CHECK(cmd_err.str() == runner.err);
    }
}

TEST_CASE("mdlist runner: source csv keeps batch's collision advice",
          "[cli][mdlist][batch-jobs][csv]") {
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");
    md::BatchSettings settings = tree.validated(out, /*dry_run=*/true);
    settings.label = "titles.csv";
    const Captured r = run_jobs(make_jobs(settings, {clean_id, twin_id}), settings);
    CHECK(r.exit_code == 0);
    CHECK(r.err.contains(std::format("mangadex-dl: warning: titles.csv:2: {} was already written "
                                     "by line 1; give one of them a 'title' column\n",
                                     (out / clean_vol1).string())));
}
