// tests/unit/core/greet_test.cpp: tier-1 example against mangadex_dl::core. Replace it with real
// module tests. Test both sides of every Result: the value and each Error path.
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/greet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("greet formats a greeting for a name", "[core][greet]") {
    CHECK(mangadex_dl::greet("Ada") == std::string{"Hello, Ada!"});
}

TEST_CASE("greet keeps UTF-8 bytes intact", "[core][greet]") {
    CHECK(mangadex_dl::greet("Zoë") == std::string{"Hello, Zoë!"});
}

TEST_CASE("greet rejects an empty name as a usage error", "[core][greet]") {
    const auto result = mangadex_dl::greet("");
    CHECK_FALSE(result.has_value());
    // error_or never dereferences: a value would yield the default Error (failure), and fail here.
    CHECK(result.error_or(mangadex_dl::Error{}).code == mangadex_dl::Errc::usage);
}

TEST_CASE("greet rejects control characters", "[core][greet]") {
    const auto result = mangadex_dl::greet("a\tb");
    CHECK_FALSE(result.has_value());
    CHECK(result.error_or(mangadex_dl::Error{}).code == mangadex_dl::Errc::usage);
}
