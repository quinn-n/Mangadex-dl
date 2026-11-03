// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/auth/auth_inputs_test.cpp: read_password_stdin (through pipes, and a loopback UDP
// socket for POLLERR with EAGAIN), check_raw_auth_args and check_auth_api_base (follows plan T9).
// Passwords are obviously fake. rev3 S18: a stalled pipe (its writer kept open) and an interrupt,
// simulated or a real SIGINT under the SA_RESTART handlers, end the read promptly with
// Errc::interrupted.

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
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): pthread_kill and SIGINT are POSIX <signal.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/socket.h>
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

/// How long a watchdog waits before it closes a stalled pipe's write end: a read that ignored the
/// interrupt then sees the end of input and the test fails on the result instead of hanging.
constexpr std::chrono::seconds watchdog_timeout{5};

/// After 200 ms, interrupts the thread that constructed it: a simulated interrupt (the flag
/// only), or a real SIGINT sent to that thread (pthread_kill), which the installed handler turns
/// into the flag. It owns the pipe's write end and, as a watchdog, closes it watchdog_timeout
/// later unless finish() came first. The thread is joined by finish() or the destructor
/// (std::jthread, the last member).
class Interrupter {
public:
    Interrupter(bool real_signal, md::UniqueFd write_end)
        : target_{::pthread_self()},
          thread_{[real_signal, target = target_, sent = &sent_, fired = &watchdog_fired_,
                   owned = std::move(write_end)](const std::stop_token& stop) mutable {
              std::this_thread::sleep_for(std::chrono::milliseconds{200});
              if (real_signal) {
                  sent->store(::pthread_kill(target, SIGINT) == 0);
              } else {
                  md::simulate_interrupt_for_testing(SIGINT);
                  sent->store(true);
              }
              const auto give_up = std::chrono::steady_clock::now() + watchdog_timeout;
              while (!stop.stop_requested() && std::chrono::steady_clock::now() < give_up) {
                  std::this_thread::sleep_for(std::chrono::milliseconds{1});
              }
              fired->store(!stop.stop_requested());
              owned.reset();
          }} {}
    Interrupter(const Interrupter&) = delete;
    Interrupter& operator=(const Interrupter&) = delete;
    Interrupter(Interrupter&&) = delete;
    Interrupter& operator=(Interrupter&&) = delete;
    ~Interrupter() = default;

    /// Stops the watchdog and joins the thread.
    void finish() {
        thread_.request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] bool sent() const noexcept { return sent_.load(); }
    [[nodiscard]] bool watchdog_fired() const noexcept { return watchdog_fired_.load(); }

private:
    std::atomic<bool> sent_{false};
    std::atomic<bool> watchdog_fired_{false};
    ThreadId target_{};
    std::jthread thread_{};
};

} // namespace

TEST_CASE("auth inputs: an interrupt ends a stalled --password-stdin read promptly (rev3 S18)",
          "[auth][auth-inputs]") {
    const FlagReset reset{};
    const bool real_signal = GENERATE(false, true);
    CAPTURE(real_signal);
    // The writer stays open and has written part of a line: read() alone would wait forever
    // (until the watchdog closes it).
    Pipe pipe = make_pipe();
    REQUIRE(::write(pipe.write_end.get(), "partial", 7) == 7);
    std::optional<md::Result<md::ScopedInterruptHandlers>> handlers{};
    if (real_signal) {
        handlers.emplace(md::ScopedInterruptHandlers::install()); // SA_RESTART, as in the app
        REQUIRE(handlers->has_value());
    }

    const auto started = std::chrono::steady_clock::now();
    Interrupter interrupter{real_signal, std::move(pipe.write_end)};
    const auto password = read_password_stdin(pipe.read_end.get());
    const auto waited = std::chrono::steady_clock::now() - started;
    interrupter.finish();

    CHECK(interrupter.sent());
    CHECK_FALSE(interrupter.watchdog_fired());
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == md::Errc::interrupted);
    CHECK(md::interrupt_requested());
    // 200 ms until the interrupt, then at most one 100 ms poll: well below the watchdog.
    CHECK(waited < watchdog_timeout);
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
    // No Catch2 macros on the thread: the write's outcome is checked after the join.
    std::atomic<bool> wrote{false};
    std::jthread writer{[write_end = std::move(pipe.write_end), done = &wrote] mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        const std::string line = std::string{mdx::fixture_password} + "\n";
        done->store(::write(write_end.get(), line.data(), line.size()) ==
                    static_cast<ssize_t>(line.size()));
        write_end.reset();
    }};
    const auto password = read_password_stdin(pipe.read_end.get());
    writer.join();
    CHECK(wrote.load());
    REQUIRE(password.has_value());
    CHECK(password->reveal() == mdx::fixture_password);
}

// ---- a descriptor that keeps reporting an error with nothing to read ----------------------------

namespace {

/// The one place a sockaddr_in is viewed as the generic sockaddr the socket API takes.
[[nodiscard]] sockaddr* as_sockaddr(sockaddr_in& address) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr punning (BSD API)
    return reinterpret_cast<sockaddr*>(&address);
}

[[nodiscard]] sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    return address;
}

/// A UDP port on 127.0.0.1 that nothing is bound to (bound once, then released): a datagram sent
/// there is answered with an ICMP "port unreachable".
[[nodiscard]] std::uint16_t unused_udp_port() {
    const md::UniqueFd probe{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    REQUIRE(probe.valid());
    sockaddr_in address = loopback_address(0);
    socklen_t length = sizeof address;
    REQUIRE(::bind(probe.get(), as_sockaddr(address), length) == 0);
    REQUIRE(::getsockname(probe.get(), as_sockaddr(address), &length) == 0);
    return ntohs(address.sin_port);
}

[[nodiscard]] bool has_pollerr(short revents) noexcept {
    return (static_cast<unsigned>(revents) & static_cast<unsigned>(POLLERR)) != 0U;
}

/// A non-blocking, connected UDP socket on which poll() reports POLLERR at once while read()
/// fails with EAGAIN. With IP_RECVERR, the ICMP "port unreachable" answering its datagram is kept
/// on the socket's error queue (POLLERR until it is read with MSG_ERRQUEUE); the pending socket
/// error the same ICMP set is consumed by one read() (ECONNREFUSED), so a later read() has nothing
/// to report and nothing to deliver. Both halves are checked before it is returned.
[[nodiscard]] md::UniqueFd erroring_socket() {
    md::UniqueFd fd{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    REQUIRE(fd.valid());
    const int on = 1;
    REQUIRE(::setsockopt(fd.get(), IPPROTO_IP, IP_RECVERR, &on, sizeof on) == 0);
    sockaddr_in target = loopback_address(unused_udp_port());
    REQUIRE(::connect(fd.get(), as_sockaddr(target), sizeof target) == 0);
    REQUIRE(::send(fd.get(), "x", 1, 0) == 1);

    pollfd entry{.fd = fd.get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&entry, 1, 5000) == 1); // the ICMP answer has arrived
    REQUIRE(has_pollerr(entry.revents));
    std::array<char, 16> scratch{};
    const ssize_t refused = ::read(fd.get(), scratch.data(), scratch.size());
    const int refused_errno = errno;
    REQUIRE(refused == -1);
    REQUIRE(refused_errno == ECONNREFUSED); // the pending error, now consumed

    entry.revents = 0;
    REQUIRE(::poll(&entry, 1, 0) == 1); // still POLLERR: the error queue is not empty
    REQUIRE(has_pollerr(entry.revents));
    const ssize_t again = ::read(fd.get(), scratch.data(), scratch.size());
    const int again_errno = errno;
    REQUIRE(again == -1);
    REQUIRE(again_errno == EAGAIN); // and nothing else to read
    return fd;
}

} // namespace

TEST_CASE("auth inputs: a descriptor reporting an error with nothing to read fails, not spins",
          "[auth][auth-inputs]") {
    const FlagReset reset{};
    const md::UniqueFd fd = erroring_socket();
    // Without the POLLERR check read_password_stdin would spin (POLLERR at once, then EAGAIN)
    // until an interrupt: the watchdog simulates one after watchdog_timeout, so a regression fails
    // on the result instead of hanging. `fired` outlives the thread: it is declared first, and
    // the thread is joined below (or by ~jthread) before it goes away.
    std::atomic<bool> fired{false};
    std::jthread watchdog{[fired_flag = &fired](const std::stop_token& stop) {
        const auto give_up = std::chrono::steady_clock::now() + watchdog_timeout;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (!stop.stop_requested()) {
            fired_flag->store(true);
            md::simulate_interrupt_for_testing(SIGINT);
        }
    }};
    const auto password = read_password_stdin(fd.get());
    watchdog.request_stop();
    watchdog.join();

    CHECK_FALSE(fired.load());
    REQUIRE_FALSE(password.has_value());
    CHECK(password.error().code == md::Errc::failure);
    CHECK(password.error().exit_code() == 1);
    CHECK(password.error().message == "cannot read stdin: the descriptor reported an error");
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
