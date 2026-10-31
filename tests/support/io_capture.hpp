#pragma once
// tests/support/io_capture.hpp: run the CLI in-process through the run(args, Io&) seam and capture
// both streams. Tier 2 (cpp-testing skill): help/version text, usage errors, exit-code mapping, the
// --json shape and color decisions, with TTY flags and env injected instead of read from the
// process.

#include "app/cli.hpp"
#include "app/io.hpp"

#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mangadex_dl::test {

struct Tty {
    bool stdin_tty = false;
    bool stdout_tty = false;
    bool stderr_tty = false;
};

struct Captured {
    int exit_code = -1;
    std::string out;
    std::string err;
};

// argv[0] is supplied; pass only the arguments. Arguments are copied, so temporaries are safe here.
// `stdin_fd` becomes Io::stdin_fd: borrowed (the caller opens and closes it); the default -1 is a
// closed stdin, so no in-process test ever reads the test runner's own stdin.
[[nodiscard]] inline Captured run_cli(std::initializer_list<std::string> args, Tty tty = {},
                                      app::EnvSnapshot env = {}, int stdin_fd = -1) {
    std::vector<std::string> storage{"mangadex-dl"};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    argv.reserve(storage.size());
    for (const auto& arg : storage) {
        argv.push_back(arg.c_str());
    }

    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out,
               .err = err,
               .stdin_tty = tty.stdin_tty,
               .stdout_tty = tty.stdout_tty,
               .stderr_tty = tty.stderr_tty,
               .env = std::move(env),
               .stdin_fd = stdin_fd};
    Captured result;
    result.exit_code = app::run(std::span<const char* const>{argv}, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

} // namespace mangadex_dl::test
