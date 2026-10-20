// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/fs/output_dir_test.cpp: OutputDir and TempFile (download plan T8). Every publish
// case runs on both temporary paths: O_TMPFILE (the default on tmpfs/ext4) and the named
// `.<final>.part-<hex>` fallback, forced through the private testing seam. The seam also forces
// the rarer fallbacks (no RENAME_NOREPLACE, no /proc).

#include "fs/output_dir_testing.hpp" // core-private seam (src/core is on the test include path)
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "support/fs_test_util.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace testing = mangadex_dl::testing;
using mangadex_dl::Errc;
using mangadex_dl::OutputDir;
using mangadex_dl::PublishOutcome;
using mangadex_dl::test::TempDir;
using mangadex_dl::test::fs_util::list_dir;
using mangadex_dl::test::fs_util::make_temp_dir;
using mangadex_dl::test::fs_util::read_file;
using mangadex_dl::test::fs_util::write_file;

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span{text.data(), text.size()});
}

/// Whether `name` is a named fallback temporary for `final_name`.
[[nodiscard]] bool is_part_name_for(std::string_view name, std::string_view final_name) {
    const std::string prefix = "." + std::string{final_name} + ".part-";
    return name.starts_with(prefix) && name.size() == prefix.size() + 12;
}

/// Opens `dir` in the requested temporary mode (the seam is read once, at open time).
[[nodiscard]] OutputDir open_dir(const fs::path& dir, bool fallback) {
    std::optional<testing::ScopedOutputDirFault> fault{};
    if (fallback) {
        fault.emplace(&testing::force_fallback_tempfiles);
    }
    auto opened = OutputDir::open(dir, false);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// create_temp + write + publish in one go.
[[nodiscard]] mangadex_dl::Result<PublishOutcome>
write_and_publish(OutputDir& out, std::string_view name, std::string_view content, bool overwrite) {
    auto temp = out.create_temp(name);
    REQUIRE(temp.has_value());
    REQUIRE(temp->write(as_bytes(content)).has_value());
    return out.publish(std::move(*temp), name, overwrite);
}

} // namespace

TEST_CASE("output_dir publishes a new file and leaves no temporary", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);
    CHECK(out.path() == dir.path());

    const auto outcome = write_and_publish(out, "Title - Vol. 01.epub", "epub bytes", false);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == PublishOutcome::written);
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"Title - Vol. 01.epub"});
    CHECK(read_file(dir.path() / "Title - Vol. 01.epub") == "epub bytes");

    const fs::file_status status = fs::symlink_status(dir.path() / "Title - Vol. 01.epub");
    CHECK(status.type() == fs::file_type::regular);
    CHECK((status.permissions() & (fs::perms::group_write | fs::perms::others_write)) ==
          fs::perms::none);
}

TEST_CASE("output_dir no-clobber keeps an existing file symlink or directory", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    const int kind = GENERATE(0, 1, 2, 3); // file, symlink, dangling symlink, directory
    CAPTURE(fallback, kind);
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "taken.epub";
    write_file(dir.path() / "other", "other content");
    switch (kind) {
    case 0:
        write_file(target, "old content");
        break;
    case 1:
        fs::create_symlink("other", target);
        break;
    case 2:
        fs::create_symlink("does-not-exist", target);
        break;
    default:
        fs::create_directory(target);
        break;
    }
    OutputDir out = open_dir(dir.path(), fallback);
    const auto exists = out.exists("taken.epub");
    REQUIRE(exists.has_value());
    CHECK(*exists);

    const auto outcome = write_and_publish(out, "taken.epub", "new content", false);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == PublishOutcome::exists);
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"other", "taken.epub"});
    CHECK(read_file(dir.path() / "other") == "other content");
    switch (kind) {
    case 0:
        CHECK(read_file(target) == "old content");
        break;
    case 1:
        CHECK(fs::read_symlink(target) == "other");
        break;
    case 2:
        CHECK(fs::read_symlink(target) == "does-not-exist");
        break;
    default:
        CHECK(fs::is_directory(fs::symlink_status(target)));
        CHECK(fs::is_empty(target));
        break;
    }
}

TEST_CASE("output_dir overwrite replaces the content atomically", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);

    SECTION("an existing file is replaced") {
        write_file(dir.path() / "book.epub", "old content that is longer");
        const auto outcome = write_and_publish(out, "book.epub", "new", true);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == PublishOutcome::replaced);
        CHECK(read_file(dir.path() / "book.epub") == "new");
    }
    SECTION("an existing symlink is replaced, not followed") {
        write_file(dir.path() / "victim", "victim content");
        fs::create_symlink("victim", dir.path() / "book.epub");
        const auto outcome = write_and_publish(out, "book.epub", "new", true);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == PublishOutcome::replaced);
        CHECK(fs::is_regular_file(fs::symlink_status(dir.path() / "book.epub")));
        CHECK(read_file(dir.path() / "book.epub") == "new");
        CHECK(read_file(dir.path() / "victim") == "victim content");
    }
    SECTION("a missing name is written") {
        const auto outcome = write_and_publish(out, "book.epub", "new", true);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == PublishOutcome::written);
        CHECK(read_file(dir.path() / "book.epub") == "new");
    }
    SECTION("a directory is never replaced: PublishOutcome::directory, not an error") {
        fs::create_directory(dir.path() / "book.epub");
        write_file(dir.path() / "book.epub" / "inside", "kept");
        const auto outcome = write_and_publish(out, "book.epub", "new", true);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == PublishOutcome::directory);
        CHECK(fs::is_directory(fs::symlink_status(dir.path() / "book.epub")));
        CHECK(list_dir(dir.path() / "book.epub") == std::vector<std::string>{"inside"});
        CHECK(read_file(dir.path() / "book.epub" / "inside") == "kept");
    }
    const std::vector<std::string> names = list_dir(dir.path());
    CHECK(std::ranges::none_of(names, [](const std::string& n) { return n.contains(".part-"); }));
}

TEST_CASE("output_dir dropping a TempFile discards it on both paths", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);
    {
        auto temp = out.create_temp("dropped.epub");
        REQUIRE(temp.has_value());
        REQUIRE(temp->write(as_bytes("partial data")).has_value());
        REQUIRE(temp->sync_and_close().has_value());
        const std::vector<std::string> names = list_dir(dir.path());
        if (fallback) {
            REQUIRE(names.size() == 1);
            CHECK(is_part_name_for(names.front(), "dropped.epub"));
        } else {
            CHECK(names.empty()); // an O_TMPFILE inode has no name
        }
    }
    CHECK(list_dir(dir.path()).empty());
}

TEST_CASE("output_dir moved-from TempFile owns nothing and move-assign discards", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);

    auto first = out.create_temp("one.epub");
    auto second = out.create_temp("two.epub");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->write(as_bytes("first")).has_value());
    REQUIRE(second->write(as_bytes("second")).has_value());
    if (fallback) {
        CHECK(list_dir(dir.path()).size() == 2);
    }

    *first = std::move(*second); // discards "one.epub"'s temporary
    if (fallback) {
        const std::vector<std::string> names = list_dir(dir.path());
        REQUIRE(names.size() == 1);
        CHECK(is_part_name_for(names.front(), "two.epub"));
    }
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from contract
    const auto refused = second->write(as_bytes("x"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Errc::failure);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from contract
    CHECK_FALSE(second->sync_and_close().has_value());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from contract
    const auto refused_publish = out.publish(std::move(*second), "x.epub", false);
    CHECK_FALSE(refused_publish.has_value());

    const auto published = out.publish(std::move(*first), "two.epub", false);
    REQUIRE(published.has_value());
    CHECK(*published == PublishOutcome::written);
    CHECK(read_file(dir.path() / "two.epub") == "second");
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"two.epub"});
}

TEST_CASE("output_dir buffers large and small writes correctly", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);

    std::string expected{};
    auto temp = out.create_temp("big.epub");
    REQUIRE(temp.has_value());
    // Many small writes cross the 1 MiB buffer boundary; one 2.5 MiB write bypasses it.
    for (int i = 0; i < 300; ++i) {
        const std::string chunk(10'000, static_cast<char>('a' + (i % 26)));
        expected += chunk;
        REQUIRE(temp->write(as_bytes(chunk)).has_value());
    }
    const std::string large((std::size_t{5} << 20U) / 2, 'Z');
    expected += large;
    REQUIRE(temp->write(as_bytes(large)).has_value());
    REQUIRE(temp->write({}).has_value());
    expected += "tail";
    REQUIRE(temp->write(as_bytes("tail")).has_value());
    REQUIRE(temp->sync_and_close().has_value());
    REQUIRE(temp->sync_and_close().has_value()); // idempotent
    CHECK_FALSE(temp->write(as_bytes("late")).has_value());

    const auto outcome = out.publish(std::move(*temp), "big.epub", false);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == PublishOutcome::written);
    CHECK(read_file(dir.path() / "big.epub") == expected);
}

TEST_CASE("output_dir open creates missing directories and open_if_exists never does",
          "[output_dir]") {
    const TempDir dir = make_temp_dir();
    const fs::path nested = dir.path() / "a" / "b" / "c";

    auto missing = OutputDir::open(nested, false);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Errc::failure);
    CHECK(missing.error().message.contains("No such file or directory"));

    auto if_exists = OutputDir::open_if_exists(nested);
    REQUIRE(if_exists.has_value());
    CHECK_FALSE(if_exists->has_value());
    CHECK_FALSE(fs::exists(dir.path() / "a"));

    auto created = OutputDir::open(nested / "", true); // a trailing separator is fine
    REQUIRE(created.has_value());
    CHECK(fs::is_directory(nested));
    CHECK((fs::status(nested).permissions() & (fs::perms::group_write | fs::perms::others_write)) ==
          fs::perms::none);
    const auto outcome = write_and_publish(*created, "x.epub", "x", false);
    REQUIRE(outcome.has_value());
    CHECK(fs::exists(nested / "x.epub"));

    const auto again = OutputDir::open(nested, true); // existing components are fine
    REQUIRE(again.has_value());
    auto existing = OutputDir::open_if_exists(nested);
    REQUIRE(existing.has_value());
    REQUIRE(existing->has_value());
    CHECK((existing->has_value() && (*existing)->path() == nested));

    const auto dotted = OutputDir::open(dir.path() / "a" / "." / ".." / "d", true);
    REQUIRE(dotted.has_value());
    CHECK(fs::is_directory(dir.path() / "d"));
}

TEST_CASE("output_dir rejects a DIR that is a file", "[output_dir]") {
    const TempDir dir = make_temp_dir();
    const fs::path file = dir.path() / "not-a-dir";
    write_file(file, "data");

    for (const bool create : {false, true}) {
        auto opened = OutputDir::open(file, create);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == Errc::failure);
        CHECK(opened.error().message.contains("Not a directory"));
        CHECK(opened.error().message.contains("not-a-dir"));
    }
    auto below = OutputDir::open(file / "sub", true);
    REQUIRE_FALSE(below.has_value());
    CHECK(below.error().code == Errc::failure);

    auto if_exists = OutputDir::open_if_exists(file);
    REQUIRE_FALSE(if_exists.has_value());
    CHECK(if_exists.error().message.contains("Not a directory"));
    CHECK(read_file(file) == "data");
}

TEST_CASE("output_dir rejects a read-only directory", "[output_dir]") {
    if (::geteuid() == 0) {
        SKIP("running as root: permission bits do not restrict root");
    }
    const TempDir dir = make_temp_dir();
    const fs::path ro = dir.path() / "ro";
    fs::create_directory(ro);
    fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec);

    auto opened = OutputDir::open(ro, false);
    auto created = OutputDir::open(ro / "sub", true);
    auto if_exists = OutputDir::open_if_exists(ro);
    fs::permissions(ro, fs::perms::owner_all); // so TempDir can clean up whatever happens below

    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Errc::failure);
    CHECK(opened.error().message.contains("not writable"));
    CHECK(opened.error().message.contains("Permission denied"));
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().message.contains("Permission denied"));
    REQUIRE_FALSE(if_exists.has_value());
    CHECK(if_exists.error().message.contains("not writable"));
    CHECK_FALSE(fs::exists(ro / "sub"));
}

TEST_CASE("output_dir entry_kind tells a directory apart without following symlinks",
          "[output_dir]") {
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), false);
    fs::create_directory(dir.path() / "dir.epub");
    write_file(dir.path() / "file.epub", "x");
    fs::create_directory_symlink("dir.epub", dir.path() / "link-to-dir.epub");
    fs::create_symlink("missing", dir.path() / "dangling.epub");

    const auto kind_of = [&out](std::string_view name) {
        const auto kind = out.entry_kind(name);
        REQUIRE(kind.has_value());
        return *kind;
    };
    CHECK(kind_of("missing.epub") == mangadex_dl::EntryKind::absent);
    CHECK(kind_of("dir.epub") == mangadex_dl::EntryKind::directory);
    CHECK(kind_of("file.epub") == mangadex_dl::EntryKind::other);
    // A symlink is replaced, never followed: even one that points at a directory.
    CHECK(kind_of("link-to-dir.epub") == mangadex_dl::EntryKind::other);
    CHECK(kind_of("dangling.epub") == mangadex_dl::EntryKind::other);

    const auto bad = out.entry_kind("a/b");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Errc::failure);
}

TEST_CASE("output_dir validates names as single path components", "[output_dir]") {
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), false);
    const auto missing = out.exists("missing.epub");
    REQUIRE(missing.has_value());
    CHECK_FALSE(*missing);
    write_file(dir.path() / "present.epub", "x");
    const auto present = out.exists("present.epub");
    REQUIRE(present.has_value());
    CHECK(*present);

    const std::string too_long(256, 'n');
    const std::string with_nul{"a\0b", 3};
    for (const std::string_view bad :
         {std::string_view{""}, std::string_view{"."}, std::string_view{".."},
          std::string_view{"a/b"}, std::string_view{"/abs"}, std::string_view{too_long},
          std::string_view{with_nul}}) {
        CAPTURE(bad);
        const auto exists = out.exists(bad);
        REQUIRE_FALSE(exists.has_value());
        CHECK(exists.error().code == Errc::failure);
        const auto temp = out.create_temp(bad);
        REQUIRE_FALSE(temp.has_value());
        CHECK(temp.error().code == Errc::failure);
    }
}

TEST_CASE("output_dir publish with an invalid name still discards the temporary", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);
    auto temp = out.create_temp("ok.epub");
    REQUIRE(temp.has_value());
    REQUIRE(temp->write(as_bytes("data")).has_value());
    const auto outcome = out.publish(std::move(*temp), "bad/name", false);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(list_dir(dir.path()).empty());
}

TEST_CASE("output_dir shortens long fallback names on a UTF-8 boundary", "[output_dir]") {
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), true);
    // 255 bytes: "x" + 127 x U+00E9 (2 bytes each). The temp name keeps 235 bytes of it, because
    // byte 236 is the second byte of a character.
    std::string final_name = "x";
    for (int i = 0; i < 127; ++i) {
        final_name += "\xC3\xA9";
    }
    REQUIRE(final_name.size() == 255);

    auto temp = out.create_temp(final_name);
    REQUIRE(temp.has_value());
    const std::vector<std::string> names = list_dir(dir.path());
    REQUIRE(names.size() == 1);
    const std::string& part = names.front();
    CHECK(part.size() == 1 + 235 + 6 + 12);
    CHECK(part.starts_with("." + final_name.substr(0, 235) + ".part-"));

    REQUIRE(temp->write(as_bytes("long")).has_value());
    const auto outcome = out.publish(std::move(*temp), final_name, false);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == PublishOutcome::written);
    CHECK(list_dir(dir.path()) == std::vector<std::string>{final_name});
}

TEST_CASE("output_dir falls back to linkat when renameat2 lacks RENAME_NOREPLACE", "[output_dir]") {
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), true);
    const testing::ScopedOutputDirFault no_noreplace{&testing::force_renameat2_unsupported};

    const auto written = write_and_publish(out, "a.epub", "first", false);
    REQUIRE(written.has_value());
    CHECK(*written == PublishOutcome::written);
    CHECK(read_file(dir.path() / "a.epub") == "first");
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"a.epub"});

    // linkat refuses the existing name: EEXIST at the second stage is `exists` too.
    const auto exists = write_and_publish(out, "a.epub", "second", false);
    REQUIRE(exists.has_value());
    CHECK(*exists == PublishOutcome::exists);
    CHECK(read_file(dir.path() / "a.epub") == "first");
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"a.epub"});
}

TEST_CASE("output_dir uses a named temporary when /proc is unavailable", "[output_dir]") {
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), false); // O_TMPFILE mode

    SECTION("create_temp falls back when the /proc magic link does not resolve") {
        const testing::ScopedOutputDirFault no_proc{&testing::force_proc_unavailable};
        auto temp = out.create_temp("p.epub");
        REQUIRE(temp.has_value());
        const std::vector<std::string> names = list_dir(dir.path());
        REQUIRE(names.size() == 1);
        CHECK(is_part_name_for(names.front(), "p.epub"));
        REQUIRE(temp->write(as_bytes("via fallback")).has_value());
        const auto outcome = out.publish(std::move(*temp), "p.epub", false);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == PublishOutcome::written);
        CHECK(read_file(dir.path() / "p.epub") == "via fallback");
        CHECK(list_dir(dir.path()) == std::vector<std::string>{"p.epub"});
    }
    SECTION("publishing an O_TMPFILE temporary fails if /proc vanished meanwhile") {
        const bool overwrite = GENERATE(false, true);
        CAPTURE(overwrite);
        auto temp = out.create_temp("p.epub");
        REQUIRE(temp.has_value());
        CHECK(list_dir(dir.path()).empty());
        REQUIRE(temp->write(as_bytes("lost")).has_value());
        const testing::ScopedOutputDirFault no_proc{&testing::force_proc_unavailable};
        const auto outcome = out.publish(std::move(*temp), "p.epub", overwrite);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().code == Errc::failure);
        CHECK(outcome.error().message.contains("/proc/self/fd is unavailable"));
        CHECK(list_dir(dir.path()).empty());
    }
}

TEST_CASE("output_dir a failed write or sync poisons the temporary on both paths", "[output_dir]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), fallback);
    auto temp = out.create_temp("fail.epub");
    REQUIRE(temp.has_value());

    SECTION("ENOSPC on a direct (unbuffered) write") {
        const testing::ScopedIoError fault{testing::IoStage::write, ENOSPC};
        const std::string large((std::size_t{1} << 20U) + 1, 'L'); // > the buffer: direct
        const auto written = temp->write(as_bytes(large));
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().code == Errc::failure);
        CHECK(written.error().message.contains("No space left on device"));
        CHECK(written.error().message.contains("fail.epub"));
    }
    SECTION("EIO on the buffer flush in sync_and_close") {
        REQUIRE(temp->write(as_bytes("small, buffered")).has_value());
        const testing::ScopedIoError fault{testing::IoStage::write, EIO};
        const auto synced = temp->sync_and_close();
        REQUIRE_FALSE(synced.has_value());
        CHECK(synced.error().message.contains("Input/output error"));
    }
    SECTION("EIO on fsync") {
        REQUIRE(temp->write(as_bytes("data")).has_value());
        const testing::ScopedIoError fault{testing::IoStage::fsync, EIO};
        const auto synced = temp->sync_and_close();
        REQUIRE_FALSE(synced.has_value());
        CHECK(synced.error().message.contains("cannot sync"));
        CHECK(synced.error().message.contains("Input/output error"));
    }
    SECTION("ENOSPC on fsync reached through publish") {
        REQUIRE(temp->write(as_bytes("data")).has_value());
        const testing::ScopedIoError fault{testing::IoStage::fsync, ENOSPC};
        const auto outcome = out.publish(std::move(*temp), "fail.epub", false);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().message.contains("No space left on device"));
        CHECK(list_dir(dir.path()).empty());
        return; // the TempFile is consumed
    }

    // Once failed, the temporary only accepts being discarded.
    CHECK_FALSE(temp->write(as_bytes("more")).has_value());
    CHECK_FALSE(temp->sync_and_close().has_value());
    const auto outcome = out.publish(std::move(*temp), "fail.epub", false);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(list_dir(dir.path()).empty());
}

TEST_CASE("output_dir a failed close of a named temporary is reported and discards it",
          "[output_dir]") {
    // Only the named fallback closes in sync_and_close(); an O_TMPFILE descriptor stays open for
    // the publishing linkat, so the close fault cannot reach it.
    const TempDir dir = make_temp_dir();
    OutputDir out = open_dir(dir.path(), true);
    auto temp = out.create_temp("close.epub");
    REQUIRE(temp.has_value());
    REQUIRE(temp->write(as_bytes("data")).has_value());
    {
        const testing::ScopedIoError fault{testing::IoStage::close, EIO};
        const auto synced = temp->sync_and_close();
        REQUIRE_FALSE(synced.has_value());
        CHECK(synced.error().message.contains("cannot close"));
        CHECK(synced.error().message.contains("Input/output error"));
    }
    CHECK(list_dir(dir.path()).size() == 1); // the named temporary until it is discarded
    CHECK_FALSE(temp->write(as_bytes("more")).has_value());
    CHECK_FALSE(temp->sync_and_close().has_value());
    const auto outcome = out.publish(std::move(*temp), "close.epub", false);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(list_dir(dir.path()).empty());
}
