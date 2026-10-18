// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hermetic transport behind `--api-base file://<root>`: maps file:// URLs onto a fixture tree.
// Frozen interface (download plan T1); implemented in src/core/net/fixture_transport.cpp.
#pragma once

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/transport.hpp"

#include <filesystem>
#include <memory>

namespace mangadex_dl {

/// A thread-safe Transport serving `file://<root>/...` from the tree under `root` (absolute):
/// `api/<path>?...offset=<n>` reads `api/<path>/offset-<n>.json`, other `api/<path>` reads
/// `api/<path>.json`, anything else is read verbatim. Every component is opened with
/// O_NOFOLLOW relative to the previous one, so nothing outside `root` is reachable: a missing
/// file or a symlink component is HTTP 404, and `.`, `..`, empty segments or a URL outside
/// `root` are rejected. POST answers 404. Never sleeps and never touches the network.
/// Errc::failure when `root` cannot be opened as a directory.
[[nodiscard]] Result<std::unique_ptr<Transport>> make_fixture_transport(std::filesystem::path root);

} // namespace mangadex_dl
