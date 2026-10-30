// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_dir_per_title_parse_test.cpp: `batch --dir-per-title` through the real
// parse-then-dispatch path, run(args, Io&) (batch-dir-per-title plan T2, spec AC22-AC23).
// batch_dir_per_title_test.cpp drives run_batch_command with a hand-filled RawBatchArgs; this file
// proves that CLI11 binds the flag: the help surface (the option line after --fail-fast, the
// footer paragraph and example), `download` not knowing the flag, the fact-checked argv shapes
// (V-P1: the flag before, between and after the positionals, never taken as DIR) and parity with
// `mdlist --dir-per-title=<value>` (V-B1, fact-check C2: values only in the `=` form).
//
// Every run that gets past parsing reads the clean fixture tree (tests/support/mdx_fixture.hpp)
// through the hidden `--api-base file://<tree>` seam with --dry-run, so nothing reaches the
// network and nothing is created.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec). Every name
// starts with "batch --dir-per-title" so `pixi run tsan -R 'cli\.batch --dir-per-title'` selects
// them too.

#include "app/cli.hpp"
#include "app/io.hpp"
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

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using md::test::Captured;
using Json = nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view clean_dir = "Clean Title";

constexpr std::string_view batch_hint = "Run 'mangadex-dl batch --help' for more information.\n";
constexpr std::string_view mdlist_hint = "Run 'mangadex-dl mdlist --help' for more information.\n";
constexpr std::string_view download_hint =
    "Run 'mangadex-dl download --help' for more information.\n";

/// The `--help` line of `--dir-per-title`, in batch's and mdlist's options blocks alike.
constexpr std::string_view dir_per_title_line =
    "      --dir-per-title               Put each title in a subdirectory of DIR";

/// A fixture root that never exists: a usage case that slipped past parsing would fail to build
/// its backend (exit 1) instead of exiting 2.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";

// ---- running the CLI -------------------------------------------------------------------------

/// run() over two string streams for an argument list built at runtime. argv[0] is supplied;
/// `args` outlives the call (run() returns before this function does). Stdin is closed (-1):
/// no list here reads it.
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
    auto tmp = md::test::TempDir::create("mangadex-dl-batch-dpt-parse");
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

/// The clean fixture tree (it also serves L1, a list of the clean title) under a fresh temporary
/// directory, and a one-row CSV of the clean title next to it. Not copyable or movable (TempDir
/// owns the directory).
class Tree {
public:
    Tree() : tmp_(make_temp_dir()), root_(tmp_.path() / "fixture") {
        const auto written = mdx::write_fixture_tree(root_, mdx::clean_fixture());
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        write_file(csv(), std::format("{}\n", clean_id));
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Tree(Tree&&) = delete;
    Tree& operator=(Tree&&) = delete;
    ~Tree() = default;

    [[nodiscard]] std::string api_base() const { return "file://" + root_.string(); }

    /// `<tmp>/<name>`, not created.
    [[nodiscard]] fs::path path(std::string_view name) const { return tmp_.path() / name; }

    /// The one-row CSV of the clean title.
    [[nodiscard]] fs::path csv() const { return path("titles.csv"); }

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

/// The lines of a help text's "OPTIONS:" block, without the heading (up to the first blank line).
[[nodiscard]] std::vector<std::string> options_block(std::string_view help) {
    const std::size_t heading = help.find("\nOPTIONS:\n");
    REQUIRE(heading != std::string_view::npos);
    const std::size_t start = heading + std::string_view{"\nOPTIONS:\n"}.size();
    const std::size_t end = help.find("\n\n", start);
    REQUIRE(end != std::string_view::npos);
    return lines_of(help.substr(start, end - start));
}

/// The index of `line` in `lines`, or lines.size() when it is not there.
[[nodiscard]] std::size_t index_of(const std::vector<std::string>& lines, std::string_view line) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == line) {
            return i;
        }
    }
    return lines.size();
}

/// A usage error: exit 2, nothing on stdout, `hint` last, no ESC.
void check_usage(const Captured& r, std::string_view hint) {
    INFO("stderr: " << r.err);
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("mangadex-dl: error: "));
    CHECK(r.err.ends_with(hint));
    CHECK(!r.err.contains('\x1b'));
}

/// Where a successful dry run of the clean title planned its files below `out`.
enum class Layout { flat, per_title, other };

[[nodiscard]] Layout layout_of(const Captured& r, const fs::path& out) {
    if (r.out == path_lines(out, {clean_vol1, clean_vol2})) {
        return Layout::flat;
    }
    if (r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2})) {
        return Layout::per_title;
    }
    return Layout::other;
}

} // namespace

// ---- help (AC22) -------------------------------------------------------------------------------

TEST_CASE("batch --dir-per-title --help: the option follows --fail-fast, byte-identical to "
          "mdlist's",
          "[cli][batch][dir-per-title][help]") {
    const Captured batch = run_args({"batch", "--help"});
    const Captured mdlist = run_args({"mdlist", "--help"});
    REQUIRE(batch.exit_code == 0);
    REQUIRE(mdlist.exit_code == 0);
    CHECK(batch.err.empty());
    INFO("batch stdout: " << batch.out);

    const std::vector<std::string> own = options_block(batch.out);
    const std::size_t at = index_of(own, dir_per_title_line);
    REQUIRE(at < own.size());
    REQUIRE(at > 0);
    CHECK(own[at - 1].starts_with("      --fail-fast "));
    CHECK(index_of(options_block(mdlist.out), dir_per_title_line) <
          options_block(mdlist.out).size());
    CHECK(!batch.out.contains('\x1b'));
    check_fits_80_columns(batch.out);
}

TEST_CASE("batch --dir-per-title --help: the footer explains the flag after the CSV example and "
          "shows it in the examples",
          "[cli][batch][dir-per-title][help]") {
    const Captured r = run_args({"batch", "--help"});
    REQUIRE(r.exit_code == 0);
    INFO("stdout: " << r.out);
    CHECK(r.out.contains(
        "    <uuid>,My Title,series/my-title\n"
        "  With --dir-per-title, each title goes into its own subdirectory, named like\n"
        "  its files: DIR/<title>/<title> - Vol. 01.epub. A row's dir cell replaces\n"
        "  that name, so the row goes into DIR/<dir> exactly as without the flag.\n"
        "\n"));
    CHECK(r.out.contains("  Everything in the list, into ~/Manga:\n"
                         "    mangadex-dl batch titles.csv ~/Manga\n"
                         "  The same, one folder per title:\n"
                         "    mangadex-dl batch titles.csv ~/Manga --dir-per-title\n"
                         "  Show what would be written, without downloading anything:\n"));
    // The dir column line is unchanged.
    CHECK(r.out.contains("\n    dir              subdirectory of DIR for this row\n"));
    check_fits_80_columns(r.out);
}

TEST_CASE("batch --dir-per-title: download has no such option", "[cli][batch][dir-per-title]") {
    const Captured help = run_args({"download", "--help"});
    CHECK(help.exit_code == 0);
    CHECK(!help.out.contains("--dir-per-title"));

    const md::test::TempDir tmp = make_temp_dir();
    const fs::path out = tmp.path() / "out";
    const Captured r = run_args({"download", "--api-base", std::string{missing_api_base},
                                 std::string{clean_id}, out.string(), "--dir-per-title"});
    check_usage(r, download_hint);
    CHECK(r.err.contains("--dir-per-title"));
    CHECK(!fs::exists(out));
}

// ---- argv shapes (V-P1) ------------------------------------------------------------------------

TEST_CASE("batch --dir-per-title: the flag works before, between and after FILE and DIR",
          "[cli][batch][dir-per-title][options]") {
    const Tree tree;
    const std::string file = tree.csv().string();
    const fs::path out = tree.path("out");
    const std::string dir = out.string();
    const std::string base = tree.api_base();

    struct Case {
        std::string_view name{};
        std::vector<std::string> args{};
    };
    const std::vector<Case> cases{
        {.name = "after DIR",
         .args = {"batch", "--api-base", base, "--dry-run", file, dir, "--dir-per-title"}},
        {.name = "between FILE and DIR",
         .args = {"batch", "--api-base", base, "--dry-run", file, "--dir-per-title", dir}},
        {.name = "before FILE",
         .args = {"batch", "--dir-per-title", "--api-base", base, "--dry-run", file, dir}},
    };
    for (const Case& c : cases) {
        INFO("case: " << c.name);
        const Captured r = run_args(c.args);
        INFO("stderr: " << r.err);
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        CHECK(layout_of(r, out) == Layout::per_title);
        CHECK(!fs::exists(out));
    }

    SECTION("FILE --dir-per-title: DIR stays '.'; the flag is never taken as DIR") {
        const Captured r =
            run_args({"batch", "--api-base", base, "--dry-run", file, "--dir-per-title"});
        INFO("stderr: " << r.err);
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        CHECK(layout_of(r, fs::path{"."}) == Layout::per_title);
        CHECK(!r.out.contains("--dir-per-title"));
    }
    SECTION("--dir-per-title FILE DIR --json: the document has dir DIR/<title>") {
        const Captured r = run_args(
            {"batch", "--dir-per-title", "--api-base", base, "--dry-run", file, dir, "--json"});
        INFO("stderr: " << r.err);
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        REQUIRE(Json::accept(r.out));
        const Json doc = Json::parse(r.out);
        CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
        CHECK(doc.at("titles").at(0).at("report").at("epubs").at(0).at("path") ==
              (out / clean_dir / clean_vol1).string());
        CHECK(!fs::exists(out));
    }
}

// ---- parity with mdlist (AC23) -----------------------------------------------------------------

TEST_CASE("batch --dir-per-title=<value> parses exactly as mdlist --dir-per-title=<value>",
          "[cli][batch][dir-per-title][options]") {
    const Tree tree;
    const std::string base = tree.api_base();
    const std::string list = mdx::list_id(1); // L1: the clean title
    const fs::path batch_out = tree.path("out-batch");
    const fs::path mdlist_out = tree.path("out-mdlist");

    // Fact-check C2: only the `=` form takes a value (a separate word would become FILE/DIR).
    const std::vector<std::string> values{"true", "false", "1",  "0",     "yes", "no",   "on",
                                          "off",  "",      "-1", "bogus", "2",   "TRUE", "Off"};
    bool saw_true = false;
    bool saw_false = false;
    bool saw_error = false;
    for (const std::string& value : values) {
        INFO("--dir-per-title=" << value);
        const std::string flag = "--dir-per-title=" + value;
        const Captured batch = run_args({"batch", "--api-base", base, "--dry-run",
                                         tree.csv().string(), batch_out.string(), flag});
        const Captured mdlist =
            run_args({"mdlist", "--api-base", base, "--dry-run", list, mdlist_out.string(), flag});
        INFO("batch stderr: " << batch.err);
        INFO("mdlist stderr: " << mdlist.err);
        CHECK(batch.exit_code == mdlist.exit_code);
        if (batch.exit_code == 0) {
            const Layout batch_layout = layout_of(batch, batch_out);
            CHECK(batch_layout != Layout::other);
            CHECK(batch_layout == layout_of(mdlist, mdlist_out));
            CHECK(batch.err.empty());
            saw_true = saw_true || batch_layout == Layout::per_title;
            saw_false = saw_false || batch_layout == Layout::flat;
        } else {
            // Never a raw CLI11 code (100-114): a usage error with each command's own hint.
            check_usage(batch, batch_hint);
            check_usage(mdlist, mdlist_hint);
            saw_error = true;
        }
        CHECK(!fs::exists(batch_out));
        CHECK(!fs::exists(mdlist_out));
    }
    // The table exercises all three outcomes, so parity is not vacuous.
    CHECK(saw_true);
    CHECK(saw_false);
    CHECK(saw_error);
}
