// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example core module: pure logic, no I/O, no CLI11, unit-testable. Replace it with real features.
#pragma once

#include "mangadex_dl/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace mangadex_dl {

inline constexpr std::size_t max_name_length = 256;

/// "Hello, <name>!". The name is bytes (argv on Linux is not guaranteed UTF-8) and is kept
/// verbatim. Errc::usage when it is empty, longer than max_name_length bytes, or contains ASCII
/// control characters.
[[nodiscard]] Result<std::string> greet(std::string_view name);

} // namespace mangadex_dl
