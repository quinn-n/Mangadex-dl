// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/download_update_cli_test.cpp: re-runs of `download` over an existing DIR, end to end
// through run(args, Io&) (skip-existing plan T13a). Each test first downloads the `clean` fixture
// into a fresh DIR, then runs again against a variant of the same title
// (tests/support/mdx_fixture.hpp) and asserts the exact exit code, stdout, stderr, the --json
// report and the bytes on disk. Covers the spec's (docs/design/skip-existing-spec.md) acceptance
// criteria 7-11 and 15, gate decision 5's three [cli] rows, idempotence, and the `chapters` and
// `single` layouts. The in-process share of criterion 20 (progress while updating, the note never
// colored, `warning:` yellow and plain under NO_COLOR) is asserted exactly in
// download_existing_cli_test.cpp (T13b).
//
// The variants that serve only some chapters (`clean-grown-reuse-only`, `clean-regrouped`,
// `clean-regrouped-grown`) make the reuse observable: fetching a chapter they do not serve is a
// 404 and fails the file with exit 6, so exit 0 proves the held pages were copied.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec).

#include "support/download_cli_harness.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
using md::test::Captured;
using md::test::download_cli::concat;
using md::test::download_cli::lines_of;
using md::test::download_cli::list_dir;
using md::test::download_cli::parse_document;
using md::test::download_cli::path_lines;
using md::test::download_cli::read_file;
using md::test::download_cli::run_args;
using md::test::download_cli::snapshot;
using md::test::download_cli::Tree;
using Json = nlohmann::json;
using Snapshot = std::vector<std::pair<std::string, std::string>>;

constexpr std::string_view vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view single_book = "Clean Title.epub";
constexpr std::string_view ch1_file = "Clean Title - Vol. 01 Ch. 001.epub";
constexpr std::string_view ch2_file = "Clean Title - Vol. 01 Ch. 002.epub";
constexpr std::string_view ch3_file = "Clean Title - Vol. 02 Ch. 003.epub";
constexpr std::string_view ch4_file = "Clean Title - Vol. 02 Ch. 004.epub";

/// The closing note for exactly one up-to-date file (spec "Report and output", stderr).
constexpr std::string_view note_one =
    "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n";

/// The closing note for `count` (>= 2) up-to-date files.
[[nodiscard]] std::string note_many(std::size_t count) {
    return std::format("mangadex-dl: {} files are already up to date; pass --overwrite to "
                       "download them again\n",
                       count);
}

/// The clean title's chapter ids for the fixture numbers `numbers` (1..4 group A, 101..104 B).
[[nodiscard]] Json ids(std::initializer_list<std::uint64_t> numbers) {
    Json out = Json::array();
    for (const std::uint64_t n : numbers) {
        out.push_back(mdx::clean_chapter_id(n));
    }
    return out;
}

/// The `id` of every entry of `epub.chapters`, in order.
[[nodiscard]] Json chapter_ids(const Json& epub) {
    Json out = Json::array();
    for (const Json& chapter : epub.at("chapters")) {
        out.push_back(chapter.at("id"));
    }
    return out;
}

/// A `skipped_chapters` entry for a duplicate release `id` that lost to `kept`.
[[nodiscard]] Json duplicate(std::uint64_t id, std::string_view volume, std::string_view chapter,
                             std::uint64_t kept) {
    return Json{{"id", mdx::clean_chapter_id(id)},
                {"volume", volume},
                {"chapter", chapter},
                {"reason", "duplicate"},
                {"kept", mdx::clean_chapter_id(kept)}};
}

/// `names` as owned strings, the shape list_dir() returns.
[[nodiscard]] std::vector<std::string> names_of(std::initializer_list<std::string_view> names) {
    std::vector<std::string> out{};
    out.reserve(names.size());
    for (const std::string_view name : names) {
        out.emplace_back(name);
    }
    return out;
}

/// `tree`'s first download into the empty `out`: exit 0, nothing on stderr. The files written.
[[nodiscard]] Snapshot seed(const Tree& tree, const std::filesystem::path& out,
                            std::initializer_list<std::string> options = {}) {
    const Captured r = run_args(tree.download(out, options));
    INFO("seed stderr: " << r.err);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.err.empty());
    Snapshot files = snapshot(out);
    REQUIRE(!files.empty());
    return files;
}

/// The bytes of OEBPS/content.opf in the EPUB at `path`.
[[nodiscard]] std::string opf_of(const std::filesystem::path& path) {
    const auto archive = zip::read_archive_file(path);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    const zip::Entry* const opf = archive->find("OEBPS/content.opf");
    REQUIRE(opf != nullptr);
    return opf->text();
}

// ---- criterion 7: nothing to do ----------------------------------------------------------------

TEST_CASE("download update cli: a re-run over complete files prints only the note",
          "[cli][download][update]") {
    const Tree clean{mdx::clean_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out);

    const Captured again = run_args(clean.download(out));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == note_many(2));
    CHECK(snapshot(out) == first);

    const Captured json = run_args(concat({"--json"}, clean.download(out)));
    CHECK(json.exit_code == 0);
    CHECK(json.err == note_many(2)); // the note goes to stderr, never into the document
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);
    struct Expected {
        std::string_view name{};
        Json chapters{};
        std::size_t pages = 0; ///< the pages the file holds
    };
    const std::vector<Expected> expected{
        {.name = vol1, .chapters = ids({1, 2}), .pages = 5},
        {.name = vol2, .chapters = ids({3}), .pages = 2},
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const Json& epub = epubs.at(i);
        INFO("epub " << expected[i].name);
        CHECK(epub.at("path") == (out / expected[i].name).string());
        CHECK(epub.at("status") == "exists");
        CHECK(epub.at("existing") == "complete");
        CHECK(epub.at("replaced") == false);
        CHECK(epub.at("new_chapters") == Json::array());
        CHECK(epub.at("reused_chapters") == Json::array());
        CHECK(epub.at("pages") == expected[i].pages);
        CHECK(epub.at("error").is_null());
        CHECK(chapter_ids(epub) == expected[i].chapters);
    }
    CHECK(doc.at("skipped_chapters") == Json::array());
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 0}, {"exists", 2}, {"failed", 0}});
    CHECK(snapshot(out) == first);
}

TEST_CASE("download update cli: -v names each up-to-date file with its chapter count",
          "[cli][download][update][verbose]") {
    const Tree clean{mdx::clean_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out);

    const Captured r = run_args(concat({"-v"}, clean.download(out)));
    CHECK(r.exit_code == 0);
    CHECK(r.out.empty());
    const std::vector<std::string> expected{
        "mangadex-dl: planned 2 files; 0 chapters skipped",
        std::format("mangadex-dl: {}: up to date (2 chapters)", (out / vol1).string()),
        std::format("mangadex-dl: {}: up to date (1 chapter)", (out / vol2).string()),
        "mangadex-dl: 2 files are already up to date; pass --overwrite to download them again",
    };
    CHECK(lines_of(r.err) == expected);
    CHECK(snapshot(out) == first);
}

TEST_CASE("download update cli: one up-to-date file gets the singular note",
          "[cli][download][update][layout]") {
    const Tree clean{mdx::clean_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out, {"--layout", "single"});
    REQUIRE(list_dir(out) == names_of({single_book}));

    const Captured r = run_args(clean.download(out, {"--layout", "single"}));
    CHECK(r.exit_code == 0);
    CHECK(r.out.empty());
    CHECK(r.err == note_one);
    CHECK(snapshot(out) == first);
}

// ---- criteria 8 and 9: reuse and byte identity -------------------------------------------------

TEST_CASE("download update cli: a grown volume is rebuilt from its held pages plus the new chapter",
          "[cli][download][update][reuse]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out);
    const std::string old_vol1 = read_file(out / vol1);

    // Only ch 4 is served (no cover image either): exit 0 means ch 3 and the cover were copied.
    const Captured r = run_args(reuse_only.download(out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {vol2}));
    CHECK(r.err == note_one);                       // Vol. 01 is up to date
    CHECK(list_dir(out) == names_of({vol1, vol2})); // no temporary left behind
    CHECK(read_file(out / vol1) == old_vol1);
    CHECK(read_file(out / vol2) != first.at(1).second);
}

TEST_CASE("download update cli: --json reports the update with its new and reused chapters",
          "[cli][download][update][reuse][json]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const std::filesystem::path out = clean.path("out");
    static_cast<void>(seed(clean, out));
    const std::string old_vol1 = read_file(out / vol1);

    const Captured r = run_args(concat({"--json"}, reuse_only.download(out)));
    CHECK(r.exit_code == 0);
    CHECK(r.err == note_one);
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);

    const Json& first = epubs.at(0);
    CHECK(first.at("status") == "exists");
    CHECK(first.at("existing") == "complete");
    CHECK(first.at("replaced") == false);

    const Json& updated = epubs.at(1);
    CHECK(updated.at("path") == (out / vol2).string());
    CHECK(updated.at("status") == "written");
    CHECK(updated.at("existing") == "incomplete");
    CHECK(updated.at("replaced") == true);
    CHECK(updated.at("new_chapters") == ids({mdx::clean_ch4_n}));
    CHECK(updated.at("reused_chapters") == ids({3}));
    CHECK(updated.at("pages") == 4);
    CHECK(updated.at("bytes") == std::filesystem::file_size(out / vol2));
    CHECK(updated.at("error").is_null());
    CHECK(chapter_ids(updated) == ids({3, mdx::clean_ch4_n}));

    CHECK(doc.at("skipped_chapters") == Json::array());
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 1}, {"exists", 1}, {"failed", 0}});
    CHECK(read_file(out / vol1) == old_vol1);
}

TEST_CASE("download update cli: an updated volume is byte-identical to a fresh download at -j1 "
          "and -j8",
          "[cli][download][update][reuse][jobs]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree grown{mdx::clean_grown_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    Snapshot fresh_j1{};
    for (const std::string jobs : {"1", "8"}) {
        INFO("--jobs " << jobs);
        const std::filesystem::path updated = clean.path("updated-j" + jobs);
        const std::filesystem::path fresh = clean.path("fresh-j" + jobs);
        static_cast<void>(seed(clean, updated, {"--jobs", jobs}));

        const Captured r = run_args(reuse_only.download(updated, {"--jobs", jobs}));
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(updated, {vol2}));
        CHECK(r.err == note_one);

        const Captured f = run_args(grown.download(fresh, {"--jobs", jobs}));
        REQUIRE(f.exit_code == 0);
        CHECK(f.out == path_lines(fresh, {vol1, vol2}));
        const Snapshot expected = snapshot(fresh);
        REQUIRE(expected.size() == 2);
        CHECK(snapshot(updated) == expected); // Vol. 02 rebuilt, Vol. 01 kept: both as fresh
        if (fresh_j1.empty()) {
            fresh_j1 = expected;
        } else {
            CHECK(expected == fresh_j1);
        }
    }
}

TEST_CASE("download update cli: run, update, run again: the second re-run is up to date",
          "[cli][download][update][idempotent]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const std::filesystem::path out = clean.path("out");
    static_cast<void>(seed(clean, out));
    REQUIRE(run_args(reuse_only.download(out)).exit_code == 0);
    const Snapshot updated = snapshot(out);

    const Captured r = run_args(reuse_only.download(out));
    CHECK(r.exit_code == 0);
    CHECK(r.out.empty());
    CHECK(r.err == note_many(2));
    CHECK(snapshot(out) == updated);
}

// ---- criterion 10 and gate decision 5: another group's releases --------------------------------

TEST_CASE("download update cli: another group's releases of the held chapters rebuild nothing",
          "[cli][download][update][regroup]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree regrouped{mdx::clean_regrouped_fixture()}; // B's ch 1-3, nothing served
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out);

    const Captured r = run_args(regrouped.download(out));
    CHECK(r.exit_code == 0);
    CHECK(r.out.empty());
    CHECK(r.err == note_many(2));
    CHECK(snapshot(out) == first);

    const Captured json = run_args(concat({"--json"}, regrouped.download(out)));
    CHECK(json.exit_code == 0);
    CHECK(json.err == note_many(2));
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    REQUIRE(doc.at("epubs").size() == 2);
    for (const Json& epub : doc.at("epubs")) {
        INFO("epub " << epub.at("path"));
        CHECK(epub.at("status") == "exists");
        CHECK(epub.at("existing") == "complete");
        CHECK(epub.at("new_chapters") == Json::array());
        CHECK(epub.at("reused_chapters") == Json::array());
    }
    // The files hold A's releases, so `chapters` lists them and `kept` names them.
    CHECK(chapter_ids(doc.at("epubs").at(0)) == ids({1, 2}));
    CHECK(chapter_ids(doc.at("epubs").at(1)) == ids({3}));
    const Json skipped = Json::array({duplicate(mdx::bravo_ch1_n, "1", "1", 1),
                                      duplicate(mdx::bravo_ch2_n, "1", "2", 2),
                                      duplicate(mdx::bravo_ch3_n, "2", "3", 3)});
    CHECK(doc.at("skipped_chapters") == skipped);
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 0}, {"exists", 2}, {"failed", 0}});
    CHECK(snapshot(out) == first);
}

TEST_CASE("download update cli: --group naming the other group re-picks and rebuilds both volumes",
          "[cli][download][update][regroup][group]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree served{mdx::clean_regrouped_served_fixture()}; // B's ch 1-3, all served
    const std::string bravo{mdx::bravo_group_name};
    const std::filesystem::path out = clean.path("out");
    static_cast<void>(seed(clean, out));

    const Captured r = run_args(concat({"-v"}, served.download(out, {"--group", bravo})));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {vol1, vol2}));
    const std::string p1 = (out / vol1).string();
    const std::string p2 = (out / vol2).string();
    const std::vector<std::string> expected_err{
        "mangadex-dl: planned 2 files; 0 chapters skipped",
        std::format("mangadex-dl: {}: Vol. 1 Ch. 1: replacing the release in the file (Clean "
                    "Scans) with the release by Bravo Scans (--group)",
                    p1),
        std::format("mangadex-dl: {}: Vol. 1 Ch. 2: replacing the release in the file (Clean "
                    "Scans) with the release by Bravo Scans (--group)",
                    p1),
        std::format("mangadex-dl: updating {}: downloading 2 chapters (Ch. 1, Ch. 2), reusing 0",
                    p1),
        std::format("mangadex-dl: wrote {} (5 pages, {} bytes, replaced the existing file)", p1,
                    std::filesystem::file_size(out / vol1)),
        std::format("mangadex-dl: {}: Vol. 2 Ch. 3: replacing the release in the file (Clean "
                    "Scans) with the release by Bravo Scans (--group)",
                    p2),
        std::format("mangadex-dl: updating {}: downloading 1 chapter (Ch. 3), reusing 0", p2),
        std::format("mangadex-dl: wrote {} (2 pages, {} bytes, replaced the existing file)", p2,
                    std::filesystem::file_size(out / vol2)),
    };
    CHECK(lines_of(r.err) == expected_err);

    // Byte-identical to a fresh download of B's releases, with or without --group.
    const std::filesystem::path fresh = clean.path("fresh");
    REQUIRE(run_args(served.download(fresh, {"--group", bravo})).exit_code == 0);
    const Snapshot expected = snapshot(fresh);
    REQUIRE(expected.size() == 2);
    CHECK(snapshot(out) == expected);
    const std::filesystem::path plain = clean.path("fresh-without-group");
    REQUIRE(run_args(served.download(plain)).exit_code == 0);
    CHECK(snapshot(plain) == expected);

    // Idempotent: the files now hold B's releases, which --group still ranks first.
    const Captured again = run_args(served.download(out, {"--group", bravo}));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == note_many(2));
    CHECK(snapshot(out) == expected);
}

TEST_CASE("download update cli: --json lists the re-picked releases as new chapters",
          "[cli][download][update][regroup][group][json]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree served{mdx::clean_regrouped_served_fixture()};
    const std::filesystem::path out = clean.path("out");
    static_cast<void>(seed(clean, out));

    const Captured r = run_args(
        concat({"--json"}, served.download(out, {"--group", std::string{mdx::bravo_group_name}})));
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);
    struct Expected {
        std::string_view name{};
        Json fresh{};
        std::size_t pages = 0;
    };
    const std::vector<Expected> expected{
        {.name = vol1, .fresh = ids({mdx::bravo_ch1_n, mdx::bravo_ch2_n}), .pages = 5},
        {.name = vol2, .fresh = ids({mdx::bravo_ch3_n}), .pages = 2},
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const Json& epub = epubs.at(i);
        INFO("epub " << expected[i].name);
        CHECK(epub.at("path") == (out / expected[i].name).string());
        CHECK(epub.at("status") == "written");
        CHECK(epub.at("existing") == "incomplete");
        CHECK(epub.at("replaced") == true);
        CHECK(epub.at("new_chapters") == expected[i].fresh);
        CHECK(epub.at("reused_chapters") == Json::array());
        CHECK(epub.at("pages") == expected[i].pages);
        CHECK(chapter_ids(epub) == expected[i].fresh);
    }
    // A's releases are not in the feed and no longer in the files: nothing to reconcile.
    CHECK(doc.at("skipped_chapters") == Json::array());
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 2}, {"exists", 0}, {"failed", 0}});
}

TEST_CASE("download update cli: without --group, or with one matching neither group, nothing "
          "is re-picked",
          "[cli][download][update][regroup][group]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree served{mdx::clean_regrouped_served_fixture()};
    struct Variant {
        std::string_view name{};
        std::vector<std::string> options{};
    };
    const std::vector<Variant> variants{
        {.name = "no --group", .options = {}},
        {.name = "--group Charlie Scans",
         .options = {"--group", std::string{mdx::unmatched_group_name}}},
    };
    for (std::size_t i = 0; i < variants.size(); ++i) {
        const Variant& v = variants[i];
        INFO("variant: " << v.name);
        const std::filesystem::path out = clean.path(std::format("out-{}", i));
        const Snapshot first = seed(clean, out);
        // `[--group X] M DIR`: one of the accepted argv shapes.
        std::vector<std::string> args{"-v", "download", "--api-base", served.api_base()};
        args.insert(args.end(), v.options.begin(), v.options.end());
        args.emplace_back(mdx::clean_manga_id);
        args.push_back(out.string());
        const Captured r = run_args(args);
        CHECK(r.exit_code == 0);
        CHECK(r.out.empty());
        const std::vector<std::string> err = lines_of(r.err);
        INFO("stderr: " << r.err);
        REQUIRE(!err.empty());
        CHECK(err.back() ==
              "mangadex-dl: 2 files are already up to date; pass --overwrite to download them "
              "again");
        CHECK(!r.err.contains("replacing the release"));
        CHECK(r.err.contains(std::format("mangadex-dl: {}: Vol. 1 Ch. 1: kept the release "
                                         "already in the file ({}) instead of {}\n",
                                         (out / vol1).string(), mdx::clean_chapter_id(1),
                                         mdx::clean_chapter_id(mdx::bravo_ch1_n))));
        CHECK(snapshot(out) == first);
    }
}

// ---- criterion 11: a held release kept next to a new one ---------------------------------------

TEST_CASE("download update cli: a regrouped, grown volume keeps the held releases and adds the new "
          "one",
          "[cli][download][update][regroup][reuse]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree regrouped_grown{mdx::clean_regrouped_grown_fixture()}; // only B ch 2.5 served
    const Tree mixed{mdx::clean_mixed_reference_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out);
    const std::string old_vol2 = read_file(out / vol2);

    const Captured r = run_args(concat({"-v"}, regrouped_grown.download(out)));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {vol1}));
    const std::string p1 = (out / vol1).string();
    const std::string p2 = (out / vol2).string();
    const std::vector<std::string> expected_err{
        "mangadex-dl: planned 2 files; 0 chapters skipped",
        std::format("mangadex-dl: {}: Vol. 1 Ch. 1: kept the release already in the file ({}) "
                    "instead of {}",
                    p1, mdx::clean_chapter_id(1), mdx::clean_chapter_id(mdx::bravo_ch1_n)),
        std::format("mangadex-dl: {}: Vol. 1 Ch. 2: kept the release already in the file ({}) "
                    "instead of {}",
                    p1, mdx::clean_chapter_id(2), mdx::clean_chapter_id(mdx::bravo_ch2_n)),
        std::format("mangadex-dl: updating {}: downloading 1 chapter (Ch. 2.5), reusing 2", p1),
        std::format("mangadex-dl: wrote {} (7 pages, {} bytes, replaced the existing file)", p1,
                    std::filesystem::file_size(out / vol1)),
        std::format("mangadex-dl: {}: Vol. 2 Ch. 3: kept the release already in the file ({}) "
                    "instead of {}",
                    p2, mdx::clean_chapter_id(3), mdx::clean_chapter_id(mdx::bravo_ch3_n)),
        std::format("mangadex-dl: {}: up to date (1 chapter)", p2),
        "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again",
    };
    CHECK(lines_of(r.err) == expected_err);
    CHECK(list_dir(out) == names_of({vol1, vol2}));
    CHECK(read_file(out / vol2) == old_vol2); // Vol. 02 is complete: untouched

    // Vol. 01 is byte-identical to a fresh download of A's ch 1-2 plus B's ch 2.5.
    const std::filesystem::path reference = clean.path("reference");
    REQUIRE(run_args(mixed.download(reference)).exit_code == 0);
    CHECK(read_file(out / vol1) == read_file(reference / vol1));
    CHECK(read_file(out / vol1) != first.at(0).second);

    // Contributors come from the final chapter list, in reading order: A, then B.
    const std::string opf = opf_of(out / vol1);
    CHECK(opf.contains("    <dc:contributor id=\"contributor-1\">Clean Scans</dc:contributor>\n"
                       "    <dc:contributor id=\"contributor-2\">Bravo Scans</dc:contributor>\n"));

    // Idempotent: both files now hold every chapter the plan wants.
    const Snapshot updated = snapshot(out);
    const Captured again = run_args(regrouped_grown.download(out));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == note_many(2));
    CHECK(snapshot(out) == updated);
}

TEST_CASE("download update cli: --json for a regrouped, grown volume reconciles skipped_chapters",
          "[cli][download][update][regroup][json]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree regrouped_grown{mdx::clean_regrouped_grown_fixture()};
    const std::filesystem::path out = clean.path("out");
    static_cast<void>(seed(clean, out));

    const Captured r = run_args(concat({"--json"}, regrouped_grown.download(out)));
    CHECK(r.exit_code == 0);
    CHECK(r.err == note_one);
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);

    const Json& updated = epubs.at(0);
    CHECK(updated.at("path") == (out / vol1).string());
    CHECK(updated.at("status") == "written");
    CHECK(updated.at("existing") == "incomplete");
    CHECK(updated.at("replaced") == true);
    CHECK(updated.at("new_chapters") == ids({mdx::bravo_ch2_5_n}));
    CHECK(updated.at("reused_chapters") == ids({1, 2}));
    CHECK(updated.at("pages") == 7);
    // The file holds A's ch 1-2 (the held releases, kept) and B's ch 2.5, in reading order.
    CHECK(chapter_ids(updated) == ids({1, 2, mdx::bravo_ch2_5_n}));
    const Json& chapters = updated.at("chapters");
    REQUIRE(chapters.size() == 3);
    CHECK(chapters.at(0).at("groups") ==
          Json::array({{{"id", mdx::clean_group_a_id()}, {"name", "Clean Scans"}}}));
    CHECK(chapters.at(1).at("title") == "Middles"); // metadata of the held release
    CHECK(chapters.at(2).at("groups") ==
          Json::array({{{"id", mdx::bravo_group_id()}, {"name", "Bravo Scans"}}}));

    const Json& complete = epubs.at(1);
    CHECK(complete.at("status") == "exists");
    CHECK(complete.at("existing") == "complete");
    CHECK(chapter_ids(complete) == ids({3}));

    // B's ch 1-3 lost to the releases the files hold (ch 3 via the complete Vol. 02).
    const Json skipped = Json::array({duplicate(mdx::bravo_ch1_n, "1", "1", 1),
                                      duplicate(mdx::bravo_ch2_n, "1", "2", 2),
                                      duplicate(mdx::bravo_ch3_n, "2", "3", 3)});
    CHECK(doc.at("skipped_chapters") == skipped);
    CHECK(doc.at("summary") == Json{{"planned", 0}, {"written", 1}, {"exists", 1}, {"failed", 0}});
}

// ---- criterion 15: dry run ---------------------------------------------------------------------

TEST_CASE("download update cli: a dry run names the file it would update and modifies nothing",
          "[cli][download][update][dry-run]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree grown{mdx::clean_grown_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out);

    const Captured r = run_args(concat({"-v"}, grown.download(out, {"--dry-run"})));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {vol2}));
    const std::vector<std::string> expected_err{
        "mangadex-dl: planned 2 files; 0 chapters skipped",
        std::format("mangadex-dl: {}: up to date (2 chapters)", (out / vol1).string()),
        std::format("mangadex-dl: would update {} (download 1 chapter: Ch. 4; reuse 1)",
                    (out / vol2).string()),
        "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again",
    };
    CHECK(lines_of(r.err) == expected_err);
    CHECK(snapshot(out) == first);

    const Captured json = run_args(concat({"--json"}, grown.download(out, {"--dry-run"})));
    CHECK(json.exit_code == 0);
    CHECK(json.err == note_one);
    const Json doc = parse_document(json.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("dry_run") == true);
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 2);
    CHECK(epubs.at(0).at("status") == "exists");
    CHECK(epubs.at(0).at("existing") == "complete");
    const Json& planned = epubs.at(1);
    CHECK(planned.at("path") == (out / vol2).string());
    CHECK(planned.at("status") == "planned");
    CHECK(planned.at("existing") == "incomplete");
    CHECK(planned.at("replaced") == true);
    CHECK(planned.at("new_chapters") == ids({mdx::clean_ch4_n}));
    CHECK(planned.at("reused_chapters") == ids({3}));
    CHECK(planned.at("pages") == 4);
    CHECK(planned.at("bytes") == 0);
    CHECK(chapter_ids(planned) == ids({3, mdx::clean_ch4_n}));
    CHECK(doc.at("summary") == Json{{"planned", 1}, {"written", 0}, {"exists", 1}, {"failed", 0}});
    CHECK(snapshot(out) == first);
}

// ---- layouts -----------------------------------------------------------------------------------

TEST_CASE("download update cli: --layout single rebuilds the one book from held pages",
          "[cli][download][update][reuse][layout]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree grown{mdx::clean_grown_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const std::filesystem::path out = clean.path("out");
    static_cast<void>(seed(clean, out, {"--layout", "single"}));

    const Captured r =
        run_args(concat({"--json"}, reuse_only.download(out, {"--layout", "single"})));
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    REQUIRE(doc.at("epubs").size() == 1);
    const Json& epub = doc.at("epubs").at(0);
    CHECK(epub.at("path") == (out / single_book).string());
    CHECK(epub.at("kind") == "single");
    CHECK(epub.at("status") == "written");
    CHECK(epub.at("existing") == "incomplete");
    CHECK(epub.at("replaced") == true);
    CHECK(epub.at("new_chapters") == ids({mdx::clean_ch4_n}));
    CHECK(epub.at("reused_chapters") == ids({1, 2, 3}));
    CHECK(epub.at("pages") == 9);
    CHECK(list_dir(out) == names_of({single_book}));

    const std::filesystem::path fresh = clean.path("fresh");
    REQUIRE(run_args(grown.download(fresh, {"--layout", "single"})).exit_code == 0);
    CHECK(snapshot(out) == snapshot(fresh));

    const Captured again = run_args(reuse_only.download(out, {"--layout", "single"}));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == note_one);
}

TEST_CASE("download update cli: --layout chapters adds the new chapter's file and keeps the rest",
          "[cli][download][update][layout]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree grown{mdx::clean_grown_fixture()};
    const Tree reuse_only{mdx::clean_grown_reuse_only_fixture()};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out, {"--layout", "chapters"});
    REQUIRE(list_dir(out) == names_of({ch1_file, ch2_file, ch3_file}));

    const Captured r = run_args(reuse_only.download(out, {"--layout", "chapters"}));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {ch4_file}));
    CHECK(r.err == note_many(3));
    CHECK(list_dir(out) == names_of({ch1_file, ch2_file, ch3_file, ch4_file}));
    for (const auto& [name, bytes] : first) {
        INFO("file " << name);
        CHECK(read_file(out / name) == bytes);
    }

    const std::filesystem::path fresh = clean.path("fresh");
    REQUIRE(run_args(grown.download(fresh, {"--layout", "chapters"})).exit_code == 0);
    CHECK(snapshot(out) == snapshot(fresh));

    const Captured again = run_args(reuse_only.download(out, {"--layout", "chapters"}));
    CHECK(again.exit_code == 0);
    CHECK(again.out.empty());
    CHECK(again.err == note_many(4));
}

TEST_CASE("download update cli: --layout chapters with --group re-picks every chapter file",
          "[cli][download][update][regroup][group][layout]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree served{mdx::clean_regrouped_served_fixture()};
    const std::string bravo{mdx::bravo_group_name};
    const std::filesystem::path out = clean.path("out");
    const Snapshot first = seed(clean, out, {"--layout", "chapters"});

    // Without --group the held releases stay.
    const Captured kept = run_args(served.download(out, {"--layout", "chapters"}));
    CHECK(kept.exit_code == 0);
    CHECK(kept.out.empty());
    CHECK(kept.err == note_many(3));
    CHECK(snapshot(out) == first);

    const Captured r = run_args(
        concat({"--json"}, served.download(out, {"--layout", "chapters", "--group", bravo})));
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    const Json& epubs = doc.at("epubs");
    REQUIRE(epubs.size() == 3);
    const std::vector<std::uint64_t> bravo_ids{mdx::bravo_ch1_n, mdx::bravo_ch2_n,
                                               mdx::bravo_ch3_n};
    for (std::size_t i = 0; i < epubs.size(); ++i) {
        const Json& epub = epubs.at(i);
        INFO("epub " << epub.at("path"));
        CHECK(epub.at("kind") == "chapter");
        CHECK(epub.at("status") == "written");
        CHECK(epub.at("existing") == "incomplete");
        CHECK(epub.at("replaced") == true);
        CHECK(epub.at("new_chapters") == ids({bravo_ids[i]}));
        CHECK(epub.at("reused_chapters") == Json::array());
    }
    CHECK(list_dir(out) == names_of({ch1_file, ch2_file, ch3_file}));

    const std::filesystem::path fresh = clean.path("fresh");
    REQUIRE(
        run_args(served.download(fresh, {"--layout", "chapters", "--group", bravo})).exit_code ==
        0);
    CHECK(snapshot(out) == snapshot(fresh));
}

} // namespace
