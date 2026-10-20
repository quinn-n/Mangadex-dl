// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/fs/bounded_read_test.cpp: read_bounded_file / read_bounded_fd (batch plan T2,
// rev1). The cap is enforced on the bytes actually read, at most cap + 1 are ever consumed from
// a stream, the fstat type policy decides what is read, a FIFO without a writer never blocks, and
// the fd form never closes the caller's descriptor or changes its flags.
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/bounded_read.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using mangadex_dl::Errc;
using mangadex_dl::Error;
using mangadex_dl::read_bounded_fd;
using mangadex_dl::read_bounded_file;
using mangadex_dl::UniqueFd;
using mangadex_dl::test::TempDir;
using std::string_view_literals::operator""sv;

[[nodiscard]] TempDir make_temp_dir() {
    auto dir = TempDir::create("mangadex-dl-bounded");
    REQUIRE(dir.has_value());
    return std::move(*dir);
}

void write_file(const fs::path& path, std::string_view content) {
    std::ofstream out{path, std::ios::binary};
    out << content;
    out.close();
    REQUIRE(out.good());
}

/// open(2) without O_CREAT (the variadic mode is never read).
[[nodiscard]] UniqueFd open_fd(const fs::path& path, int flags) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic in C (the mode)
    return UniqueFd{::open(path.c_str(), flags)};
}

[[nodiscard]] int fd_flags(int fd) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    return ::fcntl(fd, F_GETFL);
}

[[nodiscard]] int fd_descriptor_flags(int fd) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
    return ::fcntl(fd, F_GETFD);
}

/// The O_NONBLOCK bit of an F_GETFL result (done unsigned: the flags are a bit set).
[[nodiscard]] bool nonblocking(int flags) {
    return (static_cast<unsigned int>(flags) & static_cast<unsigned int>(O_NONBLOCK)) != 0;
}

[[nodiscard]] int with_nonblock(int flags) {
    return static_cast<int>(static_cast<unsigned int>(flags) |
                            static_cast<unsigned int>(O_NONBLOCK));
}

struct FdPair {
    UniqueFd first{};
    UniqueFd second{};
};

[[nodiscard]] FdPair make_pipe() {
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::pipe2(fds.data(), O_CLOEXEC) == 0);
    return FdPair{.first = UniqueFd{fds[0]}, .second = UniqueFd{fds[1]}};
}

void write_all(int fd, std::string_view data) {
    REQUIRE(::write(fd, data.data(), data.size()) == static_cast<::ssize_t>(data.size()));
}

[[nodiscard]] Error usage_error(std::string_view label, std::size_t cap) {
    return Error{.code = Errc::usage,
                 .message =
                     std::string{label} + ": is larger than " + std::to_string(cap) + " bytes"};
}

[[nodiscard]] Error read_failure(std::string_view label, std::string_view why) {
    return Error{.code = Errc::failure,
                 .message =
                     std::string{"cannot read "} + std::string{label} + ": " + std::string{why}};
}

} // namespace

TEST_CASE("read_bounded_file reads a regular file up to the cap", "[bounded_read]") {
    const TempDir dir = make_temp_dir();
    const fs::path file = dir.path() / "list.csv";

    SECTION("an empty file") {
        write_file(file, "");
        const auto got = read_bounded_file(file, "list.csv", 16);
        REQUIRE(got.has_value());
        CHECK(got->empty());
    }
    SECTION("exactly cap bytes") {
        write_file(file, "0123456789abcdef");
        const auto got = read_bounded_file(file, "list.csv", 16);
        REQUIRE(got.has_value());
        CHECK(*got == "0123456789abcdef");
    }
    SECTION("cap + 1 bytes is a usage error naming the limit") {
        write_file(file, "0123456789abcdefX");
        const auto got = read_bounded_file(file, "list.csv", 16);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == usage_error("list.csv", 16));
    }
    SECTION("a cap of 0 accepts only an empty file") {
        write_file(file, "x");
        const auto got = read_bounded_file(file, "list.csv", 0);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == usage_error("list.csv", 0));
    }
    SECTION("bytes are returned unchanged (CRLF, BOM, NUL, invalid UTF-8)") {
        const std::string content{"\xEF\xBB\xBF"
                                  "a,b\r\n\xFF\x00z"sv};
        write_file(file, content);
        const auto got = read_bounded_file(file, "list.csv", 1024);
        REQUIRE(got.has_value());
        CHECK(*got == content);
    }
    SECTION("a file larger than one read chunk") {
        const std::string content(std::size_t{200} * 1024, 'x');
        write_file(file, content);
        const auto got = read_bounded_file(file, "list.csv", content.size());
        REQUIRE(got.has_value());
        CHECK(*got == content);
        const auto over = read_bounded_file(file, "list.csv", content.size() - 1);
        REQUIRE_FALSE(over.has_value());
        CHECK(over.error() == usage_error("list.csv", content.size() - 1));
    }
}

TEST_CASE(
    "read_bounded_file and read_bounded_fd: a cap of SIZE_MAX is refused before anything is read",
    "[bounded_read]") {
    constexpr std::size_t no_cap = std::numeric_limits<std::size_t>::max();
    const TempDir dir = make_temp_dir();
    const fs::path file = dir.path() / "list.csv";
    write_file(file, "abc");

    const auto from_file = read_bounded_file(file, "list.csv", no_cap);
    REQUIRE_FALSE(from_file.has_value());
    CHECK(from_file.error() == read_failure("list.csv", "the size limit is out of range"));

    const FdPair pipe = make_pipe();
    write_all(pipe.second.get(), "abc");
    const auto from_fd = read_bounded_fd(pipe.first.get(), "<stdin>", no_cap);
    REQUIRE_FALSE(from_fd.has_value());
    CHECK(from_fd.error() == read_failure("<stdin>", "the size limit is out of range"));

    // Nothing was consumed from the pipe.
    std::array<char, 8> buffer{};
    CHECK(::read(pipe.first.get(), buffer.data(), buffer.size()) == 3);
}

TEST_CASE("read_bounded_fd consumes at most cap + 1 bytes from a pipe", "[bounded_read]") {
    const FdPair pipe = make_pipe();
    std::string payload{};
    for (int i = 0; i < 100; ++i) {
        payload.push_back(static_cast<char>('a' + (i % 26)));
    }
    write_all(pipe.second.get(), payload);

    const auto got = read_bounded_fd(pipe.first.get(), "<stdin>", 16);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == usage_error("<stdin>", 16));

    // The other 83 bytes are still in the pipe for whoever reads next.
    std::array<char, 256> rest{};
    const ::ssize_t left = ::read(pipe.first.get(), rest.data(), rest.size());
    REQUIRE(left == 83);
    CHECK(std::string_view{rest.data(), 83} == std::string_view{payload}.substr(17));
}

TEST_CASE("read_bounded_fd reads a pipe to EOF and leaves the descriptor alone", "[bounded_read]") {
    FdPair pipe = make_pipe();
    write_all(pipe.second.get(), "manga\n1111,x\n");
    pipe.second.reset(); // EOF after the data

    const int fd = pipe.first.get();
    const int status_before = fd_flags(fd);
    const int descriptor_before = fd_descriptor_flags(fd);
    REQUIRE(status_before >= 0);
    REQUIRE(descriptor_before >= 0);

    const auto got = read_bounded_fd(fd, "<stdin>", 13);
    REQUIRE(got.has_value());
    CHECK(*got == "manga\n1111,x\n");

    // Still open (F_GETFD would fail with EBADF otherwise) and the flags are unchanged.
    CHECK(fd_descriptor_flags(fd) == descriptor_before);
    CHECK(fd_flags(fd) == status_before);
}

TEST_CASE("read_bounded_fd never clears O_NONBLOCK on the caller's descriptor", "[bounded_read]") {
    SECTION("a non-blocking FIFO without a writer reads as empty") {
        const TempDir dir = make_temp_dir();
        const fs::path fifo = dir.path() / "list.fifo";
        REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
        const UniqueFd fd = open_fd(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        REQUIRE(fd.valid());
        const int before = fd_flags(fd.get());

        const auto got = read_bounded_fd(fd.get(), "<stdin>", 64);
        REQUIRE(got.has_value());
        CHECK(got->empty());
        CHECK(fd_flags(fd.get()) == before);
        CHECK(nonblocking(fd_flags(fd.get())));
    }
    SECTION("an inherited non-blocking pipe with an idle writer is EAGAIN, not a hang") {
        const FdPair pipe = make_pipe(); // the writer stays open and writes nothing
        const int flags = fd_flags(pipe.first.get());
        REQUIRE(flags >= 0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl is variadic in C
        REQUIRE(::fcntl(pipe.first.get(), F_SETFL, with_nonblock(flags)) == 0);

        const auto got = read_bounded_fd(pipe.first.get(), "<stdin>", 64);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == read_failure("<stdin>", std::system_category().message(EAGAIN)));
        CHECK(fd_flags(pipe.first.get()) == with_nonblock(flags));
    }
}

TEST_CASE("read_bounded_fd rejects a bad descriptor", "[bounded_read]") {
    const std::string bad_fd = std::system_category().message(EBADF);
    REQUIRE(bad_fd == "Bad file descriptor");

    SECTION("fd -1") {
        const auto got = read_bounded_fd(-1, "<stdin>", 64);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == read_failure("<stdin>", bad_fd));
    }
    SECTION("a closed descriptor") {
        FdPair pipe = make_pipe();
        const int closed = pipe.first.get();
        pipe.first.reset();
        pipe.second.reset();
        const auto got = read_bounded_fd(closed, "<stdin>", 64);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == read_failure("<stdin>", bad_fd));
    }
}

TEST_CASE("read_bounded_file and read_bounded_fd: the fstat type policy", "[bounded_read]") {
    const TempDir dir = make_temp_dir();

    SECTION("a directory, by path and by descriptor") {
        const auto by_path = read_bounded_file(dir.path(), "lists", 64);
        REQUIRE_FALSE(by_path.has_value());
        CHECK(by_path.error() == read_failure("lists", "is a directory"));

        const UniqueFd fd = open_fd(dir.path(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        REQUIRE(fd.valid());
        const auto by_fd = read_bounded_fd(fd.get(), "<stdin>", 64);
        REQUIRE_FALSE(by_fd.has_value());
        CHECK(by_fd.error() == read_failure("<stdin>", "is a directory"));
    }
    SECTION("a symbolic link itself (O_PATH) is neither file, pipe nor terminal") {
        const fs::path link = dir.path() / "link";
        fs::create_symlink("nowhere", link);
        const UniqueFd fd = open_fd(link, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        REQUIRE(fd.valid());
        const auto got = read_bounded_fd(fd.get(), "<stdin>", 64);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == read_failure("<stdin>", "not a regular file, pipe or terminal"));
    }
    SECTION("a socket on the descriptor form is read") {
        std::array<int, 2> fds{-1, -1};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) == 0);
        const UniqueFd reader{fds[0]};
        UniqueFd writer{fds[1]};
        write_all(writer.get(), "a,b\n");
        writer.reset();
        const auto got = read_bounded_fd(reader.get(), "<stdin>", 64);
        REQUIRE(got.has_value());
        CHECK(*got == "a,b\n");
    }
    SECTION("a missing file reports strerror") {
        const auto got = read_bounded_file(dir.path() / "missing.csv", "missing.csv", 64);
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error() == read_failure("missing.csv", std::system_category().message(ENOENT)));
    }
    SECTION("character devices are read with the cap") {
        const auto null = read_bounded_file("/dev/null", "/dev/null", 64);
        REQUIRE(null.has_value());
        CHECK(null->empty());

        const auto zero = read_bounded_file("/dev/zero", "/dev/zero", 16);
        REQUIRE_FALSE(zero.has_value());
        CHECK(zero.error() == usage_error("/dev/zero", 16));
    }
}

TEST_CASE("read_bounded_file reads procfs although st_size is 0", "[bounded_read]") {
    struct stat st{};
    REQUIRE(::stat("/proc/self/status", &st) == 0);
    REQUIRE(st.st_size == 0);

    const auto got = read_bounded_file("/proc/self/status", "status", std::size_t{1024} * 1024);
    REQUIRE(got.has_value());
    CHECK(got->starts_with("Name:"));
    CHECK(got->ends_with('\n'));
    CHECK(got->contains("\nPid:"));
}

TEST_CASE("read_bounded_file on a FIFO without a writer returns empty at once", "[bounded_read]") {
    const TempDir dir = make_temp_dir();
    const fs::path fifo = dir.path() / "list.fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    // O_NONBLOCK on open, so the open itself never waits for a writer; with no writer the first
    // read is EOF. If either blocked, this test would hang until the CTest timeout.
    const auto got = read_bounded_file(fifo, "list.fifo", 64);
    REQUIRE(got.has_value());
    CHECK(got->empty());
}

TEST_CASE("read_bounded_file on a FIFO with a writer blocks until the writer closes",
          "[bounded_read]") {
    const TempDir dir = make_temp_dir();
    const fs::path fifo = dir.path() / "list.fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    // A reader that never reads keeps the FIFO open, so the writer's open succeeds at once and
    // the writer exists before read_bounded_file starts: the result never depends on timing.
    const UniqueFd holder = open_fd(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    REQUIRE(holder.valid());
    UniqueFd opened_writer = open_fd(fifo, O_WRONLY | O_CLOEXEC);
    REQUIRE(opened_writer.valid());

    mangadex_dl::Result<std::string> got{};
    {
        // `slot` points at `got`, which outlives the thread: the jthread joins at this scope's
        // end, before `got` is read.
        const std::jthread reader{
            [slot = &got, path = fifo] { *slot = read_bounded_file(path, "list.fifo", 1024); }};
        // Declared after the jthread, so it is destroyed (closed) before the join even when a
        // REQUIRE below throws: the reader then sees EOF instead of waiting forever.
        UniqueFd writer = std::move(opened_writer);
        // Scheduling hints only (the result does not depend on them): the reader likely finds
        // the FIFO empty with a live writer first, where a left-over O_NONBLOCK would fail with
        // EAGAIN instead of waiting.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        write_all(writer.get(), "manga,title\n");
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        write_all(writer.get(), "1111,One\n2222,Two\n");
        writer.reset();
    }
    REQUIRE(got.has_value());
    CHECK(*got == "manga,title\n1111,One\n2222,Two\n");
}
