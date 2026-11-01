// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_jobs_follows_test.cpp: the shared multi-title runner (run_batch_jobs) as
// `follows` drives it (follows plan T14, spec AC11): the `follows:<n>: <uuid>: ` row prefix with
// mdlist's "item" wording, the closing line, the duplicate warning, the -v start line, the --json
// overload with the `follows` object right after `source`, the flat collision advice, and (rev1,
// V27) the run's claimed paths under --overwrite: a second title with the same name never
// replaces the first one's files in the same run.
//
// Nothing is fetched or logged in here: the settings come from validate_follows_args and the
// jobs from plan_follows over hand-picked ids (what the command hands over after its collection
// fetch), run over a fixture Backend from make_backend and a hermetic fixture tree in a fresh
// temporary directory. The overwrite case mirrors mdlist's (commit 2a67cc0) over the clean-twin
// tree (tests/support/mdx_twin.hpp).
//
// Catch2 test names must not start with '-'. Every name starts with "batch jobs follows" so
// `pixi run test -R 'cli\.batch jobs follows'` selects them all.

#include "app/cmd_batch_jobs.hpp"
#include "app/cmd_download_output.hpp"
#include "app/cmd_download_run.hpp"
#include "app/command.hpp"
#include "app/io.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"
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
#include <iterator>
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

/// The exit-4 line of the unknown title, without the row prefix.
[[nodiscard]] std::string not_found_message(std::string_view id) {
    return std::format("title {} not found on MangaDex (HTTP 404); check the UUID or URL", id);
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-batch-jobs-follows");
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

[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    REQUIRE(file.is_open());
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// Does `path` hold exactly `bytes`? (A bool, so a failure does not print two EPUBs.)
[[nodiscard]] bool holds(const fs::path& path, const std::string& bytes) {
    return read_file(path) == bytes;
}

/// Selects the clean-twin tree (mdx_twin.hpp) instead of one canned title.
struct CleanTwin {};

/// What the overwrite and dry-run knobs of one follows run are.
struct RunFlags {
    bool dry_run = false;
    bool overwrite = false;
};

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

    /// What validate_follows_args makes of DIR plus the tree's api base (every other option at
    /// its default unless `flags` says otherwise).
    [[nodiscard]] md::FollowsSettings settings(const fs::path& dir, RunFlags flags = {}) const {
        md::RawFollowsArgs raw{};
        raw.defaults.dir = dir.string();
        raw.defaults.api_base = api_base();
        raw.defaults.dry_run = flags.dry_run;
        raw.defaults.overwrite = flags.overwrite;
        md::Result<md::FollowsSettings> settings = md::validate_follows_args(raw);
        INFO((settings ? std::string{} : settings.error().message));
        REQUIRE(settings.has_value());
        return std::move(*settings);
    }

private:
    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    fs::path root_{};
};

/// plan_follows over `ids`, in order (what the command hands over after its fetch and filter).
[[nodiscard]] std::vector<md::BatchJob> make_jobs(const md::FollowsSettings& settings,
                                                  std::initializer_list<std::string_view> ids) {
    std::vector<md::MangaId> titles{};
    for (const std::string_view id : ids) {
        md::Result<md::MangaId> manga = md::MangaId::parse(id);
        REQUIRE(manga.has_value());
        titles.push_back(std::move(*manga));
    }
    return md::plan_follows(titles, settings);
}

/// The follows object for a run of `count` follows without --status.
[[nodiscard]] md::FollowsSummary summary_of(std::size_t count) {
    return md::FollowsSummary{.username = "fixture-user", .titles = count, .selected = count};
}

// ---- running the runner ------------------------------------------------------------------------

/// run_batch_jobs's follows overload over two string streams and a fresh fixture Backend, as the
/// follows command calls it after its collection fetch.
[[nodiscard]] Captured run_jobs(const std::vector<md::BatchJob>& jobs,
                                const md::FollowsSettings& settings,
                                const app::GlobalOptions& globals = {}) {
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out, .err = err};
    const app::OutputPolicy policy = app::output_policy(globals, io);
    // Lifetime: agent, backend and the summary are declared before the call and outlive it.
    const std::string agent = app::user_agent();
    const md::Result<app::Backend> backend = app::make_backend(settings.batch.base.api_base, agent);
    INFO((backend ? std::string{} : backend.error().message));
    REQUIRE(backend.has_value());
    const md::FollowsSummary follows = summary_of(jobs.size());
    Captured result;
    result.exit_code =
        app::run_batch_jobs(jobs, settings.batch, *backend, agent, io, policy, follows);
    result.out = out.str();
    result.err = err.str();
    return result;
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

/// The keys of `object` in `text` (top level when `member` is empty), in document order.
[[nodiscard]] std::vector<std::string> ordered_keys(const std::string& text,
                                                    std::string_view member = {}) {
    std::vector<std::string> keys{};
    if (!OrderedJson::accept(text)) {
        return keys;
    }
    const OrderedJson doc = OrderedJson::parse(text);
    const OrderedJson& object = member.empty() ? doc : doc.at(std::string{member});
    for (const auto& item : object.items()) {
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

/// The follows collision line for `path` (item 2 = the twin, item 1 = the clean title).
[[nodiscard]] std::string follows_collision(const fs::path& path) {
    return std::format("mangadex-dl: warning: follows:2: {}: {} was already written by item 1 "
                       "(another title with the same name); pass --dir-per-title to give each "
                       "title its own directory, or download one of them on its own with "
                       "'mangadex-dl download <title> --title ...'\n",
                       twin_id, path.string());
}

} // namespace

// ---- item wording (AC11) -----------------------------------------------------------------------

TEST_CASE("batch jobs follows: rows carry 'follows:<n>: <uuid>: '; the closing line counts items",
          "[cli][follows][batch-jobs]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path out = tree.path("out");
    const md::FollowsSettings settings = tree.settings(out);
    const std::vector<md::BatchJob> jobs = make_jobs(settings, {nochapters_id, unknown_id});

    const Captured human = run_jobs(jobs, settings);
    CHECK(human.exit_code == 7);
    CHECK(human.out.empty());
    INFO("stderr: " << human.err);
    const std::vector<std::string> lines = lines_of(human.err);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with(
        std::format("mangadex-dl: error: follows:1: {}: no downloadable chapters matched --lang en",
                    nochapters_id)));
    CHECK(lines[1] == std::format("mangadex-dl: error: follows:2: {}: {}", unknown_id,
                                  not_found_message(unknown_id)));
    CHECK(lines[2] == "mangadex-dl: error: 2 of 2 titles did not complete (items 1, 2); see the "
                      "errors above");
    CHECK(!fs::exists(out));

    SECTION("--json: the same stderr; batch's document plus `follows` right after `source`") {
        const Captured json = run_jobs(jobs, settings, {.json = true});
        CHECK(json.exit_code == 7);
        CHECK(json.err == human.err);
        CHECK(ordered_keys(json.out) == std::vector<std::string>{"format_version", "dry_run",
                                                                 "source", "follows", "titles",
                                                                 "summary"});
        CHECK(ordered_keys(json.out, "follows") ==
              std::vector<std::string>{"username", "titles", "statuses", "selected"});
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("format_version") == 1);
        CHECK(doc.at("source") == "follows");
        CHECK(doc.at("follows") == Json{{"username", "fixture-user"},
                                        {"titles", 2},
                                        {"statuses", nullptr},
                                        {"selected", 2}});
        CHECK(!doc.contains("list"));
        CHECK(statuses(doc) == std::vector<std::string>{"no_chapters", "not_found"});
        CHECK(doc.at("titles").at(0).at("line") == 1);
        CHECK(doc.at("titles").at(1).at("line") == 2);
        CHECK(doc.at("titles").at(1).at("dir") == out.string());
    }
    SECTION("a dry run says 'would fail (items ...)'") {
        const md::FollowsSettings dry = tree.settings(out, {.dry_run = true});
        const Captured r = run_jobs(make_jobs(dry, {nochapters_id, unknown_id}), dry);
        CHECK(r.exit_code == 7);
        CHECK(r.err.ends_with("mangadex-dl: error: 2 of 2 titles would fail (items 1, 2)\n"));
    }
    SECTION("--fail-fast: 'stopped at item 1'") {
        md::FollowsSettings fail_fast = settings;
        fail_fast.batch.fail_fast = true;
        const Captured r = run_jobs(jobs, fail_fast);
        CHECK(r.exit_code == 5);
        CHECK(r.err.ends_with("mangadex-dl: error: stopped at item 1: --fail-fast; 1 title not "
                              "run\n"));
    }
}

TEST_CASE("batch jobs follows: a repeated follow is 'same as item <k>; skipped'; -q hides it",
          "[cli][follows][batch-jobs]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const md::FollowsSettings settings = tree.settings(out, {.dry_run = true});
    const std::vector<md::BatchJob> jobs = make_jobs(settings, {clean_id, clean_id});
    REQUIRE(jobs[1].duplicate_of == 1);

    const Captured r = run_jobs(jobs, settings);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err ==
          std::format("mangadex-dl: warning: follows:2: {}: same as item 1; skipped\n", clean_id));

    const Captured json = run_jobs(jobs, settings, {.json = true});
    CHECK(json.exit_code == 0);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"ok", "duplicate"});
    CHECK(doc.at("titles").at(1).at("duplicate_of") == 1);

    const Captured quiet = run_jobs(jobs, settings, {.quiet = true});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.err.empty());
}

TEST_CASE("batch jobs follows: -v starts each item with 'follows:<n>: <uuid>' (the uuid once)",
          "[cli][follows][batch-jobs]") {
    const Tree tree{mdx::clean_fixture()};
    const md::FollowsSettings settings = tree.settings(tree.path("out"), {.dry_run = true});
    const Captured r = run_jobs(make_jobs(settings, {clean_id}), settings, {.verbose = 1});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    REQUIRE(!lines.empty());
    CHECK(lines.front() == std::format("mangadex-dl: follows:1: {}", clean_id));
    CHECK(!r.err.contains(std::format("{}: {}", clean_id, clean_id)));
}

// ---- flat collisions ---------------------------------------------------------------------------

TEST_CASE("batch jobs follows: two titles writing the same file get mdlist's collision advice",
          "[cli][follows][batch-jobs][collision]") {
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");
    const md::FollowsSettings settings = tree.settings(out, {.dry_run = true});
    const Captured r = run_jobs(make_jobs(settings, {clean_id, twin_id}), settings);
    CHECK(r.exit_code == 0);
    CHECK(r.err == follows_collision(out / clean_vol1) + follows_collision(out / clean_vol2));
    CHECK(!r.err.contains("'title' column"));
    CHECK(!r.err.contains("by line"));
}

TEST_CASE("batch jobs follows: flat --overwrite, a second title with the same name never "
          "replaces the first one's files",
          "[cli][follows][batch-jobs][collision][overwrite]") {
    // V27: follows hands every row the run's claimed paths (claims_paths), as mdlist does.
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");
    const std::string collisions =
        follows_collision(out / clean_vol1) + follows_collision(out / clean_vol2);

    // Item 1's own bytes and the twin's: each title alone, in a directory of its own.
    const auto alone = [&tree](std::string_view manga, std::string_view dir) {
        const md::FollowsSettings one = tree.settings(tree.path(dir));
        REQUIRE(run_jobs(make_jobs(one, {manga}), one).exit_code == 0);
        return std::pair{read_file(tree.path(dir) / clean_vol1),
                         read_file(tree.path(dir) / clean_vol2)};
    };
    const auto [vol1, vol2] = alone(clean_id, "clean-alone");
    const auto twin_files = alone(twin_id, "twin-alone");
    const bool different = std::pair{vol1, vol2} != twin_files;
    REQUIRE(different); // otherwise "the first one's bytes" proves nothing

    const md::FollowsSettings overwrite = tree.settings(out, {.overwrite = true});
    const std::vector<md::BatchJob> jobs = make_jobs(overwrite, {clean_id, twin_id});

    SECTION("a real run: item 2 leaves item 1's files alone; each path is listed once") {
        const Captured r = run_jobs(jobs, overwrite);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == collisions); // nothing says a file was replaced: none was
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
    }
    SECTION("--json: item 2's files are left alone as foreign, item 1's written") {
        const Captured r = run_jobs(jobs, overwrite, {.json = true});
        CHECK(r.exit_code == 0);
        CHECK(r.err == collisions);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        for (const Json& epub : doc.at("titles").at(0).at("report").at("epubs")) {
            CHECK(epub.at("status") == "written");
            CHECK(epub.at("replaced") == false);
        }
        const Json& second = doc.at("titles").at(1).at("report").at("epubs");
        REQUIRE(second.size() == 2);
        for (const Json& epub : second) {
            CHECK(epub.at("status") == "exists");
            CHECK(epub.at("existing") == "foreign");
            CHECK(epub.at("replaced") == false);
        }
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
    }
    SECTION("a later --overwrite run still replaces item 1's files from an earlier run") {
        REQUIRE(run_jobs(jobs, overwrite).exit_code == 0);
        write_file(out / clean_vol1, "stale");
        write_file(out / clean_vol2, "stale");
        const Captured r = run_jobs(jobs, overwrite, {.json = true});
        CHECK(r.exit_code == 0);
        CHECK(r.err == collisions);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        for (const Json& epub : doc.at("titles").at(0).at("report").at("epubs")) {
            CHECK(epub.at("status") == "written");
            CHECK(epub.at("replaced") == true);
        }
        for (const Json& epub : doc.at("titles").at(1).at("report").at("epubs")) {
            CHECK(epub.at("status") == "exists");
            CHECK(epub.at("existing") == "foreign");
        }
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
    }
    SECTION("--dry-run --overwrite: the same answer as the real run, and nothing is created") {
        const md::FollowsSettings dry = tree.settings(out, {.dry_run = true, .overwrite = true});
        const std::vector<md::BatchJob> dry_jobs = make_jobs(dry, {clean_id, twin_id});
        const Captured r = run_jobs(dry_jobs, dry);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == collisions);
        CHECK(!fs::exists(out));

        const Captured json = run_jobs(dry_jobs, dry, {.json = true});
        CHECK(json.exit_code == 0);
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        for (const Json& epub : doc.at("titles").at(1).at("report").at("epubs")) {
            CHECK(epub.at("status") == "exists");
            CHECK(epub.at("existing") == "foreign");
            CHECK(epub.at("replaced") == false);
        }
        CHECK(!fs::exists(out));
    }
}

// ---- usage lines -------------------------------------------------------------------------------

TEST_CASE("batch jobs follows: an unknown --status word reaches stderr only through "
          "terminal_safe",
          "[cli][follows][batch-jobs][usage]") {
    // StatusFilter::parse echoes the word raw (cut to 32 bytes); the app's usage line must make it
    // safe for the terminal (follows spec A1). ESC, BEL, CR and an invalid byte each become '?'.
    md::RawFollowsArgs raw{};
    raw.defaults.dir = "out";
    raw.status = {"\x1b[2Jx\x07\r\xff"};
    const md::Result<md::FollowsSettings> settings = md::validate_follows_args(raw);
    REQUIRE(!settings.has_value());
    REQUIRE(settings.error().code == md::Errc::usage);
    CHECK(settings.error().message.contains('\x1b')); // raw in core, by design

    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out, .err = err};
    app::print_usage_line(io, settings.error().message);
    CHECK(app::usage_hint(io, "follows") == 2);
    CHECK(out.str().empty());
    // Three '?' in a row, spelled out: "??'" in a literal would be a trigraph.
    const std::string replaced(3, '?');
    CHECK(err.str() ==
          std::format("mangadex-dl: error: --status: unknown reading status '?[2Jx{}'; use "
                      "reading, on_hold, plan_to_read, dropped, re_reading or completed\n"
                      "Run 'mangadex-dl follows --help' for more information.\n",
                      replaced));
}
