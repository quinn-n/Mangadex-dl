// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixed-layout EPUB 3 generation on top of ZipWriter, plus the pure XML generators it uses.
// Frozen interface (download plan T1); implemented in src/core/epub.cpp.
#pragma once

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/image.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/zip.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mangadex_dl {

/// One table-of-contents entry (a chapter).
struct EpubNavEntry {
    std::string label{};
    std::size_t first_page = 0;         ///< 0-based index into the pages added so far
    std::optional<std::string> group{}; ///< layout single: the volume label it is nested under

    friend bool operator==(const EpubNavEntry&, const EpubNavEntry&) = default;
};

struct EpubMetadata {
    std::string identifier{};                ///< dc:identifier, urn:mangadex:<id>:<lang>:<unit>
    std::string title{};                     ///< dc:title
    std::string language{};                  ///< dc:language
    std::vector<std::string> creators{};     ///< authors, then artists
    std::vector<std::string> contributors{}; ///< scanlation groups
    std::string source{};                    ///< dc:source, the title URL
    std::string modified{};                  ///< CCYY-MM-DDThh:mm:ssZ (see format_modified)
    std::string series{};                    ///< belongs-to-collection; empty for none
    std::optional<std::string> series_position{};
    Direction direction = Direction::ltr;

    friend bool operator==(const EpubMetadata&, const EpubMetadata&) = default;
};

/// Writes one book: begin(), optionally add_cover(), add_page() per page in reading order,
/// add_nav_entry() as chapters start, then finish(). Calls out of order fail with
/// Errc::failure. Output is deterministic (no clocks, fixed ordering).
///
/// Borrows the ZipWriter, which must outlive this writer (and whose sink must outlive both).
/// Not thread-safe; neither copyable nor movable. The state lives in epub.cpp (pimpl).
class EpubWriter {
public:
    EpubWriter(ZipWriter& zip MANGADEX_DL_LIFETIMEBOUND, EpubMetadata meta);
    EpubWriter(const EpubWriter&) = delete;
    EpubWriter& operator=(const EpubWriter&) = delete;
    EpubWriter(EpubWriter&&) = delete;
    EpubWriter& operator=(EpubWriter&&) = delete;
    ~EpubWriter();

    /// Writes `mimetype` (first, STORED, no extra field) and META-INF/container.xml.
    [[nodiscard]] Result<void> begin();

    /// Adds the cover image and its XHTML page. Without it the first page image is the cover.
    /// `image` is only read during the call and must not borrow from the ZipWriter's sink
    /// (ZipWriter::add_stored precondition). Add the cover before the first page.
    [[nodiscard]] Result<void> add_cover(std::span<const std::byte> image, ImageInfo info);

    /// Adds the next page image and its XHTML page (viewport = the image size). `image` is only
    /// read during the call and must not borrow from the ZipWriter's sink.
    [[nodiscard]] Result<void> add_page(std::span<const std::byte> image, ImageInfo info);

    /// Records a navigation entry; written out by finish().
    void add_nav_entry(EpubNavEntry entry);

    /// Writes content.opf, nav.xhtml and toc.ncx. Does not finish the ZipWriter.
    [[nodiscard]] Result<void> finish();

private:
    struct State;
    std::unique_ptr<State> state_{};
};

/// Escapes `& < > " '` and drops characters XML 1.0 forbids, invalid UTF-8 sequences and
/// explicit bidi embedding/override/isolate controls (U+202A–U+202E, U+2066–U+2069).
[[nodiscard]] std::string xml_escape(std::string_view text);

/// META-INF/container.xml pointing at OEBPS/content.opf.
[[nodiscard]] std::string container_xml();

/// One fixed-layout XHTML page showing `image_href` at the image's size.
[[nodiscard]] std::string page_xhtml(std::string_view image_href, ImageInfo info,
                                     std::string_view title);

/// ISO 8601 with an offset ("2024-01-02T03:04:05+09:00") to UTC "CCYY-MM-DDThh:mm:ssZ" (no
/// fractional seconds). Errc::failure for anything else.
[[nodiscard]] Result<std::string> format_modified(std::string_view iso8601);

} // namespace mangadex_dl
