// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/session_store_test.cpp: the user's directories, StateDir (the private state
// directory, its session files and the session lock) (follows plan T5). Every case works in a
// fresh TempDir. The lock waits run on a FakeClock: the "wait, then succeed" case lives here only
// (V32), with an on_sleep hook that closes the competing open file description at the 3rd poll.
//
// Ownership cases need a directory or file owned by another user: as root the test chowns one to
// uid 65534; otherwise it uses "/" (owned by root) or SKIPs with the reason.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "support/fake_clock.hpp"
#include "support/fs_test_util.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
using md::Errc;
using md::FileProblemKind;
using md::LockTiming;
using md::Secret;
using md::SessionLock;
using md::SessionSlot;
using md::StateDir;
using md::UniqueFd;
using md::UserDirs;
using md::test::FakeClock;
using md::test::fs_util::list_dir;
using md::test::fs_util::make_temp_dir;
using md::test::fs_util::read_file;
using md::test::fs_util::write_file;

inline constexpr uid_t nobody_uid = 65534;
inline constexpr gid_t nobody_gid = 65534;
inline constexpr std::string_view timeout_message =
    "another mangadex-dl is using the saved login; try again";

/// Sets the process umask for the scope and restores the previous one.
class UmaskGuard {
public:
    explicit UmaskGuard(mode_t mask) noexcept : saved_(::umask(mask)) {}
    UmaskGuard(const UmaskGuard&) = delete;
    UmaskGuard& operator=(const UmaskGuard&) = delete;
    UmaskGuard(UmaskGuard&&) = delete;
    UmaskGuard& operator=(UmaskGuard&&) = delete;
    ~UmaskGuard() { static_cast<void>(::umask(saved_)); }

private:
    mode_t saved_ = 0;
};

/// Clears the interrupt flag on entry and on exit, so a simulated signal never leaks into
/// another test.
class InterruptReset {
public:
    InterruptReset() noexcept { md::reset_interrupt_for_testing(); }
    InterruptReset(const InterruptReset&) = delete;
    InterruptReset& operator=(const InterruptReset&) = delete;
    InterruptReset(InterruptReset&&) = delete;
    InterruptReset& operator=(InterruptReset&&) = delete;
    ~InterruptReset() { md::reset_interrupt_for_testing(); }
};

/// lstat permission bits (0777) of `path`, which must exist.
[[nodiscard]] mode_t mode_of(const fs::path& path) {
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    return st.st_mode & 0777U;
}

/// open(2) for the tests: the one variadic call in this file.
[[nodiscard]] int open_path(const fs::path& path, int flags, mode_t mode = 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    return ::open(path.c_str(), flags, mode);
}

/// Writes `content` to a new file at `path` with exactly `mode` (fchmod, so the umask does not
/// apply).
void write_with_mode(const fs::path& path, std::string_view content, mode_t mode) {
    const UniqueFd fd{open_path(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    REQUIRE(fd.valid());
    REQUIRE(::write(fd.get(), content.data(), content.size()) ==
            static_cast<ssize_t>(content.size()));
    REQUIRE(::fchmod(fd.get(), mode) == 0);
}

/// Makes `path` a directory with exactly `mode`.
void make_dir_with_mode(const fs::path& path, mode_t mode) {
    REQUIRE(::mkdir(path.c_str(), 0700) == 0);
    REQUIRE(::chmod(path.c_str(), mode) == 0);
}

/// Opens (creating) the state directory at `path`, required to succeed.
[[nodiscard]] StateDir open_state(const fs::path& path) {
    md::FileResult<StateDir> dir = StateDir::open(path, true);
    INFO((dir ? std::string{} : dir.error().reason));
    REQUIRE(dir.has_value());
    return std::move(*dir);
}

/// A copy of the document in `slot`, which must be present and readable (test values only).
[[nodiscard]] std::string read_present(const StateDir& dir, SessionSlot slot) {
    const md::FileResult<std::optional<Secret>> read = dir.read(slot);
    INFO((read ? std::string{} : read.error().reason));
    REQUIRE(read.has_value());
    const std::optional<Secret>& document = *read;
    REQUIRE(document.has_value());
    // REQUIRE is invisible to bugprone-unchecked-optional-access; this check is not (an empty
    // optional here was already reported by the REQUIRE above).
    if (!document.has_value()) {
        return std::string{};
    }
    return std::string{document->reveal()};
}

/// The names in `dir` that start with '.', i.e. leftover temporaries.
[[nodiscard]] std::vector<std::string> hidden_names(const fs::path& dir) {
    std::vector<std::string> hidden{};
    for (std::string& name : list_dir(dir)) {
        if (name.starts_with('.')) {
            hidden.push_back(std::move(name));
        }
    }
    return hidden;
}

/// An independent open file description of `<dir>/session.lock`, holding flock(LOCK_EX).
[[nodiscard]] UniqueFd hold_lock(const fs::path& dir) {
    UniqueFd fd{open_path(dir / "session.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
    REQUIRE(fd.valid());
    REQUIRE(::flock(fd.get(), LOCK_EX | LOCK_NB) == 0);
    return fd;
}

/// Whether another open file description could take the lock right now (it is released at once).
[[nodiscard]] bool lock_is_free(const fs::path& dir) {
    const UniqueFd fd{open_path(dir / "session.lock", O_RDWR | O_CLOEXEC)};
    REQUIRE(fd.valid());
    return ::flock(fd.get(), LOCK_EX | LOCK_NB) == 0;
}

// ---- the user's directories ------------------------------------------------------------------

TEST_CASE("session store: make_user_dirs keeps only non-empty absolute paths", "[auth][session]") {
    const UserDirs dirs = md::make_user_dirs("/home/u", "relative/config", "");
    CHECK(dirs.home == fs::path{"/home/u"});
    CHECK_FALSE(dirs.xdg_config_home.has_value());
    CHECK_FALSE(dirs.xdg_state_home.has_value());

    const UserDirs all = md::make_user_dirs("/h", "/c", "/s");
    CHECK(all == UserDirs{.home = fs::path{"/h"},
                          .xdg_config_home = fs::path{"/c"},
                          .xdg_state_home = fs::path{"/s"}});

    CHECK(md::make_user_dirs(std::nullopt, std::nullopt, std::nullopt) == UserDirs{});
    CHECK(md::make_user_dirs("", "", "") == UserDirs{});
}

TEST_CASE("session store: state and credentials paths prefer XDG, then HOME", "[auth][session]") {
    const UserDirs xdg = md::make_user_dirs("/home/u", "/cfg", "/st");
    CHECK(md::state_dir_path(xdg) == fs::path{"/st/mangadex-dl"});
    CHECK(md::default_credentials_path(xdg) == fs::path{"/cfg/mangadex-dl/credentials"});

    const UserDirs home_only = md::make_user_dirs("/home/u", "cfg", "st");
    CHECK(md::state_dir_path(home_only) == fs::path{"/home/u/.local/state/mangadex-dl"});
    CHECK(md::default_credentials_path(home_only) ==
          fs::path{"/home/u/.config/mangadex-dl/credentials"});

    const UserDirs none{};
    CHECK_FALSE(md::state_dir_path(none).has_value());
    CHECK_FALSE(md::default_credentials_path(none).has_value());
}

TEST_CASE("session store: session file names per slot", "[auth][session]") {
    CHECK(md::session_file_name(SessionSlot::real) == "session.json");
    CHECK(md::session_file_name(SessionSlot::fixture) == "fixture-session.json");
}

// ---- StateDir::open ----------------------------------------------------------------------------

TEST_CASE("session store: open creates missing directories 0700, parents too", "[auth][session]") {
    const mode_t mask = GENERATE(mode_t{022}, mode_t{077}, mode_t{0});
    CAPTURE(mask);
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "a" / "b" / "mangadex-dl";
    {
        const UmaskGuard umask{mask};
        const StateDir dir = open_state(path);
        CHECK(dir.path() == path);
    }
    CHECK(mode_of(root.path() / "a") == 0700);
    CHECK(mode_of(root.path() / "a" / "b") == 0700);
    CHECK(mode_of(path) == 0700);
    CHECK(list_dir(path).empty());
}

TEST_CASE("session store: open accepts an existing private directory and symlinked parents",
          "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    make_dir_with_mode(root.path() / "real", 0755); // a parent may be as open as it likes
    make_dir_with_mode(root.path() / "real" / "mangadex-dl", 0700);
    fs::create_directory_symlink(root.path() / "real", root.path() / "link");

    CHECK(StateDir::open(root.path() / "real" / "mangadex-dl", false).has_value());
    CHECK(StateDir::open(root.path() / "link" / "mangadex-dl", false).has_value());
    CHECK(StateDir::open(root.path() / "link" / "mangadex-dl", true).has_value());

    // A trailing separator names the same directory.
    CHECK(StateDir::open(root.path() / "real" / "mangadex-dl" / "", false).has_value());
}

TEST_CASE("session store: open without create reports a missing directory", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "missing" / "mangadex-dl";

    const auto parent_missing = StateDir::open(path, false);
    REQUIRE_FALSE(parent_missing.has_value());
    CHECK(parent_missing.error().kind == FileProblemKind::missing);

    REQUIRE(::mkdir((root.path() / "missing").c_str(), 0700) == 0);
    const auto final_missing = StateDir::open(path, false);
    REQUIRE_FALSE(final_missing.has_value());
    CHECK(final_missing.error().kind == FileProblemKind::missing);
    CHECK(final_missing.error().reason == "it does not exist");
    CHECK_FALSE(fs::exists(path));

    const auto empty = StateDir::open(fs::path{}, false);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind == FileProblemKind::io);
}

TEST_CASE("session store: an existing 0755 directory is too open and nothing is written",
          "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const mode_t mode = GENERATE(mode_t{0755}, mode_t{0750}, mode_t{0701}, mode_t{0777});
    CAPTURE(mode);
    make_dir_with_mode(path, mode);

    const bool create = GENERATE(true, false);
    const auto dir = StateDir::open(path, create);
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().kind == FileProblemKind::too_open);
    CHECK(dir.error().mode == mode);
    CHECK_FALSE(dir.error().reason.empty());
    CHECK_FALSE(dir.error().reason.contains(path.string()));
    CHECK(list_dir(path).empty());
    CHECK(mode_of(path) == mode); // never "fixed" behind the user's back
}

TEST_CASE("session store: a symlinked state directory is refused", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    make_dir_with_mode(root.path() / "elsewhere", 0700);
    const fs::path path = root.path() / "mangadex-dl";
    fs::create_directory_symlink(root.path() / "elsewhere", path);

    const bool create = GENERATE(true, false);
    const auto dir = StateDir::open(path, create);
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().kind == FileProblemKind::symlink);
    CHECK(dir.error().reason == "it is a symbolic link");
    CHECK(list_dir(root.path() / "elsewhere").empty());

    // A dangling symlink is still a symlink, and nothing is created through it.
    fs::remove(path);
    fs::create_directory_symlink(root.path() / "nowhere", path);
    const auto dangling = StateDir::open(path, true);
    REQUIRE_FALSE(dangling.has_value());
    CHECK(dangling.error().kind == FileProblemKind::symlink);
    CHECK_FALSE(fs::exists(root.path() / "nowhere"));
}

TEST_CASE("session store: a regular file at the directory path is not a directory",
          "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    write_with_mode(path, "x", 0600);

    const bool create = GENERATE(true, false);
    const auto dir = StateDir::open(path, create);
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().kind == FileProblemKind::not_regular);
    CHECK(dir.error().reason == "it is not a directory");

    // A regular file as a parent component.
    const auto below = StateDir::open(path / "mangadex-dl", create);
    REQUIRE_FALSE(below.has_value());
    CHECK(below.error().kind == FileProblemKind::not_regular);
    CHECK(read_file(path) == "x");
}

TEST_CASE("session store: a directory owned by another user is refused", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    fs::path path{};
    if (::geteuid() == 0) {
        path = root.path() / "mangadex-dl";
        make_dir_with_mode(path, 0700);
        REQUIRE(::chown(path.c_str(), nobody_uid, nobody_gid) == 0);
    } else {
        struct stat st{};
        if (::stat("/", &st) != 0 || st.st_uid == ::geteuid()) {
            SKIP("not root, and \"/\" is owned by this user: no directory owned by another user");
        }
        path = "/";
    }
    const auto dir = StateDir::open(path, false);
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().kind == FileProblemKind::not_owned);
    CHECK(dir.error().reason == "it is owned by another user");
}

// ---- read / write / remove -------------------------------------------------------------------

TEST_CASE("session store: write then read round-trips, per slot", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);

    const auto absent = dir.read(SessionSlot::real);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of(R"({"format_version":1})")).has_value());
    REQUIRE(dir.write(SessionSlot::fixture, Secret::copy_of("fixture")).has_value());

    CHECK(read_present(dir, SessionSlot::real) == R"({"format_version":1})");
    CHECK(read_present(dir, SessionSlot::fixture) == "fixture");

    // Rewriting replaces; an empty document is a valid (empty) file.
    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("second")).has_value());
    CHECK(read_present(dir, SessionSlot::real) == "second");

    REQUIRE(dir.write(SessionSlot::real, Secret{}).has_value());
    CHECK(read_present(dir, SessionSlot::real).empty());

    CHECK(list_dir(path) == std::vector<std::string>{"fixture-session.json", "session.json"});
}

TEST_CASE("session store: a write is 0600 even under umask 0 and leaves no temporary",
          "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const UmaskGuard umask{0};
    const StateDir dir = open_state(path);
    CHECK(mode_of(path) == 0700);

    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("document")).has_value());
    CHECK(mode_of(path / "session.json") == 0600);
    CHECK(hidden_names(path).empty());

    // A replaced 0644 file comes back 0600 (the new inode is the temporary, fchmod'ed).
    REQUIRE(::chmod((path / "session.json").c_str(), 0644) == 0);
    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("document 2")).has_value());
    CHECK(mode_of(path / "session.json") == 0600);
    CHECK(read_file(path / "session.json") == "document 2");
}

TEST_CASE("session store: a failed rename removes the temporary", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    // A non-empty directory under the final name: renameat of a file over it fails (EISDIR).
    REQUIRE(::mkdir((path / "session.json").c_str(), 0700) == 0);
    write_file(path / "session.json" / "keep", "k");

    const auto written = dir.write(SessionSlot::real, Secret::copy_of("document"));
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Errc::failure);
    CHECK(written.error().message.starts_with("cannot put it in place: "));
    CHECK_FALSE(written.error().message.contains("document"));
    CHECK(hidden_names(path).empty());
    CHECK(list_dir(path) == std::vector<std::string>{"session.json"});
    CHECK(list_dir(path / "session.json") == std::vector<std::string>{"keep"});

    // Reading a directory under the session name is not_regular, never a hang or a crash.
    const auto read = dir.read(SessionSlot::real);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::not_regular);
}

TEST_CASE("session store: a symlinked session file is refused and never written through",
          "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    const fs::path target = root.path() / "target";
    write_with_mode(target, "target bytes", 0600);
    fs::create_symlink(target, path / "session.json");

    const auto read = dir.read(SessionSlot::real);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::symlink);
    CHECK(read.error().reason == "it is a symbolic link");
    CHECK_FALSE(read.error().reason.contains("target bytes"));

    // renameat replaces the link itself, never the file it points to.
    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("new")).has_value());
    CHECK(read_file(target) == "target bytes");
    CHECK_FALSE(fs::is_symlink(path / "session.json"));

    // remove() unlinks a link, never its target.
    fs::remove(path / "session.json");
    fs::create_symlink(target, path / "session.json");
    const auto removed = dir.remove(SessionSlot::real);
    REQUIRE(removed.has_value());
    CHECK(*removed);
    CHECK(read_file(target) == "target bytes");
}

TEST_CASE("session store: read refuses an open or oversized file", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    const fs::path file = path / "session.json";

    SECTION("0644 is too open") {
        write_with_mode(file, "{}", 0644);
        const auto read = dir.read(SessionSlot::real);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileProblemKind::too_open);
        CHECK(read.error().mode == 0644);
    }
    SECTION("exactly the cap is read") {
        write_with_mode(file, std::string(md::max_session_file_bytes, 'a'), 0600);
        CHECK(read_present(dir, SessionSlot::real).size() == md::max_session_file_bytes);
    }
    SECTION("one byte over the cap is too large") {
        write_with_mode(file, std::string(md::max_session_file_bytes + 1, 'a'), 0600);
        const auto read = dir.read(SessionSlot::real);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileProblemKind::too_large);
    }
    SECTION("a FIFO never blocks") {
        REQUIRE(::mkfifo(file.c_str(), 0600) == 0);
        const auto read = dir.read(SessionSlot::real);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileProblemKind::not_regular);
    }
}

TEST_CASE("session store: read refuses a session file owned by another user", "[auth][session]") {
    if (::geteuid() != 0) {
        SKIP("not root: a file owned by another user cannot be created in the state directory");
    }
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    write_with_mode(path / "session.json", "{}", 0600);
    REQUIRE(::chown((path / "session.json").c_str(), nobody_uid, nobody_gid) == 0);

    const auto read = dir.read(SessionSlot::real);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().kind == FileProblemKind::not_owned);
}

TEST_CASE("session store: remove reports whether the file was there", "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);

    const auto absent = dir.remove(SessionSlot::real);
    REQUIRE(absent.has_value());
    CHECK_FALSE(*absent);

    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("x")).has_value());
    REQUIRE(dir.write(SessionSlot::fixture, Secret::copy_of("y")).has_value());
    const auto present = dir.remove(SessionSlot::real);
    REQUIRE(present.has_value());
    CHECK(*present);
    CHECK(list_dir(path) == std::vector<std::string>{"fixture-session.json"});

    const auto again = dir.remove(SessionSlot::real);
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
}

TEST_CASE("session store: the directory descriptor survives a rename of its path",
          "[auth][session]") {
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    fs::rename(path, root.path() / "moved");

    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("x")).has_value());
    CHECK(read_file(root.path() / "moved" / "session.json") == "x");
    CHECK_FALSE(fs::exists(path));
    CHECK(dir.path() == path); // path() is what open() was given
}

// ---- the lock --------------------------------------------------------------------------------

TEST_CASE("session store: lock takes and releases the session lock", "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    FakeClock clock{};

    {
        auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE(lock.has_value());
        CHECK(lock->held());
        CHECK(mode_of(path / "session.lock") == 0600);
        CHECK_FALSE(lock_is_free(path));

        // A move transfers the one descriptor: the moved-from lock holds nothing.
        SessionLock moved = std::move(*lock);
        CHECK_FALSE(lock->held());
        CHECK(moved.held());
        CHECK_FALSE(lock_is_free(path));

        // Assigning an empty lock releases it.
        moved = SessionLock{};
        CHECK_FALSE(moved.held());
        CHECK(lock_is_free(path));
    }

    {
        const auto again = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE(again.has_value());
        CHECK_FALSE(lock_is_free(path));
    }
    CHECK(lock_is_free(path)); // destruction releases it
    CHECK(clock.sleeps().empty());
    CHECK_FALSE(SessionLock{}.held());
}

TEST_CASE("session store: a held lock times out with the exact message", "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    const UniqueFd other = hold_lock(path);
    FakeClock clock{};
    const auto start = clock.now();

    const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
    REQUIRE_FALSE(lock.has_value());
    CHECK(lock.error().code == Errc::failure);
    CHECK(lock.error().message == timeout_message);
    CHECK(clock.now() - start == std::chrono::seconds{60});
    const auto sleeps = clock.sleeps();
    CHECK(sleeps.size() == 240); // every 250 ms for 60 s
    CHECK(clock.total_slept() == std::chrono::seconds{60});
    for (const md::test::SleepRecord& sleep : sleeps) {
        CHECK(sleep.requested == std::chrono::milliseconds{250});
    }

    // Custom timing: the last wait is cut at the deadline.
    FakeClock short_clock{};
    const auto custom = dir.lock(
        short_clock,
        LockTiming{.poll = std::chrono::milliseconds{400}, .timeout = std::chrono::seconds{1}},
        std::stop_token{});
    REQUIRE_FALSE(custom.has_value());
    CHECK(custom.error().message == timeout_message);
    const auto custom_sleeps = short_clock.sleeps();
    REQUIRE(custom_sleeps.size() == 3);
    CHECK(custom_sleeps.back().requested == std::chrono::milliseconds{200});

    // A zero timeout tries once and never sleeps.
    FakeClock zero_clock{};
    const auto once =
        dir.lock(zero_clock, LockTiming{.timeout = std::chrono::seconds{0}}, std::stop_token{});
    REQUIRE_FALSE(once.has_value());
    CHECK(once.error().message == timeout_message);
    CHECK(zero_clock.sleeps().empty());
}

TEST_CASE("session store: lock waits, then succeeds once the other holder closes",
          "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    // Declared before the clock, so both outlive the hook the clock stores.
    UniqueFd other = hold_lock(path);
    int polls = 0;
    FakeClock clock{};
    // The hook runs on this thread inside dir.lock(); `other` and `polls` outlive the clock.
    clock.set_on_sleep([other_fd = &other, count = &polls] {
        ++*count;
        if (*count == 3) {
            other_fd->reset(); // closes the one open file description that holds the lock
        }
    });

    const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
    REQUIRE(lock.has_value());
    CHECK(lock->held());
    CHECK(polls == 3);
    CHECK(clock.sleeps().size() == 3);
    CHECK(clock.total_slept() == std::chrono::milliseconds{750});
}

TEST_CASE("session store: an interrupt or a stop ends the wait", "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    const UniqueFd other = hold_lock(path);

    SECTION("a pending SIGINT") {
        md::simulate_interrupt_for_testing(SIGINT);
        FakeClock clock{};
        const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().code == Errc::interrupted);
        CHECK(clock.sleeps().empty());
    }
    SECTION("a signal during the wait") {
        FakeClock clock{};
        clock.set_on_sleep([] { md::simulate_interrupt_for_testing(SIGTERM); });
        const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().code == Errc::interrupted);
        CHECK(clock.sleeps().size() == 1);
    }
    SECTION("a stop requested during the wait") {
        const std::stop_source source{};
        FakeClock clock{};
        clock.set_on_sleep([source] mutable { source.request_stop(); });
        const auto lock = dir.lock(clock, LockTiming{}, source.get_token());
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().code == Errc::interrupted);
        REQUIRE(clock.sleeps().size() == 1);
        CHECK(clock.sleeps().front().stopped);
    }
}

TEST_CASE("session store: a symlinked or non-regular session.lock is refused",
          "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    FakeClock clock{};

    SECTION("a symlink") {
        const fs::path target = root.path() / "target";
        write_with_mode(target, "keep", 0600);
        fs::create_symlink(target, path / "session.lock");
        const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().code == Errc::failure);
        CHECK(lock.error().message == "the session lock is a symbolic link");
        CHECK(read_file(target) == "keep");
    }
    SECTION("a FIFO") {
        REQUIRE(::mkfifo((path / "session.lock").c_str(), 0600) == 0);
        const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().message == "the session lock is not a regular file");
    }
    SECTION("a lock file other users can open") {
        write_with_mode(path / "session.lock", "", 0644);
        const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().code == Errc::failure);
        CHECK(lock.error().message ==
              "cannot use the session lock: other users can access it (mode 0644)");
    }
}

TEST_CASE("session store: a session.lock owned by another user is refused",
          "[auth][session][lock]") {
    if (::geteuid() != 0) {
        SKIP("not root: a file owned by another user cannot be created in the state directory");
    }
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    write_with_mode(path / "session.lock", "", 0600);
    REQUIRE(::chown((path / "session.lock").c_str(), nobody_uid, nobody_gid) == 0);
    FakeClock clock{};

    const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
    REQUIRE_FALSE(lock.has_value());
    CHECK(lock.error().message == "cannot use the session lock: it is owned by another user");
}

TEST_CASE("session store: extreme lock timings never overflow", "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);

    SECTION("a maximal timeout saturates the deadline; the holder lets go at the 3rd poll") {
        // Declared before the clock, so both outlive the hook the clock stores.
        UniqueFd other = hold_lock(path);
        int polls = 0;
        FakeClock clock{};
        // The hook runs on this thread inside dir.lock(); `other` and `polls` outlive the clock.
        clock.set_on_sleep([other_fd = &other, count = &polls] {
            if (++*count == 3) {
                other_fd->reset();
            }
        });
        const auto lock =
            dir.lock(clock, LockTiming{.timeout = std::chrono::seconds::max()}, std::stop_token{});
        REQUIRE(lock.has_value());
        const auto sleeps = clock.sleeps();
        REQUIRE(sleeps.size() == 3);
        for (const md::test::SleepRecord& sleep : sleeps) {
            CHECK(sleep.requested == std::chrono::milliseconds{250});
        }
    }
    SECTION("a negative timeout tries once and never sleeps") {
        const UniqueFd other = hold_lock(path);
        FakeClock clock{};
        const auto lock =
            dir.lock(clock, LockTiming{.timeout = std::chrono::seconds{-5}}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().message == timeout_message);
        CHECK(clock.sleeps().empty());
    }
    SECTION("a maximal poll waits once, exactly until the deadline") {
        const UniqueFd other = hold_lock(path);
        FakeClock clock{};
        const auto lock = dir.lock(clock,
                                   LockTiming{.poll = std::chrono::milliseconds::max(),
                                              .timeout = std::chrono::seconds{1}},
                                   std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().message == timeout_message);
        const auto sleeps = clock.sleeps();
        REQUIRE(sleeps.size() == 1);
        CHECK(sleeps.front().requested == std::chrono::seconds{1});
    }
    SECTION("a zero or negative poll is clamped to 1 ms") {
        const UniqueFd other = hold_lock(path);
        const std::chrono::milliseconds poll =
            GENERATE(std::chrono::milliseconds{0}, std::chrono::milliseconds{-7},
                     std::chrono::milliseconds::min());
        CAPTURE(poll.count());
        FakeClock clock{};
        const auto lock = dir.lock(
            clock, LockTiming{.poll = poll, .timeout = std::chrono::seconds{1}}, std::stop_token{});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().message == timeout_message);
        const auto sleeps = clock.sleeps();
        CHECK(sleeps.size() == 1000);
        REQUIRE_FALSE(sleeps.empty());
        CHECK(sleeps.front().requested == std::chrono::milliseconds{1});
        CHECK(clock.total_slept() == std::chrono::seconds{1});
    }
}

TEST_CASE("session store: taking the lock sweeps stale temporaries of both slots",
          "[auth][session][lock]") {
    const InterruptReset reset{};
    const md::test::TempDir root = make_temp_dir();
    const fs::path path = root.path() / "mangadex-dl";
    const StateDir dir = open_state(path);
    REQUIRE(dir.write(SessionSlot::real, Secret::copy_of("kept")).has_value());

    const std::vector<std::string> stale{".session.json.0123456789ab",
                                         ".fixture-session.json.abcdef012345"};
    const std::vector<std::string> kept_names{
        ".session.json.0123456789AB",   // uppercase hex is not ours
        ".session.json.0123456789a",    // 11 digits
        ".session.json.0123456789abc",  // 13 digits
        ".session.json.0123456789ab.x", // a suffix
        ".session.json.012345678zab",   // not hex
        ".other.json.0123456789ab",     // another name
        "session.json.0123456789ab",    // no leading dot
        ".session.json-0123456789ab",   // wrong separator
    };
    for (const std::string& name : stale) {
        write_with_mode(path / name, "stale", 0600);
    }
    for (const std::string& name : kept_names) {
        write_with_mode(path / name, "other", 0600);
    }
    // A directory that matches the pattern cannot be unlinked; the lock is still taken.
    REQUIRE(::mkdir((path / ".session.json.ffffffffffff").c_str(), 0700) == 0);

    FakeClock clock{};
    const auto lock = dir.lock(clock, LockTiming{}, std::stop_token{});
    REQUIRE(lock.has_value());

    const std::vector<std::string> names = list_dir(path);
    for (const std::string& name : stale) {
        CAPTURE(name);
        CHECK_FALSE(std::ranges::contains(names, name));
    }
    for (const std::string& name : kept_names) {
        CAPTURE(name);
        CHECK(std::ranges::contains(names, name));
    }
    CHECK(std::ranges::contains(names, std::string{".session.json.ffffffffffff"}));
    CHECK(read_present(dir, SessionSlot::real) == "kept");
}

} // namespace
