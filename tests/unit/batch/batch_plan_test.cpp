// tests/unit/batch/batch_plan_test.cpp: `batch` phase 1 after parsing (batch plan T3a):
// validate_batch_args, the per-row merge and its column-attributed diagnostics (AC4, AC5), the
// `dir` containment rules (validate_list_dir, join_list_dir) and the dedupe (AC6). Records are
// built by hand, never through the CSV parser.
#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/batch.hpp"
#include "mangadex_dl/batch_csv.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/selection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace md = mangadex_dl;

namespace {

constexpr std::string_view uuid_a = "a1b2c3d4-e5f6-4789-8abc-def012345678";
constexpr std::string_view uuid_b = "11111111-2222-4333-8444-555555555555";

constexpr std::string_view manga_text =
    "manga: must be a MangaDex title UUID or a https://mangadex.org/title/<uuid> URL";
constexpr std::string_view unsafe_text =
    "must not contain invisible, bidirectional or terminal control characters";

/// The usage-error message of `result`, or "<accepted>" when it holds a value.
template <class T> [[nodiscard]] std::string usage_message(const md::Result<T>& result) {
    if (result.has_value()) {
        return "<accepted>";
    }
    CHECK(result.error().code == md::Errc::usage);
    return result.error().message;
}

[[nodiscard]] std::string dir_error(std::string_view cell) {
    return usage_message(md::validate_list_dir(cell));
}

[[nodiscard]] md::RawBatchArgs batch_args(std::string file = "list.csv", std::string dir = "out") {
    md::RawBatchArgs raw{.file = std::move(file)};
    raw.defaults.dir = std::move(dir);
    return raw;
}

[[nodiscard]] md::BatchSettings settings_for(const md::RawBatchArgs& raw) {
    md::Result<md::BatchSettings> settings = md::validate_batch_args(raw);
    REQUIRE(settings.has_value());
    return std::move(*settings);
}

[[nodiscard]] md::BatchSettings default_settings() { return settings_for(batch_args()); }

[[nodiscard]] md::ListRecord record(std::size_t line, std::string_view manga = uuid_a) {
    return md::ListRecord{.line = line, .manga = std::string{manga}};
}

[[nodiscard]] md::TitleList list_of(std::vector<md::ListRecord> records) {
    return md::TitleList{.has_header = true, .records = std::move(records)};
}

[[nodiscard]] std::vector<md::BatchJob> plan_ok(const md::TitleList& list,
                                                const md::BatchSettings& settings) {
    md::ListResult<std::vector<md::BatchJob>> jobs = md::plan_batch(list, settings);
    if (!jobs) {
        for (const std::string& message : jobs.error().messages) {
            UNSCOPED_INFO(message);
        }
    }
    REQUIRE(jobs.has_value());
    return std::move(*jobs);
}

[[nodiscard]] md::BatchJob plan_one(const md::ListRecord& row,
                                    const md::BatchSettings& settings = default_settings()) {
    std::vector<md::BatchJob> jobs = plan_ok(list_of({row}), settings);
    REQUIRE(jobs.size() == 1);
    return std::move(jobs.front());
}

[[nodiscard]] md::ListErrors plan_errors(const md::TitleList& list,
                                         const md::BatchSettings& settings = default_settings()) {
    md::ListResult<std::vector<md::BatchJob>> jobs = md::plan_batch(list, settings);
    // REQUIRE(!x) rather than REQUIRE_FALSE: clang-analyzer flags Catch2's Flags | Flags cast
    // (optin.core.EnumCastOutOfRange) on paths through this helper (as in mangadex_api_test).
    REQUIRE(!jobs.has_value());
    return std::move(jobs).error();
}

/// The single message for a one-record list.
[[nodiscard]] std::string row_error(const md::ListRecord& row,
                                    const md::BatchSettings& settings = default_settings()) {
    const md::ListErrors errors = plan_errors(list_of({row}), settings);
    REQUIRE(errors.messages.size() == 1);
    CHECK(errors.more == 0);
    return errors.messages.front();
}

[[nodiscard]] md::Selection selection(std::string_view spec) {
    md::Result<md::Selection> parsed = md::Selection::parse(spec);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// `cp` as UTF-8, built at run time: bidi controls must not appear in a string literal, not even
/// as hex escapes (GCC's -Wbidi-chars=any and clang-tidy's misc-misleading-bidirectional).
[[nodiscard]] std::string utf8(std::uint32_t cp) {
    const auto byte = [](std::uint32_t bits) { return static_cast<char>(bits); };
    std::string out{};
    if (cp < 0x80U) {
        out += byte(cp);
    } else if (cp < 0x800U) {
        out += byte(0xC0U | (cp >> 6U));
        out += byte(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
        out += byte(0xE0U | (cp >> 12U));
        out += byte(0x80U | ((cp >> 6U) & 0x3FU));
        out += byte(0x80U | (cp & 0x3FU));
    } else {
        out += byte(0xF0U | (cp >> 18U));
        out += byte(0x80U | ((cp >> 12U) & 0x3FU));
        out += byte(0x80U | ((cp >> 6U) & 0x3FU));
        out += byte(0x80U | (cp & 0x3FU));
    }
    return out;
}

/// Selection::parse's own message for a bad SPEC (the column message must equal it).
[[nodiscard]] std::string selection_error(std::string_view spec) {
    const md::Result<md::Selection> parsed = md::Selection::parse(spec);
    REQUIRE(!parsed.has_value()); // not REQUIRE_FALSE: see plan_errors

    return parsed.error().message;
}

/// A heap copy of `text` with no terminator, so ASan reports any read one past the view's end.
class ExactBuffer {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): exact-size alloc
    using Bytes = char[];

public:
    explicit ExactBuffer(std::string_view text)
        : size_(text.size()), bytes_(std::make_unique<Bytes>(text.size())) {
        std::ranges::copy(text, bytes_.get());
    }

    [[nodiscard]] std::string_view view() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return {bytes_.get(), size_};
    }

private:
    std::size_t size_ = 0;
    std::unique_ptr<Bytes> bytes_{};
};

/// `count` segments of `width` bytes each, joined by '/'.
[[nodiscard]] std::string segments(std::size_t count, std::size_t width = 1) {
    std::string out{};
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out += '/';
        }
        out += std::string(width, 'a');
    }
    return out;
}

} // namespace

// --- validate_batch_args --------------------------------------------------------------------

TEST_CASE("validate_batch_args keeps FILE, DIR and the CLI-level options", "[batch][plan]") {
    md::RawBatchArgs raw = batch_args("titles.csv", "Manga");
    raw.defaults.lang = "ja";
    raw.defaults.groups = {"G"};
    raw.fail_fast = true;
    const md::BatchSettings settings = settings_for(raw);
    CHECK(settings.file == "titles.csv");
    CHECK(settings.label == "titles.csv");
    CHECK(settings.defaults.dir == "Manga");
    CHECK(settings.defaults.lang == "ja");
    CHECK(settings.fail_fast);
    CHECK(settings.base.manga == md::MangaId{});
    CHECK(settings.base.dir == std::filesystem::path{"Manga"});
    CHECK(settings.base.lang == "ja");
    CHECK(settings.base.groups == std::vector<std::string>{"G"});
}

TEST_CASE("validate_batch_args labels stdin as <stdin>", "[batch][plan]") {
    const md::BatchSettings settings = settings_for(batch_args("-"));
    CHECK(settings.file == "-");
    CHECK(settings.label == "<stdin>");
}

TEST_CASE("validate_batch_args rejects FILE and DIR mistakes with exact messages",
          "[batch][plan]") {
    const auto message = [](const md::RawBatchArgs& raw) {
        return usage_message(md::validate_batch_args(raw));
    };
    CHECK(message(batch_args("")) == "FILE must not be empty");
    CHECK(message(batch_args("", "")) == "FILE must not be empty"); // FILE is checked first
    CHECK(message(batch_args(std::string{uuid_a})) ==
          "FILE looks like a MangaDex title; use 'mangadex-dl download <title>' for one title");
    CHECK(message(batch_args(std::format("https://mangadex.org/title/{}", uuid_a))) ==
          "FILE looks like a MangaDex title; use 'mangadex-dl download <title>' for one title");
    CHECK(message(batch_args("list.csv", "")) == "DIR must not be empty");
    CHECK(message(batch_args("list.csv", std::string{uuid_b})) ==
          "DIR looks like a MangaDex title; list the titles in FILE");
}

TEST_CASE("validate_batch_args validates the CLI-level options as download does", "[batch][plan]") {
    const auto message = [](const auto& tweak) {
        md::RawBatchArgs raw = batch_args();
        tweak(raw.defaults);
        return usage_message(md::validate_batch_args(raw));
    };
    CHECK(message([](md::RawDownloadArgs& d) { d.lang = "bogus"; }) ==
          "--lang must be a lowercase language code such as en, pt-br or es-la");
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

TEST_CASE("A bad CLI-level --lang is rejected even when every row sets lang", "[batch][plan]") {
    md::RawBatchArgs raw = batch_args();
    raw.defaults.lang = "bogus";
    // The rows never reach plan_batch: the CLI-level check fails first.
    CHECK(usage_message(md::validate_batch_args(raw)) ==
          "--lang must be a lowercase language code such as en, pt-br or es-la");
}

// --- validate_list_dir / join_list_dir --------------------------------------------------------

TEST_CASE("validate_list_dir accepts relative paths inside DIR", "[batch][plan][dir]") {
    const std::string accepted = GENERATE(as<std::string>{}, "a", "a/b", "Zoë/x", "Series 1/Vol",
                                          "a.b/..c/c..", ".hidden/x", "a b", "日本/語");
    CAPTURE(accepted);
    const md::Result<std::string> result = md::validate_list_dir(accepted);
    REQUIRE(result.has_value());
    CHECK(*result == accepted);
}

TEST_CASE("validate_list_dir accepts the limits exactly", "[batch][plan][dir]") {
    const std::string long_segment(255, 'x');
    CHECK(md::validate_list_dir(long_segment) == long_segment);
    CHECK(md::validate_list_dir(segments(32)) == segments(32));
    // 255 * 3 + 254 + 1 bytes in 5 segments, plus 4 slashes = 1024 bytes exactly.
    const std::string exact = std::format("{}/{}/a", segments(3, 255), std::string(254, 'b'));
    REQUIRE(exact.size() == 1024);
    CHECK(md::validate_list_dir(exact) == exact);
}

TEST_CASE("The test's utf8() helper encodes like the decoder decodes", "[batch][plan][dir]") {
    CHECK(utf8(0x41) == "A");
    CHECK(utf8(0xEB) == "\xc3\xab");
    CHECK(utf8(0x3042) == "\xe3\x81\x82");
    CHECK(utf8(0x1F600) == "\xf0\x9f\x98\x80");
}

TEST_CASE("validate_list_dir trims ASCII spaces and tabs", "[batch][plan][dir]") {
    CHECK(md::validate_list_dir(" a ") == "a");
    CHECK(md::validate_list_dir("\t a/b \t") == "a/b");
}

TEST_CASE("validate_list_dir rejects escapes and unsafe text with exact messages",
          "[batch][plan][dir]") {
    using namespace std::string_view_literals;
    constexpr std::string_view controls = "must not contain control characters";
    constexpr std::string_view relative = "must be a relative path inside DIR";
    constexpr std::string_view empty_segments =
        "must not contain empty segments ('//' or a trailing '/')";
    constexpr std::string_view dots = "must not contain '.' or '..' segments";

    CHECK(dir_error("") == "must not be empty");
    CHECK(dir_error(" \t ") == "must not be empty");
    CHECK(dir_error("/abs") == relative);
    CHECK(dir_error("/") == relative);
    CHECK(dir_error("../x") == dots);
    CHECK(dir_error("a/..") == dots);
    CHECK(dir_error(".") == dots);
    CHECK(dir_error("..") == dots);
    CHECK(dir_error("a/./b") == dots);
    CHECK(dir_error("a//b") == empty_segments);
    CHECK(dir_error("a/") == empty_segments);
    CHECK(dir_error("a\\b") == "must not contain '\\'");
    CHECK(dir_error("..\\x") == "must not contain '\\'");
    CHECK(dir_error(std::string(256, 'x')) == "a segment is longer than 255 bytes");
    CHECK(dir_error(segments(33)) == "has more than 32 levels");
    const std::string total_1025 = std::format("{}/{}/a", segments(3, 255), std::string(255, 'b'));
    REQUIRE(total_1025.size() == 1025);
    CHECK(dir_error(total_1025) == "is longer than 1024 bytes");

    CHECK(dir_error("a\x01"
                    "b") == controls);
    CHECK(dir_error("a\nb") == controls);
    CHECK(dir_error("a\rb") == controls);
    CHECK(dir_error("a\x7f") == controls);
    CHECK(dir_error("a\0b"sv) == controls); // an embedded NUL
    CHECK(dir_error("\x1b[31m") == controls);

    CHECK(dir_error("a\xff") == "must be valid UTF-8");
    CHECK(dir_error("\xc0\xaf") == "must be valid UTF-8");     // overlong '/'
    CHECK(dir_error("\xed\xa0\x80") == "must be valid UTF-8"); // a surrogate

    // Unsafe code points, encoded at run time (see utf8()).
    const std::uint32_t unsafe =
        GENERATE(0x9BU, 0x85U, 0x80U, 0x9FU, 0x202EU, 0x202AU, 0x2028U, 0x2029U, 0xFEFFU, 0x2066U,
                 0x2069U, 0x200BU, 0x200EU, 0x061CU, 0x2060U);
    CAPTURE(unsafe);
    CHECK(dir_error(std::format("a{}b", utf8(unsafe))) == unsafe_text);
    CHECK(dir_error(std::format("{}a", utf8(unsafe))) == unsafe_text);
    CHECK(dir_error(std::format("x/y{}", utf8(unsafe))) == unsafe_text);
}

TEST_CASE("validate_list_dir applies its rules in the documented order", "[batch][plan][dir]") {
    // control > UTF-8 > unsafe code point > '\' > absolute > empty segments > dots > depth >
    // segment size > total size.
    CHECK(dir_error("\xff\x01") == "must not contain control characters");
    CHECK(dir_error(utf8(0x202E) + "\xff") == "must be valid UTF-8");
    CHECK(dir_error("/" + utf8(0x202E)) == unsafe_text);
    CHECK(dir_error("/a\\b") == "must not contain '\\'");
    CHECK(dir_error("/a//b") == "must be a relative path inside DIR");
    CHECK(dir_error("../a//") == "must not contain empty segments ('//' or a trailing '/')");
    CHECK(dir_error(segments(40) + "/..") == "must not contain '.' or '..' segments");
    CHECK(dir_error(segments(33, 300)) == "has more than 32 levels");
    CHECK(dir_error(segments(5, 256)) == "a segment is longer than 255 bytes");
}

TEST_CASE("validate_list_dir never reads past the end of its input", "[batch][plan][dir]") {
    // Truncated multi-byte sequences at the very end of an exact-size heap buffer.
    const std::string truncated =
        GENERATE(as<std::string>{}, "a\xe2", "a\xe2\x80", "a\xf0\x9f\x98", "a\xc2");
    CAPTURE(truncated);
    const ExactBuffer buffer{truncated};
    CHECK(dir_error(buffer.view()) == "must be valid UTF-8");
    const ExactBuffer ok{"Zo\xc3\xab"};
    CHECK(md::validate_list_dir(ok.view()) == "Zoë");
    const std::string emoji = "x" + utf8(0x1F600); // a 4-byte sequence ending the buffer
    const ExactBuffer four{emoji};
    CHECK(md::validate_list_dir(four.view()) == emoji);
}

TEST_CASE("join_list_dir joins DIR and the cell without doubling '/'", "[batch][plan][dir]") {
    CHECK(md::join_list_dir("out", "x") == "out/x");
    CHECK(md::join_list_dir("out/", "x") == "out/x");
    CHECK(md::join_list_dir(".", "x") == "./x");
    CHECK(md::join_list_dir("/abs/out", "a/b") == "/abs/out/a/b");
    CHECK(md::join_list_dir("out", "") == "out");
    CHECK(md::join_list_dir("", "x") == "x"); // never "/x"
}

// --- plan_batch: the merge (AC4) --------------------------------------------------------------

TEST_CASE("plan_batch inherits every CLI value when the row has no cells", "[batch][plan]") {
    md::RawBatchArgs raw = batch_args();
    raw.defaults.lang = "ja";
    raw.defaults.groups = {"X", "Y"};
    raw.defaults.layout = "chapters";
    raw.defaults.overwrite = true;
    raw.defaults.jobs = 2;
    const md::BatchSettings settings = settings_for(raw);
    md::ListRecord row = record(3);
    row.lang = std::string{}; // an empty cell inherits, like an absent one
    const md::BatchJob job = plan_one(row, settings);
    CHECK(job.line == 3);
    CHECK(job.options.manga.str() == uuid_a);
    CHECK(job.raw.manga == uuid_a);
    CHECK(job.options.lang == "ja");
    CHECK(job.options.groups == std::vector<std::string>{"X", "Y"});
    CHECK(job.options.layout == md::Layout::chapters);
    CHECK(job.options.overwrite);
    CHECK(job.options.jobs == 2U);
    CHECK(job.dir == "out");
    CHECK(job.options.dir == std::filesystem::path{"out"});
    CHECK(job.sub_dir.empty());
    CHECK_FALSE(job.duplicate_of.has_value());
    CHECK_FALSE(job.options.title.has_value());
    // Apart from the manga, the options are exactly the CLI-level ones.
    md::DownloadOptions expected = settings.base;
    expected.manga = job.options.manga;
    CHECK(job.options == expected);
}

TEST_CASE("plan_batch lets a non-empty cell override the CLI value", "[batch][plan]") {
    md::ListRecord row =
        record(2, "HTTPS://mangadex.org/title/A1B2C3D4-E5F6-4789-8ABC-DEF012345678/slug");
    row.title = "My Title";
    row.dir = "series/one";
    row.volumes = "1-3";
    row.loose_chapters = "none";
    row.layout = "single";
    row.lang = "pt-br";
    row.group = "A | B ";
    row.direction = "rtl";
    md::RawBatchArgs raw = batch_args("list.csv", "out/");
    raw.defaults.groups = {"CLI"};
    const md::BatchJob job = plan_one(row, settings_for(raw));
    CHECK(job.options.manga.str() == uuid_a);
    CHECK(job.raw.manga == uuid_a); // canonical, not the cell as typed
    CHECK(job.options.title == "My Title");
    CHECK(job.options.volumes == selection("1-3"));
    CHECK(job.options.loose_chapters.is_none());
    CHECK(job.options.layout == md::Layout::single);
    CHECK(job.options.lang == "pt-br");
    CHECK(job.options.groups == std::vector<std::string>{"A", "B"}); // replaces, never appends
    CHECK(job.options.direction == md::DirectionChoice::rtl);
    CHECK(job.dir == "out/series/one");
    CHECK(job.options.dir == std::filesystem::path{"out/series/one"});
    CHECK(job.sub_dir == "series/one");
    CHECK(job.raw.dir == "out/series/one");
    CHECK(job.raw.volumes == "1-3");
    CHECK(job.raw.lang == "pt-br");
    CHECK(job.raw.groups == std::vector<std::string>{"A", "B"});
}

TEST_CASE("plan_batch trims the dir cell and joins it under DIR", "[batch][plan][dir]") {
    md::ListRecord row = record(2);
    row.dir = " a ";
    const md::BatchJob job = plan_one(row);
    CHECK(job.sub_dir == "a");
    CHECK(job.dir == "out/a");

    md::ListRecord dotted = record(2);
    dotted.dir = "x";
    CHECK(plan_one(dotted, settings_for(batch_args("list.csv", "."))).dir == "./x");
}

TEST_CASE("plan_batch keeps '&' and '#' in group names and splits only on '|'",
          "[batch][plan][group]") {
    md::ListRecord row = record(2);
    row.group = "A&B|C#D";
    CHECK(plan_one(row).options.groups == std::vector<std::string>{"A&B", "C#D"});
    row.group = "  Only One\t";
    CHECK(plan_one(row).options.groups == std::vector<std::string>{"Only One"});
}

TEST_CASE("plan_batch rejects an empty group name or a control character in one",
          "[batch][plan][group]") {
    const auto message = [](std::string group) {
        md::ListRecord row = record(4);
        row.group = std::move(group);
        return row_error(row);
    };
    CHECK(message("A||B") == "list.csv:4: group: empty name in the list");
    CHECK(message("A| |B") == "list.csv:4: group: empty name in the list");
    CHECK(message("A|") == "list.csv:4: group: empty name in the list");
    CHECK(message("|") == "list.csv:4: group: empty name in the list");
    CHECK(message("A\nB") == "list.csv:4: group: must not contain control characters");
    CHECK(message("A|B\x7f") == "list.csv:4: group: must not contain control characters");
}

TEST_CASE("plan_batch reports selections that are both none after the merge", "[batch][plan]") {
    md::RawBatchArgs raw = batch_args();
    raw.defaults.volumes = "none";
    md::ListRecord row = record(5);
    row.loose_chapters = "none";
    CHECK(row_error(row, settings_for(raw)) ==
          "list.csv:5: --volumes and --loose-chapters are both none: nothing to download");

    md::ListRecord both = record(6);
    both.volumes = "none";
    both.loose_chapters = "none";
    CHECK(row_error(both) ==
          "list.csv:6: --volumes and --loose-chapters are both none: nothing to download");
}

// --- plan_batch: diagnostics (AC5) ------------------------------------------------------------

TEST_CASE("plan_batch prints <label>:<line>: <column>: <message> without the option prefix",
          "[batch][plan][diagnostics]") {
    const auto cell_error = [](const auto& set) {
        md::ListRecord row = record(7);
        set(row);
        return row_error(row);
    };
    CHECK(cell_error([](md::ListRecord& r) { r.lang = "EN"; }) ==
          "list.csv:7: lang: must be a lowercase language code such as en, pt-br or es-la");
    CHECK(cell_error([](md::ListRecord& r) { r.layout = "pdf"; }) ==
          "list.csv:7: layout: must be volumes, chapters or single");
    CHECK(cell_error([](md::ListRecord& r) { r.direction = "up"; }) ==
          "list.csv:7: direction: must be auto, ltr or rtl");
    CHECK(cell_error([](md::ListRecord& r) { r.volumes = "x"; }) ==
          "list.csv:7: volumes: " + selection_error("x"));
    CHECK(cell_error([](md::ListRecord& r) { r.loose_chapters = "1,,2"; }) ==
          "list.csv:7: loose-chapters: " + selection_error("1,,2"));
    CHECK(cell_error([](md::ListRecord& r) { r.title = std::string(1025, 't'); }) ==
          "list.csv:7: title: is longer than 1024 bytes");
    CHECK(cell_error([](md::ListRecord& r) { r.title = "a\x01"; }) ==
          "list.csv:7: title: must not contain control characters");
    CHECK(cell_error([](md::ListRecord& r) { r.title = "  \t"; }) ==
          "list.csv:7: title: must not be empty");
    CHECK(cell_error([](md::ListRecord& r) { r.dir = "../x"; }) ==
          "list.csv:7: dir: must not contain '.' or '..' segments");
    CHECK(cell_error([](md::ListRecord& r) { r.dir = "/abs"; }) ==
          "list.csv:7: dir: must be a relative path inside DIR");
    CHECK(cell_error([](md::ListRecord& r) { r.dir = "a" + utf8(0x202E); }) ==
          std::format("list.csv:7: dir: {}", unsafe_text));
}

TEST_CASE("plan_batch never echoes a bad manga cell", "[batch][plan][diagnostics]") {
    const std::string cell = GENERATE(as<std::string>{}, "SECRET-not-a-title",
                                      "https://evil.example/title/SECRET", "\x1b[31mSECRET");
    CAPTURE(cell);
    const std::string message = row_error(record(4, cell));
    CHECK(message == std::format("list.csv:4: {}", manga_text));
    CHECK_FALSE(message.contains("SECRET"));
}

TEST_CASE("plan_batch labels stdin rows as <stdin>", "[batch][plan][diagnostics]") {
    md::ListRecord row = record(1);
    row.lang = "x";
    CHECK(row_error(row, settings_for(batch_args("-"))) ==
          "<stdin>:1: lang: must be a lowercase language code such as en, pt-br or es-la");
}

TEST_CASE("plan_batch reports every bad cell of a row, in column order",
          "[batch][plan][diagnostics]") {
    md::ListRecord row = record(9, "nope");
    row.dir = "..";
    row.lang = "EN";
    row.group = "A||B";
    const md::ListErrors errors = plan_errors(list_of({row}));
    CHECK(errors.messages ==
          std::vector<std::string>{
              std::format("list.csv:9: {}", manga_text),
              "list.csv:9: dir: must not contain '.' or '..' segments",
              "list.csv:9: lang: must be a lowercase language code such as en, pt-br or es-la",
              "list.csv:9: group: empty name in the list",
          });
    CHECK(errors.more == 0);
}

TEST_CASE("plan_batch accumulates errors across records", "[batch][plan][diagnostics]") {
    md::ListRecord bad_lang = record(2);
    bad_lang.lang = "EN";
    md::ListRecord bad_layout = record(5, uuid_b);
    bad_layout.layout = "x";
    const md::ListErrors errors = plan_errors(list_of({bad_lang, record(3), bad_layout}));
    CHECK(errors.messages ==
          std::vector<std::string>{
              "list.csv:2: lang: must be a lowercase language code such as en, pt-br or es-la",
              "list.csv:5: layout: must be volumes, chapters or single",
          });
    CHECK(errors.more == 0);
}

TEST_CASE("plan_batch caps the messages at 20 and counts the rest", "[batch][plan][diagnostics]") {
    std::vector<md::ListRecord> rows{};
    for (std::size_t line = 1; line <= 25; ++line) {
        rows.push_back(record(line, "bad"));
    }
    const md::ListErrors errors = plan_errors(list_of(std::move(rows)));
    REQUIRE(errors.messages.size() == md::max_list_errors);
    CHECK(errors.more == 5);
    CHECK(errors.messages.front() == std::format("list.csv:1: {}", manga_text));
    CHECK(errors.messages.back() == std::format("list.csv:20: {}", manga_text));
}

TEST_CASE("plan_batch returns no jobs for an empty list", "[batch][plan]") {
    CHECK(plan_ok(md::TitleList{}, default_settings()).empty());
}

// --- plan_batch: dedupe (AC6) -----------------------------------------------------------------

TEST_CASE("plan_batch marks rows identical to an earlier row as duplicates", "[batch][plan][dup]") {
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({record(2), record(3, uuid_b), record(4), record(6)}), default_settings());
    REQUIRE(jobs.size() == 4);
    CHECK_FALSE(jobs[0].duplicate_of.has_value());
    CHECK_FALSE(jobs[1].duplicate_of.has_value());
    CHECK(jobs[2].duplicate_of == std::optional<std::size_t>{2});
    CHECK(jobs[3].duplicate_of == std::optional<std::size_t>{2}); // the first, not the previous
}

TEST_CASE("plan_batch compares the canonical title id", "[batch][plan][dup]") {
    const std::string url = std::format("https://mangadex.org/title/{}/some-slug", uuid_a);
    const std::string upper = "A1B2C3D4-E5F6-4789-8ABC-DEF012345678";
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({record(1), record(2, url), record(3, upper)}), default_settings());
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[1].duplicate_of == std::optional<std::size_t>{1});
    CHECK(jobs[2].duplicate_of == std::optional<std::size_t>{1});
}

TEST_CASE("plan_batch runs rows that differ only in lang or only in dir", "[batch][plan][dup]") {
    md::ListRecord other_lang = record(3);
    other_lang.lang = "fr";
    md::ListRecord other_dir = record(4);
    other_dir.dir = "elsewhere";
    md::ListRecord same_as_cli = record(5);
    same_as_cli.lang = "en"; // equal to the CLI default after the merge
    const std::vector<md::BatchJob> jobs =
        plan_ok(list_of({record(2), other_lang, other_dir, same_as_cli}), default_settings());
    REQUIRE(jobs.size() == 4);
    CHECK_FALSE(jobs[1].duplicate_of.has_value());
    CHECK_FALSE(jobs[2].duplicate_of.has_value());
    CHECK(jobs[3].duplicate_of == std::optional<std::size_t>{2}); // effective options compared
}
