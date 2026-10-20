// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/fs/output_dir_existing_test.cpp: OutputDir::open_existing and
// OutputDir::publish_replacing (skip-existing plan T6).
//
// open_existing runs on every file type, first through the fstatat pre-check, then with the
// pre-check skipped through the private testing seam, so the openat errno mapping (ELOOP, ENXIO)
// and the post-open fstat re-check are asserted exactly. Every publish_replacing case runs on both
// temporary paths (O_TMPFILE and the forced named fallback) and keeps the inspection descriptor
// open until publish_replacing returns, as the V5 contract requires. A driverless char device
// (ENXIO, fact-check V11) needs mknod privileges and is not created here; overlayfs is untested.
// open_existing's O_NOCTTY is not observable afterwards (the kernel drops it from the F_GETFL
// flags) and only matters for a terminal opened by a process without a controlling terminal,
// so it is not asserted. publish_replacing keeping the old permission bits is asserted on both
// temporary paths, for a temporary the caller synced first (including a failed fsync after its
// fchmod), for a failed fchmod, and for a link, FIFO or directory planted under a closed named
// temporary.

#include "fs/output_dir_testing.hpp" // core-private seam (src/core is on the test include path)
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "support/fs_test_util.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/fsuid.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace testing = mangadex_dl::testing;
using mangadex_dl::Errc;
using mangadex_dl::ExistingHandle;
using mangadex_dl::ExistingOpen;
using mangadex_dl::FileIdentity;
using mangadex_dl::OutputDir;
using mangadex_dl::ReplaceOutcome;
using mangadex_dl::UniqueFd;
using mangadex_dl::test::TempDir;
using mangadex_dl::test::fs_util::list_dir;
using mangadex_dl::test::fs_util::make_temp_dir;
using mangadex_dl::test::fs_util::read_file;
using mangadex_dl::test::fs_util::write_file;

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span{text.data(), text.size()});
}

/// Every byte of the file behind `fd`, read with pread from offset 0 (the descriptor's offset
/// is left alone).
[[nodiscard]] std::string pread_all(const UniqueFd& fd) {
    std::string content{};
    std::array<char, 4096> buffer{};
    off_t offset = 0;
    while (true) {
        const ssize_t got = ::pread(fd.get(), buffer.data(), buffer.size(), offset);
        REQUIRE(got >= 0);
        if (got == 0) {
            return content;
        }
        content.append(buffer.data(), static_cast<std::size_t>(got));
        offset += got;
    }
}

/// `flags & mask` for the non-negative int flag words of fcntl(2), computed unsigned.
[[nodiscard]] unsigned masked(int flags, int mask) noexcept {
    return static_cast<unsigned>(flags) & static_cast<unsigned>(mask);
}

/// The number of open descriptors of this process (the iterator's own descriptor included, so
/// only differences between two calls mean anything).
[[nodiscard]] std::size_t open_fd_count() {
    const auto entries = fs::directory_iterator{"/proc/self/fd"};
    return static_cast<std::size_t>(std::ranges::distance(entries, fs::directory_iterator{}));
}

/// lstat(2) of `path`, required to succeed.
[[nodiscard]] struct stat lstat_of(const fs::path& path) {
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    return st;
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

/// A listening-free AF_UNIX socket bound at `path`; the descriptor closes on destruction (the
/// socket file stays, as a name in the directory). nullopt when `path` does not fit sun_path.
[[nodiscard]] std::optional<UniqueFd> bind_unix_socket(const fs::path& path) {
    const std::string name = path.string();
    sockaddr_un addr{};
    if (name.size() >= sizeof(addr.sun_path)) {
        return std::nullopt;
    }
    addr.sun_family = AF_UNIX;
    std::ranges::copy(name, std::begin(addr.sun_path)); // NUL-terminated: addr is zeroed
    UniqueFd sock{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    REQUIRE(sock.valid());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockaddr API takes a base
    const auto* base = reinterpret_cast<const sockaddr*>(&addr);
    REQUIRE(::bind(sock.get(), base, sizeof(addr)) == 0);
    return sock;
}

/// The kinds of non-regular names open_existing must refuse.
enum class Kind { symlink, dangling_symlink, symlink_loop, fifo, directory, socket };

/// Creates `name` in `dir` as `kind` (a symlink points at the regular file `target`). Returns the
/// socket's descriptor for Kind::socket (keep it alive), nullopt otherwise. SKIPs the test when the
/// socket path is too long for sun_path.
[[nodiscard]] std::optional<UniqueFd> make_kind(const fs::path& dir, std::string_view name,
                                                Kind kind) {
    const fs::path path = dir / name;
    switch (kind) {
    case Kind::symlink:
        write_file(dir / "target", "target content");
        fs::create_symlink("target", path);
        return std::nullopt;
    case Kind::dangling_symlink:
        fs::create_symlink("does-not-exist", path);
        return std::nullopt;
    case Kind::symlink_loop:
        fs::create_symlink(std::string{name}, path); // points at itself
        return std::nullopt;
    case Kind::fifo:
        REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
        return std::nullopt;
    case Kind::directory:
        fs::create_directory(path);
        return std::nullopt;
    case Kind::socket: {
        std::optional<UniqueFd> sock = bind_unix_socket(path);
        if (!sock.has_value()) {
            SKIP("the temporary directory path is too long for an AF_UNIX socket name");
        }
        return sock;
    }
    }
    FAIL("unknown kind");
    return std::nullopt;
}

/// Opens `name` for inspection, requiring a regular file.
[[nodiscard]] ExistingHandle open_regular(const OutputDir& out, std::string_view name) {
    auto handle = out.open_existing(name);
    REQUIRE(handle.has_value());
    REQUIRE(handle->status == ExistingOpen::opened);
    REQUIRE(handle->fd.valid());
    return std::move(*handle);
}

/// A temporary for `name` holding `content`.
[[nodiscard]] mangadex_dl::TempFile make_temp(OutputDir& out, std::string_view name,
                                              std::string_view content) {
    auto temp = out.create_temp(name);
    REQUIRE(temp.has_value());
    REQUIRE(temp->write(as_bytes(content)).has_value());
    return std::move(*temp);
}

/// Permission bits do not restrict root (CAP_DAC_OVERRIDE). As root, this switches the calling
/// thread's filesystem uid to nobody for its lifetime, which also drops the filesystem
/// capabilities from the effective set; the destructor switches back to 0, which restores them.
/// Only filesystem permission checks of this thread change. Does nothing when not root.
class ScopedUnprivilegedFs {
public:
    ScopedUnprivilegedFs() noexcept {
        if (::geteuid() != 0) {
            return;
        }
        static_cast<void>(::setfsuid(nobody_uid));
        // setfsuid reports only the previous value: a second call shows whether the first took.
        switched_ = std::cmp_equal(::setfsuid(nobody_uid), nobody_uid);
        usable_ = switched_;
    }
    ScopedUnprivilegedFs(const ScopedUnprivilegedFs&) = delete;
    ScopedUnprivilegedFs& operator=(const ScopedUnprivilegedFs&) = delete;
    ScopedUnprivilegedFs(ScopedUnprivilegedFs&&) = delete;
    ScopedUnprivilegedFs& operator=(ScopedUnprivilegedFs&&) = delete;
    ~ScopedUnprivilegedFs() {
        if (switched_) {
            static_cast<void>(::setfsuid(0));
        }
    }

    /// Whether permission bits now restrict this thread.
    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    static constexpr uid_t nobody_uid = 65534;
    bool switched_ = false;
    bool usable_ = true;
};

/// out.open_existing(name) with filesystem permission checks in force, even as root. SKIPs the
/// test when root cannot drop them.
[[nodiscard]] mangadex_dl::Result<ExistingHandle>
open_existing_unprivileged(const OutputDir& out, std::string_view name) {
    const ScopedUnprivilegedFs unprivileged{};
    if (!unprivileged.usable()) {
        SKIP("running as root without a usable setfsuid: permission bits do not restrict root");
    }
    return out.open_existing(name);
}

[[nodiscard]] bool has_part_name(const std::vector<std::string>& names) {
    return std::ranges::any_of(names, [](const std::string& n) { return n.contains(".part-"); });
}

/// The one `.part-` name in `dir` (a named fallback temporary), required to exist.
[[nodiscard]] std::string the_part_name(const fs::path& dir) {
    const std::vector<std::string> names = list_dir(dir);
    const auto found =
        std::ranges::find_if(names, [](const std::string& n) { return n.contains(".part-"); });
    REQUIRE(found != names.end());
    REQUIRE(std::ranges::count_if(names,
                                  [](const std::string& n) { return n.contains(".part-"); }) == 1);
    return *found;
}

/// The mode bits of `path` that FileIdentity records (no file type), from lstat.
[[nodiscard]] mode_t mode_of(const fs::path& path) { return lstat_of(path).st_mode & 07777U; }

/// Sets the process umask for the lifetime of the object, then restores the previous one.
class ScopedUmask {
public:
    explicit ScopedUmask(mode_t mask) noexcept : previous_(::umask(mask)) {}
    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;
    ScopedUmask(ScopedUmask&&) = delete;
    ScopedUmask& operator=(ScopedUmask&&) = delete;
    ~ScopedUmask() { static_cast<void>(::umask(previous_)); }

private:
    mode_t previous_ = 0;
};

} // namespace

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

// ---- open_existing -------------------------------------------------------------------------

TEST_CASE("open_existing opens a regular file read-only and records its identity",
          "[output_dir][existing]") {
    const TempDir dir = make_temp_dir();
    write_file(dir.path() / "book.epub", "old epub bytes");
    const OutputDir out = open_dir(dir.path(), false);

    const bool skip_precheck = GENERATE(false, true);
    CAPTURE(skip_precheck);
    std::optional<testing::ScopedOutputDirFault> fault{};
    if (skip_precheck) {
        fault.emplace(&testing::force_skip_existing_precheck);
    }

    ExistingHandle handle = open_regular(out, "book.epub");
    CHECK(handle.error == 0);
    CHECK(pread_all(handle.fd) == "old epub bytes");

    const struct stat st = lstat_of(dir.path() / "book.epub");
    CHECK(handle.identity.device == st.st_dev);
    CHECK(handle.identity.inode == st.st_ino);
    CHECK(handle.identity.size == st.st_size);
    CHECK(handle.identity.mtime_sec == st.st_mtim.tv_sec);
    CHECK(handle.identity.mtime_nsec == st.st_mtim.tv_nsec);
    CHECK(handle.identity.ctime_sec == st.st_ctim.tv_sec);
    CHECK(handle.identity.ctime_nsec == st.st_ctim.tv_nsec);
    CHECK(handle.identity.mode == (st.st_mode & 07777U));

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    const int status_flags = ::fcntl(handle.fd.get(), F_GETFL);
    REQUIRE(status_flags >= 0);
    CHECK(masked(status_flags, O_ACCMODE) == masked(O_RDONLY, O_ACCMODE));
    CHECK(masked(status_flags, O_NONBLOCK) != 0U);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    const int fd_flags = ::fcntl(handle.fd.get(), F_GETFD);
    REQUIRE(fd_flags >= 0);
    CHECK(masked(fd_flags, FD_CLOEXEC) != 0U);

    // The header's moved-from contract: the status stays, the descriptor moves.
    const ExistingHandle moved = std::move(handle);
    CHECK(moved.fd.valid());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from contract
    CHECK(handle.status == ExistingOpen::opened);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from contract
    CHECK(!handle.fd.valid());
}

TEST_CASE("open_existing reports an absent name", "[output_dir][existing]") {
    const TempDir dir = make_temp_dir();
    const OutputDir out = open_dir(dir.path(), false);
    const bool skip_precheck = GENERATE(false, true);
    CAPTURE(skip_precheck);
    std::optional<testing::ScopedOutputDirFault> fault{};
    if (skip_precheck) {
        fault.emplace(&testing::force_skip_existing_precheck);
    }

    const auto handle = out.open_existing("missing.epub");
    REQUIRE(handle.has_value());
    CHECK(handle->status == ExistingOpen::absent);
    CHECK(!handle->fd.valid());
    CHECK(handle->error == 0);
    CHECK(handle->identity == FileIdentity{});
}

TEST_CASE("open_existing pre-check classifies non-regular names without opening them",
          "[output_dir][existing]") {
    const Kind kind = GENERATE(Kind::symlink, Kind::dangling_symlink, Kind::symlink_loop,
                               Kind::fifo, Kind::directory, Kind::socket);
    CAPTURE(kind);
    const TempDir dir = make_temp_dir();
    const std::optional<UniqueFd> sock = make_kind(dir.path(), "book.epub", kind);
    const OutputDir out = open_dir(dir.path(), false);

    const std::size_t fds_before = open_fd_count();
    const auto handle = out.open_existing("book.epub"); // a FIFO must not block here
    REQUIRE(handle.has_value());
    CHECK(handle->status == ExistingOpen::not_regular);
    CHECK(!handle->fd.valid());
    CHECK(handle->error == 0); // decided by fstatat, before any open
    CHECK(open_fd_count() == fds_before);
    CHECK(fs::symlink_status(dir.path() / "book.epub").type() != fs::file_type::regular);
}

TEST_CASE("open_existing without the pre-check maps the openat errnos exactly",
          "[output_dir][existing]") {
    const TempDir dir = make_temp_dir();
    const testing::ScopedOutputDirFault fault{&testing::force_skip_existing_precheck};

    SECTION("a symlink of any kind is ELOOP under O_NOFOLLOW") {
        const Kind kind = GENERATE(Kind::symlink, Kind::dangling_symlink, Kind::symlink_loop);
        CAPTURE(kind);
        const std::optional<UniqueFd> none = make_kind(dir.path(), "book.epub", kind);
        const OutputDir out = open_dir(dir.path(), false);
        const auto handle = out.open_existing("book.epub");
        REQUIRE(handle.has_value());
        CHECK(handle->status == ExistingOpen::not_regular);
        CHECK(handle->error == ELOOP);
        CHECK(!handle->fd.valid());
        if (kind == Kind::symlink) {
            CHECK(read_file(dir.path() / "target") == "target content");
        }
    }
    SECTION("an AF_UNIX socket is ENXIO") {
        const std::optional<UniqueFd> sock = make_kind(dir.path(), "book.epub", Kind::socket);
        const OutputDir out = open_dir(dir.path(), false);
        const auto handle = out.open_existing("book.epub");
        REQUIRE(handle.has_value());
        CHECK(handle->status == ExistingOpen::not_regular);
        CHECK(handle->error == ENXIO);
        CHECK(!handle->fd.valid());
    }
    SECTION("a directory or FIFO opens, then the fstat re-check closes it again") {
        const Kind kind = GENERATE(Kind::fifo, Kind::directory);
        CAPTURE(kind);
        const std::optional<UniqueFd> none = make_kind(dir.path(), "book.epub", kind);
        const OutputDir out = open_dir(dir.path(), false);
        const std::size_t fds_before = open_fd_count();
        const auto handle = out.open_existing("book.epub"); // O_NONBLOCK: no hang on the FIFO
        REQUIRE(handle.has_value());
        CHECK(handle->status == ExistingOpen::not_regular);
        CHECK(handle->error == 0); // decided by fstat, after a successful open
        CHECK(!handle->fd.valid());
        CHECK(open_fd_count() == fds_before); // the opened descriptor was closed
    }
}

TEST_CASE("open_existing reports an unreadable file or directory entry", "[output_dir][existing]") {
    const TempDir dir = make_temp_dir();
    // Searchable by everyone, so an unprivileged filesystem uid reaches the file itself.
    fs::permissions(dir.path(),
                    fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);

    SECTION("a mode-000 file fails the open with EACCES") {
        const bool skip_precheck = GENERATE(false, true);
        CAPTURE(skip_precheck);
        std::optional<testing::ScopedOutputDirFault> fault{};
        if (skip_precheck) {
            fault.emplace(&testing::force_skip_existing_precheck);
        }
        write_file(dir.path() / "book.epub", "secret");
        fs::permissions(dir.path() / "book.epub", fs::perms::none);
        const OutputDir out = open_dir(dir.path(), false);
        const auto handle = open_existing_unprivileged(out, "book.epub");
        REQUIRE(handle.has_value());
        CHECK(handle->status == ExistingOpen::unreadable);
        CHECK(handle->error == EACCES);
        CHECK(!handle->fd.valid());
    }
    SECTION("a directory without search permission fails the pre-check with EACCES") {
        write_file(dir.path() / "book.epub", "content");
        const OutputDir out = open_dir(dir.path(), false);
        fs::permissions(dir.path(), fs::perms::owner_read | fs::perms::owner_write);
        const auto handle = open_existing_unprivileged(out, "book.epub");
        fs::permissions(dir.path(), fs::perms::owner_all); // so TempDir can clean up
        REQUIRE(handle.has_value());
        CHECK(handle->status == ExistingOpen::unreadable);
        CHECK(handle->error == EACCES);
        CHECK(!handle->fd.valid());
    }
}

TEST_CASE("open_existing rejects a name that is not one path component", "[output_dir][existing]") {
    const TempDir dir = make_temp_dir();
    fs::create_directory(dir.path() / "sub");
    write_file(dir.path() / "sub" / "book.epub", "nested");
    const OutputDir out = open_dir(dir.path(), false);

    const std::string too_long(256, 'n');
    const std::string with_nul{"book\0.epub", 10};
    for (const std::string_view bad :
         {std::string_view{""}, std::string_view{"."}, std::string_view{".."},
          std::string_view{"sub/book.epub"}, std::string_view{"/etc/passwd"},
          std::string_view{too_long}, std::string_view{with_nul}}) {
        CAPTURE(bad);
        const auto handle = out.open_existing(bad);
        REQUIRE(!handle.has_value());
        CHECK(handle.error().code == Errc::failure);
        CHECK(handle.error().message.contains("invalid file name"));
    }
}

// ---- publish_replacing ---------------------------------------------------------------------

TEST_CASE("publish_replacing replaces an unchanged file and the old descriptor keeps the old bytes",
          "[output_dir][existing]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    write_file(dir.path() / "book.epub", "old content");
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub"); // open through publish (V5)

    const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content, longer"),
                                               "book.epub", inspected.identity);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == ReplaceOutcome::replaced);
    CHECK(read_file(dir.path() / "book.epub") == "new content, longer");
    CHECK(pread_all(inspected.fd) == "old content"); // V5/V6: the reader keeps the old inode
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
    CHECK(fs::is_regular_file(fs::symlink_status(dir.path() / "book.epub")));
    CHECK(lstat_of(dir.path() / "book.epub").st_ino != inspected.identity.inode);
}

TEST_CASE("publish_replacing publishes without clobbering when the name vanished",
          "[output_dir][existing]") {
    const bool fallback = GENERATE(false, true);
    const bool no_renameat2 = GENERATE(false, true);
    CAPTURE(fallback, no_renameat2);
    const TempDir dir = make_temp_dir();
    write_file(dir.path() / "book.epub", "old content");
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");
    fs::remove(dir.path() / "book.epub");

    std::optional<testing::ScopedOutputDirFault> fault{};
    if (no_renameat2) {
        fault.emplace(&testing::force_renameat2_unsupported);
    }
    const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content"),
                                               "book.epub", inspected.identity);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == ReplaceOutcome::written);
    CHECK(read_file(dir.path() / "book.epub") == "new content");
    CHECK(pread_all(inspected.fd) == "old content");
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

TEST_CASE("publish_replacing leaves a replaced name alone and discards the temporary",
          "[output_dir][existing]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    SECTION("by another regular file of the same size") {
        write_file(dir.path() / "other", "OLD CONTENT");
        fs::rename(dir.path() / "other", target);
        const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content"),
                                                   "book.epub", inspected.identity);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == ReplaceOutcome::changed);
        CHECK(read_file(target) == "OLD CONTENT");
        CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
    }
    SECTION("by a symlink to the inspected file itself (never followed)") {
        fs::rename(target, dir.path() / "moved");
        fs::create_symlink("moved", target);
        const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content"),
                                                   "book.epub", inspected.identity);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == ReplaceOutcome::changed);
        CHECK(fs::read_symlink(target) == "moved");
        CHECK(read_file(dir.path() / "moved") == "old content");
        CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub", "moved"});
    }
    SECTION("by a directory") {
        fs::remove(target);
        fs::create_directory(target);
        const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content"),
                                                   "book.epub", inspected.identity);
        REQUIRE(outcome.has_value());
        CHECK(*outcome == ReplaceOutcome::changed);
        CHECK(fs::is_directory(fs::symlink_status(target)));
        CHECK(fs::is_empty(target));
        CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
    }
    CHECK(pread_all(inspected.fd) == "old content");
}

TEST_CASE("publish_replacing catches a same-size in-place rewrite with the mtime restored",
          "[output_dir][existing]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");
    const FileIdentity& before = inspected.identity;

    // Rewrite the same inode in place with bytes of the same size, then put the old mtime back:
    // only the ctime still tells the files apart (V12). The kernel may keep a coarse ctime, so
    // repeat the futimens (which updates the ctime) until the ctime has visibly moved on.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const UniqueFd writer{::open(target.c_str(), O_WRONLY | O_CLOEXEC)};
    REQUIRE(writer.valid());
    const std::string_view replacement = "OLD CONTENT";
    REQUIRE(::pwrite(writer.get(), replacement.data(), replacement.size(), 0) ==
            static_cast<ssize_t>(replacement.size()));
    // atime untouched, mtime put back.
    const std::array<timespec, 2> times{
        timespec{.tv_sec = 0, .tv_nsec = UTIME_OMIT},
        timespec{.tv_sec = before.mtime_sec, .tv_nsec = before.mtime_nsec}};
    struct stat now{};
    for (int attempt = 0; attempt < 200; ++attempt) {
        REQUIRE(::futimens(writer.get(), times.data()) == 0);
        REQUIRE(::fstat(writer.get(), &now) == 0);
        if (now.st_ctim.tv_sec != before.ctime_sec || now.st_ctim.tv_nsec != before.ctime_nsec) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    // The precondition this test is about: everything but the ctime is as inspected.
    REQUIRE(now.st_ino == before.inode);
    REQUIRE(now.st_size == before.size);
    REQUIRE(now.st_mtim.tv_sec == before.mtime_sec);
    REQUIRE(now.st_mtim.tv_nsec == before.mtime_nsec);
    REQUIRE((now.st_ctim.tv_sec != before.ctime_sec || now.st_ctim.tv_nsec != before.ctime_nsec));

    const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content"),
                                               "book.epub", inspected.identity);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == ReplaceOutcome::changed);
    CHECK(read_file(target) == "OLD CONTENT");
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

TEST_CASE("publish_replacing refuses a bad name or a consumed temporary and discards",
          "[output_dir][existing]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    write_file(dir.path() / "book.epub", "old content");
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    const auto bad = out.publish_replacing(std::move(temp), "sub/book.epub", inspected.identity);
    REQUIRE(!bad.has_value());
    CHECK(bad.error().code == Errc::failure);
    CHECK(bad.error().message.contains("invalid file name"));
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});

    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from contract
    const auto consumed = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(!consumed.has_value());
    CHECK(consumed.error().code == Errc::failure);
    CHECK(read_file(dir.path() / "book.epub") == "old content");
    CHECK(!has_part_name(list_dir(dir.path())));
}

TEST_CASE("publish_replacing a failed sync discards the temporary and keeps the old file",
          "[output_dir][existing]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    write_file(dir.path() / "book.epub", "old content");
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    const testing::ScopedIoError fault{testing::IoStage::fsync, ENOSPC};
    const auto outcome = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(!outcome.has_value());
    CHECK(outcome.error().message.contains("No space left on device"));
    CHECK(read_file(dir.path() / "book.epub") == "old content");
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

// ---- publish_replacing keeps the permission bits -------------------------------------------

TEST_CASE("publish_replacing gives the new file the inspected file's permission bits",
          "[output_dir][existing][mode]") {
    const bool fallback = GENERATE(false, true);
    const bool vanished = GENERATE(false, true);
    // 0666 would be 0644 under the umask below, so it proves the umask is not applied; the
    // set-user-ID bit of 04755 is recorded in the identity but never carried over. 0400 shows the
    // fchmod, which comes before the last buffered write and the fsync, does not block them.
    const mode_t old_mode =
        GENERATE(mode_t{0600}, mode_t{0400}, mode_t{0644}, mode_t{0666}, mode_t{04755});
    CAPTURE(fallback, vanished, old_mode);
    const ScopedUmask umask{022};
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    REQUIRE(::chmod(target.c_str(), old_mode) == 0);
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");
    REQUIRE(inspected.identity.mode == old_mode);
    if (vanished) {
        fs::remove(target);
    }

    const auto outcome = out.publish_replacing(make_temp(out, "book.epub", "new content"),
                                               "book.epub", inspected.identity);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == (vanished ? ReplaceOutcome::written : ReplaceOutcome::replaced));
    CHECK(read_file(target) == "new content");
    CHECK(mode_of(target) == (old_mode & 0777U));
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

TEST_CASE("publish_replacing keeps the permission bits for a temporary the caller synced first",
          "[output_dir][existing][mode]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const ScopedUmask umask{022};
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    REQUIRE(::chmod(target.c_str(), 0600) == 0);
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    // A named temporary is closed by sync_and_close, so publish_replacing reopens it by name.
    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    REQUIRE(temp.sync_and_close().has_value());
    const auto outcome = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == ReplaceOutcome::replaced);
    CHECK(read_file(target) == "new content");
    CHECK(mode_of(target) == 0600U);
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

TEST_CASE("publish_replacing never changes the mode of a link planted under a closed temporary",
          "[output_dir][existing][mode]") {
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    const fs::path victim = dir.path() / "victim";
    write_file(target, "old content");
    REQUIRE(::chmod(target.c_str(), 0600) == 0);
    write_file(victim, "victim content");
    REQUIRE(::chmod(victim.c_str(), 0640) == 0);
    OutputDir out = open_dir(dir.path(), true);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    // A symlink is refused by O_NOFOLLOW (ELOOP), a hard link by its second link.
    const bool hard_link = GENERATE(false, true);
    CAPTURE(hard_link);
    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    REQUIRE(temp.sync_and_close().has_value());
    const fs::path part = dir.path() / the_part_name(dir.path());
    fs::remove(part);
    if (hard_link) {
        fs::create_hard_link(victim, part);
    } else {
        fs::create_symlink("victim", part);
    }
    const auto outcome = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(!outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(outcome.error().message.contains("cannot set the mode of 'book.epub': "));
    CHECK(outcome.error().message.contains(hard_link ? "its temporary file was replaced"
                                                     : "Too many levels of symbolic links"));
    CHECK(mode_of(victim) == 0640U);
    CHECK(read_file(victim) == "victim content");
    CHECK(read_file(target) == "old content");
    CHECK(mode_of(target) == 0600U);
    // The discarded temporary unlinked its name, i.e. the planted link.
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub", "victim"});
}

TEST_CASE("publish_replacing a failed fchmod discards the temporary and keeps the old file",
          "[output_dir][existing][mode]") {
    const bool fallback = GENERATE(false, true);
    const bool presynced = GENERATE(false, true);
    CAPTURE(fallback, presynced);
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    REQUIRE(::chmod(target.c_str(), 0600) == 0);
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    if (presynced) {
        REQUIRE(temp.sync_and_close().has_value());
    }
    const testing::ScopedIoError fault{testing::IoStage::chmod, EPERM};
    const auto outcome = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(!outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(outcome.error().message.contains("cannot set the mode of 'book.epub'"));
    CHECK(outcome.error().message.contains("Operation not permitted"));
    CHECK(read_file(target) == "old content");
    CHECK(mode_of(target) == 0600U);
    CHECK(lstat_of(target).st_ino == inspected.identity.inode);
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

TEST_CASE("publish_replacing a failed sync after the mode change of a synced temporary keeps the "
          "old file",
          "[output_dir][existing][mode]") {
    // A temporary the caller synced first gets a second fsync after its fchmod (a named one is
    // reopened by name for it), so the new mode is durable before the rename.
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    REQUIRE(::chmod(target.c_str(), 0600) == 0);
    OutputDir out = open_dir(dir.path(), fallback);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    REQUIRE(temp.sync_and_close().has_value());
    const testing::ScopedIoError fault{testing::IoStage::fsync, EIO};
    const auto outcome = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(!outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(outcome.error().message.contains("cannot sync 'book.epub'"));
    CHECK(outcome.error().message.contains("Input/output error"));
    CHECK(read_file(target) == "old content");
    CHECK(mode_of(target) == 0600U);
    CHECK(lstat_of(target).st_ino == inspected.identity.inode);
    CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
}

TEST_CASE("publish_replacing refuses a FIFO or directory planted under a closed temporary",
          "[output_dir][existing][mode]") {
    // The reopen of a closed named temporary opens a FIFO (O_NONBLOCK: no writer needed) or a
    // directory, then refuses it on file type. A FIFO has one link, so only the type check stops
    // it; a directory has two, so either check would.
    const Kind kind = GENERATE(Kind::fifo, Kind::directory);
    CAPTURE(kind);
    const TempDir dir = make_temp_dir();
    const fs::path target = dir.path() / "book.epub";
    write_file(target, "old content");
    REQUIRE(::chmod(target.c_str(), 0600) == 0);
    OutputDir out = open_dir(dir.path(), true);
    const ExistingHandle inspected = open_regular(out, "book.epub");

    // Every descriptor the temporary opens (its own, a private directory descriptor, the reopen)
    // is closed again by the time publish_replacing returns.
    const std::size_t fds_before = open_fd_count();
    mangadex_dl::TempFile temp = make_temp(out, "book.epub", "new content");
    REQUIRE(temp.sync_and_close().has_value());
    const std::string part = the_part_name(dir.path());
    fs::remove(dir.path() / part);
    const std::optional<UniqueFd> none = make_kind(dir.path(), part, kind);
    const mode_t planted_mode = mode_of(dir.path() / part);

    const auto outcome = out.publish_replacing(std::move(temp), "book.epub", inspected.identity);
    REQUIRE(!outcome.has_value());
    CHECK(outcome.error().code == Errc::failure);
    CHECK(outcome.error().message ==
          "cannot set the mode of 'book.epub': its temporary file was replaced");
    CHECK(open_fd_count() == fds_before);
    CHECK(read_file(target) == "old content");
    CHECK(mode_of(target) == 0600U);
    CHECK(lstat_of(target).st_ino == inspected.identity.inode);
    if (kind == Kind::fifo) {
        // The discarded temporary unlinked its name, i.e. the planted FIFO.
        CHECK(list_dir(dir.path()) == std::vector<std::string>{"book.epub"});
    } else {
        // unlinkat without AT_REMOVEDIR cannot remove a directory: it stays, mode unchanged.
        CHECK(list_dir(dir.path()) == std::vector<std::string>{part, "book.epub"}); // dot first
        CHECK(fs::is_directory(fs::symlink_status(dir.path() / part)));
        CHECK(mode_of(dir.path() / part) == planted_mode);
    }
}
