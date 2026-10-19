// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/download/download_update_test.cpp: run_download over a DIR that already holds files
// (skip-existing plan T10). Several fixture trees (tests/support/mdx_fixture.hpp, the `clean`
// variants) are written side by side and each is served by its own fixture transport, wrapped in
// an overlay with a per-request hook; every run writes into the same output DIR, so a later run
// sees what an earlier one wrote. Covers: up-to-date files; updates that reuse held pages, byte
// for byte equal to a fresh download at -j1 and -j8; regroups with and without --group; damaged
// held pages; the existing file changing between pre-pass and copy; publish races; interrupts in
// the pre-pass, the copy and the download on both temporary-file paths; foreign files; plan
// errors; dry runs; idempotence; the chapters and single layouts; the observer's thread.

#include "fs/output_dir_testing.hpp" // core-private seam (src/core is on the test include path)
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_forge.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): SIGINT is POSIX <signal.h>
#include <signal.h>
#include <sys/stat.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
namespace forge = mangadex_dl::test::forge;
using Catch::Matchers::ContainsSubstring;

// ---- test doubles ------------------------------------------------------------------------------

/// The fixture transport, with a hook that runs on every fetch (on the fetching thread, before
/// the request is answered) and a record of every URL asked for. Thread-safe.
class HookedTransport final : public md::Transport {
public:
    explicit HookedTransport(std::unique_ptr<md::Transport> inner) : inner_(std::move(inner)) {}

    void set_on_fetch(std::function<void(const md::HttpRequest&)> hook) {
        const std::scoped_lock lock{mutex_};
        on_fetch_ = std::move(hook);
    }

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& request) override {
        std::function<void(const md::HttpRequest&)> hook;
        {
            const std::scoped_lock lock{mutex_};
            urls_.push_back(request.url);
            hook = on_fetch_;
        }
        if (hook) {
            hook(request);
        }
        if (request.cancel.stop_requested()) {
            return std::unexpected(
                md::TransportError{.kind = md::TransportErrc::cancelled, .message = "cancelled"});
        }
        return inner_->fetch(request);
    }

    /// Requests whose URL contains `part`.
    [[nodiscard]] std::size_t count_containing(std::string_view part) const {
        const std::scoped_lock lock{mutex_};
        return static_cast<std::size_t>(std::ranges::count_if(
            urls_, [part](const std::string& url) { return url.contains(part); }));
    }

private:
    std::unique_ptr<md::Transport> inner_{};
    mutable std::mutex mutex_{};
    std::function<void(const md::HttpRequest&)> on_fetch_{}; ///< guarded
    std::vector<std::string> urls_{};                        ///< guarded
};

enum class Event {
    pages,
    warning,
    verbose,
};

/// Records every observer call and the thread it came from, and runs an optional hook after a
/// pages, warning or verbose call (on the calling thread, outside the lock). A pages event's
/// message is "<done>/<total>".
class RecordingObserver final : public md::DownloadObserver {
public:
    RecordingObserver() = default;

    void set_hook(std::function<void(Event, std::string_view)> hook) { hook_ = std::move(hook); }

    void on_plan(const md::Plan& /*plan*/) override {
        const std::scoped_lock lock{mutex_};
        record();
    }
    void on_epub_begin(const md::PlannedEpub& epub, std::size_t index, std::size_t total) override {
        const std::scoped_lock lock{mutex_};
        record();
        begins_.push_back(epub.file_name);
        CHECK(index < total);
    }
    void on_pages(std::size_t done, std::size_t total) override {
        {
            const std::scoped_lock lock{mutex_};
            record();
            CHECK(done <= total);
        }
        fire(Event::pages, std::format("{}/{}", done, total)); // e.g. "4/4"
    }
    void on_epub_end(const md::EpubOutcome& outcome) override {
        const std::scoped_lock lock{mutex_};
        record();
        ends_.push_back(outcome);
    }
    void on_warning(std::string_view message) override {
        {
            const std::scoped_lock lock{mutex_};
            record();
            warnings_.emplace_back(message);
        }
        fire(Event::warning, message);
    }
    void on_verbose(int level, std::string_view message) override {
        {
            const std::scoped_lock lock{mutex_};
            record();
            verbose_.emplace_back(level, std::string{message});
        }
        fire(Event::verbose, message);
    }

    [[nodiscard]] std::vector<std::thread::id> threads() const {
        const std::scoped_lock lock{mutex_};
        return threads_;
    }
    [[nodiscard]] std::vector<std::string> begins() const {
        const std::scoped_lock lock{mutex_};
        return begins_;
    }
    [[nodiscard]] std::vector<md::EpubOutcome> ends() const {
        const std::scoped_lock lock{mutex_};
        return ends_;
    }
    [[nodiscard]] std::vector<std::string> warnings() const {
        const std::scoped_lock lock{mutex_};
        return warnings_;
    }
    /// The -v (level 1) lines.
    [[nodiscard]] std::vector<std::string> verbose1() const {
        const std::scoped_lock lock{mutex_};
        std::vector<std::string> out{};
        for (const auto& [level, text] : verbose_) {
            if (level == 1) {
                out.push_back(text);
            }
        }
        return out;
    }

private:
    void record() { threads_.push_back(std::this_thread::get_id()); }
    void fire(Event event, std::string_view message) const {
        if (hook_) {
            hook_(event, message);
        }
    }

    std::function<void(Event, std::string_view)> hook_{}; ///< set before the run only
    mutable std::mutex mutex_{};
    std::vector<std::thread::id> threads_{};
    std::vector<std::string> begins_{};
    std::vector<md::EpubOutcome> ends_{};
    std::vector<std::string> warnings_{};
    std::vector<std::pair<int, std::string>> verbose_{};
};

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

// ---- the harness -------------------------------------------------------------------------------

/// Fixture trees side by side in one temporary directory, each with its own transport, plus an
/// instant clock. Output directories go next to the trees.
class Trees {
public:
    Trees() : tmp_(make_temp_dir()) {}

    /// Writes `manga` as the tree `name`.
    void add(const std::string& name, mdx::FixtureManga manga) {
        const std::filesystem::path root = tmp_.path() / "trees" / name;
        const auto written = mdx::write_fixture_tree(root, manga);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        auto inner = md::make_fixture_transport(root);
        REQUIRE(inner.has_value());
        trees_.insert_or_assign(
            name, Tree{.manga = std::move(manga),
                       .root = root,
                       .transport = std::make_unique<HookedTransport>(std::move(*inner))});
    }

    [[nodiscard]] HookedTransport& transport(std::string_view name) {
        return *tree(name).transport;
    }

    /// `<tmp>/<name>`, not created.
    [[nodiscard]] std::filesystem::path out(std::string_view name = "out") const {
        return tmp_.path() / name;
    }

    /// Options for tree `name`'s title, writing into out(dir_name).
    [[nodiscard]] md::DownloadOptions options(std::string_view name,
                                              std::string_view dir_name = "out") const {
        auto id = md::MangaId::parse(tree(name).manga.id);
        REQUIRE(id.has_value());
        auto base = md::ApiBase::parse(base_url(name));
        REQUIRE(base.has_value());
        return md::DownloadOptions{
            .manga = std::move(*id),
            .dir = out(dir_name),
            .api_base = std::move(*base),
            .report = false,
        };
    }

    [[nodiscard]] md::Result<md::RunReport> run(std::string_view name,
                                                const md::DownloadOptions& options,
                                                RecordingObserver& observer,
                                                std::stop_source stop = {}) {
        const md::DownloadServices services{
            .transport = tree(name).transport.get(),
            .clock = clock_.get(),
            .jitter = {},
            .stop = std::move(stop),
            .user_agent = "mangadex-dl/test",
        };
        return md::run_download(options, services, observer);
    }

    /// run() that must succeed.
    [[nodiscard]] md::RunReport run_ok(std::string_view name, const md::DownloadOptions& options,
                                       RecordingObserver& observer) {
        md::Result<md::RunReport> report = run(name, options, observer);
        INFO((report ? std::string{} : report.error().message));
        REQUIRE(report.has_value());
        return std::move(*report);
    }

    /// run_ok() with a fresh observer.
    md::RunReport run_ok(std::string_view name, const md::DownloadOptions& options) {
        RecordingObserver observer;
        return run_ok(name, options, observer);
    }

    [[nodiscard]] std::string base_url(std::string_view name) const {
        return "file://" + tree(name).root.string();
    }

    /// The URL of page `page` of the chapter with id `chapter_id` in tree `name`.
    [[nodiscard]] std::string page_url(std::string_view name, std::string_view chapter_id,
                                       std::size_t page) const {
        const mdx::FixtureManga& manga = tree(name).manga;
        const auto chapter =
            std::ranges::find_if(manga.chapters, [chapter_id](const mdx::FixtureChapter& c) {
                return c.id == chapter_id;
            });
        REQUIRE(chapter != manga.chapters.end());
        return md::image_url(base_url(name) + "/at-home", md::ImageQuality::data, chapter->hash,
                             mdx::page_file_name(page, chapter->images.at(page)));
    }

private:
    struct Tree {
        mdx::FixtureManga manga{};
        std::filesystem::path root{};
        std::unique_ptr<HookedTransport> transport{};
    };

    [[nodiscard]] const Tree& tree(std::string_view name) const {
        const auto found = trees_.find(name);
        REQUIRE(found != trees_.end());
        return found->second;
    }
    [[nodiscard]] Tree& tree(std::string_view name) {
        const auto found = trees_.find(name);
        REQUIRE(found != trees_.end());
        return found->second;
    }

    [[nodiscard]] static md::test::TempDir make_temp_dir() {
        auto tmp = md::test::TempDir::create("mangadex-dl-update");
        REQUIRE(tmp.has_value());
        return std::move(*tmp);
    }

    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    std::map<std::string, Tree, std::less<>> trees_{};
    std::unique_ptr<md::Clock> clock_{md::make_instant_clock()};
};

// ---- helpers -----------------------------------------------------------------------------------

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";

[[nodiscard]] std::vector<std::string> list_dir(const std::filesystem::path& dir) {
    std::vector<std::string> names{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

[[nodiscard]] std::vector<std::string> both_volumes() {
    return {std::string{clean_vol1}, std::string{clean_vol2}};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    REQUIRE_FALSE(out.fail());
}

[[nodiscard]] std::vector<md::EpubStatus> statuses(const md::RunReport& report) {
    std::vector<md::EpubStatus> out{};
    out.reserve(report.epubs.size());
    for (const md::EpubOutcome& outcome : report.epubs) {
        out.push_back(outcome.status);
    }
    return out;
}

[[nodiscard]] std::vector<md::ExistingKind> existing(const md::RunReport& report) {
    std::vector<md::ExistingKind> out{};
    out.reserve(report.epubs.size());
    for (const md::EpubOutcome& outcome : report.epubs) {
        out.push_back(outcome.existing);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> chapter_ids(const md::PlannedEpub& epub) {
    std::vector<std::string> out{};
    out.reserve(epub.chapters.size());
    for (const md::Chapter& chapter : epub.chapters) {
        out.push_back(chapter.id);
    }
    return out;
}

[[nodiscard]] zip::Archive read_epub(const std::filesystem::path& path) {
    auto archive = zip::read_archive_file(path);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

/// The book manifest of the EPUB at `path`, parsed by the core reader.
[[nodiscard]] md::BookManifest manifest_of(const std::filesystem::path& path) {
    const zip::Archive archive = read_epub(path);
    REQUIRE_FALSE(archive.entries.empty());
    CHECK(archive.entries.back().name == md::manifest_entry_name);
    const zip::Entry* entry = archive.find(md::manifest_entry_name);
    REQUIRE(entry != nullptr);
    const std::string text = entry->text();
    std::expected<md::BookManifest, md::ManifestFault> manifest = md::parse_manifest(text);
    REQUIRE(manifest.has_value());
    return std::move(*manifest);
}

/// Where the last byte of `entry_name`'s data lies in the EPUB at `path`, and its value.
struct BytePosition {
    std::uint64_t offset = 0;
    std::byte value{};
};

[[nodiscard]] BytePosition last_data_byte(const std::filesystem::path& path,
                                          std::string_view entry_name) {
    const zip::Archive archive = read_epub(path);
    const zip::Entry* entry = archive.find(entry_name);
    REQUIRE(entry != nullptr);
    REQUIRE_FALSE(entry->data.empty());
    return BytePosition{.offset = entry->data_offset + entry->data.size() - 1,
                        .value = entry->data.back()};
}

/// Flips every bit of one data byte of `entry_name` in place (same inode, same size): the
/// central directory and the manifest stay valid, the entry's CRC no longer matches.
void damage(const std::filesystem::path& path, std::string_view entry_name) {
    const BytePosition at = last_data_byte(path, entry_name);
    REQUIRE(forge::patch_file_byte(path, at.offset, ~at.value));
}

/// Whether any descriptor of this process refers to the file at `path` (same device and inode),
/// found by walking /proc/self/fd. False when `path` cannot be stat'ed. Calling thread only.
[[nodiscard]] bool process_holds_open(const std::filesystem::path& path) {
    struct stat target{};
    if (::stat(path.c_str(), &target) != 0) {
        return false;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        const std::string name = entry.path().filename().string();
        int fd = -1;
        const char* const first = std::to_address(name.begin());
        const char* const last = std::to_address(name.end());
        const auto [end, parsed] = std::from_chars(first, last, fd);
        if (parsed != std::errc{} || end != last) {
            continue;
        }
        struct stat st{};
        if (::fstat(fd, &st) == 0 && st.st_dev == target.st_dev && st.st_ino == target.st_ino) {
            return true;
        }
    }
    return false;
}

/// The clean title's planned chapter ids.
[[nodiscard]] std::string ch(std::uint64_t n) { return mdx::clean_chapter_id(n); }

/// No temporary or stray file next to the books.
void check_no_temporaries(const std::filesystem::path& dir,
                          const std::vector<std::string>& expected) {
    CHECK(list_dir(dir) == expected);
}

} // namespace

// ---- up to date ---------------------------------------------------------------------------------

TEST_CASE("download update: a re-run of an unchanged title leaves every file alone",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol1 = read_file(t.out() / clean_vol1);
    const std::string vol2 = read_file(t.out() / clean_vol2);
    const std::size_t at_home = t.transport("clean").count_containing("/at-home/");
    const std::size_t uploads = t.transport("clean").count_containing("/uploads/");

    RecordingObserver observer;
    const md::RunReport report = t.run_ok("clean", t.options("clean"), observer);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(existing(report) == std::vector{md::ExistingKind::complete, md::ExistingKind::complete});
    CHECK(md::exit_code_for(report) == 0);
    CHECK(report.epubs[0].pages == 5);
    CHECK(report.epubs[1].pages == 2);
    CHECK(chapter_ids(report.epubs[0].epub) == std::vector{ch(1), ch(2)});
    CHECK(report.epubs[0].new_chapters.empty());
    CHECK(report.epubs[0].reused_chapters.empty());
    CHECK_FALSE(report.epubs[0].replaced);
    CHECK(report.skipped.empty());
    // Nothing was downloaded: no at-home lookup, no page, no cover.
    CHECK(t.transport("clean").count_containing("/at-home/") == at_home);
    CHECK(t.transport("clean").count_containing("/uploads/") == uploads);
    CHECK(observer.begins().empty());
    CHECK(observer.warnings().empty());
    CHECK(observer.verbose1().empty());
    CHECK(observer.ends() == report.epubs);
    CHECK(read_file(t.out() / clean_vol1) == vol1);
    CHECK(read_file(t.out() / clean_vol2) == vol2);
}

TEST_CASE("download update: every new file carries a manifest that describes the planned chapters",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    const md::RunReport report = t.run_ok("clean", t.options("clean"));
    REQUIRE(report.epubs.size() == 2);
    // A planner-produced chapter list went through EpubWriter::finish(cover, chapters), and the
    // manifest reader accepts what it wrote.
    const md::BookManifest vol1 = manifest_of(t.out() / clean_vol1);
    CHECK(vol1.identifier == report.epubs[0].epub.identifier);
    REQUIRE(vol1.chapters.size() == 2);
    CHECK(vol1.chapters[0].id == ch(1));
    CHECK(vol1.chapters[0].images.size() == 3);
    CHECK(vol1.chapters[1].images.size() == 2);
    CHECK(vol1.chapters[0].quality == md::ImageQuality::data);
    REQUIRE(vol1.cover.has_value());
    const md::ManifestCover cover = vol1.cover.value_or(md::ManifestCover{});
    CHECK(cover.file_name == "cover-v1.png");
    CHECK(cover.entry == "OEBPS/images/cover.png");
}

// ---- updates with reuse -------------------------------------------------------------------------

TEST_CASE("download update: a grown volume reuses its held chapter and equals a fresh download",
          "[download][update]") {
    const unsigned jobs = GENERATE(1U, 8U);
    INFO("jobs: " << jobs);
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    md::DownloadOptions first = t.options("clean");
    first.jobs = jobs;
    static_cast<void>(t.run_ok("clean", first));
    const std::string vol1 = read_file(t.out() / clean_vol1);

    // Only ch 4 is served: the update succeeds only by copying ch 3 and the cover.
    md::DownloadOptions update = t.options("reuse-only");
    update.jobs = jobs;
    RecordingObserver observer;
    const md::RunReport report = t.run_ok("reuse-only", update, observer);
    CHECK(md::exit_code_for(report) == 0);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::written});
    CHECK(existing(report) ==
          std::vector{md::ExistingKind::complete, md::ExistingKind::incomplete});
    const md::EpubOutcome& vol2 = report.epubs[1];
    CHECK(vol2.replaced);
    CHECK(vol2.new_chapters == std::vector{ch(4)});
    CHECK(vol2.reused_chapters == std::vector{ch(3)});
    CHECK(vol2.pages == 4);
    CHECK(vol2.bytes == std::filesystem::file_size(t.out() / clean_vol2));
    CHECK(chapter_ids(vol2.epub) == std::vector{ch(3), ch(4)});
    CHECK(vol2.error.empty());
    CHECK(read_file(t.out() / clean_vol1) == vol1);
    check_no_temporaries(t.out(), both_volumes());
    CHECK(t.transport("reuse-only").count_containing("/uploads/") == 0);
    CHECK(observer.begins() == std::vector{std::string{clean_vol2}});
    CHECK(observer.warnings().empty());
    CHECK(observer.verbose1() ==
          std::vector{std::format("updating {}: 1 new chapter, 1 reused (Ch. 4)",
                                  (t.out() / clean_vol2).string())});

    const md::BookManifest manifest = manifest_of(t.out() / clean_vol2);
    REQUIRE(manifest.chapters.size() == 2);
    CHECK(manifest.chapters[0].id == ch(3));
    CHECK(manifest.chapters[1].id == ch(4));

    // A fresh download of the grown title writes exactly the same bytes.
    md::DownloadOptions fresh = t.options("grown", "fresh");
    fresh.jobs = jobs;
    static_cast<void>(t.run_ok("grown", fresh));
    CHECK(read_file(t.out("fresh") / clean_vol2) == read_file(t.out() / clean_vol2));
    CHECK(read_file(t.out("fresh") / clean_vol1) == vol1);
}

TEST_CASE("download update: a run, an update, then another run reports everything up to date",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    static_cast<void>(t.run_ok("reuse-only", t.options("reuse-only")));
    const std::string vol2 = read_file(t.out() / clean_vol2);
    for (const std::string_view tree : {"reuse-only", "grown"}) {
        INFO("tree: " << tree);
        const md::RunReport again = t.run_ok(tree, t.options(tree));
        CHECK(statuses(again) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
        CHECK(existing(again) ==
              std::vector{md::ExistingKind::complete, md::ExistingKind::complete});
        CHECK(again.epubs[1].pages == 4);
    }
    CHECK(read_file(t.out() / clean_vol2) == vol2);
}

// ---- regroups -----------------------------------------------------------------------------------

TEST_CASE("download update: another group's releases of held chapters leave the files alone",
          "[download][update][group]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("regrouped", mdx::clean_regrouped_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol1 = read_file(t.out() / clean_vol1);
    const std::string vol2 = read_file(t.out() / clean_vol2);

    md::DownloadOptions options = t.options("regrouped");
    const bool unmatched_group = GENERATE(false, true);
    INFO("--group Charlie Scans: " << unmatched_group);
    if (unmatched_group) {
        options.groups = {std::string{mdx::unmatched_group_name}};
    }
    RecordingObserver observer;
    const md::RunReport report = t.run_ok("regrouped", options, observer);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(existing(report) == std::vector{md::ExistingKind::complete, md::ExistingKind::complete});
    // The files hold A's releases, and say so.
    CHECK(chapter_ids(report.epubs[0].epub) == std::vector{ch(1), ch(2)});
    CHECK(chapter_ids(report.epubs[1].epub) == std::vector{ch(3)});
    const std::vector<md::SkippedChapter> skipped{
        {.id = ch(mdx::bravo_ch1_n),
         .volume = "1",
         .chapter = "1",
         .reason = md::SkipReason::duplicate,
         .kept = ch(1)},
        {.id = ch(mdx::bravo_ch2_n),
         .volume = "1",
         .chapter = "2",
         .reason = md::SkipReason::duplicate,
         .kept = ch(2)},
        {.id = ch(mdx::bravo_ch3_n),
         .volume = "2",
         .chapter = "3",
         .reason = md::SkipReason::duplicate,
         .kept = ch(3)},
    };
    CHECK(report.skipped == skipped);
    const std::string path1 = (t.out() / clean_vol1).string();
    CHECK(observer.verbose1() ==
          std::vector<std::string>{
              std::format("{}: Vol. 1 Ch. 1: kept the release already in the file ({}) instead "
                          "of {}",
                          path1, ch(1), ch(mdx::bravo_ch1_n)),
              std::format("{}: Vol. 1 Ch. 2: kept the release already in the file ({}) instead "
                          "of {}",
                          path1, ch(2), ch(mdx::bravo_ch2_n)),
              std::format("{}: Vol. 2 Ch. 3: kept the release already in the file ({}) instead "
                          "of {}",
                          (t.out() / clean_vol2).string(), ch(3), ch(mdx::bravo_ch3_n))});
    CHECK(t.transport("regrouped").count_containing("/at-home/") == 0);
    CHECK(read_file(t.out() / clean_vol1) == vol1);
    CHECK(read_file(t.out() / clean_vol2) == vol2);
}

TEST_CASE("download update: --group re-picks a better-ranked group's releases (decision 5)",
          "[download][update][group]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("served", mdx::clean_regrouped_served_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));

    md::DownloadOptions options = t.options("served");
    options.groups = {std::string{mdx::bravo_group_name}};
    RecordingObserver observer;
    const md::RunReport report = t.run_ok("served", options, observer);
    CHECK(statuses(report) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(existing(report) ==
          std::vector{md::ExistingKind::incomplete, md::ExistingKind::incomplete});
    CHECK(report.epubs[0].replaced);
    CHECK(report.epubs[0].new_chapters == std::vector{ch(mdx::bravo_ch1_n), ch(mdx::bravo_ch2_n)});
    CHECK(report.epubs[0].reused_chapters.empty());
    CHECK(report.epubs[1].new_chapters == std::vector{ch(mdx::bravo_ch3_n)});
    CHECK(report.skipped.empty()); // nothing held is kept
    const std::vector<std::string> lines = observer.verbose1();
    CHECK(std::ranges::count(
              lines, std::format("{}: Vol. 1 Ch. 1: replacing the release in the file (Clean "
                                 "Scans) with Bravo Scans's (--group)",
                                 (t.out() / clean_vol1).string())) == 1);
    CHECK(std::ranges::count_if(lines, [](const std::string& line) {
              return line.contains("replacing the release");
          }) == 3);

    // Byte-identical to a fresh download of B's releases.
    static_cast<void>(t.run_ok("served", t.options("served", "fresh")));
    CHECK(read_file(t.out("fresh") / clean_vol1) == read_file(t.out() / clean_vol1));
    CHECK(read_file(t.out("fresh") / clean_vol2) == read_file(t.out() / clean_vol2));
}

TEST_CASE(
    "download update: a regrouped and grown volume keeps A's releases and adds B's new chapter",
    "[download][update][group]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("regrouped-grown", mdx::clean_regrouped_grown_fixture());
    t.add("mixed", mdx::clean_mixed_reference_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol2 = read_file(t.out() / clean_vol2);

    RecordingObserver observer;
    const md::RunReport report =
        t.run_ok("regrouped-grown", t.options("regrouped-grown"), observer);
    CHECK(md::exit_code_for(report) == 0);
    CHECK(statuses(report) == std::vector{md::EpubStatus::written, md::EpubStatus::exists});
    CHECK(existing(report) ==
          std::vector{md::ExistingKind::incomplete, md::ExistingKind::complete});
    CHECK(report.epubs[0].reused_chapters == std::vector{ch(1), ch(2)});
    CHECK(report.epubs[0].new_chapters == std::vector{ch(mdx::bravo_ch2_5_n)});
    CHECK(chapter_ids(report.epubs[0].epub) == std::vector{ch(1), ch(2), ch(mdx::bravo_ch2_5_n)});
    // skipped_chapters lists B's ch 1-3 as duplicates of the A releases the files hold.
    REQUIRE(report.skipped.size() == 3);
    CHECK(report.skipped[0].id == ch(mdx::bravo_ch1_n));
    CHECK(report.skipped[0].kept == ch(1));
    CHECK(report.skipped[1].id == ch(mdx::bravo_ch2_n));
    CHECK(report.skipped[1].kept == ch(2));
    CHECK(report.skipped[2].id == ch(mdx::bravo_ch3_n));
    CHECK(report.skipped[2].kept == ch(3));
    CHECK(std::ranges::count_if(observer.verbose1(), [](const std::string& line) {
              return line.contains("kept the release already in the file");
          }) == 3);
    CHECK(read_file(t.out() / clean_vol2) == vol2);

    // Contributors: A, then B (reading order).
    const zip::Archive archive = read_epub(t.out() / clean_vol1);
    const zip::Entry* opf = archive.find("OEBPS/content.opf");
    REQUIRE(opf != nullptr);
    const std::string text = opf->text();
    const std::size_t a = text.find(mdx::clean_group_a_name);
    const std::size_t b = text.find(mdx::bravo_group_name);
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    CHECK(a < b);

    // Byte-identical to a fresh download of A's ch 1-2 + B's ch 2.5.
    static_cast<void>(t.run_ok("mixed", t.options("mixed", "fresh")));
    CHECK(read_file(t.out("fresh") / clean_vol1) == read_file(t.out() / clean_vol1));
}

// ---- damage and change --------------------------------------------------------------------------

TEST_CASE("download update: a damaged held page is downloaded again",
          "[download][update][damage]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    damage(t.out() / clean_vol2, "OEBPS/images/p00002.png"); // ch 3, page 2

    RecordingObserver observer;
    const md::RunReport report = t.run_ok("grown", t.options("grown"), observer);
    CHECK(md::exit_code_for(report) == 0);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::written});
    const md::EpubOutcome& vol2 = report.epubs[1];
    CHECK(vol2.existing == md::ExistingKind::incomplete);
    CHECK(vol2.replaced);
    CHECK(vol2.reused_chapters.empty());
    CHECK(vol2.new_chapters == std::vector{ch(3), ch(4)});
    CHECK(observer.warnings() ==
          std::vector{std::format("{}: Ch. 3 in the existing file is damaged; downloading it again",
                                  (t.out() / clean_vol2).string())});
    CHECK(t.transport("grown").count_containing(ch(3)) == 1); // its at-home lookup

    static_cast<void>(t.run_ok("grown", t.options("grown", "fresh")));
    CHECK(read_file(t.out("fresh") / clean_vol2) == read_file(t.out() / clean_vol2));
}

TEST_CASE(
    "download update: a file that changes between the pre-pass and the copy fails and is kept",
    "[download][update][damage]") {
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Trees t;
    t.add("clean", mdx::clean_fixture());
    // Vol 2's cover has a new file name, so it is downloaded between the pre-pass and the copy
    // of ch 3: the hook damages ch 3 in place then.
    mdx::FixtureManga renamed = mdx::clean_grown_fixture();
    renamed.covers.at(1).file = "cover-v2-new.png";
    t.add("renamed", std::move(renamed));
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::filesystem::path vol2 = t.out() / clean_vol2;
    const BytePosition at = last_data_byte(vol2, "OEBPS/images/p00001.png");
    std::string damaged = read_file(vol2);
    damaged.at(at.offset) = static_cast<char>(std::to_integer<unsigned char>(~at.value));
    std::atomic<bool> fired{false};
    std::atomic<bool> patched{false};
    // The cover is fetched on the calling thread; the hook still only records its result.
    t.transport("renamed").set_on_fetch(
        [path = vol2, at, flag = &fired, ok = &patched](const md::HttpRequest& request) {
            if (request.url.contains("cover-v2-new.png") && !flag->exchange(true)) {
                ok->store(forge::patch_file_byte(path, at.offset, ~at.value));
            }
        });

    const md::RunReport report = t.run_ok("renamed", t.options("renamed"));
    CHECK(fired.load());
    CHECK(patched.load());
    CHECK(md::exit_code_for(report) == 6);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::failed});
    const md::EpubOutcome& outcome = report.epubs[1];
    CHECK(outcome.existing == md::ExistingKind::incomplete);
    CHECK(outcome.error == "the existing file changed or could not be read during the update");
    check_no_temporaries(t.out(), both_volumes());
    // The existing file is kept as it now is (with the damage), never replaced.
    CHECK(read_file(vol2) == damaged);
}

TEST_CASE("download update: a held cover that changes between the pre-pass and the copy fails the "
          "file",
          "[download][update][damage]") {
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::filesystem::path vol2 = t.out() / clean_vol2;
    const BytePosition at = last_data_byte(vol2, "OEBPS/images/cover.png");
    std::string damaged = read_file(vol2);
    damaged.at(at.offset) = static_cast<char>(std::to_integer<unsigned char>(~at.value));
    // The "updating" line comes after the pre-pass validated the cover and before the build
    // copies it (calling thread): the hook damages the cover in place then.
    RecordingObserver observer;
    std::atomic<bool> patched{false};
    observer.set_hook([path = vol2, at, ok = &patched](Event event, std::string_view message) {
        if (event == Event::verbose && message.starts_with("updating ") && !ok->load()) {
            ok->store(forge::patch_file_byte(path, at.offset, ~at.value));
        }
    });

    const md::RunReport report = t.run_ok("reuse-only", t.options("reuse-only"), observer);
    CHECK(patched.load());
    CHECK(md::exit_code_for(report) == 6);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::failed});
    CHECK(report.epubs[1].existing == md::ExistingKind::incomplete);
    CHECK(report.epubs[1].error ==
          "the existing file changed or could not be read during the update");
    CHECK(t.transport("reuse-only").count_containing("/at-home/") == 0); // failed before ch 4
    check_no_temporaries(t.out(), both_volumes());
    CHECK(read_file(vol2) == damaged); // kept as it now is, never replaced
}

// ---- publish races ------------------------------------------------------------------------------

TEST_CASE("download update: the target vanishing or being replaced during the update",
          "[download][update][race]") {
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    const bool replace = GENERATE(false, true);
    INFO("replaced by another file: " << replace);
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::filesystem::path vol2 = t.out() / clean_vol2;
    const std::filesystem::path stray = t.out() / "someone-else.tmp";
    std::atomic<bool> fired{false};
    std::atomic<bool> hook_ok{false};
    // Runs on a pool worker while ch 4 is downloaded, after the inspection. Catch2 assertions are
    // not thread-safe, so the hook only records whether its file operations worked.
    t.transport("grown").set_on_fetch([page = t.page_url("grown", ch(4), 0), vol2, stray, replace,
                                       flag = &fired,
                                       ok = &hook_ok](const md::HttpRequest& request) {
        if (request.url != page || flag->exchange(true)) {
            return;
        }
        std::error_code ec;
        if (replace) {
            std::ofstream{stray} << "someone else's file";
            std::filesystem::rename(stray, vol2, ec);
        } else {
            std::filesystem::remove(vol2, ec);
        }
        ok->store(!ec);
    });

    RecordingObserver observer;
    const md::RunReport report = t.run_ok("grown", t.options("grown"), observer);
    t.transport("grown").set_on_fetch({});
    CHECK(fired.load());
    CHECK(hook_ok.load());
    CHECK(md::exit_code_for(report) == 0);
    check_no_temporaries(t.out(), both_volumes());
    const md::EpubOutcome& outcome = report.epubs[1];
    if (replace) {
        CHECK(outcome.status == md::EpubStatus::exists);
        CHECK(outcome.existing == md::ExistingKind::foreign);
        CHECK(outcome.new_chapters.empty());
        CHECK(outcome.reused_chapters.empty());
        CHECK(outcome.pages == 0);
        CHECK(observer.warnings() ==
              std::vector{std::format("{}: left alone: changed while it was being updated; pass "
                                      "--overwrite to replace it",
                                      vol2.string())});
        CHECK(read_file(vol2) == "someone else's file");
    } else {
        CHECK(outcome.status == md::EpubStatus::written);
        CHECK_FALSE(outcome.replaced);
        CHECK(outcome.existing == md::ExistingKind::incomplete);
        CHECK(observer.warnings().empty());
        static_cast<void>(t.run_ok("grown", t.options("grown", "fresh")));
        CHECK(read_file(t.out("fresh") / clean_vol2) == read_file(vol2));
    }
}

TEST_CASE("download update: the inspection descriptor is still open when the update publishes",
          "[download][update][race]") {
    // V5: publish_replacing's identity check is ABA-safe only while the inspected inode stays
    // referenced. The last progress report comes after the last page is added and before the
    // EPUB is finished and published; at that point this process must still hold the old
    // Vol. 02 open (and Vol. 01, which is complete, must not be held).
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::filesystem::path vol1 = t.out() / clean_vol1;
    const std::filesystem::path vol2 = t.out() / clean_vol2;

    RecordingObserver observer;
    std::atomic<bool> checked{false};
    std::atomic<bool> vol2_held{false};
    std::atomic<bool> vol1_held{true};
    // Runs on the calling thread; it only looks (stat, /proc/self/fd, fstat), never touches vol2.
    observer.set_hook([vol1, vol2, done = &checked, held2 = &vol2_held,
                       held1 = &vol1_held](Event event, std::string_view message) {
        if (event == Event::pages && message == "4/4" && !done->exchange(true)) {
            held2->store(process_holds_open(vol2));
            held1->store(process_holds_open(vol1));
        }
    });
    const md::RunReport report = t.run_ok("reuse-only", t.options("reuse-only"), observer);
    CHECK(checked.load());
    CHECK(vol2_held.load());
    CHECK_FALSE(vol1_held.load());
    CHECK(report.epubs[1].status == md::EpubStatus::written);
    CHECK(report.epubs[1].replaced);
    // Once published, nothing holds the replaced inode (or the new file) any more.
    CHECK_FALSE(process_holds_open(vol2));
}

TEST_CASE("download update: a diverged file is left alone and lists what it holds",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("diverged", mdx::clean_diverged_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol2 = read_file(t.out() / clean_vol2);

    RecordingObserver observer;
    const md::RunReport report = t.run_ok("diverged", t.options("diverged"), observer);
    CHECK(md::exit_code_for(report) == 0);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(existing(report) == std::vector{md::ExistingKind::complete, md::ExistingKind::diverged});
    const md::EpubOutcome& outcome = report.epubs[1];
    // D2: the held chapters (ch 3, which MangaDex no longer lists), with the held page counts.
    REQUIRE(outcome.epub.chapters.size() == 1);
    CHECK(outcome.epub.chapters[0].id == ch(3));
    CHECK(outcome.epub.chapters[0].pages == 2);
    CHECK(outcome.pages == 2);
    CHECK(outcome.new_chapters.empty());
    CHECK(outcome.reused_chapters.empty());
    CHECK_FALSE(outcome.replaced);
    CHECK(observer.warnings() ==
          std::vector{std::format("{}: MangaDex lists 1 chapter this file lacks, but the file "
                                  "holds 1 chapter MangaDex no longer lists; left it alone (pass "
                                  "--overwrite to rebuild it without them)",
                                  (t.out() / clean_vol2).string())});
    CHECK(observer.begins().empty());
    CHECK(t.transport("diverged").count_containing("/at-home/") == 0);
    CHECK(read_file(t.out() / clean_vol2) == vol2);
}

TEST_CASE("download update: a complete file with extra held chapters reports all its pages",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    // Vol 1 now lists only ch 1; the file also holds ch 2 (2 pages), which is extra.
    mdx::FixtureManga shrunk = mdx::clean_fixture();
    std::erase_if(shrunk.chapters,
                  [](const mdx::FixtureChapter& chapter) { return chapter.id == ch(2); });
    t.add("shrunk", std::move(shrunk));
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol1 = read_file(t.out() / clean_vol1);

    RecordingObserver observer;
    const md::RunReport report = t.run_ok("shrunk", t.options("shrunk"), observer);
    CHECK(md::exit_code_for(report) == 0);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(existing(report) == std::vector{md::ExistingKind::complete, md::ExistingKind::complete});
    CHECK(report.epubs[0].pages == 5); // every image in the file: ch 1 (3) + ch 2 (2)
    CHECK(chapter_ids(report.epubs[0].epub) == std::vector{ch(1)}); // the final list
    CHECK(observer.warnings().empty());
    CHECK(read_file(t.out() / clean_vol1) == vol1);
}

// ---- interrupts ---------------------------------------------------------------------------------

TEST_CASE(
    "download update: an interrupt in the pre-pass, the copy or the download keeps the old file",
    "[download][update][interrupt]") {
    const InterruptFlagReset reset;
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    const std::string_view where = GENERATE("pre-pass", "copy", "download");
    INFO("interrupted in the " << where);
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    t.add("mixed", mdx::clean_mixed_reference_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));

    RecordingObserver observer;
    std::atomic<bool> fired{false};
    std::string tree = "reuse-only";
    std::filesystem::path target = t.out() / clean_vol2;
    if (where == "pre-pass") {
        // Vol 1 gains ch 2.5 and reuses ch 1 and ch 2. Ch 1 is damaged: its warning comes from
        // the pre-pass, which then stops at ch 2's first page.
        tree = "mixed";
        target = t.out() / clean_vol1;
        damage(target, "OEBPS/images/p00001.png");
        observer.set_hook([flag = &fired](Event event, std::string_view message) {
            if (event == Event::warning && message.contains("damaged") && !flag->exchange(true)) {
                md::simulate_interrupt_for_testing(SIGINT);
            }
        });
    } else if (where == "copy") {
        // The first copied page of ch 3 reports progress; the copy stops at the second.
        observer.set_hook([flag = &fired](Event event, std::string_view /*message*/) {
            if (event == Event::pages && !flag->exchange(true)) {
                md::simulate_interrupt_for_testing(SIGINT);
            }
        });
    } else {
        t.transport("reuse-only")
            .set_on_fetch([page = t.page_url("reuse-only", ch(4), 0),
                           flag = &fired](const md::HttpRequest& request) {
                if (request.url == page && !flag->exchange(true)) {
                    md::simulate_interrupt_for_testing(SIGINT);
                }
            });
    }
    const std::string before = read_file(target);
    const md::Result<md::RunReport> result = t.run(tree, t.options(tree), observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::interrupted);
    CHECK(fired.load());
    CHECK(read_file(target) == before);
    check_no_temporaries(t.out(), both_volumes());
    if (where != "download") {
        CHECK(t.transport(tree).count_containing("/at-home/") == 0); // nothing downloaded
    }
}

// ---- foreign files ------------------------------------------------------------------------------

TEST_CASE("download update: a foreign file under the name is left alone with its reason",
          "[download][update][foreign]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::filesystem::path vol2 = t.out() / clean_vol2;
    const std::string kind =
        GENERATE("fifo", "symlink", "directory", "random", "other book", "stale opf");
    INFO("foreign: " << kind);
    const std::string original = read_file(vol2);
    std::filesystem::remove(vol2);
    std::string reason = "not a regular file; rename or remove it, or use another DIR";
    if (kind == "fifo") {
        REQUIRE(::mkfifo(vol2.c_str(), 0600) == 0);
    } else if (kind == "symlink") {
        std::filesystem::create_symlink(clean_vol1, vol2); // to a valid book of this title
    } else if (kind == "directory") {
        REQUIRE(std::filesystem::create_directory(vol2));
    } else if (kind == "random") {
        write_file(vol2, "random bytes, not a zip archive");
        reason = "not an EPUB made by mangadex-dl; use another DIR or --title";
    } else if (kind == "other book") {
        std::filesystem::copy_file(t.out() / clean_vol1, vol2); // Vol. 01's identifier
        reason = "holds a different book; use another DIR or --title";
    } else {
        // Vol. 02 itself, with content.opf edited but the manifest's opf_crc32 left as it was.
        const auto edited = forge::rewrite_entries(
            zip::byte_vector(original), [](std::vector<forge::ForgeEntry>& entries) {
                for (forge::ForgeEntry& entry : entries) {
                    if (entry.name == "OEBPS/content.opf") {
                        entry.data.push_back(std::byte{0x0A});
                    }
                }
            });
        REQUIRE(edited.has_value());
        REQUIRE(forge::write_file(vol2, *edited));
        reason = "changed by another program; pass --overwrite to replace it";
    }
    const std::filesystem::file_status status_before = std::filesystem::symlink_status(vol2);
    const bool regular = std::filesystem::is_regular_file(status_before);
    const std::string bytes_before = regular ? read_file(vol2) : std::string{};

    RecordingObserver observer;
    const md::RunReport report = t.run_ok("clean", t.options("clean"), observer);
    CHECK(md::exit_code_for(report) == 0);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(existing(report) == std::vector{md::ExistingKind::complete, md::ExistingKind::foreign});
    const md::EpubOutcome& outcome = report.epubs[1];
    CHECK(outcome.pages == 0);
    CHECK(chapter_ids(outcome.epub) == std::vector{ch(3)}); // the planned chapters (D2)
    CHECK(outcome.new_chapters.empty());
    CHECK(observer.warnings() ==
          std::vector{std::format("{}: left alone: {}", vol2.string(), reason)});
    CHECK(std::filesystem::symlink_status(vol2).type() == status_before.type());
    if (regular) {
        CHECK(read_file(vol2) == bytes_before);
    }
}

TEST_CASE("download update: --overwrite replaces a foreign file without inspecting it",
          "[download][update][foreign]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol2 = read_file(t.out() / clean_vol2);
    write_file(t.out() / clean_vol2, "random bytes, not a zip archive");
    md::DownloadOptions options = t.options("clean");
    options.overwrite = true;
    RecordingObserver observer;
    const md::RunReport report = t.run_ok("clean", options, observer);
    CHECK(statuses(report) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(existing(report) ==
          std::vector{md::ExistingKind::unchecked, md::ExistingKind::unchecked});
    CHECK(report.epubs[1].replaced);
    CHECK(observer.warnings().empty());
    CHECK(read_file(t.out() / clean_vol2) == vol2);
}

// ---- plan errors --------------------------------------------------------------------------------

TEST_CASE("download update: a plan error leaves a complete file alone and fails an incomplete one",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    // Ch 3 (held) and, in the grown variant, ch 4 claim more pages than the planner accepts.
    mdx::FixtureManga huge_held = mdx::clean_fixture();
    huge_held.chapters.at(2).images.clear();
    huge_held.chapters.at(2).hash.clear();
    huge_held.chapters.at(2).pages = 6000;
    t.add("huge-held", std::move(huge_held));
    mdx::FixtureManga huge_new = mdx::clean_grown_fixture();
    huge_new.chapters.at(3).images.clear();
    huge_new.chapters.at(3).hash.clear();
    huge_new.chapters.at(3).pages = 6000;
    t.add("huge-new", std::move(huge_new));
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol2 = read_file(t.out() / clean_vol2);

    const md::RunReport complete = t.run_ok("huge-held", t.options("huge-held"));
    CHECK(md::exit_code_for(complete) == 0);
    CHECK(complete.epubs[1].status == md::EpubStatus::exists);
    CHECK(complete.epubs[1].existing == md::ExistingKind::complete);

    const md::RunReport incomplete = t.run_ok("huge-new", t.options("huge-new"));
    CHECK(md::exit_code_for(incomplete) == 6);
    const md::EpubOutcome& failed = incomplete.epubs[1];
    CHECK(failed.status == md::EpubStatus::failed);
    CHECK(failed.existing == md::ExistingKind::incomplete);
    CHECK_THAT(failed.error, ContainsSubstring("6000 pages"));
    CHECK(failed.new_chapters == std::vector{ch(4)});
    CHECK(t.transport("huge-new").count_containing("/at-home/") == 0);
    CHECK(read_file(t.out() / clean_vol2) == vol2);
    check_no_temporaries(t.out(), both_volumes());
}

// ---- dry runs -----------------------------------------------------------------------------------

TEST_CASE("download update: a dry run plans the update from the central directory only",
          "[download][update][dry-run]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    t.add("regrouped", mdx::clean_regrouped_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    const std::string vol1 = read_file(t.out() / clean_vol1);
    const std::string vol2 = read_file(t.out() / clean_vol2);

    md::DownloadOptions options = t.options("grown");
    options.dry_run = true;
    RecordingObserver observer;
    const md::RunReport report = t.run_ok("grown", options, observer);
    CHECK(report.dry_run);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::planned});
    CHECK(existing(report) ==
          std::vector{md::ExistingKind::complete, md::ExistingKind::incomplete});
    const md::EpubOutcome& vol2_plan = report.epubs[1];
    CHECK(vol2_plan.replaced);
    CHECK(vol2_plan.pages == 4);
    CHECK(vol2_plan.new_chapters == std::vector{ch(4)});
    CHECK(vol2_plan.reused_chapters == std::vector{ch(3)});
    CHECK(observer.begins().empty());
    CHECK(observer.warnings().empty());
    CHECK(t.transport("grown").count_containing("/at-home/") == 0);
    CHECK(t.transport("grown").count_containing("/uploads/") == 0);
    CHECK(read_file(t.out() / clean_vol1) == vol1);
    CHECK(read_file(t.out() / clean_vol2) == vol2);
    check_no_temporaries(t.out(), both_volumes());

    // Substitutions are reconciled in a dry run too.
    md::DownloadOptions regrouped = t.options("regrouped");
    regrouped.dry_run = true;
    const md::RunReport kept = t.run_ok("regrouped", regrouped);
    CHECK(statuses(kept) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    REQUIRE(kept.skipped.size() == 3);
    CHECK(kept.skipped[0].kept == ch(1));

    // A foreign file is reported (with its warning) and left alone.
    write_file(t.out() / clean_vol2, "not an epub");
    RecordingObserver foreign_observer;
    const md::RunReport foreign = t.run_ok("grown", options, foreign_observer);
    CHECK(foreign.epubs[1].status == md::EpubStatus::exists);
    CHECK(foreign.epubs[1].existing == md::ExistingKind::foreign);
    CHECK(foreign_observer.warnings().size() == 1);
    CHECK(read_file(t.out() / clean_vol2) == "not an epub");
}

// ---- layouts ------------------------------------------------------------------------------------

TEST_CASE("download update: layout chapters writes only the new chapter's file",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    md::DownloadOptions first = t.options("clean");
    first.layout = md::Layout::chapters;
    static_cast<void>(t.run_ok("clean", first));
    const std::vector<std::string> before = list_dir(t.out());
    REQUIRE(before.size() == 3);

    md::DownloadOptions update = t.options("grown");
    update.layout = md::Layout::chapters;
    const md::RunReport report = t.run_ok("grown", update);
    REQUIRE(report.epubs.size() == 4);
    CHECK(statuses(report) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists,
                                          md::EpubStatus::exists, md::EpubStatus::written});
    CHECK(existing(report) == std::vector{md::ExistingKind::complete, md::ExistingKind::complete,
                                          md::ExistingKind::complete, md::ExistingKind::none});
    CHECK_FALSE(report.epubs[3].replaced);
    CHECK(list_dir(t.out()).size() == 4);
}

TEST_CASE("download update: layout single rebuilds the one file with reuse", "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    t.add("grown", mdx::clean_grown_fixture());
    md::DownloadOptions first = t.options("clean");
    first.layout = md::Layout::single;
    static_cast<void>(t.run_ok("clean", first));

    md::DownloadOptions update = t.options("reuse-only");
    update.layout = md::Layout::single;
    const md::RunReport report = t.run_ok("reuse-only", update);
    REQUIRE(report.epubs.size() == 1);
    const md::EpubOutcome& outcome = report.epubs[0];
    CHECK(outcome.status == md::EpubStatus::written);
    CHECK(outcome.existing == md::ExistingKind::incomplete);
    CHECK(outcome.replaced);
    CHECK(outcome.new_chapters == std::vector{ch(4)});
    CHECK(outcome.reused_chapters == std::vector{ch(1), ch(2), ch(3)});
    CHECK(outcome.pages == 9);

    md::DownloadOptions fresh = t.options("grown", "fresh");
    fresh.layout = md::Layout::single;
    const md::RunReport reference = t.run_ok("grown", fresh);
    REQUIRE(reference.epubs.size() == 1);
    const std::string name = reference.epubs[0].epub.file_name;
    CHECK(read_file(t.out("fresh") / name) == read_file(t.out() / name));
}

// ---- threads ------------------------------------------------------------------------------------

TEST_CASE("download update: observer calls stay on the calling thread during an update",
          "[download][update]") {
    Trees t;
    t.add("clean", mdx::clean_fixture());
    t.add("reuse-only", mdx::clean_grown_reuse_only_fixture());
    static_cast<void>(t.run_ok("clean", t.options("clean")));
    md::DownloadOptions update = t.options("reuse-only");
    update.jobs = 8;
    RecordingObserver observer;
    static_cast<void>(t.run_ok("reuse-only", update, observer));
    const std::vector<std::thread::id> threads = observer.threads();
    REQUIRE_FALSE(threads.empty());
    const std::thread::id self = std::this_thread::get_id();
    CHECK(std::ranges::all_of(threads, [self](std::thread::id id) { return id == self; }));
}
