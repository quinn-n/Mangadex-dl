// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a small input (the batch title list) whole, with a hard cap on the bytes consumed.
// Frozen interface (batch plan T1); implemented in src/core/fs/bounded_read.cpp (T2).
#pragma once

#include "mangadex_dl/error.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace mangadex_dl {

/// Opens `path` with O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK (a FIFO without a writer never blocks
/// the open), then fstat: S_ISREG, S_ISFIFO and S_ISCHR are read; S_ISDIR -> Errc::failure
/// "cannot read <label>: is a directory"; any other type -> Errc::failure "cannot read <label>:
/// not a regular file, pipe or terminal". For S_ISFIFO and S_ISCHR, O_NONBLOCK is cleared
/// (fcntl F_GETFL/F_SETFL) before reading. st_size is only a reserve hint (procfs reports 0).
/// Then the read_bounded_fd loop. The descriptor is closed before return. `label` is only read
/// during the call (copied into messages).
[[nodiscard]] Result<std::string> read_bounded_file(const std::filesystem::path& path,
                                                    std::string_view label, std::size_t cap);

/// The loop on an fd the caller owns (never closed, flags never changed). fstat type policy:
/// S_ISREG/S_ISFIFO/S_ISCHR/S_ISSOCK read, S_ISDIR -> "is a directory", others -> "not a regular
/// file, pipe or terminal". Then read until EOF, EINTR retried, every request at most
/// cap + 1 - got bytes, so at most cap + 1 bytes are ever consumed. More than `cap` bytes ->
/// Errc::usage "<label>: is larger than <cap> bytes". fd < 0, EBADF or any other read error
/// (EAGAIN on an inherited non-blocking fd included) -> Errc::failure
/// "cannot read <label>: <strerror>". Precondition: cap < SIZE_MAX.
[[nodiscard]] Result<std::string> read_bounded_fd(int fd, std::string_view label, std::size_t cap);

} // namespace mangadex_dl
