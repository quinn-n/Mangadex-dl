// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MangaDex custom-list (MDList) document (mdlist.hpp; list plan T7): the request and website
// URLs, the untrusted parse of `GET /list/{id}`, the 401/403 error-body check, and the name cuts.
//
// The document is untrusted third-party data. It goes through json_util::parse_untrusted (16 MiB,
// depth 64, BOM and raw NUL rejected, strict UTF-8, plus a separator cap) and is read only through
// json_util::FieldReader (get_ptr access, never a throwing accessor). Every value kept is an owned
// copy (MangaId, std::string): no view into the response outlives parse_custom_list. Messages are
// fixed text naming fields and the requested (validated) list id, never response bytes.
#include "mangadex_dl/mdlist.hpp"

#include "json_util.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_url.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

using json_util::FieldReader;
using json_util::Json;
using json_util::Need;

/// Names the document in parse messages: "malformed MangaDex API response (custom list): …".
inline constexpr std::string_view list_document = "custom list";
/// Names the 401/403 error body (only ever parsed for a yes/no answer; never shown).
inline constexpr std::string_view error_document = "error body";
/// The 401/403 body is a small error document; anything larger is not one.
inline constexpr std::size_t max_error_body_bytes = std::size_t{64} * 1024;
/// errors[i].detail of MangaDex's CustomList errors ("CustomList `<id>` not found.").
inline constexpr std::string_view custom_list_detail_prefix = "CustomList ";
inline constexpr std::string_view display_ellipsis = "...";

/// The longest prefix of `text` that is at most `max_bytes` long and does not end inside a UTF-8
/// sequence: when the cut falls on a continuation byte, it backs off to that sequence's lead
/// byte. Never reads past `text` and never goes below 0.
[[nodiscard]] std::string_view utf8_prefix(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes; // < text.size(): text[cut] is the first byte left out
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    return text.substr(0, cut);
}

/// "list <id>: <message>": the requested id is validated and canonical, so echoing it is safe.
[[nodiscard]] Error prefixed(const ListId& expected, const Error& error) {
    return Error{.code = error.code,
                 .message = std::format("list {}: {}", expected.str(), error.message)};
}

[[nodiscard]] Error over_cap(const ListId& expected) {
    return Error{
        .code = Errc::network,
        .message = std::format("list {} has more than {} titles; refusing to truncate it; split it "
                               "into smaller lists",
                               expected.str(), max_list_titles),
    };
}

/// The outcome of reading the document: a list, a FieldReader failure (prefixed by the caller),
/// or the self-contained cap error.
struct Extracted {
    std::optional<CustomList> list{};
    std::optional<Error> cap{};
};

/// attributes.name: absent or null (attributes absent too) -> nullopt; otherwise a string, cut.
[[nodiscard]] std::optional<std::string> read_name(FieldReader& reader,
                                                   const Json::object_t& data) {
    const Json::object_t* attributes = reader.object(data, "attributes", Need::optional);
    if (attributes == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string> name = reader.string(*attributes, "name", Need::optional);
    if (!name) {
        return std::nullopt;
    }
    // The strict parse rejected ill-formed UTF-8 (fact-check V18), so only the cut can split a
    // sequence, and utf8_prefix never does.
    return std::string{utf8_prefix(*name, max_list_name_bytes)};
}

/// data.relationships: the manga entries, in array order. False on the first malformed entry
/// (recorded in `reader`) or when the list is over the cap (`cap` set).
[[nodiscard]] bool read_titles(FieldReader& reader, const Json::array_t& relationships,
                               const ListId& expected, std::vector<MangaId>& titles,
                               std::optional<Error>& cap) {
    for (const Json& entry : relationships) {
        const auto* rel = entry.get_ptr<const Json::object_t*>();
        if (rel == nullptr) {
            reader.fail("a relationship is not an object");
            return false;
        }
        const std::optional<std::string> type = reader.string(*rel, "type", Need::optional);
        if (!reader.ok()) {
            return false;
        }
        if (!type) {
            reader.fail("a relationship has no 'type'");
            return false;
        }
        if (*type != "manga") {
            continue; // the owner (`user`) and any future type
        }
        const std::optional<std::string> id = reader.string(*rel, "id", Need::required);
        if (!reader.ok() || !id) {
            return false;
        }
        if (!is_uuid(*id)) {
            reader.fail("manga relationship 'id' is not a UUID");
            return false;
        }
        Result<MangaId> manga = MangaId::parse(*id);
        if (!manga) {
            reader.fail("manga relationship 'id' is not a UUID");
            return false;
        }
        // Checked before the push: the vector never grows past the cap.
        if (titles.size() >= max_list_titles) {
            cap = over_cap(expected);
            return false;
        }
        titles.push_back(std::move(*manga));
    }
    return true;
}

[[nodiscard]] Extracted extract_list(const Json& doc, FieldReader& reader, const ListId& expected) {
    const Json::object_t* root = reader.as_object(doc, "document");
    const Json::object_t* data =
        root == nullptr ? nullptr : reader.object(*root, "data", Need::required);
    if (!reader.ok() || data == nullptr) {
        return {};
    }
    const std::optional<std::string> type = reader.string(*data, "type", Need::required);
    if (reader.ok() && type != "custom_list") {
        reader.fail("'type' is not custom_list");
    }
    const std::optional<std::string> id = reader.string(*data, "id", Need::required);
    if (reader.ok()) {
        if (!id || !is_uuid(*id)) {
            reader.fail("'id' is not a UUID");
        } else if (detail::lowercase_ascii(*id) != expected.str()) {
            reader.fail("'id' is not the requested list");
        }
    }
    const Json::array_t* relationships = reader.array(*data, "relationships", Need::required);
    std::optional<std::string> name = read_name(reader, *data);
    if (!reader.ok() || relationships == nullptr) {
        return {};
    }

    Extracted out{};
    std::vector<MangaId> titles{};
    if (!read_titles(reader, *relationships, expected, titles, out.cap)) {
        return out;
    }
    out.list = CustomList{.id = expected, .name = std::move(name), .titles = std::move(titles)};
    return out;
}

} // namespace

std::string list_request_url(std::string_view api, const ListId& id) {
    return std::format("{}/list/{}", api, id.str());
}

std::string list_web_url(const ListId& id) {
    return std::format("https://mangadex.org/list/{}", id.str());
}

Result<CustomList> parse_custom_list(std::string_view json, const ListId& expected) {
    const Result<Json> doc = json_util::parse_untrusted(
        json, list_document, json_util::ParseLimits{.max_separators = max_list_separators});
    if (!doc) {
        return std::unexpected(prefixed(expected, doc.error()));
    }
    try {
        FieldReader reader{list_document};
        Extracted extracted = extract_list(*doc, reader, expected);
        if (extracted.cap) {
            return std::unexpected(std::move(*extracted.cap));
        }
        if (!reader.ok() || !extracted.list) {
            return std::unexpected(prefixed(expected, reader.error()));
        }
        return std::move(*extracted.list);
    } catch (const nlohmann::json::exception&) {
        // Defensive, as in mangadex_api.cpp: every access goes through get_ptr. e.what() is not
        // forwarded: nlohmann messages can quote input bytes.
        return std::unexpected(Error{
            .code = Errc::network,
            .message = std::format("list {}: malformed MangaDex API response ({}): unexpected "
                                   "structure",
                                   expected.str(), list_document),
        });
    }
}

bool is_custom_list_error_body(std::string_view body) {
    const Result<Json> doc = json_util::parse_untrusted(
        body, error_document, json_util::ParseLimits{.max_bytes = max_error_body_bytes});
    if (!doc) {
        return false;
    }
    try {
        const auto* root = doc->get_ptr<const Json::object_t*>();
        if (root == nullptr) {
            return false;
        }
        const auto result = root->find("result");
        if (result == root->end()) {
            return false;
        }
        const auto* result_text = result->second.get_ptr<const Json::string_t*>();
        if (result_text == nullptr || *result_text != "error") {
            return false;
        }
        const auto errors = root->find("errors");
        if (errors == root->end()) {
            return false;
        }
        const auto* list = errors->second.get_ptr<const Json::array_t*>();
        if (list == nullptr) {
            return false;
        }
        return std::ranges::any_of(*list, [](const Json& error) {
            const auto* entry = error.get_ptr<const Json::object_t*>();
            if (entry == nullptr) {
                return false;
            }
            const auto detail = entry->find("detail");
            if (detail == entry->end()) {
                return false;
            }
            const auto* text = detail->second.get_ptr<const Json::string_t*>();
            return text != nullptr && text->starts_with(custom_list_detail_prefix);
        });
    } catch (const nlohmann::json::exception&) {
        return false; // defensive: get_ptr and find do not throw
    }
}

std::string list_name_for_display(std::string_view name) {
    const std::string_view kept = utf8_prefix(name, max_list_display_name_bytes);
    if (kept.size() == name.size()) {
        return std::string{name};
    }
    std::string shown{kept};
    shown += display_ellipsis;
    return shown;
}

ListSummary list_summary(const CustomList& list) {
    return ListSummary{
        .id = std::string{list.id.str()},
        .name = list.name,
        .url = list_web_url(list.id),
    };
}

} // namespace mangadex_dl
