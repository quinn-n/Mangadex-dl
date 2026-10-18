#pragma once
// tests/support/loopback_http_server.hpp: a scripted HTTP/1.1 server on 127.0.0.1 for hermetic
// transport tests (never the real network).
//
// The server binds 127.0.0.1:0 (a free port), and an acceptor jthread serves every accepted
// connection on its own jthread. Each request is recorded, then answered with whatever the
// handler returns: status, headers, body, Content-Length or chunked framing, a delay, a slow
// drip, a stall, or a dropped connection. Connections are kept alive between requests unless the
// response ends them. The destructor stops and joins every thread (RAII).
//
//     Server server{[](const Request&) {
//         return Response{.body = "hello"};
//     }};
//     REQUIRE(server.ok());
//     ... fetch server.url("/path") ...
//     CHECK(server.requests().at(0).target == "/path");
//
// The handler runs on a connection thread and may run concurrently with itself: it must be
// thread-safe (a stateless lambda, or one that only touches atomics).
//
// Writes use MSG_NOSIGNAL, so a client that hangs up never raises SIGPIPE in the server.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mangadex_dl::test::loopback {

/// How the body is framed on the wire.
enum class Framing {
    content_length, ///< Content-Length: <declared_length or body.size()>
    chunked,        ///< Transfer-Encoding: chunked (one chunk per send)
};

/// One scripted answer. Every behaviour field defaults to "off".
struct Response {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string body{};
    Framing framing = Framing::content_length;
    std::optional<std::size_t> declared_length{}; ///< Content-Length to announce instead
    std::chrono::milliseconds delay{0};           ///< wait before the status line
    std::size_t drip_bytes = 0;                   ///< > 0: send the body in pieces this size...
    std::chrono::milliseconds drip_interval{0};   ///< ...this far apart
    std::optional<std::size_t> stall_after{};     ///< send this many body bytes, then go silent
    std::optional<std::size_t> drop_after{};      ///< send this many body bytes, then close
    bool drop_before_response = false;            ///< close without sending anything
};

struct Request {
    std::string method{};
    std::string target{};
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string body{};

    /// The first header named `name` (ASCII case-insensitive), or nullopt.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        for (const auto& [key, value] : headers) {
            if (std::ranges::equal(key, name, {}, lower, lower)) {
                return value;
            }
        }
        return std::nullopt;
    }
};

using Handler = std::function<Response(const Request&)>;

namespace detail {

/// Owns one socket descriptor.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Socket() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

/// The one place a sockaddr_in is viewed as the generic sockaddr the socket API takes.
[[nodiscard]] inline sockaddr* as_sockaddr(sockaddr_in& address) noexcept {
    // The BSD socket API takes every address family through the generic sockaddr.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr punning
    return reinterpret_cast<sockaddr*>(&address);
}

[[nodiscard]] inline sockaddr_in loopback_address(std::uint16_t port) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr));
    return address;
}

/// Sleeps for `duration` unless `stop` is requested first. False when stopped.
inline bool sleep_for(const std::stop_token& stop, std::chrono::milliseconds duration) {
    if (duration <= std::chrono::milliseconds::zero()) {
        return !stop.stop_requested();
    }
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock{mutex};
    static_cast<void>(wake.wait_for(lock, stop, duration, [] { return false; }));
    return !stop.stop_requested();
}

/// Waits until `fd` is readable (or hung up), polling `stop` every 20 ms. False when stopped.
inline bool wait_readable(int fd, const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        pollfd entry{.fd = fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&entry, 1, 20);
        if (ready > 0) {
            return true;
        }
        if (ready < 0 && errno != EINTR) {
            return true; // let the following recv/accept report the problem
        }
    }
    return false;
}

/// Sends all of `bytes`. False when the peer is gone.
inline bool send_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ::ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

inline std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace detail

/// A port on 127.0.0.1 that nothing listens on (bound once, then released): connecting to it is
/// refused. 0 if no socket could be made.
[[nodiscard]] inline std::uint16_t unused_port() {
    const detail::Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!socket.valid()) {
        return 0;
    }
    sockaddr_in address = detail::loopback_address(0);
    socklen_t length = sizeof address;
    if (::bind(socket.get(), detail::as_sockaddr(address), length) != 0 ||
        ::getsockname(socket.get(), detail::as_sockaddr(address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

class Server {
public:
    explicit Server(Handler handler) : handler_(std::move(handler)) {
        using detail::as_sockaddr;
        listener_ = detail::Socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (!listener_.valid()) {
            error_ = std::format("socket: {}", std::generic_category().message(errno));
            return;
        }
        const int reuse = 1;
        static_cast<void>(
            ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse));
        sockaddr_in address = detail::loopback_address(0);
        socklen_t length = sizeof address;
        if (::bind(listener_.get(), as_sockaddr(address), length) != 0 ||
            ::listen(listener_.get(), 64) != 0 ||
            ::getsockname(listener_.get(), as_sockaddr(address), &length) != 0) {
            error_ = std::format("bind/listen: {}", std::generic_category().message(errno));
            listener_.reset();
            return;
        }
        port_ = ntohs(address.sin_port);
        // `self` outlives the thread: the destructor joins it before any member dies.
        acceptor_ =
            std::jthread{[self = this](const std::stop_token& stop) { self->accept_loop(stop); }};
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    ~Server() {
        if (acceptor_.joinable()) {
            acceptor_.request_stop();
            acceptor_.join(); // no new connection threads after this
        }
        std::vector<std::jthread> connections;
        {
            const std::scoped_lock lock{mutex_};
            connections.swap(connections_);
        }
        for (std::jthread& connection : connections) {
            connection.request_stop();
        }
        connections.clear(); // joins each
    }

    [[nodiscard]] bool ok() const noexcept { return port_ != 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// "http://127.0.0.1:<port><path>"; `path` starts with '/'.
    [[nodiscard]] std::string url(std::string_view path) const {
        return std::format("http://127.0.0.1:{}{}", port_, path);
    }

    /// A copy of every request received so far, in arrival order.
    [[nodiscard]] std::vector<Request> requests() const {
        const std::scoped_lock lock{mutex_};
        return requests_;
    }

    /// How many connections were accepted so far (keep-alive reuse shows up as fewer).
    [[nodiscard]] std::size_t connections_accepted() const {
        const std::scoped_lock lock{mutex_};
        return accepted_;
    }

private:
    void accept_loop(const std::stop_token& stop) {
        while (detail::wait_readable(listener_.get(), stop)) {
            detail::Socket client{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
            if (!client.valid()) {
                continue;
            }
            const std::scoped_lock lock{mutex_};
            ++accepted_;
            // `self` outlives the thread: the destructor joins every connection thread.
            connections_.emplace_back([self = this, socket = std::move(client)](
                                          const std::stop_token& conn_stop) mutable {
                self->serve(conn_stop, std::move(socket));
            });
        }
    }

    /// Reads one request head + body. nullopt when the peer closed, on a malformed request, or
    /// on stop.
    [[nodiscard]] static std::optional<Request> read_request(int fd, std::string& buffer,
                                                             const std::stop_token& stop) {
        constexpr std::size_t max_head = std::size_t{64} * 1024;
        std::size_t head_end = std::string::npos;
        while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (buffer.size() > max_head || !receive_more(fd, buffer, stop)) {
                return std::nullopt;
            }
        }
        const std::string head = buffer.substr(0, head_end);
        buffer.erase(0, head_end + 4);

        Request request{};
        std::string_view rest{head};
        const auto first_eol = rest.find("\r\n");
        const std::string_view request_line = rest.substr(0, first_eol);
        rest =
            first_eol == std::string_view::npos ? std::string_view{} : rest.substr(first_eol + 2);
        const auto sp1 = request_line.find(' ');
        const auto sp2 = request_line.find(' ', sp1 == std::string_view::npos ? 0 : sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
            return std::nullopt;
        }
        request.method = std::string{request_line.substr(0, sp1)};
        request.target = std::string{request_line.substr(sp1 + 1, sp2 - sp1 - 1)};
        while (!rest.empty()) {
            const auto eol = rest.find("\r\n");
            const std::string_view line = rest.substr(0, eol);
            rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 2);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                request.headers.emplace_back(detail::trim(line.substr(0, colon)),
                                             detail::trim(line.substr(colon + 1)));
            }
        }
        std::size_t content_length = 0;
        if (const auto value = request.header("Content-Length"); value.has_value()) {
            try {
                content_length = static_cast<std::size_t>(std::stoull(*value));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
        // curl only waits for 100 Continue when it sends "Expect: 100-continue".
        if (const auto expect = request.header("Expect");
            expect.has_value() && *expect == "100-continue" &&
            !detail::send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n")) {
            return std::nullopt;
        }
        while (buffer.size() < content_length) {
            if (!receive_more(fd, buffer, stop)) {
                return std::nullopt;
            }
        }
        request.body = buffer.substr(0, content_length);
        buffer.erase(0, content_length);
        return request;
    }

    /// Appends whatever arrives next. False when the peer closed, on error, or on stop.
    [[nodiscard]] static bool receive_more(int fd, std::string& buffer,
                                           const std::stop_token& stop) {
        if (!detail::wait_readable(fd, stop)) {
            return false;
        }
        std::array<char, 4096> chunk{};
        const ::ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got <= 0) {
            return false;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(got));
        return true;
    }

    /// Serves requests on one connection until it ends.
    void serve(const std::stop_token& stop, detail::Socket socket) {
        const int fd = socket.get();
        std::string buffer;
        while (!stop.stop_requested()) {
            std::optional<Request> request = read_request(fd, buffer, stop);
            if (!request.has_value()) {
                return;
            }
            {
                const std::scoped_lock lock{mutex_};
                requests_.push_back(*request);
            }
            const Response response = handler_(*request);
            if (!respond(fd, response, stop)) {
                return; // the response ended the connection (or the peer is gone)
            }
        }
    }

    /// Sends one response. False when the connection must end afterwards.
    [[nodiscard]] static bool respond(int fd, const Response& response,
                                      const std::stop_token& stop) {
        using detail::send_all;
        if (response.drop_before_response) {
            return false;
        }
        if (!detail::sleep_for(stop, response.delay)) {
            return false;
        }
        std::string head = std::format("HTTP/1.1 {} {}\r\n", response.status, response.reason);
        for (const auto& [name, value] : response.headers) {
            head += std::format("{}: {}\r\n", name, value);
        }
        if (response.framing == Framing::chunked) {
            head += "Transfer-Encoding: chunked\r\n";
        } else {
            head += std::format("Content-Length: {}\r\n",
                                response.declared_length.value_or(response.body.size()));
        }
        head += "\r\n";
        if (!send_all(fd, head)) {
            return false;
        }

        std::size_t limit = response.body.size();
        if (response.stall_after.has_value()) {
            limit = std::min(limit, *response.stall_after);
        }
        if (response.drop_after.has_value()) {
            limit = std::min(limit, *response.drop_after);
        }
        const std::size_t piece = response.drip_bytes > 0 ? response.drip_bytes : limit;
        std::string_view remaining = std::string_view{response.body}.substr(0, limit);
        bool first = true;
        while (!remaining.empty()) {
            if (!first && !detail::sleep_for(stop, response.drip_interval)) {
                return false;
            }
            first = false;
            const std::string_view part = remaining.substr(0, std::max<std::size_t>(piece, 1));
            remaining.remove_prefix(part.size());
            const bool sent = response.framing == Framing::chunked
                                  ? send_all(fd, std::format("{:x}\r\n", part.size())) &&
                                        send_all(fd, part) && send_all(fd, "\r\n")
                                  : send_all(fd, part);
            if (!sent) {
                return false;
            }
        }
        if (response.drop_after.has_value()) {
            return false; // close mid-body
        }
        if (response.stall_after.has_value()) {
            // Silent until the client gives up and hangs up (or the server stops).
            std::string ignored;
            while (receive_more(fd, ignored, stop)) {
                ignored.clear();
            }
            return false;
        }
        if (response.framing == Framing::chunked) {
            return send_all(fd, "0\r\n\r\n");
        }
        return !response.declared_length.has_value(); // a wrong length desyncs keep-alive
    }

    Handler handler_{};
    detail::Socket listener_{};
    std::uint16_t port_ = 0;
    std::string error_{};
    mutable std::mutex mutex_{};
    std::vector<Request> requests_{};         ///< guarded by mutex_
    std::size_t accepted_ = 0;                ///< guarded by mutex_
    std::vector<std::jthread> connections_{}; ///< guarded by mutex_
    std::jthread acceptor_{};                 ///< last: started after everything above exists
};

} // namespace mangadex_dl::test::loopback
