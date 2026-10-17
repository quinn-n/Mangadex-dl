// tests/cli/cli_test.cpp: tier-2 in-process CLI tests through run(args, Io&).
// Catch2 test names must not start with '-' (CTest passes the name as a test spec, and a leading
// "--" is parsed as an option).
// Each run() call constructs the whole CLI11 app, so a CLI::ConstructionError (duplicate option
// name, bad validator wiring, fallthrough ordering) fails here instead of at a user's first
// invocation.
#include "support/io_capture.hpp"

#include <catch2/catch_test_macros.hpp>

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
    CHECK_FALSE(r.err.empty());
}
