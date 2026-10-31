// SPDX-License-Identifier: GPL-3.0-or-later
//
// The saved-login store (follows plan T5): the user's directories, the private state directory,
// its session files, the session lock and the checked reader for private files.
//
// The state directory is opened once, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC (not
// O_PATH: it is fsynced, V17), and checked: owned by the euid, (mode & 077) == 0. Every file in it
// is reached with *at() calls relative to that descriptor, so a rename of the path mid-run cannot
// redirect a read or a write. A symlink as the final component fails the open with ENOTDIR (V17);
// fstatat(AT_SYMLINK_NOFOLLOW) then tells "a symbolic link" from "not a directory".
//
// Session writes are atomic: `.<name>.<12 hex>` (O_EXCL | O_NOFOLLOW, 0600) -> fchmod 0600 ->
// write all -> fsync -> close (checked) -> renameat over the name -> fsync(dir). Until renameat
// succeeds, a guard unlinks the temporary on every path out.
//
// The lock is flock(LOCK_EX | LOCK_NB) on `session.lock`, polled on the injected Clock. A flock
// belongs to one open file description (V16): the descriptor is never dup'ed, SessionLock is its
// only owner, and closing it releases the lock.
//
// Reads go straight into a Secret reserved from fstat's size (a growing file makes the Secret
// grow, which wipes the old buffer); the bytes pass through one stack chunk, wiped on every path.
// Problem reasons are fixed phrases plus strerror text: never a path, never file bytes.
#include "mangadex_dl/session_store.hpp"

#include "fs/errno_text.hpp"
#include "mangadex_dl/auth.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/net/transport.hpp"
#include "mangadex_dl/secret.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): renameat is POSIX, declared only in <stdio.h>
#include <stdio.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): explicit_bzero is a glibc extension of <string.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

inline constexpr std::string_view app_dir_name = "mangadex-dl";
inline constexpr std::string_view lock_file_name = "session.lock";
inline constexpr mode_t dir_mode = 0700;
inline constexpr mode_t file_mode = 0600;
/// The permission bits FileProblem::mode reports.
inline constexpr mode_t permission_bits = 0777;
/// Group and other bits: any of them set makes a private file or directory too open.
inline constexpr mode_t group_other_bits = 077;
inline constexpr std::size_t random_suffix_bytes = 6; // 12 hex digits
inline constexpr int max_name_attempts = 16;
inline constexpr std::size_t read_chunk_bytes = 4096;
/// The lock poll interval is clamped to [1 ms, 1 h] in milliseconds, before any conversion to
/// the clock's nanoseconds: a zero or negative poll would never let a virtual clock move, and a
/// huge one would overflow the conversion.
inline constexpr std::chrono::milliseconds min_lock_poll{1};
inline constexpr std::chrono::milliseconds max_lock_poll{std::chrono::hours{1}};
/// The errnos of a directory fsync that mean "this filesystem does not sync directories": best
/// effort, success. (ENOTSUP and EOPNOTSUPP are equal on Linux, so this is a list, not an ||.)
inline constexpr std::array<int, 4> sync_unsupported_errnos{EINVAL, ENOTSUP, EOPNOTSUPP, ENOSYS};

inline constexpr std::string_view lock_timeout_message =
    "another mangadex-dl is using the saved login; try again";

[[nodiscard]] FileProblem problem(FileProblemKind kind, std::string reason, unsigned mode = 0) {
    return FileProblem{.kind = kind, .mode = mode, .reason = std::move(reason)};
}

/// "<what>: <strerror>" as an io problem.
[[nodiscard]] FileProblem io_problem(std::string_view what, int err) {
    return problem(FileProblemKind::io, std::format("{}: {}", what, errno_text(err)));
}

[[nodiscard]] Error failure(std::string message) {
    return Error{.code = Errc::failure, .message = std::move(message)};
}

/// "<what>: <strerror>" as an Errc::failure.
[[nodiscard]] Error errno_failure(std::string_view what, int err) {
    return failure(std::format("{}: {}", what, errno_text(err)));
}

[[nodiscard]] Error interrupted_error() {
    return Error{.code = Errc::interrupted, .message = "interrupted"};
}

/// openat(2). The only call of the variadic C function here; `mode` is used only with O_CREAT.
[[nodiscard]] int open_at(int dir, const char* name, int flags, mode_t mode = 0) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic in C (the mode)
    return ::openat(dir, name, flags, mode);
}

/// fsync(2), retrying EINTR. 0 or an errno.
[[nodiscard]] int fsync_retry(int fd) noexcept {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) {
            return errno;
        }
    }
    return 0;
}

/// write(2) until all of `data` is written; retries EINTR and short writes. 0 or an errno.
[[nodiscard]] int write_all(int fd, std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
        const ssize_t written = ::write(fd, data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (written == 0) {
            return EIO; // a regular file never accepts 0 bytes of a non-empty write
        }
        data = data.subspan(static_cast<std::size_t>(written));
    }
    return 0;
}

/// 12 lowercase hex digits from getrandom(2), or the errno.
[[nodiscard]] std::expected<std::string, int> random_hex() {
    std::array<unsigned char, random_suffix_bytes> bytes{};
    std::span<unsigned char> rest{bytes};
    while (!rest.empty()) {
        const ssize_t got = ::getrandom(rest.data(), rest.size(), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errno);
        }
        rest = rest.subspan(static_cast<std::size_t>(got));
    }
    std::string hex{};
    hex.reserve(2 * bytes.size());
    for (const unsigned char byte : bytes) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

/// Overwrites a stack buffer that held file bytes when it goes out of scope. Borrows `bytes`,
/// which the caller declares before the guard (so it outlives it).
class ChunkWiper {
public:
    explicit ChunkWiper(std::span<char> bytes) noexcept : bytes_(bytes) {}
    ChunkWiper(const ChunkWiper&) = delete;
    ChunkWiper& operator=(const ChunkWiper&) = delete;
    ChunkWiper(ChunkWiper&&) = delete;
    ChunkWiper& operator=(ChunkWiper&&) = delete;
    ~ChunkWiper() { ::explicit_bzero(bytes_.data(), bytes_.size()); }

private:
    std::span<char> bytes_{};
};

/// Unlinks `name` in `dir` on destruction unless dismissed. Borrows `dir` (the StateDir's
/// descriptor, alive for the whole write).
class UnlinkGuard {
public:
    UnlinkGuard(int dir, std::string name) noexcept : dir_(dir), name_(std::move(name)) {}
    UnlinkGuard(const UnlinkGuard&) = delete;
    UnlinkGuard& operator=(const UnlinkGuard&) = delete;
    UnlinkGuard(UnlinkGuard&&) = delete;
    UnlinkGuard& operator=(UnlinkGuard&&) = delete;
    ~UnlinkGuard() {
        if (armed_) {
            // Best effort: there is nothing left to do if the temporary cannot be removed.
            static_cast<void>(::unlinkat(dir_, name_.c_str(), 0));
        }
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void dismiss() noexcept { armed_ = false; }

private:
    int dir_ = -1;
    std::string name_{};
    bool armed_ = true;
};

/// The checks every private file and the state directory share: owned by the euid, then no
/// group or other permission bit.
[[nodiscard]] std::optional<FileProblem> check_owner_and_mode(const struct stat& st) {
    if (st.st_uid != ::geteuid()) {
        return problem(FileProblemKind::not_owned, "it is owned by another user");
    }
    const mode_t mode = st.st_mode & permission_bits;
    if ((mode & group_other_bits) != 0) {
        return problem(FileProblemKind::too_open,
                       std::format("other users can access it (mode {:04o})", mode),
                       static_cast<unsigned>(mode));
    }
    return std::nullopt;
}

/// fstat on an open file: a regular file, owned by the euid, (mode & 077) == 0. Its size as a
/// reservation hint (0 when fstat reports none).
[[nodiscard]] FileResult<std::size_t> check_private_file(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        return std::unexpected(io_problem("cannot inspect it", errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(problem(FileProblemKind::not_regular, "it is not a regular file"));
    }
    if (std::optional<FileProblem> bad = check_owner_and_mode(st)) {
        return std::unexpected(std::move(*bad));
    }
    return st.st_size > 0 && std::in_range<std::size_t>(st.st_size)
               ? static_cast<std::size_t>(st.st_size)
               : std::size_t{0};
}

/// Reads `fd` to its end into a Secret, at most `cap` bytes (more is too_large). The Secret is
/// reserved for min(cap, size_hint) bytes up front, so an accurate hint means no growth; growth
/// (a file that grew since fstat) wipes the old buffer. The stack chunk is wiped on every path.
[[nodiscard]] FileResult<Secret> read_capped(int fd, std::size_t cap, std::size_t size_hint) {
    Secret secret{};
    secret.reserve(std::min(cap, size_hint));
    std::array<char, read_chunk_bytes> chunk{};
    const ChunkWiper wiper{chunk};
    while (true) {
        const ssize_t got = ::read(fd, chunk.data(), chunk.size());
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(io_problem("cannot read it", errno));
        }
        if (got == 0) {
            return secret;
        }
        const auto count = static_cast<std::size_t>(got);
        // secret.size() <= cap holds on entry, so the subtraction cannot wrap.
        if (count > cap - secret.size()) {
            return std::unexpected(problem(FileProblemKind::too_large,
                                           std::format("it is larger than {} bytes", cap)));
        }
        secret.append(std::string_view{chunk.data(), count});
    }
}

/// After an open with O_DIRECTORY | O_NOFOLLOW failed with ENOTDIR or ELOOP: what `name` in `at`
/// really is.
[[nodiscard]] FileProblem not_a_directory_problem(int at, const char* name) {
    struct stat st{};
    if (::fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
        return problem(FileProblemKind::symlink, "it is a symbolic link");
    }
    return problem(FileProblemKind::not_regular, "it is not a directory");
}

/// Why a parent component could not be opened: the mkdir errno (other than EEXIST) explains it
/// first, then the open errno.
[[nodiscard]] FileProblem parent_problem(int mkdir_err, int open_err) {
    if (mkdir_err != 0 && mkdir_err != EEXIST) {
        return io_problem("cannot create a parent directory", mkdir_err);
    }
    if (open_err == ENOENT) {
        return problem(FileProblemKind::missing, "a parent directory does not exist");
    }
    if (open_err == ENOTDIR) {
        return problem(FileProblemKind::not_regular, "a parent is not a directory");
    }
    return io_problem("cannot open a parent directory", open_err);
}

/// Where the state directory's final component lives.
struct ParentAndName {
    UniqueFd parent{};  ///< an O_PATH directory descriptor; invalid means the working directory
    std::string name{}; ///< the final component ("." for the root directory itself)
};

/// Walks to the parent of the state directory, one component at a time (symlinks followed),
/// creating each missing one with mode 0700 when `create`.
[[nodiscard]] FileResult<ParentAndName> walk_to_parent(const std::filesystem::path& path,
                                                       bool create) {
    if (path.empty()) {
        return std::unexpected(problem(FileProblemKind::io, "no path was given"));
    }
    std::vector<std::string> parts{};
    for (const std::filesystem::path& part : path.relative_path()) {
        if (std::string name = part.string(); !name.empty()) {
            parts.push_back(std::move(name)); // an empty part is a trailing separator
        }
    }
    UniqueFd current{};
    if (path.has_root_directory()) {
        current.reset(open_at(AT_FDCWD, "/", O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (!current.valid()) {
            return std::unexpected(io_problem("cannot open the root directory", errno));
        }
    }
    if (parts.empty()) {
        // The path is "/": the final component is the root directory itself.
        return ParentAndName{.parent = std::move(current), .name = "."};
    }
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& name = parts[i];
        const int at = current.valid() ? current.get() : AT_FDCWD;
        int mkdir_err = 0;
        if (create && name != "." && name != ".." && ::mkdirat(at, name.c_str(), dir_mode) != 0) {
            mkdir_err = errno; // EEXIST is the common, harmless case
        }
        UniqueFd next{open_at(at, name.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
        if (!next.valid()) {
            const int open_err = errno;
            return std::unexpected(parent_problem(mkdir_err, open_err));
        }
        current = std::move(next);
    }
    return ParentAndName{.parent = std::move(current), .name = parts.back()};
}

/// Whether `name` is a temporary that StateDir::write makes for `final_name`:
/// `.<final_name>.<12 lowercase hex>`, exactly.
[[nodiscard]] bool is_temp_name_for(std::string_view name, std::string_view final_name) noexcept {
    if (name.size() != 1 + final_name.size() + 1 + (2 * random_suffix_bytes) ||
        !name.starts_with('.') || name.substr(1, final_name.size()) != final_name ||
        name[1 + final_name.size()] != '.') {
        return false;
    }
    return std::ranges::all_of(name.substr(final_name.size() + 2), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

/// closedir(3) for a DIR stream from fdopendir (which owns its descriptor).
struct DirCloser {
    void operator()(DIR* stream) const noexcept {
        // Best effort: a read-only listing has nothing to flush.
        static_cast<void>(::closedir(stream));
    }
};
using DirPtr = std::unique_ptr<DIR, DirCloser>;

/// Best effort: unlinks the temporaries a killed run left in `dir` (names that exactly match
/// the write() pattern for either slot). Called only while the session lock is held, and every
/// write happens under that lock, so no live temporary can match. Failures are ignored.
void sweep_stale_temps(int dir) {
    // A fresh open file description of the directory (never a dup of `dir`: fdopendir takes over
    // the descriptor, and its reads must not move `dir`'s offset).
    UniqueFd list_fd{open_at(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (!list_fd.valid()) {
        return;
    }
    const DirPtr stream{::fdopendir(list_fd.get())};
    if (!stream) {
        return; // list_fd still owns the descriptor and closes it
    }
    static_cast<void>(list_fd.release()); // now owned by `stream`

    const std::string_view real = session_file_name(SessionSlot::real);
    const std::string_view fixture = session_file_name(SessionSlot::fixture);
    std::vector<std::string> stale{};
    while (true) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): this DIR stream is used by this thread only
        const struct dirent* entry = ::readdir(stream.get());
        if (entry == nullptr) {
            break; // the end, or a read error: either way the sweep is over
        }
        const std::string_view name{&entry->d_name[0]};
        if (is_temp_name_for(name, real) || is_temp_name_for(name, fixture)) {
            stale.emplace_back(name);
        }
    }
    for (const std::string& name : stale) {
        static_cast<void>(::unlinkat(dir, name.c_str(), 0)); // EISDIR etc.: left alone
    }
}

} // namespace

UserDirs make_user_dirs(std::optional<std::string_view> home,
                        std::optional<std::string_view> config,
                        std::optional<std::string_view> state) {
    const auto absolute = [](std::optional<std::string_view> value) {
        std::optional<std::filesystem::path> kept{};
        if (value && !value->empty()) {
            std::filesystem::path path{*value};
            if (path.is_absolute()) {
                kept = std::move(path);
            }
        }
        return kept;
    };
    return UserDirs{.home = absolute(home),
                    .xdg_config_home = absolute(config),
                    .xdg_state_home = absolute(state)};
}

std::optional<std::filesystem::path> state_dir_path(const UserDirs& dirs) {
    if (dirs.xdg_state_home) {
        return *dirs.xdg_state_home / app_dir_name;
    }
    if (dirs.home) {
        return *dirs.home / ".local" / "state" / app_dir_name;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> default_credentials_path(const UserDirs& dirs) {
    if (dirs.xdg_config_home) {
        return *dirs.xdg_config_home / app_dir_name / "credentials";
    }
    if (dirs.home) {
        return *dirs.home / ".config" / app_dir_name / "credentials";
    }
    return std::nullopt;
}

std::string_view session_file_name(SessionSlot slot) noexcept {
    switch (slot) {
    case SessionSlot::real:
        return "session.json";
    case SessionSlot::fixture:
        return "fixture-session.json";
    }
    return "fixture-session.json"; // out-of-range value: never the real login
}

FileResult<Secret> read_private_file(const std::filesystem::path& path, std::size_t cap) {
    const std::string name = path.string();
    const UniqueFd fd{
        open_at(AT_FDCWD, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK)};
    if (!fd.valid()) {
        const int err = errno;
        if (err == ENOENT) {
            return std::unexpected(problem(FileProblemKind::missing, "it does not exist"));
        }
        return std::unexpected(io_problem("cannot open it", err));
    }
    FileResult<std::size_t> size_hint = check_private_file(fd.get());
    if (!size_hint) {
        return std::unexpected(std::move(size_hint).error());
    }
    return read_capped(fd.get(), cap, *size_hint);
}

FileResult<StateDir> StateDir::open(const std::filesystem::path& path, bool create) {
    FileResult<ParentAndName> walked = walk_to_parent(path, create);
    if (!walked) {
        return std::unexpected(std::move(walked).error());
    }
    const int at = walked->parent.valid() ? walked->parent.get() : AT_FDCWD;
    const char* name = walked->name.c_str();
    int mkdir_err = 0;
    if (create && walked->name != "." && walked->name != ".." &&
        ::mkdirat(at, name, dir_mode) != 0) {
        mkdir_err = errno; // EEXIST: it is there already (or something else is)
    }
    UniqueFd fd{open_at(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (!fd.valid()) {
        const int err = errno;
        if (err == ENOTDIR || err == ELOOP) {
            return std::unexpected(not_a_directory_problem(at, name));
        }
        if (mkdir_err != 0 && mkdir_err != EEXIST) {
            return std::unexpected(io_problem("cannot create it", mkdir_err));
        }
        if (err == ENOENT) {
            return std::unexpected(problem(FileProblemKind::missing, "it does not exist"));
        }
        return std::unexpected(io_problem("cannot open it", err));
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        return std::unexpected(io_problem("cannot inspect it", errno));
    }
    if (std::optional<FileProblem> bad = check_owner_and_mode(st)) {
        return std::unexpected(std::move(*bad));
    }
    return StateDir{std::move(fd), path};
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): the by-value signature is frozen (T1 header)
Result<SessionLock> StateDir::lock(Clock& clock, LockTiming timing, std::stop_token stop) const {
    const std::string lock_name{lock_file_name};
    // O_NONBLOCK: a device node or FIFO planted under the name never blocks the open. It stays
    // set: nothing reads or writes this descriptor, and flock ignores it.
    UniqueFd fd{open_at(fd_.get(), lock_name.c_str(),
                        O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC,
                        file_mode)};
    if (!fd.valid()) {
        const int err = errno;
        if (err == ELOOP) {
            return std::unexpected(failure("the session lock is a symbolic link"));
        }
        return std::unexpected(errno_failure("cannot open the session lock", err));
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        return std::unexpected(errno_failure("cannot inspect the session lock", errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(failure("the session lock is not a regular file"));
    }
    if (const std::optional<FileProblem> bad = check_owner_and_mode(st)) {
        return std::unexpected(
            failure(std::format("cannot use the session lock: {}", bad->reason)));
    }

    using SteadyDuration = std::chrono::steady_clock::duration;
    const auto start = clock.now();
    // deadline = start + timeout, saturating: compared in whole seconds, so a huge timeout never
    // overflows the nanosecond representation; a negative one means "try once".
    const SteadyDuration room = std::chrono::steady_clock::time_point::max() - start;
    const std::chrono::seconds timeout = std::max(timing.timeout, std::chrono::seconds{0});
    const auto deadline = timeout >= std::chrono::floor<std::chrono::seconds>(room)
                              ? std::chrono::steady_clock::time_point::max()
                              : start + std::chrono::duration_cast<SteadyDuration>(timeout);
    // Clamped in milliseconds first, so the conversion below cannot overflow (1 h fits easily).
    const auto poll = std::chrono::duration_cast<SteadyDuration>(
        std::clamp(timing.poll, min_lock_poll, max_lock_poll));

    while (true) {
        if (::flock(fd.get(), LOCK_EX | LOCK_NB) == 0) {
            sweep_stale_temps(fd_.get());
            return SessionLock{std::move(fd)};
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err != EWOULDBLOCK) {
            return std::unexpected(errno_failure("cannot lock the saved login", err));
        }
        if (interrupt_requested() || stop.stop_requested()) {
            return std::unexpected(interrupted_error());
        }
        const auto now = clock.now();
        if (now >= deadline) {
            return std::unexpected(failure(std::string{lock_timeout_message}));
        }
        // now < deadline <= max, so deadline - now is positive and representable.
        const auto wait = std::min(poll, deadline - now);
        if (!clock.sleep_until(now + wait, stop)) {
            return std::unexpected(interrupted_error());
        }
    }
}

FileResult<std::optional<Secret>> StateDir::read(SessionSlot slot) const {
    const std::string name{session_file_name(slot)};
    const UniqueFd fd{open_at(fd_.get(), name.c_str(),
                              O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)};
    if (!fd.valid()) {
        const int err = errno;
        if (err == ENOENT) {
            return std::optional<Secret>{};
        }
        if (err == ELOOP) {
            return std::unexpected(problem(FileProblemKind::symlink, "it is a symbolic link"));
        }
        if (err == ENXIO || err == ENODEV) {
            return std::unexpected(
                problem(FileProblemKind::not_regular, "it is not a regular file"));
        }
        return std::unexpected(io_problem("cannot open it", err));
    }
    FileResult<std::size_t> size_hint = check_private_file(fd.get());
    if (!size_hint) {
        return std::unexpected(std::move(size_hint).error());
    }
    FileResult<Secret> bytes = read_capped(fd.get(), max_session_file_bytes, *size_hint);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    return std::optional<Secret>{std::move(*bytes)};
}

Result<void> StateDir::write(SessionSlot slot, const Secret& bytes) const {
    const std::string final_name{session_file_name(slot)};
    UniqueFd temp{};
    std::string temp_name{};
    int open_err = EEXIST;
    for (int attempt = 0; attempt < max_name_attempts && open_err == EEXIST; ++attempt) {
        std::expected<std::string, int> hex = random_hex();
        if (!hex) {
            return std::unexpected(errno_failure("cannot get random bytes", hex.error()));
        }
        temp_name = std::format(".{}.{}", final_name, *hex);
        temp.reset(open_at(fd_.get(), temp_name.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC,
                           file_mode));
        open_err = temp.valid() ? 0 : errno;
    }
    if (open_err != 0) {
        return std::unexpected(errno_failure("cannot create a temporary file", open_err));
    }
    // Declared after `temp`, so it is destroyed first: on an early return the name is unlinked,
    // then `temp` closes the descriptor (unlinking an open file is fine on Linux).
    UnlinkGuard guard{fd_.get(), std::move(temp_name)};

    if (::fchmod(temp.get(), file_mode) != 0) {
        return std::unexpected(errno_failure("cannot set the file mode", errno));
    }
    const std::string_view text = bytes.reveal();
    if (const int err = write_all(temp.get(), std::as_bytes(std::span{text.data(), text.size()}));
        err != 0) {
        return std::unexpected(errno_failure("cannot write it", err));
    }
    if (const int err = fsync_retry(temp.get()); err != 0) {
        return std::unexpected(errno_failure("cannot write it", err));
    }
    // EINTR still closes the descriptor on Linux, and the data is already fsynced.
    if (::close(temp.release()) != 0 && errno != EINTR) {
        return std::unexpected(errno_failure("cannot write it", errno));
    }
    if (::renameat(fd_.get(), guard.name().c_str(), fd_.get(), final_name.c_str()) != 0) {
        return std::unexpected(errno_failure("cannot put it in place", errno));
    }
    guard.dismiss();
    // The new file is already in place; callers treat this as "may not be durable", not
    // "unchanged". A filesystem that cannot sync directories is best effort (as in remove()).
    if (const int err = fsync_retry(fd_.get());
        err != 0 && !std::ranges::contains(sync_unsupported_errnos, err)) {
        return std::unexpected(errno_failure("saved, but cannot sync the state directory", err));
    }
    return {};
}

Result<bool> StateDir::remove(SessionSlot slot) const {
    const std::string name{session_file_name(slot)};
    if (::unlinkat(fd_.get(), name.c_str(), 0) != 0) {
        const int err = errno;
        if (err == ENOENT) {
            return false;
        }
        return std::unexpected(errno_failure("cannot remove it", err));
    }
    // Best effort: the name is gone either way; a failed sync only weakens durability.
    static_cast<void>(fsync_retry(fd_.get()));
    return true;
}

} // namespace mangadex_dl
