// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <plainhttp.h>

#include <support/events.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

namespace {

/** Reply structure for the request-done callback to fill in. */
struct HTTPReply {
    HTTPReply() : status(0), error(-1) {}
    int status;
    int error;
    std::string body;
};

void http_request_done(struct evhttp_request* req, void* ctx)
{
    HTTPReply* reply = static_cast<HTTPReply*>(ctx);
    if (req == nullptr) { reply->status = 0; return; }
    reply->status = evhttp_request_get_response_code(req);
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (buf) {
        size_t size = evbuffer_get_length(buf);
        const char* data = (const char*)evbuffer_pullup(buf, size);
        if (data) reply->body = std::string(data, size);
        evbuffer_drain(buf, size);
    }
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void http_error_cb(enum evhttp_request_error err, void* ctx)
{
    static_cast<HTTPReply*>(ctx)->error = err;
}
#endif

} // namespace

bool PlainHttpGet(const std::string& url, int timeout_secs, size_t max_body,
                  std::string& out_body, std::string& out_err)
{
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) { out_err = "only http:// URLs are supported"; return false; }
    const std::string rest = url.substr(scheme.size());
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

    raii_event_base base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, port);
    evhttp_connection_set_timeout(evcon.get(), timeout_secs);
    evhttp_connection_set_max_body_size(evcon.get(), max_body);

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    if (req == nullptr) { out_err = "create http request failed"; return false; }
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), http_error_cb);
#endif

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    if (output_headers) {
        evhttp_add_header(output_headers, "Host", hostport.c_str());
        evhttp_add_header(output_headers, "Connection", "close");
    }

    const int r = evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_GET, path.c_str());
    req.release(); // ownership moved to evcon in the call above
    if (r != 0) { out_err = "send http request failed"; return false; }

    event_base_dispatch(base.get());

    if (response.status == 0) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
        if (response.error == EVREQ_HTTP_DATA_TOO_LONG) { out_err = strprintf("the reply is larger than %u bytes", max_body); return false; }
        if (response.error == EVREQ_HTTP_TIMEOUT) { out_err = "timed out"; return false; }
#endif
        out_err = "could not connect";
        return false;
    }
    if (response.status != 200) { out_err = strprintf("the server returned HTTP %d", response.status); return false; }
    out_body = response.body;
    return true;
}

