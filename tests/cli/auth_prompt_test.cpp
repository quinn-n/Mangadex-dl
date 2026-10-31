// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/auth_prompt_test.cpp: the real terminal prompter, TerminalPrompter (follows plan T7),
// over a pseudo-terminal and over pipes.
//
// Pty cases (support/pty_pair.hpp): the prompter reads the slave; a "typist" thread types into
// the master. For Echo::off the typist first waits until tcgetattr(master) shows ECHO cleared
// (the master reports the slave's termios), so nothing it types can be flushed by TCSAFLUSH or
// echoed. If the typist gives up waiting it simulates an interrupt, so ask() returns instead of
// hanging and the test fails on the status. Every typist joins before the descriptors close: the
// PtyPair is always declared before it.
//
// When posix_openpt itself fails (no /dev/ptmx, e.g. a CI container), the pty cases SKIP with
// the reason; catch_discover_tests sets SKIP_RETURN_CODE 4, so ctest reports them as Skipped,
// never as Passed. A failure after posix_openpt succeeded is a real failure.
//
// Pipe cases cover what a canonical-mode terminal can't produce (an ICANON line stops at 4095
// bytes): the too-long answer, the 4096-byte boundary, '\r\n', end of input, and Echo::off on a
// descriptor that is not a terminal.
//
// Catch2 test names start with "prompt:" so `pixi run tsan -R 'cli\.prompt'` selects them.

#include "app/cmd_auth_prompt.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/prompt.hpp"
#include "support/pty_pair.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <ios>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <sys/poll.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): pthread_kill and SIGINT are POSIX <signal.h>
#include <signal.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
namespace mdt = mangadex_dl::test;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// Clears the process-wide interrupt flag before and after each use, so test order does not
/// matter.
class FlagReset {
public:
    FlagReset() noexcept { md::reset_interrupt_for_testing(); }
    FlagReset(const FlagReset&) = delete;
    FlagReset& operator=(const FlagReset&) = delete;
    FlagReset(FlagReset&&) = delete;
    FlagReset& operator=(FlagReset&&) = delete;
    ~FlagReset() { md::reset_interrupt_for_testing(); }
};

/// A pty pair, or a SKIP when the system has no pseudo-terminals.
[[nodiscard]] mdt::PtyPair require_pty() {
    mdt::PtyPair pty = mdt::open_pty_pair();
    if (!pty.master.valid()) {
        SKIP("no pseudo-terminal: " << pty.failure);
    }
    INFO(pty.failure);
    REQUIRE(pty.slave.valid());
    return pty;
}

/// The local-mode flags of the terminal behind `fd`; required to succeed.
[[nodiscard]] tcflag_t local_flags(int fd) {
    termios attributes{};
    REQUIRE(::tcgetattr(fd, &attributes) == 0);
    return attributes.c_lflag;
}

constexpr tcflag_t echo_flag = ECHO;

// NOLINTNEXTLINE(misc-include-cleaner): POSIX pthread_t; glibc defines it in a bits/ header
using ThreadId = pthread_t;

/// No Catch2 macros: it also runs on the typist thread. A failed tcgetattr reads as "echo on".
[[nodiscard]] bool echo_is_on(int fd) noexcept {
    termios attributes{};
    if (::tcgetattr(fd, &attributes) != 0) {
        return true;
    }
    return (attributes.c_lflag & echo_flag) != 0;
}

/// Writes all of `text` to `fd` (EINTR retried). False on any other error. No Catch2 macros.
[[nodiscard]] bool write_all(int fd, std::string_view text) noexcept {
    while (!text.empty()) {
        const ssize_t wrote = ::write(fd, text.data(), text.size());
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(wrote));
    }
    return true;
}

/// Everything `fd` delivers until it has been quiet for `quiet` (at most 5 s in total). Echo
/// output reaches the master asynchronously, so a single non-blocking read could miss it.
[[nodiscard]] std::string drain(int fd, milliseconds quiet) {
    std::string seen{};
    std::array<char, 1024> chunk{};
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds{5};
    while (Clock::now() < give_up) {
        pollfd entry{.fd = fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&entry, 1, static_cast<int>(quiet.count()));
        if (ready <= 0) {
            break; // quiet for `quiet`, or poll failed
        }
        const ssize_t got = ::read(fd, chunk.data(), chunk.size());
        if (got <= 0) {
            break;
        }
        seen.append(chunk.data(), static_cast<std::size_t>(got));
    }
    return seen;
}

enum class Then {
    type,               ///< write the text to the master
    simulate_interrupt, ///< simulate_interrupt_for_testing(SIGINT)
    send_sigint,        ///< pthread_kill(the constructing thread, SIGINT): a real handler run
};

struct TypistPlan {
    bool wait_for_echo_off = true; ///< first wait until the slave's ECHO flag is cleared
    Then then = Then::type;
    std::string text{};
};

/// The other side of the keyboard: one thread that carries out a TypistPlan on the master.
/// `master` is borrowed: declare the PtyPair before the Typist, so the thread joins first.
class Typist {
public:
    Typist(int master, TypistPlan plan)
        : target_{::pthread_self()},
          // The thread only touches `timed_out_` and `wrote_`, members that outlive it: thread_
          // is declared last, so it is joined before they are destroyed.
          thread_{[master, owned = std::move(plan), target = target_, timed_out = &timed_out_,
                   wrote = &wrote_] { carry_out(master, owned, target, *timed_out, *wrote); }} {}
    Typist(const Typist&) = delete;
    Typist& operator=(const Typist&) = delete;
    Typist(Typist&&) = delete;
    Typist& operator=(Typist&&) = delete;
    ~Typist() = default;

    /// Joins the thread; call it before looking at the results or the master.
    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] bool timed_out() const noexcept { return timed_out_.load(); }
    [[nodiscard]] bool wrote() const noexcept { return wrote_.load(); }

private:
    static void carry_out(int master, const TypistPlan& plan, ThreadId target,
                          std::atomic<bool>& timed_out, std::atomic<bool>& wrote) noexcept {
        const Clock::time_point give_up = Clock::now() + std::chrono::seconds{10};
        while (plan.wait_for_echo_off && echo_is_on(master)) {
            if (Clock::now() > give_up) {
                timed_out.store(true);
                md::simulate_interrupt_for_testing(SIGINT); // unblocks ask(); the test fails
                return;
            }
            std::this_thread::sleep_for(milliseconds{1});
        }
        switch (plan.then) {
        case Then::type:
            wrote.store(write_all(master, plan.text));
            return;
        case Then::simulate_interrupt:
            md::simulate_interrupt_for_testing(SIGINT);
            return;
        case Then::send_sigint:
            wrote.store(::pthread_kill(target, SIGINT) == 0);
            return;
        }
    }

    std::atomic<bool> timed_out_{false};
    std::atomic<bool> wrote_{false};
    ThreadId target_{};
    std::jthread thread_{};
};

/// A diagnostics sink whose every write fails. Before failing it records whether the terminal
/// behind `fd` had echo on, so a test can prove the failure happened while echo was off.
class FailingBuf final : public std::streambuf {
public:
    explicit FailingBuf(int fd) noexcept : fd_(fd) {}

    [[nodiscard]] bool wrote_while_echo_off() const noexcept { return wrote_while_echo_off_; }

protected:
    int_type overflow(int_type /*c*/) override {
        wrote_while_echo_off_ = !echo_is_on(fd_);
        return traits_type::eof();
    }

private:
    int fd_ = -1;
    bool wrote_while_echo_off_ = false;
};

/// A pipe; both ends close-on-exec and owned.
struct Pipe {
    md::UniqueFd read_end{};
    md::UniqueFd write_end{};
};

[[nodiscard]] Pipe make_pipe() {
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    return Pipe{.read_end = md::UniqueFd{ends[0]}, .write_end = md::UniqueFd{ends[1]}};
}

/// Asks once over a pipe that holds `input`; `close_writer` closes the write end first (end of
/// input after `input`).
[[nodiscard]] md::PromptReply ask_over_pipe(std::string_view input, bool close_writer,
                                            std::ostringstream& err, md::Echo echo = md::Echo::on) {
    Pipe pipe = make_pipe();
    REQUIRE(write_all(pipe.write_end.get(), input));
    if (close_writer) {
        pipe.write_end.reset();
    }
    md::app::TerminalPrompter prompter{pipe.read_end.get(), err};
    return prompter.ask("Question: ", echo);
}

constexpr std::string_view question = "MangaDex password for reader: ";

} // namespace

// ---- pseudo-terminal ----------------------------------------------------------------------------

TEST_CASE("prompt: echo off reads the answer without echoing it and restores the terminal",
          "[prompt]") {
    const FlagReset reset{};
    const mdt::PtyPair pty = require_pty();
    const tcflag_t before = local_flags(pty.slave.get());
    REQUIRE((before & echo_flag) != 0); // a new pty echoes

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    Typist typist{pty.master.get(), TypistPlan{.text = "s3cret\n"}};
    const md::PromptReply reply = prompter.ask(question, md::Echo::off);
    typist.join();

    CHECK_FALSE(typist.timed_out());
    CHECK(typist.wrote());
    CHECK(reply.status == md::PromptStatus::answered);
    CHECK(reply.answer.reveal() == "s3cret");
    CHECK(local_flags(pty.slave.get()) == before);
    CHECK(err.str() == std::string{question} + "\n");
    const std::string echoed = drain(pty.master.get(), milliseconds{200});
    CHECK(echoed.find("s3cret") == std::string::npos);
}

TEST_CASE("prompt: echo on leaves the terminal alone and the answer is echoed", "[prompt]") {
    const FlagReset reset{};
    const mdt::PtyPair pty = require_pty();
    const tcflag_t before = local_flags(pty.slave.get());

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    Typist typist{pty.master.get(), TypistPlan{.wait_for_echo_off = false, .text = "reader\n"}};
    const md::PromptReply reply = prompter.ask("MangaDex username: ", md::Echo::on);
    typist.join();

    CHECK(typist.wrote());
    CHECK(reply.status == md::PromptStatus::answered);
    CHECK(reply.answer.reveal() == "reader");
    CHECK(local_flags(pty.slave.get()) == before);
    CHECK(err.str() == "MangaDex username: "); // the terminal echoed the newline itself
    // The positive control for the echo-off case: the same drain does see an echo.
    const std::string echoed = drain(pty.master.get(), milliseconds{200});
    CHECK(echoed.find("reader") != std::string::npos);
}

TEST_CASE("prompt: a 4095-byte line, the canonical-mode maximum, is answered", "[prompt]") {
    const FlagReset reset{};
    const mdt::PtyPair pty = require_pty();
    const std::string typed(4095, 'k');

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    Typist typist{pty.master.get(), TypistPlan{.text = typed + "\n"}};
    const md::PromptReply reply = prompter.ask(question, md::Echo::off);
    typist.join();

    CHECK_FALSE(typist.timed_out());
    CHECK(typist.wrote());
    REQUIRE(reply.status == md::PromptStatus::answered);
    CHECK(reply.answer.size() == 4095);
    CHECK(reply.answer.reveal() == typed);
}

TEST_CASE("prompt: an interrupt while echo is off returns interrupted and restores echo",
          "[prompt]") {
    const FlagReset reset{};
    const mdt::PtyPair pty = require_pty();
    const tcflag_t before = local_flags(pty.slave.get());

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    Typist typist{pty.master.get(), TypistPlan{.then = Then::simulate_interrupt}};
    const md::PromptReply reply = prompter.ask(question, md::Echo::off);
    typist.join();

    CHECK_FALSE(typist.timed_out());
    CHECK(reply.status == md::PromptStatus::interrupted);
    CHECK(reply.answer.empty());
    CHECK(local_flags(pty.slave.get()) == before);
    CHECK(err.str() == std::string{question} + "\n");
}

TEST_CASE("prompt: a real SIGINT under the SA_RESTART handlers ends the wait", "[prompt]") {
    const FlagReset reset{};
    const mdt::PtyPair pty = require_pty();
    const tcflag_t before = local_flags(pty.slave.get());
    const auto handlers = md::ScopedInterruptHandlers::install();
    REQUIRE(handlers.has_value());

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    Typist typist{pty.master.get(), TypistPlan{.then = Then::send_sigint}};
    const md::PromptReply reply = prompter.ask(question, md::Echo::off);
    typist.join();

    CHECK_FALSE(typist.timed_out());
    CHECK(typist.wrote()); // pthread_kill succeeded
    CHECK(reply.status == md::PromptStatus::interrupted);
    CHECK(md::interrupt_signal() == SIGINT);
    CHECK(reply.answer.empty());
    CHECK(local_flags(pty.slave.get()) == before);
    CHECK(err.str() == std::string{question} + "\n");
}

TEST_CASE("prompt: the master closing while the prompter waits is end of input", "[prompt]") {
    const FlagReset reset{};
    mdt::PtyPair pty = require_pty();

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    // The closer owns the master: the slave then reports POLLIN|POLLHUP and read() fails (EIO).
    std::jthread closer{[master = std::move(pty.master)] mutable {
        std::this_thread::sleep_for(milliseconds{200});
        master.reset();
    }};
    const md::PromptReply reply = prompter.ask("MangaDex username: ", md::Echo::on);
    closer.join();

    CHECK(reply.status == md::PromptStatus::empty);
    CHECK(reply.answer.empty());
}

TEST_CASE("prompt: an exception while echo is off still restores the terminal", "[prompt]") {
    const FlagReset reset{};
    const mdt::PtyPair pty = require_pty();
    const tcflag_t before = local_flags(pty.slave.get());
    REQUIRE((before & echo_flag) != 0);

    // Writing the question fails, and badbit throws: the throw leaves ask() after echo went off
    // and before anything was read. No typist: nothing is ever typed.
    FailingBuf failing{pty.slave.get()};
    std::ostream err{&failing};
    err.exceptions(std::ios::badbit);
    md::app::TerminalPrompter prompter{pty.slave.get(), err};
    CHECK_THROWS_AS(prompter.ask(question, md::Echo::off), std::ios_base::failure);

    CHECK(failing.wrote_while_echo_off()); // the exception really left with echo off
    CHECK(local_flags(pty.slave.get()) == before);
}

// ---- pipes --------------------------------------------------------------------------------------

TEST_CASE("prompt: 5000 bytes on a pipe are too long", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    const md::PromptReply reply = ask_over_pipe(std::string(5000, 'x'), false, err);
    CHECK(reply.status == md::PromptStatus::too_long);
    CHECK(reply.answer.empty());
    CHECK(err.str() == "Question: ");
}

TEST_CASE("prompt: the answer limit is 4096 bytes without the line ending", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    const std::string longest(md::max_prompt_answer_bytes, 'y');

    SECTION("4096 bytes and a newline are answered") {
        const md::PromptReply reply = ask_over_pipe(longest + "\n", false, err);
        REQUIRE(reply.status == md::PromptStatus::answered);
        CHECK(reply.answer.reveal() == longest);
    }
    SECTION("4096 bytes and a CRLF are answered") {
        const md::PromptReply reply = ask_over_pipe(longest + "\r\n", false, err);
        REQUIRE(reply.status == md::PromptStatus::answered);
        CHECK(reply.answer.reveal() == longest);
    }
    SECTION("4097 bytes and a newline are too long") {
        const md::PromptReply reply = ask_over_pipe(longest + "y\n", false, err);
        CHECK(reply.status == md::PromptStatus::too_long);
        CHECK(reply.answer.empty());
    }
    SECTION("4097 bytes and end of input are too long") {
        const md::PromptReply reply = ask_over_pipe(longest + "y", true, err);
        CHECK(reply.status == md::PromptStatus::too_long);
        CHECK(reply.answer.empty());
    }
}

TEST_CASE("prompt: end of input before any byte is empty", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    const md::PromptReply reply = ask_over_pipe("", true, err);
    CHECK(reply.status == md::PromptStatus::empty);
    CHECK(reply.answer.empty());
}

TEST_CASE("prompt: an empty line is empty", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    SECTION("a bare newline") {
        const md::PromptReply reply = ask_over_pipe("\n", false, err);
        CHECK(reply.status == md::PromptStatus::empty);
    }
    SECTION("a bare CRLF") {
        const md::PromptReply reply = ask_over_pipe("\r\n", false, err);
        CHECK(reply.status == md::PromptStatus::empty);
    }
}

TEST_CASE("prompt: line endings are stripped and end of input ends a line", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    SECTION("CRLF") {
        const md::PromptReply reply = ask_over_pipe("hunter\r\n", false, err);
        REQUIRE(reply.status == md::PromptStatus::answered);
        CHECK(reply.answer.reveal() == "hunter");
    }
    SECTION("a lone CR inside the answer is kept") {
        const md::PromptReply reply = ask_over_pipe("a\rb\n", false, err);
        REQUIRE(reply.status == md::PromptStatus::answered);
        CHECK(reply.answer.reveal() == "a\rb");
    }
    SECTION("end of input after some bytes") {
        const md::PromptReply reply = ask_over_pipe("partial", true, err);
        REQUIRE(reply.status == md::PromptStatus::answered);
        CHECK(reply.answer.reveal() == "partial");
    }
    SECTION("only the first line of one read is kept") {
        const md::PromptReply reply = ask_over_pipe("first\nsecond\n", false, err);
        REQUIRE(reply.status == md::PromptStatus::answered);
        CHECK(reply.answer.reveal() == "first");
    }
}

TEST_CASE("prompt: echo off on a pipe is unavailable and writes nothing", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    const md::PromptReply reply = ask_over_pipe("s3cret\n", false, err, md::Echo::off);
    CHECK(reply.status == md::PromptStatus::unavailable);
    CHECK(reply.answer.empty());
    CHECK(err.str().empty());
}

TEST_CASE("prompt: a closed stdin is unavailable and writes nothing", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    md::app::TerminalPrompter prompter{-1, err};
    CHECK(prompter.ask("Question: ", md::Echo::on).status == md::PromptStatus::unavailable);
    CHECK(prompter.ask("Question: ", md::Echo::off).status == md::PromptStatus::unavailable);
    CHECK(err.str().empty());
}

TEST_CASE("prompt: a pending interrupt returns interrupted without reading", "[prompt]") {
    const FlagReset reset{};
    Pipe pipe = make_pipe();
    REQUIRE(write_all(pipe.write_end.get(), "unread\n"));
    md::simulate_interrupt_for_testing(SIGTERM);

    std::ostringstream err{};
    md::app::TerminalPrompter prompter{pipe.read_end.get(), err};
    const md::PromptReply reply = prompter.ask("Question: ", md::Echo::on);
    CHECK(reply.status == md::PromptStatus::interrupted);
    CHECK(reply.answer.empty());
    // The line is still in the pipe.
    pipe.write_end.reset();
    CHECK(drain(pipe.read_end.get(), milliseconds{50}) == "unread\n");
}

TEST_CASE("prompt: questions and notices are made terminal-safe", "[prompt]") {
    const FlagReset reset{};
    std::ostringstream err{};
    SECTION("notice adds the newline") {
        md::app::TerminalPrompter prompter{-1, err};
        prompter.notice("mangadex-dl: create a client \x1b[31mnow");
        CHECK(err.str() == "mangadex-dl: create a client ?[31mnow\n");
    }
    SECTION("a username with control bytes in the question") {
        const Pipe pipe = make_pipe();
        REQUIRE(write_all(pipe.write_end.get(), "x\n"));
        md::app::TerminalPrompter prompter{pipe.read_end.get(), err};
        const md::PromptReply reply = prompter.ask("MangaDex password for a\rb: ", md::Echo::on);
        CHECK(reply.status == md::PromptStatus::answered);
        CHECK(err.str() == "MangaDex password for a?b: ");
    }
}
