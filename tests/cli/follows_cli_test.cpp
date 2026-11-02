// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/follows_cli_test.cpp: the whole `follows` command in-process (follows plan T18),
// through run_follows_command with a filled RawFollowsArgs (no registered command: registration
// is T19's) and the test-only option parser, over generated follows trees
// (support/mdx_follows_fixture.hpp) in a fresh temporary directory, with HOME and the XDG
// directories inside it and std::ostringstream sinks.
//
// The command runs over an injected fixture Backend whose transport records every request (its
// URL, whether it carried an Authorization and a body) and can answer or interrupt one itself, so
// a test can assert "no request", count token POSTs and interrupt the collection fetch.
//
// Covers AC13 (the first run asks, the second does not), AC14 at the command level, AC19 (the
// validate-level usage errors), AC20 (-v, -vv, no secret anywhere), AC21 (interrupts during the
// collection fetch and at the password prompt), AC22 (the follows half: a session that can't be
// saved), AC55-AC58 (--status), AC59's follows half, the collection failures (request, content,
// refusal, an ended session), the empty runs, a failed stdout, color, the help footer, the option
// binding (--status like --group, --credentials verbatim), and the rev1 gate: a DIR that holds
// the user's password is never echoed by the empty-stdin, no-login, wrong-password or collection
// errors.
//
// Every credential is a fake from support/mdx_auth_fixture.hpp; no test contacts a real host.
// Catch2 test names must not start with '-'. Every name starts with "follows cli:" so
// `pixi run test -R 'cli\.follows cli'` selects them all.

#include "app/cmd_download_run.hpp"
#include "app/cmd_follows.hpp"
#include "app/command.hpp"
#include "app/exit_help.hpp"
#include "app/io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/follows.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/net/fixture_transport.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "support/fake_prompter.hpp"
#include "support/fs_test_util.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/mdx_follows_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

// NOLINTNEXTLINE(modernize-deprecated-headers): SIGINT is POSIX <signal.h>
#include <signal.h>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using md::PromptStatus;
using md::test::Captured;
using md::test::FakePrompter;
using md::test::Tty;
using md::test::fs_util::read_file;
using md::test::fs_util::write_file;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view clean_id = mdx::clean_manga_id;
constexpr std::string_view unknown_id = mdx::unknown_manga_id;
constexpr std::string_view clean_vol1 = "Clean Title - Vol. 01.epub";
constexpr std::string_view clean_vol2 = "Clean Title - Vol. 02.epub";

constexpr std::string_view hint = "Run 'mangadex-dl follows --help' for more information.\n";
constexpr std::string_view remedy =
    "; check your network connection or proxy (https_proxy) and try again";
constexpr std::string_view hint_notice =
    "mangadex-dl: reading your follows needs a MangaDex personal API client; create one at "
    "https://mangadex.org/settings (API Clients)";

/// A DIR that is really the user's password (follows plan rev1 gate): never echoed.
constexpr std::string_view password_dir = "hunter2";

/// A fixture root that never exists: a case whose phase 1 were missed would fail to build its
/// backend (exit 1) instead of exiting 2.
constexpr std::string_view missing_api_base = "file:///nonexistent-mangadex-dl-follows-fixture";

constexpr Tty terminals{.stdin_tty = true, .stdout_tty = true, .stderr_tty = true};

/// The fixture values that must never appear in any output.
constexpr std::array<std::string_view, 6> secrets{
    mdx::fixture_password, mdx::fixture_client_secret, mdx::fixture_access_1,
    mdx::fixture_access_2, mdx::fixture_refresh_1,     mdx::fixture_refresh_2,
};

/// The exit-4 line of a title that isn't on MangaDex, without the row prefix.
[[nodiscard]] std::string title_not_found(std::string_view id) {
    return std::format("title {} not found on MangaDex (HTTP 404); check the UUID or URL", id);
}

// ---- the recording transport -------------------------------------------------------------------

/// One request as the transport saw it (no secret is kept: only whether there was one).
struct Seen {
    md::HttpMethod method = md::HttpMethod::get;
    std::string url{};
    bool authorized = false; ///< a non-empty Authorization
    bool has_body = false;
};

/// Answers a request itself instead of the fixture (nullopt: let the fixture answer). Called on
/// whichever thread fetches (image workers included), so it must be thread-safe: capture values.
using Intercept = std::function<std::optional<md::TransportResult>(const md::HttpRequest&)>;

/// The fixture transport, recording every request (thread-safe, as Transport requires), with an
/// optional intercept.
class RecordingTransport final : public md::Transport {
public:
    explicit RecordingTransport(std::unique_ptr<md::Transport> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& request) override {
        Intercept intercept{};
        {
            const std::scoped_lock lock{mutex_};
            seen_.push_back(Seen{.method = request.method,
                                 .url = request.url,
                                 .authorized = !request.authorization.empty(),
                                 .has_body = !request.body.empty()});
            intercept = intercept_;
        }
        if (intercept) {
            if (std::optional<md::TransportResult> answer = intercept(request)) {
                return std::move(*answer);
            }
        }
        return inner_->fetch(request);
    }

    void set_intercept(Intercept intercept) {
        const std::scoped_lock lock{mutex_};
        intercept_ = std::move(intercept);
    }

    [[nodiscard]] std::vector<Seen> seen() const {
        const std::scoped_lock lock{mutex_};
        return seen_;
    }

private:
    std::unique_ptr<md::Transport> inner_{};
    mutable std::mutex mutex_{};
    std::vector<Seen> seen_{}; ///< guarded by mutex_
    Intercept intercept_{};    ///< guarded by mutex_
};

// ---- the harness -------------------------------------------------------------------------------

/// Clears the process-wide interrupt flag on entry and exit, so test order does not matter.
class InterruptFlagReset {
public:
    InterruptFlagReset() noexcept { md::reset_interrupt_for_testing(); }
    InterruptFlagReset(const InterruptFlagReset&) = delete;
    InterruptFlagReset& operator=(const InterruptFlagReset&) = delete;
    InterruptFlagReset(InterruptFlagReset&&) = delete;
    InterruptFlagReset& operator=(InterruptFlagReset&&) = delete;
    ~InterruptFlagReset() { md::reset_interrupt_for_testing(); }
};

/// What one run sees besides its RawFollowsArgs.
struct Context {
    app::GlobalOptions globals{};
    Tty tty{};
    int stdin_fd = -1; ///< borrowed; -1 = a closed stdin
    md::Prompter* prompter = nullptr;
    std::optional<app::EnvSnapshot> env{}; ///< nullopt: the harness's
};

/// A follows tree under a fresh TempDir, HOME and the XDG directories next to it, and a fixture
/// Backend over it whose transport records every request. Not copyable or movable: the Backend is
/// borrowed by every run.
class Harness {
public:
    explicit Harness(const mdx::FollowsFixture& fixture = mdx::follows_clean_fixture())
        : tmp_(md::test::fs_util::make_temp_dir("mangadex-dl-follows-cli")) {
        const auto written = mdx::write_follows_tree(tree(), fixture);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        md::Result<std::unique_ptr<md::Transport>> inner = md::make_fixture_transport(tree());
        REQUIRE(inner.has_value());
        auto recorder = std::make_unique<RecordingTransport>(std::move(*inner));
        recorder_ = recorder.get(); // heap object owned by backend_: stable
        std::unique_ptr<md::Clock> clock = md::make_instant_clock();
        md::Clock* const borrowed = clock.get();
        backend_ = app::Backend{
            .transport = std::move(recorder), .owned_clock = std::move(clock), .clock = borrowed};
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;
    ~Harness() = default;

    [[nodiscard]] fs::path root() const { return tmp_.path(); }
    [[nodiscard]] fs::path tree() const { return root() / "tree"; }
    [[nodiscard]] fs::path path(std::string_view name) const { return root() / name; }
    [[nodiscard]] fs::path creds(std::string_view name) const {
        return tree() / "creds" / std::string{name};
    }
    [[nodiscard]] fs::path state_dir() const { return root() / "state" / "mangadex-dl"; }
    [[nodiscard]] fs::path session_file() const {
        return state_dir() / md::session_file_name(md::SessionSlot::fixture);
    }
    [[nodiscard]] fs::path default_credentials() const {
        return root() / "config" / "mangadex-dl" / "credentials";
    }
    [[nodiscard]] app::EnvSnapshot env() const {
        return app::EnvSnapshot{.home = (root() / "home").string(),
                                .xdg_config_home = (root() / "config").string(),
                                .xdg_state_home = (root() / "state").string()};
    }
    [[nodiscard]] std::string api_base() const { return "file://" + tree().string(); }
    [[nodiscard]] std::string api() const { return api_base() + "/api"; }
    [[nodiscard]] std::string token_url() const { return mdx::token_url(tree()); }
    [[nodiscard]] std::string follows_url(std::size_t offset = 0) const {
        return md::follows_request_url(api(), offset);
    }
    [[nodiscard]] std::string status_url() const { return md::status_map_request_url(api()); }

    [[nodiscard]] std::vector<Seen> seen() const { return recorder_->seen(); }
    [[nodiscard]] std::size_t count(std::string_view url) const {
        const std::vector<Seen> all = seen();
        return static_cast<std::size_t>(
            std::ranges::count_if(all, [url](const Seen& s) { return s.url == url; }));
    }
    void intercept(Intercept hook) const { recorder_->set_intercept(std::move(hook)); }

    /// DIR, the tree's API base and `--credentials creds/<credentials>` (empty: none given).
    [[nodiscard]] md::RawFollowsArgs args(const fs::path& dir,
                                          std::string_view credentials = "credentials") const {
        md::RawFollowsArgs raw{};
        raw.defaults.dir = dir.string();
        raw.defaults.api_base = api_base();
        if (!credentials.empty()) {
            raw.auth.credentials = creds(credentials).string();
            raw.auth.credentials_given = true;
        }
        return raw;
    }

    /// run_follows_command over this harness's backend and two string streams.
    [[nodiscard]] Captured run(const md::RawFollowsArgs& args, const Context& context = {}) const {
        std::ostringstream out;
        std::ostringstream err;
        app::Io io{.out = out,
                   .err = err,
                   .stdin_tty = context.tty.stdin_tty,
                   .stdout_tty = context.tty.stdout_tty,
                   .stderr_tty = context.tty.stderr_tty,
                   .env = context.env.value_or(env()),
                   .stdin_fd = context.stdin_fd,
                   .prompter = context.prompter};
        Captured result;
        result.exit_code = app::run_follows_command(args, context.globals, io, backend_);
        result.out = out.str();
        result.err = err.str();
        return result;
    }

    /// The same through the overload that builds its own Backend from the API base.
    [[nodiscard]] Captured run_own_backend(const md::RawFollowsArgs& args,
                                           const Context& context = {}) const {
        std::ostringstream out;
        std::ostringstream err;
        app::Io io{.out = out,
                   .err = err,
                   .env = context.env.value_or(env()),
                   .stdin_fd = context.stdin_fd};
        Captured result;
        result.exit_code = app::run_follows_command(args, context.globals, io);
        result.out = out.str();
        result.err = err.str();
        return result;
    }

    /// Writes `saved` into the fixture slot through the store (0700 directory, 0600 file).
    void seed(const md::Session& saved) const {
        md::FileResult<md::StateDir> dir = md::StateDir::open(state_dir(), true);
        REQUIRE(dir.has_value());
        const std::unique_ptr<md::Clock> clock = md::make_instant_clock();
        const md::Result<md::SessionLock> lock = dir->lock(*clock, md::LockTiming{}, {});
        REQUIRE(lock.has_value());
        REQUIRE(dir->write(md::SessionSlot::fixture, md::session_to_json(saved)).has_value());
    }

    [[nodiscard]] std::string issuer() const {
        md::Result<md::ApiBase> base = md::ApiBase::parse(api_base());
        REQUIRE(base.has_value());
        const std::optional<md::AuthEndpoints> endpoints = md::auth_endpoints_for(*base);
        REQUIRE(endpoints.has_value());
        return endpoints.has_value() ? endpoints->issuer : std::string{};
    }

private:
    md::test::TempDir tmp_;                  ///< set by the constructor
    RecordingTransport* recorder_ = nullptr; ///< borrowed from backend_
    app::Backend backend_{};
};

[[nodiscard]] md::BearerToken token(std::string_view text) {
    md::Result<md::BearerToken> parsed = md::BearerToken::parse(md::Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Both ends of a pipe whose read end holds `content` and then end of input (the write end is
/// closed before this returns).
[[nodiscard]] md::UniqueFd pipe_with(std::string_view content) {
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::pipe2(fds.data(), O_CLOEXEC) == 0);
    md::UniqueFd read_end{fds[0]};
    md::UniqueFd write_end{fds[1]};
    REQUIRE(content.size() < 4096); // fits the pipe buffer: write() never blocks
    if (!content.empty()) {
        REQUIRE(::write(write_end.get(), content.data(), content.size()) ==
                static_cast<::ssize_t>(content.size()));
    }
    write_end.reset();
    return read_end;
}

/// A FakePrompter that answers every detail of the fixture account, in the prompt order.
void answer_everything(FakePrompter& prompter) {
    prompter.answer(mdx::fixture_client_id);
    prompter.answer(mdx::fixture_client_secret);
    prompter.answer(mdx::fixture_username);
    prompter.answer(mdx::fixture_password);
}

/// An answer for the intercept: HTTP `status` with an empty JSON body.
[[nodiscard]] md::TransportResult http(int status) {
    return md::HttpResponse{.status = status, .body = "{}"};
}

/// An intercept that answers every request to `url` with `answer` (copies both).
[[nodiscard]] Intercept answer_url(std::string target, md::TransportResult reply) {
    return [url = std::move(target), answer = std::move(reply)](
               const md::HttpRequest& request) -> std::optional<md::TransportResult> {
        if (request.url != url) {
            return std::nullopt;
        }
        return answer;
    };
}

// ---- output checks -----------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> lines_of(std::string_view text) {
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
[[nodiscard]] std::string path_lines(const fs::path& dir,
                                     std::initializer_list<std::string_view> names) {
    std::string out;
    for (const std::string_view name : names) {
        out += (dir / name).string();
        out += '\n';
    }
    return out;
}

/// Exactly one JSON document (plus the final newline), parsed; a discarded value otherwise.
[[nodiscard]] Json parse_document(const std::string& text) {
    if (!text.ends_with('\n') || !Json::accept(text)) {
        return Json::value_t::discarded;
    }
    return Json::parse(text);
}

/// The top-level keys of the document, in document order.
[[nodiscard]] std::vector<std::string> top_level_keys(const std::string& text) {
    std::vector<std::string> keys{};
    if (!OrderedJson::accept(text)) {
        return keys;
    }
    for (const auto& item : OrderedJson::parse(text).items()) {
        keys.push_back(item.key());
    }
    return keys;
}

/// The keys of the document's `follows` object, in document order.
[[nodiscard]] std::vector<std::string> follows_keys(const std::string& text) {
    std::vector<std::string> keys{};
    if (!OrderedJson::accept(text)) {
        return keys;
    }
    for (const auto& item : OrderedJson::parse(text).at("follows").items()) {
        keys.push_back(item.key());
    }
    return keys;
}

/// The top-level keys of a follows document, in order.
[[nodiscard]] std::vector<std::string> document_keys() {
    return {"format_version", "dry_run", "source", "follows", "titles", "summary"};
}

/// The statuses of the document's titles, in order.
[[nodiscard]] std::vector<std::string> statuses(const Json& doc) {
    std::vector<std::string> out{};
    for (const Json& row : doc.at("titles")) {
        out.push_back(row.at("status").get<std::string>());
    }
    return out;
}

/// No output carries a fixture secret.
void check_no_secret(const Captured& r) {
    const std::string all = r.out + r.err;
    for (const std::string_view secret : secrets) {
        CHECK_THAT(all, !ContainsSubstring(std::string{secret}));
    }
}

/// Every request carrying an Authorization went to one of the two allowlisted collection URLs.
void check_authorization_only_on_collection(const Harness& h) {
    for (const Seen& s : h.seen()) {
        INFO(s.url);
        const bool collection =
            s.url.starts_with(h.api() + "/user/follows/manga?") || s.url == h.status_url();
        CHECK(s.authorized == collection);
    }
}

} // namespace

// ---- success -----------------------------------------------------------------------------------

TEST_CASE("follows cli: a private credentials file downloads every follow (F1)", "[cli][follows]") {
    const Harness h{};
    const fs::path out = h.path("out");
    const Captured r = h.run(h.args(out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(fs::is_regular_file(out / clean_vol1));
    CHECK(fs::is_regular_file(out / clean_vol2));
    CHECK(h.count(h.token_url()) == 1);
    CHECK(h.count(h.follows_url()) == 1);
    CHECK(h.count(h.status_url()) == 0); // no --status: no status request
    check_authorization_only_on_collection(h);
    CHECK(fs::is_regular_file(h.session_file()));
    check_no_secret(r);

    SECTION("--json: batch's document plus `follows` after `source`") {
        const fs::path json_out = h.path("out-json");
        const Captured json = h.run(h.args(json_out), {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err.empty());
        CHECK(top_level_keys(json.out) == document_keys());
        CHECK(follows_keys(json.out) ==
              std::vector<std::string>{"username", "titles", "statuses", "selected"});
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("source") == "follows");
        CHECK(doc.at("follows") == Json{{"username", mdx::fixture_username},
                                        {"titles", 1},
                                        {"statuses", nullptr},
                                        {"selected", 1}});
        CHECK(statuses(doc) == std::vector<std::string>{"ok"});
        CHECK(doc.at("titles").at(0).at("line") == 1);
        CHECK(doc.at("titles").at(0).at("manga_id") == clean_id);
        check_no_secret(json);
    }
}

TEST_CASE("follows cli: the first run asks for every detail; the second asks nothing (AC13)",
          "[cli][follows][prompt]") {
    const Harness h{};
    const fs::path out = h.path("out");
    FakePrompter first{};
    answer_everything(first);
    const Captured r = h.run(h.args(out, ""), {.tty = terminals, .prompter = &first});
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(first.events() == std::vector<std::string>{
                                std::format("notice: {}", hint_notice),
                                "ask (echo on): MangaDex API client id: ",
                                "ask (echo off): MangaDex API client secret: ",
                                "ask (echo on): MangaDex username: ",
                                "ask (echo off): MangaDex password for fixture-user: ",
                            });
    CHECK(first.remaining() == 0);
    check_no_secret(r);

    // The saved login: 0600 in a 0700 directory, without the password.
    REQUIRE(fs::is_regular_file(h.session_file()));
    CHECK(fs::status(h.session_file()).permissions() ==
          (fs::perms::owner_read | fs::perms::owner_write));
    CHECK(fs::status(h.state_dir()).permissions() == fs::perms::owner_all);
    CHECK_THAT(read_file(h.session_file()), !ContainsSubstring(std::string{mdx::fixture_password}));

    // The second run: no prompt, and at most one token request (here none: the saved access token
    // is still valid).
    const std::size_t posts_before = h.count(h.token_url());
    FakePrompter second{};
    const Captured again =
        h.run(h.args(out, ""), {.globals = {.verbose = 1}, .tty = terminals, .prompter = &second});
    CHECK(again.exit_code == 0);
    CHECK(second.events().empty());
    CHECK(h.count(h.token_url()) - posts_before <= 1);
    CHECK(again.err.contains("mangadex-dl: logged in to MangaDex as fixture-user (saved login)\n"));
    check_no_secret(again);
}

TEST_CASE("follows cli: no terminal, no session and no credentials -> 8, nothing sent (AC14)",
          "[cli][follows][auth]") {
    const Harness h{};
    const fs::path out = h.path("out");
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = h.run(h.args(out, ""), {.globals = {.json = json}});
        CHECK(r.exit_code == 8);
        CHECK(r.out.empty());
        CHECK(r.err == std::format("mangadex-dl: error: no MangaDex API client is set up; run "
                                   "'mangadex-dl login' in a terminal, or put client_id and "
                                   "client_secret in {}\n",
                                   h.default_credentials().string()));
        CHECK(!fs::exists(out));
    }
    CHECK(h.seen().empty());
}

// ---- usage errors (AC19, AC59) -----------------------------------------------------------------

TEST_CASE("follows cli: usage errors exit 2 with the follows hint, before any request (AC19)",
          "[cli][follows][usage]") {
    const Harness h{};
    const fs::path out = h.path("out");
    const std::string_view not_a_dir =
        "DIR looks like a MangaDex id; follows takes no title or list (use 'mangadex-dl mdlist "
        "<list>' or 'download <title>')";
    const std::string_view status_words =
        "use reading, on_hold, plan_to_read, dropped, re_reading or completed";

    struct Case {
        std::string_view name{};
        std::function<void(md::RawFollowsArgs&)> edit{};
        std::string message{};
    };
    const std::vector<Case> cases{
        {.name = "DIR empty",
         .edit = [](md::RawFollowsArgs& a) { a.defaults.dir.clear(); },
         .message = "DIR must not be empty"},
        {.name = "DIR a title id",
         .edit = [](md::RawFollowsArgs& a) { a.defaults.dir = std::string{clean_id}; },
         .message = std::string{not_a_dir}},
        {.name = "DIR a list URL",
         .edit =
             [](md::RawFollowsArgs& a) {
                 a.defaults.dir = std::format("https://mangadex.org/list/{}", clean_id);
             },
         .message = std::string{not_a_dir}},
        {.name = "--title",
         .edit = [](md::RawFollowsArgs& a) { a.defaults.title = "hunter2"; },
         .message = "follows has no --title option; to name a title yourself, download it on its "
                    "own with 'mangadex-dl download <title> --title ...'"},
        {.name = "--api-base a custom host",
         .edit = [](md::RawFollowsArgs& a) { a.defaults.api_base = "https://example.org"; },
         .message = "follows logs in only to MangaDex itself; --api-base must be the official API"},
        {.name = "--credentials ''",
         .edit =
             [](md::RawFollowsArgs& a) {
                 a.auth.credentials.clear();
                 a.auth.credentials_given = true;
             },
         .message = "--credentials needs a file name"},
        {.name = "--status all",
         .edit = [](md::RawFollowsArgs& a) { a.status = {"all"}; },
         .message = std::format("--status: unknown reading status 'all'; {}", status_words)},
        {.name = "--status paused",
         .edit = [](md::RawFollowsArgs& a) { a.status = {"reading", "paused"}; },
         .message = std::format("--status: unknown reading status 'paused'; {}", status_words)},
        {.name = "--status ''",
         .edit = [](md::RawFollowsArgs& a) { a.status = {""}; },
         .message = "--status needs a reading status (reading, on_hold, plan_to_read, dropped, "
                    "re_reading or completed)"},
        {.name = "--lang bogus",
         .edit = [](md::RawFollowsArgs& a) { a.defaults.lang = "bogus"; },
         .message = "--lang must be a lowercase language code such as en, pt-br or es-la"},
    };

    for (const Case& c : cases) {
        INFO(c.name);
        for (const bool json : {false, true}) {
            INFO((json ? "--json" : "human"));
            md::RawFollowsArgs args = h.args(out);
            c.edit(args);
            const Captured r = h.run(args, {.globals = {.json = json}});
            CHECK(r.exit_code == 2);
            CHECK(r.out.empty());
            CHECK(r.err == std::format("mangadex-dl: error: {}\n{}", c.message, hint));
            CHECK_THAT(r.err, !ContainsSubstring(std::string{password_dir}));
            CHECK(!fs::exists(out));

            // Before any backend: a fixture root that does not exist changes nothing.
            if (args.defaults.api_base == h.api_base()) {
                args.defaults.api_base = std::string{missing_api_base};
            }
            const Captured own = h.run_own_backend(args, {.globals = {.json = json}});
            CHECK(own.exit_code == 2);
            CHECK(own.out.empty());
            CHECK(own.err == r.err);
        }
    }
    CHECK(h.seen().empty());
    CHECK(!fs::exists(h.state_dir()));
}

TEST_CASE("follows cli: an unknown --status word is echoed terminal-safe and cut to 32 bytes",
          "[cli][follows][usage][status]") {
    const Harness h{};
    const fs::path out = h.path("out");
    struct Case {
        std::string_view name{};
        std::string word{};
        std::string echoed{};
    };
    const std::vector<Case> cases{
        {.name = "an ESC sequence", .word = "\x1b[31mred", .echoed = "?[31mred"},
        {.name = "longer than 32 bytes",
         .word = std::string(32, 's') + "MARKER-after-the-cut",
         .echoed = std::string(32, 's')},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        md::RawFollowsArgs args = h.args(out);
        args.status = {c.word};
        const Captured r = h.run(args);
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == std::format("mangadex-dl: error: --status: unknown reading status '{}'; use "
                                   "reading, on_hold, plan_to_read, dropped, re_reading or "
                                   "completed\n{}",
                                   c.echoed, hint));
        CHECK(!r.err.contains('\x1b'));
        CHECK(!r.err.contains("MARKER"));
    }
    CHECK(h.seen().empty());
}

TEST_CASE("follows cli: an empty --password-stdin is exit 2 before any request (AC19)",
          "[cli][follows][usage]") {
    const Harness h{};
    const fs::path out = h.path("out");
    const md::UniqueFd empty = pipe_with("");
    const bool closed = GENERATE(true, false);
    CAPTURE(closed);
    md::RawFollowsArgs args = h.args(out, "credentials-client");
    args.auth.password_stdin = true;
    const Captured r = h.run(args, {.stdin_fd = closed ? -1 : empty.get()});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == std::format("mangadex-dl: error: --password-stdin was given, but stdin held no "
                               "password\n{}",
                               hint));
    CHECK(h.seen().empty());
    CHECK(!fs::exists(out));
}

// ---- the rev1 gate: a DIR that holds the user's password ---------------------------------------

TEST_CASE("follows cli: a DIR that holds the password is never echoed by the login or the "
          "collection errors",
          "[cli][follows][usage][secret]") {
    struct Case {
        std::string_view name{};
        mdx::FollowsFixture (*tree)() = nullptr;
        std::string_view credentials{}; ///< empty: no --credentials
        std::string_view stdin_text{};
        int exit_code = 0;
        std::string_view stderr_starts{};
    };
    const std::vector<Case> cases{
        {.name = "an empty stdin",
         .tree = &mdx::follows_clean_fixture,
         .credentials = "credentials-client",
         .stdin_text = "",
         .exit_code = 2,
         .stderr_starts = "mangadex-dl: error: --password-stdin was given, but stdin held no "
                          "password\n"},
        {.name = "no session and no terminal",
         .tree = &mdx::follows_clean_fixture,
         .credentials = "",
         .stdin_text = "fixture-password-not-a-secret\n",
         .exit_code = 8,
         .stderr_starts = "mangadex-dl: error: no MangaDex API client is set up; "},
        {.name = "a wrong password through stdin",
         .tree = &mdx::follows_clean_fixture,
         .credentials = "credentials-client",
         .stdin_text = "fixture-wrong-password-not-a-secret\n",
         .exit_code = 9,
         .stderr_starts = "mangadex-dl: error: MangaDex rejected the username or password for "
                          "fixture-user (invalid_grant); "},
        {.name = "a collection failure",
         .tree = &mdx::follows_bad_id_fixture,
         .credentials = "credentials-client",
         .stdin_text = "fixture-password-not-a-secret\n",
         .exit_code = 3,
         .stderr_starts = "mangadex-dl: error: file://"},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const Harness h{c.tree()};
        // As the user typed `follows --password-stdin hunter2`: DIR swallowed the password.
        md::RawFollowsArgs args{};
        args.defaults.dir = std::string{password_dir};
        args.defaults.api_base = h.api_base();
        args.auth.password_stdin = true;
        if (!c.credentials.empty()) {
            args.auth.credentials = h.creds(c.credentials).string();
            args.auth.credentials_given = true;
        }
        for (const bool verbose : {false, true}) {
            INFO((verbose ? "-vv" : "default"));
            const md::UniqueFd stdin_fd = pipe_with(c.stdin_text); // fresh: each run reads it
            const Captured r =
                h.run(args, {.globals = {.verbose = verbose ? 2 : 0}, .stdin_fd = stdin_fd.get()});
            CHECK(r.exit_code == c.exit_code);
            if (!verbose) {
                CHECK_THAT(r.err, StartsWith(std::string{c.stderr_starts}));
            }
            CHECK(r.out.empty());
            INFO("stderr: " << r.err);
            CHECK_THAT(r.err, !ContainsSubstring(std::string{password_dir}));
            check_no_secret(r);
        }
    }
}

// ---- -v and -vv (AC20) -------------------------------------------------------------------------

TEST_CASE("follows cli: -v names the login, the follows count and each item; -vv traces the "
          "requests; no secret anywhere (AC20)",
          "[cli][follows][verbose]") {
    const Harness h{};
    md::RawFollowsArgs args = h.args(h.path("out"));
    args.defaults.dry_run = true;

    const Captured v = h.run(args, {.globals = {.verbose = 1}});
    CHECK(v.exit_code == 0);
    INFO("stderr: " << v.err);
    const std::vector<std::string> lines = lines_of(v.err);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "mangadex-dl: logged in to MangaDex as fixture-user (password)");
    CHECK(lines[1] == "mangadex-dl: follows: 1 title");
    CHECK(lines[2] == std::format("mangadex-dl: follows:1: {}", clean_id));
    check_no_secret(v);

    const Captured vv = h.run(args, {.globals = {.verbose = 2}});
    CHECK(vv.exit_code == 0);
    INFO("stderr -vv: " << vv.err);
    const std::vector<std::string> traced = lines_of(vv.err);
    const auto line_starting = [&traced](const std::string& prefix) {
        return std::ranges::find_if(
            traced, [&prefix](const std::string& line) { return line.starts_with(prefix); });
    };
    // The saved access token of the first run is reused: no token POST, the follows GET traced.
    const auto follows_trace =
        line_starting(std::format("mangadex-dl: GET {} -> 200 (", h.follows_url()));
    const auto count_line = std::ranges::find(traced, "mangadex-dl: follows: 1 title");
    const auto item_line =
        std::ranges::find(traced, std::format("mangadex-dl: follows:1: {}", clean_id));
    CHECK(follows_trace != traced.end());
    CHECK(follows_trace < count_line);
    CHECK(count_line < item_line);
    CHECK(item_line != traced.end());
    check_no_secret(vv);

    SECTION("-vv traces the token POST, never its body") {
        fs::remove(h.session_file());
        const Captured post = h.run(args, {.globals = {.verbose = 2}});
        CHECK(post.exit_code == 0);
        CHECK(post.err.contains(std::format("mangadex-dl: POST {} -> 200 (", h.token_url())));
        CHECK(!post.err.contains("grant_type"));
        check_no_secret(post);
    }
    SECTION("-q wins over -vv: nothing on stderr") {
        const Captured quiet = h.run(args, {.globals = {.quiet = true, .verbose = 2}});
        CHECK(quiet.exit_code == 0);
        CHECK(quiet.err.empty());
    }
}

// ---- interrupts (AC21) -------------------------------------------------------------------------

TEST_CASE("follows cli: an interrupt during the collection fetch prints nothing and writes "
          "nothing (AC21)",
          "[cli][follows][interrupt]") {
    const InterruptFlagReset reset;
    const Harness h{};
    const fs::path out = h.path("out");
    // Runs inside Transport::fetch of the first follows page: the "signal" arrives mid-request,
    // and the intercept waits until the fetch's InterruptBridge has turned it into a stop on the
    // request's token, then answers `cancelled` as a real transport would. Captures a URL copy.
    h.intercept([url = h.follows_url()](
                    const md::HttpRequest& request) -> std::optional<md::TransportResult> {
        if (request.url != url) {
            return std::nullopt;
        }
        md::simulate_interrupt_for_testing(SIGINT);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (!request.cancel.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return std::unexpected(
            md::TransportError{.kind = md::TransportErrc::cancelled, .message = "cancelled"});
    });

    const bool json = GENERATE(false, true);
    CAPTURE(json);
    const Captured r = h.run(h.args(out), {.globals = {.json = json}});
    CHECK(r.exit_code == 1); // main() would re-raise; this is only the fallback
    CHECK(r.out.empty());    // no path, no document
    CHECK(r.err.empty());
    CHECK(!fs::exists(out));
    CHECK(md::interrupt_signal() == SIGINT);
    // The session as last written: the password grant's, saved before the fetch began.
    REQUIRE(fs::is_regular_file(h.session_file()));
    const md::Result<md::Session> saved =
        md::parse_session(read_file(h.session_file()), h.issuer());
    REQUIRE(saved.has_value());
    CHECK(saved->username == mdx::fixture_username);
    CHECK(h.count(h.follows_url()) == 1);
    md::reset_interrupt_for_testing();

    // The handlers were released and the session lock too: a second run works.
    h.intercept(Intercept{});
    const Captured second = h.run(h.args(out));
    CHECK(second.exit_code == 0);
    CHECK(second.out == path_lines(out, {clean_vol1, clean_vol2}));
}

TEST_CASE("follows cli: an interrupt at the password prompt prints nothing and saves nothing "
          "(AC21)",
          "[cli][follows][interrupt][prompt]") {
    const Harness h{};
    const fs::path out = h.path("out");
    FakePrompter prompter{};
    prompter.answer(mdx::fixture_client_id);
    prompter.answer(mdx::fixture_client_secret);
    prompter.answer(mdx::fixture_username);
    prompter.reply(PromptStatus::interrupted);
    const Captured r = h.run(h.args(out, ""),
                             {.globals = {.json = true}, .tty = terminals, .prompter = &prompter});
    CHECK(r.exit_code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.empty());
    CHECK(prompter.calls().size() == 4);
    CHECK(!fs::exists(h.session_file()));
    CHECK(!fs::exists(out));
    CHECK(h.seen().empty());
}

// ---- AC22 (follows half) -----------------------------------------------------------------------

TEST_CASE("follows cli: a session that can't be saved is a warning; the run still exits 0 (AC22)",
          "[cli][follows][auth]") {
    const Harness h{};
    const fs::path out = h.path("out");
    fs::create_directories(h.state_dir().parent_path());
    write_file(h.state_dir(), "not a directory");
    const Captured r = h.run(h.args(out));
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err == std::format("mangadex-dl: warning: could not save the MangaDex login to {}: the "
                               "directory {} is not usable: it is not a directory; the next run "
                               "will need the password again\n",
                               h.session_file().string(), h.state_dir().string()));
    CHECK(read_file(h.state_dir()) == "not a directory");
}

// ---- --status (AC55-AC57) ----------------------------------------------------------------------

TEST_CASE("follows cli: --status reading keeps the reading follow; -v counts and notes (AC55)",
          "[cli][follows][status]") {
    const Harness h{mdx::follows_status_fixture()};
    const fs::path out = h.path("out");
    md::RawFollowsArgs args = h.args(out);
    args.status = {"reading"};

    const Captured r = h.run(args);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(r.err.empty());
    CHECK(h.count(h.follows_url()) == 1);
    CHECK(h.count(h.status_url()) == 1);
    // The follows pages first, then the one status request.
    const std::vector<Seen> seen = h.seen();
    const auto page = std::ranges::find(seen, h.follows_url(), &Seen::url);
    const auto status = std::ranges::find(seen, h.status_url(), &Seen::url);
    CHECK(page < status);
    check_authorization_only_on_collection(h);

    SECTION("-v") {
        md::RawFollowsArgs dry = args;
        dry.defaults.dry_run = true;
        const Captured v = h.run(dry, {.globals = {.verbose = 1}});
        CHECK(v.exit_code == 0);
        INFO("stderr: " << v.err);
        const std::vector<std::string> lines = lines_of(v.err);
        REQUIRE(lines.size() >= 5);
        CHECK(lines[0] == "mangadex-dl: logged in to MangaDex as fixture-user (saved login)");
        CHECK(lines[1] == "mangadex-dl: follows: 4 titles");
        CHECK(lines[2] == "mangadex-dl: follows: 1 of 4 have the reading status reading");
        CHECK(lines[3] == "mangadex-dl: follows: 1 title has a reading status mangadex-dl does "
                          "not know ('paused') and is left out");
        CHECK(lines[4] == std::format("mangadex-dl: follows:1: {}", clean_id));
    }
    SECTION("--json") {
        md::RawFollowsArgs json_args = h.args(h.path("out-json"));
        json_args.status = {"reading"};
        const Captured json = h.run(json_args, {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err.empty());
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("follows") == Json{{"username", mdx::fixture_username},
                                        {"titles", 4},
                                        {"statuses", Json::array({"reading"})},
                                        {"selected", 1}});
        CHECK(doc.at("titles").size() == 1);
        CHECK(doc.at("titles").at(0).at("line") == 1);
        CHECK(doc.at("titles").at(0).at("manga_id") == clean_id);
    }
    SECTION("without --status every follow is an item") {
        const Captured all = h.run(h.args(h.path("out-all")), {.globals = {.json = true}});
        CHECK(all.exit_code == 4); // 6666…, 7777… and 8888… are not on the fixture
        const Json doc = parse_document(all.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("titles").size() == 4);
        CHECK(doc.at("follows").at("statuses") == nullptr);
        CHECK(doc.at("follows").at("selected") == 4);
        CHECK(h.count(h.status_url()) == 1); // only the first run asked for statuses
    }
}

TEST_CASE("follows cli: --status reading,completed keeps follows order; item 1 fails (AC56)",
          "[cli][follows][status]") {
    const Harness h{mdx::follows_status_fixture()};
    const auto words = GENERATE(std::vector<std::string>{"reading,completed"},
                                std::vector<std::string>{"completed", "reading"});
    CAPTURE(words);
    const fs::path out = h.path(std::format("out-{}", words.size()));
    md::RawFollowsArgs args = h.args(out);
    args.status = words;

    const Captured r = h.run(args, {.globals = {.json = true}});
    CHECK(r.exit_code == 4);
    CHECK(r.err == std::format("mangadex-dl: error: follows:1: {}: {}\n"
                               "mangadex-dl: error: 1 of 2 titles did not complete (item 1); see "
                               "the errors above\n",
                               unknown_id, title_not_found(unknown_id)));
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(statuses(doc) == std::vector<std::string>{"not_found", "ok"});
    CHECK(doc.at("titles").at(0).at("manga_id") == unknown_id);
    CHECK(doc.at("titles").at(1).at("manga_id") == clean_id);
    CHECK(doc.at("titles").at(1).at("line") == 2);
    const Json expected_statuses = words.size() == 1 ? Json::array({"reading", "completed"})
                                                     : Json::array({"completed", "reading"});
    CHECK(doc.at("follows") == Json{{"username", mdx::fixture_username},
                                    {"titles", 4},
                                    {"statuses", expected_statuses},
                                    {"selected", 2}});
}

TEST_CASE("follows cli: nothing to run warns, exits 0 and leaves DIR alone (AC57)",
          "[cli][follows][empty]") {
    struct Case {
        std::string_view name{};
        mdx::FollowsFixture (*tree)() = nullptr;
        std::vector<std::string> status{};
        std::string warning{};
        Json follows{};
        bool status_request = false;
        std::string verbose_lines{}; ///< the -v lines after the login line
    };
    const std::vector<Case> cases{
        {.name = "no follows",
         .tree = &mdx::follows_empty_fixture,
         .status = {},
         .warning = "mangadex-dl: warning: you follow no titles on MangaDex\n",
         .follows = Json{{"username", mdx::fixture_username},
                         {"titles", 0},
                         {"statuses", nullptr},
                         {"selected", 0}},
         .status_request = false,
         .verbose_lines = "mangadex-dl: follows: 0 titles\n"},
        {.name = "no follows, --status reading (no status request)",
         .tree = &mdx::follows_empty_fixture,
         .status = {"reading"},
         .warning = "mangadex-dl: warning: you follow no titles on MangaDex\n",
         .follows = Json{{"username", mdx::fixture_username},
                         {"titles", 0},
                         {"statuses", Json::array({"reading"})},
                         {"selected", 0}},
         .status_request = false,
         // Nothing to filter: no "0 of 0" line.
         .verbose_lines = "mangadex-dl: follows: 0 titles\n"},
        {.name = "none matched (FS2, statuses [])",
         .tree = &mdx::follows_status_empty_fixture,
         .status = {"reading", "on-hold"},
         .warning = "mangadex-dl: warning: none of the titles you follow have the reading status "
                    "reading, on_hold\n",
         .follows = Json{{"username", mdx::fixture_username},
                         {"titles", 1},
                         {"statuses", Json::array({"reading", "on_hold"})},
                         {"selected", 0}},
         .status_request = true,
         .verbose_lines = "mangadex-dl: follows: 1 title\n"
                          "mangadex-dl: follows: 0 of 1 have the reading status reading, "
                          "on_hold\n"},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const Harness h{c.tree()};
        const fs::path out = h.path("out");
        md::RawFollowsArgs args = h.args(out);
        args.status = c.status;

        const Captured r = h.run(args);
        CHECK(r.exit_code == 0);
        CHECK(r.out.empty());
        CHECK(r.err == c.warning);
        CHECK(!fs::exists(out));
        CHECK(h.count(h.status_url()) == (c.status_request ? 1U : 0U));

        const Captured quiet = h.run(args, {.globals = {.quiet = true}});
        CHECK(quiet.exit_code == 0);
        CHECK(quiet.err.empty());

        const Captured verbose = h.run(args, {.globals = {.verbose = 1}});
        CHECK(verbose.exit_code == 0);
        CHECK(verbose.err ==
              std::format("mangadex-dl: logged in to MangaDex as fixture-user (saved "
                          "login)\n{}{}",
                          c.verbose_lines, c.warning));

        const Captured json = h.run(args, {.globals = {.json = true}});
        CHECK(json.exit_code == 0);
        CHECK(json.err == c.warning);
        CHECK(top_level_keys(json.out) == document_keys());
        const Json doc = parse_document(json.out);
        REQUIRE(!doc.is_discarded());
        CHECK(doc.at("source") == "follows");
        CHECK(doc.at("follows") == c.follows);
        CHECK(doc.at("titles") == Json::array());
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("follows cli: -v names at most five unknown reading statuses, then counts the rest",
          "[cli][follows][status][verbose]") {
    // The clean title (reading) plus seven followed titles, each with its own unknown status.
    mdx::FollowsFixture fixture = mdx::follows_clean_fixture();
    Json statuses_map = Json::object();
    statuses_map[std::string{clean_id}] = "reading";
    for (std::size_t i = 0; i < 7; ++i) {
        const std::string id = std::format("c0000000-0000-4000-8000-{:012x}", i);
        fixture.follows.push_back(id);
        statuses_map[id] = std::format("unknown_{}", static_cast<char>('a' + i));
    }
    // One more follow with an unknown status already counted: the note counts titles.
    const std::string extra = "c0000000-0000-4000-8000-0000000000ff";
    fixture.follows.push_back(extra);
    statuses_map[extra] = "unknown_a";
    fixture.statuses = statuses_map;

    const Harness h{fixture};
    md::RawFollowsArgs args = h.args(h.path("out"));
    args.status = {"reading"};
    args.defaults.dry_run = true;
    const Captured r = h.run(args, {.globals = {.verbose = 1}});
    CHECK(r.exit_code == 0);
    INFO("stderr: " << r.err);
    const std::vector<std::string> lines = lines_of(r.err);
    REQUIRE(lines.size() >= 4);
    CHECK(lines[1] == "mangadex-dl: follows: 9 titles");
    CHECK(lines[2] == "mangadex-dl: follows: 1 of 9 have the reading status reading");
    CHECK(lines[3] == "mangadex-dl: follows: 8 titles have a reading status mangadex-dl does not "
                      "know ('unknown_a', 'unknown_b', 'unknown_c', 'unknown_d', 'unknown_e' and 2 "
                      "more) and are left out");
}

// ---- collection failures (AC58 and the rest) ---------------------------------------------------

TEST_CASE("follows cli: a malformed collection exits 3 without the network remedy (AC58)",
          "[cli][follows][collection]") {
    struct Case {
        std::string_view name{};
        mdx::FollowsFixture (*tree)() = nullptr;
        std::vector<std::string> status{};
        bool status_document = false;
    };
    const std::vector<Case> cases{
        {.name = "FS3a: a numeric status",
         .tree = &mdx::follows_status_bad_value_fixture,
         .status = {"reading"},
         .status_document = true},
        {.name = "FS3b: a status key ../x",
         .tree = &mdx::follows_status_bad_key_fixture,
         .status = {"reading"},
         .status_document = true},
        {.name = "F5: a followed id ../x",
         .tree = &mdx::follows_bad_id_fixture,
         .status = {},
         .status_document = false},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const Harness h{c.tree()};
        const fs::path out = h.path("out");
        md::RawFollowsArgs args = h.args(out);
        args.status = c.status;
        for (const bool json : {false, true}) {
            INFO((json ? "--json" : "human"));
            const Captured r = h.run(args, {.globals = {.json = json}});
            CHECK(r.exit_code == 3);
            CHECK(r.out.empty());
            INFO("stderr: " << r.err);
            CHECK(lines_of(r.err).size() == 1);
            const std::string url = c.status_document ? h.status_url() : h.follows_url();
            CHECK_THAT(r.err, StartsWith(std::format("mangadex-dl: error: {}: malformed", url)));
            CHECK(!r.err.contains("check your network"));
            CHECK(!r.err.contains("../x")); // never response bytes
            CHECK(!fs::exists(out));
        }
    }
}

TEST_CASE("follows cli: a failed collection request exits 3 with the network remedy",
          "[cli][follows][collection]") {
    const Harness h{};
    const fs::path out = h.path("out");
    h.intercept(answer_url(h.follows_url(), http(503)));
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = h.run(h.args(out), {.globals = {.json = json}});
        CHECK(r.exit_code == 3);
        CHECK(r.out.empty());
        CHECK(r.err == std::format("mangadex-dl: error: {}: HTTP 503 (server error){}\n",
                                   h.follows_url(), remedy));
        CHECK(!fs::exists(out));
    }
}

TEST_CASE("follows cli: a second 401 on the collection exits 9 with the refusal",
          "[cli][follows][collection][auth]") {
    const Harness h{mdx::follows_status_fixture()};
    const fs::path out = h.path("out");
    SECTION("the follows pages") {
        h.intercept(answer_url(h.follows_url(), http(401)));
        const Captured r = h.run(h.args(out), {.globals = {.json = true, .verbose = 1}});
        CHECK(r.exit_code == 9);
        CHECK(r.out.empty());
        CHECK(r.err.ends_with("mangadex-dl: error: MangaDex rejected a new access token for your "
                              "follows (HTTP 401); check that the API client is approved and "
                              "allowed to read your account\n"));
        CHECK(!r.err.contains("login server answered")); // the API's 401, not the login server's
        CHECK(h.count(h.follows_url()) == 2);            // one refresh, one retry
        check_no_secret(r);
    }
    SECTION("the status map") {
        h.intercept(answer_url(h.status_url(), http(401)));
        md::RawFollowsArgs args = h.args(out);
        args.status = {"reading"};
        const Captured r = h.run(args);
        CHECK(r.exit_code == 9);
        CHECK(r.out.empty());
        CHECK(r.err == "mangadex-dl: error: MangaDex rejected a new access token for your reading "
                       "statuses (HTTP 401); check that the API client is approved and allowed to "
                       "read your account\n");
        CHECK(h.count(h.status_url()) == 2);
    }
    CHECK(!fs::exists(out));
}

TEST_CASE("follows cli: a saved login that ends during the fetch, with no password -> 8",
          "[cli][follows][collection][auth]") {
    // The refresh after the API's 401 is answered invalid_grant, and nothing can supply a
    // password: a local failure, exit 8, with the "expired" message.
    mdx::FollowsFixture fixture = mdx::follows_clean_fixture();
    // Every account that shares the default token chain writes that refresh file (the last one
    // wins), so each gets the script.
    for (mdx::FixtureAuth& account : fixture.accounts) {
        account.refresh_scripts = {mdx::FixtureRefreshScript{
            .token = std::string{mdx::fixture_refresh_1},
            .answer = mdx::scripted_error(400, "invalid_grant", "Token is not active")}};
    }
    const Harness h{fixture};
    h.seed(md::Session{
        .issuer = h.issuer(),
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = md::Secret::copy_of(mdx::fixture_client_secret),
        .username = std::string{mdx::fixture_username},
        .refresh_token = token(mdx::fixture_refresh_1),
        .refresh_expires_at = std::nullopt,
        .access_token = token(mdx::fixture_access_1),
        .access_expires_at = unix_now() + 3600,
    });
    h.intercept(answer_url(h.follows_url(), http(401)));
    const fs::path out = h.path("out");
    const Captured r = h.run(h.args(out, "credentials-client"), {.globals = {.json = true}});
    CHECK(r.exit_code == 8);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: your saved MangaDex login has expired or was ended; run "
                   "'mangadex-dl login' in a terminal\n");
    CHECK(!fs::exists(out));
    CHECK(h.count(h.follows_url()) == 1);
    check_no_secret(r);
}

TEST_CASE("follows cli: -v shows what the login server answered when the password fallback "
          "is refused during the fetch",
          "[cli][follows][collection][auth][verbose]") {
    // A saved login for fixture-user-400 (whose wrong password is answered 400), the API's 401 on
    // the follows page, the refresh refused (invalid_grant: the session ended), then the password
    // fallback with the --password-stdin password, refused too: exit 9, and with -v the login
    // server's answer before the error.
    mdx::FollowsFixture fixture = mdx::follows_clean_fixture();
    for (mdx::FixtureAuth& account : fixture.accounts) { // see the "ends during the fetch" test
        account.refresh_scripts = {mdx::FixtureRefreshScript{
            .token = std::string{mdx::fixture_refresh_1},
            .answer = mdx::scripted_error(400, "invalid_grant", "Token is not active")}};
    }
    const Harness h{fixture};
    h.seed(md::Session{
        .issuer = h.issuer(),
        .client_id = std::string{mdx::fixture_client_id},
        .client_secret = md::Secret::copy_of(mdx::fixture_client_secret),
        .username = std::string{mdx::fixture_user_400},
        .refresh_token = token(mdx::fixture_refresh_1),
        .refresh_expires_at = std::nullopt,
        .access_token = token(mdx::fixture_access_1),
        .access_expires_at = unix_now() + 3600,
    });
    h.intercept(answer_url(h.follows_url(), http(401)));
    const fs::path out = h.path("out");
    md::RawFollowsArgs args = h.args(out, "credentials-wrong-password-400");
    args.auth.password_stdin = true;
    const md::UniqueFd stdin_fd =
        pipe_with(std::format("{}\n", mdx::fixture_wrong_password)); // fake, wrong on purpose
    const Captured r =
        h.run(args, {.globals = {.json = true, .verbose = 1}, .stdin_fd = stdin_fd.get()});
    CHECK(r.exit_code == 9);
    CHECK(r.out.empty());
    INFO("stderr: " << r.err);
    CHECK(r.err.ends_with(
        "mangadex-dl: the MangaDex login server answered HTTP 400 with invalid_grant: Invalid "
        "user credentials\n"
        "mangadex-dl: error: MangaDex rejected the username or password for fixture-user-400 "
        "(invalid_grant); check them, or log in on the website to see whether the account is "
        "locked\n"));
    CHECK(r.err.starts_with(
        "mangadex-dl: logged in to MangaDex as fixture-user-400 (saved login)\n"));
    CHECK(h.count(h.follows_url()) == 1);
    CHECK(!fs::exists(out));
    check_no_secret(r);
    CHECK_THAT(r.err, !ContainsSubstring(std::string{mdx::fixture_wrong_password}));
}

// ---- stdout, color and the footer --------------------------------------------------------------

TEST_CASE("follows cli: a stdout write error stops the run after the item that hit it",
          "[cli][follows][output]") {
    const Harness h{mdx::follows_unknown_fixture()};
    const fs::path out = h.path("out");
    std::ofstream full{"/dev/full"};
    REQUIRE(full.is_open());
    std::ostringstream err;
    app::Io io{.out = full, .err = err, .env = h.env()};
    md::RawFollowsArgs args = h.args(out);
    args.defaults.dry_run = true;
    const int rc = app::run_follows_command(args, app::GlobalOptions{}, io);
    // Every item that ran succeeded; main()'s finish_stdout turns the write error into exit 1.
    CHECK(rc == 0);
    CHECK(err.str() ==
          "mangadex-dl: error: stopped at item 1: cannot write to stdout; 1 title not run\n");
    CHECK(!fs::exists(out));
}

TEST_CASE("follows cli: --color=always colors stderr only; stdout and the document have no ESC",
          "[cli][follows][color]") {
    const Harness h{mdx::follows_unknown_fixture()};
    for (const bool json : {false, true}) {
        INFO((json ? "--json" : "human"));
        const Captured r = h.run(h.args(h.path(json ? "out-json" : "out")),
                                 {.globals = {.json = json, .color = app::ColorMode::always},
                                  .tty = {.stdout_tty = true}});
        CHECK(r.exit_code == 4);
        CHECK(!r.out.empty());
        CHECK(!r.out.contains('\x1b'));
        CHECK(r.err.contains(
            std::format("mangadex-dl: \x1b[31merror:\x1b[0m follows:2: {}: ", unknown_id)));
    }
}

TEST_CASE("follows cli: the help footer explains the login and --status, fits 80 columns and "
          "ends with the exit table",
          "[cli][follows][help]") {
    const std::string footer = app::follows_help_footer();
    CHECK(footer.starts_with("LOGIN:\n  Reading your follows needs a MangaDex login through a "));
    CHECK(footer.contains("\nREADING STATUS:\n  --status narrows the titles you follow"));
    CHECK(
        footer.contains("    pass show mangadex | mangadex-dl follows ~/Manga --password-stdin\n"));
    CHECK(footer.contains("  Never put a password on the command line.\n"));
    CHECK(footer.ends_with(app::exit_status_footer()));
    CHECK(!footer.ends_with('\n'));
    for (const std::string& line : lines_of(footer)) {
        INFO(line);
        CHECK(line.size() <= 80);
        CHECK(!line.ends_with(' '));
    }
}

// ---- the registered command (cli.cpp's run()) ---------------------------------------------------

namespace {

/// The whole CLI (run_cli: cli.cpp's run(), which builds its own fixture Backend for the API
/// base), with the harness's environment, `tty` and a closed stdin.
[[nodiscard]] Captured run_registered(const Harness& h, std::initializer_list<std::string> args,
                                      Tty tty = {}) {
    return md::test::run_cli(args, tty, h.env());
}

} // namespace

TEST_CASE("follows cli: the registered command downloads with --credentials, then without",
          "[cli][follows][parse]") {
    const Harness h{};
    const fs::path out = h.path("out");
    const Captured first =
        run_registered(h, {"follows", out.string(), "--credentials",
                           h.creds("credentials").string(), "--api-base", h.api_base()});
    CHECK(first.exit_code == 0);
    CHECK(first.out == path_lines(out, {clean_vol1, clean_vol2}));
    CHECK(first.err.empty());
    CHECK(fs::is_regular_file(out / clean_vol1));
    CHECK(fs::is_regular_file(h.session_file()));
    check_no_secret(first);

    // No --credentials and no default credentials file: the saved login is reused.
    REQUIRE(!fs::exists(h.default_credentials()));
    const fs::path again = h.path("again");
    const Captured second =
        run_registered(h, {"follows", again.string(), "--api-base", h.api_base(), "-v"});
    CHECK(second.exit_code == 0);
    CHECK(second.out == path_lines(again, {clean_vol1, clean_vol2}));
    CHECK_THAT(second.err,
               StartsWith("mangadex-dl: logged in to MangaDex as fixture-user (saved login)\n"));
    CHECK(fs::is_regular_file(again / clean_vol2));
    check_no_secret(second);
}

TEST_CASE("follows cli: the options as CLI11 binds them", "[cli][follows][parse]") {
    const Harness h{};
    const std::string creds = h.creds("credentials").string();
    const std::string base = h.api_base();

    SECTION("DIR defaults to '.'") {
        const Captured r =
            run_registered(h, {"follows", "--dry-run", "--credentials", creds, "--api-base", base});
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(".", {clean_vol1, clean_vol2}));
        CHECK(r.err.empty());
    }
    SECTION("--dir-per-title and --dry-run") {
        const fs::path out = h.path("out");
        const Captured r =
            run_registered(h, {"follows", out.string(), "--dir-per-title", "--dry-run",
                               "--credentials", creds, "--api-base", base});
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out / "Clean Title", {clean_vol1, clean_vol2}));
        CHECK(!fs::exists(out));
    }
    SECTION("--credentials '' is given, but empty -> 2 (V10)") {
        const Captured r = run_registered(h, {"follows", "--credentials", "", "--api-base", base});
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == "mangadex-dl: error: --credentials needs a file name\n" + std::string{hint});
    }
    SECTION("--credentials takes the next word verbatim") {
        const Captured r =
            run_registered(h, {"follows", "--credentials", "--no-input", "--api-base", base});
        CHECK(r.exit_code == 1);
        CHECK(r.out.empty());
        CHECK(r.err == "mangadex-dl: error: credentials file '--no-input' does not exist\n");
    }
    SECTION("--no-input never prompts, even on a terminal -> 8") {
        const Captured r = run_registered(h, {"follows", "--no-input", "--api-base", base},
                                          Tty{.stdin_tty = true, .stderr_tty = true});
        CHECK(r.exit_code == 8);
        CHECK(r.out.empty());
        // stderr is a terminal here, so `error:` is colored.
        CHECK_THAT(r.err, ContainsSubstring("error:\x1b[0m no MangaDex API client is set up; "));
    }
    SECTION("--password-stdin reads stdin (closed here) -> 2") {
        const Captured r = run_registered(
            h, {"follows", "--password-stdin", "--credentials", creds, "--api-base", base});
        CHECK(r.exit_code == 2);
        CHECK(r.err == "mangadex-dl: error: --password-stdin was given, but stdin held no "
                       "password\n" +
                           std::string{hint});
    }
    SECTION("--fail-fast stops after the first failed title") {
        const Harness status{mdx::follows_status_fixture()};
        const Captured r = md::test::run_cli(
            {"follows", status.path("out").string(), "--fail-fast", "--json", "--credentials",
             status.creds("credentials").string(), "--api-base", status.api_base()},
            Tty{}, status.env());
        CHECK(r.exit_code == 4); // 6666… (item 1) is not on the fixture
        const Json doc = parse_document(r.out);
        REQUIRE(!doc.is_discarded());
        CHECK(statuses(doc) ==
              std::vector<std::string>{"not_found", "not_run", "not_run", "not_run"});
    }
    SECTION("no --title and no --password option") {
        CHECK(run_registered(h, {"follows", "--title", "x"}).exit_code == 2);
        CHECK(run_registered(h, {"follows", "--password", "x"}).exit_code == 2);
    }
}
