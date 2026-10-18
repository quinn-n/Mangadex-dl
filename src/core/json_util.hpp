// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private to the core library: safe parsing of untrusted JSON (network responses) and non-throwing,
// typed field access. Used by src/core/mangadex_api.cpp.
//
// Why the pre-scan: nlohmann::json parses and destroys iteratively, but dump(), copying and ==
// recurse, so a deeply nested document could overflow the stack later (fact-check V-E3: ASan stack
// overflow at depth 1e6). parse_untrusted() therefore rejects any document nested deeper than the
// limit before nlohmann ever sees it. Never dump/copy/compare a document that did not come from
// parse_untrusted().
#pragma once

#include "mangadex_dl/error.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl::json_util {

using Json = nlohmann::json;

/// True when no array/object in `text` is nested deeper than `max_depth` levels (a top-level `{}`
/// is depth 1). Brackets inside JSON strings (including escaped quotes) are not counted. The scan
/// is linear, allocation-free and never recurses; it does not validate the JSON (the parser does).
[[nodiscard]] bool depth_ok(std::string_view text, std::size_t max_depth) noexcept;

/// Parses one untrusted JSON document: the size cap (max_api_json_bytes), the depth pre-scan
/// (max_json_depth), then nlohmann's non-throwing parser (strict: no comments, no trailing data,
/// valid UTF-8 only). Every failure is Errc::network; `what` names the document in the message
/// (a string literal, never response data).
[[nodiscard]] Result<Json> parse_untrusted(std::string_view text, std::string_view what);

/// Whether a field must be present.
enum class Need {
    required,
    optional,
};

/// Typed, non-throwing access to the members of a parsed document. Every read goes through
/// get_ptr, so a type mismatch never throws.
///
/// The first error is sticky (like a stream's failbit): once a read fails, ok() is false, error()
/// describes the first failure, and later reads return empty results. Callers read every field
/// they need, then check ok() once before using the values.
///
/// Rules: an optional field may be absent or null (both read as "not present"); a present,
/// non-null value of the wrong type is an error, as is an absent or null required field. Unknown
/// members are ignored. Error messages contain only key names chosen by the caller (string
/// literals), never response content.
class FieldReader {
public:
    /// `what` names the document in error messages, e.g. "feed page".
    explicit FieldReader(std::string_view what) : what_(what) {}

    /// `value` as an object, or nullptr (an error) when it is anything else.
    [[nodiscard]] const Json::object_t* as_object(const Json& value, std::string_view name);

    [[nodiscard]] const Json::object_t* object(const Json::object_t& parent, std::string_view key,
                                               Need need);
    [[nodiscard]] const Json::array_t* array(const Json::object_t& parent, std::string_view key,
                                             Need need);
    [[nodiscard]] std::optional<std::string> string(const Json::object_t& parent,
                                                    std::string_view key, Need need);
    [[nodiscard]] std::optional<bool> boolean(const Json::object_t& parent, std::string_view key,
                                              Need need);
    /// A non-negative integer (a float such as 3.0, or a negative number, is the wrong type).
    [[nodiscard]] std::optional<std::uint64_t> unsigned_integer(const Json::object_t& parent,
                                                                std::string_view key, Need need);
    /// Records a semantic error (`detail` is a fixed message, never response content).
    void fail(std::string_view detail);

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    /// The first error, as Errc::network. Only meaningful when !ok().
    [[nodiscard]] Error error() const;

private:
    /// The member `key`, or nullptr when it is absent or null (an error when `need` is required).
    [[nodiscard]] const Json* find(const Json::object_t& parent, std::string_view key, Need need);
    void wrong_type(std::string_view key);

    std::string what_{};
    std::optional<std::string> error_{};
};

} // namespace mangadex_dl::json_util
