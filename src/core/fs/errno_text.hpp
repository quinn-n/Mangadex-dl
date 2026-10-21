// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private: the strerror text for an errno value, for Error messages of the fs modules
// (skip-existing plan T6, moved out of output_dir.cpp). Not part of the public interface.
#pragma once

#include <string>

namespace mangadex_dl {

/// The system's message for `err` ("No such file or directory"), or "error <n>" when the C
/// library has none. Thread-safe (strerror_r, never strerror). It may clobber errno, so callers
/// save errno into an int before building the message.
[[nodiscard]] std::string errno_text(int err);

} // namespace mangadex_dl
