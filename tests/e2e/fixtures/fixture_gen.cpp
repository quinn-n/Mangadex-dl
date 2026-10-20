// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/e2e/fixtures/fixture_gen.cpp: mangadex_dl_fixture_gen <out-dir>
//
// The body of the e2e.fixtures CTest fixture (FIXTURES_SETUP e2e_fixtures). Writes one
// fixture-mode tree <out-dir>/<name> per entry of mdx::all_fixtures() in
// tests/support/mdx_fixture.hpp (twelve: clean, basic, bulk, incomplete, nochapters and the seven
// clean-* variants), replacing any previous copy of each tree, so E2E cases can run
// `mangadex-dl download --api-base file://{{FIXTURES}}/<name> ...`.
// Trees and layout: tests/e2e/inputs/README.
//
// Test-only: never installed. Exit 0 on success, 1 on any write failure, 2 on bad usage.

#include "support/mdx_fixture.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <system_error>

namespace {

[[nodiscard]] int generate(const std::filesystem::path& out) {
    namespace mdx = mangadex_dl::test::mdx;
    std::size_t written = 0;
    for (const mdx::NamedFixture& fixture : mdx::all_fixtures()) {
        const std::filesystem::path tree = out / fixture.name;
        std::error_code ec;
        // Replace the whole tree: a stale file from an older generator must not linger. This is not
        // atomic and does not need to be: scripts/preset.sh holds the build-dir lock for the whole
        // ctest run, so no other run's E2E case can read <build>/e2e-fixtures meanwhile.
        static_cast<void>(std::filesystem::remove_all(tree, ec));
        if (ec) {
            std::println(stderr, "fixture_gen: {}: {}", tree.string(), ec.message());
            return 1;
        }
        if (const auto ok = mdx::write_fixture_tree(tree, fixture.make()); !ok) {
            std::println(stderr, "fixture_gen: {}", ok.error());
            return 1;
        }
        if (!std::filesystem::is_directory(tree / "api" / "manga", ec)) {
            std::println(stderr, "fixture_gen: {}: tree not written", tree.string());
            return 1;
        }
        ++written;
    }
    std::println("fixture_gen: wrote {} fixture trees under {}", written, out.string());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // The handlers write with fputs, which cannot throw, so no exception escapes main.
    try {
        const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
        if (args.size() != 2 || std::string{args[1]}.empty()) {
            std::println(stderr, "usage: mangadex_dl_fixture_gen <out-dir>");
            return 2;
        }
        return generate(std::filesystem::path{std::string{args[1]}});
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs("fixture_gen: ", stderr));
        static_cast<void>(std::fputs(e.what(), stderr));
        static_cast<void>(std::fputs("\n", stderr));
        return 1;
    } catch (...) {
        static_cast<void>(std::fputs("fixture_gen: unknown exception\n", stderr));
        return 1;
    }
}
