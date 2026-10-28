// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_parse_test.cpp: `batch` through the real parse-then-dispatch path, run(args,
// Io&) (batch plan T6). batch_cli_test.cpp drives run_batch_command with hand-filled arguments;
// this file proves that CLI11 binds them: register_batch and BatchCommand::bind under ASan+UBSan,
// the help surface, the fact-checked argv shapes (V-A1: a lone '-' is FILE; V-A2: global flags
// after the positionals still apply; V-A3: the options shared with `download`; V-A4: `--title`
// is not a batch option) and the usage errors, plus a `download` smoke next to the new command.
//
// Every run that gets past parsing reads a fixture tree (tests/support/mdx_fixture.hpp) through
// the hidden `--api-base file://<tree>` seam in a fresh temporary directory, so nothing reaches
// the network and every output directory is inside the test's own directory.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec). Every name
// starts with "batch" so `pixi run tsan -R 'cli\.batch'` selects them too.

#include "app/cli.hpp"
#include "app/io.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
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

#include <fcntl.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using md::test::Captured;
using Json = nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view basic_id = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view nochapters_id = "55555555-5555-4555-8555-555555555555";
constexpr std::string_view unknown_id = "66666666-6666-4666-8666-666666666666";

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";

constexpr std::string_view batch_hint = "Run 'mangadex-dl batch --help' for more information.\n";

/// A fixture root that never exists: a usage case that slipped past parsing would fail to build
/// its backend (exit 1) instead of exiting 2.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";

// ---- running the CLI -------------------------------------------------------------------------

/// run() over two string streams for an argument list built at runtime. argv[0] is supplied;
/// `args` outlives the call (run() returns before this function does). `stdin_fd` is borrowed
/// (-1 = a closed stdin).
[[nodiscard]] Captured run_args(const std::vector<std::string>& args, int stdin_fd = -1) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back("mangadex-dl"); // a string literal: static storage
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    std::ostringstream out;
    std::ostringstream err;
    md::app::Io io{.out = out, .err = err, .stdin_fd = stdin_fd};
    Captured result;
    result.exit_code = md::app::run(std::span<const char* const>{argv}, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

/// `front` followed by `rest`.
[[nodiscard]] std::vector<std::string> concat(std::initializer_list<std::string> front,
                                              const std::vector<std::string>& rest) {
    std::vector<std::string> all{front};
    all.insert(all.end(), rest.begin(), rest.end());
    return all;
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-batch-parse");
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

/// open(2) without O_CREAT (the variadic mode is never read).
[[nodiscard]] md::UniqueFd open_read(const fs::path& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    md::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    REQUIRE(fd.valid());
    return fd;
}

/// A fixture tree for one title under a fresh temporary directory; the CSV lists and output
/// directories go next to it. Not copyable or movable (TempDir owns the directory).
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

    /// Writes `<tmp>/<name>` with `content` and returns its path.
    [[nodiscard]] fs::path csv(std::string_view name, std::string_view content) const {
        fs::path file = path(name);
        write_file(file, content);
        return file;
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

/// Every line of `text` fits an 80-column terminal (the help is ASCII, so bytes are columns).
void check_fits_80_columns(std::string_view text) {
    for (const std::string& line : lines_of(text)) {
        INFO("line: " << line);
        CHECK(line.size() <= 80);
    }
}

/// A usage error from `batch`: exit 2, nothing on stdout, the batch hint last, no ESC.
void check_batch_usage(const Captured& r) {
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("mangadex-dl: error: "));
    CHECK(r.err.ends_with(batch_hint));
    CHECK(!r.err.contains('\x1b'));
}

// ---- help ------------------------------------------------------------------------------------

TEST_CASE("batch --help documents FILE, DIR, the CSV format and exit 7, without the hidden seam",
          "[cli][batch][help]") {
    const Captured r = run_args({"batch", "--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    INFO("stdout: " << r.out);
    CHECK(r.out.contains("Download every title listed in a CSV file"));
    CHECK(r.out.contains("FILE"));
    CHECK(r.out.contains("DIR"));
    CHECK(r.out.contains("--fail-fast"));
    CHECK(r.out.contains("--group"));
    CHECK(r.out.contains("--dry-run"));
    CHECK(r.out.contains("\nCSV file:\n"));
    CHECK(r.out.contains("    manga            the title, as MANGA in 'download' (required)\n"));
    CHECK(r.out.contains("grep -v '^#done' titles.csv | mangadex-dl batch - ~/Manga\n"));
    CHECK(r.out.contains("    note             ignored\n"
                         "  For example:\n"
                         "    manga,title,dir\n"
                         "    https://mangadex.org/title/<uuid>,,\n"
                         "    <uuid>,My Title,series/my-title\n"
                         "\n"
                         "For a MangaDex list (MDList) instead of a file, see 'mangadex-dl mdlist "
                         "--help'.\n"
                         "\n"
                         "Examples:\n"));
    CHECK(r.out.contains("Symlinks inside DIR are followed"));
    CHECK(r.out.contains("\nExit status:\n"));
    CHECK(r.out.contains(
        "\n  7   batch/mdlist: titles failed in more than one way (see per-title results)\n"));
    CHECK(!r.out.contains("--api-base"));   // hidden: a test and loopback seam, not a user option
    CHECK(!r.out.contains("--title TEXT")); // a per-row column only, not an option
    CHECK(!r.out.contains('\x1b'));
    check_fits_80_columns(r.out);
}

TEST_CASE(
    "batch: the root and download help list batch, row 7 and the pointers to batch and mdlist",
    "[cli][batch][help]") {
    const Captured root = run_args({"--help"});
    CHECK(root.exit_code == 0);
    CHECK(root.err.empty());
    CHECK(root.out.contains("\n  batch "));
    CHECK(root.out.contains("\n  7   batch/mdlist: "));
    check_fits_80_columns(root.out);

    const Captured download = run_args({"download", "--help"});
    CHECK(download.exit_code == 0);
    CHECK(download.err.empty());
    CHECK(download.out.contains(
        "the new ones, so an interrupted run can be restarted and a series kept up to\n"
        "date.\n\n"
        "For many titles at once, see 'mangadex-dl batch --help' (a CSV file)\n"
        "or 'mangadex-dl mdlist --help' (a MangaDex list).\n\n"
        "Exit status:\n"));
    CHECK(download.out.contains("\n  7   batch/mdlist: "));
    check_fits_80_columns(download.out);
}

// ---- V-A1: a lone '-' is FILE ----------------------------------------------------------------

TEST_CASE("batch - DIR binds '-' to FILE and reads the list from stdin", "[cli][batch][stdin]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
    const fs::path out = tree.path("out");
    const std::string base = tree.api_base();

    struct Shape {
        std::string_view name{};
        std::vector<std::string> args{};
    };
    const std::vector<Shape> shapes{
        {.name = "batch - DIR --dry-run",
         .args = {"batch", "-", out.string(), "--api-base", base, "--dry-run"}},
        {.name = "batch --dry-run -- - DIR",
         .args = {"batch", "--api-base", base, "--dry-run", "--", "-", out.string()}},
    };
    for (const Shape& shape : shapes) {
        INFO("shape: " << shape.name);
        const md::UniqueFd fd = open_read(list);
        const Captured r = run_args(shape.args, fd.get());
        INFO("stderr: " << r.err);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err.empty());
        CHECK(!fs::exists(out)); // a dry run creates nothing
    }
}

TEST_CASE("batch - with a closed stdin is a read failure, not a usage error",
          "[cli][batch][stdin]") {
    const Tree tree{mdx::clean_fixture()};
    const Captured r = run_args({"batch", "-", tree.path("out").string(), "--api-base",
                                 tree.api_base()}); // stdin_fd -1: closed
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("mangadex-dl: error: cannot read <stdin>: "));
    CHECK(!r.err.contains(batch_hint));
    CHECK(!fs::exists(tree.path("out")));
}

// ---- V-A2: global flags after the positionals ------------------------------------------------

TEST_CASE("batch FILE DIR --json -q: global flags after the positionals apply",
          "[cli][batch][globals]") {
    const Tree tree{mdx::clean_fixture()};
    // Line 2 duplicates line 1: without -q its "skipped" warning is the only stderr output.
    const fs::path list = tree.csv("titles.csv", std::format("{}\n{}\n", clean_id, clean_id));
    const fs::path out = tree.path("out");
    const std::vector<std::string> base_args{"batch",      list.string(),   out.string(),
                                             "--api-base", tree.api_base(), "--dry-run"};

    const Captured plain = run_args(base_args);
    INFO("plain stderr: " << plain.err);
    REQUIRE(plain.exit_code == 0);
    CHECK(plain.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(plain.err ==
          std::format("mangadex-dl: warning: {}:2: same as line 1; skipped\n", list.string()));

    std::vector<std::string> globals_after = base_args;
    globals_after.insert(globals_after.end(), {"--json", "-q"});
    const Captured r = run_args(globals_after);
    INFO("stderr: " << r.err);
    REQUIRE(r.exit_code == 0);
    CHECK(r.err.empty()); // -q applied: the duplicate warning is gone
    // --json applied: one document instead of the path stream.
    const Json doc = Json::parse(r.out);
    CHECK(doc.at("source") == list.string());
    CHECK(doc.at("dry_run") == true);
    REQUIRE(doc.at("titles").size() == 2);
    CHECK(doc.at("titles").at(0).at("status") == "ok");
    CHECK(doc.at("titles").at(1).at("status") == "duplicate");
    CHECK(doc.at("titles").at(1).at("duplicate_of") == 1);
    CHECK(!fs::exists(out));
}

TEST_CASE("batch FILE DIR -v --color always: verbose and color after the positionals apply",
          "[cli][batch][globals]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", nochapters_id));
    const Captured r = run_args({"batch", list.string(), tree.path("out").string(), "--api-base",
                                 tree.api_base(), "--dry-run", "-v", "--color", "always"});
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 5);
    CHECK(r.out.empty());
    // -v: the row-start line; --color always: a colored error prefix on stderr.
    CHECK(r.err.contains(std::format("mangadex-dl: {}:1: {}\n", list.string(), nochapters_id)));
    CHECK(r.err.contains("\x1b["));
}

// ---- V-A3: the options shared with download --------------------------------------------------

TEST_CASE("batch parses download's shared options and --fail-fast, and they reach every row",
          "[cli][batch][options]") {
    const mdx::FixtureManga basic = mdx::basic_fixture();
    const Tree tree{basic};
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", basic_id));
    // Vol. 1 Ch. 1 has an Alpha release (chapters[0]) and a Beta release (chapters[1]). Without
    // --group, Alpha wins on coverage; the first matching --group wins as "preferred group".
    const std::string& alpha_ch1 = basic.chapters.at(0).id;
    const std::string& beta_ch1 = basic.chapters.at(1).id;
    const fs::path out = tree.path("out");

    struct Shape {
        std::string_view name{};
        std::vector<std::string> options{};
        std::string kept{};
        std::string_view rule{};
    };
    const std::vector<Shape> shapes{
        {.name = "--group B --group A -j 8 --fail-fast",
         .options = {"--group", "Beta Scans", "--group", "Alpha Scans", "-j", "8", "--fail-fast"},
         .kept = beta_ch1,
         .rule = "preferred group (--group)"},
        {.name = "--group A --group B",
         .options = {"--group", "Alpha Scans", "--group", "Beta Scans"},
         .kept = alpha_ch1,
         .rule = "preferred group (--group)"},
        {.name = "no --group",
         .options = {},
         .kept = alpha_ch1,
         .rule = "group covering the most chapters"},
    };
    for (const Shape& shape : shapes) {
        INFO("shape: " << shape.name);
        const Captured r = run_args(concat({"-v", "batch", list.string(), out.string(),
                                            "--api-base", tree.api_base(), "--dry-run"},
                                           shape.options));
        INFO("stderr: " << r.err);
        REQUIRE(r.exit_code == 0);
        CHECK(r.err.contains(
            std::format("Vol. 1 Ch. 1: kept {} ({}); dropped ", shape.kept, shape.rule)));
        CHECK(lines_of(r.out).size() == 5);
    }
    CHECK(!fs::exists(out));
}

TEST_CASE("batch --fail-fast stops at the first title that fails", "[cli][batch][options]") {
    const Tree tree{mdx::nochapters_fixture()};
    // Row 1 ends 5 (no chapters), row 2 ends 4 (not in the tree): mixed, so 7 without the flag.
    const fs::path list =
        tree.csv("titles.csv", std::format("{}\n{}\n", nochapters_id, unknown_id));
    const std::vector<std::string> args{
        "--json",     "batch",         list.string(), tree.path("out").string(),
        "--api-base", tree.api_base(), "--dry-run"};

    const Captured all = run_args(args);
    INFO("stderr: " << all.err);
    CHECK(all.exit_code == 7);
    const Json all_doc = Json::parse(all.out);
    REQUIRE(all_doc.at("titles").size() == 2);
    CHECK(all_doc.at("titles").at(1).at("status") == "not_found");

    std::vector<std::string> with_flag = args;
    with_flag.emplace_back("--fail-fast");
    const Captured fast = run_args(with_flag);
    INFO("stderr: " << fast.err);
    CHECK(fast.exit_code == 5);
    CHECK(fast.err.contains("--fail-fast"));
    const Json fast_doc = Json::parse(fast.out);
    REQUIRE(fast_doc.at("titles").size() == 2);
    CHECK(fast_doc.at("titles").at(0).at("status") == "no_chapters");
    CHECK(fast_doc.at("titles").at(1).at("status") == "not_run");
}

// ---- usage errors ----------------------------------------------------------------------------

TEST_CASE("batch usage errors exit 2 with the batch hint and create nothing",
          "[cli][batch][usage]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
    const fs::path out = tree.path("out");
    const std::string file = list.string();
    const std::string dir = out.string();
    const std::string base{missing_api_base};

    struct Case {
        std::string_view name{};
        std::vector<std::string> args{};
        std::string message{}; ///< contained in stderr
    };
    const std::vector<Case> cases{
        {.name = "no FILE", .args = {"batch"}, .message = "FILE"},
        {.name = "no FILE, only options",
         .args = {"batch", "--dry-run", "-j", "2"},
         .message = "FILE"},
        // V-A4: --title is a per-row column; X must never be bound to DIR.
        {.name = "--title",
         .args = {"batch", file, "--title", "X"},
         .message = "batch has no --title option"},
        {.name = "--title after DIR",
         .args = {"batch", file, dir, "--title", "X"},
         .message = "batch has no --title option"},
        {.name = "three positionals", .args = {"batch", file, dir, "extra"}, .message = "extra"},
        {.name = "-j 9", .args = {"batch", file, dir, "-j", "9"}, .message = "--jobs"},
        {.name = "--layout bogus",
         .args = {"batch", file, dir, "--layout", "bogus"},
         .message = "--layout"},
        {.name = "--group without a value",
         .args = {"batch", file, dir, "--group"},
         .message = "--group"},
        {.name = "--direction bogus",
         .args = {"batch", file, dir, "--direction", "up"},
         .message = "--direction"},
        // Semantic usage errors, after parse: still the batch hint.
        {.name = "FILE looks like a title",
         .args = {"batch", std::string{clean_id}, dir},
         .message = "FILE looks like a MangaDex title"},
        {.name = "--lang bogus",
         .args = {"batch", file, dir, "--lang", "bogus!"},
         .message = "--lang"},
    };
    for (const Case& c : cases) {
        INFO("case: " << c.name);
        // `batch --api-base <missing> ...`: a usage error that slipped through would exit 1.
        std::vector<std::string> args = c.args;
        args.insert(args.begin() + 1, {"--api-base", base});
        const Captured r = run_args(args);
        check_batch_usage(r);
        INFO("stderr: " << r.err);
        CHECK(r.err.contains(c.message));
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("batch --title says the title is a per-row column, in every spelling",
          "[cli][batch][usage]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
    const fs::path out = tree.path("out");
    const std::string file = list.string();
    const std::string dir = out.string();
    const std::string expected = std::format(
        "mangadex-dl: error: batch has no --title option; give each row a 'title' column "
        "instead\n{}",
        batch_hint);

    const std::vector<std::vector<std::string>> spellings{
        {"batch", file, "--title", "X"},      {"batch", file, dir, "--title", "X"},
        {"batch", file, "--title=X"},         {"batch", file, dir, "--title=X"},
        {"batch", "--title", "X", file, dir}, {"batch", file, dir, "--title"},
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
            {"batch", "--api-base", std::string{missing_api_base}, file, dir, "--titles", "X"});
        check_batch_usage(r);
        CHECK(r.err.contains("--titles"));
        CHECK(!r.err.contains("batch has no --title option"));
    }
    SECTION("download keeps its --title option") {
        const Captured r = run_args({"download", "--api-base", tree.api_base(),
                                     std::string{clean_id}, dir, "--title", "X", "--dry-run"});
        INFO("stderr: " << r.err);
        CHECK(r.exit_code == 0);
        CHECK(!r.err.contains("batch has no --title option"));
    }
}

// ---- download next to batch ------------------------------------------------------------------

TEST_CASE("batch registration leaves download working: a dry-run smoke", "[cli][batch][download]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const Captured r = run_args({"download", "--api-base", tree.api_base(), std::string{clean_id},
                                 out.string(), "--dry-run"});
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(!fs::exists(out));
}

} // namespace
