#pragma once
// tests/support/download_cli_harness.hpp: shared helpers for the in-process `download` tests
// (tests/cli/download_*_test.cpp), which run the whole chain through run(args, Io&) against a
// hermetic fixture tree (tests/support/mdx_fixture.hpp) written into a fresh temporary directory.
//
// Header-only and Catch2-aware: make_temp_dir(), read_file() (both from fs_test_util.hpp) and
// Tree's constructor REQUIRE their preconditions, so they may only be called from inside a
// running TEST_CASE.
//
//     const Tree tree{mdx::clean_fixture()};
//     const std::filesystem::path out = tree.path("out");
//     const Captured r = run_args(tree.download(out, {"--dry-run"}));
//     CHECK(r.out == path_lines(out, {"Clean Title - Vol. 01.epub"}));

#include "app/cli.hpp"
#include "app/io.hpp"
#include "support/fs_test_util.hpp" // IWYU pragma: export
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::test::download_cli {

// ---- running the CLI -------------------------------------------------------------------------

/// run_cli for an argument list built at runtime. argv[0] is supplied; `args` must outlive the
/// call (it does: run() returns before this function does).
[[nodiscard]] inline Captured run_args(const std::vector<std::string>& args, Tty tty = {},
                                       app::EnvSnapshot env = {}) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back("mangadex-dl"); // a string literal: static storage
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out,
               .err = err,
               .stdin_tty = tty.stdin_tty,
               .stdout_tty = tty.stdout_tty,
               .stderr_tty = tty.stderr_tty,
               .env = std::move(env)};
    Captured result;
    result.exit_code = app::run(std::span<const char* const>{argv}, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

/// `front` followed by `rest`.
[[nodiscard]] inline std::vector<std::string> concat(std::initializer_list<std::string> front,
                                                     const std::vector<std::string>& rest) {
    std::vector<std::string> out{front};
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

// ---- files -----------------------------------------------------------------------------------

using fs_util::make_temp_dir;
using fs_util::read_file;

/// The names in `dir`, sorted; empty when it does not exist (a run that failed before creating
/// DIR), unlike fs_util::list_dir.
[[nodiscard]] inline std::vector<std::string> list_dir(const std::filesystem::path& dir) {
    return fs_util::list_dir_if_exists(dir);
}

/// Every file in `dir` by name, with its bytes.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
snapshot(const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, std::string>> files{};
    for (const std::string& name : list_dir(dir)) {
        files.emplace_back(name, read_file(dir / name));
    }
    return files;
}

/// A fixture tree for one title under a fresh temporary directory; output directories go next to
/// it. Not copyable or movable (TempDir owns the directory).
class Tree {
public:
    explicit Tree(const mdx::FixtureManga& manga)
        : tmp_(make_temp_dir("mangadex-dl-cli")), root_(tmp_.path() / "fixture"), id_(manga.id) {
        const auto written = mdx::write_fixture_tree(root_, manga);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Tree(Tree&&) = delete;
    Tree& operator=(Tree&&) = delete;
    ~Tree() = default;

    [[nodiscard]] std::string api_base() const { return "file://" + root_.string(); }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    /// `<tmp>/<name>`, not created.
    [[nodiscard]] std::filesystem::path path(std::string_view name) const {
        return tmp_.path() / name;
    }

    /// `download --api-base <tree> <options...> <id> <dir>`.
    [[nodiscard]] std::vector<std::string>
    download(const std::filesystem::path& dir,
             std::initializer_list<std::string> options = {}) const {
        std::vector<std::string> args{"download", "--api-base", api_base()};
        args.insert(args.end(), options.begin(), options.end());
        args.push_back(id_);
        args.push_back(dir.string());
        return args;
    }

private:
    TempDir tmp_; ///< no default constructor: initialized by the constructor
    std::filesystem::path root_{};
    std::string id_{};
};

// ---- output checks ---------------------------------------------------------------------------

/// `text` split at '\n'; a final line without a newline is kept, a trailing newline adds none.
[[nodiscard]] inline std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines{};
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

/// `<dir>/<name>\n` for each name: the stdout path stream.
[[nodiscard]] inline std::string path_lines(const std::filesystem::path& dir,
                                            std::initializer_list<std::string_view> names) {
    std::string out;
    for (const std::string_view name : names) {
        out += (dir / name).string();
        out += '\n';
    }
    return out;
}

/// Exactly one JSON document (plus the final newline), parsed; a discarded value otherwise.
[[nodiscard]] inline nlohmann::json parse_document(const std::string& text) {
    if (!text.ends_with('\n') || !nlohmann::json::accept(text)) {
        return nlohmann::json::value_t::discarded;
    }
    return nlohmann::json::parse(text);
}

} // namespace mangadex_dl::test::download_cli
