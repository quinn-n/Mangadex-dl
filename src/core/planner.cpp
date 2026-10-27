// SPDX-License-Identifier: GPL-3.0-or-later
//
// The planner (download plan T9): language filter, selection, skips, dedupe, ordering, grouping per
// layout, covers, identifiers and file names. Pure and deterministic: every sort is by a total
// order that ends in the (unique) chapter id, so the input order never shows in the plan.
#include "mangadex_dl/planner.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/filename.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mangadex_api.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/selection.hpp"

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

/// mimetype, META-INF/container.xml, OEBPS/content.opf, OEBPS/nav.xhtml and OEBPS/toc.ncx.
inline constexpr std::size_t fixed_epub_entries = 5;
/// The page image and its XHTML page.
inline constexpr std::size_t entries_per_page = 2;
/// The cover image and its XHTML page.
inline constexpr std::size_t cover_entries = 2;
/// A file name is one path component.
inline constexpr std::size_t max_component_bytes = 255;
inline constexpr std::string_view title_url_prefix = "https://mangadex.org/title/";

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string ascii_lower_copy(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), ascii_lower);
    return out;
}

[[nodiscard]] bool ascii_iequal(std::string_view lhs, std::string_view rhs) noexcept {
    return std::ranges::equal(
        lhs, rhs, [](char a, char b) noexcept { return ascii_lower(a) == ascii_lower(b); });
}

// ---- labels ------------------------------------------------------------------------------------

enum class LabelRank {
    numeric, ///< sorts first, by value
    text,    ///< then non-numeric labels, bytewise
    none,    ///< a null (or empty) label sorts last
};

/// The sort and grouping key of one volume or chapter label.
struct LabelKey {
    LabelRank rank = LabelRank::none;
    std::optional<ChapterNumber> number{}; ///< set for numeric labels only
    std::string text{};                    ///< the normalized label; empty for none

    friend bool operator==(const LabelKey& lhs, const LabelKey& rhs) {
        return lhs.rank == rhs.rank && lhs.text == rhs.text;
    }
};

[[nodiscard]] std::weak_ordering compare_labels(const LabelKey& lhs, const LabelKey& rhs) {
    if (lhs.rank != rhs.rank) {
        return lhs.rank <=> rhs.rank;
    }
    if (lhs.rank == LabelRank::numeric && lhs.number && rhs.number) {
        return *lhs.number <=> *rhs.number;
    }
    return lhs.text <=> rhs.text;
}

/// An empty label is treated like a null one.
[[nodiscard]] LabelKey make_label_key(const std::optional<std::string>& label) {
    if (!label || label->empty()) {
        return LabelKey{};
    }
    std::optional<ChapterNumber> number = ChapterNumber::parse(*label);
    if (number) {
        std::string text = number->normalized();
        return LabelKey{
            .rank = LabelRank::numeric, .number = std::move(number), .text = std::move(text)};
    }
    return LabelKey{.rank = LabelRank::text, .number = std::nullopt, .text = *label};
}

[[nodiscard]] std::optional<std::string> label_value(const LabelKey& key) {
    return key.rank == LabelRank::none ? std::nullopt : std::optional<std::string>{key.text};
}

// ---- publishAt ---------------------------------------------------------------------------------

/// Parses exactly `width` (at most 4) decimal digits at `pos`; nullopt if any is not a digit.
/// Locale-independent; four digits cannot overflow an int.
[[nodiscard]] std::optional<int> fixed_digits(std::string_view text, std::size_t pos,
                                              std::size_t width) noexcept {
    if (width > 4 || pos > text.size() || text.size() - pos < width) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text.substr(pos, width)) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    return value;
}

/// `YYYY-MM-DDThh:mm:ss[.fraction](Z|±hh:mm)` to UTC seconds (the fraction is ignored). nullopt
/// for anything else, including impossible dates.
[[nodiscard]] std::optional<std::chrono::sys_seconds> parse_timestamp(std::string_view text) {
    using std::chrono::hours;
    using std::chrono::minutes;
    using std::chrono::seconds;
    const auto separator_at = [text](std::size_t pos, char c) noexcept {
        return pos < text.size() && text[pos] == c;
    };
    const std::optional<int> year = fixed_digits(text, 0, 4);
    const std::optional<int> month = fixed_digits(text, 5, 2);
    const std::optional<int> day = fixed_digits(text, 8, 2);
    const std::optional<int> hour = fixed_digits(text, 11, 2);
    const std::optional<int> minute = fixed_digits(text, 14, 2);
    const std::optional<int> second = fixed_digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second || !separator_at(4, '-') ||
        !separator_at(7, '-') || !separator_at(10, 'T') || !separator_at(13, ':') ||
        !separator_at(16, ':') || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::size_t pos = 19;
    if (separator_at(pos, '.')) {
        ++pos;
        const std::size_t digits_begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
        if (pos == digits_begin) {
            return std::nullopt;
        }
    }
    minutes offset{0};
    if (separator_at(pos, 'Z') && pos + 1 == text.size()) {
        // UTC
    } else if ((separator_at(pos, '+') || separator_at(pos, '-')) && text.size() - pos == 6 &&
               separator_at(pos + 3, ':')) {
        const std::optional<int> offset_hours = fixed_digits(text, pos + 1, 2);
        const std::optional<int> offset_minutes = fixed_digits(text, pos + 4, 2);
        if (!offset_hours || !offset_minutes || *offset_hours > 23 || *offset_minutes > 59) {
            return std::nullopt;
        }
        offset = hours{*offset_hours} + minutes{*offset_minutes};
        if (text[pos] == '-') {
            offset = -offset;
        }
    } else {
        return std::nullopt;
    }
    // Calendar arithmetic on sys_days (year_month_day + days does not exist).
    const std::chrono::sys_days midnight{date};
    return std::chrono::sys_seconds{midnight} + hours{*hour} + minutes{*minute} + seconds{*second} -
           offset;
}

// ---- candidates ------------------------------------------------------------------------------

/// One chapter that passed the language filter and the selection, with its derived keys.
struct Candidate {
    Chapter chapter{};
    LabelKey volume{};
    LabelKey number{}; ///< the chapter label
    std::optional<std::chrono::sys_seconds> published{};
    std::string id_key{};                ///< lowercased id
    std::vector<std::string> identity{}; ///< sorted, unique, lowercased group ids
};

[[nodiscard]] Candidate make_candidate(const Chapter& chapter) {
    Candidate candidate{
        .chapter = chapter,
        .volume = make_label_key(chapter.volume),
        .number = make_label_key(chapter.chapter),
        .published = parse_timestamp(chapter.publish_at),
        .id_key = ascii_lower_copy(chapter.id),
        .identity = {},
    };
    for (const ScanGroup& group : chapter.groups) {
        candidate.identity.push_back(ascii_lower_copy(group.id));
    }
    std::ranges::sort(candidate.identity);
    const auto duplicates = std::ranges::unique(candidate.identity);
    candidate.identity.erase(duplicates.begin(), duplicates.end());
    return candidate;
}

/// Reading order: volume, chapter, publishAt (unknown last), id.
[[nodiscard]] bool reading_order_less(const Candidate& lhs, const Candidate& rhs) {
    if (const auto by_volume = compare_labels(lhs.volume, rhs.volume); std::is_neq(by_volume)) {
        return std::is_lt(by_volume);
    }
    if (const auto by_number = compare_labels(lhs.number, rhs.number); std::is_neq(by_number)) {
        return std::is_lt(by_number);
    }
    if (lhs.published != rhs.published) {
        if (!lhs.published || !rhs.published) {
            return lhs.published.has_value();
        }
        return *lhs.published < *rhs.published;
    }
    return lhs.id_key < rhs.id_key;
}

/// A total order over everything the plan copies from a chapter, used only to pick one of two
/// feed entries that share an id (paging can return a chapter twice).
[[nodiscard]] bool content_less(const Chapter& lhs, const Chapter& rhs) {
    const auto group_less = [](const ScanGroup& a, const ScanGroup& b) {
        return std::tie(a.id, a.name) < std::tie(b.id, b.name);
    };
    if (lhs.groups != rhs.groups) {
        return std::ranges::lexicographical_compare(lhs.groups, rhs.groups, group_less);
    }
    return std::tie(lhs.updated_at, lhs.id, lhs.volume, lhs.chapter, lhs.title, lhs.language,
                    lhs.pages, lhs.external, lhs.unavailable, lhs.publish_at) <
           std::tie(rhs.updated_at, rhs.id, rhs.volume, rhs.chapter, rhs.title, rhs.language,
                    rhs.pages, rhs.external, rhs.unavailable, rhs.publish_at);
}

[[nodiscard]] bool is_selected(const Candidate& candidate, const PlanOptions& options) {
    // LabelKey::text is the label (normalized when numeric, which matches the same ranges).
    if (candidate.volume.rank != LabelRank::none) {
        return options.volumes.matches(candidate.volume.text);
    }
    if (candidate.number.rank != LabelRank::none) {
        return options.loose_chapters.matches(candidate.number.text);
    }
    return options.loose_chapters.is_all(); // a oneshot matches only `all`
}

[[nodiscard]] std::optional<SkipReason> skip_reason(const Chapter& chapter) noexcept {
    if (chapter.external) {
        return SkipReason::external;
    }
    if (chapter.pages <= 0) {
        return SkipReason::no_pages;
    }
    if (chapter.unavailable) {
        return SkipReason::unavailable;
    }
    return std::nullopt;
}

[[nodiscard]] SkippedChapter make_skip(const Chapter& chapter, SkipReason reason,
                                       std::optional<std::string> kept = std::nullopt) {
    return SkippedChapter{.id = chapter.id,
                          .volume = chapter.volume,
                          .chapter = chapter.chapter,
                          .reason = reason,
                          .kept = std::move(kept)};
}

// ---- dedupe ------------------------------------------------------------------------------------

/// (normalized volume, normalized chapter); only chapters with a chapter label have one.
struct DedupeKey {
    LabelRank volume_rank = LabelRank::none;
    std::string volume{};
    std::string chapter{};

    friend auto operator<=>(const DedupeKey&, const DedupeKey&) = default;
};

[[nodiscard]] DedupeKey dedupe_key(const Candidate& candidate) {
    return DedupeKey{.volume_rank = candidate.volume.rank,
                     .volume = candidate.volume.text,
                     .chapter = candidate.number.text};
}

/// The key as the skip warnings label a chapter: "Vol. 1 Ch. 1", or "Ch. 10" for a loose chapter.
/// The labels are the normalized ones the releases were grouped by.
[[nodiscard]] std::string describe_key(const DedupeKey& key) {
    if (key.volume_rank == LabelRank::none) {
        return std::format("Ch. {}", key.chapter);
    }
    return std::format("Vol. {} Ch. {}", key.volume, key.chapter);
}

/// Keeps the candidates of `pool` whose `score` is smallest.
template <class Score>
void keep_best(std::vector<std::size_t>& pool, const std::vector<Candidate>& candidates,
               Score score) {
    const auto best = std::ranges::min(
        pool, {}, [&candidates, &score](std::size_t index) { return score(candidates[index]); });
    const auto best_score = score(candidates[best]);
    std::erase_if(pool, [&candidates, &score, &best_score](std::size_t index) {
        return score(candidates[index]) != best_score;
    });
}

struct DedupeWinner {
    std::size_t index = 0;
    std::string rule{};
};

/// Applies the dedupe rules in order until one candidate remains. `pool` has at least two
/// entries with distinct ids.
[[nodiscard]] DedupeWinner
pick_release(std::vector<std::size_t> pool, const std::vector<Candidate>& candidates,
             const std::vector<std::string>& preferred,
             const std::map<std::vector<std::string>, std::size_t>& coverage) {
    const auto decided = [&pool](std::string rule) -> std::optional<DedupeWinner> {
        if (pool.size() == 1) {
            return DedupeWinner{.index = pool.front(), .rule = std::move(rule)};
        }
        return std::nullopt;
    };
    keep_best(pool, candidates, [&preferred](const Candidate& candidate) {
        return group_preference_rank(candidate.chapter.groups, preferred);
    });
    if (auto winner = decided("preferred group (--group)")) {
        return *std::move(winner);
    }
    keep_best(pool, candidates, [&coverage](const Candidate& candidate) {
        const auto found = coverage.find(candidate.identity);
        const std::size_t covered = found == coverage.end() ? 0 : found->second;
        return std::numeric_limits<std::size_t>::max() - covered; // most keys first
    });
    if (auto winner = decided("group covering the most chapters")) {
        return *std::move(winner);
    }
    keep_best(pool, candidates, [](const Candidate& candidate) {
        return -candidate.chapter.pages; // pages > 0 here, so the negation cannot overflow
    });
    if (auto winner = decided("more pages")) {
        return *std::move(winner);
    }
    keep_best(pool, candidates, [](const Candidate& candidate) {
        // An unparseable publishAt ranks after every parseable one.
        return std::pair{!candidate.published.has_value(),
                         candidate.published.value_or(std::chrono::sys_seconds{})};
    });
    if (auto winner = decided("earlier publishAt")) {
        return *std::move(winner);
    }
    keep_best(pool, candidates, [](const Candidate& candidate) { return candidate.id_key; });
    return DedupeWinner{.index = pool.front(), .rule = "smallest chapter id"};
}

// ---- covers ------------------------------------------------------------------------------------

/// The cover for a volume: locale == --lang, then the original language, then any other
/// locale, then a null locale; ties go to the smallest file name. A null cover volume never
/// matches.
[[nodiscard]] std::optional<Cover> cover_for_volume(const LabelKey& volume,
                                                    const std::vector<Cover>& covers,
                                                    std::string_view lang,
                                                    std::string_view original_language) {
    if (volume.rank == LabelRank::none) {
        return std::nullopt;
    }
    const auto locale_rank = [lang, original_language](const Cover& cover) {
        if (!cover.locale) {
            return 3;
        }
        if (*cover.locale == lang) {
            return 0;
        }
        return *cover.locale == original_language ? 1 : 2;
    };
    const Cover* best = nullptr;
    for (const Cover& cover : covers) {
        if (cover.file_name.empty() || make_label_key(cover.volume) != volume) {
            continue;
        }
        if (best == nullptr ||
            std::pair{locale_rank(cover), std::string_view{cover.file_name}} <
                std::pair{locale_rank(*best), std::string_view{best->file_name}}) {
            best = &cover;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<Cover>{*best};
}

// ---- per epub ----------------------------------------------------------------------------------

/// Percent-encodes everything but ASCII letters, digits and '.' (identifier units).
[[nodiscard]] std::string encode_unit(std::string_view label) {
    std::string out{};
    for (const char c : label) {
        const auto byte = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.') {
            out += c;
        } else {
            out += std::format("%{:02X}", byte);
        }
    }
    return out;
}

[[nodiscard]] std::string identifier_unit(const PlannedEpub& epub) {
    switch (epub.kind) {
    case EpubKind::volume:
        return "vol-" + encode_unit(epub.volume.value_or(std::string{}));
    case EpubKind::chapter:
        if (epub.volume) {
            return std::format("vol-{}-ch-{}", encode_unit(*epub.volume),
                               encode_unit(epub.chapter.value_or(std::string{})));
        }
        return "ch-" + encode_unit(epub.chapter.value_or(std::string{}));
    case EpubKind::oneshot:
        return "oneshot-" +
               (epub.chapters.empty() ? std::string{} : ascii_lower_copy(epub.chapters.front().id));
    case EpubKind::single:
        return "single";
    }
    return "unknown"; // only reachable through an out-of-range enum value
}

[[nodiscard]] std::string display_title(std::string_view title, const PlannedEpub& epub) {
    switch (epub.kind) {
    case EpubKind::volume:
        return std::format("{} Vol. {}", title, epub.volume.value_or(std::string{}));
    case EpubKind::chapter:
        if (epub.volume) {
            return std::format("{} Vol. {} Ch. {}", title, *epub.volume,
                               epub.chapter.value_or(std::string{}));
        }
        return std::format("{} Ch. {}", title, epub.chapter.value_or(std::string{}));
    case EpubKind::oneshot: {
        const std::string_view chapter_title =
            epub.chapters.empty() ? std::string_view{} : epub.chapters.front().title;
        return chapter_title.empty() ? std::format("{} Oneshot", title)
                                     : std::format("{} Oneshot: {}", title, chapter_title);
    }
    case EpubKind::single:
        return std::string{title};
    }
    return std::string{title}; // only reachable through an out-of-range enum value
}

[[nodiscard]] std::optional<std::string> series_position(const PlannedEpub& epub) {
    std::optional<std::string> label{};
    switch (epub.kind) {
    case EpubKind::volume:
        label = epub.volume;
        break;
    case EpubKind::chapter:
        label = epub.chapter;
        break;
    case EpubKind::oneshot:
    case EpubKind::single:
        break;
    }
    if (label && !ChapterNumber::parse(*label)) {
        return std::nullopt; // only numeric labels are positions
    }
    return label;
}

/// Why this file cannot be built, or nullopt.
[[nodiscard]] std::optional<std::string> plan_error(const PlannedEpub& epub) {
    for (const Chapter& chapter : epub.chapters) {
        if (std::cmp_greater(chapter.pages, max_pages_per_chapter)) {
            return std::format(
                "chapter {} has {} pages; at most {} pages per chapter are supported", chapter.id,
                chapter.pages, max_pages_per_chapter);
        }
    }
    const std::size_t fixed = fixed_epub_entries + (epub.cover ? cover_entries : 0);
    // fixed + 2 * pages > limit, without the multiplication overflowing.
    if (epub.planned_pages > (max_zip_entries_per_epub - fixed) / entries_per_page) {
        return std::format("{} pages need more than the {} entries an EPUB file can hold",
                           epub.planned_pages, max_zip_entries_per_epub);
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t count_pages(const std::vector<Chapter>& chapters) noexcept {
    std::size_t total = 0;
    for (const Chapter& chapter : chapters) {
        const auto pages = static_cast<std::size_t>(std::max(chapter.pages, 0));
        total = pages > std::numeric_limits<std::size_t>::max() - total
                    ? std::numeric_limits<std::size_t>::max()
                    : total + pages;
    }
    return total;
}

/// `name` with " (n)" before ".epub" for the smallest n >= 2 not yet in `used`.
[[nodiscard]] std::string unique_name(const std::string& name, const std::set<std::string>& used) {
    if (!used.contains(name)) {
        return name;
    }
    constexpr std::string_view extension = ".epub";
    const std::string_view base = std::string_view{name}.substr(0, name.size() - extension.size());
    for (std::size_t n = 2;; ++n) {
        std::string candidate = std::format("{} ({}){}", base, n, extension);
        if (!used.contains(candidate)) {
            return candidate;
        }
    }
}

struct Grouping {
    std::vector<PlannedEpub> epubs{};
    std::vector<LabelKey> cover_volumes{}; ///< per epub: the volume whose cover it gets
};

[[nodiscard]] PlannedEpub single_chapter_epub(const Candidate& candidate, bool with_volume) {
    const bool oneshot = candidate.number.rank == LabelRank::none;
    return PlannedEpub{
        .kind = oneshot ? EpubKind::oneshot : EpubKind::chapter,
        .volume = with_volume ? label_value(candidate.volume) : std::nullopt,
        .chapter = label_value(candidate.number),
        .chapters = {candidate.chapter},
    };
}

/// Splits the kept chapters (in reading order) into output files.
[[nodiscard]] Grouping group_chapters(const std::vector<const Candidate*>& kept, Layout layout) {
    Grouping grouping{};
    const auto add = [&grouping](PlannedEpub epub, LabelKey cover_volume) {
        grouping.epubs.push_back(std::move(epub));
        grouping.cover_volumes.push_back(std::move(cover_volume));
    };
    switch (layout) {
    case Layout::volumes:
        for (const Candidate* candidate : kept) {
            if (candidate->volume.rank == LabelRank::none) {
                add(single_chapter_epub(*candidate, false), LabelKey{});
            } else if (!grouping.epubs.empty() &&
                       grouping.cover_volumes.back() == candidate->volume) {
                grouping.epubs.back().chapters.push_back(candidate->chapter);
            } else {
                add(PlannedEpub{.kind = EpubKind::volume,
                                .volume = label_value(candidate->volume),
                                .chapters = {candidate->chapter}},
                    candidate->volume);
            }
        }
        break;
    case Layout::chapters:
        for (const Candidate* candidate : kept) {
            add(single_chapter_epub(*candidate, true), LabelKey{});
        }
        break;
    case Layout::single:
        if (!kept.empty()) {
            PlannedEpub epub{.kind = EpubKind::single};
            for (const Candidate* candidate : kept) {
                epub.chapters.push_back(candidate->chapter);
            }
            add(std::move(epub), LabelKey{});
        }
        break;
    }
    return grouping;
}

// ---- stages ------------------------------------------------------------------------------------

[[nodiscard]] Error malformed(std::string message) {
    return Error{.code = Errc::network, .message = std::move(message)};
}

/// The language filter and the selection: one candidate per chapter id, in reading order.
[[nodiscard]] Result<std::vector<Candidate>> select_candidates(const PlanInput& input,
                                                               const PlanOptions& options) {
    std::vector<Candidate> candidates{};
    for (const Chapter& chapter : input.chapters) {
        if (chapter.language != options.lang) {
            continue;
        }
        if (!is_uuid(chapter.id)) {
            return std::unexpected(malformed("the API returned a chapter id that is not a UUID"));
        }
        Candidate candidate = make_candidate(chapter);
        if (is_selected(candidate, options)) {
            candidates.push_back(std::move(candidate));
        }
    }
    std::ranges::sort(candidates, [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.id_key != rhs.id_key) {
            return lhs.id_key < rhs.id_key;
        }
        return content_less(rhs.chapter, lhs.chapter); // the greatest copy (latest update) first
    });
    const auto repeated = std::ranges::unique(candidates, {}, &Candidate::id_key);
    candidates.erase(repeated.begin(), repeated.end());
    std::ranges::sort(candidates, reading_order_less);
    return candidates;
}

/// What happens to each candidate. `skipped` keeps the candidate index, so the list can be put
/// in reading order at the end.
struct Triage {
    std::vector<bool> kept{};
    std::vector<std::pair<std::size_t, SkippedChapter>> skipped{};
    std::vector<DedupeNote> dedupe{};
};

[[nodiscard]] Triage apply_skips(const std::vector<Candidate>& candidates) {
    Triage triage{.kept = std::vector<bool>(candidates.size(), false)};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (const std::optional<SkipReason> reason = skip_reason(candidates[i].chapter)) {
            triage.skipped.emplace_back(i, make_skip(candidates[i].chapter, *reason));
        } else {
            triage.kept[i] = true;
        }
    }
    return triage;
}

/// Releases of the same volume+chapter among the kept candidates, in reading order of their
/// first release.
struct Releases {
    std::map<DedupeKey, std::vector<std::size_t>> by_key{};
    std::vector<DedupeKey> key_order{};
    std::map<std::vector<std::string>, std::size_t> coverage{}; ///< keys per group identity
};

[[nodiscard]] Releases collect_releases(const std::vector<Candidate>& candidates,
                                        const std::vector<bool>& kept) {
    Releases releases{};
    std::map<std::vector<std::string>, std::set<DedupeKey>> keys_by_identity{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!kept[i] || candidates[i].number.rank == LabelRank::none) {
            continue; // a null chapter is never deduped
        }
        DedupeKey key = dedupe_key(candidates[i]);
        keys_by_identity[candidates[i].identity].insert(key);
        auto [slot, inserted] = releases.by_key.try_emplace(key);
        if (inserted) {
            releases.key_order.push_back(std::move(key));
        }
        slot->second.push_back(i);
    }
    for (const auto& [identity, keys] : keys_by_identity) {
        releases.coverage.emplace(identity, keys.size());
    }
    return releases;
}

/// Keeps one release per key; the others become duplicate skips with a DedupeNote.
void dedupe_releases(const std::vector<Candidate>& candidates,
                     const std::vector<std::string>& preferred, Triage& triage) {
    const Releases releases = collect_releases(candidates, triage.kept);
    for (const DedupeKey& key : releases.key_order) {
        const std::vector<std::size_t>& pool = releases.by_key.at(key);
        if (pool.size() < 2) {
            continue;
        }
        DedupeWinner winner = pick_release(pool, candidates, preferred, releases.coverage);
        const std::string& kept_id = candidates[winner.index].chapter.id;
        DedupeNote note{
            .key = describe_key(key), .kept_id = kept_id, .rule = std::move(winner.rule)};
        for (const std::size_t loser : pool) {
            if (loser == winner.index) {
                continue;
            }
            triage.kept[loser] = false;
            triage.skipped.emplace_back(
                loser, make_skip(candidates[loser].chapter, SkipReason::duplicate, kept_id));
            note.dropped_ids.push_back(candidates[loser].chapter.id);
        }
        triage.dedupe.push_back(std::move(note));
    }
}

/// Layout single: the cover of the first volume (in reading order) that has one.
[[nodiscard]] std::optional<Cover> first_volume_cover(const std::vector<const Candidate*>& kept,
                                                      const PlanInput& input,
                                                      std::string_view lang) {
    for (const Candidate* candidate : kept) {
        std::optional<Cover> cover =
            cover_for_volume(candidate->volume, input.covers, lang, input.manga.original_language);
        if (cover) {
            return cover;
        }
    }
    return std::nullopt;
}

/// Groups the kept chapters and fills in everything derived per file. `stem` is the title's
/// file stem (Plan::file_stem): every file name starts with it.
[[nodiscard]] Result<std::vector<PlannedEpub>>
make_epubs(const std::vector<const Candidate*>& kept, const PlanInput& input,
           const PlanOptions& options, std::string_view manga_id, std::string_view title,
           std::string_view stem) {
    Grouping grouping = group_chapters(kept, options.layout);
    const std::optional<Cover> single_cover = options.layout == Layout::single
                                                  ? first_volume_cover(kept, input, options.lang)
                                                  : std::nullopt;
    std::set<std::string> used_names{};
    for (std::size_t i = 0; i < grouping.epubs.size(); ++i) {
        PlannedEpub& epub = grouping.epubs[i];
        epub.cover = options.layout == Layout::single
                         ? single_cover
                         : cover_for_volume(grouping.cover_volumes[i], input.covers, options.lang,
                                            input.manga.original_language);
        epub.display_title = display_title(title, epub);
        epub.identifier =
            std::format("urn:mangadex:{}:{}:{}", manga_id, options.lang, identifier_unit(epub));
        epub.series_position = series_position(epub);
        epub.planned_pages = count_pages(epub.chapters);
        epub.plan_error = plan_error(epub);
        epub.file_name = unique_name(epub_file_name(stem, epub), used_names);
        if (epub.file_name.size() > max_component_bytes) {
            return std::unexpected(
                Error{.code = Errc::failure,
                      .message = std::format("internal error: a planned file name is {} bytes long",
                                             epub.file_name.size())});
        }
        used_names.insert(epub.file_name);
    }
    return std::move(grouping.epubs);
}

} // namespace

std::size_t group_preference_rank(std::span<const ScanGroup> groups,
                                  std::span<const std::string> preferred) {
    for (std::size_t rank = 0; rank < preferred.size(); ++rank) {
        const std::string& wanted = preferred[rank];
        if (std::ranges::any_of(groups, [&wanted](const ScanGroup& group) {
                return ascii_iequal(group.name, wanted) || ascii_iequal(group.id, wanted);
            })) {
            return rank;
        }
    }
    return preferred.size();
}

std::string resolve_title(const MangaInfo& manga, std::string_view lang) {
    for (const auto& [language, text] : manga.titles) {
        if (language == "en" && !text.empty()) {
            return text;
        }
    }
    for (const auto& [language, text] : manga.alt_titles) {
        if (language == lang && !text.empty()) {
            return text;
        }
    }
    // The first title value: the lexicographically first language (the parser's title map is in
    // language-code order; sorting here keeps that true for any input order).
    const LocalizedText* first = nullptr;
    for (const LocalizedText& title : manga.titles) {
        if (!title.second.empty() && (first == nullptr || title.first < first->first)) {
            first = &title;
        }
    }
    return first == nullptr ? manga.id : first->second;
}

Direction resolve_direction(DirectionChoice choice, std::string_view original_language) noexcept {
    switch (choice) {
    case DirectionChoice::automatic:
        return original_language == "ja" || original_language == "ja-ro" ? Direction::rtl
                                                                         : Direction::ltr;
    case DirectionChoice::ltr:
        return Direction::ltr;
    case DirectionChoice::rtl:
        return Direction::rtl;
    }
    return Direction::ltr; // only reachable through an out-of-range enum value
}

Result<Plan> build_plan(const PlanInput& input, const PlanOptions& options) {
    if (!is_uuid(input.manga.id)) {
        return std::unexpected(malformed("the API returned a title id that is not a UUID"));
    }
    const std::string manga_id = ascii_lower_copy(input.manga.id);
    const std::string title = options.title_override && !options.title_override->empty()
                                  ? *options.title_override
                                  : resolve_title(input.manga, options.lang);
    Plan plan{
        .manga = MangaSummary{.id = manga_id,
                              .title = title,
                              .url = std::string{title_url_prefix} + manga_id,
                              .language = options.lang,
                              .direction = resolve_direction(options.direction,
                                                             input.manga.original_language)},
        .layout = options.layout,
        // One source of truth for the stem: make_epubs builds every file name from it, and
        // --dir-per-title names the title's directory after it.
        .file_stem = sanitize_file_stem(title, manga_id),
    };

    Result<std::vector<Candidate>> selected = select_candidates(input, options);
    if (!selected) {
        return std::unexpected(std::move(selected).error());
    }
    const std::vector<Candidate>& candidates = *selected;
    Triage triage = apply_skips(candidates);
    dedupe_releases(candidates, options.preferred_groups, triage);

    std::ranges::sort(triage.skipped, {}, [](const auto& entry) { return entry.first; });
    for (auto& entry : triage.skipped) {
        plan.skipped.push_back(std::move(entry.second));
    }
    plan.dedupe = std::move(triage.dedupe);

    std::vector<const Candidate*> kept{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (triage.kept[i]) {
            kept.push_back(&candidates[i]);
        }
    }
    Result<std::vector<PlannedEpub>> epubs =
        make_epubs(kept, input, options, manga_id, title, plan.file_stem);
    if (!epubs) {
        return std::unexpected(std::move(epubs).error());
    }
    plan.epubs = std::move(*epubs);
    return plan;
}

} // namespace mangadex_dl
