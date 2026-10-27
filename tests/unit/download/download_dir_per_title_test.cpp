// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/download/download_dir_per_title_test.cpp: run_download with
// DownloadOptions::dir_per_title (list plan T6; AC38, V13, spec issue 2). Fixture trees
// (tests/support/mdx_fixture.hpp) in a temporary directory, served by the fixture transport with
// the instant clock, as download_test.cpp does. A test-local FakeResolver stands in for the app's
// resolver: it records every call and returns a scripted choice.
//
// Covers: the flag off is the old path byte for byte (and never asks the resolver); the plain stem
// with a null resolver; a resolver's suffixed name and its warning; the read-only pre-flight
// (title directory a file -> Errc::row_dir, DIR a file -> Errc::failure); dry runs create nothing;
// real runs create both levels; empty plans, unknown titles and interrupts never resolve; a stop
// during the execute phase reaches its copied context and leaves nothing behind; a
// resolver that breaks its contract is an internal error and nothing is created.

#include "fs/output_dir_testing.hpp" // core-private seam (src/core is on the test include path)
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/title_dir.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): SIGINT is POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view clean_stem = "Clean Title";
constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";
constexpr std::string_view unknown_title = "66666666-6666-4666-8666-666666666666";

// ---- test doubles ------------------------------------------------------------------------------

/// One recorded resolve() call: owned copies (the stem is borrowed for the call only).
struct ResolveCall {
    std::string stem{};
    std::string manga{};
    friend bool operator==(const ResolveCall&, const ResolveCall&) = default;
};

/// Records every call and returns `choice` (nullopt: the stem as given, no warning).
class FakeResolver final : public md::TitleDirResolver {
public:
    explicit FakeResolver(std::optional<md::TitleDirChoice> choice = std::nullopt)
        : choice_(std::move(choice)) {}

    [[nodiscard]] md::TitleDirChoice resolve(std::string_view stem,
                                             const md::MangaId& manga) override {
        calls_.push_back(ResolveCall{.stem = std::string{stem}, .manga = std::string{manga.str()}});
        return choice_.value_or(md::TitleDirChoice{.name = std::string{stem}});
    }

    [[nodiscard]] const std::vector<ResolveCall>& calls() const noexcept { return calls_; }

private:
    std::optional<md::TitleDirChoice> choice_{};
    std::vector<ResolveCall> calls_{};
};

/// Every observer call as one line, in order ("warning: ...", "verbose1: ...", "end: <path>",
/// ...), plus optional hooks run at the end of on_plan (the last plan-phase observer call that
/// precedes the title directory step) and of on_epub_begin (inside the execute phase).
class EventObserver final : public md::DownloadObserver {
public:
    EventObserver() = default;

    void set_on_plan(std::function<void()> hook) { on_plan_ = std::move(hook); }
    void set_on_epub_begin(std::function<void()> hook) { on_epub_begin_ = std::move(hook); }

    void on_plan(const md::Plan& plan) override {
        events_.push_back("plan: " + plan.file_stem);
        if (on_plan_) {
            on_plan_();
        }
    }
    void on_epub_begin(const md::PlannedEpub& epub, std::size_t index, std::size_t total) override {
        CHECK(index < total);
        events_.push_back("begin: " + epub.file_name);
        if (on_epub_begin_) {
            on_epub_begin_();
        }
    }
    void on_pages(std::size_t done, std::size_t total) override { CHECK(done <= total); }
    void on_epub_end(const md::EpubOutcome& outcome) override {
        events_.push_back("end: " + outcome.path);
    }
    void on_warning(std::string_view message) override {
        events_.push_back("warning: " + std::string{message});
    }
    void on_verbose(int level, std::string_view message) override {
        events_.push_back("verbose" + std::to_string(level) + ": " + std::string{message});
    }

    [[nodiscard]] const std::vector<std::string>& events() const noexcept { return events_; }

    /// Every event except the level-2 traces, in order.
    [[nodiscard]] std::vector<std::string> ordered_events() const {
        std::vector<std::string> out{};
        std::ranges::copy_if(events_, std::back_inserter(out),
                             [](const std::string& e) { return !e.starts_with(trace_prefix); });
        return out;
    }

    /// The level-2 traces, sorted.
    [[nodiscard]] std::vector<std::string> sorted_traces() const {
        std::vector<std::string> out = starting_with(trace_prefix);
        std::ranges::sort(out);
        return out;
    }

    /// Events starting with `prefix`.
    [[nodiscard]] std::vector<std::string> starting_with(std::string_view prefix) const {
        std::vector<std::string> out{};
        std::ranges::copy_if(events_, std::back_inserter(out),
                             [prefix](const std::string& e) { return e.starts_with(prefix); });
        return out;
    }

    /// Index of the first event starting with `prefix`, or events().size().
    [[nodiscard]] std::size_t first(std::string_view prefix) const {
        const auto it = std::ranges::find_if(
            events_, [prefix](const std::string& e) { return e.starts_with(prefix); });
        return static_cast<std::size_t>(std::distance(events_.begin(), it));
    }

private:
    static constexpr std::string_view trace_prefix = "verbose2: ";

    std::function<void()> on_plan_{};
    std::function<void()> on_epub_begin_{};
    std::vector<std::string> events_{};
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
/// directories go next to the tree (`<tmp>/<name>`).
class Harness {
public:
    explicit Harness(const mdx::FixtureManga& manga)
        : id_(manga.id), tmp_(make_temp_dir()), root_(tmp_.path() / "fixture") {
        const auto written = mdx::write_fixture_tree(root_, manga);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        auto transport = md::make_fixture_transport(root_);
        REQUIRE(transport.has_value());
        transport_ = std::move(*transport);
    }

    /// `<tmp>/<name>`, not created.
    [[nodiscard]] std::filesystem::path out(std::string_view name = "out") const {
        return tmp_.path() / name;
    }

    /// Options for this fixture's title (or `manga_id`), writing into out(dir_name).
    [[nodiscard]] md::DownloadOptions options(bool dir_per_title, std::string_view dir_name = "out",
                                              std::optional<std::string_view> manga_id = {}) const {
        auto id = md::MangaId::parse(manga_id.value_or(id_));
        REQUIRE(id.has_value());
        auto base = md::ApiBase::parse("file://" + root_.string());
        REQUIRE(base.has_value());
        return md::DownloadOptions{
            .manga = std::move(*id),
            .dir = out(dir_name),
            .api_base = std::move(*base),
            .report = false,
            .dir_per_title = dir_per_title,
        };
    }

    /// DownloadServices filled member by member, as run_title does.
    [[nodiscard]] md::Result<md::RunReport> run(const md::DownloadOptions& options,
                                                EventObserver& observer,
                                                md::TitleDirResolver* resolver = nullptr,
                                                std::stop_source stop = {}) {
        md::DownloadServices services{};
        services.transport = transport_.get();
        services.clock = clock_.get();
        services.stop = std::move(stop);
        services.user_agent = "mangadex-dl/test";
        services.title_dir = resolver;
        return md::run_download(options, services, observer);
    }

private:
    [[nodiscard]] static md::test::TempDir make_temp_dir() {
        auto tmp = md::test::TempDir::create("mangadex-dl-dir-per-title");
        REQUIRE(tmp.has_value());
        return std::move(*tmp);
    }

    std::string id_{};
    md::test::TempDir tmp_; ///< no default constructor: initialized by the constructor
    std::filesystem::path root_{};
    std::unique_ptr<md::Transport> transport_{};
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

/// Every path below `dir`, relative to it, sorted (a regular file's size appended), so a
/// "nothing changed" check covers names, nesting and contents' sizes.
[[nodiscard]] std::vector<std::string> list_tree(const std::filesystem::path& dir) {
    std::vector<std::string> names{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        std::string name = std::filesystem::relative(entry.path(), dir).string();
        if (entry.is_regular_file()) {
            name += " (" + std::to_string(entry.file_size()) + ")";
        }
        names.push_back(std::move(name));
    }
    std::ranges::sort(names);
    return names;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    REQUIRE(out.good());
}

[[nodiscard]] std::vector<md::EpubStatus> statuses(const md::RunReport& report) {
    std::vector<md::EpubStatus> out{};
    out.reserve(report.epubs.size());
    for (const md::EpubOutcome& outcome : report.epubs) {
        out.push_back(outcome.status);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> paths(const md::RunReport& report) {
    std::vector<std::string> out{};
    out.reserve(report.epubs.size());
    for (const md::EpubOutcome& outcome : report.epubs) {
        out.push_back(outcome.path);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> clean_paths(const std::filesystem::path& dir) {
    return {(dir / clean_vol1).string(), (dir / clean_vol2).string()};
}

[[nodiscard]] std::vector<std::string> clean_files() {
    return {std::string{clean_vol1}, std::string{clean_vol2}};
}

/// U+202E RIGHT-TO-LEFT OVERRIDE as UTF-8, built at run time: GCC's -Wbidi-chars=any and
/// clang-tidy's misc-misleading-bidirectional reject bidi controls in literals, escaped or not.
[[nodiscard]] std::string right_to_left_override() {
    return std::string{static_cast<char>(0xE2), static_cast<char>(0x80), static_cast<char>(0xAE)};
}

[[nodiscard]] std::string output_dir_line(const std::filesystem::path& dir) {
    return "verbose1: output directory: " + dir.string();
}

} // namespace

// ---- the flag off: the old path ----------------------------------------------------------------

TEST_CASE("download dir per title: off is the old path byte for byte and never resolves",
          "[download][dir_per_title]") {
    const bool dry_run = GENERATE(false, true);
    INFO("dry run: " << dry_run);
    Harness h{mdx::clean_fixture()};
    md::DownloadOptions options = h.options(false);
    options.dry_run = dry_run;

    EventObserver plain_observer;
    const auto plain = h.run(options, plain_observer);
    REQUIRE(plain.has_value());
    const std::vector<std::string> plain_tree = list_tree(h.out());
    std::error_code ec;
    static_cast<void>(std::filesystem::remove_all(h.out(), ec));
    REQUIRE_FALSE(ec);

    // A resolver handed in with the flag off is never consulted.
    FakeResolver resolver{md::TitleDirChoice{.name = "Elsewhere", .warning = "never printed"}};
    EventObserver observer;
    const auto result = h.run(options, observer, &resolver);
    REQUIRE(result.has_value());

    CHECK(md::report_to_json(*result) == md::report_to_json(*plain));
    CHECK(paths(*result) == clean_paths(h.out()));
    // The same observer calls: in order, except the -vv request traces, which FetchPool workers
    // queue in whatever order they finish.
    CHECK(observer.ordered_events() == plain_observer.ordered_events());
    CHECK(observer.sorted_traces() == plain_observer.sorted_traces());
    CHECK(resolver.calls().empty());
    CHECK(observer.starting_with("verbose1: output directory").empty());
    CHECK(list_tree(h.out()) == plain_tree);
    if (dry_run) {
        CHECK_FALSE(std::filesystem::exists(h.out()));
    } else {
        CHECK(list_dir(h.out()) == clean_files());
    }
}

// ---- the flag on -------------------------------------------------------------------------------

TEST_CASE("download dir per title: a null resolver puts the files in DIR/<stem>, creating both",
          "[download][dir_per_title]") {
    Harness h{mdx::clean_fixture()};
    REQUIRE_FALSE(std::filesystem::exists(h.out()));
    EventObserver observer;
    const auto result = h.run(h.options(true), observer);
    REQUIRE(result.has_value());

    const std::filesystem::path title_dir = h.out() / clean_stem;
    CHECK(md::exit_code_for(*result) == 0);
    CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
    CHECK(paths(*result) == clean_paths(title_dir));
    // Both levels were created (V13); nothing else, and no temporaries left behind.
    CHECK(list_dir(h.out()) == std::vector<std::string>{std::string{clean_stem}});
    CHECK(list_dir(title_dir) == clean_files());
    for (const md::EpubOutcome& outcome : result->epubs) {
        const auto archive = zip::read_archive_file(outcome.path);
        INFO((archive ? std::string{} : archive.error()));
        REQUIRE(archive.has_value());
        CHECK(archive->find("OEBPS/content.opf") != nullptr);
    }
    // Exactly one -v line, after the plan and before the first file.
    CHECK(observer.starting_with("verbose1: output directory") ==
          std::vector{output_dir_line(title_dir)});
    CHECK(observer.first("plan: ") < observer.first("verbose1: output directory"));
    CHECK(observer.first("verbose1: output directory") < observer.first("begin: "));
    CHECK(observer.starting_with("warning: ").empty());
}

TEST_CASE("download dir per title: a re-run finds the files in the title directory",
          "[download][dir_per_title]") {
    Harness h{mdx::clean_fixture()};
    {
        EventObserver first;
        REQUIRE(h.run(h.options(true), first).has_value());
    }
    const std::filesystem::path title_dir = h.out() / clean_stem;
    const std::vector<std::string> before = list_tree(h.out());
    const bool dry_run = GENERATE(false, true);
    INFO("dry run: " << dry_run);
    md::DownloadOptions options = h.options(true);
    options.dry_run = dry_run;
    EventObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());
    CHECK(statuses(*result) == std::vector{md::EpubStatus::exists, md::EpubStatus::exists});
    CHECK(paths(*result) == clean_paths(title_dir));
    CHECK(list_tree(h.out()) == before);
}

TEST_CASE("download dir per title: the resolver's name and warning are used",
          "[download][dir_per_title]") {
    const bool dry_run = GENERATE(false, true);
    INFO("dry run: " << dry_run);
    Harness h{mdx::clean_fixture()};
    FakeResolver resolver{md::TitleDirChoice{
        .name = "Clean Title [77777777]",
        .warning = "another title in this list is already named 'Clean Title'",
    }};
    md::DownloadOptions options = h.options(true);
    options.dry_run = dry_run;
    EventObserver observer;
    const auto result = h.run(options, observer, &resolver);
    REQUIRE(result.has_value());

    CHECK(resolver.calls() == std::vector{ResolveCall{.stem = std::string{clean_stem},
                                                      .manga = std::string{clean_id}}});
    const std::filesystem::path title_dir = h.out() / "Clean Title [77777777]";
    CHECK(paths(*result) == clean_paths(title_dir));
    if (dry_run) {
        CHECK(statuses(*result) == std::vector{md::EpubStatus::planned, md::EpubStatus::planned});
        CHECK_FALSE(std::filesystem::exists(h.out()));
    } else {
        CHECK(statuses(*result) == std::vector{md::EpubStatus::written, md::EpubStatus::written});
        CHECK(list_dir(h.out()) == std::vector<std::string>{"Clean Title [77777777]"});
        CHECK(list_dir(title_dir) == clean_files());
    }
    // The warning once, before the -v line and before any file ends.
    CHECK(observer.starting_with("warning: ") ==
          std::vector<std::string>{
              "warning: another title in this list is already named 'Clean Title'"});
    CHECK(observer.starting_with("verbose1: output directory") ==
          std::vector{output_dir_line(title_dir)});
    CHECK(observer.first("warning: ") < observer.first("verbose1: output directory"));
    CHECK(observer.first("warning: ") < observer.first("end: "));
}

TEST_CASE("download dir per title: a dry run creates neither DIR nor the title directory",
          "[download][dir_per_title]") {
    Harness h{mdx::clean_fixture()};
    const bool dir_exists = GENERATE(false, true);
    INFO("DIR exists: " << dir_exists);
    if (dir_exists) {
        REQUIRE(std::filesystem::create_directory(h.out()));
    }
    md::DownloadOptions options = h.options(true);
    options.dry_run = true;
    EventObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE(result.has_value());

    const std::filesystem::path title_dir = h.out() / clean_stem;
    CHECK(result->dry_run);
    CHECK(statuses(*result) == std::vector{md::EpubStatus::planned, md::EpubStatus::planned});
    CHECK(paths(*result) == clean_paths(title_dir));
    CHECK(observer.starting_with("verbose1: output directory") ==
          std::vector{output_dir_line(title_dir)});
    CHECK(std::filesystem::exists(h.out()) == dir_exists);
    CHECK_FALSE(std::filesystem::exists(title_dir));
}

// ---- the pre-flight ----------------------------------------------------------------------------

TEST_CASE("download dir per title: a title directory that is a file fails the title",
          "[download][dir_per_title]") {
    const bool dry_run = GENERATE(false, true);
    INFO("dry run: " << dry_run);
    Harness h{mdx::clean_fixture()};
    REQUIRE(std::filesystem::create_directory(h.out()));
    write_text(h.out() / clean_stem, "not a directory\n");
    const std::vector<std::string> before = list_tree(h.out());

    md::DownloadOptions options = h.options(true);
    options.dry_run = dry_run;
    EventObserver observer;
    const auto result = h.run(options, observer);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::row_dir);
    CHECK(result.error().exit_code() == 1);
    CHECK(result.error().message ==
          "cannot open directory '" + (h.out() / clean_stem).string() + "': Not a directory");
    CHECK(list_tree(h.out()) == before);
    CHECK(observer.starting_with("end: ").empty());
    CHECK(observer.starting_with("begin: ").empty());
}

TEST_CASE("download dir per title: DIR that is a file is a global failure",
          "[download][dir_per_title]") {
    const bool dry_run = GENERATE(false, true);
    INFO("dry run: " << dry_run);
    Harness h{mdx::clean_fixture()};
    write_text(h.out(), "DIR is a file\n");
    FakeResolver resolver{};

    md::DownloadOptions options = h.options(true);
    options.dry_run = dry_run;
    EventObserver observer;
    const auto result = h.run(options, observer, &resolver);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::failure);
    CHECK(result.error().message ==
          "cannot open directory '" + h.out().string() + "': Not a directory");
    CHECK(resolver.calls().size() == 1);
    CHECK(std::filesystem::is_regular_file(h.out()));
    CHECK(std::filesystem::file_size(h.out()) == std::string_view{"DIR is a file\n"}.size());
    CHECK(observer.starting_with("end: ").empty());
}

// ---- no resolution -----------------------------------------------------------------------------

TEST_CASE("download dir per title: an empty plan claims no directory",
          "[download][dir_per_title]") {
    const bool dry_run = GENERATE(false, true);
    INFO("dry run: " << dry_run);
    Harness h{mdx::nochapters_fixture()};
    FakeResolver resolver{};
    md::DownloadOptions options = h.options(true);
    options.dry_run = dry_run;
    EventObserver observer;
    const auto result = h.run(options, observer, &resolver);
    REQUIRE(result.has_value());
    CHECK(result->epubs.empty());
    CHECK(md::exit_code_for(*result) == 5);
    CHECK(resolver.calls().empty());
    CHECK(observer.starting_with("verbose1: output directory").empty());
    CHECK_FALSE(std::filesystem::exists(h.out()));
}

TEST_CASE("download dir per title: an unknown title never resolves", "[download][dir_per_title]") {
    Harness h{mdx::clean_fixture()};
    FakeResolver resolver{};
    EventObserver observer;
    const auto result = h.run(h.options(true, "out", unknown_title), observer, &resolver);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::not_found);
    CHECK(resolver.calls().empty());
    CHECK(observer.starting_with("verbose1: output directory").empty());
    CHECK_FALSE(std::filesystem::exists(h.out()));
}

TEST_CASE("download dir per title: a stop after the plan never resolves",
          "[download][dir_per_title]") {
    const InterruptFlagReset reset;
    const bool by_signal = GENERATE(true, false);
    INFO("by signal: " << by_signal);
    Harness h{mdx::clean_fixture()};
    FakeResolver resolver{};
    const std::stop_source stop{};
    EventObserver observer;
    // on_plan is the last plan-phase call before the title directory step. `source` is an
    // init-capture copy of `stop` (a copy shares the stop state) and, the lambda being mutable,
    // a non-const member: request_stop is a non-const member function in the standard.
    observer.set_on_plan([by_signal, source = stop] mutable {
        if (by_signal) {
            md::simulate_interrupt_for_testing(SIGINT);
        } else {
            static_cast<void>(source.request_stop());
        }
    });
    const auto result = h.run(h.options(true), observer, &resolver, stop);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::interrupted);
    CHECK(md::interrupt_requested() == by_signal);
    CHECK(resolver.calls().empty());
    CHECK(observer.starting_with("verbose1: output directory").empty());
    CHECK_FALSE(std::filesystem::exists(h.out()));
}

TEST_CASE("download dir per title: a stop during the execute phase leaves nothing behind",
          "[download][dir_per_title]") {
    const InterruptFlagReset reset;
    const bool by_signal = GENERATE(false, true);
    const bool fallback = GENERATE(false, true);
    INFO("by signal: " << by_signal << ", fallback temporaries: " << fallback);
    std::optional<md::testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&md::testing::force_fallback_tempfiles);
    }
    Harness h{mdx::clean_fixture()};
    const std::stop_source stop{};
    EventObserver observer;
    // The execute phase runs on a copy of the run's context (title_ctx in run_download): the
    // caller's stop must still reach it through the copied stop_source's shared state. `source`
    // is an init-capture copy and a non-const member of the mutable lambda (request_stop is
    // non-const in the standard).
    observer.set_on_epub_begin([by_signal, source = stop] mutable {
        if (by_signal) {
            md::simulate_interrupt_for_testing(SIGINT);
        } else {
            static_cast<void>(source.request_stop());
        }
    });
    const auto result = h.run(h.options(true), observer, nullptr, stop);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::interrupted);
    CHECK(md::interrupt_requested() == by_signal);
    CHECK(stop.stop_requested());
    CHECK(observer.starting_with("begin: ").size() == 1);
    CHECK(observer.starting_with("end: ").empty());
    // DIR/<name> was created by the execute phase; no partial file or temporary is left in it.
    const std::filesystem::path title_dir = h.out() / clean_stem;
    CHECK(list_dir(h.out()) == std::vector<std::string>{std::string{clean_stem}});
    CHECK(std::filesystem::is_directory(title_dir));
    CHECK(list_dir(title_dir).empty());
}

// ---- a resolver that breaks its contract -------------------------------------------------------

TEST_CASE("download dir per title: an unsafe name from the resolver is an internal error",
          "[download][dir_per_title]") {
    const std::string name = GENERATE(std::string{"../x"}, std::string{"a/b"}, std::string{".."},
                                      std::string{"."}, std::string{""}, std::string{"/abs"},
                                      std::string{" padded "}, std::string{"bad\x01name"},
                                      std::string{"bidi"} + right_to_left_override() + "name",
                                      std::string{"bad\xff"}, std::string(256, 'x'));
    const bool dir_exists = GENERATE(false, true);
    INFO("name: " << name << ", DIR exists: " << dir_exists);
    Harness h{mdx::clean_fixture()};
    if (dir_exists) {
        REQUIRE(std::filesystem::create_directory(h.out()));
    }
    FakeResolver resolver{md::TitleDirChoice{.name = name, .warning = "never printed"}};
    EventObserver observer;
    const auto result = h.run(h.options(true), observer, &resolver);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == md::Errc::failure);
    CHECK_THAT(result.error().message, StartsWith("internal error: "));
    if (!name.empty()) {
        CHECK_THAT(result.error().message, !ContainsSubstring(name)); // never echoes the name
    }
    CHECK(resolver.calls().size() == 1);
    // Nothing was printed for the broken choice, and nothing was created anywhere.
    CHECK(observer.starting_with("warning: ").empty());
    CHECK(observer.starting_with("verbose1: output directory").empty());
    CHECK(std::filesystem::exists(h.out()) == dir_exists);
    CHECK(list_tree(h.out()).empty());
    CHECK_FALSE(std::filesystem::exists(h.out().parent_path() / "x"));
}
