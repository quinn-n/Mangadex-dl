// SPDX-License-Identifier: GPL-3.0-or-later
//
// Error vocabulary for core code. Core functions return Result<T>; only main()/run() turn an Error
// into an exit code. Failure is the exception form, for the rare place that cannot return a Result.
#pragma once

#include "mangadex_dl/exit_codes.hpp"

#include <expected>
#include <stdexcept>
#include <string>

namespace mangadex_dl {

/// Error categories. Each maps to exactly one exit code (see exit_code_for).
enum class Errc {
    failure,     ///< expected runtime failure (missing file, permission, malformed data)
    usage,       ///< the invocation is invalid in a way the parser cannot see
    io,          ///< a read or write on a file or stream failed
    network,     ///< the network or the API failed (connect, TLS, timeout, retries, bad response)
    not_found,   ///< the requested title (or, for mdlist, the list) does not exist
    no_chapters, ///< nothing downloadable matched the language and selection
    incomplete,  ///< at least one output file failed while others may have been written
    interrupted, ///< SIGINT/SIGTERM arrived; the run unwound and cleaned up its temporaries
    mixed_failures, ///< batch/mdlist: the titles that did not complete failed in more than one way
    /// --dir-per-title: this title's own directory below DIR is unusable (exit 1; the run
    /// continues with the next title)
    row_dir,
};

[[nodiscard]] constexpr int exit_code_for(Errc code) noexcept {
    switch (code) {
    case Errc::usage:
        return exit_code::usage;
    case Errc::failure:
    case Errc::io:
    case Errc::row_dir:
        return exit_code::failure;
    case Errc::network:
        return exit_code::network;
    case Errc::not_found:
        return exit_code::not_found;
    case Errc::no_chapters:
        return exit_code::no_chapters;
    case Errc::incomplete:
        return exit_code::incomplete;
    case Errc::mixed_failures:
        return exit_code::mixed_failures;
    case Errc::interrupted:
        // Only a fallback: main() re-raises the pending signal after finish_stdout(), so the shell
        // sees the signal death (130/143). The code is returned only if re-raising is impossible.
        return exit_code::failure;
    }
    return exit_code::internal; // only reachable through an out-of-range enum value
}

/// A value type: aggregate + default member initializers + defaulted ==.
struct Error {
    Errc code = Errc::failure;
    std::string message{}; ///< one line, no trailing newline, no "<tool>: " prefix

    [[nodiscard]] int exit_code() const noexcept { return exit_code_for(code); }
    friend bool operator==(const Error&, const Error&) = default;
};

/// Every fallible core function returns this. Never dereference it unchecked.
template <class T> using Result = std::expected<T, Error>;

/// Exception carrying an Error. It stores only the code plus runtime_error's message, so copying it
/// never allocates (safe to catch and inspect inside main()'s catch ladder).
class Failure : public std::runtime_error {
public:
    explicit Failure(const Error& err) : std::runtime_error(err.message), code_(err.code) {}

    [[nodiscard]] Errc code() const noexcept { return code_; }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_for(code_); }
    [[nodiscard]] Error error() const { return Error{.code = code_, .message = what()}; }

private:
    Errc code_ = Errc::failure;
};

} // namespace mangadex_dl
