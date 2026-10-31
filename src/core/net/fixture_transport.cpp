// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fixture transport (net/fixture_transport.hpp): serves file://<root>/... from a directory
// tree, opening every component with O_NOFOLLOW relative to its parent, so no URL reaches
// outside <root> (no `..`, no symlinks, no absolute re-rooting).
//
// Auth routing (follows plan T6): a stand-in for MangaDex's Keycloak (the token and revoke POSTs,
// answered from <root>/auth/{password,refresh,revoke}/<name>.json) and for its authorized GETs
// (<root>/auth/access/<token>.json). Every auth file name comes from a form field or the Bearer
// token, so it is checked against a strict name rule before any openat and never appears in a
// message. What is wiped (fixture values only; best effort, like the real transport): the path
// strings built from those names, the counter keys, the raw text of every file read and the read
// buffer. The parsed auth-file DOM is not: it holds only the fixture's own fake values.
#include "mangadex_dl/net/fixture_transport.hpp"

#include "auth/form.hpp"
#include "json_util.hpp"
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): explicit_bzero is a glibc extension of <string.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

using json_util::Json;

inline constexpr std::string_view file_scheme = "file://";
inline constexpr std::size_t read_chunk = std::size_t{64} * 1024;

// The two Keycloak endpoints, relative to the root (auth_endpoints_for builds the same URLs).
inline constexpr std::string_view token_path{"auth/realms/mangadex/protocol/openid-connect/token"};
inline constexpr std::string_view revoke_path{
    "auth/realms/mangadex/protocol/openid-connect/revoke"};
// The only URLs that accept an Authorization (relative to the root, without the query).
inline constexpr std::string_view follows_path = "api/user/follows/manga";
inline constexpr std::string_view status_path = "api/manga/status";
inline constexpr std::string_view user_path = "api/user";
inline constexpr std::string_view auth_dir = "auth";
inline constexpr std::string_view auth_dir_slash = "auth/";

inline constexpr std::string_view form_content_type = "application/x-www-form-urlencoded";
inline constexpr std::string_view bearer_prefix = "Bearer ";
inline constexpr std::size_t max_auth_form_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t max_auth_file_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t max_auth_name_bytes = 128;

// Keycloak's answers (V2, V3) and MangaDex's 401 for an API request.
inline constexpr std::string_view invalid_user_body =
    R"({"error":"invalid_grant","error_description":"Invalid user credentials"})";
inline constexpr std::string_view inactive_token_body =
    R"({"error":"invalid_grant","error_description":"Token is not active"})";
inline constexpr std::string_view invalid_client_body =
    R"({"error":"invalid_client","error_description":"Invalid client or Invalid client credentials"})";
inline constexpr std::string_view unknown_revoke_body = R"({"error":"invalid_token"})";
inline constexpr std::string_view unsupported_grant_body =
    R"({"error":"unsupported_grant_type","error_description":"Unsupported grant_type"})";
inline constexpr std::string_view api_unauthorized_body =
    R"({"result":"error","errors":[{"status":401,"title":"unauthorized_http_exception"}]})";

[[nodiscard]] std::string errno_text(int err) { return std::generic_category().message(err); }

[[nodiscard]] TransportError local_error(std::string message) {
    return TransportError{.kind = TransportErrc::local, .message = std::move(message)};
}

[[nodiscard]] TransportError bad_url(std::string_view url, std::string_view why) {
    return local_error(std::format("fixture: {}: {}", url, why));
}

/// The fixed refusal of a name that breaks the auth name rule (the name is never echoed).
[[nodiscard]] TransportError unsafe_name() { return local_error("fixture: unsafe auth name"); }

/// openat(2) for flags without O_CREAT/O_TMPFILE: the variadic mode argument is never read.
/// `name` is a std::string, so it is NUL-terminated.
[[nodiscard]] int open_at(int dir, const std::string& name, int flags) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic only for the mode
    return ::openat(dir, name.c_str(), flags);
}

[[nodiscard]] HttpResponse status_only(int status) { return HttpResponse{.status = status}; }

[[nodiscard]] HttpResponse json_response(int status, std::string_view body) {
    return HttpResponse{.status = status, .body = std::string{body}};
}

/// ENOENT and ENOTDIR are "not in the tree". ELOOP (or ENOTDIR, for an intermediate component
/// opened with O_DIRECTORY) is a symlink component: refused the same way.
[[nodiscard]] bool is_not_found(int err) noexcept {
    return err == ENOENT || err == ENOTDIR || err == ELOOP;
}

/// The value of the first `offset` parameter of `query`: nullopt when there is none, an empty
/// string when it is not a plain decimal number (rejected by the caller).
[[nodiscard]] std::optional<std::string_view> offset_param(std::string_view query) noexcept {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view param = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        constexpr std::string_view key = "offset=";
        if (param.starts_with(key)) {
            const std::string_view value = param.substr(key.size());
            const bool digits = !value.empty() && value.size() <= 18 &&
                                value.find_first_not_of("0123456789") == std::string_view::npos;
            return digits ? value : std::string_view{};
        }
    }
    return std::nullopt;
}

/// Splits `path` on '/' into owned components. False when a component is empty, `.` or `..`.
[[nodiscard]] bool split_components(std::string_view path, std::vector<std::string>& out) {
    while (true) {
        const auto slash = path.find('/');
        const std::string_view comp = path.substr(0, slash);
        if (comp.empty() || comp == "." || comp == "..") {
            return false;
        }
        out.emplace_back(comp);
        if (slash == std::string_view::npos) {
            return true;
        }
        path.remove_prefix(slash + 1);
    }
}

/// `[A-Za-z0-9._-]{1,128}`, and neither `.` nor `..`: safe as one file name below `auth/<dir>/`.
[[nodiscard]] bool safe_auth_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > max_auth_name_bytes || name == "." || name == "..") {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

/// CR, LF or NUL would split or cut a real header line (HttpRequest::authorization).
[[nodiscard]] bool breaks_header(std::string_view value) noexcept {
    return value.find_first_of(std::string_view{"\r\n\0", 3}) != std::string_view::npos;
}

/// `application/x-www-form-urlencoded`, optionally followed by parameters (`; charset=UTF-8`).
[[nodiscard]] bool is_form_content_type(std::string_view type) noexcept {
    return type.starts_with(form_content_type) &&
           (type.size() == form_content_type.size() || type[form_content_type.size()] == ';');
}

/// The tripwire: an Authorization value reached a URL that must never see one.
[[nodiscard]] TransportError authorization_leak(std::string_view url) {
    if (url.contains('\0')) {
        return local_error("fixture: Authorization sent to a URL containing a NUL byte");
    }
    return local_error(std::format("fixture: Authorization sent to {}", url));
}

/// The read buffer of one fixture file, wiped on every return (an auth file holds fixture
/// secrets).
struct WipedBuffer {
    WipedBuffer() = default;
    WipedBuffer(const WipedBuffer&) = delete;
    WipedBuffer& operator=(const WipedBuffer&) = delete;
    WipedBuffer(WipedBuffer&&) = delete;
    WipedBuffer& operator=(WipedBuffer&&) = delete;
    ~WipedBuffer() { ::explicit_bzero(bytes.data(), bytes.size()); }

    std::array<char, read_chunk> bytes{};
};

/// A URL split below the root. Both views borrow the URL string of the request being served and
/// are used only within that fetch().
struct UrlParts {
    std::string_view path{};  ///< relative to the root, no leading '/', no query
    std::string_view query{}; ///< after the first '?'; empty when there is none
};

/// The components of `auth/<dir>/<name>.json`. `name` is a user name or a fixture token, so every
/// string is wiped when this dies. The file name is built in place in its final buffer (reserved
/// first), so no moved-from or reallocated copy of it is left behind.
class AuthFilePath {
public:
    AuthFilePath(std::string_view dir, std::string_view name) {
        constexpr std::string_view suffix = ".json";
        components_.reserve(3);
        components_.emplace_back("auth");
        components_.emplace_back(dir);
        std::string& file = components_.emplace_back();
        file.reserve(name.size() + suffix.size());
        file.append(name).append(suffix);
    }
    AuthFilePath(const AuthFilePath&) = delete;
    AuthFilePath& operator=(const AuthFilePath&) = delete;
    AuthFilePath(AuthFilePath&&) = delete;
    AuthFilePath& operator=(AuthFilePath&&) = delete;
    ~AuthFilePath() {
        for (std::string& component : components_) {
            secure_wipe(component);
        }
    }

    [[nodiscard]] const std::vector<std::string>&
    components() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return components_;
    }

private:
    std::vector<std::string> components_{};
};

/// A malformed auth file: the message names the request URL and the directory, never a value.
[[nodiscard]] TransportError malformed_auth_file(std::string_view url, std::string_view dir) {
    return local_error(std::format("fixture: {}: malformed auth/{} file", url, dir));
}

/// A status member: absent -> `fallback`; else an integer within [low, high].
[[nodiscard]] std::optional<int> status_member(const Json::object_t& file, std::string_view key,
                                               int fallback, int low, int high) {
    const auto it = file.find(key);
    if (it == file.end()) {
        return fallback;
    }
    const auto* const value = it->second.get_ptr<const Json::number_unsigned_t*>();
    if (value == nullptr || std::cmp_less(*value, low) || std::cmp_greater(*value, high)) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

/// Whether `expect[key]` (absent: anything goes) equals the form field `key`; nullopt when the
/// expected value is not a string.
[[nodiscard]] std::optional<bool> expect_matches(const Json::object_t* expect, std::string_view key,
                                                 const std::vector<detail::FormField>& form) {
    if (expect == nullptr) {
        return true;
    }
    const auto it = expect->find(key);
    if (it == expect->end()) {
        return true;
    }
    const auto* const wanted = it->second.get_ptr<const std::string*>();
    if (wanted == nullptr) {
        return std::nullopt;
    }
    const detail::FormField* const field = detail::find_form_field(form, key);
    return field != nullptr && field->value.reveal() == *wanted;
}

/// The file's `expect` object (nullptr when absent); `ok` false when it is not an object.
[[nodiscard]] const Json::object_t* expect_member(const Json::object_t& file, bool& ok) {
    const auto it = file.find("expect");
    if (it == file.end()) {
        return nullptr;
    }
    const auto* const expect = it->second.get_ptr<const Json::object_t*>();
    ok = expect != nullptr;
    return expect;
}

class FixtureTransport final : public Transport {
public:
    FixtureTransport(UniqueFd root_fd, std::string root_prefix) noexcept
        : root_fd_(std::move(root_fd)), root_prefix_(std::move(root_prefix)) {}
    FixtureTransport(const FixtureTransport&) = delete;
    FixtureTransport& operator=(const FixtureTransport&) = delete;
    FixtureTransport(FixtureTransport&&) = delete;
    FixtureTransport& operator=(FixtureTransport&&) = delete;
    /// The counter keys are fixture tokens: wiped through node handles (the only non-const
    /// access to a map key) before they are freed.
    ~FixtureTransport() override {
        while (!counters_.empty()) {
            const auto node = counters_.extract(counters_.begin());
            secure_wipe(node.key());
        }
    }

    [[nodiscard]] TransportResult fetch(const HttpRequest& request) override {
        if (request.cancel.stop_requested()) {
            return std::unexpected(
                TransportError{.kind = TransportErrc::cancelled, .message = "cancelled"});
        }
        if (!request.authorization.empty()) {
            if (breaks_header(request.authorization)) {
                return std::unexpected(
                    local_error("fixture: the Authorization value contains CR, LF or NUL"));
            }
            if (request.method != HttpMethod::get || !accepts_authorization(request.url)) {
                return std::unexpected(authorization_leak(request.url));
            }
        }
        if (request.method == HttpMethod::post) {
            return post(request);
        }
        const auto parts = split_url(request.url);
        if (!parts.has_value()) {
            return std::unexpected(parts.error());
        }
        if (parts->path == auth_dir || parts->path.starts_with(auth_dir_slash)) {
            return status_only(404); // the auth files are the stand-in's own, never served
        }
        if (needs_token(*parts)) {
            auto refused = check_token(request);
            if (!refused.has_value()) {
                return std::unexpected(std::move(refused.error()));
            }
            if (refused->has_value()) {
                return bounded(std::move(**refused), request);
            }
        }
        auto components = map_url(*parts, request.url);
        if (!components.has_value()) {
            return std::unexpected(std::move(components.error()));
        }
        return read_file(*components, request.url, request.max_body_bytes);
    }

private:
    /// file://<root>/<rel>[?query] -> <rel> and <query>, borrowing `url`.
    [[nodiscard]] std::expected<UrlParts, TransportError>
    split_url(std::string_view url MANGADEX_DL_LIFETIMEBOUND) const {
        if (url.contains('\0')) {
            // Not echoed: a NUL byte must never reach a diagnostic line.
            return std::unexpected(local_error("fixture: URL contains a NUL byte"));
        }
        if (url.contains('#')) {
            return std::unexpected(bad_url(url, "fragments are not supported"));
        }
        if (!url.starts_with(file_scheme)) {
            return std::unexpected(bad_url(url, "not a file:// URL"));
        }
        std::string_view rest = url.substr(file_scheme.size());
        const auto question = rest.find('?');
        const std::string_view query =
            question == std::string_view::npos ? std::string_view{} : rest.substr(question + 1);
        rest = rest.substr(0, question);
        if (!rest.starts_with(root_prefix_) || rest.size() <= root_prefix_.size() ||
            rest[root_prefix_.size()] != '/') {
            return std::unexpected(bad_url(url, "outside the fixture root"));
        }
        rest.remove_prefix(root_prefix_.size() + 1);
        return UrlParts{.path = rest, .query = query};
    }

    /// <rel>[?query] -> the components of the file to read under the root.
    [[nodiscard]] static std::expected<std::vector<std::string>, TransportError>
    map_url(const UrlParts& parts, std::string_view url) {
        std::vector<std::string> components;
        if (!split_components(parts.path, components)) {
            return std::unexpected(bad_url(url, "empty, '.' or '..' path segment"));
        }
        if (components.size() >= 2 && components.front() == "api") {
            const auto offset = offset_param(parts.query);
            if (offset.has_value()) {
                if (offset->empty()) {
                    return std::unexpected(bad_url(url, "offset is not a decimal number"));
                }
                components.push_back(std::format("offset-{}.json", *offset));
            } else {
                components.back() += ".json";
            }
        }
        return components;
    }

    /// The two allowlisted GETs: the follows pages (with `offset=`) and the status map.
    [[nodiscard]] bool accepts_authorization(std::string_view url) const {
        const auto parts = split_url(url);
        if (!parts.has_value()) {
            return false;
        }
        return parts->path == status_path ||
               (parts->path == follows_path && offset_param(parts->query).has_value());
    }

    /// The allowlisted GETs and every other `api/user/…` URL need a token.
    [[nodiscard]] static bool needs_token(const UrlParts& parts) noexcept {
        return parts.path == follows_path || parts.path == status_path || parts.path == user_path ||
               (parts.path.starts_with(user_path) && parts.path[user_path.size()] == '/');
    }

    /// nullopt: the request's token is good (and was counted), so the URL is served. A response:
    /// the API's 401 (no token, not a Bearer token, no access file, or no requests left).
    [[nodiscard]] std::expected<std::optional<HttpResponse>, TransportError>
    check_token(const HttpRequest& request) {
        const std::string_view authorization = request.authorization;
        if (!authorization.starts_with(bearer_prefix)) {
            return json_response(401, api_unauthorized_body); // also covers "no token"
        }
        const std::string_view token = authorization.substr(bearer_prefix.size());
        if (!safe_auth_name(token)) {
            return std::unexpected(unsafe_name());
        }
        auto file = read_auth_file("access", token, request.url);
        if (!file.has_value()) {
            return std::unexpected(std::move(file.error()));
        }
        if (!file->has_value()) {
            return json_response(401, api_unauthorized_body);
        }
        const auto* const object = (*file)->get_ptr<const Json::object_t*>();
        if (object == nullptr) { // read_auth_file checked it; kept for the optimizer's sake
            return std::unexpected(malformed_auth_file(request.url, "access"));
        }
        std::optional<std::size_t> limit{};
        if (const auto it = object->find("valid_requests"); it != object->end()) {
            const auto* const count = it->second.get_ptr<const Json::number_unsigned_t*>();
            if (count == nullptr || !std::in_range<std::size_t>(*count)) {
                return std::unexpected(malformed_auth_file(request.url, "access"));
            }
            limit = static_cast<std::size_t>(*count);
        }
        if (limit.has_value() && !take_request(token, *limit)) {
            return json_response(401, api_unauthorized_body);
        }
        return std::optional<HttpResponse>{};
    }

    /// Counts one request against `token`'s limit (set from `limit` on first use). False when no
    /// request is left.
    [[nodiscard]] bool take_request(std::string_view token, std::size_t limit) {
        const std::scoped_lock lock{counters_mutex_};
        auto it = counters_.find(token);
        if (it == counters_.end()) {
            // Built in place from the view: no temporary (and no moved-from SSO copy) of the token.
            it = counters_
                     .emplace(std::piecewise_construct, std::forward_as_tuple(token),
                              std::forward_as_tuple(limit))
                     .first;
        }
        if (it->second == 0) {
            return false;
        }
        --it->second;
        return true;
    }

    /// The token and revoke endpoints; every other POST is 404 (fixture mode sends no MD@H
    /// reports).
    [[nodiscard]] TransportResult post(const HttpRequest& request) const {
        const auto parts = split_url(request.url);
        const bool token = parts.has_value() && parts->path == token_path;
        const bool revoke = parts.has_value() && parts->path == revoke_path;
        if (!token && !revoke) {
            return status_only(404);
        }
        if (!is_form_content_type(request.content_type)) {
            return std::unexpected(bad_url(request.url, "an auth POST needs Content-Type "
                                                        "application/x-www-form-urlencoded"));
        }
        if (request.body.size() > max_auth_form_bytes) {
            return std::unexpected(bad_url(request.url, "the form body exceeds 65536 bytes"));
        }
        const auto form = detail::form_decode(request.body);
        if (!form.has_value()) {
            return std::unexpected(bad_url(request.url, form.error().message));
        }
        auto response = token ? token_grant(*form, request.url) : revoke_token(*form, request.url);
        if (!response.has_value()) {
            return response;
        }
        return bounded(std::move(*response), request);
    }

    /// The token endpoint: the password and refresh_token grants.
    [[nodiscard]] TransportResult token_grant(const std::vector<detail::FormField>& form,
                                              std::string_view url) const {
        const detail::FormField* const grant = detail::find_form_field(form, "grant_type");
        if (grant == nullptr) {
            return json_response(
                400, R"({"error":"invalid_request","error_description":"Missing grant_type"})");
        }
        if (grant->value.reveal() == "password") {
            return password_grant(form, url);
        }
        if (grant->value.reveal() == "refresh_token") {
            return refresh_grant(form, url);
        }
        return json_response(400, unsupported_grant_body);
    }

    [[nodiscard]] TransportResult password_grant(const std::vector<detail::FormField>& form,
                                                 std::string_view url) const {
        const detail::FormField* const user = detail::find_form_field(form, "username");
        if (user == nullptr) {
            return json_response(
                400, R"({"error":"invalid_request","error_description":"Missing username"})");
        }
        if (!safe_auth_name(user->value.reveal())) {
            return std::unexpected(unsafe_name());
        }
        const auto file = read_auth_file("password", user->value.reveal(), url);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        if (!file->has_value()) {
            return json_response(401, invalid_user_body);
        }
        const auto* const file_object = (*file)->get_ptr<const Json::object_t*>();
        if (file_object == nullptr) { // read_auth_file checked it; kept for the optimizer's sake
            return std::unexpected(malformed_auth_file(url, "password"));
        }
        const Json::object_t& object = *file_object;
        const auto mismatch_status = status_member(object, "mismatch_status", 401, 400, 499);
        bool expect_ok = true;
        const Json::object_t* const expect = expect_member(object, expect_ok);
        if (!mismatch_status.has_value() || !expect_ok) {
            return std::unexpected(malformed_auth_file(url, "password"));
        }
        const auto client = client_matches(expect, form);
        const auto password = expect_matches(expect, "password", form);
        if (!client.has_value() || !password.has_value()) {
            return std::unexpected(malformed_auth_file(url, "password"));
        }
        if (!*client) {
            return json_response(401, invalid_client_body);
        }
        if (!*password) {
            return json_response(*mismatch_status, invalid_user_body);
        }
        return scripted(object, url, "password");
    }

    [[nodiscard]] TransportResult refresh_grant(const std::vector<detail::FormField>& form,
                                                std::string_view url) const {
        const detail::FormField* const refresh = detail::find_form_field(form, "refresh_token");
        if (refresh == nullptr) {
            return json_response(
                400, R"({"error":"invalid_request","error_description":"Missing refresh_token"})");
        }
        return answer_from(form, url, "refresh", refresh->value.reveal(), 400, inactive_token_body);
    }

    /// The revoke endpoint: a missing file is Keycloak's 200 `invalid_token` (V3).
    [[nodiscard]] TransportResult revoke_token(const std::vector<detail::FormField>& form,
                                               std::string_view url) const {
        const detail::FormField* const token = detail::find_form_field(form, "token");
        if (token == nullptr) {
            return json_response(
                400, R"({"error":"invalid_request","error_description":"Missing token"})");
        }
        return answer_from(form, url, "revoke", token->value.reveal(), 200, unknown_revoke_body);
    }

    /// Answers from `auth/<dir>/<name>.json`: `missing_status` + `missing_body` without one, a
    /// client mismatch is 401 invalid_client, otherwise the file's script.
    [[nodiscard]] TransportResult answer_from(const std::vector<detail::FormField>& form,
                                              std::string_view url, std::string_view dir,
                                              std::string_view name, int missing_status,
                                              std::string_view missing_body) const {
        if (!safe_auth_name(name)) {
            return std::unexpected(unsafe_name());
        }
        const auto file = read_auth_file(dir, name, url);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        if (!file->has_value()) {
            return json_response(missing_status, missing_body);
        }
        const auto* const file_object = (*file)->get_ptr<const Json::object_t*>();
        if (file_object == nullptr) { // read_auth_file checked it; kept for the optimizer's sake
            return std::unexpected(malformed_auth_file(url, dir));
        }
        const Json::object_t& object = *file_object;
        bool expect_ok = true;
        const Json::object_t* const expect = expect_member(object, expect_ok);
        const auto client = expect_ok ? client_matches(expect, form) : std::nullopt;
        if (!client.has_value()) {
            return std::unexpected(malformed_auth_file(url, dir));
        }
        if (!*client) {
            return json_response(401, invalid_client_body);
        }
        return scripted(object, url, dir);
    }

    [[nodiscard]] static std::optional<bool>
    client_matches(const Json::object_t* expect, const std::vector<detail::FormField>& form) {
        const auto id = expect_matches(expect, "client_id", form);
        const auto secret = expect_matches(expect, "client_secret", form);
        if (!id.has_value() || !secret.has_value()) {
            return std::nullopt;
        }
        return *id && *secret;
    }

    /// The file's `status` (default 200) and `body`: a string is sent verbatim, any other value
    /// as its JSON text, an absent body as nothing.
    [[nodiscard]] static TransportResult scripted(const Json::object_t& file, std::string_view url,
                                                  std::string_view dir) {
        const auto status = status_member(file, "status", 200, 100, 599);
        if (!status.has_value()) {
            return std::unexpected(malformed_auth_file(url, dir));
        }
        HttpResponse response{.status = *status};
        if (const auto it = file.find("body"); it != file.end()) {
            if (const auto* const text = it->second.get_ptr<const std::string*>()) {
                response.body = *text;
            } else {
                response.body = it->second.dump();
            }
        }
        return response;
    }

    /// Reads `auth/<dir>/<name>.json` as a JSON object; nullopt when it is not in the tree. The
    /// file's text is wiped once parsed. `url` (the request's) is the only name in messages.
    [[nodiscard]] std::expected<std::optional<Json>, TransportError>
    read_auth_file(std::string_view dir, std::string_view name, std::string_view url) const {
        const AuthFilePath path{dir, name};
        auto read = read_file(path.components(), url, max_auth_file_bytes);
        if (!read.has_value()) {
            if (read.error().kind == TransportErrc::too_large) {
                return std::unexpected(local_error(
                    std::format("fixture: {}: an auth/{} file exceeds 65536 bytes", url, dir)));
            }
            return std::unexpected(std::move(read.error()));
        }
        if (read->status == 404) {
            return std::optional<Json>{};
        }
        auto parsed = json_util::parse_untrusted(
            read->body, "fixture auth file",
            json_util::ParseLimits{.max_bytes = max_auth_file_bytes, .code = Errc::failure});
        secure_wipe(read->body);
        if (!parsed.has_value() || !parsed->is_object()) {
            return std::unexpected(malformed_auth_file(url, dir));
        }
        return std::optional<Json>{std::move(*parsed)};
    }

    /// A synthesized response obeys max_body_bytes like a real one; the refused body (it may hold
    /// fixture tokens) is wiped.
    [[nodiscard]] static TransportResult bounded(HttpResponse response,
                                                 const HttpRequest& request) {
        if (response.body.size() > request.max_body_bytes) {
            secure_wipe(response.body);
            return std::unexpected(TransportError{
                .kind = TransportErrc::too_large,
                .message = std::format("fixture: {}: body exceeds {} bytes", request.url,
                                       request.max_body_bytes),
            });
        }
        return response;
    }

    /// Walks `components` from the root fd. Each intermediate directory is opened O_PATH with
    /// O_DIRECTORY|O_NOFOLLOW and closed (RAII) once its child is open. `url` names the request
    /// in messages; at most `max_bytes` are read.
    [[nodiscard]] TransportResult read_file(const std::vector<std::string>& components,
                                            std::string_view url, std::size_t max_bytes) const {
        UniqueFd current{};
        int dir = root_fd_.get();
        for (std::size_t i = 0; i + 1 < components.size(); ++i) {
            const int fd =
                open_at(dir, components[i], O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                return open_failure(errno, url);
            }
            current.reset(fd); // closes the previous intermediate directory
            dir = current.get();
        }
        // O_NONBLOCK: opening a FIFO planted in the tree must not block the caller.
        const UniqueFd file{
            open_at(dir, components.back(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
        if (!file.valid()) {
            return open_failure(errno, url);
        }
        current.reset();

        struct stat st{};
        if (::fstat(file.get(), &st) != 0) {
            const int err = errno;
            return std::unexpected(
                local_error(std::format("fixture: {}: fstat: {}", url, errno_text(err))));
        }
        if (!S_ISREG(st.st_mode)) {
            return status_only(404); // a directory, FIFO or device is not a fixture document
        }
        return read_body(file.get(), url, max_bytes);
    }

    [[nodiscard]] static TransportResult open_failure(int err, std::string_view url) {
        if (is_not_found(err)) {
            return status_only(404);
        }
        return std::unexpected(local_error(std::format("fixture: {}: {}", url, errno_text(err))));
    }

    /// Reads the whole file, but never more than `max_bytes` (0 refuses any non-empty body).
    [[nodiscard]] static TransportResult read_body(int fd, std::string_view url,
                                                   std::size_t max_bytes) {
        HttpResponse response{.status = 200};
        WipedBuffer buffer{};
        while (true) {
            const ::ssize_t got = ::read(fd, buffer.bytes.data(), buffer.bytes.size());
            if (got < 0) {
                const int err = errno;
                if (err == EINTR) {
                    continue;
                }
                return std::unexpected(
                    local_error(std::format("fixture: {}: read: {}", url, errno_text(err))));
            }
            if (got == 0) {
                return response;
            }
            const auto count = static_cast<std::size_t>(got);
            if (count > max_bytes - response.body.size()) {
                secure_wipe(response.body);
                return std::unexpected(TransportError{
                    .kind = TransportErrc::too_large,
                    .message = std::format("fixture: {}: body exceeds {} bytes", url, max_bytes),
                });
            }
            response.body.append(buffer.bytes.data(), count);
        }
    }

    UniqueFd root_fd_{};        ///< O_PATH directory; only read after construction
    std::string root_prefix_{}; ///< the root as it appears in URLs, without a trailing '/'
    /// Requests left per access token (`valid_requests`), keyed by the token; guarded by
    /// counters_mutex_, because fetch() runs concurrently on several threads.
    std::mutex counters_mutex_{};
    std::map<std::string, std::size_t, std::less<>> counters_{};
};

} // namespace

// NOLINTNEXTLINE(performance-unnecessary-value-param): the by-value signature is frozen (T1 header)
Result<std::unique_ptr<Transport>> make_fixture_transport(std::filesystem::path root) {
    std::string text = root.lexically_normal().string();
    if (!root.is_absolute() || text.contains('\0')) {
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = std::format("fixture root '{}' is not an absolute path", root.string()),
        });
    }
    // The URL form has no trailing '/': "/a/b/" becomes "/a/b", and "/" becomes "".
    while (!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    const std::string open_name = text.empty() ? std::string{"/"} : text;
    UniqueFd fd{open_at(AT_FDCWD, open_name, O_PATH | O_DIRECTORY | O_CLOEXEC)};
    if (!fd.valid()) {
        const int err = errno;
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = std::format("cannot open fixture root '{}': {}", open_name, errno_text(err)),
        });
    }
    return std::make_unique<FixtureTransport>(std::move(fd), std::move(text));
}

} // namespace mangadex_dl
