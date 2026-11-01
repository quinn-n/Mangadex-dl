// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logging in to MangaDex (Keycloak, OAuth2 password grant with a personal API client): the
// limits, the token and credential types, the raw CLI arguments of follows/login/logout, the pure
// codecs (form bodies, the credentials file, the session document, token responses and refusals),
// the token client, the token-provider seam the collection fetch uses, the login session and
// logout. Frozen interface (follows plan T1); implemented in src/core/auth/*.cpp (T6, T8, T9, T11,
// T12).
//
// Secret hygiene: no password, client secret or token ever appears in an Error message, a trace
// line, --json or a format argument. Every function that builds or holds one does it in a Secret.
#pragma once

#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {

// ---- Constants ----------------------------------------------------------------------------

/// MangaDex's Keycloak realm. Token: <issuer>/protocol/openid-connect/token; revoke:
/// <issuer>/protocol/openid-connect/revoke (fixed, no discovery request).
inline constexpr std::string_view official_auth_issuer =
    "https://auth.mangadex.org/realms/mangadex";
/// Where a user creates and checks a personal API client.
inline constexpr std::string_view api_clients_url = "https://mangadex.org/settings";

/// The longest access or refresh token accepted.
inline constexpr std::size_t max_token_bytes = 16384;
/// Body cap of a token or revoke response.
inline constexpr std::size_t max_token_response_bytes = std::size_t{64} * 1024;
/// Size cap of the credentials file.
inline constexpr std::size_t max_credentials_file_bytes = std::size_t{16} * 1024;
/// Size cap of a session file.
inline constexpr std::size_t max_session_file_bytes = std::size_t{64} * 1024;
/// The most bytes --password-stdin reads.
inline constexpr std::size_t max_password_stdin_bytes = 4096;
/// At most this many logical token requests (password or refresh grants) per run; each may retry
/// transport errors, 5xx and 429 within its RetryConfig. The revoke is not counted.
inline constexpr int max_token_requests = 3;
/// A saved access token is reused only if it stays valid at least this long.
inline constexpr std::chrono::seconds reuse_min_validity{60};
/// An access token expiring within this long is refreshed before a request.
inline constexpr std::chrono::seconds refresh_ahead{30};
/// expires_in when a token response has no usable one.
inline constexpr std::chrono::seconds default_access_lifetime{900};
/// The largest expires_in accepted.
inline constexpr std::chrono::seconds max_access_lifetime{86400};
/// The token client's own rate limit between requests.
inline constexpr std::chrono::milliseconds auth_request_interval{1000};
/// Connect and stall limit of the revoke request (logout never hangs on it).
inline constexpr std::chrono::seconds revoke_timeout{5};

// ---- Tokens and credentials -----------------------------------------------------------------

/// A validated OAuth2 access or refresh token. Move-only (through Secret).
class BearerToken {
public:
    /// An empty placeholder for default-constructed aggregates; parse() never yields it.
    BearerToken() = default;

    /// Accepts `[A-Za-z0-9._~+/=-]{1,16384}` (max_token_bytes); anything else is Errc::network
    /// with a fixed message that never contains the token.
    [[nodiscard]] static Result<BearerToken> parse(Secret raw);

    /// The token. Borrows from *this: never bind it to a temporary BearerToken.
    [[nodiscard]] const Secret& value() const noexcept MANGADEX_DL_LIFETIMEBOUND { return value_; }

    /// "Bearer <token>", the whole Authorization value. Precondition: *this came from parse()
    /// (never call it on the default-constructed placeholder).
    [[nodiscard]] Secret authorization() const;

private:
    /// For parse() only: `value` is already validated.
    explicit BearerToken(Secret value) noexcept : value_(std::move(value)) {}

    Secret value_{};
};

/// A personal API client.
struct ClientCredentials {
    std::string client_id{};
    Secret client_secret{};
};

/// The credentials file (`key = value` lines). Each key is optional; an empty value counts as
/// absent.
struct CredentialsFile {
    std::optional<std::string> client_id{};
    std::optional<Secret> client_secret{};
    std::optional<std::string> username{};
    std::optional<Secret> password{};
};

/// A saved login (the session file, format_version 1). The password is never stored.
struct Session {
    std::string issuer{};
    std::string client_id{};
    Secret client_secret{};
    std::string username{};
    std::optional<BearerToken> refresh_token{};
    std::optional<std::int64_t> refresh_expires_at{}; ///< UNIX seconds; nullopt = no known expiry
    std::optional<BearerToken> access_token{};
    std::int64_t access_expires_at = 0; ///< UNIX seconds
};

/// A successful token response.
struct TokenGrant {
    BearerToken access{};
    std::optional<BearerToken> refresh{};
    std::chrono::seconds expires_in{900}; ///< 1..max_access_lifetime, else default_access_lifetime
    std::optional<std::chrono::seconds> refresh_expires_in{}; ///< nullopt for 0 or absent
};

// ---- Raw CLI arguments (owned by the Commands; nothing here is validated yet) -----------------

/// The login options shared by follows and login.
struct RawAuthArgs {
    std::string credentials{};      ///< --credentials FILE, verbatim
    bool credentials_given = false; ///< Option::count() > 0, set by the Command after parse
    bool password_stdin = false;    ///< --password-stdin
    bool no_input = false;          ///< --no-input
};

struct RawLoginArgs {
    RawAuthArgs auth{};
    std::string api_base = std::string{default_api_base};
};

struct RawLogoutArgs {
    std::string api_base = std::string{default_api_base};
};

// ---- Pure functions -------------------------------------------------------------------------

/// application/x-www-form-urlencoded value encoding: RFC 3986 unreserved bytes are kept, every
/// other byte becomes %XX (upper-case hex).
[[nodiscard]] Secret form_encode(std::string_view text);

/// `grant_type=password&username=…&password=…&client_id=…&client_secret=…`, each value
/// form-encoded.
[[nodiscard]] Secret password_grant_body(const ClientCredentials& client, std::string_view username,
                                         const Secret& password);
/// `grant_type=refresh_token&refresh_token=…&client_id=…&client_secret=…`.
[[nodiscard]] Secret refresh_grant_body(const ClientCredentials& client,
                                        const BearerToken& refresh);
/// `token=…&token_type_hint=refresh_token&client_id=…&client_secret=…`.
[[nodiscard]] Secret revoke_body(const ClientCredentials& client, const BearerToken& refresh);

/// The credentials file format: comments, blank lines, CRLF, the first '=' splits, spaces and
/// tabs trimmed, an empty value is absent. Errc::failure "line <n>: …" messages that never
/// contain a value.
[[nodiscard]] Result<CredentialsFile> parse_credentials_file(std::string_view text);

/// read_private_file (max_credentials_file_bytes) then parse_credentials_file. A missing file is
/// an error only for an `explicit_path` (--credentials); a missing default file is an empty
/// CredentialsFile. Errc::failure messages name the path and the problem, never the bytes.
[[nodiscard]] Result<CredentialsFile> load_credentials_file(const std::filesystem::path& path,
                                                            bool explicit_path);

/// The first line of `fd` (at most max_password_stdin_bytes, '\n'/'\r\n' stripped). EOF before
/// any byte, a NUL, an empty first line or too many bytes -> Errc::usage "--password-stdin was
/// given, but stdin held no password". Every intermediate buffer is wiped. If stdin is a
/// terminal, the typed password echoes; that is not refused, because the user chose the flag.
[[nodiscard]] Result<Secret> read_password_stdin(int fd);

/// `credentials_given && credentials.empty()` -> Errc::usage "--credentials needs a file name".
[[nodiscard]] Result<void> check_raw_auth_args(const RawAuthArgs& raw);

/// The official API or a fixture tree; anything else is Errc::usage naming `command` ("<command>
/// logs in only to MangaDex itself; --api-base must be the official API").
[[nodiscard]] Result<void> check_auth_api_base(const ApiBase& base, std::string_view command);

/// Parses an untrusted session document (at most max_session_file_bytes). Every field is
/// validated and `issuer` must equal the expected one. On failure, an Error whose message is a
/// reason phrase that never contains a token or secret. Token strings in the parsed DOM are wiped
/// in place, and the DOM is never copied; `json` is the caller's to wipe.
[[nodiscard]] Result<Session> parse_session(std::string_view json, std::string_view issuer);

/// The session document (format_version 1), written straight into a Secret.
[[nodiscard]] Secret session_to_json(const Session& session);

/// Parses an untrusted 2xx token response (at most max_token_response_bytes): access_token
/// required, refresh_token optional, expires_in / refresh_expires_in as TokenGrant documents.
/// Errc::network with fixed messages. Token strings in the parsed DOM are wiped in place, and the
/// DOM is never copied; the caller wipes `body`.
[[nodiscard]] Result<TokenGrant> parse_token_response(std::string_view body);

// ---- Token errors ---------------------------------------------------------------------------

enum class GrantKind {
    password,
    refresh,
};

/// What a token endpoint's `error` code means (classified on the code only, never the HTTP
/// status: V2).
enum class TokenRefusal {
    bad_credentials,    ///< invalid_grant on a password grant
    bad_client,         ///< invalid_client
    client_not_allowed, ///< unauthorized_client
    session_ended,      ///< invalid_grant on a refresh grant
    other,              ///< any other well-formed code
};

struct TokenErrorInfo {
    TokenRefusal refusal = TokenRefusal::other;
    int status = 0;                    ///< the HTTP status, recorded by the caller (for -v)
    std::optional<std::string> code{}; ///< `error`, only if it matches [a-z_]{1,64}
    /// `error_description`, raw and untrusted, cut to 200 bytes. Shown only under -v and only
    /// through terminal_safe; never in --json.
    std::optional<std::string> description{};

    friend bool operator==(const TokenErrorInfo&, const TokenErrorInfo&) = default;
};

/// Classifies an error response body by its `error` code. nullopt when the body has no parsable
/// `error`. `status` is left 0.
[[nodiscard]] std::optional<TokenErrorInfo> classify_token_error(GrantKind grant,
                                                                 std::string_view body);

/// The user-facing error for a refusal: Errc::auth_rejected with the fixed text for
/// bad_credentials (naming `username`), bad_client, client_not_allowed and other ("MangaDex
/// refused the login (<code>)"). session_ended is the login flow's decision (a password fallback,
/// or Errc::login_required "expired").
[[nodiscard]] Error token_refusal_error(const TokenErrorInfo& info, GrantKind grant,
                                        std::string_view username);

// ---- Endpoints and failures -----------------------------------------------------------------

/// No member has a trailing '/'. Always obtained from auth_endpoints_for, never built from a
/// caller's string.
struct AuthEndpoints {
    std::string issuer{};
    std::string token_url{};
    std::string revoke_url{};

    friend bool operator==(const AuthEndpoints&, const AuthEndpoints&) = default;
};

/// Official API: official_auth_issuer and its two fixed URLs. Fixture: the same under
/// `file://<root>/auth/realms/mangadex`. Any other base: nullopt.
[[nodiscard]] std::optional<AuthEndpoints> auth_endpoints_for(const ApiBase& base);

/// Which step of a login or authorized request failed: it decides the message and whether the
/// network remedy applies.
enum class AuthFailureKind {
    request, ///< transport, HTTP status or interrupt: the remedy applies to exit 3
    content, ///< a malformed response or a cap: a self-contained message
    refused, ///< the server refused the details (info says how)
    local,   ///< a local problem (state directory, lock, missing detail)
};

struct AuthFailure {
    AuthFailureKind kind = AuthFailureKind::local;
    TokenErrorInfo info{}; ///< meaningful for refused
    Error error{};

    friend bool operator==(const AuthFailure&, const AuthFailure&) = default;
};

template <class T> using AuthResult = std::expected<T, AuthFailure>;

// ---- The token client -----------------------------------------------------------------------

/// POSTs token and revoke requests to `endpoints`, which must come from auth_endpoints_for. As a
/// second guard, request() and revoke() refuse with Errc::usage ({local, …} for request()) and do
/// no I/O unless the token or revoke URL starts with official_auth_issuer or `file://`. Has its
/// own rate limiter
/// (auth_request_interval) and RetryConfig (429, 5xx, timeout and protocol errors retried; any
/// other 4xx answered once). Counts logical requests: the (max_token_requests + 1)-th is refused.
/// Never sets an Authorization header; the -vv trace is "POST <url> -> <status> (<ms> ms)", never
/// a body. Every body copy it makes is wiped.
///
/// Borrows `transport`, `clock` and `trace` (nullable): all three must outlive the client.
/// `jitter(max)` returns a value in [0, max] milliseconds. Not thread-safe (one login flow uses
/// it). Neither copyable nor movable; state lives in auth_client.cpp (pimpl).
class AuthClient {
public:
    AuthClient(Transport& transport MANGADEX_DL_LIFETIMEBOUND,
               Clock& clock MANGADEX_DL_LIFETIMEBOUND, AuthEndpoints endpoints,
               std::function<std::uint32_t(std::uint32_t)> jitter,
               TraceSink* trace MANGADEX_DL_LIFETIMEBOUND);
    AuthClient(const AuthClient&) = delete;
    AuthClient& operator=(const AuthClient&) = delete;
    AuthClient(AuthClient&&) = delete;
    AuthClient& operator=(AuthClient&&) = delete;
    ~AuthClient();

    /// One logical grant request with `body` (password_grant_body / refresh_grant_body).
    /// A 4xx with a classifiable body -> {refused, info}; a 4xx without one, or 5xx/429 retries
    /// exhausted, or a transport failure -> {request, "<url>: …"}; a malformed 2xx -> content;
    /// over the cap -> {content, Errc::network "more than 3 MangaDex login requests in one run;
    /// refusing to continue"}; a stop -> {request, Errc::interrupted}, except that a 2xx grant that
    /// arrives with a stop pending is still returned: the caller persists it (a rotated refresh
    /// token must never be lost), then honours the stop.
    [[nodiscard]] AuthResult<TokenGrant> request(GrantKind grant, const Secret& body,
                                                 std::stop_token stop);

    /// Revokes with `body` (revoke_body): success on any HTTP 2xx, whatever the body (V3); a 4xx,
    /// 5xx or transport error is an error. revoke_timeout limits, no retries, not counted.
    [[nodiscard]] Result<void> revoke(const Secret& body, std::stop_token stop);

    /// Logical grant requests made so far.
    [[nodiscard]] int requests() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

// ---- The token-provider seam ----------------------------------------------------------------

/// Hands out Authorization values for the collection fetch. Polymorphic base per house rules:
/// neither copyable nor movable. Called on one thread only.
class TokenProvider {
public:
    TokenProvider(const TokenProvider&) = delete;
    TokenProvider& operator=(const TokenProvider&) = delete;
    TokenProvider(TokenProvider&&) = delete;
    TokenProvider& operator=(TokenProvider&&) = delete;
    virtual ~TokenProvider() = default;

    /// "Bearer <t>", valid for at least refresh_ahead (refreshing first if needed).
    [[nodiscard]] virtual AuthResult<Secret> authorization(std::stop_token stop) = 0;
    /// After the API answered 401: one refresh (or the password fallback), then the new value.
    [[nodiscard]] virtual AuthResult<Secret> refresh_after_unauthorized(std::stop_token stop) = 0;

protected:
    TokenProvider() = default;
};

// ---- The login session and logout -----------------------------------------------------------

/// reuse (follows): a saved access token, else a refresh, else the password grant.
/// password (login): straight to the password grant.
enum class LoginMode {
    reuse,
    password,
};

/// How the session got its access token.
enum class LoginHow {
    saved,
    refreshed,
    password,
};

/// What LoginSession::open needs. Every detail is looked for in the order file > session >
/// prompt (client and username) or stdin > file > prompt (password).
struct LoginRequest {
    ApiBase api_base{};
    UserDirs dirs{};
    LoginMode mode = LoginMode::reuse;
    CredentialsFile credentials{};
    std::filesystem::path credentials_path{}; ///< for messages only
    std::optional<Secret> password_stdin{};
    Prompter* prompter = nullptr; ///< borrowed for the session's life; null = prompts not allowed
    LockTiming lock{};
};

/// What the login flow talks to. Every pointer is borrowed and must outlive the session (or the
/// logout call): transport and clock non-null; trace nullable and thread-safe. `jitter` must be
/// non-empty (see ApiClient); it is copied into the session (and its AuthClient), so anything it
/// borrows must outlive the session too. Fill it member by member, not with designated
/// initializers: the clang-analyzer leak false positive explained in run_title
/// (src/app/cmd_download_run.cpp).
struct AuthServices {
    Transport* transport = nullptr;
    Clock* clock = nullptr;
    std::function<std::uint32_t(std::uint32_t)> jitter{};
    TraceSink* trace = nullptr;
};

/// What LoginSession::open reports besides its result (for the app to print). Never a secret.
struct LoginNotes {
    std::vector<std::string> warnings{}; ///< raw lines; the app sanitizes them
    std::string username{};
    LoginHow how = LoginHow::saved;
    std::optional<std::filesystem::path> session_path{};
    std::optional<std::string> save_error{}; ///< why the session could not be saved, if so
};

/// A logged-in session: holds the session lock and the secrets until close(). The session file
/// is rewritten after every token response, before anything else. Neither copyable nor movable
/// (the base deletes both); state lives in login_session.cpp (pimpl).
class LoginSession final : public TokenProvider {
private:
    struct State;
    /// Passkey: only open() can construct a session.
    struct Key {
        explicit Key() = default;
    };

public:
    /// For open() only (through std::make_unique).
    LoginSession(Key key, std::unique_ptr<State> state) noexcept;
    LoginSession(const LoginSession&) = delete;
    LoginSession& operator=(const LoginSession&) = delete;
    LoginSession(LoginSession&&) = delete;
    LoginSession& operator=(LoginSession&&) = delete;
    ~LoginSession() override; ///< runs close()

    /// Endpoints, state directory, lock, the saved session, then the token per `request.mode`.
    /// auth_endpoints_for(request.api_base) giving nullopt -> {local, Errc::usage} before any
    /// I/O. Problems with the saved login become notes.warnings; a missing detail that can't be
    /// asked for is {local, Errc::login_required}; a refusal is {refused, Errc::auth_rejected}.
    [[nodiscard]] static AuthResult<std::unique_ptr<LoginSession>>
    open(LoginRequest request, const AuthServices& services, std::stop_token stop,
         LoginNotes& notes);

    /// After close(), both return {local, Errc::failure} and touch nothing (no request, no file).
    [[nodiscard]] AuthResult<Secret> authorization(std::stop_token stop) override;
    [[nodiscard]] AuthResult<Secret> refresh_after_unauthorized(std::stop_token stop) override;

    /// Wipes every secret and releases the lock. Idempotent.
    void close() noexcept;

private:
    std::unique_ptr<State> state_{};
};

struct LogoutOutcome {
    bool removed = false;                 ///< false: there was no saved login
    std::filesystem::path path{};         ///< the session file
    std::optional<std::string> warning{}; ///< the revoke failed, or the session was unreadable

    friend bool operator==(const LogoutOutcome&, const LogoutOutcome&) = default;
};

/// Revokes the saved refresh token (best effort) and removes the session file.
/// auth_endpoints_for(api_base) giving nullopt -> Errc::usage before any I/O. A missing state
/// directory or session -> removed false. An unreadable or malformed session is removed without
/// a revoke, with a warning. An insecure state directory -> Errc::failure "could not remove the
/// saved login at <path>: <why>", nothing touched.
[[nodiscard]] Result<LogoutOutcome> logout(const ApiBase& api_base, const UserDirs& dirs,
                                           const AuthServices& services, LockTiming timing,
                                           std::stop_token stop);

} // namespace mangadex_dl
