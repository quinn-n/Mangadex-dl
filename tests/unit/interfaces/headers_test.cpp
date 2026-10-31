// tests/unit/interfaces/headers_test.cpp: the frozen interface headers (download plan T1, batch
// plan T1) compile together, and their header-inline parts behave as documented: the exit-code
// mapping, the limits, the defaults of every option/config aggregate, the ownership shape of every
// class, and UniqueFd. Everything declared out of line is defined by later tasks, so it is only
// checked in unevaluated contexts here (signatures), never called. The skip-existing plan's T1
// headers (manifest, read source, zip index, existing-EPUB inspection, update decision) are
// checked the same way; of their functions only group_preference_rank (extracted from the
// planner) is defined yet, so it is the only one called. The list plan's T1 headers (ListId, the
// title-directory seam, the mdlist document/fetch/settings, and the additive members and
// overloads in the existing headers) are checked the same way: only their header-inline parts
// run.
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/download.hpp"
#include "mangadex_dl/epub.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/existing_epub.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/fetch_pool.hpp"
#include "mangadex_dl/filename.hpp"
#include "mangadex_dl/fs/bounded_read.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/fs/read_source.hpp"
#include "mangadex_dl/fs/row_dir.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/mdlist.hpp"
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
#include "mangadex_dl/title_dir.hpp"
#include "mangadex_dl/update_plan.hpp"
#include "mangadex_dl/zip.hpp"
#include "mangadex_dl/zip_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/types.h>
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
// mdlist --dir-per-title: a title whose own directory is unusable fails with 1; the run goes on.
static_assert(md::exit_code_for(md::Errc::row_dir) == md::exit_code::failure);
static_assert(md::exit_code_for(md::Errc::row_dir) == 1);

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
// batch_closing_message and batch_to_json are overloaded (list plan T1): the static_cast selects
// (and so requires) exactly the original overload.
static_assert(std::is_same_v<
              decltype(static_cast<std::optional<std::string> (*)(const md::BatchResult&, bool)>(
                  &md::batch_closing_message)),
              std::optional<std::string> (*)(const md::BatchResult&, bool)>);
static_assert(
    std::is_same_v<decltype(&md::up_to_date_notice), std::optional<std::string> (*)(std::size_t)>);
static_assert(
    std::is_same_v<decltype(static_cast<std::string (*)(const md::BatchResult&, std::string_view,
                                                        bool)>(&md::batch_to_json)),
                   std::string (*)(const md::BatchResult&, std::string_view, bool)>);
static_assert(
    std::is_same_v<decltype(&md::OutputRegistry::claim),
                   std::optional<std::size_t> (md::OutputRegistry::*)(std::string, std::size_t)>);
static_assert(
    std::is_same_v<decltype(&md::OutputRegistry::claimed_by),
                   std::optional<std::size_t> (md::OutputRegistry::*)(std::string_view) const>);
// Dry-run records of an earlier row's planned file: new names (no overloads), a copy out.
static_assert(std::is_same_v<decltype(&md::OutputRegistry::record_planned),
                             void (md::OutputRegistry::*)(std::string, md::PlannedEpub)>);
static_assert(
    std::is_same_v<decltype(&md::OutputRegistry::planned_at),
                   std::optional<md::PlannedEpub> (md::OutputRegistry::*)(std::string_view) const>);
static_assert(std::is_same_v<decltype(&md::TitleRunner::run),
                             md::RowOutcome (md::TitleRunner::*)(const md::BatchJob&, std::size_t,
                                                                 std::size_t)>);
// exit_code_for is overloaded (Errc and RunReport), so check the RunReport overload by call.
static_assert(
    std::is_same_v<decltype(md::exit_code_for(std::declval<const md::RunReport&>())), int>);
static_assert(noexcept(md::exit_code_for(std::declval<const md::RunReport&>())));

// ---- skip-existing plan T1: limits and names --------------------------------------------------
static_assert(md::manifest_entry_name == "META-INF/mangadex-dl.json");
static_assert(md::manifest_generator == "mangadex-dl" && md::manifest_format == 1);
// The manifest is untrusted JSON under the same cap as an API response.
static_assert(md::max_manifest_bytes == md::max_api_json_bytes);
static_assert(md::max_manifest_string_bytes == 4096 && md::max_manifest_depth == 8);
static_assert(md::max_manifest_separators == 2097152); // 2^21 (fact-check V16)
static_assert(md::max_central_directory_bytes == std::uint64_t{8} * 1024 * 1024);
static_assert(md::opf_entry_name == "OEBPS/content.opf");

// ---- skip-existing plan T1: value types ------------------------------------------------------
static_assert(std::is_aggregate_v<md::BookManifest> && std::is_aggregate_v<md::ManifestChapter>);
static_assert(std::is_aggregate_v<md::ManifestCover> && std::is_aggregate_v<md::EpubBookChapter>);
static_assert(std::is_aggregate_v<md::ReadError> && std::is_aggregate_v<md::ZipError>);
static_assert(std::is_aggregate_v<md::ZipEntryRecord> && std::is_aggregate_v<md::InspectFailure>);
static_assert(std::is_aggregate_v<md::HeldEntry> && std::is_aggregate_v<md::HeldBook>);
static_assert(std::is_aggregate_v<md::HeldPage> && std::is_aggregate_v<md::FinalChapter>);
static_assert(std::is_aggregate_v<md::KeptRelease> && std::is_aggregate_v<md::Repick>);
static_assert(std::is_aggregate_v<md::UpdateDecision>);
static_assert(std::is_aggregate_v<md::FileIdentity> && std::is_aggregate_v<md::ExistingHandle>);
// The identity is a plain value with the native stat types (fact-check V19/V20).
static_assert(std::is_trivially_copyable_v<md::FileIdentity>);
static_assert(std::is_same_v<decltype(md::FileIdentity::device), dev_t>);
static_assert(std::is_same_v<decltype(md::FileIdentity::inode), ino_t>);
static_assert(std::is_same_v<decltype(md::FileIdentity::size), off_t>);
static_assert(std::is_same_v<decltype(md::FileIdentity::mtime_sec), time_t>);
static_assert(std::is_same_v<decltype(md::FileIdentity::ctime_sec), time_t>);
static_assert(std::is_same_v<decltype(md::FileIdentity::mode), mode_t>);
// Indices, never iterators, into the decision's inputs.
static_assert(std::is_same_v<decltype(md::FinalChapter::held), std::optional<std::size_t>>);
static_assert(std::is_same_v<decltype(md::FinalChapter::planned), std::size_t>);
static_assert(
    std::is_same_v<decltype(md::HeldBook::pages), std::vector<std::vector<md::HeldEntry>>>);

// ---- skip-existing plan T1: ownership shapes -------------------------------------------------
static_assert(is_interface_v<md::ReadSource>);
static_assert(std::is_final_v<md::FdReadSource> &&
              std::is_base_of_v<md::ReadSource, md::FdReadSource>);
static_assert(is_pinned_v<md::FdReadSource>);
// FdReadSource borrows the descriptor (const UniqueFd&, lifetimebound) and cannot fail; a
// temporary descriptor is rejected at compile time.
static_assert(std::is_nothrow_constructible_v<md::FdReadSource, md::UniqueFd&, std::uint64_t>);
static_assert(!std::is_constructible_v<md::FdReadSource, md::UniqueFd, std::uint64_t>);
static_assert(!std::is_constructible_v<md::FdReadSource, const md::UniqueFd&&, std::uint64_t>);
static_assert(
    std::is_nothrow_constructible_v<md::FdReadSource, const md::UniqueFd&, std::uint64_t>);
// The handle owns the inspection descriptor; moving it moves ownership (a moved-from one is -1).
static_assert(is_move_only_v<md::ExistingHandle>);
static_assert(is_move_only_v<md::ZipIndex> && std::is_nothrow_move_assignable_v<md::ZipIndex>);
static_assert(!std::is_default_constructible_v<md::ZipIndex>); // only ZipIndex::read makes one
// The borrowing accessors work on an lvalue index and are deleted on an rvalue one.
template <class T>
concept can_find = requires { std::declval<T>().find(std::string_view{}); };
template <class T>
concept can_list = requires { std::declval<T>().entries(); };
static_assert(can_find<md::ZipIndex&> && can_find<const md::ZipIndex&>);
static_assert(!can_find<md::ZipIndex> && !can_find<const md::ZipIndex&&>);
static_assert(can_list<md::ZipIndex&> && can_list<const md::ZipIndex&>);
static_assert(!can_list<md::ZipIndex> && !can_list<const md::ZipIndex&&>);

// ---- skip-existing plan T1: frozen signatures (unevaluated: nothing is linked) --------------
static_assert(
    std::is_same_v<decltype(&md::manifest_to_json), std::string (*)(const md::BookManifest&)>);
static_assert(
    std::is_same_v<decltype(&md::parse_manifest),
                   std::expected<md::BookManifest, md::ManifestFault> (*)(std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::is_held_image_entry_name), bool (*)(std::string_view) noexcept>);
static_assert(std::is_same_v<decltype(&md::manifest_chapter_from),
                             md::ManifestChapter (*)(const md::Chapter&, md::ImageQuality)>);
static_assert(
    std::is_same_v<decltype(&md::chapter_from), md::Chapter (*)(const md::ManifestChapter&)>);
static_assert(std::is_same_v<decltype(&md::ReadSource::size),
                             std::uint64_t (md::ReadSource::*)() const noexcept>);
static_assert(std::is_same_v<decltype(&md::ReadSource::read_exact),
                             std::expected<void, md::ReadError> (md::ReadSource::*)(
                                 std::uint64_t, std::span<std::byte>)>);
static_assert(std::is_same_v<decltype(&md::ZipIndex::read),
                             std::expected<md::ZipIndex, md::ZipError> (*)(md::ReadSource&)>);
// find is overloaded on the ref-qualifier: the static_cast selects (and so requires) the lvalue
// one.
static_assert(
    std::is_same_v<
        decltype(static_cast<const md::ZipEntryRecord* (md::ZipIndex::*)(std::string_view) const &
                             noexcept>(&md::ZipIndex::find)),
        const md::ZipEntryRecord* (md::ZipIndex::*)(std::string_view) const & noexcept>);
static_assert(std::is_same_v<decltype(&md::locate_entry_data),
                             std::expected<std::uint64_t, md::ZipError> (*)(
                                 md::ReadSource&, const md::ZipIndex&, const md::ZipEntryRecord&)>);
static_assert(std::is_same_v<decltype(&md::foreign_reason_text),
                             std::string_view (*)(md::ForeignReason) noexcept>);
static_assert(std::is_same_v<decltype(&md::foreign_remedy),
                             std::string_view (*)(md::ForeignReason) noexcept>);
static_assert(std::is_same_v<decltype(&md::inspect_existing),
                             std::expected<md::HeldBook, md::InspectFailure> (*)(
                                 md::ReadSource&, std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::read_held_page), std::expected<md::HeldPage, md::PageFault> (*)(
                                                      md::ReadSource&, const md::HeldEntry&)>);
static_assert(
    std::is_same_v<decltype(&md::decide_update),
                   md::UpdateDecision (*)(const md::PlannedEpub&, const md::BookManifest&,
                                          std::span<const std::string>, md::ImageQuality)>);
static_assert(std::is_same_v<decltype(&md::fall_back_to_download),
                             md::UpdateDecision (*)(md::UpdateDecision, std::size_t,
                                                    const md::PlannedEpub&, md::ImageQuality)>);
static_assert(
    std::is_same_v<decltype(&md::final_epub),
                   md::PlannedEpub (*)(const md::PlannedEpub&, const md::UpdateDecision&)>);
static_assert(std::is_same_v<decltype(&md::new_chapter_ids),
                             std::vector<std::string> (*)(const md::UpdateDecision&)>);
static_assert(std::is_same_v<decltype(&md::reused_chapter_ids),
                             std::vector<std::string> (*)(const md::UpdateDecision&)>);
static_assert(std::is_same_v<decltype(&md::reconcile_skipped),
                             std::vector<md::SkippedChapter> (*)(std::vector<md::SkippedChapter>,
                                                                 std::span<const md::KeptRelease>,
                                                                 const md::PlannedEpub&)>);
static_assert(std::is_same_v<decltype(&md::chapter_labels),
                             std::string (*)(std::span<const md::Chapter>, std::size_t)>);
// max_shown has a default.
static_assert(
    std::is_same_v<decltype(md::chapter_labels(std::span<const md::Chapter>{})), std::string>);
static_assert(
    std::is_same_v<decltype(&md::page_image_entry), std::string (*)(std::size_t, md::MediaType)>);
static_assert(std::is_same_v<decltype(&md::cover_image_entry), std::string (*)(md::MediaType)>);
// finish is overloaded: the static_cast selects (and so requires) exactly this overload.
static_assert(
    std::is_same_v<decltype(static_cast<md::Result<void> (md::EpubWriter::*)(
                                std::optional<std::string>, std::vector<md::EpubBookChapter>)>(
                       &md::EpubWriter::finish)),
                   md::Result<void> (md::EpubWriter::*)(std::optional<std::string>,
                                                        std::vector<md::EpubBookChapter>)>);
static_assert(std::is_same_v<decltype(static_cast<md::Result<void> (md::EpubWriter::*)()>(
                                 &md::EpubWriter::finish)),
                             md::Result<void> (md::EpubWriter::*)()>);
static_assert(
    std::is_same_v<decltype(&md::OutputDir::open_existing),
                   md::Result<md::ExistingHandle> (md::OutputDir::*)(std::string_view) const>);
static_assert(std::is_same_v<decltype(&md::OutputDir::publish_replacing),
                             md::Result<md::ReplaceOutcome> (md::OutputDir::*)(
                                 md::TempFile&&, std::string_view, const md::FileIdentity&)>);
static_assert(
    std::is_same_v<decltype(&md::group_preference_rank),
                   std::size_t (*)(std::span<const md::ScanGroup>, std::span<const std::string>)>);

// ---- list plan T1: limits and names ----------------------------------------------------------
static_assert(md::max_list_separators == 200000);
static_assert(md::max_list_name_bytes == 1024 && md::max_list_display_name_bytes == 64);
static_assert(md::max_list_display_name_bytes < md::max_list_name_bytes);
static_assert(md::list_row_label == "list");

// ---- list plan T1: value types and ownership shapes -------------------------------------------
static_assert(std::is_aggregate_v<md::TitleDirChoice> && std::is_aggregate_v<md::TitleDirClaim>);
static_assert(std::is_aggregate_v<md::ListSummary> && std::is_aggregate_v<md::CustomList>);
static_assert(std::is_aggregate_v<md::ListFetchError> &&
              std::is_aggregate_v<md::ListFetchServices>);
static_assert(std::is_aggregate_v<md::RawMdlistArgs> && std::is_aggregate_v<md::MdlistSettings>);
static_assert(std::is_same_v<md::ListFetchResult<int>, std::expected<int, md::ListFetchError>>);
static_assert(is_interface_v<md::TitleDirResolver>);
// ListId is a value with an invariant: a public nil default, otherwise only parse() makes one.
static_assert(std::is_default_constructible_v<md::ListId>);
static_assert(!std::is_constructible_v<md::ListId, std::string>);
static_assert(std::is_nothrow_move_constructible_v<md::ListId>);
// The title-directory registry owns its keys and starts empty.
static_assert(std::is_nothrow_default_constructible_v<md::TitleDirRegistry>);
// The additive members (last, with DMIs) keep every existing designated initializer valid.
static_assert(std::is_same_v<decltype(md::DownloadOptions::dir_per_title), bool>);
static_assert(std::is_same_v<decltype(md::Plan::file_stem), std::string>);
static_assert(std::is_same_v<decltype(md::DownloadServices::title_dir), md::TitleDirResolver*>);
static_assert(
    std::is_same_v<decltype(md::DownloadServices::claimed), std::function<bool(std::string_view)>>);
static_assert(std::is_same_v<decltype(md::DownloadServices::planned_in_run),
                             std::function<std::optional<md::PlannedEpub>(std::string_view)>>);
static_assert(std::is_same_v<decltype(md::BatchSettings::source), md::BatchSource>);
static_assert(std::is_same_v<decltype(md::RowOutcome::dir), std::string>);

// ---- list plan T1: frozen signatures (unevaluated: nothing is linked) ------------------------
static_assert(
    std::is_same_v<decltype(&md::ListId::parse), md::Result<md::ListId> (*)(std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::looks_like_list_id), bool (*)(std::string_view) noexcept>);
static_assert(
    std::is_same_v<decltype(&md::ListId::str), std::string_view (md::ListId::*)() const noexcept>);
static_assert(
    std::is_same_v<decltype(&md::ApiClient::fetch_list_document),
                   md::Result<std::string> (md::ApiClient::*)(const md::ListId&, std::stop_token)>);
static_assert(std::is_same_v<decltype(&md::TitleDirResolver::resolve),
                             md::TitleDirChoice (md::TitleDirResolver::*)(std::string_view,
                                                                          const md::MangaId&)>);
static_assert(std::is_same_v<decltype(&md::TitleDirRegistry::claim),
                             md::TitleDirClaim (md::TitleDirRegistry::*)(
                                 std::string_view, const md::MangaId&, std::size_t)>);
static_assert(
    std::is_same_v<decltype(static_cast<std::optional<std::string> (*)(const md::BatchResult&, bool,
                                                                       md::BatchSource)>(
                       &md::batch_closing_message)),
                   std::optional<std::string> (*)(const md::BatchResult&, bool, md::BatchSource)>);
static_assert(std::is_same_v<
              decltype(static_cast<std::string (*)(const md::BatchResult&, std::string_view, bool,
                                                   const std::optional<md::ListSummary>&)>(
                  &md::batch_to_json)),
              std::string (*)(const md::BatchResult&, std::string_view, bool,
                              const std::optional<md::ListSummary>&)>);
static_assert(std::is_same_v<decltype(&md::list_request_url),
                             std::string (*)(std::string_view, const md::ListId&)>);
static_assert(std::is_same_v<decltype(&md::list_web_url), std::string (*)(const md::ListId&)>);
static_assert(std::is_same_v<decltype(&md::parse_custom_list),
                             md::Result<md::CustomList> (*)(std::string_view, const md::ListId&)>);
static_assert(std::is_same_v<decltype(&md::is_custom_list_error_body), bool (*)(std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::list_name_for_display), std::string (*)(std::string_view)>);
static_assert(
    std::is_same_v<decltype(&md::list_summary), md::ListSummary (*)(const md::CustomList&)>);
static_assert(
    std::is_same_v<decltype(&md::fetch_custom_list),
                   md::ListFetchResult<md::CustomList> (*)(const md::ListId&, const md::ApiBase&,
                                                           const md::ListFetchServices&)>);
static_assert(std::is_same_v<decltype(&md::validate_mdlist_args),
                             md::Result<md::MdlistSettings> (*)(const md::RawMdlistArgs&)>);
static_assert(std::is_same_v<decltype(&md::plan_list),
                             std::vector<md::BatchJob> (*)(const md::CustomList&,
                                                           const md::MdlistSettings&)>);

/// A resolver written the way the app writes its own: the seam is implementable and dispatches
/// through the base, and the choice owns its strings (nothing borrows `stem`).
class StemResolver final : public md::TitleDirResolver {
public:
    StemResolver() = default;

    [[nodiscard]] md::TitleDirChoice resolve(std::string_view stem,
                                             const md::MangaId& manga) override {
        ++calls_;
        if (manga == md::MangaId{}) {
            return md::TitleDirChoice{.name = std::string{stem}};
        }
        return md::TitleDirChoice{.name = std::string{stem} + " [x]", .warning = "moved aside"};
    }

    [[nodiscard]] int calls() const noexcept { return calls_; }

private:
    int calls_ = 0;
};

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
    CHECK(raw.jobs == 8);
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
    CHECK(options.jobs == 8U);
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
    CHECK_FALSE(services.claimed);
    CHECK_FALSE(services.planned_in_run);
    CHECK(services.stop.stop_possible());
    CHECK_FALSE(services.stop.stop_requested());
}

TEST_CASE("The new report fields default to no existing file", "[interfaces][report]") {
    const md::EpubOutcome outcome{};
    CHECK(outcome.status == md::EpubStatus::planned);
    CHECK_FALSE(outcome.replaced);
    CHECK(outcome.error.empty());
    CHECK(outcome.existing == md::ExistingKind::none);
    CHECK(outcome.new_chapters.empty());
    CHECK(outcome.reused_chapters.empty());

    // Partial designated initialization keeps the rest at their defaults.
    const md::EpubOutcome updated{.status = md::EpubStatus::written,
                                  .replaced = true,
                                  .existing = md::ExistingKind::incomplete,
                                  .new_chapters = {"new"},
                                  .reused_chapters = {"old"}};
    CHECK(updated.pages == 0);
    CHECK(updated.error.empty());
    CHECK(updated != outcome);
}

TEST_CASE("FileIdentity compares every field", "[interfaces][fs]") {
    CHECK(md::FileIdentity{} == md::FileIdentity{});
    const md::FileIdentity base{.device = 1,
                                .inode = 2,
                                .size = 3,
                                .mtime_sec = 4,
                                .mtime_nsec = 5,
                                .ctime_sec = 6,
                                .ctime_nsec = 7,
                                .mode = 0600};
    CHECK(base == base);
    CHECK(base != md::FileIdentity{});
    // A ctime-only difference (chmod, link count, an mtime restored after a rewrite) is a change.
    md::FileIdentity touched = base;
    touched.ctime_nsec = 8;
    CHECK(touched != base);
    touched = base;
    touched.inode = 9;
    CHECK(touched != base);
    // The mode is part of it (a chmod also moves the ctime, so this adds no new "changed").
    touched = base;
    touched.mode = 0644;
    CHECK(touched != base);
}

TEST_CASE("The skip-existing value types default to their documented values", "[interfaces][fs]") {
    const md::ExistingHandle handle{};
    CHECK(handle.status == md::ExistingOpen::absent);
    CHECK_FALSE(handle.fd.valid());
    CHECK(handle.identity == md::FileIdentity{});
    CHECK(handle.error == 0);

    CHECK(md::ReadError{} == md::ReadError{.fault = md::ReadFault::io, .error = 0});
    CHECK(md::ZipError{} == md::ZipError{.fault = md::ZipFault::bad_directory, .error = 0});
    CHECK(md::InspectFailure{} ==
          md::InspectFailure{.reason = md::ForeignReason::not_ours, .error = 0});

    const md::ZipEntryRecord record{};
    CHECK(record.name.empty());
    CHECK(record.method == 0);
    CHECK(record.local_header_offset == 0);

    const md::BookManifest manifest{};
    CHECK(manifest.identifier.empty());
    CHECK(manifest.opf_crc32 == 0);
    CHECK_FALSE(manifest.cover.has_value());
    CHECK(manifest.chapters.empty());
    CHECK(md::ManifestChapter{}.quality == md::ImageQuality::data);

    const md::HeldBook held{};
    CHECK(held.pages.empty());
    CHECK_FALSE(held.cover.has_value());
    CHECK(md::HeldEntry{}.type == md::MediaType::jpeg);
    CHECK(md::HeldPage{}.bytes.empty());

    const md::UpdateDecision decision{};
    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.chapters.empty());
    CHECK(decision.missing.empty());
    CHECK(decision.extra.empty());
    CHECK(decision.kept.empty());
    CHECK(decision.repicked.empty());

    const md::FinalChapter chapter{};
    CHECK(chapter.source == md::ChapterSource::download);
    CHECK(chapter.quality == md::ImageQuality::data);
    CHECK_FALSE(chapter.held.has_value());
    CHECK(chapter.planned == 0);

    const md::EpubBookChapter book_chapter{};
    CHECK(book_chapter.record == md::ManifestChapter{});
    CHECK(book_chapter.first_page == 0);
    CHECK(book_chapter.page_count == 0);
}

TEST_CASE("group_preference_rank ranks by the first matching --group entry",
          "[interfaces][planner]") {
    const std::vector<md::ScanGroup> groups{
        {.id = "11111111-0002-4000-8000-000000000001", .name = "Clean Scans"},
        {.id = "aaaaaaaa-0002-4000-8000-00000000000b", .name = "Bravo Scans"},
    };
    const std::vector<std::string> by_name{"Charlie Scans", "bravo scans", "Clean Scans"};
    CHECK(md::group_preference_rank(groups, by_name) == 1); // ASCII case-insensitive name
    const std::vector<std::string> by_id{"11111111-0002-4000-8000-000000000001"};
    CHECK(md::group_preference_rank(groups, by_id) == 0);
    const std::vector<std::string> upper_id{"11111111-0002-4000-8000-00000000000a",
                                            "AAAAAAAA-0002-4000-8000-00000000000B"};
    CHECK(md::group_preference_rank(groups, upper_id) == 1); // ASCII case-insensitive id
    const std::vector<std::string> none{"Charlie Scans", "Delta"};
    CHECK(md::group_preference_rank(groups, none) == none.size()); // the worst rank
    CHECK(md::group_preference_rank(groups, std::span<const std::string>{}) == 0);
    CHECK(md::group_preference_rank(std::span<const md::ScanGroup>{}, by_name) == by_name.size());
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
    CHECK(stdin_args.defaults.jobs == 8);

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

TEST_CASE("Error and Failure map the mdlist row_dir category to exit code 1",
          "[interfaces][mdlist]") {
    CHECK(md::Error{.code = md::Errc::row_dir}.exit_code() == md::exit_code::failure);

    const md::Failure failure{md::Error{.code = md::Errc::row_dir, .message = "out/T: Not a dir"}};
    CHECK(failure.exit_code() == 1);
    CHECK(failure.error() == md::Error{.code = md::Errc::row_dir, .message = "out/T: Not a dir"});
}

TEST_CASE("A default list id is the nil UUID placeholder", "[interfaces][mdlist]") {
    const md::ListId nil{};
    CHECK(nil.str() == "00000000-0000-0000-0000-000000000000");
    CHECK(nil == md::ListId{});
    CHECK_FALSE(nil < md::ListId{});
}

TEST_CASE("The additive download members default to today's behaviour", "[interfaces][mdlist]") {
    const md::DownloadOptions options{};
    CHECK_FALSE(options.dir_per_title);
    CHECK(options == md::DownloadOptions{});
    CHECK(md::DownloadOptions{.dir_per_title = true} != options);
    CHECK(md::DownloadOptions{.report = false, .dir_per_title = true} ==
          md::DownloadOptions{.report = false, .dir_per_title = true});

    const md::Plan plan{};
    CHECK(plan.file_stem.empty());
    CHECK(md::Plan{.layout = md::Layout::volumes, .file_stem = "Title"} != plan);

    const md::DownloadServices services{};
    CHECK(services.title_dir == nullptr);
}

TEST_CASE("The additive batch members default to a CSV batch", "[interfaces][mdlist]") {
    const md::BatchSettings settings{};
    CHECK(settings.source == md::BatchSource::csv);
    const md::BatchSettings list_settings{.label = "list", .source = md::BatchSource::mdlist};
    CHECK(list_settings.source == md::BatchSource::mdlist);
    CHECK(list_settings.base == md::DownloadOptions{});
    CHECK_FALSE(list_settings.fail_fast);

    const md::RowOutcome outcome{};
    CHECK(outcome.dir.empty());
    const md::RowOutcome moved{.exit_code = 1, .dir = "out/Title"};
    CHECK_FALSE(moved.fatal);
    CHECK(moved.dir == "out/Title");
    CHECK(moved != outcome);

    const md::ListSummary summary{};
    CHECK(summary.id.empty());
    CHECK_FALSE(summary.name.has_value());
    CHECK(summary.url.empty());
    const md::ListSummary named{.id = "id", .url = "https://mangadex.org/list/id"};
    CHECK_FALSE(named.name.has_value());
    CHECK(named != summary);
}

TEST_CASE("The title-directory seam dispatches through the base and owns its strings",
          "[interfaces][mdlist]") {
    const md::TitleDirChoice choice{};
    CHECK(choice.name.empty());
    CHECK_FALSE(choice.warning.has_value());
    CHECK(md::TitleDirChoice{.name = "T"} == md::TitleDirChoice{.name = "T", .warning = {}});

    const md::TitleDirClaim claim{};
    CHECK(claim.name.empty());
    CHECK_FALSE(claim.taken_by.has_value());
    const md::TitleDirClaim moved{.name = "T [77777777]", .taken_by = 1};
    CHECK(moved.taken_by == std::optional<std::size_t>{1});
    CHECK(moved != claim);

    StemResolver concrete{};
    md::TitleDirResolver& resolver = concrete;
    md::TitleDirChoice plain{};
    {
        const std::string stem{"Clean Title"}; // dies before `plain` is read
        plain = resolver.resolve(stem, md::MangaId{});
    }
    CHECK(plain == md::TitleDirChoice{.name = "Clean Title"});
    const auto twin = md::MangaId::parse("77777777-0000-4000-8000-000000000001");
    REQUIRE(twin.has_value());
    const md::TitleDirChoice aside = resolver.resolve("Clean Title", *twin);
    CHECK(aside.name == "Clean Title [x]");
    CHECK(aside.warning == std::optional<std::string>{"moved aside"});
    CHECK(concrete.calls() == 2);
}

TEST_CASE("The mdlist aggregates default-construct and take partial designated init",
          "[interfaces][mdlist]") {
    const md::CustomList list{};
    CHECK(list.id == md::ListId{});
    CHECK_FALSE(list.name.has_value());
    CHECK(list.titles.empty());
    const md::CustomList dupes{.titles = {md::MangaId{}, md::MangaId{}}}; // duplicates kept
    CHECK(dupes.titles.size() == 2);
    CHECK(dupes != list);

    const md::ListFetchError error{};
    CHECK(error.kind == md::ListFailureKind::request);
    CHECK(error.error == md::Error{});
    const md::ListFetchError content{
        .kind = md::ListFailureKind::content,
        .error = {.code = md::Errc::network, .message = "list x: malformed"}};
    CHECK(content.error.exit_code() == md::exit_code::network);
    CHECK(content != error);

    const md::ListFetchResult<md::CustomList> failed{std::unexpect, content};
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().kind == md::ListFailureKind::content);

    const md::ListFetchServices services{};
    CHECK(services.transport == nullptr);
    CHECK(services.clock == nullptr);
    CHECK_FALSE(static_cast<bool>(services.jitter));
    CHECK(services.trace == nullptr);

    const md::RawMdlistArgs raw{};
    CHECK(raw.list.empty());
    CHECK(raw.defaults.dir == ".");
    CHECK(raw.defaults.manga.empty());
    CHECK_FALSE(raw.defaults.title.has_value());
    CHECK_FALSE(raw.fail_fast);
    CHECK_FALSE(raw.dir_per_title);
    const md::RawMdlistArgs flagged{.list = "L", .dir_per_title = true};
    CHECK(flagged.defaults.jobs == 8);
    CHECK_FALSE(flagged.fail_fast);

    const md::MdlistSettings settings{};
    CHECK(settings.batch.source == md::BatchSource::csv); // validate_mdlist_args sets mdlist
    CHECK(settings.list == md::ListId{});
    CHECK_FALSE(settings.dir_per_title);
    const md::MdlistSettings partial{
        .batch = {.label = std::string{md::list_row_label}, .source = md::BatchSource::mdlist},
        .dir_per_title = true};
    CHECK(partial.batch.label == "list");
    CHECK(partial.batch.base == md::DownloadOptions{});
    CHECK(partial.list == md::ListId{});
}

// ---- batch-dir-per-title plan T1: additive members and the reservation signature -------------
// The additive members (last, with DMIs) keep every existing designated initializer valid.
static_assert(std::is_same_v<decltype(md::RawBatchArgs::dir_per_title), bool>);
static_assert(std::is_same_v<decltype(md::BatchSettings::dir_per_title), bool>);
static_assert(std::is_same_v<decltype(md::TitleDirClaim::reserved), bool>);
static_assert(std::is_same_v<decltype(&md::TitleDirRegistry::reserve),
                             void (md::TitleDirRegistry::*)(std::string_view, const md::MangaId&,
                                                            std::size_t)>);
// Still default-constructible without throwing, with the reservations map.
static_assert(std::is_nothrow_default_constructible_v<md::TitleDirRegistry>);

TEST_CASE("The batch --dir-per-title members default to off and take partial designated init",
          "[interfaces][batch]") {
    const md::RawBatchArgs raw{};
    CHECK_FALSE(raw.dir_per_title);
    const md::RawBatchArgs flagged{.file = "-", .dir_per_title = true};
    CHECK(flagged.file == "-");
    CHECK_FALSE(flagged.fail_fast);
    CHECK(flagged.dir_per_title);

    const md::BatchSettings settings{};
    CHECK_FALSE(settings.dir_per_title);
    CHECK_FALSE(settings.base.dir_per_title);
    const md::BatchSettings per_title{.label = "x", .dir_per_title = true};
    CHECK(per_title.label == "x");
    CHECK(per_title.source == md::BatchSource::csv);
    CHECK(per_title.dir_per_title);
    CHECK_FALSE(per_title.base.dir_per_title);

    const md::TitleDirClaim claim{};
    CHECK_FALSE(claim.reserved);
    CHECK(md::TitleDirClaim{.name = "T", .taken_by = 1, .reserved = true} !=
          md::TitleDirClaim{.name = "T", .taken_by = 1});
    CHECK(md::TitleDirClaim{.name = "T", .taken_by = 1} ==
          md::TitleDirClaim{.name = "T", .taken_by = 1, .reserved = false});
}
