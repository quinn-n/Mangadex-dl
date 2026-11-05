// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading statuses (follows.hpp; follows plan T10, spec A1 rev2 + rev3 S1/S2): the six API
// values, the --status filter, the untrusted parse of `GET /manga/status`, and the
// follows ∩ status filter.
//
// The status map is untrusted third-party data. It goes through json_util::parse_untrusted
// (16 MiB, depth 64, BOM and raw NUL rejected, strict UTF-8, plus max_status_separators) and is
// read only through get_ptr. Every value kept is an owned copy (MangaId, std::string). Messages
// are fixed text naming fields, never response bytes or ids.
//
// Parser facts the map relies on (fact-check V23): json_util::Json is nlohmann::json, whose
// object_t is a std::map. A duplicate JSON key therefore cannot be detected: the parser keeps the
// last value and it is not treated as malformed. Iteration is in the byte order of the raw keys,
// not document order, so "the first of two keys that canonicalise to the same id" means the first
// in key byte order (an upper-case hex digit sorts before its lower-case form). Either way the
// outcome is deterministic for a given document.
#include "json_util.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

using json_util::Json;

/// Names the document in parse messages: "malformed MangaDex API response (reading statuses): …".
inline constexpr std::string_view map_document = "reading statuses";

/// A well-formed status value is 1 to this many bytes of [a-z_].
inline constexpr std::size_t max_status_value_bytes = 32;
/// An unknown --status word is echoed cut to this many bytes (the app sanitizes it).
inline constexpr std::size_t max_word_echo_bytes = 32;

inline constexpr std::string_view status_word_list =
    "reading, on_hold, plan_to_read, dropped, re_reading or completed";

struct StatusSpelling {
    ReadingStatus status = ReadingStatus::reading;
    std::string_view api{};
    std::string_view kebab{}; ///< the --status alias; equal to `api` when there is none
};

inline constexpr std::array<StatusSpelling, 6> spellings{{
    {.status = ReadingStatus::reading, .api = "reading", .kebab = "reading"},
    {.status = ReadingStatus::on_hold, .api = "on_hold", .kebab = "on-hold"},
    {.status = ReadingStatus::plan_to_read, .api = "plan_to_read", .kebab = "plan-to-read"},
    {.status = ReadingStatus::dropped, .api = "dropped", .kebab = "dropped"},
    {.status = ReadingStatus::re_reading, .api = "re_reading", .kebab = "re-reading"},
    {.status = ReadingStatus::completed, .api = "completed", .kebab = "completed"},
}};

/// One --status word (exact lower case), API spelling or kebab alias.
[[nodiscard]] std::optional<ReadingStatus> parse_status_word(std::string_view word) noexcept {
    for (const StatusSpelling& spelling : spellings) {
        if (word == spelling.api || word == spelling.kebab) {
            return spelling.status;
        }
    }
    return std::nullopt;
}

/// [a-z_]{1,32}: the only status values a map may hold.
[[nodiscard]] bool is_well_formed_status(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_status_value_bytes &&
           std::ranges::all_of(value, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
}

/// The first `max_bytes` bytes of `text`, backed off by at most 3 bytes so a well-formed UTF-8
/// sequence is not split (argv may be any bytes; the app's terminal_safe handles the rest).
[[nodiscard]] std::string_view cut_word(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes; // < text.size(): text[cut] is the first byte left out
    for (int step = 0; step < 3 && cut > 0; ++step) {
        if ((static_cast<unsigned char>(text[cut]) & 0xC0U) != 0x80U) {
            break;
        }
        --cut;
    }
    if ((static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        cut = max_bytes; // not a well-formed sequence anyway: cut at the byte limit
    }
    return text.substr(0, cut);
}

[[nodiscard]] Error empty_word_error() {
    return Error{.code = Errc::usage,
                 .message = std::format("--status needs a reading status ({})", status_word_list)};
}

[[nodiscard]] Error unknown_word_error(std::string_view word) {
    return Error{.code = Errc::usage,
                 .message = std::format("--status: unknown reading status '{}'; use {}",
                                        cut_word(word, max_word_echo_bytes), status_word_list)};
}

[[nodiscard]] Error malformed_map(std::string_view detail) {
    return Error{.code = Errc::network,
                 .message =
                     std::format("malformed MangaDex API response ({}): {}", map_document, detail)};
}

[[nodiscard]] Error over_entry_cap() {
    return Error{.code = Errc::network,
                 .message = std::format("your MangaDex reading statuses hold more than {} titles; "
                                        "refusing to truncate them",
                                        max_status_entries)};
}

/// The `statuses` object's entries, validated and canonicalised into `map`.
[[nodiscard]] Result<StatusMap> read_entries(const Json::object_t& statuses) {
    // Checked before anything is copied: the map never grows past the cap.
    if (statuses.size() > max_status_entries) {
        return std::unexpected(over_entry_cap());
    }
    StatusMap map{};
    for (const auto& [key, value] : statuses) {
        if (!is_uuid(key)) {
            return std::unexpected(malformed_map("a 'statuses' key is not a UUID"));
        }
        const auto* text = value.get_ptr<const Json::string_t*>();
        if (text == nullptr) {
            return std::unexpected(malformed_map("a 'statuses' value is not a string"));
        }
        if (!is_well_formed_status(*text)) {
            return std::unexpected(malformed_map("a 'statuses' value is not a reading status"));
        }
        Result<MangaId> id = MangaId::parse(key);
        if (!id) {
            // Unreachable after is_uuid; kept so the error path never depends on that.
            return std::unexpected(malformed_map("a 'statuses' key is not a UUID"));
        }
        // try_emplace keeps the entry already there (the first in key byte order of two keys that
        // canonicalise alike) and builds no node for the later one.
        map.statuses.try_emplace(std::move(*id), *text);
    }
    return map;
}

[[nodiscard]] Result<StatusMap> extract_map(const Json& doc) {
    const auto* root = doc.get_ptr<const Json::object_t*>();
    if (root == nullptr) {
        return std::unexpected(malformed_map("'document' has the wrong type"));
    }
    const auto result = root->find("result");
    if (result == root->end() || result->second.is_null()) {
        return std::unexpected(malformed_map("'result' is missing"));
    }
    const auto* result_text = result->second.get_ptr<const Json::string_t*>();
    if (result_text == nullptr) {
        return std::unexpected(malformed_map("'result' has the wrong type"));
    }
    if (*result_text != "ok") {
        return std::unexpected(malformed_map("'result' is not ok"));
    }
    const auto statuses = root->find("statuses");
    if (statuses == root->end() || statuses->second.is_null()) {
        return std::unexpected(malformed_map("'statuses' is missing"));
    }
    if (const auto* object = statuses->second.get_ptr<const Json::object_t*>()) {
        return read_entries(*object);
    }
    // A PHP backend may serialise an empty map as `[]` (spec D1).
    if (const auto* array = statuses->second.get_ptr<const Json::array_t*>();
        array != nullptr && array->empty()) {
        return StatusMap{};
    }
    return std::unexpected(malformed_map("'statuses' has the wrong type"));
}

} // namespace

std::string_view reading_status_name(ReadingStatus status) noexcept {
    switch (status) {
    case ReadingStatus::reading:
        return "reading";
    case ReadingStatus::on_hold:
        return "on_hold";
    case ReadingStatus::plan_to_read:
        return "plan_to_read";
    case ReadingStatus::dropped:
        return "dropped";
    case ReadingStatus::re_reading:
        return "re_reading";
    case ReadingStatus::completed:
        return "completed";
    }
    return {}; // only reachable through an out-of-range enum value
}

std::optional<ReadingStatus> parse_reading_status(std::string_view api_value) noexcept {
    for (const StatusSpelling& spelling : spellings) {
        if (api_value == spelling.api) {
            return spelling.status;
        }
    }
    return std::nullopt;
}

Result<StatusFilter> StatusFilter::parse(std::span<const std::string> words) {
    if (words.empty()) {
        return std::unexpected(empty_word_error());
    }
    std::vector<ReadingStatus> statuses{};
    for (const std::string& value : words) {
        std::string_view rest{value};
        while (true) {
            const std::size_t comma = rest.find(',');
            const std::string_view word = rest.substr(0, comma);
            if (word.empty()) {
                return std::unexpected(empty_word_error());
            }
            const std::optional<ReadingStatus> status = parse_status_word(word);
            if (!status) {
                return std::unexpected(unknown_word_error(word));
            }
            if (!std::ranges::contains(statuses, *status)) {
                statuses.push_back(*status);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
    }
    return StatusFilter{std::move(statuses)};
}

bool StatusFilter::contains(ReadingStatus status) const noexcept {
    return std::ranges::contains(statuses_, status);
}

std::vector<std::string> StatusFilter::names() const {
    std::vector<std::string> out{};
    out.reserve(statuses_.size());
    for (const ReadingStatus status : statuses_) {
        out.emplace_back(reading_status_name(status));
    }
    return out;
}

Result<StatusMap> parse_status_map(std::string_view json) {
    const Result<Json> doc = json_util::parse_untrusted(
        json, map_document, json_util::ParseLimits{.max_separators = max_status_separators});
    if (!doc) {
        return std::unexpected(doc.error());
    }
    try {
        return extract_map(*doc);
    } catch (const nlohmann::json::exception&) {
        // Defensive, as in mdlist.cpp: every access goes through get_ptr or find. e.what() is not
        // forwarded: nlohmann messages can quote input bytes.
        return std::unexpected(malformed_map("unexpected structure"));
    }
}

FilteredFollows filter_follows(std::span<const MangaId> follows, const StatusMap& map,
                               const StatusFilter& filter) {
    FilteredFollows out{.titles = {}, .follows = follows.size(), .unknown = {}};
    for (const MangaId& title : follows) {
        const auto entry = map.statuses.find(title);
        if (entry == map.statuses.end()) {
            continue; // followed without a reading status: never kept
        }
        const std::string& value = entry->second;
        const std::optional<ReadingStatus> status = parse_reading_status(value);
        if (!status) {
            // parse_status_map only keeps well-formed values; a map built elsewhere may not, and
            // such a value never reaches the -v note.
            if (is_well_formed_status(value)) {
                ++out.unknown[value];
            }
            continue;
        }
        if (filter.contains(*status)) {
            out.titles.push_back(title);
        }
    }
    return out;
}

} // namespace mangadex_dl
