// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/download_cli_test.cpp: `download` end to end through run(args, Io&) (download plan
// T18a + rev1). Each test writes a hermetic fixture tree (tests/support/mdx_fixture.hpp) into a
// fresh temporary directory and points `--api-base file://<tree>` at it, so the whole chain runs
// in-process: CLI11 parse -> validation -> fixture backend -> run_download -> ConsoleObserver ->
// report / --json -> exit code. Covers: the help surface; every usage error, also with --json; the
// four --group argv shapes and their priority order; success, re-run, --overwrite and --jobs byte
// identity; the --json shapes for success, dry run and exits 4, 5 and 6; exit 5's message; the
// TTY progress line and its clearing before a later error line; a simulated interrupt followed by
// a second run; backend construction failures; fixture mode being fast and libcurl-free. The
// shared helpers (Tree, run_args, snapshot, ...) live in tests/support/download_cli_harness.hpp.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec).

#include "app/io.hpp"
#include "core/fs/output_dir_testing.hpp" // core-private fault seam (atomics; restored by RAII)
#include "mangadex_dl/interrupt.hpp"
#include "support/download_cli_harness.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction, SIGINT and SIGTERM are POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
using md::test::Captured;
using md::test::Tty;
using md::test::download_cli::concat;
using md::test::download_cli::lines_of;
using md::test::download_cli::list_dir;
using md::test::download_cli::make_temp_dir;
using md::test::download_cli::parse_document;
using md::test::download_cli::path_lines;
using md::test::download_cli::run_args;
using md::test::download_cli::snapshot;
using md::test::download_cli::Tree;
using Json = nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view basic_id = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view nochapters_id = "55555555-5555-4555-8555-555555555555";
constexpr std::string_view unknown_id = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view skips_id = "77777777-7777-4777-8777-777777777777";

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";

/// A fixture root that never exists: a usage case whose validation were missed would fail to
/// build its backend (exit 1) instead of reaching the network or writing anything.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";

// ---- output checks ---------------------------------------------------------------------------

[[nodiscard]] bool all_spaces(std::string_view text) noexcept {
    return std::ranges::all_of(text, [](char c) { return c == ' '; });
}

/// True when every "mangadex-dl: " diagnostic in `err` begins a line: at the very start, after a
/// '\n', or right after a progress line was cleared ("\r<spaces>\r"). A progress line that was
/// not cleared would leave "... pages" in front of it.
[[nodiscard]] bool diagnostics_start_clean(std::string_view err) {
    constexpr std::string_view prefix = "mangadex-dl: ";
    for (std::size_t pos = err.find(prefix); pos != std::string_view::npos;
         pos = err.find(prefix, pos + 1)) {
        if (pos == 0 || err[pos - 1] == '\n') {
            continue;
        }
        if (err[pos - 1] != '\r' || pos < 2) {
            return false;
        }
        const std::size_t open = err.rfind('\r', pos - 2);
        if (open == std::string_view::npos) {
            return false;
        }
        const std::string_view cleared = err.substr(open + 1, pos - 1 - (open + 1));
        if (cleared.empty() || !all_spaces(cleared)) {
            return false;
        }
    }
    return true;
}

/// Whether stderr ends with the progress line cleared (or never drew one).
[[nodiscard]] bool progress_cleared_at_end(std::string_view err) {
    if (err.empty() || err.ends_with('\n')) {
        return true;
    }
    if (!err.ends_with('\r') || err.size() < 2) {
        return false;
    }
    const std::size_t open = err.rfind('\r', err.size() - 2);
    if (open == std::string_view::npos) {
        return false;
    }
    const std::string_view cleared = err.substr(open + 1, err.size() - 1 - (open + 1));
    return !cleared.empty() && all_spaces(cleared);
}

/// Whether libcurl is mapped into this process (/proc/self/maps names every loaded object).
[[nodiscard]] bool libcurl_mapped() {
    std::ifstream maps("/proc/self/maps");
    REQUIRE(maps.good());
    const std::string text{std::istreambuf_iterator<char>{maps}, std::istreambuf_iterator<char>{}};
    return text.contains("libcurl");
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

/// The current SIGINT/SIGTERM handlers (Catch2 installs its own; run() must restore them).
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

// ---- fixtures built here ---------------------------------------------------------------------

/// 77777777-...: every en chapter is skipped for a reason other than dedupe (vol 1: ch 1
/// external, ch 2 without pages, ch 3 unavailable), so exit 5 reports "3 chapters skipped".
/// Vol 2 ch 4 is downloadable, so `--volumes 1` keeps exactly the three skips.
[[nodiscard]] mdx::FixtureManga skips_fixture() {
    using mdx::detail::IdKind;
    constexpr char d = '7';
    const std::string group = mdx::detail::fixture_uuid(d, IdKind::group, 1);
    mdx::FixtureManga m{
        .id = std::string{skips_id},
        .title_en = "Skips Title",
        .original_language = "ja",
        .groups = {{.id = group, .name = "Skip Scans"}},
    };
    const mdx::PageImage png{.kind = mdx::ImageKind::png};
    m.chapters = {
        mdx::detail::numbered(d, 1,
                              {.volume = "1",
                               .chapter = "1",
                               .title = "Away",
                               .group_ids = {group},
                               .pages = 0,
                               .external_url = "https://example.invalid/chapter/1"}),
        mdx::detail::numbered(
            d, 2,
            {.volume = "1", .chapter = "2", .title = "Blank", .group_ids = {group}, .pages = 0}),
        mdx::detail::numbered(d, 3,
                              {.volume = "1",
                               .chapter = "3",
                               .title = "Gone",
                               .group_ids = {group},
                               .unavailable = true,
                               .images = {png, png}}),
        mdx::detail::numbered(d, 4,
                              {.volume = "2",
                               .chapter = "4",
                               .title = "Here",
                               .group_ids = {group},
                               .images = {png}}),
    };
    return m;
}

/// 88888888-...: one external loose chapter and a downloadable vol 1, so `--volumes none` leaves
/// exactly one skipped chapter.
[[nodiscard]] mdx::FixtureManga one_skip_fixture() {
    using mdx::detail::IdKind;
    constexpr char d = '8';
    const std::string group = mdx::detail::fixture_uuid(d, IdKind::group, 1);
    mdx::FixtureManga m{
        .id = "88888888-8888-4888-8888-888888888888",
        .title_en = "One Skip Title",
        .original_language = "ko",
        .groups = {{.id = group, .name = "Solo Scans"}},
    };
    m.chapters = {
        mdx::detail::numbered(d, 1,
                              {.volume = "1",
                               .chapter = "1",
                               .title = "Kept",
                               .group_ids = {group},
                               .images = {mdx::PageImage{}}}),
        mdx::detail::numbered(d, 2,
                              {.chapter = "7",
                               .title = "Elsewhere",
                               .group_ids = {group},
                               .pages = 0,
                               .external_url = "https://example.invalid/chapter/7"}),
    };
    return m;
}

/// 99999999-...: `count` chapters of one tiny page each. Every chapter costs one at-home API
/// request, and API requests are spaced 250 ms apart on a real clock.
[[nodiscard]] mdx::FixtureManga many_chapters_fixture(std::size_t count) {
    using mdx::detail::IdKind;
    constexpr char d = '9';
    const std::string group = mdx::detail::fixture_uuid(d, IdKind::group, 1);
    mdx::FixtureManga m{
        .id = "99999999-9999-4999-8999-999999999999",
        .title_en = "Many Chapters Title",
        .original_language = "ko",
        .groups = {{.id = group, .name = "Many Scans"}},
    };
    m.chapters.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
        m.chapters.push_back(mdx::detail::numbered(
            d, i,
            {.chapter = std::to_string(i),
             .group_ids = {group},
             .images = {mdx::PageImage{.kind = mdx::ImageKind::png, .width = 1, .height = 1}}}));
    }
    return m;
}

/// 99999999-...: one volume whose feed says 5001 pages, over the EPUB entry limit, so a dry run
/// reports its plan error as a failed file.
[[nodiscard]] mdx::FixtureManga too_big_fixture() {
    using mdx::detail::IdKind;
    constexpr char d = '9';
    const std::string group = mdx::detail::fixture_uuid(d, IdKind::group, 1);
    mdx::FixtureManga m{
        .id = "99999999-9999-4999-8999-999999999999",
        .title_en = "Too Big Title",
        .original_language = "ko",
        .groups = {{.id = group, .name = "Solo Scans"}},
    };
    m.chapters = {mdx::detail::numbered(d, 1,
                                        {.volume = "1",
                                         .chapter = "1",
                                         .title = "Huge",
                                         .group_ids = {group},
                                         .pages = 5001,
                                         .images = {mdx::PageImage{}}})};
    return m;
}

} // namespace

// ---- help --------------------------------------------------------------------------------------

TEST_CASE("download --help lists every public option, the examples and no hidden seam",
          "[cli][download][help]") {
    for (const char* flag : {"--help", "-h"}) {
        INFO("flag " << flag);
        const Captured r = run_args({"download", flag});
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        for (const std::string_view option :
             {"MANGA", "DIR", "--volumes", "--loose-chapters", "--layout", "--lang", "-l,",
              "--group", "--data-saver", "--direction", "--title", "--overwrite", "--dry-run",
              "-n,", "--jobs", "-j,", "--no-report", "Examples:"}) {
            INFO("option " << option);
            CHECK(r.out.contains(option));
        }
        CHECK(!r.out.contains("--api-base"));
        CHECK(!r.out.contains('\x1b'));
    }
}

// ---- usage errors ------------------------------------------------------------------------------

namespace {

struct UsageCase {
    std::string_view name{};
    std::vector<std::string> args{}; ///< after `download --api-base <missing root>`
    std::string_view message{};      ///< a part of the diagnostic
    bool own_api_base = false;       ///< args carry their own --api-base
};

[[nodiscard]] std::vector<UsageCase> usage_cases() {
    const std::string id{clean_id};
    return {
        {.name = "empty --title",
         .args = {"--title", "", id, "out"},
         .message = "--title must not be empty"},
        {.name = "blank --title",
         .args = {"--title", "  ", id, "out"},
         .message = "--title must not be empty"},
        {.name = "repeated --title",
         .args = {"--title", "A", "--title", "B", id, "out"},
         .message = "--title: At most 1"},
        {.name = "-j 0", .args = {"-j", "0", id}, .message = "--jobs: Value 0 not in range"},
        {.name = "-j 9", .args = {"-j", "9", id}, .message = "--jobs: Value 9 not in range"},
        {.name = "--jobs word", .args = {"--jobs", "four", id}, .message = "--jobs"},
        {.name = "--direction bogus",
         .args = {"--direction", "bogus", id},
         .message = "--direction: bogus not in"},
        {.name = "--layout bogus",
         .args = {"--layout", "bogus", id},
         .message = "--layout: bogus not in"},
        {.name = "bad --volumes SPEC",
         .args = {"--volumes", "x", id},
         .message = "--volumes: 'x' is not a number"},
        {.name = "bad --loose-chapters SPEC",
         .args = {"--loose-chapters", "1,,2", id},
         .message = "--loose-chapters: "},
        {.name = "both selections none",
         .args = {"--volumes", "none", "--loose-chapters", "none", id},
         .message = "both none"},
        {.name = "upper-case --lang", .args = {"--lang", "EN", id}, .message = "--lang must be"},
        {.name = "empty DIR", .args = {id, ""}, .message = "DIR must not be empty"},
        {.name = "DIR is a manga id",
         .args = {id, std::string{basic_id}},
         .message = "only one manga per run"},
        {.name = "bad MANGA", .args = {"not-a-manga"}, .message = "MANGA must be"},
        {.name = "missing MANGA", .args = {}, .message = "MANGA is required"},
        {.name = "extra positional",
         .args = {id, "out", "extra"},
         .message = "not expected: extra"},
        {.name = "unknown option",
         .args = {"--frobnicate", id},
         .message = "not expected: --frobnicate"},
        {.name = "--group without a value",
         .args = {id, "--group"},
         .message = "--group: 1 required"},
        {.name = "--api-base ftp://",
         .args = {"--api-base", "ftp://x", id},
         .message = "--api-base must start with",
         .own_api_base = true},
        {.name = "--api-base file:// relative",
         .args = {"--api-base", "file://relative/dir", id},
         .message = "--api-base",
         .own_api_base = true},
    };
}

} // namespace

TEST_CASE("every download usage error exits 2 with nothing on stdout, with and without --json",
          "[cli][download][usage]") {
    for (const UsageCase& c : usage_cases()) {
        const std::vector<std::string> args =
            c.own_api_base
                ? concat({"download"}, c.args)
                : concat({"download", "--api-base", std::string{missing_api_base}}, c.args);
        for (const bool json : {false, true}) {
            INFO("case: " << c.name << (json ? " (--json)" : ""));
            const Captured r = run_args(json ? concat({"--json"}, args) : args);
            CHECK(r.exit_code == 2);
            CHECK(r.out.empty());
            INFO("stderr: " << r.err);
            CHECK(r.err.contains(c.message));
            CHECK(r.err.contains("--help")); // a hint: CLI11's or download's own
            CHECK(r.err.ends_with('\n'));
            CHECK(!r.err.contains('\x1b'));
        }
    }
}

TEST_CASE("a semantic usage error names the problem and points at download --help",
          "[cli][download][usage]") {
    const Captured r = run_args({"download", "--api-base", std::string{missing_api_base}, "--title",
                                 "", std::string{clean_id}, "out"});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: --title must not be empty\n"
                   "Run 'mangadex-dl download --help' for more information.\n");
}

TEST_CASE("a dry run's exit-6 closing line says 'would fail', not 'run again'",
          "[cli][download][incomplete]") {
    const Tree tree{too_big_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(tree.download(out, {"--dry-run"}));
    CHECK(r.exit_code == 6);
    CHECK(r.out.empty());
    INFO("stderr: " << r.err);
    CHECK(lines_of(r.err).size() == 2);
    CHECK(r.err.ends_with("mangadex-dl: error: 1 of 1 file would fail\n"));
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("a subcommand-level extra argument drops CLI11's own program prefix",
          "[cli][download][usage]") {
    const Captured r = run_args(
        {"download", "--api-base", std::string{missing_api_base}, std::string{clean_id}, "a", "b"});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: The following argument was not expected: b\n"
                   "Run 'mangadex-dl download --help' for more information.\n");
}

TEST_CASE("a CLI11 parse error shares the 'error:' format and points at download --help",
          "[cli][download][usage]") {
    const Captured r = run_args({"download", "--api-base", std::string{missing_api_base}, "-j", "9",
                                 std::string{clean_id}, "out"});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: --jobs: Value 9 not in range [1 - 8]\n"
                   "Run 'mangadex-dl download --help' for more information.\n");
}

TEST_CASE("root usage errors: missing command, unknown command, unknown option, empty word",
          "[cli][usage]") {
    const std::string hint = "Run 'mangadex-dl --help' for more information.\n";
    const std::string missing = "mangadex-dl: error: missing command; try 'mangadex-dl download "
                                "--help'\n";
    struct Case {
        std::vector<std::string> args{};
        std::string expected{};
    };
    const std::string unknown_pre = "mangadex-dl: error: unknown command '";
    const std::string unknown_post = "' (the only command is 'download')\n" + hint;
    const std::string option_pre = "mangadex-dl: error: unknown option '--foo'\n";
    const std::vector<Case> cases{
        {.args = {}, .expected = missing},
        {.args = {""}, .expected = missing},
        {.args = {"bogus"}, .expected = unknown_pre + "bogus" + unknown_post},
        {.args = {"dowload", "x"}, .expected = unknown_pre + "dowload" + unknown_post},
        {.args = {"do\x01x"}, .expected = unknown_pre + "do?x" + unknown_post},
        {.args = {"--foo"}, .expected = option_pre + hint},
        {.args = {"--foo", "bar"}, .expected = option_pre + hint},
    };
    for (const Case& c : cases) {
        INFO("args: " << c.args.size() << " first=" << (c.args.empty() ? "" : c.args.front()));
        const Captured r = run_args(c.args);
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == c.expected);
    }
}

// ---- --group argv shapes -----------------------------------------------------------------------

TEST_CASE("download --group parses in all four argv shapes, keeps the positionals and its order",
          "[cli][download][group]") {
    const mdx::FixtureManga basic = mdx::basic_fixture();
    const Tree tree{basic};
    // Vol. 1 Ch. 1 has an Alpha release (chapters[0]) and a Beta release (chapters[1]). Without
    // --group, Alpha wins on coverage; a matching --group decides it as "preferred group".
    const std::string& alpha_ch1 = basic.chapters.at(0).id;
    const std::string& beta_ch1 = basic.chapters.at(1).id;
    const std::string base = tree.api_base();
    const std::string id{basic_id};
    const std::string dir = tree.path("out").string();

    struct Shape {
        std::string_view name{};
        std::vector<std::string> args{}; ///< after `-v download --api-base <tree> --dry-run`
        std::string kept{};
        std::string_view rule{};
        std::string dir_prefix{}; ///< every stdout path starts with this
    };
    const std::vector<Shape> shapes{
        {.name = "--group X --group Y M",
         .args = {"--group", "Beta Scans", "--group", "Alpha Scans", id},
         .kept = beta_ch1,
         .rule = "preferred group (--group)",
         .dir_prefix = "./"},
        {.name = "--group X M DIR",
         .args = {"--group", "beta scans", id, dir},
         .kept = beta_ch1,
         .rule = "preferred group (--group)",
         .dir_prefix = dir + "/"},
        {.name = "M --group X --group Y",
         .args = {id, "--group", "Alpha Scans", "--group", "Beta Scans"},
         .kept = alpha_ch1,
         .rule = "preferred group (--group)",
         .dir_prefix = "./"},
        {.name = "M DIR --group X",
         .args = {id, dir, "--group", basic.groups.at(1).id}, // by UUID: Beta
         .kept = beta_ch1,
         .rule = "preferred group (--group)",
         .dir_prefix = dir + "/"},
        {.name = "no --group",
         .args = {id, dir},
         .kept = alpha_ch1,
         .rule = "group covering the most chapters",
         .dir_prefix = dir + "/"},
    };
    for (const Shape& shape : shapes) {
        INFO("shape: " << shape.name);
        // A dry run: nothing is written, not even into the working directory (DIR ".").
        const Captured r =
            run_args(concat({"-v", "download", "--api-base", base, "--dry-run"}, shape.args));
        REQUIRE(r.exit_code == 0);
        INFO("stderr: " << r.err);
        CHECK(r.err.contains(std::format("mangadex-dl: Vol. 1 Ch. 1: kept {} ({}); dropped ",
                                         shape.kept, shape.rule)));
        const std::vector<std::string> paths = lines_of(r.out);
        REQUIRE(paths.size() == 5);
        for (const std::string& path : paths) {
            INFO("path: " << path);
            CHECK(path.starts_with(shape.dir_prefix));
        }
    }
    CHECK(!std::filesystem::exists(dir));
}

// ---- success, re-run, --overwrite, --jobs ------------------------------------------------------

TEST_CASE("download writes valid EPUBs and lists exactly their paths on stdout",
          "[cli][download][success]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(tree.download(out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    REQUIRE(list_dir(out) ==
            std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
    for (const std::string_view name : {clean_vol1, clean_vol2}) {
        INFO("file " << name);
        const auto archive = zip::read_archive_file(out / name);
        INFO((archive ? std::string{} : archive.error()));
        REQUIRE(archive.has_value());
        REQUIRE(!archive->entries.empty());
        CHECK(archive->entries.front().name == "mimetype");
        CHECK(archive->entries.front().text() == "application/epub+zip");
        CHECK(archive->find("META-INF/container.xml") != nullptr);
        CHECK(archive->find("OEBPS/content.opf") != nullptr);
    }
}

TEST_CASE("a re-run skips existing files; --overwrite rewrites them byte for byte",
          "[cli][download][success][overwrite]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    REQUIRE(run_args(tree.download(out)).exit_code == 0);
    const auto first = snapshot(out);
    REQUIRE(first.size() == 2);

    const Captured again = run_args(tree.download(out));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == "mangadex-dl: 2 files are already up to date; pass --overwrite to "
                       "download them again\n");
    CHECK(snapshot(out) == first);

    const Captured overwritten = run_args(tree.download(out, {"--overwrite"}));
    CHECK(overwritten.exit_code == 0);
    CHECK(overwritten.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(overwritten.err.empty());
    CHECK(snapshot(out) == first);

    const Captured json = run_args(concat({"--json"}, tree.download(out, {"--overwrite"})));
    CHECK(json.exit_code == 0);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    REQUIRE(doc.at("epubs").size() == 2);
    for (const Json& epub : doc.at("epubs")) {
        CHECK(epub.at("status") == "written");
        CHECK(epub.at("replaced") == true);
    }
    CHECK(snapshot(out) == first);
}

TEST_CASE("download --jobs 1 and --jobs 8 write byte-identical files",
          "[cli][download][success][jobs]") {
    const Tree tree{mdx::basic_fixture()};
    const std::filesystem::path one = tree.path("j1");
    const std::filesystem::path eight = tree.path("j8");
    const Captured r1 = run_args(tree.download(one, {"-j", "1"}));
    const Captured r8 = run_args(tree.download(eight, {"--jobs", "8"}));
    CHECK(r1.exit_code == 0);
    CHECK(r8.exit_code == 0);
    CHECK(lines_of(r1.out).size() == 5);
    CHECK(r1.err == r8.err); // the same warnings (WebP), in the same order
    const auto files1 = snapshot(one);
    REQUIRE(files1.size() == 5);
    CHECK(snapshot(eight) == files1);
}

// ---- --json shapes -----------------------------------------------------------------------------

TEST_CASE("download --json success: one document, every key, no path stream",
          "[cli][download][json][success]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(concat({"--json"}, tree.download(out)));
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("dry_run") == false);
    CHECK(doc.at("layout") == "volumes");
    const Json& manga = doc.at("manga");
    CHECK(manga.at("id") == clean_id);
    CHECK(manga.at("title") == "Clean Title");
    CHECK(manga.at("url") == std::format("https://mangadex.org/title/{}", clean_id));
    CHECK(manga.at("language") == "en");
    CHECK(manga.at("direction") == "rtl"); // auto: a Japanese original reads right to left
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);
    std::size_t index = 0;
    for (const std::string_view name : {clean_vol1, clean_vol2}) {
        const Json& epub = epubs.at(index++);
        INFO("epub " << name);
        CHECK(epub.at("path") == (out / name).string());
        CHECK(epub.at("kind") == "volume");
        CHECK(epub.at("chapter").is_null());
        CHECK(epub.at("status") == "written");
        CHECK(epub.at("replaced") == false);
        CHECK(epub.at("pages").get<std::size_t>() > 0);
        CHECK(epub.at("bytes").get<std::uintmax_t>() == std::filesystem::file_size(out / name));
        CHECK(epub.at("error").is_null());
        CHECK(!epub.at("chapters").empty());
    }
    CHECK(epubs.at(0).at("volume") == "1");
    CHECK(epubs.at(0).at("chapters").size() == 2);
    CHECK(epubs.at(1).at("volume") == "2");
    CHECK(doc.at("skipped_chapters").empty());
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 2}, {"exists", 0}, {"failed", 0}});
}

TEST_CASE("download --json --dry-run plans the files and creates nothing",
          "[cli][download][json]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(concat({"--json"}, tree.download(out, {"--dry-run"})));
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("dry_run") == true);
    REQUIRE(doc.at("epubs").size() == 2);
    for (const Json& epub : doc.at("epubs")) {
        CHECK(epub.at("status") == "planned");
        CHECK(epub.at("bytes") == 0);
    }
    CHECK(doc.at("summary") == Json{{"planned", 2}, {"written", 0}, {"exists", 0}, {"failed", 0}});
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("download --color=always --json keeps ANSI out of stdout (stderr may be colored)",
          "[cli][download][json][color]") {
    const Tree tree{mdx::basic_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r =
        run_args(concat({"--color=always", "--json"}, tree.download(out)), Tty{.stdout_tty = true});
    CHECK(r.exit_code == 0);
    CHECK(!r.out.contains('\x1b'));
    CHECK(!parse_document(r.out).is_discarded());
    CHECK(r.err.contains("\x1b[33mwarning:\x1b[0m")); // the color really was on (WebP warning)
}

TEST_CASE("exit 4: an unknown title prints no document, only an error line",
          "[cli][download][json][not-found]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const std::vector<std::string> args{"download", "--api-base", tree.api_base(),
                                        std::string{unknown_id}, out.string()};
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = run_args(json ? concat({"--json"}, args) : args);
        CHECK(r.exit_code == 4);
        CHECK(r.out.empty());
        CHECK(r.err == std::format("mangadex-dl: error: title {} not found on MangaDex (HTTP 404); "
                                   "check the UUID or URL\n",
                                   unknown_id));
    }
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("exit 6: the good file is written and listed, the failed one is reported",
          "[cli][download][json][incomplete]") {
    const Tree tree{mdx::incomplete_fixture()};
    const std::string vol1 = "Incomplete Title - Vol. 01.epub";
    const std::string vol2 = "Incomplete Title - Vol. 02.epub";

    const std::filesystem::path human_out = tree.path("human");
    const Captured human = run_args(tree.download(human_out));
    CHECK(human.exit_code == 6);
    CHECK(human.out == path_lines(human_out, {vol1}));
    CHECK(human.err.starts_with(
        std::format("mangadex-dl: error: {}: ", (human_out / vol2).string())));
    CHECK(lines_of(human.err).size() == 2);
    CHECK(human.err.ends_with("mangadex-dl: error: 1 of 2 files failed; run the same command "
                              "again to retry them\n"));
    CHECK(list_dir(human_out) == std::vector<std::string>{vol1}); // no temporaries left

    const std::filesystem::path json_out = tree.path("json");
    const Captured json = run_args(concat({"--json"}, tree.download(json_out)));
    CHECK(json.exit_code == 6);
    CHECK(
        json.err.starts_with(std::format("mangadex-dl: error: {}: ", (json_out / vol2).string())));
    CHECK(json.err.ends_with("mangadex-dl: error: 1 of 2 files failed; run the same command "
                             "again to retry them\n"));
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);
    CHECK(epubs.at(0).at("status") == "written");
    CHECK(epubs.at(0).at("error").is_null());
    CHECK(epubs.at(1).at("status") == "failed");
    CHECK(epubs.at(1).at("path") == (json_out / vol2).string());
    CHECK(epubs.at(1).at("bytes") == 0);
    CHECK(epubs.at(1).at("error").is_string());
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 1}, {"exists", 0}, {"failed", 1}});
    CHECK(list_dir(json_out) == std::vector<std::string>{vol1});
}

TEST_CASE("exit 6 closing line says '1 of 1 file failed' in the singular",
          "[cli][download][incomplete]") {
    const Tree tree{mdx::incomplete_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(tree.download(out, {"--layout", "single"}));
    CHECK(r.exit_code == 6);
    CHECK(r.out.empty());
    CHECK(r.err.ends_with("mangadex-dl: error: 1 of 1 file failed; run the same command again to "
                          "retry them\n"));
    CHECK(lines_of(r.err).size() == 2);
}

TEST_CASE("verbose dry-run with --overwrite says 'would replace' only for files that already exist",
          "[cli][download][overwrite][verbose]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");

    const Captured fresh = run_args(concat({"-v"}, tree.download(out, {"--dry-run"})));
    CHECK(fresh.exit_code == 0);
    CHECK(fresh.err.contains(
        std::format("mangadex-dl: would write {} (5 pages)\n", (out / clean_vol1).string())));
    CHECK(!fresh.err.contains("would replace"));

    REQUIRE(run_args(tree.download(out)).exit_code == 0);
    const auto before = snapshot(out);
    const Captured again = run_args(concat({"-v"}, tree.download(out, {"-n", "--overwrite"})));
    CHECK(again.exit_code == 0);
    CHECK(again.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(again.err.contains(
        std::format("mangadex-dl: would replace {} (5 pages)\n", (out / clean_vol1).string())));
    CHECK(again.err.contains(
        std::format("mangadex-dl: would replace {} (2 pages)\n", (out / clean_vol2).string())));
    CHECK(again.err.contains("mangadex-dl: planned 2 files; 0 chapters skipped\n"));
    CHECK(snapshot(out) == before); // a dry run touches nothing
}

// ---- exit 5 ------------------------------------------------------------------------------------

TEST_CASE("exit 5 without skipped chapters: the message echoes language and selection",
          "[cli][download][json][no-chapters]") {
    const Tree tree{mdx::nochapters_fixture()}; // only `de` chapters
    const std::filesystem::path out = tree.path("out");
    const std::string message =
        "mangadex-dl: error: no downloadable chapters matched --lang en with --volumes all "
        "--loose-chapters all; try another --lang or a wider --volumes/--loose-chapters\n";

    const Captured human = run_args(tree.download(out));
    CHECK(human.exit_code == 5);
    CHECK(human.out.empty());
    CHECK(human.err == message);

    const Captured json = run_args(concat({"--json"}, tree.download(out)));
    CHECK(json.exit_code == 5);
    CHECK(json.err == message);
    CHECK(json.out == std::format(R"({{
  "format_version": 1,
  "dry_run": false,
  "layout": "volumes",
  "manga": {{
    "id": "{0}",
    "title": "No Chapters Title",
    "url": "https://mangadex.org/title/{0}",
    "language": "en",
    "direction": "ltr"
  }},
  "epubs": [],
  "skipped_chapters": [],
  "summary": {{
    "planned": 0,
    "written": 0,
    "exists": 0,
    "failed": 0
  }}
}}
)",
                                  nochapters_id));
    CHECK(!std::filesystem::exists(out)); // DIR is left alone
}

TEST_CASE("exit 5 counts external, empty and unavailable chapters in its message",
          "[cli][download][json][no-chapters]") {
    const mdx::FixtureManga manga = skips_fixture();
    const Tree tree{manga};
    const std::filesystem::path out = tree.path("out");
    const auto args = tree.download(out, {"--volumes", "1", "--loose-chapters", "none"});
    const std::string expected_err =
        "mangadex-dl: warning: skipped Vol. 1 Ch. 1: hosted outside MangaDex\n"
        "mangadex-dl: warning: skipped Vol. 1 Ch. 2: it has no pages\n"
        "mangadex-dl: warning: skipped Vol. 1 Ch. 3: unavailable on MangaDex\n"
        "mangadex-dl: error: no downloadable chapters matched --lang en with --volumes 1 "
        "--loose-chapters none (3 chapters skipped: external, no pages or unavailable); "
        "try another --lang or a wider --volumes/--loose-chapters\n";

    const Captured human = run_args(args);
    CHECK(human.exit_code == 5);
    CHECK(human.out.empty());
    CHECK(human.err == expected_err);

    const Captured quiet = run_args(concat({"-q"}, args)); // the error line survives -q
    CHECK(quiet.exit_code == 5);
    CHECK(quiet.out.empty());
    CHECK(quiet.err == lines_of(expected_err).back() + "\n");

    const Captured json = run_args(concat({"--json"}, args));
    CHECK(json.exit_code == 5);
    CHECK(json.err == expected_err);
    CHECK(json.out == std::format(R"({{
  "format_version": 1,
  "dry_run": false,
  "layout": "volumes",
  "manga": {{
    "id": "{0}",
    "title": "Skips Title",
    "url": "https://mangadex.org/title/{0}",
    "language": "en",
    "direction": "rtl"
  }},
  "epubs": [],
  "skipped_chapters": [
    {{
      "id": "{1}",
      "volume": "1",
      "chapter": "1",
      "reason": "external",
      "kept": null
    }},
    {{
      "id": "{2}",
      "volume": "1",
      "chapter": "2",
      "reason": "no_pages",
      "kept": null
    }},
    {{
      "id": "{3}",
      "volume": "1",
      "chapter": "3",
      "reason": "unavailable",
      "kept": null
    }}
  ],
  "summary": {{
    "planned": 0,
    "written": 0,
    "exists": 0,
    "failed": 0
  }}
}}
)",
                                  skips_id, manga.chapters.at(0).id, manga.chapters.at(1).id,
                                  manga.chapters.at(2).id));
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("exit 5 with exactly one skipped chapter says '1 chapter skipped'",
          "[cli][download][no-chapters]") {
    const Tree tree{one_skip_fixture()};
    const std::filesystem::path out = tree.path("out");
    const auto args = tree.download(out, {"--volumes", "none"});
    const std::string error_line =
        "mangadex-dl: error: no downloadable chapters matched --lang en with --volumes none "
        "--loose-chapters all (1 chapter skipped: external, no pages or unavailable); "
        "try another --lang or a wider --volumes/--loose-chapters\n";
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = run_args(json ? concat({"--json"}, args) : args);
        CHECK(r.exit_code == 5);
        CHECK(r.err ==
              "mangadex-dl: warning: skipped Ch. 7: hosted outside MangaDex\n" + error_line);
        if (json) {
            const Json doc = parse_document(r.out);
            REQUIRE(!doc.is_discarded());
            CHECK(doc.at("epubs").empty());
            REQUIRE(doc.at("skipped_chapters").size() == 1);
            CHECK(doc.at("skipped_chapters").at(0).at("reason") == "external");
        } else {
            CHECK(r.out.empty());
        }
    }
}

// ---- progress ----------------------------------------------------------------------------------

TEST_CASE("a TTY stderr gets a progress line that is cleared before the run ends",
          "[cli][download][progress]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(tree.download(out), Tty{.stderr_tty = true});
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2})); // progress never reaches stdout
    CHECK(r.err.contains("\r[1/2] Vol. 1: "));
    CHECK(r.err.contains("\r[2/2] Vol. 2: "));
    CHECK(r.err.contains(" pages"));
    CHECK(!r.err.contains('\n')); // the clean title prints no diagnostics
    CHECK(progress_cleared_at_end(r.err));
}

TEST_CASE("no progress line with --json, -q or TERM=dumb, even on a TTY",
          "[cli][download][progress]") {
    const Tree tree{mdx::clean_fixture()};
    struct Variant {
        std::string_view name{};
        std::vector<std::string> globals{};
        md::app::EnvSnapshot env{};
    };
    const std::vector<Variant> variants{
        {.name = "--json", .globals = {"--json"}},
        {.name = "-q", .globals = {"-q"}},
        {.name = "TERM=dumb", .globals = {}, .env = {.term_dumb = true}},
    };
    for (const Variant& v : variants) {
        INFO("variant " << v.name);
        const std::filesystem::path out =
            tree.path(std::format("out-{}", v.globals.size()) + (v.env.term_dumb ? "-dumb" : ""));
        std::vector<std::string> args = v.globals;
        const std::vector<std::string> download = tree.download(out);
        args.insert(args.end(), download.begin(), download.end());
        const Captured r = run_args(args, Tty{.stderr_tty = true}, v.env);
        CHECK(r.exit_code == 0);
        CHECK(!r.err.contains('\r'));
        CHECK(!r.err.contains("[1/"));
    }
}

TEST_CASE("an error after a drawn progress line is printed on a cleared line",
          "[cli][download][progress]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    // fsync of the first temporary fails: a local failure that aborts the run after Vol. 01's
    // progress line was drawn; the error line comes from the command after observer.finish().
    const md::testing::ScopedIoError eio{md::testing::IoStage::fsync, EIO};
    const Captured r = run_args(tree.download(out), Tty{.stderr_tty = true});
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    INFO("stderr: " << r.err);
    CHECK(r.err.contains("\r[1/2] Vol. 1: "));
    // A TTY stderr is colored by default, so `error:` carries SGR codes.
    const std::size_t error_at = r.err.find("mangadex-dl: \x1b[31merror:\x1b[0m cannot sync ");
    REQUIRE(error_at != std::string::npos);
    CHECK(r.err.rfind("\r[", error_at) != std::string::npos); // progress came first
    CHECK(diagnostics_start_clean(r.err));
    CHECK(r.err.ends_with('\n'));
    CHECK(lines_of(r.err.substr(error_at)).size() == 1); // the error is the last thing printed
    CHECK(list_dir(out).empty());                        // nothing published, no temporaries
}

TEST_CASE("diagnostics during a TTY download never share a line with the progress",
          "[cli][download][progress]") {
    const Tree tree{mdx::incomplete_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Captured r = run_args(concat({"-v"}, tree.download(out)), Tty{.stderr_tty = true});
    CHECK(r.exit_code == 6);
    INFO("stderr: " << r.err);
    CHECK(r.err.contains("\r[2/2] Vol. 2: "));
    CHECK(r.err.contains("mangadex-dl: \x1b[31merror:\x1b[0m "));
    CHECK(diagnostics_start_clean(r.err));
    CHECK(progress_cleared_at_end(r.err));
}

// ---- interrupts --------------------------------------------------------------------------------

TEST_CASE("an interrupt before the run exits 1, writes nothing and leaves a second run working",
          "[cli][download][interrupt]") {
    const InterruptFlagReset reset;
    const int signal = GENERATE(SIGINT, SIGTERM);
    INFO("signal " << signal);
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const Dispositions before = current_dispositions();

    md::simulate_interrupt_for_testing(signal);
    const Captured interrupted = run_args(tree.download(out), Tty{.stderr_tty = true});
    CHECK(interrupted.exit_code == 1); // main() would re-raise; run() only returns the fallback
    CHECK(interrupted.out.empty());
    CHECK(interrupted.err.empty()); // no message: the signal death is the report
    CHECK(md::interrupt_signal() == signal);
    CHECK(!std::filesystem::exists(out));
    CHECK(current_dispositions() == before); // the handlers were restored on the way out

    md::reset_interrupt_for_testing();
    CHECK(md::interrupt_signal() == 0);
    // A second install must succeed: the previous run released its handlers.
    const Captured second = run_args(tree.download(out));
    CHECK(second.exit_code == 0);
    CHECK(second.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(second.err.empty());
    CHECK(current_dispositions() == before);
}

TEST_CASE("an interrupt with --json prints no document", "[cli][download][interrupt][json]") {
    const InterruptFlagReset reset;
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    md::simulate_interrupt_for_testing(SIGINT);
    const Captured r = run_args(concat({"--json"}, tree.download(out)));
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.empty());
    CHECK(!std::filesystem::exists(out));
}

// ---- backend and API failures ------------------------------------------------------------------

TEST_CASE("a fixture root that cannot be opened is a runtime failure, exit 1",
          "[cli][download][backend]") {
    const md::test::TempDir tmp = make_temp_dir();
    const std::filesystem::path file = tmp.path() / "not-a-dir";
    {
        const std::ofstream touch(file);
        REQUIRE(touch.good());
    }
    for (const std::filesystem::path& root : {tmp.path() / "missing", file}) {
        for (const bool json : {false, true}) {
            INFO("root " << root.string() << (json ? " (--json)" : ""));
            const std::vector<std::string> args{"download", "--api-base", "file://" + root.string(),
                                                std::string{clean_id},
                                                (tmp.path() / "out").string()};
            const Captured r = run_args(json ? concat({"--json"}, args) : args);
            CHECK(r.exit_code == 1);
            CHECK(r.out.empty());
            CHECK(r.err.starts_with("mangadex-dl: error: cannot open fixture root '"));
            CHECK(lines_of(r.err).size() == 1);
        }
    }
    CHECK(!std::filesystem::exists(tmp.path() / "out"));
}

TEST_CASE("a malformed API document is a network failure, exit 3", "[cli][download][network]") {
    const Tree tree{mdx::clean_fixture()};
    {
        std::ofstream broken(tree.root() / "api" / "manga" / (std::string{clean_id} + ".json"),
                             std::ios::binary | std::ios::trunc);
        broken << "{\"result\": ";
        REQUIRE(broken.good());
    }
    const std::filesystem::path out = tree.path("out");
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const auto args = tree.download(out);
        const Captured r = run_args(json ? concat({"--json"}, args) : args);
        CHECK(r.exit_code == 3);
        CHECK(r.out.empty());
        CHECK(r.err.starts_with("mangadex-dl: error: "));
        CHECK(lines_of(r.err).size() == 1);
    }
    CHECK(!std::filesystem::exists(out));
}

// ---- fixture mode ------------------------------------------------------------------------------

TEST_CASE("fixture mode uses the instant clock and never loads libcurl",
          "[cli][download][fixture]") {
    constexpr std::size_t chapters = 100;
    const Tree tree{many_chapters_fixture(chapters)};
    const std::filesystem::path out = tree.path("out");
    const bool curl_before = libcurl_mapped();

    const auto start = std::chrono::steady_clock::now();
    const Captured r = run_args(tree.download(out, {"--layout", "single"}));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(r.exit_code == 0);
    CHECK(lines_of(r.out).size() == 1);
    // 100 at-home requests plus the title, feed and cover requests are spaced 250 ms apart on a
    // real clock (over 25 s). The instant clock never sleeps; 15 s leaves room for a loaded,
    // sanitized machine while still catching a real clock.
    INFO("elapsed ms: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    CHECK(elapsed < std::chrono::seconds{15});
    if (!curl_before) { // another test in this process may have loaded it legitimately
        CHECK(!libcurl_mapped());
    }
}
