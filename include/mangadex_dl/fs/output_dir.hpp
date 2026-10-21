// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output directory: no-clobber checks, temporary files and atomic publish, all relative to
// one directory file descriptor. Linux only (O_TMPFILE + linkat, renameat2 fallback).
// Frozen interface (download plan T1, extended by skip-existing plan T1); implemented in
// src/core/fs/output_dir.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/zip.hpp"

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {

/// Owns one file descriptor (or none, -1) and closes it on destruction. Move-only; a moved-from
/// UniqueFd holds -1. Fully inline, so any core module may use it.
class UniqueFd {
public:
    UniqueFd() = default;
    /// Takes ownership of `fd` (-1 for none).
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        reset(other.release());
        return *this;
    }
    ~UniqueFd() { reset(); }

    /// The descriptor, still owned by *this.
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /// Gives up ownership: the caller now closes the returned descriptor.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    /// Closes the current descriptor (if any) and takes ownership of `fd`. A close error is
    /// ignored, and close is never retried on EINTR (on Linux the descriptor is gone either
    /// way). Callers that must see write errors fsync/close explicitly before this runs.
    void reset(int fd = -1) noexcept {
        const int old = std::exchange(fd_, fd);
        if (old >= 0 && old != fd) {
            static_cast<void>(::close(old));
        }
    }

private:
    int fd_ = -1;
};

/// How publish() ended.
enum class PublishOutcome {
    written,   ///< the name did not exist; the file is now in place
    replaced,  ///< overwrite: an existing file of that name was atomically replaced
    exists,    ///< no-clobber: the name already existed (file, symlink or directory); untouched
    directory, ///< overwrite: the name is a directory, which a file never replaces; untouched
};

/// What one name in an OutputDir is, without following symlinks (a symlink, even to a
/// directory, is replaced by an overwrite, never followed).
enum class EntryKind {
    absent,    ///< nothing has that name
    directory, ///< a directory: an overwrite can never replace it
    other,     ///< a regular file, symlink, FIFO, socket or device
};

/// What identifies one existing file between inspection and publish: the inode (device and
/// number) plus its size, modification and status-change times and its mode bits, as fstat
/// reported them. Native types, assigned straight from `struct stat`. Any chmod, link-count change
/// or rewrite updates the ctime, so it compares unequal (conservatively "changed"); comparing
/// `mode` as well therefore adds no new "changed" case.
struct FileIdentity {
    dev_t device{};
    ino_t inode{};
    off_t size{};
    time_t mtime_sec{};
    long mtime_nsec{};
    time_t ctime_sec{};
    long ctime_nsec{};
    /// `st_mode & 07777`: the permission bits plus set-user-ID, set-group-ID and sticky (the file
    /// type is not part of it). publish_replacing gives the new file the permission bits.
    mode_t mode{};

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

/// How OutputDir::open_existing ended.
enum class ExistingOpen {
    absent,      ///< no such name (or it vanished before the open)
    opened,      ///< a regular file, open read-only in ExistingHandle::fd
    not_regular, ///< a directory, symlink, FIFO, socket or device; never opened (or closed again)
    unreadable,  ///< EACCES, EPERM or EIO
};

/// The result of OutputDir::open_existing. Move-only (it owns the descriptor). A moved-from
/// handle keeps `status == opened` but its `fd` holds -1, so consumers check `fd.valid()`.
struct ExistingHandle {
    ExistingOpen status = ExistingOpen::absent;
    /// Valid only for opened (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC). For an
    /// update, keep it open until publish_replacing returns: `identity` is only meaningful while
    /// it is (V5).
    UniqueFd fd{};
    FileIdentity identity{}; ///< for opened: from fstat on `fd`
    int error = 0;           ///< the errno that decided unreadable, or not_regular when the
                             ///< open failed; 0 when fstat decided
};

/// How OutputDir::publish_replacing ended.
enum class ReplaceOutcome {
    replaced, ///< the name still had the expected identity and was atomically replaced
    written,  ///< the name had vanished; the file was published without clobbering
    changed,  ///< the name now holds something else; the temporary was discarded
};

class OutputDir;

/// A file being written in the output directory, invisible under its final name until
/// OutputDir::publish(). Created only by OutputDir::create_temp. Writes are buffered.
///
/// Destruction without a successful publish discards the data: the O_TMPFILE descriptor is
/// closed (the inode vanishes), or the fallback `.<final>.part-<hex>` name is unlinked. Move-only;
/// a moved-from TempFile owns nothing, its destructor is a no-op and write() fails.
///
/// Borrows the OutputDir's directory descriptor: the OutputDir must outlive every TempFile it
/// created. Not thread-safe. State lives in output_dir.cpp (pimpl).
class TempFile final : public ByteSink {
public:
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept;
    /// Discards the current temporary first (as the destructor would), then takes over `other`.
    TempFile& operator=(TempFile&& other) noexcept;
    ~TempFile() override;

    /// Errc::failure on ENOSPC, EIO and the like (EINTR is retried). `data` is only read during
    /// the call.
    [[nodiscard]] Result<void> write(std::span<const std::byte> data) override;

    /// Flushes the buffer and fsyncs; for the named fallback also closes and checks the close.
    /// publish() calls it if the caller has not.
    [[nodiscard]] Result<void> sync_and_close();

private:
    friend class OutputDir;
    struct State;

    explicit TempFile(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_{};
};

/// An open output directory. Move-only (it owns the directory descriptor); a moved-from
/// OutputDir may only be destroyed or assigned to.
class OutputDir {
public:
    /// Opens `path` as a directory (O_DIRECTORY | O_CLOEXEC). With `create`, missing components
    /// are created first with mode 0755 (subject to umask). Errc::failure (with the strerror
    /// text) when it is not a directory, not accessible or read-only.
    [[nodiscard]] static Result<OutputDir> open(const std::filesystem::path& path, bool create);

    /// Like open(path, false), but a missing directory is nullopt instead of an error (dry-run
    /// never creates DIR).
    [[nodiscard]] static Result<std::optional<OutputDir>>
    open_if_exists(const std::filesystem::path& path);

    /// Whether `name` (one path component) exists, without following symlinks: a dangling
    /// symlink exists.
    [[nodiscard]] Result<bool> exists(std::string_view name) const;

    /// What `name` (one path component) is, without following symlinks.
    [[nodiscard]] Result<EntryKind> entry_kind(std::string_view name) const;

    /// A new temporary for the eventual `final_name` (one path component): O_TMPFILE where the
    /// filesystem supports it, else a named `.<final>.part-<12 hex>` file (O_EXCL, O_NOFOLLOW).
    [[nodiscard]] Result<TempFile> create_temp(std::string_view final_name);

    /// Makes `temp` visible as `final_name`, consuming it (on failure it is still discarded).
    /// No-clobber unless `overwrite`: an existing name gives PublishOutcome::exists. With
    /// `overwrite`, a directory under that name gives PublishOutcome::directory (a file never
    /// replaces one). Either way the temporary is discarded and nothing changes. After a
    /// publish, a best-effort fsync of the directory.
    [[nodiscard]] Result<PublishOutcome> publish(TempFile&& temp, std::string_view final_name,
                                                 bool overwrite);

    /// Opens the existing `name` (one path component) for inspection, without following
    /// symlinks and without blocking on a FIFO: fstatat first (a non-regular name is never
    /// opened), then openat, then fstat on the descriptor. Errc::failure for any errno other
    /// than the ones ExistingOpen documents (EMFILE included).
    [[nodiscard]] Result<ExistingHandle> open_existing(std::string_view name) const;

    /// Publishes `temp` as `final_name`, consuming it (on failure or `changed` it is
    /// discarded). Replaces the name only while it still has the identity `expected`; publishes
    /// without clobbering when it vanished; leaves it alone otherwise. Then a best-effort fsync
    /// of the directory.
    ///
    /// The new file gets the permission bits of the inspected one, `expected.mode & 0777`, set
    /// with fchmod before the file is synced, so the umask does not apply (on `replaced` and on
    /// `written` alike). Set-user-ID, set-group-ID and sticky are never carried over, and owner,
    /// group, ACLs and extended attributes are not preserved: the new file is created by this
    /// process like any other. A failed fchmod is Errc::failure; the temporary is discarded
    /// and the existing file is left untouched.
    ///
    /// `expected` is meaningful only while the ExistingHandle descriptor it came from is still
    /// open: a filesystem may reuse a freed inode number at once (ext4 does), so the caller keeps
    /// that descriptor open until this returns. Between the final check and the rename there is
    /// a short window in which another process could still swap the name (accepted for a
    /// user-owned directory, cpp-security section 4).
    [[nodiscard]] Result<ReplaceOutcome>
    publish_replacing(TempFile&& temp, std::string_view final_name, const FileIdentity& expected);

    /// The path as given to open(). Borrows from *this.
    [[nodiscard]] const std::filesystem::path& path() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return path_;
    }

private:
    OutputDir(UniqueFd fd, std::filesystem::path path, bool fallback_temps) noexcept
        : fd_(std::move(fd)), path_(std::move(path)), fallback_temps_(fallback_temps) {}

    UniqueFd fd_{};
    std::filesystem::path path_{};
    bool fallback_temps_ = false; ///< skip O_TMPFILE and use named temporaries
};

} // namespace mangadex_dl
