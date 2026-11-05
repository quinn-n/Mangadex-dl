#pragma once
// tests/support/fake_transport.hpp: a scripted, thread-safe Transport for unit tests.
//
// Each URL has its own FIFO queue of results. fetch() records the request, runs the optional
// on_fetch hook (outside the lock), then answers in this order:
//   1. TransportErrc::cancelled when request.cancel is stopped (nothing is consumed);
//   2. the next queued result for request.url (consumed);
//   3. the fallback (set_fallback), or TransportErrc::local "unscripted URL" when none is set.
// Like the real transports, a response body larger than request.max_body_bytes becomes
// TransportErrc::too_large (0 refuses any non-empty body). wait_for_answered() lets a test wait
// for requests that the code under test sends from its own background thread.
//
//     mangadex_dl::test::FakeTransport transport;
//     transport.push_status("https://api.example/x", 503);
//     transport.push_status("https://api.example/x", 200, R"({"result":"ok"})");
//     ... exercise the code under test ...
//     CHECK(transport.count("https://api.example/x") == 2);
//     CHECK(transport.drained());

#include "mangadex_dl/net/transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangadex_dl::test {

/// A copy of what fetch() was asked (the stop token is not kept). Test-only: `body` and
/// `authorization` are kept as plain copies (tests use fake values), so a test can assert what
/// the code under test sent.
struct RecordedRequest {
    HttpMethod method = HttpMethod::get;
    std::string url{};
    std::string body{};
    std::string content_type{};
    std::size_t max_body_bytes = 0;
    std::string authorization{}; ///< the whole Authorization value; empty = no header

    friend bool operator==(const RecordedRequest&, const RecordedRequest&) = default;
};

class FakeTransport final : public Transport {
public:
    FakeTransport() = default;

    /// Queues `result` as the next answer for `url`.
    void push(std::string url, TransportResult result) {
        const std::scoped_lock lock{mutex_};
        queues_[std::move(url)].push_back(std::move(result));
    }

    void push_status(std::string url, int status, std::string body = {}) {
        push(std::move(url), HttpResponse{.status = status, .body = std::move(body)});
    }

    void push_error(std::string url, TransportErrc kind,
                    std::string message = "fake transport error") {
        push(std::move(url),
             std::unexpected(TransportError{.kind = kind, .message = std::move(message)}));
    }

    /// The answer for any URL whose queue is empty.
    void set_fallback(TransportResult result) {
        const std::scoped_lock lock{mutex_};
        fallback_ = std::move(result);
    }

    /// Runs on every fetch(), after the request is recorded and before it is answered.
    void set_on_fetch(std::function<void(const HttpRequest&)> hook) {
        const std::scoped_lock lock{mutex_};
        on_fetch_ = std::move(hook);
    }

    [[nodiscard]] TransportResult fetch(const HttpRequest& request) override {
        TransportResult result = answer(request);
        {
            const std::scoped_lock lock{mutex_};
            ++(request.method == HttpMethod::post ? answered_posts_ : answered_gets_);
        }
        answered_changed_.notify_all();
        return result;
    }

    /// Waits until at least `n` fetch() calls with `method` have returned, or until `limit`
    /// passes. True when the count was reached. For code under test that sends requests from a
    /// background thread.
    [[nodiscard]] bool wait_for_answered(HttpMethod method, std::size_t n,
                                         std::chrono::milliseconds limit) const {
        std::unique_lock lock{mutex_};
        return answered_changed_.wait_for(lock, limit, [this, method, n] {
            return (method == HttpMethod::post ? answered_posts_ : answered_gets_) >= n;
        });
    }

    /// Every request so far, in arrival order.
    [[nodiscard]] std::vector<RecordedRequest> requests() const {
        const std::scoped_lock lock{mutex_};
        return requests_;
    }

    /// How many requests went to `url`.
    [[nodiscard]] std::size_t count(std::string_view url) const {
        const std::scoped_lock lock{mutex_};
        std::size_t n = 0;
        for (const RecordedRequest& request : requests_) {
            if (request.url == url) {
                ++n;
            }
        }
        return n;
    }

    /// True when every queued result has been consumed.
    [[nodiscard]] bool drained() const {
        const std::scoped_lock lock{mutex_};
        return std::ranges::all_of(queues_, [](const auto& entry) { return entry.second.empty(); });
    }

private:
    /// fetch() without the bookkeeping of answered calls.
    [[nodiscard]] TransportResult answer(const HttpRequest& request) {
        std::function<void(const HttpRequest&)> hook;
        {
            const std::scoped_lock lock{mutex_};
            requests_.push_back(RecordedRequest{
                .method = request.method,
                .url = request.url,
                .body = request.body,
                .content_type = request.content_type,
                .max_body_bytes = request.max_body_bytes,
                .authorization = request.authorization,
            });
            hook = on_fetch_;
        }
        if (hook) {
            hook(request);
        }
        if (request.cancel.stop_requested()) {
            return std::unexpected(
                TransportError{.kind = TransportErrc::cancelled, .message = "cancelled"});
        }
        TransportResult result = next(request.url);
        if (result.has_value() && result->body.size() > request.max_body_bytes) {
            return std::unexpected(TransportError{
                .kind = TransportErrc::too_large,
                .message = std::format("body exceeds {} bytes", request.max_body_bytes),
            });
        }
        return result;
    }

    [[nodiscard]] TransportResult next(const std::string& url) {
        const std::scoped_lock lock{mutex_};
        if (const auto it = queues_.find(url); it != queues_.end() && !it->second.empty()) {
            TransportResult result = std::move(it->second.front());
            it->second.pop_front();
            return result;
        }
        if (fallback_.has_value()) {
            return *fallback_;
        }
        return std::unexpected(TransportError{
            .kind = TransportErrc::local,
            .message = std::format("fake transport: unscripted URL {}", url),
        });
    }

    mutable std::mutex mutex_{};
    std::map<std::string, std::deque<TransportResult>, std::less<>> queues_{}; ///< guarded
    std::optional<TransportResult> fallback_{};                                ///< guarded
    std::function<void(const HttpRequest&)> on_fetch_{};                       ///< guarded
    std::vector<RecordedRequest> requests_{};                                  ///< guarded
    std::size_t answered_gets_ = 0;                                            ///< guarded
    std::size_t answered_posts_ = 0;                                           ///< guarded
    mutable std::condition_variable answered_changed_{};
};

} // namespace mangadex_dl::test
