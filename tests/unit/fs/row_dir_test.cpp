// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/fs/row_dir_test.cpp: the read-only row-directory pre-flight (batch plan T2, rev1).
// classify_dir_errno is a pure table, so its EACCES/EPERM rows need no chmod-based test; one
// chmod-based case (skipped as root, which ignores permissions) pins the faccessat message.
// check_row_dir runs against real trees in a TempDir, and every case also proves that nothing
// was created or changed.
#include "mangadex_dl/fs/row_dir.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using mangadex_dl::check_row_dir;
using mangadex_dl::classify_dir_errno;
using mangadex_dl::RowDirKind;
using mangadex_dl::RowDirVerdict;
using mangadex_dl::test::TempDir;

// NOLINTNEXTLINE(misc-include-cleaner): EDQUOT is defined only in the private <asm-generic/errno.h>
constexpr int edquot = EDQUOT;

[[nodiscard]] TempDir make_temp_dir() {
    auto dir = TempDir::create("mangadex-dl-rowdir");
    REQUIRE(dir.has_value());
    return std::move(*dir);
}

void write_file(const fs::path& path) {
    std::ofstream out{path, std::ios::binary};
    out << "x";
    out.close();
    REQUIRE(out.good());
}

/// Appends every entry below `dir` as "<type> <path relative to root>". Recurses only into real
/// directories (symlink_status), so a symlink loop or a symlink out of the tree is listed, never
/// followed.
void collect(const fs::path& root, const fs::path& dir, std::vector<std::string>& entries) {
    for (const fs::directory_entry& entry : fs::directory_iterator{dir}) {
        const fs::file_type type = entry.symlink_status().type();
        entries.push_back(std::to_string(static_cast<int>(type)) + " " +
                          entry.path().lexically_relative(root).generic_string());
        if (type == fs::file_type::directory) {
            collect(root, entry.path(), entries);
        }
    }
}

/// Every entry below `root`, sorted.
[[nodiscard]] std::vector<std::string> snapshot(const fs::path& root) {
    std::vector<std::string> entries{};
    collect(root, root, entries);
    std::ranges::sort(entries);
    return entries;
}

[[nodiscard]] std::string strerror_text(int err) { return std::system_category().message(err); }

/// The message for a directory that cannot be opened (open/openat or the dangling-symlink check).
[[nodiscard]] std::string open_error(const fs::path& path, std::string_view why) {
    return std::format("cannot open directory '{}': {}", path.string(), why);
}

/// Restores owner rwx on `path` when it goes out of scope, so TempDir can remove the tree after
/// a test took the permissions away.
class RestoreOwnerAll {
public:
    explicit RestoreOwnerAll(fs::path path) : path_(std::move(path)) {}
    RestoreOwnerAll(const RestoreOwnerAll&) = delete;
    RestoreOwnerAll& operator=(const RestoreOwnerAll&) = delete;
    RestoreOwnerAll(RestoreOwnerAll&&) = delete;
    RestoreOwnerAll& operator=(RestoreOwnerAll&&) = delete;
    ~RestoreOwnerAll() {
        std::error_code ec;
        fs::permissions(path_, fs::perms::owner_all, ec);
    }

private:
    fs::path path_{};
};

/// check_row_dir, asserting that the tree under `top` is the same before and after.
[[nodiscard]] RowDirVerdict checked(const fs::path& top, const fs::path& root,
                                    std::string_view sub) {
    const std::vector<std::string> before = snapshot(top);
    RowDirVerdict verdict = check_row_dir(root, sub);
    CHECK(snapshot(top) == before);
    return verdict;
}

} // namespace

TEST_CASE("classify_dir_errno: path errors are row-scoped only below DIR", "[row_dir]") {
    const int err = GENERATE(ENOTDIR, ELOOP, ENAMETOOLONG, EACCES, EPERM);
    CAPTURE(err);
    CHECK(classify_dir_errno(err, true) == RowDirKind::row_error);
    CHECK(classify_dir_errno(err, false) == RowDirKind::global_error);
}

TEST_CASE("classify_dir_errno: filesystem and resource errors are always global", "[row_dir]") {
    const int err =
        GENERATE(ENOSPC, EIO, EROFS, edquot, EMFILE, ENFILE, ENOMEM, ENOENT, 0, -1, 999999);
    CAPTURE(err);
    CHECK(classify_dir_errno(err, true) == RowDirKind::global_error);
    CHECK(classify_dir_errno(err, false) == RowDirKind::global_error);
}

TEST_CASE("check_row_dir: DIR itself", "[row_dir]") {
    const TempDir dir = make_temp_dir();
    const fs::path& top = dir.path();

    SECTION("a missing DIR is ok (download creates it), with or without a sub-directory") {
        CHECK(checked(top, top / "out", "") == RowDirVerdict{});
        CHECK(checked(top, top / "out", "a/b") == RowDirVerdict{});
        CHECK(checked(top, top / "missing" / "out", "a") == RowDirVerdict{});
    }
    SECTION("an existing, writable DIR is ok") {
        fs::create_directory(top / "out");
        CHECK(checked(top, top / "out", "") == RowDirVerdict{});
    }
    SECTION("a DIR that is a regular file is a global error") {
        write_file(top / "out");
        const RowDirVerdict verdict = checked(top, top / "out", "");
        CHECK(verdict.kind == RowDirKind::global_error);
        CHECK(verdict.message == open_error(top / "out", strerror_text(ENOTDIR)));

        // The row's `dir` does not make it row-scoped: DIR itself is unusable.
        CHECK(checked(top, top / "out", "a").kind == RowDirKind::global_error);
    }
    SECTION("a DIR reached through a symlink is followed") {
        fs::create_directory(top / "real");
        fs::create_directory_symlink("real", top / "out");
        CHECK(checked(top, top / "out", "") == RowDirVerdict{});
    }
}

TEST_CASE("check_row_dir: the row's sub-directory", "[row_dir]") {
    const TempDir dir = make_temp_dir();
    const fs::path& top = dir.path();
    const fs::path root = top / "out";
    fs::create_directory(root);

    SECTION("a component that is a regular file is a row error") {
        write_file(root / "a");
        const RowDirVerdict verdict = checked(top, root, "a/b");
        CHECK(verdict.kind == RowDirKind::row_error);
        CHECK(verdict.message == open_error(root / "a", strerror_text(ENOTDIR)));
    }
    SECTION("the last component a regular file is a row error too") {
        fs::create_directory(root / "a");
        write_file(root / "a" / "b");
        const RowDirVerdict verdict = checked(top, root, "a/b");
        CHECK(verdict.kind == RowDirKind::row_error);
        CHECK(verdict.message == open_error(root / "a" / "b", strerror_text(ENOTDIR)));
    }
    SECTION("a dangling symlink is a row error") {
        fs::create_symlink("nowhere", root / "a");
        const RowDirVerdict verdict = checked(top, root, "a/b");
        CHECK(verdict.kind == RowDirKind::row_error);
        CHECK(verdict.message == open_error(root / "a", "it is a dangling symbolic link"));
    }
    SECTION("a symlink loop is a row error") {
        fs::create_symlink("a", root / "a");
        const RowDirVerdict verdict = checked(top, root, "a");
        CHECK(verdict.kind == RowDirKind::row_error);
        CHECK(verdict.message == open_error(root / "a", strerror_text(ELOOP)));
    }
    SECTION("a symlink to a directory is followed (OD1)") {
        fs::create_directory(top / "elsewhere");
        fs::create_directory_symlink("../elsewhere", root / "a");
        CHECK(checked(top, root, "a") == RowDirVerdict{});
        CHECK(checked(top, root, "a/b") == RowDirVerdict{});
    }
    SECTION("an existing first component and a missing second one is ok") {
        fs::create_directory(root / "a");
        CHECK(checked(top, root, "a/b") == RowDirVerdict{});
    }
    SECTION("an existing full path is ok") {
        fs::create_directories(root / "a" / "b");
        CHECK(checked(top, root, "a/b") == RowDirVerdict{});
    }
    SECTION("a missing first component is ok") {
        CHECK(checked(top, root, "a/b/c") == RowDirVerdict{});
    }
    SECTION("UTF-8 names are passed through") {
        fs::create_directory(root / "Zo\xC3\xAB");
        CHECK(checked(top, root, "Zo\xC3\xAB/x") == RowDirVerdict{});
    }
}

TEST_CASE("check_row_dir: a directory without write permission", "[row_dir]") {
    if (::geteuid() == 0) {
        SKIP("root ignores directory permissions");
    }
    const TempDir dir = make_temp_dir();
    const fs::path& top = dir.path();
    const fs::path root = top / "out";
    fs::create_directories(root / "a");
    const fs::perms read_only = fs::perms::owner_read | fs::perms::owner_exec;

    SECTION("below DIR it is a row error") {
        const RestoreOwnerAll restore{root / "a"};
        fs::permissions(root / "a", read_only);
        const RowDirVerdict verdict = checked(top, root, "a/b");
        CHECK(verdict.kind == RowDirKind::row_error);
        CHECK(verdict.message == std::format("cannot write to directory '{}': {}",
                                             (root / "a").string(), strerror_text(EACCES)));
    }
    SECTION("DIR itself is a global error") {
        const RestoreOwnerAll restore{root};
        fs::permissions(root, read_only);
        const RowDirVerdict verdict = checked(top, root, "");
        CHECK(verdict.kind == RowDirKind::global_error);
        CHECK(verdict.message == std::format("cannot write to directory '{}': {}", root.string(),
                                             strerror_text(EACCES)));
    }
}
