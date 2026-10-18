// tests/cli/cli_test.cpp: tier-2 in-process CLI tests through run(args, Io&).
// Catch2 test names must not start with '-' (CTest passes the name as a test spec, and a leading
// "--" is parsed as an option).
// Each run() call constructs the whole CLI11 app, so a CLI::ConstructionError (duplicate option
// name, bad validator wiring, fallthrough ordering) fails here instead of at a user's first
// invocation.
#include "support/io_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>

using mangadex_dl::test::run_cli;

TEST_CASE("the whole app constructs and --help goes to stdout with exit 0", "[cli][help]") {
    const auto r = run_cli({"--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.out.contains("mangadex-dl"));
    CHECK(r.err.empty());
}

TEST_CASE("version prints 'mangadex-dl X.Y.Z' first, on stdout, with exit 0", "[cli][version]") {
    const auto r = run_cli({"--version"});
    CHECK(r.exit_code == 0);
    CHECK(r.out.starts_with("mangadex-dl "));
    CHECK(r.err.empty());
}

TEST_CASE("an unknown option is a usage error: exit 2, nothing on stdout", "[cli][usage]") {
    const auto r = run_cli({"--definitely-not-an-option"});
    CHECK(r.exit_code == 2); // never a raw CLI11 code (100-114)
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: unknown option '--definitely-not-an-option'\n"
                   "Run 'mangadex-dl --help' for more information.\n");
}

TEST_CASE("the root help lists the download subcommand and no scaffold example", "[cli][help]") {
    const auto r = run_cli({"--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.out.contains("SUBCOMMANDS:"));
    CHECK(r.out.contains("\n  download "));
    CHECK(!r.out.contains("hello"));
    CHECK(r.err.empty());
}

TEST_CASE("the root help footer documents every exit code, 3-6 included", "[cli][help]") {
    const auto r = run_cli({"--help"});
    REQUIRE(r.exit_code == 0);
    CHECK(r.out.contains("Exit status:\n"));
    // One row per code, in exit_codes.hpp order: "  <code padded to 3> <meaning>".
    for (const int code : {0, 1, 2, 3, 4, 5, 6, 70}) {
        INFO("exit code " << code);
        CHECK(r.out.contains(std::format("\n  {:<3} ", code)));
    }
    CHECK(r.out.contains("network"));
    CHECK(r.out.contains("not found"));
    CHECK(r.out.contains("no downloadable chapters"));
    CHECK(r.out.contains("incomplete"));
    CHECK(r.err.empty());
}

TEST_CASE("download --help exits 0 and documents the public options only",
          "[cli][help][download]") {
    const auto r = run_cli({"download", "--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.out.contains("--no-report"));
    CHECK(r.out.contains("--dry-run"));
    CHECK(!r.out.contains("--api-base")); // hidden: a test and loopback seam, not a user option
    CHECK(r.err.empty());
}

TEST_CASE("an unknown subcommand is a usage error: exit 2, nothing on stdout", "[cli][usage]") {
    const auto r = run_cli({"hello"});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: unknown command 'hello' (the only command is "
                   "'download')\n"
                   "Run 'mangadex-dl --help' for more information.\n");
}
