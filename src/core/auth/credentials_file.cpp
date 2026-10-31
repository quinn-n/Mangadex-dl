// SPDX-License-Identifier: GPL-3.0-or-later
//
// The credentials file (follows plan T9): `key = value` lines with the keys client_id,
// client_secret, username and password.
//
// The file belongs to the user but is still treated as untrusted text. Every error names the line
// and the problem, never a value: a mistyped line may hold the password. An unknown key is echoed
// only when it looks like a key ([a-z_]{1,32}), so `hunter2` on a line of its own, or as a key, is
// never shown. The secret values (client_secret, password) are copied straight from the caller's
// buffer into a Secret; no plain std::string ever holds them. load_credentials_file reads the file
// with read_private_file (owner, mode and size checked, the bytes go straight into a Secret).
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/prompt.hpp"
#include "mangadex_dl/secret.hpp"
#include "mangadex_dl/session_store.hpp"
#include "mangadex_dl/text_safety.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace mangadex_dl {
namespace {

/// The longest key echoed in an "unknown key" message.
inline constexpr std::size_t max_echoed_key_bytes = 32;

/// The keys, as they are listed in the "unknown key" message.
inline constexpr std::string_view known_keys_text =
    "the keys are client_id, client_secret, username and password";

enum class Key {
    client_id,
    client_secret,
    username,
    password,
};

[[nodiscard]] std::optional<Key> key_named(std::string_view name) noexcept {
    if (name == "client_id") {
        return Key::client_id;
    }
    if (name == "client_secret") {
        return Key::client_secret;
    }
    if (name == "username") {
        return Key::username;
    }
    if (name == "password") {
        return Key::password;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view key_name(Key key) noexcept {
    switch (key) {
    case Key::client_id:
        return "client_id";
    case Key::client_secret:
        return "client_secret";
    case Key::username:
        return "username";
    case Key::password:
        return "password";
    }
    return "?"; // only reachable through an out-of-range enum value
}

[[nodiscard]] constexpr bool is_blank(char c) noexcept { return c == ' ' || c == '\t'; }

/// `text` without leading and trailing spaces and tabs. A sub-view of `text`.
[[nodiscard]] constexpr std::string_view trim_blanks(std::string_view text) noexcept {
    while (!text.empty() && is_blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] constexpr bool is_key_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || c == '_';
}

/// `[a-z_]{1,32}`: only a key that looks like one is echoed.
[[nodiscard]] constexpr bool is_echoable_key(std::string_view key) noexcept {
    return !key.empty() && key.size() <= max_echoed_key_bytes &&
           std::ranges::all_of(key, is_key_char);
}

/// True when every byte sequence in `text` is well-formed UTF-8.
[[nodiscard]] constexpr bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const Utf8Decoded decoded = decode_utf8_at(text, pos);
        if (decoded.length == 0) {
            return false;
        }
        pos += decoded.length;
    }
    return true;
}

/// A C0 control (NUL and tab included), DEL or a C1 control. Precondition: `text` is valid UTF-8.
[[nodiscard]] constexpr bool has_control(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const Utf8Decoded decoded = decode_utf8_at(text, pos);
        const std::uint32_t cp = decoded.code_point;
        if (decoded.length == 0 || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            return true;
        }
        pos += decoded.length;
    }
    return false;
}

/// Errc::failure "line <n>: <what>". `what` never holds a value from the file.
[[nodiscard]] Error line_error(std::size_t line, std::string_view what) {
    return Error{.code = Errc::failure, .message = std::format("line {}: {}", line, what)};
}

/// Remembers which keys were seen, so a second one is a duplicate even if the first was empty.
struct SeenKeys {
    bool client_id = false;
    bool client_secret = false;
    bool username = false;
    bool password = false;

    [[nodiscard]] bool& slot(Key key) noexcept {
        switch (key) {
        case Key::client_id:
            return client_id;
        case Key::client_secret:
            return client_secret;
        case Key::username:
            return username;
        case Key::password:
            break;
        }
        return password;
    }
};

/// Stores a non-empty `value` (a view into the caller's buffer) under `key`. The secret keys go
/// straight into a Secret.
void store(CredentialsFile& out, Key key, std::string_view value) {
    switch (key) {
    case Key::client_id:
        out.client_id = std::string{value};
        return;
    case Key::client_secret:
        out.client_secret = Secret::copy_of(value);
        return;
    case Key::username:
        out.username = std::string{value};
        return;
    case Key::password:
        out.password = Secret::copy_of(value);
        return;
    }
}

[[nodiscard]] constexpr bool is_shell_safe(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
           c == '.' || c == '_' || c == '-' || c == '+' || c == ',' || c == ':' || c == '@' ||
           c == '%' || c == '=';
}

/// `path` as one POSIX shell word, so the chmod hint can be pasted as is: unchanged when every
/// byte is shell-safe, otherwise in single quotes with each `'` written as `'\''`.
[[nodiscard]] std::string shell_quoted(std::string_view path) {
    if (!path.empty() && std::ranges::all_of(path, is_shell_safe)) {
        return std::string{path};
    }
    std::string quoted{"'"};
    for (const char c : path) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

/// The message for a FileProblem. `where` is "credentials file '<path>'"; the problem's reason
/// is a fixed phrase (plus strerror text), never file bytes.
[[nodiscard]] Error problem_error(const FileProblem& problem, const std::filesystem::path& path,
                                  std::string_view where) {
    switch (problem.kind) {
    case FileProblemKind::missing:
        return Error{.code = Errc::failure, .message = std::format("{} does not exist", where)};
    case FileProblemKind::too_open:
        return Error{.code = Errc::failure,
                     .message = std::format("{} is readable or writable by others (mode {:04o}); "
                                            "run 'chmod 600 {}'",
                                            where, problem.mode, shell_quoted(path.string()))};
    case FileProblemKind::not_owned:
        return Error{.code = Errc::failure,
                     .message = std::format("{} is not owned by you", where)};
    case FileProblemKind::not_regular:
        return Error{.code = Errc::failure,
                     .message = std::format("{} is not a regular file", where)};
    case FileProblemKind::too_large:
        return Error{.code = Errc::failure,
                     .message = std::format("{} is larger than {} bytes", where,
                                            max_credentials_file_bytes)};
    case FileProblemKind::symlink:
    case FileProblemKind::io:
        break;
    }
    return Error{.code = Errc::failure, .message = std::format("{}: {}", where, problem.reason)};
}

} // namespace

Result<CredentialsFile> parse_credentials_file(std::string_view text) {
    CredentialsFile out{};
    SeenKeys seen{};
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        if (!is_valid_utf8(line)) {
            return std::unexpected(line_error(line_number, "invalid UTF-8"));
        }
        const std::string_view content = trim_blanks(line);
        if (content.empty() || content.front() == '#') {
            continue;
        }
        const std::size_t equals = content.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(line_error(line_number, "no '=' (each line is 'key = value')"));
        }
        const std::string_view name = trim_blanks(content.substr(0, equals));
        const std::optional<Key> key = key_named(name);
        if (!key) {
            if (is_echoable_key(name)) {
                return std::unexpected(line_error(
                    line_number, std::format("unknown key '{}'; {}", name, known_keys_text)));
            }
            return std::unexpected(line_error(
                line_number, std::format("unknown key (not shown); {}", known_keys_text)));
        }
        bool& was_seen = seen.slot(*key);
        if (was_seen) {
            return std::unexpected(
                line_error(line_number, std::format("duplicate key '{}'", key_name(*key))));
        }
        was_seen = true;

        const std::string_view value = trim_blanks(content.substr(equals + 1));
        if (has_control(value)) {
            return std::unexpected(line_error(
                line_number,
                std::format("the value of '{}' contains a control character", key_name(*key))));
        }
        if (value.size() > max_prompt_answer_bytes) {
            return std::unexpected(
                line_error(line_number, std::format("the value of '{}' is longer than {} bytes",
                                                    key_name(*key), max_prompt_answer_bytes)));
        }
        if (!value.empty()) {
            store(out, *key, value);
        }
    }
    return out;
}

Result<CredentialsFile> load_credentials_file(const std::filesystem::path& path,
                                              bool explicit_path) {
    // The path is shown raw: the app prints every line through print_error, whose whole-line
    // terminal_safe neutralises control characters in it.
    const std::string where = std::format("credentials file '{}'", path.string());
    const FileResult<Secret> bytes = read_private_file(path, max_credentials_file_bytes);
    if (!bytes) {
        if (bytes.error().kind == FileProblemKind::missing && !explicit_path) {
            return CredentialsFile{};
        }
        return std::unexpected(problem_error(bytes.error(), path, where));
    }
    Result<CredentialsFile> parsed = parse_credentials_file(bytes->reveal());
    if (!parsed) {
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = std::format("{}: {}", where, parsed.error().message),
        });
    }
    return parsed;
}

} // namespace mangadex_dl
