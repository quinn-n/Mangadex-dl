// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output file names. Frozen interface (download plan T1); implemented in src/core/filename.cpp.
#pragma once

#include "mangadex_dl/model.hpp"

#include <string>
#include <string_view>

namespace mangadex_dl {

/// Makes `text` safe as the stem of one path component: C0 controls, DEL and `/\<>:"|?*` become
/// `_`; bidi controls, invisible format characters (U+200B–U+200D, U+2060–U+2065,
/// U+206A–U+206F, U+FEFF), Unicode non-characters (U+FDD0–U+FDEF, U+xFFFE/U+xFFFF) and
/// U+2028/U+2029 are dropped; invalid UTF-8 bytes become `_`; whitespace
/// runs collapse to one space; leading/trailing spaces and dots and a leading `-` are trimmed;
/// the result is cut to at most 150 bytes on a UTF-8 boundary. An empty, "." or ".." result is
/// replaced by `fallback` (the title UUID).
[[nodiscard]] std::string sanitize_file_stem(std::string_view text, std::string_view fallback);

/// A volume label for display: numeric labels are normalized (normalize_label) and zero-padded to
/// two integer digits ("3" -> "03", "007" -> "07"); non-numeric labels are unchanged.
[[nodiscard]] std::string format_volume_label(std::string_view label);

/// A chapter label for display: numeric labels are normalized (normalize_label) and zero-padded
/// to three integer digits ("12" -> "012", "12.50" -> "012.5"); non-numeric labels are unchanged.
[[nodiscard]] std::string format_chapter_label(std::string_view label);

/// The final file name for `epub` from an already sanitized `stem`: `<stem> - Vol. 03.epub`,
/// `<stem> - Ch. 012.epub`, `<stem> - Vol. 03 Ch. 021.epub`, `<stem> - Oneshot [<8 hex>].epub`
/// or `<stem>.epub`. Always one path component of at most 255 bytes.
[[nodiscard]] std::string epub_file_name(std::string_view stem, const PlannedEpub& epub);

} // namespace mangadex_dl
