// SPDX-License-Identifier: GPL-3.0-or-later
//
// The read-only pre-flight for one batch row's output directory (batch plan T2, rev1).
//
// It walks DIR and then each component of the row's `dir` cell with openat(O_DIRECTORY),
// following symlinks (OD1), and ends with an access check on the deepest directory that exists.
// It never creates, modifies or removes anything: the only calls are open/openat, fstatat and
// faccessat. Every directory descriptor is a UniqueFd; the parent is closed as soon as the child
// is open, and openat only ever borrows the live parent.
#include "mangadex_dl/fs/row_dir.hpp"

#include "mangadex_dl/fs/output_dir.hpp"

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

inline constexpr int dir_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;

[[nodiscard]] std::string errno_text(int err) { return std::system_category().message(err); }

/// open(2)/openat(2) without O_CREAT: the variadic mode argument is never read. Both names are
/// NUL-terminated (a std::filesystem::path and a std::string own their buffers).
[[nodiscard]] int open_dir(const std::filesystem::path& path) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    return ::open(path.c_str(), dir_flags);
}

[[nodiscard]] int open_dir_at(int parent, const std::string& name) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic in C (the mode)
    return ::openat(parent, name.c_str(), dir_flags);
}

/// The non-empty '/'-separated components of `sub`, each an owned (NUL-terminated) string.
/// `sub` already passed validate_list_dir, so empty components are not expected; skipping them
/// keeps the walk well-defined anyway.
[[nodiscard]] std::vector<std::string> split_components(std::string_view sub) {
    std::vector<std::string> parts{};
    while (!sub.empty()) {
        const std::size_t slash = sub.find('/');
        const std::string_view part = sub.substr(0, slash);
        if (!part.empty()) {
            parts.emplace_back(part);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        sub.remove_prefix(slash + 1);
    }
    return parts;
}

/// What the pre-flight was doing when it failed: the start of every message.
enum class DirStep {
    open,  ///< open/openat (or the fstatat behind a dangling-symlink check)
    write, ///< the faccessat write/search check on the deepest directory
};

[[nodiscard]] constexpr std::string_view step_text(DirStep step) noexcept {
    switch (step) {
    case DirStep::open:
        return "cannot open directory";
    case DirStep::write:
        return "cannot write to directory";
    }
    return "cannot open directory"; // only reachable through an out-of-range enum value
}

/// "cannot open directory '<path>': <why>" / "cannot write to directory '<path>': <why>". The
/// path is raw bytes: the app sanitizes the whole line when it prints it.
[[nodiscard]] RowDirVerdict verdict(RowDirKind kind, DirStep step,
                                    const std::filesystem::path& path, std::string_view why) {
    return RowDirVerdict{
        .kind = kind, .message = std::format("{} '{}': {}", step_text(step), path.string(), why)};
}

[[nodiscard]] RowDirVerdict errno_verdict(DirStep step, const std::filesystem::path& path, int err,
                                          bool below_root) {
    return verdict(classify_dir_errno(err, below_root), step, path, errno_text(err));
}

} // namespace

RowDirKind classify_dir_errno(int err, bool below_root) noexcept {
    switch (err) {
    case ENOTDIR:
    case ELOOP:
    case ENAMETOOLONG:
    case EACCES:
    case EPERM:
        return below_root ? RowDirKind::row_error : RowDirKind::global_error;
    default:
        return RowDirKind::global_error;
    }
}

RowDirVerdict check_row_dir(const std::filesystem::path& root, std::string_view sub) {
    UniqueFd current{open_dir(root)};
    if (!current.valid()) {
        const int err = errno;
        if (err == ENOENT) {
            return RowDirVerdict{}; // download creates DIR later; nothing below it can exist
        }
        return verdict(RowDirKind::global_error, DirStep::open, root, errno_text(err));
    }

    std::filesystem::path deepest = root;
    bool below_root = false;
    for (const std::string& component : split_components(sub)) {
        std::filesystem::path next = deepest / component;
        const int fd = open_dir_at(current.get(), component);
        if (fd < 0) {
            const int err = errno;
            if (err != ENOENT) {
                return errno_verdict(DirStep::open, next, err, true);
            }
            // ENOENT with symlinks followed: a missing name, or a symlink whose target is missing.
            struct stat st{};
            if (::fstatat(current.get(), component.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
                return verdict(RowDirKind::row_error, DirStep::open, next,
                               "it is a dangling symbolic link");
            }
            const int stat_err = errno;
            if (stat_err != ENOENT) {
                return errno_verdict(DirStep::open, next, stat_err, true);
            }
            break; // nothing below this point exists yet; download creates it in `deepest`
        }
        current.reset(fd); // closes the parent: only the deepest open directory is kept
        deepest = std::move(next);
        below_root = true;
    }

    if (::faccessat(current.get(), ".", W_OK | X_OK, AT_EACCESS) != 0) {
        const int err = errno;
        return errno_verdict(DirStep::write, deepest, err, below_root);
    }
    return RowDirVerdict{};
}

} // namespace mangadex_dl
