// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch/batch_json_follows_test.cpp: the `follows --json` document (follows plan T14,
// spec "--json" rev2, AC11): batch's document plus the additive `follows` object right after
// `source`, with exactly the keys username, titles, statuses (null without --status), selected,
// in that order; `format_version` stays 1; untrusted username bytes (ESC, invalid UTF-8) give
// valid JSON; the empty run; and the batch and mdlist documents are untouched (the follows
// document minus `follows` is the batch document byte for byte, and neither of the other two
// ever carries the key).
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/report.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace md = mangadex_dl;
using OrderedJson = nlohmann::ordered_json;
using md::TitleStatus;

constexpr std::string_view first_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view second_id = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view replacement = "\xEF\xBF\xBD"; // U+FFFD

/// Without --status: every follow selected, `statuses` null.
[[nodiscard]] md::FollowsSummary unfiltered(std::size_t titles = 2) {
    return md::FollowsSummary{.username = "fixture-user", .titles = titles, .selected = titles};
}

/// Two items, as a follows run reports them: line = the position among the selected follows.
[[nodiscard]] md::BatchResult two_items() {
    md::BatchResult result{};
    md::RunReport report{};
    report.manga.id = std::string{first_id};
    report.manga.title = "Clean Title";
    result.titles = {
        md::TitleResult{.line = 1,
                        .manga_id = std::string{first_id},
                        .dir = "out",
                        .status = TitleStatus::ok,
                        .exit_code = 0,
                        .report = report},
        md::TitleResult{.line = 2,
                        .manga_id = std::string{second_id},
                        .dir = "out",
                        .status = TitleStatus::not_found,
                        .exit_code = 4,
                        .error = "title 66666666-6666-4666-8666-666666666666 not found"},
    };
    return result;
}

/// Parses with key order kept; strict UTF-8, so invalid UTF-8 in the output fails the test.
[[nodiscard]] OrderedJson parse(const std::string& text) {
    OrderedJson doc = OrderedJson::parse(text, nullptr, false);
    REQUIRE(!doc.is_discarded());
    return doc;
}

[[nodiscard]] std::vector<std::string> keys(const OrderedJson& object) {
    REQUIRE(object.is_object());
    std::vector<std::string> out{};
    for (const auto& [key, value] : object.items()) {
        out.push_back(key);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> document_keys() {
    return {"format_version", "dry_run", "source", "follows", "titles", "summary"};
}

[[nodiscard]] std::vector<std::string> follows_keys() {
    return {"username", "titles", "statuses", "selected"};
}

/// Dumps like batch_to_json does.
[[nodiscard]] std::string dump(const OrderedJson& doc) {
    std::string text = doc.dump(2, ' ', false, OrderedJson::error_handler_t::replace);
    text += '\n';
    return text;
}

} // namespace

TEST_CASE("batch json follows: batch's keys plus follows{username,titles,statuses,selected} "
          "right after source",
          "[batch][json][follows]") {
    const std::string text = md::batch_to_json(two_items(), "follows", false, unfiltered());
    REQUIRE(!text.empty());
    CHECK(text.back() == '\n');

    const OrderedJson doc = parse(text);
    CHECK(keys(doc) == document_keys());
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("dry_run") == false);
    CHECK(doc.at("source") == "follows");

    const OrderedJson& follows = doc.at("follows");
    CHECK(keys(follows) == follows_keys());
    CHECK(follows.at("username") == "fixture-user");
    CHECK(follows.at("titles") == 2);
    CHECK(follows.at("statuses").is_null()); // no --status
    CHECK(follows.at("selected") == 2);      // == titles without --status

    const OrderedJson& titles = doc.at("titles");
    REQUIRE(titles.is_array());
    REQUIRE(titles.size() == 2);
    for (std::size_t i = 0; i < titles.size(); ++i) {
        CHECK(titles.at(i).at("line") == i + 1);
    }
    CHECK(titles.at(0).at("report").is_object());
    CHECK(titles.at(1).at("status") == "not_found");
    CHECK(doc.at("summary").at("titles").at("total") == 2);
    CHECK(doc.at("summary").at("titles").at("not_found") == 1);
}

TEST_CASE("batch json follows: --status gives the normalised statuses and the selected count",
          "[batch][json][follows]") {
    const md::FollowsSummary summary{
        .username = "fixture-user",
        .titles = 4,
        .statuses = std::vector<std::string>{"reading", "on_hold"},
        .selected = 1,
    };
    const OrderedJson doc = parse(md::batch_to_json(two_items(), "follows", true, summary));
    CHECK(keys(doc) == document_keys());
    CHECK(doc.at("dry_run") == true);
    const OrderedJson& follows = doc.at("follows");
    CHECK(keys(follows) == follows_keys());
    CHECK(follows.at("titles") == 4);
    CHECK(follows.at("statuses") == OrderedJson::array({"reading", "on_hold"}));
    CHECK(follows.at("selected") == 1);
}

TEST_CASE("batch json follows: an empty --status match is statuses [] distinct from null",
          "[batch][json][follows]") {
    // The summary is the command's to fill; an engaged empty list still prints as an array.
    const md::FollowsSummary summary{
        .username = "u", .titles = 3, .statuses = std::vector<std::string>{}, .selected = 0};
    const OrderedJson doc = parse(md::batch_to_json(md::BatchResult{}, "follows", false, summary));
    CHECK(doc.at("follows").at("statuses") == OrderedJson::array());
    CHECK(doc.at("follows").at("selected") == 0);
}

TEST_CASE("batch json follows: nothing selected prints titles [] plus the follows object",
          "[batch][json][follows]") {
    const md::FollowsSummary summary{.username = "fixture-user",
                                     .titles = 4,
                                     .statuses = std::vector<std::string>{"dropped"},
                                     .selected = 0};
    const std::string text = md::batch_to_json(md::BatchResult{}, "follows", false, summary);
    CHECK(text.contains("\"titles\": []"));
    const OrderedJson doc = parse(text);
    CHECK(keys(doc) == document_keys());
    CHECK(doc.at("titles") == OrderedJson::array());
    CHECK(doc.at("follows").at("titles") == 4);
    CHECK(doc.at("follows").at("selected") == 0);
    CHECK(doc.at("summary").at("titles").at("total") == 0);

    // No follows at all: the counts are 0 and the shape is the same.
    const OrderedJson none =
        parse(md::batch_to_json(md::BatchResult{}, "follows", false, unfiltered(0)));
    CHECK(keys(none.at("follows")) == follows_keys());
    CHECK(none.at("follows").at("titles") == 0);
    CHECK(none.at("follows").at("statuses").is_null());
}

TEST_CASE("batch json follows: ESC and invalid UTF-8 in the username give valid JSON",
          "[batch][json][follows]") {
    md::FollowsSummary summary = unfiltered();
    summary.username = "evil\x1b[2J\x1b]0;t\x07user\xff\xc3!";
    const std::string text = md::batch_to_json(two_items(), "follows", false, summary);

    CHECK(!text.contains('\x1b'));
    CHECK(!text.contains('\x07'));
    CHECK(text.contains("\\u001b[2J"));

    const OrderedJson doc = parse(text); // strict: fails on any invalid UTF-8
    CHECK(doc.at("follows").at("username") ==
          std::format("evil\x1b[2J\x1b]0;t\x07user{}{}!", replacement, replacement));
}

TEST_CASE("batch json follows: the exact bytes of a small follows document",
          "[batch][json][follows]") {
    md::BatchResult result{};
    result.titles = {md::TitleResult{.line = 1,
                                     .manga_id = std::string{second_id},
                                     .dir = "out",
                                     .status = TitleStatus::not_found,
                                     .exit_code = 4,
                                     .error = "not found"}};
    const md::FollowsSummary summary{.username = "fixture-user",
                                     .titles = 3,
                                     .statuses = std::vector<std::string>{"reading"},
                                     .selected = 1};
    const std::string expected = R"({
  "format_version": 1,
  "dry_run": true,
  "source": "follows",
  "follows": {
    "username": "fixture-user",
    "titles": 3,
    "statuses": [
      "reading"
    ],
    "selected": 1
  },
  "titles": [
    {
      "line": 1,
      "manga_id": "66666666-6666-4666-8666-666666666666",
      "dir": "out",
      "status": "not_found",
      "exit_code": 4,
      "error": "not found",
      "duplicate_of": null,
      "report": null
    }
  ],
  "summary": {
    "titles": {
      "total": 1,
      "ok": 0,
      "not_found": 1,
      "no_chapters": 0,
      "incomplete": 0,
      "network": 0,
      "failed": 0,
      "duplicate": 0,
      "not_run": 0
    },
    "files": {
      "planned": 0,
      "written": 0,
      "exists": 0,
      "failed": 0
    }
  }
}
)";
    CHECK(md::batch_to_json(result, "follows", true, summary) == expected);
}

TEST_CASE("batch json follows: batch and mdlist documents are unchanged and carry no follows",
          "[batch][json][follows]") {
    const md::BatchResult result = two_items();
    const std::string batch = md::batch_to_json(result, "titles.csv", true);
    CHECK(md::batch_to_json(result, "titles.csv", true, std::nullopt) == batch);
    CHECK(!parse(batch).contains("follows"));

    const md::ListSummary list{.id = "a0000000-0000-4000-8000-000000000001",
                               .name = std::string{"A list"},
                               .url = "https://mangadex.org/list/"
                                      "a0000000-0000-4000-8000-000000000001"};
    const OrderedJson mdlist = parse(md::batch_to_json(result, "L1", true, list));
    CHECK(!mdlist.contains("follows"));
    CHECK(keys(mdlist) == std::vector<std::string>{"format_version", "dry_run", "source", "list",
                                                   "titles", "summary"});

    // The follows document is exactly the batch document with one more key (and no `list`).
    OrderedJson follows = parse(md::batch_to_json(result, "titles.csv", true, unfiltered()));
    CHECK(!follows.contains("list"));
    REQUIRE(follows.contains("follows"));
    follows.erase("follows");
    CHECK(dump(follows) == batch);
}
