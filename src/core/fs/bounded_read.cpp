// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a small input whole with a hard cap on the bytes consumed (batch plan T2, rev1).
//
// Both forms share one read loop: every read(2) request is at most `cap + 1 - got` bytes, so at
// most cap + 1 bytes are ever taken from the source, and more than `cap` is a usage error. That
// matters for pipes and terminals, where the bytes consumed are gone for whoever reads next.
//
// read_bounded_file opens with O_NONBLOCK so that a FIFO without a writer never blocks the open;
// the type comes from fstat (never from read's EISDIR), and O_NONBLOCK is cleared again on that
// descriptor for FIFOs and character devices so the reads block normally. read_bounded_fd
// borrows the caller's descriptor and never closes it or changes its flags.
#include "mangadex_dl/fs/bounded_read.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

inline constexpr std::size_t read_chunk = std::size_t{64} * 1024;
/// Upper bound on the up-front reserve: st_size is only a hint, and a sparse file may claim more.
inline constexpr std::size_t max_reserve_hint = std::size_t{16} * 1024 * 1024;

[[nodiscard]] std::string errno_text(int err) { return std::system_category().message(err); }

/// Errc::failure "cannot read <label>: <why>".
[[nodiscard]] Error cannot_read(std::string_view label, std::string_view why) {
    return Error{.code = Errc::failure, .message = std::format("cannot read {}: {}", label, why)};
}

[[nodiscard]] Error cannot_read_errno(std::string_view label, int err) {
    return cannot_read(label, errno_text(err));
}

/// `cap + 1` is computed below, so SIZE_MAX would wrap to 0 and defeat the cap.
[[nodiscard]] bool cap_in_range(std::size_t cap) noexcept {
    return cap < std::numeric_limits<std::size_t>::max();
}

[[nodiscard]] Error cap_out_of_range(std::string_view label) {
    return cannot_read(label, "the size limit is out of range");
}

/// Which descriptors the two forms accept (the fd form also reads a socket on stdin).
enum class TypePolicy { path, descriptor };

/// The fstat type policy shared by both forms: an error for a directory or an unreadable type.
[[nodiscard]] Result<void> check_type(const struct stat& st, std::string_view label,
                                      TypePolicy policy) {
    const mode_t mode = st.st_mode;
    if (S_ISREG(mode) || S_ISFIFO(mode) || S_ISCHR(mode)) {
        return {};
    }
    if (S_ISSOCK(mode) && policy == TypePolicy::descriptor) {
        return {};
    }
    if (S_ISDIR(mode)) {
        return std::unexpected(cannot_read(label, "is a directory"));
    }
    return std::unexpected(cannot_read(label, "not a regular file, pipe or terminal"));
}

[[nodiscard]] Result<struct stat> stat_fd(int fd, std::string_view label) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        return std::unexpected(cannot_read_errno(label, err));
    }
    return st;
}

/// min(st_size, cap + 1, max_reserve_hint) for a regular file; 0 otherwise (procfs reports 0).
[[nodiscard]] std::size_t reserve_hint(const struct stat& st, std::size_t cap) noexcept {
    if (!S_ISREG(st.st_mode) || !std::in_range<std::size_t>(st.st_size)) {
        return 0;
    }
    return std::min({static_cast<std::size_t>(st.st_size), cap + 1, max_reserve_hint});
}

/// The shared loop: read to EOF, EINTR retried, never more than cap + 1 bytes consumed.
/// Precondition (checked by both callers): cap < SIZE_MAX.
[[nodiscard]] Result<std::string> read_loop(int fd, std::string_view label, std::size_t cap,
                                            std::size_t hint) {
    std::string out{};
    out.reserve(hint);
    std::array<char, read_chunk> chunk{};
    while (true) {
        // out.size() <= cap here (checked after every read), so the request is at least 1.
        const std::size_t want = std::min(chunk.size(), cap + 1 - out.size());
        const ::ssize_t got = ::read(fd, chunk.data(), want);
        if (got < 0) {
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            return std::unexpected(cannot_read_errno(label, err));
        }
        if (got == 0) {
            return out;
        }
        out.append(chunk.data(), static_cast<std::size_t>(got));
        if (out.size() > cap) {
            return std::unexpected(Error{
                .code = Errc::usage,
                .message = std::format("{}: is larger than {} bytes", label, cap),
            });
        }
    }
}

/// open(2) without O_CREAT: the variadic mode argument is never read. `path.c_str()` is
/// NUL-terminated (std::filesystem::path owns its string).
[[nodiscard]] int open_path(const std::filesystem::path& path, int flags) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    return ::open(path.c_str(), flags);
}

/// `flags` without O_NONBLOCK. The bit arithmetic is done unsigned; `flags` is a non-negative
/// F_GETFL result, so the result fits in an int again.
[[nodiscard]] int without_nonblock(int flags) noexcept {
    return static_cast<int>(static_cast<unsigned int>(flags) &
                            ~static_cast<unsigned int>(O_NONBLOCK));
}

/// Clears O_NONBLOCK on a descriptor this module opened itself; 0 or an errno.
[[nodiscard]] int clear_nonblock(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return errno;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    if (::fcntl(fd, F_SETFL, without_nonblock(flags)) != 0) {
        return errno;
    }
    return 0;
}

} // namespace

Result<std::string> read_bounded_file(const std::filesystem::path& path, std::string_view label,
                                      std::size_t cap) {
    if (!cap_in_range(cap)) {
        return std::unexpected(cap_out_of_range(label));
    }
    const UniqueFd fd{open_path(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK)};
    if (!fd.valid()) {
        const int err = errno;
        return std::unexpected(cannot_read_errno(label, err));
    }
    const auto st = stat_fd(fd.get(), label);
    if (!st) {
        return std::unexpected(st.error());
    }
    if (auto type = check_type(*st, label, TypePolicy::path); !type) {
        return std::unexpected(std::move(type).error());
    }
    if (S_ISFIFO(st->st_mode) || S_ISCHR(st->st_mode)) {
        if (const int err = clear_nonblock(fd.get()); err != 0) {
            return std::unexpected(cannot_read_errno(label, err));
        }
    }
    return read_loop(fd.get(), label, cap, reserve_hint(*st, cap));
}

Result<std::string> read_bounded_fd(int fd, std::string_view label, std::size_t cap) {
    if (!cap_in_range(cap)) {
        return std::unexpected(cap_out_of_range(label));
    }
    if (fd < 0) {
        return std::unexpected(cannot_read_errno(label, EBADF));
    }
    const auto st = stat_fd(fd, label);
    if (!st) {
        return std::unexpected(st.error());
    }
    if (auto type = check_type(*st, label, TypePolicy::descriptor); !type) {
        return std::unexpected(std::move(type).error());
    }
    return read_loop(fd, label, cap, reserve_hint(*st, cap));
}

} // namespace mangadex_dl
