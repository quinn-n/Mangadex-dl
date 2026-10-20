// SPDX-License-Identifier: GPL-3.0-or-later
//
// OutputDir and TempFile (download plan T8 + rev1): every operation is relative to one directory
// file descriptor, so a rename of DIR's path mid-run cannot redirect a write.
//
// Temporaries: O_TMPFILE (an unnamed inode, gone on close) where the filesystem supports it and
// /proc/self/fd is usable; otherwise a named `.<final>.part-<12 hex>` file created with O_EXCL |
// O_NOFOLLOW, unlinked on discard through a private dup of the directory descriptor.
//
// Publish (after write -> fsync -> close-check):
//   O_TMPFILE, no-clobber   linkat("/proc/self/fd/N", AT_SYMLINK_FOLLOW); EEXIST -> exists
//   O_TMPFILE, overwrite    linkat to a fresh `.part-` name, then renameat over the final name
//   named,     no-clobber   renameat2(RENAME_NOREPLACE); EINVAL/ENOSYS -> linkat + unlinkat
//   named,     overwrite    renameat
// then a best-effort fsync of the directory.
//
// Update in place (skip-existing plan T6): open_existing opens DIR/<name> read-only for inspection
// (fstatat pre-check, openat with O_NOFOLLOW | O_NONBLOCK, fstat re-check) and records its
// FileIdentity. publish_replacing stages the new file under a `.part-` name (an O_TMPFILE inode is
// linked to one first), re-checks the name with fstatat and renames over it only while the
// identity still matches; a vanished name gets a no-clobber publish, anything else is `changed`.
// Before the staging, the new file gets the inspected file's permission bits (fchmod on the
// temporary, before its fsync), so a rebuilt file keeps a `chmod 600`.
#include "mangadex_dl/fs/output_dir.hpp"

#include "fs/errno_text.hpp"
#include "fs/output_dir_testing.hpp"
#include "mangadex_dl/error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): renameat2/RENAME_NOREPLACE are in <stdio.h> only
#include <stdio.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

// Test-only fault switches (fs/output_dir_testing.hpp). Atomics, so flipping one is never a data
// race; relaxed ordering is enough because each is an independent flag that publishes nothing.
struct FaultSwitches {
    std::atomic<bool> fallback_tempfiles{false};
    std::atomic<bool> renameat2_unsupported{false};
    std::atomic<bool> proc_unavailable{false};
    std::atomic<bool> skip_existing_precheck{false};
    std::atomic<int> write_errno{0};
    std::atomic<int> fsync_errno{0};
    std::atomic<int> close_errno{0};
    std::atomic<int> chmod_errno{0};
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): test-only fault switches
constinit FaultSwitches g_faults{};

inline constexpr std::size_t write_buffer_bytes = std::size_t{1} << 20U;
inline constexpr std::size_t max_component_bytes = 255;
inline constexpr std::string_view part_infix = ".part-";
inline constexpr std::size_t random_suffix_bytes = 6; // 12 hex digits
inline constexpr int max_name_attempts = 16;
inline constexpr mode_t file_mode = 0644;
inline constexpr mode_t dir_mode = 0755;
/// The bits publish_replacing carries over from the replaced file: read, write and execute for
/// owner, group and others. Set-user-ID, set-group-ID and sticky are dropped, as `cp` without
/// `-p` and a write by a non-owner drop them: a new file is never made set-ID on the say-so of
/// whatever was under its name.
inline constexpr mode_t preserved_mode_bits = 0777;
/// The mode bits FileIdentity records: permissions plus set-ID and sticky, no file type.
inline constexpr mode_t identity_mode_bits = 07777;

/// "<what> '<name>': <strerror>".
[[nodiscard]] Error fs_error(std::string_view what, std::string_view name, int err) {
    return Error{.code = Errc::failure,
                 .message = std::format("{} '{}': {}", what, name, errno_text(err))};
}

[[nodiscard]] Error plain_error(std::string message) {
    return Error{.code = Errc::failure, .message = std::move(message)};
}

/// One path component: non-empty, at most 255 bytes, no '/', no NUL, not "." or "..".
[[nodiscard]] bool is_component(std::string_view name) noexcept {
    return !name.empty() && name.size() <= max_component_bytes && name != "." && name != ".." &&
           !name.contains('/') && !name.contains('\0');
}

[[nodiscard]] Error invalid_name(std::string_view name) {
    return plain_error(std::format("invalid file name '{}'", name));
}

/// openat(2). The only call of the variadic C function, so the vararg finding is suppressed in
/// one place; `mode` is used only with O_CREAT / O_TMPFILE and ignored otherwise.
[[nodiscard]] int open_at(int dir, const char* name, int flags, mode_t mode = 0) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic in C (the mode)
    return ::openat(dir, name, flags, mode);
}

/// A close-on-exec duplicate of `fd`, or -1 with errno set.
[[nodiscard]] int dup_cloexec(int fd) noexcept { return ::fcntl(fd, F_DUPFD_CLOEXEC, 0); }

/// write(2) until all of `data` is written; retries EINTR and short writes. 0 or an errno.
[[nodiscard]] int write_all(int fd, std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
        const ssize_t written = ::write(fd, data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (written == 0) {
            return EIO; // a regular file never accepts 0 of a non-empty write
        }
        data = data.subspan(static_cast<std::size_t>(written));
    }
    return 0;
}

/// fsync(2), retrying EINTR. 0 or an errno.
[[nodiscard]] int fsync_retry(int fd) noexcept {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) {
            return errno;
        }
    }
    return 0;
}

/// write_all for TempFile data, honouring the test fault switch. 0 or an errno.
[[nodiscard]] int temp_write(int fd, std::span<const std::byte> data) noexcept {
    if (const int fault = g_faults.write_errno.load(std::memory_order_relaxed); fault != 0) {
        return fault;
    }
    return write_all(fd, data);
}

/// fsync_retry for a TempFile, honouring the test fault switch. 0 or an errno.
[[nodiscard]] int temp_fsync(int fd) noexcept {
    if (const int fault = g_faults.fsync_errno.load(std::memory_order_relaxed); fault != 0) {
        return fault;
    }
    return fsync_retry(fd);
}

/// Closes a TempFile descriptor (always) and reports the close error, or the test fault. EINTR
/// still closes the descriptor on Linux, and the data was already fsynced, so it is success.
[[nodiscard]] int temp_close(int fd) noexcept {
    int err = 0;
    if (::close(fd) != 0 && errno != EINTR) {
        err = errno;
    }
    const int fault = g_faults.close_errno.load(std::memory_order_relaxed);
    return fault != 0 ? fault : err;
}

/// fchmod(2) of a TempFile, honouring the test fault switch. 0 or an errno.
[[nodiscard]] int temp_fchmod(int fd, mode_t mode) noexcept {
    if (const int fault = g_faults.chmod_errno.load(std::memory_order_relaxed); fault != 0) {
        return fault;
    }
    if (::fchmod(fd, mode) != 0) {
        return errno;
    }
    return 0;
}

/// 12 lowercase hex digits from getrandom(2).
[[nodiscard]] Result<std::string> random_hex() {
    std::array<unsigned char, random_suffix_bytes> bytes{};
    std::span<unsigned char> rest{bytes};
    while (!rest.empty()) {
        const ssize_t got = ::getrandom(rest.data(), rest.size(), 0);
        if (got < 0) {
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            return std::unexpected(
                plain_error(std::format("cannot get random bytes: {}", errno_text(err))));
        }
        rest = rest.subspan(static_cast<std::size_t>(got));
    }
    std::string hex{};
    hex.reserve(2 * bytes.size());
    for (const unsigned char byte : bytes) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

/// `.<final>.part-<hex>`, kept within one 255-byte component by shortening <final> on a UTF-8
/// character boundary.
[[nodiscard]] std::string temp_name_for(std::string_view final_name, std::string_view hex) {
    const std::size_t room = max_component_bytes - 1 - part_infix.size() - hex.size();
    std::size_t keep = std::min(final_name.size(), room);
    if (keep < final_name.size()) {
        // final_name[keep] is the first byte cut off; never cut inside a multi-byte character.
        while (keep > 0 && (static_cast<unsigned char>(final_name[keep]) & 0xC0U) == 0x80U) {
            --keep;
        }
    }
    return std::format(".{}{}{}", final_name.substr(0, keep), part_infix, hex);
}

/// Calls `attempt(name)` (returning 0 or an errno) with fresh temporary names for `final_name`
/// until it does not fail with EEXIST. The name that worked, or the errno of the last attempt
/// (EEXIST when every attempt collided). A getrandom failure is an Error.
template <class Attempt>
[[nodiscard]] Result<std::expected<std::string, int>> with_unique_name(std::string_view final_name,
                                                                       const Attempt& attempt) {
    int err = EEXIST;
    for (int i = 0; i < max_name_attempts && err == EEXIST; ++i) {
        Result<std::string> hex = random_hex();
        if (!hex) {
            return std::unexpected(std::move(hex).error());
        }
        std::string name = temp_name_for(final_name, *hex);
        err = attempt(name);
        if (err == 0) {
            return std::expected<std::string, int>{std::move(name)};
        }
    }
    return std::expected<std::string, int>{std::unexpect, err};
}

/// "/proc/self/fd/<fd>", built before any call that uses it.
[[nodiscard]] std::string proc_fd_path(int fd) { return std::format("/proc/self/fd/{}", fd); }

/// Whether the magic link for `fd` resolves, i.e. /proc is mounted and linkat can publish it.
[[nodiscard]] bool proc_fd_usable(int fd) {
    if (g_faults.proc_unavailable.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::string path = proc_fd_path(fd);
    struct stat st{};
    return ::fstatat(AT_FDCWD, path.c_str(), &st, 0) == 0;
}

/// linkat of an O_TMPFILE inode (through its /proc magic link) to `name` in `dir`. 0 or errno.
[[nodiscard]] int link_proc(const std::string& proc_path, int dir,
                            const std::string& name) noexcept {
    if (g_faults.proc_unavailable.load(std::memory_order_relaxed)) {
        return ENOENT;
    }
    if (::linkat(AT_FDCWD, proc_path.c_str(), dir, name.c_str(), AT_SYMLINK_FOLLOW) != 0) {
        return errno;
    }
    return 0;
}

/// Whether `name` exists in `dir` (not following symlinks). Errors count as "no": this only
/// decides between PublishOutcome::written and ::replaced.
[[nodiscard]] bool name_exists(int dir, const std::string& name) noexcept {
    struct stat st{};
    return ::fstatat(dir, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}

/// Unlinks `name` in `dir` on destruction unless dismissed. Borrows `dir`.
class UnlinkGuard {
public:
    UnlinkGuard(int dir, std::string name) noexcept : dir_(dir), name_(std::move(name)) {}
    UnlinkGuard(const UnlinkGuard&) = delete;
    UnlinkGuard& operator=(const UnlinkGuard&) = delete;
    UnlinkGuard(UnlinkGuard&&) = delete;
    UnlinkGuard& operator=(UnlinkGuard&&) = delete;
    ~UnlinkGuard() {
        if (armed_) {
            static_cast<void>(::unlinkat(dir_, name_.c_str(), 0));
        }
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void dismiss() noexcept { armed_ = false; }

private:
    int dir_ = -1;
    std::string name_{};
    bool armed_ = true;
};

/// The error for a failed linkat of an O_TMPFILE inode through /proc (ENOENT: /proc is gone).
[[nodiscard]] Error proc_publish_error(const std::string& final_name, int err) {
    if (err == ENOENT) {
        return plain_error(std::format("cannot publish '{}': /proc/self/fd is unavailable ({})",
                                       final_name, errno_text(err)));
    }
    return fs_error("cannot publish", final_name, err);
}

/// Links an O_TMPFILE inode (`tmp_fd`, already fsynced) to a fresh `.<final>.part-<hex>` name in
/// `dir`, so a single renameat can then move it over `final_name`. The new name; the caller
/// owns it (an UnlinkGuard) from here.
[[nodiscard]] Result<std::string> link_to_part_name(int tmp_fd, int dir,
                                                    const std::string& final_name) {
    const std::string proc_path = proc_fd_path(tmp_fd);
    Result<std::expected<std::string, int>> linked =
        with_unique_name(final_name, [&proc_path, dir](const std::string& name) noexcept {
            return link_proc(proc_path, dir, name);
        });
    if (!linked) {
        return std::unexpected(std::move(linked).error());
    }
    if (!linked->has_value()) {
        return std::unexpected(proc_publish_error(final_name, linked->error()));
    }
    return std::move(**linked);
}

/// Publishes an O_TMPFILE inode (`tmp_fd`, already fsynced) as `final_name` in `dir`.
[[nodiscard]] Result<PublishOutcome>
publish_tmpfile(int tmp_fd, int dir, const std::string& final_name, bool overwrite) {
    if (!overwrite) {
        const int err = link_proc(proc_fd_path(tmp_fd), dir, final_name);
        if (err == 0) {
            return PublishOutcome::written;
        }
        if (err == EEXIST) {
            return PublishOutcome::exists;
        }
        return std::unexpected(proc_publish_error(final_name, err));
    }

    // Overwrite: give the inode a temporary name, then rename it over the final name atomically.
    Result<std::string> linked = link_to_part_name(tmp_fd, dir, final_name);
    if (!linked) {
        return std::unexpected(std::move(linked).error());
    }
    UnlinkGuard temp_name{dir, std::move(*linked)};
    const bool existed = name_exists(dir, final_name);
    if (::renameat(dir, temp_name.name().c_str(), dir, final_name.c_str()) != 0) {
        return std::unexpected(fs_error("cannot publish", final_name, errno));
    }
    temp_name.dismiss();
    return existed ? PublishOutcome::replaced : PublishOutcome::written;
}

/// Publishes the named temporary `temp_name` (in `temp_dir`, already closed) as `final_name` in
/// `dir`. Clears `temp_name` once the temporary name is gone, so the discard does not unlink it.
[[nodiscard]] Result<PublishOutcome> publish_named(int temp_dir, std::string& temp_name, int dir,
                                                   const std::string& final_name, bool overwrite) {
    if (overwrite) {
        const bool existed = name_exists(dir, final_name);
        if (::renameat(temp_dir, temp_name.c_str(), dir, final_name.c_str()) != 0) {
            return std::unexpected(fs_error("cannot publish", final_name, errno));
        }
        temp_name.clear();
        return existed ? PublishOutcome::replaced : PublishOutcome::written;
    }

    int err = EINVAL;
    if (!g_faults.renameat2_unsupported.load(std::memory_order_relaxed)) {
        if (::renameat2(temp_dir, temp_name.c_str(), dir, final_name.c_str(), RENAME_NOREPLACE) ==
            0) {
            temp_name.clear();
            return PublishOutcome::written;
        }
        err = errno;
    }
    if (err == EEXIST) {
        return PublishOutcome::exists;
    }
    if (err != EINVAL && err != ENOSYS) {
        return std::unexpected(fs_error("cannot publish", final_name, err));
    }

    // No RENAME_NOREPLACE on this filesystem: a hard link also refuses an existing name.
    if (::linkat(temp_dir, temp_name.c_str(), dir, final_name.c_str(), 0) != 0) {
        err = errno;
        if (err == EEXIST) {
            return PublishOutcome::exists;
        }
        return std::unexpected(fs_error("cannot publish", final_name, err));
    }
    // The file is published; dropping the temporary name is best effort (the discard retries).
    if (::unlinkat(temp_dir, temp_name.c_str(), 0) == 0) {
        temp_name.clear();
    }
    return PublishOutcome::written;
}

/// Opens `path` as a directory descriptor for reading (O_RDONLY | O_DIRECTORY), or the errno.
[[nodiscard]] std::expected<UniqueFd, int> open_directory(const std::filesystem::path& path) {
    const std::string name = path.empty() ? std::string{"."} : path.string();
    const int fd = open_at(AT_FDCWD, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(errno);
    }
    return UniqueFd{fd};
}

/// mkdir -p, one component at a time relative to the previous component's descriptor (0755,
/// subject to umask), then opens the result for reading.
[[nodiscard]] Result<UniqueFd> make_directories(const std::filesystem::path& path) {
    UniqueFd current{};
    std::filesystem::path walked = path.root_path();
    if (path.has_root_directory()) {
        current.reset(open_at(AT_FDCWD, "/", O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (!current.valid()) {
            return std::unexpected(fs_error("cannot open directory", "/", errno));
        }
    }
    for (const std::filesystem::path& part : path.relative_path()) {
        const std::string name = part.string();
        if (name.empty()) {
            continue; // a trailing separator
        }
        walked /= part;
        const int at = current.valid() ? current.get() : AT_FDCWD;
        int mkdir_err = 0;
        if (name != "." && name != ".." && ::mkdirat(at, name.c_str(), dir_mode) != 0) {
            mkdir_err = errno; // EEXIST is the common, harmless case
        }
        const int next = open_at(at, name.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (next < 0) {
            const int open_err = errno;
            if (mkdir_err != 0 && mkdir_err != EEXIST) {
                return std::unexpected(
                    fs_error("cannot create directory", walked.string(), mkdir_err));
            }
            return std::unexpected(fs_error("cannot open directory", walked.string(), open_err));
        }
        current.reset(next);
    }
    const int at = current.valid() ? current.get() : AT_FDCWD;
    const int fd = open_at(at, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(fs_error("cannot open directory", path.string(), errno));
    }
    return UniqueFd{fd};
}

/// The directory must accept new entries (write + search for us; not on a read-only mount).
[[nodiscard]] Result<void> check_writable(int fd, const std::filesystem::path& path) {
    if (::faccessat(fd, ".", W_OK | X_OK, AT_EACCESS) != 0) {
        const int err = errno;
        return std::unexpected(plain_error(std::format("output directory '{}' is not writable: {}",
                                                       path.string(), errno_text(err))));
    }
    return {};
}

/// The errnos that make an existing name `unreadable` (shown to the user as "cannot be read").
[[nodiscard]] bool is_unreadable_errno(int err) noexcept {
    return err == EACCES || err == EPERM || err == EIO;
}

/// The openat errnos that mean "not a regular file": ELOOP for any symlink under O_NOFOLLOW
/// (dangling and looping ones too), ENXIO for a UNIX socket or a char device without a driver,
/// ENODEV for a device without one.
[[nodiscard]] bool is_not_regular_open_errno(int err) noexcept {
    return err == ELOOP || err == ENXIO || err == ENODEV;
}

/// The identity of the file `st` describes; every field assigned from a `struct stat` lvalue of
/// the same type.
[[nodiscard]] FileIdentity identity_of(const struct stat& st) noexcept {
    return FileIdentity{.device = st.st_dev,
                        .inode = st.st_ino,
                        .size = st.st_size,
                        .mtime_sec = st.st_mtim.tv_sec,
                        .mtime_nsec = st.st_mtim.tv_nsec,
                        .ctime_sec = st.st_ctim.tv_sec,
                        .ctime_nsec = st.st_ctim.tv_nsec,
                        .mode = st.st_mode & identity_mode_bits};
}

} // namespace

namespace testing {

void force_fallback_tempfiles(bool on) noexcept {
    g_faults.fallback_tempfiles.store(on, std::memory_order_relaxed);
}

void force_renameat2_unsupported(bool on) noexcept {
    g_faults.renameat2_unsupported.store(on, std::memory_order_relaxed);
}

void force_proc_unavailable(bool on) noexcept {
    g_faults.proc_unavailable.store(on, std::memory_order_relaxed);
}

void force_skip_existing_precheck(bool on) noexcept {
    g_faults.skip_existing_precheck.store(on, std::memory_order_relaxed);
}

void force_io_error(IoStage stage, int err) noexcept {
    switch (stage) {
    case IoStage::write:
        g_faults.write_errno.store(err, std::memory_order_relaxed);
        return;
    case IoStage::fsync:
        g_faults.fsync_errno.store(err, std::memory_order_relaxed);
        return;
    case IoStage::close:
        g_faults.close_errno.store(err, std::memory_order_relaxed);
        return;
    case IoStage::chmod:
        g_faults.chmod_errno.store(err, std::memory_order_relaxed);
        return;
    }
}

} // namespace testing

// ---- TempFile
// ------------------------------------------------------------------------------------

struct TempFile::State {
    UniqueFd fd{};            ///< the open temporary; -1 once a named temporary is closed
    UniqueFd dir{};           ///< named only: a private dup of the directory, for the unlink
    std::string temp_name{};  ///< named only: `.<final>.part-<hex>`; empty once it is gone
    std::string final_name{}; ///< for messages
    std::vector<std::byte> buffer{};
    bool named = false;  ///< a named fallback temporary (else O_TMPFILE)
    bool synced = false; ///< sync_and_close() succeeded: no more writes
    bool failed = false; ///< a write, sync or close failed: the data can only be discarded

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    /// Discards an unpublished temporary: the named file is unlinked here, and closing `fd`
    /// (member destruction, afterwards) drops an O_TMPFILE inode.
    ~State() {
        if (!temp_name.empty() && dir.valid()) {
            static_cast<void>(::unlinkat(dir.get(), temp_name.c_str(), 0));
        }
    }

    [[nodiscard]] Result<void> flush() {
        if (buffer.empty()) {
            return {};
        }
        const int err = temp_write(fd.get(), buffer);
        buffer.clear();
        if (err != 0) {
            failed = true;
            return std::unexpected(fs_error("cannot write", final_name, err));
        }
        return {};
    }

    /// Gives the temporary the permission bits `mode` with fchmod (the umask does not apply).
    /// Called before sync_and_close, the open descriptor is used and the fsync there makes the
    /// mode durable. A no-op on a failed temporary: sync_and_close reports that failure.
    ///
    /// A named temporary the caller already synced is closed by then. It is reopened by name
    /// through the private directory descriptor and fsynced again after the fchmod. O_NOFOLLOW
    /// refuses a symlink planted under the `.part-` name, and only a regular file with a single
    /// link is changed, so a hard link to another file of ours is refused too (fchmod itself
    /// needs ownership, which rules out anything else).
    [[nodiscard]] Result<void> set_mode(mode_t mode) {
        if (failed) {
            return {};
        }
        UniqueFd reopened{};
        int target = fd.get();
        if (!fd.valid()) {
            reopened.reset(open_at(dir.get(), temp_name.c_str(),
                                   O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
            if (!reopened.valid()) {
                return std::unexpected(fs_error("cannot set the mode of", final_name, errno));
            }
            struct stat st{};
            if (::fstat(reopened.get(), &st) != 0) {
                return std::unexpected(fs_error("cannot set the mode of", final_name, errno));
            }
            if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
                return std::unexpected(plain_error(std::format(
                    "cannot set the mode of '{}': its temporary file was replaced", final_name)));
            }
            target = reopened.get();
        }
        if (const int err = temp_fchmod(target, mode); err != 0) {
            return std::unexpected(fs_error("cannot set the mode of", final_name, err));
        }
        if (synced) {
            if (const int err = temp_fsync(target); err != 0) {
                return std::unexpected(fs_error("cannot sync", final_name, err));
            }
        }
        return {};
    }
};

TempFile::TempFile(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TempFile::TempFile(TempFile&& other) noexcept = default;
TempFile& TempFile::operator=(TempFile&& other) noexcept = default;
TempFile::~TempFile() = default;

Result<void> TempFile::write(std::span<const std::byte> data) {
    if (!state_) {
        return std::unexpected(plain_error("write to a discarded temporary file"));
    }
    State& s = *state_;
    if (s.failed) {
        return std::unexpected(
            plain_error(std::format("cannot write '{}': an earlier write failed", s.final_name)));
    }
    if (s.synced) {
        return std::unexpected(plain_error(
            std::format("cannot write '{}': the temporary file is closed", s.final_name)));
    }
    if (data.size() > write_buffer_bytes - s.buffer.size()) {
        if (Result<void> flushed = s.flush(); !flushed) {
            return flushed;
        }
        if (data.size() >= write_buffer_bytes) {
            if (const int err = temp_write(s.fd.get(), data); err != 0) {
                s.failed = true;
                return std::unexpected(fs_error("cannot write", s.final_name, err));
            }
            return {};
        }
    }
    if (s.buffer.capacity() < write_buffer_bytes) {
        s.buffer.reserve(write_buffer_bytes);
    }
    s.buffer.insert(s.buffer.end(), data.begin(), data.end());
    return {};
}

Result<void> TempFile::sync_and_close() {
    if (!state_) {
        return std::unexpected(plain_error("sync of a discarded temporary file"));
    }
    State& s = *state_;
    if (s.synced) {
        return {};
    }
    if (s.failed) {
        return std::unexpected(
            plain_error(std::format("cannot write '{}': an earlier write failed", s.final_name)));
    }
    if (Result<void> flushed = s.flush(); !flushed) {
        return flushed;
    }
    if (const int err = temp_fsync(s.fd.get()); err != 0) {
        s.failed = true;
        return std::unexpected(fs_error("cannot sync", s.final_name, err));
    }
    if (s.named) {
        // Close and check: some filesystems (NFS) report write errors only here.
        if (const int err = temp_close(s.fd.release()); err != 0) {
            s.failed = true;
            return std::unexpected(fs_error("cannot close", s.final_name, err));
        }
    }
    s.synced = true;
    return {};
}

// ---- OutputDir
// -----------------------------------------------------------------------------------

Result<OutputDir> OutputDir::open(const std::filesystem::path& path, bool create) {
    UniqueFd fd{};
    if (create) {
        Result<UniqueFd> made = make_directories(path);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        fd = std::move(*made);
    } else {
        std::expected<UniqueFd, int> opened = open_directory(path);
        if (!opened) {
            return std::unexpected(
                fs_error("cannot open directory", path.string(), opened.error()));
        }
        fd = std::move(*opened);
    }
    if (Result<void> writable = check_writable(fd.get(), path); !writable) {
        return std::unexpected(std::move(writable).error());
    }
    return OutputDir{std::move(fd), path,
                     g_faults.fallback_tempfiles.load(std::memory_order_relaxed)};
}

Result<std::optional<OutputDir>> OutputDir::open_if_exists(const std::filesystem::path& path) {
    std::expected<UniqueFd, int> opened = open_directory(path);
    if (!opened) {
        if (opened.error() == ENOENT) {
            return std::optional<OutputDir>{};
        }
        return std::unexpected(fs_error("cannot open directory", path.string(), opened.error()));
    }
    if (Result<void> writable = check_writable(opened->get(), path); !writable) {
        return std::unexpected(std::move(writable).error());
    }
    return std::optional<OutputDir>{OutputDir{
        std::move(*opened), path, g_faults.fallback_tempfiles.load(std::memory_order_relaxed)}};
}

Result<bool> OutputDir::exists(std::string_view name) const {
    if (!is_component(name)) {
        return std::unexpected(invalid_name(name));
    }
    const std::string component{name};
    struct stat st{};
    if (::fstatat(fd_.get(), component.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        return true;
    }
    const int err = errno;
    if (err == ENOENT) {
        return false;
    }
    return std::unexpected(fs_error("cannot check", component, err));
}

Result<TempFile> OutputDir::create_temp(std::string_view final_name) {
    if (!is_component(final_name)) {
        return std::unexpected(invalid_name(final_name));
    }
    // The State exists before any file does, so nothing below can leak a created file.
    auto state = std::make_unique<TempFile::State>();
    state->final_name = std::string{final_name};

    if (!fallback_temps_) {
        const int fd = open_at(fd_.get(), ".", O_TMPFILE | O_WRONLY | O_CLOEXEC, file_mode);
        if (fd >= 0) {
            state->fd.reset(fd);
            if (proc_fd_usable(fd)) {
                return TempFile{std::move(state)};
            }
            // Without /proc the inode could never be linked: drop it, use a named temporary.
            state->fd.reset();
        } else {
            const int err = errno;
            if (err != EOPNOTSUPP && err != EISDIR && err != EINVAL) {
                return std::unexpected(
                    fs_error("cannot create a temporary file for", state->final_name, err));
            }
        }
    }

    state->named = true;
    state->dir.reset(dup_cloexec(fd_.get()));
    if (!state->dir.valid()) {
        return std::unexpected(
            fs_error("cannot create a temporary file for", state->final_name, errno));
    }
    TempFile::State& s = *state;
    Result<std::expected<std::string, int>> created =
        with_unique_name(s.final_name, [&s](const std::string& name) noexcept {
            const int fd = open_at(s.dir.get(), name.c_str(),
                                   O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, file_mode);
            if (fd < 0) {
                return errno;
            }
            s.fd.reset(fd);
            return 0;
        });
    if (!created) {
        return std::unexpected(std::move(created).error());
    }
    if (!created->has_value()) {
        return std::unexpected(
            fs_error("cannot create a temporary file for", s.final_name, created->error()));
    }
    // From here the State owns the name: discarding the TempFile unlinks it.
    s.temp_name = std::move(**created);
    return TempFile{std::move(state)};
}

Result<PublishOutcome> OutputDir::publish(TempFile&& temp, std::string_view final_name,
                                          bool overwrite) {
    // Consume `temp` on every path: `local` discards it unless it was published.
    TempFile local{std::move(temp)};
    if (!local.state_) {
        return std::unexpected(plain_error("publish of a discarded temporary file"));
    }
    if (!is_component(final_name)) {
        return std::unexpected(invalid_name(final_name));
    }
    if (Result<void> synced = local.sync_and_close(); !synced) {
        return std::unexpected(std::move(synced).error());
    }
    TempFile::State& s = *local.state_;
    const std::string name{final_name};
    Result<PublishOutcome> outcome =
        s.named ? publish_named(s.dir.get(), s.temp_name, fd_.get(), name, overwrite)
                : publish_tmpfile(s.fd.get(), fd_.get(), name, overwrite);
    if (outcome && *outcome != PublishOutcome::exists) {
        // Best effort: make the new directory entry durable. Some filesystems refuse a directory
        // fsync (EINVAL); the file itself is already synced.
        static_cast<void>(fsync_retry(fd_.get()));
    }
    return outcome;
}

Result<ExistingHandle> OutputDir::open_existing(std::string_view name) const {
    if (!is_component(name)) {
        return std::unexpected(invalid_name(name));
    }
    const std::string component{name};

    // 1. Classify without opening: a FIFO, socket, device, directory or symlink is never opened.
    if (!g_faults.skip_existing_precheck.load(std::memory_order_relaxed)) {
        struct stat st{};
        if (::fstatat(fd_.get(), component.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            const int err = errno;
            if (err == ENOENT) {
                return ExistingHandle{.status = ExistingOpen::absent};
            }
            if (is_unreadable_errno(err)) {
                return ExistingHandle{.status = ExistingOpen::unreadable, .error = err};
            }
            return std::unexpected(fs_error("cannot check", component, err));
        }
        if (!S_ISREG(st.st_mode)) {
            return ExistingHandle{.status = ExistingOpen::not_regular};
        }
    }

    // 2. Open. The name may have changed since the pre-check: O_NOFOLLOW refuses a symlink and
    // O_NONBLOCK keeps a FIFO from blocking (it has no effect on a regular file).
    // O_NOCTTY: a terminal device swapped in here never becomes the controlling terminal.
    UniqueFd fd{open_at(fd_.get(), component.c_str(),
                        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)};
    if (!fd.valid()) {
        const int err = errno;
        if (err == ENOENT) {
            return ExistingHandle{.status = ExistingOpen::absent};
        }
        if (is_not_regular_open_errno(err)) {
            return ExistingHandle{.status = ExistingOpen::not_regular, .error = err};
        }
        if (is_unreadable_errno(err)) {
            return ExistingHandle{.status = ExistingOpen::unreadable, .error = err};
        }
        return std::unexpected(fs_error("cannot open", component, err));
    }

    // 3. Re-check what was opened (a directory, FIFO or char device opens successfully) and
    // record its identity. A non-regular file is closed again when `fd` goes out of scope.
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        const int err = errno;
        if (is_unreadable_errno(err)) {
            return ExistingHandle{.status = ExistingOpen::unreadable, .error = err};
        }
        return std::unexpected(fs_error("cannot check", component, err));
    }
    if (!S_ISREG(st.st_mode)) {
        return ExistingHandle{.status = ExistingOpen::not_regular};
    }
    return ExistingHandle{
        .status = ExistingOpen::opened, .fd = std::move(fd), .identity = identity_of(st)};
}

Result<ReplaceOutcome> OutputDir::publish_replacing(TempFile&& temp, std::string_view final_name,
                                                    const FileIdentity& expected) {
    // Consume `temp` on every path: `local` discards it unless it was published.
    TempFile local{std::move(temp)};
    if (!local.state_) {
        return std::unexpected(plain_error("publish of a discarded temporary file"));
    }
    if (!is_component(final_name)) {
        return std::unexpected(invalid_name(final_name));
    }
    TempFile::State& s = *local.state_;
    // Keep the inspected file's permission bits (a user's `chmod 600` must survive a rebuild),
    // before the sync so that its fsync covers the mode. On error `local` discards the new
    // content and the existing file is untouched.
    if (Result<void> moded = s.set_mode(expected.mode & preserved_mode_bits); !moded) {
        return std::unexpected(std::move(moded).error());
    }
    if (Result<void> synced = local.sync_and_close(); !synced) {
        return std::unexpected(std::move(synced).error());
    }
    const std::string name{final_name};

    // Stage the new content under a name of its own before the check, so that the replacement
    // after it is a single renameat. A named temporary already has one (the State unlinks it on
    // discard); an O_TMPFILE inode is linked to a fresh `.part-` name, which `staged` unlinks
    // unless the rename consumes it.
    std::optional<UnlinkGuard> staged{};
    if (!s.named) {
        Result<std::string> linked = link_to_part_name(s.fd.get(), fd_.get(), name);
        if (!linked) {
            return std::unexpected(std::move(linked).error());
        }
        staged.emplace(fd_.get(), std::move(*linked));
    }

    // The final check. `expected` came from an fstat on a descriptor the caller still holds, so
    // its inode number cannot have been reused (V5). From here to the renameat below another
    // process could still swap the name: a window of microseconds, accepted for a user-owned
    // directory (cpp-security section 4). Not tested on overlayfs (V6).
    struct stat st{};
    if (::fstatat(fd_.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        const int err = errno;
        if (err != ENOENT) {
            return std::unexpected(fs_error("cannot check", name, err));
        }
        // The name vanished: publish without clobbering whatever may appear meanwhile.
        Result<PublishOutcome> published =
            s.named ? publish_named(s.dir.get(), s.temp_name, fd_.get(), name, false)
                    : publish_tmpfile(s.fd.get(), fd_.get(), name, false);
        if (!published) {
            return std::unexpected(std::move(published).error());
        }
        if (*published == PublishOutcome::exists) {
            return ReplaceOutcome::changed;
        }
        static_cast<void>(fsync_retry(fd_.get())); // best effort, as in publish()
        return ReplaceOutcome::written;
    }
    if (!S_ISREG(st.st_mode) || identity_of(st) != expected) {
        return ReplaceOutcome::changed; // `staged` and `local` discard the new content
    }

    if (staged.has_value()) {
        if (::renameat(fd_.get(), staged->name().c_str(), fd_.get(), name.c_str()) != 0) {
            return std::unexpected(fs_error("cannot publish", name, errno));
        }
        staged->dismiss();
    } else {
        if (::renameat(s.dir.get(), s.temp_name.c_str(), fd_.get(), name.c_str()) != 0) {
            return std::unexpected(fs_error("cannot publish", name, errno));
        }
        s.temp_name.clear();
    }
    // Best effort: make the new directory entry durable (see publish()).
    static_cast<void>(fsync_retry(fd_.get()));
    return ReplaceOutcome::replaced;
}

} // namespace mangadex_dl
