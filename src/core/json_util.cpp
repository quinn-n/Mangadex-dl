// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_util.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/mangadex_api.hpp"

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

[[nodiscard]] Error malformed(std::string_view what, std::string_view detail) {
    return Error{.code = Errc::network,
                 .message = std::format("malformed MangaDex API response ({}): {}", what, detail)};
}

} // namespace

bool depth_ok(std::string_view text, std::size_t max_depth) noexcept {
    std::size_t depth = 0;
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
                return false;
            }
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
    }
    return true;
}

Result<Json> parse_untrusted(std::string_view text, std::string_view what) {
    if (text.size() > max_api_json_bytes) {
        return std::unexpected(Error{
            .code = Errc::network,
            .message = std::format("MangaDex API response ({}) is larger than {} MiB", what,
                                   max_api_json_bytes / (std::size_t{1024} * 1024)),
        });
    }
    if (!depth_ok(text, max_json_depth)) {
        return std::unexpected(
            malformed(what, std::format("nested deeper than {} levels", max_json_depth)));
    }
    // allow_exceptions = false: a syntax error, trailing data or ill-formed UTF-8 yields a
    // "discarded" value instead of throwing. Comments stay disallowed (ignore_comments = false).
    Json doc = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false,
                           /*ignore_comments=*/false);
    if (doc.is_discarded()) {
        return std::unexpected(malformed(what, "not valid JSON"));
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

Error FieldReader::error() const { return malformed(what_, error_.value_or("unknown error")); }

} // namespace mangadex_dl::json_util
