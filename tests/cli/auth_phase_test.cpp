// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/auth_phase_test.cpp: the login glue `follows` and `login` share (follows plan T16),
// in-process through run_login_phase, close_login_phase and the auth options, without a
// registered command (registration is T19's).
//
// Covers AC14 (no way to log in -> 8, no request), AC15 (--no-input never prompts), AC16 (the
// credentials file: 0644, all four keys, the default path, a missing explicit path), AC17 (wrong
// details -> 9, the saved login unchanged), AC18 (a refresh answered invalid_grant), AC22 (the
// phase's half: a save failure is a warning for follows and left to the command for login), AC25
// (an insecure state directory, a symlinked session), AC26 (a lock held elsewhere -> 1 at once),
// the TTY rule, --password-stdin through a pipe, --credentials '' and its verbatim value (V10,
// V36), the API-base check before any credential is touched, the late warnings drained before
// close(), and terminal-safe printing of untrusted text (paths, usernames).
//
// Every run uses the fixture transport over a generated follows tree (support/
// mdx_follows_fixture.hpp) in a fresh temporary directory, with HOME and the XDG directories
// inside it, so no test touches the real home or contacts a real host. Every credential is a
// fake from support/mdx_auth_fixture.hpp.
//
// Catch2 test names must not start with '-'. Every name starts with "auth phase" so
// `pixi run test -R 'cli\.auth phase'` selects them all.

#include "app/cmd_auth.hpp"
#include "app/cmd_download_output.hpp"
#include "app/cmd_download_run.hpp"
#include "app/cmd_trace.hpp"
#include "app/io.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
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
#include "support/mdx_follows_fixture.hpp"
#include "support/pty_pair.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using md::Echo;
using md::LoginHow;
using md::LoginMode;
using md::PromptStatus;
using md::test::FakePrompter;
using md::test::Tty;
using md::test::fs_util::read_file;
using md::test::fs_util::write_file;

constexpr std::string_view hint = "Run 'mangadex-dl follows --help' for more information.\n";
constexpr std::string_view not_logged_in_line =
    "mangadex-dl: error: you are not logged in to MangaDex; run 'mangadex-dl login' in a "
    "terminal (or pass --password-stdin)\n";
constexpr std::string_view expired_line =
    "mangadex-dl: error: your saved MangaDex login has expired or was ended; run 'mangadex-dl "
    "login' in a terminal\n";
constexpr std::string_view lock_timeout_line =
    "mangadex-dl: error: another mangadex-dl is using the saved login; try again\n";
constexpr std::string_view hint_notice =
    "mangadex-dl: reading your follows needs a MangaDex personal API client; create one at "
    "https://mangadex.org/settings (API Clients)";

constexpr Tty terminals{.stdin_tty = true, .stdout_tty = true, .stderr_tty = true};

/// The fixture values that must never appear in any output.
constexpr std::array<std::string_view, 6> secrets{
    mdx::fixture_password, mdx::fixture_client_secret, mdx::fixture_access_1,
    mdx::fixture_access_2, mdx::fixture_refresh_1,     mdx::fixture_refresh_2,
};

/// U+202E RIGHT-TO-LEFT OVERRIDE (a bidi control terminal_safe must neutralise in a username or a
/// server text), built from its UTF-8 bytes at run time so that no source literal holds one.
[[nodiscard]] std::string rlo() { return std::string{'\xE2', '\x80', '\xAE'}; }

/// The value of `optional`, which the caller has just REQUIREd to be engaged. An empty one (a
/// test bug) throws, which Catch2 reports; unlike REQUIRE, the check is visible to
/// bugprone-unchecked-optional-access.
template <class T> [[nodiscard]] const T& engaged(const std::optional<T>& optional) {
    if (!optional.has_value()) {
        throw std::logic_error("engaged(): the optional is empty");
    }
    return *optional;
}

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

/// The fixture transport, recording each request's URL (thread-safe, as Transport requires).
class RecordingTransport final : public md::Transport {
public:
    explicit RecordingTransport(std::unique_ptr<md::Transport> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] md::TransportResult fetch(const md::HttpRequest& request) override {
        {
            const std::scoped_lock lock{mutex_};
            urls_.push_back(request.url);
        }
        return inner_->fetch(request);
    }

    [[nodiscard]] std::vector<std::string> urls() const {
        const std::scoped_lock lock{mutex_};
        return urls_;
    }

private:
    std::unique_ptr<md::Transport> inner_{};
    mutable std::mutex mutex_{};
    std::vector<std::string> urls_{};
};

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

/// What is left to read on `fd` (up to 4 KiB).
[[nodiscard]] std::string drain(int fd) {
    std::string bytes(4096, '\0');
    const ::ssize_t got = ::read(fd, bytes.data(), bytes.size());
    REQUIRE(got >= 0);
    bytes.resize(static_cast<std::size_t>(got));
    return bytes;
}

/// A file with exactly `mode` (write_file, then chmod: the umask can't widen or narrow it).
void write_private_file(const fs::path& path, std::string_view content, fs::perms mode) {
    fs::create_directories(path.parent_path());
    write_file(path, content);
    fs::permissions(path, mode, fs::perm_options::replace);
}

/// A fresh follows-clean tree under a TempDir, HOME and the XDG directories next to it, and a
/// fixture Backend over it whose transport records every request. Not copyable or movable: the
/// Backend is borrowed by every Run.
class Harness {
public:
    Harness() : tmp_(md::test::fs_util::make_temp_dir("mangadex-dl-auth-phase")) {
        const auto written = mdx::write_follows_tree(tree(), mdx::follows_clean_fixture());
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        md::Result<md::ApiBase> base =
            md::ApiBase::parse(std::format("file://{}", tree().string()));
        REQUIRE(base.has_value());
        base_ = std::move(*base);
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
    [[nodiscard]] const md::ApiBase& base() const noexcept { return base_; }
    [[nodiscard]] const app::Backend& backend() const noexcept { return backend_; }
    [[nodiscard]] std::vector<std::string> requests() const { return recorder_->urls(); }
    [[nodiscard]] std::string token_url() const { return mdx::token_url(tree()); }

    [[nodiscard]] std::string issuer() const {
        const std::optional<md::AuthEndpoints> endpoints = md::auth_endpoints_for(base_);
        REQUIRE(endpoints.has_value());
        return engaged(endpoints).issuer;
    }

    /// A saved fixture-user login with `refresh` and `access` (expiring at `access_expires_at`).
    [[nodiscard]] md::Session session(std::string_view refresh, std::string_view access,
                                      std::int64_t access_expires_at,
                                      std::string_view username = mdx::fixture_username) const {
        return md::Session{
            .issuer = issuer(),
            .client_id = std::string{mdx::fixture_client_id},
            .client_secret = md::Secret::copy_of(mdx::fixture_client_secret),
            .username = std::string{username},
            .refresh_token = token(refresh),
            .refresh_expires_at = std::nullopt,
            .access_token = token(access),
            .access_expires_at = access_expires_at,
        };
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

/// How one login phase is run.
struct Setup {
    Tty tty{};
    int stdin_fd = -1; ///< borrowed; -1 = a closed stdin
    md::Prompter* prompter = nullptr;
    int verbose = 0;
    bool quiet = false;
    LoginMode mode = LoginMode::reuse;
    std::string_view command = "follows";
    std::optional<app::EnvSnapshot> env{}; ///< nullopt: the harness's
    std::optional<md::ApiBase> api_base{}; ///< nullopt: the harness's fixture base
};

/// One run_login_phase call, with its streams, Io, policy and trace kept alive next to the phase
/// it returned (the session borrows the trace, which borrows the Io). Members are destroyed in
/// reverse order, so the phase goes first. Not copyable or movable.
class Run {
public:
    Run(const Harness& harness, const md::RawAuthArgs& args, const Setup& setup = {})
        : io_{.out = out_,
              .err = err_,
              .stdin_tty = setup.tty.stdin_tty,
              .stdout_tty = setup.tty.stdout_tty,
              .stderr_tty = setup.tty.stderr_tty,
              .env = setup.env.value_or(harness.env()),
              .stdin_fd = setup.stdin_fd,
              .prompter = setup.prompter},
          policy_{.quiet = setup.quiet, .verbose = setup.verbose}, trace_{io_, policy_},
          phase_(app::run_login_phase(args, setup.api_base.value_or(harness.base()), setup.mode,
                                      io_, policy_, harness.backend(), trace_, setup.command)) {}
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    Run(Run&&) = delete;
    Run& operator=(Run&&) = delete;
    ~Run() = default;

    [[nodiscard]] int exit_code() const { return phase_.has_value() ? 0 : phase_.error(); }
    [[nodiscard]] std::string out() const { return out_.str(); }
    [[nodiscard]] std::string err() const { return err_.str(); }

    /// The phase; the caller has checked exit_code() == 0.
    [[nodiscard]] app::LoginPhase& phase() {
        REQUIRE(phase_.has_value());
        return *phase_;
    }

    void close() {
        REQUIRE(phase_.has_value());
        app::close_login_phase(*phase_, io_, policy_);
    }

    /// No output carries a fixture secret.
    void check_no_secret() const {
        const std::string all = out() + err();
        for (const std::string_view secret : secrets) {
            CHECK_THAT(all, !ContainsSubstring(std::string{secret}));
        }
    }

private:
    std::ostringstream out_{};
    std::ostringstream err_{};
    app::Io io_;
    app::OutputPolicy policy_{};
    app::StderrTrace trace_;
    std::expected<app::LoginPhase, int> phase_;
};

[[nodiscard]] md::RawAuthArgs with_credentials(const fs::path& path) {
    return md::RawAuthArgs{.credentials = path.string(), .credentials_given = true};
}

/// A FakePrompter that answers every detail of the fixture account, in the prompt order.
void answer_everything(FakePrompter& prompter) {
    prompter.answer(mdx::fixture_client_id);
    prompter.answer(mdx::fixture_client_secret);
    prompter.answer(mdx::fixture_username);
    prompter.answer(mdx::fixture_password);
}

/// A pty pair, or a SKIP when the system has no pseudo-terminals (posix_openpt fails, as in some
/// CI containers); any later step failing is a real failure.
[[nodiscard]] md::test::PtyPair require_pty() {
    md::test::PtyPair pty = md::test::open_pty_pair();
    if (!pty.master.valid()) {
        SKIP("no pseudo-terminal: " << pty.failure);
    }
    INFO(pty.failure);
    REQUIRE(pty.slave.valid());
    return pty;
}

/// Types `line` into the pty master once the terminal's ECHO flag is off (the prompter clears it
/// with TCSAFLUSH before asking, which would discard anything typed earlier). No Catch2 macros on
/// the thread. Gives up after 10 s and simulates an interrupt, so ask() returns and the test
/// fails instead of hanging. `master` is borrowed: declare the PtyPair before the Typist, so the
/// thread joins (thread_ is the last member) before the descriptor closes.
class Typist {
public:
    Typist(int master, std::string line)
        : thread_{[master, text = std::move(line), timed_out = &timed_out_, wrote = &wrote_] {
              type(master, text, *timed_out, *wrote);
          }} {}
    Typist(const Typist&) = delete;
    Typist& operator=(const Typist&) = delete;
    Typist(Typist&&) = delete;
    Typist& operator=(Typist&&) = delete;
    ~Typist() = default;

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] bool timed_out() const noexcept { return timed_out_.load(); }
    [[nodiscard]] bool wrote() const noexcept { return wrote_.load(); }

private:
    static void type(int master, std::string_view text, std::atomic<bool>& timed_out,
                     std::atomic<bool>& wrote) noexcept {
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (echo_on(master)) {
            if (std::chrono::steady_clock::now() > give_up) {
                timed_out.store(true);
                md::simulate_interrupt_for_testing(SIGINT);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        while (!text.empty()) {
            const ::ssize_t got = ::write(master, text.data(), text.size());
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            text.remove_prefix(static_cast<std::size_t>(got));
        }
        wrote.store(true);
    }

    /// A failed tcgetattr reads as "echo on".
    [[nodiscard]] static bool echo_on(int fd) noexcept {
        ::termios attributes{};
        if (::tcgetattr(fd, &attributes) != 0) {
            return true;
        }
        return (attributes.c_lflag & static_cast<::tcflag_t>(ECHO)) != 0;
    }

    std::atomic<bool> timed_out_{false};
    std::atomic<bool> wrote_{false};
    std::jthread thread_{};
};

[[nodiscard]] std::string no_client_line(const fs::path& credentials) {
    return std::format("mangadex-dl: error: no MangaDex API client is set up; run 'mangadex-dl "
                       "login' in a terminal, or put client_id and client_secret in {}\n",
                       credentials.string());
}

} // namespace

// ---- AC14, AC15 and the TTY rule
// -----------------------------------------------------------------

TEST_CASE("auth phase: no terminal, no session and no credentials -> 8, nothing sent (AC14)",
          "[auth phase]") {
    const Harness h{};
    const Run run{h, md::RawAuthArgs{}};
    CHECK(run.exit_code() == 8);
    CHECK(run.out().empty());
    CHECK(run.err() == no_client_line(h.default_credentials()));
    CHECK(h.requests().empty());
    CHECK(!fs::exists(h.session_file()));
}

TEST_CASE("auth phase: a client but no password and no way to ask -> 8 'not logged in'",
          "[auth phase]") {
    const Harness h{};
    const Run run{h, with_credentials(h.creds("credentials-client"))};
    CHECK(run.exit_code() == 8);
    CHECK(run.out().empty());
    CHECK(run.err() == not_logged_in_line);
    CHECK(h.requests().empty());
}

TEST_CASE("auth phase: --no-input with terminals never prompts (AC15)", "[auth phase]") {
    const Harness h{};
    FakePrompter prompter{};
    answer_everything(prompter);
    const Run run{h, md::RawAuthArgs{.no_input = true},
                  Setup{.tty = terminals, .prompter = &prompter}};
    CHECK(run.exit_code() == 8);
    CHECK(run.err() == no_client_line(h.default_credentials()));
    CHECK(prompter.events().empty());
    CHECK(h.requests().empty());
}

TEST_CASE("auth phase: no prompt unless stdin and stderr are terminals and stdin is free",
          "[auth phase]") {
    const Harness h{};
    FakePrompter prompter{};
    answer_everything(prompter);
    const auto [tty, no_input] = GENERATE(
        std::pair{Tty{.stdin_tty = false, .stdout_tty = true, .stderr_tty = true}, false},
        std::pair{Tty{.stdin_tty = true, .stdout_tty = true, .stderr_tty = false}, false},
        std::pair{Tty{.stdin_tty = false, .stdout_tty = false, .stderr_tty = false}, false},
        std::pair{terminals, true});
    CAPTURE(tty.stdin_tty, tty.stderr_tty, no_input);
    const Run run{h, md::RawAuthArgs{.no_input = no_input},
                  Setup{.tty = tty, .prompter = &prompter}};
    CHECK(run.exit_code() == 8);
    CHECK(prompter.events().empty());
    CHECK(h.requests().empty());
}

TEST_CASE("auth phase: with both terminals the prompter asks for every detail in order",
          "[auth phase]") {
    const Harness h{};
    FakePrompter prompter{};
    answer_everything(prompter);
    Run run{h, md::RawAuthArgs{}, Setup{.tty = terminals, .prompter = &prompter}};
    REQUIRE(run.exit_code() == 0);
    CHECK(run.out().empty());
    CHECK(run.err().empty());
    CHECK(prompter.events() == std::vector<std::string>{
                                   std::format("notice: {}", hint_notice),
                                   "ask (echo on): MangaDex API client id: ",
                                   "ask (echo off): MangaDex API client secret: ",
                                   "ask (echo on): MangaDex username: ",
                                   "ask (echo off): MangaDex password for fixture-user: ",
                               });
    CHECK(run.phase().notes.how == LoginHow::password);
    CHECK(run.phase().notes.username == mdx::fixture_username);
    CHECK(run.phase().prompter == nullptr); // the injected one is borrowed, never owned
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    const std::string saved = read_file(h.session_file());
    CHECK_THAT(saved, !ContainsSubstring(std::string{mdx::fixture_password}));
    run.check_no_secret();
}

TEST_CASE("auth phase: --password-stdin reads the password from a pipe, never prompting",
          "[auth phase]") {
    const Harness h{};
    FakePrompter prompter{};
    answer_everything(prompter);
    const md::UniqueFd stdin_fd = pipe_with(std::format("{}\n", mdx::fixture_password));
    md::RawAuthArgs args = with_credentials(h.creds("credentials-client"));
    args.password_stdin = true;
    Run run{h, args, Setup{.tty = terminals, .stdin_fd = stdin_fd.get(), .prompter = &prompter}};
    REQUIRE(run.exit_code() == 0);
    CHECK(run.err().empty());
    CHECK(prompter.events().empty());
    CHECK(run.phase().notes.how == LoginHow::password);
    CHECK(h.read_back().username == mdx::fixture_username);
    CHECK_THAT(read_file(h.session_file()), !ContainsSubstring(std::string{mdx::fixture_password}));
    run.check_no_secret();
}

TEST_CASE("auth phase: an empty or closed --password-stdin is exit 2 before any request",
          "[auth phase]") {
    const Harness h{};
    const md::UniqueFd empty = pipe_with("");
    const bool closed = GENERATE(true, false);
    CAPTURE(closed);
    const int fd = closed ? -1 : empty.get();
    md::RawAuthArgs args = with_credentials(h.creds("credentials-client"));
    args.password_stdin = true;
    const Run run{h, args, Setup{.stdin_fd = fd}};
    CHECK(run.exit_code() == 2);
    CHECK(run.out().empty());
    CHECK(run.err() == std::format("mangadex-dl: error: --password-stdin was given, but stdin "
                                   "held no password\n{}",
                                   hint));
    CHECK(h.requests().empty());
}

// ---- AC16: the credentials file
// ------------------------------------------------------------------

TEST_CASE("auth phase: a credentials file others can read is exit 1 with the chmod hint (AC16)",
          "[auth phase]") {
    const Harness h{};
    const fs::path path = h.creds("credentials-0644");
    const Run run{h, with_credentials(path)};
    CHECK(run.exit_code() == 1);
    CHECK(run.err() ==
          std::format("mangadex-dl: error: credentials file '{}' is readable or writable by "
                      "others (mode 0644); run 'chmod 600 {}'\n",
                      path.string(), path.string()));
    CHECK(h.requests().empty());
    run.check_no_secret();
}

TEST_CASE("auth phase: a private credentials file with all four keys logs in silently (AC16)",
          "[auth phase]") {
    const Harness h{};
    FakePrompter prompter{};
    SECTION("given with --credentials") {
        Run run{h, with_credentials(h.creds("credentials")),
                Setup{.tty = terminals, .prompter = &prompter}};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err().empty());
        CHECK(run.phase().notes.how == LoginHow::password);
    }
    SECTION("at the default path") {
        write_private_file(h.default_credentials(), read_file(h.creds("credentials")),
                           fs::perms::owner_read | fs::perms::owner_write);
        Run run{h, md::RawAuthArgs{}, Setup{.tty = terminals, .prompter = &prompter}};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err().empty());
        CHECK(run.phase().notes.how == LoginHow::password);
    }
    CHECK(prompter.events().empty());
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
}

TEST_CASE("auth phase: a missing --credentials file is exit 1, a missing default is not",
          "[auth phase]") {
    const Harness h{};
    const fs::path missing = h.root() / "no-such-credentials";
    const Run run{h, with_credentials(missing)};
    CHECK(run.exit_code() == 1);
    CHECK(run.err() == std::format("mangadex-dl: error: credentials file '{}' does not exist\n",
                                   missing.string()));
    CHECK(h.requests().empty());

    // No default file: not an error by itself (the missing client is, later: exit 8).
    REQUIRE(!fs::exists(h.default_credentials()));
    const Run fallback{h, md::RawAuthArgs{}};
    CHECK(fallback.exit_code() == 8);
}

// ---- --credentials as CLI11 binds it (V10, V36)
// --------------------------------------------------

TEST_CASE("auth phase: --credentials '' is given but empty, and exit 2 (V10)", "[auth phase]") {
    const std::vector<std::string> argv{"--credentials", ""};
    const md::Result<md::RawAuthArgs> parsed = app::parse_auth_args_for_testing(argv);
    REQUIRE(parsed.has_value());
    CHECK(parsed->credentials_given);
    CHECK(parsed->credentials.empty());

    const Harness h{};
    const Run run{h, *parsed};
    CHECK(run.exit_code() == 2);
    CHECK(run.out().empty());
    CHECK(run.err() ==
          std::format("mangadex-dl: error: --credentials needs a file name\n{}", hint));
    CHECK(h.requests().empty());
}

TEST_CASE("auth phase: the auth options as CLI11 binds them", "[auth phase]") {
    SECTION("nothing given") {
        const md::Result<md::RawAuthArgs> parsed = app::parse_auth_args_for_testing({});
        REQUIRE(parsed.has_value());
        CHECK(!parsed->credentials_given);
        CHECK(parsed->credentials.empty());
        CHECK(!parsed->password_stdin);
        CHECK(!parsed->no_input);
    }
    SECTION("both flags") {
        const std::vector<std::string> argv{"--password-stdin", "--no-input"};
        const md::Result<md::RawAuthArgs> parsed = app::parse_auth_args_for_testing(argv);
        REQUIRE(parsed.has_value());
        CHECK(!parsed->credentials_given);
        CHECK(parsed->password_stdin);
        CHECK(parsed->no_input);
    }
    SECTION("--credentials=FILE") {
        const std::vector<std::string> argv{"--credentials=creds.txt"};
        const md::Result<md::RawAuthArgs> parsed = app::parse_auth_args_for_testing(argv);
        REQUIRE(parsed.has_value());
        CHECK(parsed->credentials_given);
        CHECK(parsed->credentials == "creds.txt");
    }
}

TEST_CASE("auth phase: --credentials takes the next word verbatim, even --no-input",
          "[auth phase]") {
    const std::vector<std::string> argv{"--credentials", "--no-input"};
    const md::Result<md::RawAuthArgs> parsed = app::parse_auth_args_for_testing(argv);
    INFO((parsed ? std::string{} : parsed.error().message));
    REQUIRE(parsed.has_value());
    CHECK(parsed->credentials_given);
    CHECK(parsed->credentials == "--no-input");
    CHECK(!parsed->no_input);

    // A file named `--no-input` (relative to the working directory) does not exist: exit 1 with
    // that path, and no prompt even with terminals.
    REQUIRE(!fs::exists("--no-input"));
    const Harness h{};
    FakePrompter prompter{};
    answer_everything(prompter);
    const Run run{h, *parsed, Setup{.tty = terminals, .prompter = &prompter}};
    CHECK(run.exit_code() == 1);
    CHECK(run.err() == "mangadex-dl: error: credentials file '--no-input' does not exist\n");
    CHECK(prompter.events().empty());
    CHECK(h.requests().empty());
}

// ---- the API base ------------------------------------------------------------------------------

TEST_CASE("auth phase: a custom --api-base is exit 2 before stdin, a file or a request",
          "[auth phase]") {
    const Harness h{};
    const std::string_view command = GENERATE(std::string_view{"follows"}, "login");
    CAPTURE(command);
    md::Result<md::ApiBase> custom = md::ApiBase::parse("https://example.org");
    REQUIRE(custom.has_value());
    const std::string piped = std::format("{}\n", mdx::fixture_password);
    const md::UniqueFd stdin_fd = pipe_with(piped);
    FakePrompter prompter{};
    answer_everything(prompter);
    md::RawAuthArgs args = with_credentials(h.creds("credentials-0644")); // would be exit 1
    args.password_stdin = true;
    const Run run{h, args,
                  Setup{.tty = terminals,
                        .stdin_fd = stdin_fd.get(),
                        .prompter = &prompter,
                        .mode = command == "login" ? LoginMode::password : LoginMode::reuse,
                        .command = command,
                        .api_base = *custom}};
    CHECK(run.exit_code() == 2);
    CHECK(run.out().empty());
    CHECK(run.err() == std::format("mangadex-dl: error: {} logs in only to MangaDex itself; "
                                   "--api-base must be the official API\nRun 'mangadex-dl {} "
                                   "--help' for more information.\n",
                                   command, command));
    CHECK(drain(stdin_fd.get()) == piped); // stdin was never read
    CHECK(prompter.events().empty());
    CHECK(h.requests().empty());
    CHECK(!fs::exists(h.state_dir()));
}

// ---- AC17: refusals
// ------------------------------------------------------------------------------

TEST_CASE("auth phase: rejected login details -> 9, the saved login unchanged (AC17)",
          "[auth phase]") {
    const Harness h{};
    {
        const Run first{h, with_credentials(h.creds("credentials")),
                        Setup{.mode = LoginMode::password}};
        REQUIRE(first.exit_code() == 0);
    }
    const std::string before = read_file(h.session_file());

    SECTION("a wrong password, answered 401") {
        const Run run{h, with_credentials(h.creds("credentials-wrong-password")),
                      Setup{.mode = LoginMode::password}};
        CHECK(run.exit_code() == 9);
        CHECK(run.err() ==
              "mangadex-dl: error: MangaDex rejected the username or password for fixture-user "
              "(invalid_grant); check them, or log in on the website to see whether the account "
              "is locked\n");
        run.check_no_secret();
    }
    SECTION("a wrong password, answered 400, with -v detail") {
        const Run run{h, with_credentials(h.creds("credentials-wrong-password-400")),
                      Setup{.verbose = 1, .mode = LoginMode::password}};
        CHECK(run.exit_code() == 9);
        CHECK_THAT(run.err(),
                   ContainsSubstring("mangadex-dl: the MangaDex login server answered HTTP 400 "
                                     "with invalid_grant: Invalid user credentials\n"));
        CHECK_THAT(run.err(), EndsWith("mangadex-dl: error: MangaDex rejected the username or "
                                       "password for fixture-user-400 (invalid_grant); check "
                                       "them, or log in on the website to see whether the "
                                       "account is locked\n"));
    }
    SECTION("a wrong client secret") {
        const fs::path path = h.root() / "credentials-bad-secret";
        write_private_file(path,
                           std::format("client_id = {}\nclient_secret = wrong-not-a-secret\n"
                                       "username = {}\npassword = {}\n",
                                       mdx::fixture_client_id, mdx::fixture_username,
                                       mdx::fixture_password),
                           fs::perms::owner_read | fs::perms::owner_write);
        const Run run{h, with_credentials(path), Setup{.mode = LoginMode::password}};
        CHECK(run.exit_code() == 9);
        CHECK(run.err() == "mangadex-dl: error: MangaDex rejected the API client "
                           "(invalid_client); check client_id and client_secret, and whether the "
                           "client is approved at https://mangadex.org/settings\n");
    }
    SECTION("a client not yet approved") {
        const Run run{h, with_credentials(h.creds("credentials-unapproved")),
                      Setup{.mode = LoginMode::password}};
        CHECK(run.exit_code() == 9);
        CHECK_THAT(run.err(), EndsWith("mangadex-dl: error: MangaDex did not let the API client "
                                       "log in (unauthorized_client); a new personal client may "
                                       "need approval by MangaDex first, see "
                                       "https://mangadex.org/settings\n"));
    }
    CHECK(read_file(h.session_file()) == before);
}

// ---- AC18: the session ended --------------------------------------------------------------------

TEST_CASE("auth phase: a refresh answered invalid_grant falls back to the password (AC18)",
          "[auth phase]") {
    const Harness h{};
    // fixture-refresh-2 ends the default chain: refreshing it is 400 invalid_grant.
    h.seed(h.session(mdx::fixture_refresh_2, mdx::fixture_access_2, unix_now() - 10));

    SECTION("a password in the credentials file: a silent password login") {
        Run run{h, with_credentials(h.creds("credentials"))};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err().empty());
        CHECK(run.phase().notes.how == LoginHow::password);
        CHECK(h.requests() == std::vector<std::string>{h.token_url(), h.token_url()});
        const md::Session saved = h.read_back();
        REQUIRE(saved.refresh_token.has_value());
        CHECK(engaged(saved.refresh_token).value().reveal() == mdx::fixture_refresh_1);
    }
    SECTION("no password anywhere: exit 8 'expired'") {
        const Run run{h, with_credentials(h.creds("credentials-client"))};
        CHECK(run.exit_code() == 8);
        CHECK(run.err() == expired_line);
        CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    }
}

TEST_CASE("auth phase: a valid saved login is reused without a request; -v names the user",
          "[auth phase]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() + 3600));
    SECTION("-v") {
        Run run{h, md::RawAuthArgs{}, Setup{.verbose = 1}};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err() == "mangadex-dl: logged in to MangaDex as fixture-user (saved login)\n");
        CHECK(run.phase().notes.how == LoginHow::saved);
    }
    SECTION("-v -q") {
        const Run run{h, md::RawAuthArgs{}, Setup{.verbose = 1, .quiet = true}};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err().empty());
    }
    CHECK(h.requests().empty());
}

TEST_CASE("auth phase: -v says 'refreshed' after a refresh grant", "[auth phase]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() - 10));
    const Run run{h, md::RawAuthArgs{}, Setup{.verbose = 1}};
    REQUIRE(run.exit_code() == 0);
    CHECK(run.err() == "mangadex-dl: logged in to MangaDex as fixture-user (refreshed)\n");
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    const md::Session saved = h.read_back();
    REQUIRE(saved.refresh_token.has_value());
    CHECK(engaged(saved.refresh_token).value().reveal() == mdx::fixture_refresh_2);
}

TEST_CASE("auth phase: -vv traces the token request, never a secret", "[auth phase]") {
    const Harness h{};
    const Run run{h, with_credentials(h.creds("credentials")),
                  Setup{.verbose = 2, .mode = LoginMode::password}};
    REQUIRE(run.exit_code() == 0);
    CHECK_THAT(run.err(), StartsWith(std::format("mangadex-dl: POST {} -> 200 (", h.token_url())));
    CHECK_THAT(run.err(),
               EndsWith("mangadex-dl: logged in to MangaDex as fixture-user (password)\n"));
    CHECK(run.out().empty());
    run.check_no_secret();
}

// ---- AC22: the session can't be saved
// ------------------------------------------------------------

TEST_CASE("auth phase: a session that can't be saved: follows warns, login leaves it (AC22)",
          "[auth phase]") {
    const Harness h{};
    fs::create_directories(h.state_dir().parent_path());
    write_file(h.state_dir(), "not a directory");
    const std::string why = std::format("the directory {} is not usable: it is not a directory",
                                        h.state_dir().string());

    SECTION("follows (reuse): a warning, and the login still works") {
        Run run{h, with_credentials(h.creds("credentials"))};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err() == std::format("mangadex-dl: warning: could not save the MangaDex login "
                                       "to {}: {}; the next run will need the password again\n",
                                       h.session_file().string(), why));
        CHECK(app::save_outcome(run.phase().notes) == app::SaveOutcome::failed);
    }
    SECTION("login (password): nothing printed, the command reports save_outcome") {
        Run run{h, with_credentials(h.creds("credentials")), Setup{.mode = LoginMode::password}};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err().empty());
        CHECK(run.phase().notes.save_error == std::optional<std::string>{why});
        CHECK(app::save_outcome(run.phase().notes) == app::SaveOutcome::failed);
    }
    CHECK(read_file(h.state_dir()) == "not a directory");
}

TEST_CASE("auth phase: without a home directory the login works but is not saved", "[auth phase]") {
    const Harness h{};
    const LoginMode mode = GENERATE(LoginMode::reuse, LoginMode::password);
    CAPTURE(mode == LoginMode::password);
    Run run{h, with_credentials(h.creds("credentials")),
            Setup{.mode = mode, .env = app::EnvSnapshot{}}};
    REQUIRE(run.exit_code() == 0);
    CHECK(!run.phase().notes.session_path.has_value());
    CHECK(app::save_outcome(run.phase().notes) == app::SaveOutcome::failed);
    if (mode == LoginMode::reuse) {
        CHECK(run.err() == "mangadex-dl: warning: could not save the MangaDex login: there is no "
                           "home directory (HOME is not set to an absolute path); the next run "
                           "will need the password again\n");
    } else {
        CHECK(run.err().empty());
    }
}

TEST_CASE("auth phase: a failed save is printed once by follows, never by login's phase",
          "[auth phase]") {
    // Pins the copy of the core's save-warning wording in cmd_auth.cpp (save_warning_text): the
    // core raises the warning once in both modes; follows prints it exactly once, and login's
    // phase leaves it out (the command reports save_outcome). A drift would print it in login.
    const Harness h{};
    const bool home = GENERATE(true, false);
    const LoginMode mode = GENERATE(LoginMode::reuse, LoginMode::password);
    CAPTURE(home, mode == LoginMode::password);
    std::optional<app::EnvSnapshot> env{};
    if (home) {
        fs::create_directories(h.state_dir().parent_path());
        write_file(h.state_dir(), "not a directory"); // the save warning names the session path
    } else {
        env = app::EnvSnapshot{}; // no session path: the warning's second wording
    }
    Run run{h, with_credentials(h.creds("credentials")), Setup{.mode = mode, .env = env}};
    REQUIRE(run.exit_code() == 0);
    const md::LoginNotes& notes = run.phase().notes;
    REQUIRE(notes.warnings.size() == 1);
    CHECK_THAT(notes.warnings.front(), StartsWith("could not save the MangaDex login"));
    const std::string printed = std::format("mangadex-dl: warning: {}\n", notes.warnings.front());
    if (mode == LoginMode::reuse) {
        CHECK(run.err() == printed);
    } else {
        CHECK(run.err().empty());
    }
}

TEST_CASE("auth phase: save_outcome tells a saved, an unsynced and a failed save apart",
          "[auth phase]") {
    CHECK(app::save_outcome(md::LoginNotes{}) == app::SaveOutcome::saved);
    CHECK(app::save_outcome(md::LoginNotes{
              .save_error = "saved, but cannot sync the state directory: Input/output error"}) ==
          app::SaveOutcome::unsynced);
    CHECK(app::save_outcome(md::LoginNotes{
              .save_error = "cannot create a temporary file: No space left on device"}) ==
          app::SaveOutcome::failed);
}

// ---- AC25, AC26: the state directory ------------------------------------------------------------

TEST_CASE("auth phase: an insecure state directory or a symlinked session is ignored (AC25)",
          "[auth phase]") {
    const Harness h{};
    SECTION("a world-writable state directory") {
        h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() + 3600));
        const std::string before = read_file(h.session_file());
        fs::permissions(h.state_dir(), fs::perms::all, fs::perm_options::replace);
        Run run{h, with_credentials(h.creds("credentials"))};
        REQUIRE(run.exit_code() == 0);
        CHECK_THAT(run.err(),
                   StartsWith(std::format("mangadex-dl: warning: ignoring the saved login at {}: ",
                                          h.session_file().string())));
        CHECK_THAT(run.err(), ContainsSubstring(std::format(
                                  "mangadex-dl: warning: could not save the MangaDex login to {}",
                                  h.session_file().string())));
        CHECK(run.phase().notes.how == LoginHow::password); // the saved token was not used
        CHECK(read_file(h.session_file()) == before);       // nothing written
        fs::permissions(h.state_dir(), fs::perms::owner_all, fs::perm_options::replace);
    }
    SECTION("a symlinked session file") {
        const md::FileResult<md::StateDir> dir = md::StateDir::open(h.state_dir(), true);
        REQUIRE(dir.has_value());
        const fs::path target = h.root() / "elsewhere.json";
        const md::Secret document = md::session_to_json(
            h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() + 3600));
        const std::string bytes{document.reveal()};
        write_private_file(target, bytes, fs::perms::owner_read | fs::perms::owner_write);
        fs::create_symlink(target, h.session_file());
        Run run{h, with_credentials(h.creds("credentials"))};
        REQUIRE(run.exit_code() == 0);
        CHECK_THAT(run.err(),
                   StartsWith(std::format("mangadex-dl: warning: ignoring the saved login at {}: ",
                                          h.session_file().string())));
        CHECK(run.phase().notes.how == LoginHow::password);
        CHECK(read_file(target) == bytes); // nothing written through the link
    }
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
}

TEST_CASE("auth phase: a session lock held elsewhere is exit 1 at once, then it proceeds (AC26)",
          "[auth phase]") {
    const Harness h{};
    {
        // A second open file description holds the lock (flock is per description, V16).
        const md::FileResult<md::StateDir> dir = md::StateDir::open(h.state_dir(), true);
        REQUIRE(dir.has_value());
        const std::unique_ptr<md::Clock> clock = md::make_instant_clock();
        const md::Result<md::SessionLock> held = dir->lock(*clock, md::LockTiming{}, {});
        REQUIRE(held.has_value());

        const Run run{h, with_credentials(h.creds("credentials"))};
        CHECK(run.exit_code() == 1);
        CHECK(run.out().empty());
        CHECK(run.err() == lock_timeout_line);
        CHECK(h.requests().empty());
    }
    const Run again{h, with_credentials(h.creds("credentials"))};
    CHECK(again.exit_code() == 0);
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
}

// ---- the real terminal prompter (owned by the phase) --------------------------------------------

TEST_CASE("auth phase: with terminals and no injected prompter, the phase's own prompter asks",
          "[auth phase]") {
    const md::test::PtyPair pty = require_pty(); // before the Typist: it borrows the master
    const Harness h{};
    md::reset_interrupt_for_testing();
    Typist typist{pty.master.get(), std::format("{}\n", mdx::fixture_password)};
    // credentials-client has the client and the username: open() asks only for the password,
    // through a TerminalPrompter over the pty slave that the phase owns.
    Run run{h, with_credentials(h.creds("credentials-client")),
            Setup{.tty = terminals, .stdin_fd = pty.slave.get()}};
    typist.join();
    CHECK(!typist.timed_out());
    CHECK(typist.wrote());
    REQUIRE(run.exit_code() == 0);
    CHECK(run.phase().prompter != nullptr);
    CHECK(run.phase().notes.how == LoginHow::password);
    CHECK(run.err() == "MangaDex password for fixture-user: \n");
    CHECK(h.requests() == std::vector<std::string>{h.token_url()});
    run.close();
    run.check_no_secret();
    md::reset_interrupt_for_testing();
}

TEST_CASE("auth phase: the session asks the phase's own prompter after the phase returned",
          "[auth phase]") {
    const md::test::PtyPair pty = require_pty(); // before the Typist: it borrows the master
    const Harness h{};
    md::reset_interrupt_for_testing();
    // A valid saved access token (reused, nothing asked), whose refresh token ends the chain:
    // the refresh after a 401 is invalid_grant, so the session falls back to the password and
    // asks the prompter the LoginPhase owns.
    h.seed(h.session(mdx::fixture_refresh_2, mdx::fixture_access_2, unix_now() + 3600));
    Run run{h, with_credentials(h.creds("credentials-client")),
            Setup{.tty = terminals, .stdin_fd = pty.slave.get()}};
    REQUIRE(run.exit_code() == 0);
    CHECK(run.phase().notes.how == LoginHow::saved);
    REQUIRE(run.phase().prompter != nullptr);
    CHECK(run.err().empty());
    CHECK(h.requests().empty());

    Typist typist{pty.master.get(), std::format("{}\n", mdx::fixture_password)};
    const md::AuthResult<md::Secret> renewed = run.phase().session->refresh_after_unauthorized({});
    typist.join();
    CHECK(!typist.timed_out());
    CHECK(typist.wrote());
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(mdx::fixture_access_1));
    CHECK(run.err() == "MangaDex password for fixture-user: \n");
    CHECK(h.requests() == std::vector<std::string>{h.token_url(), h.token_url()});
    CHECK(engaged(h.read_back().refresh_token).value().reveal() == mdx::fixture_refresh_1);
    run.close();
    run.check_no_secret();
    md::reset_interrupt_for_testing();
}

// ---- interrupts and the end of the phase --------------------------------------------------------

TEST_CASE("auth phase: an interrupt at a prompt prints nothing and saves nothing", "[auth phase]") {
    const Harness h{};
    FakePrompter prompter{};
    prompter.answer(mdx::fixture_client_id);
    prompter.reply(PromptStatus::interrupted);
    const Run run{h, md::RawAuthArgs{}, Setup{.tty = terminals, .prompter = &prompter}};
    CHECK(run.exit_code() == md::exit_code_for(md::Errc::interrupted));
    CHECK(run.out().empty());
    CHECK(run.err().empty());
    CHECK(prompter.calls().size() == 2);
    CHECK(prompter.calls().back().echo == Echo::off);
    CHECK(h.requests().empty());
    CHECK(!fs::exists(h.session_file()));
}

TEST_CASE("auth phase: close_login_phase prints the late warnings, then closes the session",
          "[auth phase]") {
    const Harness h{};
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() + 3600));
    Run run{h, md::RawAuthArgs{}};
    REQUIRE(run.exit_code() == 0);
    CHECK(run.err().empty());

    // The state directory disappears under the open session (its descriptor stays open), so the
    // save after the next refresh fails: a late warning.
    fs::remove(h.session_file());
    fs::remove(h.state_dir() / "session.lock");
    REQUIRE(fs::remove(h.state_dir()));
    md::LoginSession& session = *run.phase().session;
    const md::AuthResult<md::Secret> renewed = session.refresh_after_unauthorized({});
    REQUIRE(renewed.has_value());
    CHECK(renewed->reveal() == mdx::bearer(mdx::fixture_access_2));

    run.close();
    CHECK_THAT(run.err(),
               StartsWith(std::format("mangadex-dl: warning: could not save the MangaDex login "
                                      "to {}: cannot create a temporary file: ",
                                      h.session_file().string())));
    CHECK_THAT(run.err(), EndsWith("; the next run will need the password again\n"));
    const md::AuthResult<md::Secret> closed = session.authorization({});
    REQUIRE(!closed.has_value());
    CHECK(closed.error().error.message == "the MangaDex login session is already closed");

    // Idempotent: nothing more is printed.
    const std::string printed = run.err();
    run.close();
    CHECK(run.err() == printed);
    run.check_no_secret();
}

// ---- untrusted text on the terminal
// --------------------------------------------------------------

TEST_CASE("auth phase: paths and usernames are printed terminal-safe", "[auth phase]") {
    const Harness h{};
    SECTION("ESC in a --credentials path") {
        const fs::path missing = h.root() / "creds\x1b[2Jfile";
        const Run run{h, with_credentials(missing)};
        CHECK(run.exit_code() == 1);
        CHECK_THAT(run.err(), !ContainsSubstring("\x1b"));
        CHECK_THAT(run.err(), ContainsSubstring("creds?[2Jfile' does not exist"));
    }
    SECTION("ESC and BEL in the state directory path of a save warning") {
        const fs::path state = h.root() / "st\x1b]0;x\x07"
                                          "ate";
        fs::create_directories(state);
        write_file(state / "mangadex-dl", "not a directory");
        app::EnvSnapshot env = h.env();
        env.xdg_state_home = state.string();
        const Run run{h, with_credentials(h.creds("credentials")), Setup{.env = env}};
        REQUIRE(run.exit_code() == 0);
        CHECK_THAT(run.err(),
                   StartsWith("mangadex-dl: warning: could not save the MangaDex login"));
        CHECK_THAT(run.err(), !ContainsSubstring("\x1b"));
        CHECK_THAT(run.err(), !ContainsSubstring("\x07"));
        CHECK_THAT(run.err(), ContainsSubstring("st?]0;x?ate"));
    }
    SECTION("ESC and a bidi override in the server's error_description on -v") {
        // One more account on the tree whose correct password is refused with a hostile
        // description (untrusted server text).
        const mdx::FixtureAuth evil{
            .username = "fixture-user-evil",
            .password_refusal = mdx::scripted_error(
                400, "unauthorized_client", std::format("Client \x1b[31mnot{} allowed", rlo())),
            .chain = {},
            .revokes = {},
        };
        const auto written = mdx::write_auth_files(h.tree(), evil);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
        const fs::path path = h.root() / "credentials-evil";
        write_private_file(path, mdx::credentials_text(evil.username, mdx::fixture_password),
                           fs::perms::owner_read | fs::perms::owner_write);
        const Run run{h, with_credentials(path), Setup{.verbose = 1, .mode = LoginMode::password}};
        CHECK(run.exit_code() == 9);
        CHECK_THAT(run.err(), !ContainsSubstring("\x1b"));
        CHECK_THAT(run.err(), !ContainsSubstring(rlo()));
        CHECK_THAT(run.err(), StartsWith("mangadex-dl: the MangaDex login server answered HTTP 400 "
                                         "with unauthorized_client: Client ?[31mnot? allowed\n"));
    }
    SECTION("a bidi override in a saved username on -v") {
        const std::string username = std::format("fixture{}user", rlo());
        h.seed(
            h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() + 3600, username));
        Run run{h, md::RawAuthArgs{}, Setup{.verbose = 1}};
        REQUIRE(run.exit_code() == 0);
        CHECK(run.err() == "mangadex-dl: logged in to MangaDex as fixture?user (saved login)\n");
        CHECK(run.phase().notes.username == username); // kept raw for --json
    }
}

TEST_CASE("auth phase: a long username is cut to 64 bytes on a UTF-8 boundary", "[auth phase]") {
    CHECK(app::username_for_display(std::string(64, 'a')) == std::string(64, 'a'));
    CHECK(app::username_for_display(std::string(65, 'a')) == std::string(64, 'a'));
    CHECK(app::username_for_display(std::string(62, 'a') + "\u00e9b") ==
          std::string(62, 'a') + "\u00e9");
    CHECK(app::username_for_display(std::string(63, 'a') + "\u00e9") == std::string(63, 'a'));
    CHECK(app::username_for_display(std::string(62, 'a') + "\u20ac") == std::string(62, 'a'));
    CHECK(app::username_for_display("").empty());

    const Harness h{};
    const std::string long_name(100, 'u');
    h.seed(h.session(mdx::fixture_refresh_1, mdx::fixture_access_1, unix_now() + 3600, long_name));
    const Run run{h, md::RawAuthArgs{}, Setup{.verbose = 1}};
    REQUIRE(run.exit_code() == 0);
    CHECK(run.err() == std::format("mangadex-dl: logged in to MangaDex as {} (saved login)\n",
                                   std::string(64, 'u')));
}

// ---- small helpers
// -------------------------------------------------------------------------------

TEST_CASE("auth phase: user_dirs keeps only absolute paths from the environment", "[auth phase]") {
    const md::UserDirs dirs = app::user_dirs(app::EnvSnapshot{
        .home = "/home/u", .xdg_config_home = "relative/config", .xdg_state_home = "/state"});
    CHECK(dirs == md::UserDirs{.home = fs::path{"/home/u"}, .xdg_state_home = fs::path{"/state"}});
    CHECK(app::user_dirs(app::EnvSnapshot{}) == md::UserDirs{});
}

namespace {

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

} // namespace

TEST_CASE("auth phase: guard_secrets turns core dumps off", "[auth phase]") {
    if (!raise_core_limit()) {
        SKIP("the hard RLIMIT_CORE is already 0: nothing to turn off");
    }
    REQUIRE(core_limit() != 0);
    app::guard_secrets();
    CHECK(core_limit() == 0);
}

TEST_CASE("auth phase: the phase turns core dumps off once its pure checks pass", "[auth phase]") {
    if (!raise_core_limit()) {
        SKIP("the hard RLIMIT_CORE is already 0: nothing to turn off");
    }
    const Harness h{};
    {
        // Refused by a pure check: nothing guarded yet (nothing was read).
        const md::RawAuthArgs empty_path{.credentials = {}, .credentials_given = true};
        const Run run{h, empty_path};
        REQUIRE(run.exit_code() == 2);
        CHECK(core_limit() != 0);
    }
    // Past the pure checks, before stdin or the credentials file is read (the read fails here).
    const Run run{h, with_credentials(h.root() / "no-such-credentials")};
    REQUIRE(run.exit_code() == 1);
    CHECK(core_limit() == 0);
}
