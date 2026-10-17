// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "command.hpp"

#include <memory>

namespace CLI {
class App;
} // namespace CLI

namespace mangadex_dl::app {

/// Adds `hello` to `parent`. The returned Command owns the bound variables: keep it alive until
/// dispatch has returned. Its callback stores the Command in `selected` when `hello` is chosen.
[[nodiscard]] std::unique_ptr<Command> register_hello(CLI::App& parent, Command*& selected);

} // namespace mangadex_dl::app
