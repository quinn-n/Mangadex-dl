// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/follows/follows_plan_test.cpp: `follows` phase 1 (follows plan T14): the usage
// errors of validate_follows_args in their documented order (follows spec "Semantic usage
// checks", AC19 unit side, rev1 `--credentials ''`), the settings it builds (label and source
// "follows", the --status filter, the raw auth flags), and plan_follows's ids-to-jobs step with
// batch's dedupe (AC11: positions 1..k with no gaps, the "item" wording of the closing line).
// Follows are built by hand, never fetched.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/options.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view uuid_a = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view uuid_b = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view uuid_c = "33333333-3333-4333-8333-333333333333";
constexpr std::string_view list_uuid = "a0000000-0000-4000-8000-000000000001";

constexpr std::string_view dir_empty_message = "DIR must not be empty";
constexpr std::string_view dir_id_message =
    "DIR looks like a MangaDex id; follows takes no title or list (use 'mangadex-dl mdlist "
    "<list>' or 'download <title>')";
constexpr std::string_view credentials_message = "--credentials needs a file name";
constexpr std::string_view title_message =
    "follows has no --title option; to name a title yourself, download it on its own with "
    "'mangadex-dl download <title> --title ...'";
constexpr std::string_view lang_message =
    "--lang must be a lowercase language code such as en, pt-br or es-la";
constexpr std::string_view word_list =
    "reading, on_hold, plan_to_read, dropped, re_reading or completed";

/// The usage-error message of `result`, or "<accepted>" when it holds a value.
template <class T> [[nodiscard]] std::string usage_message(const md::Result<T>& result) {
    if (result.has_value()) {
        return "<accepted>";
    }
    CHECK(result.error().code == md::Errc::usage);
    CHECK(md::exit_code_for(result.error().code) == md::exit_code::usage);
    return result.error().message;
}

[[nodiscard]] md::RawFollowsArgs follows_args(std::string dir = "out") {
    md::RawFollowsArgs raw{};
    raw.defaults.dir = std::move(dir);
    return raw;
}

[[nodiscard]] std::string follows_message(const md::RawFollowsArgs& raw) {
    return usage_message(md::validate_follows_args(raw));
}

[[nodiscard]] md::FollowsSettings settings_for(const md::RawFollowsArgs& raw) {
    md::Result<md::FollowsSettings> settings = md::validate_follows_args(raw);
    INFO((settings ? std::string{} : settings.error().message));
    REQUIRE(settings.has_value());
    return std::move(*settings);
}

[[nodiscard]] md::MangaId manga(std::string_view text) {
    md::Result<md::MangaId> id = md::MangaId::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] std::vector<md::MangaId> ids(const std::vector<std::string_view>& texts) {
    std::vector<md::MangaId> out{};
    out.reserve(texts.size());
    for (const std::string_view text : texts) {
        out.push_back(manga(text));
    }
    return out;
}

/// The value of `optional`, which the caller has just REQUIREd to be engaged. An empty one (a
/// test bug) throws, which Catch2 reports; unlike REQUIRE, the check is visible to
/// bugprone-unchecked-optional-access.
template <class T> [[nodiscard]] const T& engaged(const std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

[[nodiscard]] std::string unknown_status_message(std::string_view word) {
    return std::format("--status: unknown reading status '{}'; use {}", word, word_list);
}

} // namespace

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

// --- validate_follows_args ------------------------------------------------------------------

TEST_CASE("follows plan: validate_follows_args keeps DIR, the options and the flags",
          "[follows][plan]") {
    md::RawFollowsArgs raw = follows_args("Manga");
    raw.defaults.lang = "ja";
    raw.defaults.groups = {"G"};
    raw.defaults.dry_run = true;
    raw.fail_fast = true;
    raw.dir_per_title = true;
    raw.auth = md::RawAuthArgs{.credentials = "creds.txt",
                               .credentials_given = true,
                               .password_stdin = true,
                               .no_input = true};
    const md::FollowsSettings settings = settings_for(raw);

    CHECK(settings.batch.file == "follows"); // `source`: the literal command word
    CHECK(settings.batch.label == md::follows_row_label);
    CHECK(settings.batch.label == "follows");
    CHECK(settings.batch.source == md::BatchSource::follows);
    CHECK(settings.batch.fail_fast);
    CHECK(settings.batch.defaults.dir == "Manga");
    CHECK(settings.batch.defaults.lang == "ja");
    CHECK(settings.batch.defaults.manga.empty());
    CHECK(settings.batch.base.manga == md::MangaId{});
    CHECK(settings.batch.base.dir == std::filesystem::path{"Manga"});
    CHECK(settings.batch.base.lang == "ja");
    CHECK(settings.batch.base.groups == std::vector<std::string>{"G"});
    CHECK(settings.batch.base.dry_run);
    // The flag reaches the jobs through plan_follows, never through the base options.
    CHECK(!settings.batch.base.dir_per_title);
    CHECK(settings.dir_per_title);
    CHECK(!settings.filter.has_value()); // no --status
    CHECK(settings.auth.credentials == "creds.txt");
    CHECK(settings.auth.credentials_given);
    CHECK(settings.auth.password_stdin);
    CHECK(settings.auth.no_input);
}

TEST_CASE("follows plan: validate_follows_args keeps the flags off by default", "[follows][plan]") {
    const md::FollowsSettings settings = settings_for(follows_args());
    CHECK(!settings.dir_per_title);
    CHECK(!settings.batch.fail_fast);
    CHECK(!settings.filter.has_value());
    CHECK(settings.batch.defaults.dir == "out");
    CHECK(!settings.auth.credentials_given);
    CHECK(!settings.auth.password_stdin);
    CHECK(!settings.auth.no_input);
}

TEST_CASE("follows plan: validate_follows_args rejects each mistake with its exact message",
          "[follows][plan]") {
    CHECK(follows_message(follows_args("")) == dir_empty_message);
    const std::string id_dir =
        GENERATE(std::string{uuid_b}, std::format("https://mangadex.org/title/{}", uuid_b),
                 std::format("https://mangadex.org/title/{}/slug", uuid_b),
                 std::format("https://mangadex.org/list/{}", list_uuid),
                 std::format("https://mangadex.org/list/{}/slug/", list_uuid));
    CAPTURE(id_dir);
    CHECK(follows_message(follows_args(id_dir)) == dir_id_message);

    md::RawFollowsArgs empty_credentials = follows_args();
    empty_credentials.auth.credentials_given = true; // `--credentials ''`
    CHECK(follows_message(empty_credentials) == credentials_message);

    md::RawFollowsArgs bad_lang = follows_args();
    bad_lang.defaults.lang = "bogus";
    CHECK(follows_message(bad_lang) == lang_message);

    md::RawFollowsArgs bad_status = follows_args();
    bad_status.status = {"all"};
    CHECK(follows_message(bad_status) == unknown_status_message("all"));
    bad_status.status = {"reading,"};
    CHECK(follows_message(bad_status) ==
          std::format("--status needs a reading status ({})", word_list));
}

TEST_CASE("follows plan: `--credentials` with a name, or not given, is accepted",
          "[follows][plan]") {
    md::RawFollowsArgs raw = follows_args();
    raw.auth.credentials = "--no-input"; // taken verbatim (spec rev3 S8): a file name
    raw.auth.credentials_given = true;
    CHECK(follows_message(raw) == "<accepted>");

    raw.auth.credentials_given = false; // never given: the empty default is not checked
    raw.auth.credentials.clear();
    CHECK(follows_message(raw) == "<accepted>");
}

TEST_CASE("follows plan: validate_follows_args checks DIR, auth, --title, the options, then "
          "--status",
          "[follows][plan]") {
    md::RawFollowsArgs raw = follows_args("");
    raw.auth.credentials_given = true;
    raw.defaults.title = "X";
    raw.defaults.lang = "bogus";
    raw.status = {"all"};
    CHECK(follows_message(raw) == dir_empty_message); // DIR empty comes first

    raw.defaults.dir = std::string{uuid_a};
    CHECK(follows_message(raw) == dir_id_message);

    raw.defaults.dir = "out";
    CHECK(follows_message(raw) == credentials_message); // the raw auth args before the options

    raw.auth.credentials_given = false;
    CHECK(follows_message(raw) == title_message);

    raw.defaults.title.reset();
    CHECK(follows_message(raw) == lang_message); // the options before --status

    raw.defaults.lang = "en";
    CHECK(follows_message(raw) == unknown_status_message("all"));

    raw.status = {"reading"};
    CHECK(follows_message(raw) == "<accepted>");
}

TEST_CASE("follows plan: validate_follows_args rejects a title for every follow",
          "[follows][plan]") {
    // Any engaged title, even an empty one, would be copied into every job.
    const std::string title = GENERATE(std::string{"X"}, std::string{});
    CAPTURE(title);
    md::RawFollowsArgs raw = follows_args();
    raw.defaults.title = title;
    CHECK(follows_message(raw) == title_message);
}

TEST_CASE("follows plan: validate_follows_args validates the CLI-level options as batch does",
          "[follows][plan]") {
    const auto message = [](const auto& tweak) {
        md::RawFollowsArgs raw = follows_args();
        tweak(raw.defaults);
        std::string follows = follows_message(raw);
        const md::RawBatchArgs batch{.file = "titles.csv", .defaults = raw.defaults};
        CHECK(usage_message(md::validate_batch_args(batch)) == follows);
        return follows;
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
}

TEST_CASE("follows plan: no validate_follows_args message echoes DIR or the credentials path",
          "[follows][plan]") {
    // DIR may be a mistyped password (follows plan rev1 gate): never echoed by a usage error.
    constexpr std::string_view marker = "fixture-password-not-a-secret";
    md::RawFollowsArgs raw = follows_args(std::string{marker});
    raw.auth.credentials = std::string{marker};
    raw.auth.credentials_given = true;
    const std::string field = GENERATE(std::string{"lang"}, std::string{"title"},
                                       std::string{"status"}, std::string{"layout"});
    CAPTURE(field);
    if (field == "lang") {
        raw.defaults.lang = "bogus";
    } else if (field == "title") {
        raw.defaults.title = "X";
    } else if (field == "status") {
        raw.status = {"bogus"};
    } else {
        raw.defaults.layout = "pdf";
    }
    const std::string message = follows_message(raw);
    CHECK(message != "<accepted>");
    CHECK(!message.contains(marker));
}

TEST_CASE("follows plan: --status becomes the normalised filter, in the order given",
          "[follows][plan]") {
    md::RawFollowsArgs raw = follows_args();
    raw.status = {"on-hold,reading", "plan-to-read", "reading"};
    const md::FollowsSettings settings = settings_for(raw);
    REQUIRE(settings.filter.has_value());
    const md::StatusFilter& filter = engaged(settings.filter);
    CHECK(filter.names() == std::vector<std::string>{"on_hold", "reading", "plan_to_read"});
    CHECK(filter.contains(md::ReadingStatus::on_hold));
    CHECK(!filter.contains(md::ReadingStatus::completed));
    // The filter never reaches the download options: every job downloads the same way.
    CHECK(settings.batch.base == settings_for(follows_args()).batch.base);
}

// --- plan_follows -----------------------------------------------------------------------------

TEST_CASE("follows plan: plan_follows makes one job per title, positions 1..k in order",
          "[follows][plan]") {
    md::RawFollowsArgs raw = follows_args("Manga");
    raw.defaults.lang = "ja";
    const md::FollowsSettings settings = settings_for(raw);
    const std::vector<md::MangaId> titles = ids({uuid_c, uuid_a, uuid_b});

    const std::vector<md::BatchJob> jobs = md::plan_follows(titles, settings);
    REQUIRE(jobs.size() == 3);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        CAPTURE(i);
        const md::BatchJob& job = jobs[i];
        CHECK(job.line == i + 1);
        CHECK(job.dir == "Manga");
        CHECK(job.sub_dir.empty());
        CHECK(!job.duplicate_of.has_value());
        CHECK(job.options.manga == titles[i]);
        CHECK(job.raw.manga == titles[i].str());

        // options = base plus the manga; raw = defaults plus the manga string.
        md::DownloadOptions expected_options = settings.batch.base;
        expected_options.manga = titles[i];
        CHECK(job.options == expected_options);
        CHECK(!job.options.dir_per_title);
        CHECK(job.options.dir == std::filesystem::path{"Manga"});
        CHECK(job.options.lang == "ja");
        CHECK(job.raw.dir == settings.batch.defaults.dir);
        CHECK(job.raw.lang == settings.batch.defaults.lang);
        CHECK(!job.raw.title.has_value());
    }
    CHECK(jobs[0].raw.manga == uuid_c);
    CHECK(jobs[1].raw.manga == uuid_a);
    CHECK(jobs[2].raw.manga == uuid_b);
}

TEST_CASE("follows plan: plan_follows numbers a filtered sequence with no gaps",
          "[follows][plan]") {
    // What the command hands over after --status: the kept follows, in follows order. Their
    // positions among all follows (1, 3 and 4 here) never show: the items are 1, 2 and 3.
    md::RawFollowsArgs raw = follows_args();
    raw.status = {"reading"};
    const md::FollowsSettings settings = settings_for(raw);
    const std::vector<md::MangaId> follows = ids({uuid_a, uuid_b, uuid_c, uuid_a});
    const md::StatusMap map{
        .statuses = {{follows[0], "reading"}, {follows[1], "completed"}, {follows[2], "reading"}}};
    REQUIRE(settings.filter.has_value());
    const md::FilteredFollows kept = md::filter_follows(follows, map, engaged(settings.filter));
    REQUIRE(kept.titles == ids({uuid_a, uuid_c, uuid_a}));

    const std::vector<md::BatchJob> jobs = md::plan_follows(kept.titles, settings);
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[0].line == 1);
    CHECK(jobs[1].line == 2);
    CHECK(jobs[2].line == 3);
    CHECK(jobs[1].raw.manga == uuid_c);
    // A repeated follow is a duplicate of its first item, counted in the filtered sequence.
    CHECK(jobs[2].duplicate_of == std::optional<std::size_t>{1});
}

TEST_CASE("follows plan: plan_follows sets dir_per_title on every job when asked",
          "[follows][plan]") {
    md::RawFollowsArgs raw = follows_args();
    raw.dir_per_title = true;
    const md::FollowsSettings settings = settings_for(raw);

    const std::vector<md::BatchJob> jobs = md::plan_follows(ids({uuid_a, uuid_b}), settings);
    REQUIRE(jobs.size() == 2);
    for (const md::BatchJob& job : jobs) {
        CHECK(job.options.dir_per_title);
        CHECK(job.dir == "out"); // still DIR: the engine picks the title directory after the plan
        CHECK(job.sub_dir.empty());
    }
}

TEST_CASE("follows plan: plan_follows marks a repeated follow as a duplicate of its first "
          "position",
          "[follows][plan]") {
    const bool dir_per_title = GENERATE(false, true);
    CAPTURE(dir_per_title);
    md::RawFollowsArgs raw = follows_args();
    raw.dir_per_title = dir_per_title;
    const md::FollowsSettings settings = settings_for(raw);

    const std::vector<md::BatchJob> jobs =
        md::plan_follows(ids({uuid_a, uuid_b, uuid_a, uuid_c, uuid_b, uuid_a}), settings);
    REQUIRE(jobs.size() == 6);
    CHECK(jobs[0].duplicate_of == std::nullopt);
    CHECK(jobs[1].duplicate_of == std::nullopt);
    CHECK(jobs[2].duplicate_of == std::optional<std::size_t>{1});
    CHECK(jobs[3].duplicate_of == std::nullopt);
    CHECK(jobs[4].duplicate_of == std::optional<std::size_t>{2});
    CHECK(jobs[5].duplicate_of == std::optional<std::size_t>{1});
    // Duplicates keep their own position and options.
    CHECK(jobs[2].line == 3);
    CHECK(jobs[2].options == jobs[0].options);
}

TEST_CASE("follows plan: plan_follows of no follows makes no jobs", "[follows][plan]") {
    const md::FollowsSettings settings = settings_for(follows_args());
    CHECK(md::plan_follows({}, settings).empty());
}

// --- AC11: follows rows are items -------------------------------------------------------------

TEST_CASE("follows plan: the closing line of a follows run counts items, as mdlist's does",
          "[follows][plan]") {
    md::BatchResult result{};
    result.titles = {
        md::TitleResult{.line = 1, .status = md::TitleStatus::ok, .exit_code = 0},
        md::TitleResult{.line = 2, .status = md::TitleStatus::not_found, .exit_code = 4},
        md::TitleResult{.line = 3, .status = md::TitleStatus::network, .exit_code = 3},
    };
    for (const bool dry_run : {false, true}) {
        CAPTURE(dry_run);
        const std::optional<std::string> follows =
            md::batch_closing_message(result, dry_run, md::BatchSource::follows);
        REQUIRE(follows.has_value());
        CHECK(follows == md::batch_closing_message(result, dry_run, md::BatchSource::mdlist));
        CHECK(engaged(follows).contains("(items 2, 3)"));
        CHECK(!engaged(follows).contains("line"));
    }
}
