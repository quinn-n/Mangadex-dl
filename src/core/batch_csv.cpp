// SPDX-License-Identifier: GPL-3.0-or-later
//
// The batch title list parser (batch plan T2): bytes -> TitleList, per the spec's "CSV format".
//
// Stages:
//   1. encoding: a UTF-16 BOM is refused, a UTF-8 BOM skipped, then the whole input must be
//      well-formed UTF-8 without NUL (one diagnostic, at the line of the first bad byte);
//   2. lexing (RFC 4180 over ','): one record at a time. A lexical error is fatal at the first
//      one, because the stream cannot be resynchronized after it;
//   3. records: the header (when the first field is not a manga id), then each title record.
//      These errors accumulate (max_list_errors messages, `more` counts the rest). A would-be
//      header with a single unknown word gets one message that also says it is not a title,
//      since it may as well be a mistyped first title of a headerless list.
//
// Every cell is an owned std::string from the moment it is lexed; nothing in the result or in
// the messages borrows from `bytes` or `label`. Because the input is validated before lexing,
// the lexer only ever looks at ASCII delimiters: a multi-byte sequence never contains a byte
// below 0x80, and '\0' can serve as the end-of-input sentinel.
#include "mangadex_dl/batch_csv.hpp"

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/text_safety.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl {
namespace {

using std::string_view_literals::operator""sv;

inline constexpr std::string_view utf8_bom = "\xEF\xBB\xBF"sv;
inline constexpr std::string_view utf16le_bom = "\xFF\xFE"sv;
inline constexpr std::string_view utf16be_bom = "\xFE\xFF"sv;

enum class Column {
    manga,
    title,
    dir,
    volumes,
    loose_chapters,
    layout,
    lang,
    group,
    direction,
    note
};

struct ColumnName {
    std::string_view name{}; ///< lowercase; a string literal
    Column column = Column::note;
};

inline constexpr std::array<ColumnName, 10> column_names{{
    {.name = "manga", .column = Column::manga},
    {.name = "title", .column = Column::title},
    {.name = "dir", .column = Column::dir},
    {.name = "volumes", .column = Column::volumes},
    {.name = "loose-chapters", .column = Column::loose_chapters},
    {.name = "layout", .column = Column::layout},
    {.name = "lang", .column = Column::lang},
    {.name = "group", .column = Column::group},
    {.name = "direction", .column = Column::direction},
    {.name = "note", .column = Column::note},
}};

inline constexpr std::string_view known_columns =
    "manga, title, dir, volumes, loose-chapters, layout, lang, group, direction, note";

[[nodiscard]] constexpr bool is_blank(char c) noexcept { return c == ' ' || c == '\t'; }

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// `text` without leading and trailing ASCII spaces and tabs.
[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return std::string{};
    }
    const auto last = text.find_last_not_of(" \t");
    return std::string{text.substr(first, last - first + 1)};
}

/// The 1-based line of the first byte that is NUL or not part of a well-formed UTF-8 sequence.
[[nodiscard]] std::optional<std::size_t> first_invalid_line(std::string_view text) {
    std::size_t line = 1;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char byte = text[pos];
        if (byte == '\0') {
            return line;
        }
        const Utf8Decoded decoded = decode_utf8_at(text, pos);
        if (decoded.length == 0) {
            return line;
        }
        if (byte == '\n') {
            ++line;
        }
        pos += decoded.length;
    }
    return std::nullopt;
}

/// At most max_list_echo_bytes of `text`, cut back to a UTF-8 sequence boundary (the input is
/// already known to be well-formed), with "..." appended when something was cut.
[[nodiscard]] std::string echo(std::string_view text) {
    if (text.size() <= max_list_echo_bytes) {
        return std::string{text};
    }
    std::size_t cut = max_list_echo_bytes;
    while (cut > 0 && detail::is_utf8_continuation(static_cast<unsigned char>(text[cut]))) {
        --cut;
    }
    return std::format("{}...", text.substr(0, cut));
}

/// One lexed record: its physical start line and its fields, unquoted and trimmed.
struct RawRecord {
    std::size_t line = 0;
    std::vector<std::string> fields{}; ///< never empty
};

/// A fatal lexical error. `what` points at a string literal.
struct LexError {
    std::size_t line = 0;
    std::string_view what{};
};

using LexResult = std::expected<std::optional<RawRecord>, LexError>;

/// RFC 4180 over ',', one record per next() call; comments are skipped here, blank records are
/// returned (the caller skips them). Borrows `text`: a Lexer is only ever a local of
/// ListParser::run, and the caller's buffer behind parse_title_list's `bytes` outlives that call.
class Lexer {
public:
    explicit Lexer(std::string_view text) noexcept : text_(text) {}

    /// The next record, nullopt at the end of the input, or the first lexical error.
    [[nodiscard]] LexResult next() {
        skip_comments();
        if (at_end()) {
            return std::nullopt;
        }
        RawRecord record{.line = line_}; // after skip_comments(): the record's own start line
        while (true) {
            skip_blanks();
            auto field = peek() == '"' ? quoted_field(record.line) : unquoted_field(record.line);
            if (!field) {
                return std::unexpected(field.error());
            }
            record.fields.push_back(trimmed(*field));
            auto end = delimiter(record.line);
            if (!end) {
                return std::unexpected(end.error());
            }
            if (*end) {
                return record;
            }
        }
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

    /// The byte `ahead` positions on, or '\0' past the end (the input holds no NUL).
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return text_.size() - pos_ > ahead ? text_[pos_ + ahead] : '\0';
    }

    void skip_blanks() noexcept {
        while (is_blank(peek())) {
            ++pos_;
        }
    }

    /// At a record start: skips every line whose first non-blank byte is '#'.
    void skip_comments() noexcept {
        while (!at_end()) {
            std::size_t probe = pos_;
            while (probe < text_.size() && is_blank(text_[probe])) {
                ++probe;
            }
            if (probe == text_.size() || text_[probe] != '#') {
                return;
            }
            const std::size_t newline = text_.find('\n', probe);
            if (newline == std::string_view::npos) {
                pos_ = text_.size();
                return;
            }
            pos_ = newline + 1;
            ++line_;
        }
    }

    /// Positioned on the opening quote. `""` is a quote; CR, LF and ',' are literal.
    [[nodiscard]] std::expected<std::string, LexError> quoted_field(std::size_t start_line) {
        ++pos_; // the opening quote
        std::string value{};
        while (true) {
            if (at_end()) {
                return std::unexpected(LexError{
                    .line = start_line,
                    .what = "unterminated quote; the field that opens with '\"' on this line never "
                            "closes",
                });
            }
            const char c = text_[pos_];
            if (c == '"') {
                if (peek(1) != '"') {
                    ++pos_; // the closing quote
                    return value;
                }
                value.push_back('"');
                pos_ += 2;
                continue;
            }
            if (c == '\n') {
                ++line_;
            }
            value.push_back(c);
            ++pos_;
        }
    }

    /// Up to the next ',', CR, LF or the end; a '"' inside it is an error.
    [[nodiscard]] std::expected<std::string, LexError> unquoted_field(std::size_t start_line) {
        const std::size_t start = pos_;
        const std::size_t stop = std::min(text_.find_first_of(",\r\n\"", start), text_.size());
        pos_ = stop;
        if (peek() == '"') {
            return std::unexpected(LexError{
                .line = start_line,
                .what = "a '\"' inside an unquoted field; quote the whole field and write '\"\"' "
                        "for a quote",
            });
        }
        return std::string{text_.substr(start, stop - start)};
    }

    /// After a field (and the blanks after a closing quote): true at the end of the record,
    /// false after a ','.
    [[nodiscard]] std::expected<bool, LexError> delimiter(std::size_t start_line) {
        skip_blanks();
        if (at_end()) {
            return true;
        }
        switch (peek()) {
        case ',':
            ++pos_;
            return false;
        case '\n':
            ++pos_;
            ++line_;
            return true;
        case '\r':
            if (peek(1) == '\n') {
                pos_ += 2;
                ++line_;
                return true;
            }
            return std::unexpected(LexError{
                .line = start_line,
                .what = "a carriage return (CR) outside quotes; save the file with LF or CRLF "
                        "line endings",
            });
        default:
            return std::unexpected(LexError{
                .line = start_line,
                .what = "unexpected text after a closing quote; quote the whole field and write "
                        "'\"\"' for a quote",
            });
        }
    }

    std::string_view text_{}; ///< borrowed: see the class comment
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

/// Whether every field from index `from` on is empty (true when there are none).
[[nodiscard]] bool all_empty(const std::vector<std::string>& fields, std::size_t from = 0) {
    return std::ranges::all_of(fields | std::views::drop(static_cast<std::ptrdiff_t>(from)),
                               [](const std::string& field) { return field.empty(); });
}

[[nodiscard]] bool has_long_field(const std::vector<std::string>& fields) {
    return std::ranges::any_of(
        fields, [](const std::string& field) { return field.size() > max_list_field_bytes; });
}

/// The only non-empty field of `fields`, or nullopt when there are none or several. Borrows
/// from `fields`.
[[nodiscard]] std::optional<std::string_view>
lone_field(const std::vector<std::string>& fields MANGADEX_DL_LIFETIMEBOUND) {
    std::optional<std::string_view> lone{};
    for (const std::string& field : fields) {
        if (field.empty()) {
            continue;
        }
        if (lone) {
            return std::nullopt;
        }
        lone = field;
    }
    return lone;
}

[[nodiscard]] std::optional<Column> find_column(std::string_view name) {
    for (const ColumnName& known : column_names) {
        if (std::ranges::equal(name, known.name, {}, ascii_lower)) {
            return known.column;
        }
    }
    return std::nullopt;
}

/// The ListRecord member a column fills; nullptr for `manga` (handled separately) and `note`
/// (dropped). Points into `record`.
[[nodiscard]] std::optional<std::string>* slot_for(ListRecord& record, Column column) noexcept {
    switch (column) {
    case Column::manga:
    case Column::note:
        return nullptr;
    case Column::title:
        return &record.title;
    case Column::dir:
        return &record.dir;
    case Column::volumes:
        return &record.volumes;
    case Column::loose_chapters:
        return &record.loose_chapters;
    case Column::layout:
        return &record.layout;
    case Column::lang:
        return &record.lang;
    case Column::group:
        return &record.group;
    case Column::direction:
        return &record.direction;
    }
    return nullptr; // only reachable through an out-of-range enum value
}

/// Everything after the encoding check. Borrows `label`: a ListParser is only ever a local of
/// parse_title_list, whose `label` parameter outlives it.
class ListParser {
public:
    explicit ListParser(std::string_view label) noexcept : label_(label) {}

    [[nodiscard]] ListResult<TitleList> run(std::string_view text) {
        Lexer lexer{text};
        bool first = true;
        std::size_t titles = 0;
        while (true) {
            auto next = lexer.next();
            if (!next) {
                add_error(next.error().line, next.error().what);
                break;
            }
            if (!next->has_value()) {
                break;
            }
            RawRecord& record = **next;
            if (all_empty(record.fields)) {
                continue;
            }
            if (std::exchange(first, false) && !looks_like_manga_id(record.fields.front())) {
                list_.has_header = true;
                if (!read_header(record)) {
                    break; // without a usable header the records cannot be mapped
                }
                continue;
            }
            if (++titles > max_list_titles) {
                add_message(std::format("{}: lists more than {} titles", label_, max_list_titles));
                break;
            }
            if (list_.has_header) {
                add_header_record(std::move(record));
            } else {
                add_headerless_record(std::move(record));
            }
        }
        if (!errors_.messages.empty() || errors_.more != 0) {
            return std::unexpected(std::move(errors_));
        }
        return std::move(list_);
    }

private:
    void add_message(std::string message) {
        if (errors_.messages.size() < max_list_errors) {
            errors_.messages.push_back(std::move(message));
        } else {
            ++errors_.more;
        }
    }

    void add_error(std::size_t line, std::string_view what) {
        add_message(std::format("{}:{}: {}", label_, line, what));
    }

    /// Fills columns_; false (with the errors recorded) when a name is bad or `manga` is absent.
    [[nodiscard]] bool read_header(const RawRecord& record) {
        // A ';'-separated export arrives as one field holding ';'.
        const bool semicolons = record.fields.size() == 1 && record.fields.front().contains(';');
        const std::string_view hint = semicolons ? "; the separator is ','" : "";
        if (!semicolons) {
            // A lone unknown word is as likely a mistyped title (a chapter URL, a short id) in a
            // headerless list as a header: one message that covers both readings.
            if (const std::optional<std::string_view> lone = lone_field(record.fields);
                lone && !find_column(*lone)) {
                add_error(record.line,
                          std::format("'{}' is not a MangaDex title UUID or "
                                      "https://mangadex.org/title/<uuid> URL, nor a known column "
                                      "({})",
                                      echo(*lone), known_columns));
                return false;
            }
        }
        bool ok = true;
        for (const std::string& name : record.fields) {
            const std::optional<Column> column = find_column(name);
            if (!column) {
                add_error(record.line, std::format("unknown column '{}' (known: {}){}", echo(name),
                                                   known_columns, hint));
                ok = false;
                continue;
            }
            if (std::ranges::contains(columns_, *column)) {
                add_error(record.line, std::format("duplicate column '{}' (known: {})", echo(name),
                                                   known_columns));
                ok = false;
                continue;
            }
            columns_.push_back(*column);
        }
        if (ok && !std::ranges::contains(columns_, Column::manga)) {
            add_error(record.line, "the header has no 'manga' column");
            ok = false;
        }
        return ok;
    }

    void add_header_record(RawRecord record) {
        const std::size_t width = columns_.size();
        if (has_long_field(record.fields)) {
            add_error(record.line,
                      std::format("a field is longer than {} bytes", max_list_field_bytes));
            return;
        }
        if (record.fields.size() > width && !all_empty(record.fields, width)) {
            add_error(record.line,
                      std::format("{} fields but the header has {}", record.fields.size(), width));
            return;
        }
        ListRecord out{.line = record.line};
        const std::size_t cells = std::min(record.fields.size(), width);
        for (std::size_t i = 0; i < cells; ++i) {
            std::string& cell = record.fields[i];
            if (cell.empty()) {
                continue;
            }
            if (columns_[i] == Column::manga) {
                out.manga = std::move(cell);
            } else if (std::optional<std::string>* slot = slot_for(out, columns_[i])) {
                *slot = std::move(cell);
            }
        }
        if (out.manga.empty()) {
            // The record is not blank, so some other cell holds something.
            add_error(record.line, "manga: the title is missing");
            return;
        }
        list_.records.push_back(std::move(out));
    }

    void add_headerless_record(RawRecord record) {
        if (has_long_field(record.fields)) {
            add_error(record.line,
                      std::format("a field is longer than {} bytes", max_list_field_bytes));
            return;
        }
        if (!all_empty(record.fields, 1)) {
            add_error(record.line, "more than one column; add a header row (manga,title,...)");
            return;
        }
        // Not blank and every extra field is empty, so field 1 holds the title.
        list_.records.push_back(
            ListRecord{.line = record.line, .manga = std::move(record.fields.front())});
    }

    std::string_view label_{}; ///< borrowed for run(); copied into every message
    std::vector<Column> columns_{};
    TitleList list_{};
    ListErrors errors_{};
};

[[nodiscard]] ListErrors single_error(std::string message) {
    ListErrors errors{};
    errors.messages.push_back(std::move(message));
    return errors;
}

} // namespace

ListResult<TitleList> parse_title_list(std::string_view bytes, std::string_view label) {
    if (bytes.starts_with(utf16le_bom) || bytes.starts_with(utf16be_bom)) {
        return std::unexpected(single_error(std::format("{}: is UTF-16; save it as UTF-8", label)));
    }
    std::string_view text = bytes;
    if (text.starts_with(utf8_bom)) {
        text.remove_prefix(utf8_bom.size());
    }
    if (const std::optional<std::size_t> line = first_invalid_line(text)) {
        return std::unexpected(single_error(std::format("{}:{}: not valid UTF-8", label, *line)));
    }
    ListParser parser{label};
    return parser.run(text);
}

} // namespace mangadex_dl
