// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/update_plan/update_plan_test.cpp: the update decision (skip-existing plan T7,
// acceptance 1 and 2 and decision 5's [unit] row): matching, classification, the --group re-pick,
// the final list, final_epub and fall_back_to_download. Inputs are small hand-built plans and
// manifests, each isolating one deciding factor.
#include "mangadex_dl/book_manifest.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/update_plan.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
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
[[nodiscard]] md::ScanGroup group_c() {
    return md::ScanGroup{.id = "cccccccc-0000-4000-8000-00000000000c", .name = "Group C"};
}

/// A UUID-shaped chapter id; `series` keeps the releases of different groups apart.
[[nodiscard]] std::string chapter_id(char series, int n) {
    return std::format("{}{}{}{}{}{}{}{}-0000-4000-8000-{:012}", series, series, series, series,
                       series, series, series, series, n);
}

[[nodiscard]] md::Chapter planned_chapter(std::string id, Label volume, Label chapter,
                                          md::ScanGroup group, int pages = 3) {
    return md::Chapter{
        .id = std::move(id),
        .volume = std::move(volume),
        .chapter = std::move(chapter),
        .title = "Fresh title",
        .language = "en",
        .groups = {std::move(group)},
        .pages = pages,
        .external = false,
        .unavailable = false,
        .publish_at = "2024-02-01T00:00:00+00:00",
        .updated_at = "2024-02-02T00:00:00+00:00",
    };
}

[[nodiscard]] md::ManifestChapter held_chapter(std::string id, Label volume, Label chapter,
                                               md::ScanGroup group, std::size_t images = 2,
                                               md::ImageQuality quality = md::ImageQuality::data) {
    md::ManifestChapter record{
        .id = std::move(id),
        .volume = std::move(volume),
        .chapter = std::move(chapter),
        .title = "Held title",
        .groups = {std::move(group)},
        .publish_at = "2020-01-01T00:00:00+00:00",
        .updated_at = "2020-01-02T00:00:00+00:00",
        .quality = quality,
        .images = {},
    };
    for (std::size_t page = 0; page < images; ++page) {
        record.images.push_back(std::format("OEBPS/images/p{:05}.jpg", page + 1));
    }
    return record;
}

[[nodiscard]] md::PlannedEpub epub_of(std::vector<md::Chapter> chapters) {
    md::PlannedEpub epub{
        .kind = md::EpubKind::volume,
        .file_name = "Title Vol. 01.epub",
        .display_title = "Title Vol. 1",
        .volume = "1",
        .chapter = std::nullopt,
        .chapters = std::move(chapters),
        .cover = md::Cover{.volume = "1", .file_name = "cover.jpg", .locale = "ja"},
        .identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:v1",
        .series_position = "1",
        .planned_pages = 0,
        .plan_error = std::nullopt,
    };
    for (const md::Chapter& chapter : epub.chapters) {
        epub.planned_pages += static_cast<std::size_t>(chapter.pages);
    }
    return epub;
}

[[nodiscard]] md::BookManifest manifest_of(std::vector<md::ManifestChapter> chapters) {
    return md::BookManifest{
        .identifier = "urn:mangadex:11111111-1111-4111-8111-111111111111:en:v1",
        .opf_crc32 = 0x12345678U,
        .cover = md::ManifestCover{.file_name = "cover.jpg", .entry = "OEBPS/images/cover.jpg"},
        .chapters = std::move(chapters)};
}

[[nodiscard]] md::UpdateDecision decide(const md::PlannedEpub& planned,
                                        const md::BookManifest& held,
                                        const std::vector<std::string>& preferred = {},
                                        md::ImageQuality quality = md::ImageQuality::data) {
    return md::decide_update(planned, held, preferred, quality);
}

[[nodiscard]] std::vector<std::size_t> indices(std::initializer_list<std::size_t> values) {
    return std::vector<std::size_t>{values};
}

/// Vol. 1: ch 1 and 2 by group A; the "clean" fixture's first volume.
[[nodiscard]] md::BookManifest held_volume_one() {
    return manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a(), 2),
                        held_chapter(chapter_id('a', 2), "1", "2", group_a(), 3)});
}

} // namespace

// ---- matching table ----------------------------------------------------------------------------

TEST_CASE("same ids: complete, every chapter reused with the plan's metadata", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a(), 7),
                 planned_chapter(chapter_id('a', 2), "1", "2", group_a(), 8)});
    md::BookManifest held = held_volume_one();
    held.chapters[1].quality = md::ImageQuality::data_saver;
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.missing.empty());
    CHECK(decision.extra.empty());
    CHECK(decision.kept.empty());
    CHECK(decision.repicked.empty());
    REQUIRE(decision.chapters.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const md::FinalChapter& entry = decision.chapters[i];
        md::Chapter expected = planned.chapters[i];
        expected.pages = static_cast<int>(held.chapters[i].images.size()); // what the file holds
        CHECK(entry.chapter == expected);
        CHECK(entry.source == md::ChapterSource::reuse);
        CHECK(entry.held == i);
        CHECK(entry.planned == i);
    }
    CHECK(decision.chapters[0].quality == md::ImageQuality::data);
    CHECK(decision.chapters[1].quality == md::ImageQuality::data_saver);
}

TEST_CASE("same numbers with different ids: complete, held release kept under the plan's labels",
          "[update_plan]") {
    // Group B re-released ch 1 and 2 under new ids; the plan spells the labels differently.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "01", "1.0", group_b()),
                 planned_chapter(chapter_id('b', 2), "1", "02", group_b())});
    const md::BookManifest held = held_volume_one();
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.missing.empty());
    CHECK(decision.extra.empty());
    CHECK(decision.repicked.empty());
    REQUIRE(decision.chapters.size() == 2);
    const md::FinalChapter& first = decision.chapters[0];
    CHECK(first.source == md::ChapterSource::reuse);
    CHECK(first.held == 0U);
    CHECK(first.planned == 0U);
    CHECK(first.chapter == md::Chapter{
                               .id = chapter_id('a', 1),
                               .volume = "01",
                               .chapter = "1.0",
                               .title = "Held title",
                               .language = "en",
                               .groups = {group_a()},
                               .pages = 2,
                               .external = false,
                               .unavailable = false,
                               .publish_at = "2020-01-01T00:00:00+00:00",
                               .updated_at = "2020-01-02T00:00:00+00:00",
                           });
    CHECK(decision.chapters[1].chapter.id == chapter_id('a', 2));
    CHECK(decision.chapters[1].chapter.chapter == "02");
    CHECK(decision.chapters[1].chapter.pages == 3);
    CHECK(decision.kept == std::vector<md::KeptRelease>{
                               {.held_id = chapter_id('a', 1),
                                .planned_id = chapter_id('b', 1),
                                .key_label = "Vol. 1 Ch. 1"},
                               {.held_id = chapter_id('a', 2),
                                .planned_id = chapter_id('b', 2),
                                .key_label = "Vol. 1 Ch. 2"},
                           });
}

TEST_CASE("an added chapter: incomplete, with exact lists", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a()),
                 planned_chapter(chapter_id('a', 2), "1", "2", group_a()),
                 planned_chapter(chapter_id('a', 3), "1", "3", group_a(), 5)});
    const md::UpdateDecision decision =
        decide(planned, held_volume_one(), {}, md::ImageQuality::data_saver);

    CHECK(decision.state == md::UpdateState::incomplete);
    CHECK(decision.missing == indices({2}));
    CHECK(decision.extra.empty());
    REQUIRE(decision.chapters.size() == 3);
    const md::FinalChapter& added = decision.chapters[2];
    CHECK(added == md::FinalChapter{.chapter = planned.chapters[2],
                                    .source = md::ChapterSource::download,
                                    .quality = md::ImageQuality::data_saver,
                                    .held = std::nullopt,
                                    .planned = 2});
    CHECK(md::new_chapter_ids(decision) == std::vector<std::string>{chapter_id('a', 3)});
    CHECK(md::reused_chapter_ids(decision) ==
          std::vector<std::string>{chapter_id('a', 1), chapter_id('a', 2)});
}

TEST_CASE("extras only: complete", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a()),
                 planned_chapter(chapter_id('a', 3), "1", "3", group_a())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a()),
                     held_chapter(chapter_id('a', 2), "1", "2", group_a()),
                     held_chapter(chapter_id('a', 3), "1", "3", group_a())});
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.missing.empty());
    CHECK(decision.extra == indices({1}));
    REQUIRE(decision.chapters.size() == 2);
    CHECK(decision.chapters[1].held == 2U);
}

TEST_CASE("extras and missing: diverged", "[update_plan]") {
    // clean-diverged: ch 3 is gone from MangaDex, ch 4 is new.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 4), "2", "4", group_a())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 3), "2", "3", group_a())});
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::diverged);
    CHECK(decision.missing == indices({0}));
    CHECK(decision.extra == indices({0}));
}

TEST_CASE("a loose chapter that the plan moved into a volume is matched", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 5), "2", "5", group_b())});

    SECTION("by number, keeping the held release under the plan's volume") {
        const md::BookManifest held =
            manifest_of({held_chapter(chapter_id('a', 5), std::nullopt, "05", group_a())});
        const md::UpdateDecision decision = decide(planned, held);
        CHECK(decision.state == md::UpdateState::complete);
        REQUIRE(decision.chapters.size() == 1);
        CHECK(decision.chapters[0].chapter.id == chapter_id('a', 5));
        CHECK(decision.chapters[0].chapter.volume == "2");
        CHECK(decision.chapters[0].chapter.chapter == "5");
        CHECK(decision.kept == std::vector<md::KeptRelease>{{.held_id = chapter_id('a', 5),
                                                             .planned_id = chapter_id('b', 5),
                                                             .key_label = "Vol. 2 Ch. 5"}});
    }
    SECTION("an empty held volume label counts as loose") {
        const md::BookManifest held =
            manifest_of({held_chapter(chapter_id('a', 5), "", "5", group_a())});
        CHECK(decide(planned, held).state == md::UpdateState::complete);
    }
    SECTION("a held chapter in another volume is not the same chapter") {
        const md::BookManifest held =
            manifest_of({held_chapter(chapter_id('a', 5), "1", "5", group_a())});
        const md::UpdateDecision decision = decide(planned, held);
        CHECK(decision.state == md::UpdateState::diverged);
        CHECK(decision.missing == indices({0}));
        CHECK(decision.extra == indices({0}));
    }
    SECTION("a planned loose chapter never takes a held one from a volume") {
        const md::PlannedEpub loose =
            epub_of({planned_chapter(chapter_id('b', 5), std::nullopt, "5", group_b())});
        const md::BookManifest held =
            manifest_of({held_chapter(chapter_id('a', 5), "2", "5", group_a())});
        CHECK(decide(loose, held).state == md::UpdateState::diverged);
    }
}

TEST_CASE("an exact number match wins over a loose-into-volume match", "[update_plan]") {
    // Layout single: loose ch 5 and Vol. 2 Ch. 5 are different chapters; the volume comes first.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 25), "2", "5", group_b()),
                 planned_chapter(chapter_id('b', 5), std::nullopt, "5", group_b())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 5), std::nullopt, "5", group_a())});
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::incomplete);
    CHECK(decision.missing == indices({0}));
    REQUIRE(decision.chapters.size() == 2);
    CHECK(decision.chapters[1].held == 0U);
    CHECK(decision.chapters[1].chapter.id == chapter_id('a', 5));
}

TEST_CASE("chapters without a chapter label are matched by id only", "[update_plan]") {
    const auto label = GENERATE(Label{std::nullopt}, Label{std::string{}});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 9), std::nullopt, label, group_a())});

    SECTION("a different id is a different oneshot") {
        const md::PlannedEpub planned =
            epub_of({planned_chapter(chapter_id('b', 9), std::nullopt, label, group_b())});
        const md::UpdateDecision decision = decide(planned, held);
        CHECK(decision.state == md::UpdateState::diverged);
        CHECK(decision.kept.empty());
    }
    SECTION("the same id is the same oneshot") {
        const md::PlannedEpub planned =
            epub_of({planned_chapter(chapter_id('a', 9), std::nullopt, std::nullopt, group_a())});
        const md::UpdateDecision decision = decide(planned, held);
        CHECK(decision.state == md::UpdateState::complete);
        REQUIRE(decision.chapters.size() == 1);
        CHECK(decision.chapters[0].source == md::ChapterSource::reuse);
    }
}

TEST_CASE("a relabelled chapter with the same id is reused with the plan's labels",
          "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 2), "1", "2.5", group_a())});
    const md::UpdateDecision decision = decide(planned, held_volume_one());

    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.extra == indices({0}));
    REQUIRE(decision.chapters.size() == 1);
    CHECK(decision.chapters[0].held == 1U);
    CHECK(decision.chapters[0].chapter.chapter == "2.5");
    CHECK(decision.chapters[0].source == md::ChapterSource::reuse);
    CHECK(decision.kept.empty());
}

TEST_CASE("an id match wins over a number match for another planned chapter", "[update_plan]") {
    // The held ch 1 (id A1) was relabelled ch 2 by MangaDex, and a new ch 1 appeared.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_a()),
                 planned_chapter(chapter_id('a', 1), "1", "2", group_a())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a())});
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::incomplete);
    CHECK(decision.missing == indices({0}));
    REQUIRE(decision.chapters.size() == 2);
    CHECK(decision.chapters[1].held == 0U);
    CHECK(decision.kept.empty());
}

TEST_CASE("numeric labels are compared normalized", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 7), "01", "7.0", group_b())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 7), "1", "07", group_a())});
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::complete);
    REQUIRE(decision.kept.size() == 1);
    CHECK(decision.kept[0].key_label == "Vol. 1 Ch. 7");
}

TEST_CASE("text labels match exactly", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "Extra", group_b())});
    SECTION("the same text") {
        const md::BookManifest held =
            manifest_of({held_chapter(chapter_id('a', 1), "1", "Extra", group_a())});
        CHECK(decide(planned, held).state == md::UpdateState::complete);
    }
    SECTION("different text") {
        const md::BookManifest held =
            manifest_of({held_chapter(chapter_id('a', 1), "1", "extra", group_a())});
        CHECK(decide(planned, held).state == md::UpdateState::diverged);
    }
}

TEST_CASE("a held duplicate key: the first held release in file order is matched",
          "[update_plan]") {
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a()),
                     held_chapter(chapter_id('b', 1), "1", "1", group_b())});

    SECTION("by number") {
        const md::PlannedEpub planned =
            epub_of({planned_chapter(chapter_id('c', 1), "1", "1", group_c())});
        const md::UpdateDecision decision = decide(planned, held);
        CHECK(decision.state == md::UpdateState::complete);
        REQUIRE(decision.chapters.size() == 1);
        CHECK(decision.chapters[0].held == 0U);
        CHECK(decision.extra == indices({1}));
    }
    SECTION("by id, the matching id wins over file order") {
        const md::PlannedEpub planned =
            epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b())});
        const md::UpdateDecision decision = decide(planned, held);
        REQUIRE(decision.chapters.size() == 1);
        CHECK(decision.chapters[0].held == 1U);
        CHECK(decision.extra == indices({0}));
        CHECK(decision.kept.empty());
    }
}

TEST_CASE("ids are matched ASCII case-insensitively", "[update_plan]") {
    std::string upper = chapter_id('a', 1);
    std::ranges::transform(upper, upper.begin(), [](char c) {
        return c >= 'a' && c <= 'f' ? static_cast<char>(c - 32) : c;
    });
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "9", group_a())});
    const md::BookManifest held = manifest_of({held_chapter(upper, "1", "1", group_a())});
    const md::UpdateDecision decision = decide(planned, held);

    CHECK(decision.state == md::UpdateState::complete);
    REQUIRE(decision.chapters.size() == 1);
    CHECK(decision.chapters[0].source == md::ChapterSource::reuse);
    CHECK(decision.chapters[0].chapter.id == chapter_id('a', 1)); // the plan's spelling
    CHECK(decision.kept.empty());
}

TEST_CASE("an empty plan or an empty manifest", "[update_plan]") {
    SECTION("nothing planned: complete, everything held is extra") {
        const md::UpdateDecision decision = decide(epub_of({}), held_volume_one());
        CHECK(decision.state == md::UpdateState::complete);
        CHECK(decision.chapters.empty());
        CHECK(decision.extra == indices({0, 1}));
    }
    SECTION("nothing held: incomplete, everything is downloaded") {
        const md::PlannedEpub planned =
            epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a())});
        const md::UpdateDecision decision = decide(planned, manifest_of({}));
        CHECK(decision.state == md::UpdateState::incomplete);
        CHECK(decision.missing == indices({0}));
        CHECK(md::reused_chapter_ids(decision).empty());
    }
}

TEST_CASE("the order of the held chapters does not change the decision", "[update_plan]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a()),
                 planned_chapter(chapter_id('b', 2), "1", "2", group_b()),
                 planned_chapter(chapter_id('b', 3), "1", "3", group_b()),
                 planned_chapter(chapter_id('b', 4), "2", "4", group_b()),
                 planned_chapter(chapter_id('b', 6), "2", "6", group_b())});
    std::vector<md::ManifestChapter> chapters{
        held_chapter(chapter_id('a', 1), "1", "1", group_a(), 1),
        held_chapter(chapter_id('a', 2), "1", "2", group_a(), 2),
        held_chapter(chapter_id('a', 4), std::nullopt, "4", group_a(), 3),
        held_chapter(chapter_id('a', 5), "2", "5", group_a(), 4),
        held_chapter(chapter_id('a', 7), std::nullopt, std::nullopt, group_a(), 5),
    };
    const md::UpdateDecision reference = decide(planned, manifest_of(chapters));
    REQUIRE(reference.state == md::UpdateState::diverged);
    REQUIRE(reference.kept.size() == 2);

    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng{seed};
        std::ranges::shuffle(chapters, rng);
        const md::BookManifest held = manifest_of(chapters);
        const md::UpdateDecision decision = decide(planned, held);
        CHECK(decision.state == reference.state);
        CHECK(decision.missing == reference.missing);
        CHECK(decision.kept == reference.kept);
        REQUIRE(decision.chapters.size() == reference.chapters.size());
        for (std::size_t i = 0; i < decision.chapters.size(); ++i) {
            const md::FinalChapter& entry = decision.chapters[i];
            CHECK(entry.chapter == reference.chapters[i].chapter);
            CHECK(entry.source == reference.chapters[i].source);
            CHECK(entry.held.has_value() == reference.chapters[i].held.has_value());
            if (entry.held) {
                REQUIRE(*entry.held < held.chapters.size());
                CHECK(held.chapters[*entry.held].id == entry.chapter.id);
            }
        }
        std::vector<std::string> extra_ids{};
        for (const std::size_t index : decision.extra) {
            REQUIRE(index < held.chapters.size());
            extra_ids.push_back(held.chapters[index].id);
        }
        std::ranges::sort(extra_ids);
        CHECK(extra_ids == std::vector<std::string>{chapter_id('a', 5), chapter_id('a', 7)});
    }
}

// ---- re-pick on --group (decision 5) -----------------------------------------------------------

TEST_CASE("re-pick on --group: a release matched by number is replaced only when it ranks better",
          "[update_plan][repick]") {
    // clean, then clean-regrouped: B's ch 1 and 2 replace A's under new ids.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b()),
                 planned_chapter(chapter_id('b', 2), "1", "2", group_b())});
    const md::BookManifest held = held_volume_one();

    SECTION("better replaces, and the held release is neither reused nor extra") {
        const auto preferred =
            GENERATE(std::vector<std::string>{"Group B"}, std::vector<std::string>{"group b"},
                     std::vector<std::string>{"BBBBBBBB-0000-4000-8000-00000000000B"},
                     std::vector<std::string>{"Group B", "Group A"});
        const md::UpdateDecision decision =
            decide(planned, held, preferred, md::ImageQuality::data_saver);
        CHECK(decision.state == md::UpdateState::incomplete);
        CHECK(decision.missing == indices({0, 1}));
        CHECK(decision.extra.empty());
        CHECK(decision.kept.empty());
        CHECK(decision.repicked == std::vector<md::Repick>{
                                       {.held_id = chapter_id('a', 1),
                                        .planned_id = chapter_id('b', 1),
                                        .held_group = "Group A",
                                        .planned_group = "Group B",
                                        .key_label = "Vol. 1 Ch. 1"},
                                       {.held_id = chapter_id('a', 2),
                                        .planned_id = chapter_id('b', 2),
                                        .held_group = "Group A",
                                        .planned_group = "Group B",
                                        .key_label = "Vol. 1 Ch. 2"},
                                   });
        REQUIRE(decision.chapters.size() == 2);
        CHECK(decision.chapters[0] == md::FinalChapter{.chapter = planned.chapters[0],
                                                       .source = md::ChapterSource::download,
                                                       .quality = md::ImageQuality::data_saver,
                                                       .held = 0,
                                                       .planned = 0});
        CHECK(md::new_chapter_ids(decision) ==
              std::vector<std::string>{chapter_id('b', 1), chapter_id('b', 2)});
        CHECK(md::reused_chapter_ids(decision).empty());
    }
    SECTION("equal, worse, both unmatched and no --group keep the held release") {
        const auto preferred = GENERATE(
            std::vector<std::string>{}, std::vector<std::string>{"Group C"},
            std::vector<std::string>{"Group A"}, std::vector<std::string>{"Group A", "Group B"});
        const md::UpdateDecision decision = decide(planned, held, preferred);
        CHECK(decision.state == md::UpdateState::complete);
        CHECK(decision.repicked.empty());
        CHECK(decision.kept.size() == 2);
        CHECK(md::reused_chapter_ids(decision) ==
              std::vector<std::string>{chapter_id('a', 1), chapter_id('a', 2)});
    }
}

TEST_CASE("equal ranks keep the held release even when both releases match a --group entry",
          "[update_plan][repick]") {
    md::Chapter joint = planned_chapter(chapter_id('b', 1), "1", "1", group_b());
    joint.groups.push_back(group_a()); // a joint release of B and A ranks like A's own
    const md::PlannedEpub planned = epub_of({std::move(joint)});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a())});
    const md::UpdateDecision decision = decide(planned, held, {"Group A"});
    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.repicked.empty());
    CHECK(decision.kept.size() == 1);
}

TEST_CASE("a release matched by id is never re-picked", "[update_plan][repick]") {
    // The same chapter whose groups changed on MangaDex.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_b())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 1), "1", "1", group_a())});
    const md::UpdateDecision decision = decide(planned, held, {"Group B"});
    CHECK(decision.state == md::UpdateState::complete);
    CHECK(decision.repicked.empty());
    REQUIRE(decision.chapters.size() == 1);
    CHECK(decision.chapters[0].source == md::ChapterSource::reuse);
}

TEST_CASE("a loose-into-volume match is re-picked too", "[update_plan][repick]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 5), "2", "5", group_b())});
    const md::BookManifest held =
        manifest_of({held_chapter(chapter_id('a', 5), std::nullopt, "5", group_a())});
    const md::UpdateDecision decision = decide(planned, held, {"Group B"});
    CHECK(decision.state == md::UpdateState::incomplete);
    REQUIRE(decision.repicked.size() == 1);
    CHECK(decision.repicked[0].key_label == "Vol. 2 Ch. 5");
    CHECK(decision.extra.empty());
}

TEST_CASE("a re-pick with extras held elsewhere is diverged", "[update_plan][repick]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b())});
    const md::UpdateDecision decision = decide(planned, held_volume_one(), {"Group B"});
    CHECK(decision.state == md::UpdateState::diverged);
    CHECK(decision.missing == indices({0}));
    CHECK(decision.extra == indices({1}));
}

TEST_CASE("re-pick group names list every group of each release", "[update_plan][repick]") {
    md::Chapter joint = planned_chapter(chapter_id('b', 1), "1", "1", group_b());
    joint.groups.push_back(group_c());
    md::ManifestChapter none = held_chapter(chapter_id('a', 1), "1", "1", group_a());
    none.groups.clear();
    const md::UpdateDecision decision =
        decide(epub_of({std::move(joint)}), manifest_of({std::move(none)}), {"Group C"});
    REQUIRE(decision.repicked.size() == 1);
    CHECK(decision.repicked[0].planned_group == "Group B, Group C");
    CHECK(decision.repicked[0].held_group.empty());
}

// ---- final list --------------------------------------------------------------------------------

TEST_CASE("final_epub holds the final list, its page total and the plan's file fields",
          "[update_plan][final]") {
    // clean, then clean-regrouped-grown: A's ch 1-2 are kept, B's ch 2.5 is new.
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('b', 1), "1", "1", group_b(), 9),
                 planned_chapter(chapter_id('b', 2), "1", "2", group_b(), 9),
                 planned_chapter(chapter_id('b', 25), "1", "2.5", group_b(), 5)});
    md::BookManifest held = held_volume_one();
    held.chapters[0].quality = md::ImageQuality::data_saver;
    const md::UpdateDecision decision = decide(planned, held);
    REQUIRE(decision.state == md::UpdateState::incomplete);

    const md::PlannedEpub out = md::final_epub(planned, decision);
    CHECK(out.kind == planned.kind);
    CHECK(out.file_name == planned.file_name);
    CHECK(out.display_title == planned.display_title);
    CHECK(out.volume == planned.volume);
    CHECK(out.chapter == planned.chapter);
    CHECK(out.cover == planned.cover);
    CHECK(out.identifier == planned.identifier);
    CHECK(out.series_position == planned.series_position);
    CHECK(out.plan_error == planned.plan_error);
    CHECK(out.planned_pages == 2U + 3U + 5U); // held images + planned pages
    REQUIRE(out.chapters.size() == 3);
    for (std::size_t i = 0; i < out.chapters.size(); ++i) {
        CHECK(out.chapters[i] == decision.chapters[i].chapter);
    }
    // Contributors and modified are derived from these chapters: A's (held) then B's (new).
    CHECK(out.chapters[0].groups == std::vector<md::ScanGroup>{group_a()});
    CHECK(out.chapters[1].groups == std::vector<md::ScanGroup>{group_a()});
    CHECK(out.chapters[2].groups == std::vector<md::ScanGroup>{group_b()});
    CHECK(out.chapters[0].updated_at == "2020-01-02T00:00:00+00:00");
    CHECK(out.chapters[2].updated_at == "2024-02-02T00:00:00+00:00");
    // Mixed qualities: each chapter keeps its own; the new one gets the run's.
    CHECK(decision.chapters[0].quality == md::ImageQuality::data_saver);
    CHECK(decision.chapters[1].quality == md::ImageQuality::data);
    CHECK(decision.chapters[2].quality == md::ImageQuality::data);
}

TEST_CASE("final_epub treats a negative page count as zero", "[update_plan][final]") {
    md::UpdateDecision decision{};
    decision.chapters.push_back(
        md::FinalChapter{.chapter = planned_chapter(chapter_id('a', 1), "1", "1", group_a(), -4),
                         .source = md::ChapterSource::download,
                         .quality = md::ImageQuality::data,
                         .held = std::nullopt,
                         .planned = 0});
    CHECK(md::final_epub(epub_of({}), decision).planned_pages == 0U);
}

TEST_CASE("fall_back_to_download switches one entry to the planned release",
          "[update_plan][fallback]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a(), 6),
                 planned_chapter(chapter_id('b', 2), "1", "2", group_b(), 7),
                 planned_chapter(chapter_id('b', 3), "1", "3", group_b(), 8)});
    const md::UpdateDecision before = decide(planned, held_volume_one());
    REQUIRE(before.state == md::UpdateState::incomplete);
    REQUIRE(before.missing == indices({2}));
    REQUIRE(before.kept.size() == 1);

    SECTION("a kept held release: the planned release is downloaded and the substitution goes") {
        const md::UpdateDecision after =
            md::fall_back_to_download(before, 1, planned, md::ImageQuality::data_saver);
        CHECK(after.chapters[1] == md::FinalChapter{.chapter = planned.chapters[1],
                                                    .source = md::ChapterSource::download,
                                                    .quality = md::ImageQuality::data_saver,
                                                    .held = 1,
                                                    .planned = 1});
        CHECK(after.kept.empty());
        CHECK(after.missing == indices({1, 2}));
        CHECK(after.state == md::UpdateState::incomplete);
        CHECK(after.chapters[0] == before.chapters[0]);
        CHECK(after.chapters[2] == before.chapters[2]);
        CHECK(md::new_chapter_ids(after) ==
              std::vector<std::string>{chapter_id('b', 2), chapter_id('b', 3)});
        CHECK(md::reused_chapter_ids(after) == std::vector<std::string>{chapter_id('a', 1)});
        CHECK(md::final_epub(planned, after).planned_pages == 2U + 7U + 8U);
    }
    SECTION("a same-id held chapter: the planned chapter with its own page count") {
        const md::UpdateDecision after =
            md::fall_back_to_download(before, 0, planned, md::ImageQuality::data);
        CHECK(after.chapters[0].chapter == planned.chapters[0]);
        CHECK(after.chapters[0].chapter.pages == 6);
        CHECK(after.chapters[0].source == md::ChapterSource::download);
        CHECK(after.chapters[0].held == 0U);
        CHECK(after.missing == indices({0, 2}));
        CHECK(after.kept == before.kept);
    }
    SECTION("both fall back, in either order") {
        const md::UpdateDecision forward = md::fall_back_to_download(
            md::fall_back_to_download(before, 0, planned, md::ImageQuality::data), 1, planned,
            md::ImageQuality::data);
        const md::UpdateDecision backward = md::fall_back_to_download(
            md::fall_back_to_download(before, 1, planned, md::ImageQuality::data), 0, planned,
            md::ImageQuality::data);
        CHECK(forward == backward);
        CHECK(forward.missing == indices({0, 1, 2}));
        CHECK(md::reused_chapter_ids(forward).empty());
    }
    SECTION("an entry that already downloads is unchanged") {
        CHECK(md::fall_back_to_download(before, 2, planned, md::ImageQuality::data_saver) ==
              before);
    }
    SECTION("an out-of-range index is checked: the decision comes back unchanged") {
        const auto index =
            GENERATE(std::size_t{3}, std::size_t{4}, std::numeric_limits<std::size_t>::max());
        CHECK(md::fall_back_to_download(before, index, planned, md::ImageQuality::data) == before);
    }
    SECTION("an entry whose planned index is outside the given plan is unchanged") {
        CHECK(md::fall_back_to_download(before, 1, epub_of({}), md::ImageQuality::data) == before);
    }
}

TEST_CASE("fall_back_to_download makes a complete decision incomplete", "[update_plan][fallback]") {
    const md::PlannedEpub planned =
        epub_of({planned_chapter(chapter_id('a', 1), "1", "1", group_a()),
                 planned_chapter(chapter_id('a', 2), "1", "2", group_a())});
    const md::UpdateDecision before = decide(planned, held_volume_one());
    REQUIRE(before.state == md::UpdateState::complete);
    const md::UpdateDecision after =
        md::fall_back_to_download(before, 1, planned, md::ImageQuality::data);
    CHECK(after.state == md::UpdateState::incomplete);
    CHECK(after.missing == indices({1}));
}
