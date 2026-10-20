// tests/unit/interfaces/headers_test.cpp: the frozen interface headers (download plan T1, batch
// plan T1) compile together, and their header-inline parts behave as documented: the exit-code
// mapping, the limits, the defaults of every option/config aggregate, the ownership shape of every
// class, and UniqueFd. Everything declared out of line is defined by later tasks, so it is only
// checked in unevaluated contexts here (signatures), never called.
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/fetch_pool.hpp"
#include "mangadex_dl/filename.hpp"
#include "mangadex_dl/fs/bounded_read.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/row_dir.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/curl_transport.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/planner.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/selection.hpp"
#include "mangadex_dl/text_safety.hpp"
#include "mangadex_dl/zip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace md = mangadex_dl;

// ---- exit codes ------------------------------------------------------------------------------
static_assert(md::exit_code_for(md::Errc::network) == 3);
static_assert(md::exit_code_for(md::Errc::not_found) == 4);
static_assert(md::exit_code_for(md::Errc::no_chapters) == 5);
static_assert(md::exit_code_for(md::Errc::incomplete) == 6);
// main() re-raises the signal; the code is only a fallback.
static_assert(md::exit_code_for(md::Errc::interrupted) == md::exit_code::failure);
// The pre-existing categories are unchanged.
static_assert(md::exit_code_for(md::Errc::failure) == md::exit_code::failure);
static_assert(md::exit_code_for(md::Errc::io) == md::exit_code::failure);
static_assert(md::exit_code_for(md::Errc::usage) == md::exit_code::usage);
// batch: titles that failed in more than one way.
static_assert(md::exit_code::mixed_failures == 7);
static_assert(md::exit_code_for(md::Errc::mixed_failures) == 7);

// ---- limits ----------------------------------------------------------------------------------
static_assert(md::zip_max_entries == 65534 && md::max_zip_entries_per_epub == 65534);
static_assert(md::zip_max_u32 == 0xFFFFFFFE);
static_assert(md::max_image_side == 65535);
static_assert(md::max_api_json_bytes == std::size_t{16} * 1024 * 1024);
static_assert(md::max_image_bytes == std::size_t{32} * 1024 * 1024);
static_assert(md::max_pages_per_chapter == 5000);
static_assert(md::feed_page_limit == 500 && md::feed_offset_cap == 10000);
static_assert(md::cover_page_limit == 100 && md::max_json_depth == 64);
static_assert(md::default_api_base == "https://api.mangadex.org");
static_assert(md::ca_bundle_candidates.size() == 6);
static_assert(md::ca_bundle_candidates.front() == "/etc/ssl/certs/ca-certificates.crt");
static_assert(md::max_list_bytes == std::size_t{1024} * 1024);
static_assert(md::max_list_titles == 10000 && md::max_list_field_bytes == 4096);
static_assert(md::max_list_errors == 20 && md::max_list_echo_bytes == 32);
static_assert(md::network_streak_limit == 3);

// ---- value types are aggregates (designated initializers, defaulted comparisons) ------------
static_assert(std::is_aggregate_v<md::Chapter> && std::is_aggregate_v<md::Cover>);
static_assert(std::is_aggregate_v<md::PlannedEpub> && std::is_aggregate_v<md::Plan>);
// The planner marks an unbuildable file (decision 6); the downloader reports it as failed.
static_assert(std::is_same_v<decltype(md::PlannedEpub::plan_error), std::optional<std::string>>);
static_assert(std::is_aggregate_v<md::RunReport> && std::is_aggregate_v<md::EpubOutcome>);
static_assert(std::is_aggregate_v<md::DownloadOptions> && std::is_aggregate_v<md::RawDownloadArgs>);
static_assert(std::is_aggregate_v<md::HttpRequest> && std::is_aggregate_v<md::RetryConfig>);
static_assert(std::is_aggregate_v<md::EpubMetadata> && std::is_aggregate_v<md::EpubNavEntry>);
static_assert(std::is_aggregate_v<md::FetchedPage> && std::is_aggregate_v<md::DownloadServices>);
static_assert(std::is_aggregate_v<md::ListRecord> && std::is_aggregate_v<md::TitleList>);
static_assert(std::is_aggregate_v<md::ListErrors> && std::is_aggregate_v<md::RowDirVerdict>);
static_assert(std::is_aggregate_v<md::RawBatchArgs> && std::is_aggregate_v<md::BatchSettings>);
static_assert(std::is_aggregate_v<md::BatchJob> && std::is_aggregate_v<md::RowOutcome>);
static_assert(std::is_aggregate_v<md::TitleResult> && std::is_aggregate_v<md::BatchResult>);
static_assert(std::is_aggregate_v<md::Utf8Decoded>);
static_assert(std::is_same_v<md::ListResult<int>, std::expected<int, md::ListErrors>>);

// ---- abstract seams: polymorphic bases that can be neither copied nor sliced ----------------
template <class T>
constexpr bool is_interface_v =
    std::is_abstract_v<T> && std::has_virtual_destructor_v<T> && !std::is_copy_constructible_v<T> &&
    !std::is_move_constructible_v<T>;
static_assert(is_interface_v<md::Transport> && is_interface_v<md::Clock>);
static_assert(is_interface_v<md::TraceSink> && is_interface_v<md::DownloadObserver>);
static_assert(is_interface_v<md::ByteSink>); // its moves are protected, for move-only sinks
static_assert(is_interface_v<md::TitleRunner>);

// ---- ownership shapes ------------------------------------------------------------------------
template <class T>
constexpr bool is_pinned_v = !std::is_copy_constructible_v<T> && !std::is_move_constructible_v<T>;
static_assert(is_pinned_v<md::ZipWriter> && is_pinned_v<md::EpubWriter>);
static_assert(is_pinned_v<md::ApiClient> && is_pinned_v<md::FetchPool>);
static_assert(is_pinned_v<md::RateLimiter> && is_pinned_v<md::InterruptBridge>);

template <class T>
constexpr bool is_move_only_v =
    !std::is_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T>;
static_assert(is_move_only_v<md::UniqueFd> && is_move_only_v<md::TempFile>);
static_assert(is_move_only_v<md::OutputDir> && is_move_only_v<md::ScopedInterruptHandlers>);

static_assert(std::is_final_v<md::MemorySink> && std::is_base_of_v<md::ByteSink, md::MemorySink>);
static_assert(std::is_final_v<md::TempFile> && std::is_base_of_v<md::ByteSink, md::TempFile>);
// Types with an invariant are only made by their factories.
static_assert(!std::is_default_constructible_v<md::ChapterNumber>);
static_assert(!std::is_default_constructible_v<md::TempFile>);
static_assert(!std::is_default_constructible_v<md::OutputDir>);
static_assert(!std::is_default_constructible_v<md::ScopedInterruptHandlers>);
// The collision registry owns its keys and starts empty.
static_assert(std::is_nothrow_default_constructible_v<md::OutputRegistry>);

// ---- frozen signatures of out-of-line functions (unevaluated: nothing is linked) ------------
static_assert(std::is_same_v<decltype(&md::run_download),
                             md::Result<md::RunReport> (*)(const md::DownloadOptions&,
                                                           const md::DownloadServices&,
                                                           md::DownloadObserver&)>);
static_assert(
    std::is_same_v<decltype(&md::parse_retry_after),
                   std::optional<std::chrono::milliseconds> (*)(
                       std::string_view, std::string_view, std::chrono::system_clock::time_point)>);
static_assert(
    std::is_same_v<decltype(&md::decide_retry),
                   md::RetryDecision (*)(const md::AttemptOutcome&, const md::RetryConfig&,
                                         std::chrono::milliseconds) noexcept>);
static_assert(
    std::is_same_v<decltype(&md::make_fixture_transport),
                   md::Result<std::unique_ptr<md::Transport>> (*)(std::filesystem::path)>);
static_assert(std::is_same_v<decltype(&md::crc32), std::uint32_t (*)(std::span<const std::byte>,
                                                                     std::uint32_t) noexcept>);
static_assert(std::is_same_v<decltype(&md::sanitize_file_stem),
                             std::string (*)(std::string_view, std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::build_plan),
                   md::Result<md::Plan> (*)(const md::PlanInput&, const md::PlanOptions&)>);
static_assert(std::is_same_v<decltype(&md::validate_download_args),
                             md::Result<md::DownloadOptions> (*)(const md::RawDownloadArgs&)>);
static_assert(std::is_same_v<decltype(&md::sniff_image),
                             md::Result<md::ImageInfo> (*)(std::span<const std::byte>)>);
static_assert(
    std::is_same_v<decltype(&md::format_modified), md::Result<std::string> (*)(std::string_view)>);
static_assert(std::is_same_v<decltype(&md::parse_at_home),
                             md::Result<md::AtHomeServer> (*)(std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::endpoints_for), md::Endpoints (*)(const md::ApiBase&, bool)>);
static_assert(std::is_same_v<decltype(&md::is_uuid), bool (*)(std::string_view) noexcept>);
static_assert(std::is_same_v<decltype(&md::normalize_label), std::string (*)(std::string_view)>);
static_assert(std::is_same_v<decltype(&md::interrupt_requested), bool (*)() noexcept>);
static_assert(
    std::is_same_v<decltype(&md::FetchPool::run),
                   md::Result<void> (md::FetchPool::*)(std::size_t, const md::PageFetcher&,
                                                       const md::PageConsumer&, std::stop_token)>);
// batch (T2, T3a, T3b).
static_assert(std::is_same_v<decltype(&md::read_bounded_file),
                             md::Result<std::string> (*)(const std::filesystem::path&,
                                                         std::string_view, std::size_t)>);
static_assert(std::is_same_v<decltype(&md::read_bounded_fd),
                             md::Result<std::string> (*)(int, std::string_view, std::size_t)>);
static_assert(
    std::is_same_v<decltype(&md::check_row_dir),
                   md::RowDirVerdict (*)(const std::filesystem::path&, std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::classify_dir_errno), md::RowDirKind (*)(int, bool) noexcept>);
static_assert(
    std::is_same_v<decltype(&md::parse_title_list),
                   md::ListResult<md::TitleList> (*)(std::string_view, std::string_view)>);
static_assert(std::is_same_v<decltype(&md::validate_batch_args),
                             md::Result<md::BatchSettings> (*)(const md::RawBatchArgs&)>);
static_assert(
    std::is_same_v<decltype(&md::plan_batch), md::ListResult<std::vector<md::BatchJob>> (*)(
                                                  const md::TitleList&, const md::BatchSettings&)>);
static_assert(std::is_same_v<decltype(&md::validate_list_dir),
                             md::Result<std::string> (*)(std::string_view)>);
static_assert(std::is_same_v<decltype(&md::join_list_dir),
                             std::string (*)(std::string_view, std::string_view)>);
static_assert(std::is_same_v<decltype(&md::title_status_for), md::TitleStatus (*)(int) noexcept>);
static_assert(std::is_same_v<decltype(&md::title_status_name),
                             std::string_view (*)(md::TitleStatus) noexcept>);
static_assert(
    std::is_same_v<decltype(&md::run_batch),
                   md::BatchResult (*)(std::span<const md::BatchJob>, md::TitleRunner&, bool)>);
static_assert(
    std::is_same_v<decltype(&md::batch_exit_code), int (*)(const md::BatchResult&) noexcept>);
static_assert(std::is_same_v<decltype(&md::batch_closing_message),
                             std::optional<std::string> (*)(const md::BatchResult&, bool)>);
static_assert(
    std::is_same_v<decltype(&md::exists_notice), std::optional<std::string> (*)(std::size_t)>);
static_assert(std::is_same_v<decltype(&md::batch_to_json),
                             std::string (*)(const md::BatchResult&, std::string_view, bool)>);
static_assert(
    std::is_same_v<decltype(&md::OutputRegistry::claim),
                   std::optional<std::size_t> (md::OutputRegistry::*)(std::string, std::size_t)>);
static_assert(std::is_same_v<decltype(&md::TitleRunner::run),
                             md::RowOutcome (md::TitleRunner::*)(const md::BatchJob&, std::size_t,
                                                                 std::size_t)>);
// exit_code_for is overloaded (Errc and RunReport), so check the RunReport overload by call.
static_assert(
    std::is_same_v<decltype(md::exit_code_for(std::declval<const md::RunReport&>())), int>);
static_assert(noexcept(md::exit_code_for(std::declval<const md::RunReport&>())));

/// A borrower written the way the headers write theirs: the shim expands to a valid attribute
/// (Clang) or to nothing (GCC) after the qualifiers.
struct Borrower {
    std::string text{};
    [[nodiscard]] const std::string& get() const noexcept MANGADEX_DL_LIFETIMEBOUND { return text; }
};

} // namespace

TEST_CASE("Error and Failure map the download categories to exit codes 3-6", "[interfaces]") {
    CHECK(md::Error{.code = md::Errc::network}.exit_code() == md::exit_code::network);
    CHECK(md::Error{.code = md::Errc::not_found}.exit_code() == md::exit_code::not_found);
    CHECK(md::Error{.code = md::Errc::no_chapters}.exit_code() == md::exit_code::no_chapters);
    CHECK(md::Error{.code = md::Errc::incomplete}.exit_code() == md::exit_code::incomplete);
    CHECK(md::Error{.code = md::Errc::interrupted}.exit_code() == md::exit_code::failure);

    const md::Failure failure{md::Error{.code = md::Errc::not_found, .message = "no such title"}};
    CHECK(failure.exit_code() == 4);
    CHECK(failure.error() == md::Error{.code = md::Errc::not_found, .message = "no such title"});
}

TEST_CASE("A planned epub is buildable unless the planner marks it", "[interfaces]") {
    const md::PlannedEpub buildable{};
    CHECK_FALSE(buildable.plan_error.has_value());
    CHECK(buildable.planned_pages == 0);

    const md::PlannedEpub too_big{.planned_pages = 40000, .plan_error = "too many zip entries"};
    CHECK(too_big.plan_error == std::optional<std::string>{"too many zip entries"});
    CHECK(too_big != buildable);
}

TEST_CASE("Header-inline value defaults need no out-of-line definitions", "[interfaces]") {
    const md::Selection all = md::Selection::all();
    CHECK(all.is_all());
    CHECK_FALSE(all.is_none());
    CHECK(all == md::Selection::all());

    const md::MangaId nil{};
    CHECK(nil.str() == "00000000-0000-0000-0000-000000000000");
    CHECK(nil == md::MangaId{});

    const md::ApiBase official{};
    CHECK(official.kind() == md::ApiBase::Kind::official);
    CHECK(official.url() == md::default_api_base);
    CHECK(official.fixture_root().empty());
}

TEST_CASE("Option aggregates default to the documented CLI defaults", "[interfaces]") {
    const md::RawDownloadArgs raw{};
    CHECK(raw.dir == ".");
    CHECK(raw.volumes == "all");
    CHECK(raw.loose_chapters == "all");
    CHECK(raw.layout == "volumes");
    CHECK(raw.lang == "en");
    CHECK(raw.direction == "auto");
    CHECK(raw.jobs == 4);
    CHECK(raw.api_base == md::default_api_base);
    CHECK_FALSE(raw.no_report);

    const md::DownloadOptions options{};
    CHECK(options.manga == md::MangaId{});
    CHECK(options.dir == std::filesystem::path{"."});
    CHECK(options.volumes.is_all());
    CHECK(options.loose_chapters.is_all());
    CHECK(options.layout == md::Layout::volumes);
    CHECK(options.quality == md::ImageQuality::data);
    CHECK(options.direction == md::DirectionChoice::automatic);
    CHECK(options.jobs == 4U);
    CHECK(options.api_base == md::ApiBase{});
    CHECK(options.report);

    const md::PlanOptions plan{};
    CHECK(plan.volumes.is_all());
    CHECK(plan.loose_chapters.is_all());
    CHECK(plan.lang == "en");
}

TEST_CASE("Network config aggregates default to the documented policy", "[interfaces][net]") {
    const md::RetryConfig retry{};
    CHECK(retry.max_attempts == 4);
    CHECK(retry.max_attempts_429 == 2);
    CHECK(retry.base_delay == std::chrono::milliseconds{1000});
    CHECK(retry.max_jitter == std::chrono::milliseconds{500});
    CHECK(retry.retry_after_cap == std::chrono::seconds{60});
    CHECK(retry.default_429_delay == std::chrono::seconds{60});

    const md::HttpRequest request{};
    CHECK(request.method == md::HttpMethod::get);
    CHECK(request.connect_timeout == std::chrono::milliseconds{15000});
    CHECK(request.stall_timeout == std::chrono::seconds{30});
    CHECK_FALSE(request.cancel.stop_possible());

    const md::Endpoints endpoints{};
    CHECK(endpoints.api == md::default_api_base);
    CHECK(endpoints.uploads == "https://uploads.mangadex.org");
    CHECK_FALSE(endpoints.at_home_override.has_value());
    CHECK_FALSE(endpoints.report_url.has_value());

    const md::ClientConfig client{};
    CHECK(client.endpoints == endpoints);
    CHECK(client.retry == retry);
    CHECK(client.api_interval == std::chrono::milliseconds{250});
    CHECK(client.at_home_interval == std::chrono::milliseconds{1500});

    const md::DownloadServices services{};
    CHECK(services.transport == nullptr);
    CHECK(services.clock == nullptr);
    CHECK(services.stop.stop_possible());
    CHECK_FALSE(services.stop.stop_requested());
}

TEST_CASE("The lifetimebound shim compiles on accessors", "[interfaces]") {
    const Borrower borrower{.text = "kept"};
    CHECK(borrower.get() == "kept");
}

TEST_CASE("UniqueFd closes exactly once, when its last owner dies", "[interfaces][fs]") {
    // dup2(fd, fd) returns fd while fd is open and -1 (EBADF) once it is closed, without side
    // effects, so it observes ownership without touching any UniqueFd.
    const auto is_open = [](int fd) { return ::dup2(fd, fd) == fd; };

    SECTION("destruction closes") {
        const int raw = ::dup(STDERR_FILENO);
        REQUIRE(raw >= 0);
        {
            const md::UniqueFd owner{raw};
            CHECK(owner.valid());
            CHECK(owner.get() == raw);
            CHECK(is_open(raw));
        }
        CHECK_FALSE(is_open(raw));
    }

    SECTION("a moved-from owner does not close; the new owner does") {
        const int raw = ::dup(STDERR_FILENO);
        REQUIRE(raw >= 0);
        {
            md::UniqueFd assigned{};
            {
                md::UniqueFd owner{raw};
                md::UniqueFd constructed{std::move(owner)};
                assigned = std::move(constructed);
            }
            CHECK(assigned.get() == raw);
            CHECK(is_open(raw));
        }
        CHECK_FALSE(is_open(raw));
    }

    SECTION("move-assigning onto a live owner closes its old descriptor") {
        const int first = ::dup(STDERR_FILENO);
        REQUIRE(first >= 0);
        const int second = ::dup(STDERR_FILENO);
        REQUIRE(second >= 0);
        {
            md::UniqueFd target{first};
            md::UniqueFd source{second};
            target = std::move(source);
            CHECK_FALSE(is_open(first));
            CHECK(target.get() == second);
            // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state (-1) is the contract
            CHECK(source.get() == -1);
            CHECK(is_open(second));
        }
        CHECK_FALSE(is_open(second));
    }

    SECTION("self-move-assignment keeps the descriptor open and owned") {
        const int raw = ::dup(STDERR_FILENO);
        REQUIRE(raw >= 0);
        {
            md::UniqueFd owner{raw};
            md::UniqueFd& alias = owner; // through a reference, so -Wself-move does not apply
            owner = std::move(alias);
            CHECK(owner.get() == raw);
            CHECK(is_open(raw));
        }
        CHECK_FALSE(is_open(raw));
    }

    SECTION("reset to the descriptor already owned keeps it open and owned") {
        const int raw = ::dup(STDERR_FILENO);
        REQUIRE(raw >= 0);
        {
            md::UniqueFd owner{raw};
            owner.reset(owner.get());
            CHECK(owner.get() == raw);
            CHECK(is_open(raw));
        }
        CHECK_FALSE(is_open(raw));
    }

    SECTION("release hands ownership back; reset closes") {
        const int raw = ::dup(STDERR_FILENO);
        REQUIRE(raw >= 0);
        md::UniqueFd owner{raw};
        const int released = owner.release();
        CHECK(released == raw);
        CHECK_FALSE(owner.valid());
        CHECK(is_open(raw));

        owner.reset(released);
        CHECK(owner.get() == raw);
        owner.reset();
        CHECK(owner.get() == -1);
        CHECK_FALSE(is_open(raw));
    }
}

TEST_CASE("Error and Failure map the batch category to exit code 7", "[interfaces][batch]") {
    CHECK(md::Error{.code = md::Errc::mixed_failures}.exit_code() == md::exit_code::mixed_failures);

    const md::Failure failure{md::Error{.code = md::Errc::mixed_failures, .message = "mixed"}};
    CHECK(failure.exit_code() == 7);
    CHECK(failure.error() == md::Error{.code = md::Errc::mixed_failures, .message = "mixed"});
}

TEST_CASE("Validated download options compare member-wise", "[interfaces][batch]") {
    CHECK(md::DownloadOptions{} == md::DownloadOptions{});

    const md::DownloadOptions dry{.dry_run = true};
    CHECK(dry != md::DownloadOptions{});
    CHECK(dry == md::DownloadOptions{.dry_run = true});

    const md::DownloadOptions other_dir{.dir = "out"};
    CHECK(other_dir != md::DownloadOptions{});
    const md::DownloadOptions grouped{.groups = {"A", "B"}};
    CHECK(grouped != md::DownloadOptions{.groups = {"B", "A"}}); // priority order matters
    CHECK(md::DownloadOptions{.title = "T"} != md::DownloadOptions{});
}

TEST_CASE("Batch list aggregates default-construct and take partial designated init",
          "[interfaces][batch]") {
    const md::ListRecord record{};
    CHECK(record.line == 0);
    CHECK(record.manga.empty());
    CHECK_FALSE(record.title.has_value());
    CHECK_FALSE(record.dir.has_value());
    CHECK_FALSE(record.group.has_value());
    CHECK_FALSE(record.direction.has_value());

    const md::ListRecord partial{.line = 3, .manga = "m", .group = "A|B"};
    CHECK(partial.line == 3);
    CHECK(partial.group == std::optional<std::string>{"A|B"});
    CHECK_FALSE(partial.lang.has_value());
    CHECK(partial != record);

    const md::TitleList list{.records = {partial}};
    CHECK_FALSE(list.has_header);
    CHECK(list.records.size() == 1);
    CHECK(list == md::TitleList{.has_header = false, .records = {partial}});

    const md::ListErrors errors{.messages = {"f.csv:2: bad"}};
    CHECK(errors.more == 0);
    CHECK(errors == md::ListErrors{.messages = {"f.csv:2: bad"}, .more = 0});

    const md::ListResult<md::TitleList> failed{std::unexpect, errors};
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error() == errors);
}

TEST_CASE("Batch settings and jobs default to download's defaults", "[interfaces][batch]") {
    const md::RawBatchArgs raw{};
    CHECK(raw.file.empty());
    CHECK(raw.defaults.dir == ".");
    CHECK(raw.defaults.manga.empty());
    CHECK_FALSE(raw.defaults.title.has_value());
    CHECK_FALSE(raw.fail_fast);

    const md::RawBatchArgs stdin_args{.file = "-", .fail_fast = true};
    CHECK(stdin_args.file == "-");
    CHECK(stdin_args.fail_fast);
    CHECK(stdin_args.defaults.jobs == 4);

    const md::BatchSettings settings{.label = "<stdin>"};
    CHECK(settings.file.empty());
    CHECK(settings.label == "<stdin>");
    CHECK(settings.base == md::DownloadOptions{});
    CHECK_FALSE(settings.fail_fast);

    const md::BatchJob job{.line = 2, .dir = "out/sub", .sub_dir = "sub"};
    CHECK(job.line == 2);
    CHECK(job.options == md::DownloadOptions{});
    CHECK_FALSE(job.duplicate_of.has_value());
    CHECK(job.dir == "out/sub");
    CHECK(job.sub_dir == "sub");
}

TEST_CASE("Batch outcome aggregates default to an unrun, non-fatal row", "[interfaces][batch]") {
    const md::RowOutcome outcome{};
    CHECK(outcome.exit_code == 0);
    CHECK(outcome.error.empty());
    CHECK(outcome.files == md::RunSummary{});
    CHECK_FALSE(outcome.report.has_value());
    CHECK_FALSE(outcome.interrupted);
    CHECK_FALSE(outcome.fatal);
    CHECK(md::RowOutcome{.exit_code = 1, .fatal = true} != outcome);

    const md::TitleResult title{};
    CHECK(title.status == md::TitleStatus::not_run);
    CHECK_FALSE(title.exit_code.has_value());
    CHECK_FALSE(title.error.has_value());
    CHECK_FALSE(title.duplicate_of.has_value());
    CHECK_FALSE(title.report.has_value());

    const md::TitleResult dup{.line = 4, .status = md::TitleStatus::duplicate, .duplicate_of = 2};
    CHECK(dup.duplicate_of == std::optional<std::size_t>{2});

    const md::BatchResult result{};
    CHECK(result.titles.empty());
    CHECK(result.stop == md::BatchStop::none);
    CHECK(result.stopped_at_line == 0);
    CHECK(md::BatchResult{.titles = {dup}, .stop = md::BatchStop::network_streak} != result);

    const md::RowDirVerdict verdict{};
    CHECK(verdict.kind == md::RowDirKind::ok);
    CHECK(verdict.message.empty());
    CHECK(md::RowDirVerdict{.kind = md::RowDirKind::row_error, .message = "x: Not a directory"} !=
          verdict);

    const md::Utf8Decoded decoded{};
    CHECK(decoded.length == 0);
    CHECK(decoded.code_point == 0);
}
