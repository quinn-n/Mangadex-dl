// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_dir_per_title_test.cpp: `batch --dir-per-title` in-process through
// run_batch_command with a hand-filled RawBatchArgs (batch-dir-per-title plan T2, spec AC9-AC21).
// Each test writes a hermetic fixture tree (tests/support/mdx_fixture.hpp, or the clean-twin tree
// of tests/support/mdx_twin.hpp) and a CSV list into a fresh temporary directory and points
// `api_base = file://<tree>` at it, so the whole chain runs: plan_batch sets the flag on rows
// without a `dir` cell, run_batch_jobs reserves the explicit cells' first segments, and each
// derived row claims its directory through the runner's resolver.
//
// AC20 (interrupts) is covered here only for an interrupt pending before the call (plan S5);
// interrupts during and between rows are unit-tested on the unchanged engine and loop.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec). Every name
// starts with "batch --dir-per-title" so `pixi run tsan -R 'cli\.batch --dir-per-title'` selects
// them all.

#include "app/cmd_batch.hpp"
#include "app/command.hpp"
#include "app/io.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/interrupt.hpp"
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

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction, SIGINT and SIGTERM are POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using md::test::Captured;
using Json = nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view nochapters_id = "55555555-5555-4555-8555-555555555555";
constexpr std::string_view unknown_id = mdx::unknown_manga_id;
constexpr std::string_view twin_id = mdx::twin_manga_id;

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view other_vol1 = "Other Title - Vol. 01.epub";
constexpr std::string_view other_vol2 = "Other Title - Vol. 02.epub";
constexpr std::string_view clean_dir = "Clean Title";
constexpr std::string_view twin_dir = "Clean Title [77777777]";    ///< the twin moved aside
constexpr std::string_view clean_aside = "Clean Title [11111111]"; ///< clean moved aside

/// The exit-4 line of the unknown title, without the row prefix.
[[nodiscard]] std::string title_not_found(std::string_view id) {
    return std::format("title {} not found on MangaDex (HTTP 404); check the UUID or URL", id);
}

// ---- running the command ---------------------------------------------------------------------

/// What the command sees besides its RawBatchArgs.
struct Context {
    app::GlobalOptions globals{};
};

/// run_batch_command over two string streams; stdin is closed (-1): no list here reads it.
[[nodiscard]] Captured run_command(const md::RawBatchArgs& args, const Context& context = {}) {
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out, .err = err, .stdin_fd = -1};
    Captured result;
    result.exit_code = app::run_batch_command(args, context.globals, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-batch-dpt");
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

/// Selects the clean-twin tree (mdx_twin.hpp) instead of one canned title.
struct CleanTwin {};

/// A fixture tree under a fresh temporary directory; the CSV lists and output directories go next
/// to it. Not copyable or movable (TempDir owns the directory).
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

    /// Writes `<tmp>/<name>` with `content` and returns its path.
    [[nodiscard]] fs::path csv(std::string_view name, std::string_view content) const {
        fs::path file = path(name);
        write_file(file, content);
        return file;
    }

    /// `batch --api-base <tree> FILE DIR --dir-per-title`, everything else at its default.
    [[nodiscard]] md::RawBatchArgs args(const fs::path& file, const fs::path& dir) const {
        md::RawBatchArgs args{.file = file.string(), .dir_per_title = true};
        args.defaults.dir = dir.string();
        args.defaults.api_base = api_base();
        return args;
    }

private:
    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    fs::path root_{};
};

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

/// The statuses of the document's titles, in order.
[[nodiscard]] std::vector<std::string> statuses(const Json& doc) {
    std::vector<std::string> out{};
    for (const Json& row : doc.at("titles")) {
        out.push_back(row.at("status").get<std::string>());
    }
    return out;
}

/// The `dir` of each of the document's titles, in order.
[[nodiscard]] std::vector<std::string> dirs(const Json& doc) {
    std::vector<std::string> out{};
    for (const Json& row : doc.at("titles")) {
        out.push_back(row.at("dir").get<std::string>());
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

/// One entry of a tree snapshot: a path relative to the root and the file's bytes ("/" for a
/// directory).
struct Entry {
    std::string path{};
    std::string bytes{};

    friend bool operator==(const Entry&, const Entry&) = default;
};

/// Every path below `root` with its bytes, sorted by path (empty when `root` does not exist).
[[nodiscard]] std::vector<Entry> snapshot(const fs::path& root) {
    std::vector<Entry> entries{};
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator{root, ec}) {
        const std::string relative = entry.path().lexically_relative(root).generic_string();
        if (entry.is_directory()) {
            entries.push_back(Entry{.path = relative, .bytes = "/"});
        } else {
            entries.push_back(Entry{.path = relative, .bytes = read_file(entry.path())});
        }
    }
    std::ranges::sort(entries, {}, &Entry::path);
    return entries;
}

/// Does some line of `lines` equal `line`?
[[nodiscard]] bool has_line(const std::vector<std::string>& lines, std::string_view line) {
    return std::ranges::find(lines, line) != lines.end();
}

/// Do `first` and then `second` both occur in `lines`, `second` after `first` (not necessarily
/// next to it: the engine's `-v` "planned ..." line can sit between them)?
[[nodiscard]] bool in_order(const std::vector<std::string>& lines, std::string_view first,
                            std::string_view second) {
    const auto at = std::ranges::find(lines, first);
    return at != lines.end() && std::ranges::find(at + 1, lines.end(), second) != lines.end();
}

// ---- process state ---------------------------------------------------------------------------

/// Clears the process-wide interrupt flag on entry and exit, so test order does not matter.
class InterruptFlagReset {
public:
    InterruptFlagReset() noexcept { md::reset_interrupt_for_testing(); }
    InterruptFlagReset(const InterruptFlagReset&) = delete;
    InterruptFlagReset& operator=(const InterruptFlagReset&) = delete;
    InterruptFlagReset(InterruptFlagReset&&) = delete;
    InterruptFlagReset& operator=(InterruptFlagReset&&) = delete;
    ~InterruptFlagReset() { md::reset_interrupt_for_testing(); }
};

/// The current SIGINT/SIGTERM handlers (Catch2 installs its own; the command must restore them).
struct Dispositions {
    void (*sigint)(int) = nullptr;
    void (*sigterm)(int) = nullptr;

    friend bool operator==(const Dispositions&, const Dispositions&) = default;
};

[[nodiscard]] Dispositions current_dispositions() {
    struct sigaction int_action{};
    struct sigaction term_action{};
    REQUIRE(::sigaction(SIGINT, nullptr, &int_action) == 0);
    REQUIRE(::sigaction(SIGTERM, nullptr, &term_action) == 0);
    return Dispositions{.sigint = int_action.sa_handler, .sigterm = term_action.sa_handler};
}

} // namespace

// ---- an explicit `dir` cell replaces the derived name (AC9) ------------------------------------

TEST_CASE("batch --dir-per-title: a row with a dir cell is byte-identical to a run without the "
          "flag",
          "[cli][batch][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list =
        tree.csv("row-dir.csv", std::format("manga,dir\n{},series/clean\n", clean_id));

    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Context context{.globals = {.json = json}};
        md::RawBatchArgs flat_args = tree.args(list, out);
        flat_args.dir_per_title = false;

        const Captured flat = run_command(flat_args, context);
        const std::vector<Entry> flat_files = snapshot(out);
        fs::remove_all(out);
        const Captured per_title = run_command(tree.args(list, out), context);
        const std::vector<Entry> per_title_files = snapshot(out);
        fs::remove_all(out);

        CHECK(flat.exit_code == 0);
        CHECK(per_title.exit_code == flat.exit_code);
        CHECK(per_title.out == flat.out);
        CHECK(per_title.err == flat.err);
        CHECK(per_title.err.empty());
        CHECK(per_title_files == flat_files);
        CHECK(!per_title_files.empty());
        if (json) {
            const Json doc = parse_document(per_title.out);
            REQUIRE(!doc.is_discarded());
            CHECK(dirs(doc) == std::vector<std::string>{(out / "series/clean").string()});
        } else {
            CHECK(per_title.out == path_lines(out / "series/clean", {clean_vol1, clean_vol2}));
        }
    }
}

// ---- derived rows (AC10, AC11, AC19) -----------------------------------------------------------

TEST_CASE("batch --dir-per-title: a headerless row goes to DIR/<title>/; nothing is flat",
          "[cli][batch][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));

    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(fs::is_regular_file(out / clean_dir / clean_vol1));
    CHECK(fs::is_regular_file(out / clean_dir / clean_vol2));
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});
}

TEST_CASE("batch --dir-per-title: a dry run plans under DIR/<title>/ and creates nothing",
          "[cli][batch][dir-per-title][dry-run]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
    md::RawBatchArgs args = tree.args(list, out);
    args.defaults.dry_run = true;

    const Captured r = run_command(args);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(!fs::exists(out));
}

TEST_CASE("batch --dir-per-title: derived rows of one manga share its directory; a dir row "
          "keeps its own",
          "[cli][batch][dir-per-title]") {
    // Spec per-title-mixed.csv: derived, the same manga with another stem (QB3: it stays in the
    // manga's first directory), then explicit.
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv(
        "per-title-mixed.csv",
        std::format("manga,title,dir\n{0},,\n{0},Other Title,\n{0},,series/clean\n", clean_id));

    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2, other_vol1, other_vol2}) +
                       path_lines(out / "series/clean", {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}, "series"});
    CHECK(listing(out / clean_dir) ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2},
                                   std::string{other_vol1}, std::string{other_vol2}});
    CHECK(listing(out / "series/clean") ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
    CHECK(!fs::exists(out / "Other Title"));

    SECTION("--json: the dirs are DIR/<title>, DIR/<title> and DIR/<dir>") {
        const fs::path json_out = tree.path("out-json");
        const Captured json =
            run_command(tree.args(list, json_out), Context{.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err.empty());
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok", "ok"});
        CHECK(dirs(doc) == std::vector<std::string>{(json_out / clean_dir).string(),
                                                    (json_out / clean_dir).string(),
                                                    (json_out / "series/clean").string()});
        CHECK(doc.at("titles").at(1).at("report").at("epubs").at(0).at("path") ==
              (json_out / clean_dir / other_vol1).string());
    }
}

// ---- two titles with the same name (AC12) ------------------------------------------------------

TEST_CASE("batch --dir-per-title: a second title with the same name is moved aside, naming the "
          "line",
          "[cli][batch][dir-per-title]") {
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n{}\n", clean_id, twin_id));
    const std::string warning =
        std::format("mangadex-dl: warning: {}:2: directory 'Clean Title' already holds line 1 (a "
                    "title with the same name); using 'Clean Title [77777777]'\n",
                    list.string());

    SECTION("both titles are downloaded, each into its own directory") {
        const Captured r = run_command(tree.args(list, out));
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}) +
                           path_lines(out / twin_dir, {clean_vol1, clean_vol2}));
        CHECK(r.err == warning);
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_dir}, std::string{twin_dir}});
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol2));
    }
    SECTION("-q hides the warning") {
        const Captured r = run_command(tree.args(list, out), Context{.globals = {.quiet = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol1));
    }
    SECTION("--json: titles[1].dir is the moved-aside directory") {
        const Captured r = run_command(tree.args(list, out), Context{.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == warning);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(dirs(doc) ==
              std::vector<std::string>{(out / clean_dir).string(), (out / twin_dir).string()});
    }
}

// ---- explicit dir cells reserve their first segment (AC13) -------------------------------------

TEST_CASE("batch --dir-per-title: a name another manga's dir cell uses is moved aside, in either "
          "order",
          "[cli][batch][dir-per-title][reserve]") {
    const Tree tree{CleanTwin{}};
    const fs::path out = tree.path("out");

    struct Case {
        std::string_view name{};
        std::string csv{};
        std::size_t clean_line = 0;
        std::size_t twin_line = 0;
        std::string twin_cell{}; ///< the twin row's dir cell
    };
    const std::vector<Case> cases{
        {.name = "the dir row first",
         .csv = std::format("manga,dir\n{},Clean Title\n{},\n", twin_id, clean_id),
         .clean_line = 3,
         .twin_line = 2,
         .twin_cell = "Clean Title"},
        {.name = "the derived row first",
         .csv = std::format("manga,dir\n{},\n{},Clean Title\n", clean_id, twin_id),
         .clean_line = 2,
         .twin_line = 3,
         .twin_cell = "Clean Title"},
        {.name = "a first segment",
         .csv = std::format("manga,dir\n{},Clean Title/extras\n{},\n", twin_id, clean_id),
         .clean_line = 3,
         .twin_line = 2,
         .twin_cell = "Clean Title/extras"},
    };
    for (const Case& c : cases) {
        INFO("case: " << c.name);
        const fs::path list = tree.csv("titles.csv", c.csv);
        const std::string clean_paths = path_lines(out / clean_aside, {clean_vol1, clean_vol2});
        const std::string twin_paths = path_lines(out / c.twin_cell, {clean_vol1, clean_vol2});
        const std::string warning = std::format(
            "mangadex-dl: warning: {}:{}: directory 'Clean Title' is used by the 'dir' of line {}; "
            "using 'Clean Title [11111111]'\n",
            list.string(), c.clean_line, c.twin_line);

        const Captured r = run_command(tree.args(list, out));
        CHECK(r.exit_code == 0);
        CHECK(r.out ==
              (c.clean_line < c.twin_line ? clean_paths + twin_paths : twin_paths + clean_paths));
        CHECK(r.err == warning);
        // Each manga's files are in a directory of its own: the twin's under its cell, clean's
        // moved aside, and nothing else below DIR.
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_dir}, std::string{clean_aside}});
        CHECK(listing(out / clean_aside) ==
              std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
        CHECK(listing(out / c.twin_cell) ==
              std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});

        const Captured quiet = run_command(tree.args(list, tree.path("out-quiet")),
                                           Context{.globals = {.quiet = true}});
        CHECK(quiet.exit_code == 0);
        CHECK(quiet.err.empty());

        const fs::path json_out = tree.path("out-json");
        const Captured json =
            run_command(tree.args(list, json_out), Context{.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        const std::vector<std::string> got = dirs(doc);
        REQUIRE(got.size() == 2);
        CHECK(got.at(c.clean_line - 2) == (json_out / clean_aside).string());
        CHECK(got.at(c.twin_line - 2) == (json_out / c.twin_cell).string());

        fs::remove_all(out);
        fs::remove_all(tree.path("out-quiet"));
        fs::remove_all(json_out);
    }
}

TEST_CASE("batch --dir-per-title: a dir cell reserves its name even when its row fails (404)",
          "[cli][batch][dir-per-title][reserve]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv(
        "dir-reserved.csv", std::format("manga,dir\n{},Clean Title\n{},\n", unknown_id, clean_id));
    const std::string expected_err = std::format(
        "mangadex-dl: error: {0}:2: {1}\n"
        "mangadex-dl: warning: {0}:3: directory 'Clean Title' is used by the 'dir' of line 2; "
        "using 'Clean Title [11111111]'\n",
        list.string(), title_not_found(unknown_id));

    SECTION("download") {
        const Captured r = run_command(tree.args(list, out));
        CHECK(r.exit_code == 4);
        CHECK(r.out == path_lines(out / clean_aside, {clean_vol1, clean_vol2}));
        CHECK(r.err == expected_err + "mangadex-dl: error: 1 of 2 titles did not complete (line "
                                      "2); see the errors above\n");
        CHECK(listing(out) == std::vector<std::string>{std::string{clean_aside}});
    }
    SECTION("dry run") {
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.dry_run = true;
        const Captured r = run_command(args);
        CHECK(r.exit_code == 4);
        CHECK(r.out == path_lines(out / clean_aside, {clean_vol1, clean_vol2}));
        CHECK(r.err == expected_err + "mangadex-dl: error: 1 of 2 titles would fail (line 2)\n");
        CHECK(!fs::exists(out));
    }
}

// ---- the same manga, explicit and derived (AC14) -----------------------------------------------

TEST_CASE("batch --dir-per-title: a manga's own dir cell does not move its derived row; the "
          "second row finds the files",
          "[cli][batch][dir-per-title][reserve]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list =
        tree.csv("titles.csv", std::format("manga,dir\n{0},Clean Title\n{0},\n", clean_id));
    const std::string collisions = std::format(
        "mangadex-dl: warning: {0}:3: {1} was already written by line 2; give one of them a "
        "'title' column\n"
        "mangadex-dl: warning: {0}:3: {2} was already written by line 2; give one of them a "
        "'title' column\n",
        list.string(), (out / clean_dir / clean_vol1).string(),
        (out / clean_dir / clean_vol2).string());

    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}));
    CHECK(r.err == collisions + "mangadex-dl: 2 files are already up to date; pass --overwrite to "
                                "download them again\n");
    CHECK(!r.err.contains("directory 'Clean Title'"));
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});

    SECTION("--json: both rows report DIR/Clean Title; the second row's files exist") {
        const fs::path json_out = tree.path("out-json");
        const Captured json =
            run_command(tree.args(list, json_out), Context{.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(dirs(doc) == std::vector<std::string>{(json_out / clean_dir).string(),
                                                    (json_out / clean_dir).string()});
        const Json& epubs = doc.at("titles").at(1).at("report").at("epubs");
        REQUIRE(epubs.size() == 2);
        CHECK(epubs.at(0).at("status") == "exists");
        CHECK(epubs.at(1).at("status") == "exists");
    }
}

// ---- -v (AC15) ---------------------------------------------------------------------------------

TEST_CASE("batch --dir-per-title -v: 'output directory:' follows the row's start line",
          "[cli][batch][dir-per-title][verbose]") {
    SECTION("a derived row") {
        const Tree tree{mdx::clean_fixture()};
        const fs::path out = tree.path("out");
        const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.dry_run = true;
        const Captured r = run_command(args, Context{.globals = {.verbose = 1}});
        CHECK(r.exit_code == 0);
        INFO("stderr: " << r.err);
        CHECK(
            in_order(lines_of(r.err), std::format("mangadex-dl: {}:1: {}", list.string(), clean_id),
                     std::format("mangadex-dl: output directory: {}", (out / clean_dir).string())));
        CHECK(!fs::exists(out));
    }
    SECTION("a moved-aside row: start line, then the warning, then 'output directory:'") {
        const Tree tree{CleanTwin{}};
        const fs::path out = tree.path("out");
        const fs::path list = tree.csv("titles.csv", std::format("{}\n{}\n", clean_id, twin_id));
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.dry_run = true;
        const Captured r = run_command(args, Context{.globals = {.verbose = 1}});
        CHECK(r.exit_code == 0);
        INFO("stderr: " << r.err);
        const std::vector<std::string> lines = lines_of(r.err);
        const std::string start = std::format("mangadex-dl: {}:2: {}", list.string(), twin_id);
        const std::string warning =
            std::format("mangadex-dl: warning: {}:2: directory 'Clean Title' already holds line 1 "
                        "(a title with the same name); using 'Clean Title [77777777]'",
                        list.string());
        const std::string directory =
            std::format("mangadex-dl: output directory: {}", (out / twin_dir).string());
        CHECK(in_order(lines, start, warning));
        CHECK(in_order(lines, warning, directory));
    }
    SECTION("a row with a dir cell prints no 'output directory:' line") {
        const Tree tree{mdx::clean_fixture()};
        const fs::path out = tree.path("out");
        const fs::path list =
            tree.csv("row-dir.csv", std::format("manga,dir\n{},series/clean\n", clean_id));
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.dry_run = true;
        const Captured r = run_command(args, Context{.globals = {.verbose = 1}});
        CHECK(r.exit_code == 0);
        INFO("stderr: " << r.err);
        const std::vector<std::string> lines = lines_of(r.err);
        CHECK(has_line(lines, std::format("mangadex-dl: {}:2: {}", list.string(), clean_id)));
        CHECK(std::ranges::none_of(lines, [](const std::string& line) {
            return line.starts_with("mangadex-dl: output directory:");
        }));
    }
}

// ---- unusable directories (AC16, AC17) ---------------------------------------------------------

TEST_CASE("batch --dir-per-title: a row whose directory is a file fails alone (exit 7)",
          "[cli][batch][dir-per-title][row-dir]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    fs::create_directories(out);
    write_file(out / clean_dir, "a regular file\n");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n{}\n", clean_id, unknown_id));
    const std::string rows = std::format(
        "mangadex-dl: error: {0}:1: cannot open directory '{1}': Not a directory; rename or remove "
        "it, or run without --dir-per-title\n"
        "mangadex-dl: error: {0}:2: {2}\n",
        list.string(), (out / clean_dir).string(), title_not_found(unknown_id));

    SECTION("download") {
        const Captured r = run_command(tree.args(list, out));
        CHECK(r.exit_code == 7); // failed (1) + not_found (4)
        CHECK(r.out.empty());
        CHECK(r.err == rows + "mangadex-dl: error: 2 of 2 titles did not complete (lines 1, 2); "
                              "see the errors above\n");
        CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});
        CHECK(read_file(out / clean_dir) == "a regular file\n");
    }
    SECTION("dry run") {
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.dry_run = true;
        const Captured r = run_command(args);
        CHECK(r.exit_code == 7);
        CHECK(r.out.empty());
        CHECK(r.err == rows + "mangadex-dl: error: 2 of 2 titles would fail (lines 1, 2)\n");
        CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});
    }
    SECTION("--json: row 1 is failed with exit_code 1 and dir DIR/Clean Title") {
        const Captured r = run_command(tree.args(list, out), Context{.globals = {.json = true}});
        CHECK(r.exit_code == 7);
        CHECK(!r.err.contains("stopped at"));
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"failed", "not_found"});
        CHECK(doc.at("titles").at(0).at("exit_code") == 1);
        CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
        CHECK(doc.at("titles").at(1).at("dir") == out.string());
    }
    SECTION("--fail-fast stops at line 1") {
        md::RawBatchArgs args = tree.args(list, out);
        args.fail_fast = true;
        const Captured r = run_command(args);
        CHECK(r.exit_code == 1);
        CHECK(r.out.empty());
        CHECK(r.err.ends_with(
            "mangadex-dl: error: stopped at line 1: --fail-fast; 1 title not run\n"));
    }
}

TEST_CASE("batch --dir-per-title: an unusable DIR still stops the batch at the first row",
          "[cli][batch][dir-per-title][row-dir]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path not_a_dir = tree.path("not-a-dir");
    write_file(not_a_dir, "a regular file\n");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n{}\n", clean_id, unknown_id));

    const Captured r = run_command(tree.args(list, not_a_dir));
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          std::format("mangadex-dl: error: {}:1: cannot open directory '{}': Not a directory\n"
                      "mangadex-dl: error: stopped at line 1: the error above affects every "
                      "title; 1 title not run\n",
                      list.string(), not_a_dir.string()));
    CHECK(read_file(not_a_dir) == "a regular file\n");
}

// ---- rows that never reach a directory (AC18) --------------------------------------------------

TEST_CASE("batch --dir-per-title: exit-4 and exit-5 rows leave no directory; their dir is DIR",
          "[cli][batch][dir-per-title]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("mixed.csv", std::format("{}\n{}\n", unknown_id, nochapters_id));

    const Captured human = run_command(tree.args(list, out));
    CHECK(human.exit_code == 7);
    CHECK(human.out.empty());
    CHECK(!fs::exists(out));

    const Captured json = run_command(tree.args(list, out), Context{.globals = {.json = true}});
    CHECK(json.exit_code == 7);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"not_found", "no_chapters"});
    CHECK(dirs(doc) == std::vector<std::string>{out.string(), out.string()});
    CHECK(!fs::exists(out));
}

// ---- interrupts (AC20, plan S5) ----------------------------------------------------------------

TEST_CASE("batch --dir-per-title: an interrupt before the call runs no row and creates nothing",
          "[cli][batch][dir-per-title][interrupt]") {
    const InterruptFlagReset reset;
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list =
        tree.csv("titles.csv", std::format("manga,dir\n{0},\n{0},series/clean\n", clean_id));
    const Dispositions before = current_dispositions();

    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        md::simulate_interrupt_for_testing(SIGINT);
        const Captured r = run_command(tree.args(list, out), Context{.globals = {.json = json}});
        CHECK(r.exit_code == 1); // main() would re-raise; this is only the fallback
        CHECK(r.out.empty());    // no path, no document
        CHECK(r.err.empty());    // no warning, no closing line
        CHECK(!fs::exists(out));
        CHECK(current_dispositions() == before);
        md::reset_interrupt_for_testing();
    }
}

// ---- after a flat run (AC21) -------------------------------------------------------------------

TEST_CASE("batch --dir-per-title after a flat run leaves the flat files alone",
          "[cli][batch][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
    md::RawBatchArgs flat_args = tree.args(list, out);
    flat_args.dir_per_title = false;
    const Captured flat = run_command(flat_args);
    REQUIRE(flat.exit_code == 0);
    const std::string vol1 = read_file(out / clean_vol1);
    const std::string vol2 = read_file(out / clean_vol2);
    const fs::file_time_type vol1_time = fs::last_write_time(out / clean_vol1);
    const fs::file_time_type vol2_time = fs::last_write_time(out / clean_vol2);

    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(read_file(out / clean_vol1) == vol1);
    CHECK(read_file(out / clean_vol2) == vol2);
    CHECK(fs::last_write_time(out / clean_vol1) == vol1_time);
    CHECK(fs::last_write_time(out / clean_vol2) == vol2_time);
    CHECK(fs::is_regular_file(out / clean_dir / clean_vol1));
    CHECK(fs::is_regular_file(out / clean_dir / clean_vol2));
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}, std::string{clean_vol1},
                                                   std::string{clean_vol2}});
}
