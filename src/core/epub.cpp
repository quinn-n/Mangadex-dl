// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixed-layout EPUB 3 generation (download plan T10; download-plan-rev1 T10; fact-check cluster
// G, W3C EPUB 3.3 + OCF). Archive layout, in entry order:
//
//   mimetype                         STORED first, no extra field, "application/epub+zip"
//   META-INF/container.xml           rootfile OEBPS/content.opf
//   OEBPS/images/cover.<ext>         \ only after add_cover()
//   OEBPS/cover.xhtml                /
//   OEBPS/images/p00001.<ext>        \ one pair per add_page(), in reading order
//   OEBPS/pages/p00001.xhtml         /
//   OEBPS/content.opf                package 3.0, written by finish()
//   OEBPS/nav.xhtml                  EPUB 3 navigation document
//   OEBPS/toc.ncx                    EPUB 2 compatibility table of contents
//   META-INF/mangadex-dl.json        the book manifest; only finish(cover, chapters) writes it,
//                                    always last (skip-existing plan T8)
//
// Package rules (fact-checked): `mimetype` is never listed in the manifest; `dcterms:modified` is
// a <meta property> in CCYY-MM-DDThh:mm:ssZ form; `rendition:layout` pre-paginated,
// `rendition:orientation` auto, `rendition:spread` landscape, and no package-level
// `rendition:viewport` (deprecated): each page carries its own viewport <meta>. The cover image
// has properties="cover-image" (the first page image when there is no cover) plus the legacy
// <meta name="cover">. In the nav document a nested <ol> appears only inside an <li> that starts
// with an <a>, and never empty.
//
// Determinism: no clocks, no locale, no hash ordering. Every byte is a function of the metadata,
// the call sequence and the image bytes. Every generated string is a local value owned here, so
// nothing handed to ZipWriter::add_stored borrows from the sink (its precondition).
#include "mangadex_dl/epub.hpp"

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

constexpr std::string_view epub_media_type = "application/epub+zip";
constexpr std::string_view xml_declaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";

[[nodiscard]] std::unexpected<Error> epub_error(std::string message) {
    return std::unexpected(Error{.code = Errc::failure, .message = std::move(message)});
}

/// Views the characters of `text` as bytes. Borrows `text` for the duration of the call only.
[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

// ---- UTF-8 and XML characters ------------------------------------------------------------------

struct CodePoint {
    std::uint32_t value = 0;
    std::size_t length = 0; ///< bytes in the encoded sequence, 1..4
};

[[nodiscard]] constexpr bool between(unsigned char byte, unsigned char low,
                                     unsigned char high) noexcept {
    return byte >= low && byte <= high;
}

/// The well-formed UTF-8 sequence at the start of `text` (Unicode table 3-7: no overlongs, no
/// surrogates, nothing above U+10FFFF), or nullopt when the first byte does not start one.
[[nodiscard]] constexpr std::optional<CodePoint> decode_utf8(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead < 0x80U) {
        return CodePoint{.value = lead, .length = 1};
    }
    std::size_t length = 0;
    std::uint32_t value = 0;
    unsigned char second_low = 0x80U; // the second byte's range is narrower after some leads
    unsigned char second_high = 0xBFU;
    if (between(lead, 0xC2U, 0xDFU)) {
        length = 2;
        value = lead & 0x1FU;
    } else if (between(lead, 0xE0U, 0xEFU)) {
        length = 3;
        value = lead & 0x0FU;
        if (lead == 0xE0U) {
            second_low = 0xA0U; // no overlong 3-byte forms
        } else if (lead == 0xEDU) {
            second_high = 0x9FU; // no UTF-16 surrogates
        }
    } else if (between(lead, 0xF0U, 0xF4U)) {
        length = 4;
        value = lead & 0x07U;
        if (lead == 0xF0U) {
            second_low = 0x90U; // no overlong 4-byte forms
        } else if (lead == 0xF4U) {
            second_high = 0x8FU; // nothing above U+10FFFF
        }
    } else {
        return std::nullopt; // a continuation byte, an overlong lead (C0, C1) or F5..FF
    }
    if (text.size() < length) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const unsigned char low = i == 1 ? second_low : static_cast<unsigned char>(0x80U);
        const unsigned char high = i == 1 ? second_high : static_cast<unsigned char>(0xBFU);
        if (!between(byte, low, high)) {
            return std::nullopt;
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    return CodePoint{.value = value, .length = length};
}

/// XML 1.0 `Char`: #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF].
[[nodiscard]] constexpr bool is_xml_char(std::uint32_t c) noexcept {
    return c == 0x9U || c == 0xAU || c == 0xDU || (c >= 0x20U && c <= 0xD7FFU) ||
           (c >= 0xE000U && c <= 0xFFFDU) || (c >= 0x10000U && c <= 0x10FFFFU);
}

/// Explicit bidi embeddings, overrides (U+202A..U+202E) and isolates (U+2066..U+2069): they can
/// reorder what a reader displays, so they are dropped from all EPUB text, as from file names.
/// The implicit marks LRM/RLM (U+200E/U+200F) are kept: they are legitimate in RTL titles.
/// Code points are numbers: GCC's -Wbidi-chars=any also rejects them as universal characters.
[[nodiscard]] constexpr bool is_explicit_bidi_control(std::uint32_t c) noexcept {
    return (c >= 0x202AU && c <= 0x202EU) || (c >= 0x2066U && c <= 0x2069U);
}

[[nodiscard]] bool is_blank(std::string_view text) noexcept {
    return std::ranges::all_of(
        text, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

/// The escaped `text`, or `fallback` (already markup-safe) when nothing visible survives escaping.
[[nodiscard]] std::string escaped_or(std::string_view text, std::string fallback) {
    std::string escaped = xml_escape(text);
    if (is_blank(escaped)) {
        return fallback;
    }
    return escaped;
}

// ---- timestamps --------------------------------------------------------------------------------

/// `count` ASCII digits at `offset` as a number, or nullopt (out of range or not all digits).
[[nodiscard]] constexpr std::optional<int> digits_at(std::string_view text, std::size_t offset,
                                                     std::size_t count) noexcept {
    if (offset > text.size() || count > text.size() - offset) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text.substr(offset, count)) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    return value;
}

/// True if `text` is exactly CCYY-MM-DDThh:mm:ssZ (shape only, as dcterms:modified needs it).
[[nodiscard]] constexpr bool is_utc_timestamp(std::string_view text) noexcept {
    constexpr std::string_view shape = "dddd-dd-ddTdd:dd:ddZ";
    if (text.size() != shape.size()) {
        return false;
    }
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const bool ok = shape[i] == 'd' ? (text[i] >= '0' && text[i] <= '9') : text[i] == shape[i];
        if (!ok) {
            return false;
        }
    }
    return true;
}

// ---- archive names -----------------------------------------------------------------------------

/// "p00001" for page index 0: the manifest id of the page document and its file stem.
[[nodiscard]] std::string page_stem(std::size_t index) { return std::format("p{:05}", index + 1); }

/// The page document's href relative to OEBPS/.
[[nodiscard]] std::string page_href(std::size_t index) {
    return std::format("pages/{}.xhtml", page_stem(index));
}

/// The directory every package file lives in; hrefs inside the package are relative to it.
constexpr std::string_view package_dir = "OEBPS/";

/// `entry` (a zip entry name inside package_dir) as an href relative to package_dir.
[[nodiscard]] std::string package_href(std::string_view entry) {
    if (entry.starts_with(package_dir)) {
        entry.remove_prefix(package_dir.size());
    }
    return std::string{entry};
}

/// The page image's href relative to OEBPS/ (derived from page_image_entry, the naming source).
[[nodiscard]] std::string page_image_href(std::size_t index, MediaType type) {
    return package_href(page_image_entry(index, type));
}

/// Width and height within [1, max_image_side] and a known media type.
[[nodiscard]] Result<void> check_image(std::span<const std::byte> image, ImageInfo info) {
    if (image.empty()) {
        return epub_error("EPUB image is empty");
    }
    if (file_extension(info.type).empty()) {
        return epub_error("EPUB image has an unknown media type");
    }
    if (info.width == 0 || info.height == 0 || info.width > max_image_side ||
        info.height > max_image_side) {
        return epub_error(std::format("EPUB image size {}x{} is outside 1..{}", info.width,
                                      info.height, max_image_side));
    }
    return {};
}

[[nodiscard]] std::string_view progression(Direction direction) noexcept {
    switch (direction) {
    case Direction::ltr:
        return "ltr";
    case Direction::rtl:
        return "rtl";
    }
    return "ltr"; // only reachable through an out-of-range enum value
}

// ---- navigation tree ---------------------------------------------------------------------------

/// One <li> of the table of contents: a chapter, or a volume heading with its chapters.
struct NavNode {
    std::string label{}; ///< escaped, never blank
    std::size_t page = 0;
    std::vector<NavNode> children{};
};

/// Groups consecutive entries that share a `group` under one heading pointing at the first of
/// them. A heading therefore always has at least one child, so no <ol> is ever empty; entries
/// without a group are top-level leaves.
[[nodiscard]] std::vector<NavNode> build_nav_tree(const std::vector<EpubNavEntry>& entries) {
    std::vector<NavNode> roots;
    std::optional<std::string> open_group; // the group of roots.back(), if it is a heading
    for (const EpubNavEntry& entry : entries) {
        NavNode leaf{
            .label = escaped_or(entry.label, std::format("Page {}", entry.first_page + 1)),
            .page = entry.first_page,
            .children = {},
        };
        if (!entry.group) {
            open_group.reset();
            roots.push_back(std::move(leaf));
            continue;
        }
        if (open_group != entry.group || roots.empty()) {
            open_group = entry.group;
            roots.push_back(NavNode{
                .label = escaped_or(*entry.group, std::format("Page {}", entry.first_page + 1)),
                .page = entry.first_page,
                .children = {},
            });
        }
        roots.back().children.push_back(std::move(leaf));
    }
    return roots;
}

void render_nav_list(std::string& out, const std::vector<NavNode>& nodes, std::size_t depth) {
    const std::string indent(2 * depth, ' ');
    std::format_to(std::back_inserter(out), "{}<ol>\n", indent);
    for (const NavNode& node : nodes) {
        std::format_to(std::back_inserter(out), "{}  <li><a href=\"{}\">{}</a>", indent,
                       page_href(node.page), node.label);
        if (!node.children.empty()) {
            out += '\n';
            render_nav_list(out, node.children, depth + 2);
            out += indent;
            out += "  ";
        }
        out += "</li>\n";
    }
    std::format_to(std::back_inserter(out), "{}</ol>\n", indent);
}

/// NCX navPoints. playOrder follows the first appearance of each target, and navPoints that share
/// a target (a volume heading and its first chapter) share its playOrder, as the NCX spec asks.
struct NcxRenderer {
    std::string out{};
    std::map<std::size_t, std::size_t> play_order{}; ///< page index -> playOrder
    std::size_t next_id = 1;

    void render(const std::vector<NavNode>& nodes, std::size_t depth) {
        const std::string indent(2 * depth, ' ');
        for (const NavNode& node : nodes) {
            // The candidate is computed before the call, so a new target gets size() + 1.
            const std::size_t candidate = play_order.size() + 1;
            const std::size_t order = play_order.try_emplace(node.page, candidate).first->second;
            std::format_to(std::back_inserter(out),
                           "{}<navPoint id=\"navpoint-{}\" playOrder=\"{}\">\n"
                           "{}  <navLabel><text>{}</text></navLabel>\n"
                           "{}  <content src=\"{}\"/>\n",
                           indent, next_id, order, indent, node.label, indent,
                           page_href(node.page));
            ++next_id;
            render(node.children, depth + 1);
            std::format_to(std::back_inserter(out), "{}</navPoint>\n", indent);
        }
    }
};

[[nodiscard]] std::size_t tree_depth(const std::vector<NavNode>& nodes) {
    std::size_t depth = 0;
    for (const NavNode& node : nodes) {
        depth = std::max(depth, 1 + tree_depth(node.children));
    }
    return depth;
}

// ---- manifest ----------------------------------------------------------------------------------

/// Ok when `chapters` tile the pages [0, total) exactly, in order: each starts where the previous
/// one ended, covers at least one page (a manifest chapter always has an image), and the last
/// ends at `total`. The bounds are checked before any sum, so no addition can wrap.
[[nodiscard]] Result<void> check_tiling(std::span<const EpubBookChapter> chapters,
                                        std::size_t total) {
    std::size_t next = 0; // the first page not yet covered; never above total
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const EpubBookChapter& chapter = chapters[i];
        if (chapter.first_page > total || chapter.page_count > total - chapter.first_page) {
            return epub_error(std::format("EPUB manifest chapter {} covers pages past the {} added",
                                          i + 1, total));
        }
        if (chapter.first_page != next) {
            return epub_error(std::format("EPUB manifest chapter {} starts at page {}, not {}",
                                          i + 1, chapter.first_page + 1, next + 1));
        }
        if (chapter.page_count == 0) {
            return epub_error(std::format("EPUB manifest chapter {} covers no pages", i + 1));
        }
        // first_page == next and page_count <= total - first_page, so this stays <= total.
        next += chapter.page_count;
    }
    if (next != total) {
        return epub_error(
            std::format("EPUB manifest chapters cover {} of the {} pages added", next, total));
    }
    return {};
}

} // namespace

// ---- free functions ----------------------------------------------------------------------------

std::string page_image_entry(std::size_t index, MediaType type) {
    return std::format("{}images/{}.{}", package_dir, page_stem(index), file_extension(type));
}

std::string cover_image_entry(MediaType type) {
    return std::format("{}images/cover.{}", package_dir, file_extension(type));
}

std::string xml_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    while (!text.empty()) {
        const std::optional<CodePoint> cp = decode_utf8(text);
        if (!cp) {
            // Invalid UTF-8: drop one byte and resynchronise. Continuation bytes never start a
            // sequence, so this drops exactly the ill-formed bytes.
            text.remove_prefix(1);
            continue;
        }
        const std::string_view sequence = text.substr(0, cp->length);
        text.remove_prefix(cp->length);
        // Every text that reaches the package (metadata, nav and NCX labels, page titles) goes
        // through here, so dropping the bidi controls here covers all of it.
        if (!is_xml_char(cp->value) || is_explicit_bidi_control(cp->value)) {
            continue;
        }
        switch (cp->value) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;"; // also keeps "]]>" out of character data
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;"; // not &apos;, which HTML-parsing readers may not know
            break;
        default:
            out += sequence;
            break;
        }
    }
    return out;
}

std::string container_xml() {
    std::string out{xml_declaration};
    out += "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
           "  <rootfiles>\n"
           "    <rootfile full-path=\"OEBPS/content.opf\" "
           "media-type=\"application/oebps-package+xml\"/>\n"
           "  </rootfiles>\n"
           "</container>\n";
    return out;
}

std::string page_xhtml(std::string_view image_href, ImageInfo info, std::string_view title) {
    std::string out{xml_declaration};
    std::format_to(std::back_inserter(out),
                   "<!DOCTYPE html>\n"
                   "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                   "xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
                   "<head>\n"
                   "<meta charset=\"UTF-8\"/>\n"
                   "<title>{0}</title>\n"
                   "<meta name=\"viewport\" content=\"width={1}, height={2}\"/>\n"
                   "<style>html, body {{ margin: 0; padding: 0; }} "
                   "img {{ display: block; width: {1}px; height: {2}px; }}</style>\n"
                   "</head>\n"
                   "<body>\n"
                   "<div><img src=\"{3}\" alt=\"\" width=\"{1}\" height=\"{2}\"/></div>\n"
                   "</body>\n"
                   "</html>\n",
                   xml_escape(title), info.width, info.height, xml_escape(image_href));
    return out;
}

Result<std::string> format_modified(std::string_view iso8601) {
    // CCYY-MM-DDThh:mm:ss[.fraction](Z|+hh:mm|-hh:mm). The input is untrusted API data, so the
    // error never echoes it.
    const auto invalid = [] {
        return epub_error("timestamp is not ISO 8601 with a UTC offset "
                          "(CCYY-MM-DDThh:mm:ss+hh:mm)");
    };
    const std::optional<int> year = digits_at(iso8601, 0, 4);
    const std::optional<int> month = digits_at(iso8601, 5, 2);
    const std::optional<int> day = digits_at(iso8601, 8, 2);
    const std::optional<int> hour = digits_at(iso8601, 11, 2);
    const std::optional<int> minute = digits_at(iso8601, 14, 2);
    const std::optional<int> second = digits_at(iso8601, 17, 2);
    constexpr std::size_t fixed_size = 19;
    if (!year || !month || !day || !hour || !minute || !second || iso8601[4] != '-' ||
        iso8601[7] != '-' || iso8601[10] != 'T' || iso8601[13] != ':' || iso8601[16] != ':') {
        return invalid();
    }
    std::string_view rest = iso8601.substr(fixed_size);
    if (rest.starts_with('.')) {
        const std::size_t end = rest.find_first_not_of("0123456789", 1);
        if (end == 1 || end == std::string_view::npos) {
            return invalid(); // a '.' needs at least one digit, and an offset must follow
        }
        rest.remove_prefix(end); // fractional seconds are dropped (truncated)
    }
    int offset_minutes = 0; // "Z"
    if (rest != "Z") {
        const std::optional<int> offset_hour = digits_at(rest, 1, 2);
        const std::optional<int> offset_minute = digits_at(rest, 4, 2);
        if (rest.size() != 6 || (rest[0] != '+' && rest[0] != '-') || rest[3] != ':' ||
            !offset_hour || !offset_minute || *offset_hour > 23 || *offset_minute > 59) {
            return invalid();
        }
        offset_minutes = ((*offset_hour * 60) + *offset_minute) * (rest[0] == '-' ? -1 : 1);
    }
    if (*hour > 23 || *minute > 59 || *second > 59) {
        return invalid();
    }

    namespace chrono = std::chrono;
    // Both month and day are 0..99 here (two digits), so the unsigned conversions are exact.
    const chrono::year_month_day date{chrono::year{*year},
                                      chrono::month{static_cast<unsigned>(*month)},
                                      chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return invalid();
    }
    // Calendar arithmetic on sys_days (a year_month_day plus days does not compile, V-D3).
    const chrono::sys_seconds local = chrono::sys_days{date} + chrono::hours{*hour} +
                                      chrono::minutes{*minute} + chrono::seconds{*second};
    const chrono::sys_seconds utc = local - chrono::minutes{offset_minutes};
    const chrono::sys_days utc_day = chrono::floor<chrono::sys_days::duration>(utc);
    const chrono::year_month_day utc_date{utc_day};
    const chrono::hh_mm_ss<chrono::seconds> utc_time{utc - utc_day};
    const int utc_year = static_cast<int>(utc_date.year());
    if (utc_year < 0 || utc_year > 9999) {
        return epub_error("timestamp converts to a year outside 0000..9999");
    }
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc_year,
                       static_cast<unsigned>(utc_date.month()),
                       static_cast<unsigned>(utc_date.day()), utc_time.hours().count(),
                       utc_time.minutes().count(), utc_time.seconds().count());
}

// ---- EpubWriter --------------------------------------------------------------------------------

struct EpubWriter::State {
    enum class Phase {
        created,  ///< begin() not called yet
        open,     ///< accepting the cover, pages and nav entries
        finished, ///< finish() succeeded
        failed,   ///< a zip write failed: the archive is truncated, refuse further use
    };

    ZipWriter* zip = nullptr; ///< borrowed; outlives the writer (constructor contract)
    EpubMetadata meta{};
    Phase phase = Phase::created;
    std::optional<MediaType> cover{};
    std::vector<MediaType> pages{}; ///< per page, in reading order
    std::vector<EpubNavEntry> nav{};

    [[nodiscard]] Result<void> require_open(std::string_view call) const {
        switch (phase) {
        case Phase::created:
            return epub_error(std::format("EPUB {} called before begin()", call));
        case Phase::open:
            return {};
        case Phase::finished:
            return epub_error(std::format("EPUB {} called after finish()", call));
        case Phase::failed:
            return epub_error("EPUB archive is unusable after an earlier write failure");
        }
        return epub_error("EPUB writer is in an invalid state");
    }

    /// Adds one STORED entry; `data` is a local of the caller (never the sink's storage).
    [[nodiscard]] Result<void> add(std::string_view name, std::span<const std::byte> data) {
        Result<void> result = zip->add_stored(name, data);
        if (!result) {
            phase = Phase::failed;
        }
        return result;
    }

    [[nodiscard]] Result<void> add_text(std::string_view name, std::string_view text) {
        return add(name, bytes_of(text));
    }

    [[nodiscard]] std::string cover_image_id() const {
        return cover ? std::string{"cover-image"} : std::format("img-{}", page_stem(0));
    }

    [[nodiscard]] std::string content_opf() const {
        std::string out{xml_declaration};
        const std::string language = xml_escape(meta.language);
        std::format_to(std::back_inserter(out),
                       "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                       "unique-identifier=\"book-id\" xml:lang=\"{}\">\n"
                       "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                       "    <dc:identifier id=\"book-id\">{}</dc:identifier>\n"
                       "    <dc:title id=\"title\">{}</dc:title>\n"
                       "    <dc:language>{}</dc:language>\n",
                       language, xml_escape(meta.identifier), xml_escape(meta.title), language);

        // Creators and contributors: blank names skipped, exact repeats (an author who is also the
        // artist) listed once, first occurrence wins.
        const auto people = [&out](const std::vector<std::string>& names, std::string_view element,
                                   std::string_view prefix) {
            std::vector<std::string> seen;
            for (const std::string& name : names) {
                std::string escaped = xml_escape(name);
                if (is_blank(escaped) || std::ranges::contains(seen, escaped)) {
                    continue;
                }
                std::format_to(std::back_inserter(out), "    <dc:{0} id=\"{1}-{2}\">{3}</dc:{0}>\n",
                               element, prefix, seen.size() + 1, escaped);
                seen.push_back(std::move(escaped));
            }
        };
        people(meta.creators, "creator", "creator");
        people(meta.contributors, "contributor", "contributor");

        if (const std::string source = xml_escape(meta.source); !is_blank(source)) {
            std::format_to(std::back_inserter(out), "    <dc:source>{}</dc:source>\n", source);
        }
        std::format_to(std::back_inserter(out),
                       "    <meta property=\"dcterms:modified\">{}</meta>\n", meta.modified);
        if (const std::string series = xml_escape(meta.series); !is_blank(series)) {
            std::format_to(std::back_inserter(out),
                           "    <meta property=\"belongs-to-collection\" id=\"series\">{}</meta>\n"
                           "    <meta refines=\"#series\" property=\"collection-type\">series"
                           "</meta>\n",
                           series);
            if (meta.series_position) {
                if (const std::string position = xml_escape(*meta.series_position);
                    !is_blank(position)) {
                    std::format_to(
                        std::back_inserter(out),
                        "    <meta refines=\"#series\" property=\"group-position\">{}</meta>\n",
                        position);
                }
            }
        }
        std::format_to(
            std::back_inserter(out),
            "    <meta property=\"rendition:layout\">pre-paginated</meta>\n"
            "    <meta property=\"rendition:orientation\">auto</meta>\n"
            "    <meta property=\"rendition:spread\">landscape</meta>\n"
            "    <meta name=\"cover\" content=\"{}\"/>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
            "properties=\"nav\"/>\n"
            "    <item id=\"ncx\" href=\"toc.ncx\" "
            "media-type=\"application/x-dtbncx+xml\"/>\n",
            cover_image_id());
        if (cover) {
            std::format_to(std::back_inserter(out),
                           "    <item id=\"cover-image\" href=\"{}\" "
                           "media-type=\"{}\" properties=\"cover-image\"/>\n"
                           "    <item id=\"cover\" href=\"cover.xhtml\" "
                           "media-type=\"application/xhtml+xml\"/>\n",
                           package_href(cover_image_entry(*cover)), media_type_name(*cover));
        }
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const std::string_view properties =
                !cover && i == 0 ? std::string_view{" properties=\"cover-image\""}
                                 : std::string_view{};
            std::format_to(std::back_inserter(out),
                           "    <item id=\"img-{0}\" href=\"{1}\" media-type=\"{2}\"{3}/>\n"
                           "    <item id=\"{0}\" href=\"{4}\" "
                           "media-type=\"application/xhtml+xml\"/>\n",
                           page_stem(i), page_image_href(i, pages[i]), media_type_name(pages[i]),
                           properties, page_href(i));
        }
        std::format_to(std::back_inserter(out),
                       "  </manifest>\n"
                       "  <spine toc=\"ncx\" page-progression-direction=\"{}\">\n",
                       progression(meta.direction));
        if (cover) {
            out += "    <itemref idref=\"cover\"/>\n";
        }
        for (std::size_t i = 0; i < pages.size(); ++i) {
            std::format_to(std::back_inserter(out), "    <itemref idref=\"{}\"/>\n", page_stem(i));
        }
        out += "  </spine>\n"
               "</package>\n";
        return out;
    }

    [[nodiscard]] std::string nav_xhtml(const std::vector<NavNode>& tree) const {
        std::string out{xml_declaration};
        const std::string language = xml_escape(meta.language);
        const std::string title = xml_escape(meta.title);
        std::format_to(
            std::back_inserter(out),
            "<!DOCTYPE html>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
            "xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"{0}\" lang=\"{0}\">\n"
            "<head>\n"
            "<meta charset=\"UTF-8\"/>\n"
            "<title>{1}</title>\n"
            "</head>\n"
            "<body>\n"
            "<nav epub:type=\"toc\" id=\"toc\">\n"
            "<h1>{1}</h1>\n",
            language, title);
        render_nav_list(out, tree, 0);
        out += "</nav>\n"
               "</body>\n"
               "</html>\n";
        return out;
    }

    [[nodiscard]] std::string toc_ncx(const std::vector<NavNode>& tree) const {
        NcxRenderer navmap;
        navmap.render(tree, 2);
        std::string out{xml_declaration};
        std::format_to(std::back_inserter(out),
                       "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\" "
                       "xml:lang=\"{}\">\n"
                       "  <head>\n"
                       "    <meta name=\"dtb:uid\" content=\"{}\"/>\n"
                       "    <meta name=\"dtb:depth\" content=\"{}\"/>\n"
                       "    <meta name=\"dtb:totalPageCount\" content=\"0\"/>\n"
                       "    <meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n"
                       "  </head>\n"
                       "  <docTitle><text>{}</text></docTitle>\n"
                       "  <navMap>\n"
                       "{}"
                       "  </navMap>\n"
                       "</ncx>\n",
                       xml_escape(meta.language), xml_escape(meta.identifier), tree_depth(tree),
                       xml_escape(meta.title), navmap.out);
        return out;
    }

    /// Validates the pages and nav entries, then writes content.opf, nav.xhtml and toc.ncx.
    /// Returns the CRC-32 of the exact content.opf bytes written. Leaves `phase` to the caller.
    [[nodiscard]] Result<std::uint32_t> write_package() {
        if (pages.empty()) {
            return epub_error("EPUB has no pages");
        }
        for (const EpubNavEntry& entry : nav) {
            if (entry.first_page >= pages.size()) {
                return epub_error(std::format("EPUB navigation entry points at page {} of {}",
                                              entry.first_page + 1, pages.size()));
            }
        }
        // The toc <ol> must not be empty: without entries, one points at the first page. Built by
        // push_back into a named vector (no initializer_list of a temporary node).
        std::vector<NavNode> tree = build_nav_tree(nav);
        if (tree.empty()) {
            tree.push_back(
                NavNode{.label = escaped_or(meta.title, "Page 1"), .page = 0, .children = {}});
        }

        const std::string opf = content_opf();
        if (Result<void> ok = add_text(opf_entry_name, opf); !ok) {
            return std::unexpected(std::move(ok.error()));
        }
        if (Result<void> ok = add_text("OEBPS/nav.xhtml", nav_xhtml(tree)); !ok) {
            return std::unexpected(std::move(ok.error()));
        }
        if (Result<void> ok = add_text("OEBPS/toc.ncx", toc_ncx(tree)); !ok) {
            return std::unexpected(std::move(ok.error()));
        }
        return crc32(bytes_of(opf));
    }
};

EpubWriter::EpubWriter(ZipWriter& zip, EpubMetadata meta) : state_(std::make_unique<State>()) {
    state_->zip = &zip;
    state_->meta = std::move(meta);
}

EpubWriter::~EpubWriter() = default;

Result<void> EpubWriter::begin() {
    State& s = *state_;
    if (s.phase != State::Phase::created) {
        return epub_error("EPUB begin() called twice");
    }
    if (s.zip->entry_count() != 0) {
        return epub_error("EPUB mimetype must be the first entry of the archive");
    }
    const EpubMetadata& meta = s.meta;
    if (is_blank(xml_escape(meta.identifier)) || is_blank(xml_escape(meta.title)) ||
        is_blank(xml_escape(meta.language))) {
        return epub_error("EPUB metadata needs an identifier, a title and a language");
    }
    if (!is_utc_timestamp(meta.modified)) {
        return epub_error("EPUB modified time must be CCYY-MM-DDThh:mm:ssZ");
    }
    s.phase = State::Phase::open;
    if (Result<void> ok = s.add_text("mimetype", epub_media_type); !ok) {
        return ok;
    }
    return s.add_text("META-INF/container.xml", container_xml());
}

Result<void> EpubWriter::add_cover(std::span<const std::byte> image, ImageInfo info) {
    State& s = *state_;
    if (Result<void> ok = s.require_open("add_cover()"); !ok) {
        return ok;
    }
    if (s.cover) {
        return epub_error("EPUB add_cover() called twice");
    }
    if (!s.pages.empty()) {
        return epub_error("EPUB add_cover() called after add_page()");
    }
    if (Result<void> ok = check_image(image, info); !ok) {
        return ok;
    }
    const std::string entry = cover_image_entry(info.type);
    s.cover = info.type;
    if (Result<void> ok = s.add(entry, image); !ok) {
        return ok;
    }
    const std::string href = package_href(entry);
    return s.add_text("OEBPS/cover.xhtml", page_xhtml(href, info, "Cover"));
}

Result<void> EpubWriter::add_page(std::span<const std::byte> image, ImageInfo info) {
    State& s = *state_;
    if (Result<void> ok = s.require_open("add_page()"); !ok) {
        return ok;
    }
    if (Result<void> ok = check_image(image, info); !ok) {
        return ok;
    }
    const std::size_t index = s.pages.size();
    s.pages.push_back(info.type);
    if (Result<void> ok = s.add(page_image_entry(index, info.type), image); !ok) {
        return ok;
    }
    // The page document lives in OEBPS/pages/, its image in OEBPS/images/.
    return s.add_text(std::format("OEBPS/{}", page_href(index)),
                      page_xhtml(std::format("../{}", page_image_href(index, info.type)), info,
                                 std::format("Page {}", index + 1)));
}

void EpubWriter::add_nav_entry(EpubNavEntry entry) { state_->nav.push_back(std::move(entry)); }

Result<void> EpubWriter::finish() {
    State& s = *state_;
    if (Result<void> ok = s.require_open("finish()"); !ok) {
        return ok;
    }
    if (Result<std::uint32_t> opf_crc = s.write_package(); !opf_crc) {
        return std::unexpected(std::move(opf_crc.error()));
    }
    s.phase = State::Phase::finished;
    return {};
}

Result<void> EpubWriter::finish(std::optional<std::string> cover_file_name,
                                std::vector<EpubBookChapter> chapters) {
    State& s = *state_;
    if (Result<void> ok = s.require_open("finish()"); !ok) {
        return ok;
    }
    if (Result<void> ok = check_tiling(chapters, s.pages.size()); !ok) {
        return ok;
    }
    // Everything but the OPF CRC is known before the package is written.
    BookManifest manifest{
        .identifier = s.meta.identifier,
        .opf_crc32 = 0,
        .cover = std::nullopt,
        .chapters = {},
    };
    if (s.cover && cover_file_name) {
        manifest.cover = ManifestCover{
            .file_name = std::move(*cover_file_name),
            .entry = cover_image_entry(*s.cover),
        };
    }
    manifest.chapters.reserve(chapters.size());
    for (EpubBookChapter& chapter : chapters) {
        ManifestChapter& record = chapter.record;
        record.images.clear();
        record.images.reserve(chapter.page_count);
        // check_tiling: [first_page, first_page + page_count) lies within s.pages.
        for (std::size_t i = 0; i < chapter.page_count; ++i) {
            const std::size_t page = chapter.first_page + i;
            record.images.push_back(page_image_entry(page, s.pages[page]));
        }
        manifest.chapters.push_back(std::move(record));
    }

    Result<std::uint32_t> opf_crc = s.write_package();
    if (!opf_crc) {
        return std::unexpected(std::move(opf_crc.error()));
    }
    manifest.opf_crc32 = *opf_crc;
    // The last entry, after toc.ncx (a re-run finds it at the end of the central directory).
    if (Result<void> ok = s.add_text(manifest_entry_name, manifest_to_json(manifest)); !ok) {
        return ok;
    }
    s.phase = State::Phase::finished;
    return {};
}

} // namespace mangadex_dl
