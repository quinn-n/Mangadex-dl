// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/download_row_output_test.cpp: the row-aware ConsoleObserver (batch plan T4). Driven
// directly over ostringstream-backed Io: a default RowContext prints exactly what `download`
// prints; a row prefix goes after `warning:`/`error:` (never on verbose lines) and is sanitized
// with the message; position/total add a `(i/n) ` progress counter; exists_notice=false leaves the
// "already exist" notice to the caller. Plus Io::stdin_fd and run_cli's stdin_fd parameter.

#include "app/cmd_download_output.hpp"
#include "app/command.hpp"
#include "app/io.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "support/io_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;

/// An Io over two string streams. Not copyable or movable (Io holds references to the members,
/// which a copy or a move would leave pointing into the source), so all four are deleted.
struct Capture {
    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;
    ~Capture() = default;

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

[[nodiscard]] md::EpubOutcome outcome(md::EpubStatus status, std::string path,
                                      std::string error = {}) {
    return md::EpubOutcome{
        .epub = volume_epub("03", 180),
        .path = std::move(path),
        .status = status,
        .pages = 180,
        .bytes = 4096,
        .error = std::move(error),
    };
}

/// The three lines `download` prints from the observer for one warning, one failed file and one
/// existing file (finish()), in that order.
void drive_warning_failed_exists(app::ConsoleObserver& observer) {
    observer.on_warning("skipped Vol. 2 Ch. 3: hosted outside MangaDex");
    observer.on_epub_end(outcome(md::EpubStatus::failed, "out/T - Vol. 03.epub", "HTTP 404"));
    observer.on_epub_end(outcome(md::EpubStatus::exists, "out/T - Vol. 04.epub"));
    observer.finish();
}

constexpr std::string_view download_lines =
    "mangadex-dl: warning: skipped Vol. 2 Ch. 3: hosted outside MangaDex\n"
    "mangadex-dl: error: out/T - Vol. 03.epub: HTTP 404\n"
    "mangadex-dl: warning: 1 file already exists; pass --overwrite to replace it\n";

} // namespace

TEST_CASE("RowContext defaults: no prefix, no counter, the notice on", "[download-row-output]") {
    const app::RowContext defaults{};
    CHECK(defaults.prefix.empty());
    CHECK(defaults.position == 0);
    CHECK(defaults.total == 0);
    CHECK(defaults.exists_notice);
    CHECK(app::RowContext{.prefix = "", .position = 0, .total = 0, .exists_notice = true} ==
          defaults);
    CHECK(app::RowContext{.prefix = "a.csv:2: "} != defaults);
}

TEST_CASE("ConsoleObserver with a default RowContext prints exactly download's lines",
          "[download-row-output][stderr]") {
    SECTION("two-argument constructor") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
        drive_warning_failed_exists(observer);
        CHECK(cap.err.str() == download_lines);
        CHECK(cap.out.str().empty());
    }
    SECTION("an explicit RowContext{}") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{}, app::RowContext{});
        drive_warning_failed_exists(observer);
        CHECK(cap.err.str() == download_lines);
        CHECK(cap.out.str().empty());
    }
}

TEST_CASE("ConsoleObserver row prefix: after the warning:/error: label, never on verbose lines",
          "[download-row-output][stderr]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 2},
                                  app::RowContext{.prefix = "list.csv:4: "});
    observer.on_warning("WebP pages");
    observer.on_verbose(2, "GET https://api.mangadex.org/manga/x");
    observer.on_epub_end(outcome(md::EpubStatus::written, "out/T - Vol. 03.epub"));
    observer.on_epub_end(outcome(md::EpubStatus::failed, "out/T - Vol. 05.epub", "HTTP 404"));
    observer.on_epub_end(outcome(md::EpubStatus::failed, "out/T - Vol. 06.epub"));
    observer.on_epub_end(outcome(md::EpubStatus::exists, "out/T - Vol. 07.epub"));
    observer.finish();
    CHECK(cap.err.str() ==
          "mangadex-dl: warning: list.csv:4: WebP pages\n"
          "mangadex-dl: GET https://api.mangadex.org/manga/x\n"
          "mangadex-dl: wrote out/T - Vol. 03.epub (180 pages, 4096 bytes)\n"
          "mangadex-dl: error: list.csv:4: out/T - Vol. 05.epub: HTTP 404\n"
          "mangadex-dl: error: list.csv:4: out/T - Vol. 06.epub: failed\n"
          "mangadex-dl: skipped out/T - Vol. 07.epub: it already exists\n"
          "mangadex-dl: warning: list.csv:4: 1 file already exists; pass --overwrite to replace "
          "it\n");
    // stdout stays download's path stream, unprefixed.
    CHECK(cap.out.str() == "out/T - Vol. 03.epub\n");
}

TEST_CASE("ConsoleObserver row prefix with color: the prefix follows the painted label",
          "[download-row-output][color]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.color_stderr = true},
                                  app::RowContext{.prefix = "l.csv:2: "});
    observer.on_warning("w");
    observer.on_epub_end(outcome(md::EpubStatus::failed, "out/a.epub", "boom"));
    CHECK(cap.err.str() == "mangadex-dl: \x1b[33mwarning:\x1b[0m l.csv:2: w\n"
                           "mangadex-dl: \x1b[31merror:\x1b[0m l.csv:2: out/a.epub: boom\n");
}

TEST_CASE("ConsoleObserver row prefix holding ESC or invalid UTF-8 prints as '?'",
          "[download-row-output][stderr]") {
    Capture cap;
    // FILE as typed may hold anything: an ESC sequence and a lone 0xFF byte.
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{},
                                  app::RowContext{.prefix = "a\x1b[31m\xff.csv:3: "});
    observer.on_warning("w");
    observer.on_epub_end(outcome(md::EpubStatus::failed, "out/a.epub", "boom"));
    CHECK(cap.err.str() == "mangadex-dl: warning: a?[31m?.csv:3: w\n"
                           "mangadex-dl: error: a?[31m?.csv:3: out/a.epub: boom\n");
    CHECK(!cap.err.str().contains('\x1b'));
}

TEST_CASE("ConsoleObserver row counter: (i/n) in front of the progress line",
          "[download-row-output][progress]") {
    Capture cap;
    cap.io.stderr_tty = true; // output_policy turns progress on for a TTY stderr
    const app::OutputPolicy policy = app::output_policy(app::GlobalOptions{}, cap.io);
    REQUIRE(policy.progress);
    app::ConsoleObserver observer(
        cap.io, policy, app::RowContext{.prefix = "l.csv:3: ", .position = 2, .total = 5});
    observer.on_epub_begin(volume_epub("1", 3), 0, 2);
    const std::string line = "(2/5) [1/2] Vol. 1: 0/3 pages";
    CHECK(cap.err.str() == "\r" + line);

    // The counter is part of the drawn width, so clearing covers it.
    cap.err.str("");
    observer.finish();
    CHECK(cap.err.str() == "\r" + std::string(line.size(), ' ') + "\r");
}

TEST_CASE("ConsoleObserver without a row position draws no counter",
          "[download-row-output][progress]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{.progress = true},
                                  app::RowContext{.prefix = "l.csv:3: ", .total = 5});
    observer.on_epub_begin(volume_epub("1", 3), 0, 2);
    CHECK(cap.err.str() == "\r[1/2] Vol. 1: 0/3 pages");
}

TEST_CASE("ConsoleObserver exists_notice=false leaves the notice to the caller",
          "[download-row-output][stderr]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{},
                                  app::RowContext{.prefix = "l.csv:2: ", .exists_notice = false});
    observer.on_epub_end(outcome(md::EpubStatus::exists, "out/a.epub"));
    observer.on_epub_end(outcome(md::EpubStatus::exists, "out/b.epub"));
    observer.finish();
    observer.finish();
    CHECK(cap.err.str().empty());
    CHECK(cap.out.str().empty());
}

TEST_CASE("Io::stdin_fd: -1 (closed) by default, 0 for the real process", "[download-row-output]") {
    std::ostringstream out;
    std::ostringstream err;
    const app::Io io{.out = out, .err = err};
    CHECK(io.stdin_fd == -1);
    const app::Io with_fd{.out = out, .err = err, .stdin_fd = 7};
    CHECK(with_fd.stdin_fd == 7);
    CHECK(app::process_io().stdin_fd == STDIN_FILENO);
}

TEST_CASE("run_cli takes a stdin_fd and leaves it open", "[download-row-output]") {
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    const md::UniqueFd read_end{ends[0]};
    const md::UniqueFd write_end{ends[1]};

    const auto r = md::test::run_cli({"--version"}, {}, {}, read_end.get());
    CHECK(r.exit_code == 0);
    CHECK(r.out.starts_with("mangadex-dl "));
    CHECK(r.err.empty());
    // Borrowed: still open and unread (--version never touches stdin), so a byte written now is
    // the first byte read back.
    REQUIRE(::write(write_end.get(), "x", 1) == 1);
    std::array<char, 2> got{};
    CHECK(::read(read_end.get(), got.data(), got.size()) == 1);
    CHECK(got[0] == 'x');

    const auto closed = md::test::run_cli({"--version"});
    CHECK(closed.exit_code == 0);
}
