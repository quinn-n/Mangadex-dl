// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/download_output_test.cpp: what `download` prints (download plan T13, skip-existing
// plan T11). The ConsoleObserver is driven directly over ostringstream-backed Io (run_download is
// not called): the stdout path stream, warnings and errors, -q/-v/-vv, the progress line and its
// clearing, color decisions, terminal-escape neutralization, the up-to-date note, the update
// lines, and the --json report never carrying ANSI. One case runs the whole chain in-process to
// prove that text read from an existing file's manifest reaches the terminal neutralized.

#include "app/cmd_download_output.hpp"
#include "app/command.hpp"
#include "app/io.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "support/download_cli_harness.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/zip_forge.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
using mangadex_dl::test::Captured;

/// An Io over two string streams. Not copyable or movable (Io holds references to the members).
struct Capture {
    std::ostringstream out{};
    std::ostringstream err{};
    app::Io io{.out = out, .err = err};
};

[[nodiscard]] md::PlannedEpub volume_epub(std::string volume, std::size_t pages) {
    return md::PlannedEpub{
        .kind = md::EpubKind::volume,
        .file_name = "T - Vol. " + volume + ".epub",
        .display_title = "T Vol. " + volume,
        .volume = std::move(volume),
        .planned_pages = pages,
    };
}

[[nodiscard]] md::EpubOutcome outcome(md::EpubStatus status, std::string path) {
    return md::EpubOutcome{
        .epub = volume_epub("03", 180),
        .path = std::move(path),
        .status = status,
        .pages = 180,
        .bytes = 4096,
    };
}

/// The UTF-8 encoding of `cp` (U+0080 to U+FFFF), built at runtime: bidi controls written in
/// source, even as escapes, trip GCC -Wbidi-chars and clang-tidy misc-misleading-bidirectional.
[[nodiscard]] std::string utf8(std::uint32_t cp) {
    std::string out;
    if (cp < 0x800) {
        out += static_cast<char>(0xC0U | (cp >> 6U));
    } else {
        out += static_cast<char>(0xE0U | (cp >> 12U));
        out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    }
    out += static_cast<char>(0x80U | (cp & 0x3FU));
    return out;
}

/// An outcome left alone because the file of that name already holds every chapter.
[[nodiscard]] md::EpubOutcome up_to_date(std::string path) {
    md::EpubOutcome out = outcome(md::EpubStatus::exists, std::move(path));
    out.existing = md::ExistingKind::complete;
    return out;
}

[[nodiscard]] md::Chapter numbered(std::string id, std::string number) {
    return md::Chapter{.id = std::move(id), .volume = "2", .chapter = std::move(number)};
}

constexpr std::string_view yellow_warning = "\x1b[33mwarning:\x1b[0m";
constexpr std::string_view red_error = "\x1b[31merror:\x1b[0m";

} // namespace

TEST_CASE("output_policy: progress needs a TTY stderr and no -q, --json or TERM=dumb",
          "[download-output][policy]") {
    Capture cap;
    const app::GlobalOptions defaults{};
    CHECK(!app::output_policy(defaults, cap.io).progress); // stderr is not a TTY

    cap.io.stderr_tty = true;
    CHECK(app::output_policy(defaults, cap.io).progress);
    CHECK(!app::output_policy(app::GlobalOptions{.json = true}, cap.io).progress);
    CHECK(!app::output_policy(app::GlobalOptions{.quiet = true}, cap.io).progress);

    cap.io.env.term_dumb = true;
    CHECK(!app::output_policy(defaults, cap.io).progress);
}

TEST_CASE("output_policy: copies the flags and decides stderr color with resolve_color",
          "[download-output][policy]") {
    Capture cap;
    cap.io.stderr_tty = true;
    cap.io.stdout_tty = false;
    const app::OutputPolicy policy =
        app::output_policy(app::GlobalOptions{.json = true, .quiet = false, .verbose = 2}, cap.io);
    CHECK(policy ==
          app::OutputPolicy{
              .json = true, .quiet = false, .verbose = 2, .progress = false, .color_stderr = true});

    CHECK(!app::output_policy(app::GlobalOptions{.color = app::ColorMode::never}, cap.io)
               .color_stderr);
    cap.io.env.no_color = true;
    CHECK(!app::output_policy(app::GlobalOptions{}, cap.io).color_stderr);
    CHECK(app::output_policy(app::GlobalOptions{.color = app::ColorMode::always}, cap.io)
              .color_stderr);

    cap.io.env = {};
    cap.io.stderr_tty = false;
    cap.io.stdout_tty = true; // stdout's TTY does not color stderr
    CHECK(!app::output_policy(app::GlobalOptions{}, cap.io).color_stderr);
}

TEST_CASE("ConsoleObserver streams written and planned paths to stdout, one per line",
          "[download-output][stdout]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
    observer.on_epub_end(outcome(md::EpubStatus::written, "out/T - Vol. 01.epub"));
    observer.on_epub_end(up_to_date("out/T - Vol. 02.epub"));
    observer.on_epub_end(outcome(md::EpubStatus::planned, "out/T - Vol. 03.epub"));
    observer.on_epub_end(outcome(md::EpubStatus::written, "out/bad \xff name.epub"));
    // Existing (skipped) files are not listed; paths are data, written byte for byte.
    CHECK(cap.out.str() == "out/T - Vol. 01.epub\nout/T - Vol. 03.epub\nout/bad \xff name.epub\n");
    CHECK(cap.err.str().empty());
}

TEST_CASE("ConsoleObserver with --json prints no paths", "[download-output][stdout][json]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.json = true});
    observer.on_epub_end(outcome(md::EpubStatus::written, "a.epub"));
    observer.on_epub_end(outcome(md::EpubStatus::planned, "b.epub"));
    observer.finish();
    CHECK(cap.out.str().empty());
    CHECK(cap.err.str().empty());
}

TEST_CASE("ConsoleObserver reports a failed file as an error on stderr, even with -q",
          "[download-output][stderr]") {
    for (const bool quiet : {false, true}) {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.quiet = quiet});
        md::EpubOutcome failed = outcome(md::EpubStatus::failed, "out/x.epub");
        failed.error = "image server returned HTTP 503";
        observer.on_epub_end(failed);
        CHECK(cap.out.str().empty());
        CHECK(cap.err.str() == "mangadex-dl: error: out/x.epub: image server returned HTTP 503\n");
    }
}

TEST_CASE("ConsoleObserver warnings: prefixed, suppressed by -q", "[download-output][stderr]") {
    {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
        observer.on_warning("skipped chapter 3: hosted externally");
        CHECK(cap.err.str() == "mangadex-dl: warning: skipped chapter 3: hosted externally\n");
        CHECK(cap.out.str().empty());
    }
    {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.quiet = true, .verbose = 2});
        observer.on_warning("skipped chapter 3: hosted externally");
        observer.on_verbose(1, "detail");
        observer.on_epub_end(up_to_date("a.epub"));
        observer.on_epub_end(outcome(md::EpubStatus::exists, "appeared.epub"));
        md::EpubOutcome planned = outcome(md::EpubStatus::planned, "update.epub");
        planned.existing = md::ExistingKind::incomplete;
        observer.on_epub_end(planned);
        observer.finish();
        CHECK(cap.err.str().empty());
    }
}

TEST_CASE("ConsoleObserver -v and -vv levels", "[download-output][verbose]") {
    for (const int level : {0, 1, 2}) {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = level});
        observer.on_verbose(1, "level one");
        observer.on_verbose(2, "level two");
        const std::string err = cap.err.str();
        CHECK(err.contains("mangadex-dl: level one\n") == (level >= 1));
        CHECK(err.contains("mangadex-dl: level two\n") == (level >= 2));
    }
}

TEST_CASE("ConsoleObserver -v prints per-file and dedupe detail", "[download-output][verbose]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1});
    md::Plan plan{};
    plan.epubs = {volume_epub("01", 10)};
    plan.dedupe = {md::DedupeNote{.key = "Vol. 1 Ch. 2",
                                  .kept_id = "kept-id",
                                  .dropped_ids = {"x", "y"},
                                  .rule = "most pages"}};
    observer.on_plan(plan);
    md::EpubOutcome written = outcome(md::EpubStatus::written, "a.epub");
    written.replaced = true;
    observer.on_epub_end(written);
    const std::string err = cap.err.str();
    CHECK(err.contains("mangadex-dl: planned 1 file; 0 chapters skipped\n"));
    CHECK(err.contains("mangadex-dl: Vol. 1 Ch. 2: kept kept-id (most pages); dropped x, y\n"));
    CHECK(err.contains("mangadex-dl: wrote a.epub (180 pages, 4096 bytes, replaced the existing "
                       "file)\n"));
    CHECK(cap.out.str() == "a.epub\n");
}

TEST_CASE("ConsoleObserver prints one up-to-date note at finish: no prefix, never colored",
          "[download-output][stderr][up-to-date]") {
    SECTION("plural") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.color_stderr = true});
        observer.on_epub_end(up_to_date("a.epub"));
        observer.on_epub_end(up_to_date("b.epub"));
        CHECK(cap.err.str().empty()); // nothing until the end
        observer.finish();
        observer.finish(); // idempotent
        CHECK(cap.err.str() == "mangadex-dl: 2 files are already up to date; pass --overwrite to "
                               "download them again\n");
        CHECK(cap.out.str().empty());
    }
    SECTION("singular") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.color_stderr = true});
        observer.on_epub_end(up_to_date("a.epub"));
        observer.finish();
        CHECK(cap.err.str() == "mangadex-dl: 1 file is already up to date; pass --overwrite to "
                               "download it again\n");
    }
    SECTION("none") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
        observer.on_epub_end(outcome(md::EpubStatus::written, "a.epub"));
        observer.finish();
        CHECK(cap.err.str().empty());
    }
    SECTION("-q hides it") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.quiet = true});
        observer.on_epub_end(up_to_date("a.epub"));
        observer.on_epub_end(up_to_date("b.epub"));
        observer.finish();
        CHECK(cap.err.str().empty());
    }
}

TEST_CASE("The up-to-date note counts only files left alone because they are complete",
          "[download-output][stderr][up-to-date]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
    observer.on_epub_end(up_to_date("a.epub"));
    for (const md::ExistingKind kind :
         {md::ExistingKind::diverged, md::ExistingKind::foreign, md::ExistingKind::unchecked}) {
        md::EpubOutcome left = outcome(md::EpubStatus::exists, "left.epub");
        left.existing = kind;
        observer.on_epub_end(left); // the run warned about these when it assessed them
    }
    md::EpubOutcome updated = outcome(md::EpubStatus::written, "u.epub");
    updated.existing = md::ExistingKind::incomplete;
    updated.replaced = true;
    observer.on_epub_end(updated);
    md::EpubOutcome planned = outcome(md::EpubStatus::planned, "p.epub");
    planned.existing = md::ExistingKind::complete; // only an exists outcome counts
    observer.on_epub_end(planned);
    CHECK(cap.err.str().empty());
    observer.finish();
    CHECK(cap.err.str() == "mangadex-dl: 1 file is already up to date; pass --overwrite to "
                           "download it again\n");
    CHECK(cap.out.str() == "u.epub\np.epub\n");
}

TEST_CASE("The up-to-date note is never colored, even with --color=always on a TTY",
          "[download-output][stderr][up-to-date][color]") {
    Capture cap;
    cap.io.stderr_tty = true;
    const app::OutputPolicy policy =
        app::output_policy(app::GlobalOptions{.color = app::ColorMode::always}, cap.io);
    REQUIRE(policy.color_stderr);
    app::ConsoleObserver observer(cap.io, policy);
    observer.on_epub_end(up_to_date("a.epub"));
    observer.on_warning("w");
    observer.finish();
    CHECK(cap.err.str() == "mangadex-dl: " + std::string{yellow_warning} +
                               " w\n"
                               "mangadex-dl: 1 file is already up to date; pass --overwrite to "
                               "download it again\n");
}

TEST_CASE("Verbose output names an up-to-date file with its chapter count",
          "[download-output][verbose]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1});
    md::EpubOutcome two = up_to_date("out/T - Vol. 01.epub");
    two.epub.chapters = {numbered("a", "1"), numbered("b", "2")};
    md::EpubOutcome one = up_to_date("out/T - Vol. 02.epub");
    one.epub.chapters = {numbered("c", "3")};
    observer.on_epub_end(two);
    observer.on_epub_end(one);
    CHECK(cap.err.str() == "mangadex-dl: out/T - Vol. 01.epub: up to date (2 chapters)\n"
                           "mangadex-dl: out/T - Vol. 02.epub: up to date (1 chapter)\n");
    CHECK(cap.out.str().empty()); // files left alone are not data

    Capture plain;
    app::ConsoleObserver quiet_v(plain.io, app::OutputPolicy{});
    quiet_v.on_epub_end(two);
    CHECK(plain.err.str().empty()); // without -v only the closing note mentions it
}

TEST_CASE("Verbose dry run: 'would update' for an incomplete file, 'would replace' for "
          "--overwrite",
          "[download-output][verbose][dry-run]") {
    SECTION("one new chapter, two reused") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1});
        md::EpubOutcome planned = outcome(md::EpubStatus::planned, "out/T - Vol. 02.epub");
        planned.existing = md::ExistingKind::incomplete;
        planned.replaced = true;
        planned.epub.chapters = {numbered("c2", "2"), numbered("c3", "3"), numbered("c4", "4")};
        planned.new_chapters = {"c4"};
        planned.reused_chapters = {"c2", "c3"};
        observer.on_epub_end(planned);
        CHECK(cap.out.str() == "out/T - Vol. 02.epub\n");
        CHECK(cap.err.str() ==
              "mangadex-dl: would update out/T - Vol. 02.epub (1 new chapter, 2 reused: Ch. 4)\n");
    }
    SECTION("at most five labels, then 'and K more', in the file's order") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1});
        md::EpubOutcome planned = outcome(md::EpubStatus::planned, "v.epub");
        planned.existing = md::ExistingKind::incomplete;
        planned.replaced = true;
        planned.epub.chapters = {numbered("r", "1")};
        for (int n = 2; n <= 8; ++n) {
            planned.epub.chapters.push_back(numbered(std::format("n{}", n), std::to_string(n)));
        }
        // Listed in another order than the file's: the labels follow the file.
        planned.new_chapters = {"n8", "n7", "n6", "n5", "n4", "n3", "n2"};
        planned.reused_chapters = {"r"};
        observer.on_epub_end(planned);
        CHECK(cap.err.str() == "mangadex-dl: would update v.epub (7 new chapters, 1 reused: Ch. 2, "
                               "Ch. 3, Ch. 4, Ch. 5, Ch. 6 and 2 more)\n");
    }
    SECTION("no labels when the new ids are not in the file's list") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1});
        md::EpubOutcome planned = outcome(md::EpubStatus::planned, "v.epub");
        planned.existing = md::ExistingKind::incomplete;
        planned.epub.chapters = {numbered("r", "1")};
        planned.new_chapters = {"elsewhere"};
        planned.reused_chapters = {"r"};
        observer.on_epub_end(planned);
        CHECK(cap.err.str() == "mangadex-dl: would update v.epub (1 new chapter, 1 reused)\n");
    }
    SECTION("--overwrite and new files keep their lines") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1});
        md::EpubOutcome replaced = outcome(md::EpubStatus::planned, "a.epub");
        replaced.existing = md::ExistingKind::unchecked;
        replaced.replaced = true;
        observer.on_epub_end(replaced);
        observer.on_epub_end(outcome(md::EpubStatus::planned, "b.epub"));
        CHECK(cap.err.str() == "mangadex-dl: would replace a.epub (180 pages)\n"
                               "mangadex-dl: would write b.epub (180 pages)\n");
    }
    SECTION("-q hides it, the path stays on stdout") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.quiet = true, .verbose = 1});
        md::EpubOutcome planned = outcome(md::EpubStatus::planned, "v.epub");
        planned.existing = md::ExistingKind::incomplete;
        planned.new_chapters = {"x"};
        observer.on_epub_end(planned);
        CHECK(cap.err.str().empty());
        CHECK(cap.out.str() == "v.epub\n");
    }
}

TEST_CASE("A failed update says the existing file was kept; other failures do not",
          "[download-output][stderr]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.quiet = true});
    md::EpubOutcome update = outcome(md::EpubStatus::failed, "out/T - Vol. 02.epub");
    update.existing = md::ExistingKind::incomplete;
    update.error = "image server returned HTTP 503";
    observer.on_epub_end(update);
    for (const md::ExistingKind kind : {md::ExistingKind::none, md::ExistingKind::unchecked}) {
        md::EpubOutcome other = outcome(md::EpubStatus::failed, "x.epub");
        other.existing = kind;
        other.error = "boom";
        observer.on_epub_end(other);
    }
    md::EpubOutcome no_message = outcome(md::EpubStatus::failed, "y.epub");
    no_message.existing = md::ExistingKind::incomplete;
    observer.on_epub_end(no_message);
    CHECK(cap.err.str() == "mangadex-dl: error: out/T - Vol. 02.epub: image server returned HTTP "
                           "503 (the existing file was kept)\n"
                           "mangadex-dl: error: x.epub: boom\n"
                           "mangadex-dl: error: x.epub: boom\n"
                           "mangadex-dl: error: y.epub: failed (the existing file was kept)\n");
    CHECK(cap.out.str().empty());
}

TEST_CASE("A file that appeared while ours was being written gets its own warning",
          "[download-output][stderr]") {
    // status exists without an existing file: nothing was there at assessment, but another
    // program published a file of that name before ours, so ours was discarded.
    const md::EpubOutcome appeared = outcome(md::EpubStatus::exists, "out/\x1b[2JT - Vol. 01.epub");
    {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
        observer.on_epub_end(appeared);
        observer.finish();
        CHECK(cap.err.str() ==
              "mangadex-dl: warning: out/?[2JT - Vol. 01.epub: appeared while it was being "
              "written; left alone (pass --overwrite to replace it)\n");
        CHECK(cap.out.str().empty()); // not data, and not counted as up to date
    }
    {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.color_stderr = true});
        observer.on_epub_end(appeared);
        CHECK(cap.err.str().starts_with("mangadex-dl: " + std::string{yellow_warning} + " out/"));
    }
    {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.quiet = true});
        observer.on_epub_end(appeared);
        observer.finish();
        CHECK(cap.err.str().empty());
    }
}

TEST_CASE("Hostile text from an existing file's manifest is neutralized in warnings and -v lines",
          "[download-output][stderr][terminal-safe]") {
    // The shapes the run emits with text it read from a manifest: group names (re-pick), chapter
    // labels (damaged). Each carries an escape sequence, a newline and a bidi override.
    const std::string group = "Evil\x1b]0;pwned\x07\nScans" + utf8(0x202E);
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1, .color_stderr = true});
    observer.on_verbose(1, std::format("a.epub: Vol. 1 Ch. 1: replacing the release in the file "
                                       "({}) with Bravo Scans's (--group)",
                                       group));
    observer.on_warning("a.epub: Ch. 3\x1b[2J in the existing file is damaged; downloading it "
                        "again");
    const std::string err = cap.err.str();
    CHECK(err == "mangadex-dl: a.epub: Vol. 1 Ch. 1: replacing the release in the file "
                 "(Evil?]0;pwned??Scans?) with Bravo Scans's (--group)\n"
                 "mangadex-dl: " +
                     std::string{yellow_warning} +
                     " a.epub: Ch. 3?[2J in the existing file is damaged; downloading it again\n");
}

TEST_CASE("A group name read from an existing EPUB reaches the terminal neutralized",
          "[download-output][stderr][terminal-safe][update]") {
    namespace cli = md::test::download_cli;
    namespace forge = md::test::forge;
    const cli::Tree clean{mdx::clean_fixture()};
    const cli::Tree served{mdx::clean_regrouped_served_fixture()};
    const std::filesystem::path out = clean.path("out");
    REQUIRE(cli::run_args(clean.download(out)).exit_code == 0);

    // Vol. 01's manifest names its group with an OSC title sequence, a newline and a BEL (JSON
    // escapes, so the manifest stays valid); content.opf is untouched, so the file stays ours.
    const std::filesystem::path vol1 = out / "Clean Title - Vol. 01.epub";
    const std::string original = cli::read_file(vol1);
    bool patched = false;
    const auto edited = forge::rewrite_entries(
        zip::byte_vector(original), [&patched](std::vector<forge::ForgeEntry>& entries) {
            for (forge::ForgeEntry& entry : entries) {
                if (entry.name != "META-INF/mangadex-dl.json") {
                    continue;
                }
                std::string text(entry.data.size(), '\0');
                std::ranges::transform(entry.data, text.begin(),
                                       [](std::byte b) { return std::to_integer<char>(b); });
                const std::string from = R"("name":"Clean Scans")";
                const std::string to = R"("name":"Clean\u001b]0;pwned\u0007\nScans")";
                for (std::size_t at = text.find(from); at != std::string::npos;
                     at = text.find(from, at + to.size())) {
                    text.replace(at, from.size(), to);
                    patched = true;
                }
                entry.data = forge::text_bytes(text);
            }
        });
    REQUIRE(edited.has_value());
    REQUIRE(patched);
    REQUIRE(forge::write_file(vol1, *edited));

    // --group re-picks B's releases, so the -v line names the held group read from the file.
    const Captured r = cli::run_args(
        cli::concat({"-v", "--color=always"}, served.download(out, {"--group", "Bravo Scans"})));
    INFO(r.err);
    CHECK(r.exit_code == 0);
    CHECK(r.err.contains("Vol. 1 Ch. 1: replacing the release in the file "
                         "(Clean?]0;pwned??Scans) with Bravo Scans's (--group)\n"));
    CHECK(!r.err.contains('\x07'));
    CHECK(!r.err.contains("\x1b]"));
    for (const std::string& line : cli::lines_of(r.err)) {
        INFO(line);
        CHECK(line.starts_with("mangadex-dl: "));
    }
}

TEST_CASE("ConsoleObserver progress: \\r-rewritten line, padded, cleared before warnings",
          "[download-output][progress]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.progress = true});
    observer.on_epub_begin(volume_epub("03", 180), 2, 12);
    observer.on_pages(45, 180);
    CHECK(cap.err.str() == "\r[3/12] Vol. 03: 0/180 pages\r[3/12] Vol. 03: 45/180 pages");

    cap.err.str("");
    observer.on_warning("WebP pages");
    // Cleared with '\r' + spaces + '\r', then the warning, then the line is drawn again.
    const std::string line = "[3/12] Vol. 03: 45/180 pages";
    CHECK(cap.err.str() == "\r" + std::string(line.size(), ' ') + "\r" +
                               "mangadex-dl: warning: WebP pages\n" + "\r" + line);

    cap.err.str("");
    observer.on_epub_end(outcome(md::EpubStatus::written, "out/a.epub"));
    CHECK(cap.err.str() == "\r" + std::string(line.size(), ' ') + "\r");
    CHECK(cap.out.str() == "out/a.epub\n");

    // The next file starts on a clean line; finish() clears it and leaves the cursor at column 0.
    cap.err.str("");
    observer.on_epub_begin(volume_epub("4", 9), 4, 12);
    const std::string next_line = "[5/12] Vol. 4: 0/9 pages";
    CHECK(cap.err.str() == "\r" + next_line);
    cap.err.str("");
    observer.finish();
    CHECK(cap.err.str() == "\r" + std::string(next_line.size(), ' ') + "\r");
}

TEST_CASE("ConsoleObserver progress pads a shorter redraw to the previous width",
          "[download-output][progress]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.progress = true});
    observer.on_epub_begin(volume_epub("1", 100), 0, 1);
    observer.on_pages(100, 100);
    cap.err.str("");
    observer.on_pages(5, 6); // shorter than "[1/1] Vol. 1: 100/100 pages"
    const std::string previous = "[1/1] Vol. 1: 100/100 pages";
    const std::string current = "[1/1] Vol. 1: 5/6 pages";
    CHECK(cap.err.str() == "\r" + current + std::string(previous.size() - current.size(), ' '));
}

TEST_CASE("ConsoleObserver draws no progress without policy.progress",
          "[download-output][progress]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.progress = false});
    observer.on_epub_begin(volume_epub("03", 180), 0, 1);
    observer.on_pages(45, 180);
    observer.on_warning("w");
    observer.on_epub_end(outcome(md::EpubStatus::written, "a.epub"));
    observer.finish();
    CHECK(cap.err.str() == "mangadex-dl: warning: w\n");
    CHECK(!cap.err.str().contains('\r'));
}

TEST_CASE("ConsoleObserver progress labels per kind, never the title, escapes neutralized",
          "[download-output][progress]") {
    struct Case {
        md::PlannedEpub epub{};
        std::string label{};
    };
    const md::PlannedEpub chapter_in_volume{
        .kind = md::EpubKind::chapter, .volume = "3", .chapter = "21", .planned_pages = 1};
    const md::PlannedEpub loose{
        .kind = md::EpubKind::chapter, .chapter = "12.5", .planned_pages = 1};
    const md::PlannedEpub oneshot{.kind = md::EpubKind::oneshot, .planned_pages = 1};
    const md::PlannedEpub single{
        .kind = md::EpubKind::single, .display_title = "Secret Title", .planned_pages = 1};
    const md::PlannedEpub hostile{
        .kind = md::EpubKind::volume, .volume = "\x1b[2J\x1b[H", .planned_pages = 1};
    const md::PlannedEpub long_volume{
        .kind = md::EpubKind::volume, .volume = std::string(100, 'x'), .planned_pages = 1};
    std::string cjk = "x";
    for (int i = 0; i < 9; ++i) {
        cjk += "巻";
    }
    const md::PlannedEpub cjk_volume{
        .kind = md::EpubKind::volume, .volume = cjk, .planned_pages = 1};
    const std::vector<Case> cases = {
        {.epub = chapter_in_volume, .label = "Vol. 3 Ch. 21"},
        {.epub = loose, .label = "Ch. 12.5"},
        {.epub = oneshot, .label = "Oneshot"},
        {.epub = single, .label = "All chapters"},
        {.epub = hostile, .label = "Vol. ?[2J?[H"},
        {.epub = long_volume, .label = "Vol. " + std::string(27, 'x') + "..."},
        // "Vol. x" is 6 bytes and each 巻 is 3, so byte 32 falls inside the ninth 巻: the cut
        // backs up to the code-point boundary at byte 30.
        {.epub = cjk_volume, .label = "Vol. x巻巻巻巻巻巻巻巻..."},
    };
    for (const Case& c : cases) {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.progress = true});
        observer.on_epub_begin(c.epub, 0, 1);
        CHECK(cap.err.str() == "\r[1/1] " + c.label + ": 0/1 pages");
        CHECK(!cap.err.str().contains('\x1b'));
    }
}

TEST_CASE("ConsoleObserver color: yellow warning and red error only when enabled",
          "[download-output][color]") {
    {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.color_stderr = true});
        observer.on_warning("w");
        md::EpubOutcome failed = outcome(md::EpubStatus::failed, "x.epub");
        failed.error = "boom";
        observer.on_epub_end(failed);
        CHECK(cap.err.str() == "mangadex-dl: " + std::string{yellow_warning} + " w\n" +
                                   "mangadex-dl: " + std::string{red_error} + " x.epub: boom\n");
    }
    // --color=never and NO_COLOR both reach the observer as color_stderr = false.
    for (const bool no_color_env : {false, true}) {
        Capture cap;
        cap.io.stderr_tty = true;
        cap.io.env.no_color = no_color_env;
        const app::GlobalOptions globals{.color = no_color_env ? app::ColorMode::automatic
                                                               : app::ColorMode::never};
        app::ConsoleObserver observer(cap.io, app::output_policy(globals, cap.io));
        observer.on_epub_begin(volume_epub("1", 2), 0, 1);
        observer.on_warning("w");
        observer.on_epub_end(outcome(md::EpubStatus::failed, "x.epub"));
        observer.finish();
        CHECK(!cap.err.str().contains('\x1b')); // no color, no cursor escapes
        CHECK(cap.err.str().contains("mangadex-dl: warning: w\n"));
    }
}

TEST_CASE("Messages and paths on stderr are terminal-safe", "[download-output][stderr]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 2});
    observer.on_warning("chapter \x1b]0;pwned\x07 skipped");
    observer.on_verbose(2, "GET /x\r\nInjected: yes");
    md::EpubOutcome failed = outcome(md::EpubStatus::failed, "dir\x1b[31m/x.epub");
    failed.error = "bad\nline";
    observer.on_epub_end(failed);
    const std::string err = cap.err.str();
    CHECK(!err.contains('\x1b'));
    CHECK(!err.contains('\x07'));
    CHECK(!err.contains('\r'));
    CHECK(err == "mangadex-dl: warning: chapter ?]0;pwned? skipped\n"
                 "mangadex-dl: GET /x??Injected: yes\n"
                 "mangadex-dl: error: dir?[31m/x.epub: bad?line\n");
}

TEST_CASE("terminal_safe keeps printable UTF-8 and replaces everything that can steer a terminal",
          "[download-output][terminal-safe]") {
    CHECK(app::terminal_safe("") == "");
    CHECK(app::terminal_safe("Vol. 3 - plain ASCII ~") == "Vol. 3 - plain ASCII ~");
    CHECK(app::terminal_safe("進撃の巨人 é 😀") == "進撃の巨人 é 😀");
    CHECK(app::terminal_safe(std::string_view{"a\0b", 3}) == "a?b");
    CHECK(app::terminal_safe("\t\n\r\x1b\x7f") == "?????");
    CHECK(app::terminal_safe("\xc2\x9b"
                             "31m") == "?31m"); // C1 CSI, UTF-8 encoded
    CHECK(app::terminal_safe("\x9b"
                             "31m") == "?31m"); // raw 8-bit CSI (invalid UTF-8)
}

TEST_CASE("terminal_safe replaces bidi, invisible and separator code points, keeps neighbours",
          "[download-output][terminal-safe]") {
    struct Range {
        std::uint32_t first = 0;
        std::uint32_t last = 0;
    };
    const std::vector<Range> unsafe = {
        {.first = 0x0080, .last = 0x009F}, // C1 controls
        {.first = 0x061C, .last = 0x061C}, // ARABIC LETTER MARK
        {.first = 0x200B, .last = 0x200F}, // ZWSP, ZWNJ, ZWJ, LRM, RLM
        {.first = 0x202A, .last = 0x202E}, // bidi embeddings and overrides
        {.first = 0x2028, .last = 0x2029}, // line and paragraph separators
        {.first = 0x2060, .last = 0x206F}, // word joiner, invisible operators, isolates, ...
        {.first = 0xFEFF, .last = 0xFEFF}, // ZERO WIDTH NO-BREAK SPACE (BOM)
    };
    for (const Range& range : unsafe) {
        for (std::uint32_t cp = range.first; cp <= range.last; ++cp) {
            INFO("U+" << std::hex << cp);
            CHECK(app::terminal_safe(std::format("a{}b", utf8(cp))) == "a?b");
        }
    }
    // The code points right next to each range are ordinary text and are kept.
    for (const std::uint32_t cp : {0x00A0U, 0x061BU, 0x061DU, 0x200AU, 0x2010U, 0x2027U, 0x202FU,
                                   0x205FU, 0x2070U, 0xFEFEU, 0xFF01U}) {
        INFO("U+" << std::hex << cp);
        const std::string text = std::format("a{}b", utf8(cp));
        CHECK(app::terminal_safe(text) == text);
    }
}

TEST_CASE("terminal_safe replaces each invalid UTF-8 byte", "[download-output][terminal-safe]") {
    CHECK(app::terminal_safe("\xff\xfe") == "??");
    CHECK(app::terminal_safe("\xc0\xaf") == "??");           // overlong '/'
    CHECK(app::terminal_safe("\xed\xa0\x80") == "???");      // UTF-16 surrogate
    CHECK(app::terminal_safe("\xf4\x90\x80\x80") == "????"); // above U+10FFFF
    CHECK(app::terminal_safe("x\xe3\x81") == "x??");         // truncated at the end
    CHECK(app::terminal_safe("\xe3\x81x") == "??x");         // truncated before ASCII
}

TEST_CASE("print_error: prefix, red only when enabled, terminal-safe",
          "[download-output][stderr]") {
    Capture cap;
    app::print_error(cap.io, app::OutputPolicy{}, "network down\x1b[0m");
    app::print_error(cap.io, app::OutputPolicy{.color_stderr = true}, "again");
    CHECK(cap.err.str() == "mangadex-dl: error: network down?[0m\n"
                           "mangadex-dl: " +
                               std::string{red_error} + " again\n");
    CHECK(cap.out.str().empty());
}

TEST_CASE("print_report_json writes one valid document without ANSI, even with --color=always",
          "[download-output][json]") {
    Capture cap;
    cap.io.stdout_tty = true;
    cap.io.stderr_tty = true;
    cap.io.env.clicolor_force = true;
    const app::OutputPolicy policy = app::output_policy(
        app::GlobalOptions{.json = true, .color = app::ColorMode::always}, cap.io);
    CHECK(policy.color_stderr); // stderr may be colored; the document never is
    CHECK(!policy.progress);

    md::RunReport report{};
    report.manga.title = "\x1b[31mred\x1b[0m";
    report.epubs = {outcome(md::EpubStatus::written, "a.epub")};
    app::ConsoleObserver observer(cap.io, policy);
    observer.on_epub_end(report.epubs.front());
    observer.finish();
    app::print_report_json(cap.io, report);

    const std::string out = cap.out.str();
    CHECK(!out.contains('\x1b'));
    REQUIRE(!out.empty());
    CHECK(out.back() == '\n');
    CHECK(out == md::report_to_json(report));
    const nlohmann::json doc = nlohmann::json::parse(out, nullptr, false);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("epubs").at(0).at("path") == "a.epub");
    CHECK(cap.err.str().empty());
}
