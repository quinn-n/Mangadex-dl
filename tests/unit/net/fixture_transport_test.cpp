// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/fixture_transport_test.cpp: the file:// fixture transport. The URL mapping
// table (api offsets, .json suffixes, verbatim uploads and at-home paths), 404s for missing
// files, directories, FIFOs and symlink components, rejected URLs (`.`, `..`, empty segments,
// outside the root), the body-size cap, cancellation, concurrent use, and a real fixture tree
// written by tests/support/mdx_fixture.hpp.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace mdx = mangadex_dl::test::mdx;
using mangadex_dl::HttpMethod;
using mangadex_dl::HttpRequest;
using mangadex_dl::make_fixture_transport;
using mangadex_dl::Transport;
using mangadex_dl::TransportErrc;
using mangadex_dl::TransportResult;
using mangadex_dl::test::TempDir;

constexpr std::string_view manga_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view chapter_id = "11111111-0001-4000-8000-000000000001";
constexpr std::string_view hash = "1111111100000000000000000000000000000001";
constexpr std::size_t generous = std::size_t{1} << 20U;

void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out.good());
}

/// A temp dir holding a hand-made fixture tree under `<dir>/root`, plus a file outside it.
struct Tree {
    std::optional<TempDir> dir{}; ///< always engaged; optional only for the member initializer
    fs::path root{};
    std::unique_ptr<Transport> transport{};

    [[nodiscard]] std::string url(std::string_view rel) const {
        return std::format("file://{}/{}", root.string(), rel);
    }
};

[[nodiscard]] Tree make_tree() {
    auto dir = TempDir::create("mangadex-dl-fixture-transport");
    REQUIRE(dir.has_value());
    const fs::path root = fs::absolute(dir->path()) / "root";
    const std::string id{manga_id};
    const std::string ch{chapter_id};
    const std::string h{hash};
    write(root / "api" / "manga" / (id + ".json"), "manga");
    write(root / "api" / "manga" / id / "feed" / "offset-0.json", "feed0");
    write(root / "api" / "manga" / id / "feed" / "offset-500.json", "feed500");
    write(root / "api" / "cover" / "offset-0.json", "cover0");
    write(root / "api" / "at-home" / "server" / (ch + ".json"), "athome");
    write(root / "uploads" / "covers" / id / "cover-v1.png", "cover-bytes");
    write(root / "at-home" / "data" / h / "p001.png", "page-1");
    write(root / "at-home" / "data-saver" / h / "s001.png", "saver-1");
    write(root / "empty.bin", "");
    write(dir->path() / "outside" / "secret.json", "secret");

    auto transport = make_fixture_transport(root);
    REQUIRE(transport.has_value());
    REQUIRE(*transport != nullptr);
    return Tree{.dir = std::move(*dir), .root = root, .transport = std::move(*transport)};
}

[[nodiscard]] TransportResult get(Transport& transport, const std::string& url,
                                  std::size_t max_body_bytes = generous) {
    return transport.fetch(HttpRequest{.url = url, .max_body_bytes = max_body_bytes});
}

/// The body of a 200 response; fails the test otherwise.
[[nodiscard]] std::string body_of(const TransportResult& result) {
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    return result->body;
}

[[nodiscard]] int status_of(const TransportResult& result) {
    REQUIRE(result.has_value());
    return result->status;
}

[[nodiscard]] TransportErrc error_of(const TransportResult& result) {
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().message.empty());
    return result.error().kind;
}

} // namespace

TEST_CASE("fixture transport: the URL mapping table", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    Transport& t = *tree.transport;
    const std::string id{manga_id};
    const std::string ch{chapter_id};
    const std::string h{hash};

    // api/<path> -> api/<path>.json; the query is ignored.
    CHECK(body_of(get(t, tree.url("api/manga/" + id))) == "manga");
    CHECK(body_of(get(t, tree.url("api/manga/" + id +
                                  "?includes[]=author&includes[]=artist&includes[]=cover_art"))) ==
          "manga");
    CHECK(body_of(get(t, tree.url("api/at-home/server/" + ch))) == "athome");
    CHECK(body_of(get(t, tree.url("api/at-home/server/" + ch + "?forcePort443=false"))) ==
          "athome");

    // api/<path>?...offset=<n>... -> api/<path>/offset-<n>.json, wherever offset sits.
    CHECK(body_of(get(t, tree.url("api/manga/" + id +
                                  "/feed?translatedLanguage[]=en&limit=500&offset=0&order[volume]="
                                  "asc&includeUnavailable=1"))) == "feed0");
    CHECK(body_of(get(t, tree.url("api/manga/" + id + "/feed?offset=500"))) == "feed500");
    CHECK(body_of(get(t, tree.url("api/manga/" + id + "/feed?limit=500&offset=500"))) == "feed500");
    CHECK(body_of(get(t, tree.url("api/cover?manga[]=" + id + "&limit=100&offset=0"))) == "cover0");
    // `xoffset` is not `offset`.
    CHECK(status_of(get(t, tree.url("api/cover?xoffset=0"))) == 404);

    // Everything else is verbatim (the query is ignored there too).
    CHECK(body_of(get(t, tree.url("uploads/covers/" + id + "/cover-v1.png"))) == "cover-bytes");
    CHECK(body_of(get(t, tree.url("at-home/data/" + h + "/p001.png"))) == "page-1");
    CHECK(body_of(get(t, tree.url("at-home/data-saver/" + h + "/s001.png?x=1"))) == "saver-1");
    // A top-level "api" file name alone is not an API path.
    CHECK(status_of(get(t, tree.url("api"))) == 404); // a directory
}

TEST_CASE("fixture transport: missing documents are HTTP 404", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    Transport& t = *tree.transport;
    const std::string id{manga_id};
    const std::string h{hash};

    CHECK(status_of(get(t, tree.url("api/manga/66666666-6666-4666-8666-666666666666"))) == 404);
    CHECK(status_of(get(t, tree.url("api/manga/" + id + "/feed?offset=1000"))) == 404);
    CHECK(status_of(get(t, tree.url("api/at-home/server/no-such-chapter"))) == 404);
    CHECK(status_of(get(t, tree.url("at-home/data/" + h + "/p002.png"))) == 404);
    CHECK(status_of(get(t, tree.url("no/such/dir/file"))) == 404);
    // A file used as a directory (ENOTDIR).
    CHECK(status_of(get(t, tree.url("empty.bin/child"))) == 404);
    // A directory is not a document.
    CHECK(status_of(get(t, tree.url("at-home/data"))) == 404);
    // 404 responses carry no body.
    const TransportResult missing = get(t, tree.url("nope"));
    REQUIRE(missing.has_value());
    CHECK(missing->body.empty());
}

TEST_CASE("fixture transport: symlink components are 404, never followed",
          "[net][fixture-transport]") {
    const Tree tree = make_tree();
    Transport& t = *tree.transport;
    const fs::path outside = tree.root.parent_path() / "outside";

    // A symlinked final component, pointing inside and outside the root.
    fs::create_symlink(tree.root / "empty.bin", tree.root / "inside-link.bin");
    fs::create_symlink(outside / "secret.json", tree.root / "outside-link.json");
    // Symlinked intermediate directories (openat with O_DIRECTORY|O_NOFOLLOW: ENOTDIR or ELOOP).
    fs::create_directory_symlink(outside, tree.root / "out");
    fs::create_directory_symlink(tree.root / "at-home", tree.root / "alias");
    fs::create_directory_symlink(outside, tree.root / "api" / "linked");

    CHECK(status_of(get(t, tree.url("inside-link.bin"))) == 404);
    CHECK(status_of(get(t, tree.url("outside-link.json"))) == 404);
    CHECK(status_of(get(t, tree.url("out/secret.json"))) == 404);
    CHECK(status_of(get(t, tree.url(std::format("alias/data/{}/p001.png", hash)))) == 404);
    CHECK(status_of(get(t, tree.url("api/linked/secret"))) == 404); // -> api/linked/secret.json
    // The real paths still work.
    CHECK(body_of(get(t, tree.url(std::format("at-home/data/{}/p001.png", hash)))) == "page-1");
}

TEST_CASE("fixture transport: '.', '..' and empty segments are rejected",
          "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const std::string rel = GENERATE(as<std::string>{}, "../outside/secret.json", "./empty.bin",
                                     "api/../empty.bin", "api/manga/..", "at-home//data", "",
                                     "empty.bin/", "api/", "api/./cover?offset=0", "/empty.bin");
    CAPTURE(rel);
    CHECK(error_of(get(*tree.transport, tree.url(rel))) == TransportErrc::local);
}

TEST_CASE("fixture transport: URLs outside the root are rejected", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    Transport& t = *tree.transport;
    const std::string root = tree.root.string();
    const std::string url = GENERATE_COPY(
        as<std::string>{}, "file:///etc/passwd",
        std::format("file://{}/outside/secret.json", fs::path{root}.parent_path().string()),
        std::format("file://{}x/empty.bin", root), std::format("file://{}", root),
        std::format("https://{}/empty.bin", root), std::format("FILE://{}/empty.bin", root),
        std::format("file:/{}/empty.bin", root), std::format("{}/empty.bin", root),
        std::format("file://{}/empty.bin#frag", root),
        std::format("file://{}/empty.bin", root) + std::string(1, '\0') + "x");
    CAPTURE(url);
    CHECK(error_of(get(t, url)) == TransportErrc::local);
}

TEST_CASE("fixture transport: offset must be a decimal number", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const std::string query =
        GENERATE(as<std::string>{}, "offset=", "offset=abc", "offset=-1", "offset=1e3",
                 "offset=0x10", "offset=%30", "offset=1234567890123456789");
    CAPTURE(query);
    const std::string url = tree.url(std::format("api/manga/{}/feed?{}", manga_id, query));
    CHECK(error_of(get(*tree.transport, url)) == TransportErrc::local);
}

TEST_CASE("fixture transport: the body cap", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    Transport& t = *tree.transport;

    const std::string page = tree.url(std::format("at-home/data/{}/p001.png", hash)); // 6 bytes
    CHECK(body_of(get(t, page, 6)) == "page-1");
    CHECK(error_of(get(t, page, 5)) == TransportErrc::too_large);
    // 0 refuses any non-empty body, but an empty file is fine.
    CHECK(error_of(get(t, page, 0)) == TransportErrc::too_large);
    CHECK(body_of(get(t, tree.url("empty.bin"), 0)).empty());

    // Larger than one read chunk (64 KiB): the cap holds across chunks.
    std::string big(200'000, 'x');
    big.back() = 'y';
    write(tree.root / "big.bin", big);
    CHECK(body_of(get(t, tree.url("big.bin"), big.size())) == big);
    CHECK(error_of(get(t, tree.url("big.bin"), big.size() - 1)) == TransportErrc::too_large);
    CHECK(error_of(get(t, tree.url("big.bin"), 65'536)) == TransportErrc::too_large);
}

TEST_CASE("fixture transport: POST answers 404", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const TransportResult result = tree.transport->fetch(HttpRequest{
        .method = HttpMethod::post,
        .url = tree.url("empty.bin"),
        .body = R"({"url":"x","success":true})",
        .content_type = "application/json",
        .max_body_bytes = generous,
    });
    CHECK(status_of(result) == 404);
}

TEST_CASE("fixture transport: a requested stop is cancelled", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const std::stop_source source;
    source.request_stop();
    const TransportResult result = tree.transport->fetch(HttpRequest{
        .url = tree.url("empty.bin"),
        .max_body_bytes = generous,
        .cancel = source.get_token(),
    });
    CHECK(error_of(result) == TransportErrc::cancelled);
}

TEST_CASE("fixture transport: a FIFO is 404 and never blocks", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const std::string fifo = (tree.root / "pipe").string();
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(status_of(get(*tree.transport, tree.url("pipe"))) == 404);
}

TEST_CASE("fixture transport: an unsearchable directory is a local error, not a 404",
          "[net][fixture-transport]") {
    if (::geteuid() == 0) {
        SKIP("root ignores directory permissions");
    }
    const Tree tree = make_tree();
    const fs::path locked = tree.root / "locked";
    write(locked / "doc.json", "hidden");
    fs::permissions(locked, fs::perms::none);
    // Restores the mode on every path, so TempDir can remove the tree.
    struct Restore {
        fs::path path{};
        Restore(const Restore&) = delete;
        Restore& operator=(const Restore&) = delete;
        Restore(Restore&&) = delete;
        Restore& operator=(Restore&&) = delete;
        explicit Restore(fs::path p) : path(std::move(p)) {}
        ~Restore() {
            std::error_code ec;
            fs::permissions(path, fs::perms::owner_all, ec);
        }
    };
    const Restore restore{locked};

    // O_PATH opens the directory itself; the child lookup then fails with EACCES.
    const TransportResult result = get(*tree.transport, tree.url("locked/doc.json"));
    CHECK(error_of(result) == TransportErrc::local);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.contains("locked/doc.json"));
}

TEST_CASE("fixture transport: unusable roots are Errc::failure", "[net][fixture-transport]") {
    auto dir = TempDir::create("mangadex-dl-fixture-root");
    REQUIRE(dir.has_value());
    const fs::path base = fs::absolute(dir->path());
    write(base / "file", "x");

    const auto relative = make_fixture_transport(fs::path{"relative/root"});
    REQUIRE_FALSE(relative.has_value());
    CHECK(relative.error().code == mangadex_dl::Errc::failure);

    const auto missing = make_fixture_transport(base / "missing");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == mangadex_dl::Errc::failure);
    CHECK(missing.error().message.contains("missing"));

    const auto file = make_fixture_transport(base / "file");
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == mangadex_dl::Errc::failure);
}

TEST_CASE("fixture transport: a trailing '/' on the root is accepted", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const auto transport = make_fixture_transport(fs::path{tree.root.string() + "/"});
    REQUIRE(transport.has_value());
    CHECK(body_of(get(**transport, tree.url("api/cover?offset=0"))) == "cover0");
}

TEST_CASE("fixture transport: concurrent fetches from 4 threads", "[net][fixture-transport]") {
    const Tree tree = make_tree();
    const std::vector<std::string> urls{
        tree.url(std::format("at-home/data/{}/p001.png", hash)),
        tree.url(std::format("api/manga/{}/feed?offset=500", manga_id)),
        tree.url("missing.json"),
        tree.url("../escape"),
    };
    std::vector<int> failures(4, 0);
    {
        std::vector<std::jthread> workers;
        workers.reserve(4);
        for (std::size_t w = 0; w < 4; ++w) {
            // `tree`, `urls` and `failures` outlive the jthreads (joined at the end of this
            // block); each worker writes only its own `failures` slot.
            workers.emplace_back([t = tree.transport.get(), u = &urls, out = &failures[w]] {
                for (int i = 0; i < 50; ++i) {
                    const TransportResult page = get(*t, (*u)[0]);
                    const TransportResult feed = get(*t, (*u)[1]);
                    const TransportResult missing = get(*t, (*u)[2]);
                    const TransportResult escape = get(*t, (*u)[3]);
                    const bool ok = page.has_value() && page->body == "page-1" &&
                                    feed.has_value() && feed->body == "feed500" &&
                                    missing.has_value() && missing->status == 404 &&
                                    !escape.has_value();
                    if (!ok) {
                        ++*out;
                    }
                }
            });
        }
    }
    CHECK(failures == std::vector<int>(4, 0));
}

TEST_CASE("fixture transport: serves a tree written by write_fixture_tree",
          "[net][fixture-transport]") {
    auto dir = TempDir::create("mangadex-dl-fixture-tree");
    REQUIRE(dir.has_value());
    const fs::path root = fs::absolute(dir->path());
    const mdx::FixtureManga manga = mdx::clean_fixture();
    const auto written = mdx::write_fixture_tree(root, manga);
    REQUIRE(written.has_value());
    auto made = make_fixture_transport(root);
    REQUIRE(made.has_value());
    Transport& t = **made;
    const auto url = [&root](const std::string& rel) {
        return std::format("file://{}/{}", root.string(), rel);
    };

    // Endpoints for fixtures: api = file://<root>/api, uploads = file://<root>/uploads, and the
    // at-home baseUrl replaced by file://<root>/at-home.
    const std::string manga_doc =
        body_of(get(t, url("api/manga/" + manga.id + "?includes[]=author&includes[]=artist")));
    CHECK(manga_doc.contains(manga.id));
    const std::string feed = body_of(
        get(t, url("api/manga/" + manga.id + "/feed?translatedLanguage[]=en&limit=500&offset=0")));
    CHECK(feed.contains(manga.chapters.front().id));
    CHECK(status_of(get(t, url("api/manga/" + manga.id + "/feed?offset=500"))) == 404);
    const std::string covers =
        body_of(get(t, url("api/cover?manga[]=" + manga.id + "&limit=100&offset=0")));
    CHECK(covers.contains(manga.covers.front().file));

    const mdx::FixtureChapter& chapter = manga.chapters.front();
    const std::string at_home = body_of(get(t, url("api/at-home/server/" + chapter.id)));
    CHECK(at_home.contains(chapter.hash));
    CHECK(at_home.contains(mdx::at_home_base_url));
    for (std::size_t i = 0; i < chapter.images.size(); ++i) {
        CAPTURE(i);
        const std::string expected = mdx::image_bytes(chapter.images[i]);
        CHECK(body_of(get(t, url(std::format("at-home/data/{}/{}", chapter.hash,
                                             mdx::page_file_name(i, chapter.images[i]))))) ==
              expected);
        CHECK(body_of(get(t, url(std::format("at-home/data-saver/{}/{}", chapter.hash,
                                             mdx::data_saver_file_name(i, chapter.images[i]))))) ==
              expected);
    }
    const std::string cover = body_of(
        get(t, url(std::format("uploads/covers/{}/{}", manga.id, manga.covers.front().file))));
    CHECK(cover == mdx::image_bytes(manga.covers.front().image));
}
