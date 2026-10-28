// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/list_hint_download_test.cpp: the `download <list URL>` exit-2 hint through
// run(args, Io&) (list spec "Command surface", AC15 download part; list plan T4). A list URL as
// MANGA names mdlist; every other bad MANGA keeps today's text. Nothing is created and no request
// is sent: the fixture root never exists, so a missed validation would fail with exit 1 instead.
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec).

#include "support/io_capture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace {

namespace md = mangadex_dl;
using md::test::Captured;
using md::test::run_cli;

constexpr std::string_view list_uuid = "a1b2c3d4-e5f6-4789-8abc-def012345678";
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-cli-fixture";
constexpr std::string_view download_hint =
    "Run 'mangadex-dl download --help' for more information.\n";

[[nodiscard]] std::string list_url(std::string_view suffix) {
    std::string url{"https://mangadex.org/list/"};
    url += list_uuid;
    url += suffix;
    return url;
}

} // namespace

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

TEST_CASE("list hint: download with a list URL exits 2 and names mdlist",
          "[cli][download][list_hint]") {
    const std::string manga = GENERATE(list_url("/x"), list_url("/"), list_url(""));
    const bool json = GENERATE(false, true);
    CAPTURE(manga, json);
    auto dir = md::test::TempDir::create();
    REQUIRE(dir.has_value());
    const std::filesystem::path out = dir->path() / "out";
    const Captured r = json ? run_cli({"--json", "download", "--api-base",
                                       std::string{missing_api_base}, manga, out.string()})
                            : run_cli({"download", "--api-base", std::string{missing_api_base},
                                       manga, out.string()});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == std::string{"mangadex-dl: error: MANGA is a MangaDex list, not a title; use "
                               "'mangadex-dl mdlist <list>' to download every title in it\n"} +
                       std::string{download_hint});
    CHECK(!r.err.contains(list_uuid)); // MANGA is never echoed
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("list hint: download with any other bad MANGA keeps today's text",
          "[cli][download][list_hint]") {
    const std::string manga = GENERATE(std::string{"not-a-manga"}, list_url("/a/b"),
                                       std::string{"https://mangadex.org/list//"});
    CAPTURE(manga);
    const Captured r = run_cli({"download", "--api-base", std::string{missing_api_base}, manga});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == std::string{"mangadex-dl: error: MANGA must be a MangaDex title UUID or a "
                               "https://mangadex.org/title/<uuid> URL\n"} +
                       std::string{download_hint});
}
