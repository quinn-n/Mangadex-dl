// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/mdlist_parse_test.cpp: `mdlist` through the real parse-then-dispatch path, run(args,
// Io&) (list plan T11). mdlist_cli_test.cpp drives run_mdlist_command with hand-filled arguments;
// this file proves that CLI11 binds them: register_mdlist, MdlistCommand::bind and the selecting
// callback under ASan+UBSan, the help surface, the fact-checked argv shapes (V7a: the shared
// binders on a third sibling, `--dir-per-title` last of mdlist's own options; V7b: global flags
// after both positionals still apply; V7c: `--title` is not an mdlist option; V7d: a missing LIST
// is a usage error with the mdlist hint) and the usage errors, plus a `download` and a `batch`
// smoke next to the new command.
//
// The "every bound option" case is the carried gate from the T10 memory-safety review: one run()
// that parses LIST, DIR, every shared selection and run option, --fail-fast and --dir-per-title,
// and dispatches through the Command the callback selected.
//
// Every run that gets past parsing reads a fixture tree (tests/support/mdx_fixture.hpp) through
// the hidden `--api-base file://<tree>` seam in a fresh temporary directory, so nothing reaches
// the network and every output directory is inside the test's own directory.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec). Every name
// starts with "mdlist" so `pixi run tsan -R 'cli\.mdlist'` selects them too.

#include "app/cli.hpp"
#include "app/io.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using md::test::Captured;
using Json = nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view basic_id = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view nochapters_id = "55555555-5555-4555-8555-555555555555";

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view clean_dir = "Clean Title";

constexpr std::string_view mdlist_hint = "Run 'mangadex-dl mdlist --help' for more information.\n";

/// The `--help` line `--dir-per-title` adds to batch's options block (V7a).
constexpr std::string_view dir_per_title_line =
    "      --dir-per-title               Put each title in a subdirectory of DIR";

/// A fixture root that never exists: a usage case that slipped past parsing would fail to build
/// its backend (exit 1) instead of exiting 2.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";

/// The list the basic tree serves in this file only (no canned tree serves this id).
constexpr std::uint64_t basic_list_n = 0x20;

// ---- running the CLI -------------------------------------------------------------------------

/// run() over two string streams for an argument list built at runtime. argv[0] is supplied;
/// `args` outlives the call (run() returns before this function does). Stdin is closed (-1):
/// mdlist never reads it.
[[nodiscard]] Captured run_args(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back("mangadex-dl"); // a string literal: static storage
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    std::ostringstream out;
    std::ostringstream err;
    md::app::Io io{.out = out, .err = err, .stdin_fd = -1};
    Captured result;
    result.exit_code = md::app::run(std::span<const char* const>{argv}, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-mdlist-parse");
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

/// A fixture tree for one title (and the lists it serves) under a fresh temporary directory; the
/// output directories and CSV files go next to it. Not copyable or movable (TempDir owns the
/// directory).
class Tree {
public:
    explicit Tree(const mdx::FixtureManga& manga)
        : tmp_(make_temp_dir()), root_(tmp_.path() / "fixture") {
        const auto written = mdx::write_fixture_tree(root_, manga);
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

private:
    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    fs::path root_{};
};

/// The basic tree (two groups, five volumes and loose chapters) serving one list of its title.
[[nodiscard]] mdx::FixtureManga basic_with_list() {
    mdx::FixtureManga basic = mdx::basic_fixture();
    basic.lists = {{.id = mdx::list_id(basic_list_n),
                    .name = "Basic List",
                    .manga_ids = {std::string{basic_id}}}};
    return basic;
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

/// Every line of `text` fits an 80-column terminal (the help is ASCII, so bytes are columns).
void check_fits_80_columns(std::string_view text) {
    for (const std::string& line : lines_of(text)) {
        INFO("line: " << line);
        CHECK(line.size() <= 80);
    }
}

/// The lines of a help text's "OPTIONS:" block, without the heading (up to the first blank line).
[[nodiscard]] std::vector<std::string> options_block(std::string_view help) {
    const std::size_t heading = help.find("\nOPTIONS:\n");
    REQUIRE(heading != std::string_view::npos);
    const std::size_t start = heading + std::string_view{"\nOPTIONS:\n"}.size();
    const std::size_t end = help.find("\n\n", start);
    REQUIRE(end != std::string_view::npos);
    return lines_of(help.substr(start, end - start));
}

/// A usage error from `mdlist`: exit 2, nothing on stdout, the mdlist hint last, no ESC.
void check_mdlist_usage(const Captured& r) {
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("mangadex-dl: error: "));
    CHECK(r.err.ends_with(mdlist_hint));
    CHECK(!r.err.contains('\x1b'));
}

// ---- help (V7a, AC16) ------------------------------------------------------------------------

TEST_CASE("mdlist --help documents LIST, DIR, the list footer and exit rows 4 and 7, without the "
          "hidden seam",
          "[cli][mdlist][help]") {
    const Captured r = run_args({"mdlist", "--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    INFO("stdout: " << r.out);
    CHECK(r.out.starts_with("Download every title in a public MDList\n"));
    CHECK(r.out.contains("\nmangadex-dl mdlist [OPTIONS] LIST [DIR]\n"));
    CHECK(r.out.contains("\n  LIST REQUIRED                     Public MDList: its UUID or list "
                         "URL\n"));
    CHECK(r.out.contains("\n  DIR [.]                           Output directory (created if "
                         "missing)\n"));
    CHECK(r.out.contains("\nLIST:\n  A public MangaDex custom list (MDList): its UUID, or its "
                         "address on the\n"));
    CHECK(
        r.out.contains("  To download the titles you follow, see 'mangadex-dl follows --help'.\n"));
    CHECK(r.out.contains("    mangadex-dl mdlist <uuid> ~/Manga --dir-per-title\n"));
    CHECK(r.out.contains("Symlinks inside DIR are followed"));
    CHECK(r.out.contains("\nExit status:\n"));
    CHECK(r.out.contains("\n  4   title or list not found on MangaDex (a private list counts)\n"));
    CHECK(r.out.contains(
        "\n  7   several titles failed in different ways (see per-title results)\n"));
    CHECK(!r.out.contains("--api-base")); // hidden: a test and loopback seam, not a user option
    CHECK(!r.out.contains("--title"));    // one title for every item would collide (V7c)
    CHECK(!r.out.contains('\x1b'));
    check_fits_80_columns(r.out);
}

TEST_CASE("mdlist --help: the options block is batch's plus --dir-per-title, the last of "
          "mdlist's own options",
          "[cli][mdlist][help]") {
    const Captured mdlist = run_args({"mdlist", "--help"});
    const Captured batch = run_args({"batch", "--help"});
    const Captured root = run_args({"--help"});
    REQUIRE(mdlist.exit_code == 0);
    REQUIRE(batch.exit_code == 0);
    REQUIRE(root.exit_code == 0);

    const std::vector<std::string> own = options_block(mdlist.out);
    std::size_t at = own.size();
    for (std::size_t i = 0; i < own.size(); ++i) {
        if (own[i] == dir_per_title_line) {
            at = i;
        }
    }
    INFO("mdlist stdout: " << mdlist.out);
    REQUIRE(at < own.size());
    REQUIRE(at > 0);
    CHECK(own[at - 1].starts_with("      --fail-fast "));

    // Everything after --dir-per-title is the root's globals (fact-check amendment 8): the root's
    // own block without its -h line, in the same order and spelling.
    const std::vector<std::string> root_block = options_block(root.out);
    REQUIRE(!root_block.empty());
    CHECK(root_block.front().starts_with("  -h, --help "));
    const std::vector<std::string> root_globals(root_block.begin() + 1, root_block.end());
    const std::vector<std::string> after(own.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                                         own.end());
    CHECK(after == root_globals);

    // Byte-identical to batch's block once the one new line is removed (V7a).
    std::vector<std::string> without = own;
    without.erase(without.begin() + static_cast<std::ptrdiff_t>(at));
    CHECK(without == options_block(batch.out));
}

TEST_CASE("mdlist: the root help lists mdlist, and download and batch point at it",
          "[cli][mdlist][help]") {
    const Captured root = run_args({"--help"});
    CHECK(root.exit_code == 0);
    CHECK(root.err.empty());
    // One line, like its siblings: the one-liner fits the 44-column right column.
    CHECK(root.out.contains(
        "\n  batch                             Download every title listed in a CSV file\n"
        "  mdlist                            Download every title in a public MDList\n"
        "  follows "));
    check_fits_80_columns(root.out);

    const Captured download = run_args({"download", "--help"});
    CHECK(download.exit_code == 0);
    CHECK(download.out.contains("For many titles at once, see 'mangadex-dl batch --help' (a CSV "
                                "file)\nor 'mangadex-dl mdlist --help' (a MangaDex list).\n"));
    check_fits_80_columns(download.out);

    const Captured batch = run_args({"batch", "--help"});
    CHECK(batch.exit_code == 0);
    CHECK(batch.out.contains("\n\nFor a MangaDex list (MDList) instead of a file, see "
                             "'mangadex-dl mdlist --help'.\n\nExamples:\n"));
    check_fits_80_columns(batch.out);
}

// ---- the carried gate: every bound option through run() --------------------------------------

TEST_CASE("mdlist parses LIST, DIR and every option it binds, and dispatches the selected command",
          "[cli][mdlist][options]") {
    const mdx::FixtureManga basic = basic_with_list();
    const Tree tree{basic};
    const fs::path out = tree.path("out");
    const std::string list = mdx::list_id(basic_list_n);
    // Vol. 1 Ch. 1 has an Alpha release (chapters[0]) and a Beta release (chapters[1]); without
    // --group, Alpha wins on coverage.
    const std::string& beta_ch1 = basic.chapters.at(1).id;

    // LIST and DIR, every selection option, every run option (the hidden seam included),
    // --fail-fast and --dir-per-title, then the root's globals after both positionals.
    const std::vector<std::string> args{"mdlist",
                                        list,
                                        out.string(),
                                        "--volumes",
                                        "1",
                                        "--loose-chapters",
                                        "none",
                                        "--layout",
                                        "chapters",
                                        "-l",
                                        "en",
                                        "--group",
                                        "Beta Scans",
                                        "--data-saver",
                                        "--direction",
                                        "ltr",
                                        "--overwrite",
                                        "-n",
                                        "-j",
                                        "3",
                                        "--no-report",
                                        "--api-base",
                                        tree.api_base(),
                                        "--fail-fast",
                                        "--dir-per-title",
                                        "--json",
                                        "-v",
                                        "--color",
                                        "never"};
    const Captured r = run_args(args);
    INFO("stderr: " << r.err);
    REQUIRE(r.exit_code == 0);
    CHECK(!r.err.contains('\x1b')); // --color never

    // -v: the list line and the item line.
    CHECK(r.err.contains(std::format("mangadex-dl: list {} \"Basic List\": 1 title\n", list)));
    CHECK(r.err.contains(std::format("mangadex-dl: list:1: {}\n", basic_id)));

    // --json: one document with the list object; LIST as typed is the source.
    const Json doc = Json::parse(r.out);
    CHECK(doc.at("source") == list);
    CHECK(doc.at("dry_run") == true); // -n
    CHECK(doc.at("list").at("id") == list);
    CHECK(doc.at("list").at("name") == "Basic List");
    REQUIRE(doc.at("titles").size() == 1);
    const Json& title = doc.at("titles").at(0);
    CHECK(title.at("line") == 1);
    CHECK(title.at("manga_id") == basic_id);
    CHECK(title.at("status") == "ok");
    const fs::path title_dir = out / "Basic Title"; // --dir-per-title under DIR
    CHECK(title.at("dir") == title_dir.string());

    const Json& report = title.at("report");
    CHECK(report.at("dry_run") == true);
    CHECK(report.at("layout") == "chapters");           // --layout
    CHECK(report.at("manga").at("direction") == "ltr"); // --direction (ja origin: rtl)
    CHECK(report.at("manga").at("language") == "en");   // -l
    // --volumes 1 and --loose-chapters none: only volume 1's two chapters, one file each.
    std::vector<std::string> paths{};
    for (const Json& epub : report.at("epubs")) {
        paths.push_back(epub.at("path").get<std::string>());
    }
    CHECK(paths ==
          std::vector<std::string>{(title_dir / "Basic Title - Vol. 01 Ch. 001.epub").string(),
                                   (title_dir / "Basic Title - Vol. 01 Ch. 002.epub").string()});
    // --group "Beta Scans": the Beta release of Vol. 1 Ch. 1 is kept, the Alpha one dropped.
    bool beta_kept = false;
    for (const Json& skipped : report.at("skipped_chapters")) {
        if (skipped.at("chapter") == "1" && skipped.at("kept") == beta_ch1) {
            beta_kept = true;
        }
    }
    CHECK(beta_kept);
    CHECK(!fs::exists(out)); // a dry run creates nothing
}

TEST_CASE("mdlist -l and --fail-fast reach every item through run()", "[cli][mdlist][options]") {
    SECTION("-l fr: the clean title has no French chapters, so item 1 ends 5") {
        const Tree tree{mdx::clean_fixture()};
        const Captured r = run_args({"mdlist", mdx::list_id(1), tree.path("out").string(),
                                     "--api-base", tree.api_base(), "--dry-run", "-l", "fr"});
        INFO("stderr: " << r.err);
        CHECK(r.exit_code == 5);
        CHECK(r.out.empty());
        CHECK(r.err.contains(std::format("list:1: {}: ", clean_id)));
    }
    SECTION("--fail-fast: L8 is 5 then 4 (mixed, 7); the flag stops at item 1 with 5") {
        const Tree tree{mdx::nochapters_fixture()};
        const std::vector<std::string> args{
            "--json",     "mdlist",        mdx::list_id(8), tree.path("out").string(),
            "--api-base", tree.api_base(), "--dry-run"};

        const Captured all = run_args(args);
        INFO("stderr: " << all.err);
        CHECK(all.exit_code == 7);
        const Json all_doc = Json::parse(all.out);
        REQUIRE(all_doc.at("titles").size() == 2);
        CHECK(all_doc.at("titles").at(0).at("manga_id") == nochapters_id);
        CHECK(all_doc.at("titles").at(1).at("status") == "not_found");

        std::vector<std::string> with_flag = args;
        with_flag.emplace_back("--fail-fast");
        const Captured fast = run_args(with_flag);
        INFO("stderr: " << fast.err);
        CHECK(fast.exit_code == 5);
        CHECK(fast.err.contains("stopped at item 1: --fail-fast; 1 title not run"));
        const Json fast_doc = Json::parse(fast.out);
        REQUIRE(fast_doc.at("titles").size() == 2);
        CHECK(fast_doc.at("titles").at(0).at("status") == "no_chapters");
        CHECK(fast_doc.at("titles").at(1).at("status") == "not_run");
    }
}

// ---- V7b: global flags after both positionals ------------------------------------------------

TEST_CASE("mdlist LIST DIR --json -q --dir-per-title: global flags after the positionals apply",
          "[cli][mdlist][globals]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    // L4 lists the clean title twice: without -q its "same as item 1" warning is the only stderr.
    const std::vector<std::string> base_args{"mdlist",     mdx::list_id(4), out.string(),
                                             "--api-base", tree.api_base(), "--dry-run"};

    const Captured plain = run_args(base_args);
    INFO("plain stderr: " << plain.err);
    REQUIRE(plain.exit_code == 0);
    CHECK(plain.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(plain.err ==
          std::format("mangadex-dl: warning: list:2: {}: same as item 1; skipped\n", clean_id));

    std::vector<std::string> globals_after = base_args;
    globals_after.insert(globals_after.end(), {"--json", "-q", "--dir-per-title"});
    const Captured r = run_args(globals_after);
    INFO("stderr: " << r.err);
    REQUIRE(r.exit_code == 0);
    CHECK(r.err.empty()); // -q applied: the duplicate warning is gone
    // --json applied: one document instead of the path stream; --dir-per-title gave the item a
    // directory of its own.
    const Json doc = Json::parse(r.out);
    CHECK(doc.at("source") == mdx::list_id(4));
    CHECK(doc.at("list").at("name") == "Twice");
    REQUIRE(doc.at("titles").size() == 2);
    CHECK(doc.at("titles").at(0).at("status") == "ok");
    CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
    CHECK(doc.at("titles").at(1).at("status") == "duplicate");
    CHECK(doc.at("titles").at(1).at("duplicate_of") == 1);
    CHECK(!fs::exists(out));
}

TEST_CASE("mdlist LIST DIR -v --color always: verbose and color after the positionals apply",
          "[cli][mdlist][globals]") {
    const Tree tree{mdx::clean_fixture()};
    // L2: the clean title, then one the tree doesn't serve (exit 4).
    const Captured r = run_args({"mdlist", mdx::list_id(2), tree.path("out").string(), "--api-base",
                                 tree.api_base(), "--dry-run", "-v", "--color", "always"});
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 4);
    CHECK(r.out == path_lines(tree.path("out"), {clean_vol1, clean_vol2}));
    CHECK(r.err.contains(
        std::format("mangadex-dl: list {} \"Clean And Unknown\": 2 titles\n", mdx::list_id(2))));
    CHECK(r.err.contains(std::format("mangadex-dl: list:1: {}\n", clean_id)));
    CHECK(r.err.contains("\x1b[")); // a colored error prefix on stderr
}

// ---- V7c: --title is not an mdlist option ----------------------------------------------------

TEST_CASE("mdlist --title says to download that title on its own, in every spelling",
          "[cli][mdlist][usage]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const std::string list = mdx::list_id(1);
    const std::string dir = out.string();
    const std::string expected = std::format(
        "mangadex-dl: error: mdlist has no --title option; to name a title yourself, download it "
        "on its own with 'mangadex-dl download <title> --title ...'\n{}",
        mdlist_hint);

    const std::vector<std::vector<std::string>> spellings{
        {"mdlist", list, "--title", "X"},      {"mdlist", list, dir, "--title", "X"},
        {"mdlist", list, "--title=X"},         {"mdlist", list, dir, "--title=X"},
        {"mdlist", "--title", "X", list, dir}, {"mdlist", list, dir, "--title"},
    };
    for (std::vector<std::string> args : spellings) {
        args.insert(args.begin() + 1, {"--api-base", std::string{missing_api_base}});
        const Captured r = run_args(args);
        INFO("args: " << args.at(3) << " ... (" << args.size() << " words)");
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == expected);
        CHECK(!fs::exists(out));
    }

    SECTION("any other unknown option keeps CLI11's message") {
        const Captured r = run_args(
            {"mdlist", "--api-base", std::string{missing_api_base}, list, dir, "--titles", "X"});
        check_mdlist_usage(r);
        CHECK(r.err.contains("--titles"));
        CHECK(!r.err.contains("has no --title option"));
    }
    SECTION("batch keeps its own --title message, byte for byte") {
        const Captured r = run_args({"batch", "--api-base", std::string{missing_api_base},
                                     "titles.csv", dir, "--title", "X"});
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == "mangadex-dl: error: batch has no --title option; give each row a 'title' "
                       "column instead\n"
                       "Run 'mangadex-dl batch --help' for more information.\n");
    }
}

// ---- V7d and the other usage errors ----------------------------------------------------------

TEST_CASE("mdlist usage errors exit 2 with the mdlist hint and create nothing",
          "[cli][mdlist][usage]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const std::string list = mdx::list_id(1);
    const std::string dir = out.string();

    struct Case {
        std::string_view name{};
        std::vector<std::string> args{};
        std::string message{}; ///< contained in stderr
    };
    const std::vector<Case> cases{
        // V7d: a missing LIST is CLI11's RequiredError, reached mdlist.
        {.name = "no LIST", .args = {"mdlist"}, .message = "LIST"},
        {.name = "no LIST, only options",
         .args = {"mdlist", "--dry-run", "-j", "2", "--dir-per-title"},
         .message = "LIST"},
        {.name = "three positionals", .args = {"mdlist", list, dir, "extra"}, .message = "extra"},
        {.name = "-j 9", .args = {"mdlist", list, dir, "-j", "9"}, .message = "--jobs"},
        {.name = "--layout bogus",
         .args = {"mdlist", list, dir, "--layout", "bogus"},
         .message = "--layout"},
        {.name = "--group without a value",
         .args = {"mdlist", list, dir, "--group"},
         .message = "--group"},
        // A flag takes no value; CLI11 accepts only a boolean word after '=', as for --fail-fast.
        {.name = "--dir-per-title with a non-boolean value",
         .args = {"mdlist", list, dir, "--dir-per-title=bogus"},
         .message = "--dir-per-title"},
        // Semantic usage errors, after parse: still the mdlist hint.
        {.name = "LIST is a title URL",
         .args = {"mdlist", std::format("https://mangadex.org/title/{}", clean_id), dir},
         .message = "LIST is a MangaDex title, not a list"},
        {.name = "LIST is a CSV path",
         .args = {"mdlist", "titles.csv", dir},
         .message = "LIST must be a MangaDex list UUID"},
        {.name = "DIR looks like an id",
         .args = {"mdlist", list, std::string{clean_id}},
         .message = "DIR looks like a MangaDex id"},
        {.name = "--lang bogus",
         .args = {"mdlist", list, dir, "--lang", "bogus!"},
         .message = "--lang"},
    };
    for (const Case& c : cases) {
        INFO("case: " << c.name);
        // `mdlist --api-base <missing> ...`: a usage error that slipped through would exit 1.
        std::vector<std::string> args = c.args;
        args.insert(args.begin() + 1, {"--api-base", std::string{missing_api_base}});
        const Captured r = run_args(args);
        check_mdlist_usage(r);
        INFO("stderr: " << r.err);
        CHECK(r.err.contains(c.message));
        CHECK(!fs::exists(out));
    }
}

// ---- download and batch next to mdlist -------------------------------------------------------

TEST_CASE("mdlist registration leaves download and batch working: dry-run smokes",
          "[cli][mdlist][download][batch]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");

    const Captured download = run_args({"download", "--api-base", tree.api_base(),
                                        std::string{clean_id}, out.string(), "--dry-run"});
    INFO("download stderr: " << download.err);
    CHECK(download.exit_code == 0);
    CHECK(download.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(download.err.empty());

    const fs::path csv = tree.path("titles.csv");
    write_file(csv, std::format("{}\n", clean_id));
    const Captured batch =
        run_args({"batch", csv.string(), out.string(), "--api-base", tree.api_base(), "--dry-run"});
    INFO("batch stderr: " << batch.err);
    CHECK(batch.exit_code == 0);
    CHECK(batch.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(batch.err.empty());

    const Captured mdlist = run_args(
        {"mdlist", mdx::list_id(1), out.string(), "--api-base", tree.api_base(), "--dry-run"});
    INFO("mdlist stderr: " << mdlist.err);
    CHECK(mdlist.exit_code == 0);
    CHECK(mdlist.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(mdlist.err.empty());
    CHECK(!fs::exists(out));
}

} // namespace
