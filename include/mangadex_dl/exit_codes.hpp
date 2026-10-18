// SPDX-License-Identifier: GPL-3.0-or-later
//
// The exit-code table: the single source of truth. Mirrored in the --help footer (cli.cpp builds it
// from these constants) and in README / the man page EXIT STATUS section. Scripts depend on these
// numbers, so changing one is a breaking change.
#pragma once

namespace mangadex_dl::exit_code {

/// The command did what was asked.
inline constexpr int success = 0;
/// Expected runtime failure, including a failed write to stdout.
inline constexpr int failure = 1;
/// Invalid invocation. Every CLI11 ParseError is remapped to this.
inline constexpr int usage = 2;

// 3-63: tool-specific failures. Add them here, documented, before any code returns them.

/// download: the plan could not be built because of the network or the API (connection, TLS,
/// timeout, 5xx or 429 retries exhausted, a malformed or oversized API response).
inline constexpr int network = 3;
/// download: the title id does not exist on MangaDex (the API answered 404).
inline constexpr int not_found = 4;
/// download: the title exists, but nothing downloadable matched the language and selection.
inline constexpr int no_chapters = 5;
/// download: at least one output file failed; others may have been written.
inline constexpr int incomplete = 6;

/// An exception reached main(): a bug. Tests assert it never appears.
inline constexpr int internal = 70;
/// The ASan/UBSan/LSan exitcode in sanitized dev builds. Never returned by code.
inline constexpr int sanitizer_reserved = 86;

// Never return 126, 127, 128+n (130 and 141 come from signals) or anything above 255.

} // namespace mangadex_dl::exit_code
