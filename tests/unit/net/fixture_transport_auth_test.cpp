// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/fixture_transport_auth_test.cpp: the fixture transport's Keycloak stand-in and
// its authorized GETs (follows plan T6, AC10), over trees written by
// tests/support/mdx_auth_fixture.hpp. Exact statuses (V2, V3): 401 for a wrong password (400 in
// the newer-Keycloak variant), 400 for an inactive refresh token, 401 invalid_client, a scripted
// 400 unauthorized_client, 200 for an unknown revoke and a scripted 503 revoke. Unsafe names are
// local errors that never echo the name; `valid_requests` counts down per token (also from
// several threads, for TSan); the leak tripwire names the URL, never the token; MD@H POSTs stay
// 404.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace mdx = mangadex_dl::test::mdx;
using mangadex_dl::form_encode;
using mangadex_dl::HttpMethod;
using mangadex_dl::HttpRequest;
using mangadex_dl::make_fixture_transport;
using mangadex_dl::Secret;
using mangadex_dl::Transport;
using mangadex_dl::TransportErrc;
using mangadex_dl::TransportResult;
using mangadex_dl::test::TempDir;

constexpr std::string_view form_type = "application/x-www-form-urlencoded";
constexpr std::string_view manga_id = "11111111-1111-4111-8111-111111111111";
constexpr std::size_t generous = std::size_t{1} << 20U;
constexpr std::string_view follows_page = "api/user/follows/manga?limit=100&offset=0";

void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out.good());
}

/// A temp dir with the auth files of `accounts` and three API documents under `<dir>/root`.
struct AuthTree {
    std::optional<TempDir> dir{}; ///< always engaged; optional only for the member initializer
    fs::path root{};
    std::unique_ptr<Transport> transport{};

    [[nodiscard]] std::string url(std::string_view rel) const {
        return std::format("file://{}/{}", root.string(), rel);
    }
};

[[nodiscard]] AuthTree make_auth_tree(const std::vector<mdx::FixtureAuth>& accounts) {
    auto dir = TempDir::create("mangadex-dl-fixture-auth");
    REQUIRE(dir.has_value());
    const fs::path root = fs::absolute(dir->path()) / "root";
    for (const mdx::FixtureAuth& account : accounts) {
        const auto written = mdx::write_auth_files(root, account);
        INFO((written.has_value() ? std::string{} : written.error()));
        REQUIRE(written.has_value());
    }
    write(root / "api" / "user" / "follows" / "manga" / "offset-0.json", "follows-page-0");
    write(root / "api" / "manga" / "status.json", "status-map");
    write(root / "api" / "manga" / (std::string{manga_id} + ".json"), "manga");
    auto transport = make_fixture_transport(root);
    REQUIRE(transport.has_value());
    return AuthTree{.dir = std::move(*dir), .root = root, .transport = std::move(*transport)};
}

[[nodiscard]] AuthTree make_auth_tree() { return make_auth_tree({mdx::FixtureAuth{}}); }

/// `name=<form_encode(value)>&...`
[[nodiscard]] std::string
form(std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    std::string body{};
    for (const auto& [name, value] : fields) {
        if (!body.empty()) {
            body.push_back('&');
        }
        const Secret encoded = form_encode(value);
        body.append(name).append("=").append(encoded.reveal());
    }
    return body;
}

[[nodiscard]] std::string
password_form(std::string_view username, std::string_view password,
              std::string_view client_id = mdx::fixture_client_id,
              std::string_view client_secret = mdx::fixture_client_secret) {
    return form({{"grant_type", "password"},
                 {"username", username},
                 {"password", password},
                 {"client_id", client_id},
                 {"client_secret", client_secret}});
}

[[nodiscard]] std::string
refresh_form(std::string_view token, std::string_view client_secret = mdx::fixture_client_secret) {
    return form({{"grant_type", "refresh_token"},
                 {"refresh_token", token},
                 {"client_id", mdx::fixture_client_id},
                 {"client_secret", client_secret}});
}

[[nodiscard]] std::string revoke_form(std::string_view token,
                                      std::string_view client_secret = mdx::fixture_client_secret) {
    return form({{"token", token},
                 {"token_type_hint", "refresh_token"},
                 {"client_id", mdx::fixture_client_id},
                 {"client_secret", client_secret}});
}

[[nodiscard]] TransportResult post(const AuthTree& tree, std::string url, std::string body,
                                   std::string_view content_type = form_type,
                                   std::size_t max_body = generous) {
    return tree.transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = std::move(url),
        .body = std::move(body),
        .content_type = std::string{content_type},
        .max_body_bytes = max_body,
    });
}

[[nodiscard]] TransportResult token_post(const AuthTree& tree, std::string body) {
    return post(tree, mdx::token_url(tree.root), std::move(body));
}

[[nodiscard]] TransportResult revoke_post(const AuthTree& tree, std::string body) {
    return post(tree, mdx::revoke_url(tree.root), std::move(body));
}

[[nodiscard]] TransportResult get(const AuthTree& tree, std::string_view rel,
                                  std::string authorization = {}) {
    return tree.transport->fetch(HttpRequest{
        .url = tree.url(rel),
        .authorization = std::move(authorization),
        .max_body_bytes = generous,
    });
}

[[nodiscard]] int status_of(const TransportResult& result) {
    return result.has_value() ? result->status : -1;
}

[[nodiscard]] std::string body_of(const TransportResult& result) {
    return result.has_value() ? result->body : std::string{"<error>"};
}

[[nodiscard]] std::optional<TransportErrc> error_of(const TransportResult& result) {
    return result.has_value() ? std::nullopt : std::optional{result.error().kind};
}

[[nodiscard]] std::string message_of(const TransportResult& result) {
    return result.has_value() ? std::string{"<response>"} : result.error().message;
}

/// The string member `key` of the JSON response body, "<none>" if there is none.
[[nodiscard]] std::string member(const TransportResult& result, std::string_view key) {
    if (!result.has_value()) {
        return "<error>";
    }
    const nlohmann::json doc = nlohmann::json::parse(result->body, nullptr, false);
    if (!doc.is_object() || !doc.contains(key) || !doc.at(key).is_string()) {
        return "<none>";
    }
    return doc.at(key).get<std::string>();
}

/// The request that reads `auth/<rel>`, where rel is `<dir>/fixture-broken.json`.
[[nodiscard]] TransportResult request_broken(const AuthTree& tree, std::string_view rel) {
    if (rel.starts_with("password/")) {
        return token_post(tree, password_form("fixture-broken", mdx::fixture_password));
    }
    if (rel.starts_with("refresh/")) {
        return token_post(tree, refresh_form("fixture-broken"));
    }
    if (rel.starts_with("revoke/")) {
        return revoke_post(tree, revoke_form("fixture-broken"));
    }
    return get(tree, follows_page, mdx::bearer("fixture-broken"));
}

/// A transport error that is `local` and contains neither of `secrets`.
void check_local_without(const TransportResult& result,
                         std::initializer_list<std::string_view> secrets) {
    REQUIRE(error_of(result) == TransportErrc::local);
    for (const std::string_view secret : secrets) {
        CHECK_FALSE(result.error().message.contains(secret));
    }
}

} // namespace

TEST_CASE("fixture auth: the password grant answers the first grant of the chain",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const TransportResult result =
        token_post(tree, password_form(mdx::fixture_username, mdx::fixture_password));
    REQUIRE(status_of(result) == 200);
    CHECK(member(result, "access_token") == mdx::fixture_access_1);
    CHECK(member(result, "refresh_token") == mdx::fixture_refresh_1);
    CHECK(member(result, "token_type") == "Bearer");

    // A charset parameter on the Content-Type is accepted.
    const TransportResult with_charset =
        post(tree, mdx::token_url(tree.root),
             password_form(mdx::fixture_username, mdx::fixture_password),
             "application/x-www-form-urlencoded; charset=UTF-8");
    CHECK(status_of(with_charset) == 200);
}

TEST_CASE("fixture auth: a wrong password or an unknown user is 401 invalid_grant",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const TransportResult wrong = token_post(tree, password_form(mdx::fixture_username, "nope"));
    CHECK(status_of(wrong) == 401);
    CHECK(member(wrong, "error") == "invalid_grant");
    CHECK(member(wrong, "error_description") == "Invalid user credentials");

    const TransportResult unknown =
        token_post(tree, password_form("fixture-nobody", mdx::fixture_password));
    CHECK(status_of(unknown) == 401);
    CHECK(member(unknown, "error") == "invalid_grant");

    const TransportResult no_password =
        token_post(tree, form({{"grant_type", "password"},
                               {"username", mdx::fixture_username},
                               {"client_id", mdx::fixture_client_id},
                               {"client_secret", mdx::fixture_client_secret}}));
    CHECK(status_of(no_password) == 401);
    CHECK(member(no_password, "error") == "invalid_grant");
}

TEST_CASE("fixture auth: mismatch_status 400 is the newer-Keycloak variant",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree({mdx::FixtureAuth{
        .username = "fixture-user-400",
        .mismatch_status = 400,
    }});
    const TransportResult wrong = token_post(tree, password_form("fixture-user-400", "nope"));
    CHECK(status_of(wrong) == 400);
    CHECK(member(wrong, "error") == "invalid_grant");

    const TransportResult right =
        token_post(tree, password_form("fixture-user-400", mdx::fixture_password));
    CHECK(status_of(right) == 200);
    // The client is checked first, and its answer does not follow mismatch_status.
    const TransportResult bad_client =
        token_post(tree, password_form("fixture-user-400", "nope", mdx::fixture_client_id, "nope"));
    CHECK(status_of(bad_client) == 401);
    CHECK(member(bad_client, "error") == "invalid_client");
}

TEST_CASE("fixture auth: a wrong client is 401 invalid_client", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const auto [label, body] = GENERATE(
        std::pair{"wrong id", password_form(mdx::fixture_username, mdx::fixture_password,
                                            "fixture-other-client")},
        std::pair{"wrong secret", password_form(mdx::fixture_username, mdx::fixture_password,
                                                mdx::fixture_client_id, "nope")},
        std::pair{"no secret", form({{"grant_type", "password"},
                                     {"username", mdx::fixture_username},
                                     {"password", mdx::fixture_password},
                                     {"client_id", mdx::fixture_client_id}})});
    CAPTURE(label);
    const TransportResult result = token_post(tree, body);
    CHECK(status_of(result) == 401);
    CHECK(member(result, "error") == "invalid_client");
}

TEST_CASE("fixture auth: refresh follows the chain, then the session ends with 400",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const TransportResult first = token_post(tree, refresh_form(mdx::fixture_refresh_1));
    REQUIRE(status_of(first) == 200);
    CHECK(member(first, "access_token") == mdx::fixture_access_2);
    CHECK(member(first, "refresh_token") == mdx::fixture_refresh_2);

    // The last refresh token of the chain, and an unknown one: inactive.
    for (const std::string_view token : {mdx::fixture_refresh_2, std::string_view{"fixture-x"}}) {
        CAPTURE(token);
        const TransportResult ended = token_post(tree, refresh_form(token));
        CHECK(status_of(ended) == 400);
        CHECK(member(ended, "error") == "invalid_grant");
        CHECK(member(ended, "error_description") == "Token is not active");
    }

    const TransportResult bad_client =
        token_post(tree, refresh_form(mdx::fixture_refresh_1, "nope"));
    CHECK(status_of(bad_client) == 401);
    CHECK(member(bad_client, "error") == "invalid_client");
}

TEST_CASE("fixture auth: scripted unauthorized_client refusals answer 400", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree({mdx::FixtureAuth{
        .username = "fixture-user-unapproved",
        .password_refusal = mdx::scripted_error(400, "unauthorized_client",
                                                "Client not allowed for direct access grants"),
        .refresh_scripts = {mdx::FixtureRefreshScript{
            .token = std::string{mdx::fixture_refresh_1},
            .answer = mdx::scripted_error(400, "unauthorized_client"),
        }},
    }});
    const TransportResult password =
        token_post(tree, password_form("fixture-user-unapproved", mdx::fixture_password));
    CHECK(status_of(password) == 400);
    CHECK(member(password, "error") == "unauthorized_client");
    // A wrong password is still refused as one: the script answers only the right password.
    CHECK(status_of(token_post(tree, password_form("fixture-user-unapproved", "nope"))) == 401);

    const TransportResult refresh = token_post(tree, refresh_form(mdx::fixture_refresh_1));
    CHECK(status_of(refresh) == 400);
    CHECK(member(refresh, "error") == "unauthorized_client");
}

TEST_CASE("fixture auth: revoke answers 200, also for an unknown token, unless scripted",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree({mdx::FixtureAuth{
        .revokes = {mdx::FixtureRevoke{.token = std::string{mdx::fixture_refresh_1}},
                    mdx::FixtureRevoke{
                        .token = std::string{"fixture-refresh-broken"},
                        .status = 503,
                        .body = "Service Unavailable",
                    }},
    }});
    const TransportResult known = revoke_post(tree, revoke_form(mdx::fixture_refresh_1));
    CHECK(status_of(known) == 200);
    CHECK(body_of(known).empty());

    const TransportResult unknown = revoke_post(tree, revoke_form("fixture-refresh-unknown"));
    CHECK(status_of(unknown) == 200);
    CHECK(body_of(unknown) == R"({"error":"invalid_token"})");

    const TransportResult scripted = revoke_post(tree, revoke_form("fixture-refresh-broken"));
    CHECK(status_of(scripted) == 503);
    CHECK(body_of(scripted) == "Service Unavailable");

    const TransportResult bad_client =
        revoke_post(tree, revoke_form(mdx::fixture_refresh_1, "nope"));
    CHECK(status_of(bad_client) == 401);
    CHECK(member(bad_client, "error") == "invalid_client");
}

TEST_CASE("fixture auth: missing fields and other grants are 400", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const auto [body, error] =
        GENERATE(std::pair{std::string{"username=fixture-user"}, "invalid_request"},
                 std::pair{std::string{"grant_type=password&client_id=x"}, "invalid_request"},
                 std::pair{std::string{"grant_type=refresh_token"}, "invalid_request"},
                 std::pair{std::string{"grant_type=client_credentials"}, "unsupported_grant_type"},
                 std::pair{std::string{}, "invalid_request"});
    CAPTURE(body);
    const TransportResult result = token_post(tree, body);
    CHECK(status_of(result) == 400);
    CHECK(member(result, "error") == error);

    const TransportResult no_token = revoke_post(tree, "token_type_hint=refresh_token");
    CHECK(status_of(no_token) == 400);
    CHECK(member(no_token, "error") == "invalid_request");
}

TEST_CASE("fixture auth: unsafe names are local errors that never echo the name",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const std::string too_long(129, 'a');
    const std::string name =
        GENERATE_COPY(std::string{"../x"}, std::string{"a/b"}, std::string{"."}, std::string{".."},
                      std::string{}, std::string{"a b"}, std::string{"fixture~x"}, too_long);
    CAPTURE(name);
    const auto check_unsafe = [&name](const TransportResult& result) {
        REQUIRE(error_of(result) == TransportErrc::local);
        CHECK(result.error().message == "fixture: unsafe auth name");
        if (name.size() > 2) {
            CHECK_FALSE(result.error().message.contains(name));
        }
    };
    check_unsafe(token_post(tree, password_form(name, mdx::fixture_password)));
    check_unsafe(token_post(tree, refresh_form(name)));
    check_unsafe(revoke_post(tree, revoke_form(name)));
    check_unsafe(get(tree, follows_page, mdx::bearer(name)));

    // 128 bytes is still a name (of an unknown user).
    const std::string longest(128, 'a');
    CHECK(status_of(token_post(tree, password_form(longest, mdx::fixture_password))) == 401);
}

TEST_CASE("fixture auth: an auth POST needs the form Content-Type, a small body and a valid form",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const std::string good = password_form(mdx::fixture_username, mdx::fixture_password);

    for (const std::string_view type : {std::string_view{"application/json"}, std::string_view{},
                                        std::string_view{"application/x-www-form-urlencodedX"}}) {
        CAPTURE(type);
        check_local_without(post(tree, mdx::token_url(tree.root), good, type),
                            {mdx::fixture_password});
    }

    std::string big = good;
    big.append("&padding=");
    big.append((std::size_t{64} * 1024) - big.size() + 1, 'x');
    REQUIRE(big.size() == (std::size_t{64} * 1024) + 1);
    check_local_without(token_post(tree, big), {mdx::fixture_password});

    const std::string malformed =
        std::string{"password="}.append(mdx::fixture_password).append("%zz&grant_type=password");
    const TransportResult bad_form = token_post(tree, malformed);
    check_local_without(bad_form, {mdx::fixture_password});
    CHECK(message_of(bad_form).contains("form body"));
}

TEST_CASE("fixture auth: valid_requests counts down per token", "[net][fixture-auth]") {
    mdx::FixtureAuth auth{};
    auth.chain[0].valid_requests = 2;
    const AuthTree tree = make_auth_tree({auth});
    const std::string first = mdx::bearer(mdx::fixture_access_1);
    const std::string second = mdx::bearer(mdx::fixture_access_2);

    CHECK(status_of(get(tree, follows_page, first)) == 200);
    CHECK(status_of(get(tree, "api/manga/status", first)) == 200); // the same counter
    const TransportResult spent = get(tree, follows_page, first);
    CHECK(status_of(spent) == 401);
    CHECK(body_of(spent) ==
          R"({"result":"error","errors":[{"status":401,"title":"unauthorized_http_exception"}]})");
    CHECK(status_of(get(tree, follows_page, first)) == 401);

    // Another token keeps its own count (here: unlimited).
    for (int i = 0; i < 4; ++i) {
        CHECK(status_of(get(tree, follows_page, second)) == 200);
    }
    CHECK(body_of(get(tree, follows_page, second)) == "follows-page-0");
    CHECK(body_of(get(tree, "api/manga/status", second)) == "status-map");
}

TEST_CASE("fixture auth: no token, an unknown token or another scheme is 401",
          "[net][fixture-auth]") {
    mdx::FixtureAuth auth{};
    auth.chain[1].access_accepted = false;
    const AuthTree tree = make_auth_tree({auth});
    for (const std::string_view rel : {follows_page, std::string_view{"api/manga/status"}}) {
        CAPTURE(rel);
        CHECK(status_of(get(tree, rel)) == 401);
        CHECK(status_of(get(tree, rel, mdx::bearer("fixture-access-unknown"))) == 401);
        CHECK(status_of(get(tree, rel, mdx::bearer(mdx::fixture_access_2))) == 401);
        CHECK(status_of(get(tree, rel, "Basic Zml4dHVyZQ")) == 401);
        CHECK(status_of(get(tree, rel, "bearer fixture-access-1")) == 401); // case matters
        CHECK(status_of(get(tree, rel, mdx::bearer(mdx::fixture_access_1))) == 200);
    }
    // Every other api/user/... URL needs a token, too.
    for (const std::string_view rel :
         {std::string_view{"api/user/follows/manga"}, std::string_view{"api/user/me"},
          std::string_view{"api/user"}, std::string_view{"api/user/list?offset=0"}}) {
        CAPTURE(rel);
        const TransportResult result = get(tree, rel);
        CHECK(status_of(result) == 401);
        CHECK(body_of(result).contains("unauthorized_http_exception"));
    }
    // A public URL without a token is served as before.
    CHECK(body_of(get(tree, std::format("api/manga/{}", manga_id))) == "manga");
}

TEST_CASE("fixture auth: the tripwire names the URL, never the token", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const std::string token = mdx::bearer(mdx::fixture_access_1);

    const std::string manga_rel = std::format("api/manga/{}", manga_id);
    for (const std::string_view rel :
         {std::string_view{manga_rel}, std::string_view{"api/user/follows/manga"},
          std::string_view{"api/user/me"}, std::string_view{"api/manga/status/x"},
          std::string_view{"uploads/covers/x.png"}}) {
        CAPTURE(rel);
        const TransportResult result = get(tree, rel, token);
        check_local_without(result, {mdx::fixture_access_1});
        CHECK(message_of(result) ==
              std::format("fixture: Authorization sent to {}", tree.url(rel)));
    }
    // Outside the root, another scheme, and every POST (the token client never sends one).
    check_local_without(tree.transport->fetch(HttpRequest{
                            .url = "https://api.mangadex.org/manga",
                            .authorization = token,
                            .max_body_bytes = generous,
                        }),
                        {mdx::fixture_access_1});
    const TransportResult on_post = tree.transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = mdx::token_url(tree.root),
        .body = password_form(mdx::fixture_username, mdx::fixture_password),
        .content_type = std::string{form_type},
        .authorization = token,
        .max_body_bytes = generous,
    });
    check_local_without(on_post, {mdx::fixture_access_1, mdx::fixture_password});
    CHECK(message_of(on_post).contains(mdx::token_url(tree.root)));

    // The allowlisted URLs with the same token are served.
    CHECK(status_of(get(tree, "api/manga/status", token)) == 200);
    CHECK(status_of(get(tree, follows_page, token)) == 200);
}

TEST_CASE("fixture auth: the tripwire never echoes a URL containing a NUL byte",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    std::string url = tree.url("api/manga/status");
    url.push_back('\0');
    url.append("x");
    const TransportResult result = tree.transport->fetch(HttpRequest{
        .url = url,
        .authorization = mdx::bearer(mdx::fixture_access_1),
        .max_body_bytes = generous,
    });
    check_local_without(result, {mdx::fixture_access_1});
    CHECK(message_of(result) == "fixture: Authorization sent to a URL containing a NUL byte");
}

TEST_CASE("fixture auth: CR, LF or NUL in the authorization is a local error",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const std::string suffix =
        GENERATE(std::string{"\r\nX: y"}, std::string{"\n"}, std::string{"\0", 1});
    std::string value = mdx::bearer(mdx::fixture_access_1);
    value.append(suffix);
    const TransportResult result = get(tree, follows_page, value);
    check_local_without(result, {mdx::fixture_access_1});
    CHECK(message_of(result) == "fixture: the Authorization value contains CR, LF or NUL");
}

TEST_CASE("fixture auth: other POSTs still answer 404", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    for (const std::string& url :
         {std::string{"https://api.mangadex.network/report"}, tree.url("api/manga/status"),
          tree.url("auth/realms/mangadex/protocol/openid-connect/userinfo"),
          tree.url("auth/realms/mangadex/protocol/openid-connect/token/x"), std::string{"x"}}) {
        CAPTURE(url);
        CHECK(status_of(post(tree, url, R"({"url":"x","success":true})", "application/json")) ==
              404);
    }
}

TEST_CASE("fixture auth: synthesized answers obey max_body_bytes", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const TransportResult result =
        post(tree, mdx::token_url(tree.root),
             password_form(mdx::fixture_username, mdx::fixture_password), form_type, 16);
    REQUIRE(error_of(result) == TransportErrc::too_large);
    CHECK_FALSE(message_of(result).contains(mdx::fixture_access_1));
    const TransportResult refused = tree.transport->fetch(HttpRequest{
        .url = tree.url(follows_page),
        .max_body_bytes = 16,
    });
    CHECK(error_of(refused) == TransportErrc::too_large);
}

TEST_CASE("fixture auth: malformed auth files are local errors", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const auto [file, content] =
        GENERATE(std::pair{"password/fixture-broken.json", R"({"status":"200"})"},
                 std::pair{"password/fixture-broken.json", R"({"status":99})"},
                 std::pair{"password/fixture-broken.json", R"({"mismatch_status":302})"},
                 std::pair{"password/fixture-broken.json", R"({"expect":[]})"},
                 std::pair{"password/fixture-broken.json", R"({"expect":{"client_id":1}})"},
                 std::pair{"password/fixture-broken.json", "not json"},
                 std::pair{"password/fixture-broken.json", "[]"},
                 std::pair{"access/fixture-broken.json", R"({"valid_requests":-1})"},
                 std::pair{"access/fixture-broken.json", R"({"valid_requests":"2"})"},
                 std::pair{"refresh/fixture-broken.json", R"({"expect":"x"})"},
                 std::pair{"refresh/fixture-broken.json", R"({"expect":{"client_id":1}})"},
                 std::pair{"refresh/fixture-broken.json", R"({"status":600})"},
                 std::pair{"revoke/fixture-broken.json", R"({"expect":[1]})"},
                 std::pair{"revoke/fixture-broken.json", R"({"expect":{"client_id":null}})"},
                 std::pair{"revoke/fixture-broken.json", "{"});
    CAPTURE(file, content);
    write(tree.root / "auth" / file, content);
    const TransportResult result = request_broken(tree, file);
    REQUIRE(error_of(result) == TransportErrc::local);
    CHECK(message_of(result).contains("malformed auth/"));
    CHECK_FALSE(message_of(result).contains("fixture-broken"));
}

TEST_CASE("fixture auth: an auth file over 64 KiB is a local error, not too_large",
          "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const std::string dir = GENERATE(std::string{"access"}, std::string{"password"},
                                     std::string{"refresh"}, std::string{"revoke"});
    CAPTURE(dir);
    std::string content{R"({"pad":")"};
    content.append(std::size_t{64} * 1024, 'x');
    content.append(R"("})");
    const std::string file = dir + "/fixture-broken.json";
    write(tree.root / "auth" / file, content);
    const TransportResult result = request_broken(tree, file);
    REQUIRE(error_of(result) == TransportErrc::local);
    CHECK(message_of(result).contains(std::format("an auth/{} file exceeds 65536 bytes", dir)));
    CHECK_FALSE(message_of(result).contains("fixture-broken"));
}

TEST_CASE("fixture auth: the auth files are never served by a GET", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    for (const std::string_view rel :
         {std::string_view{"auth"}, std::string_view{"auth/password/fixture-user.json"},
          std::string_view{"auth/access/fixture-access-1.json"},
          std::string_view{"auth/realms/mangadex/protocol/openid-connect/token"}}) {
        CAPTURE(rel);
        const TransportResult result = get(tree, rel);
        CHECK(status_of(result) == 404);
        CHECK(body_of(result).empty());
    }
}

TEST_CASE("fixture auth: a symlinked auth file is never followed", "[net][fixture-auth]") {
    const AuthTree tree = make_auth_tree();
    const fs::path outside = tree.root.parent_path() / "outside.json";
    write(outside, "{}");
    fs::create_symlink(outside, tree.root / "auth" / "access" / "fixture-linked.json");
    CHECK(status_of(get(tree, follows_page, mdx::bearer("fixture-linked"))) == 401);
}

TEST_CASE("fixture auth: concurrent requests share one counter per token", "[net][fixture-auth]") {
    mdx::FixtureAuth auth{};
    auth.chain[0].valid_requests = 100;
    auth.chain[1].valid_requests = 40;
    const AuthTree tree = make_auth_tree({auth});
    constexpr std::size_t threads = 4;
    constexpr std::size_t per_thread = 50;
    std::vector<std::size_t> served(threads, 0);
    std::vector<std::size_t> refused(threads, 0);
    {
        std::vector<std::jthread> workers{};
        workers.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            // Captures by value except the two slots and the tree, which outlive the jthreads
            // (joined at the end of this block).
            workers.emplace_back(
                [t, served_slot = &served[t], refused_slot = &refused[t], tree_ptr = &tree] {
                    const std::string first = mdx::bearer(mdx::fixture_access_1);
                    const std::string second = mdx::bearer(mdx::fixture_access_2);
                    for (std::size_t i = 0; i < per_thread; ++i) {
                        const std::string& value = (i + t) % 2 == 0 ? first : second;
                        const int status = status_of(get(*tree_ptr, follows_page, value));
                        if (status == 200) {
                            ++*served_slot;
                        } else if (status == 401) {
                            ++*refused_slot;
                        }
                    }
                });
        }
    }
    std::size_t total_served = 0;
    std::size_t total_refused = 0;
    for (std::size_t t = 0; t < threads; ++t) {
        total_served += served[t];
        total_refused += refused[t];
    }
    // 100 requests per token: all 100 for the first, 40 of 100 for the second.
    CHECK(total_served == 140);
    CHECK(total_refused == 60);
}

TEST_CASE("fixture auth: the auth fixture writer refuses unsafe or inconsistent accounts",
          "[net][fixture-auth]") {
    auto dir = TempDir::create("mangadex-dl-fixture-auth-writer");
    REQUIRE(dir.has_value());
    const fs::path root = dir->path();

    CHECK_FALSE(mdx::write_auth_files(root, mdx::FixtureAuth{.username = "../x"}).has_value());
    mdx::FixtureAuth gap{};
    gap.chain[0].refresh_token.reset();
    CHECK_FALSE(mdx::write_auth_files(root, gap).has_value());
    CHECK_FALSE(mdx::write_auth_files(root, mdx::FixtureAuth{.chain = {}}).has_value());
    CHECK_FALSE(mdx::write_auth_files(root, mdx::FixtureAuth{.mismatch_status = 302}).has_value());
    CHECK_FALSE(fs::exists(root / "auth")); // every refusal comes before the first write

    REQUIRE(mdx::write_auth_files(root, mdx::FixtureAuth{}).has_value());
    for (const std::string_view rel :
         {"auth/password/fixture-user.json", "auth/refresh/fixture-refresh-1.json",
          "auth/access/fixture-access-1.json", "auth/access/fixture-access-2.json",
          "auth/revoke/fixture-refresh-1.json", "auth/revoke/fixture-refresh-2.json"}) {
        CAPTURE(rel);
        CHECK(fs::is_regular_file(root / rel));
    }
    // The last refresh token of the chain has no refresh file (the session ends there).
    CHECK_FALSE(fs::exists(root / "auth" / "refresh" / "fixture-refresh-2.json"));
}
