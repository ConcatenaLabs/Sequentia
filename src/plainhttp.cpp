// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <plainhttp.h>

#include <support/events.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <event2/buffer.h>
#include <event2/dns.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <chrono>
#include <memory>

namespace {

/** What the callbacks fill in while the event loop runs. */
struct Exchange {
    struct event_base* base{nullptr};
    int status{0};
    int error{-1};
    bool done{false};
    bool timed_out{false};
    bool interrupted{false};
    std::string body;
    std::chrono::steady_clock::time_point deadline;
    const std::function<bool()>* interrupt{nullptr};
};

void request_done(struct evhttp_request* req, void* ctx)
{
    Exchange* x = static_cast<Exchange*>(ctx);
    x->done = true;
    if (req != nullptr) {
        x->status = evhttp_request_get_response_code(req);
        struct evbuffer* buf = evhttp_request_get_input_buffer(req);
        if (buf) {
            const size_t size = evbuffer_get_length(buf);
            const char* data = (const char*)evbuffer_pullup(buf, size);
            if (data) x->body = std::string(data, size);
            evbuffer_drain(buf, size);
        }
    }
    event_base_loopbreak(x->base);
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void request_error(enum evhttp_request_error err, void* ctx)
{
    static_cast<Exchange*>(ctx)->error = err;
}
#endif

/** Several times a second: end the loop at the deadline, or when asked to. */
void tick(evutil_socket_t, short, void* ctx)
{
    Exchange* x = static_cast<Exchange*>(ctx);
    if (x->interrupt && *x->interrupt && (*x->interrupt)()) {
        x->interrupted = true;
        event_base_loopbreak(x->base);
    } else if (std::chrono::steady_clock::now() >= x->deadline) {
        x->timed_out = true;
        event_base_loopbreak(x->base);
    }
}

struct DnsBaseDeleter {
    void operator()(struct evdns_base* dns) const { evdns_base_free(dns, /*fail_requests=*/0); }
};
struct HttpConnectionDeleter {
    void operator()(struct evhttp_connection* c) const { evhttp_connection_free(c); }
};

} // namespace

bool PlainHttpFetch(const PlainHttpRequest& request, int& out_status, std::string& out_body, std::string& out_err)
{
    out_status = 0;
    const std::string scheme = "http://";
    if (request.url.rfind(scheme, 0) != 0) { out_err = "only http:// URLs are supported"; return false; }
    const std::string rest = request.url.substr(scheme.size());
    std::string hostport, path;
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) { hostport = rest; path = "/"; }
    else { hostport = rest.substr(0, slash); path = rest.substr(slash); }

    std::string host = hostport;
    int port = 80;
    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = LocaleIndependentAtoi<int>(hostport.substr(colon + 1));
    }
    if (host.empty() || port <= 0 || port > 65535) { out_err = "malformed URL"; return false; }
    const int timeout_secs = std::max(1, request.timeout_secs);

    // Declared in this order so they are freed in the reverse one: the timer
    // and the connection (which cancels a lookup in flight) before the
    // resolver, and everything before the event base.
    raii_event_base base = obtain_event_base();
    // The name is resolved inside the event loop, so the deadline covers it.
    // Without a resolver of its own libevent resolves with a blocking
    // getaddrinfo() that no timer can end.
    std::unique_ptr<struct evdns_base, DnsBaseDeleter> dns{
        evdns_base_new(base.get(), EVDNS_BASE_INITIALIZE_NAMESERVERS | EVDNS_BASE_DISABLE_WHEN_INACTIVE)};
    std::unique_ptr<struct evhttp_connection, HttpConnectionDeleter> evcon{
        evhttp_connection_base_new(base.get(), dns.get(), host.c_str(), static_cast<uint16_t>(port))};
    if (!evcon) { out_err = "create http connection failed"; return false; }
    evhttp_connection_set_timeout(evcon.get(), timeout_secs);
    evhttp_connection_set_max_headers_size(evcon.get(), PLAIN_HTTP_MAX_HEADERS);
    evhttp_connection_set_max_body_size(evcon.get(), request.max_body);

    Exchange x;
    x.base = base.get();
    x.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeout_secs};
    x.interrupt = &request.interrupted;
    raii_event timer = obtain_event(base.get(), -1, EV_PERSIST, tick, &x);
    if (!timer) { out_err = "create timer failed"; return false; }
    const struct timeval every{0, 100 * 1000};
    evtimer_add(timer.get(), &every);

    raii_evhttp_request req = obtain_evhttp_request(request_done, (void*)&x);
    if (req == nullptr) { out_err = "create http request failed"; return false; }
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), request_error);
#endif

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    if (output_headers) {
        evhttp_add_header(output_headers, "Host", hostport.c_str());
        evhttp_add_header(output_headers, "Connection", "close");
        if (!request.body.empty()) {
            evhttp_add_header(output_headers, "Content-Type", "application/json");
            evhttp_add_header(output_headers, "Content-Length", strprintf("%d", (int)request.body.size()).c_str());
        }
    }
    if (!request.body.empty()) {
        struct evbuffer* out = evhttp_request_get_output_buffer(req.get());
        if (!out || evbuffer_add(out, request.body.data(), request.body.size()) != 0) {
            out_err = "could not build the request body";
            return false;
        }
    }

    const int r = evhttp_make_request(evcon.get(), req.get(),
                                      request.body.empty() ? EVHTTP_REQ_GET : EVHTTP_REQ_POST, path.c_str());
    req.release(); // ownership moved to evcon in the call above
    if (r != 0) { out_err = "send http request failed"; return false; }

    event_base_dispatch(base.get());

    if (x.interrupted) { out_err = "interrupted"; return false; }
    if (!x.done || x.timed_out) { out_err = strprintf("no complete answer within %d s", timeout_secs); return false; }
    if (x.status == 0) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
        if (x.error == EVREQ_HTTP_DATA_TOO_LONG) {
            out_err = strprintf("the reply is too large (its headers are capped at %u bytes and its body at %u)",
                                PLAIN_HTTP_MAX_HEADERS, request.max_body);
            return false;
        }
        if (x.error == EVREQ_HTTP_INVALID_HEADER) {
            out_err = strprintf("the reply's status line or headers are malformed or larger than %u bytes", PLAIN_HTTP_MAX_HEADERS);
            return false;
        }
        if (x.error == EVREQ_HTTP_TIMEOUT) { out_err = strprintf("no complete answer within %d s", timeout_secs); return false; }
#endif
        out_err = "could not connect";
        return false;
    }
    out_status = x.status;
    out_body = std::move(x.body);
    return true;
}

bool PlainHttpGet(const std::string& url, int timeout_secs, size_t max_body,
                  std::string& out_body, std::string& out_err,
                  const std::function<bool()>& interrupted)
{
    PlainHttpRequest request;
    request.url = url;
    request.timeout_secs = timeout_secs;
    request.max_body = max_body;
    request.interrupted = interrupted;
    int status{0};
    if (!PlainHttpFetch(request, status, out_body, out_err)) return false;
    if (status != 200) { out_err = strprintf("the server returned HTTP %d", status); out_body.clear(); return false; }
    return true;
}
