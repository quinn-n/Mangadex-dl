// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/auth_inputs_test.cpp: read_password_stdin (through pipes), check_raw_auth_args
// and check_auth_api_base (follows plan T9). Passwords are obviously fake. rev3 S18: a stalled
// pipe (its writer kept open) and an interrupt, simulated or a real SIGINT under the SA_RESTART
// handlers, end the read promptly with Errc::interrupted.

#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/options.hpp"
#include "mangadex_dl/secret.hpp"
#include "support/fs_test_util.hpp"
#include "support/mdx_auth_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): pthread_kill and SIGINT are POSIX <signal.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
using md::read_password_stdin;

inline constexpr std::string_view no_password{
    "--password-stdin was given, but stdin held no password"};

/// A pipe; `write_end` may be closed early.
struct Pipe {
    md::UniqueFd read_end{};
    md::UniqueFd write_end{};
};

[[nodiscard]] Pipe make_pipe() {
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::pipe2(fds.data(), O_CLOEXEC) == 0);
    return Pipe{.read_end = md::UniqueFd{fds[0]}, .write_end = md::UniqueFd{fds[1]}};
}

/// A pipe whose reader sees exactly `bytes`, then EOF (the bytes fit in the pipe buffer).
[[nodiscard]] md::UniqueFd stdin_with(std::string_view bytes) {
    Pipe pipe = make_pipe();
    REQUIRE(::write(pipe.write_end.get(), bytes.data(), bytes.size()) ==
            static_cast<ssize_t>(bytes.size()));
    pipe.write_end.reset();
    return std::move(pipe.read_end);
}

[[nodiscard]] std::string password_from(std::string_view bytes) {
    const md::UniqueFd fd = stdin_with(bytes);
    const auto password = read_password_stdin(fd.get());
    REQUIRE(password.has_value());
    return std::string{password->reveal()};
}

void check_no_password(int fd) {
    const auto password = read_password_stdin(fd);
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == md::Errc::usage);
    CHECK(password.error().exit_code() == 2);
    CHECK(password.error().message == no_password);
}

TEST_CASE("auth inputs: the password is the first line of stdin", "[auth][auth-inputs]") {
    const std::string pw{mdx::fixture_password};
    CHECK(password_from(pw + "\n") == pw);
    CHECK(password_from(pw + "\r\n") == pw);
    CHECK(password_from(pw) == pw);
    CHECK(password_from(pw + "\nsecond line\n") == pw);
    CHECK(password_from(" spaces kept \n") == " spaces kept ");
    CHECK(password_from("ends in a lone CR\r") == "ends in a lone CR\r");
}

TEST_CASE("auth inputs: an empty stdin or an empty first line holds no password",
          "[auth][auth-inputs]") {
    const std::string_view bytes =
        GENERATE(std::string_view{}, std::string_view{"\n"}, std::string_view{"\r\n"},
                 std::string_view{"\nsecond line\n"});
    CAPTURE(bytes.size());
    const md::UniqueFd fd = stdin_with(bytes);
    check_no_password(fd.get());
}

TEST_CASE("auth inputs: a NUL anywhere on stdin holds no password", "[auth][auth-inputs]") {
    const std::string_view bytes = GENERATE(
        std::string_view{"p\0w\n", 4}, std::string_view{"\0", 1}, std::string_view{"pw\n\0", 4});
    CAPTURE(bytes.size());
    const md::UniqueFd fd = stdin_with(bytes);
    check_no_password(fd.get());
}

TEST_CASE("auth inputs: stdin may hold at most 4096 bytes", "[auth][auth-inputs]") {
    const std::string longest(md::max_password_stdin_bytes, 'p');
    CHECK(password_from(longest) == longest);
    CHECK(password_from(std::string(md::max_password_stdin_bytes - 1, 'p') + "\n") ==
          std::string(md::max_password_stdin_bytes - 1, 'p'));

    const md::UniqueFd fd = stdin_with(longest + "\n");
    check_no_password(fd.get());
}

TEST_CASE("auth inputs: never more than 4097 bytes are taken from stdin", "[auth][auth-inputs]") {
    constexpr std::size_t written = 5000;
    const md::UniqueFd fd = stdin_with(std::string(written, 'p'));
    check_no_password(fd.get());

    std::array<char, written> rest{};
    const ::ssize_t left = ::read(fd.get(), rest.data(), rest.size());
    CHECK(left == static_cast<ssize_t>(written - (md::max_password_stdin_bytes + 1)));
}

TEST_CASE("auth inputs: a closed stdin holds no password", "[auth][auth-inputs]") {
    check_no_password(-1);

    Pipe pipe = make_pipe();
    const int closed = pipe.read_end.get();
    pipe.read_end.reset();
    pipe.write_end.reset();
    check_no_password(closed);
}

TEST_CASE("auth inputs: /dev/null on stdin holds no password", "[auth][auth-inputs]") {
    // The character-device branch of the type policy: read, and EOF at once.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const md::UniqueFd fd{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
    REQUIRE(fd.valid());
    check_no_password(fd.get());
}

TEST_CASE("auth inputs: a regular file on stdin is read", "[auth][auth-inputs]") {
    const md::test::TempDir dir = md::test::fs_util::make_temp_dir();
    const std::string path = (dir.path() / "password.txt").string();
    md::test::fs_util::write_file(path, std::string{mdx::fixture_password} + "\n");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const md::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    REQUIRE(fd.valid());
    const auto password = read_password_stdin(fd.get());
    REQUIRE(password.has_value());
    CHECK(password->reveal() == mdx::fixture_password);
}

TEST_CASE("auth inputs: a directory on stdin cannot be read", "[auth][auth-inputs]") {
    const md::test::TempDir dir = md::test::fs_util::make_temp_dir();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    const md::UniqueFd fd{::open(dir.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    REQUIRE(fd.valid());
    const auto password = read_password_stdin(fd.get());
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == md::Errc::failure);
    CHECK(password.error().message == "cannot read stdin: is a directory");
}

// ---- rev3 S18: an interrupt ends a stalled read ------------------------------------------------

namespace {

/// Clears the process-wide interrupt flag before and after each use.
class FlagReset {
public:
    FlagReset() noexcept { md::reset_interrupt_for_testing(); }
    FlagReset(const FlagReset&) = delete;
    FlagReset& operator=(const FlagReset&) = delete;
    FlagReset(FlagReset&&) = delete;
    FlagReset& operator=(FlagReset&&) = delete;
    ~FlagReset() { md::reset_interrupt_for_testing(); }
};

// NOLINTNEXTLINE(misc-include-cleaner): POSIX pthread_t; glibc defines it in a bits/ header
using ThreadId = pthread_t;

/// After 200 ms, interrupts the thread that constructed it: a simulated interrupt (the flag
/// only), or a real SIGINT sent to that thread (pthread_kill), which the installed handler turns
/// into the flag. The thread is joined by the destructor (std::jthread, the last member).
class Interrupter {
public:
    explicit Interrupter(bool real_signal)
        : target_{::pthread_self()}, thread_{[real_signal, target = target_, sent = &sent_] {
              std::this_thread::sleep_for(std::chrono::milliseconds{200});
              if (real_signal) {
                  sent->store(::pthread_kill(target, SIGINT) == 0);
              } else {
                  md::simulate_interrupt_for_testing(SIGINT);
                  sent->store(true);
              }
          }} {}
    Interrupter(const Interrupter&) = delete;
    Interrupter& operator=(const Interrupter&) = delete;
    Interrupter(Interrupter&&) = delete;
    Interrupter& operator=(Interrupter&&) = delete;
    ~Interrupter() = default;

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] bool sent() const noexcept { return sent_.load(); }

private:
    std::atomic<bool> sent_{false};
    ThreadId target_{};
    std::jthread thread_{};
};

} // namespace

TEST_CASE("auth inputs: an interrupt ends a stalled --password-stdin read promptly (rev3 S18)",
          "[auth][auth-inputs]") {
    const FlagReset reset{};
    const bool real_signal = GENERATE(false, true);
    CAPTURE(real_signal);
    // The writer stays open and has written part of a line: read() alone would wait forever.
    const Pipe pipe = make_pipe();
    REQUIRE(::write(pipe.write_end.get(), "partial", 7) == 7);
    std::optional<md::Result<md::ScopedInterruptHandlers>> handlers{};
    if (real_signal) {
        handlers.emplace(md::ScopedInterruptHandlers::install()); // SA_RESTART, as in the app
        REQUIRE(handlers->has_value());
    }

    const auto started = std::chrono::steady_clock::now();
    Interrupter interrupter{real_signal};
    const auto password = read_password_stdin(pipe.read_end.get());
    const auto waited = std::chrono::steady_clock::now() - started;
    interrupter.join();

    CHECK(interrupter.sent());
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == md::Errc::interrupted);
    CHECK(md::interrupt_requested());
    // 200 ms until the interrupt, then at most one 100 ms poll: far below this bound.
    CHECK(waited < std::chrono::seconds{3});
}

TEST_CASE("auth inputs: a pending interrupt is honoured before stdin is read",
          "[auth][auth-inputs]") {
    const FlagReset reset{};
    Pipe pipe = make_pipe();
    const std::string line = std::string{mdx::fixture_password} + "\n";
    REQUIRE(::write(pipe.write_end.get(), line.data(), line.size()) ==
            static_cast<ssize_t>(line.size()));
    pipe.write_end.reset();
    md::simulate_interrupt_for_testing(SIGTERM);

    const auto password = read_password_stdin(pipe.read_end.get());
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == md::Errc::interrupted);
    std::array<char, 64> rest{};
    CHECK(::read(pipe.read_end.get(), rest.data(), rest.size()) ==
          static_cast<ssize_t>(line.size())); // nothing was consumed
}

TEST_CASE("auth inputs: a non-blocking stdin is waited for, not an error", "[auth][auth-inputs]") {
    const FlagReset reset{};
    Pipe pipe = make_pipe();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    REQUIRE(::fcntl(pipe.read_end.get(), F_SETFL, O_NONBLOCK) == 0);
    std::jthread writer{[write_end = std::move(pipe.write_end)] mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        const std::string line = std::string{mdx::fixture_password} + "\n";
        static_cast<void>(::write(write_end.get(), line.data(), line.size()));
        write_end.reset();
    }};
    const auto password = read_password_stdin(pipe.read_end.get());
    writer.join();
    REQUIRE(password.has_value());
    CHECK(password->reveal() == mdx::fixture_password);
}

// ---- check_raw_auth_args ------------------------------------------------------------------------

TEST_CASE("auth inputs: --switch-account without --credentials is a usage error (rev3 S19)",
          "[auth][auth-inputs]") {
    const auto checked = md::check_raw_auth_args(md::RawAuthArgs{.switch_account = true});
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().code == md::Errc::usage);
    CHECK(checked.error().message == "--switch-account needs --credentials");
    CHECK(md::check_raw_auth_args(md::RawAuthArgs{
                                      .credentials = "creds",
                                      .credentials_given = true,
                                      .switch_account = true,
                                  })
              .has_value());
}

TEST_CASE("auth inputs: --credentials given with an empty value is a usage error",
          "[auth][auth-inputs]") {
    const auto checked =
        md::check_raw_auth_args(md::RawAuthArgs{.credentials = {}, .credentials_given = true});
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().code == md::Errc::usage);
    CHECK(checked.error().message == "--credentials needs a file name");
}

TEST_CASE("auth inputs: an absent or non-empty --credentials passes", "[auth][auth-inputs]") {
    CHECK(md::check_raw_auth_args(md::RawAuthArgs{}).has_value());
    CHECK(
        md::check_raw_auth_args(md::RawAuthArgs{.credentials = "creds", .credentials_given = true})
            .has_value());
    // CLI11 takes the next word verbatim, so `--credentials --no-input` names a file called
    // "--no-input" (exit 1 later, when it is missing): not a usage error here (spec S8).
    CHECK(md::check_raw_auth_args(
              md::RawAuthArgs{.credentials = "--no-input", .credentials_given = true})
              .has_value());
    CHECK(md::check_raw_auth_args(md::RawAuthArgs{.password_stdin = true, .no_input = true})
              .has_value());
}

// ---- check_auth_api_base ------------------------------------------------------------------------

[[nodiscard]] md::ApiBase base_of(std::string_view text) {
    auto base = md::ApiBase::parse(text);
    REQUIRE(base.has_value());
    return std::move(*base);
}

TEST_CASE("auth inputs: the official API and a fixture tree may log in", "[auth][auth-inputs]") {
    CHECK(md::check_auth_api_base(md::ApiBase{}, "follows").has_value());
    CHECK(md::check_auth_api_base(base_of(md::default_api_base), "login").has_value());
    CHECK(md::check_auth_api_base(base_of("file:///tmp/fixture-tree"), "follows").has_value());
}

TEST_CASE("auth inputs: any other API base may not log in", "[auth][auth-inputs]") {
    const std::string_view text =
        GENERATE(std::string_view{"https://example.org"}, std::string_view{"http://127.0.0.1:8080"},
                 std::string_view{"https://api.mangadex.org/v2"});
    CAPTURE(text);
    const md::ApiBase base = base_of(text);

    const auto follows = md::check_auth_api_base(base, "follows");
    REQUIRE_FALSE(follows.has_value());
    CHECK(follows.error().code == md::Errc::usage);
    CHECK(follows.error().message ==
          "follows logs in only to MangaDex itself; --api-base must be the official API");

    const auto login = md::check_auth_api_base(base, "login");
    REQUIRE_FALSE(login.has_value());
    CHECK(login.error().message ==
          "login logs in only to MangaDex itself; --api-base must be the official API");
}

} // namespace
