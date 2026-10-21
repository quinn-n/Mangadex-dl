// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_cli_test.cpp: the `batch` command in-process through run_batch_command (batch
// plan T5). `batch` is not registered yet (T6 does that), so every test fills a RawBatchArgs and a
// GlobalOptions by hand, exactly as CLI11 would bind them. Each test writes a hermetic fixture
// tree (tests/support/mdx_fixture.hpp) and a CSV list into a fresh temporary directory and points
// `api_base = file://<tree>` at it, so the whole chain runs: validation -> bounded read -> parse ->
// plan -> run_batch over one shared fixture Backend -> run_title per row -> aggregation, closing
// line and --json.
//
// Covers AC9-AC13 at the cli tier; stdin via an fd, a closed stdin and the terminal hint; the JSON
// document for exits 0, 1 and 7 and for an empty list; dry runs; row directories; duplicates; a
// stdout write error; the progress counter; color; rev1 (fact-check): row-scoped vs global
// directory failures, nothing left behind for exit-4/5 rows, group names never reaching a URL, and
// terminal_safe agreeing with is_unsafe_code_point.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec). Every name
// starts with "batch" so `pixi run tsan -R 'cli\.batch'` selects them all.

#include "app/cmd_batch.hpp"
#include "app/cmd_download_output.hpp"
#include "app/command.hpp"
#include "app/io.hpp"
#include "core/fs/output_dir_testing.hpp" // core-private fault seam (atomics; restored by RAII)
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/text_safety.hpp"
#include "support/fs_test_util.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction, SIGINT and SIGTERM are POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
namespace fs_util = mangadex_dl::test::fs_util;
using md::test::Captured;
using md::test::Tty;
using Json = nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view nochapters_id = "55555555-5555-4555-8555-555555555555";
constexpr std::string_view unknown_id = "66666666-6666-4666-8666-666666666666";

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";

constexpr std::string_view hint = "Run 'mangadex-dl batch --help' for more information.\n";

/// A fixture root that never exists: a case whose phase 1 were missed would fail to build its
/// backend (exit 1) instead of exiting 2.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";

// ---- running the command ---------------------------------------------------------------------

/// What the command sees besides its RawBatchArgs.
struct Context {
    app::GlobalOptions globals{};
    Tty tty{};
    app::EnvSnapshot env{};
    int stdin_fd = -1; ///< borrowed; -1 = a closed stdin
};

/// run_batch_command over two string streams.
[[nodiscard]] Captured run_command(const md::RawBatchArgs& args, const Context& context = {}) {
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out,
               .err = err,
               .stdin_tty = context.tty.stdin_tty,
               .stdout_tty = context.tty.stdout_tty,
               .stderr_tty = context.tty.stderr_tty,
               .env = context.env,
               .stdin_fd = context.stdin_fd};
    Captured result;
    result.exit_code = app::run_batch_command(args, context.globals, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] md::test::TempDir make_temp_dir() {
    auto tmp = md::test::TempDir::create("mangadex-dl-batch");
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

[[nodiscard]] bool fd_open(int fd) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    return ::fcntl(fd, F_GETFD) != -1;
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

    /// `batch --api-base <tree> FILE DIR` with every other option at its default.
    [[nodiscard]] md::RawBatchArgs args(const fs::path& file, const fs::path& dir) const {
        md::RawBatchArgs args{.file = file.string()};
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

// ---- text ------------------------------------------------------------------------------------

/// The UTF-8 encoding of `cp` (a scalar value: not a surrogate, at most U+10FFFF).
[[nodiscard]] std::string utf8(std::uint32_t cp) {
    std::string out;
    const auto byte = [&out](std::uint32_t value) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    };
    if (cp < 0x80U) {
        byte(cp);
    } else if (cp < 0x800U) {
        byte(0xC0U | (cp >> 6U));
        byte(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
        byte(0xE0U | (cp >> 12U));
        byte(0x80U | ((cp >> 6U) & 0x3FU));
        byte(0x80U | (cp & 0x3FU));
    } else {
        byte(0xF0U | (cp >> 18U));
        byte(0x80U | ((cp >> 12U) & 0x3FU));
        byte(0x80U | ((cp >> 6U) & 0x3FU));
        byte(0x80U | (cp & 0x3FU));
    }
    return out;
}

} // namespace

// ---- success (AC9) -----------------------------------------------------------------------------

TEST_CASE("batch: a clean title exits 0 with exactly download's paths and an empty stderr",
          "[cli][batch]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("titles.csv", std::format("{}\n", clean_id));
    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(fs::is_regular_file(out / clean_vol1));
    CHECK(fs::is_regular_file(out / clean_vol2));
}

TEST_CASE("batch: -v prints '<file>:<line>: <uuid>' as each row starts", "[cli][batch]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path list =
        tree.csv("titles.csv", std::format("manga,volumes\n{0},1\n{0},2\n", clean_id));
    const Captured r =
        run_command(tree.args(list, tree.path("out")), Context{.globals = {.verbose = 1}});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    CHECK(std::ranges::count(lines,
                             std::format("mangadex-dl: {}:2: {}", list.string(), clean_id)) == 1);
    CHECK(std::ranges::count(lines,
                             std::format("mangadex-dl: {}:3: {}", list.string(), clean_id)) == 1);
}

// ---- interrupts (AC10) -------------------------------------------------------------------------

TEST_CASE("batch: an interrupt before the call runs no row, prints nothing; a second run works",
          "[cli][batch][interrupt]") {
    const InterruptFlagReset reset;
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list =
        tree.csv("titles.csv", std::format("manga,volumes\n{0},1\n{0},2\n", clean_id));
    const Dispositions before = current_dispositions();

    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        md::simulate_interrupt_for_testing(SIGINT);
        const Captured r = run_command(tree.args(list, out), Context{.globals = {.json = json}});
        CHECK(r.exit_code == 1); // main() would re-raise; this is only the fallback
        CHECK(r.out.empty());    // no path, no document
        CHECK(r.err.empty());    // no closing line, no notice
        CHECK(!fs::exists(out));
        CHECK(current_dispositions() == before);
        md::reset_interrupt_for_testing();
    }

    // The handlers (and the one-live-install flag) were released: a second run installs again.
    const Captured second = run_command(tree.args(list, out));
    CHECK(second.exit_code == 0);
    CHECK(second.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(second.err.empty());
    CHECK(current_dispositions() == before);
}

// ---- phase 1 (AC11) ----------------------------------------------------------------------------

TEST_CASE("batch: phase-1 usage errors exit 2 before any backend, request or directory",
          "[cli][batch][usage]") {
    const md::test::TempDir tmp = make_temp_dir();
    const fs::path out = tmp.path() / "out";
    const fs::path good = tmp.path() / "good.csv";
    write_file(good, std::format("{}\n", clean_id));
    const fs::path bad = tmp.path() / "bad.csv";
    write_file(bad, std::format("manga,title\n{},\"unterminated\n", clean_id));

    struct Case {
        std::string_view name{};
        md::RawBatchArgs args{};
        std::string message{}; ///< the first stderr line, after "mangadex-dl: error: "
    };
    const auto make = [&](const fs::path& file) {
        md::RawBatchArgs args{.file = file.string()};
        args.defaults.dir = out.string();
        args.defaults.api_base = std::string{missing_api_base}; // the backend would fail: exit 1
        return args;
    };
    std::vector<Case> cases{};
    cases.push_back(
        {.name = "csv error", .args = make(bad), .message = std::format("{}:2: ", bad.string())});
    {
        md::RawBatchArgs args = make(good);
        args.defaults.lang = "bogus";
        cases.push_back({.name = "--lang bogus", .args = args, .message = "--lang"});
    }
    {
        md::RawBatchArgs args = make(good);
        args.file = std::string{clean_id};
        cases.push_back({.name = "FILE looks like an id",
                         .args = args,
                         .message = "FILE looks like a MangaDex title; use 'mangadex-dl download "
                                    "<title>' for one title"});
    }
    {
        md::RawBatchArgs args = make(good);
        args.defaults.dir = std::string{clean_id};
        cases.push_back(
            {.name = "DIR looks like an id", .args = args, .message = "DIR looks like a MangaDex"});
    }
    {
        md::RawBatchArgs args = make(good);
        args.file.clear();
        cases.push_back({.name = "FILE empty", .args = args, .message = "FILE must not be empty"});
    }
    for (const Case& c : cases) {
        for (const bool json : {false, true}) {
            INFO(c.name << (json ? " (--json)" : ""));
            const Captured r = run_command(c.args, Context{.globals = {.json = json}});
            INFO("stderr: " << r.err);
            CHECK(r.exit_code == 2);
            CHECK(r.out.empty());
            CHECK(r.err.starts_with("mangadex-dl: error: " + c.message));
            CHECK(r.err.ends_with(hint));
            CHECK(!r.err.contains('\x1b'));
            CHECK(!fs::exists(out));
            CHECK(!fs::exists(clean_id)); // a DIR that looks like an id was not created either
        }
    }
}

TEST_CASE("batch: more than 20 list errors print 20, a count of the rest and one hint",
          "[cli][batch][usage]") {
    const md::test::TempDir tmp = make_temp_dir();
    std::string text = "manga\n";
    for (int i = 0; i < 25; ++i) {
        text += "not-a-title\n";
    }
    const fs::path list = tmp.path() / "many.csv";
    write_file(list, text);
    md::RawBatchArgs args{.file = list.string()};
    args.defaults.dir = (tmp.path() / "out").string();
    args.defaults.api_base = std::string{missing_api_base};
    const Captured r = run_command(args);
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    const std::vector<std::string> lines = lines_of(r.err);
    REQUIRE(lines.size() == 22);
    CHECK(
        lines.front().starts_with(std::format("mangadex-dl: error: {}:2: manga: ", list.string())));
    CHECK(!lines.front().contains("not-a-title")); // the manga cell is never echoed
    CHECK(lines[20] == "mangadex-dl: error: ... and 5 more errors");
    CHECK(lines[21] + '\n' == hint);
}

TEST_CASE("batch: a list over 1 MiB is a usage error", "[cli][batch][usage]") {
    const md::test::TempDir tmp = make_temp_dir();
    const fs::path list = tmp.path() / "huge.csv";
    write_file(list, std::string(md::max_list_bytes + 1, '#'));
    md::RawBatchArgs args{.file = list.string()};
    args.defaults.dir = (tmp.path() / "out").string();
    args.defaults.api_base = std::string{missing_api_base};
    const Captured r = run_command(args);
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == std::format("mangadex-dl: error: {}: is larger than {} bytes\n{}", list.string(),
                               md::max_list_bytes, hint));
}

TEST_CASE("batch: an unreadable FILE is exit 1 with one line and no hint", "[cli][batch]") {
    const md::test::TempDir tmp = make_temp_dir();
    md::RawBatchArgs args{};
    args.defaults.dir = (tmp.path() / "out").string();
    args.defaults.api_base = std::string{missing_api_base};

    SECTION("missing") {
        args.file = (tmp.path() / "nope.csv").string();
        const Captured r = run_command(args);
        CHECK(r.exit_code == 1);
        CHECK(r.out.empty());
        CHECK(r.err ==
              std::format("mangadex-dl: error: cannot read {}: No such file or directory\n",
                          args.file));
    }
    SECTION("a directory") {
        args.file = tmp.path().string();
        const Captured r = run_command(args);
        CHECK(r.exit_code == 1);
        CHECK(r.out.empty());
        CHECK(r.err ==
              std::format("mangadex-dl: error: cannot read {}: is a directory\n", args.file));
    }
    CHECK(!fs::exists(tmp.path() / "out"));
}

// ---- stdin -------------------------------------------------------------------------------------

TEST_CASE("batch: FILE '-' reads io.stdin_fd and prints what the FILE form prints",
          "[cli][batch][stdin]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("titles.csv", std::format("{0}\n{0}\n", clean_id));

    md::RawBatchArgs args = tree.args(list, out);
    args.file = "-";
    const md::UniqueFd fd = open_read(list);
    const Captured piped = run_command(args, Context{.stdin_fd = fd.get()});
    CHECK(piped.exit_code == 0);
    CHECK(piped.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(piped.err == "mangadex-dl: warning: <stdin>:2: same as line 1; skipped\n");
    CHECK(fd_open(fd.get())); // borrowed: never closed

    fs::remove_all(out);
    const Captured named = run_command(tree.args(list, out));
    CHECK(named.exit_code == 0);
    CHECK(named.out == piped.out);
    CHECK(named.err ==
          std::format("mangadex-dl: warning: {}:2: same as line 1; skipped\n", list.string()));
}

TEST_CASE("batch: FILE '-' with a closed stdin is exit 1 'cannot read <stdin>'",
          "[cli][batch][stdin]") {
    const md::test::TempDir tmp = make_temp_dir();
    md::RawBatchArgs args{.file = "-"};
    args.defaults.dir = (tmp.path() / "out").string();
    args.defaults.api_base = std::string{missing_api_base};
    const Captured r = run_command(args); // stdin_fd -1
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: cannot read <stdin>: Bad file descriptor\n");
}

TEST_CASE("batch: the terminal hint needs a TTY stdin and stderr, and is quiet under -q",
          "[cli][batch][stdin]") {
    const md::test::TempDir tmp = make_temp_dir();
    const fs::path list = tmp.path() / "comments.csv";
    write_file(list, "# nothing yet\n");
    md::RawBatchArgs args{.file = "-"};
    args.defaults.dir = (tmp.path() / "out").string();
    args.defaults.api_base = std::string{missing_api_base};
    constexpr std::string_view terminal_hint =
        "mangadex-dl: reading titles from the terminal; end with Ctrl-D\n";
    constexpr std::string_view empty_warning = "mangadex-dl: warning: <stdin>: lists no titles\n";

    struct Case {
        std::string_view name{};
        Tty tty{};
        bool quiet = false;
        bool hinted = false;
    };
    const std::vector<Case> cases{
        {.name = "both TTYs", .tty = {.stdin_tty = true, .stderr_tty = true}, .hinted = true},
        {.name = "-q", .tty = {.stdin_tty = true, .stderr_tty = true}, .quiet = true},
        {.name = "stdin TTY only", .tty = {.stdin_tty = true}},
        {.name = "stderr TTY only", .tty = {.stderr_tty = true}},
        {.name = "no TTY"},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const md::UniqueFd fd = open_read(list);
        // NO_COLOR: a TTY stderr would otherwise color `warning:`.
        const Captured r = run_command(args, Context{.globals = {.quiet = c.quiet},
                                                     .tty = c.tty,
                                                     .env = {.no_color = true},
                                                     .stdin_fd = fd.get()});
        CHECK(r.exit_code == 0);
        CHECK(r.out.empty());
        std::string expected = c.hinted ? std::string{terminal_hint} : std::string{};
        if (!c.quiet) {
            expected += empty_warning;
        }
        CHECK(r.err == expected);
    }
}

// ---- empty list --------------------------------------------------------------------------------

TEST_CASE("batch: an empty list exits 0 with a warning and builds no backend",
          "[cli][batch][json]") {
    const md::test::TempDir tmp = make_temp_dir();
    const fs::path list = tmp.path() / "empty.csv";
    write_file(list, "# a comment\n\n   \nmanga,title\n");
    md::RawBatchArgs args{.file = list.string()};
    args.defaults.dir = (tmp.path() / "out").string();
    args.defaults.api_base = std::string{missing_api_base}; // never opened

    const Captured human = run_command(args);
    CHECK(human.exit_code == 0);
    CHECK(human.out.empty());
    CHECK(human.err == std::format("mangadex-dl: warning: {}: lists no titles\n", list.string()));

    const Captured quiet = run_command(args, Context{.globals = {.quiet = true}});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.err.empty());

    const Captured json = run_command(args, Context{.globals = {.json = true}});
    CHECK(json.exit_code == 0);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("source") == list.string());
    CHECK(doc.at("titles") == Json::array());
    CHECK(doc.at("summary").at("titles").at("total") == 0);
    CHECK(!fs::exists(tmp.path() / "out"));
}

// ---- JSON and aggregation ----------------------------------------------------------------------

TEST_CASE("batch: different failures exit 7; --json lists each row and the summary",
          "[cli][batch][json]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("mixed.csv", std::format("{}\n{}\n", nochapters_id, unknown_id));

    const Captured human = run_command(tree.args(list, out));
    CHECK(human.exit_code == 7);
    CHECK(human.out.empty());
    INFO("stderr: " << human.err);
    const std::vector<std::string> lines = lines_of(human.err);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with(
        std::format("mangadex-dl: error: {}:1: no downloadable chapters matched", list.string())));
    CHECK(lines[1].starts_with(std::format("mangadex-dl: error: {}:2: ", list.string())));
    // Neither exit 5 nor exit 4 goes away on a retry: no "run the same command again".
    CHECK(lines[2] == "mangadex-dl: error: 2 of 2 titles did not complete (lines 1, 2); see the "
                      "errors above");

    const Captured json = run_command(tree.args(list, out), Context{.globals = {.json = true}});
    CHECK(json.exit_code == 7);
    CHECK(json.err == human.err); // the same diagnostics; the document goes to stdout
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("dry_run") == false);
    CHECK(statuses(doc) == std::vector<std::string>{"no_chapters", "not_found"});
    CHECK(doc.at("titles").at(0).at("exit_code") == 5);
    CHECK(doc.at("titles").at(0).at("report").is_object()); // exit 5 has a report
    CHECK(doc.at("titles").at(1).at("exit_code") == 4);
    CHECK(doc.at("titles").at(1).at("report").is_null());
    CHECK(doc.at("summary").at("titles").at("total") == 2);
    CHECK(!fs::exists(out));
}

TEST_CASE("batch: --fail-fast stops at the first title that does not succeed", "[cli][batch]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path list = tree.csv("mixed.csv", std::format("{}\n{}\n", nochapters_id, unknown_id));
    md::RawBatchArgs args = tree.args(list, tree.path("out"));
    args.fail_fast = true;
    const Captured r = run_command(args, Context{.globals = {.json = true}});
    CHECK(r.exit_code == 5);
    CHECK(r.err.ends_with("mangadex-dl: error: stopped at line 1: --fail-fast; 1 title not run\n"));
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"no_chapters", "not_run"});
}

// ---- row directories (rev1: row-scoped vs global) ----------------------------------------------

TEST_CASE("batch: a row whose dir runs into a file fails alone; the batch continues",
          "[cli][batch][row-dir]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    fs::create_directories(out);
    write_file(out / "blocked", "a regular file\n");
    const fs::path list =
        tree.csv("rows.csv", std::format("manga,dir\n{0},blocked/x\n{0},ok\n", clean_id));
    const std::string row_error =
        std::format("mangadex-dl: error: {}:2: cannot open directory '{}': Not a directory\n",
                    list.string(), (out / "blocked").string());
    // A row-scoped directory failure fails the same way on a retry: no "run again" advice.
    const std::string closing =
        "mangadex-dl: error: 1 of 2 titles did not complete (line 2); see the errors above\n";

    SECTION("download") {
        const Captured r = run_command(tree.args(list, out));
        CHECK(r.exit_code == 1);
        CHECK(r.out == path_lines(out / "ok", {clean_vol1, clean_vol2}));
        CHECK(r.err == row_error + closing);
        CHECK(fs::is_regular_file(out / "ok" / clean_vol1));
    }
    SECTION("dry run") {
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.dry_run = true;
        const Captured r = run_command(args);
        CHECK(r.exit_code == 1);
        CHECK(r.out == path_lines(out / "ok", {clean_vol1, clean_vol2}));
        CHECK(r.err == row_error + "mangadex-dl: error: 1 of 2 titles would fail (line 2)\n");
        CHECK(!fs::exists(out / "ok"));
    }
    SECTION("--json") {
        const Captured r = run_command(tree.args(list, out), Context{.globals = {.json = true}});
        CHECK(r.exit_code == 1);
        CHECK(!r.err.contains("stopped at line"));
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) == std::vector<std::string>{"failed", "ok"});
        CHECK(doc.at("titles").at(0).at("exit_code") == 1);
        CHECK(
            doc.at("titles").at(0).at("error") ==
            std::format("cannot open directory '{}': Not a directory", (out / "blocked").string()));
        CHECK(doc.at("titles").at(0).at("dir") == (out / "blocked/x").string());
    }
    SECTION("with a not-found row after it: exit 7") {
        const fs::path mixed = tree.csv(
            "mixed.csv", std::format("manga,dir\n{},blocked/x\n{},ok\n", clean_id, unknown_id));
        const Captured r = run_command(tree.args(mixed, out));
        CHECK(r.exit_code == 7);
        CHECK(!r.err.contains("stopped at line"));
    }
}

TEST_CASE("batch: an unusable DIR stops the batch at the first row; the rest is not run",
          "[cli][batch][row-dir]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path not_a_dir = tree.path("not-a-dir");
    write_file(not_a_dir, "a regular file\n");
    const fs::path list =
        tree.csv("rows.csv", std::format("manga,volumes\n{0},1\n{0},2\n", clean_id));

    const Captured human = run_command(tree.args(list, not_a_dir));
    CHECK(human.exit_code == 1);
    CHECK(human.out.empty());
    CHECK(human.err ==
          std::format("mangadex-dl: error: {}:2: cannot open directory '{}': Not a directory\n"
                      "mangadex-dl: error: stopped at line 2: the error above affects every "
                      "title; 1 title not run\n",
                      list.string(), not_a_dir.string()));

    // --json still prints the document on this exit 1.
    const Captured json =
        run_command(tree.args(list, not_a_dir), Context{.globals = {.json = true}});
    CHECK(json.exit_code == 1);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"failed", "not_run"});
    CHECK(doc.at("titles").at(1).at("exit_code").is_null());
    CHECK(doc.at("summary").at("titles").at("not_run") == 1);
}

TEST_CASE("batch: exit-4 and exit-5 rows with a dir leave no directory behind",
          "[cli][batch][row-dir]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list =
        tree.csv("rows.csv", std::format("manga,dir\n{},sub/deeper\n{},sub/deeper\n", unknown_id,
                                         nochapters_id));
    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 7);
    CHECK(r.out.empty());
    CHECK(!fs::exists(out));
}

TEST_CASE("batch: a row dir lands under DIR/<dir>; a dry run creates neither", "[cli][batch]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("manga,dir\n{},series/x\n", clean_id));

    md::RawBatchArgs dry = tree.args(list, out);
    dry.defaults.dry_run = true;
    const Captured planned = run_command(dry);
    CHECK(planned.exit_code == 0);
    CHECK(planned.out == path_lines(out / "series/x", {clean_vol1, clean_vol2}));
    CHECK(planned.err.empty());
    CHECK(!fs::exists(out));

    const Captured written = run_command(tree.args(list, out));
    CHECK(written.exit_code == 0);
    CHECK(written.out == planned.out);
    CHECK(fs::is_regular_file(out / "series" / "x" / clean_vol1));
    CHECK(fs::is_regular_file(out / "series" / "x" / clean_vol2));
}

// ---- duplicates, collisions, notices (AC12, AC13) ----------------------------------------------

TEST_CASE("batch: a duplicate row warns and does not run", "[cli][batch]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv(
        "dups.csv", std::format("{}\nhttps://mangadex.org/title/{}\n", clean_id, clean_id));
    const Captured r = run_command(tree.args(list, out), Context{.globals = {.json = true}});
    CHECK(r.exit_code == 0);
    CHECK(r.err ==
          std::format("mangadex-dl: warning: {}:2: same as line 1; skipped\n", list.string()));
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"ok", "duplicate"});
    CHECK(doc.at("titles").at(1).at("duplicate_of") == 1);
}

TEST_CASE("batch: two rows writing the same file warn once naming both lines, also with "
          "--overwrite",
          "[cli][batch][collision]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list =
        tree.csv("rows.csv", std::format("manga,volumes\n{0},1\n{0},1-2\n", clean_id));
    const std::string collision =
        std::format("mangadex-dl: warning: {}:3: {} was already written by line 2; give one of "
                    "them a 'title' column\n",
                    list.string(), (out / clean_vol1).string());

    SECTION("the second row finds the file the first one wrote") {
        const Captured r = run_command(tree.args(list, out));
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
        CHECK(r.err == collision + "mangadex-dl: 1 file is already up to date; pass --overwrite to "
                                   "download it again\n");
    }
    SECTION("--overwrite: the second row replaces it, and still warns") {
        md::RawBatchArgs args = tree.args(list, out);
        args.defaults.overwrite = true;
        const Captured r = run_command(args);
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {clean_vol1, clean_vol1, clean_vol2}));
        CHECK(r.err == collision);
    }
}

TEST_CASE("batch: a re-run prints one aggregated 'already up to date' note, at the end",
          "[cli][batch]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("manga,dir\n{0},a\n{0},b\n", clean_id));
    const Captured first = run_command(tree.args(list, out));
    REQUIRE(first.exit_code == 0);
    CHECK(first.err.empty());

    const Captured again = run_command(tree.args(list, out));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == "mangadex-dl: 4 files are already up to date; pass --overwrite to download "
                       "them again\n");

    const Captured quiet = run_command(tree.args(list, out), Context{.globals = {.quiet = true}});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.err.empty());
}

TEST_CASE("batch: a re-run rebuilds grown files and counts only the up-to-date ones in the note",
          "[cli][batch][update]") {
    const Tree tree{mdx::clean_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("manga,dir\n{0},a\n{0},b\n", clean_id));
    const Captured first = run_command(tree.args(list, out));
    REQUIRE(first.exit_code == 0);
    const std::string old_vol1 = fs_util::read_file(out / "a" / clean_vol1);

    // MangaDex added ch 4 to Vol. 2 and serves only it: each row rebuilds its Vol. 2 from the
    // held pages plus ch 4 and leaves its Vol. 1 alone, so the note counts 2 files, not 4.
    md::RawBatchArgs again_args = tree.args(list, out);
    again_args.defaults.api_base = reuse_only.api_base();
    const Captured again = run_command(again_args);
    CHECK(again.exit_code == 0);
    CHECK(again.out == path_lines(out / "a", {clean_vol2}) + path_lines(out / "b", {clean_vol2}));
    CHECK(again.err == "mangadex-dl: 2 files are already up to date; pass --overwrite to download "
                       "them again\n");
    CHECK(fs_util::read_file(out / "a" / clean_vol1) == old_vol1);
}

TEST_CASE("batch: files a row left up to date count in the note even if the row then failed",
          "[cli][batch][update]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("manga\n{}\n", clean_id));
    REQUIRE(run_command(tree.args(list, out)).exit_code == 0);
    REQUIRE(fs::remove(out / clean_vol2));

    // Vol. 01 is left alone as up to date; then the fsync of Vol. 02's temporary fails, a local
    // error that ends the row (and the batch) without a report. The note still counts Vol. 01,
    // as `download`'s would.
    const md::testing::ScopedIoError eio{md::testing::IoStage::fsync, EIO};
    const Captured r = run_command(tree.args(list, out));
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    INFO("stderr: " << r.err);
    // The row's error, then the note (before the closing line, as on a clean run).
    CHECK(r.err ==
          std::format("mangadex-dl: error: {}:2: cannot sync '{}': Input/output error\n"
                      "mangadex-dl: 1 file is already up to date; pass --overwrite to download "
                      "it again\n"
                      "mangadex-dl: error: stopped at line 2: the error above affects every "
                      "title\n",
                      list.string(), clean_vol2));
    CHECK(!fs::exists(out / clean_vol2));
}

TEST_CASE("batch --json: a re-run still counts the up-to-date files in the note",
          "[cli][batch][update][json]") {
    const Tree tree{mdx::clean_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("manga,dir\n{0},a\n{0},b\n", clean_id));
    REQUIRE(run_command(tree.args(list, out)).exit_code == 0);

    // With --json each row's report is moved into the document: the note must be counted first.
    md::RawBatchArgs again_args = tree.args(list, out);
    again_args.defaults.api_base = reuse_only.api_base();
    const Captured r = run_command(again_args, Context{.globals = {.json = true}});
    CHECK(r.exit_code == 0);
    CHECK(r.err == "mangadex-dl: 2 files are already up to date; pass --overwrite to download "
                   "them again\n");
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    for (const Json& title : doc.at("titles")) {
        const Json& epubs = title.at("report").at("epubs");
        REQUIRE(epubs.size() == 2);
        CHECK(epubs.at(0).at("status") == "exists");
        CHECK(epubs.at(0).at("existing") == "complete");
        CHECK(epubs.at(1).at("status") == "written");
        CHECK(epubs.at(1).at("existing") == "incomplete");
    }
}

TEST_CASE("batch: row lines carry '<file>:<line>: '; -q keeps errors and the closing line",
          "[cli][batch]") {
    const Tree tree{mdx::nochapters_fixture()};
    const fs::path list = tree.csv(
        "rows.csv", std::format("{0}\n{1}\n{0}\n", nochapters_id, unknown_id)); // 3 = dup of 1
    const Captured loud = run_command(tree.args(list, tree.path("out")));
    CHECK(loud.exit_code == 7);
    CHECK(loud.err.contains(
        std::format("mangadex-dl: warning: {}:3: same as line 1; skipped\n", list.string())));

    const Captured quiet =
        run_command(tree.args(list, tree.path("out")), Context{.globals = {.quiet = true}});
    CHECK(quiet.exit_code == 7);
    INFO("stderr: " << quiet.err);
    CHECK(!quiet.err.contains("warning:"));
    const std::vector<std::string> lines = lines_of(quiet.err);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with(std::format("mangadex-dl: error: {}:1: ", list.string())));
    CHECK(lines[1].starts_with(std::format("mangadex-dl: error: {}:2: ", list.string())));
    CHECK(lines[2] == "mangadex-dl: error: 2 of 2 titles did not complete (lines 1, 2); see the "
                      "errors above");
}

// ---- stdout errors -----------------------------------------------------------------------------

TEST_CASE("batch: a stdout write error stops the batch after the row that hit it",
          "[cli][batch][output]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("manga,dir\n{0},a\n{0},b\n", clean_id));
    std::ofstream full{"/dev/full"};
    REQUIRE(full.is_open());
    std::ostringstream err;
    app::Io io{.out = full, .err = err};
    const int rc = app::run_batch_command(tree.args(list, out), app::GlobalOptions{}, io);
    // Every row that ran succeeded; main()'s finish_stdout turns the write error into exit 1.
    CHECK(rc == 0);
    CHECK(err.str() ==
          "mangadex-dl: error: stopped at line 2: cannot write to stdout; 1 title not run\n");
    CHECK(fs::is_regular_file(out / "a" / clean_vol1));
    CHECK(!fs::exists(out / "b"));
}

// ---- progress and color ------------------------------------------------------------------------

TEST_CASE("batch: a TTY stderr gets the '(i/n) ' row counter; --json and -q get none",
          "[cli][batch][progress]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path list = tree.csv("rows.csv", std::format("manga,dir\n{0},a\n{0},b\n", clean_id));

    const Captured tty =
        run_command(tree.args(list, tree.path("out-tty")), Context{.tty = {.stderr_tty = true}});
    CHECK(tty.exit_code == 0);
    CHECK(tty.err.contains("\r(1/2) [1/2] Vol. 1: "));
    CHECK(tty.err.contains("\r(1/2) [2/2] Vol. 2: "));
    CHECK(tty.err.contains("\r(2/2) [1/2] Vol. 1: "));
    CHECK(!tty.out.contains('\r'));

    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "-q"));
        const Captured r = run_command(
            tree.args(list, tree.path(json ? "out-json" : "out-quiet")),
            Context{.globals = {.json = json, .quiet = !json}, .tty = {.stderr_tty = true}});
        CHECK(r.exit_code == 0);
        CHECK(!r.err.contains('\r'));
        CHECK(!r.err.contains("(1/2)"));
    }
}

TEST_CASE("batch: --color=always colors stderr only; stdout and the document have no ESC",
          "[cli][batch][color]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path out = tree.path("out");
    const fs::path list = tree.csv("rows.csv", std::format("{}\n{}\n", clean_id, unknown_id));
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r =
            run_command(tree.args(list, out),
                        Context{.globals = {.json = json, .color = app::ColorMode::always},
                                .tty = {.stdout_tty = true}});
        CHECK(r.exit_code == 4);
        CHECK(!r.out.empty());
        CHECK(!r.out.contains('\x1b'));
        CHECK(r.err.contains("mangadex-dl: \x1b[31merror:\x1b[0m "));
        fs::remove_all(out);
    }
}

// ---- rev1 (fact-check): group names never reach a URL ------------------------------------------

TEST_CASE("batch: group names from the list never reach a request URL (-vv)",
          "[cli][batch][group]") {
    const Tree tree{mdx::clean_fixture()};
    const fs::path list = tree.csv("rows.csv", std::format("manga,group\n{},A&B|C#D\n", clean_id));
    const Captured r =
        run_command(tree.args(list, tree.path("out")), Context{.globals = {.verbose = 2}});
    CHECK(r.exit_code == 0);
    std::vector<std::string> requests{};
    for (const std::string& line : lines_of(r.err)) {
        if (line.contains(" -> ")) { // "GET <url> -> <status> (<n> ms)"
            requests.push_back(line);
        }
    }
    REQUIRE(!requests.empty());
    for (const std::string& line : requests) {
        INFO(line);
        CHECK(!line.contains("A&B"));
        CHECK(!line.contains("C#D"));
        CHECK(!line.contains("%26"));
        CHECK(!line.contains("%23"));
    }
}

// ---- rev1 (fact-check): one unsafe-code-point predicate ----------------------------------------

TEST_CASE("batch text safety: terminal_safe hides exactly C0, DEL and is_unsafe_code_point",
          "[cli][batch][text-safety]") {
    std::vector<std::uint32_t> code_points{};
    for (std::uint32_t cp = 0; cp <= 0x2FFFU; ++cp) {
        code_points.push_back(cp);
    }
    for (std::uint32_t cp = 0xFE00U; cp <= 0xFFFFU; ++cp) {
        code_points.push_back(cp);
    }
    // A sample of every higher plane: its first, a middle and its last code point.
    for (std::uint32_t plane = 1; plane <= 0x10U; ++plane) {
        const std::uint32_t base = plane << 16U;
        code_points.insert(code_points.end(), {base, base + 0x8000U, base + 0xFFFFU});
    }

    std::size_t mismatches = 0;
    std::string first_mismatch{};
    for (const std::uint32_t cp : code_points) {
        const std::string text = utf8(cp);
        const bool unsafe = cp < 0x20U || cp == 0x7FU || md::is_unsafe_code_point(cp);
        const std::string expected = unsafe ? std::string{"?"} : text;
        if (app::terminal_safe(text) != expected) {
            if (mismatches == 0) {
                first_mismatch = std::format("U+{:04X}", cp);
            }
            ++mismatches;
        }
    }
    INFO("first mismatch: " << first_mismatch);
    CHECK(mismatches == 0);
    CHECK(code_points.size() > 0x3000U);
}

// ---- help --------------------------------------------------------------------------------------

TEST_CASE("batch --help footer: CSV format, examples, the symlink note and the exit table",
          "[cli][batch][help]") {
    const std::string footer = app::batch_help_footer();
    CHECK(footer.starts_with("CSV file:\n"));
    CHECK(footer.contains("    manga            the title, as MANGA in 'download' (required)\n"));
    CHECK(footer.contains("    mangadex-dl batch titles.csv ~/Manga --dry-run\n"));
    CHECK(footer.contains("Symlinks inside DIR are followed: don't point DIR at a directory that "
                          "others\ncan write to. Each title costs several API requests: a long "
                          "list is a long run.\n\nExit status:\n"));
    CHECK(!footer.ends_with('\n'));
    CHECK(!footer.contains('\x1b'));
    CHECK(!footer.contains("--api-base"));
    for (const std::string& line : lines_of(footer)) {
        INFO(line);
        CHECK(line.size() <= 80); // ASCII only: bytes are columns
        CHECK(std::ranges::all_of(line, [](char c) { return c >= 0x20 && c < 0x7F; }));
    }
}
