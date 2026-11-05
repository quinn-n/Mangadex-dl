// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/session_json_test.cpp: parse_session and session_to_json (follows plan T9,
// AC5). The round trip is exact; every malformed shape is refused with a reason phrase that holds
// no token or secret. All values are the obviously fake fixture constants.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/mdx_auth_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using md::parse_session;
using md::session_to_json;
using Json = nlohmann::json;

inline constexpr std::string_view issuer = md::official_auth_issuer;
inline constexpr std::int64_t refresh_expiry = 1'900'000'000;
inline constexpr std::int64_t access_expiry = 1'800'000'900;

[[nodiscard]] md::BearerToken token_of(std::string_view text) {
    auto token = md::BearerToken::parse(md::Secret::copy_of(text));
    REQUIRE(token.has_value());
    return std::move(*token);
}

/// A complete session with the fixture values.
[[nodiscard]] md::Session full_session() {
    return md::Session{
        .issuer = std::string{issuer},
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = md::Secret::copy_of(mdx::fixture_client_secret),
        .username = std::string{mdx::fixture_username},
        .refresh_token = token_of(mdx::fixture_refresh_1),
        .refresh_expires_at = refresh_expiry,
        .access_token = token_of(mdx::fixture_access_1),
        .access_expires_at = access_expiry,
    };
}

/// The document of full_session(), as a DOM to edit.
[[nodiscard]] Json full_document() {
    const md::Secret text = session_to_json(full_session());
    return Json::parse(text.reveal());
}

/// The reason a document is refused; checks that it is Errc::failure and holds no secret.
[[nodiscard]] std::string refusal(std::string_view text) {
    const auto parsed = parse_session(text, issuer);
    REQUIRE_FALSE(parsed.has_value());
    const md::Error& error = parsed.error();
    CHECK(error.code == md::Errc::failure);
    for (const std::string_view secret : {mdx::fixture_client_secret, mdx::fixture_refresh_1,
                                          mdx::fixture_access_1, mdx::fixture_password}) {
        CHECK_FALSE(error.message.contains(secret));
    }
    return error.message;
}

[[nodiscard]] std::string refusal_doc(const Json& doc) { return refusal(doc.dump()); }

/// The token's text, or nullopt when there is none (a checked access, visible to clang-tidy).
[[nodiscard]] std::optional<std::string_view>
token_text(const std::optional<md::BearerToken>& token) {
    if (!token.has_value()) {
        return std::nullopt;
    }
    return token->value().reveal();
}

void check_same(const md::Session& got, const md::Session& want) {
    CHECK(got.issuer == want.issuer);
    CHECK(got.client_id == want.client_id);
    CHECK(got.client_secret.reveal() == want.client_secret.reveal());
    CHECK(got.username == want.username);
    CHECK(token_text(got.refresh_token) == token_text(want.refresh_token));
    CHECK(got.refresh_expires_at == want.refresh_expires_at);
    CHECK(token_text(got.access_token) == token_text(want.access_token));
    CHECK(got.access_expires_at == want.access_expires_at);
}

TEST_CASE("session json: a full session round-trips", "[auth][session-json]") {
    const md::Session session = full_session();
    const md::Secret text = session_to_json(session);
    const auto parsed = parse_session(text.reveal(), issuer);
    REQUIRE(parsed.has_value());
    check_same(*parsed, session);
}

TEST_CASE("session json: a session without tokens round-trips", "[auth][session-json]") {
    md::Session session = full_session();
    session.refresh_token.reset();
    session.refresh_expires_at.reset();
    session.access_token.reset();
    session.access_expires_at = 0;
    const md::Secret text = session_to_json(session);
    const auto parsed = parse_session(text.reveal(), issuer);
    REQUIRE(parsed.has_value());
    check_same(*parsed, session);
}

TEST_CASE("session json: quotes, backslashes and non-ASCII text round-trip",
          "[auth][session-json]") {
    md::Session session = full_session();
    session.client_id = "client \"one\" \\ caf\xc3\xa9";
    session.username = "\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\"\\";
    session.client_secret = md::Secret::copy_of("s\\\"/x");
    session.issuer = "file:///tmp/a \"b\"/auth/realms/mangadex";
    const md::Secret text = session_to_json(session);
    const auto parsed = parse_session(text.reveal(), session.issuer);
    REQUIRE(parsed.has_value());
    check_same(*parsed, session);
}

TEST_CASE("session json: the document is format_version 1 with exactly the eight fields",
          "[auth][session-json]") {
    const md::Secret text = session_to_json(full_session());
    const std::string want = std::format(
        "{{\n  \"format_version\": 1,\n  \"issuer\": \"{}\",\n  \"client_id\": \"{}\",\n"
        "  \"client_secret\": \"{}\",\n  \"username\": \"{}\",\n  \"refresh_token\": \"{}\",\n"
        "  \"refresh_expires_at\": {},\n  \"access_token\": \"{}\",\n"
        "  \"access_expires_at\": {}\n}}\n",
        issuer, mdx::fixture_client_id, mdx::fixture_client_secret, mdx::fixture_username,
        mdx::fixture_refresh_1, refresh_expiry, mdx::fixture_access_1, access_expiry);
    CHECK(text.reveal() == want);
}

TEST_CASE("session json: absent values are written as null", "[auth][session-json]") {
    md::Session session = full_session();
    session.refresh_token.reset();
    session.refresh_expires_at.reset();
    session.access_token.reset();
    const md::Secret text = session_to_json(session);
    const Json doc = Json::parse(text.reveal());
    CHECK(doc.at("refresh_token").is_null());
    CHECK(doc.at("refresh_expires_at").is_null());
    CHECK(doc.at("access_token").is_null());
    CHECK(doc.at("access_expires_at") == access_expiry);
}

TEST_CASE("session json: a control character in a written value stays valid JSON",
          "[auth][session-json]") {
    md::Session session = full_session();
    session.username = std::string{"a\x01\n", 3};
    const md::Secret text = session_to_json(session);
    const Json doc = Json::parse(text.reveal()); // throws if the escaping were wrong
    CHECK(doc.at("username") == session.username);
    // ... and parse_session refuses it on the way back.
    CHECK(refusal(text.reveal()) == "'username' contains a control character");
}

TEST_CASE("session json: absent optional members read as null", "[auth][session-json]") {
    Json doc = full_document();
    doc.erase("refresh_token");
    doc.erase("refresh_expires_at");
    doc.erase("access_token");
    const auto parsed = parse_session(doc.dump(), issuer);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->refresh_token.has_value());
    CHECK_FALSE(parsed->refresh_expires_at.has_value());
    CHECK_FALSE(parsed->access_token.has_value());
}

TEST_CASE("session json: unknown members are ignored", "[auth][session-json]") {
    Json doc = full_document();
    doc["note"] = Json{{"nested", Json::array({"x", 1})}};
    const auto parsed = parse_session(doc.dump(), issuer);
    REQUIRE(parsed.has_value());
    CHECK(parsed->username == std::string{mdx::fixture_username});
}

TEST_CASE("session json: a duplicate key is not detected; the last value wins",
          "[auth][session-json]") {
    // V23: nlohmann keeps the last value of a repeated key. Documented, not treated as malformed.
    std::string text = full_document().dump();
    REQUIRE(text.ends_with('}'));
    text.pop_back();
    text += R"(,"username":"second-user"})";
    const auto parsed = parse_session(text, issuer);
    REQUIRE(parsed.has_value());
    CHECK(parsed->username == "second-user");
}

TEST_CASE("session json: an unknown format_version is refused with a reason",
          "[auth][session-json]") {
    Json doc = full_document();
    doc["format_version"] = GENERATE(Json(2), Json(0), Json("1"), Json(1.0), Json(-1));
    CHECK(refusal_doc(doc) == "it has an unknown format_version");

    doc.erase("format_version");
    CHECK(refusal_doc(doc) == "it has no format_version");
}

TEST_CASE("session json: a session for another issuer is refused", "[auth][session-json]") {
    const md::Secret text = session_to_json(full_session());
    const auto parsed = parse_session(text.reveal(), "file:///fixture/auth/realms/mangadex");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message == "it is for another login server");
}

TEST_CASE("session json: a token outside the charset is refused without being shown",
          "[auth][session-json]") {
    const std::string key = GENERATE(std::string{"refresh_token"}, std::string{"access_token"});
    const std::string bad = GENERATE(std::string{mdx::fixture_access_1} + " x",
                                     std::string{mdx::fixture_access_1} + "\"",
                                     std::string{mdx::fixture_access_1} + "\r\n", std::string{},
                                     std::string(md::max_token_bytes + 1, 'a'));
    Json doc = full_document();
    doc[key] = bad;
    CHECK(refusal_doc(doc) == std::format("'{}' is not a valid token", key));
}

TEST_CASE("session json: a token of max_token_bytes is accepted", "[auth][session-json]") {
    Json doc = full_document();
    const std::string longest(md::max_token_bytes, 'a');
    doc["access_token"] = longest;
    const auto parsed = parse_session(doc.dump(), issuer);
    REQUIRE(parsed.has_value());
    CHECK(token_text(parsed->access_token) == std::string_view{longest});
}

TEST_CASE("session json: more than 64 KiB is refused", "[auth][session-json]") {
    std::string text = full_document().dump();
    REQUIRE(text.size() < md::max_session_file_bytes);
    text.resize(md::max_session_file_bytes, ' '); // trailing whitespace: still a valid document
    CHECK(parse_session(text, issuer).has_value());
    text.push_back(' ');
    CHECK(refusal(text) == "it is larger than 65536 bytes");
}

TEST_CASE("session json: text that is not a JSON object is refused", "[auth][session-json]") {
    CHECK(refusal(std::string_view{"{\"format_version\": 1,"}) == "it is not valid JSON");
    CHECK(refusal(std::string_view{}) == "it is not valid JSON");
    CHECK(refusal(std::string_view{"\xEF\xBB\xBF{}"}) == "it is not valid JSON");
    CHECK(refusal(std::string_view{"[[[[[[]]]]]]"}) == "it is not valid JSON");
    CHECK(refusal(std::string_view{"[1]"}) == "it is not a JSON object");
    CHECK(refusal(std::string_view{"\"text\""}) == "it is not a JSON object");
}

TEST_CASE("session json: a missing or mistyped detail is refused", "[auth][session-json]") {
    const std::string key = GENERATE(std::string{"issuer"}, std::string{"client_id"},
                                     std::string{"client_secret"}, std::string{"username"});
    Json doc = full_document();
    doc[key] = 7;
    CHECK(refusal_doc(doc) == std::format("'{}' has the wrong type", key));
    doc[key] = nullptr;
    CHECK(refusal_doc(doc) == std::format("'{}' is missing", key));
    doc.erase(key);
    CHECK(refusal_doc(doc) == std::format("'{}' is missing", key));
}

TEST_CASE("session json: an empty, overlong or control-laden detail is refused",
          "[auth][session-json]") {
    const std::string key =
        GENERATE(std::string{"client_id"}, std::string{"client_secret"}, std::string{"username"});
    Json doc = full_document();
    doc[key] = "";
    CHECK(refusal_doc(doc) == std::format("'{}' is empty", key));
    doc[key] = std::string(md::max_prompt_answer_bytes + 1, 'u');
    CHECK(refusal_doc(doc) == std::format("'{}' is longer than 4096 bytes", key));
    doc[key] = std::string(md::max_prompt_answer_bytes, 'u');
    CHECK(parse_session(doc.dump(), issuer).has_value());
    const std::string control =
        GENERATE(std::string{"\x01"}, std::string(1, '\0'), std::string{"\x7f"},
                 std::string{"\xc2\x9b"}, std::string{"\t"});
    doc[key] = std::string{mdx::fixture_client_secret} + control;
    CHECK(refusal_doc(doc) == std::format("'{}' contains a control character", key));
}

TEST_CASE("session json: a token of the wrong type is refused", "[auth][session-json]") {
    const std::string key = GENERATE(std::string{"refresh_token"}, std::string{"access_token"});
    Json doc = full_document();
    doc[key] = Json::array({std::string{mdx::fixture_access_1}});
    CHECK(refusal_doc(doc) == std::format("'{}' has the wrong type", key));
}

TEST_CASE("session json: the times are non-negative whole seconds", "[auth][session-json]") {
    const std::string key =
        GENERATE(std::string{"refresh_expires_at"}, std::string{"access_expires_at"});
    Json doc = full_document();
    doc[key] = -1;
    CHECK(refusal_doc(doc) == std::format("'{}' is not a valid time", key));
    doc[key] = std::uint64_t{9'223'372'036'854'775'808U}; // INT64_MAX + 1
    CHECK(refusal_doc(doc) == std::format("'{}' is not a valid time", key));
    doc[key] = 1.5;
    CHECK(refusal_doc(doc) == std::format("'{}' has the wrong type", key));
    doc[key] = "1800000000";
    CHECK(refusal_doc(doc) == std::format("'{}' has the wrong type", key));
    doc[key] = std::int64_t{9'223'372'036'854'775'807};
    CHECK(parse_session(doc.dump(), issuer).has_value());
}

TEST_CASE("session json: access_expires_at is required", "[auth][session-json]") {
    Json doc = full_document();
    doc.erase("access_expires_at");
    CHECK(refusal_doc(doc) == "'access_expires_at' is missing");
}

} // namespace
