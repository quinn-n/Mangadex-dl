// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// tests/support/fake_prompter.hpp: a scripted Prompter for unit and in-process CLI tests
// (follows plan T12).
//
// ask() answers from a FIFO queue of scripted replies: an answer (`answer("text")`) or a bare
// status (`reply(PromptStatus::interrupted)`, `empty`, `too_long`, `unavailable`). An empty queue
// answers `unavailable` (no usable terminal), so a test that forgot a reply fails on the login
// flow's "can't ask" path instead of hanging. Every notice() and ask() is logged in order
// (events()), and every ask() also as a Call (question + echo), so a test can check the prompt
// order, the echo mode and that the hint came first.
//
// Test-only: answers are plain copies of fake values (support/mdx_auth_fixture.hpp); a reply is
// handed out as a Secret, as the real prompter does. Not thread-safe (the login flow asks from one
// thread).
//
//     mangadex_dl::test::FakePrompter prompter;
//     prompter.answer(mdx::fixture_client_id);
//     prompter.reply(mangadex_dl::PromptStatus::interrupted);
//     ... LoginRequest{.prompter = &prompter} ...
//     CHECK(prompter.calls().size() == 2);

#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::test {

class FakePrompter final : public Prompter {
public:
    /// One ask(): the question as written and the echo mode asked for.
    struct Call {
        std::string question{};
        Echo echo = Echo::on;

        friend bool operator==(const Call&, const Call&) = default;
    };

    FakePrompter() = default;

    /// Queues an `answered` reply with `text` (which should be non-empty, as a real answer is).
    void answer(std::string_view text) {
        replies_.push_back(Scripted{.status = PromptStatus::answered, .text = std::string{text}});
    }

    /// Queues a reply with `status` and no answer (empty, too_long, interrupted, unavailable).
    void reply(PromptStatus status) { replies_.push_back(Scripted{.status = status}); }

    void notice(std::string_view line) override {
        notices_.emplace_back(line);
        events_.push_back(std::string{"notice: "}.append(line));
    }

    [[nodiscard]] PromptReply ask(std::string_view question, Echo echo) override {
        calls_.push_back(Call{.question = std::string{question}, .echo = echo});
        events_.push_back(
            std::string{echo == Echo::off ? "ask (echo off): " : "ask (echo on): "}.append(
                question));
        if (replies_.empty()) {
            return PromptReply{.status = PromptStatus::unavailable};
        }
        Scripted next = std::move(replies_.front());
        replies_.pop_front();
        if (next.status != PromptStatus::answered) {
            return PromptReply{.status = next.status};
        }
        return PromptReply{.status = PromptStatus::answered,
                           .answer = Secret::adopt(std::move(next.text))};
    }

    /// Every ask() so far, in order.
    [[nodiscard]] const std::vector<Call>& calls() const noexcept { return calls_; }
    /// Every notice() line so far, in order.
    [[nodiscard]] const std::vector<std::string>& notices() const noexcept { return notices_; }
    /// notice() and ask() together, in order: "notice: <line>", "ask (echo on|off): <question>".
    [[nodiscard]] const std::vector<std::string>& events() const noexcept { return events_; }
    /// Scripted replies not yet taken.
    [[nodiscard]] std::size_t remaining() const noexcept { return replies_.size(); }

private:
    struct Scripted {
        PromptStatus status = PromptStatus::unavailable;
        std::string text{};
    };

    std::deque<Scripted> replies_{};
    std::vector<Call> calls_{};
    std::vector<std::string> notices_{};
    std::vector<std::string> events_{};
};

} // namespace mangadex_dl::test
