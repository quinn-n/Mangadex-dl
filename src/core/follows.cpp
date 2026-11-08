// SPDX-License-Identifier: GPL-3.0-or-later
//
// The follows collection document (follows.hpp; follows plan T10): the two request URLs and the
// untrusted parse of one `GET /user/follows/manga` page (a MangaDex `MangaList`).
//
// The page is untrusted third-party data. It goes through json_util::parse_untrusted (16 MiB,
// depth 64, BOM and raw NUL rejected, strict UTF-8, plus a separator cap) and is read only through
// json_util::FieldReader (get_ptr access, never a throwing accessor). Every value kept is an owned
// copy (MangaId): no view into the response outlives parse_follows_page. Messages are fixed text
// naming fields, never response bytes or ids.
#include "mangadex_dl/follows.hpp"

#include "json_util.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
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

/// Names the document in parse messages: "malformed MangaDex API response (follows page): …".
inline constexpr std::string_view page_document = "follows page";

/// Guard on one page's JSON separators. The request names no `includes[]`, so an entry is an id,
/// a type, its attributes (titles, alt titles, descriptions, links, tags) and id/type
/// relationships: a few hundred separators each, a few tens of thousands for a full page of 100.
/// The cap leaves wide headroom and still bounds what a hostile, separator-dense 16 MiB page can
/// make nlohmann allocate (fact-check V16 of the batch plan: ~0.5 GiB unbounded).
inline constexpr std::size_t max_page_separators = 1000000;

/// limit/offset/total: required, non-negative integers that fit a size_t.
[[nodiscard]] std::size_t read_size(FieldReader& reader, const Json::object_t& root,
                                    std::string_view key) {
    const std::optional<std::uint64_t> value = reader.unsigned_integer(root, key, Need::required);
    if (!value) {
        return 0;
    }
    if (!std::in_range<std::size_t>(*value)) {
        reader.fail(std::format("'{}' is out of range", key));
        return 0;
    }
    return *value;
}

/// One `data[i]`: an object with `type` "manga" and a UUID `id`. nullopt on failure (recorded in
/// `reader`). Attributes and relationships are not read.
[[nodiscard]] std::optional<MangaId> read_title(FieldReader& reader, const Json& entry) {
    const Json::object_t* item = reader.as_object(entry, "data entry");
    if (item == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string> type = reader.string(*item, "type", Need::required);
    if (reader.ok() && type != "manga") {
        reader.fail("a 'data' entry's 'type' is not manga");
    }
    const std::optional<std::string> id = reader.string(*item, "id", Need::required);
    if (!reader.ok() || !id) {
        return std::nullopt;
    }
    if (!is_uuid(*id)) {
        reader.fail("a 'data' entry's 'id' is not a UUID");
        return std::nullopt;
    }
    Result<MangaId> manga = MangaId::parse(*id);
    if (!manga) {
        reader.fail("a 'data' entry's 'id' is not a UUID"); // unreachable after is_uuid
        return std::nullopt;
    }
    return std::move(*manga);
}

[[nodiscard]] Result<FollowsPage> extract_page(const Json& doc, FieldReader& reader) {
    const Json::object_t* root = reader.as_object(doc, "document");
    if (root == nullptr) {
        return std::unexpected(reader.error());
    }
    const std::optional<std::string> result = reader.string(*root, "result", Need::required);
    if (reader.ok() && result != "ok") {
        reader.fail("'result' is not ok");
    }
    const Json::array_t* data = reader.array(*root, "data", Need::required);
    const std::size_t limit = read_size(reader, *root, "limit");
    static_cast<void>(read_size(reader, *root, "offset")); // validated, not needed
    const std::size_t total = read_size(reader, *root, "total");
    if (!reader.ok() || data == nullptr) {
        return std::unexpected(reader.error());
    }
    if (limit > follows_page_limit) {
        reader.fail(std::format("'limit' is more than {}", follows_page_limit));
    } else if (data->size() > limit) {
        reader.fail("'data' holds more entries than 'limit'");
    }
    if (!reader.ok()) {
        return std::unexpected(reader.error());
    }

    FollowsPage page{.titles = {}, .total = total};
    page.titles.reserve(data->size()); // at most follows_page_limit, checked above
    for (const Json& entry : *data) {
        std::optional<MangaId> title = read_title(reader, entry);
        if (!reader.ok() || !title) {
            return std::unexpected(reader.error());
        }
        page.titles.push_back(std::move(*title));
    }
    return page;
}

} // namespace

std::string follows_request_url(std::string_view api, std::size_t offset) {
    return std::format("{}/user/follows/manga?limit={}&offset={}", api, follows_page_limit, offset);
}

std::string status_map_request_url(std::string_view api) {
    return std::format("{}/manga/status", api);
}

Result<FollowsPage> parse_follows_page(std::string_view json) {
    const Result<Json> doc = json_util::parse_untrusted(
        json, page_document, json_util::ParseLimits{.max_separators = max_page_separators});
    if (!doc) {
        return std::unexpected(doc.error());
    }
    try {
        FieldReader reader{page_document};
        return extract_page(*doc, reader);
    } catch (const nlohmann::json::exception&) {
        // Defensive, as in mangadex_api.cpp: every access goes through get_ptr. e.what() is not
        // forwarded: nlohmann messages can quote input bytes.
        return std::unexpected(Error{
            .code = Errc::network,
            .message = std::format("malformed MangaDex API response ({}): unexpected structure",
                                   page_document),
        });
    }
}

} // namespace mangadex_dl
