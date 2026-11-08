#pragma once
// tests/support/mdx_follows_fixture.hpp: the `follows` fixture trees (follows plan T15).
//
// Header-only: nlohmann/json, the standard library and POSIX only (like mdx_fixture.hpp and
// mdx_auth_fixture.hpp), so the fixture generator, which links nlohmann_json and nothing else, can
// write these trees too. One tree is
//   - one title tree (write_fixture_tree), reused from mdx_fixture.hpp;
//   - the follows collection, one page per file:
//       api/user/follows/manga/offset-<k>.json    GET /user/follows/manga?limit=100&offset=<k>
//   - optionally the reading-status map:
//       api/manga/status.json                     GET /manga/status ({result, statuses})
//   - the fake accounts of follows_accounts() (write_auth_files: auth/password, auth/refresh,
//     auth/revoke and an auth/access file for every access token a grant hands out);
//   - generated credentials files under creds/ (follows_creds_files()), written with exact modes
//     by raw POSIX calls: std::ofstream would create them `0666 & ~umask`.
//
// Every value is obviously fake (`fixture-…`, `…-not-a-secret`). Nothing credential-shaped is
// committed: the creds files exist only in generated trees (git can keep only 0644 and 0755).

#include "support/mdx_auth_fixture.hpp"
#include "support/mdx_fixture.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl::test::mdx {

// ---- the fake accounts
// ---------------------------------------------------------------------------

/// A password no account has (a wrong-password answer).
inline constexpr std::string_view fixture_wrong_password{"fixture-wrong-password-not-a-secret"};
/// Like fixture_username, but a wrong password is answered 400 (newer Keycloak, V2), not 401.
inline constexpr std::string_view fixture_user_400{"fixture-user-400"};
/// Its correct password is refused with 400 `unauthorized_client` (a client not yet approved).
inline constexpr std::string_view fixture_user_unapproved{"fixture-user-unapproved"};
/// Logs in normally, with its own tokens; revoking its refresh token is answered 503.
inline constexpr std::string_view fixture_user_revoke_503{"fixture-user-revoke-503"};
inline constexpr std::string_view fixture_access_revoke_503{"fixture-access-revoke-503"};
inline constexpr std::string_view fixture_refresh_revoke_503{"fixture-refresh-revoke-503"};

/// Every account of every follows tree, all on the fixture client (fixture_client_id/_secret)
/// and, except where noted, with fixture_password:
///   fixture-user             the default token chain (fixture-access-1/-refresh-1, then -2);
///                            a wrong password is 401 invalid_grant
///   fixture-user-400         the same chain (identical files); a wrong password is 400
///   fixture-user-unapproved  the password grant is 400 unauthorized_client; no tokens
///   fixture-user-revoke-503  one grant (fixture-access-revoke-503/-refresh-revoke-503); a revoke
///                            of that refresh token is 503
[[nodiscard]] inline std::vector<FixtureAuth> follows_accounts() {
    std::vector<FixtureAuth> accounts{};
    accounts.push_back(FixtureAuth{});
    accounts.push_back(
        FixtureAuth{.username = std::string{fixture_user_400}, .mismatch_status = 400});
    accounts.push_back(FixtureAuth{
        .username = std::string{fixture_user_unapproved},
        .password_refusal = scripted_error(400, "unauthorized_client",
                                           "Client not allowed for direct access grants"),
        .chain = {},
        .revokes = {},
    });
    accounts.push_back(FixtureAuth{
        .username = std::string{fixture_user_revoke_503},
        .chain = {FixtureGrant{.access_token = std::string{fixture_access_revoke_503},
                               .refresh_token = std::string{fixture_refresh_revoke_503}}},
        .revokes = {FixtureRevoke{.token = std::string{fixture_refresh_revoke_503},
                                  .status = 503,
                                  .body = nlohmann::json{{"error", "temporarily_unavailable"}}}},
    });
    return accounts;
}

// ---- generated credentials files ----------------------------------------------------------------

inline constexpr ::mode_t creds_private_mode = 0600;
inline constexpr ::mode_t creds_shared_mode = 0644;
inline constexpr ::mode_t creds_dir_mode = 0700;

/// One file under <root>/creds/.
struct FixtureCredsFile {
    std::string name{};  ///< the file name (a safe path component)
    std::string bytes{}; ///< the whole content
    ::mode_t mode = creds_private_mode;
};

/// A credentials file in the `key = value` format: the fixture client, `username` and, when
/// given, `password`.
[[nodiscard]] inline std::string credentials_text(std::string_view username,
                                                  std::optional<std::string_view> password) {
    std::string text{"# mangadex-dl test fixture: every value is fake "
                     "(tests/support/mdx_follows_fixture.hpp)\n"};
    text += std::format("client_id = {}\n", fixture_client_id);
    text += std::format("client_secret = {}\n", fixture_client_secret);
    text += std::format("username = {}\n", username);
    if (password.has_value()) {
        text += std::format("password = {}\n", *password);
    }
    return text;
}

/// The files of <root>/creds/, all 0600 except credentials-0644:
///   credentials                     fixture-user with all four keys
///   credentials-client              fixture-user without a password (prompt or --password-stdin)
///   credentials-wrong-password      fixture-user with fixture_wrong_password (401)
///   credentials-wrong-password-400  fixture-user-400 with fixture_wrong_password (400)
///   credentials-unapproved          fixture-user-unapproved (400 unauthorized_client)
///   credentials-revoke-503          fixture-user-revoke-503 (logout's revoke is 503)
///   credentials-0644                `credentials`, mode 0644 (too open: refused)
///   password.txt                    fixture_password and a newline (for --password-stdin)
///   password-wrong.txt              fixture_wrong_password and a newline
[[nodiscard]] inline std::vector<FixtureCredsFile> follows_creds_files() {
    const std::string all_four = credentials_text(fixture_username, fixture_password);
    std::vector<FixtureCredsFile> files{};
    files.push_back({.name = "credentials", .bytes = all_four});
    files.push_back(
        {.name = "credentials-client", .bytes = credentials_text(fixture_username, std::nullopt)});
    files.push_back({.name = "credentials-wrong-password",
                     .bytes = credentials_text(fixture_username, fixture_wrong_password)});
    files.push_back({.name = "credentials-wrong-password-400",
                     .bytes = credentials_text(fixture_user_400, fixture_wrong_password)});
    files.push_back({.name = "credentials-unapproved",
                     .bytes = credentials_text(fixture_user_unapproved, fixture_password)});
    files.push_back({.name = "credentials-revoke-503",
                     .bytes = credentials_text(fixture_user_revoke_503, fixture_password)});
    files.push_back({.name = "credentials-0644", .bytes = all_four, .mode = creds_shared_mode});
    files.push_back({.name = "password.txt", .bytes = std::format("{}\n", fixture_password)});
    files.push_back(
        {.name = "password-wrong.txt", .bytes = std::format("{}\n", fixture_wrong_password)});
    return files;
}

namespace detail {

/// Owns one file descriptor; closes it on destruction (best effort). release() hands it back for
/// a close whose result is checked.
class FixtureFd {
public:
    explicit FixtureFd(int fd) noexcept : fd_(fd) {}
    FixtureFd(const FixtureFd&) = delete;
    FixtureFd& operator=(const FixtureFd&) = delete;
    FixtureFd(FixtureFd&&) = delete;
    FixtureFd& operator=(FixtureFd&&) = delete;
    ~FixtureFd() {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_)); // an error path: the first failure is reported
        }
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_ = -1;
};

[[nodiscard]] inline std::unexpected<std::string> os_error(const std::string& path,
                                                           std::string_view what, int err) {
    return std::unexpected(
        std::format("{}: {}: {}", path, what, std::generic_category().message(err)));
}

/// Writes `bytes` to a new file `name` in the open directory `dir_fd` with exactly `mode`,
/// whatever the umask: any old entry is unlinked first (unlinkat never follows a symlink), then
/// openat(O_CREAT | O_EXCL | O_NOFOLLOW) never writes through anything that appears meanwhile,
/// and fchmod sets the bits the umask filtered out. `dir` is `dir_fd`'s path, for messages only.
[[nodiscard]] inline std::expected<void, std::string>
write_exact_mode_file(int dir_fd, const std::filesystem::path& dir, const std::string& name,
                      std::string_view bytes, ::mode_t mode) {
    const std::string shown = (dir / name).string();
    if (::unlinkat(dir_fd, name.c_str(), 0) != 0 && errno != ENOENT) {
        return os_error(shown, "cannot replace it", errno);
    }
    constexpr int file_flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic in C (the mode)
    FixtureFd fd{::openat(dir_fd, name.c_str(), file_flags, mode)};
    if (fd.get() < 0) {
        return os_error(shown, "cannot create it", errno);
    }
    std::string_view rest = bytes;
    while (!rest.empty()) {
        const ::ssize_t written = ::write(fd.get(), rest.data(), rest.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return os_error(shown, "write failed", errno);
        }
        if (written == 0) { // no progress: an error, never a spin
            return os_error(shown, "write failed", EIO);
        }
        rest.remove_prefix(static_cast<std::size_t>(written));
    }
    if (::fchmod(fd.get(), mode) != 0) {
        return os_error(shown, "fchmod failed", errno);
    }
    if (::close(fd.release()) != 0) {
        return os_error(shown, "close failed", errno);
    }
    return {};
}

} // namespace detail

/// Writes follows_creds_files() into `dir`. Its parents are created if needed; `dir` itself is
/// created 0700, or must already be a real directory (a symlink at `dir` is refused, never
/// followed), and is then fchmod'ed to exactly 0700. The files are written relative to that open
/// directory.
[[nodiscard]] inline std::expected<void, std::string>
write_creds_files(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir.parent_path(), ec);
    if (ec) {
        return std::unexpected(std::format("{}: {}", dir.parent_path().string(), ec.message()));
    }
    const std::string name = dir.string();
    if (::mkdir(name.c_str(), creds_dir_mode) != 0 && errno != EEXIST) {
        return detail::os_error(name, "cannot create it", errno);
    }
    constexpr int dir_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic in C
    const detail::FixtureFd dir_fd{::openat(AT_FDCWD, name.c_str(), dir_flags)};
    if (dir_fd.get() < 0) {
        // ELOOP: a symlink; ENOTDIR: some other non-directory.
        return detail::os_error(name, "not a directory (or a symlink)", errno);
    }
    if (::fchmod(dir_fd.get(), creds_dir_mode) != 0) {
        return detail::os_error(name, "fchmod failed", errno);
    }
    for (const FixtureCredsFile& file : follows_creds_files()) {
        if (const auto ok =
                detail::write_exact_mode_file(dir_fd.get(), dir, file.name, file.bytes, file.mode);
            !ok) {
            return ok;
        }
    }
    return {};
}

// ---- follows and reading statuses ---------------------------------------------------------------

/// The follows page size of the trees (the API's maximum, and what the client asks for).
inline constexpr std::size_t follows_fixture_page_limit = 100;

/// Title ids the follows trees name (no tree serves a manga document for 6666…, 7777… or 8888…).
inline constexpr std::string_view follows_bulk_id{"33333333-3333-4333-8333-333333333333"};
inline constexpr std::string_view follows_nochapters_id{"55555555-5555-4555-8555-555555555555"};
/// FS1: followed, with the unknown status `paused`.
inline constexpr std::string_view follows_paused_id{"77777777-7777-4777-8777-777777777777"};
/// FS1: followed, without a status.
inline constexpr std::string_view follows_no_status_id{"88888888-8888-4888-8888-888888888888"};
/// FS1: has the status `reading`, but is not followed.
inline constexpr std::string_view status_not_followed_id{"99999999-9999-4999-8999-999999999999"};
/// F5 and FS3b: an id that is not a UUID (a path escape).
inline constexpr std::string_view follows_bad_id{"../x"};

/// One follows tree. Raw strings and raw JSON throughout, so hostile documents are expressible.
struct FollowsFixture {
    std::vector<FixtureManga> titles{}; ///< the title trees written into the root
    std::vector<std::string> follows{}; ///< the followed ids, in API order (duplicates allowed)
    std::size_t page_limit = follows_fixture_page_limit; ///< each page's `limit` (at least 1)
    /// The `statuses` member of api/manga/status.json; nullopt: no status document (a 404).
    std::optional<nlohmann::json> statuses{};
    std::vector<FixtureAuth> accounts = follows_accounts();
};

/// One `data[]` entry of a follows page: a manga entity (unknown members included on purpose).
[[nodiscard]] inline nlohmann::json followed_title_json(std::string_view id) {
    return {{"id", id},
            {"type", "manga"},
            {"attributes",
             {{"title", {{"en", "A Followed Title"}}},
              {"originalLanguage", "ja"},
              {"status", "ongoing"},
              {"version", 1}}},
            {"relationships", nlohmann::json::array()}};
}

/// GET /user/follows/manga?limit=100&offset=<k>: one document per page of `limit`, offsets
/// 0, limit, ...; always at least one (possibly empty) page.
[[nodiscard]] inline std::vector<nlohmann::json>
follows_pages_json(const std::vector<std::string>& follows,
                   std::size_t limit = follows_fixture_page_limit) {
    return detail::collection_pages(follows.size(), limit, [&follows](std::size_t i) {
        return followed_title_json(follows[i]);
    });
}

/// GET /manga/status: {"result":"ok","statuses":<statuses>}.
[[nodiscard]] inline nlohmann::json status_map_json(const nlohmann::json& statuses) {
    return {{"result", "ok"}, {"statuses", statuses}};
}

/// Writes the tree for `fixture` under `root` (created if needed; existing files are replaced,
/// stale ones are NOT removed): every title tree, the follows pages, the status map (when set),
/// every account's auth files and <root>/creds/.
[[nodiscard]] inline std::expected<void, std::string>
write_follows_tree(const std::filesystem::path& root, const FollowsFixture& fixture) {
    if (fixture.page_limit == 0) {
        return std::unexpected(std::string{"a FollowsFixture needs a page_limit of at least 1"});
    }
    for (const FixtureManga& title : fixture.titles) {
        if (const auto ok = write_fixture_tree(root, title); !ok) {
            return ok;
        }
    }
    const std::filesystem::path follows_dir = root / "api" / "user" / "follows" / "manga";
    const std::vector<nlohmann::json> pages =
        follows_pages_json(fixture.follows, fixture.page_limit);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const std::string name = std::format("offset-{}.json", i * fixture.page_limit);
        if (const auto ok = detail::write_json(follows_dir / name, pages[i]); !ok) {
            return ok;
        }
    }
    if (fixture.statuses.has_value()) {
        if (const auto ok = detail::write_json(root / "api" / "manga" / "status.json",
                                               status_map_json(*fixture.statuses));
            !ok) {
            return ok;
        }
    }
    for (const FixtureAuth& account : fixture.accounts) {
        if (const auto ok = write_auth_files(root, account); !ok) {
            return ok;
        }
    }
    return write_creds_files(root / "creds");
}

// ---- the trees (follows spec "Hermetic test story" #3) ------------------------------------------

namespace detail {

[[nodiscard]] inline FollowsFixture clean_follows(std::vector<std::string> follows) {
    return FollowsFixture{.titles = {clean_fixture()}, .follows = std::move(follows)};
}

/// FS1-FS3: clean, following only the clean title, with `statuses`.
[[nodiscard]] inline FollowsFixture clean_status_follows(nlohmann::json statuses) {
    FollowsFixture fixture = clean_follows({std::string{clean_manga_id}});
    fixture.statuses = std::move(statuses);
    return fixture;
}

} // namespace detail

/// F1: clean; follows the clean title.
[[nodiscard]] inline FollowsFixture follows_clean_fixture() {
    return detail::clean_follows({std::string{clean_manga_id}});
}

/// F2: clean; follows the clean title, then 6666… (not found: exit 4).
[[nodiscard]] inline FollowsFixture follows_unknown_fixture() {
    return detail::clean_follows({std::string{clean_manga_id}, std::string{unknown_manga_id}});
}

/// F3: clean; follows nothing (one empty page, total 0).
[[nodiscard]] inline FollowsFixture follows_empty_fixture() { return detail::clean_follows({}); }

/// F4: clean; follows the clean title twice (item 2 is a duplicate).
[[nodiscard]] inline FollowsFixture follows_twice_fixture() {
    return detail::clean_follows({std::string{clean_manga_id}, std::string{clean_manga_id}});
}

/// F5: clean; follows the clean title, then `../x`: a malformed follows page (exit 3).
[[nodiscard]] inline FollowsFixture follows_bad_id_fixture() {
    return detail::clean_follows({std::string{clean_manga_id}, std::string{follows_bad_id}});
}

/// F6: bulk; two pages of page size 1: the bulk title (offset 0), then 6666… (offset 1). A dry run
/// of item 1 alone fills any pipe.
[[nodiscard]] inline FollowsFixture follows_bulk_fixture() {
    return FollowsFixture{.titles = {bulk_fixture()},
                          .follows = {std::string{follows_bulk_id}, std::string{unknown_manga_id}},
                          .page_limit = 1};
}

/// F7: nochapters; follows its title (exit 5), then 6666… (exit 4): mixed failures (exit 7).
[[nodiscard]] inline FollowsFixture follows_nochapters_fixture() {
    return FollowsFixture{
        .titles = {nochapters_fixture()},
        .follows = {std::string{follows_nochapters_id}, std::string{unknown_manga_id}}};
}

/// FS1: clean; follows 6666…, 1111…, 7777…, 8888… (API order); statuses {1111…: reading,
/// 6666…: completed, 7777…: paused (unknown), 9999…: reading (not followed)}; 8888… has none.
[[nodiscard]] inline FollowsFixture follows_status_fixture() {
    FollowsFixture fixture =
        detail::clean_follows({std::string{unknown_manga_id}, std::string{clean_manga_id},
                               std::string{follows_paused_id}, std::string{follows_no_status_id}});
    nlohmann::json statuses = nlohmann::json::object();
    statuses[std::string{clean_manga_id}] = "reading";
    statuses[std::string{unknown_manga_id}] = "completed";
    statuses[std::string{follows_paused_id}] = "paused";
    statuses[std::string{status_not_followed_id}] = "reading";
    fixture.statuses = std::move(statuses);
    return fixture;
}

/// FS2: clean; follows the clean title; `statuses: []` (MangaDex's empty form).
[[nodiscard]] inline FollowsFixture follows_status_empty_fixture() {
    return detail::clean_status_follows(nlohmann::json::array());
}

/// FS3a: clean; follows the clean title; one status value is a number (malformed, exit 3).
[[nodiscard]] inline FollowsFixture follows_status_bad_value_fixture() {
    nlohmann::json statuses = nlohmann::json::object();
    statuses[std::string{clean_manga_id}] = "reading";
    statuses[std::string{unknown_manga_id}] = 1;
    return detail::clean_status_follows(std::move(statuses));
}

/// FS3b: clean; follows the clean title; one status key is `../x` (malformed, exit 3).
[[nodiscard]] inline FollowsFixture follows_status_bad_key_fixture() {
    nlohmann::json statuses = nlohmann::json::object();
    statuses[std::string{clean_manga_id}] = "reading";
    statuses[std::string{follows_bad_id}] = "reading";
    return detail::clean_status_follows(std::move(statuses));
}

struct NamedFollowsFixture {
    /// Always a string literal (static storage), so the view never dangles.
    std::string_view name{};
    FollowsFixture (*make)() = nullptr;
};

/// The follows trees fixture_gen writes next to all_fixtures(), by directory name (each a safe,
/// unique path component, none shared with all_fixtures()).
[[nodiscard]] inline std::vector<NamedFollowsFixture> follows_fixtures() {
    return {{.name = "follows-clean", .make = &follows_clean_fixture},
            {.name = "follows-unknown", .make = &follows_unknown_fixture},
            {.name = "follows-empty", .make = &follows_empty_fixture},
            {.name = "follows-twice", .make = &follows_twice_fixture},
            {.name = "follows-bad-id", .make = &follows_bad_id_fixture},
            {.name = "follows-bulk", .make = &follows_bulk_fixture},
            {.name = "follows-nochapters", .make = &follows_nochapters_fixture},
            {.name = "follows-status", .make = &follows_status_fixture},
            {.name = "follows-status-empty", .make = &follows_status_empty_fixture},
            {.name = "follows-status-bad-value", .make = &follows_status_bad_value_fixture},
            {.name = "follows-status-bad-key", .make = &follows_status_bad_key_fixture}};
}

} // namespace mangadex_dl::test::mdx
