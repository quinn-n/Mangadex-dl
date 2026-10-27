// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/support/mdx_fixture_list_test.cpp: the generated custom-list documents in
// tests/support/mdx_fixture.hpp (list spec "E2E fixtures needed", L1-L10; list plan T7) and the
// fixture-mode list fetch end to end (fact-check V9: `file://<root>/api/list/<id>` reads
// `<root>/api/list/<id>.json`; V19: a missing document is HTTP 404, so exit 4).
// Test names start with "mdlist" so the TSan run over `unit.mdlist*` covers the fetches here.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view clean_id = "11111111-1111-4111-8111-111111111111";

[[nodiscard]] md::ListId list(std::uint64_t n) {
    auto id = md::ListId::parse(mdx::list_id(n));
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] fs::path list_file(const fs::path& root, std::uint64_t n) {
    return root / "api" / "list" / (mdx::list_id(n) + ".json");
}

/// A fixture tree in a fresh temporary directory, fetched through the real fixture transport.
class Tree {
public:
    explicit Tree(const mdx::FixtureManga& manga) {
        auto dir = md::test::TempDir::create("mangadex-dl-mdlist-fixture");
        REQUIRE(dir.has_value());
        dir_.emplace(std::move(*dir));
        root_ = fs::absolute(dir_->path()) / "tree";
        const auto written = mdx::write_fixture_tree(root_, manga);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        auto transport = md::make_fixture_transport(root_);
        REQUIRE(transport.has_value());
        transport_ = std::move(*transport);
        auto base = md::ApiBase::parse(std::format("file://{}", root_.string()));
        REQUIRE(base.has_value());
        base_ = std::move(*base);
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

    [[nodiscard]] md::ListFetchResult<md::CustomList> fetch(const md::ListId& id) {
        md::ListFetchServices services{};
        services.transport = transport_.get();
        services.clock = clock_.get();
        services.jitter = [](std::uint32_t) { return std::uint32_t{0}; };
        return md::fetch_custom_list(id, base_, services);
    }

private:
    std::optional<md::test::TempDir> dir_{};
    fs::path root_{};
    std::unique_ptr<md::Transport> transport_{};
    std::unique_ptr<md::Clock> clock_{md::make_instant_clock()};
    md::ApiBase base_{};
};

// ---- the documents ------------------------------------------------------------------------------

TEST_CASE("mdlist fixture: list ids follow a0000000-0000-4000-8000-<n>", "[mdlist][mdx_fixture]") {
    CHECK(mdx::list_id(1) == "a0000000-0000-4000-8000-000000000001");
    CHECK(mdx::list_id(10) == "a0000000-0000-4000-8000-00000000000a");
    CHECK(mdx::list_id(mdx::unknown_list_n) == "a0000000-0000-4000-8000-0000000000ff");
    CHECK(md::looks_like_list_id(mdx::list_id(1)));
}

TEST_CASE("mdlist fixture: list_json has the live shape with one user entry, last",
          "[mdlist][mdx_fixture]") {
    const mdx::FixtureList fixture{
        .id = mdx::list_id(2),
        .name = "Clean And Unknown",
        .manga_ids = {std::string{clean_id}, std::string{mdx::unknown_manga_id}}};
    const json doc = mdx::list_json(fixture);
    CHECK(doc.at("result") == "ok");
    CHECK(doc.at("response") == "entity");
    const json& data = doc.at("data");
    CHECK(data.at("id") == mdx::list_id(2));
    CHECK(data.at("type") == "custom_list");
    CHECK(data.at("attributes") ==
          json{{"name", "Clean And Unknown"}, {"visibility", "public"}, {"version", 1}});
    const json& rels = data.at("relationships");
    REQUIRE(rels.size() == 3);
    CHECK(rels.at(0) == json{{"id", clean_id}, {"type", "manga"}});
    CHECK(rels.at(1) == json{{"id", mdx::unknown_manga_id}, {"type", "manga"}});
    CHECK(rels.at(2) == json{{"id", mdx::list_owner_id}, {"type", "user"}});

    const json unnamed = mdx::list_json(
        {.id = mdx::list_id(6), .manga_ids = {std::string{clean_id}}, .data_id = mdx::list_id(1)});
    CHECK(unnamed.at("data").at("id") == mdx::list_id(1));
    CHECK(unnamed.at("data").at("attributes").at("name").is_null());
}

TEST_CASE("mdlist fixture: the trees carry the spec's lists", "[mdlist][mdx_fixture]") {
    const auto ids_of = [](const mdx::FixtureManga& manga) {
        std::vector<std::string> ids;
        ids.reserve(manga.lists.size());
        for (const mdx::FixtureList& l : manga.lists) {
            ids.push_back(l.id);
        }
        return ids;
    };
    // Only these four trees serve lists; every other tree (basic and the seven clean-* variants,
    // which derive from clean_fixture() and must drop its lists) serves none.
    const auto expected_for = [](std::string_view name) -> std::vector<std::string> {
        if (name == "clean") {
            return {mdx::list_id(1), mdx::list_id(2), mdx::list_id(3),
                    mdx::list_id(4), mdx::list_id(5), mdx::list_id(6)};
        }
        if (name == "nochapters") {
            return {mdx::list_id(8)};
        }
        if (name == "incomplete") {
            return {mdx::list_id(9)};
        }
        if (name == "bulk") {
            return {mdx::list_id(10)};
        }
        return {};
    };
    std::size_t with_lists = 0;
    for (const mdx::NamedFixture& fixture : mdx::all_fixtures()) {
        INFO("fixture " << fixture.name);
        const mdx::FixtureManga manga = fixture.make();
        CHECK(ids_of(manga) == expected_for(fixture.name));
        with_lists += manga.lists.empty() ? 0U : 1U;
        for (const mdx::FixtureList& l : manga.lists) {
            CHECK(l.id != mdx::list_id(mdx::unknown_list_n));
            CHECK(mdx::detail::safe_component(l.id));
        }
    }
    CHECK(with_lists == 4);
    CHECK(mdx::all_fixtures().size() == 12);
}

TEST_CASE("mdlist fixture: write_fixture_tree writes api/list/<id>.json", "[mdlist][mdx_fixture]") {
    const mdx::FixtureManga clean = mdx::clean_fixture();
    const Tree tree{clean};
    for (const mdx::FixtureList& l : clean.lists) {
        INFO("list " << l.id);
        const fs::path path = tree.root() / "api" / "list" / (l.id + ".json");
        REQUIRE(fs::is_regular_file(path));
        CHECK(json::parse(read_file(path)) == mdx::list_json(l));
    }
    CHECK(!fs::exists(list_file(tree.root(), mdx::unknown_list_n)));

    const Tree grown{mdx::clean_grown_fixture()};
    CHECK(!fs::exists(grown.root() / "api" / "list"));
}

TEST_CASE("mdlist fixture: write_fixture_tree refuses an unsafe list id", "[mdlist][mdx_fixture]") {
    auto dir = md::test::TempDir::create();
    REQUIRE(dir.has_value());
    mdx::FixtureManga manga = mdx::clean_fixture();
    manga.lists.push_back({.id = "../escape"});
    const auto written = mdx::write_fixture_tree(dir->path() / "t", manga);
    REQUIRE(!written.has_value());
    CHECK(written.error().contains("unsafe list id"));
    CHECK(!fs::exists(dir->path() / "t"));
    CHECK(!fs::exists(dir->path() / "escape.json"));
}

// ---- fixture-mode fetches (V9, V19) -------------------------------------------------------------

TEST_CASE("mdlist fixture: L1 is fetched from <root>/api/list/<id>.json", "[mdlist][mdx_fixture]") {
    Tree tree{mdx::clean_fixture()};
    const auto l1 = tree.fetch(list(1));
    REQUIRE(l1.has_value());
    CHECK(*l1 == md::CustomList{.id = list(1), .name = "Clean List", .titles = {manga(clean_id)}});

    const auto l2 = tree.fetch(list(2));
    REQUIRE(l2.has_value());
    CHECK(l2->titles == std::vector{manga(clean_id), manga(mdx::unknown_manga_id)});
    const auto l3 = tree.fetch(list(3));
    REQUIRE(l3.has_value());
    CHECK(l3->titles.empty());
    CHECK(l3->name == "Empty");
    const auto l4 = tree.fetch(list(4));
    REQUIRE(l4.has_value());
    CHECK(l4->titles == std::vector{manga(clean_id), manga(clean_id)});
}

TEST_CASE("mdlist fixture: an unknown list is not_found (HTTP 404)", "[mdlist][mdx_fixture]") {
    Tree tree{mdx::clean_fixture()};
    const auto unknown = tree.fetch(list(mdx::unknown_list_n));
    REQUIRE(!unknown.has_value());
    CHECK(unknown.error().kind == md::ListFailureKind::request);
    CHECK(unknown.error().error.code == md::Errc::not_found);
    CHECK(unknown.error().error.message ==
          "list a0000000-0000-4000-8000-0000000000ff not found on MangaDex (HTTP 404): it does not "
          "exist or is private; check the id, make the list public on MangaDex, or use "
          "'mangadex-dl download' if it is a title");
    // L8 lives in the nochapters tree only.
    const auto elsewhere = tree.fetch(list(8));
    REQUIRE(!elsewhere.has_value());
    CHECK(elsewhere.error().error.code == md::Errc::not_found);
}

TEST_CASE("mdlist fixture: L5 and L6 are content errors", "[mdlist][mdx_fixture]") {
    Tree tree{mdx::clean_fixture()};
    for (const std::uint64_t n : {std::uint64_t{5}, std::uint64_t{6}}) {
        INFO("L" << n);
        const auto bad = tree.fetch(list(n));
        REQUIRE(!bad.has_value());
        CHECK(bad.error().kind == md::ListFailureKind::content);
        CHECK(bad.error().error.code == md::Errc::network);
        CHECK(bad.error().error.message.starts_with(std::format(
            "list {}: malformed MangaDex API response (custom list): ", mdx::list_id(n))));
    }
}

TEST_CASE("mdlist fixture: L8, L9 and L10 come from their own trees", "[mdlist][mdx_fixture]") {
    Tree nochapters{mdx::nochapters_fixture()};
    const auto l8 = nochapters.fetch(list(8));
    REQUIRE(l8.has_value());
    CHECK(l8->titles ==
          std::vector{manga("55555555-5555-4555-8555-555555555555"), manga(mdx::unknown_manga_id)});
    CHECK(!l8->name.has_value());

    Tree incomplete{mdx::incomplete_fixture()};
    const auto l9 = incomplete.fetch(list(9));
    REQUIRE(l9.has_value());
    CHECK(l9->titles == std::vector{manga("44444444-4444-4444-8444-444444444444")});

    Tree bulk{mdx::bulk_fixture()};
    const auto l10 = bulk.fetch(list(10));
    REQUIRE(l10.has_value());
    CHECK(l10->titles == std::vector{manga("33333333-3333-4333-8333-333333333333")});
}

} // namespace
