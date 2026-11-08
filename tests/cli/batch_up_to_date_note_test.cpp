// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/batch_up_to_date_note_test.cpp: what the "N files are already up to date" note counts
// in a multi-title run (batch-spec rev5). ConsoleObserver, driven directly over
// ostringstream-backed Io, counts a file left alone as up to date only when no earlier row of the
// run claimed its path (RowContext::registry). The -v "up to date" line is a fact about the file
// and is printed either way. Without a registry (`download`) every up-to-date file counts, as
// before.

#include "app/cmd_download_output.hpp"
#include "app/io.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

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

/// A file left alone at `path`, of kind `existing`.
[[nodiscard]] md::EpubOutcome left_alone(std::string path, md::ExistingKind existing) {
    return md::EpubOutcome{
        .epub = md::PlannedEpub{.kind = md::EpubKind::volume, .file_name = "T - Vol. 01.epub"},
        .path = std::move(path),
        .status = md::EpubStatus::exists,
        .existing = existing,
    };
}

constexpr std::string_view one_note =
    "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n";

} // namespace

TEST_CASE("ConsoleObserver: an up-to-date file at a path an earlier row claimed is not counted",
          "[download-row-output][up-to-date]") {
    md::OutputRegistry registry{};
    REQUIRE(!registry.claim("out/T - Vol. 01.epub", 2).has_value());
    const app::RowContext row{.prefix = "l.csv:3: ", .registry = &registry};

    SECTION("claimed: not counted, no note") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{}, row);
        observer.on_epub_end(left_alone("out/T - Vol. 01.epub", md::ExistingKind::complete));
        observer.finish();
        CHECK(observer.up_to_date() == 0);
        CHECK(cap.err.str().empty());
        CHECK(cap.out.str().empty());
    }
    SECTION("the registry's key: a path spelled differently but the same file is claimed") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{}, row);
        observer.on_epub_end(left_alone("out/./T - Vol. 01.epub", md::ExistingKind::complete));
        observer.finish();
        CHECK(observer.up_to_date() == 0);
        CHECK(cap.err.str().empty());
    }
    SECTION("unclaimed: counted once, beside a claimed one") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{}, row);
        observer.on_epub_end(left_alone("out/T - Vol. 01.epub", md::ExistingKind::complete));
        observer.on_epub_end(left_alone("out/T - Vol. 02.epub", md::ExistingKind::complete));
        CHECK(observer.up_to_date() == 1);
        observer.finish();
        CHECK(cap.err.str() == one_note);
    }
    SECTION("-v: the 'up to date' line is printed for a claimed path too") {
        Capture cap;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{.verbose = 1}, row);
        observer.on_epub_end(left_alone("out/T - Vol. 01.epub", md::ExistingKind::complete));
        observer.on_epub_end(left_alone("out/T - Vol. 02.epub", md::ExistingKind::complete));
        observer.finish();
        CHECK(observer.up_to_date() == 1);
        CHECK(cap.err.str() == std::string{"mangadex-dl: out/T - Vol. 01.epub: up to date (0 "
                                           "chapters)\n"
                                           "mangadex-dl: out/T - Vol. 02.epub: up to date (0 "
                                           "chapters)\n"} +
                                   std::string{one_note});
    }
    SECTION("exists_notice=false: the count the caller prints skips the claimed path too") {
        Capture cap;
        app::RowContext caller_notes = row;
        caller_notes.exists_notice = false;
        app::ConsoleObserver observer(cap.io, app::OutputPolicy{}, caller_notes);
        observer.on_epub_end(left_alone("out/T - Vol. 01.epub", md::ExistingKind::complete));
        observer.on_epub_end(left_alone("out/T - Vol. 02.epub", md::ExistingKind::complete));
        observer.finish();
        CHECK(observer.up_to_date() == 1);
        CHECK(cap.err.str().empty());
    }
}

TEST_CASE("ConsoleObserver without a registry counts every up-to-date file (download)",
          "[download-row-output][up-to-date]") {
    Capture cap;
    app::ConsoleObserver observer(cap.io, app::OutputPolicy{});
    observer.on_epub_end(left_alone("out/T - Vol. 01.epub", md::ExistingKind::complete));
    observer.on_epub_end(left_alone("out/T - Vol. 01.epub", md::ExistingKind::complete));
    observer.on_epub_end(left_alone("out/T - Vol. 02.epub", md::ExistingKind::foreign));
    observer.finish();
    CHECK(observer.up_to_date() == 2);
    CHECK(cap.err.str() ==
          "mangadex-dl: 2 files are already up to date; pass --overwrite to download them again\n");
}
