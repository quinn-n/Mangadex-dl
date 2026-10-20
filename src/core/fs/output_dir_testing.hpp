// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private testing seam for src/core/fs/output_dir.cpp (download plan T8, skip-existing plan T6,
// IoStage::chmod from the mode-preserving replace).
// Not part of the public interface: only output_dir.cpp and the unit tests include it. Each switch
// is a process-wide fault injection that forces one of the paths a normal Linux test machine never
// takes (a filesystem without O_TMPFILE or RENAME_NOREPLACE, a system without /proc, a name that
// changes type between two system calls).
//
// The switches are atomics, so flipping one is never a data race, but they are global: set them
// only from single-threaded test code, and restore them (ScopedOutputDirFault) before the test
// ends. Production code never calls these; every switch defaults to off.
#pragma once

namespace mangadex_dl::testing {

/// While on, OutputDir::open / open_if_exists return directories that skip O_TMPFILE and use the
/// named `.<final>.part-<hex>` temporaries. Read once per open(): an OutputDir keeps the mode it
/// was opened with.
void force_fallback_tempfiles(bool on) noexcept;

/// While on, the fallback no-clobber publish behaves as if renameat2(RENAME_NOREPLACE) had failed
/// with EINVAL (unsupported filesystem), so publish takes the linkat + unlinkat path.
void force_renameat2_unsupported(bool on) noexcept;

/// While on, every use of /proc/self/fd/<n> behaves as if /proc were not mounted (ENOENT): the
/// O_TMPFILE probe in create_temp falls back to a named temporary, and publishing an O_TMPFILE
/// temporary fails with Errc::failure.
void force_proc_unavailable(bool on) noexcept;

/// While on, OutputDir::open_existing skips its fstatat pre-check and goes straight to openat
/// (skip-existing plan T6), so tests reach the openat errno mapping (ELOOP for a symlink, ENXIO for
/// a socket) and the post-open fstat re-check (a directory or FIFO opens, then is closed again).
void force_skip_existing_precheck(bool on) noexcept;

/// The TempFile system calls force_io_error can fail.
enum class IoStage {
    write, ///< write(2) of buffered or direct data
    fsync, ///< fsync(2) of the temporary in sync_and_close()
    close, ///< close(2) of a named fallback temporary in sync_and_close() (O_TMPFILE never closes
           ///< there: its descriptor is needed for the publishing linkat)
    chmod, ///< fchmod(2) of the temporary in publish_replacing() (keeping the old file's mode)
};

/// While `err` is nonzero, every TempFile call of `stage` fails with errno `err` (ENOSPC, EIO,
/// ...) instead of reaching the kernel; a faulted close still closes the descriptor. 0 turns the
/// fault off. Never affects the directory fsync after a publish.
void force_io_error(IoStage stage, int err) noexcept;

/// Sets one switch for the lifetime of the object and turns it off again on destruction.
class ScopedOutputDirFault {
public:
    using Setter = void (*)(bool) noexcept;

    explicit ScopedOutputDirFault(Setter setter) noexcept : setter_(setter) {
        if (setter_ != nullptr) {
            setter_(true);
        }
    }
    ScopedOutputDirFault(const ScopedOutputDirFault&) = delete;
    ScopedOutputDirFault& operator=(const ScopedOutputDirFault&) = delete;
    ScopedOutputDirFault(ScopedOutputDirFault&&) = delete;
    ScopedOutputDirFault& operator=(ScopedOutputDirFault&&) = delete;
    ~ScopedOutputDirFault() {
        if (setter_ != nullptr) {
            setter_(false);
        }
    }

private:
    Setter setter_ = nullptr;
};

/// Fails one TempFile system call with `err` for the lifetime of the object.
class ScopedIoError {
public:
    ScopedIoError(IoStage stage, int err) noexcept : stage_(stage) { force_io_error(stage_, err); }
    ScopedIoError(const ScopedIoError&) = delete;
    ScopedIoError& operator=(const ScopedIoError&) = delete;
    ScopedIoError(ScopedIoError&&) = delete;
    ScopedIoError& operator=(ScopedIoError&&) = delete;
    ~ScopedIoError() { force_io_error(stage_, 0); }

private:
    IoStage stage_ = IoStage::write;
};

} // namespace mangadex_dl::testing
