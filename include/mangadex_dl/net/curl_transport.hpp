// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real HTTPS transport: libcurl, loaded at runtime with dlopen (never linked). No curl
// header is included here; curl types stay private to src/core/net/.
// Frozen interface (download plan T1); implemented in src/core/net/curl_transport.cpp.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/transport.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl {

struct CurlTransportConfig {
    std::string user_agent{}; ///< exactly "mangadex-dl/<version>"
};

/// Loads libcurl.so.4 (then libcurl-gnutls.so.4) once per process, checks the runtime version
/// (>= 7.85.0) and TLS/https support, and returns a thread-safe Transport. TLS peer and host
/// verification are always on. Errc::failure with an actionable message when no usable libcurl
/// is installed. Never called in fixture mode.
[[nodiscard]] Result<std::unique_ptr<Transport>>
make_curl_transport(const CurlTransportConfig& config);

/// libcurl's compiled-in CA locations (curl_version_info), when it reports them.
struct CaDefaults {
    std::optional<std::string> cainfo{};
    std::optional<std::string> capath{};

    friend bool operator==(const CaDefaults&, const CaDefaults&) = default;
};

/// Probed, in order, when libcurl's own defaults are unusable. The views point at string
/// literals (static storage); convert to std::string before handing one to a C API.
inline constexpr std::array<std::string_view, 6> ca_bundle_candidates{
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    "/etc/ssl/cert.pem",
    "/etc/pki/tls/cacert.pem",
};

/// The CAINFO to set, or nullopt to leave CAINFO unset:
/// - nullopt when libcurl's default is usable: `cainfo` present and `readable`, or (only when
///   there is no `cainfo` at all) `capath` present and `readable`. A present but unreadable
///   `cainfo` is never "usable", even with a readable `capath`: libcurl would fail every
///   handshake on it;
/// - otherwise the first `readable` entry of ca_bundle_candidates;
/// - nullopt when none is readable (a later CA error then carries an install hint).
/// `readable` is only called during this function.
[[nodiscard]] std::optional<std::string>
select_ca_bundle(const CaDefaults& defaults,
                 const std::function<bool(const std::string&)>& readable);

} // namespace mangadex_dl
