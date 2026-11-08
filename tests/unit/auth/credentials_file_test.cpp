// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/credentials_file_test.cpp: parse_credentials_file and load_credentials_file
// (follows plan T9, AC2). Every error names the line and never a value; the secret values land
// in Secrets. All values are the obviously fake fixture constants, assembled at run time.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/fs_test_util.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using md::load_credentials_file;
using md::parse_credentials_file;
using md::test::fs_util::make_temp_dir;

/// A value planted in bad lines: no error message may contain it.
inline constexpr std::string_view marker{"zq-marker-value"};

inline constexpr std::string_view keys_text{
    "the keys are client_id, client_secret, username and password"};

/// "<key> = <value>\n", assembled at run time.
[[nodiscard]] std::string line(std::string_view key, std::string_view value) {
    return std::format("{} = {}\n", key, value);
}

/// All four keys with the fixture values.
[[nodiscard]] std::string full_file() {
    return line("client_id", mdx::fixture_client_id) +
           line("client_secret", mdx::fixture_client_secret) +
           line("username", mdx::fixture_username) + line("password", mdx::fixture_password);
}

/// The secret's text, or nullopt when there is none (a checked access, visible to clang-tidy).
[[nodiscard]] std::optional<std::string_view> revealed(const std::optional<md::Secret>& secret) {
    if (!secret.has_value()) {
        return std::nullopt;
    }
    return secret->reveal();
}

/// Writes `content` to a new file at `path` with exactly `mode` (fchmod, so the umask does not
/// apply).
void write_with_mode(const fs::path& path, std::string_view content, mode_t mode) {
    constexpr int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const md::UniqueFd fd{::open(path.c_str(), flags, 0600)};
    REQUIRE(fd.valid());
    REQUIRE(::write(fd.get(), content.data(), content.size()) ==
            static_cast<ssize_t>(content.size()));
    REQUIRE(::fchmod(fd.get(), mode) == 0);
}

/// The error message of a parse that must fail.
[[nodiscard]] std::string parse_error(std::string_view text) {
    const auto parsed = parse_credentials_file(text);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == md::Errc::failure);
    CHECK(parsed.error().exit_code() == 1);
    return parsed.error().message;
}

TEST_CASE("credentials file: all four keys are read", "[auth][credentials-file]") {
    const auto parsed = parse_credentials_file(full_file());
    REQUIRE(parsed.has_value());
    CHECK(parsed->client_id == std::string{mdx::fixture_client_id});
    CHECK(revealed(parsed->client_secret) == std::string_view{mdx::fixture_client_secret});
    CHECK(parsed->username == std::string{mdx::fixture_username});
    CHECK(revealed(parsed->password) == std::string_view{mdx::fixture_password});
}

TEST_CASE("credentials file: comments, blank lines, CRLF and trimming",
          "[auth][credentials-file]") {
    const std::string text = std::format("# my MangaDex login\n"
                                         "\n"
                                         "   \t\n"
                                         "  # an indented comment = with an equals sign\n"
                                         "client_id\t=\t{}  \r\n"
                                         "   username={}\r\n",
                                         mdx::fixture_client_id, mdx::fixture_username);
    const auto parsed = parse_credentials_file(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->client_id == std::string{mdx::fixture_client_id});
    CHECK(parsed->username == std::string{mdx::fixture_username});
    CHECK_FALSE(parsed->client_secret.has_value());
    CHECK_FALSE(parsed->password.has_value());
}

TEST_CASE("credentials file: the value is everything after the first '='",
          "[auth][credentials-file]") {
    const auto parsed = parse_credentials_file(line("password", "a=b = c==") + "username = x y");
    REQUIRE(parsed.has_value());
    CHECK(revealed(parsed->password) == std::string_view{"a=b = c=="});
    CHECK(parsed->username == "x y"); // the last line needs no newline
}

TEST_CASE("credentials file: an empty value counts as absent", "[auth][credentials-file]") {
    const auto parsed = parse_credentials_file("client_id =\npassword = \t \nusername=\n");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->client_id.has_value());
    CHECK_FALSE(parsed->password.has_value());
    CHECK_FALSE(parsed->username.has_value());
    CHECK_FALSE(parsed->client_secret.has_value());
}

TEST_CASE("credentials file: an empty file has nothing in it", "[auth][credentials-file]") {
    const std::string_view text = GENERATE(std::string_view{}, std::string_view{"\n\n"},
                                           std::string_view{"# only a comment\n"});
    const auto parsed = parse_credentials_file(text);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->client_id.has_value());
    CHECK_FALSE(parsed->client_secret.has_value());
    CHECK_FALSE(parsed->username.has_value());
    CHECK_FALSE(parsed->password.has_value());
}

TEST_CASE("credentials file: a value of 4096 bytes is accepted, 4097 is not",
          "[auth][credentials-file]") {
    const std::string longest(md::max_prompt_answer_bytes, 'p');
    const auto parsed = parse_credentials_file(line("password", longest));
    REQUIRE(parsed.has_value());
    CHECK(revealed(parsed->password).value_or("").size() == md::max_prompt_answer_bytes);

    const std::string message = parse_error(line("password", longest + "p"));
    CHECK(message == "line 1: the value of 'password' is longer than 4096 bytes");
}

TEST_CASE("credentials file: an unknown key that looks like one is named",
          "[auth][credentials-file]") {
    const std::string message =
        parse_error(line("username", mdx::fixture_username) + line("colour", marker));
    CHECK(message == std::format("line 2: unknown key 'colour'; {}", keys_text));
}

TEST_CASE("credentials file: an unknown key that does not look like one is not shown",
          "[auth][credentials-file]") {
    const std::string_view key = GENERATE(std::string_view{"Bad-Key!"}, std::string_view{"hunter2"},
                                          std::string_view{"UserName"}, std::string_view{""},
                                          std::string_view{"a_key_that_is_longer_than_32_bytes"});
    CAPTURE(key);
    const std::string message = parse_error(std::format("\n{} = {}\n", key, marker));
    CHECK(message == std::format("line 2: unknown key (not shown); {}", keys_text));
}

TEST_CASE("credentials file: a duplicate key is refused, even after an empty value",
          "[auth][credentials-file]") {
    CHECK(parse_error(line("username", "a") + "# c\n" + line("username", marker)) ==
          "line 3: duplicate key 'username'");
    CHECK(parse_error("password =\n" + line("password", marker)) ==
          "line 2: duplicate key 'password'");
}

TEST_CASE("credentials file: a line without '=' is refused without echoing it",
          "[auth][credentials-file]") {
    const std::string message = parse_error(line("username", "a") + std::string{marker} + "\n");
    CHECK(message == "line 2: no '=' (each line is 'key = value')");
}

TEST_CASE("credentials file: a control byte or NUL in a value is refused",
          "[auth][credentials-file]") {
    const std::string_view control =
        GENERATE(std::string_view{"\x01", 1}, std::string_view{"\0", 1}, std::string_view{"\t"},
                 std::string_view{"\r"}, std::string_view{"\x1b"}, std::string_view{"\x7f"},
                 std::string_view{"\xc2\x85"}, std::string_view{"\xc2\x9b"});
    CAPTURE(control.size());
    const std::string value = std::string{marker} + std::string{control} + std::string{marker};
    const std::string message = parse_error(line("password", value));
    CHECK(message == "line 1: the value of 'password' contains a control character");
}

TEST_CASE("credentials file: invalid UTF-8 is refused anywhere, comments included",
          "[auth][credentials-file]") {
    CHECK(parse_error(line("username", std::string{marker} + "\xff")) == "line 1: invalid UTF-8");
    CHECK(parse_error("\n# caf\xe9\n") == "line 2: invalid UTF-8");
    CHECK(parse_error(line("password", "\xed\xa0\x80")) == "line 1: invalid UTF-8"); // surrogate
}

TEST_CASE("credentials file: non-ASCII UTF-8 values are kept", "[auth][credentials-file]") {
    const auto parsed = parse_credentials_file(line("username", "caf\xc3\xa9"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->username == "caf\xc3\xa9");
}

TEST_CASE("credentials file: no error message contains a value", "[auth][credentials-file]") {
    const std::string bad = GENERATE(
        line("colour", marker), line("Bad-Key!", marker), std::string{marker} + "\n",
        line("password", std::string{marker} + "\x01"),
        line("password", std::string{marker} + "\xff"),
        line("password", marker) + line("password", marker),
        line("password", std::string(md::max_prompt_answer_bytes, 'x') + std::string{marker}));
    const std::string message = parse_error(bad);
    CHECK_FALSE(message.contains(marker));
}

// ---- load_credentials_file --------------------------------------------------------------------

TEST_CASE("credentials file: a private file is loaded", "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    write_with_mode(path, full_file(), 0600);

    const auto loaded = load_credentials_file(path, /*explicit_path=*/true);
    REQUIRE(loaded.has_value());
    CHECK(loaded->client_id == std::string{mdx::fixture_client_id});
    CHECK(revealed(loaded->password) == std::string_view{mdx::fixture_password});
}

TEST_CASE("credentials file: a file others can read gets the chmod hint",
          "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    write_with_mode(path, full_file(), 0644);

    const bool explicit_path = GENERATE(true, false);
    const auto loaded = load_credentials_file(path, explicit_path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == md::Errc::failure);
    CHECK(loaded.error().message ==
          std::format("credentials file '{0}' is readable or writable by others (mode 0644); "
                      "run 'chmod 600 {0}'",
                      path.string()));
    CHECK_FALSE(loaded.error().message.contains(mdx::fixture_password));
}

TEST_CASE("credentials file: the chmod hint shell-quotes a path with a space or a quote",
          "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "my creds's file";
    write_with_mode(path, full_file(), 0640);

    const auto loaded = load_credentials_file(path, /*explicit_path=*/true);
    REQUIRE_FALSE(loaded.has_value());
    const std::string quoted = std::format("'{}/my creds'\\''s file'",
                                           dir.path().string()); // the TempDir path has no quote
    CHECK(loaded.error().message ==
          std::format("credentials file '{}' is readable or writable by others (mode 0640); "
                      "run 'chmod 600 {}'",
                      path.string(), quoted));
}

TEST_CASE("credentials file: a missing file is an error only when it was named",
          "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";

    const auto named = load_credentials_file(path, /*explicit_path=*/true);
    REQUIRE_FALSE(named.has_value());
    CHECK(named.error().code == md::Errc::failure);
    CHECK(named.error().message ==
          std::format("credentials file '{}' does not exist", path.string()));

    const auto by_default = load_credentials_file(path, /*explicit_path=*/false);
    REQUIRE(by_default.has_value());
    CHECK_FALSE(by_default->client_id.has_value());
    CHECK_FALSE(by_default->client_secret.has_value());
    CHECK_FALSE(by_default->username.has_value());
    CHECK_FALSE(by_default->password.has_value());
}

TEST_CASE("credentials file: a directory is not a regular file", "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    REQUIRE(::mkdir(path.c_str(), 0700) == 0);

    const auto loaded = load_credentials_file(path, /*explicit_path=*/false);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().message ==
          std::format("credentials file '{}' is not a regular file", path.string()));
}

TEST_CASE("credentials file: more than 16 KiB is refused", "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    write_with_mode(path, std::string(md::max_credentials_file_bytes + 1, '\n'), 0600);

    const auto loaded = load_credentials_file(path, /*explicit_path=*/true);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().message ==
          std::format("credentials file '{}' is larger than 16384 bytes", path.string()));

    const fs::path exact = dir.path() / "exact";
    write_with_mode(exact, std::string(md::max_credentials_file_bytes, '\n'), 0600);
    CHECK(load_credentials_file(exact, /*explicit_path=*/true).has_value());
}

TEST_CASE("credentials file: a malformed file names the path and the line, not the value",
          "[auth][credentials-file]") {
    const md::test::TempDir dir = make_temp_dir();
    const fs::path path = dir.path() / "credentials";
    write_with_mode(path, line("password", mdx::fixture_password) + line("password", marker), 0600);

    const auto loaded = load_credentials_file(path, /*explicit_path=*/true);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == md::Errc::failure);
    CHECK(loaded.error().message ==
          std::format("credentials file '{}': line 2: duplicate key 'password'", path.string()));
    CHECK_FALSE(loaded.error().message.contains(mdx::fixture_password));
    CHECK_FALSE(loaded.error().message.contains(marker));
}

} // namespace
