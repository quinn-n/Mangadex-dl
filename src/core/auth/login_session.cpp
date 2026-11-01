// SPDX-License-Identifier: GPL-3.0-or-later
//
// The login session (auth.hpp; follows plan T12): LoginSession::open gathers the login details,
// takes the session lock, reads the saved login and gets an access token; authorization() and
// refresh_after_unauthorized() keep it fresh for the collection fetch; close() wipes and unlocks.
//
// open():
//   endpoints (nullopt -> usage, no I/O) -> state directory (create, 0700) -> the session lock
//   -> read and parse the slot (fixture slot iff a fixture API base) -> drop it, with a warning,
//   if it names another client or account than the credentials file -> the token:
//     reuse:    a saved access token valid for reuse_min_validity -> 0 requests (how = saved);
//               else a refresh grant, if there is a refresh token not known to be expired;
//               a refresh answered invalid_grant (the session ended) -> the password grant;
//     password: straight to the password grant.
//   The password grant gathers each missing detail in the Q1 order: client_id, client_secret and
//   username from the credentials file > the saved session > a prompt; the password from
//   --password-stdin > the credentials file > a prompt. The client hint is shown before the first
//   client prompt. A detail that can't be asked for is Errc::login_required with the spec's text.
//
// Persisting: after every successful token response the session is written (atomically, by
// StateDir::write) before anything else, and only then is a pending stop or interrupt honoured:
// AuthClient hands back a 2xx grant even with a stop pending, and a rotated refresh token must
// never be lost. StateDir::write runs only while the SessionLock is held (the store's stale-temp
// sweep relies on it); State keeps the directory and the lock together and declares the lock after
// the directory, so the lock is released first and only after the last write. A failed write is a
// warning plus LoginNotes::save_error, and the run goes on; that includes "saved, but cannot sync
// the state directory", where the new file IS in place: it is never retried or rolled back.
// A write failure after open() returns (a refresh during the collection fetch) has no LoginNotes
// to land in: the same warning is kept in the state for take_late_warnings(). When there was no
// usable state directory at all, open() has already said so, and a later save is not reported
// again. The session in memory stays usable either way.
//
// Refusals: AuthClient's refusal Error is a placeholder without the username, so every refusal the
// user sees is rebuilt with token_refusal_error(info, grant, username). A password grant's refusal
// is final (exit 9); a refresh grant's session_ended leads to the password fallback (or exit 8
// "expired"), any other refresh refusal is final. One AuthClient per session: its cap of
// max_token_requests logical grants is per instance, and it counts failed grants too.
//
// Secret hygiene: no secret is ever a format argument or part of an Error. The client secret, the
// tokens and the passwords live in Secrets (wiped on destruction and reassignment); the form bodies
// are built straight into Secrets; close() destroys the whole state, which wipes every one of them.
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "mangadex_dl/text_safety.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// The username in a prompt is cut to this many bytes (on a UTF-8 boundary).
inline constexpr std::size_t max_username_display_bytes = 64;

/// The one line shown before the first client prompt.
[[nodiscard]] std::string client_hint() {
    return std::format("mangadex-dl: reading your follows needs a MangaDex personal API client; "
                       "create one at {} (API Clients)",
                       api_clients_url);
}

/// A detail the password grant needs, for prompts and messages.
enum class Field {
    client_id,
    client_secret,
    username,
    password,
};

[[nodiscard]] std::string_view field_name(Field field) noexcept {
    switch (field) {
    case Field::client_id:
        return "client id";
    case Field::client_secret:
        return "client secret";
    case Field::username:
        return "username";
    case Field::password:
        return "password";
    }
    return "detail"; // out-of-range value
}

/// The longest prefix of `text` of at most `max` bytes that does not split a UTF-8 sequence.
[[nodiscard]] std::string_view utf8_prefix(std::string_view text, std::size_t max) noexcept {
    if (text.size() <= max) {
        return text;
    }
    std::size_t length = max;
    while (length > 0 && detail::is_utf8_continuation(static_cast<unsigned char>(text[length]))) {
        --length;
    }
    return text.substr(0, length);
}

/// What parse_session accepts for client_id, client_secret and username (so a saved session
/// always reads back): well-formed UTF-8 without a C0 control, DEL or a C1 control.
[[nodiscard]] constexpr bool is_clean_detail(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const Utf8Decoded decoded = decode_utf8_at(text, pos);
        const std::uint32_t cp = decoded.code_point;
        if (decoded.length == 0 || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            return false;
        }
        pos += decoded.length;
    }
    return true;
}

[[nodiscard]] AuthFailure local_failure(Errc code, std::string message) {
    return AuthFailure{.kind = AuthFailureKind::local,
                       .error = Error{.code = code, .message = std::move(message)}};
}

[[nodiscard]] AuthFailure interrupted_failure() {
    return AuthFailure{.kind = AuthFailureKind::request,
                       .error = Error{.code = Errc::interrupted, .message = "interrupted"}};
}

[[nodiscard]] AuthFailure closed_failure() {
    return local_failure(Errc::failure, "the MangaDex login session is already closed");
}

/// 8: no client id or secret, and none can be asked for.
[[nodiscard]] AuthFailure no_client_failure(const std::filesystem::path& credentials_path) {
    const std::string where = credentials_path.empty()
                                  ? std::string{"a credentials file (--credentials FILE)"}
                                  : credentials_path.string();
    return local_failure(Errc::login_required,
                         std::format("no MangaDex API client is set up; run 'mangadex-dl login' "
                                     "in a terminal, or put client_id and client_secret in {}",
                                     where));
}

/// 8: the username or password is missing and can't be asked for. `ended`: there was a saved
/// login, and it expired or was ended.
[[nodiscard]] AuthFailure missing_login_failure(bool ended) {
    if (ended) {
        const TokenErrorInfo info{.refusal = TokenRefusal::session_ended};
        return AuthFailure{.kind = AuthFailureKind::local,
                           .error = token_refusal_error(info, GrantKind::refresh, {})};
    }
    return local_failure(Errc::login_required,
                         "you are not logged in to MangaDex; run 'mangadex-dl login' in a "
                         "terminal (or pass --password-stdin)");
}

/// A refusal as the user sees it: AuthClient's placeholder rebuilt with the username.
[[nodiscard]] AuthFailure refusal(AuthFailure failure, GrantKind grant, std::string_view username) {
    failure.error = token_refusal_error(failure.info, grant, username);
    return failure;
}

/// SIGINT/SIGTERM, or the caller's stop.
[[nodiscard]] bool stop_pending(const std::stop_token& stop) noexcept {
    return stop.stop_requested() || interrupt_requested();
}

/// The wall clock in UNIX seconds (the session file's unit).
[[nodiscard]] std::int64_t unix_now(const Clock& clock) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(clock.wall_now().time_since_epoch())
        .count();
}

/// `ignoring the saved login at <path>: <why>`.
[[nodiscard]] std::string ignore_warning(const std::filesystem::path& session_path,
                                         std::string_view why) {
    return std::format("ignoring the saved login at {}: {}", session_path.string(), why);
}

/// Where a FileProblem with the state directory means a saved login may exist but can't be used
/// (rather than "there is nowhere to save one").
[[nodiscard]] bool hides_saved_login(FileProblemKind kind) noexcept {
    switch (kind) {
    case FileProblemKind::symlink:
    case FileProblemKind::not_owned:
    case FileProblemKind::too_open:
        return true;
    case FileProblemKind::missing:
    case FileProblemKind::not_regular:
    case FileProblemKind::too_large:
    case FileProblemKind::io:
        return false;
    }
    return false;
}

/// The login session's state (LoginSession::State, private in the header, derives from it so the
/// helpers below can name it).
struct SessionState {
    SessionState(Transport& transport, Clock& clock_ref, AuthEndpoints auth_endpoints,
                 std::function<std::uint32_t(std::uint32_t)> jitter, TraceSink* trace)
        : clock(&clock_ref), issuer(auth_endpoints.issuer),
          client(transport, clock_ref, std::move(auth_endpoints), std::move(jitter), trace) {}

    Clock* clock = nullptr; ///< borrowed; never null
    std::string issuer{};   ///< the expected issuer of a saved session
    // AuthClient has no default constructor (it borrows the transport and clock): initialized in
    // the constructor's member-initializer list. One per session: the request cap is per client.
    AuthClient client;
    SessionSlot slot = SessionSlot::fixture;
    std::optional<std::filesystem::path> session_path{}; ///< nullopt: no state directory path
    std::string no_store_reason{}; ///< why there is no usable state directory (if so)
    /// The state directory, engaged only while `lock` is held (both are set together in open()).
    std::optional<StateDir> dir{};
    /// Declared after `dir`: destroyed first, so the lock is released before the directory closes
    /// and only after the last write (no write happens during destruction).
    SessionLock lock{};
    Session session{};                      ///< the login in memory: details, tokens, expiry times
    std::optional<Secret> stdin_password{}; ///< --password-stdin, kept for the password fallback
    std::optional<Secret> file_password{};  ///< the credentials file's, kept likewise
    Prompter* prompter = nullptr;           ///< borrowed; null: prompts are not allowed
    std::filesystem::path credentials_path{}; ///< for messages only
    LoginHow how = LoginHow::saved;
    std::vector<std::string> late_warnings{}; ///< raised after open(); see take_late_warnings()
};

/// The spec's save warning.
[[nodiscard]] std::string save_warning(const SessionState& s, std::string_view why) {
    if (s.session_path.has_value()) {
        return std::format("could not save the MangaDex login to {}: {}; the next run will need "
                           "the password again",
                           s.session_path->string(), why);
    }
    return std::format(
        "could not save the MangaDex login: {}; the next run will need the password again", why);
}

/// Writes the session in memory. Precondition for StateDir::write: the SessionLock is held (the
/// stale-temp sweep relies on it), which `dir` being engaged guarantees. A failure is recorded in
/// `notes` as save_error plus the spec's warning, or, after open() (`notes` null), as a late
/// warning; it is never retried or rolled back, also when the new file is already in place
/// ("saved, but cannot sync the state directory").
void persist(SessionState& s, LoginNotes* notes) {
    std::string why{};
    const bool have_store = s.dir.has_value() && s.lock.held();
    if (have_store) {
        const Secret document = session_to_json(s.session);
        // The lock is held (checked above): StateDir::write's precondition.
        if (Result<void> written = s.dir->write(s.slot, document); !written) {
            why = std::move(written.error().message);
        } else {
            return;
        }
    } else {
        why = s.no_store_reason.empty() ? std::string{"the state directory is not usable"}
                                        : s.no_store_reason;
    }
    if (notes == nullptr) {
        // After open(): kept for take_late_warnings(). Without a store, open() already warned.
        if (have_store) {
            s.late_warnings.push_back(save_warning(s, why));
        }
        return;
    }
    notes->warnings.push_back(save_warning(s, why));
    notes->save_error = std::move(why);
}

/// Stores a successful grant in the session: the access token and its expiry; a new refresh
/// token replaces the old one (rotation). A password grant without one leaves no refresh token; a
/// refresh grant without one keeps the old.
void apply_grant(SessionState& s, TokenGrant grant, GrantKind kind) {
    const std::int64_t now = unix_now(*s.clock);
    s.session.access_token = std::move(grant.access);
    // expires_in <= max_access_lifetime and refresh_expires_in <= 100 years (parse_token_response),
    // and a wall clock in seconds is far from the int64 limit: the sums cannot overflow.
    s.session.access_expires_at = now + grant.expires_in.count();
    if (grant.refresh.has_value()) {
        s.session.refresh_token = std::move(grant.refresh);
        s.session.refresh_expires_at =
            grant.refresh_expires_in.has_value()
                ? std::optional<std::int64_t>{now + grant.refresh_expires_in->count()}
                : std::nullopt;
    } else if (kind == GrantKind::password) {
        s.session.refresh_token.reset();
        s.session.refresh_expires_at.reset();
    }
}

/// The client details of the session, copied for a form body (the copy is wiped when it dies).
[[nodiscard]] ClientCredentials client_of(const Session& session) {
    return ClientCredentials{
        .client_id = session.client_id,
        .client_secret = Secret::copy_of(session.client_secret.reveal()),
    };
}

/// Asks for `field`. An answer for client_id, client_secret or username must be what
/// parse_session accepts back (non-empty, at most max_prompt_answer_bytes, well-formed UTF-8, no
/// control character); the password only needs to be non-empty and within the limit.
/// `unavailable` (no usable terminal) becomes `cannot_ask`, the caller's "can't ask" failure.
[[nodiscard]] AuthResult<Secret> ask(Prompter& prompter, Field field, std::string_view question,
                                     Echo echo, const AuthFailure& cannot_ask) {
    PromptReply reply = prompter.ask(question, echo);
    const std::string_view name = field_name(field);
    const auto too_long = [name] {
        return local_failure(
            Errc::login_required,
            std::format("the {} you typed is longer than {} bytes", name, max_prompt_answer_bytes));
    };
    const auto none_given = [name] {
        return local_failure(Errc::login_required, std::format("no {} given", name));
    };
    switch (reply.status) {
    case PromptStatus::answered: {
        const std::string_view text = reply.answer.reveal();
        if (text.empty()) {
            return std::unexpected(none_given());
        }
        if (text.size() > max_prompt_answer_bytes) {
            return std::unexpected(too_long());
        }
        if (field != Field::password && !is_clean_detail(text)) {
            return std::unexpected(local_failure(
                Errc::login_required,
                std::format("the {} you typed contains a control character or is not valid "
                            "UTF-8",
                            name)));
        }
        return std::move(reply.answer);
    }
    case PromptStatus::empty:
        return std::unexpected(none_given());
    case PromptStatus::too_long:
        return std::unexpected(too_long());
    case PromptStatus::interrupted:
        return std::unexpected(interrupted_failure());
    case PromptStatus::unavailable:
        return std::unexpected(cannot_ask);
    }
    return std::unexpected(cannot_ask); // out-of-range status
}

/// Fills in the client and the username for a password grant, prompting for what is missing (the
/// hint first when a client detail is asked for). `ended`: a saved login expired or was ended.
[[nodiscard]] AuthResult<void> gather_details(SessionState& s, bool ended) {
    Session& session = s.session;
    const bool need_client = session.client_id.empty() || session.client_secret.empty();
    if (need_client) {
        const AuthFailure no_client = no_client_failure(s.credentials_path);
        if (s.prompter == nullptr) {
            return std::unexpected(no_client);
        }
        s.prompter->notice(client_hint());
        if (session.client_id.empty()) {
            AuthResult<Secret> id =
                ask(*s.prompter, Field::client_id, "MangaDex API client id: ", Echo::on, no_client);
            if (!id) {
                return std::unexpected(std::move(id).error());
            }
            session.client_id = std::string{id->reveal()}; // not a secret
        }
        if (session.client_secret.empty()) {
            AuthResult<Secret> secret = ask(*s.prompter, Field::client_secret,
                                            "MangaDex API client secret: ", Echo::off, no_client);
            if (!secret) {
                return std::unexpected(std::move(secret).error());
            }
            session.client_secret = std::move(*secret);
        }
    }
    if (session.username.empty()) {
        const AuthFailure missing = missing_login_failure(ended);
        if (s.prompter == nullptr) {
            return std::unexpected(missing);
        }
        AuthResult<Secret> user =
            ask(*s.prompter, Field::username, "MangaDex username: ", Echo::on, missing);
        if (!user) {
            return std::unexpected(std::move(user).error());
        }
        session.username = std::string{user->reveal()}; // not a secret
    }
    return {};
}

/// The password for a grant: --password-stdin > the credentials file > a prompt naming the user.
[[nodiscard]] AuthResult<Secret> take_password(SessionState& s, bool ended) {
    if (s.stdin_password.has_value()) {
        return Secret::copy_of(s.stdin_password->reveal());
    }
    if (s.file_password.has_value()) {
        return Secret::copy_of(s.file_password->reveal());
    }
    const AuthFailure missing = missing_login_failure(ended);
    if (s.prompter == nullptr) {
        return std::unexpected(missing);
    }
    const std::string question = std::format(
        "MangaDex password for {}: ", utf8_prefix(s.session.username, max_username_display_bytes));
    return ask(*s.prompter, Field::password, question, Echo::off, missing);
}

/// The password grant: gather, POST, persist, then honour a pending stop.
[[nodiscard]] AuthResult<void> password_login(SessionState& s, std::stop_token stop,
                                              LoginNotes* notes, bool ended) {
    if (AuthResult<void> details = gather_details(s, ended); !details) {
        return details;
    }
    AuthResult<Secret> password = take_password(s, ended);
    if (!password) {
        return std::unexpected(std::move(password).error());
    }
    Secret body = password_grant_body(client_of(s.session), s.session.username, *password);
    password->clear();
    AuthResult<TokenGrant> grant = s.client.request(GrantKind::password, body, std::move(stop));
    body.clear();
    if (!grant) {
        if (grant.error().kind == AuthFailureKind::refused) {
            return std::unexpected(
                refusal(std::move(grant).error(), GrantKind::password, s.session.username));
        }
        return std::unexpected(std::move(grant).error());
    }
    apply_grant(s, std::move(*grant), GrantKind::password);
    s.how = LoginHow::password;
    persist(s, notes);
    return {};
}

/// The refresh grant. true: the session has a fresh access token; false: the server ended the
/// session (invalid_grant), whose tokens are now dropped from memory (the file is left as it is).
[[nodiscard]] AuthResult<bool> refresh_login(SessionState& s, const std::stop_token& stop,
                                             LoginNotes* notes) {
    // The caller checked that there is a refresh token.
    if (!s.session.refresh_token.has_value()) {
        return false;
    }
    Secret body = refresh_grant_body(client_of(s.session), *s.session.refresh_token);
    AuthResult<TokenGrant> grant = s.client.request(GrantKind::refresh, body, stop);
    body.clear();
    if (!grant) {
        if (grant.error().kind == AuthFailureKind::refused) {
            if (grant.error().info.refusal == TokenRefusal::session_ended) {
                s.session.refresh_token.reset();
                s.session.refresh_expires_at.reset();
                s.session.access_token.reset();
                s.session.access_expires_at = 0;
                return false;
            }
            return std::unexpected(
                refusal(std::move(grant).error(), GrantKind::refresh, s.session.username));
        }
        return std::unexpected(std::move(grant).error());
    }
    apply_grant(s, std::move(*grant), GrantKind::refresh);
    s.how = LoginHow::refreshed;
    persist(s, notes);
    return true;
}

/// A refresh token that may still work: present, and not past a known expiry.
[[nodiscard]] bool can_refresh(const SessionState& s) noexcept {
    if (!s.session.refresh_token.has_value()) {
        return false;
    }
    return !s.session.refresh_expires_at.has_value() ||
           *s.session.refresh_expires_at > unix_now(*s.clock);
}

/// A new access token: a refresh if possible, else (or when the session ended) the password
/// grant. Every grant is persisted before a pending stop is honoured.
[[nodiscard]] AuthResult<void> renew(SessionState& s, const std::stop_token& stop,
                                     LoginNotes* notes) {
    // A saved login existed (tokens in memory): a missing password then means "expired".
    const bool ended = s.session.refresh_token.has_value() || s.session.access_token.has_value();
    if (can_refresh(s)) {
        AuthResult<bool> refreshed = refresh_login(s, stop, notes);
        if (!refreshed) {
            return std::unexpected(std::move(refreshed).error());
        }
        if (*refreshed) {
            if (stop_pending(stop)) {
                return std::unexpected(interrupted_failure());
            }
            return {};
        }
    }
    if (AuthResult<void> login = password_login(s, stop, notes, ended); !login) {
        return login;
    }
    if (stop_pending(stop)) {
        return std::unexpected(interrupted_failure());
    }
    return {};
}

/// The access token's Authorization value; renews first when `force`, or when the token expires
/// within refresh_ahead.
[[nodiscard]] AuthResult<Secret> current_authorization(SessionState& s, const std::stop_token& stop,
                                                       bool force) {
    const bool fresh = s.session.access_token.has_value() &&
                       s.session.access_expires_at > unix_now(*s.clock) + refresh_ahead.count();
    if (force || !fresh) {
        if (AuthResult<void> renewed = renew(s, stop, nullptr); !renewed) {
            return std::unexpected(std::move(renewed).error());
        }
    }
    if (!s.session.access_token.has_value()) {
        // renew() always leaves an access token behind on success.
        return std::unexpected(
            local_failure(Errc::failure, "the MangaDex login has no access token"));
    }
    return s.session.access_token->authorization();
}

/// The state directory, the lock and the saved session (nullopt: none, or ignored with a
/// warning). Only the lock is fatal: every other problem becomes a warning or a reason to report
/// when the session can't be saved.
[[nodiscard]] AuthResult<std::optional<Session>> open_store(SessionState& s, const UserDirs& dirs,
                                                            LockTiming timing,
                                                            const std::stop_token& stop,
                                                            LoginNotes& notes) {
    const std::optional<std::filesystem::path> dir_path = state_dir_path(dirs);
    if (!dir_path.has_value()) {
        s.no_store_reason = "there is no home directory (HOME is not set to an absolute path)";
        return std::optional<Session>{};
    }
    s.session_path = *dir_path / session_file_name(s.slot);
    notes.session_path = s.session_path;
    const std::filesystem::path& session_path = *s.session_path;

    FileResult<StateDir> dir = StateDir::open(*dir_path, true);
    if (!dir) {
        s.no_store_reason = std::format("the directory {} is not usable: {}", dir_path->string(),
                                        dir.error().reason);
        if (hides_saved_login(dir.error().kind)) {
            notes.warnings.push_back(ignore_warning(session_path, s.no_store_reason));
        }
        return std::optional<Session>{};
    }
    Result<SessionLock> lock = dir->lock(*s.clock, timing, stop);
    if (!lock) {
        if (lock.error().code == Errc::interrupted) {
            return std::unexpected(interrupted_failure());
        }
        return std::unexpected(
            AuthFailure{.kind = AuthFailureKind::local, .error = std::move(lock).error()});
    }
    s.dir.emplace(std::move(*dir));
    s.lock = std::move(*lock);

    // The lock is held from here on (StateDir::write/remove's precondition).
    const FileResult<std::optional<Secret>> bytes = s.dir->read(s.slot);
    if (!bytes) {
        notes.warnings.push_back(ignore_warning(session_path, bytes.error().reason));
        return std::optional<Session>{};
    }
    if (!bytes->has_value()) {
        return std::optional<Session>{};
    }
    Result<Session> saved = parse_session((*bytes)->reveal(), s.issuer);
    if (!saved) {
        notes.warnings.push_back(ignore_warning(session_path, saved.error().message));
        return std::optional<Session>{};
    }
    return std::optional<Session>{std::move(*saved)};
}

/// The details in the Q1 order (the credentials file > the saved session) and the saved tokens.
/// A saved session for another client or account than the credentials file is dropped with a
/// warning first.
void seed_session(SessionState& s, CredentialsFile& file, std::optional<Session> saved,
                  LoginNotes& notes) {
    if (saved.has_value() && s.session_path.has_value()) {
        const std::string credentials = s.credentials_path.empty()
                                            ? std::string{"the credentials file"}
                                            : s.credentials_path.string();
        if (file.client_id.has_value() && *file.client_id != saved->client_id) {
            notes.warnings.push_back(ignore_warning(
                *s.session_path,
                std::format("it is for another API client than the one in {}", credentials)));
            saved.reset();
        } else if (file.username.has_value() && *file.username != saved->username) {
            notes.warnings.push_back(ignore_warning(
                *s.session_path,
                std::format("it is for another account than the one in {}", credentials)));
            saved.reset();
        }
    }

    Session& session = s.session;
    session.issuer = s.issuer;
    if (file.client_id.has_value()) {
        session.client_id = std::move(*file.client_id);
    } else if (saved.has_value()) {
        session.client_id = std::move(saved->client_id);
    }
    if (file.client_secret.has_value()) {
        session.client_secret = std::move(*file.client_secret);
    } else if (saved.has_value()) {
        session.client_secret = std::move(saved->client_secret);
    }
    if (file.username.has_value()) {
        session.username = std::move(*file.username);
    } else if (saved.has_value()) {
        session.username = std::move(saved->username);
    }
    if (saved.has_value()) {
        session.refresh_token = std::move(saved->refresh_token);
        session.refresh_expires_at = saved->refresh_expires_at;
        session.access_token = std::move(saved->access_token);
        session.access_expires_at = saved->access_expires_at;
    }
}

} // namespace

struct LoginSession::State : SessionState {
    using SessionState::SessionState;
};

LoginSession::LoginSession(Key /*key*/, std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

LoginSession::~LoginSession() { close(); }

AuthResult<std::unique_ptr<LoginSession>>
LoginSession::open(LoginRequest request, const AuthServices& services,
                   // NOLINTNEXTLINE(performance-unnecessary-value-param): frozen by-value (T1)
                   std::stop_token stop, LoginNotes& notes) {
    std::optional<AuthEndpoints> endpoints = auth_endpoints_for(request.api_base);
    if (!endpoints.has_value()) {
        return std::unexpected(local_failure(
            Errc::usage, "a MangaDex login works only with the official API; --api-base must be "
                         "the official API"));
    }
    if (services.transport == nullptr || services.clock == nullptr) {
        return std::unexpected(
            local_failure(Errc::failure, "the MangaDex login has no transport or clock"));
    }

    auto state = std::make_unique<State>(*services.transport, *services.clock,
                                         std::move(*endpoints), services.jitter, services.trace);
    SessionState& s = *state;
    s.slot = request.api_base.kind() == ApiBase::Kind::fixture ? SessionSlot::fixture
                                                               : SessionSlot::real;
    s.prompter = request.prompter;
    s.credentials_path = std::move(request.credentials_path);
    s.stdin_password = std::move(request.password_stdin);
    s.file_password = std::move(request.credentials.password);

    AuthResult<std::optional<Session>> saved =
        open_store(s, request.dirs, request.lock, stop, notes);
    if (!saved) {
        return std::unexpected(std::move(saved).error());
    }
    seed_session(s, request.credentials, std::move(*saved), notes);

    const std::int64_t now = unix_now(*s.clock);
    if (request.mode == LoginMode::reuse && s.session.access_token.has_value() &&
        s.session.access_expires_at >= now + reuse_min_validity.count()) {
        s.how = LoginHow::saved;
    } else if (request.mode == LoginMode::reuse) {
        if (AuthResult<void> renewed = renew(s, stop, &notes); !renewed) {
            return std::unexpected(std::move(renewed).error());
        }
    } else {
        // LoginMode::password (or an out-of-range value): always a fresh password grant.
        AuthResult<void> login = password_login(s, stop, &notes, false);
        if (!login) {
            return std::unexpected(std::move(login).error());
        }
        if (stop_pending(stop)) {
            return std::unexpected(interrupted_failure());
        }
    }
    notes.username = s.session.username;
    notes.how = s.how;
    return std::make_unique<LoginSession>(Key{}, std::move(state));
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): the by-value signature is frozen (T1 header)
AuthResult<Secret> LoginSession::authorization(std::stop_token stop) {
    if (state_ == nullptr) {
        return std::unexpected(closed_failure());
    }
    return current_authorization(*state_, stop, false);
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): the by-value signature is frozen (T1 header)
AuthResult<Secret> LoginSession::refresh_after_unauthorized(std::stop_token stop) {
    if (state_ == nullptr) {
        return std::unexpected(closed_failure());
    }
    return current_authorization(*state_, stop, true);
}

std::vector<std::string> LoginSession::take_late_warnings() {
    if (state_ == nullptr) {
        return {};
    }
    return std::exchange(state_->late_warnings, {});
}

void LoginSession::close() noexcept {
    // Destroys the state in reverse declaration order: the secrets are wiped (Secret's destructor),
    // then the lock is released, then the state directory closes. No write happens here.
    state_.reset();
}

} // namespace mangadex_dl
