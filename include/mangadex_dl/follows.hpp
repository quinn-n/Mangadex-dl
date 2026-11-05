// SPDX-License-Identifier: GPL-3.0-or-later
//
// `follows`: every title the logged-in user follows on MangaDex, optionally narrowed by reading
// status. The request URLs, the untrusted follows page and status map parsers, the --status
// filter, the CLI-level settings that turn the follows into ordinary BatchJobs for run_batch, and
// the authorized collection fetch. Frozen interface (follows plan T1); implemented in
// src/core/follows.cpp, follows_status.cpp (T10), follows_fetch.cpp (T13) and follows_plan.cpp
// (T14).
#pragma once

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {

/// Titles requested per follows page (the API's maximum).
inline constexpr std::size_t follows_page_limit = 100;
/// At most this many follows: the window MangaDex documents for collection endpoints. More are
/// refused, not truncated.
inline constexpr std::size_t max_follows = 10000;
/// More status-map entries than this are refused, not truncated.
inline constexpr std::size_t max_status_entries = 100000;
/// Guard on the status map's JSON separators: two per entry at the entry cap, plus headroom.
inline constexpr std::size_t max_status_separators = 1000000;
/// The row label of follows' row-scoped messages (`follows:<n>: …`).
inline constexpr std::string_view follows_row_label = "follows";

// ---- Reading statuses -----------------------------------------------------------------------

/// The six reading statuses MangaDex knows.
enum class ReadingStatus {
    reading,
    on_hold,
    plan_to_read,
    dropped,
    re_reading,
    completed,
};

/// The API spelling ("reading", "on_hold", …). Points at static storage.
[[nodiscard]] std::string_view reading_status_name(ReadingStatus status) noexcept;

/// The status for an exact API value; nullopt for anything else (an unknown value is not an
/// error: such a title just never matches).
[[nodiscard]] std::optional<ReadingStatus>
parse_reading_status(std::string_view api_value) noexcept;

/// The validated --status set.
class StatusFilter {
public:
    /// Each word is split at ','; the kebab aliases on-hold, plan-to-read and re-reading become
    /// the API spelling; exact lower case only; repeats are dropped, keeping the first. An empty
    /// word or an unknown one (`all` included) is Errc::usage with the spec's text (the word cut
    /// to 32 bytes; the app sanitizes it for the terminal).
    [[nodiscard]] static Result<StatusFilter> parse(std::span<const std::string> words);

    [[nodiscard]] bool contains(ReadingStatus status) const noexcept;

    /// The statuses in the order given, API spelling.
    [[nodiscard]] std::vector<std::string> names() const;

    friend bool operator==(const StatusFilter&, const StatusFilter&) = default;

private:
    /// For parse() only: normalised and de-duplicated.
    explicit StatusFilter(std::vector<ReadingStatus> statuses) noexcept
        : statuses_(std::move(statuses)) {}

    std::vector<ReadingStatus> statuses_{};
};

// ---- Documents ------------------------------------------------------------------------------

/// One validated follows page.
struct FollowsPage {
    std::vector<MangaId> titles{}; ///< API order, canonical
    std::size_t total = 0;         ///< the API's `total`

    friend bool operator==(const FollowsPage&, const FollowsPage&) = default;
};

/// The validated status map: canonical id -> the raw (well-formed, possibly unknown) status.
struct StatusMap {
    std::map<MangaId, std::string> statuses{};

    friend bool operator==(const StatusMap&, const StatusMap&) = default;
};

/// The follows kept by a --status filter.
struct FilteredFollows {
    std::vector<MangaId> titles{}; ///< follows order kept; duplicates survive
    std::size_t follows = 0;       ///< how many follows were filtered
    /// Well-formed status values no --status word can name, with how many follows carry each
    /// (for the -v note).
    std::map<std::string, std::size_t, std::less<>> unknown{};

    friend bool operator==(const FilteredFollows&, const FilteredFollows&) = default;
};

/// "<api>/user/follows/manga?limit=100&offset=<offset>" (`api` has no trailing '/').
[[nodiscard]] std::string follows_request_url(std::string_view api, std::size_t offset);

/// "<api>/manga/status".
[[nodiscard]] std::string status_map_request_url(std::string_view api);

/// Parses an untrusted follows page. Errc::network with fixed messages, never response bytes.
[[nodiscard]] Result<FollowsPage> parse_follows_page(std::string_view json);

/// Parses an untrusted status map (max_status_separators; `[]` is an empty map; more than
/// max_status_entries is refused). A duplicate JSON key is not detected (the last value wins,
/// V23); two keys that canonicalise to the same id keep the first. Errc::network with fixed
/// messages.
[[nodiscard]] Result<StatusMap> parse_status_map(std::string_view json);

/// Keeps each of `follows`, in order, whose status in `map` is in `filter`.
[[nodiscard]] FilteredFollows filter_follows(std::span<const MangaId> follows, const StatusMap& map,
                                             const StatusFilter& filter);

// ---- Settings and plan ----------------------------------------------------------------------

/// What CLI11 binds for `follows` (owned by the follows Command). Nothing here is validated yet.
struct RawFollowsArgs {
    RawDownloadArgs defaults{}; ///< .manga and .title stay empty; .dir is DIR
    bool fail_fast = false;
    bool dir_per_title = false;
    std::vector<std::string> status{}; ///< --status, one value per occurrence, split later
    RawAuthArgs auth{};
};

struct FollowsSettings {
    BatchSettings batch{};                ///< label = "follows", source = follows
    std::optional<StatusFilter> filter{}; ///< nullopt without --status
    bool dir_per_title = false;
    RawAuthArgs auth{};
};

/// Errc::usage, in this order: "DIR must not be empty"; DIR looks like a manga or list id;
/// check_raw_auth_args; every CLI-level option exactly as `download` validates it
/// (detail::validate_common, manga excluded); StatusFilter::parse when --status was given.
[[nodiscard]] Result<FollowsSettings> validate_follows_args(const RawFollowsArgs& raw);

/// One BatchJob per title, as plan_list builds them: line = 1-based position in `titles`,
/// duplicates marked.
[[nodiscard]] std::vector<BatchJob> plan_follows(std::span<const MangaId> titles,
                                                 const FollowsSettings& settings);

// ---- Fetch ----------------------------------------------------------------------------------

/// Everything fetch_follows_collection read.
struct FollowsCollection {
    std::vector<MangaId> follows{};      ///< API order, duplicates kept
    std::optional<StatusMap> statuses{}; ///< only when asked for and there is a follow

    friend bool operator==(const FollowsCollection&, const FollowsCollection&) = default;
};

/// What fetch_follows_collection talks to. Every pointer is borrowed for the call only and never
/// stored past it: transport and clock non-null; trace nullable and thread-safe. `jitter` must be
/// non-empty (see ApiClient). Fill it member by member, not with designated initializers: the
/// clang-analyzer leak false positive explained in run_title (src/app/cmd_download_run.cpp).
struct FollowsFetchServices {
    Transport* transport = nullptr;
    Clock* clock = nullptr;
    std::function<std::uint32_t(std::uint32_t)> jitter{}; ///< see ApiClient
    TraceSink* trace = nullptr;
};

/// One ApiClient (no reporter thread), a stop_source wired to an InterruptBridge; then every
/// follows page through ApiClient::get_authorized with `provider`'s authorization (a 401 gets
/// one refresh_after_unauthorized and one retry of that request; a second 401 is
/// {refused, Errc::auth_rejected}); more than max_follows -> {content, Errc::network}; then, with
/// `with_statuses` and at least one follow, the status map once. `provider` is called on this
/// thread only. Everything it creates is joined before it returns.
[[nodiscard]] AuthResult<FollowsCollection>
fetch_follows_collection(const ApiBase& api_base, const FollowsFetchServices& services,
                         TokenProvider& provider, bool with_statuses);

} // namespace mangadex_dl
