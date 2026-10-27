// tests/unit/mdlist/mdlist_plan_test.cpp: `mdlist` phase 1 (list plan T8): validate_mdlist_args
// with its usage errors in their documented order (list spec "Command surface", AC10 unit side),
// plan_list's list-to-jobs step with batch's dedupe (AC6), and the `batch <list URL>` check in
// validate_batch_args (AC15, unit side). Lists are built by hand, never through the parser.
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "mangadex_dl/options.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view list_uuid = "a0000000-0000-4000-8000-000000000001";
constexpr std::string_view list_upper = "A0000000-0000-4000-8000-000000000001";
constexpr std::string_view uuid_a = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view uuid_b = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view uuid_c = "33333333-3333-4333-8333-333333333333";

constexpr std::string_view list_empty_message = "LIST must not be empty";
constexpr std::string_view list_title_message =
    "LIST is a MangaDex title, not a list; use 'mangadex-dl download <title>' for one title";
constexpr std::string_view list_invalid_message =
    "LIST must be a MangaDex list UUID or a https://mangadex.org/list/<uuid> URL; for a CSV file "
    "of titles use 'mangadex-dl batch <file>'";
constexpr std::string_view dir_empty_message = "DIR must not be empty";
constexpr std::string_view dir_id_message = "DIR looks like a MangaDex id; give one list per run";
constexpr std::string_view lang_message =
    "--lang must be a lowercase language code such as en, pt-br or es-la";

constexpr std::string_view batch_title_message =
    "FILE looks like a MangaDex title; use 'mangadex-dl download <title>' for one title";
constexpr std::string_view batch_list_message =
    "FILE looks like a MangaDex list; use 'mangadex-dl mdlist <list>'";

/// The usage-error message of `result`, or "<accepted>" when it holds a value.
template <class T> [[nodiscard]] std::string usage_message(const md::Result<T>& result) {
    if (result.has_value()) {
        return "<accepted>";
    }
    CHECK(result.error().code == md::Errc::usage);
    return result.error().message;
}

[[nodiscard]] md::RawMdlistArgs mdlist_args(std::string list = std::string{list_uuid},
                                            std::string dir = "out") {
    md::RawMdlistArgs raw{.list = std::move(list)};
    raw.defaults.dir = std::move(dir);
    return raw;
}

[[nodiscard]] std::string mdlist_message(const md::RawMdlistArgs& raw) {
    return usage_message(md::validate_mdlist_args(raw));
}

[[nodiscard]] md::MdlistSettings settings_for(const md::RawMdlistArgs& raw) {
    md::Result<md::MdlistSettings> settings = md::validate_mdlist_args(raw);
    REQUIRE(settings.has_value());
    return std::move(*settings);
}

[[nodiscard]] md::MangaId manga(std::string_view text) {
    md::Result<md::MangaId> id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] md::CustomList list_of(const std::vector<std::string_view>& ids) {
    md::Result<md::ListId> list_id = md::ListId::parse(list_uuid);
    REQUIRE(list_id.has_value());
    md::CustomList list{.id = *list_id, .name = std::string{"A list"}};
    for (const std::string_view id : ids) {
        list.titles.push_back(manga(id));
    }
    return list;
}

[[nodiscard]] std::string list_url(std::string_view suffix = "") {
    return std::format("https://mangadex.org/list/{}{}", list_uuid, suffix);
}

} // namespace

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

// --- validate_mdlist_args -------------------------------------------------------------------

TEST_CASE("mdlist: validate_mdlist_args keeps LIST, DIR and the CLI-level options",
          "[mdlist][plan]") {
    md::RawMdlistArgs raw = mdlist_args(list_url("/My-List"), "Manga");
    raw.defaults.lang = "ja";
    raw.defaults.groups = {"G"};
    raw.defaults.dry_run = true;
    raw.fail_fast = true;
    raw.dir_per_title = true;
    const md::MdlistSettings settings = settings_for(raw);

    CHECK(settings.list.str() == list_uuid);
    CHECK(settings.dir_per_title);
    CHECK(settings.batch.file == list_url("/My-List")); // LIST as typed
    CHECK(settings.batch.label == md::list_row_label);
    CHECK(settings.batch.label == "list");
    CHECK(settings.batch.source == md::BatchSource::mdlist);
    CHECK(settings.batch.fail_fast);
    CHECK(settings.batch.defaults.dir == "Manga");
    CHECK(settings.batch.defaults.lang == "ja");
    CHECK(settings.batch.defaults.manga.empty());
    CHECK(settings.batch.base.manga == md::MangaId{});
    CHECK(settings.batch.base.dir == std::filesystem::path{"Manga"});
    CHECK(settings.batch.base.lang == "ja");
    CHECK(settings.batch.base.groups == std::vector<std::string>{"G"});
    CHECK(settings.batch.base.dry_run);
    // The flag reaches the jobs through plan_list, never through the base options.
    CHECK(!settings.batch.base.dir_per_title);
}

TEST_CASE("mdlist: validate_mdlist_args lowercases LIST and keeps the flags off by default",
          "[mdlist][plan]") {
    const md::MdlistSettings settings = settings_for(mdlist_args(std::string{list_upper}));
    CHECK(settings.list.str() == list_uuid);
    CHECK(settings.batch.file == list_upper);
    CHECK(!settings.dir_per_title);
    CHECK(!settings.batch.fail_fast);
    CHECK(settings.batch.defaults.dir == "out");
}

TEST_CASE("mdlist: validate_mdlist_args rejects each mistake with its exact message",
          "[mdlist][plan]") {
    const auto title_url = std::format("https://mangadex.org/title/{}/slug", uuid_a);
    CHECK(mdlist_message(mdlist_args("")) == list_empty_message);
    CHECK(mdlist_message(mdlist_args(title_url)) == list_title_message);
    CHECK(mdlist_message(mdlist_args("-")) == list_invalid_message);
    CHECK(mdlist_message(mdlist_args("titles.csv")) == list_invalid_message);
    CHECK(mdlist_message(mdlist_args("junk junk")) == list_invalid_message);
    CHECK(mdlist_message(mdlist_args(list_url("/a/b"))) == list_invalid_message);
    CHECK(mdlist_message(mdlist_args(std::string{list_uuid}, "")) == dir_empty_message);
    CHECK(mdlist_message(mdlist_args(std::string{list_uuid}, std::string{uuid_b})) ==
          dir_id_message);
    CHECK(mdlist_message(mdlist_args(std::string{list_uuid},
                                     std::format("https://mangadex.org/title/{}", uuid_b))) ==
          dir_id_message);
    CHECK(mdlist_message(mdlist_args(std::string{list_uuid}, list_url())) == dir_id_message);
    CHECK(mdlist_message(mdlist_args(std::string{list_uuid}, list_url("/slug/"))) ==
          dir_id_message);

    md::RawMdlistArgs bad_lang = mdlist_args();
    bad_lang.defaults.lang = "bogus";
    CHECK(mdlist_message(bad_lang) == lang_message);
}

TEST_CASE("mdlist: validate_mdlist_args checks LIST, then DIR, then the options",
          "[mdlist][plan]") {
    md::RawMdlistArgs raw = mdlist_args("", "");
    raw.defaults.lang = "bogus";
    CHECK(mdlist_message(raw) == list_empty_message); // LIST empty comes first

    raw.list = "titles.csv";
    CHECK(mdlist_message(raw) == list_invalid_message); // a bad LIST before an empty DIR

    raw.list = std::string{list_uuid};
    CHECK(mdlist_message(raw) == dir_empty_message); // DIR before the options

    raw.defaults.dir = std::string{uuid_a};
    CHECK(mdlist_message(raw) == dir_id_message);

    raw.defaults.dir = "out";
    CHECK(mdlist_message(raw) == lang_message);

    raw.defaults.lang = "en";
    CHECK(mdlist_message(raw) == "<accepted>");
}

TEST_CASE("mdlist: validate_mdlist_args validates the CLI-level options as batch does",
          "[mdlist][plan]") {
    const auto message = [](const auto& tweak) {
        md::RawMdlistArgs raw = mdlist_args();
        tweak(raw.defaults);
        std::string mdlist = mdlist_message(raw);
        const md::RawBatchArgs batch{.file = "titles.csv", .defaults = raw.defaults};
        CHECK(usage_message(md::validate_batch_args(batch)) == mdlist);
        return mdlist;
    };
    CHECK(message([](md::RawDownloadArgs& d) { d.layout = "pdf"; }) ==
          "--layout must be volumes, chapters or single");
    CHECK(message([](md::RawDownloadArgs& d) { d.direction = "up"; }) ==
          "--direction must be auto, ltr or rtl");
    CHECK(message([](md::RawDownloadArgs& d) {
              d.volumes = "none";
              d.loose_chapters = "none";
          }) == "--volumes and --loose-chapters are both none: nothing to download");
    CHECK(message([](md::RawDownloadArgs& d) { d.jobs = 9; }) == "--jobs must be between 1 and 8");
    CHECK(message([](md::RawDownloadArgs& d) { d.api_base = "ftp://x"; }) ==
          "--api-base must start with https://, http:// (loopback only) or file://");
    CHECK(message([](md::RawDownloadArgs& d) { d.volumes = ""; }).starts_with("--volumes: "));
}

TEST_CASE("mdlist: no validate_mdlist_args message contains LIST or DIR", "[mdlist][plan]") {
    constexpr std::string_view marker = "zq-marker-zq";
    const std::string list =
        GENERATE(std::string{"zq-marker-zq"},
                 std::format("https://mangadex.org/list/{}/zq-marker-zq/x", list_uuid),
                 std::format("https://mangadex.org/title/{}/zq-marker-zq", uuid_a));
    CAPTURE(list);
    const std::string message = mdlist_message(mdlist_args(list));
    CHECK(message != "<accepted>");
    CHECK(!message.contains(marker));
}

TEST_CASE("mdlist: validate_mdlist_args rejects a title for every item", "[mdlist][plan]") {
    // Any engaged title, even an empty one, would be copied into every job.
    const std::string title = GENERATE(std::string{"X"}, std::string{});
    CAPTURE(title);
    md::RawMdlistArgs raw = mdlist_args();
    raw.defaults.title = title;
    raw.defaults.lang = "bogus"; // the title is checked before the options
    CHECK(mdlist_message(raw) ==
          "mdlist has no --title option; to name a title yourself, download it on its own with "
          "'mangadex-dl download <title> --title ...'");

    raw.defaults.dir = ""; // and after LIST and DIR
    CHECK(mdlist_message(raw) == dir_empty_message);
}

// --- plan_list --------------------------------------------------------------------------------

TEST_CASE("mdlist: plan_list makes one job per title in API order", "[mdlist][plan]") {
    md::RawMdlistArgs raw = mdlist_args(std::string{list_uuid}, "Manga");
    raw.defaults.lang = "ja";
    const md::MdlistSettings settings = settings_for(raw);
    const md::CustomList list = list_of({uuid_c, uuid_a, uuid_b});

    const std::vector<md::BatchJob> jobs = md::plan_list(list, settings);
    REQUIRE(jobs.size() == 3);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        CAPTURE(i);
        const md::BatchJob& job = jobs[i];
        CHECK(job.line == i + 1);
        CHECK(job.dir == "Manga");
        CHECK(job.sub_dir.empty());
        CHECK(!job.duplicate_of.has_value());
        CHECK(job.options.manga == list.titles[i]);
        CHECK(job.raw.manga == list.titles[i].str());

        // options = base plus the manga; raw = defaults plus the manga string.
        md::DownloadOptions expected_options = settings.batch.base;
        expected_options.manga = list.titles[i];
        CHECK(job.options == expected_options);
        CHECK(!job.options.dir_per_title);
        CHECK(job.options.dir == std::filesystem::path{"Manga"});
        CHECK(job.options.lang == "ja");
        md::RawDownloadArgs expected_raw = settings.batch.defaults;
        expected_raw.manga = std::string{list.titles[i].str()};
        CHECK(job.raw.dir == expected_raw.dir);
        CHECK(job.raw.lang == expected_raw.lang);
        CHECK(job.raw.volumes == expected_raw.volumes);
        CHECK(!job.raw.title.has_value());
    }
    CHECK(jobs[0].raw.manga == uuid_c);
    CHECK(jobs[1].raw.manga == uuid_a);
    CHECK(jobs[2].raw.manga == uuid_b);
}

TEST_CASE("mdlist: plan_list sets dir_per_title on every job when asked", "[mdlist][plan]") {
    md::RawMdlistArgs raw = mdlist_args();
    raw.dir_per_title = true;
    const md::MdlistSettings settings = settings_for(raw);

    const std::vector<md::BatchJob> jobs = md::plan_list(list_of({uuid_a, uuid_b}), settings);
    REQUIRE(jobs.size() == 2);
    for (const md::BatchJob& job : jobs) {
        CHECK(job.options.dir_per_title);
        CHECK(job.dir == "out"); // still DIR: the engine picks the title directory after the plan
        CHECK(job.sub_dir.empty());
    }
}

TEST_CASE("mdlist: plan_list marks a repeated title as a duplicate of its first position",
          "[mdlist][plan]") {
    const bool dir_per_title = GENERATE(false, true);
    CAPTURE(dir_per_title);
    md::RawMdlistArgs raw = mdlist_args();
    raw.dir_per_title = dir_per_title;
    const md::MdlistSettings settings = settings_for(raw);

    const std::vector<md::BatchJob> jobs =
        md::plan_list(list_of({uuid_a, uuid_b, uuid_a, uuid_c, uuid_b, uuid_a}), settings);
    REQUIRE(jobs.size() == 6);
    CHECK(jobs[0].duplicate_of == std::nullopt);
    CHECK(jobs[1].duplicate_of == std::nullopt);
    CHECK(jobs[2].duplicate_of == std::optional<std::size_t>{1});
    CHECK(jobs[3].duplicate_of == std::nullopt);
    CHECK(jobs[4].duplicate_of == std::optional<std::size_t>{2});
    CHECK(jobs[5].duplicate_of == std::optional<std::size_t>{1}); // the first, never a duplicate
    // Duplicates keep their own position and options.
    CHECK(jobs[2].line == 3);
    CHECK(jobs[2].options == jobs[0].options);
}

TEST_CASE("mdlist: plan_list of an empty list makes no jobs", "[mdlist][plan]") {
    const md::MdlistSettings settings = settings_for(mdlist_args());
    CHECK(md::plan_list(list_of({}), settings).empty());
}

// --- validate_batch_args: the `batch <list URL>` check -----------------------------------------

TEST_CASE("mdlist: validate_batch_args names mdlist for a list URL", "[mdlist][plan][batch]") {
    const std::string file =
        GENERATE(list_url(), list_url("/"), list_url("/slug"), list_url("/slug/?q#f"),
                 std::format("mangadex.org/list/{}", list_upper),
                 std::format("http://www.mangadex.org/list/{}", list_uuid));
    CAPTURE(file);
    const md::RawBatchArgs raw{.file = file};
    CHECK(usage_message(md::validate_batch_args(raw)) == batch_list_message);
    // DIR empty is checked after FILE.
    const md::RawBatchArgs empty_dir{.file = file, .defaults = md::RawDownloadArgs{.dir = ""}};
    CHECK(usage_message(md::validate_batch_args(empty_dir)) == batch_list_message);
}

TEST_CASE("mdlist: validate_batch_args keeps the title message for a bare UUID",
          "[mdlist][plan][batch]") {
    // A bare UUID is a title and a list spelling at once: the title check comes first.
    const std::string file = GENERATE(std::string{list_uuid}, std::string{list_upper});
    CAPTURE(file);
    const md::RawBatchArgs raw{.file = file};
    CHECK(usage_message(md::validate_batch_args(raw)) == batch_title_message);
}

TEST_CASE("mdlist: validate_batch_args still accepts files with list-like names",
          "[mdlist][plan][batch]") {
    const std::string file =
        GENERATE(std::string{"list.csv"}, std::string{"mangadex.org/list/"}, list_url("/a/b"));
    CAPTURE(file);
    const md::RawBatchArgs raw{.file = file};
    CHECK(usage_message(md::validate_batch_args(raw)) == "<accepted>");
}
