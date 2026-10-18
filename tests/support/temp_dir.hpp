#pragma once
// tests/support/temp_dir.hpp: an RAII, uniquely named temporary directory for tests.
//
// TempDir::create() makes a fresh directory with mkdtemp(3) (atomic create, mode 0700, random
// suffix) under std::filesystem::temp_directory_path(), which honours TMPDIR. The destructor
// removes it recursively. std::filesystem::remove_all never follows symlinks, so a symlink planted
// inside the tree is unlinked, never traversed.
//
//     auto dir = mangadex_dl::test::TempDir::create();
//     REQUIRE(dir.has_value());
//     const std::filesystem::path root = dir->path(); // a copy (or keep `dir` alive)
//
// path() returns a reference into the TempDir: never bind it to a temporary
// (`const auto& p = TempDir::create()->path();` dangles once the expected is destroyed).

#include "mangadex_dl/attributes.hpp"

#include <cerrno>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mangadex_dl::test {

class TempDir {
public:
    /// Creates `<temp_directory_path()>/<prefix>-XXXXXX`. `prefix` must not contain '/'.
    [[nodiscard]] static std::expected<TempDir, std::string>
    create(std::string_view prefix = "mangadex-dl-test") {
        if (prefix.empty() || prefix.contains('/')) {
            return std::unexpected(std::format("TempDir: invalid prefix '{}'", prefix));
        }
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return std::unexpected(std::format("TempDir: no temp directory: {}", ec.message()));
        }
        // mkdtemp rewrites the trailing XXXXXX in place; std::string::data() is a writable,
        // NUL-terminated buffer of exactly size() characters.
        std::string name = (base / std::format("{}-XXXXXX", prefix)).string();
        if (::mkdtemp(name.data()) == nullptr) {
            const int err = errno;
            return std::unexpected(std::format("TempDir: mkdtemp({}): {}", name,
                                               std::generic_category().message(err)));
        }
        return TempDir{std::filesystem::path{std::move(name)}};
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&& other) noexcept : path_(std::exchange(other.path_, {})) {}
    TempDir& operator=(TempDir&& other) noexcept {
        if (this != &other) {
            remove();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }
    ~TempDir() { remove(); }

    /// The directory (absolute when TMPDIR is). Empty only for a moved-from TempDir.
    [[nodiscard]] const std::filesystem::path& path() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return path_;
    }

private:
    explicit TempDir(std::filesystem::path path) noexcept : path_(std::move(path)) {}

    void remove() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            // Best effort: a destructor cannot report a failure.
            static_cast<void>(std::filesystem::remove_all(path_, ec));
            path_.clear();
        }
    }

    std::filesystem::path path_{};
};

} // namespace mangadex_dl::test
