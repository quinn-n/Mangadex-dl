// SPDX-License-Identifier: GPL-3.0-or-later
//
// Program identity. The version has one source, project(VERSION) in CMakeLists.txt, which
// configure_file() writes into generated/version.cpp from src/core/version.cpp.in.
#pragma once

#include <string_view>

namespace mangadex_dl {

inline constexpr std::string_view tool_name{"mangadex-dl"};

/// "X.Y.Z", from project(VERSION).
[[nodiscard]] std::string_view version() noexcept;

/// "mangadex-dl X.Y.Z": the first line of --version (help2man parses it).
[[nodiscard]] std::string_view version_line() noexcept;

} // namespace mangadex_dl
