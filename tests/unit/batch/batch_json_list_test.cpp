// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/batch/batch_json_list_test.cpp: the `mdlist --json` document (list plan T5, AC8,
// fact-check V17): batch's document plus the additive `list` object right after `source`, with
// `name` null when absent; untrusted name bytes (ESC, invalid UTF-8) give valid JSON with \u001b
// and U+FFFD; the empty list; and the batch document (the three-argument overload, or nullopt)
// stays byte-identical to today's and never carries `list`. Per the plan-gate decision only C0/ESC
// escaping is asserted (DEL and C1 are out of scope here).
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/report.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace md = mangadex_dl;
using OrderedJson = nlohmann::ordered_json;
using md::TitleStatus;

constexpr std::string_view list_id = "a0000000-0000-4000-8000-000000000001";
constexpr std::string_view list_url =
    "https://mangadex.org/list/a0000000-0000-4000-8000-000000000001";
constexpr std::string_view first_id = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view second_id = "66666666-6666-4666-8666-666666666666";
constexpr std::string_view replacement = "\xEF\xBF\xBD"; // U+FFFD

[[nodiscard]] md::ListSummary summary(std::optional<std::string> name) {
    return md::ListSummary{
        .id = std::string{list_id}, .name = std::move(name), .url = std::string{list_url}};
}

/// Two items, as an mdlist run reports them: line = the list position, a --dir-per-title dir for
/// the first and DIR for the second (never learned its name).
[[nodiscard]] md::BatchResult two_items() {
    md::BatchResult result{};
    md::RunReport report{};
    report.manga.id = std::string{first_id};
    report.manga.title = "Clean Title";
    result.titles = {
        md::TitleResult{.line = 1,
                        .manga_id = std::string{first_id},
                        .dir = "out/Clean Title",
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

/// The keys of one `titles[]` entry, in order (batch's).
[[nodiscard]] std::vector<std::string> title_keys() {
    return {"line", "manga_id", "dir", "status", "exit_code", "error", "duplicate_of", "report"};
}

/// Dumps like batch_to_json does.
[[nodiscard]] std::string dump(const OrderedJson& doc) {
    std::string text = doc.dump(2, ' ', false, OrderedJson::error_handler_t::replace);
    text += '\n';
    return text;
}

} // namespace

TEST_CASE("batch json list: batch's keys plus list{id,name,url} right after source",
          "[batch][json][list]") {
    const std::string text =
        md::batch_to_json(two_items(), "L1", true, summary(std::string{"My list"}));
    REQUIRE(!text.empty());
    CHECK(text.back() == '\n');

    const OrderedJson doc = parse(text);
    CHECK(keys(doc) == std::vector<std::string>{"format_version", "dry_run", "source", "list",
                                                "titles", "summary"});
    CHECK(doc.at("format_version") == 1);
    CHECK(doc.at("dry_run") == true);
    CHECK(doc.at("source") == "L1");

    const OrderedJson& list = doc.at("list");
    CHECK(keys(list) == std::vector<std::string>{"id", "name", "url"});
    CHECK(list.at("id") == list_id);
    CHECK(list.at("name") == "My list");
    CHECK(list.at("url") == list_url);

    const OrderedJson& titles = doc.at("titles");
    REQUIRE(titles.is_array());
    REQUIRE(titles.size() == 2);
    for (std::size_t i = 0; i < titles.size(); ++i) {
        const OrderedJson& row = titles.at(i);
        CHECK(keys(row) == title_keys());
        CHECK(row.at("line") == i + 1); // the list position
    }
    CHECK(titles.at(0).at("dir") == "out/Clean Title");
    CHECK(titles.at(0).at("report").is_object());
    CHECK(titles.at(1).at("dir") == "out");
    CHECK(titles.at(1).at("status") == "not_found");

    CHECK(keys(doc.at("summary")) == std::vector<std::string>{"titles", "files"});
    CHECK(doc.at("summary").at("titles").at("total") == 2);
    CHECK(doc.at("summary").at("titles").at("ok") == 1);
    CHECK(doc.at("summary").at("titles").at("not_found") == 1);
}

TEST_CASE("batch json list: name is null when absent", "[batch][json][list]") {
    const OrderedJson doc =
        parse(md::batch_to_json(two_items(), "L1", false, summary(std::nullopt)));
    const OrderedJson& list = doc.at("list");
    CHECK(keys(list) == std::vector<std::string>{"id", "name", "url"});
    CHECK(list.at("name").is_null());
    CHECK(list.at("id") == list_id);
    CHECK(list.at("url") == list_url);
}

TEST_CASE("batch json list: ESC and invalid UTF-8 in the name give valid JSON (V17)",
          "[batch][json][list]") {
    const std::string name = "Evil \x1b[2J\x1b]0;t\x07 list \xff\xc3 end";
    const std::string text = md::batch_to_json(two_items(), "L1", false, summary(name));

    // C0 controls are escaped: no raw ESC or BEL byte anywhere, the escape is spelled \u001b.
    CHECK(!text.contains('\x1b'));
    CHECK(!text.contains('\x07'));
    CHECK(text.contains("\\u001b[2J"));

    const OrderedJson doc = parse(text); // strict: fails on any invalid UTF-8
    CHECK(doc.at("list").at("name") ==
          std::format("Evil \x1b[2J\x1b]0;t\x07 list {}{} end", replacement, replacement));
}

TEST_CASE("batch json list: the empty list prints titles [] plus the list object",
          "[batch][json][list]") {
    const std::string text =
        md::batch_to_json(md::BatchResult{}, list_url, false, summary(std::nullopt));
    CHECK(text.contains("\"titles\": []"));
    const OrderedJson doc = parse(text);
    CHECK(keys(doc) == std::vector<std::string>{"format_version", "dry_run", "source", "list",
                                                "titles", "summary"});
    CHECK(doc.at("source") == list_url);
    CHECK(doc.at("titles") == OrderedJson::array());
    CHECK(doc.at("list").at("id") == list_id);
    CHECK(doc.at("summary").at("titles").at("total") == 0);
}

TEST_CASE("batch json list: a batch document is byte-identical to today's and has no list",
          "[batch][json][list]") {
    md::BatchResult result{};
    result.titles = {md::TitleResult{.line = 2,
                                     .manga_id = std::string{second_id},
                                     .dir = "out",
                                     .status = TitleStatus::not_found,
                                     .exit_code = 4,
                                     .error = "not found"},
                     md::TitleResult{.line = 3,
                                     .manga_id = std::string{second_id},
                                     .dir = "out",
                                     .status = TitleStatus::duplicate,
                                     .duplicate_of = 2}};
    // What batch_to_json printed for this result before the list overloads existed.
    const std::string today = R"({
  "format_version": 1,
  "dry_run": true,
  "source": "titles.csv",
  "titles": [
    {
      "line": 2,
      "manga_id": "66666666-6666-4666-8666-666666666666",
      "dir": "out",
      "status": "not_found",
      "exit_code": 4,
      "error": "not found",
      "duplicate_of": null,
      "report": null
    },
    {
      "line": 3,
      "manga_id": "66666666-6666-4666-8666-666666666666",
      "dir": "out",
      "status": "duplicate",
      "exit_code": null,
      "error": null,
      "duplicate_of": 2,
      "report": null
    }
  ],
  "summary": {
    "titles": {
      "total": 2,
      "ok": 0,
      "not_found": 1,
      "no_chapters": 0,
      "incomplete": 0,
      "network": 0,
      "failed": 0,
      "duplicate": 1,
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
    const std::string three = md::batch_to_json(result, "titles.csv", true);
    CHECK(three == today);
    CHECK(md::batch_to_json(result, "titles.csv", true, std::nullopt) == today);
    CHECK(!parse(three).contains("list"));

    // The list document is exactly the batch document with one more key.
    OrderedJson with_list = parse(md::batch_to_json(result, "titles.csv", true, summary("x")));
    REQUIRE(with_list.contains("list"));
    with_list.erase("list");
    CHECK(dump(with_list) == today);
}
