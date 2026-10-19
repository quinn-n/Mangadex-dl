// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/download/download_test.cpp: run_download end to end (download plan T14 + rev1) over
// fixture trees (tests/support/mdx_fixture.hpp) served by the fixture transport, wrapped in an
// overlay that scripts failures for chosen URLs, with the instant clock. Covers: clean, basic,
// incomplete and no-chapter titles; byte-identical output for any --jobs; re-run, --overwrite and
// dry-run; at-home refreshes; covers that fail; plan errors; not-found, network and local
// failures; interrupts and caller stops on both temporary-file paths; the observer's thread.

#include "fs/output_dir_testing.hpp" // core-private seam (src/core is on the test include path)
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
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
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

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
using Catch::Matchers::ContainsSubstring;

// ---- test doubles ------------------------------------------------------------------------------

/// The fixture transport, plus per-URL scripted answers that win over it (each consumed once), a
/// hook that runs on every fetch, and a record of every URL asked for. Thread-safe.
class OverlayTransport final : public md::Transport {
public:
    explicit OverlayTransport(std::unique_ptr<md::Transport> inner) : inner_(std::move(inner)) {}

    void push(std::string url, md::TransportResult result) {
        const std::scoped_lock lock{mutex_};
        scripted_[std::move(url)].push_back(std::move(result));
    }

    void push_status(const std::string& url, int status, int times = 1) {
        for (int i = 0; i < times; ++i) {
            push(url, md::HttpResponse{.status = status});
        }
    }

    /// Runs on the fetching thread, outside the lock, before the request is answered.
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
        {
            const std::scoped_lock lock{mutex_};
            if (const auto it = scripted_.find(request.url);
                it != scripted_.end() && !it->second.empty()) {
                md::TransportResult result = std::move(it->second.front());
                it->second.pop_front();
                return result;
            }
        }
        return inner_->fetch(request);
    }

    /// Requests to exactly `url`.
    [[nodiscard]] std::size_t count(std::string_view url) const {
        const std::scoped_lock lock{mutex_};
        return static_cast<std::size_t>(std::ranges::count(urls_, url));
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
    std::map<std::string, std::deque<md::TransportResult>, std::less<>> scripted_{}; ///< guarded
    std::function<void(const md::HttpRequest&)> on_fetch_{};                         ///< guarded
    std::vector<std::string> urls_{};                                                ///< guarded
};

/// Records every observer call and the thread it came from. The mutex only keeps a broken
/// implementation (a call from a worker) from corrupting the record; the thread ids catch it.
class RecordingObserver final : public md::DownloadObserver {
public:
    RecordingObserver() = default;

    void on_plan(const md::Plan& plan) override {
        const std::scoped_lock lock{mutex_};
        record();
        plans_.push_back(plan);
    }
    void on_epub_begin(const md::PlannedEpub& epub, std::size_t index, std::size_t total) override {
        const std::scoped_lock lock{mutex_};
        record();
        begins_.push_back(epub.file_name);
        CHECK(index < total);
    }
    void on_pages(std::size_t done, std::size_t total) override {
        const std::scoped_lock lock{mutex_};
        record();
        ++page_events_;
        CHECK(done <= total);
    }
    void on_epub_end(const md::EpubOutcome& outcome) override {
        const std::scoped_lock lock{mutex_};
        record();
        ends_.push_back(outcome);
    }
    void on_warning(std::string_view message) override {
        const std::scoped_lock lock{mutex_};
        record();
        warnings_.emplace_back(message);
    }
    void on_verbose(int level, std::string_view message) override {
        const std::scoped_lock lock{mutex_};
        record();
        verbose_.emplace_back(level, std::string{message});
    }

    [[nodiscard]] std::vector<std::thread::id> threads() const {
        const std::scoped_lock lock{mutex_};
        return threads_;
    }
    [[nodiscard]] std::size_t plan_count() const {
        const std::scoped_lock lock{mutex_};
        return plans_.size();
    }
    [[nodiscard]] std::vector<std::string> begins() const {
        const std::scoped_lock lock{mutex_};
        return begins_;
    }
    [[nodiscard]] std::size_t page_events() const {
        const std::scoped_lock lock{mutex_};
        return page_events_;
    }
    [[nodiscard]] std::vector<md::EpubOutcome> ends() const {
        const std::scoped_lock lock{mutex_};
        return ends_;
    }
    [[nodiscard]] std::vector<std::string> warnings() const {
        const std::scoped_lock lock{mutex_};
        return warnings_;
    }
    [[nodiscard]] std::vector<std::pair<int, std::string>> verbose() const {
        const std::scoped_lock lock{mutex_};
        return verbose_;
    }

    /// Warnings containing `part`.
    [[nodiscard]] std::size_t warnings_containing(std::string_view part) const {
        const std::scoped_lock lock{mutex_};
        return static_cast<std::size_t>(std::ranges::count_if(
            warnings_, [part](const std::string& line) { return line.contains(part); }));
    }

private:
    void record() { threads_.push_back(std::this_thread::get_id()); }

    mutable std::mutex mutex_{};
    std::vector<std::thread::id> threads_{};
    std::vector<md::Plan> plans_{};
    std::vector<std::string> begins_{};
    std::size_t page_events_ = 0;
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

/// A fixture tree in a fresh temporary directory, its transport and an instant clock. Output
/// directories go next to the tree.
class Harness {
public:
    explicit Harness(mdx::FixtureManga manga)
        : manga_(std::move(manga)), tmp_(make_temp_dir()), root_(tmp_.path() / "fixture") {
        const auto written = mdx::write_fixture_tree(root_, manga_);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        auto inner = md::make_fixture_transport(root_);
        REQUIRE(inner.has_value());
        transport_ = std::make_unique<OverlayTransport>(std::move(*inner));
    }

    [[nodiscard]] const mdx::FixtureManga& manga() const noexcept { return manga_; }
    [[nodiscard]] OverlayTransport& transport() noexcept { return *transport_; }

    /// `<tmp>/<name>`, not created.
    [[nodiscard]] std::filesystem::path out(std::string_view name) const {
        return tmp_.path() / name;
    }

    /// Options for this fixture's title, writing into out(dir_name).
    [[nodiscard]] md::DownloadOptions options(std::string_view dir_name = "out") const {
        return options_for(manga_.id, dir_name);
    }

    [[nodiscard]] md::DownloadOptions options_for(std::string_view manga_id,
                                                  std::string_view dir_name) const {
        auto id = md::MangaId::parse(manga_id);
        REQUIRE(id.has_value());
        auto base = md::ApiBase::parse(base_url());
        REQUIRE(base.has_value());
        return md::DownloadOptions{
            .manga = std::move(*id),
            .dir = out(dir_name),
            .api_base = std::move(*base),
            .report = false,
        };
    }

    [[nodiscard]] md::Result<md::RunReport> run(const md::DownloadOptions& options,
                                                RecordingObserver& observer,
                                                std::stop_source stop = {}) {
        const md::DownloadServices services{
            .transport = transport_.get(),
            .clock = clock_.get(),
            .jitter = {},
            .stop = std::move(stop),
            .user_agent = "mangadex-dl/test",
        };
        return md::run_download(options, services, observer);
    }

    // URLs exactly as the client builds them in fixture mode.
    [[nodiscard]] std::string base_url() const { return "file://" + root_.string(); }
    [[nodiscard]] std::string api() const { return base_url() + "/api"; }

    [[nodiscard]] std::string at_home_url(std::size_t chapter) const {
        return md::at_home_request_url(api(), manga_.chapters.at(chapter).id);
    }
    [[nodiscard]] std::string page_url(std::size_t chapter, std::size_t page) const {
        const mdx::FixtureChapter& c = manga_.chapters.at(chapter);
        return md::image_url(base_url() + "/at-home", md::ImageQuality::data, c.hash,
                             mdx::page_file_name(page, c.images.at(page)));
    }
    /// Chapter `chapter`'s at-home document listing only its first `pages` pages.
    [[nodiscard]] std::string at_home_body(std::size_t chapter, std::size_t pages) const {
        mdx::FixtureChapter c = manga_.chapters.at(chapter);
        REQUIRE(pages <= c.images.size());
        c.images.resize(pages);
        return mdx::at_home_json(c).dump();
    }
    [[nodiscard]] std::string cover_url(std::string_view file) const {
        auto id = md::MangaId::parse(manga_.id);
        REQUIRE(id.has_value());
        return md::cover_image_url(base_url() + "/uploads", *id, file);
    }
    [[nodiscard]] std::string feed_url() const {
        auto id = md::MangaId::parse(manga_.id);
        REQUIRE(id.has_value());
        return md::feed_request_url(api(), *id, "en", 0);
    }

private:
    [[nodiscard]] static md::test::TempDir make_temp_dir() {
        auto tmp = md::test::TempDir::create("mangadex-dl-download");
        REQUIRE(tmp.has_value());
        return std::move(*tmp);
    }

    mdx::FixtureManga manga_{};
    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    std::filesystem::path root_{};
    std::unique_ptr<OverlayTransport> transport_{};
    std::unique_ptr<md::Clock> clock_{md::make_instant_clock()};
};

// ---- helpers -----------------------------------------------------------------------------------

/// Every entry name in `dir` (hidden ones included), sorted. Empty when `dir` does not exist.
[[nodiscard]] std::vector<std::string> list_dir(const std::filesystem::path& dir) {
    std::vector<std::string> names{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::vector<md::EpubStatus> statuses(const md::RunReport& report) {
    std::vector<md::EpubStatus> out{};
    out.reserve(report.epubs.size());
    for (const md::EpubOutcome& outcome : report.epubs) {
        out.push_back(outcome.status);
    }
    return out;
}

/// The archive at `path`, validated by the strict zip reader.
[[nodiscard]] zip::Archive read_epub(const std::filesystem::path& path) {
    auto archive = zip::read_archive_file(path);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    return std::move(*archive);
}

/// How many page images the EPUB holds.
[[nodiscard]] std::size_t page_images(const zip::Archive& archive) {
    return static_cast<std::size_t>(std::ranges::count_if(
        archive.entries, [](const zip::Entry& e) { return e.name.starts_with("OEBPS/images/p"); }));
}

constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view unknown_title = "66666666-6666-4666-8666-666666666666";

} // namespace

// ---- happy paths
// ---------------------------------------------------------------------------------

TEST_CASE("download writes every planned file of a clean title", "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    const md::RunReport& report = *result;

    CHECK(md::exit_code_for(report) == 0);
    CHECK_FALSE(report.dry_run);
    CHECK(report.manga.title == "Clean Title");
    REQUIRE(report.epubs.size() == 2);
    CHECK(statuses(report) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(report.epubs[0].path == (h.out("out") / clean_vol1).string());
    CHECK(report.epubs[1].path == (h.out("out") / clean_vol2).string());
    CHECK(report.epubs[0].pages == 5);
    CHECK(report.epubs[1].pages == 2);
    CHECK_FALSE(report.epubs[0].replaced);
    CHECK(report.epubs[0].error.empty());
    // Nothing but the two books: no temporaries left behind.
    CHECK(list_dir(h.out("out")) ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});

    for (const md::EpubOutcome& outcome : report.epubs) {
        CHECK(outcome.bytes == std::filesystem::file_size(outcome.path));
        const zip::Archive archive = read_epub(outcome.path);
        REQUIRE_FALSE(archive.entries.empty());
        CHECK(archive.entries.front().name == "mimetype");
        CHECK(archive.find("META-INF/container.xml") != nullptr);
        CHECK(archive.find("OEBPS/content.opf") != nullptr);
        CHECK(archive.find("OEBPS/nav.xhtml") != nullptr);
        CHECK(archive.find("OEBPS/images/cover.png") != nullptr);
        CHECK(page_images(archive) == outcome.pages);
    }
    const zip::Archive vol1 = read_epub(report.epubs[0].path);
    const zip::Entry* nav = vol1.find("OEBPS/nav.xhtml");
    REQUIRE(nav != nullptr);
    CHECK_THAT(nav->text(), ContainsSubstring("Ch. 1: Beginnings"));
    CHECK_THAT(nav->text(), ContainsSubstring("Ch. 2: Middles"));
    const zip::Entry* opf = vol1.find("OEBPS/content.opf");
    REQUIRE(opf != nullptr);
    CHECK_THAT(opf->text(), ContainsSubstring("Clean Author"));
    CHECK_THAT(opf->text(), ContainsSubstring("Clean Scans"));
    CHECK_THAT(opf->text(), ContainsSubstring("urn:mangadex:11111111-1111-4111-8111-111111111111"));

    CHECK(observer.plan_count() == 1);
    CHECK(observer.begins() ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
    CHECK(observer.ends() == report.epubs);
    CHECK(observer.page_events() == 7);
    CHECK(observer.warnings().empty());
}

TEST_CASE("download output is byte-identical for one and eight jobs", "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    md::DownloadOptions one = h.options("one");
    one.jobs = 1;
    md::DownloadOptions eight = h.options("eight");
    eight.jobs = 8;
    REQUIRE(h.run(one, observer).has_value());
    REQUIRE(h.run(eight, observer).has_value());
    for (const std::string_view name : {clean_vol1, clean_vol2}) {
        CHECK(read_file(h.out("one") / name) == read_file(h.out("eight") / name));
    }
}

TEST_CASE("download --data-saver fetches the data-saver images", "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    md::DownloadOptions options = h.options();
    options.quality = md::ImageQuality::data_saver;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(h.transport().count_containing("/at-home/data-saver/") == 7);
    CHECK(h.transport().count_containing("/at-home/data/") == 0);
}

TEST_CASE("download layout single nests chapters under their volumes", "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    md::DownloadOptions options = h.options();
    options.layout = md::Layout::single;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    REQUIRE(result->epubs.size() == 1);
    CHECK(result->epubs[0].status == md::EpubStatus::written);
    CHECK(result->epubs[0].pages == 7);
    const zip::Archive archive = read_epub(result->epubs[0].path);
    const zip::Entry* nav = archive.find("OEBPS/nav.xhtml");
    REQUIRE(nav != nullptr);
    const std::string text = nav->text();
    CHECK_THAT(text, ContainsSubstring("Vol. 1"));
    CHECK_THAT(text, ContainsSubstring("Vol. 2"));
    CHECK_THAT(text, ContainsSubstring("Ch. 3: Endings"));
}

// ---- existing files, overwrite, dry-run
// ------------------------------------------------------------

TEST_CASE("download re-run finds every file and --overwrite replaces it identically",
          "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver first;
    const auto written = h.run(h.options(), first);
    REQUIRE(written.has_value());
    const std::string original = read_file(h.out("out") / clean_vol1);
    const std::size_t at_home_before = h.transport().count_containing("/api/at-home/");

    RecordingObserver second;
    const auto again = h.run(h.options(), second);
    REQUIRE(again.has_value());
    CHECK(statuses(*again) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(md::exit_code_for(*again) == 0);
    // An up-to-date file reports the pages it holds (skip-existing: `pages` for exists is the
    // held count, no longer 0).
    CHECK(again->epubs[0].pages == 5);
    CHECK(again->epubs[1].pages == 2);
    CHECK(again->epubs[0].existing == md::ExistingKind::complete);
    CHECK(second.begins().empty());
    CHECK(h.transport().count_containing("/api/at-home/") == at_home_before);
    CHECK(read_file(h.out("out") / clean_vol1) == original);

    RecordingObserver third;
    md::DownloadOptions overwrite = h.options();
    overwrite.overwrite = true;
    const auto replaced = h.run(overwrite, third);
    REQUIRE(replaced.has_value());
    CHECK(statuses(*replaced) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(replaced->epubs[0].replaced);
    CHECK(replaced->epubs[1].replaced);
    CHECK(read_file(h.out("out") / clean_vol1) == original);
    CHECK(list_dir(h.out("out")) ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
}

TEST_CASE("download dry-run plans every file without creating or downloading anything",
          "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    md::DownloadOptions options = h.options();
    options.dry_run = true;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(result->dry_run);
    CHECK(statuses(*result) == std::vector{md::EpubStatus::planned, md::EpubStatus::planned});
    CHECK(result->epubs[0].path == (h.out("out") / clean_vol1).string());
    CHECK(result->epubs[0].pages == 5);
    CHECK(md::exit_code_for(*result) == 0);
    CHECK_FALSE(std::filesystem::exists(h.out("out")));
    CHECK(h.transport().count_containing("/at-home/") == 0);
    CHECK(h.transport().count_containing("/uploads/") == 0);
    CHECK(observer.begins().empty());
    CHECK(observer.ends() == result->epubs);

    SECTION("a file that already exists is reported as exists") {
        REQUIRE(std::filesystem::create_directory(h.out("out")));
        std::ofstream{h.out("out") / clean_vol2} << "not an epub";
        RecordingObserver later;
        const auto again = h.run(options, later);
        REQUIRE(again.has_value());
        CHECK(statuses(*again) == std::vector{md::EpubStatus::planned, md::EpubStatus::exists});
        CHECK(!again->epubs[0].replaced);
        CHECK(!again->epubs[1].replaced);
        CHECK(again->epubs[1].pages == 0);
        CHECK(list_dir(h.out("out")) == std::vector<std::string>{std::string{clean_vol2}});
    }

    SECTION("with --overwrite an existing file stays planned and is marked replaced") {
        REQUIRE(std::filesystem::create_directory(h.out("out")));
        std::ofstream{h.out("out") / clean_vol2} << "not an epub";
        md::DownloadOptions replacing = options;
        replacing.overwrite = true;
        RecordingObserver later;
        const auto again = h.run(replacing, later);
        REQUIRE(again.has_value());
        CHECK(statuses(*again) == std::vector{md::EpubStatus::planned, md::EpubStatus::planned});
        CHECK(!again->epubs[0].replaced);
        CHECK(again->epubs[1].replaced);
        CHECK(again->epubs[1].pages > 0);
        CHECK(md::exit_code_for(*again) == 0);
        CHECK(later.ends() == again->epubs); // the observer sees the same outcome
        CHECK(read_file(h.out("out") / clean_vol2) == "not an epub");
        CHECK(list_dir(h.out("out")) == std::vector<std::string>{std::string{clean_vol2}});
        CHECK(h.transport().count_containing("/at-home/") == 0);

        const nlohmann::json doc = nlohmann::json::parse(md::report_to_json(*again));
        REQUIRE(doc.at("epubs").size() == 2);
        CHECK(doc.at("epubs").at(0).at("status") == "planned");
        CHECK(doc.at("epubs").at(0).at("replaced") == false);
        CHECK(doc.at("epubs").at(1).at("status") == "planned");
        CHECK(doc.at("epubs").at(1).at("replaced") == true);
    }

    SECTION("with --overwrite and no existing files nothing is marked replaced") {
        md::DownloadOptions replacing = options;
        replacing.overwrite = true;
        RecordingObserver later;
        const auto again = h.run(replacing, later);
        REQUIRE(again.has_value());
        CHECK(statuses(*again) == std::vector{md::EpubStatus::planned, md::EpubStatus::planned});
        CHECK(!again->epubs[0].replaced);
        CHECK(!again->epubs[1].replaced);
        CHECK_FALSE(std::filesystem::exists(h.out("out")));
    }
}

// ---- skips, warnings, failures per file ---------------------------------------------------------

TEST_CASE("download basic title: skips, dedupe and one WebP warning", "[download]") {
    Harness h{mdx::basic_fixture()};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    const md::RunReport& report = *result;
    CHECK(md::exit_code_for(report) == 0);
    REQUIRE(report.epubs.size() == 5);
    for (const md::EpubOutcome& outcome : report.epubs) {
        INFO(outcome.path << ": " << outcome.error);
        CHECK(outcome.status == md::EpubStatus::written);
        static_cast<void>(read_epub(outcome.path));
    }

    const auto has_skip = [&report](md::SkipReason reason) {
        return std::ranges::any_of(report.skipped, [reason](const md::SkippedChapter& skipped) {
            return skipped.reason == reason;
        });
    };
    CHECK(has_skip(md::SkipReason::external));
    CHECK(has_skip(md::SkipReason::no_pages));
    CHECK(has_skip(md::SkipReason::unavailable));
    CHECK(has_skip(md::SkipReason::duplicate));

    // One warning per non-duplicate skip, one WebP warning (vol 1 only), nothing else.
    CHECK(observer.warnings_containing("skipped ") == 3);
    CHECK(observer.warnings_containing("hosted outside MangaDex") == 1);
    CHECK(observer.warnings_containing("WebP") == 1);
    CHECK(observer.warnings_containing("Basic Title - Vol. 01.epub: contains WebP") == 1);
    CHECK(observer.warnings().size() == 4);
    // The duplicate (Beta's ch 1) was never downloaded.
    CHECK(h.transport().count(h.at_home_url(1)) == 0);
}

TEST_CASE("download incomplete title fails one file and writes the others", "[download]") {
    Harness h{mdx::incomplete_fixture()};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    const md::RunReport& report = *result;
    REQUIRE(report.epubs.size() == 2);
    CHECK(report.epubs[0].status == md::EpubStatus::written);
    CHECK(report.epubs[1].status == md::EpubStatus::failed);
    CHECK_FALSE(report.epubs[1].error.empty());
    CHECK(md::exit_code_for(report) == 6);
    CHECK(md::summarize(report) == md::RunSummary{.written = 1, .failed = 1});
    CHECK(list_dir(h.out("out")) ==
          std::vector<std::string>{std::string{"Incomplete Title - Vol. 01.epub"}});
    // The missing page made the chapter refresh its at-home server three times.
    CHECK(h.transport().count(h.at_home_url(1)) == 4);
    CHECK(h.transport().count(h.page_url(1, 1)) == 4);
    CHECK(observer.ends() == report.epubs);
}

TEST_CASE("download refreshes the at-home server after an image-host 403", "[download]") {
    Harness h{mdx::clean_fixture()};
    h.transport().push_status(h.page_url(0, 1), 403);
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(h.transport().count(h.at_home_url(0)) == 2);
    CHECK(h.transport().count(h.page_url(0, 1)) == 2);
    const auto verbose = observer.verbose();
    CHECK(std::ranges::any_of(verbose, [](const auto& line) {
        return line.first == 1 && line.second.contains("refreshing its at-home server (1 of 3)");
    }));
}

TEST_CASE("download gives a file up after three at-home refreshes", "[download]") {
    Harness h{mdx::clean_fixture()};
    h.transport().push_status(h.page_url(0, 0), 403, 4);
    md::DownloadOptions options = h.options();
    options.jobs = GENERATE(1U, 8U);
    RecordingObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::failed, md::EpubStatus::written});
    CHECK_THAT(result->epubs[0].error, ContainsSubstring("403"));
    CHECK_THAT(result->epubs[0].error, ContainsSubstring("after 3 at-home server refreshes"));
    CHECK(md::exit_code_for(*result) == 6);
    CHECK(h.transport().count(h.at_home_url(0)) == 4);
    CHECK(h.transport().count(h.page_url(0, 0)) == 4);
    CHECK(list_dir(h.out("out")) == std::vector<std::string>{std::string{clean_vol2}});
}

TEST_CASE("download fails a file whose at-home refresh changes the page list", "[download]") {
    Harness h{mdx::clean_fixture()};
    // Page 2 of chapter 1 fails once; the refreshed at-home server lists one page instead of 3.
    h.transport().push_status(h.page_url(0, 1), 403);
    // The first at-home answer is the real document (scripted, so that the second can differ).
    h.transport().push(h.at_home_url(0),
                       md::HttpResponse{.status = 200, .body = h.at_home_body(0, 3)});
    h.transport().push(h.at_home_url(0),
                       md::HttpResponse{.status = 200, .body = h.at_home_body(0, 1)});
    md::DownloadOptions options = h.options();
    options.jobs = GENERATE(1U, 8U);
    INFO("jobs: " << options.jobs);
    RecordingObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::failed, md::EpubStatus::written});
    CHECK_THAT(result->epubs[0].error, ContainsSubstring("changed the chapter's page list"));
    CHECK(md::exit_code_for(*result) == 6);
    CHECK(h.transport().count(h.at_home_url(0)) == 2);
    CHECK(list_dir(h.out("out")) == std::vector<std::string>{std::string{clean_vol2}});
}

TEST_CASE("download survives concurrent page failures that share one refresh", "[download]") {
    Harness h{mdx::clean_fixture()};
    const std::size_t pages = h.manga().chapters.at(0).images.size();
    for (std::size_t page = 0; page < pages; ++page) {
        h.transport().push_status(h.page_url(0, page), 403);
    }
    md::DownloadOptions options = h.options();
    options.jobs = 8;
    RecordingObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    // Each failed page is fetched once more after a refresh, its own or another worker's; the
    // refreshes stay within the budget whatever the interleaving.
    for (std::size_t page = 0; page < pages; ++page) {
        CHECK(h.transport().count(h.page_url(0, page)) == 2);
    }
    CHECK(h.transport().count(h.at_home_url(0)) >= 2);
    CHECK(h.transport().count(h.at_home_url(0)) <= 4);
    CHECK(list_dir(h.out("out")) ==
          std::vector<std::string>{std::string{clean_vol1}, std::string{clean_vol2}});
}

TEST_CASE("download fails a file whose at-home refresh fails", "[download]") {
    Harness h{mdx::clean_fixture()};
    h.transport().push_status(h.page_url(0, 1), 403);
    // The first at-home answer is the real document; the refresh then gets 503 until the
    // client's retries are exhausted.
    h.transport().push(h.at_home_url(0),
                       md::HttpResponse{.status = 200, .body = h.at_home_body(0, 3)});
    h.transport().push_status(h.at_home_url(0), 503, 4);
    md::DownloadOptions options = h.options();
    options.jobs = GENERATE(1U, 8U);
    INFO("jobs: " << options.jobs);
    RecordingObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::failed, md::EpubStatus::written});
    CHECK_THAT(result->epubs[0].error, ContainsSubstring("503"));
    CHECK(md::exit_code_for(*result) == 6);
    CHECK(h.transport().count(h.at_home_url(0)) == 5);
    CHECK(list_dir(h.out("out")) == std::vector<std::string>{std::string{clean_vol2}});
}

TEST_CASE("download uses the first page as the cover when the cover fails", "[download]") {
    Harness h{mdx::clean_fixture()};
    h.transport().push_status(h.cover_url("cover-v1.png"), 404, 4);
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(observer.warnings_containing(
              "Clean Title - Vol. 01.epub: could not download the cover") == 1);
    CHECK(observer.warnings().size() == 1);
    CHECK(read_epub(result->epubs[0].path).find("OEBPS/images/cover.png") == nullptr);
    CHECK(read_epub(result->epubs[1].path).find("OEBPS/images/cover.png") != nullptr);
}

TEST_CASE("download reports a file the planner rejected as failed without fetching it",
          "[download]") {
    mdx::FixtureManga manga = mdx::clean_fixture();
    mdx::FixtureChapter& huge = manga.chapters.at(2); // vol 2
    huge.images.clear();
    huge.hash.clear();
    huge.pages = 6000; // over max_pages_per_chapter
    Harness h{std::move(manga)};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::failed});
    CHECK_THAT(result->epubs[1].error, ContainsSubstring("6000 pages"));
    CHECK(md::exit_code_for(*result) == 6);
    CHECK(h.transport().count(h.at_home_url(2)) == 0);
    CHECK(observer.begins() == std::vector<std::string>{std::string{clean_vol1}});
}

// ---- whole-run outcomes
// --------------------------------------------------------------------------

TEST_CASE("download of a title without matching chapters is an empty plan", "[download]") {
    Harness h{mdx::nochapters_fixture()};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE(result.has_value());
    CHECK(result->epubs.empty());
    CHECK(md::exit_code_for(*result) == 5);
    CHECK_FALSE(std::filesystem::exists(h.out("out")));
    CHECK(observer.plan_count() == 1);
}

TEST_CASE("download of an unknown title is not_found", "[download]") {
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    const auto result = h.run(h.options_for(unknown_title, "out"), observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::not_found);
    CHECK_THAT(result.error().message, ContainsSubstring(std::string{unknown_title}) &&
                                           ContainsSubstring("not found on MangaDex (HTTP 404)"));
    CHECK(result.error().exit_code() == 4);
    CHECK(observer.plan_count() == 0);
    CHECK_FALSE(std::filesystem::exists(h.out("out")));
}

TEST_CASE("download with a feed that keeps failing is a network error", "[download]") {
    Harness h{mdx::clean_fixture()};
    h.transport().push_status(h.feed_url(), 503, 4);
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::network);
    CHECK(result.error().exit_code() == 3);
    CHECK(h.transport().count(h.feed_url()) == 4);
    CHECK_FALSE(std::filesystem::exists(h.out("out")));
}

TEST_CASE("download into a DIR that is a regular file fails locally", "[download]") {
    Harness h{mdx::clean_fixture()};
    std::ofstream{h.out("file")} << "a file, not a directory";
    RecordingObserver observer;
    const auto result = h.run(h.options("file"), observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::failure);
    CHECK(result.error().exit_code() == 1);
    CHECK(h.transport().count_containing("/at-home/") == 0);
}

TEST_CASE("download aborts the run on a local write failure", "[download]") {
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Harness h{mdx::clean_fixture()};
    const md::testing::ScopedIoError enospc{md::testing::IoStage::write, ENOSPC};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::failure);
    CHECK(list_dir(h.out("out")).empty());
    CHECK(observer.ends().empty());
}

TEST_CASE("download refuses services without a transport or a clock", "[download]") {
    RecordingObserver observer;
    const auto result = md::run_download(md::DownloadOptions{}, md::DownloadServices{}, observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::failure);
    CHECK(observer.threads().empty());
}

// ---- interrupts, stops and threads
// ----------------------------------------------------------------

TEST_CASE("download interrupted during a page fetch leaves no files or temporaries", "[download]") {
    const InterruptFlagReset reset;
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Harness h{mdx::clean_fixture()};
    md::DownloadOptions options = h.options();
    options.jobs = GENERATE(1U, 4U);
    // The hook runs on a pool worker while the first file is being built. `fired` outlives the
    // run (the transport keeps the hook, but nothing calls it after run_download returns).
    std::atomic<bool> fired{false};
    h.transport().set_on_fetch([page = h.page_url(0, 1), flag = &fired](const auto& request) {
        if (request.url == page && !flag->exchange(true)) {
            md::simulate_interrupt_for_testing(SIGINT);
        }
    });
    RecordingObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::interrupted);
    CHECK(fired.load());
    CHECK(md::interrupt_signal() == SIGINT);
    CHECK(std::filesystem::is_directory(h.out("out")));
    CHECK(list_dir(h.out("out")).empty());
    CHECK(observer.ends().empty());
}

TEST_CASE("download stopped by the caller unwinds as interrupted", "[download]") {
    const InterruptFlagReset reset;
    const bool fallback = GENERATE(false, true);
    INFO("fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Harness h{mdx::clean_fixture()};
    const std::stop_source stop{};
    // `stop` is captured by value (a copy shares the stop state); request_stop is non-const in
    // the standard, hence `mutable`.
    h.transport().set_on_fetch([page = h.page_url(0, 0), stop](const auto& request) mutable {
        if (request.url == page) {
            static_cast<void>(stop.request_stop());
        }
    });
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer, stop);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::interrupted);
    CHECK_FALSE(md::interrupt_requested());
    CHECK(list_dir(h.out("out")).empty());
}

TEST_CASE("download interrupted before it starts does nothing", "[download]") {
    const InterruptFlagReset reset;
    md::simulate_interrupt_for_testing(SIGINT);
    Harness h{mdx::clean_fixture()};
    RecordingObserver observer;
    const auto result = h.run(h.options(), observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::interrupted);
    CHECK_FALSE(std::filesystem::exists(h.out("out")));
    CHECK(observer.threads().empty());
}

TEST_CASE("download calls the observer only on the calling thread", "[download]") {
    Harness h{mdx::basic_fixture()};
    md::DownloadOptions options = h.options();
    options.jobs = 8;
    RecordingObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    const std::vector<std::thread::id> threads = observer.threads();
    REQUIRE_FALSE(threads.empty());
    const std::thread::id self = std::this_thread::get_id();
    CHECK(std::ranges::all_of(threads, [self](std::thread::id id) { return id == self; }));
    // Trace lines from the workers' page fetches reached the observer (on this thread) as -vv.
    const auto verbose = observer.verbose();
    CHECK(std::ranges::any_of(verbose, [](const auto& line) {
        return line.first == 2 && line.second.contains("/at-home/data/");
    }));
    CHECK(observer.page_events() > 0);
}
