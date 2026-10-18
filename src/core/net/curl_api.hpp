// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): the dlopen()ed libcurl entry points behind CurlTransport.
// libcurl is never linked. CurlApi::load() opens libcurl.so.4 (then libcurl-gnutls.so.4) once per
// process with RTLD_NOW | RTLD_LOCAL, binds the 12 functions below with dlsym, checks the runtime
// (check_runtime) and picks the CA bundle (select_ca_bundle). The library is never dlclosed.
//
// The typed setopt/getinfo wrappers (plus the one load-time CURLOPT_PROTOCOLS_STR probe) are the
// only variadic call sites: each passes exactly the C type libcurl reads with va_arg for that kind
// of option (long, curl_off_t, char*, void*, or the callback pointer type), so a caller cannot
// pass an int where a long is read.
//
// Unit tests include this header directly (src/core is on their include path).
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/curl_transport.hpp"
#include "mangadex_dl/net/transport.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <curl/curl.h>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigset_t and pthread_sigmask are POSIX <signal.h>
#include <signal.h>

namespace mangadex_dl::detail {

/// The oldest libcurl accepted at runtime: 7.85.0, the first with CURLOPT_PROTOCOLS_STR
/// (gate decision 10). Every option the transport sets exists in it.
inline constexpr unsigned int curl_min_version_num = 0x075500;

/// CURLOPT_PROTOCOLS_STR for every request. Mandatory: load() fails when libcurl rejects it.
/// Production endpoints are https; plain http exists only for a loopback --api-base (the app
/// enforces that). Redirects are never followed, so no REDIR_PROTOCOLS_STR is needed.
inline constexpr const char* curl_allowed_protocols = "https,http";

/// Idle easy handles one CurlTransport keeps for reuse. More concurrent fetches still work; the
/// handles beyond this are closed when they come back.
inline constexpr std::size_t curl_max_pooled_handles = 16;

/// CURLOPT_TIMEOUT_MS for every request made through make_curl_transport: an overall deadline,
/// so a server that drips just above the stall threshold cannot hold a worker for hours. Generous
/// on purpose (the largest capped body at a slow 1 Mbit/s is a few minutes). Maps to
/// TransportErrc::timeout.
inline constexpr std::chrono::milliseconds curl_overall_timeout = std::chrono::minutes{10};

/// make_curl_transport with an injected overall deadline (clamped to at least 1 ms) instead of
/// curl_overall_timeout. A testing seam: production code calls make_curl_transport.
[[nodiscard]] Result<std::unique_ptr<Transport>>
make_curl_transport_with_deadline(const CurlTransportConfig& config,
                                  std::chrono::milliseconds overall_timeout);

/// Pure: whether a curl_version_info() result describes a usable runtime (>= 7.85.0, built with
/// TLS, offering https). Errc::failure with a one-line reason otherwise. `protocols` may be null.
[[nodiscard]] Result<void> check_runtime(const curl_version_info_data& info);

/// Pure: libcurl's compiled-in CA locations. Read only when `age` says the struct has them
/// (CURLVERSION_SEVENTH, 7.70.0); a NULL field is "no default".
[[nodiscard]] CaDefaults ca_defaults_from(const curl_version_info_data& info);

/// The production `readable` predicate for select_ca_bundle: `path` opens for reading
/// (O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC; a FIFO never blocks) and fstat() shows a regular
/// file, or a directory when `directory_ok` (libcurl's capath). Never follows anything but the
/// path itself; never reads the contents.
[[nodiscard]] bool ca_location_readable(const std::string& path, bool directory_ok) noexcept;

/// Blocks SIGPIPE in the calling thread while alive. On destruction it consumes a SIGPIPE that
/// became pending meanwhile (sigtimedwait with a zero timeout) and restores the previous mask
/// exactly, so a write to a dead peer inside libcurl/OpenSSL never kills the process, on any
/// thread. Wrap every call that may write to a socket (perform, cleanup) in one.
class SigpipeGuard {
public:
    SigpipeGuard() noexcept;
    SigpipeGuard(const SigpipeGuard&) = delete;
    SigpipeGuard& operator=(const SigpipeGuard&) = delete;
    SigpipeGuard(SigpipeGuard&&) = delete;
    SigpipeGuard& operator=(SigpipeGuard&&) = delete;
    ~SigpipeGuard();

private:
    sigset_t previous_{};
    bool blocked_ = false;
};

/// The bound entry points. All non-null once a CurlApi exists.
struct CurlFunctions {
    decltype(&curl_global_init) global_init = nullptr;
    decltype(&curl_global_cleanup) global_cleanup = nullptr;
    decltype(&curl_version_info) version_info = nullptr;
    decltype(&curl_easy_init) easy_init = nullptr;
    decltype(&curl_easy_cleanup) easy_cleanup = nullptr;
    decltype(&curl_easy_reset) easy_reset = nullptr;
    decltype(&curl_easy_setopt) easy_setopt = nullptr;
    decltype(&curl_easy_perform) easy_perform = nullptr;
    decltype(&curl_easy_getinfo) easy_getinfo = nullptr;
    decltype(&curl_easy_strerror) easy_strerror = nullptr;
    decltype(&curl_slist_append) slist_append = nullptr;
    decltype(&curl_slist_free_all) slist_free_all = nullptr;
};

/// One curl_global_init() reference on the dlopen()ed library. load() hands every caller the same
/// live instance; when the last shared_ptr goes, the destructor calls curl_global_cleanup() (both
/// serialized by one process-wide mutex, so a later load() initializes again). A CurlApi must not
/// outlive main(). Every member function is thread-safe (distinct handles per thread).
class CurlApi {
    struct Key {
        explicit Key() = default;
    };

public:
    /// Loads and checks libcurl on first use (thread-safe; the outcome, success or failure, is
    /// cached for the process), then takes a global-init reference. Errc::failure with an
    /// actionable message when no usable libcurl is installed.
    [[nodiscard]] static Result<std::shared_ptr<const CurlApi>> load();

    /// Only load() can call this (Key is private).
    CurlApi(Key /*key*/, const CurlFunctions& functions, std::optional<std::string> ca_bundle);
    CurlApi(const CurlApi&) = delete;
    CurlApi& operator=(const CurlApi&) = delete;
    CurlApi(CurlApi&&) = delete;
    CurlApi& operator=(CurlApi&&) = delete;
    ~CurlApi();

    /// The CAINFO to set on every handle (select_ca_bundle at load time), or nullopt to keep
    /// libcurl's compiled-in default.
    [[nodiscard]] const std::optional<std::string>&
    ca_bundle() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return ca_bundle_;
    }

    [[nodiscard]] CURL* easy_init() const noexcept;
    void easy_cleanup(CURL* handle) const noexcept; ///< inside a SigpipeGuard
    void easy_reset(CURL* handle) const noexcept;
    [[nodiscard]] CURLcode easy_perform(CURL* handle) const noexcept; ///< caller guards SIGPIPE
    /// The static message for `code` (never null).
    [[nodiscard]] const char* easy_strerror(CURLcode code) const noexcept;

    [[nodiscard]] CURLcode setopt_long(CURL* handle, CURLoption option, long value) const noexcept;
    [[nodiscard]] CURLcode setopt_off(CURL* handle, CURLoption option,
                                      curl_off_t value) const noexcept;
    /// `value` must be NUL-terminated; libcurl copies string options (CURLOPTTYPE_STRINGPOINT).
    [[nodiscard]] CURLcode setopt_str(CURL* handle, CURLoption option,
                                      const char* value) const noexcept;
    /// A borrowed pointer (userdata, buffers, lists): it must stay valid until easy_reset.
    [[nodiscard]] CURLcode setopt_ptr(CURL* handle, CURLoption option, void* value) const noexcept;
    /// HTTPHEADER and friends: a borrowed list that must stay valid until easy_reset.
    [[nodiscard]] CURLcode setopt_slist(CURL* handle, CURLoption option,
                                        curl_slist* list) const noexcept;
    [[nodiscard]] CURLcode setopt_write_cb(CURL* handle, CURLoption option,
                                           curl_write_callback callback) const noexcept;
    [[nodiscard]] CURLcode setopt_xfer_cb(CURL* handle, CURLoption option,
                                          curl_xferinfo_callback callback) const noexcept;
    [[nodiscard]] CURLcode getinfo_long(CURL* handle, CURLINFO info, long* out) const noexcept;

    /// curl_slist_append: the new head, or null on failure (then `list` is unchanged).
    [[nodiscard]] curl_slist* slist_append(curl_slist* list, const char* value) const noexcept;
    void slist_free_all(curl_slist* list) const noexcept;

private:
    CurlFunctions fns_{};
    std::optional<std::string> ca_bundle_{};
};

} // namespace mangadex_dl::detail
