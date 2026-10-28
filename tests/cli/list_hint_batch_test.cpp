// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/list_hint_batch_test.cpp: the `batch <list URL>` exit-2 hint through run(args, Io&)
// (list spec "Command surface", AC15 batch part; list plan T8). A list URL as FILE names mdlist
// (it used to be exit 1 "cannot read"); a bare UUID keeps today's "looks like a MangaDex title"
// message. Nothing is created and no request is sent: the fixture root never exists, so a missed
// validation would fail with exit 1 instead.
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
constexpr std::string_view batch_hint = "Run 'mangadex-dl batch --help' for more information.\n";

[[nodiscard]] std::string list_url(std::string_view suffix) {
    std::string url{"https://mangadex.org/list/"};
    url += list_uuid;
    url += suffix;
    return url;
}

} // namespace

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

TEST_CASE("list hint: batch with a list URL exits 2 and names mdlist", "[cli][batch][list_hint]") {
    const std::string file = GENERATE(list_url(""), list_url("/"), list_url("/My-List"));
    const bool json = GENERATE(false, true);
    CAPTURE(file, json);
    auto dir = md::test::TempDir::create();
    REQUIRE(dir.has_value());
    const std::filesystem::path out = dir->path() / "out";
    const Captured r =
        json ? run_cli({"--json", "batch", "--api-base", std::string{missing_api_base}, file,
                        out.string()})
             : run_cli({"batch", "--api-base", std::string{missing_api_base}, file, out.string()});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == std::string{"mangadex-dl: error: FILE looks like a MangaDex list; use "
                               "'mangadex-dl mdlist <list>'\n"} +
                       std::string{batch_hint});
    CHECK(!r.err.contains(list_uuid)); // FILE is never echoed
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("list hint: batch with a bare UUID keeps the title message", "[cli][batch][list_hint]") {
    auto dir = md::test::TempDir::create();
    REQUIRE(dir.has_value());
    const std::filesystem::path out = dir->path() / "out";
    const Captured r = run_cli({"batch", "--api-base", std::string{missing_api_base},
                                std::string{list_uuid}, out.string()});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == std::string{"mangadex-dl: error: FILE looks like a MangaDex title; use "
                               "'mangadex-dl download <title>' for one title\n"} +
                       std::string{batch_hint});
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("list hint: batch with a missing file that is not a list URL still exits 1",
          "[cli][batch][list_hint]") {
    // `/list/<uuid>/a/b` is not a list form: it is an ordinary (missing) path, read and failed.
    auto dir = md::test::TempDir::create();
    REQUIRE(dir.has_value());
    const std::filesystem::path out = dir->path() / "out";
    const Captured r = run_cli(
        {"batch", "--api-base", std::string{missing_api_base}, list_url("/a/b"), out.string()});
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    CHECK(!r.err.contains("MangaDex list"));
    CHECK(!r.err.contains(batch_hint));
    CHECK(!std::filesystem::exists(out));
}
