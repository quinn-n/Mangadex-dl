// SPDX-License-Identifier: GPL-3.0-or-later
//
// The saved-login store: where the state directory and the credentials file live, the private
// state directory itself (0700, reached through one O_NOFOLLOW descriptor), its session files
// (atomic 0600 writes), the session lock, and the checked reader for private files.
// Linux only. Frozen interface (follows plan T1); implemented in src/core/auth/session_store.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace mangadex_dl {

/// The user's directories, from HOME, XDG_CONFIG_HOME and XDG_STATE_HOME. Each is absolute or
/// nullopt.
struct UserDirs {
    std::optional<std::filesystem::path> home{};
    std::optional<std::filesystem::path> xdg_config_home{};
    std::optional<std::filesystem::path> xdg_state_home{};

    friend bool operator==(const UserDirs&, const UserDirs&) = default;
};

/// Keeps only the values that are non-empty absolute paths (a relative XDG value is ignored, as
/// the XDG spec asks).
[[nodiscard]] UserDirs make_user_dirs(std::optional<std::string_view> home,
                                      std::optional<std::string_view> config,
                                      std::optional<std::string_view> state);

/// `$XDG_STATE_HOME/mangadex-dl`, else `$HOME/.local/state/mangadex-dl`; nullopt without either.
[[nodiscard]] std::optional<std::filesystem::path> state_dir_path(const UserDirs& dirs);

/// `$XDG_CONFIG_HOME/mangadex-dl/credentials`, else `$HOME/.config/mangadex-dl/credentials`;
/// nullopt without either.
[[nodiscard]] std::optional<std::filesystem::path> default_credentials_path(const UserDirs& dirs);

/// Which session file: the real login, or the one fixture mode uses (so a test run can never
/// read or clobber a real login).
enum class SessionSlot {
    real,
    fixture,
};

/// "session.json" or "fixture-session.json". Points at static storage.
[[nodiscard]] std::string_view session_file_name(SessionSlot slot) noexcept;

/// How StateDir::lock waits for another process's lock.
struct LockTiming {
    std::chrono::milliseconds poll{250};
    std::chrono::seconds timeout{60};

    friend bool operator==(const LockTiming&, const LockTiming&) = default;
};

/// Why a private file or the state directory can't be used.
enum class FileProblemKind {
    missing,     ///< it does not exist
    symlink,     ///< a symbolic link where none is allowed
    not_regular, ///< not a regular file (or, for the directory, not a directory)
    not_owned,   ///< not owned by the effective user
    too_open,    ///< readable or writable by group or others ((mode & 077) != 0)
    too_large,   ///< over the caller's size cap
    io,          ///< any other system call failure
};

struct FileProblem {
    FileProblemKind kind = FileProblemKind::io;
    unsigned mode = 0;    ///< the permission bits found (too_open), else 0
    std::string reason{}; ///< a fixed phrase (plus strerror text), never the path or file bytes

    friend bool operator==(const FileProblem&, const FileProblem&) = default;
};

template <class T> using FileResult = std::expected<T, FileProblem>;

/// Reads a file the user keeps private (the credentials file): open
/// O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK with symlinks followed, then fstat: a regular file
/// (a FIFO never blocks), st_uid == geteuid(), (mode & 077) == 0; then at most `cap` bytes, read
/// straight into a pre-reserved Secret (more is too_large).
[[nodiscard]] FileResult<Secret> read_private_file(const std::filesystem::path& path,
                                                   std::size_t cap);

class StateDir;

/// The held session lock: an flock(LOCK_EX) on `session.lock` in the state directory. The only
/// owner of the lock's file descriptor, which is never dup'ed (a lock belongs to one open file
/// description, V16). Move-only RAII: a move transfers the lock, destruction (or assigning a
/// default-constructed lock) closes the descriptor and so releases it. Default-constructed, it
/// holds nothing.
class SessionLock {
public:
    SessionLock() = default;

    /// True while this object holds the lock.
    [[nodiscard]] bool held() const noexcept { return fd_.valid(); }

private:
    friend class StateDir; // StateDir::lock is the only way to take the lock

    explicit SessionLock(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

    UniqueFd fd_{};
};

/// The state directory, opened once: every file in it is reached with *at() calls relative to
/// one O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC descriptor (not O_PATH: it is fsynced, V17).
/// Move-only.
class StateDir {
public:
    /// `create`: missing components are made with mode 0700 (parents may be symlinks; the final
    /// component may not: it gives ENOTDIR or ELOOP, reported as symlink or not_regular). The
    /// final directory must be owned by the euid with (mode & 077) == 0. Without `create`, a
    /// missing directory is FileProblemKind::missing.
    [[nodiscard]] static FileResult<StateDir> open(const std::filesystem::path& path, bool create);

    /// `session.lock` (openat O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600), then
    /// flock(LOCK_EX|LOCK_NB) polled on `clock` every timing.poll until timing.timeout.
    /// interrupt_requested() or `stop` -> Errc::interrupted; timeout -> Errc::failure "another
    /// mangadex-dl is using the saved login; try again".
    [[nodiscard]] Result<SessionLock> lock(Clock& clock, LockTiming timing,
                                           std::stop_token stop) const;

    /// openat O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_CLOEXEC; a regular file owned by the
    /// euid with (mode & 077) == 0, at most max_session_file_bytes (auth.hpp), read straight into
    /// a pre-reserved Secret. nullopt when absent; ELOOP -> FileProblemKind::symlink.
    [[nodiscard]] FileResult<std::optional<Secret>> read(SessionSlot slot) const;

    /// `.<name>.<12 hex>` via openat O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC 0600 (bounded
    /// retries), fchmod 0600, write all, fsync, close (checked), renameat, fsync(dir); the temp is
    /// unlinkat'ed on every failure path. Errc::failure "<why>" on failure.
    [[nodiscard]] Result<void> write(SessionSlot slot, const Secret& bytes) const;

    /// Unlinks the slot's file; false when it was absent.
    [[nodiscard]] Result<bool> remove(SessionSlot slot) const;

    /// The path open() was given. Borrows from *this.
    [[nodiscard]] const std::filesystem::path& path() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return path_;
    }

private:
    /// For open() only: `fd` is the checked directory descriptor.
    StateDir(UniqueFd fd, std::filesystem::path path) noexcept
        : fd_(std::move(fd)), path_(std::move(path)) {}

    UniqueFd fd_{};
    std::filesystem::path path_{};
};

} // namespace mangadex_dl
