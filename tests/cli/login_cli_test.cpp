// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/login_cli_test.cpp: the `login` and `logout` commands (follows plan T17), in-process
// through run_login_command / run_logout_command (over a recording fixture backend), report_login,
// and the registered Commands through the whole CLI (run_cli: cli.cpp's run(), its own backend).
//
// Covers AC21 (an interrupt at the password prompt), AC22 (login's half: a session that can't be
// saved is exit 1), AC23 (a fixture login and logout never touch session.json), AC24 (login with
// a prompter; logout sends the revoke and removes the file; logout again; a revoke answered 200
// with an error body; a failed revoke), --json and -q (stdout always empty), the password grant
// even with a valid saved login, the reused client and username, logout's damaged-session,
// insecure-directory, lock and no-HOME paths, an interrupted revoke, the core-dump guard, the
// help footers and the Command wiring (record_auth_options, the hidden --api-base).
//
// Every run uses the fixture transport over a generated follows tree (support/
// mdx_follows_fixture.hpp) in a fresh temporary directory, with HOME and the XDG directories
// inside it, so no test touches the real home or contacts a real host. Every credential is a
// fake from support/mdx_auth_fixture.hpp.
//
// Catch2 test names must not start with '-'. Every name starts with "login cli" so
// `pixi run test -R 'cli\.login cli'` selects them all.

#include "app/cmd_download_output.hpp"
#include "app/cmd_download_run.hpp"
#include "app/cmd_login.hpp"
#include "app/command.hpp"
#include "app/exit_help.hpp"
#include "app/io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
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
#include "support/mdx_follows_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
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

#include <sys/resource.h>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using md::Echo;
using md::PromptStatus;
using md::test::FakePrompter;
using md::test::Tty;
using md::test::fs_util::read_file;
using md::test::fs_util::write_file;

constexpr Tty terminals{.stdin_tty = true, .stdout_tty = true, .stderr_tty = true};

/// The fixture values that must never appear in any output.
constexpr std::array<std::string_view, 8> secrets{
    mdx::fixture_password,          mdx::fixture_client_secret,      mdx::fixture_access_1,
    mdx::fixture_access_2,          mdx::fixture_refresh_1,          mdx::fixture_refresh_2,
    mdx::fixture_access_revoke_503, mdx::fixture_refresh_revoke_503,
};

[[nodiscard]] std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] md::BearerToken token(std::string_view text) {
    md::Result<md::BearerToken> parsed = md::BearerToken::parse(md::Secret::copy_of(text));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] fs::perms mode_of(const fs::path& path) {
    return fs::symlink_status(path).permissions() & fs::perms::mask;
}

/// The fixture transport, recording each request's URL (thread-safe, as Transport requires).
/// With interrupt_revokes(), a revoke POST is "interrupted" in flight: the interrupt flag is set
/// as if SIGINT had arrived, the request waits (at most 10 s) until the InterruptBridge has turned
/// it into a stop request, and the transfer ends as cancelled, as libcurl's would.
class RecordingTransport final : public md::Transport {
public:
    RecordingTransport(std::unique_ptr<md::Transport> inner, std::string revoke_url)
        : inner_(std::move(inner)), revoke_url_(std::move(revoke_url)) {}

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& request) override {
        {
            const std::scoped_lock lock{mutex_};
            urls_.push_back(request.url);
        }
        if (interrupt_revokes_.load() && request.url == revoke_url_) {
            md::simulate_interrupt_for_testing(SIGINT);
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (!request.cancel.stop_requested() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return std::unexpected(
                md::TransportError{.kind = md::TransportErrc::cancelled, .message = "cancelled"});
        }
        return inner_->fetch(request);
    }

    [[nodiscard]] std::vector<std::string> urls() const {
        const std::scoped_lock lock{mutex_};
        return urls_;
    }

    void interrupt_revokes() noexcept { interrupt_revokes_.store(true); }

private:
    std::unique_ptr<md::Transport> inner_{};
    std::string revoke_url_{};
    std::atomic<bool> interrupt_revokes_{false};
    mutable std::mutex mutex_{};
    std::vector<std::string> urls_{};
};

/// A fresh follows-clean tree under a TempDir, HOME and the XDG directories next to it, and a
/// fixture Backend over it whose transport records every request. Not copyable or movable: the
/// Backend is borrowed by every run.
class Harness {
public:
    Harness() : tmp_(md::test::fs_util::make_temp_dir("mangadex-dl-login-cli")) {
        const auto written = mdx::write_follows_tree(tree(), mdx::follows_clean_fixture());
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        md::Result<md::ApiBase> base = md::ApiBase::parse(api_base());
        REQUIRE(base.has_value());
        base_ = std::move(*base);
        md::Result<std::unique_ptr<md::Transport>> inner = md::make_fixture_transport(tree());
        REQUIRE(inner.has_value());
        auto recorder = std::make_unique<RecordingTransport>(std::move(*inner), revoke_url());
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
    [[nodiscard]] std::string api_base() const { return std::format("file://{}", tree().string()); }
    [[nodiscard]] fs::path creds(std::string_view name) const {
        return tree() / "creds" / std::string{name};
    }
    [[nodiscard]] fs::path state_dir() const { return root() / "state" / "mangadex-dl"; }
    [[nodiscard]] fs::path session_file() const {
        return state_dir() / md::session_file_name(md::SessionSlot::fixture);
    }
    [[nodiscard]] fs::path real_session_file() const {
        return state_dir() / md::session_file_name(md::SessionSlot::real);
    }
    [[nodiscard]] app::EnvSnapshot env() const {
        return app::EnvSnapshot{.home = (root() / "home").string(),
                                .xdg_config_home = (root() / "config").string(),
                                .xdg_state_home = (root() / "state").string()};
    }
    [[nodiscard]] const app::Backend& backend() const noexcept { return backend_; }
    [[nodiscard]] std::vector<std::string> requests() const { return recorder_->urls(); }
    [[nodiscard]] std::string token_url() const { return mdx::token_url(tree()); }
    [[nodiscard]] std::string revoke_url() const { return mdx::revoke_url(tree()); }
    void interrupt_revokes() const noexcept { recorder_->interrupt_revokes(); }

    [[nodiscard]] std::string issuer() const {
        const std::optional<md::AuthEndpoints> endpoints = md::auth_endpoints_for(base_);
        REQUIRE(endpoints.has_value());
        return endpoints.has_value() ? endpoints->issuer : std::string{};
    }

    /// A saved fixture-user login with `refresh` and `access` (valid for an hour).
    [[nodiscard]] md::Session session(std::string_view refresh, std::string_view access) const {
        return md::Session{
            .issuer = issuer(),
            .client_id = std::string{mdx::fixture_client_id},
            .client_secret = md::Secret::copy_of(mdx::fixture_client_secret),
            .username = std::string{mdx::fixture_username},
            .refresh_token = token(refresh),
            .refresh_expires_at = std::nullopt,
            .access_token = token(access),
            .access_expires_at = unix_now() + 3600,
        };
    }

    /// Writes `bytes` into `slot` through the store (0700 directory, 0600 file).
    void seed_bytes(md::SessionSlot slot, const md::Secret& bytes) const {
        md::FileResult<md::StateDir> dir = md::StateDir::open(state_dir(), true);
        REQUIRE(dir.has_value());
        const std::unique_ptr<md::Clock> clock = md::make_instant_clock();
        const md::Result<md::SessionLock> lock = dir->lock(*clock, md::LockTiming{}, {});
        REQUIRE(lock.has_value());
        REQUIRE(dir->write(slot, bytes).has_value());
    }

    void seed(const md::Session& saved) const {
        seed_bytes(md::SessionSlot::fixture, md::session_to_json(saved));
    }

    /// The fixture session file, parsed back.
    [[nodiscard]] md::Session read_back() const {
        const std::string bytes = read_file(session_file());
        md::Result<md::Session> parsed = md::parse_session(bytes, issuer());
        INFO((parsed ? std::string{} : parsed.error().message));
        REQUIRE(parsed.has_value());
        return std::move(*parsed);
    }

private:
    md::test::TempDir tmp_; ///< set by the constructor
    md::ApiBase base_{};
    RecordingTransport* recorder_ = nullptr; ///< borrowed from backend_
    app::Backend backend_{};
};

/// How one command is run. The default never colors stderr, so a TTY run prints plain bytes.
struct Setup {
    Tty tty{};
    md::Prompter* prompter = nullptr; ///< borrowed; outlives the run
    app::GlobalOptions globals{.color = app::ColorMode::never};
    std::optional<app::EnvSnapshot> env{}; ///< nullopt: the harness's
};

struct Outcome {
    int exit_code = -1;
    std::string out{};
    std::string err{};

    /// No output carries a fixture secret.
    void check_no_secret() const {
        const std::string all = out + err;
        for (const std::string_view secret : secrets) {
            CHECK_THAT(all, !ContainsSubstring(std::string{secret}));
        }
    }
};

[[nodiscard]] app::Io make_io(const Harness& h, std::ostringstream& out, std::ostringstream& err,
                              const Setup& setup) {
    return app::Io{.out = out,
                   .err = err,
                   .stdin_tty = setup.tty.stdin_tty,
                   .stdout_tty = setup.tty.stdout_tty,
                   .stderr_tty = setup.tty.stderr_tty,
                   .env = setup.env.value_or(h.env()),
                   .stdin_fd = -1,
                   .prompter = setup.prompter};
}

/// run_login_command over the harness's recording backend.
[[nodiscard]] Outcome run_login(const Harness& h, const md::RawLoginArgs& args,
                                const Setup& setup = {}) {
    std::ostringstream out{};
    std::ostringstream err{};
    app::Io io = make_io(h, out, err, setup);
    const int rc = app::run_login_command(args, setup.globals, io, h.backend());
    return Outcome{.exit_code = rc, .out = out.str(), .err = err.str()};
}

/// run_logout_command over the harness's recording backend.
[[nodiscard]] Outcome run_logout(const Harness& h, const Setup& setup = {},
                                 const std::optional<std::string>& api_base = std::nullopt) {
    std::ostringstream out{};
    std::ostringstream err{};
    app::Io io = make_io(h, out, err, setup);
    const md::RawLogoutArgs args{.api_base = api_base.value_or(h.api_base())};
    const int rc = app::run_logout_command(args, setup.globals, io, h.backend());
    return Outcome{.exit_code = rc, .out = out.str(), .err = err.str()};
}

/// The registered Commands through the whole CLI (run_cli: cli.cpp's run(), which builds its own
/// backend), with the harness's environment, no terminal and a closed stdin.
[[nodiscard]] Outcome registered(const Harness& h, std::initializer_list<std::string> args) {
    const md::test::Captured run = md::test::run_cli(args, Tty{}, h.env());
    return Outcome{.exit_code = run.exit_code, .out = run.out, .err = run.err};
}

/// login's arguments for the harness's fixture base, with `--credentials <creds/name>` when given.
[[nodiscard]] md::RawLoginArgs login_args(const Harness& h,
                                          std::optional<std::string_view> creds = std::nullopt) {
    md::RawLoginArgs args{.api_base = h.api_base()};
    if (creds.has_value()) {
        args.auth.credentials = h.creds(*creds).string();
        args.auth.credentials_given = true;
    }
    return args;
}

[[nodiscard]] std::string saved_line(const Harness& h) {
    return std::format("mangadex-dl: logged in to MangaDex as fixture-user; the login is saved in "
                       "{}\n",
                       h.session_file().string());
}

[[nodiscard]] std::string logged_out_line(const Harness& h) {
    return std::format("mangadex-dl: logged out; removed {}\n", h.session_file().string());
}

/// A FakePrompter that answers every detail of the fixture account, in the prompt order.
void answer_everything(FakePrompter& prompter) {
    prompter.answer(mdx::fixture_client_id);
    prompter.answer(mdx::fixture_client_secret);
    prompter.answer(mdx::fixture_username);
    prompter.answer(mdx::fixture_password);
}

/// A prompter interrupted at its first question, as the real one is by Ctrl-C: the interrupt flag
/// is set (main() would re-raise SIGINT) and the reply is `interrupted`.
class InterruptedPrompter final : public md::Prompter {
public:
    void notice(std::string_view /*line*/) override {}

    [[nodiscard]] md::PromptReply ask(std::string_view question, Echo echo) override {
        questions_.emplace_back(question);
        echo_ = echo;
        md::simulate_interrupt_for_testing(SIGINT);
        return md::PromptReply{.status = PromptStatus::interrupted};
    }

    [[nodiscard]] const std::vector<std::string>& questions() const noexcept { return questions_; }
    [[nodiscard]] Echo echo() const noexcept { return echo_; }

private:
    std::vector<std::string> questions_{};
    Echo echo_ = Echo::on;
};

/// Raises the soft RLIMIT_CORE to the hard limit, so a later 0 proves something. False when the
/// hard limit is already 0 (nothing to prove; the caller SKIPs).
[[nodiscard]] bool raise_core_limit() {
    ::rlimit limit{};
    REQUIRE(::getrlimit(RLIMIT_CORE, &limit) == 0);
    if (limit.rlim_max == 0) {
        return false;
    }
    limit.rlim_cur = limit.rlim_max;
    REQUIRE(::setrlimit(RLIMIT_CORE, &limit) == 0);
    return true;
}

[[nodiscard]] ::rlim_t core_limit() {
    ::rlimit limit{};
    REQUIRE(::getrlimit(RLIMIT_CORE, &limit) == 0);
    return limit.rlim_cur;
}

/// Each line of `text` (split at '\n') is at most 80 bytes (the footers are ASCII).
[[nodiscard]] bool fits_80_columns(std::string_view text) {
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        if (line.size() > 80) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return true;
}

} // namespace

// ---- login: success (AC24) ----------------------------------------------------------------------

TEST_CASE("login cli: login with a prompter -> 0, empty stdout, the saved line (AC24)",
          "[login cli]") {
    const Harness h{};
    FakePrompter prompter{};
    answer_everything(prompter);
    const Outcome run = run_login(h, login_args(h), Setup{.tty = terminals, .prompter = &prompter});
    CHECK(run.exit_code == 0);
    CHECK(run.out.empty());
    CHECK(run.err == saved_line(h));
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    REQUIRE(prompter.calls().size() == 4);
    CHECK(
        prompter.calls().back() ==
        FakePrompter::Call{.question = "MangaDex password for fixture-user: ", .echo = Echo::off});
    CHECK(mode_of(h.session_file()) == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK(mode_of(h.state_dir()) == fs::perms::owner_all);
    CHECK_THAT(read_file(h.session_file()), !ContainsSubstring(std::string{mdx::fixture_password}));
    CHECK(h.read_back().username == mdx::fixture_username);
    run.check_no_secret();
}

TEST_CASE("login cli: login always does a password grant, even with a valid saved login",
          "[login cli]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_2, mdx::fixture_access_2));
    const Outcome run = run_login(h, login_args(h, "credentials"));
    CHECK(run.exit_code == 0);
    CHECK(run.err == saved_line(h));
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    const md::Session saved = h.read_back();
    REQUIRE(saved.refresh_token.has_value());
    if (saved.refresh_token.has_value()) {
        CHECK(saved.refresh_token->value().reveal() == mdx::fixture_refresh_1); // the new grant's
    }
    run.check_no_secret();
}

TEST_CASE("login cli: login reuses the saved client and username, asking only for the password",
          "[login cli]") {
    const Harness h{};
    REQUIRE(run_login(h, login_args(h, "credentials")).exit_code == 0);

    FakePrompter prompter{};
    prompter.answer(mdx::fixture_password);
    const Outcome run = run_login(h, login_args(h), Setup{.tty = terminals, .prompter = &prompter});
    CHECK(run.exit_code == 0);
    CHECK(run.err == saved_line(h));
    CHECK(prompter.events() ==
          std::vector<std::string>{"ask (echo off): MangaDex password for fixture-user: "});
    CHECK(h.requests() == std::vector<std::string>{h.token_url(), h.token_url()});
    run.check_no_secret();
}

TEST_CASE("login cli: --json and -q keep stdout empty; -q also stderr", "[login cli]") {
    const Harness h{};
    SECTION("--json") {
        const Outcome run = run_login(
            h, login_args(h, "credentials"),
            Setup{.globals = app::GlobalOptions{.json = true, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK(run.out.empty());
        CHECK(run.err == saved_line(h));
    }
    SECTION("-q") {
        const Outcome run = run_login(
            h, login_args(h, "credentials"),
            Setup{.globals = app::GlobalOptions{.quiet = true, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK(run.out.empty());
        CHECK(run.err.empty());
    }
    CHECK(fs::exists(h.session_file()));
}

TEST_CASE("login cli: -v prints the phase's line, then the saved line; -vv no secret",
          "[login cli]") {
    const Harness h{};
    SECTION("-v") {
        const Outcome run = run_login(
            h, login_args(h, "credentials"),
            Setup{.globals = app::GlobalOptions{.verbose = 1, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK(run.err ==
              "mangadex-dl: logged in to MangaDex as fixture-user (password)\n" + saved_line(h));
    }
    SECTION("-vv") {
        const Outcome run = run_login(
            h, login_args(h, "credentials"),
            Setup{.globals = app::GlobalOptions{.verbose = 2, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK_THAT(run.err,
                   StartsWith(std::format("mangadex-dl: POST {} -> 200 (", h.token_url())));
        CHECK_THAT(run.err, EndsWith(saved_line(h)));
        run.check_no_secret();
    }
}

// ---- login: the session can't be saved (AC22) ---------------------------------------------------

TEST_CASE("login cli: a state directory that is a file -> 1, said once (AC22)", "[login cli]") {
    const Harness h{};
    fs::create_directories(h.state_dir().parent_path());
    write_file(h.state_dir(), "not a directory");
    const Outcome run = run_login(h, login_args(h, "credentials"));
    CHECK(run.exit_code == 1);
    CHECK(run.out.empty());
    CHECK(run.err ==
          std::format("mangadex-dl: error: could not save the MangaDex login to {}: the directory "
                      "{} is not usable: it is not a directory\n",
                      h.session_file().string(), h.state_dir().string()));
    CHECK(read_file(h.state_dir()) == "not a directory");
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    run.check_no_secret();
}

TEST_CASE("login cli: without a home directory the login is not saved -> 1", "[login cli]") {
    const Harness h{};
    const Outcome run =
        run_login(h, login_args(h, "credentials"), Setup{.env = app::EnvSnapshot{}});
    CHECK(run.exit_code == 1);
    CHECK(run.out.empty());
    CHECK(run.err == "mangadex-dl: error: could not save the MangaDex login: there is no home "
                     "directory (HOME is not set to an absolute path)\n");
}

TEST_CASE("login cli: report_login for a saved, an unsynced and a failed save", "[login cli]") {
    std::ostringstream out{};
    std::ostringstream err{};
    app::Io io{.out = out, .err = err};
    const app::OutputPolicy policy{};
    const fs::path path{"/state/mangadex-dl/fixture-session.json"};
    const std::string saved =
        "mangadex-dl: logged in to MangaDex as fixture-user; the login is saved in "
        "/state/mangadex-dl/fixture-session.json\n";

    SECTION("saved") {
        CHECK(app::report_login(io, policy,
                                md::LoginNotes{.username = "fixture-user", .session_path = path}) ==
              0);
        CHECK(err.str() == saved);
    }
    SECTION("unsynced: the line, then a warning, exit 0") {
        CHECK(app::report_login(io, policy,
                                md::LoginNotes{.username = "fixture-user",
                                               .session_path = path,
                                               .save_error =
                                                   "saved, but cannot sync the state directory: "
                                                   "Input/output error"}) == 0);
        CHECK(err.str() ==
              saved + "mangadex-dl: warning: the login was saved in "
                      "/state/mangadex-dl/fixture-session.json, but the directory could not be "
                      "synced; it may not survive a crash\n");
    }
    SECTION("unsynced with -q: nothing") {
        const app::OutputPolicy quiet{.quiet = true};
        CHECK(app::report_login(io, quiet,
                                md::LoginNotes{.username = "fixture-user",
                                               .session_path = path,
                                               .save_error =
                                                   "saved, but cannot sync the state directory: "
                                                   "Input/output error"}) == 0);
        CHECK(err.str().empty());
    }
    SECTION("failed: the error even with -q, exit 1") {
        const app::OutputPolicy quiet{.quiet = true};
        CHECK(app::report_login(io, quiet,
                                md::LoginNotes{.username = "fixture-user",
                                               .session_path = path,
                                               .save_error = "cannot create a temporary file: No "
                                                             "space left on device"}) == 1);
        CHECK(err.str() == "mangadex-dl: error: could not save the MangaDex login to "
                           "/state/mangadex-dl/fixture-session.json: cannot create a temporary "
                           "file: No space left on device\n");
    }
    SECTION("an untrusted username is cut and made terminal-safe") {
        const std::string username = "evil\x1b[2J" + std::string(100, 'u');
        CHECK(app::report_login(io, policy,
                                md::LoginNotes{.username = username, .session_path = path}) == 0);
        CHECK_THAT(err.str(), !ContainsSubstring("\x1b"));
        CHECK(err.str() ==
              std::format("mangadex-dl: logged in to MangaDex as evil?[2J{}; the login is saved "
                          "in /state/mangadex-dl/fixture-session.json\n",
                          std::string(56, 'u')));
    }
    CHECK(out.str().empty());
}

// ---- login: failures before or in the phase -----------------------------------------------------

TEST_CASE("login cli: usage errors are exit 2 before anything is opened", "[login cli]") {
    const Harness h{};
    SECTION("--credentials ''") {
        md::RawLoginArgs args = login_args(h);
        args.auth.credentials_given = true;
        const Outcome run = run_login(h, args);
        CHECK(run.exit_code == 2);
        CHECK(run.err == "mangadex-dl: error: --credentials needs a file name\nRun 'mangadex-dl "
                         "login --help' for more information.\n");
    }
    SECTION("a custom --api-base") {
        md::RawLoginArgs args = login_args(h, "credentials");
        args.api_base = "https://example.org";
        const Outcome run = run_login(h, args);
        CHECK(run.exit_code == 2);
        CHECK(run.err == "mangadex-dl: error: login logs in only to MangaDex itself; --api-base "
                         "must be the official API\nRun 'mangadex-dl login --help' for more "
                         "information.\n");
    }
    SECTION("a malformed --api-base") {
        md::RawLoginArgs args = login_args(h, "credentials");
        args.api_base = "ftp://example.org";
        const Outcome run = run_login(h, args);
        CHECK(run.exit_code == 2);
        CHECK_THAT(run.err, StartsWith("mangadex-dl: error: --api-base must start with"));
        CHECK_THAT(run.err, EndsWith("Run 'mangadex-dl login --help' for more information.\n"));
    }
    SECTION("--password-stdin with a closed stdin") {
        md::RawLoginArgs args = login_args(h, "credentials-client");
        args.auth.password_stdin = true;
        const Outcome run = run_login(h, args);
        CHECK(run.exit_code == 2);
        CHECK(run.err == "mangadex-dl: error: --password-stdin was given, but stdin held no "
                         "password\nRun 'mangadex-dl login --help' for more information.\n");
    }
    CHECK(h.requests().empty());
    CHECK(!fs::exists(h.state_dir()));
}

TEST_CASE("login cli: rejected details are 9, details that can't be asked for are 8",
          "[login cli]") {
    const Harness h{};
    SECTION("a wrong password") {
        const Outcome run = run_login(h, login_args(h, "credentials-wrong-password"));
        CHECK(run.exit_code == 9);
        CHECK(run.out.empty());
        CHECK_THAT(run.err, StartsWith("mangadex-dl: error: MangaDex rejected the username or "
                                       "password for fixture-user (invalid_grant)"));
        CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    }
    SECTION("no terminal and no password") {
        const Outcome run = run_login(h, login_args(h, "credentials-client"));
        CHECK(run.exit_code == 8);
        CHECK(run.out.empty());
        CHECK(run.err == "mangadex-dl: error: you are not logged in to MangaDex; run 'mangadex-dl "
                         "login' in a terminal (or pass --password-stdin)\n");
        CHECK(h.requests().empty());
    }
    CHECK(!fs::exists(h.session_file()));
}

TEST_CASE("login cli: an interrupt at the password prompt prints nothing, saves nothing (AC21)",
          "[login cli]") {
    md::reset_interrupt_for_testing();
    const Harness h{};
    InterruptedPrompter prompter{};
    const Outcome run = run_login(h, login_args(h, "credentials-client"),
                                  Setup{.tty = terminals, .prompter = &prompter});
    CHECK(run.exit_code == md::exit_code_for(md::Errc::interrupted));
    CHECK(md::interrupt_requested()); // main() re-raises SIGINT: 130 for the shell
    CHECK(run.out.empty());
    CHECK(run.err.empty());
    CHECK(prompter.questions() == std::vector<std::string>{"MangaDex password for fixture-user: "});
    CHECK(prompter.echo() == Echo::off);
    CHECK(h.requests().empty());
    CHECK(!fs::exists(h.session_file()));
    md::reset_interrupt_for_testing();
}

// ---- the fixture slot (AC23) --------------------------------------------------------------------

TEST_CASE("login cli: a fixture login and logout never touch session.json (AC23)", "[login cli]") {
    const Harness h{};
    const md::Secret real = md::Secret::copy_of(R"({"not":"a fixture session"})");
    h.seed_bytes(md::SessionSlot::real, real);

    REQUIRE(run_login(h, login_args(h, "credentials")).exit_code == 0);
    CHECK(fs::exists(h.session_file()));
    CHECK(read_file(h.real_session_file()) == real.reveal());

    const Outcome out = run_logout(h);
    CHECK(out.exit_code == 0);
    CHECK(out.err == logged_out_line(h));
    CHECK(!fs::exists(h.session_file()));
    CHECK(read_file(h.real_session_file()) == real.reveal());
}

// ---- logout (AC24) ------------------------------------------------------------------------------

TEST_CASE("login cli: logout revokes and removes; again -> not logged in (AC24)", "[login cli]") {
    const Harness h{};
    REQUIRE(run_login(h, login_args(h, "credentials")).exit_code == 0);

    const Outcome first = run_logout(h);
    CHECK(first.exit_code == 0);
    CHECK(first.out.empty());
    CHECK(first.err == logged_out_line(h));
    CHECK(h.requests() == std::vector<std::string>{h.token_url(), h.revoke_url()});
    CHECK(!fs::exists(h.session_file()));

    const Outcome second = run_logout(h);
    CHECK(second.exit_code == 0);
    CHECK(second.out.empty());
    CHECK(second.err == std::format("mangadex-dl: not logged in (no saved login at {})\n",
                                    h.session_file().string()));
    CHECK(h.requests().size() == 2); // nothing more sent
    first.check_no_secret();
}

TEST_CASE("login cli: logout without a state directory -> not logged in, nothing created",
          "[login cli]") {
    const Harness h{};
    const Outcome run = run_logout(h);
    CHECK(run.exit_code == 0);
    CHECK(run.err == std::format("mangadex-dl: not logged in (no saved login at {})\n",
                                 h.session_file().string()));
    CHECK(!fs::exists(h.state_dir()));
    CHECK(h.requests().empty());
}

TEST_CASE("login cli: a revoke answered 200 with an error body is a clean logout (AC24, V3)",
          "[login cli]") {
    const Harness h{};
    // No revoke file for this refresh token: the fixture answers Keycloak's 200
    // {"error":"invalid_token"}.
    h.seed(h.session("fixture-refresh-unknown", mdx::fixture_access_1));
    const Outcome run = run_logout(h);
    CHECK(run.exit_code == 0);
    CHECK(run.err == logged_out_line(h));
    CHECK(h.requests() == std::vector<std::string>{h.revoke_url()});
    CHECK(!fs::exists(h.session_file()));
}

TEST_CASE("login cli: a failed revoke warns, and the file is removed anyway (AC24)",
          "[login cli]") {
    const Harness h{};
    REQUIRE(run_login(h, login_args(h, "credentials-revoke-503")).exit_code == 0);
    const Outcome run = run_logout(h);
    CHECK(run.exit_code == 0);
    CHECK(run.out.empty());
    CHECK_THAT(run.err, StartsWith(std::format("mangadex-dl: warning: could not end the session "
                                               "at MangaDex ({}: HTTP 503",
                                               h.revoke_url())));
    CHECK_THAT(run.err, EndsWith("); the saved login was removed anyway\n" + logged_out_line(h)));
    CHECK(!fs::exists(h.session_file()));
    run.check_no_secret();
}

TEST_CASE("login cli: an unreadable saved login is removed with a warning, no revoke",
          "[login cli]") {
    const Harness h{};
    h.seed_bytes(md::SessionSlot::fixture, md::Secret::copy_of("not json"));
    const Outcome run = run_logout(h);
    CHECK(run.exit_code == 0);
    CHECK(run.err == "mangadex-dl: warning: could not end the session at MangaDex (the saved "
                     "login is unreadable); the saved login was removed anyway\n" +
                         logged_out_line(h));
    CHECK(h.requests().empty());
    CHECK(!fs::exists(h.session_file()));
}

TEST_CASE("login cli: logout without a home directory -> not logged in", "[login cli]") {
    const Harness h{};
    const Outcome run = run_logout(h, Setup{.env = app::EnvSnapshot{}});
    CHECK(run.exit_code == 0);
    CHECK(run.out.empty());
    CHECK(run.err ==
          "mangadex-dl: not logged in (no saved login: HOME is not set to an absolute path)\n");
    CHECK(h.requests().empty());
}

TEST_CASE("login cli: logout with an insecure state directory -> 1, nothing touched",
          "[login cli]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1));
    const std::string before = read_file(h.session_file());
    fs::permissions(h.state_dir(), fs::perms::all, fs::perm_options::replace);
    const Outcome run = run_logout(h);
    fs::permissions(h.state_dir(), fs::perms::owner_all, fs::perm_options::replace);
    CHECK(run.exit_code == 1);
    CHECK(run.out.empty());
    CHECK_THAT(run.err,
               StartsWith(std::format("mangadex-dl: error: could not remove the saved login at {}: "
                                      "the directory {} is not usable: ",
                                      h.session_file().string(), h.state_dir().string())));
    CHECK(read_file(h.session_file()) == before);
    CHECK(h.requests().empty());
}

TEST_CASE("login cli: logout with the session lock held elsewhere -> 1 at once", "[login cli]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1));
    // A second open file description holds the lock (flock is per description, V16); the fixture
    // backend's instant clock makes the 60 s wait time out at once.
    const md::FileResult<md::StateDir> dir = md::StateDir::open(h.state_dir(), false);
    REQUIRE(dir.has_value());
    const std::unique_ptr<md::Clock> clock = md::make_instant_clock();
    const md::Result<md::SessionLock> held = dir->lock(*clock, md::LockTiming{}, {});
    REQUIRE(held.has_value());

    const Outcome run = run_logout(h);
    CHECK(run.exit_code == 1);
    CHECK(run.out.empty());
    CHECK(run.err ==
          "mangadex-dl: error: another mangadex-dl is using the saved login; try again\n");
    CHECK(fs::exists(h.session_file()));
    CHECK(h.requests().empty());
}

TEST_CASE("login cli: an interrupted revoke prints nothing and keeps the file", "[login cli]") {
    md::reset_interrupt_for_testing();
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1));
    const std::string before = read_file(h.session_file());
    h.interrupt_revokes();
    const Outcome run = run_logout(h);
    CHECK(run.exit_code == md::exit_code_for(md::Errc::interrupted));
    CHECK(md::interrupt_requested()); // main() re-raises SIGINT: 130 for the shell
    CHECK(run.out.empty());
    CHECK(run.err.empty());
    CHECK(h.requests() == std::vector<std::string>{h.revoke_url()});
    CHECK(read_file(h.session_file()) == before);
    md::reset_interrupt_for_testing();
}

TEST_CASE("login cli: logout --api-base must be the official API or a fixture -> 2",
          "[login cli]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1));
    const Outcome run = run_logout(h, Setup{}, "https://example.org");
    CHECK(run.exit_code == 2);
    CHECK(run.out.empty());
    CHECK(run.err == "mangadex-dl: error: logout logs in only to MangaDex itself; --api-base must "
                     "be the official API\nRun 'mangadex-dl logout --help' for more "
                     "information.\n");
    CHECK(fs::exists(h.session_file()));
    CHECK(h.requests().empty());
}

TEST_CASE("login cli: logout with --json, -q and -vv", "[login cli]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1));
    SECTION("--json: stdout stays empty") {
        const Outcome run = run_logout(
            h, Setup{.globals = app::GlobalOptions{.json = true, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK(run.out.empty());
        CHECK(run.err == logged_out_line(h));
    }
    SECTION("-q: nothing at all") {
        const Outcome run = run_logout(
            h, Setup{.globals = app::GlobalOptions{.quiet = true, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK(run.out.empty());
        CHECK(run.err.empty());
    }
    SECTION("-vv: the revoke POST is traced, never a secret") {
        const Outcome run = run_logout(
            h, Setup{.globals = app::GlobalOptions{.verbose = 2, .color = app::ColorMode::never}});
        CHECK(run.exit_code == 0);
        CHECK_THAT(run.err,
                   StartsWith(std::format("mangadex-dl: POST {} -> 200 (", h.revoke_url())));
        CHECK_THAT(run.err, EndsWith(logged_out_line(h)));
        run.check_no_secret();
    }
    CHECK(!fs::exists(h.session_file()));
}

TEST_CASE("login cli: logout prints its paths terminal-safe", "[login cli]") {
    const Harness h{};
    app::EnvSnapshot env = h.env();
    env.xdg_state_home = (h.root() / "st\x1b]0;x\x07"
                                     "ate")
                             .string();
    const Outcome run = run_logout(h, Setup{.env = env});
    CHECK(run.exit_code == 0);
    CHECK_THAT(run.err, !ContainsSubstring("\x1b"));
    CHECK_THAT(run.err, !ContainsSubstring("\x07"));
    CHECK_THAT(run.err, ContainsSubstring("st?]0;x?ate/mangadex-dl/fixture-session.json)"));
}

// ---- the core-dump guard ------------------------------------------------------------------------

TEST_CASE("login cli: logout turns core dumps off", "[login cli]") {
    const Harness h{}; // the fixture tree is written before the soft limit is raised
    if (!raise_core_limit()) {
        SKIP("the hard RLIMIT_CORE is already 0: nothing to turn off");
    }
    REQUIRE(core_limit() != 0);
    const Outcome run = run_logout(h);
    CHECK(run.exit_code == 0);
    CHECK(core_limit() == 0);
}

TEST_CASE("login cli: login turns core dumps off", "[login cli]") {
    const Harness h{}; // the fixture tree is written before the soft limit is raised
    if (!raise_core_limit()) {
        SKIP("the hard RLIMIT_CORE is already 0: nothing to turn off");
    }
    REQUIRE(core_limit() != 0);
    const Outcome run = run_login(h, login_args(h, "credentials"));
    CHECK(run.exit_code == 0);
    CHECK(core_limit() == 0);
}

// ---- the registered Commands and their help -----------------------------------------------------

TEST_CASE("login cli: the registered login records --credentials and takes --api-base",
          "[login cli]") {
    const Harness h{};
    SECTION("--credentials FILE is read (credentials_given recorded after parse)") {
        const Outcome run =
            registered(h, {"login", "--credentials", h.creds("credentials").string(), "--api-base",
                           h.api_base()});
        CHECK(run.exit_code == 0);
        CHECK(run.out.empty());
        CHECK(run.err == saved_line(h));
        CHECK(fs::exists(h.session_file()));

        const Outcome out = registered(h, {"logout", "--api-base", h.api_base()});
        CHECK(out.exit_code == 0);
        CHECK(out.err == logged_out_line(h));
        CHECK(!fs::exists(h.session_file()));
    }
    SECTION("--credentials '' is given but empty -> 2") {
        const Outcome run =
            registered(h, {"login", "--credentials", "", "--api-base", h.api_base()});
        CHECK(run.exit_code == 2);
        CHECK(run.err == "mangadex-dl: error: --credentials needs a file name\nRun 'mangadex-dl "
                         "login --help' for more information.\n");
    }
    SECTION("an extra argument is a parse error -> 2") {
        CHECK(registered(h, {"login", "extra", "--api-base", h.api_base()}).exit_code == 2);
        CHECK(registered(h, {"logout", "extra", "--api-base", h.api_base()}).exit_code == 2);
        CHECK(registered(h, {"logout", "--credentials", "x"}).exit_code == 2);
    }
    CHECK(h.requests().empty()); // the registered commands use their own backend
}

TEST_CASE("login cli: login and logout --help show the footers, never --api-base", "[login cli]") {
    const Harness h{};
    SECTION("login") {
        const Outcome run = registered(h, {"login", "--help"});
        CHECK(run.exit_code == 0);
        CHECK(run.err.empty());
        CHECK_THAT(run.out, ContainsSubstring("Log in to MangaDex and save the login for follows"));
        CHECK_THAT(run.out, ContainsSubstring("--credentials FILE"));
        CHECK_THAT(run.out, ContainsSubstring("--password-stdin"));
        CHECK_THAT(run.out, ContainsSubstring("--no-input"));
        CHECK_THAT(run.out, ContainsSubstring(app::login_help_footer()));
        CHECK_THAT(run.out, !ContainsSubstring("--api-base"));
    }
    SECTION("logout") {
        const Outcome run = registered(h, {"logout", "--help"});
        CHECK(run.exit_code == 0);
        CHECK(run.err.empty());
        CHECK_THAT(run.out, ContainsSubstring("Remove the saved MangaDex login"));
        CHECK_THAT(run.out, ContainsSubstring(app::logout_help_footer()));
        CHECK_THAT(run.out, !ContainsSubstring("--api-base"));
        CHECK_THAT(run.out, !ContainsSubstring("--credentials"));
    }
}

TEST_CASE("login cli: the help footers fit 80 columns and end with the exit-status table",
          "[login cli]") {
    for (const std::string& footer : {app::login_help_footer(), app::logout_help_footer()}) {
        CHECK(fits_80_columns(footer));
        CHECK_THAT(footer, EndsWith(app::exit_status_footer()));
        CHECK_THAT(footer, !EndsWith("\n"));
        CHECK_THAT(footer, !ContainsSubstring(" \n")); // no trailing spaces
    }
    CHECK_THAT(app::login_help_footer(),
               ContainsSubstring("To switch accounts, run 'mangadex-dl logout' first, or use "
                                 "--credentials\nwith the other account.\n"));
    CHECK_THAT(app::login_help_footer(),
               ContainsSubstring("Nothing is written to stdout, also with --json."));
    CHECK_THAT(app::logout_help_footer(),
               ContainsSubstring("Nothing is written to stdout, also with --json."));
}
