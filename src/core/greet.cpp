// SPDX-License-Identifier: GPL-3.0-or-later
#include "mangadex_dl/greet.hpp"

#include "mangadex_dl/error.hpp"

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace mangadex_dl {
namespace {

// Byte-wise and locale-independent on purpose: never std::iscntrl or the global locale.
[[nodiscard]] bool has_control_char(std::string_view text) noexcept {
    return std::ranges::any_of(text, [](char c) {
        const int byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F;
    });
}

} // namespace

Result<std::string> greet(std::string_view name) {
    if (name.empty()) {
        return std::unexpected(Error{.code = Errc::usage, .message = "name must not be empty"});
    }
    if (name.size() > max_name_length) {
        return std::unexpected(Error{
            .code = Errc::usage,
            .message = std::format("name is longer than {} bytes", max_name_length),
        });
    }
    if (has_control_char(name)) {
        return std::unexpected(
            Error{.code = Errc::usage, .message = "name must not contain control characters"});
    }
    return std::format("Hello, {}!", name);
}

} // namespace mangadex_dl
