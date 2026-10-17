// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_hello.hpp"

#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"
#include "mangadex_dl/greet.hpp"
#include "mangadex_dl/version.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>

namespace mangadex_dl::app {
namespace {

class HelloCommand final : public Command {
public:
    HelloCommand() = default;

    /// Binds options to members of this object (never to locals of register_hello).
    void bind(CLI::App& sub) {
        sub.add_option("name", name_, "Who to greet")->capture_default_str();
        sub.add_option("-n,--count", count_, "How many times to greet")
            ->check(CLI::Range(1, max_count))
            ->capture_default_str();
    }

    [[nodiscard]] int run(Io& io, const GlobalOptions& globals) override {
        const Result<std::string> greeting = greet(name_);
        if (!greeting) {
            std::print(io.err, "{}: {}\n", tool_name, greeting.error().message);
            return greeting.error().exit_code();
        }
        if (globals.json) {
            // One document, built whole and written once; no ANSI ever.
            nlohmann::json doc = nlohmann::json::object();
            doc["greeting"] = *greeting;
            doc["count"] = count_;
            // replace: argv and paths are bytes on Linux; strict mode throws on invalid UTF-8.
            std::print(io.out, "{}\n",
                       doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
            return exit_code::success;
        }
        const bool color = resolve_color(globals.color, io.env, io.stdout_tty);
        const std::string line = paint(*greeting, Style::bold, color);
        for (int i = 0; i < count_ && io.out; ++i) { // stop early once a write has failed
            std::print(io.out, "{}\n", line);
        }
        return exit_code::success; // a failed write is reported by finish_stdout in main()
    }

private:
    static constexpr int max_count = 1'000'000;

    std::string name_ = "world";
    int count_ = 1;
};

} // namespace

std::unique_ptr<Command> register_hello(CLI::App& parent, Command*& selected) {
    auto command = std::make_unique<HelloCommand>();
    CLI::App* sub = parent.add_subcommand("hello", "Print a greeting");
    command->bind(*sub);
    // The callback only selects. It captures pointers by value: `selected` lives in run()'s
    // Invocation and `self` is owned by run()'s command list, and both outlive the app.
    sub->callback([slot = &selected, self = command.get()] { *slot = self; });
    return command;
}

} // namespace mangadex_dl::app
