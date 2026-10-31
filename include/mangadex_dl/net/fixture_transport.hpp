// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hermetic transport behind `--api-base file://<root>`: maps file:// URLs onto a fixture tree.
// Frozen interface (download plan T1; auth routing follows plan T1); implemented in
// src/core/net/fixture_transport.cpp.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/transport.hpp"

#include <filesystem>
#include <memory>

namespace mangadex_dl {

/// A thread-safe Transport serving `file://<root>/...` from the tree under `root` (absolute):
/// `api/<path>?...offset=<n>` reads `api/<path>/offset-<n>.json`, other `api/<path>` reads
/// `api/<path>.json`, anything else is read verbatim. Every component is opened with
/// O_NOFOLLOW relative to the previous one, so nothing outside `root` is reachable: a missing
/// file or a symlink component is HTTP 404, and `.`, `..`, empty segments or a URL outside
/// `root` are rejected. Never sleeps and never touches the network.
///
/// Auth routing (follows plan T6), a stand-in for MangaDex's Keycloak and its authorized GETs:
/// - POST `file://<root>/auth/realms/mangadex/protocol/openid-connect/token` (form Content-Type
///   required, body at most 64 KiB): `grant_type=password` answers from
///   `<root>/auth/password/<username>.json`, `grant_type=refresh_token` from
///   `<root>/auth/refresh/<token>.json`. A missing file or a password mismatch answers 401
///   `invalid_grant` (a password file may ask for 400 instead), a missing refresh file 400
///   `invalid_grant`, a client mismatch 401 `invalid_client`; otherwise the file's scripted
///   status and body.
/// - POST `…/protocol/openid-connect/revoke` answers from `<root>/auth/revoke/<token>.json`; a
///   missing file answers 200 `{"error":"invalid_token"}` (Keycloak's behaviour).
/// - Every file name derived from a form field or from the Bearer token must match
///   `[A-Za-z0-9._-]{1,128}` and not be `.` or `..`, else TransportErrc::local "fixture: unsafe
///   auth name". Every other POST answers 404 (MD@H).
/// - Only `api/user/follows/manga` (with `offset=`) and `api/manga/status` accept an
///   Authorization: `Bearer <T>` with `<root>/auth/access/<T>.json` present is served from the
///   normal mapping (optionally for a limited number of requests per token), anything else is
///   401. An `api/user/…` GET without a token is 401.
/// - Leak tripwire: a non-empty authorization on any other URL is TransportErrc::local
///   "fixture: Authorization sent to <url>" (the URL, never the token).
///
/// Errc::failure when `root` cannot be opened as a directory.
[[nodiscard]] Result<std::unique_ptr<Transport>> make_fixture_transport(std::filesystem::path root);

} // namespace mangadex_dl
