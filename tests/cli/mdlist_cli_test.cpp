// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/mdlist_cli_test.cpp: the whole `mdlist` command in-process (list plan T10), through
// run_mdlist_command with a filled RawMdlistArgs (no CLI11: registration is T11's), a hermetic
// fixture tree in a fresh temporary directory, and std::ostringstream sinks.
//
// Covers AC9-AC14, the list-level exit 3 (with and without the network remedy), the AC24 mirror,
// AC30 (cli half), AC40-AC43, AC46 (the real-run variant, list plan Spec issue 4), a failed
// stdout, color, and the help footer's width.
//
// The list documents are the generated L1-L6 of the clean tree (mdx_fixture.hpp) plus a few
// written here by hand (a name with ESC, no name, the clean-twin pair, the title cap). In fixture
// mode a missing document is a 404 (fact-check V19).
//
// Catch2 test names must not start with '-'. Every name starts with "mdlist" so
// `pixi run tsan -R 'cli\.mdlist'` selects them all.

#include "app/cmd_mdlist.hpp"
#include "app/command.hpp"
#include "app/exit_help.hpp"
#include "app/io.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/mangadex_api.hpp"
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
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
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
using md::test::Tty;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view unknown_id = mdx::unknown_manga_id;
constexpr std::string_view twin_id = mdx::twin_manga_id;

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view clean_dir = "Clean Title";
constexpr std::string_view twin_dir = "Clean Title [77777777]";

constexpr std::string_view hint = "Run 'mangadex-dl mdlist --help' for more information.\n";
constexpr std::string_view remedy =
    "; check your network connection or proxy (https_proxy) and try again";

/// A fixture root that never exists: a case whose phase 1 were missed would fail to build its
/// backend (exit 1) instead of exiting 2.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";

/// L<n> of the fixture trees, canonical.
[[nodiscard]] std::string list(std::uint64_t n) { return mdx::list_id(n); }

/// The website URL of L<n>: the `list.url` of the document.
[[nodiscard]] std::string web_url(std::uint64_t n) {
    return std::format("https://mangadex.org/list/{}", list(n));
}

/// The exit-4 line of the unknown title, without the row prefix.
[[nodiscard]] std::string title_not_found(std::string_view id) {
    return std::format("title {} not found on MangaDex (HTTP 404); check the UUID or URL", id);
}

// ---- running the command ---------------------------------------------------------------------

/// What the command sees besides its RawMdlistArgs.
struct Context {
    app::GlobalOptions globals{};
    Tty tty{};
    app::EnvSnapshot env{};
};

/// run_mdlist_command over two string streams.
[[nodiscard]] Captured run_command(const md::RawMdlistArgs& args, const Context& context = {}) {
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out,
               .err = err,
               .stdin_tty = context.tty.stdin_tty,
               .stdout_tty = context.tty.stdout_tty,
               .stderr_tty = context.tty.stderr_tty,
               .env = context.env};
    Captured result;
    result.exit_code = app::run_mdlist_command(args, context.globals, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-mdlist");
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

    /// The list document served for `id` (api/list/<id>.json).
    [[nodiscard]] fs::path list_file(std::string_view id) const {
        return root_ / "api" / "list" / std::format("{}.json", id);
    }

    /// Serves one more list document, in the live shape (mdx::list_json).
    void add_list(const mdx::FixtureList& extra) const {
        write_file(list_file(extra.id), mdx::list_json(extra).dump());
    }

    /// The command's arguments: LIST as typed, DIR, the tree's api base, everything else at its
    /// default.
    [[nodiscard]] md::RawMdlistArgs args(std::string_view list_text, const fs::path& dir) const {
        md::RawMdlistArgs raw{.list = std::string{list_text}};
        raw.defaults.dir = dir.string();
        raw.defaults.api_base = api_base();
        return raw;
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

// ---- success (AC9) -----------------------------------------------------------------------------

TEST_CASE("mdlist: a public list exits 0 with exactly download's paths and an empty stderr",
          "[cli][mdlist]") {
    const Tree tree{mdx::clean_fixture()};
    const std::string typed = std::format("https://mangadex.org/list/{}/clean-list", list(1));

    const fs::path out = tree.path("out");
    const Captured r = run_command(tree.args(typed, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(fs::is_regular_file(out / clean_vol1));
    CHECK(fs::is_regular_file(out / clean_vol2));

    SECTION("--json: batch's document plus `list`; `source` is LIST as typed") {
        const fs::path json_out = tree.path("out-json");
        const Captured json = run_command(tree.args(typed, json_out), {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err.empty());
        CHECK(top_level_keys(json.out) == std::vector<std::string>{"format_version", "dry_run",
                                                                   "source", "list", "titles",
                                                                   "summary"});
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("source") == typed);
        CHECK(doc.at("list") == Json{{"id", list(1)}, {"name", "Clean List"}, {"url", web_url(1)}});
        CHECK(statuses(doc) == std::vector<std::string>{"ok"});
        CHECK(doc.at("titles").at(0).at("line") == 1);
        CHECK(doc.at("titles").at(0).at("manga_id") == clean_id);
        CHECK(doc.at("titles").at(0).at("dir") == json_out.string());
        CHECK(fs::is_regular_file(json_out / clean_vol2));
    }
}

// ---- usage errors (AC10) -----------------------------------------------------------------------

TEST_CASE("mdlist: usage errors exit 2 with the mdlist hint, before any backend or directory",
          "[cli][mdlist][usage]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const std::string not_a_list =
        "LIST must be a MangaDex list UUID or a https://mangadex.org/list/<uuid> URL; for a CSV "
        "file of titles use 'mangadex-dl batch <file>'";

    struct Case {
        std::string_view name{};
        std::function<void(md::RawMdlistArgs&)> edit{};
        std::string message{};
    };
    const std::vector<Case> cases{
        {.name = "LIST empty",
         .edit = [](md::RawMdlistArgs& a) { a.list.clear(); },
         .message = "LIST must not be empty"},
        {.name = "LIST a title URL",
         .edit =
             [](md::RawMdlistArgs& a) {
                 a.list = std::format("https://mangadex.org/title/{}", clean_id);
             },
         .message = "LIST is a MangaDex title, not a list; use 'mangadex-dl download <title>' "
                    "for one title"},
        {.name = "LIST -",
         .edit = [](md::RawMdlistArgs& a) { a.list = "-"; },
         .message = not_a_list},
        {.name = "LIST a CSV path",
         .edit = [](md::RawMdlistArgs& a) { a.list = "titles.csv"; },
         .message = not_a_list},
        {.name = "LIST junk",
         .edit = [](md::RawMdlistArgs& a) { a.list = "junk-marker-text"; },
         .message = not_a_list},
        {.name = "DIR empty",
         .edit = [](md::RawMdlistArgs& a) { a.defaults.dir.clear(); },
         .message = "DIR must not be empty"},
        {.name = "DIR a title id",
         .edit = [](md::RawMdlistArgs& a) { a.defaults.dir = std::string{clean_id}; },
         .message = "DIR looks like a MangaDex id; give one list per run"},
        {.name = "DIR a list URL",
         .edit = [](md::RawMdlistArgs& a) { a.defaults.dir = web_url(2); },
         .message = "DIR looks like a MangaDex id; give one list per run"},
        {.name = "--lang bogus",
         .edit = [](md::RawMdlistArgs& a) { a.defaults.lang = "bogus"; },
         .message = "--lang must be a lowercase language code such as en, pt-br or es-la"},
    };

    for (const Case& c : cases) {
        INFO(c.name);
        for (const bool json : {false, true}) {
            INFO((json ? "--json" : "human"));
            for (const std::string& api_base : {tree.api_base(), std::string{missing_api_base}}) {
                INFO("api base: " << api_base);
                md::RawMdlistArgs args = tree.args(list(1), out);
                args.defaults.api_base = api_base;
                c.edit(args);
                const Captured r = run_command(args, {.globals = {.json = json}});
                CHECK(r.exit_code == 2);
                CHECK(r.out.empty());
                CHECK(r.err == std::format("mangadex-dl: error: {}\n{}", c.message, hint));
                CHECK(!r.err.contains("junk-marker-text")); // LIST is never echoed
                CHECK(!fs::exists(out));
            }
        }
    }
}

// ---- list-level failures (AC11, exit 3) --------------------------------------------------------

TEST_CASE("mdlist: an unknown list exits 4 with the list message; no JSON, DIR untouched",
          "[cli][mdlist][list-error]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r =
            run_command(tree.args(list(mdx::unknown_list_n), out), {.globals = {.json = json}});
        CHECK(r.exit_code == 4);
        CHECK(r.out.empty());
        CHECK(r.err ==
              std::format("mangadex-dl: error: list {} not found on MangaDex (HTTP 404): it does "
                          "not exist or is private; check the id, make the list public on "
                          "MangaDex, or use 'mangadex-dl download' if it is a title\n",
                          list(mdx::unknown_list_n)));
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("mdlist: a malformed list exits 3 without the network remedy; no JSON, DIR untouched",
          "[cli][mdlist][list-error]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    // L5: a manga id `../x`; L6: data.id is L1's.
    for (const std::uint64_t n : {std::uint64_t{5}, std::uint64_t{6}}) {
        for (const bool json : {false, true}) {
            INFO("L" << n << (json ? " --json" : ""));
            const Captured r = run_command(tree.args(list(n), out), {.globals = {.json = json}});
            CHECK(r.exit_code == 3);
            CHECK(r.out.empty());
            INFO("stderr: " << r.err);
            CHECK(lines_of(r.err).size() == 1);
            CHECK(r.err.starts_with(std::format(
                "mangadex-dl: error: list {}: malformed MangaDex API response (custom list): ",
                list(n))));
            CHECK(!r.err.contains("check your network"));
            CHECK(!r.err.contains("../x")); // never response bytes
            CHECK(!fs::exists(out));
        }
    }
}

TEST_CASE("mdlist: a list over the title cap exits 3 with the self-contained cap message",
          "[cli][mdlist][list-error]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    mdx::FixtureList big{.id = list(0x30), .name = "Too Big"};
    for (std::size_t i = 0; i <= md::max_list_titles; ++i) {
        big.manga_ids.push_back(std::format("c0000000-0000-4000-8000-{:012x}", i));
    }
    tree.add_list(big);
    const Captured r = run_command(tree.args(big.id, out));
    CHECK(r.exit_code == 3);
    CHECK(r.out.empty());
    CHECK(r.err == std::format("mangadex-dl: error: list {} has more than 10000 titles; refusing "
                               "to truncate it; split it into smaller lists\n",
                               big.id));
    CHECK(!fs::exists(out));
}

TEST_CASE("mdlist: a failed list request exits 3 with the network remedy",
          "[cli][mdlist][list-error]") {
    // Over the transport's body cap: the request fails before any parse (list plan Spec issue 8).
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const std::string id = list(0x31);
    write_file(tree.list_file(id), std::string(md::max_api_json_bytes + 1, ' '));
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = run_command(tree.args(id, out), {.globals = {.json = json}});
        CHECK(r.exit_code == 3);
        CHECK(r.out.empty());
        INFO("stderr: " << r.err);
        CHECK(lines_of(r.err).size() == 1);
        CHECK(r.err.starts_with("mangadex-dl: error: "));
        CHECK(r.err.contains(std::format("/api/list/{}", id)));
        CHECK(r.err.ends_with(std::format("{}\n", remedy)));
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("mdlist: a backend that cannot be built exits 1 before any request or directory",
          "[cli][mdlist][backend]") {
    // Valid arguments, but a fixture root that does not exist: make_backend fails opening it.
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const Dispositions before = current_dispositions();
    md::RawMdlistArgs args = tree.args(list(1), out);
    args.defaults.api_base = std::string{missing_api_base};
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = run_command(args, {.globals = {.json = json}});
        CHECK(r.exit_code == 1);
        CHECK(r.out.empty());
        CHECK(r.err == "mangadex-dl: error: cannot open fixture root "
                       "'/nonexistent-mangadex-dl-cli-fixture': No such file or directory\n");
        CHECK(!r.err.contains("check your network")); // exit 1: no network remedy
        CHECK(!fs::exists(out));
        CHECK(current_dispositions() == before); // the handlers were installed and restored
    }
}

// ---- interrupts (AC12, cli half) ---------------------------------------------------------------

TEST_CASE("mdlist: an interrupt before the call fetches nothing and prints nothing; a second run "
          "works",
          "[cli][mdlist][interrupt]") {
    const InterruptFlagReset reset;
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const Dispositions before = current_dispositions();

    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        md::simulate_interrupt_for_testing(SIGINT);
        const Captured r = run_command(tree.args(list(2), out), {.globals = {.json = json}});
        CHECK(r.exit_code == 1); // main() would re-raise; this is only the fallback
        CHECK(r.out.empty());    // no path, no document
        CHECK(r.err.empty());    // no list line, no closing line
        CHECK(!fs::exists(out));
        CHECK(current_dispositions() == before);
        md::reset_interrupt_for_testing();
    }

    // The handlers (and the one-live-install flag) were released: a second run installs again.
    const Captured second = run_command(tree.args(list(1), out));
    CHECK(second.exit_code == 0);
    CHECK(second.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(second.err.empty());
    CHECK(current_dispositions() == before);
}

// ---- -v, -vv and -q (AC13) ---------------------------------------------------------------------

TEST_CASE("mdlist: -v prints the list line, then 'list:<n>: <uuid>' for each item",
          "[cli][mdlist][verbose]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    md::RawMdlistArgs args = tree.args(list(1), out);
    args.defaults.dry_run = true;

    const Captured r = run_command(args, {.globals = {.verbose = 1}});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    REQUIRE(lines.size() >= 2);
    CHECK(lines[0] == std::format("mangadex-dl: list {} \"Clean List\": 1 title", list(1)));
    CHECK(lines[1] == std::format("mangadex-dl: list:1: {}", clean_id));

    SECTION("-q wins over -v") {
        const Captured quiet = run_command(args, {.globals = {.quiet = true, .verbose = 1}});
        CHECK(quiet.exit_code == 0);
        CHECK(quiet.err.empty());
    }
}

TEST_CASE("mdlist: -v shows the list name terminal-safe, cut and escaped; a nameless list has none",
          "[cli][mdlist][verbose]") {
    const Tree tree{mdx::clean_fixture()};
    const std::string clean{clean_id};
    std::string long_name = "a";
    for (int i = 0; i < 40; ++i) {
        long_name += "\xc3\xa9"; // U+00E9, two bytes: byte 64 is a continuation byte
    }
    const std::string a62(md::max_list_display_name_bytes - 2, 'a');
    const std::string a63(md::max_list_display_name_bytes - 1, 'a');

    struct Case {
        mdx::FixtureList list{};
        std::string expected{};
    };
    const std::vector<Case> cases{
        {.list = {.id = list(0x20), .name = "Esc\x1b[31mRed\x1b[0m", .manga_ids = {clean, clean}},
         .expected = std::format("mangadex-dl: list {} \"Esc?[31mRed?[0m\": 2 titles", list(0x20))},
        {.list = {.id = list(0x21), .manga_ids = {clean}},
         .expected = std::format("mangadex-dl: list {}: 1 title", list(0x21))},
        {.list = {.id = list(0x22), .name = long_name, .manga_ids = {clean}},
         .expected = std::format("mangadex-dl: list {} \"{}...\": 1 title", list(0x22),
                                 long_name.substr(0, 63))},
        // `\` and `"` are escaped inside the quotes (cli-ux L1).
        {.list = {.id = list(0x23), .name = R"(Say "hi" \ bye)", .manga_ids = {clean}},
         .expected =
             std::format(R"(mangadex-dl: list {} "Say \"hi\" \\ bye": 1 title)", list(0x23))},
        // The cut comes first (64 bytes of the name), then the escapes: bytes 63 and 64 are `\"`,
        // both kept and both escaped, so the cut never splits an escape.
        {.list = {.id = list(0x24), .name = a62 + R"(\"tail)", .manga_ids = {clean}},
         .expected = std::format(R"(mangadex-dl: list {} "{}\\\"...": 1 title)", list(0x24), a62)},
        // `"` as byte 64, the last one kept: escaped, then the ellipsis.
        {.list = {.id = list(0x25), .name = a63 + "\"x", .manga_ids = {clean}},
         .expected = std::format(R"(mangadex-dl: list {} "{}\"...": 1 title)", list(0x25), a63)},
        // `"` as byte 65: cut away, nothing to escape.
        {.list = {.id = list(0x26), .name = a63 + "b\"", .manga_ids = {clean}},
         .expected = std::format(R"(mangadex-dl: list {} "{}b...": 1 title)", list(0x26), a63)},
    };
    for (const Case& c : cases) {
        INFO(c.list.id);
        tree.add_list(c.list);
        md::RawMdlistArgs args = tree.args(c.list.id, tree.path("out"));
        args.defaults.dry_run = true;
        const Captured r = run_command(args, {.globals = {.verbose = 1}});
        CHECK(r.exit_code == 0);
        INFO("stderr: " << r.err);
        CHECK(!r.err.contains('\x1b'));
        const std::vector<std::string> lines = lines_of(r.err);
        REQUIRE(!lines.empty());
        CHECK(lines.front() == c.expected);
    }
}

TEST_CASE("mdlist: -vv traces the list request", "[cli][mdlist][verbose]") {
    const Tree tree{mdx::clean_fixture()};
    md::RawMdlistArgs args = tree.args(list(1), tree.path("out"));
    args.defaults.dry_run = true;
    const Captured r = run_command(args, {.globals = {.verbose = 2}});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::string traced =
        std::format("mangadex-dl: GET {}/api/list/{} -> 200 (", tree.api_base(), list(1));
    const std::vector<std::string> lines = lines_of(r.err);
    CHECK(std::ranges::count_if(
              lines, [&traced](const std::string& line) { return line.starts_with(traced); }) == 1);
    // The fetch's trace comes before the list line, which comes before the first item.
    const auto trace_line = std::ranges::find_if(
        lines, [&traced](const std::string& line) { return line.starts_with(traced); });
    const auto list_line = std::ranges::find(
        lines, std::format("mangadex-dl: list {} \"Clean List\": 1 title", list(1)));
    const auto item_line =
        std::ranges::find(lines, std::format("mangadex-dl: list:1: {}", clean_id));
    CHECK(trace_line < list_line);
    CHECK(list_line < item_line);
    CHECK(item_line != lines.end());

    SECTION("-v alone traces nothing") {
        const Captured v = run_command(args, {.globals = {.verbose = 1}});
        CHECK(!v.err.contains(" -> 200 ("));
    }
    SECTION("-q wins over -vv: no trace line, nothing at all on stderr") {
        const Captured quiet = run_command(args, {.globals = {.quiet = true, .verbose = 2}});
        CHECK(quiet.exit_code == 0);
        CHECK(!quiet.err.contains(" -> 200 ("));
        CHECK(quiet.err.empty());
    }
}

TEST_CASE("mdlist: an empty list warns and exits 0; --json prints titles [] and the list object",
          "[cli][mdlist][empty]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const std::string warning =
        std::format("mangadex-dl: warning: list {} has no titles\n", list(3));

    const Captured r = run_command(tree.args(list(3), out));
    CHECK(r.exit_code == 0);
    CHECK(r.out.empty());
    CHECK(r.err == warning);
    CHECK(!fs::exists(out));

    const Captured quiet = run_command(tree.args(list(3), out), {.globals = {.quiet = true}});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.err.empty());

    const Captured json = run_command(tree.args(list(3), out), {.globals = {.json = true}});
    CHECK(json.exit_code == 0);
    CHECK(json.err == warning);
    CHECK(top_level_keys(json.out) == std::vector<std::string>{"format_version", "dry_run",
                                                               "source", "list", "titles",
                                                               "summary"});
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("source") == list(3));
    CHECK(doc.at("list") == Json{{"id", list(3)}, {"name", "Empty"}, {"url", web_url(3)}});
    CHECK(doc.at("titles") == Json::array());
    CHECK(doc.at("dry_run") == false);
    CHECK(!fs::exists(out));
}

TEST_CASE("mdlist: a title listed twice runs once; -q hides the duplicate warning",
          "[cli][mdlist][duplicate]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const Captured r = run_command(tree.args(list(4), out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err ==
          std::format("mangadex-dl: warning: list:2: {}: same as item 1; skipped\n", clean_id));
    CHECK(listing(out) ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});

    const Captured quiet = run_command(tree.args(list(4), out), {.globals = {.quiet = true}});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.out.empty()); // both files are up to date now; -q hides that note too
    CHECK(quiet.err.empty());
}

// ---- item failures (AC14) ----------------------------------------------------------------------

TEST_CASE("mdlist: a missing title fails its item alone (exit 4); -q keeps the errors",
          "[cli][mdlist]") {
    const Tree tree{mdx::clean_fixture()};
    const std::string expected_err =
        std::format("mangadex-dl: error: list:2: {}: {}\n"
                    "mangadex-dl: error: 1 of 2 titles did not complete (item 2); see the errors "
                    "above\n",
                    unknown_id, title_not_found(unknown_id));
    for (const bool quiet : {false, true}) {
        INFO((quiet ? "-q" : "default"));
        const fs::path out = tree.path(quiet ? "out-quiet" : "out");
        const Captured r = run_command(tree.args(list(2), out), {.globals = {.quiet = quiet}});
        CHECK(r.exit_code == 4);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == expected_err);
        CHECK(fs::is_regular_file(out / clean_vol1));
    }

    SECTION("--json: the document has both items") {
        const fs::path out = tree.path("out-json");
        const Captured r = run_command(tree.args(list(2), out), {.globals = {.json = true}});
        CHECK(r.exit_code == 4);
        CHECK(r.err == expected_err);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "not_found"});
        CHECK(doc.at("titles").at(1).at("line") == 2);
        CHECK(doc.at("titles").at(1).at("manga_id") == unknown_id);
        CHECK(doc.at("list").at("name") == "Clean And Unknown");
    }
}

TEST_CASE("mdlist: DIR a regular file stops the run at item 1 (exit 1)", "[cli][mdlist]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path dir = tree.path("a-file");
    write_file(dir, "not a directory\n");
    const std::string stopped = "mangadex-dl: error: stopped at item 1: the error above affects "
                                "every title; 1 title not run";

    const Captured r = run_command(tree.args(list(2), dir));
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].starts_with(std::format("mangadex-dl: error: list:1: {}: ", clean_id)));
    CHECK(lines[1] == stopped);
    CHECK(read_file(dir) == "not a directory\n");

    SECTION("--json: printed for a run that stopped early") {
        const Captured json = run_command(tree.args(list(2), dir), {.globals = {.json = true}});
        CHECK(json.exit_code == 1);
        CHECK(json.err == r.err);
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"failed", "not_run"});
    }
}

TEST_CASE("mdlist: a stdout write error stops the run after the item that hit it",
          "[cli][mdlist][output]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    std::ofstream full{"/dev/full"};
    REQUIRE(full.is_open());
    std::ostringstream err;
    app::Io io{.out = full, .err = err};
    const int rc = app::run_mdlist_command(tree.args(list(2), out), app::GlobalOptions{}, io);
    // Every item that ran succeeded; main()'s finish_stdout turns the write error into exit 1.
    CHECK(rc == 0);
    CHECK(err.str() ==
          "mangadex-dl: error: stopped at item 1: cannot write to stdout; 1 title not run\n");
    CHECK(fs::is_regular_file(out / clean_vol1));
}

// ---- progress and color (AC30, cli half) -------------------------------------------------------

TEST_CASE("mdlist: a TTY stderr gets the '(i/n) ' item counter; --json and -q get none",
          "[cli][mdlist][progress]") {
    const Tree tree{mdx::clean_fixture()};
    const Captured tty =
        run_command(tree.args(list(2), tree.path("out-tty")), {.tty = {.stderr_tty = true}});
    CHECK(tty.exit_code == 4);
    CHECK(tty.err.contains("\r(1/2) [1/2] Vol. 1: "));
    CHECK(tty.err.contains("\r(1/2) [2/2] Vol. 2: "));
    CHECK(!tty.out.contains('\r'));

    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "-q"));
        const Captured r =
            run_command(tree.args(list(2), tree.path(json ? "out-json" : "out-quiet")),
                        {.globals = {.json = json, .quiet = !json}, .tty = {.stderr_tty = true}});
        CHECK(r.exit_code == 4);
        CHECK(!r.err.contains('\r'));
        CHECK(!r.err.contains("(1/2)"));
    }
}

TEST_CASE("mdlist: --color=always colors stderr only; stdout and the document have no ESC",
          "[cli][mdlist][color]") {
    const Tree tree{mdx::clean_fixture()};
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = run_command(tree.args(list(2), tree.path(json ? "out-json" : "out")),
                                       {.globals = {.json = json, .color = app::ColorMode::always},
                                        .tty = {.stdout_tty = true}});
        CHECK(r.exit_code == 4);
        CHECK(!r.out.empty());
        CHECK(!r.out.contains('\x1b'));
        CHECK(r.err.contains("mangadex-dl: \x1b[31merror:\x1b[0m list:2: "));
    }
}

// ---- --dir-per-title (AC40-AC43, AC46) ---------------------------------------------------------

TEST_CASE("mdlist --dir-per-title: files go to DIR/<title>/; a re-run finds them up to date",
          "[cli][mdlist][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    md::RawMdlistArgs args = tree.args(list(1), out);
    args.dir_per_title = true;

    const Captured r = run_command(args);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(fs::is_regular_file(out / clean_dir / clean_vol1));
    CHECK(fs::is_regular_file(out / clean_dir / clean_vol2));
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});

    const Captured again = run_command(args);
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == "mangadex-dl: 2 files are already up to date; pass --overwrite to download "
                       "them again\n");

    SECTION("--json: titles[0].dir is DIR/<title>; report paths are under it") {
        md::RawMdlistArgs json_args = args;
        const fs::path json_out = tree.path("out-json");
        json_args.defaults.dir = json_out.string();
        const Captured json = run_command(json_args, {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err.empty());
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("titles").at(0).at("dir") == (json_out / clean_dir).string());
        CHECK(doc.at("titles").at(0).at("report").at("epubs").at(0).at("path") ==
              (json_out / clean_dir / clean_vol1).string());
        CHECK(doc.at("titles").at(0).at("report").at("epubs").at(1).at("path") ==
              (json_out / clean_dir / clean_vol2).string());
    }
}

TEST_CASE("mdlist --dir-per-title: a second title with the same name is moved aside",
          "[cli][mdlist][dir-per-title]") {
    const Tree tree{CleanTwin{}};
    const std::string id = list(0x40);
    tree.add_list(mdx::FixtureList{
        .id = id, .name = "Twins", .manga_ids = {std::string{clean_id}, std::string{twin_id}}});
    const fs::path out = tree.path("out");
    md::RawMdlistArgs args = tree.args(id, out);
    args.dir_per_title = true;
    const std::string warning = std::format(
        "mangadex-dl: warning: list:2: {}: directory 'Clean Title' already holds item 1 (a title "
        "with the same name); using 'Clean Title [77777777]'\n",
        twin_id);

    SECTION("both titles are downloaded, each into its own directory") {
        const Captured r = run_command(args);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out / clean_dir, {clean_vol1, clean_vol2}) +
                           path_lines(out / twin_dir, {clean_vol1, clean_vol2}));
        CHECK(r.err == warning);
        CHECK(fs::is_regular_file(out / clean_dir / clean_vol2));
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol2));
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_dir}, std::string{twin_dir}});
    }
    SECTION("-q hides the warning") {
        const Captured r = run_command(args, {.globals = {.quiet = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        CHECK(fs::is_regular_file(out / twin_dir / clean_vol1));
    }
    SECTION("--json: titles[1].dir is the moved-aside directory") {
        const Captured r = run_command(args, {.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == warning);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
        CHECK(doc.at("titles").at(1).at("dir") == (out / twin_dir).string());
    }
}

TEST_CASE("mdlist: flat, a second title with the same name gets one line per file",
          "[cli][mdlist][collision]") {
    const Tree tree{CleanTwin{}};
    const std::string id = list(0x41);
    tree.add_list(mdx::FixtureList{
        .id = id, .name = "Twins", .manga_ids = {std::string{clean_id}, std::string{twin_id}}});
    const fs::path out = tree.path("out");
    std::string collisions{};
    for (const std::string_view file : {clean_vol1, clean_vol2}) {
        collisions += std::format(
            "mangadex-dl: warning: list:2: {}: {} was already written by item 1 (another title "
            "with the same name); pass --dir-per-title to give each title its own directory, or "
            "download one of them on its own with 'mangadex-dl download <title> --title ...'\n",
            twin_id, (out / file).string());
    }

    const Captured r = run_command(tree.args(id, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err == collisions); // no "left alone: holds a different book; ... --title" line
    CHECK(listing(out) ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});

    SECTION("--json: unchanged, the twin's files are left alone as foreign") {
        const Captured json = run_command(tree.args(id, out), {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err == collisions + "mangadex-dl: 2 files are already up to date; pass "
                                       "--overwrite to download them again\n");
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        for (const Json& epub : doc.at("titles").at(1).at("report").at("epubs")) {
            CHECK(epub.at("existing") == "foreign");
        }
    }
}

TEST_CASE("mdlist: flat --dry-run, a second title with the same name: what the real run prints",
          "[cli][mdlist][collision][dry-run]") {
    const Tree tree{CleanTwin{}};
    const std::string id = list(0x45);
    tree.add_list(mdx::FixtureList{
        .id = id, .name = "Twins", .manga_ids = {std::string{clean_id}, std::string{twin_id}}});
    const fs::path out = tree.path("out");
    const md::RawMdlistArgs real = tree.args(id, out);
    md::RawMdlistArgs dry = real;
    dry.defaults.dry_run = true;
    // One line per file of item 2 (the observer drops the engine's "holds a different book").
    const auto collisions = [&out](std::string_view written) {
        std::string lines{};
        for (const std::string_view file : {clean_vol1, clean_vol2}) {
            lines += std::format(
                "mangadex-dl: warning: list:2: {}: {} {} by item 1 (another title with the same "
                "name); pass --dir-per-title to give each title its own directory, or download "
                "one of them on its own with 'mangadex-dl download <title> --title ...'\n",
                twin_id, (out / file).string(), written);
        }
        return lines;
    };

    SECTION("item 2's files would be left alone: what the real run then prints") {
        const Captured r = run_command(dry);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == collisions("would already be written"));
        CHECK(!r.err.contains("left alone"));
        CHECK(!fs::exists(out));

        // The real run on the same, still fresh, DIR.
        const Captured written = run_command(real);
        CHECK(written.exit_code == r.exit_code);
        CHECK(written.out == r.out);
        CHECK(written.err == collisions("was already written"));
    }
    SECTION("--json: item 2's files exist as foreign, as in the real run") {
        const Captured r = run_command(dry, {.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == collisions("would already be written"));
        CHECK(!fs::exists(out));
        const Captured written = run_command(real, {.globals = {.json = true}});
        CHECK(written.exit_code == 0);
        CHECK(written.err == collisions("was already written"));

        const Json doc = parse_document(r.out);
        const Json real_doc = parse_document(written.out);
        REQUIRE(!doc.is_discarded());
        REQUIRE(!real_doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        const Json& dry_epubs = doc.at("titles").at(1).at("report").at("epubs");
        const Json& real_epubs = real_doc.at("titles").at(1).at("report").at("epubs");
        REQUIRE(dry_epubs.size() == 2);
        REQUIRE(real_epubs.size() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            CHECK(dry_epubs.at(i).at("status") == "exists");
            CHECK(dry_epubs.at(i).at("existing") == "foreign");
            CHECK(dry_epubs.at(i).at("replaced") == false);
            CHECK(dry_epubs.at(i) == real_epubs.at(i));
        }
        const Json& files = doc.at("summary").at("files");
        CHECK(files.at("planned") == 2);
        CHECK(files.at("exists") == 2);
    }
    SECTION("-v: no 'would write' for item 2's files, no 'left alone'; -q: nothing") {
        const Captured r = run_command(dry, {.globals = {.verbose = 1}});
        CHECK(r.exit_code == 0);
        INFO("stderr: " << r.err);
        const std::vector<std::string> lines = lines_of(r.err);
        const auto item2 =
            std::ranges::find(lines, std::format("mangadex-dl: list:2: {}", twin_id));
        REQUIRE(item2 != lines.end());
        const auto would_write = [](const std::string& line) {
            return line.starts_with("mangadex-dl: would write ");
        };
        CHECK(std::ranges::none_of(item2, lines.end(), would_write));
        CHECK(std::ranges::count_if(lines, would_write) == 2); // item 1's two files
        CHECK(!r.err.contains("left alone"));

        const Captured quiet = run_command(dry, {.globals = {.quiet = true}});
        CHECK(quiet.exit_code == 0);
        CHECK(quiet.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(quiet.err.empty());
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("mdlist: flat --overwrite, a second title with the same name never replaces the "
          "first one's files",
          "[cli][mdlist][collision][overwrite]") {
    const Tree tree{CleanTwin{}};
    const std::string id = list(0x42);
    tree.add_list(mdx::FixtureList{
        .id = id, .name = "Twins", .manga_ids = {std::string{clean_id}, std::string{twin_id}}});
    const fs::path out = tree.path("out");
    std::string collisions{};
    std::string dry_collisions{};
    for (const std::string_view file : {clean_vol1, clean_vol2}) {
        for (const auto& [lines, written] :
             {std::pair{&collisions, "was already written"},
              std::pair{&dry_collisions, "would already be written"}}) {
            *lines += std::format(
                "mangadex-dl: warning: list:2: {}: {} {} by item 1 (another title with the same "
                "name); pass --dir-per-title to give each title its own directory, or download "
                "one of them on its own with 'mangadex-dl download <title> --title ...'\n",
                twin_id, (out / file).string(), written);
        }
    }
    // Item 1's own bytes: the clean title alone (L1), in another directory.
    const fs::path ref = tree.path("ref");
    REQUIRE(run_command(tree.args(list(1), ref)).exit_code == 0);
    const std::string vol1 = read_file(ref / clean_vol1);
    const std::string vol2 = read_file(ref / clean_vol2);
    md::RawMdlistArgs overwrite = tree.args(id, out);
    overwrite.defaults.overwrite = true;

    SECTION("a real run: item 2 leaves item 1's files alone; each path is listed once") {
        const Captured r = run_command(overwrite);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == collisions); // nothing says a file was replaced: none was
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});

        // A run without --overwrite: item 1 finds its own files, up to date.
        const Captured again = run_command(tree.args(id, out));
        CHECK(again.exit_code == 0);
        CHECK(again.out.empty());
        CHECK(again.err == collisions + "mangadex-dl: 2 files are already up to date; pass "
                                        "--overwrite to download them again\n");
    }
    SECTION("--json: item 2's files are left alone as foreign, item 1's written") {
        const Captured r = run_command(overwrite, {.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == collisions);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        for (const Json& epub : doc.at("titles").at(0).at("report").at("epubs")) {
            CHECK(epub.at("status") == "written");
            CHECK(epub.at("replaced") == false);
        }
        for (const Json& epub : doc.at("titles").at(1).at("report").at("epubs")) {
            CHECK(epub.at("status") == "exists");
            CHECK(epub.at("existing") == "foreign");
            CHECK(epub.at("replaced") == false);
        }
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
    }
    SECTION("a later --overwrite run still replaces item 1's files from an earlier run") {
        REQUIRE(run_command(overwrite).exit_code == 0);
        // Stale files from that earlier run: --overwrite replaces them without looking.
        write_file(out / clean_vol1, "stale");
        write_file(out / clean_vol2, "stale");
        const Captured r = run_command(overwrite, {.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == collisions);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        for (const Json& epub : doc.at("titles").at(0).at("report").at("epubs")) {
            CHECK(epub.at("status") == "written");
            CHECK(epub.at("replaced") == true);
            CHECK(epub.at("existing") == "unchecked");
        }
        for (const Json& epub : doc.at("titles").at(1).at("report").at("epubs")) {
            CHECK(epub.at("status") == "exists");
            CHECK(epub.at("existing") == "foreign");
        }
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));

        const Captured human = run_command(overwrite);
        CHECK(human.exit_code == 0);
        CHECK(human.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(human.err == collisions);
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
    }
    SECTION("--dry-run --overwrite: the same answer as the real run, and nothing is created") {
        md::RawMdlistArgs dry = overwrite;
        dry.defaults.dry_run = true;
        const Captured r = run_command(dry);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == dry_collisions);
        CHECK(!fs::exists(out));

        const Captured json = run_command(dry, {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err == dry_collisions);
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        for (const Json& epub : doc.at("titles").at(0).at("report").at("epubs")) {
            CHECK(epub.at("status") == "planned");
            CHECK(epub.at("replaced") == false);
        }
        for (const Json& epub : doc.at("titles").at(1).at("report").at("epubs")) {
            CHECK(epub.at("status") == "exists");
            CHECK(epub.at("existing") == "foreign");
            CHECK(epub.at("replaced") == false);
        }
        CHECK(!fs::exists(out));
    }
    SECTION("--dry-run --overwrite over an earlier run's files: item 1 would replace its own") {
        REQUIRE(run_command(overwrite).exit_code == 0);
        md::RawMdlistArgs dry = overwrite;
        dry.defaults.dry_run = true;
        const Captured r = run_command(dry, {.globals = {.verbose = 1}});
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        INFO("stderr: " << r.err);
        const std::vector<std::string> lines = lines_of(r.err);
        CHECK(std::ranges::count_if(lines, [](const std::string& line) {
                  return line.starts_with("mangadex-dl: would replace ");
              }) == 2);
        CHECK(std::ranges::count_if(lines, [](const std::string& line) {
                  return line.contains("would already be written by item 1");
              }) == 2);
        CHECK(!r.err.contains("left alone"));
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
    }
    SECTION("-q: nothing on stderr, and item 1's files are still kept") {
        const Captured r = run_command(overwrite, {.globals = {.quiet = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err.empty());
        CHECK(holds(out / clean_vol1, vol1));
        CHECK(holds(out / clean_vol2, vol2));
    }
}

TEST_CASE("mdlist --dir-per-title --overwrite: a second title with the same name gets its own "
          "directory, and files from an earlier run are replaced",
          "[cli][mdlist][dir-per-title][collision][overwrite]") {
    const Tree tree{CleanTwin{}};
    const std::string id = list(0x43);
    tree.add_list(mdx::FixtureList{
        .id = id, .name = "Twins", .manga_ids = {std::string{clean_id}, std::string{twin_id}}});
    const std::string twin_list = list(0x44);
    tree.add_list(mdx::FixtureList{
        .id = twin_list, .name = "Twin alone", .manga_ids = {std::string{twin_id}}});
    const fs::path out = tree.path("out");
    const std::string warning = std::format(
        "mangadex-dl: warning: list:2: {}: directory 'Clean Title' already holds item 1 (a title "
        "with the same name); using 'Clean Title [77777777]'\n",
        twin_id);
    // Each title's own bytes: the clean title (L1) and the twin alone, in other directories.
    const fs::path clean_ref = tree.path("ref-clean");
    const fs::path twin_ref = tree.path("ref-twin");
    REQUIRE(run_command(tree.args(list(1), clean_ref)).exit_code == 0);
    REQUIRE(run_command(tree.args(twin_list, twin_ref)).exit_code == 0);
    const std::string clean1 = read_file(clean_ref / clean_vol1);
    const std::string clean2 = read_file(clean_ref / clean_vol2);
    const std::string twin1 = read_file(twin_ref / clean_vol1);
    const std::string twin2 = read_file(twin_ref / clean_vol2);
    md::RawMdlistArgs overwrite = tree.args(id, out);
    overwrite.dir_per_title = true;
    overwrite.defaults.overwrite = true;
    const std::string expected_out = path_lines(out / clean_dir, {clean_vol1, clean_vol2}) +
                                     path_lines(out / twin_dir, {clean_vol1, clean_vol2});

    SECTION("a real run into an empty DIR: each title in its own directory, nothing foreign") {
        const Captured r = run_command(overwrite);
        CHECK(r.exit_code == 0);
        CHECK(r.out == expected_out);
        CHECK(r.err == warning);
        CHECK(listing(out) ==
              std::vector<std::string>{std::string{clean_dir}, std::string{twin_dir}});
        CHECK(holds(out / clean_dir / clean_vol1, clean1));
        CHECK(holds(out / clean_dir / clean_vol2, clean2));
        CHECK(holds(out / twin_dir / clean_vol1, twin1));
        CHECK(holds(out / twin_dir / clean_vol2, twin2));
    }
    SECTION("--json, a real run: every file is written, none is foreign or replaced") {
        const Captured r = run_command(overwrite, {.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == warning);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        CHECK(doc.at("titles").at(0).at("dir") == (out / clean_dir).string());
        CHECK(doc.at("titles").at(1).at("dir") == (out / twin_dir).string());
        for (const Json& title : doc.at("titles")) {
            REQUIRE(title.at("report").at("epubs").size() == 2);
            for (const Json& epub : title.at("report").at("epubs")) {
                CHECK(epub.at("status") == "written");
                CHECK(epub.at("existing") != "foreign");
                CHECK(epub.at("replaced") == false);
            }
        }
    }
    SECTION("an --overwrite re-run replaces both titles' files from the earlier run") {
        REQUIRE(run_command(overwrite).exit_code == 0);
        // Stale files from that earlier run: --overwrite replaces them without looking.
        for (const fs::path& dir : {out / clean_dir, out / twin_dir}) {
            write_file(dir / clean_vol1, "stale");
            write_file(dir / clean_vol2, "stale");
        }
        const Captured r = run_command(overwrite, {.globals = {.json = true}});
        CHECK(r.exit_code == 0);
        CHECK(r.err == warning);
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"ok", "ok"});
        for (const Json& title : doc.at("titles")) {
            REQUIRE(title.at("report").at("epubs").size() == 2);
            for (const Json& epub : title.at("report").at("epubs")) {
                CHECK(epub.at("status") == "written");
                CHECK(epub.at("replaced") == true);
                CHECK(epub.at("existing") == "unchecked");
            }
        }
        CHECK(holds(out / clean_dir / clean_vol1, clean1));
        CHECK(holds(out / clean_dir / clean_vol2, clean2));
        CHECK(holds(out / twin_dir / clean_vol1, twin1));
        CHECK(holds(out / twin_dir / clean_vol2, twin2));
    }
    SECTION("--dry-run --overwrite: the same stdout and stderr as the real run, nothing created") {
        md::RawMdlistArgs dry = overwrite;
        dry.defaults.dry_run = true;
        const Captured planned = run_command(dry);
        CHECK(planned.exit_code == 0);
        CHECK(!fs::exists(out));

        const Captured real = run_command(overwrite);
        CHECK(real.exit_code == 0);
        CHECK(planned.out == expected_out);
        CHECK(planned.err == warning);
        CHECK(planned.out == real.out);
        CHECK(planned.err == real.err);
    }
}

TEST_CASE("mdlist --dir-per-title -v: 'output directory:' follows the item line",
          "[cli][mdlist][dir-per-title][verbose]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    md::RawMdlistArgs args = tree.args(list(1), out);
    args.dir_per_title = true;
    args.defaults.dry_run = true;
    const Captured r = run_command(args, {.globals = {.verbose = 1}});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    const auto item = std::ranges::find(lines, std::format("mangadex-dl: list:1: {}", clean_id));
    REQUIRE(item != lines.end());
    CHECK(std::ranges::find(item, lines.end(),
                            std::format("mangadex-dl: output directory: {}",
                                        (out / clean_dir).string())) != lines.end());
    CHECK(!fs::exists(out));
}

TEST_CASE("mdlist --dir-per-title after a flat run leaves the flat files alone",
          "[cli][mdlist][dir-per-title]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const Captured flat = run_command(tree.args(list(1), out));
    REQUIRE(flat.exit_code == 0);
    const std::string vol1 = read_file(out / clean_vol1);
    const std::string vol2 = read_file(out / clean_vol2);
    const fs::file_time_type vol1_time = fs::last_write_time(out / clean_vol1);
    const fs::file_time_type vol2_time = fs::last_write_time(out / clean_vol2);

    md::RawMdlistArgs args = tree.args(list(1), out);
    args.dir_per_title = true;
    const Captured r = run_command(args);
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

TEST_CASE("mdlist --dir-per-title: an item whose directory is a file fails alone (exit 7)",
          "[cli][mdlist][dir-per-title]") {
    // AC46's real-run variant (list plan Spec issue 4): DIR holds a regular file `Clean Title`.
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    fs::create_directories(out);
    write_file(out / clean_dir, "a regular file\n");
    md::RawMdlistArgs args = tree.args(list(2), out);
    args.dir_per_title = true;

    const Captured r = run_command(args);
    CHECK(r.exit_code == 7); // failed (1) + not_found (4)
    CHECK(r.out.empty());
    CHECK(r.err ==
          std::format(
              "mangadex-dl: error: list:1: {}: cannot open directory '{}': Not a directory; "
              "rename or remove it, or run without --dir-per-title\n"
              "mangadex-dl: error: list:2: {}: {}\n"
              "mangadex-dl: error: 2 of 2 titles did not complete (items 1, 2); see the errors "
              "above\n",
              clean_id, (out / clean_dir).string(), unknown_id, title_not_found(unknown_id)));
    CHECK(!r.err.contains("stopped at"));
    CHECK(listing(out) == std::vector<std::string>{std::string{clean_dir}});
    CHECK(read_file(out / clean_dir) == "a regular file\n");
}

// ---- help ------------------------------------------------------------------------------------

TEST_CASE("mdlist: the help footer explains LIST, fits 80 columns and ends with the exit table",
          "[cli][mdlist][help]") {
    const std::string footer = app::mdlist_help_footer();
    CHECK(footer.starts_with("LIST:\n  A public MangaDex custom list (MDList): its UUID, or its "));
    CHECK(footer.contains("    mangadex-dl mdlist <uuid> ~/Manga --dir-per-title\n"));
    CHECK(footer.contains("  To download the titles you follow, add them to a public MDList.\n"));
    CHECK(footer.ends_with(app::exit_status_footer()));
    CHECK(!footer.ends_with('\n'));
    for (const std::string& line : lines_of(footer)) {
        INFO(line);
        CHECK(line.size() <= 80);
        CHECK(!line.ends_with(' '));
    }
}
