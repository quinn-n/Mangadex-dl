// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update decision (skip-existing plan T7): matches the chapters a planned file should hold
// against the chapters an existing file holds, classifies the file, and builds the final chapter
// list and the report lists. Pure: no I/O, no clocks. Every function works on indices into its
// inputs and returns new values, so no iterator or reference into a caller's container escapes.
#include "mangadex_dl/update_plan.hpp"

#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/planner.hpp"
#include "mangadex_dl/selection.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

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

// ---- number identity ---------------------------------------------------------------------------

/// The number identity of a chapter: (normalized volume or none, normalized chapter). An empty
/// label counts as none, as in the planner's dedupe.
struct NumberKey {
    std::optional<std::string> volume{}; ///< nullopt for a loose chapter
    std::string chapter{};               ///< never empty

    friend auto operator<=>(const NumberKey&, const NumberKey&) = default;
};

/// nullopt for a chapter without a chapter label (a oneshot): those are matched by id only.
[[nodiscard]] std::optional<NumberKey> number_key(const std::optional<std::string>& volume,
                                                  const std::optional<std::string>& chapter) {
    if (!chapter || chapter->empty()) {
        return std::nullopt;
    }
    std::optional<std::string> volume_key{};
    if (volume && !volume->empty()) {
        volume_key = normalize_label(*volume);
    }
    return NumberKey{.volume = std::move(volume_key), .chapter = normalize_label(*chapter)};
}

/// "Vol. 1 Ch. 1", or "Ch. 10" for a loose chapter (the planner's skip-warning format).
[[nodiscard]] std::string describe_key(const NumberKey& key) {
    if (!key.volume) {
        return std::format("Ch. {}", key.chapter);
    }
    return std::format("Vol. {} Ch. {}", *key.volume, key.chapter);
}

// ---- matching ----------------------------------------------------------------------------------

enum class MatchRule {
    id,     ///< rule 1: the same id, ASCII case-insensitive
    number, ///< rule 2: the same number key
    loose,  ///< rule 3: a loose held chapter that the plan now puts in a volume
};

struct Match {
    std::optional<std::size_t> held{}; ///< index into BookManifest::chapters
    MatchRule rule = MatchRule::id;
};

/// Held chapter indices that share one lookup key, in held order. `next` skips the ones already
/// consumed, so each bucket is walked once overall.
struct Bucket {
    std::vector<std::size_t> held{};
    std::size_t next = 0;
};

/// The first held chapter of `bucket` not consumed yet, which is then consumed.
[[nodiscard]] std::optional<std::size_t> take(Bucket& bucket, std::vector<bool>& consumed) {
    while (bucket.next < bucket.held.size() && consumed[bucket.held[bucket.next]]) {
        ++bucket.next;
    }
    if (bucket.next == bucket.held.size()) {
        return std::nullopt;
    }
    const std::size_t index = bucket.held[bucket.next];
    consumed[index] = true;
    ++bucket.next;
    return index;
}

/// Runs the three rules as three passes in plan order, so an id match always wins over a number
/// match and an exact number match over a loose-into-volume one. Each held chapter is consumed at
/// most once; held order breaks ties. Lookups go through ordered maps, so a large (hostile)
/// manifest costs O((planned + held) log held), never planned x held.
[[nodiscard]] std::vector<Match> match_chapters(const PlannedEpub& planned,
                                                const BookManifest& held) {
    const std::vector<Chapter>& wanted = planned.chapters;
    std::map<std::string, Bucket> by_id{};
    std::map<NumberKey, Bucket> by_number{};
    std::map<std::string, Bucket> loose_by_chapter{};
    for (std::size_t index = 0; index < held.chapters.size(); ++index) {
        const ManifestChapter& chapter = held.chapters[index];
        by_id[ascii_lower_copy(chapter.id)].held.push_back(index);
        std::optional<NumberKey> key = number_key(chapter.volume, chapter.chapter);
        if (!key) {
            continue;
        }
        if (!key->volume) {
            loose_by_chapter[key->chapter].held.push_back(index);
        }
        by_number[std::move(*key)].held.push_back(index);
    }

    std::vector<Match> matches(wanted.size());
    std::vector<bool> consumed(held.chapters.size(), false);
    std::vector<std::optional<NumberKey>> keys{};
    keys.reserve(wanted.size());
    for (std::size_t index = 0; index < wanted.size(); ++index) {
        keys.push_back(number_key(wanted[index].volume, wanted[index].chapter));
        if (const auto bucket = by_id.find(ascii_lower_copy(wanted[index].id));
            bucket != by_id.end()) {
            matches[index] = Match{.held = take(bucket->second, consumed), .rule = MatchRule::id};
        }
    }
    for (std::size_t index = 0; index < wanted.size(); ++index) {
        const std::optional<NumberKey>& key = keys[index];
        if (matches[index].held || !key) {
            continue;
        }
        if (const auto bucket = by_number.find(*key); bucket != by_number.end()) {
            matches[index] =
                Match{.held = take(bucket->second, consumed), .rule = MatchRule::number};
        }
    }
    for (std::size_t index = 0; index < wanted.size(); ++index) {
        const std::optional<NumberKey>& key = keys[index];
        if (matches[index].held || !key || !key->volume) {
            continue;
        }
        if (const auto bucket = loose_by_chapter.find(key->chapter);
            bucket != loose_by_chapter.end()) {
            matches[index] =
                Match{.held = take(bucket->second, consumed), .rule = MatchRule::loose};
        }
    }
    return matches;
}

// ---- final-list entries ------------------------------------------------------------------------

/// The held image count as Chapter::pages. parse_manifest caps it at max_pages_per_chapter; the
/// check keeps the conversion defined for any input.
[[nodiscard]] int held_pages(const ManifestChapter& held) noexcept {
    return std::in_range<int>(held.images.size()) ? static_cast<int>(held.images.size())
                                                  : std::numeric_limits<int>::max();
}

/// Matched by id: the planned chapter (fresh metadata), with the page count of the held pages
/// that will be copied, so final_epub's page total counts what the file will hold.
[[nodiscard]] Chapter reused_planned(const Chapter& planned, const ManifestChapter& held) {
    Chapter chapter = planned;
    chapter.pages = held_pages(held);
    return chapter;
}

/// Matched by number and kept: the held release (id, title, groups, timestamps and page count from
/// the manifest) under the plan's labels. The manifest records no language; every chapter of one
/// file has the run's language, so the planned chapter's is used.
[[nodiscard]] Chapter kept_release(const Chapter& planned, const ManifestChapter& held) {
    return Chapter{
        .id = held.id,
        .volume = planned.volume,
        .chapter = planned.chapter,
        .title = held.title,
        .language = planned.language,
        .groups = held.groups,
        .pages = held_pages(held),
        .external = false,
        .unavailable = false,
        .publish_at = held.publish_at,
        .updated_at = held.updated_at,
    };
}

/// The group names of a release, for the -v re-pick line.
[[nodiscard]] std::string group_names(std::span<const ScanGroup> groups) {
    std::string out{};
    for (const ScanGroup& group : groups) {
        if (!out.empty()) {
            out += ", ";
        }
        out += group.name;
    }
    return out;
}

[[nodiscard]] UpdateState classify(const UpdateDecision& decision) noexcept {
    if (decision.missing.empty()) {
        return UpdateState::complete;
    }
    return decision.extra.empty() ? UpdateState::incomplete : UpdateState::diverged;
}

[[nodiscard]] std::vector<std::string> ids_with_source(const UpdateDecision& decision,
                                                       ChapterSource source) {
    std::vector<std::string> ids{};
    for (const FinalChapter& entry : decision.chapters) {
        if (entry.source == source) {
            ids.push_back(entry.chapter.id);
        }
    }
    return ids;
}

/// "Ch. 4" with the normalized label, or "Oneshot" for a chapter without a chapter label.
[[nodiscard]] std::string chapter_label(const Chapter& chapter) {
    if (!chapter.chapter || chapter.chapter->empty()) {
        return "Oneshot";
    }
    return std::format("Ch. {}", normalize_label(*chapter.chapter));
}

} // namespace

UpdateDecision decide_update(const PlannedEpub& planned, const BookManifest& held,
                             std::span<const std::string> preferred_groups, ImageQuality quality) {
    const std::vector<Match> matches = match_chapters(planned, held);
    UpdateDecision decision{};
    decision.chapters.reserve(planned.chapters.size());
    std::vector<bool> matched(held.chapters.size(), false);
    for (std::size_t index = 0; index < planned.chapters.size(); ++index) {
        const Chapter& wanted = planned.chapters[index];
        const Match& match = matches[index];
        if (!match.held) {
            decision.missing.push_back(index);
            decision.chapters.push_back(FinalChapter{.chapter = wanted,
                                                     .source = ChapterSource::download,
                                                     .quality = quality,
                                                     .held = std::nullopt,
                                                     .planned = index});
            continue;
        }
        const std::size_t held_index = *match.held;
        matched[held_index] = true;
        const ManifestChapter& have = held.chapters[held_index];
        if (match.rule == MatchRule::id) {
            decision.chapters.push_back(FinalChapter{.chapter = reused_planned(wanted, have),
                                                     .source = ChapterSource::reuse,
                                                     .quality = have.quality,
                                                     .held = held_index,
                                                     .planned = index});
            continue;
        }
        // Rules 2 and 3 only match planned chapters with a chapter label, so the key exists.
        const std::optional<NumberKey> key = number_key(wanted.volume, wanted.chapter);
        std::string key_label = key ? describe_key(*key) : std::string{};
        const bool repick =
            !preferred_groups.empty() && group_preference_rank(wanted.groups, preferred_groups) <
                                             group_preference_rank(have.groups, preferred_groups);
        if (repick) {
            // Decision 5: the planned release ranks strictly better under --group. The held one
            // is consumed by the match, so it is neither reused nor extra.
            decision.missing.push_back(index);
            decision.repicked.push_back(Repick{.held_id = have.id,
                                               .planned_id = wanted.id,
                                               .held_group = group_names(have.groups),
                                               .planned_group = group_names(wanted.groups),
                                               .key_label = std::move(key_label)});
            decision.chapters.push_back(FinalChapter{.chapter = wanted,
                                                     .source = ChapterSource::download,
                                                     .quality = quality,
                                                     .held = held_index,
                                                     .planned = index});
            continue;
        }
        decision.kept.push_back(KeptRelease{
            .held_id = have.id, .planned_id = wanted.id, .key_label = std::move(key_label)});
        decision.chapters.push_back(FinalChapter{.chapter = kept_release(wanted, have),
                                                 .source = ChapterSource::reuse,
                                                 .quality = have.quality,
                                                 .held = held_index,
                                                 .planned = index});
    }
    for (std::size_t index = 0; index < matched.size(); ++index) {
        if (!matched[index]) {
            decision.extra.push_back(index);
        }
    }
    decision.state = classify(decision);
    return decision;
}

UpdateDecision fall_back_to_download(UpdateDecision decision, std::size_t final_index,
                                     const PlannedEpub& planned, ImageQuality quality) {
    if (final_index >= decision.chapters.size()) {
        return decision;
    }
    FinalChapter& entry = decision.chapters[final_index];
    if (entry.source == ChapterSource::download || entry.planned >= planned.chapters.size()) {
        return decision;
    }
    const Chapter& wanted = planned.chapters[entry.planned];
    const std::string held_id = entry.chapter.id;
    std::erase_if(decision.kept, [&held_id, &wanted](const KeptRelease& kept) {
        return kept.held_id == held_id && kept.planned_id == wanted.id;
    });
    entry.chapter = wanted;
    entry.source = ChapterSource::download;
    entry.quality = quality;
    // `held` stays: the held chapter is still the one this entry matched, only its pages are not
    // copied. `missing` stays sorted (plan order) and holds each planned index once.
    const auto slot = std::ranges::lower_bound(decision.missing, entry.planned);
    if (slot == decision.missing.end() || *slot != entry.planned) {
        decision.missing.insert(slot, entry.planned);
    }
    decision.state = classify(decision);
    return decision;
}

PlannedEpub final_epub(const PlannedEpub& planned, const UpdateDecision& decision) {
    PlannedEpub out = planned;
    out.chapters.clear();
    out.chapters.reserve(decision.chapters.size());
    std::size_t pages = 0;
    for (const FinalChapter& entry : decision.chapters) {
        out.chapters.push_back(entry.chapter);
        const auto count = static_cast<std::size_t>(std::max(entry.chapter.pages, 0));
        pages = count > std::numeric_limits<std::size_t>::max() - pages
                    ? std::numeric_limits<std::size_t>::max()
                    : pages + count;
    }
    out.planned_pages = pages;
    return out;
}

std::vector<std::string> new_chapter_ids(const UpdateDecision& decision) {
    return ids_with_source(decision, ChapterSource::download);
}

std::vector<std::string> reused_chapter_ids(const UpdateDecision& decision) {
    return ids_with_source(decision, ChapterSource::reuse);
}

std::vector<SkippedChapter> reconcile_skipped(std::vector<SkippedChapter> skipped,
                                              std::span<const KeptRelease> kept,
                                              const PlannedEpub& planned) {
    for (const KeptRelease& substitution : kept) {
        // The held release is in the file again, so it is no longer a skipped duplicate.
        std::erase_if(skipped, [&substitution](const SkippedChapter& entry) {
            return entry.reason == SkipReason::duplicate &&
                   ascii_iequal(entry.id, substitution.held_id);
        });
        // A third release that lost the dedupe to the planned one now lost it to the held one:
        // `kept` must name a release the file holds.
        for (SkippedChapter& entry : skipped) {
            if (entry.reason == SkipReason::duplicate && entry.kept &&
                ascii_iequal(*entry.kept, substitution.planned_id)) {
                entry.kept = substitution.held_id;
            }
        }
    }
    for (const KeptRelease& substitution : kept) {
        const auto release =
            std::ranges::find_if(planned.chapters, [&substitution](const Chapter& chapter) {
                return ascii_iequal(chapter.id, substitution.planned_id);
            });
        const bool found = release != planned.chapters.end();
        skipped.push_back(SkippedChapter{
            .id = substitution.planned_id,
            .volume = found ? release->volume : std::nullopt,
            .chapter = found ? release->chapter : std::nullopt,
            .reason = SkipReason::duplicate,
            .kept = substitution.held_id,
        });
    }
    return skipped;
}

std::string chapter_labels(std::span<const Chapter> chapters, std::size_t max_shown) {
    std::string out{};
    const std::size_t shown = std::min(chapters.size(), max_shown);
    for (const Chapter& chapter : chapters.first(shown)) {
        if (!out.empty()) {
            out += ", ";
        }
        out += chapter_label(chapter);
    }
    if (const std::size_t hidden = chapters.size() - shown; hidden > 0) {
        if (!out.empty()) {
            out += ' ';
        }
        out += std::format("and {} more", hidden);
    }
    return out;
}

} // namespace mangadex_dl
