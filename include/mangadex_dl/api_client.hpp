// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MangaDex API client: pagination, size caps, rate limiting, retries, at-home servers and
// MD@H reporting over an injected Transport and Clock. Frozen interface (download plan T1);
// implemented in src/core/api_client.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/net/retry.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// Where each kind of request goes. No member has a trailing '/'.
struct Endpoints {
    std::string api{std::string{default_api_base}};
    std::string uploads{"https://uploads.mangadex.org"};
    std::optional<std::string> at_home_override{}; ///< replaces every at-home baseUrl (fixtures)
    std::optional<std::string> report_url{};       ///< MD@H report POST target; none = no reports

    friend bool operator==(const Endpoints&, const Endpoints&) = default;
};

/// Official: the defaults, plus the report URL when `report`. A custom https/http base: that API
/// with the default uploads host and no reports. Fixture: `file://<root>/api`,
/// `file://<root>/uploads`, at-home override `file://<root>/at-home`, no reports.
[[nodiscard]] Endpoints endpoints_for(const ApiBase& base, bool report);

/// Receives one line per request for -vv. Implementations are thread-safe: trace() is called
/// from worker threads. The line is passed by value; the sink keeps nothing borrowed. trace()
/// should not throw; if it does on a FetchPool worker, the pool catches it there and returns it
/// as Errc::failure.
class TraceSink {
public:
    TraceSink(const TraceSink&) = delete;
    TraceSink& operator=(const TraceSink&) = delete;
    TraceSink(TraceSink&&) = delete;
    TraceSink& operator=(TraceSink&&) = delete;
    virtual ~TraceSink() = default;

    virtual void trace(std::string line) = 0;

protected:
    TraceSink() = default;
};

struct ClientConfig {
    Endpoints endpoints{};
    RetryConfig retry{};
    std::chrono::milliseconds api_interval{250};      ///< shared limiter for every API request
    std::chrono::milliseconds at_home_interval{1500}; ///< extra limiter for /at-home/server

    friend bool operator==(const ClientConfig&, const ClientConfig&) = default;
};

/// Thread-safe: every fetch_* may be called concurrently (fetch_page is, from pool workers).
/// Errors: a stop request gives Errc::interrupted; HTTP 404 on the title gives Errc::not_found;
/// everything else that fails (after retries) gives Errc::network with `<url>: <reason>`.
///
/// Borrows `transport`, `clock` and `trace` (nullable): all three must outlive the client.
/// `jitter(max)` returns a value in [0, max] milliseconds; the client serializes its calls to it,
/// so it needs no locking of its own. Neither copyable nor movable; state lives in
/// api_client.cpp (pimpl).
class ApiClient {
public:
    ApiClient(Transport& transport MANGADEX_DL_LIFETIMEBOUND,
              Clock& clock MANGADEX_DL_LIFETIMEBOUND, ClientConfig config,
              std::function<std::uint32_t(std::uint32_t)> jitter,
              TraceSink* trace MANGADEX_DL_LIFETIMEBOUND);
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;
    ApiClient(ApiClient&&) = delete;
    ApiClient& operator=(ApiClient&&) = delete;
    ~ApiClient();

    [[nodiscard]] Result<MangaInfo> fetch_manga(const MangaId& id, std::stop_token stop);
    /// Every page of the feed for `lang`. Errc::network when the title has more chapters than
    /// the API's offset window allows (never a silent truncation).
    [[nodiscard]] Result<std::vector<Chapter>> fetch_feed(const MangaId& id, std::string_view lang,
                                                          std::stop_token stop);
    [[nodiscard]] Result<std::vector<Cover>> fetch_covers(const MangaId& id, std::stop_token stop);
    [[nodiscard]] Result<AtHomeServer> fetch_at_home(std::string_view chapter_id,
                                                     std::stop_token stop);
    /// One page image (at most max_image_bytes). An image-host 403 returns at once (the caller
    /// refreshes the at-home server). Sends an MD@H report for non-mangadex.org hosts.
    [[nodiscard]] Result<std::string> fetch_page(const AtHomeServer& server, ImageQuality quality,
                                                 std::string_view file, std::stop_token stop);
    [[nodiscard]] Result<std::string> fetch_cover(const MangaId& id, std::string_view file,
                                                  std::stop_token stop);
    /// GET <api>/list/<canonical id> (no query), body cap max_api_json_bytes; returns the body of
    /// a 2xx unparsed. 404 -> Errc::not_found "list <id> not found on MangaDex (HTTP 404): it does
    /// not exist or is private; check the id, make the list public on MangaDex, or use
    /// 'mangadex-dl download' if it is a title". 401/403 whose body is_custom_list_error_body ->
    /// the same. Otherwise the usual status mapping (Errc::network "<url>: <reason>"); a stop ->
    /// Errc::interrupted. Implemented in list plan T7.
    [[nodiscard]] Result<std::string> fetch_list_document(const ListId& id, std::stop_token stop);

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace mangadex_dl
