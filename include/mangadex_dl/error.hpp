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
    failure, ///< expected runtime failure (missing file, permission, malformed data)
    usage,   ///< the invocation is invalid in a way the parser cannot see
    io,      ///< a read or write on a file or stream failed
};

[[nodiscard]] constexpr int exit_code_for(Errc code) noexcept {
    switch (code) {
    case Errc::usage:
        return exit_code::usage;
    case Errc::failure:
    case Errc::io:
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
