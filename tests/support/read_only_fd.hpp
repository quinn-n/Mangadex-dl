#pragma once
// tests/support/read_only_fd.hpp: open a test file read-only as a UniqueFd. Header-only and
// Catch2-aware: open_read_only REQUIREs the open to succeed, so call it only inside a running
// TEST_CASE.
//
// Kept apart from fs_test_util.hpp on purpose: it needs <fcntl.h>, and glibc's <fcntl.h> declares
// `struct stat`, so every test that includes it (download_cli_harness.hpp includes
// fs_test_util.hpp) would have to include <fcntl.h> itself for misc-include-cleaner.
//
//     const mangadex_dl::UniqueFd fd = mangadex_dl::test::open_read_only(path);
//     const mangadex_dl::UniqueFd dir = mangadex_dl::test::open_read_only(root, O_DIRECTORY);

#include "mangadex_dl/fs/output_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <fcntl.h>

namespace mangadex_dl::test {

/// open(2) of `path` read-only and close-on-exec, plus `extra_flags` (O_DIRECTORY, ...); the open
/// is required to succeed. The one variadic call of the test support headers.
[[nodiscard]] inline UniqueFd open_read_only(const std::filesystem::path& path,
                                             int extra_flags = 0) {
    const std::string name = path.string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | extra_flags);
    REQUIRE(fd >= 0);
    return UniqueFd{fd};
}

} // namespace mangadex_dl::test
