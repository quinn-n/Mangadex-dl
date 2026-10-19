// SPDX-License-Identifier: GPL-3.0-or-later
//
// errno_text (skip-existing plan T6): strerror_r behind one signature.
#include "fs/errno_text.hpp"

#include <array>
#include <format>
#include <string>

// NOLINTNEXTLINE(modernize-deprecated-headers): strerror_r is POSIX, declared only in <string.h>
#include <string.h>

namespace mangadex_dl {
namespace {

// strerror_r has two signatures and glibc picks one by feature macros (C++ compilers predefine
// _GNU_SOURCE, so it is the GNU one here). These overloads normalize the result either way;
// whichever flavour the headers declare, the other overload is unused, hence [[maybe_unused]].

/// GNU strerror_r: returns the message, which may point into `buffer` or at a static string.
[[maybe_unused]] [[nodiscard]] const char* strerror_r_text(const char* message,
                                                           const char* /*buffer*/) noexcept {
    return message;
}

/// XSI strerror_r: returns 0 after filling `buffer`, or an error number (nothing usable).
[[maybe_unused]] [[nodiscard]] const char* strerror_r_text(int status,
                                                           const char* buffer) noexcept {
    return status == 0 ? buffer : nullptr;
}

} // namespace

std::string errno_text(int err) {
    std::array<char, 256> buffer{};
    const char* text =
        strerror_r_text(::strerror_r(err, buffer.data(), buffer.size()), buffer.data());
    if (text == nullptr) {
        return std::format("error {}", err);
    }
    return std::string{text};
}

} // namespace mangadex_dl
