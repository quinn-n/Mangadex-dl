// SPDX-License-Identifier: GPL-3.0-or-later
//
// The per-book manifest (skip-existing plan T4): a deterministic writer and a parser for untrusted
// manifest text with bounded memory. The parser goes through json_util::parse_untrusted (byte cap,
// one depth + separator pre-scan, non-throwing parse) and reads the document only through
// json_util::FieldReader. The parsed document is never dumped, copied or compared (fact-check V4:
// those recurse).
#include "mangadex_dl/book_manifest.hpp"

#include "json_util.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
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
/// The writer keeps insertion order, so the key order is fixed by the code below.
using OrderedJson = nlohmann::ordered_json;

/// Names the document in FieldReader messages (never shown: every failure becomes a ManifestFault).
inline constexpr std::string_view manifest_what = "book manifest";

// Why max_manifest_separators = 2^21 is enough for every manifest this program writes:
// - An EPUB holds at most zip_max_entries (65534) entries and every page costs two (the image and
//   its XHTML page), so a file holds at most 32767 images, hence at most 32767 chapters (each has
//   at least one image).
// - manifest_to_json emits, outside strings: at most 17 separators for the top level and the
//   cover; per chapter at most 21 + 5g for g groups (its `{`, 9 `:`, 8 `,`, the `[` of groups and
//   of images, the `,` before it; per group `{`, 2 `:`, 1 `,` and the `,` between groups); and one
//   `,` per image after a chapter's first.
// - So C chapters with g groups each and I images in total cost at most 17 + C * (20 + 5g) + I.
//   At the extreme C = I = 32767 that is 17 + 32767 * (21 + 5g): 1,343,464 for g = 4 and
//   1,998,804 for g = 8, both below 2^21 = 2,097,152. A file at the entry cap whose chapters have
//   more than 8 groups each could exceed it; it is then classified foreign (`--overwrite` replaces
//   it), which the plan accepts (D12).
// The bound test in tests/unit/book_manifest/book_manifest_test.cpp checks this arithmetic.

inline constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";

[[nodiscard]] std::string_view quality_name(ImageQuality quality) noexcept {
    switch (quality) {
    case ImageQuality::data:
        return "data";
    case ImageQuality::data_saver:
        return "data-saver";
    }
    return "data"; // only reachable through an out-of-range enum value
}

[[nodiscard]] std::optional<ImageQuality> quality_from(std::string_view name) noexcept {
    if (name == "data") {
        return ImageQuality::data;
    }
    if (name == "data-saver") {
        return ImageQuality::data_saver;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Exactly 8 lowercase hex digits, as manifest_to_json writes them.
[[nodiscard]] std::optional<std::uint32_t> parse_crc_hex(std::string_view text) noexcept {
    if (text.size() != 8) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        std::uint32_t digit = 0;
        if (is_ascii_digit(c)) {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a') + 10U;
        } else {
            return std::nullopt;
        }
        value = (value << 4U) | digit;
    }
    return value;
}

[[nodiscard]] OrderedJson nullable(const std::optional<std::string>& value) {
    return value.has_value() ? OrderedJson(*value) : OrderedJson(nullptr);
}

[[nodiscard]] OrderedJson chapter_json(const ManifestChapter& chapter) {
    OrderedJson groups = OrderedJson::array();
    for (const ScanGroup& group : chapter.groups) {
        OrderedJson entry = OrderedJson::object();
        entry["id"] = group.id;
        entry["name"] = group.name;
        groups.push_back(std::move(entry));
    }
    OrderedJson images = OrderedJson::array();
    for (const std::string& image : chapter.images) {
        images.push_back(image);
    }
    OrderedJson out = OrderedJson::object();
    out["id"] = chapter.id;
    out["volume"] = nullable(chapter.volume);
    out["chapter"] = nullable(chapter.chapter);
    out["title"] = chapter.title;
    out["groups"] = std::move(groups);
    out["publish_at"] = chapter.publish_at;
    out["updated_at"] = chapter.updated_at;
    out["quality"] = quality_name(chapter.quality);
    out["images"] = std::move(images);
    return out;
}

/// Reads the manifest's fields through one sticky FieldReader. Every check records a failure in
/// the reader; the caller looks at reader.ok() once at the end.
class ManifestParser {
public:
    explicit ManifestParser(FieldReader& reader) noexcept : reader_(&reader) {}

    /// A string of at most max_manifest_string_bytes.
    [[nodiscard]] std::optional<std::string> text(const Json::object_t& parent,
                                                  std::string_view key, Need need) {
        std::optional<std::string> value = reader_->string(parent, key, need);
        if (value.has_value() && value->size() > max_manifest_string_bytes) {
            reader_->fail("a string is too long");
            return std::nullopt;
        }
        return value;
    }

    /// A bounded string that satisfies is_uuid().
    [[nodiscard]] std::string uuid(const Json::object_t& parent, std::string_view key) {
        std::string value = text(parent, key, Need::required).value_or("");
        if (reader_->ok() && !is_uuid(value)) {
            reader_->fail("an id is not a UUID");
        }
        return value;
    }

    /// A bounded string that satisfies is_held_image_entry_name().
    [[nodiscard]] std::string entry_name(const Json::object_t& parent, std::string_view key) {
        std::string value = text(parent, key, Need::required).value_or("");
        if (reader_->ok() && !is_held_image_entry_name(value)) {
            reader_->fail("an entry name is not a held image name");
        }
        return value;
    }

    [[nodiscard]] std::optional<ManifestCover> cover(const Json::object_t& root) {
        const Json::object_t* object = reader_->object(root, "cover", Need::optional);
        if (object == nullptr) {
            return std::nullopt;
        }
        ManifestCover out{};
        out.file_name = text(*object, "file_name", Need::required).value_or("");
        out.entry = entry_name(*object, "entry");
        return out;
    }

    [[nodiscard]] std::vector<ScanGroup> groups(const Json::object_t& chapter) {
        std::vector<ScanGroup> out{};
        const Json::array_t* array = reader_->array(chapter, "groups", Need::required);
        if (array == nullptr) {
            return out;
        }
        // No reserve(array->size()): a ScanGroup is larger than an untrusted array element such as
        // `0`, so reserving first would amplify a hostile array before its first element fails.
        for (const Json& element : *array) {
            const Json::object_t* group = reader_->as_object(element, "groups");
            if (group == nullptr) {
                break;
            }
            ScanGroup parsed{};
            parsed.id = uuid(*group, "id");
            parsed.name = text(*group, "name", Need::required).value_or("");
            if (!reader_->ok()) {
                break;
            }
            out.push_back(std::move(parsed));
        }
        return out;
    }

    /// The images of one chapter: 1..max_pages_per_chapter names, and `total_images` (the images
    /// of the chapters before this one) stays within zip_max_entries. Both counts are checked
    /// before a single name is copied.
    [[nodiscard]] std::vector<std::string> images(const Json::object_t& chapter,
                                                  std::size_t& total_images) {
        std::vector<std::string> out{};
        const Json::array_t* array = reader_->array(chapter, "images", Need::required);
        if (array == nullptr) {
            return out;
        }
        if (array->empty()) {
            reader_->fail("a chapter has no images");
            return out;
        }
        if (array->size() > max_pages_per_chapter) {
            reader_->fail("a chapter has too many images");
            return out;
        }
        // total_images <= zip_max_entries here, so the sum cannot wrap.
        total_images += array->size();
        if (total_images > zip_max_entries) {
            reader_->fail("the book has too many images");
            return out;
        }
        out.reserve(array->size());
        for (const Json& element : *array) {
            const auto* name = element.get_ptr<const Json::string_t*>();
            if (name == nullptr || !is_held_image_entry_name(*name)) {
                reader_->fail("an image is not a held image name");
                break;
            }
            out.push_back(*name);
        }
        return out;
    }

    [[nodiscard]] ManifestChapter chapter(const Json& element, std::size_t& total_images) {
        ManifestChapter out{};
        const Json::object_t* object = reader_->as_object(element, "chapters");
        if (object == nullptr) {
            return out;
        }
        out.id = uuid(*object, "id");
        out.volume = text(*object, "volume", Need::optional);
        out.chapter = text(*object, "chapter", Need::optional);
        out.title = text(*object, "title", Need::required).value_or("");
        out.groups = groups(*object);
        out.publish_at = text(*object, "publish_at", Need::required).value_or("");
        out.updated_at = text(*object, "updated_at", Need::required).value_or("");
        const std::optional<std::string> quality = text(*object, "quality", Need::required);
        if (reader_->ok()) {
            const std::optional<ImageQuality> parsed = quality_from(quality.value_or(""));
            if (parsed.has_value()) {
                out.quality = *parsed;
            } else {
                reader_->fail("'quality' is not a known quality");
            }
        }
        out.images = images(*object, total_images);
        return out;
    }

    [[nodiscard]] std::vector<ManifestChapter> chapters(const Json::object_t& root) {
        std::vector<ManifestChapter> out{};
        const Json::array_t* array = reader_->array(root, "chapters", Need::required);
        if (array == nullptr) {
            return out;
        }
        if (array->empty()) {
            reader_->fail("'chapters' is empty");
            return out;
        }
        // No reserve(array->size()): every accepted chapter holds at least one image, so the
        // image cap bounds the vector; the untrusted array length does not.
        std::size_t total_images = 0;
        for (const Json& element : *array) {
            ManifestChapter parsed = chapter(element, total_images);
            if (!reader_->ok()) {
                break;
            }
            out.push_back(std::move(parsed));
        }
        return out;
    }

private:
    FieldReader* reader_ =
        nullptr; ///< borrowed; outlives this parser (both are locals of one call)
};

[[nodiscard]] std::expected<BookManifest, ManifestFault> read_manifest(const Json& doc) {
    FieldReader reader{manifest_what, Errc::failure};
    ManifestParser parser{reader};
    const std::unexpected<ManifestFault> malformed{ManifestFault::malformed};

    const Json::object_t* root = reader.as_object(doc, "manifest");
    if (root == nullptr) {
        return malformed;
    }
    // The generator and the format decide everything else: a newer format may lay out the other
    // fields differently, so they are not checked before the format is known to be ours.
    const std::optional<std::string> generator = parser.text(*root, "generator", Need::required);
    const std::optional<std::uint64_t> format =
        reader.unsigned_integer(*root, "format", Need::required);
    if (!reader.ok() || !generator.has_value() || !format.has_value() ||
        *generator != manifest_generator) {
        return malformed;
    }
    if (*format > manifest_format) {
        return std::unexpected(ManifestFault::newer_format);
    }
    if (*format < manifest_format) {
        return malformed;
    }

    BookManifest out{};
    out.identifier = parser.text(*root, "identifier", Need::required).value_or("");
    const std::optional<std::string> crc = parser.text(*root, "opf_crc32", Need::required);
    if (reader.ok()) {
        const std::optional<std::uint32_t> value = parse_crc_hex(crc.value_or(""));
        if (value.has_value()) {
            out.opf_crc32 = *value;
        } else {
            reader.fail("'opf_crc32' is not 8 lowercase hex digits");
        }
    }
    out.cover = parser.cover(*root);
    out.chapters = parser.chapters(*root);
    if (!reader.ok()) {
        return malformed;
    }
    return out;
}

} // namespace

std::string manifest_to_json(const BookManifest& manifest) {
    OrderedJson chapters = OrderedJson::array();
    for (const ManifestChapter& chapter : manifest.chapters) {
        chapters.push_back(chapter_json(chapter));
    }
    OrderedJson cover = nullptr;
    if (manifest.cover.has_value()) {
        cover = OrderedJson::object();
        cover["file_name"] = manifest.cover->file_name;
        cover["entry"] = manifest.cover->entry;
    }

    OrderedJson doc = OrderedJson::object();
    doc["format"] = manifest_format;
    doc["generator"] = manifest_generator;
    doc["identifier"] = manifest.identifier;
    doc["opf_crc32"] = std::format("{:08x}", manifest.opf_crc32);
    doc["cover"] = std::move(cover);
    doc["chapters"] = std::move(chapters);
    // Compact (indent -1), insertion order, deterministic (fact-check V14). replace: titles and
    // group names are untrusted bytes, and the strict handler would throw on invalid UTF-8.
    return doc.dump(-1, ' ', false, OrderedJson::error_handler_t::replace);
}

std::expected<BookManifest, ManifestFault> parse_manifest(std::string_view json) {
    // Two inputs nlohmann accepts that the writer never produces, rejected before the parse (the
    // byte cap comes first, so the NUL scan is bounded too):
    // - a leading UTF-8 BOM, which nlohmann skips;
    // - a raw NUL byte, which nlohmann's lexer takes as the end of input, so `doc\0junk` would
    //   parse as `doc`. Valid JSON never holds a raw NUL (strings must escape it).
    if (json.size() > max_manifest_bytes || json.starts_with(utf8_bom) || json.contains('\0')) {
        return std::unexpected(ManifestFault::malformed);
    }
    const Result<Json> doc =
        json_util::parse_untrusted(json, manifest_what,
                                   json_util::ParseLimits{
                                       .max_bytes = max_manifest_bytes,
                                       .max_depth = max_manifest_depth,
                                       .max_separators = max_manifest_separators,
                                       .code = Errc::failure,
                                   });
    if (!doc) {
        return std::unexpected(ManifestFault::malformed);
    }
    try {
        return read_manifest(*doc);
    } catch (const nlohmann::json::exception&) {
        // Defensive: every access goes through get_ptr, so nothing above is expected to throw.
        return std::unexpected(ManifestFault::malformed);
    }
}

bool is_held_image_entry_name(std::string_view name) noexcept {
    constexpr std::string_view prefix = "OEBPS/images/";
    if (!name.starts_with(prefix)) {
        return false;
    }
    name.remove_prefix(prefix.size());
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const std::string_view stem = name.substr(0, dot);
    const std::string_view extension = name.substr(dot + 1);
    const bool page = stem.size() == 6 && stem.front() == 'p' &&
                      std::ranges::all_of(stem.substr(1), is_ascii_digit);
    if (!page && stem != "cover") {
        return false;
    }
    return extension == "jpg" || extension == "png" || extension == "gif" || extension == "webp";
}

ManifestChapter manifest_chapter_from(const Chapter& chapter, ImageQuality quality) {
    return ManifestChapter{
        .id = chapter.id,
        .volume = chapter.volume,
        .chapter = chapter.chapter,
        .title = chapter.title,
        .groups = chapter.groups,
        .publish_at = chapter.publish_at,
        .updated_at = chapter.updated_at,
        .quality = quality,
    };
}

Chapter chapter_from(const ManifestChapter& record) {
    // A parsed record holds at most max_pages_per_chapter images; saturate for any other record.
    const std::size_t count = record.images.size();
    const int pages =
        std::in_range<int>(count) ? static_cast<int>(count) : std::numeric_limits<int>::max();
    return Chapter{
        .id = record.id,
        .volume = record.volume,
        .chapter = record.chapter,
        .title = record.title,
        .groups = record.groups,
        .pages = pages,
        .publish_at = record.publish_at,
        .updated_at = record.updated_at,
    };
}

} // namespace mangadex_dl
