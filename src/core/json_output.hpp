// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core-private (not under include/): how every stdout --json document is serialized (json-c1
// plan). report_to_json() (src/core/report.cpp) and batch_to_json() (src/core/batch_json.cpp) are
// the only producers, and both end in dump_document().
//
// Why the post-pass: nlohmann's dump() with ensure_ascii=false escapes only `"`, `\` and the C0
// controls U+0000..U+001F, so DEL (U+007F) and the C1 controls U+0080..U+009F (U+009B is the 8-bit
// CSI, a terminal's ESC[) would reach stdout raw. After error_handler_t::replace the dumped text is
// valid UTF-8 and every byte outside a string is ASCII, so a raw 7F is exactly a DEL and a
// C2 80..C2 9F pair (C2 is never a continuation byte) is exactly one C1 control, both inside a
// string (key or value). Escaping them there keeps the document valid and its decoded strings
// identical.
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace mangadex_dl::json_output {

/// Precondition: `text` is UTF-8 JSON text from nlohmann dump(..., error_handler_t::replace).
/// Each raw 0x7F becomes \u007f and each C2 80..C2 9F pair becomes \u0080..\u009f (lowercase
/// hex, nlohmann's C0 spelling). Every other byte is copied unchanged, including a C2 that ends
/// the text or is not followed by 80..9F. Returns `text` itself (moved) when there is nothing to
/// escape. Throws std::length_error if the escaped text would exceed std::string::max_size(),
/// which no real document reaches (it would reach main as exit 70).
[[nodiscard]] std::string escape_del_and_c1(std::string text);

/// The document as printed on stdout: dump(2, ' ', ensure_ascii=false, error_handler_t::replace),
/// then escape_del_and_c1, then one '\n'. Never carries a raw C0, DEL or C1 control, so neither a
/// 7-bit nor an 8-bit escape sequence.
[[nodiscard]] std::string dump_document(const nlohmann::ordered_json& doc);

} // namespace mangadex_dl::json_output
