// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libcurl transport (net/curl_transport.hpp) and the CA bundle choice.
//
// One CurlTransport keeps a small mutex-guarded pool of easy handles (connection reuse). Every
// fetch() takes a handle, applies the FULL option set (curl_easy_reset on return to the pool wipes
// everything, userdata pointers included), performs inside a SigpipeGuard, and returns the handle.
// The response body is bounded by the write callback (after any content decoding), which is the
// real cap; CURLOPT_MAXFILESIZE_LARGE only rejects an announced oversized Content-Length early.
// A request may carry a secret (transport.hpp, "Secrets"): the Authorization line is built in a
// Secret, the header list is wiped before it is freed, the POST body is borrowed rather than
// copied, the value is never in an error message, it goes only over https or plain http to a
// loopback host, redirects are never followed, and CURLOPT_VERBOSE is never set.
#include "mangadex_dl/net/curl_transport.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"
#include "net/curl_api.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <curl/curl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): explicit_bzero is a glibc extension of <string.h>
#include <string.h>

namespace mangadex_dl {
namespace {

using detail::CurlApi;
using detail::SigpipeGuard;

/// Longer Retry-After / X-RateLimit-Retry-After / X-Cache values are dropped (left empty), never
/// truncated: a truncated number would parse as a different delay.
inline constexpr std::size_t header_value_cap = 256;

/// Closes one easy handle. curl_easy_cleanup may write a TLS close_notify to a dead peer, so it
/// runs inside a SigpipeGuard.
struct EasyCleanup {
    const CurlApi* api = nullptr; ///< the owning transport's CurlApi, which outlives its handles

    void operator()(CURL* handle) const noexcept {
        const SigpipeGuard guard;
        api->easy_cleanup(handle);
    }
};
using EasyPtr = std::unique_ptr<CURL, EasyCleanup>;

/// Frees a request header list, wiping every node's text first: one may hold the Authorization
/// line (a bearer token). curl_slist_append stores its own copy of each line in `data`, so the
/// wipe is legal (V14). Best effort: the copies libcurl makes while sending are out of reach.
/// Runs only after the handle was reset (see the declaration order in fetch()).
struct SlistFree {
    const CurlApi* api = nullptr; ///< the owning transport's CurlApi

    void operator()(curl_slist* list) const noexcept {
        for (const curl_slist* node = list; node != nullptr; node = node->next) {
            if (node->data != nullptr) {
                ::explicit_bzero(node->data, std::strlen(node->data));
            }
        }
        api->slist_free_all(list);
    }
};
using SlistPtr = std::unique_ptr<curl_slist, SlistFree>;

/// Per-fetch state the callbacks write into. It lives on fetch()'s stack and is handed to libcurl
/// as WRITEDATA/HEADERDATA/XFERINFODATA, valid only during that fetch's perform; the handle is
/// reset before the state dies.
struct TransferState {
    std::size_t cap = 0; ///< max body bytes; 0 refuses any non-empty body
    std::stop_token cancel{};
    std::string body{};
    std::string retry_after{};
    std::string x_ratelimit_retry_after{};
    std::string x_cache{};
    bool too_large = false;
    bool out_of_memory = false;
    std::array<char, CURL_ERROR_SIZE> error_buffer{}; ///< CURLOPT_ERRORBUFFER
};

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view space = " \t\r\n";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(space);
    return text.substr(first, last - first + 1);
}

/// size * count, or nullopt on overflow (libcurl documents size == 1; never trusted).
[[nodiscard]] std::optional<std::size_t> checked_bytes(std::size_t size,
                                                       std::size_t count) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return std::nullopt;
    }
    return size * count;
}

// The callbacks run inside easy_perform on the fetching thread. `userdata` is that fetch's
// TransferState. They never throw (an exception must not cross libcurl's C frames): an
// allocation failure is recorded and aborts the transfer.

/// CURLOPT_WRITEFUNCTION: appends decoded body bytes, enforcing the cap.
// NOLINTNEXTLINE(misc-const-correctness): the signature is libcurl's curl_write_callback
std::size_t on_body(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto* state = static_cast<TransferState*>(userdata);
    const auto bytes = checked_bytes(size, count);
    if (!bytes.has_value()) {
        return 0;
    }
    // Invariant: body.size() <= cap, so the subtraction cannot wrap.
    if (*bytes > state->cap - state->body.size()) {
        state->too_large = true;
        return 0; // != bytes: libcurl aborts with CURLE_WRITE_ERROR
    }
    try {
        state->body.append(data, *bytes);
    } catch (...) {
        state->out_of_memory = true;
        return 0;
    }
    return *bytes;
}

[[nodiscard]] std::string* header_slot(TransferState& state, std::string_view name) noexcept {
    if (ascii_iequals(name, "retry-after")) {
        return &state.retry_after;
    }
    if (ascii_iequals(name, "x-ratelimit-retry-after")) {
        return &state.x_ratelimit_retry_after;
    }
    if (ascii_iequals(name, "x-cache")) {
        return &state.x_cache;
    }
    return nullptr;
}

/// CURLOPT_HEADERFUNCTION: one header line per call (status line included), not NUL-terminated.
/// Copies the three headers the API client needs; a new status line (an interim 1xx response)
/// starts over.
// NOLINTNEXTLINE(misc-const-correctness): the signature is libcurl's curl_write_callback
std::size_t on_header(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto* state = static_cast<TransferState*>(userdata);
    const auto bytes = checked_bytes(size, count);
    if (!bytes.has_value()) {
        return 0;
    }
    const std::string_view line{data, *bytes};
    if (line.starts_with("HTTP/")) {
        state->retry_after.clear();
        state->x_ratelimit_retry_after.clear();
        state->x_cache.clear();
        return *bytes;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return *bytes;
    }
    std::string* slot = header_slot(*state, trim(line.substr(0, colon)));
    if (slot == nullptr) {
        return *bytes;
    }
    const std::string_view value = trim(line.substr(colon + 1));
    if (value.size() > header_value_cap) {
        slot->clear();
        return *bytes;
    }
    try {
        slot->assign(value);
    } catch (...) {
        state->out_of_memory = true;
        return 0;
    }
    return *bytes;
}

/// CURLOPT_XFERINFOFUNCTION: called at least about once a second, also while connecting or
/// stalled. Nonzero aborts the transfer with CURLE_ABORTED_BY_CALLBACK.
// NOLINTNEXTLINE(misc-const-correctness): the signature is libcurl's curl_xferinfo_callback
int on_progress(void* userdata, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) noexcept {
    const auto* state = static_cast<const TransferState*>(userdata);
    return state->cancel.stop_requested() ? 1 : 0;
}

/// A duration count as a long option value: at least 1 (never "no limit"), saturating.
template <class Rep> [[nodiscard]] long positive_long(Rep value) noexcept {
    static_assert(std::is_integral_v<Rep>);
    if (value < 1) {
        return 1L;
    }
    if (!std::in_range<long>(value)) {
        return std::numeric_limits<long>::max();
    }
    if constexpr (std::is_same_v<Rep, long>) {
        return value;
    } else {
        return static_cast<long>(value);
    }
}

[[nodiscard]] curl_off_t to_curl_off(std::size_t value) noexcept {
    return std::in_range<curl_off_t>(value) ? static_cast<curl_off_t>(value)
                                            : std::numeric_limits<curl_off_t>::max();
}

[[nodiscard]] bool has_header_breaking_char(std::string_view text) noexcept {
    return text.find_first_of(std::string_view{"\r\n\0", 3}) != std::string_view::npos;
}

/// Whether a request may carry an Authorization header to `url`: https, or plain http to a
/// loopback host (127.0.0.1, localhost or [::1], the hosts --api-base accepts for http://; see
/// options.cpp). Defense in depth: ApiClient only builds authorized requests for its own base.
/// A URL with user information ('@' in the authority) is refused, so "http://127.0.0.1@host/"
/// cannot pass for loopback.
[[nodiscard]] bool may_carry_authorization(std::string_view url) noexcept {
    constexpr std::string_view https = "https://";
    constexpr std::string_view http = "http://";
    if (url.size() >= https.size() && ascii_iequals(url.substr(0, https.size()), https)) {
        return true;
    }
    if (url.size() < http.size() || !ascii_iequals(url.substr(0, http.size()), http)) {
        return false;
    }
    const std::string_view rest = url.substr(http.size());
    const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.contains('@')) {
        return false;
    }
    // A bracketed IPv6 literal contains ':'; the port separator is the first ':' after it.
    std::size_t host_size = authority.size();
    if (authority.starts_with('[')) {
        if (const std::size_t close = authority.find(']'); close != std::string_view::npos) {
            host_size = close + 1;
        }
    } else if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
        host_size = colon;
    }
    const std::string_view host = authority.substr(0, host_size);
    const std::string_view after_host = authority.substr(host_size);
    if (!after_host.empty() && !after_host.starts_with(':')) {
        return false;
    }
    return ascii_iequals(host, "127.0.0.1") || ascii_iequals(host, "localhost") ||
           ascii_iequals(host, "[::1]");
}

[[nodiscard]] TransportError make_error(TransportErrc kind, std::string message) {
    return TransportError{.kind = kind, .message = std::move(message)};
}

[[nodiscard]] TransportErrc classify(CURLcode rc, const TransferState& state) noexcept {
    if (state.too_large) {
        return TransportErrc::too_large;
    }
    if (state.out_of_memory) {
        return TransportErrc::local;
    }
    switch (rc) {
    case CURLE_COULDNT_CONNECT:
        return TransportErrc::connect;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return TransportErrc::resolve;
    case CURLE_OPERATION_TIMEDOUT:
        return TransportErrc::timeout;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_CLIENTCERT:
    case CURLE_USE_SSL_FAILED:
        return TransportErrc::tls;
    case CURLE_ABORTED_BY_CALLBACK:
        return state.cancel.stop_requested() ? TransportErrc::cancelled : TransportErrc::protocol;
    case CURLE_FILESIZE_EXCEEDED:
        return TransportErrc::too_large;
    case CURLE_OUT_OF_MEMORY:
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_FAILED_INIT:
    case CURLE_NOT_BUILT_IN:
    case CURLE_UNKNOWN_OPTION:
    case CURLE_BAD_FUNCTION_ARGUMENT:
        return TransportErrc::local;
    default:
        return TransportErrc::protocol; // reset, empty reply, short body, malformed response
    }
}

/// One line: control characters (a server-supplied byte could reach the error buffer) become
/// spaces.
[[nodiscard]] std::string one_line(std::string text) {
    std::ranges::replace_if(
        text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == '\x7f'; }, ' ');
    return text;
}

[[nodiscard]] std::string describe(const CurlApi& api, CURLcode rc, const TransferState& state,
                                   TransportErrc kind) {
    switch (kind) {
    case TransportErrc::too_large:
        return std::format("response body exceeds {} bytes", state.cap);
    case TransportErrc::cancelled:
        return "cancelled";
    case TransportErrc::local:
        if (state.out_of_memory) {
            return "out of memory while receiving the response";
        }
        break;
    case TransportErrc::connect:
    case TransportErrc::resolve:
    case TransportErrc::tls:
    case TransportErrc::timeout:
    case TransportErrc::protocol:
        break;
    }
    const auto& buffer = state.error_buffer;
    const auto* const end = std::ranges::find(buffer, '\0');
    std::string text = end == buffer.begin() ? std::string{api.easy_strerror(rc)}
                                             : std::string{buffer.begin(), end};
    text = one_line(std::move(text));
    if (rc == CURLE_SSL_CACERT_BADFILE) {
        text += " (no CA certificates found; install ca-certificates)";
    }
    return text;
}

/// Applies the full option set for one request to a freshly reset handle. libcurl copies string
/// options; the pointers set here (`state`, its error buffer, `headers`, and the borrowed
/// POSTFIELDS `request.body`) stay valid until the handle is reset at the end of the fetch.
///
/// CURLOPT_VERBOSE is never set, and must not be: libcurl would print every request header,
/// the Authorization bearer token included, to stderr (V12).
[[nodiscard]] std::expected<void, TransportError>
configure(const CurlApi& api, CURL* handle, const HttpRequest& request,
          const std::string& user_agent, std::chrono::milliseconds overall_timeout,
          TransferState& state, SlistPtr& headers) {
    CURLcode rc = CURLE_OK;
    std::string_view failed{};
    const auto apply = [&rc, &failed](CURLcode result, std::string_view option) {
        if (rc == CURLE_OK && result != CURLE_OK) {
            rc = result;
            failed = option;
        }
    };

    apply(api.setopt_str(handle, CURLOPT_URL, request.url.c_str()), "URL");
    apply(api.setopt_str(handle, CURLOPT_PROTOCOLS_STR, detail::curl_allowed_protocols),
          "PROTOCOLS_STR");
    apply(api.setopt_long(handle, CURLOPT_FOLLOWLOCATION, 0L), "FOLLOWLOCATION");
    apply(api.setopt_long(handle, CURLOPT_NOSIGNAL, 1L), "NOSIGNAL");
    apply(api.setopt_long(handle, CURLOPT_SSL_VERIFYPEER, 1L), "SSL_VERIFYPEER");
    apply(api.setopt_long(handle, CURLOPT_SSL_VERIFYHOST, 2L), "SSL_VERIFYHOST");
    if (const std::optional<std::string>& ca = api.ca_bundle(); ca.has_value()) {
        apply(api.setopt_str(handle, CURLOPT_CAINFO, ca->c_str()), "CAINFO");
    }
    apply(api.setopt_long(handle, CURLOPT_CONNECTTIMEOUT_MS,
                          positive_long(request.connect_timeout.count())),
          "CONNECTTIMEOUT_MS");
    apply(api.setopt_long(handle, CURLOPT_TIMEOUT_MS, positive_long(overall_timeout.count())),
          "TIMEOUT_MS");
    apply(api.setopt_long(handle, CURLOPT_LOW_SPEED_LIMIT, 1L), "LOW_SPEED_LIMIT");
    apply(api.setopt_long(handle, CURLOPT_LOW_SPEED_TIME,
                          positive_long(request.stall_timeout.count())),
          "LOW_SPEED_TIME");
    if (request.max_body_bytes > 0) { // 0 would mean "no limit" to libcurl
        apply(
            api.setopt_off(handle, CURLOPT_MAXFILESIZE_LARGE, to_curl_off(request.max_body_bytes)),
            "MAXFILESIZE_LARGE");
    }
    apply(api.setopt_str(handle, CURLOPT_ACCEPT_ENCODING, ""), "ACCEPT_ENCODING");
    apply(api.setopt_str(handle, CURLOPT_USERAGENT, user_agent.c_str()), "USERAGENT");
    apply(api.setopt_ptr(handle, CURLOPT_ERRORBUFFER, state.error_buffer.data()), "ERRORBUFFER");
    apply(api.setopt_write_cb(handle, CURLOPT_WRITEFUNCTION, &on_body), "WRITEFUNCTION");
    apply(api.setopt_ptr(handle, CURLOPT_WRITEDATA, &state), "WRITEDATA");
    apply(api.setopt_write_cb(handle, CURLOPT_HEADERFUNCTION, &on_header), "HEADERFUNCTION");
    apply(api.setopt_ptr(handle, CURLOPT_HEADERDATA, &state), "HEADERDATA");
    apply(api.setopt_long(handle, CURLOPT_NOPROGRESS, 0L), "NOPROGRESS");
    apply(api.setopt_xfer_cb(handle, CURLOPT_XFERINFOFUNCTION, &on_progress), "XFERINFOFUNCTION");
    apply(api.setopt_ptr(handle, CURLOPT_XFERINFODATA, &state), "XFERINFODATA");

    // On failure curl_slist_append leaves the list untouched, so ownership goes back. `line` is
    // NUL-terminated; libcurl copies it into a new node.
    const auto append_header = [&api, &headers](const char* line) {
        curl_slist* const previous = headers.release();
        curl_slist* const head = api.slist_append(previous, line);
        headers.reset(head == nullptr ? previous : head);
        return head != nullptr;
    };
    const auto out_of_memory = [] {
        return std::unexpected(
            make_error(TransportErrc::local, "out of memory building request headers"));
    };

    switch (request.method) {
    case HttpMethod::get:
        apply(api.setopt_long(handle, CURLOPT_HTTPGET, 1L), "HTTPGET");
        break;
    case HttpMethod::post: {
        apply(api.setopt_long(handle, CURLOPT_POST, 1L), "POST");
        // The size first, so libcurl sends exactly body.size() bytes (no strlen). POSTFIELDS is
        // borrowed, not copied: `request` is the caller's and outlives this fetch(), and the
        // lease resets the handle (forgetting the pointer) before fetch() returns (V13).
        apply(api.setopt_off(handle, CURLOPT_POSTFIELDSIZE_LARGE, to_curl_off(request.body.size())),
              "POSTFIELDSIZE_LARGE");
        apply(api.setopt_borrowed(handle, CURLOPT_POSTFIELDS, request.body.data()), "POSTFIELDS");
        // "Expect:" suppresses the 100-continue round trip.
        if (!append_header("Expect:")) {
            return out_of_memory();
        }
        if (!request.content_type.empty()) {
            const std::string line = "Content-Type: " + request.content_type;
            if (!append_header(line.c_str())) {
                return out_of_memory();
            }
        }
        break;
    }
    }

    if (!request.authorization.empty()) {
        // Assembled in a Secret (wiped when it dies), never in a plain std::string temporary. The
        // explicit trailing NUL makes the view a C string; reserved up front, so nothing grows.
        constexpr std::string_view name = "Authorization: ";
        Secret line{};
        line.reserve(name.size() + request.authorization.size() + 1);
        line.append(name);
        line.append(request.authorization);
        line.push_back('\0');
        if (!append_header(line.reveal().data())) {
            return out_of_memory();
        }
    }
    // A GET without authorization has no list: exactly the options it always had.
    if (headers != nullptr) {
        apply(api.setopt_slist(handle, CURLOPT_HTTPHEADER, headers.get()), "HTTPHEADER");
    }

    if (rc != CURLE_OK) {
        return std::unexpected(
            make_error(TransportErrc::local, std::format("libcurl rejected CURLOPT_{}: {}", failed,
                                                         api.easy_strerror(rc))));
    }
    return {};
}

[[nodiscard]] std::optional<TransportError> validate(const HttpRequest& request) {
    if (request.url.empty() || request.url.contains('\0')) {
        return make_error(TransportErrc::local, "invalid URL (empty or contains a NUL byte)");
    }
    if (request.method == HttpMethod::post && has_header_breaking_char(request.content_type)) {
        return make_error(TransportErrc::local, "invalid Content-Type (contains CR, LF or NUL)");
    }
    // The value is never echoed: it holds a token.
    if (has_header_breaking_char(request.authorization)) {
        return make_error(TransportErrc::local,
                          "invalid Authorization header (contains CR, LF or NUL)");
    }
    if (!request.authorization.empty() && !may_carry_authorization(request.url)) {
        return make_error(TransportErrc::local,
                          "refusing to send an Authorization header over plain http (only "
                          "https, or http to 127.0.0.1, localhost or [::1])");
    }
    return std::nullopt;
}

class CurlTransport final : public Transport {
public:
    CurlTransport(std::shared_ptr<const CurlApi> api, std::string user_agent,
                  std::chrono::milliseconds overall_timeout)
        : api_(std::move(api)), user_agent_(std::move(user_agent)),
          overall_timeout_(overall_timeout) {
        free_.reserve(detail::curl_max_pooled_handles); // release() then never reallocates
    }

    [[nodiscard]] TransportResult fetch(const HttpRequest& request) override {
        if (request.cancel.stop_requested()) {
            return std::unexpected(make_error(TransportErrc::cancelled, "cancelled"));
        }
        if (auto invalid = validate(request); invalid.has_value()) {
            return std::unexpected(std::move(*invalid));
        }
        // Declaration order is load-bearing: the lease is destroyed first, so the handle is reset
        // (forgetting &state, the header list and the borrowed request.body) before `state` and
        // `headers` die; SlistFree wipes the header lines only after that reset.
        TransferState state{.cap = request.max_body_bytes, .cancel = request.cancel};
        SlistPtr headers{nullptr, SlistFree{.api = api_.get()}};
        const HandleLease lease{*this, acquire()};
        if (lease.get() == nullptr) {
            return std::unexpected(make_error(TransportErrc::local, "curl_easy_init failed"));
        }
        if (auto configured = configure(*api_, lease.get(), request, user_agent_, overall_timeout_,
                                        state, headers);
            !configured.has_value()) {
            return std::unexpected(std::move(configured.error()));
        }

        const auto start = std::chrono::steady_clock::now();
        CURLcode rc = CURLE_OK;
        {
            const SigpipeGuard guard;
            rc = api_->easy_perform(lease.get());
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (rc != CURLE_OK || state.too_large || state.out_of_memory) {
            const TransportErrc kind = classify(rc, state);
            return std::unexpected(make_error(kind, describe(*api_, rc, state, kind)));
        }
        long code = 0;
        if (api_->getinfo_long(lease.get(), CURLINFO_RESPONSE_CODE, &code) != CURLE_OK ||
            !std::in_range<int>(code) || code < 100) {
            return std::unexpected(
                make_error(TransportErrc::protocol, "no valid HTTP status in the response"));
        }
        return HttpResponse{
            .status = static_cast<int>(code),
            .body = std::move(state.body),
            .retry_after = std::move(state.retry_after),
            .x_ratelimit_retry_after = std::move(state.x_ratelimit_retry_after),
            .x_cache = std::move(state.x_cache),
            .elapsed = elapsed,
        };
    }

private:
    /// Returns its handle to the pool on every path, reset first.
    class HandleLease {
    public:
        HandleLease(CurlTransport& owner, EasyPtr handle) noexcept
            : owner_(&owner), handle_(std::move(handle)) {}
        HandleLease(const HandleLease&) = delete;
        HandleLease& operator=(const HandleLease&) = delete;
        HandleLease(HandleLease&&) = delete;
        HandleLease& operator=(HandleLease&&) = delete;
        ~HandleLease() { owner_->release(std::move(handle_)); }

        [[nodiscard]] CURL* get() const noexcept { return handle_.get(); }

    private:
        CurlTransport* owner_ = nullptr; ///< the transport, which outlives every fetch
        EasyPtr handle_{};
    };

    /// A pooled handle (already reset), or a new one; null when curl_easy_init fails.
    [[nodiscard]] EasyPtr acquire() {
        {
            const std::scoped_lock lock{mutex_};
            if (!free_.empty()) {
                EasyPtr handle = std::move(free_.back());
                free_.pop_back();
                return handle;
            }
        }
        return EasyPtr{api_->easy_init(), EasyCleanup{.api = api_.get()}};
    }

    /// Resets the handle (dropping every option and userdata pointer; live connections stay
    /// cached) and pools it, or closes it when the pool is full.
    void release(EasyPtr handle) noexcept {
        if (handle == nullptr) {
            return;
        }
        api_->easy_reset(handle.get());
        try {
            const std::scoped_lock lock{mutex_};
            if (free_.size() < detail::curl_max_pooled_handles) {
                free_.push_back(std::move(handle)); // capacity reserved: never reallocates
            }
        } catch (...) {
            handle.reset(); // the mutex failed: close the handle instead of pooling it
        }
    } // a handle that was not pooled is closed here, outside the lock

    std::shared_ptr<const CurlApi> api_{}; ///< first member: outlives every handle below
    std::string user_agent_{};
    std::chrono::milliseconds overall_timeout_{detail::curl_overall_timeout};
    std::mutex mutex_{};
    std::vector<EasyPtr> free_{}; ///< guarded by mutex_
};

} // namespace

Result<std::unique_ptr<Transport>> make_curl_transport(const CurlTransportConfig& config) {
    return detail::make_curl_transport_with_deadline(config, detail::curl_overall_timeout);
}

Result<std::unique_ptr<Transport>>
detail::make_curl_transport_with_deadline(const CurlTransportConfig& config,
                                          std::chrono::milliseconds overall_timeout) {
    if (config.user_agent.empty() || has_header_breaking_char(config.user_agent)) {
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = "invalid User-Agent (empty, or contains CR, LF or NUL)",
        });
    }
    auto api = detail::CurlApi::load();
    if (!api.has_value()) {
        return std::unexpected(std::move(api.error()));
    }
    return std::unique_ptr<Transport>{
        std::make_unique<CurlTransport>(std::move(*api), config.user_agent, overall_timeout)};
}

std::optional<std::string>
select_ca_bundle(const CaDefaults& defaults,
                 const std::function<bool(const std::string&)>& readable) {
    if (!readable) {
        return std::nullopt;
    }
    // libcurl's own default stays in charge when it is usable. A configured cainfo that cannot be
    // read fails every handshake even with a good capath, so it falls through to the probe list;
    // capath alone counts only when there is no cainfo at all.
    if (defaults.cainfo.has_value()) {
        if (readable(*defaults.cainfo)) {
            return std::nullopt;
        }
    } else if (defaults.capath.has_value() && readable(*defaults.capath)) {
        return std::nullopt;
    }
    for (const std::string_view candidate : ca_bundle_candidates) {
        std::string path{candidate};
        if (readable(path)) {
            return path;
        }
    }
    return std::nullopt;
}

} // namespace mangadex_dl
