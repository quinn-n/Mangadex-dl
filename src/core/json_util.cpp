// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_util.hpp"

#include "mangadex_dl/error.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl::json_util {
namespace {

inline constexpr std::size_t mebibyte = std::size_t{1024} * 1024;

/// The document as named in a message: the API wording for Errc::network (unchanged from before
/// ParseLimits existed), the bare `what` for any other category.
[[nodiscard]] std::string subject(Errc code, std::string_view what) {
    if (code == Errc::network) {
        return std::format("MangaDex API response ({})", what);
    }
    return std::string{what};
}

[[nodiscard]] Error malformed(Errc code, std::string_view what, std::string_view detail) {
    return Error{.code = code,
                 .message = std::format("malformed {}: {}", subject(code, what), detail)};
}

[[nodiscard]] std::string size_text(std::size_t bytes) {
    if (bytes % mebibyte == 0) {
        return std::format("{} MiB", bytes / mebibyte);
    }
    return std::format("{} bytes", bytes);
}

enum class Scan {
    ok,
    too_deep,
    too_many_separators,
};

/// One linear, allocation-free pass over `text`: nesting depth and structural separators
/// (`,` `:` `[` `{`) outside strings. `max_separators == 0` disables the separator bound. Stops at
/// the first violation.
[[nodiscard]] Scan prescan(std::string_view text, std::size_t max_depth,
                           std::size_t max_separators) noexcept {
    std::size_t depth = 0;
    std::size_t separators = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        switch (c) {
        case '"':
            in_string = true;
            break;
        case '[':
        case '{':
            ++depth;
            if (depth > max_depth) {
                return Scan::too_deep;
            }
            ++separators;
            break;
        case ',':
        case ':':
            ++separators;
            break;
        case ']':
        case '}':
            // A closer at depth 0 makes the document invalid; the parser rejects it at that very
            // byte, so whatever follows is never parsed and needs no depth accounting.
            if (depth > 0) {
                --depth;
            }
            break;
        default:
            break;
        }
        // separators grows by at most one per byte, so it never wraps.
        if (max_separators != 0 && separators > max_separators) {
            return Scan::too_many_separators;
        }
    }
    return Scan::ok;
}

} // namespace

bool depth_ok(std::string_view text, std::size_t max_depth) noexcept {
    return prescan(text, max_depth, 0) != Scan::too_deep;
}

Result<Json> parse_untrusted(std::string_view text, std::string_view what) {
    return parse_untrusted(text, what, ParseLimits{});
}

Result<Json> parse_untrusted(std::string_view text, std::string_view what,
                             const ParseLimits& limits) {
    if (text.size() > limits.max_bytes) {
        return std::unexpected(Error{
            .code = limits.code,
            .message = std::format("{} is larger than {}", subject(limits.code, what),
                                   size_text(limits.max_bytes)),
        });
    }
    switch (prescan(text, limits.max_depth, limits.max_separators)) {
    case Scan::ok:
        break;
    case Scan::too_deep:
        return std::unexpected(malformed(
            limits.code, what, std::format("nested deeper than {} levels", limits.max_depth)));
    case Scan::too_many_separators:
        return std::unexpected(
            malformed(limits.code, what,
                      std::format("more than {} structural separators", limits.max_separators)));
    }
    // allow_exceptions = false: a syntax error, trailing data or ill-formed UTF-8 yields a
    // "discarded" value instead of throwing. Comments stay disallowed (ignore_comments = false).
    Json doc = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false,
                           /*ignore_comments=*/false);
    if (doc.is_discarded()) {
        return std::unexpected(malformed(limits.code, what, "not valid JSON"));
    }
    return doc;
}

const Json::object_t* FieldReader::as_object(const Json& value, std::string_view name) {
    if (!ok()) {
        return nullptr;
    }
    const auto* object = value.get_ptr<const Json::object_t*>();
    if (object == nullptr) {
        wrong_type(name);
    }
    return object;
}

const Json* FieldReader::find(const Json::object_t& parent, std::string_view key, Need need) {
    if (!ok()) {
        return nullptr;
    }
    const auto it = parent.find(key);
    if (it == parent.end() || it->second.is_null()) {
        if (need == Need::required) {
            fail(std::format("'{}' is missing", key));
        }
        return nullptr;
    }
    return &it->second;
}

const Json::object_t* FieldReader::object(const Json::object_t& parent, std::string_view key,
                                          Need need) {
    const Json* value = find(parent, key, need);
    if (value == nullptr) {
        return nullptr;
    }
    const auto* object = value->get_ptr<const Json::object_t*>();
    if (object == nullptr) {
        wrong_type(key);
    }
    return object;
}

const Json::array_t* FieldReader::array(const Json::object_t& parent, std::string_view key,
                                        Need need) {
    const Json* value = find(parent, key, need);
    if (value == nullptr) {
        return nullptr;
    }
    const auto* array = value->get_ptr<const Json::array_t*>();
    if (array == nullptr) {
        wrong_type(key);
    }
    return array;
}

std::optional<std::string> FieldReader::string(const Json::object_t& parent, std::string_view key,
                                               Need need) {
    const Json* value = find(parent, key, need);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* text = value->get_ptr<const Json::string_t*>();
    if (text == nullptr) {
        wrong_type(key);
        return std::nullopt;
    }
    return *text;
}

std::optional<bool> FieldReader::boolean(const Json::object_t& parent, std::string_view key,
                                         Need need) {
    const Json* value = find(parent, key, need);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* flag = value->get_ptr<const Json::boolean_t*>();
    if (flag == nullptr) {
        wrong_type(key);
        return std::nullopt;
    }
    return *flag;
}

std::optional<std::uint64_t> FieldReader::unsigned_integer(const Json::object_t& parent,
                                                           std::string_view key, Need need) {
    const Json* value = find(parent, key, need);
    if (value == nullptr) {
        return std::nullopt;
    }
    // nlohmann stores a non-negative integer literal as number_unsigned_t; a negative one as
    // number_integer_t (rejected here), a literal with a fraction or exponent as a float.
    const auto* number = value->get_ptr<const Json::number_unsigned_t*>();
    if (number == nullptr) {
        wrong_type(key);
        return std::nullopt;
    }
    return *number;
}

void FieldReader::fail(std::string_view detail) {
    if (ok()) {
        error_ = std::string{detail};
    }
}

void FieldReader::wrong_type(std::string_view key) {
    fail(std::format("'{}' has the wrong type", key));
}

Error FieldReader::error() const {
    return malformed(code_, what_, error_.value_or("unknown error"));
}

} // namespace mangadex_dl::json_util
