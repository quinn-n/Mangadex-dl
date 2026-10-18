// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/net/ca_bundle_test.cpp: choosing the CA bundle for the libcurl transport.
// select_ca_bundle's decision table with an injected `readable` predicate (libcurl defaults kept,
// unreadable or NULL defaults falling through to the probe list in order, nothing readable),
// ca_defaults_from honouring the struct's age and NULL fields, and the production predicate
// ca_location_readable against real files, directories, FIFOs and missing paths.

#include "mangadex_dl/net/curl_transport.hpp"
#include "net/curl_api.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using mangadex_dl::ca_bundle_candidates;
using mangadex_dl::CaDefaults;
using mangadex_dl::select_ca_bundle;
using mangadex_dl::detail::ca_defaults_from;
using mangadex_dl::detail::ca_location_readable;
using mangadex_dl::test::TempDir;

/// A `readable` predicate over a fixed set of paths that records every path it was asked about.
struct FakeFs {
    std::set<std::string> readable{};
    std::vector<std::string> asked{};

    [[nodiscard]] std::optional<std::string> select(const CaDefaults& defaults) {
        return select_ca_bundle(defaults, [this](const std::string& path) {
            asked.push_back(path);
            return readable.contains(path);
        });
    }
};

[[nodiscard]] std::string candidate(std::size_t index) {
    return std::string{ca_bundle_candidates.at(index)};
}

} // namespace

TEST_CASE("ca bundle: a readable libcurl cainfo is kept (CAINFO left unset)", "[net][ca]") {
    FakeFs fs{.readable = {"/opt/curl/cacert.pem", candidate(0)}};
    const auto chosen = fs.select(CaDefaults{.cainfo = "/opt/curl/cacert.pem"});
    CHECK(chosen == std::nullopt);
    CHECK(fs.asked == std::vector<std::string>{"/opt/curl/cacert.pem"});
}

TEST_CASE("ca bundle: an unreadable cainfo falls through to the probe list, even with a capath",
          "[net][ca]") {
    // A configured cainfo that cannot be loaded fails every handshake, capath or not.
    FakeFs fs{.readable = {"/etc/ssl/certs", candidate(2), candidate(4)}};
    const auto chosen =
        fs.select(CaDefaults{.cainfo = "/missing/ca.pem", .capath = "/etc/ssl/certs"});
    CHECK(chosen == candidate(2));
    CHECK(fs.asked ==
          std::vector<std::string>{"/missing/ca.pem", candidate(0), candidate(1), candidate(2)});
}

TEST_CASE("ca bundle: a readable capath alone is kept when libcurl has no cainfo", "[net][ca]") {
    FakeFs fs{.readable = {"/etc/ssl/certs", candidate(0)}};
    const auto chosen = fs.select(CaDefaults{.capath = "/etc/ssl/certs"});
    CHECK(chosen == std::nullopt);
    CHECK(fs.asked == std::vector<std::string>{"/etc/ssl/certs"});
}

TEST_CASE("ca bundle: an unreadable capath without cainfo falls through", "[net][ca]") {
    FakeFs fs{.readable = {candidate(5)}};
    const auto chosen = fs.select(CaDefaults{.capath = "/missing/certs"});
    CHECK(chosen == candidate(5));
    REQUIRE(fs.asked.size() == 1 + ca_bundle_candidates.size());
    CHECK(fs.asked.front() == "/missing/certs");
}

TEST_CASE("ca bundle: NULL defaults probe the candidates in order", "[net][ca]") {
    SECTION("every candidate readable: the first wins") {
        FakeFs fs{};
        for (const auto path : ca_bundle_candidates) {
            fs.readable.emplace(path);
        }
        CHECK(fs.select(CaDefaults{}) == candidate(0));
        CHECK(fs.asked == std::vector<std::string>{candidate(0)});
    }
    SECTION("only the last readable") {
        FakeFs fs{.readable = {candidate(ca_bundle_candidates.size() - 1)}};
        CHECK(fs.select(CaDefaults{}) == candidate(ca_bundle_candidates.size() - 1));
        std::vector<std::string> expected{};
        expected.reserve(ca_bundle_candidates.size());
        for (const auto path : ca_bundle_candidates) {
            expected.emplace_back(path);
        }
        CHECK(fs.asked == expected);
    }
}

TEST_CASE("ca bundle: nothing readable leaves CAINFO unset", "[net][ca]") {
    FakeFs fs{};
    CHECK(fs.select(CaDefaults{.cainfo = "/a.pem", .capath = "/certs"}) == std::nullopt);
    CHECK(fs.asked.size() == 1 + ca_bundle_candidates.size());
    CHECK(fs.select(CaDefaults{}) == std::nullopt);
}

TEST_CASE("ca bundle: an empty predicate selects nothing", "[net][ca]") {
    CHECK(select_ca_bundle(CaDefaults{.cainfo = "/a.pem"}, {}) == std::nullopt);
}

TEST_CASE("ca bundle: ca_defaults_from honours age and NULL fields", "[net][ca]") {
    curl_version_info_data info{};
    const std::string cainfo = "/etc/ssl/certs/ca-certificates.crt";
    const std::string capath = "/etc/ssl/certs";
    info.cainfo = cainfo.c_str();
    info.capath = capath.c_str();

    SECTION("older than CURLVERSION_SEVENTH: the fields are not part of the struct") {
        info.age = CURLVERSION_SIXTH;
        CHECK(ca_defaults_from(info) == CaDefaults{});
    }
    SECTION("both present") {
        info.age = CURLVERSION_SEVENTH;
        CHECK(ca_defaults_from(info) == CaDefaults{.cainfo = cainfo, .capath = capath});
    }
    SECTION("NULL capath (conda's libcurl)") {
        info.age = CURLVERSION_TENTH;
        info.capath = nullptr;
        CHECK(ca_defaults_from(info) == CaDefaults{.cainfo = cainfo});
    }
    SECTION("both NULL") {
        info.age = CURLVERSION_TENTH;
        info.cainfo = nullptr;
        info.capath = nullptr;
        CHECK(ca_defaults_from(info) == CaDefaults{});
    }
}

TEST_CASE("ca bundle: ca_location_readable accepts regular files, and directories on request",
          "[net][ca]") {
    auto dir = TempDir::create("mangadex-dl-ca");
    REQUIRE(dir.has_value());
    const std::filesystem::path root = dir->path();
    const std::string file = (root / "bundle.pem").string();
    {
        std::ofstream out{file};
        out << "-----BEGIN CERTIFICATE-----\n";
    }
    const std::string fifo = (root / "fifo.pem").string();
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    const std::string link = (root / "link.pem").string();
    std::filesystem::create_symlink(file, link);

    CHECK(ca_location_readable(file, false));
    CHECK(ca_location_readable(file, true));
    CHECK(ca_location_readable(link, false)); // a distro bundle is often a symlink
    CHECK(!ca_location_readable(root.string(), false));
    CHECK(ca_location_readable(root.string(), true));
    CHECK(!ca_location_readable(fifo, false)); // and opening it never blocks
    CHECK(!ca_location_readable((root / "missing.pem").string(), false));
    CHECK(!ca_location_readable("", false));
    CHECK(!ca_location_readable(std::string{"/etc\0x", 6}, false));

    if (::geteuid() != 0) { // root reads anything
        const std::string locked = (root / "locked.pem").string();
        {
            std::ofstream out{locked};
            out << "x";
        }
        REQUIRE(::chmod(locked.c_str(), 0) == 0);
        CHECK(!ca_location_readable(locked, false));
    }
}
