// SPDX-License-Identifier: GPL-3.0-or-later
//
// The read-only pre-flight for one batch row's output directory: is a failure to use it scoped to
// that row (the batch continues) or global (the batch stops)? Frozen interface (batch plan T1,
// rev1); implemented in src/core/fs/row_dir.cpp (T2).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mangadex_dl {

enum class RowDirKind {
    ok,           ///< usable as far as a read-only check can tell (or not created yet)
    row_error,    ///< only this row's `dir` cell is unusable: the row fails, the batch continues
    global_error, ///< DIR itself, or the filesystem, is unusable: the batch stops
};

struct RowDirVerdict {
    RowDirKind kind = RowDirKind::ok;
    /// One line, raw bytes ("cannot open directory '<path>': <strerror>" or "cannot write to
    /// directory '<path>': <strerror>"); empty when ok.
    std::string message{};

    friend bool operator==(const RowDirVerdict&, const RowDirVerdict&) = default;
};

/// Read-only pre-flight for one row's output directory root/sub (`sub` already passed
/// validate_list_dir; empty = the row has no `dir` cell). Creates and modifies NOTHING.
/// 1. open(root, O_RDONLY|O_DIRECTORY|O_CLOEXEC), symlinks followed: ENOENT -> ok (download
///    creates it later; nothing below it can exist); any other failure -> global_error.
/// 2. For each component of `sub`: openat(parent, c, O_RDONLY|O_DIRECTORY|O_CLOEXEC), symlinks
///    followed (OD1). ENOENT where fstatat(AT_SYMLINK_NOFOLLOW) also says ENOENT -> the walk
///    ends, ok. A dangling symlink, or any other errno -> classify_dir_errno(err,
///    below_root = true).
/// 3. faccessat(deepest existing dir fd, ".", W_OK|X_OK, AT_EACCESS) ->
///    classify_dir_errno(err, below_root = the deepest directory is not root).
[[nodiscard]] RowDirVerdict check_row_dir(const std::filesystem::path& root, std::string_view sub);

/// The pure errno table: ENOTDIR, ELOOP, ENAMETOOLONG, EACCES, EPERM -> row_error when
/// below_root, else global_error; everything else (ENOSPC, EIO, EROFS, EDQUOT, EMFILE, ENFILE,
/// ENOMEM, unknown) -> global_error. Never returns ok.
[[nodiscard]] RowDirKind classify_dir_errno(int err, bool below_root) noexcept;

} // namespace mangadex_dl
