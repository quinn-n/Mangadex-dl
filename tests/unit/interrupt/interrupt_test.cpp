// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/interrupt/interrupt_test.cpp: the SIGINT/SIGTERM flag handler, its scoped install
// (which must restore whatever was there before: inside a test case that is Catch2's own fatal
// signal handler), the re-raise in a forked child, and InterruptBridge's stop latency.

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

// NOLINTNEXTLINE(modernize-deprecated-headers): sigaction and raise are POSIX <signal.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// Whether SIGINT, SIGTERM and SIGPIPE are all blocked in the calling thread. No Catch2 macros:
/// it also runs on pool and poller threads.
[[nodiscard]] bool async_signals_blocked() noexcept {
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t mask{};
    if (pthread_sigmask(SIG_BLOCK, nullptr, &mask) != 0) {
        return false;
    }
    return sigismember(&mask, SIGINT) == 1 && sigismember(&mask, SIGTERM) == 1 &&
           sigismember(&mask, SIGPIPE) == 1;
}

/// The calling thread's whole signal mask, as sigismember results for signals 1..64.
[[nodiscard]] std::array<int, 64> signal_mask_snapshot() {
    sigset_t mask{};
    REQUIRE(pthread_sigmask(SIG_BLOCK, nullptr, &mask) == 0);
    std::array<int, 64> members{};
    int signal = 1;
    for (int& member : members) {
        member = sigismember(&mask, signal);
        ++signal;
    }
    return members;
}

/// Clears the process-wide flag before and after each use, so test order does not matter.
class FlagReset {
public:
    FlagReset() noexcept { md::reset_interrupt_for_testing(); }
    FlagReset(const FlagReset&) = delete;
    FlagReset& operator=(const FlagReset&) = delete;
    FlagReset(FlagReset&&) = delete;
    FlagReset& operator=(FlagReset&&) = delete;
    ~FlagReset() { md::reset_interrupt_for_testing(); }
};

[[nodiscard]] struct sigaction current_action(int signal) {
    struct sigaction action{};
    REQUIRE(::sigaction(signal, nullptr, &action) == 0);
    return action;
}

[[nodiscard]] bool has_flag(int flags, int flag) noexcept {
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0U;
}

[[nodiscard]] bool same_handler(const struct sigaction& a, const struct sigaction& b) {
    if (has_flag(a.sa_flags, SA_SIGINFO) != has_flag(b.sa_flags, SA_SIGINFO)) {
        return false;
    }
    if (has_flag(a.sa_flags, SA_SIGINFO)) {
        return a.sa_sigaction == b.sa_sigaction;
    }
    return a.sa_handler == b.sa_handler;
}

/// Sets SIG_IGN for a signal and restores the previous disposition on every path.
class ScopedIgnore {
public:
    explicit ScopedIgnore(int signal) : signal_(signal) {
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        REQUIRE(sigemptyset(&ignore.sa_mask) == 0);
        REQUIRE(::sigaction(signal_, &ignore, &previous_) == 0);
    }
    ScopedIgnore(const ScopedIgnore&) = delete;
    ScopedIgnore& operator=(const ScopedIgnore&) = delete;
    ScopedIgnore(ScopedIgnore&&) = delete;
    ScopedIgnore& operator=(ScopedIgnore&&) = delete;
    ~ScopedIgnore() { static_cast<void>(::sigaction(signal_, &previous_, nullptr)); }

private:
    int signal_ = 0;
    struct sigaction previous_{};
};

/// Waits up to `limit` for `source` to be stopped; the time it took, or nullopt.
[[nodiscard]] std::optional<milliseconds>
time_until_stopped(const std::stop_source& source, Clock::time_point since, milliseconds limit) {
    while (!source.stop_requested()) {
        if (Clock::now() - since > limit) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(milliseconds{1});
    }
    return std::chrono::duration_cast<milliseconds>(Clock::now() - since);
}

} // namespace

TEST_CASE("interrupt simulate sets the flag and reset clears it", "[interrupt]") {
    const FlagReset reset{};
    CHECK_FALSE(md::interrupt_requested());
    CHECK(md::interrupt_signal() == 0);

    md::simulate_interrupt_for_testing(SIGTERM);
    CHECK(md::interrupt_requested());
    CHECK(md::interrupt_signal() == SIGTERM);

    md::reset_interrupt_for_testing();
    CHECK_FALSE(md::interrupt_requested());
    CHECK(md::interrupt_signal() == 0);
}

TEST_CASE("interrupt a real SIGINT or SIGTERM sets the flag and the process survives",
          "[interrupt]") {
    const FlagReset reset{};
    const int signal = GENERATE(SIGINT, SIGTERM);
    CAPTURE(signal);
    const auto handlers = md::ScopedInterruptHandlers::install();
    REQUIRE(handlers.has_value());

    REQUIRE(::raise(signal) == 0);
    CHECK(md::interrupt_requested());
    CHECK(md::interrupt_signal() == signal);
}

TEST_CASE("interrupt handlers restore the previous dispositions", "[interrupt]") {
    const FlagReset reset{};
    const struct sigaction before_int = current_action(SIGINT);
    const struct sigaction before_term = current_action(SIGTERM);
    std::optional<md::ScopedInterruptHandlers> owner{};
    {
        auto handlers = md::ScopedInterruptHandlers::install();
        REQUIRE(handlers.has_value());
        CHECK_FALSE(same_handler(current_action(SIGINT), before_int));
        CHECK_FALSE(same_handler(current_action(SIGTERM), before_term));
        CHECK(has_flag(current_action(SIGINT).sa_flags, SA_RESTART));
        owner.emplace(std::move(*handlers));
    }
    // The moved-from object was destroyed above and restored nothing; the new owner restores.
    CHECK_FALSE(same_handler(current_action(SIGINT), before_int));
    owner.reset();
    CHECK(same_handler(current_action(SIGINT), before_int));
    CHECK(same_handler(current_action(SIGTERM), before_term));
}

TEST_CASE("interrupt refuses a second live install", "[interrupt]") {
    const FlagReset reset{};
    {
        auto first = md::ScopedInterruptHandlers::install();
        REQUIRE(first.has_value());
        const auto second = md::ScopedInterruptHandlers::install();
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == md::Errc::failure);

        const md::ScopedInterruptHandlers moved{std::move(*first)};
        const auto third = md::ScopedInterruptHandlers::install();
        CHECK_FALSE(third.has_value());
    }
    const auto after = md::ScopedInterruptHandlers::install();
    CHECK(after.has_value());
}

TEST_CASE("interrupt reraise does nothing without a pending interrupt", "[interrupt]") {
    const FlagReset reset{};
    md::reraise_pending_interrupt(); // returns: nothing pending
    md::simulate_interrupt_for_testing(SIGUSR1);
    md::reraise_pending_interrupt(); // returns: only SIGINT/SIGTERM are re-raised
    CHECK(md::interrupt_signal() == SIGUSR1);
}

TEST_CASE("interrupt reraise kills the process with the pending signal", "[interrupt]") {
    const int signal = GENERATE(SIGINT, SIGTERM);
    CAPTURE(signal);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // Child: only async-signal-safe work, then _exit if the re-raise did not kill us.
        md::simulate_interrupt_for_testing(signal);
        md::reraise_pending_interrupt();
        ::_exit(99);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX <sys/wait.h> macro, defined in a bits/ header
    CHECK(WIFSIGNALED(status));
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX <sys/wait.h> macro, defined in a bits/ header
    CHECK(WTERMSIG(status) == signal);
}

TEST_CASE("interrupt bridge requests stop within 500 ms", "[interrupt]") {
    const FlagReset reset{};
    const std::stop_source source{};
    const md::InterruptBridge bridge{source};
    std::this_thread::sleep_for(milliseconds{20});
    CHECK_FALSE(source.stop_requested());

    // The poll interval is 50 ms; 500 ms leaves room for a loaded, sanitized CI machine.
    const Clock::time_point raised = Clock::now();
    md::simulate_interrupt_for_testing(SIGINT);
    const std::optional<milliseconds> latency =
        time_until_stopped(source, raised, milliseconds{500});
    REQUIRE(latency.has_value());
    CHECK((latency.has_value() && latency->count() <= 500));
}

TEST_CASE("interrupt bridge sees an interrupt that arrived before it started", "[interrupt]") {
    const FlagReset reset{};
    md::simulate_interrupt_for_testing(SIGTERM);
    const std::stop_source source{};
    const md::InterruptBridge bridge{source};
    CHECK(time_until_stopped(source, Clock::now(), milliseconds{500}).has_value());
}

TEST_CASE("interrupt bridge without an interrupt never requests stop", "[interrupt]") {
    const FlagReset reset{};
    const std::stop_source source{};
    {
        const md::InterruptBridge bridge{source};
        std::this_thread::sleep_for(milliseconds{120}); // more than two poll intervals
        CHECK_FALSE(source.stop_requested());
    }
    CHECK_FALSE(source.stop_requested());
}

TEST_CASE("interrupt bridge destructor wakes the poller instead of waiting out the interval",
          "[interrupt]") {
    const FlagReset reset{};
    // Each cycle destroys the bridge 10 ms into the poller's 50 ms wait. A poller that slept out
    // its interval would make every destruction take about 40 ms; waking on the jthread's stop
    // token makes it near-instant. The best of 5 cycles must beat 25 ms, so one slow cycle on a
    // loaded machine cannot fail the test, while a sleeping poller always does.
    milliseconds best{milliseconds::max()};
    for (int cycle = 0; cycle < 5; ++cycle) {
        const std::stop_source source{};
        auto bridge = std::make_unique<md::InterruptBridge>(source);
        std::this_thread::sleep_for(milliseconds{10});
        const Clock::time_point start = Clock::now();
        bridge.reset();
        best = std::min(best, std::chrono::duration_cast<milliseconds>(Clock::now() - start));
        CHECK_FALSE(source.stop_requested());
    }
    CAPTURE(best.count());
    CHECK(best < milliseconds{25});
}

TEST_CASE("interrupt an inherited SIG_IGN stays ignored", "[interrupt]") {
    const FlagReset reset{};
    const int ignored = GENERATE(SIGINT, SIGTERM);
    const int other = ignored == SIGINT ? SIGTERM : SIGINT;
    CAPTURE(ignored);

    // Make `ignored` SIG_IGN, as nohup or a non-interactive shell's background job would.
    const ScopedIgnore ignore{ignored};
    {
        const auto handlers = md::ScopedInterruptHandlers::install();
        REQUIRE(handlers.has_value());
        CHECK(current_action(ignored).sa_handler == SIG_IGN);

        REQUIRE(::raise(ignored) == 0); // ignored: the process survives, the flag stays clear
        CHECK_FALSE(md::interrupt_requested());

        REQUIRE(::raise(other) == 0); // the other signal is still handled
        CHECK(md::interrupt_signal() == other);
    }
    CHECK(current_action(ignored).sa_handler == SIG_IGN); // not "restored" to anything else
}

TEST_CASE("interrupt bridge poller starts with SIGINT SIGTERM and SIGPIPE blocked", "[interrupt]") {
    const FlagReset reset{};
    REQUIRE_FALSE(async_signals_blocked()); // otherwise inheritance alone would pass the test
    const std::array<int, 64> before = signal_mask_snapshot();

    // The bridge calls request_stop() on the poller thread, so this callback runs there.
    std::atomic<int> poller_masked{-1};
    std::atomic<bool> on_other_thread{false};
    const std::stop_source source{};
    const std::stop_callback probe{source.get_token(),
                                   [masked_ptr = &poller_masked, other_ptr = &on_other_thread,
                                    test_thread = std::this_thread::get_id()] {
                                       masked_ptr->store(async_signals_blocked() ? 1 : 0);
                                       other_ptr->store(std::this_thread::get_id() != test_thread);
                                   }};
    {
        const md::InterruptBridge bridge{source};
        CHECK(signal_mask_snapshot() == before); // the creating thread's mask is restored exactly
        md::simulate_interrupt_for_testing(SIGINT);
        REQUIRE(time_until_stopped(source, Clock::now(), milliseconds{500}).has_value());
    }
    CHECK(on_other_thread.load());
    CHECK(poller_masked.load() == 1);
    CHECK(signal_mask_snapshot() == before);
}
