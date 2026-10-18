// SPDX-License-Identifier: GPL-3.0-or-later
//
// MangaDex API response parsing and validation, request URL building and MD@H report bodies
// (download plan T6). Every response is untrusted: it goes through json_util::parse_untrusted (size
// cap, depth pre-scan, non-throwing parse) and is read only through json_util::FieldReader
// (get_ptr access). Values are copied into the model structs immediately; the parsed document never
// leaves this file and is never dumped, copied or compared.
#include "mangadex_dl/mangadex_api.hpp"

#include "json_util.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// ---- parsing ------------------------------------------------------------------------------------

template <class T> using Extractor = Result<T> (*)(const Json&, FieldReader&);

/// The parser boundary: parse_untrusted, then `extract` with a fresh FieldReader. The catch is
/// defensive (every access goes through get_ptr, so nothing in the extractors is expected to
/// throw); a nlohmann exception must still become Errc::network, never reach main() as 70.
template <class T>
[[nodiscard]] Result<T> parse_document(std::string_view text, std::string_view what,
                                       Extractor<T> extract) {
    const Result<Json> doc = json_util::parse_untrusted(text, what);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    try {
        FieldReader reader{what};
        return extract(*doc, reader);
    } catch (const nlohmann::json::exception&) {
        // e.what() is not forwarded: nlohmann messages can quote input bytes.
        return std::unexpected(Error{
            .code = Errc::network,
            .message =
                std::format("malformed MangaDex API response ({}): unexpected structure", what),
        });
    }
}

/// Appends the entries of a JSON object of strings (a MangaDex localized-string map) to `out`.
/// Entries come in nlohmann's key order (std::map: ascending by language code).
void append_text_map(FieldReader& reader, const Json::object_t& map, std::string_view key,
                     std::vector<LocalizedText>& out) {
    for (const auto& [lang, value] : map) {
        const auto* text = value.get_ptr<const Json::string_t*>();
        if (text == nullptr) {
            reader.fail(std::format("'{}' has the wrong type", key));
            return;
        }
        out.emplace_back(lang, *text);
    }
}

void read_alt_titles(FieldReader& reader, const Json::object_t& attributes,
                     std::vector<LocalizedText>& out) {
    const Json::array_t* alt = reader.array(attributes, "altTitles", Need::optional);
    if (alt == nullptr) {
        return;
    }
    for (const Json& entry : *alt) {
        const Json::object_t* map = reader.as_object(entry, "altTitles");
        if (map == nullptr) {
            return;
        }
        append_text_map(reader, *map, "altTitles", out);
    }
}

/// Author and artist names from the manga's relationships (present only with the includes).
void read_creators(FieldReader& reader, const Json::object_t& data, MangaInfo& info) {
    const Json::array_t* rels = reader.array(data, "relationships", Need::optional);
    if (rels == nullptr) {
        return;
    }
    for (const Json& entry : *rels) {
        const Json::object_t* rel = reader.as_object(entry, "relationships");
        if (rel == nullptr) {
            return;
        }
        const std::optional<std::string> type = reader.string(*rel, "type", Need::optional);
        std::vector<std::string>* names = nullptr;
        if (type == "author") {
            names = &info.authors;
        } else if (type == "artist") {
            names = &info.artists;
        } else {
            continue;
        }
        // Without the author/artist includes a relationship carries no attributes: skip it.
        const Json::object_t* attrs = reader.object(*rel, "attributes", Need::optional);
        if (attrs == nullptr) {
            continue;
        }
        std::optional<std::string> name = reader.string(*attrs, "name", Need::optional);
        if (name && !name->empty()) {
            names->push_back(std::move(*name));
        }
    }
}

Result<MangaInfo> extract_manga(const Json& doc, FieldReader& reader) {
    const Json::object_t* root = reader.as_object(doc, "document");
    const Json::object_t* data =
        root == nullptr ? nullptr : reader.object(*root, "data", Need::required);
    const Json::object_t* attributes =
        data == nullptr ? nullptr : reader.object(*data, "attributes", Need::required);
    const Json::object_t* title =
        attributes == nullptr ? nullptr : reader.object(*attributes, "title", Need::required);
    if (!reader.ok() || data == nullptr || attributes == nullptr || title == nullptr) {
        return std::unexpected(reader.error());
    }

    MangaInfo info{};
    info.id = reader.string(*data, "id", Need::required).value_or("");
    if (reader.ok() && !is_uuid(info.id)) {
        reader.fail("'id' is not a UUID");
    }
    append_text_map(reader, *title, "title", info.titles);
    read_alt_titles(reader, *attributes, info.alt_titles);
    info.original_language =
        reader.string(*attributes, "originalLanguage", Need::optional).value_or("");
    read_creators(reader, *data, info);
    if (!reader.ok()) {
        return std::unexpected(reader.error());
    }
    return info;
}

void read_chapter_attributes(FieldReader& reader, const Json::object_t& attrs, Chapter& chapter) {
    chapter.volume = reader.string(attrs, "volume", Need::optional);
    chapter.chapter = reader.string(attrs, "chapter", Need::optional);
    chapter.title = reader.string(attrs, "title", Need::optional).value_or("");
    chapter.language = reader.string(attrs, "translatedLanguage", Need::optional).value_or("");
    chapter.external = reader.string(attrs, "externalUrl", Need::optional).has_value();
    chapter.unavailable = reader.boolean(attrs, "isUnavailable", Need::optional).value_or(false);
    chapter.publish_at = reader.string(attrs, "publishAt", Need::optional).value_or("");
    chapter.updated_at = reader.string(attrs, "updatedAt", Need::optional).value_or("");
    const std::optional<std::uint64_t> pages =
        reader.unsigned_integer(attrs, "pages", Need::required);
    if (!pages) {
        return;
    }
    if (!std::in_range<int>(*pages)) {
        reader.fail("'pages' is out of range");
        return;
    }
    chapter.pages = static_cast<int>(*pages);
}

/// The chapter's scanlation groups, in API order; other relationship types are ignored.
void read_groups(FieldReader& reader, const Json::object_t& item, std::vector<ScanGroup>& out) {
    const Json::array_t* rels = reader.array(item, "relationships", Need::optional);
    if (rels == nullptr) {
        return;
    }
    for (const Json& entry : *rels) {
        const Json::object_t* rel = reader.as_object(entry, "relationships");
        if (rel == nullptr) {
            return;
        }
        if (reader.string(*rel, "type", Need::optional) != "scanlation_group") {
            continue;
        }
        ScanGroup group{};
        group.id = reader.string(*rel, "id", Need::required).value_or("");
        if (reader.ok() && !is_uuid(group.id)) {
            reader.fail("scanlation group 'id' is not a UUID");
        }
        if (const Json::object_t* attrs = reader.object(*rel, "attributes", Need::optional)) {
            group.name = reader.string(*attrs, "name", Need::optional).value_or("");
        }
        out.push_back(std::move(group));
    }
}

/// One entry of a feed page's `data`. The caller checks reader.ok() before using the result.
Chapter extract_chapter(const Json& entry, FieldReader& reader) {
    Chapter chapter{};
    const Json::object_t* item = reader.as_object(entry, "data");
    if (item == nullptr) {
        return chapter;
    }
    chapter.id = reader.string(*item, "id", Need::required).value_or("");
    if (reader.ok() && !is_uuid(chapter.id)) {
        reader.fail("chapter 'id' is not a UUID");
    }
    if (const Json::object_t* attrs = reader.object(*item, "attributes", Need::required)) {
        read_chapter_attributes(reader, *attrs, chapter);
    }
    read_groups(reader, *item, chapter.groups);
    return chapter;
}

/// limit/offset/total of a collection page; absent fields read as 0.
struct Window {
    std::size_t offset = 0;
    std::size_t limit = 0;
    std::size_t total = 0;
};

[[nodiscard]] std::size_t read_size(FieldReader& reader, const Json::object_t& root,
                                    std::string_view key) {
    const std::optional<std::uint64_t> value = reader.unsigned_integer(root, key, Need::optional);
    if (!value) {
        return 0;
    }
    if (!std::in_range<std::size_t>(*value)) {
        reader.fail(std::format("'{}' is out of range", key));
        return 0;
    }
    return *value;
}

[[nodiscard]] Window read_window(FieldReader& reader, const Json::object_t& root) {
    return Window{.offset = read_size(reader, root, "offset"),
                  .limit = read_size(reader, root, "limit"),
                  .total = read_size(reader, root, "total")};
}

Result<FeedPage> extract_feed_page(const Json& doc, FieldReader& reader) {
    const Json::object_t* root = reader.as_object(doc, "document");
    const Json::array_t* data =
        root == nullptr ? nullptr : reader.array(*root, "data", Need::required);
    if (!reader.ok() || root == nullptr || data == nullptr) {
        return std::unexpected(reader.error());
    }
    FeedPage page{};
    // Capped: `data` may hold millions of junk entries (16 MiB of `0,`), and reserving for all of
    // them up front would allocate gigabytes before the first entry is rejected.
    page.chapters.reserve(std::min(data->size(), feed_page_limit));
    for (const Json& entry : *data) {
        Chapter chapter = extract_chapter(entry, reader);
        if (!reader.ok()) {
            return std::unexpected(reader.error());
        }
        page.chapters.push_back(std::move(chapter));
    }
    const Window window = read_window(reader, *root);
    if (!reader.ok()) {
        return std::unexpected(reader.error());
    }
    page.offset = window.offset;
    page.limit = window.limit;
    page.total = window.total;
    return page;
}

Result<CoverPage> extract_cover_page(const Json& doc, FieldReader& reader) {
    const Json::object_t* root = reader.as_object(doc, "document");
    const Json::array_t* data =
        root == nullptr ? nullptr : reader.array(*root, "data", Need::required);
    if (!reader.ok() || root == nullptr || data == nullptr) {
        return std::unexpected(reader.error());
    }
    CoverPage page{};
    page.covers.reserve(std::min(data->size(), cover_page_limit)); // capped, as for the feed
    for (const Json& entry : *data) {
        const Json::object_t* item = reader.as_object(entry, "data");
        const Json::object_t* attrs =
            item == nullptr ? nullptr : reader.object(*item, "attributes", Need::required);
        if (attrs == nullptr) {
            return std::unexpected(reader.error());
        }
        Cover cover{};
        cover.volume = reader.string(*attrs, "volume", Need::optional);
        cover.file_name = reader.string(*attrs, "fileName", Need::required).value_or("");
        cover.locale = reader.string(*attrs, "locale", Need::optional);
        // The file name becomes a URL path segment and, in fixture mode, a path component.
        if (reader.ok() && !is_valid_at_home_token(cover.file_name)) {
            reader.fail("cover 'fileName' is not a safe file name");
        }
        if (!reader.ok()) {
            return std::unexpected(reader.error());
        }
        page.covers.push_back(std::move(cover));
    }
    const Window window = read_window(reader, *root);
    if (!reader.ok()) {
        return std::unexpected(reader.error());
    }
    page.offset = window.offset;
    page.limit = window.limit;
    page.total = window.total;
    return page;
}

/// Copies an at-home file-name list after checking its length and every name.
void read_file_names(FieldReader& reader, const Json::array_t& names, std::string_view key,
                     std::vector<std::string>& out) {
    if (names.size() > max_pages_per_chapter) {
        reader.fail(std::format("'{}' lists more than {} pages", key, max_pages_per_chapter));
        return;
    }
    out.reserve(names.size());
    for (const Json& name : names) {
        const auto* text = name.get_ptr<const Json::string_t*>();
        if (text == nullptr) {
            reader.fail(std::format("'{}' has the wrong type", key));
            return;
        }
        if (!is_valid_at_home_token(*text)) {
            reader.fail(std::format("'{}' contains an unsafe file name", key));
            return;
        }
        out.push_back(*text);
    }
}

Result<AtHomeServer> extract_at_home(const Json& doc, FieldReader& reader) {
    const Json::object_t* root = reader.as_object(doc, "document");
    if (root == nullptr) {
        return std::unexpected(reader.error());
    }
    AtHomeServer server{};
    server.base_url = reader.string(*root, "baseUrl", Need::required).value_or("");
    const Json::object_t* chapter = reader.object(*root, "chapter", Need::required);
    if (!reader.ok() || chapter == nullptr) {
        return std::unexpected(reader.error());
    }
    if (const Result<void> valid = validate_at_home_base_url(server.base_url); !valid) {
        return std::unexpected(valid.error());
    }
    while (server.base_url.ends_with('/')) {
        server.base_url.pop_back();
    }

    server.hash = reader.string(*chapter, "hash", Need::required).value_or("");
    if (reader.ok() && !is_valid_at_home_token(server.hash)) {
        reader.fail("'hash' is not a safe path segment");
    }
    if (const Json::array_t* data = reader.array(*chapter, "data", Need::required)) {
        read_file_names(reader, *data, "data", server.data);
    }
    if (const Json::array_t* saver = reader.array(*chapter, "dataSaver", Need::optional)) {
        read_file_names(reader, *saver, "dataSaver", server.data_saver);
    }
    if (!reader.ok()) {
        return std::unexpected(reader.error());
    }
    return server;
}

// ---- URLs ---------------------------------------------------------------------------------------

[[nodiscard]] constexpr bool is_ascii_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// Percent-encodes everything outside RFC 3986 "unreserved", so a value can never add a path
/// segment, query parameter or fragment. Validated ids, tokens and language codes pass unchanged.
[[nodiscard]] std::string encode_component(std::string_view text) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (is_ascii_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(c);
        } else {
            const unsigned byte = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0xFU]);
        }
    }
    return out;
}

[[nodiscard]] std::string_view quality_segment(ImageQuality quality) noexcept {
    switch (quality) {
    case ImageQuality::data:
        return "data";
    case ImageQuality::data_saver:
        return "data-saver";
    }
    return "data"; // only reachable through an out-of-range enum value
}

[[nodiscard]] Error bad_base_url(std::string_view why) {
    return Error{.code = Errc::network, .message = std::format("at-home server baseUrl {}", why)};
}

/// Why `authority` (host[:port], already known to be printable ASCII) is unacceptable, or nullopt.
/// The returned view refers to a string literal.
[[nodiscard]] std::optional<std::string_view> authority_problem(std::string_view authority) {
    if (authority.contains('@')) {
        return "has userinfo";
    }
    const std::size_t colon = authority.find(':');
    const std::string_view host = authority.substr(0, colon);
    if (host.empty()) {
        return "has no host";
    }
    if (!std::ranges::all_of(host,
                             [](char c) { return is_ascii_alnum(c) || c == '-' || c == '.'; })) {
        return "has an invalid host";
    }
    if (colon != std::string_view::npos) {
        const std::string_view port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 ||
            port.find_first_not_of("0123456789") != std::string_view::npos) {
            return "has an invalid port";
        }
    }
    return std::nullopt;
}

/// True when the absolute path `path` (starting with '/') has a "." or ".." segment.
[[nodiscard]] bool has_dot_segment(std::string_view path) noexcept {
    std::size_t start = 0; // index of the '/' before the current segment
    while (start < path.size()) {
        const std::size_t next = path.find('/', start + 1);
        const std::string_view segment = path.substr(start + 1, next - (start + 1));
        if (segment == "." || segment == "..") {
            return true;
        }
        start = next == std::string_view::npos ? path.size() : next;
    }
    return false;
}

} // namespace

// ---- parsers ------------------------------------------------------------------------------------

Result<MangaInfo> parse_manga(std::string_view json) {
    return parse_document<MangaInfo>(json, "manga", &extract_manga);
}

Result<FeedPage> parse_feed_page(std::string_view json) {
    return parse_document<FeedPage>(json, "feed page", &extract_feed_page);
}

Result<CoverPage> parse_cover_page(std::string_view json) {
    return parse_document<CoverPage>(json, "cover page", &extract_cover_page);
}

Result<AtHomeServer> parse_at_home(std::string_view json) {
    return parse_document<AtHomeServer>(json, "at-home server", &extract_at_home);
}

// ---- validators ---------------------------------------------------------------------------------

bool is_valid_at_home_token(std::string_view token) noexcept {
    if (token.empty() || !is_ascii_alnum(token.front())) {
        return false;
    }
    return std::ranges::all_of(
        token, [](char c) { return is_ascii_alnum(c) || c == '.' || c == '_' || c == '-'; });
}

Result<void> validate_at_home_base_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) {
        return std::unexpected(bad_base_url("is not an https:// URL"));
    }
    const bool printable = std::ranges::all_of(url, [](char c) {
        const unsigned byte = static_cast<unsigned char>(c);
        return byte > 0x20U && byte < 0x7FU;
    });
    if (!printable) {
        return std::unexpected(bad_base_url("contains a space, control or non-ASCII byte"));
    }
    if (url.find_first_of("?#") != std::string_view::npos) {
        return std::unexpected(bad_base_url("has a query or fragment"));
    }
    if (url.contains('\\')) {
        return std::unexpected(bad_base_url("contains a backslash"));
    }
    const std::string_view rest = url.substr(scheme.size());
    const std::size_t slash = rest.find('/');
    if (const std::optional<std::string_view> why = authority_problem(rest.substr(0, slash))) {
        return std::unexpected(bad_base_url(*why));
    }
    if (slash != std::string_view::npos && has_dot_segment(rest.substr(slash))) {
        // A dot segment would let the base escape its own path once the URL is resolved.
        return std::unexpected(bad_base_url("has a dot segment"));
    }
    return {};
}

// ---- request URLs -------------------------------------------------------------------------------

std::string manga_request_url(std::string_view api, const MangaId& id) {
    return std::format("{}/manga/{}?includes[]=cover_art&includes[]=author&includes[]=artist", api,
                       id.str());
}

std::string feed_request_url(std::string_view api, const MangaId& id, std::string_view lang,
                             std::size_t offset) {
    // includeExternalUrl and includeEmptyPages are deliberately omitted. Observed against the live
    // API on 2026-10-18: they are filters, not opt-ins. `=1` returns ONLY external / ONLY
    // empty-page chapters (a normal 61-chapter title gave total 0), `=0` excludes them, and
    // omitting them applies no filter. Omitting them lets external and empty chapters arrive so the
    // planner reports them as `external` / `no_pages` skips. Do not re-add either parameter.
    return std::format("{}/manga/{}/feed?translatedLanguage[]={}&limit={}&offset={}"
                       "&order[volume]=asc&order[chapter]=asc&includes[]=scanlation_group"
                       "&contentRating[]=safe&contentRating[]=suggestive"
                       "&contentRating[]=erotica&contentRating[]=pornographic"
                       "&includeFuturePublishAt=0&includeUnavailable=1",
                       api, id.str(), encode_component(lang), feed_page_limit, offset);
}

std::string cover_request_url(std::string_view api, const MangaId& id, std::size_t offset) {
    return std::format("{}/cover?manga[]={}&limit={}&offset={}", api, id.str(), cover_page_limit,
                       offset);
}

std::string at_home_request_url(std::string_view api, std::string_view chapter_id) {
    return std::format("{}/at-home/server/{}", api, encode_component(chapter_id));
}

std::string image_url(std::string_view base, ImageQuality quality, std::string_view hash,
                      std::string_view file) {
    return std::format("{}/{}/{}/{}", base, quality_segment(quality), encode_component(hash),
                       encode_component(file));
}

std::string cover_image_url(std::string_view uploads, const MangaId& id, std::string_view file) {
    return std::format("{}/covers/{}/{}", uploads, id.str(), encode_component(file));
}

// ---- MangaDex@Home reports ----------------------------------------------------------------------

bool is_mangadex_org_host(std::string_view url) noexcept {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return false;
    }
    std::string_view authority = url.substr(scheme_end + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.starts_with('[')) {
        return false; // an IPv6 literal is never a mangadex.org host
    }
    std::string_view host = authority.substr(0, authority.find(':'));
    if (host.ends_with('.')) {
        host.remove_suffix(1); // the absolute (FQDN) form names the same host
    }
    constexpr std::string_view domain = "mangadex.org";
    constexpr std::string_view subdomain_suffix = ".mangadex.org";
    if (iequals_ascii(host, domain)) {
        return true;
    }
    return host.size() > subdomain_suffix.size() &&
           iequals_ascii(host.substr(host.size() - subdomain_suffix.size()), subdomain_suffix);
}

std::string at_home_report_json(const AtHomeReport& report) {
    // ordered_json keeps the documented key order (5 fixed keys, so its linear lookup is free).
    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["url"] = report.url;
    doc["success"] = report.success;
    doc["cached"] = report.cached;
    doc["bytes"] = report.bytes;
    doc["duration"] = report.duration_ms;
    return doc.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

} // namespace mangadex_dl
