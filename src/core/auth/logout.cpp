// SPDX-License-Identifier: GPL-3.0-or-later
//
// logout (auth.hpp; follows plan T12, spec "logout" + Spec issue 9 + rev3 S4):
//   endpoints (nullopt -> usage, no I/O) -> the state directory, never created (missing -> not
//   logged in; insecure or unusable -> Errc::failure "could not remove the saved login at <path>:
//   <why>", nothing touched) -> the session lock -> read and parse the slot (fixture slot iff a
//   fixture API base) -> revoke the refresh token, best effort -> unlink the slot.
//
// - A revoke succeeds on any HTTP 2xx, whatever the body (Keycloak answers 200 even with a JSON
//   error for an unknown token, V3); a 3xx (redirects are off), 4xx, 5xx or transport error is a
//   failed revoke: a warning, and the file is removed anyway. An interrupt during the revoke
//   leaves the file in place (Errc::interrupted), so a second logout can finish the job.
// - An unreadable or malformed session (also one for another login server, or of an unknown
//   format_version) is removed without a revoke, with the Spec issue 9 warning.
// - A session without a refresh token has nothing to revoke: it is just removed.
// - StateDir::read/remove run only while the SessionLock is held (the store's stale-temp sweep
//   relies on it): `lock` is declared after `dir`, so it is released first, after the unlink.
// - logout never reads or touches the credentials file.
//
// Secret hygiene: the session's client secret and refresh token stay in Secrets; the revoke body
// is built straight into one; no Error or warning ever holds a secret.
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"

#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {
namespace {

inline constexpr std::string_view unreadable_warning =
    "could not end the session at MangaDex (the saved login is unreadable); the saved login was "
    "removed anyway";

[[nodiscard]] Error cannot_remove(const std::filesystem::path& session_path, std::string_view why) {
    return Error{.code = Errc::failure,
                 .message = std::format("could not remove the saved login at {}: {}",
                                        session_path.string(), why)};
}

/// The revoke of `session`'s refresh token. nullopt: revoked, or nothing to revoke; otherwise the
/// warning (or Errc::interrupted, which the caller returns as is).
[[nodiscard]] Result<std::optional<std::string>> revoke_session(const Session& session,
                                                                const AuthEndpoints& endpoints,
                                                                const AuthServices& services,
                                                                const std::stop_token& stop) {
    if (!session.refresh_token.has_value()) {
        return std::optional<std::string>{};
    }
    AuthClient client{*services.transport, *services.clock, endpoints, services.jitter,
                      services.trace};
    const ClientCredentials credentials{
        .client_id = session.client_id,
        .client_secret = Secret::copy_of(session.client_secret.reveal()),
    };
    const Secret body = revoke_body(credentials, *session.refresh_token);
    Result<void> revoked = client.revoke(body, stop);
    if (revoked) {
        return std::optional<std::string>{};
    }
    if (revoked.error().code == Errc::interrupted) {
        return std::unexpected(std::move(revoked).error());
    }
    return std::optional<std::string>{
        std::format("could not end the session at MangaDex ({}); the saved login was removed "
                    "anyway",
                    revoked.error().message)};
}

} // namespace

Result<LogoutOutcome> logout(const ApiBase& api_base, const UserDirs& dirs,
                             const AuthServices& services, LockTiming timing,
                             // NOLINTNEXTLINE(performance-unnecessary-value-param): frozen (T1)
                             std::stop_token stop) {
    const std::optional<AuthEndpoints> endpoints = auth_endpoints_for(api_base);
    if (!endpoints.has_value()) {
        return std::unexpected(Error{.code = Errc::usage,
                                     .message = "a MangaDex logout works only with the official "
                                                "API; --api-base must be the official API"});
    }
    if (services.transport == nullptr || services.clock == nullptr) {
        return std::unexpected(Error{.code = Errc::failure,
                                     .message = "the MangaDex logout has no transport or clock"});
    }
    const SessionSlot slot =
        api_base.kind() == ApiBase::Kind::fixture ? SessionSlot::fixture : SessionSlot::real;

    const std::optional<std::filesystem::path> dir_path = state_dir_path(dirs);
    if (!dir_path.has_value()) {
        return LogoutOutcome{}; // no home directory: there can be no saved login (path empty)
    }
    const std::filesystem::path session_path = *dir_path / session_file_name(slot);

    FileResult<StateDir> dir = StateDir::open(*dir_path, false);
    if (!dir) {
        if (dir.error().kind == FileProblemKind::missing) {
            return LogoutOutcome{.path = session_path};
        }
        return std::unexpected(
            cannot_remove(session_path, std::format("the directory {} is not usable: {}",
                                                    dir_path->string(), dir.error().reason)));
    }
    // Declared after `dir`: released first, after the last call that needs it.
    Result<SessionLock> lock = dir->lock(*services.clock, timing, stop);
    if (!lock) {
        return std::unexpected(std::move(lock).error());
    }

    // The lock is held from here on (StateDir::read/remove's precondition).
    const FileResult<std::optional<Secret>> bytes = dir->read(slot);
    std::optional<std::string> warning{};
    if (bytes.has_value()) {
        const std::optional<Secret>& document = *bytes;
        if (!document.has_value()) {
            return LogoutOutcome{.path = session_path};
        }
        Result<Session> session = parse_session(document->reveal(), endpoints->issuer);
        if (session.has_value()) {
            Result<std::optional<std::string>> revoked =
                revoke_session(*session, *endpoints, services, stop);
            if (!revoked) {
                return std::unexpected(std::move(revoked).error());
            }
            warning = std::move(*revoked);
        } else {
            warning = std::string{unreadable_warning};
        }
    } else {
        warning = std::string{unreadable_warning}; // unreadable: removed without a revoke
    }

    // The lock is still held: StateDir::remove's precondition.
    const Result<bool> removed = dir->remove(slot);
    if (!removed) {
        return std::unexpected(cannot_remove(session_path, removed.error().message));
    }
    if (!*removed) {
        return LogoutOutcome{.path = session_path}; // gone already (not by a mangadex-dl)
    }
    return LogoutOutcome{.removed = true, .path = session_path, .warning = std::move(warning)};
}

} // namespace mangadex_dl
