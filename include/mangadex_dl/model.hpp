// SPDX-License-Identifier: GPL-3.0-or-later
//
// Domain value types shared by the API parsers, the planner, the EPUB writer and the downloader.
// Frozen interface (download plan T1). Every type here is an aggregate that owns its data (no
// views, pointers or iterators), so values may be copied freely and moved across threads.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mangadex_dl {

/// Reading direction of the generated book (spine page-progression-direction).
enum class Direction {
    ltr,
    rtl,
};

/// --direction: `auto` resolves from the title's original language (resolve_direction).
enum class DirectionChoice {
    automatic,
    ltr,
    rtl,
};

/// --layout.
enum class Layout {
    volumes,  ///< one file per volume, plus one per volume-less ("loose") chapter and oneshot
    chapters, ///< one file per chapter
    single,   ///< the whole selection in one file
};

/// What one output file contains.
enum class EpubKind {
    volume,
    chapter,
    oneshot,
    single,
};

/// Why a chapter from the feed is not downloaded.
enum class SkipReason {
    external,    ///< hosted off MangaDex (externalUrl is set)
    no_pages,    ///< pages == 0
    unavailable, ///< isUnavailable is true
    duplicate,   ///< another release of the same volume+chapter was kept
};

/// --data-saver selects the compressed image set.
enum class ImageQuality {
    data,
    data_saver,
};

/// A scanlation group as listed on a chapter.
struct ScanGroup {
    std::string id{};
    std::string name{};

    friend bool operator==(const ScanGroup&, const ScanGroup&) = default;
};

/// One chapter from the feed. `id` satisfies is_uuid() (the parser rejects anything else).
struct Chapter {
    std::string id{};
    std::optional<std::string> volume{};  ///< null for a volume-less ("loose") chapter
    std::optional<std::string> chapter{}; ///< null for a oneshot
    std::string title{};                  ///< empty when the API has none
    std::string language{};               ///< translatedLanguage
    std::vector<ScanGroup> groups{};      ///< in API order
    int pages = 0;
    bool external = false;    ///< externalUrl is non-null
    bool unavailable = false; ///< isUnavailable
    std::string publish_at{}; ///< ISO 8601 as sent by the API, offset included
    std::string updated_at{}; ///< ISO 8601 as sent by the API, offset included

    friend bool operator==(const Chapter&, const Chapter&) = default;
};

/// One cover image. Both `volume` and `locale` are nullable in the API.
struct Cover {
    std::optional<std::string> volume{}; ///< null never matches a volume
    std::string file_name{};
    std::optional<std::string> locale{}; ///< null ranks after any non-null locale

    friend bool operator==(const Cover&, const Cover&) = default;
};

/// (language code, text).
using LocalizedText = std::pair<std::string, std::string>;

/// The title record (GET /manga/{id} with cover_art, author and artist includes).
struct MangaInfo {
    std::string id{};
    /// attributes.title entries, in language-code order (the parser's JSON objects are std::map;
    /// an insertion-ordered object would cost a quadratic duplicate-key check on untrusted input).
    std::vector<LocalizedText> titles{};
    /// attributes.altTitles: the array keeps API order; the entries of each object are in
    /// language-code order.
    std::vector<LocalizedText> alt_titles{};
    std::string original_language{};
    std::vector<std::string> authors{}; ///< names
    std::vector<std::string> artists{}; ///< names

    friend bool operator==(const MangaInfo&, const MangaInfo&) = default;
};

/// One page of GET /manga/{id}/feed.
struct FeedPage {
    std::vector<Chapter> chapters{};
    std::size_t offset = 0;
    std::size_t limit = 0;
    std::size_t total = 0;

    friend bool operator==(const FeedPage&, const FeedPage&) = default;
};

/// One page of GET /cover.
struct CoverPage {
    std::vector<Cover> covers{};
    std::size_t offset = 0;
    std::size_t limit = 0;
    std::size_t total = 0;

    friend bool operator==(const CoverPage&, const CoverPage&) = default;
};

/// GET /at-home/server/{chapterId}: where one chapter's images are served from. `base_url`,
/// `hash` and every file name were validated by the parser (https, safe tokens).
struct AtHomeServer {
    std::string base_url{}; ///< no trailing '/'
    std::string hash{};
    std::vector<std::string> data{};       ///< full-quality file names, in page order
    std::vector<std::string> data_saver{}; ///< compressed file names, in page order

    friend bool operator==(const AtHomeServer&, const AtHomeServer&) = default;
};

// ---- the plan --------------------------------------------------------------------------------

/// One output file. It holds copies of its chapters (never pointers or iterators into the feed).
struct PlannedEpub {
    EpubKind kind = EpubKind::volume;
    std::string file_name{};     ///< one path component, already sanitized, ends in ".epub"
    std::string display_title{}; ///< "<T> Vol. 3", "<T> Ch. 12", ...
    std::optional<std::string> volume{};
    std::optional<std::string> chapter{};
    std::vector<Chapter> chapters{}; ///< in reading order
    std::optional<Cover> cover{};    ///< nullopt: the first page is the cover
    std::string identifier{};        ///< urn:mangadex:<id>:<lang>:<unit>
    std::optional<std::string> series_position{};
    std::size_t planned_pages = 0;
    /// Set by the planner when this file cannot be built: it would exceed
    /// max_zip_entries_per_epub zip entries, or one of its chapters has more than
    /// max_pages_per_chapter pages. The downloader then reports the file as failed with this
    /// message, without downloading anything for it. nullopt for a buildable file.
    std::optional<std::string> plan_error{};

    friend bool operator==(const PlannedEpub&, const PlannedEpub&) = default;
};

/// A feed chapter that is not downloaded, and why.
struct SkippedChapter {
    std::string id{};
    std::optional<std::string> volume{};
    std::optional<std::string> chapter{};
    SkipReason reason = SkipReason::external;
    std::optional<std::string> kept{}; ///< for duplicate: the id of the release that was kept

    friend bool operator==(const SkippedChapter&, const SkippedChapter&) = default;
};

/// One dedupe decision (for -v output).
struct DedupeNote {
    std::string key{};     ///< e.g. "Vol. 1 Ch. 1" or "Ch. 10" (normalized labels)
    std::string kept_id{}; ///< chapter id that was kept
    std::vector<std::string> dropped_ids{};
    std::string rule{}; ///< which rule decided, for humans

    friend bool operator==(const DedupeNote&, const DedupeNote&) = default;
};

/// The title as reported to the user.
struct MangaSummary {
    std::string id{};
    std::string title{};    ///< resolved (or --title)
    std::string url{};      ///< https://mangadex.org/title/<id>
    std::string language{}; ///< the --lang that was downloaded
    Direction direction = Direction::ltr;

    friend bool operator==(const MangaSummary&, const MangaSummary&) = default;
};

/// What will be written, in output order. An empty `epubs` list is valid (exit 5 at the caller).
struct Plan {
    MangaSummary manga{};
    Layout layout = Layout::volumes;
    std::vector<PlannedEpub> epubs{};
    std::vector<SkippedChapter> skipped{};
    std::vector<DedupeNote> dedupe{};

    friend bool operator==(const Plan&, const Plan&) = default;
};

} // namespace mangadex_dl
