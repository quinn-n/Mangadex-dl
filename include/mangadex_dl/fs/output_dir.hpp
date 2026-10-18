// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output directory: no-clobber checks, temporary files and atomic publish, all relative to
// one directory file descriptor. Linux only (O_TMPFILE + linkat, renameat2 fallback).
// Frozen interface (download plan T1); implemented in src/core/fs/output_dir.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/zip.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

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
    written,  ///< the name did not exist; the file is now in place
    replaced, ///< overwrite: an existing file of that name was atomically replaced
    exists,   ///< no-clobber: the name already existed (file, symlink or directory); untouched
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

    /// A new temporary for the eventual `final_name` (one path component): O_TMPFILE where the
    /// filesystem supports it, else a named `.<final>.part-<12 hex>` file (O_EXCL, O_NOFOLLOW).
    [[nodiscard]] Result<TempFile> create_temp(std::string_view final_name);

    /// Makes `temp` visible as `final_name`, consuming it (on failure it is still discarded).
    /// No-clobber unless `overwrite`: an existing name gives PublishOutcome::exists. Then a
    /// best-effort fsync of the directory.
    [[nodiscard]] Result<PublishOutcome> publish(TempFile&& temp, std::string_view final_name,
                                                 bool overwrite);

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
