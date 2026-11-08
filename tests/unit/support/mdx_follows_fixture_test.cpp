// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/support/mdx_follows_fixture_test.cpp: the follows fixture trees themselves
// (tests/support/mdx_follows_fixture.hpp, follows plan T15). The cli and E2E tiers trust them, so
// they are checked here against the real core: every tree writes; the creds files have exactly
// 0600 / 0644 under any umask and load (or are refused) through load_credentials_file; each
// follows set is served by the fixture transport and parsed by parse_follows_page; the status
// maps parse (FS1, FS2) or are malformed (FS3a, FS3b); and the fake accounts answer as designed.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/fs_test_util.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/mdx_follows_fixture.hpp"
#include "support/read_only_fd.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

namespace fs = std::filesystem;
namespace mdx = mangadex_dl::test::mdx;
using mangadex_dl::ClientCredentials;
using mangadex_dl::Errc;
using mangadex_dl::HttpMethod;
using mangadex_dl::HttpRequest;
using mangadex_dl::Secret;
using mangadex_dl::Transport;
using mangadex_dl::TransportResult;
using mangadex_dl::test::TempDir;

constexpr std::string_view form_type = "application/x-www-form-urlencoded";
constexpr std::size_t generous = std::size_t{1} << 20U;

[[nodiscard]] std::string clean_id() { return std::string{mdx::clean_manga_id}; }
[[nodiscard]] std::string unknown_id() { return std::string{mdx::unknown_manga_id}; }

/// Sets the process umask for one scope (the unit tests run one test case per process).
class UmaskGuard {
public:
    explicit UmaskGuard(::mode_t mask) noexcept : old_(::umask(mask)) {}
    UmaskGuard(const UmaskGuard&) = delete;
    UmaskGuard& operator=(const UmaskGuard&) = delete;
    UmaskGuard(UmaskGuard&&) = delete;
    UmaskGuard& operator=(UmaskGuard&&) = delete;
    ~UmaskGuard() { static_cast<void>(::umask(old_)); }

private:
    ::mode_t old_ = 0;
};

/// The permission bits (and setuid/setgid/sticky) of `path`, not following a symlink.
[[nodiscard]] ::mode_t mode_of(const fs::path& path) {
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    return st.st_mode & 07777U;
}

/// The fixture named `name` in follows_fixtures().
[[nodiscard]] mdx::FollowsFixture follows_fixture_named(std::string_view name) {
    for (const mdx::NamedFollowsFixture& fixture : mdx::follows_fixtures()) {
        if (fixture.name == name) {
            return fixture.make();
        }
    }
    FAIL("no follows fixture named " << name);
    return {};
}

/// A follows tree written into a temp dir, with its fixture transport.
struct WrittenTree {
    std::optional<TempDir> dir{}; ///< always engaged; optional only for the member initializer
    fs::path root{};
    std::unique_ptr<Transport> transport{};

    [[nodiscard]] std::string api() const { return std::format("file://{}/api", root.string()); }
};

[[nodiscard]] WrittenTree write_tree(std::string_view name) {
    auto dir = TempDir::create("mangadex-dl-follows-fixture");
    REQUIRE(dir.has_value());
    const fs::path root = fs::absolute(dir->path()) / std::string{name};
    const auto written = mdx::write_follows_tree(root, follows_fixture_named(name));
    INFO((written.has_value() ? std::string{} : written.error()));
    REQUIRE(written.has_value());
    auto transport = mangadex_dl::make_fixture_transport(root);
    REQUIRE(transport.has_value());
    return WrittenTree{.dir = std::move(*dir), .root = root, .transport = std::move(*transport)};
}

[[nodiscard]] TransportResult get(const WrittenTree& tree, std::string url,
                                  std::string authorization) {
    return tree.transport->fetch(HttpRequest{
        .url = std::move(url),
        .authorization = std::move(authorization),
        .max_body_bytes = generous,
    });
}

[[nodiscard]] TransportResult post(const WrittenTree& tree, std::string url, const Secret& body) {
    return tree.transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = std::move(url),
        .body = std::string{body.reveal()},
        .content_type = std::string{form_type},
        .max_body_bytes = generous,
    });
}

/// The ids every follows page yields, fetched as the client steps (by the items received, until
/// an empty page or offset >= total), plus how many pages that took. The first parse error
/// instead, if any.
struct FetchedFollows {
    std::vector<std::string> ids{};
    std::size_t pages = 0;
    std::optional<mangadex_dl::Error> error{};
};

[[nodiscard]] FetchedFollows fetch_follows(const WrittenTree& tree, std::string_view token) {
    FetchedFollows fetched{};
    std::size_t offset = 0;
    for (;;) {
        REQUIRE(fetched.pages < 16); // no fixture tree has more pages: a stepping bug, not a hang
        const TransportResult response =
            get(tree, mangadex_dl::follows_request_url(tree.api(), offset), mdx::bearer(token));
        REQUIRE(response.has_value());
        REQUIRE(response->status == 200);
        ++fetched.pages;
        auto page = mangadex_dl::parse_follows_page(response->body);
        if (!page) {
            fetched.error = std::move(page.error());
            return fetched;
        }
        for (const mangadex_dl::MangaId& id : page->titles) {
            fetched.ids.emplace_back(id.str());
        }
        offset += page->titles.size();
        if (page->titles.empty() || offset >= page->total) {
            return fetched;
        }
    }
}

[[nodiscard]] mangadex_dl::Result<mangadex_dl::StatusMap>
fetch_status_map(const WrittenTree& tree) {
    const TransportResult response = get(tree, mangadex_dl::status_map_request_url(tree.api()),
                                         mdx::bearer(mdx::fixture_access_1));
    REQUIRE(response.has_value());
    REQUIRE(response->status == 200);
    return mangadex_dl::parse_status_map(response->body);
}

/// The value of an optional Secret, nullopt without one. Borrows from `secret`.
[[nodiscard]] std::optional<std::string_view> revealed(const std::optional<Secret>& secret) {
    if (!secret.has_value()) {
        return std::nullopt;
    }
    return secret->reveal();
}

/// The value of an optional token, nullopt without one. Borrows from `token`.
[[nodiscard]] std::optional<std::string_view>
revealed(const std::optional<mangadex_dl::BearerToken>& token) {
    if (!token.has_value()) {
        return std::nullopt;
    }
    return token->value().reveal();
}

[[nodiscard]] ClientCredentials fixture_client() {
    return ClientCredentials{.client_id = std::string{mdx::fixture_client_id},
                             .client_secret = Secret::copy_of(mdx::fixture_client_secret)};
}

/// The password grant for `username` / `password` against `tree`'s Keycloak stand-in.
[[nodiscard]] TransportResult password_grant(const WrittenTree& tree, std::string_view username,
                                             std::string_view password) {
    const Secret secret = Secret::copy_of(password);
    return post(tree, mdx::token_url(tree.root),
                mangadex_dl::password_grant_body(fixture_client(), username, secret));
}

[[nodiscard]] TransportResult revoke(const WrittenTree& tree, std::string_view refresh) {
    const auto token = mangadex_dl::BearerToken::parse(Secret::copy_of(refresh));
    REQUIRE(token.has_value());
    return post(tree, mdx::revoke_url(tree.root),
                mangadex_dl::revoke_body(fixture_client(), *token));
}

/// The `error` member of a JSON response body, "<none>" without one.
[[nodiscard]] std::string error_code_of(const TransportResult& result) {
    REQUIRE(result.has_value());
    const nlohmann::json doc = nlohmann::json::parse(result->body, nullptr, false);
    if (!doc.is_object() || !doc.contains("error") || !doc.at("error").is_string()) {
        return "<none>";
    }
    return doc.at("error").get<std::string>();
}

/// What write_creds_files writes, sorted.
[[nodiscard]] std::vector<std::string> creds_names() {
    return {"credentials",
            "credentials-0644",
            "credentials-client",
            "credentials-revoke-503",
            "credentials-unapproved",
            "credentials-wrong-password",
            "credentials-wrong-password-400",
            "password-wrong.txt",
            "password.txt"};
}

} // namespace

TEST_CASE("follows fixture: eleven trees with unique names, none shared with all_fixtures()",
          "[follows][mdx_fixture]") {
    const std::vector<mdx::NamedFollowsFixture> trees = mdx::follows_fixtures();
    CHECK(trees.size() == 11);
    CHECK(mdx::all_fixtures().size() == 12);
    std::set<std::string_view> names{};
    for (const mdx::NamedFixture& fixture : mdx::all_fixtures()) {
        names.insert(fixture.name);
    }
    for (const mdx::NamedFollowsFixture& fixture : trees) {
        CAPTURE(fixture.name);
        CHECK(fixture.make != nullptr);
        CHECK(fixture.name.starts_with("follows-"));
        CHECK(names.insert(fixture.name).second);
    }
}

TEST_CASE("follows fixture: every tree writes its titles, pages, accounts and creds",
          "[follows][mdx_fixture]") {
    const TempDir dir = mangadex_dl::test::fs_util::make_temp_dir("mangadex-dl-follows-fixture");
    for (const mdx::NamedFollowsFixture& named : mdx::follows_fixtures()) {
        CAPTURE(named.name);
        const mdx::FollowsFixture fixture = named.make();
        const fs::path root = dir.path() / std::string{named.name};
        const auto written = mdx::write_follows_tree(root, fixture);
        INFO((written.has_value() ? std::string{} : written.error()));
        REQUIRE(written.has_value());

        for (const mdx::FixtureManga& title : fixture.titles) {
            CHECK(fs::is_regular_file(root / "api" / "manga" / (title.id + ".json")));
        }
        const fs::path follows_dir = root / "api" / "user" / "follows" / "manga";
        const std::size_t pages = std::max<std::size_t>(
            1, (fixture.follows.size() + fixture.page_limit - 1) / fixture.page_limit);
        CHECK(mangadex_dl::test::fs_util::list_dir(follows_dir).size() == pages);
        for (std::size_t i = 0; i < pages; ++i) {
            CHECK(fs::is_regular_file(follows_dir /
                                      std::format("offset-{}.json", i * fixture.page_limit)));
        }
        CHECK(fs::is_regular_file(root / "api" / "manga" / "status.json") ==
              fixture.statuses.has_value());

        // The seeding `login --credentials <tree>/creds/credentials` needs fixture-user's password
        // file, and every token a grant hands out needs its access file.
        for (const mdx::FixtureAuth& account : fixture.accounts) {
            CHECK(fs::is_regular_file(root / "auth" / "password" / (account.username + ".json")));
            for (const mdx::FixtureGrant& grant : account.chain) {
                CHECK(
                    fs::is_regular_file(root / "auth" / "access" / (grant.access_token + ".json")));
            }
        }
        CHECK(fs::is_regular_file(root / "auth" / "password" /
                                  (std::string{mdx::fixture_username} + ".json")));
        CHECK(mangadex_dl::test::fs_util::list_dir(root / "creds") == creds_names());
    }
}

TEST_CASE("follows fixture: the creds modes are exact under umask 077 and 022",
          "[follows][mdx_fixture]") {
    const ::mode_t mask = GENERATE(::mode_t{077}, ::mode_t{022});
    CAPTURE(mask);
    const UmaskGuard umask{mask};
    const TempDir dir = mangadex_dl::test::fs_util::make_temp_dir("mangadex-dl-follows-creds");
    const fs::path creds = dir.path() / "creds";

    // Twice: a rewrite replaces each file and still sets the exact mode.
    for (int round = 0; round < 2; ++round) {
        CAPTURE(round);
        const auto written = mdx::write_creds_files(creds);
        INFO((written.has_value() ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        CHECK(mode_of(creds) == 0700U);
        for (const mdx::FixtureCredsFile& file : mdx::follows_creds_files()) {
            CAPTURE(file.name);
            CHECK(fs::is_regular_file(fs::symlink_status(creds / file.name)));
            CHECK(mode_of(creds / file.name) == file.mode);
            CHECK(mangadex_dl::test::fs_util::read_file(creds / file.name) == file.bytes);
        }
    }
    CHECK(mode_of(creds / "credentials") == 0600U);
    CHECK(mode_of(creds / "credentials-0644") == 0644U);
}

TEST_CASE("follows fixture: a symlink in place of a creds file is replaced, never written through",
          "[follows][mdx_fixture]") {
    const TempDir dir = mangadex_dl::test::fs_util::make_temp_dir("mangadex-dl-follows-creds");
    const fs::path creds = dir.path() / "creds";
    const fs::path target = dir.path() / "target";
    mangadex_dl::test::fs_util::write_file(target, "untouched\n");
    fs::create_directories(creds);
    fs::create_symlink(target, creds / "credentials");

    const auto written = mdx::write_creds_files(creds);
    INFO((written.has_value() ? std::string{} : written.error()));
    REQUIRE(written.has_value());
    CHECK(fs::is_regular_file(fs::symlink_status(creds / "credentials")));
    CHECK(mode_of(creds / "credentials") == 0600U);
    CHECK(mangadex_dl::test::fs_util::read_file(target) == "untouched\n");
}

TEST_CASE("follows fixture: a directory in place of a creds file is an error naming the file",
          "[follows][mdx_fixture]") {
    const TempDir dir = mangadex_dl::test::fs_util::make_temp_dir("mangadex-dl-follows-creds");
    const fs::path creds = dir.path() / "creds";
    fs::create_directories(creds / "credentials");

    const auto written = mdx::write_creds_files(creds);
    REQUIRE_FALSE(written.has_value());
    CAPTURE(written.error());
    CHECK(written.error().starts_with((creds / "credentials").string() + ": "));
    CHECK(fs::is_directory(fs::symlink_status(creds / "credentials")));
}

TEST_CASE("follows fixture: a symlink at creds/ is refused, never followed",
          "[follows][mdx_fixture]") {
    const TempDir dir = mangadex_dl::test::fs_util::make_temp_dir("mangadex-dl-follows-creds");
    const fs::path elsewhere = dir.path() / "elsewhere";
    fs::create_directories(elsewhere);
    fs::permissions(elsewhere, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                    fs::perm_options::replace);
    const fs::path creds = dir.path() / "creds";
    fs::create_directory_symlink(elsewhere, creds);

    const auto written = mdx::write_creds_files(creds);
    REQUIRE_FALSE(written.has_value());
    CAPTURE(written.error());
    CHECK(written.error().starts_with(creds.string() + ": "));
    CHECK(mangadex_dl::test::fs_util::list_dir(elsewhere).empty());
    CHECK(mode_of(elsewhere) == 0750U);
}

TEST_CASE("follows fixture: the creds files load as the fixture accounts",
          "[follows][mdx_fixture]") {
    const TempDir dir = mangadex_dl::test::fs_util::make_temp_dir("mangadex-dl-follows-creds");
    const fs::path creds = dir.path() / "creds";
    REQUIRE(mdx::write_creds_files(creds).has_value());

    struct Expected {
        std::string_view file{};
        std::string_view username{};
        std::optional<std::string_view> password{};
    };
    const std::vector<Expected> table{
        {.file = "credentials",
         .username = mdx::fixture_username,
         .password = mdx::fixture_password},
        {.file = "credentials-client", .username = mdx::fixture_username},
        {.file = "credentials-wrong-password",
         .username = mdx::fixture_username,
         .password = mdx::fixture_wrong_password},
        {.file = "credentials-wrong-password-400",
         .username = mdx::fixture_user_400,
         .password = mdx::fixture_wrong_password},
        {.file = "credentials-unapproved",
         .username = mdx::fixture_user_unapproved,
         .password = mdx::fixture_password},
        {.file = "credentials-revoke-503",
         .username = mdx::fixture_user_revoke_503,
         .password = mdx::fixture_password},
    };
    for (const Expected& expected : table) {
        CAPTURE(expected.file);
        const auto loaded = mangadex_dl::load_credentials_file(creds / expected.file, true);
        INFO((loaded.has_value() ? std::string{} : loaded.error().message));
        REQUIRE(loaded.has_value());
        CHECK(loaded->client_id == std::string{mdx::fixture_client_id});
        CHECK(revealed(loaded->client_secret) ==
              std::optional<std::string_view>{mdx::fixture_client_secret});
        CHECK(loaded->username == std::string{expected.username});
        CHECK(revealed(loaded->password) == expected.password);
    }

    // Too open: refused, and the refusal shows no value.
    const auto shared = mangadex_dl::load_credentials_file(creds / "credentials-0644", true);
    REQUIRE_FALSE(shared.has_value());
    CHECK(shared.error().code == Errc::failure);
    CHECK_FALSE(shared.error().message.contains(mdx::fixture_password));
    CHECK_FALSE(shared.error().message.contains(mdx::fixture_client_secret));

    // --password-stdin reads the first line of password.txt / password-wrong.txt.
    for (const auto& [file, password] :
         {std::pair{std::string_view{"password.txt"}, mdx::fixture_password},
          std::pair{std::string_view{"password-wrong.txt"}, mdx::fixture_wrong_password}}) {
        CAPTURE(file);
        const mangadex_dl::UniqueFd fd = mangadex_dl::test::open_read_only(creds / file);
        const auto read = mangadex_dl::read_password_stdin(fd.get());
        REQUIRE(read.has_value());
        CHECK(read->reveal() == password);
    }
}

TEST_CASE("follows fixture: each follows set is served and parsed", "[follows][mdx_fixture]") {
    struct Expected {
        std::string_view tree{};
        std::vector<std::string> ids{};
        std::size_t pages = 1;
    };
    const std::vector<Expected> table{
        {.tree = "follows-clean", .ids = {clean_id()}},
        {.tree = "follows-unknown", .ids = {clean_id(), unknown_id()}},
        {.tree = "follows-empty", .ids = {}},
        {.tree = "follows-twice", .ids = {clean_id(), clean_id()}},
        {.tree = "follows-bulk",
         .ids = {std::string{mdx::follows_bulk_id}, unknown_id()},
         .pages = 2},
        {.tree = "follows-nochapters",
         .ids = {std::string{mdx::follows_nochapters_id}, unknown_id()}},
        {.tree = "follows-status",
         .ids = {unknown_id(), clean_id(), std::string{mdx::follows_paused_id},
                 std::string{mdx::follows_no_status_id}}},
        {.tree = "follows-status-empty", .ids = {clean_id()}},
        {.tree = "follows-status-bad-value", .ids = {clean_id()}},
        {.tree = "follows-status-bad-key", .ids = {clean_id()}},
    };
    for (const Expected& expected : table) {
        CAPTURE(expected.tree);
        const WrittenTree tree = write_tree(expected.tree);
        const FetchedFollows fetched = fetch_follows(tree, mdx::fixture_access_1);
        INFO((fetched.error.has_value() ? fetched.error->message : std::string{}));
        REQUIRE_FALSE(fetched.error.has_value());
        CHECK(fetched.ids == expected.ids);
        CHECK(fetched.pages == expected.pages);

        // The other account's token is accepted too; no token is the API's 401.
        CHECK(fetch_follows(tree, mdx::fixture_access_revoke_503).ids == expected.ids);
        const TransportResult anonymous =
            get(tree, mangadex_dl::follows_request_url(tree.api(), 0), {});
        REQUIRE(anonymous.has_value());
        CHECK(anonymous->status == 401);
    }
}

TEST_CASE("follows fixture: F5's follows page is malformed", "[follows][mdx_fixture]") {
    const WrittenTree tree = write_tree("follows-bad-id");
    const FetchedFollows fetched = fetch_follows(tree, mdx::fixture_access_1);
    REQUIRE(fetched.error.has_value());
    const mangadex_dl::Error error = fetched.error.value_or(mangadex_dl::Error{});
    CHECK(error.code == Errc::network);
    CHECK(error.message.starts_with("malformed MangaDex API response (follows page)"));
}

TEST_CASE("follows fixture: FS1 and FS2 status maps parse; FS3a and FS3b are malformed",
          "[follows][mdx_fixture]") {
    SECTION("FS1") {
        const WrittenTree tree = write_tree("follows-status");
        const auto map = fetch_status_map(tree);
        INFO((map.has_value() ? std::string{} : map.error().message));
        REQUIRE(map.has_value());
        std::vector<std::pair<std::string, std::string>> entries{};
        for (const auto& [id, status] : map->statuses) {
            entries.emplace_back(id.str(), status);
        }
        CHECK(entries == std::vector<std::pair<std::string, std::string>>{
                             {clean_id(), "reading"},
                             {unknown_id(), "completed"},
                             {std::string{mdx::follows_paused_id}, "paused"},
                             {std::string{mdx::status_not_followed_id}, "reading"},
                         });

        const std::vector<std::string> words{"reading"};
        const auto filter = mangadex_dl::StatusFilter::parse(words);
        REQUIRE(filter.has_value());
        const FetchedFollows follows = fetch_follows(tree, mdx::fixture_access_1);
        std::vector<mangadex_dl::MangaId> ids{};
        for (const std::string& id : follows.ids) {
            auto parsed = mangadex_dl::MangaId::parse(id);
            REQUIRE(parsed.has_value());
            ids.push_back(std::move(*parsed));
        }
        const mangadex_dl::FilteredFollows kept = mangadex_dl::filter_follows(ids, *map, *filter);
        REQUIRE(kept.titles.size() == 1);
        CHECK(kept.titles.front().str() == clean_id());
        CHECK(kept.follows == 4);
        CHECK(kept.unknown.size() == 1);
        CHECK(kept.unknown.contains("paused"));
    }
    SECTION("FS2") {
        const WrittenTree tree = write_tree("follows-status-empty");
        const auto map = fetch_status_map(tree);
        REQUIRE(map.has_value());
        CHECK(map->statuses.empty());
    }
    SECTION("FS3a and FS3b") {
        for (const std::string_view name : {"follows-status-bad-value", "follows-status-bad-key"}) {
            CAPTURE(name);
            const WrittenTree tree = write_tree(name);
            const auto map = fetch_status_map(tree);
            REQUIRE_FALSE(map.has_value());
            CHECK(map.error().code == Errc::network);
            CHECK(map.error().message.starts_with(
                "malformed MangaDex API response (reading statuses)"));
        }
    }
    SECTION("the F trees serve no status map") {
        const WrittenTree tree = write_tree("follows-clean");
        const TransportResult response = get(tree, mangadex_dl::status_map_request_url(tree.api()),
                                             mdx::bearer(mdx::fixture_access_1));
        REQUIRE(response.has_value());
        CHECK(response->status == 404);
    }
}

TEST_CASE("follows fixture: the accounts answer as designed", "[follows][mdx_fixture]") {
    const WrittenTree tree = write_tree("follows-clean");

    SECTION("fixture-user: the default chain; a wrong password is 401 invalid_grant") {
        const TransportResult ok =
            password_grant(tree, mdx::fixture_username, mdx::fixture_password);
        REQUIRE(ok.has_value());
        CHECK(ok->status == 200);
        const auto grant = mangadex_dl::parse_token_response(ok->body);
        REQUIRE(grant.has_value());
        CHECK(grant->access.value().reveal() == mdx::fixture_access_1);

        const TransportResult wrong =
            password_grant(tree, mdx::fixture_username, mdx::fixture_wrong_password);
        REQUIRE(wrong.has_value());
        CHECK(wrong->status == 401);
        CHECK(error_code_of(wrong) == "invalid_grant");
    }
    SECTION("fixture-user-400: a wrong password is 400 invalid_grant") {
        const TransportResult ok =
            password_grant(tree, mdx::fixture_user_400, mdx::fixture_password);
        REQUIRE(ok.has_value());
        CHECK(ok->status == 200);

        const TransportResult wrong =
            password_grant(tree, mdx::fixture_user_400, mdx::fixture_wrong_password);
        REQUIRE(wrong.has_value());
        CHECK(wrong->status == 400);
        CHECK(error_code_of(wrong) == "invalid_grant");
    }
    SECTION("fixture-user-unapproved: 400 unauthorized_client") {
        const TransportResult refused =
            password_grant(tree, mdx::fixture_user_unapproved, mdx::fixture_password);
        REQUIRE(refused.has_value());
        CHECK(refused->status == 400);
        CHECK(error_code_of(refused) == "unauthorized_client");
    }
    SECTION("fixture-user-revoke-503: its own tokens, and its revoke is 503") {
        const TransportResult ok =
            password_grant(tree, mdx::fixture_user_revoke_503, mdx::fixture_password);
        REQUIRE(ok.has_value());
        CHECK(ok->status == 200);
        const auto grant = mangadex_dl::parse_token_response(ok->body);
        REQUIRE(grant.has_value());
        CHECK(grant->access.value().reveal() == mdx::fixture_access_revoke_503);
        CHECK(revealed(grant->refresh) ==
              std::optional<std::string_view>{mdx::fixture_refresh_revoke_503});

        const TransportResult failed = revoke(tree, mdx::fixture_refresh_revoke_503);
        REQUIRE(failed.has_value());
        CHECK(failed->status == 503);
        const TransportResult revoked = revoke(tree, mdx::fixture_refresh_1);
        REQUIRE(revoked.has_value());
        CHECK(revoked->status == 200);
    }
}
