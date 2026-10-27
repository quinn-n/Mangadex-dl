// SPDX-License-Identifier: GPL-3.0-or-later
//
// fetch_custom_list (mdlist.hpp; list plan T7): one GET /list/{id} through its own ApiClient,
// then the untrusted parse. The two failure kinds stay apart so the app can add the network remedy
// to request failures only (list plan, Spec issue 1).
//
// Threads: the ApiClient is built with endpoints_for(api_base, report = false), so it starts no
// reporter thread; the InterruptBridge's poller is the only thread this file starts. Declaration
// order is the borrow order and everything is destroyed in reverse: the bridge (which requests
// the stop that cancels the client's request) is joined first, then the client, then the stop
// source, all before fetch_custom_list returns.
#include "mangadex_dl/api_client.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/interrupt.hpp"
#include "mangadex_dl/list_id.hpp"
#include "mangadex_dl/mdlist.hpp"
#include "mangadex_dl/options.hpp"

#include <expected>
#include <stop_token>
#include <string>
#include <utility>

namespace mangadex_dl {
namespace {

[[nodiscard]] ListFetchError request_error(Error error) {
    return ListFetchError{.kind = ListFailureKind::request, .error = std::move(error)};
}

} // namespace

ListFetchResult<CustomList> fetch_custom_list(const ListId& id, const ApiBase& api_base,
                                              const ListFetchServices& services) {
    if (services.transport == nullptr || services.clock == nullptr) {
        return std::unexpected(request_error(Error{
            .code = Errc::failure,
            .message = "internal error: fetch_custom_list needs a transport and a clock",
        }));
    }
    // The transport, the clock and the trace sink are the caller's and outlive the call.
    const std::stop_source stop{};
    ApiClient client{*services.transport, *services.clock,
                     ClientConfig{.endpoints = endpoints_for(api_base, /*report=*/false)},
                     services.jitter, services.trace};
    const InterruptBridge bridge{stop};

    // A signal that is already pending sends nothing (the bridge would notice it only after its
    // first 50 ms poll).
    if (interrupt_requested()) {
        return std::unexpected(
            request_error(Error{.code = Errc::interrupted, .message = "interrupted"}));
    }
    Result<std::string> body = client.fetch_list_document(id, stop.get_token());
    if (!body) {
        return std::unexpected(request_error(std::move(body.error())));
    }
    Result<CustomList> list = parse_custom_list(*body, id);
    if (!list) {
        return std::unexpected(
            ListFetchError{.kind = ListFailureKind::content, .error = std::move(list.error())});
    }
    return std::move(*list);
}

} // namespace mangadex_dl
