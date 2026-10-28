#pragma once
// tests/support/mdx_twin.hpp: the `clean-twin` fixture tree (list plan T9, spec AC41): the clean
// title plus a second manga with the same title, so --dir-per-title has two different titles that
// claim the same directory name in one run.
//
// The twin is clean_fixture() with another manga id. Both share every chapter (ids, at-home
// documents and page images) and the cover listing: fixture mode ignores `?manga[]=`, so there is
// a single `api/cover/offset-0.json`, and the planner does not filter it by manga. What the twin
// needs of its own is written under its id: its manga document, its feed pages and its cover
// images under `uploads/covers/<twin id>/` (fact-check amendment 1: without them a real run of the
// twin warns once per EPUB about a missing cover, and stderr could not be asserted exactly).
//
// Header-only, like mdx_fixture.hpp, and deliberately not in all_fixtures(): the twelve canned
// trees stay twelve.

#include "support/mdx_fixture.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace mangadex_dl::test::mdx {

/// The twin's manga id: its 8-hex prefix is the ` [77777777]` directory suffix.
inline constexpr std::string_view twin_manga_id = "77777777-7777-4777-8777-777777777777";

/// clean_fixture() under twin_manga_id: the same title ("Clean Title"), chapters and covers.
[[nodiscard]] inline FixtureManga clean_twin_fixture() {
    FixtureManga twin = clean_fixture();
    twin.id = std::string{twin_manga_id};
    return twin;
}

/// Writes the `clean-twin` tree under `root`: everything write_fixture_tree writes for the clean
/// title, plus the twin's api/manga/<twin>.json, api/manga/<twin>/feed/offset-0.json and
/// uploads/covers/<twin>/<file>. The twin is written first and clean last, so every document the
/// two share (the cover listing, the at-home documents and the page images) is clean's, byte for
/// byte as in the plain `clean` tree.
[[nodiscard]] inline std::expected<void, std::string>
write_clean_twin_tree(const std::filesystem::path& root) {
    if (const auto ok = write_fixture_tree(root, clean_twin_fixture()); !ok) {
        return ok;
    }
    return write_fixture_tree(root, clean_fixture());
}

} // namespace mangadex_dl::test::mdx
