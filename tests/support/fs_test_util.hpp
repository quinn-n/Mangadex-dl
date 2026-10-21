#pragma once
// tests/support/fs_test_util.hpp: small filesystem helpers shared by the unit and in-process CLI
// tests (a temporary directory, listing, reading and writing whole files). Header-only. A
// read-only open is in read_only_fd.hpp.
//
// Catch2-aware: every helper REQUIREs its precondition, so call them only from inside a running
// TEST_CASE. Two listing helpers on purpose, because their callers want different things for a
// missing directory: list_dir throws std::filesystem::filesystem_error (Catch2 reports an
// unexpected exception), list_dir_if_exists returns an empty list (a CLI test whose run failed
// before creating DIR). forge::write_file (zip_forge.hpp) stays separate: it writes bytes and
// returns false instead of failing the test.
//
//     namespace fs_util = mangadex_dl::test::fs_util;
//     const mangadex_dl::test::TempDir dir = fs_util::make_temp_dir();
//     fs_util::write_file(dir.path() / "a.txt", "content");
//     CHECK(fs_util::list_dir(dir.path()) == std::vector<std::string>{"a.txt"});

#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mangadex_dl::test::fs_util {

/// A fresh TempDir `<temp_directory_path()>/<prefix>-XXXXXX`, required to be created.
[[nodiscard]] inline TempDir make_temp_dir(std::string_view prefix = "mangadex-dl-test") {
    auto dir = TempDir::create(prefix);
    INFO((dir ? std::string{} : dir.error()));
    REQUIRE(dir.has_value());
    return std::move(*dir);
}

/// The names in `dir`, sorted. Throws std::filesystem::filesystem_error when `dir` cannot be
/// listed (a missing directory included).
[[nodiscard]] inline std::vector<std::string> list_dir(const std::filesystem::path& dir) {
    std::vector<std::string> names{};
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{dir}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

/// The names in `dir`, sorted; empty when it does not exist or cannot be listed.
[[nodiscard]] inline std::vector<std::string> list_dir_if_exists(const std::filesystem::path& dir) {
    std::vector<std::string> names{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

/// Every byte of the file at `path`, which must be readable.
[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Creates or truncates `path` and writes `content` to it; every step is required to succeed.
inline void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(!out.fail());
}

} // namespace mangadex_dl::test::fs_util
