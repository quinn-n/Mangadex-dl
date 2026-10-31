// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/form_test.cpp: the application/x-www-form-urlencoded codec (follows plan T6,
// AC1). form_encode keeps exactly the RFC 3986 unreserved bytes and escapes the rest as upper-case
// %XX; form_decode turns '+' into a space, accepts either hex case, and refuses bad escapes,
// empty or nameless fields, fields without '=', duplicate names and more than 16 fields, with
// messages that never contain body bytes.

#include "auth/form.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/secret.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mangadex_dl::Errc;
using mangadex_dl::form_encode;
using mangadex_dl::Secret;
using mangadex_dl::detail::find_form_field;
using mangadex_dl::detail::form_decode;
using mangadex_dl::detail::FormField;
using mangadex_dl::detail::max_form_fields;

[[nodiscard]] std::string encoded(std::string_view text) {
    const Secret secret = form_encode(text);
    return std::string{secret.reveal()};
}

/// The decoded value of `name`, or "<absent>".
[[nodiscard]] std::string value_of(const std::vector<FormField>& fields, std::string_view name) {
    const FormField* const field = find_form_field(fields, name);
    return field == nullptr ? std::string{"<absent>"} : std::string{field->value.reveal()};
}

/// Every byte 0x00..0xFF once, in order.
[[nodiscard]] std::string all_bytes() {
    std::string bytes{};
    for (unsigned b = 0; b < 256U; ++b) {
        bytes.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    }
    return bytes;
}

} // namespace

TEST_CASE("form: encode keeps the RFC 3986 unreserved bytes", "[auth][form]") {
    constexpr std::string_view unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    CHECK(encoded(unreserved) == unreserved);
    CHECK(encoded("").empty());
}

TEST_CASE("form: encode escapes every other byte as upper-case %XX", "[auth][form]") {
    CHECK(encoded("& = + % space") == "%26%20%3D%20%2B%20%25%20space");
    CHECK(encoded("\xC3\xA9") == "%C3%A9"); // é in UTF-8
    CHECK(encoded("a/b?c#d") == "a%2Fb%3Fc%23d");
    CHECK(encoded(std::string_view{"\0", 1}) == "%00");
    CHECK(encoded("\xFF\x7F") == "%FF%7F");
    CHECK(encoded("!*'();:@$,[]") == "%21%2A%27%28%29%3B%3A%40%24%2C%5B%5D");

    // Every byte: exactly 66 are kept, the other 190 take three characters each.
    const std::string every = encoded(all_bytes());
    CHECK(every.size() == 66 + (std::size_t{190} * 3));
    for (std::size_t i = 0; i < every.size(); ++i) {
        if (every[i] == '%') {
            REQUIRE(i + 2 < every.size());
            CHECK(std::string_view{"0123456789ABCDEF"}.contains(every[i + 1]));
            CHECK(std::string_view{"0123456789ABCDEF"}.contains(every[i + 2]));
        }
    }
}

TEST_CASE("form: decode round-trips every encoded byte", "[auth][form]") {
    const std::string value =
        GENERATE(std::string{"fixture-password-not-a-secret"},
                 std::string{"p&ss=w+rd %41 \xC3\xA9"}, all_bytes(), std::string{});
    const Secret value_part = form_encode(value);
    std::string body{"grant_type=password&password="};
    body.append(value_part.reveal()).append("&x=1");
    const auto fields = form_decode(body);
    REQUIRE(fields.has_value());
    REQUIRE(fields->size() == 3);
    CHECK(value_of(*fields, "grant_type") == "password");
    CHECK(value_of(*fields, "password") == value);
    CHECK(value_of(*fields, "x") == "1");
    CHECK(value_of(*fields, "missing") == "<absent>");
}

TEST_CASE("form: decode reads '+' as a space and either hex case", "[auth][form]") {
    const auto fields = form_decode("a=x+y&b=%c3%a9%C3%A9&c%5Fd=%2b&e=");
    REQUIRE(fields.has_value());
    REQUIRE(fields->size() == 4);
    CHECK((*fields)[0].name == "a");
    CHECK(value_of(*fields, "a") == "x y");
    CHECK(value_of(*fields, "b") == "\xC3\xA9\xC3\xA9");
    CHECK(value_of(*fields, "c_d") == "+"); // the name is decoded too
    CHECK(value_of(*fields, "e").empty());
    CHECK(find_form_field(*fields, "e") != nullptr);
}

TEST_CASE("form: decode keeps the first '=' as the split", "[auth][form]") {
    const auto fields = form_decode("a=b=c");
    REQUIRE(fields.has_value());
    CHECK(value_of(*fields, "a") == "b=c");
}

TEST_CASE("form: an empty body has no fields", "[auth][form]") {
    const auto fields = form_decode("");
    REQUIRE(fields.has_value());
    CHECK(fields->empty());
}

TEST_CASE("form: malformed bodies are Errc::failure without body bytes", "[auth][form]") {
    const std::string body = GENERATE(
        // bad escapes, in the value and in the name
        std::string{"password=fixture-password-not-a-secret%"},
        std::string{"password=fixture-password-not-a-secret%4"},
        std::string{"password=fixture-password-not-a-secret%G1"},
        std::string{"password=fixture-password-not-a-secret%4G"},
        std::string{"password=%%41fixture-password-not-a-secret"},
        std::string{"pass%zzword=fixture-password-not-a-secret"},
        // empty fields
        std::string{"a=1&&password=fixture-password-not-a-secret"},
        std::string{"password=fixture-password-not-a-secret&"},
        std::string{"&password=fixture-password-not-a-secret"}, std::string{"&"},
        // no '=' or no name
        std::string{"fixture-password-not-a-secret"}, std::string{"=fixture-password-not-a-secret"},
        // duplicates, also after decoding
        std::string{"password=fixture-password-not-a-secret&password=x"},
        std::string{"password=fixture-password-not-a-secret&p%61ssword=x"});
    CAPTURE(body);
    const auto fields = form_decode(body);
    REQUIRE_FALSE(fields.has_value());
    CHECK(fields.error().code == Errc::failure);
    CHECK(fields.error().message.starts_with("form body: "));
    CHECK_FALSE(fields.error().message.contains("fixture-password"));
}

TEST_CASE("form: at most 16 fields", "[auth][form]") {
    std::string body{};
    for (std::size_t i = 0; i < max_form_fields; ++i) {
        body += std::format("{}f{}=v", i == 0 ? "" : "&", i);
    }
    const auto sixteen = form_decode(body);
    REQUIRE(sixteen.has_value());
    CHECK(sixteen->size() == max_form_fields);
    CHECK(value_of(*sixteen, "f15") == "v");

    body += "&f16=v";
    const auto seventeen = form_decode(body);
    REQUIRE_FALSE(seventeen.has_value());
    CHECK(seventeen.error().message == "form body: more than 16 fields");
}
