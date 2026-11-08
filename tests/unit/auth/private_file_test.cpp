// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/private_file_test.cpp: read_private_file, the checked reader for the
// credentials file (follows plan T5). Symlinks are followed (the target is what is checked); a
// FIFO never blocks; the mode and owner are checked before a byte is read.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "support/fs_test_util.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
using md::FileProblemKind;
using md::read_private_file;
using md::UniqueFd;
using md::test::fs_util::make_temp_dir;

inline constexpr std::size_t cap = md::max_credentials_file_bytes;
inline constexpr uid_t nobody_uid = 65534;
inline constexpr gid_t nobody_gid = 65534;

/// Writes `content` to a new file at `path` with exactly `mode` (fchmod, so the umask does not
/// apply).
void write_with_mode(const fs::path& path, std::string_view content, mode_t mode) {
    constexpr int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const UniqueFd fd{::open(path.c_str(), flags, 0600)};
    REQUIRE(fd.valid());
    REQUIRE(::write(fd.get(), content.data(), content.size()) ==
            static_cast<ssize_t>(content.size()));
    REQUIRE(::fchmod(fd.get(), mode) == 0);
}

TEST_CASE("private file: a 0600 file owned by the user is read", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    const mode_t mode = GENERATE(mode_t{0600}, mode_t{0400}, mode_t{0700});
    CAPTURE(mode);
    write_with_mode(path, "username = fixture-user\n", mode);

    const auto read = read_private_file(path, cap);
    REQUIRE(read.has_value());
    CHECK(read->reveal() == "username = fixture-user\n");
}

TEST_CASE("private file: group or other permission bits are too open", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    const mode_t mode = GENERATE(mode_t{0640}, mode_t{0604}, mode_t{0644}, mode_t{0620},
                                 mode_t{0602}, mode_t{0610});
    CAPTURE(mode);
    write_with_mode(path, "password = fixture-password-not-a-secret\n", mode);

    const auto read = read_private_file(path, cap);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::too_open);
    CHECK(read.error().mode == mode);
    CHECK_FALSE(read.error().reason.contains("fixture-password"));
    CHECK_FALSE(read.error().reason.contains(path.string()));
}

TEST_CASE("private file: a FIFO is not a regular file and never blocks", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    REQUIRE(::mkfifo(path.c_str(), 0600) == 0);

    const auto read = read_private_file(path, cap);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::not_regular);
    CHECK(read.error().reason == "it is not a regular file");
}

TEST_CASE("private file: a directory is not a regular file", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const auto read = read_private_file(dir.path(), cap);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::not_regular);
}

TEST_CASE("private file: a symlink to a private file is followed", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "real-credentials";
    write_with_mode(target, "client_id = fixture-client\n", 0600);
    fs::create_symlink(target, dir.path() / "credentials");

    const auto read = read_private_file(dir.path() / "credentials", cap);
    REQUIRE(read.has_value());
    CHECK(read->reveal() == "client_id = fixture-client\n");

    // The target is what is checked: a link to an open file is too open.
    REQUIRE(::chmod(target.c_str(), 0644) == 0);
    const auto open = read_private_file(dir.path() / "credentials", cap);
    REQUIRE_FALSE(open.has_value());
    CHECK(open.error().kind == FileProblemKind::too_open);
}

TEST_CASE("private file: a missing file", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();

    const auto missing = read_private_file(dir.path() / "credentials", cap);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind == FileProblemKind::missing);
    CHECK(missing.error().reason == "it does not exist");

    fs::create_symlink(dir.path() / "nowhere", dir.path() / "dangling");
    const auto dangling = read_private_file(dir.path() / "dangling", cap);
    REQUIRE_FALSE(dangling.has_value());
    CHECK(dangling.error().kind == FileProblemKind::missing);
}

TEST_CASE("private file: the size cap", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";

    SECTION("exactly 16 KiB is read") {
        write_with_mode(path, std::string(cap, 'x'), 0600);
        const auto read = read_private_file(path, cap);
        REQUIRE(read.has_value());
        CHECK(read->size() == cap);
    }
    SECTION("16 KiB + 1 is too large") {
        write_with_mode(path, std::string(cap + 1, 'x'), 0600);
        const auto read = read_private_file(path, cap);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileProblemKind::too_large);
        CHECK(read.error().reason == "it is larger than 16384 bytes");
    }
    SECTION("an empty file is read as empty") {
        write_with_mode(path, "", 0600);
        const auto read = read_private_file(path, cap);
        REQUIRE(read.has_value());
        CHECK(read->empty());
    }
    SECTION("a cap of 0 refuses any byte") {
        write_with_mode(path, "x", 0600);
        const auto read = read_private_file(path, 0);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileProblemKind::too_large);
    }
    SECTION("a file larger than one read chunk, under a larger cap") {
        const std::string content(std::size_t{10000}, 'y');
        write_with_mode(path, content, 0600);
        const auto read = read_private_file(path, cap);
        REQUIRE(read.has_value());
        CHECK(read->reveal() == content);
    }
}

TEST_CASE("private file: a file owned by another user is refused", "[auth][private-file]") {
    const md::test::TempDir dir = make_temp_dir();
    fs::path path{};
    if (::geteuid() == 0) {
        path = dir.path() / "credentials";
        write_with_mode(path, "x", 0600);
        REQUIRE(::chown(path.c_str(), nobody_uid, nobody_gid) == 0);
    } else {
        // A system file owned by root: the owner check comes before the mode check.
        const std::array<fs::path, 3> candidates{"/etc/passwd", "/etc/hostname", "/etc/os-release"};
        for (const fs::path& candidate : candidates) {
            struct stat st{};
            if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                st.st_uid != ::geteuid()) {
                path = candidate;
                break;
            }
        }
        if (path.empty()) {
            SKIP("not root, and no regular system file owned by another user was found");
        }
    }
    const auto read = read_private_file(path, cap);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::not_owned);
    CHECK(read.error().reason == "it is owned by another user");
}

} // namespace
