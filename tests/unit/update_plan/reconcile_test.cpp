// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/update_plan/reconcile_test.cpp: the report side of the update decision (skip-existing
// plan T7, acceptance 2 and 11's [unit] share): reconcile_skipped, the new and reused id lists and
// chapter_labels.
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/update_plan.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace md = mangadex_dl;

namespace {

using Label = std::optional<std::string>;

[[nodiscard]] md::ScanGroup group_a() {
    return md::ScanGroup{.id = "aaaaaaaa-0000-4000-8000-00000000000a", .name = "Group A"};
}
[[nodiscard]] md::ScanGroup group_b() {
    return md::ScanGroup{.id = "bbbbbbbb-0000-4000-8000-00000000000b", .name = "Group B"};
}

[[nodiscard]] std::string chapter_id(char series, int n) {
    return std::format("{}{}{}{}{}{}{}{}-0000-4000-8000-{:012}", series, series, series, series,
                       series, series, series, series, n);
}

[[nodiscard]] md::Chapter planned_chapter(std::string id, Label volume, Label chapter,
                                          md::ScanGroup group) {
    return md::Chapter{.id = std::move(id),
                       .volume = std::move(volume),
                       .chapter = std::move(chapter),
                       .title = {},
                       .language = "en",
                       .groups = {std::move(group)},
                       .pages = 2,
                       .external = false,
                       .unavailable = false,
                       .publish_at = "2024-02-01T00:00:00+00:00",
                       .updated_at = "2024-02-01T00:00:00+00:00"};
}

[[nodiscard]] md::ManifestChapter held_chapter(std::string id, Label volume, Label chapter,
                                               md::ScanGroup group) {
    return md::ManifestChapter{.id = std::move(id),
                               .volume = std::move(volume),
                               .chapter = std::move(chapter),
                               .title = {},
                               .groups = {std::move(group)},
                               .publish_at = "2020-01-01T00:00:00+00:00",
                               .updated_at = "2020-01-01T00:00:00+00:00",
                               .quality = md::ImageQuality::data,
                               .images = {"OEBPS/images/p00001.jpg"}};
}

[[nodiscard]] md::PlannedEpub epub_of(std::vector<md::Chapter> chapters) {
    return md::PlannedEpub{.kind = md::EpubKind::volume,
                           .file_name = "Title Vol. 01.epub",
                           .display_title = "Title Vol. 1",
                           .volume = "1",
                           .chapter = std::nullopt,
                           .chapters = std::move(chapters),
                           .cover = std::nullopt,
                           .identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:v1",
                           .series_position = "1",
                           .planned_pages = 0,
                           .plan_error = std::nullopt};
}

[[nodiscard]] md::BookManifest manifest_of(std::vector<md::ManifestChapter> chapters) {
    return md::BookManifest{.identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:v1",
                            .opf_crc32 = 0,
                            .cover = std::nullopt,
                            .chapters = std::move(chapters)};
}

[[nodiscard]] md::SkippedChapter duplicate_of(const md::Chapter& chapter, std::string kept) {
    return md::SkippedChapter{.id = chapter.id,
                              .volume = chapter.volume,
                              .chapter = chapter.chapter,
                              .reason = md::SkipReason::duplicate,
                              .kept = std::move(kept)};
}

[[nodiscard]] md::SkippedChapter external(std::string id) {
    return md::SkippedChapter{.id = std::move(id),
                              .volume = "1",
                              .chapter = "9",
                              .reason = md::SkipReason::external,
                              .kept = std::nullopt};
}

/// clean, then clean-regrouped-grown, Vol. 01: B's ch 1, 2 and 2.5 are planned, A's ch 1 and 2
/// are held.
struct RegroupedGrown {
    md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b()),
                 planned_chapter(chapter_id('b', 2), "1", "2", group_b()),
                 planned_chapter(chapter_id('b', 25), "1", "2.5", group_b())});
    md::BookManifest held = manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a()),
                                         held_chapter(chapter_id('a', 2), "1", "2", group_a())});
};

} // namespace

TEST_CASE("held releases absent from the plan's skips: the planned ones become duplicates",
          "[update_plan][reconcile]") {
    // MangaDex no longer lists A's ch 1-2 (criterion 11), so the plan skipped nothing.
    const RegroupedGrown fixture{};
    const md::UpdateDecision decision =
        md::decide_update(fixture.planned, fixture.held, {}, md::ImageQuality::data);
    REQUIRE(decision.state == md::UpdateState::incomplete);
    CHECK(md::new_chapter_ids(decision) == std::vector<std::string>{chapter_id('b', 25)});
    CHECK(md::reused_chapter_ids(decision) ==
          std::vector<std::string>{chapter_id('a', 1), chapter_id('a', 2)});

    const std::vector<md::SkippedChapter> skipped =
        md::reconcile_skipped({}, decision.kept, fixture.planned);
    CHECK(skipped == std::vector<md::SkippedChapter>{
                         duplicate_of(fixture.planned.chapters[0], chapter_id('a', 1)),
                         duplicate_of(fixture.planned.chapters[1], chapter_id('a', 2)),
                     });
}

TEST_CASE("a held release the plan skipped as a duplicate loses that entry",
          "[update_plan][reconcile]") {
    // Both releases are listed; the plan kept B's ch 1 over A's, the file keeps A's.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b())});
    const md::Chapter held_release = planned_chapter(chapter_id('a', 1), "1", "1", group_a());
    const std::vector<md::KeptRelease> kept{{.held_id = chapter_id('a', 1),
                                             .planned_id = chapter_id('b', 1),
                                             .key_label = "Vol. 1 Ch. 1"}};
    const std::vector<md::SkippedChapter> skipped = md::reconcile_skipped(
        {external(chapter_id('e', 1)), duplicate_of(held_release, chapter_id('b', 1)),
         external(chapter_id('e', 2))},
        kept, planned);
    CHECK(skipped == std::vector<md::SkippedChapter>{
                         external(chapter_id('e', 1)),
                         external(chapter_id('e', 2)),
                         duplicate_of(planned.chapters[0], chapter_id('a', 1)),
                     });
}

TEST_CASE("only the held release's duplicate entry is removed", "[update_plan][reconcile]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b())});
    const md::SkippedChapter same_id_other_reason = external(chapter_id('a', 1));
    const std::vector<md::KeptRelease> kept{
        {.held_id = chapter_id('a', 1), .planned_id = chapter_id('b', 1), .key_label = {}}};
    const std::vector<md::SkippedChapter> skipped =
        md::reconcile_skipped({same_id_other_reason}, kept, planned);
    REQUIRE(skipped.size() == 2);
    CHECK(skipped[0] == same_id_other_reason);
}

TEST_CASE("two substitutions are appended in final-list order after the plan's skips",
          "[update_plan][reconcile]") {
    const RegroupedGrown fixture{};
    const md::UpdateDecision decision =
        md::decide_update(fixture.planned, fixture.held, {}, md::ImageQuality::data);
    REQUIRE(decision.kept.size() == 2);
    const md::Chapter a2 = planned_chapter(chapter_id('a', 2), "1", "2", group_a());
    const std::vector<md::SkippedChapter> skipped =
        md::reconcile_skipped({duplicate_of(a2, chapter_id('b', 2)), external(chapter_id('e', 1))},
                              decision.kept, fixture.planned);
    CHECK(skipped == std::vector<md::SkippedChapter>{
                         external(chapter_id('e', 1)),
                         duplicate_of(fixture.planned.chapters[0], chapter_id('a', 1)),
                         duplicate_of(fixture.planned.chapters[1], chapter_id('a', 2)),
                     });
}

TEST_CASE("a third release that lost to the planned one now names the held one as kept",
          "[update_plan][reconcile]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b())});
    const md::Chapter third = planned_chapter(chapter_id('c', 1), "1", "1", group_b());
    const std::vector<md::KeptRelease> kept{
        {.held_id = chapter_id('a', 1), .planned_id = chapter_id('b', 1), .key_label = {}}};
    const std::vector<md::SkippedChapter> skipped =
        md::reconcile_skipped({duplicate_of(third, chapter_id('b', 1))}, kept, planned);
    CHECK(skipped == std::vector<md::SkippedChapter>{
                         duplicate_of(third, chapter_id('a', 1)),
                         duplicate_of(planned.chapters[0], chapter_id('a', 1)),
                     });
}

TEST_CASE("no substitution leaves the skips unchanged", "[update_plan][reconcile]") {
    const std::vector<md::SkippedChapter> plan_skips{external(chapter_id('e', 1))};
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a())});
    CHECK(md::reconcile_skipped(plan_skips, {}, planned) == plan_skips);
}

TEST_CASE("a substitution for a release missing from the plan keeps the id without labels",
          "[update_plan][reconcile]") {
    const std::vector<md::KeptRelease> kept{
        {.held_id = chapter_id('a', 1), .planned_id = chapter_id('b', 1), .key_label = {}}};
    const std::vector<md::SkippedChapter> skipped = md::reconcile_skipped({}, kept, epub_of({}));
    CHECK(skipped == std::vector<md::SkippedChapter>{{.id = chapter_id('b', 1),
                                                      .volume = std::nullopt,
                                                      .chapter = std::nullopt,
                                                      .reason = md::SkipReason::duplicate,
                                                      .kept = chapter_id('a', 1)}});
}

TEST_CASE("a fallback removes its substitution from the reconciled skips",
          "[update_plan][reconcile]") {
    // A's ch 1 is damaged: B's ch 1 is downloaded after all, so it is no longer a duplicate.
    const RegroupedGrown fixture{};
    const md::UpdateDecision decision = md::fall_back_to_download(
        md::decide_update(fixture.planned, fixture.held, {}, md::ImageQuality::data), 0,
        fixture.planned, md::ImageQuality::data);
    CHECK(md::new_chapter_ids(decision) ==
          std::vector<std::string>{chapter_id('b', 1), chapter_id('b', 25)});
    CHECK(md::reused_chapter_ids(decision) == std::vector<std::string>{chapter_id('a', 2)});
    const std::vector<md::SkippedChapter> skipped =
        md::reconcile_skipped({}, decision.kept, fixture.planned);
    CHECK(skipped == std::vector<md::SkippedChapter>{
                         duplicate_of(fixture.planned.chapters[1], chapter_id('a', 2)),
                     });
}

TEST_CASE("a re-picked release needs no substitution", "[update_plan][reconcile]") {
    const RegroupedGrown fixture{};
    const std::vector<std::string> preferred{"Group B"};
    const md::UpdateDecision decision =
        md::decide_update(fixture.planned, fixture.held, preferred, md::ImageQuality::data);
    CHECK(decision.kept.empty());
    CHECK(md::reconcile_skipped({}, decision.kept, fixture.planned).empty());
    CHECK(md::new_chapter_ids(decision) ==
          std::vector<std::string>{chapter_id('b', 1), chapter_id('b', 2), chapter_id('b', 25)});
}

TEST_CASE("chapter_labels shows at most max_shown labels, then the rest as a count",
          "[update_plan][labels]") {
    std::vector<md::Chapter> chapters{};
    for (int n = 1; n <= 7; ++n) {
        chapters.push_back(
            planned_chapter(chapter_id('a', n), "1", std::format("{:02}", n), group_a()));
    }
    const std::span<const md::Chapter> all{chapters};

    CHECK(md::chapter_labels({}).empty());
    CHECK(md::chapter_labels(all.first(1)) == "Ch. 1");
    CHECK(md::chapter_labels(all.first(5)) == "Ch. 1, Ch. 2, Ch. 3, Ch. 4, Ch. 5");
    CHECK(md::chapter_labels(all.first(6)) == "Ch. 1, Ch. 2, Ch. 3, Ch. 4, Ch. 5 and 1 more");
    CHECK(md::chapter_labels(all) == "Ch. 1, Ch. 2, Ch. 3, Ch. 4, Ch. 5 and 2 more");
    CHECK(md::chapter_labels(all, 2) == "Ch. 1, Ch. 2 and 5 more");
    CHECK(md::chapter_labels(all, 0) == "and 7 more");
    CHECK(md::chapter_labels(all, 100).ends_with("Ch. 7"));
}

TEST_CASE("chapter_labels names chapters without a chapter label", "[update_plan][labels]") {
    const std::vector<md::Chapter> chapters{
        planned_chapter(chapter_id('a', 1), std::nullopt, std::nullopt, group_a()),
        planned_chapter(chapter_id('a', 2), "1", "", group_a()),
        planned_chapter(chapter_id('a', 3), "1", "Extra", group_a()),
        planned_chapter(chapter_id('a', 4), "1", "10.50", group_a()),
    };
    CHECK(md::chapter_labels(chapters) == "Oneshot, Oneshot, Ch. Extra, Ch. 10.5");
}
