// SPDX-License-Identifier: GPL-3.0-or-later
//
// `mdlist`: every title in a public MangaDex custom list (MDList). The list document (URL,
// untrusted parse, display name), the fetch orchestrator, and the CLI-level settings that turn a
// list into ordinary BatchJobs for run_batch. Frozen interface (list plan T1); implemented in
// src/core/mdlist.cpp (T7), mdlist_fetch.cpp (T7) and mdlist_plan.cpp (T8).
#pragma once

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// Guard on the list document's JSON separators: about 20 per title at the 10000-title cap.
inline constexpr std::size_t max_list_separators = 200000;
/// attributes.name is kept up to this many bytes, cut on a UTF-8 boundary.
inline constexpr std::size_t max_list_name_bytes = 1024;
/// The name shown on stderr is cut to this many bytes (then "...").
inline constexpr std::size_t max_list_display_name_bytes = 64;
/// The row label of mdlist's row-scoped messages (`list:<n>: …`).
inline constexpr std::string_view list_row_label = "list";

/// A validated list document.
struct CustomList {
    ListId id{};
    std::optional<std::string> name{}; ///< ≤ max_list_name_bytes, cut on a UTF-8 boundary;
                                       ///< display only
    std::vector<MangaId> titles{};     ///< API order, canonical, duplicates kept

    friend bool operator==(const CustomList&, const CustomList&) = default;
};

/// "<api>/list/<id>" (`api` has no trailing '/').
[[nodiscard]] std::string list_request_url(std::string_view api, const ListId& id);

/// "https://mangadex.org/list/<id>".
[[nodiscard]] std::string list_web_url(const ListId& id);

/// Parses an untrusted list document. Errc::network with self-contained messages that name the
/// list id, never the URL or response bytes.
[[nodiscard]] Result<CustomList> parse_custom_list(std::string_view json, const ListId& expected);

/// A 401/403 body that is {"result":"error","errors":[…]} with some errors[i].detail starting
/// "CustomList " (parsed with parse_untrusted, max 64 KiB, depth 64). False on anything else.
[[nodiscard]] bool is_custom_list_error_body(std::string_view body);

/// At most max_list_display_name_bytes on a UTF-8 boundary, then "..." when cut. Raw: the app
/// sanitizes it for the terminal.
[[nodiscard]] std::string list_name_for_display(std::string_view name);

/// The --json `list` object for `list`.
[[nodiscard]] ListSummary list_summary(const CustomList& list);

/// Which step of fetch_custom_list failed: it decides whether the network remedy applies.
enum class ListFailureKind {
    request, ///< transport, HTTP status or interrupt: the remedy applies to exit 3
    content, ///< the document itself (malformed, over the cap): a self-contained message
};

struct ListFetchError {
    ListFailureKind kind = ListFailureKind::request;
    Error error{};

    friend bool operator==(const ListFetchError&, const ListFetchError&) = default;
};

template <class T> using ListFetchResult = std::expected<T, ListFetchError>;

/// What fetch_custom_list talks to. Every pointer is borrowed for the call only and never stored
/// past it: transport and clock non-null; trace nullable and thread-safe. `jitter` must be
/// non-empty (see ApiClient). Fill it member by member, not with designated initializers: the
/// clang-analyzer leak false positive explained in run_title (src/app/cmd_download_run.cpp).
struct ListFetchServices {
    Transport* transport = nullptr;
    Clock* clock = nullptr;
    std::function<std::uint32_t(std::uint32_t)> jitter{}; ///< see ApiClient
    TraceSink* trace = nullptr;
};

/// One ApiClient (endpoints_for(api_base, report = false): no reporter thread), a stop_source
/// wired to an InterruptBridge, a pending-interrupt check before the request; then
/// fetch_list_document -> parse_custom_list. Everything it creates is joined before it returns.
[[nodiscard]] ListFetchResult<CustomList>
fetch_custom_list(const ListId& id, const ApiBase& api_base, const ListFetchServices& services);

/// What CLI11 binds for `mdlist` (owned by the mdlist Command). Nothing here is validated yet.
struct RawMdlistArgs {
    std::string list{};         ///< LIST as typed
    RawDownloadArgs defaults{}; ///< .manga and .title stay empty; .dir is DIR
    bool fail_fast = false;
    bool dir_per_title = false;
};

struct MdlistSettings {
    BatchSettings batch{}; ///< file = LIST as typed, label = "list", source = mdlist
    ListId list{};
    bool dir_per_title = false;
};

/// Errc::usage, in this order: "LIST must not be empty"; ListId::parse's messages; "DIR must not
/// be empty"; "DIR looks like a MangaDex id; give one list per run" (manga or list id); then
/// every CLI-level option exactly as `download` validates it (detail::validate_common with the nil
/// MangaId: manga excluded).
[[nodiscard]] Result<MdlistSettings> validate_mdlist_args(const RawMdlistArgs& raw);

/// One BatchJob per title: line = 1-based position; raw = defaults with .manga = the id;
/// options = base with .manga and .dir_per_title; dir = DIR; sub_dir = ""; duplicates marked as
/// plan_batch marks them.
[[nodiscard]] std::vector<BatchJob> plan_list(const CustomList& list,
                                              const MdlistSettings& settings);

} // namespace mangadex_dl
