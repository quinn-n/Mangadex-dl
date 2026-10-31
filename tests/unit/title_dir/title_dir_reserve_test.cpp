// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/title_dir/title_dir_reserve_test.cpp: `batch --dir-per-title` reservations
// (batch-dir-per-title plan T1, spec QB2/QB3). TitleDirRegistry::reserve() keeps the first
// segment of every explicit `dir` cell for its rows, so another manga's derived claim of that name
// is moved aside with `reserved` set (AC8). A manga keeps its first directory on later claims
// (AC7), reservations never feed the per-manga lookup, a stem reserved by two mangas still falls
// back to first-claim-wins among their derived rows (plan S3), and the registry owns what it is
// given (AC8, ASan-checked). The rules without any reserve() call stay pinned, unchanged, by
// title_dir_test.cpp.
#include "mangadex_dl/manga_id.hpp"
#include "mangadex_dl/title_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace md = mangadex_dl;

// The ids of title_dir_test.cpp.
constexpr std::string_view id_a = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view id_b = "77777777-7777-4777-8777-777777777777";
constexpr std::string_view id_c = "77777777-0000-4000-8000-000000000001"; ///< b's 8-hex prefix
constexpr std::string_view id_d = "99999999-9999-4999-8999-999999999999";

[[nodiscard]] md::MangaId manga(std::string_view text) {
    auto parsed = md::MangaId::parse(text);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

} // namespace

// ---- AC8: another manga's reservation moves a derived claim aside ------------------------------

TEST_CASE("title dir: reserve: a stem reserved by another manga is moved aside, naming the first "
          "reserving line",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Clean Title", manga(id_b), 2);
    registry.reserve("Clean Title", manga(id_d), 5);
    // The derived row comes before both reserving rows: the order does not matter.
    CHECK(registry.claim("Clean Title", manga(id_a), 1) ==
          md::TitleDirClaim{.name = "Clean Title [11111111]", .taken_by = 2, .reserved = true});
    // The moved-aside manga keeps its name, without a new warning.
    CHECK(registry.claim("Clean Title", manga(id_a), 7) ==
          md::TitleDirClaim{.name = "Clean Title [11111111]"});
}

TEST_CASE("title dir: reserve: the reservation wins over an earlier derived claim of a reserver",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("T", manga(id_a), 2);
    CHECK(registry.claim("T", manga(id_a), 3) == md::TitleDirClaim{.name = "T"});
    // Rule 2 comes before rule 3: the reported line is the reserving one, not the holder's.
    CHECK(registry.claim("T", manga(id_b), 4) ==
          md::TitleDirClaim{.name = "T [77777777]", .taken_by = 2, .reserved = true});
}

// ---- AC8: the same manga among the reservers ---------------------------------------------------

TEST_CASE("title dir: reserve: a manga among several reservers gets the plain stem",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Clean Title", manga(id_b), 2);
    registry.reserve("Clean Title/extras", manga(id_a), 4);
    registry.reserve("Clean Title", manga(id_d), 6);
    CHECK(registry.claim("Clean Title", manga(id_a), 3) ==
          md::TitleDirClaim{.name = "Clean Title"});
    // A manga that reserved nothing is still moved aside, naming the first reserving line.
    CHECK(registry.claim("Clean Title", manga(id_c), 8) ==
          md::TitleDirClaim{.name = "Clean Title [77777777]", .taken_by = 2, .reserved = true});
}

TEST_CASE("title dir: reserve: the only reserver gets the plain stem, with nothing reported",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Clean Title", manga(id_a), 2);
    CHECK(registry.claim("Clean Title", manga(id_a), 3) ==
          md::TitleDirClaim{.name = "Clean Title"});
}

// ---- AC8: the first segment ---------------------------------------------------------------------

TEST_CASE("title dir: reserve: only the first segment of the cell is reserved",
          "[title_dir][reserve]") {
    SECTION("a grouping folder") {
        md::TitleDirRegistry registry{};
        registry.reserve("Clean Title/extras", manga(id_b), 2);
        CHECK(registry.claim("Clean Title", manga(id_a), 3) ==
              md::TitleDirClaim{.name = "Clean Title [11111111]", .taken_by = 2, .reserved = true});
        CHECK(registry.claim("extras", manga(id_d), 4) == md::TitleDirClaim{.name = "extras"});
    }
    SECTION("a series directory") {
        md::TitleDirRegistry registry{};
        registry.reserve("series/clean", manga(id_b), 2);
        CHECK(registry.claim("clean", manga(id_a), 3) == md::TitleDirClaim{.name = "clean"});
        CHECK(registry.claim("series", manga(id_d), 4) ==
              md::TitleDirClaim{.name = "series [99999999]", .taken_by = 2, .reserved = true});
        CHECK(registry.claim("series/clean", manga(id_c), 5) ==
              md::TitleDirClaim{.name = "series/clean"});
    }
    SECTION("an empty cell reserves nothing") {
        md::TitleDirRegistry registry{};
        registry.reserve("", manga(id_b), 2);
        CHECK(registry.claim("", manga(id_a), 3) == md::TitleDirClaim{.name = ""});
        CHECK(registry.claim("Clean Title", manga(id_d), 4) ==
              md::TitleDirClaim{.name = "Clean Title"});
    }
}

TEST_CASE("title dir: reserve: names are compared byte for byte", "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("clean title", manga(id_b), 2);
    registry.reserve("Clean Title ", manga(id_b), 3); // not trimmed here: validate_list_dir trims
    CHECK(registry.claim("Clean Title", manga(id_a), 4) ==
          md::TitleDirClaim{.name = "Clean Title"});
}

// ---- AC8: the full-uuid fallback ----------------------------------------------------------------

TEST_CASE("title dir: reserve: a suffixed name reserved by another manga pushes to the full uuid",
          "[title_dir][reserve]") {
    SECTION("the stem is reserved") {
        md::TitleDirRegistry registry{};
        registry.reserve("Clean Title", manga(id_b), 2);
        registry.reserve("Clean Title [11111111]", manga(id_d), 3);
        CHECK(registry.claim("Clean Title", manga(id_a), 5) ==
              md::TitleDirClaim{
                  .name = std::format("Clean Title [{}]", id_a), .taken_by = 2, .reserved = true});
    }
    SECTION("the stem is held by a claim") {
        md::TitleDirRegistry registry{};
        registry.reserve("Clean Title [11111111]", manga(id_d), 3);
        CHECK(registry.claim("Clean Title", manga(id_b), 1) ==
              md::TitleDirClaim{.name = "Clean Title"});
        CHECK(registry.claim("Clean Title", manga(id_a), 4) ==
              md::TitleDirClaim{.name = std::format("Clean Title [{}]", id_a), .taken_by = 1});
    }
    SECTION("the full-uuid name is returned even when it is reserved too") {
        md::TitleDirRegistry registry{};
        const std::string full = std::format("Clean Title [{}]", id_a);
        registry.reserve("Clean Title", manga(id_b), 2);
        registry.reserve("Clean Title [11111111]", manga(id_d), 3);
        registry.reserve(full, manga(id_d), 4);
        CHECK(registry.claim("Clean Title", manga(id_a), 5) ==
              md::TitleDirClaim{.name = full, .taken_by = 2, .reserved = true});
    }
}

TEST_CASE("title dir: reserve: a suffixed name reserved by the same manga stays 8-hex",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Clean Title", manga(id_b), 2);
    registry.reserve("Clean Title [11111111]", manga(id_a), 3);
    CHECK(registry.claim("Clean Title", manga(id_a), 4) ==
          md::TitleDirClaim{.name = "Clean Title [11111111]", .taken_by = 2, .reserved = true});
}

// ---- AC8: reservations never feed the per-manga lookup -----------------------------------------

TEST_CASE("title dir: reserve: a reservation is not the manga's directory",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Foo", manga(id_a), 2);
    CHECK(registry.claim("Clean Title", manga(id_a), 3) ==
          md::TitleDirClaim{.name = "Clean Title"});
    // Its first derived claim fixed the directory, whatever the later stem (QB3).
    CHECK(registry.claim("Foo", manga(id_a), 4) == md::TitleDirClaim{.name = "Clean Title"});
    // The reservation still keeps "Foo" from other mangas.
    CHECK(registry.claim("Foo", manga(id_b), 5) ==
          md::TitleDirClaim{.name = "Foo [77777777]", .taken_by = 2, .reserved = true});
}

// ---- AC7: a manga stays in its first directory
// ---------------------------------------------------

TEST_CASE("title dir: reserve: a manga moved aside by a reservation keeps that name for another "
          "stem",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Clean Title", manga(id_b), 2);
    CHECK(registry.claim("Clean Title", manga(id_a), 3) ==
          md::TitleDirClaim{.name = "Clean Title [11111111]", .taken_by = 2, .reserved = true});
    CHECK(registry.claim("Other Title", manga(id_a), 4) ==
          md::TitleDirClaim{.name = "Clean Title [11111111]"});
    // The other stem stays free.
    CHECK(registry.claim("Other Title", manga(id_d), 5) ==
          md::TitleDirClaim{.name = "Other Title"});
}

// ---- plan S3: a stem reserved by two mangas
// -------------------------------------------------------

TEST_CASE("title dir: reserve: two reservers of a stem fall back to first claim wins",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("T", manga(id_a), 2);
    registry.reserve("T", manga(id_b), 3);
    CHECK(registry.claim("T", manga(id_a), 4) == md::TitleDirClaim{.name = "T"});
    // b is a reserver, so the reservation does not move it, but a's derived row holds the name:
    // derived directories of different mangas never coincide.
    CHECK(registry.claim("T", manga(id_b), 5) ==
          md::TitleDirClaim{.name = "T [77777777]", .taken_by = 4});
}

// ---- no behaviour change when nothing relevant is reserved --------------------------------------

TEST_CASE("title dir: reserve: an unrelated reservation leaves the claim rules as they were",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    registry.reserve("Unrelated/x", manga(id_d), 9);
    CHECK(registry.claim("Clean Title", manga(id_a), 1) ==
          md::TitleDirClaim{.name = "Clean Title"});
    CHECK(registry.claim("Clean Title", manga(id_b), 2) ==
          md::TitleDirClaim{.name = "Clean Title [77777777]", .taken_by = 1});
    CHECK(registry.claim("Clean Title", manga(id_c), 3) ==
          md::TitleDirClaim{.name = std::format("Clean Title [{}]", id_c), .taken_by = 1});
}

// ---- AC8: the registry owns its keys
// --------------------------------------------------------------

TEST_CASE("title dir: reserve: the registry owns the reserved name and manga (borrowed for the "
          "call only)",
          "[title_dir][reserve]") {
    md::TitleDirRegistry registry{};
    {
        std::string row_dir = "Short Lived Dir/sub";
        md::MangaId reserver = manga(id_b);
        registry.reserve(row_dir, reserver, 2);
        row_dir.assign(300, 'z'); // reallocates: a stored view would now dangle
        reserver = manga(id_d);   // a stored pointer or view would now name another manga
    }
    CHECK(registry.claim("Short Lived Dir", manga(id_a), 3) ==
          md::TitleDirClaim{.name = "Short Lived Dir [11111111]", .taken_by = 2, .reserved = true});
    // b, the reserver, gets the plain name; d, what the caller's variable later held, does not.
    CHECK(registry.claim("Short Lived Dir", manga(id_b), 4) ==
          md::TitleDirClaim{.name = "Short Lived Dir"});
    CHECK(registry.claim("Short Lived Dir", manga(id_d), 5) ==
          md::TitleDirClaim{.name = "Short Lived Dir [99999999]", .taken_by = 2, .reserved = true});
}
