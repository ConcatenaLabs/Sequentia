// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wsclient.h>

#include <crypto/sha1.h>
#include <netbase.h>
#include <random.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <cstring>

namespace {

//! RFC 6455's fixed GUID, concatenated with the client key to form the accept
//! token. Its only job is to prove the server understood the handshake rather
//! than echoing bytes back at us.
const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr uint8_t OP_CONT = 0x0;
constexpr uint8_t OP_TEXT = 0x1;
constexpr uint8_t OP_BINARY = 0x2;
constexpr uint8_t OP_CLOSE = 0x8;
constexpr uint8_t OP_PING = 0x9;
constexpr uint8_t OP_PONG = 0xA;

//! A frame no sane relay would send. A peer that announces one is either broken
//! or hostile, and either way this conversation is over: allocating on a
//! stranger's say-so is how a client becomes the vulnerability.
constexpr uint64_t MAX_FRAME = 8 * 1024 * 1024;

std::string LowerCopy(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

} // namespace

WsClient::~WsClient()
{
    Close();
}

std::unique_ptr<WsClient> WsClient::Connect(const std::string& host, uint16_t port,
                                            const std::string& path,
                                            std::chrono::milliseconds timeout,
                                            std::string& error)
{
    CService addr;
    if (!Lookup(host, addr, port, /*fAllowLookup=*/true)) {
        error = strprintf("could not resolve %s", host);
        return nullptr;
    }
    std::unique_ptr<Sock> sock = CreateSock(addr);
    if (!sock) {
        error = "could not create a socket";
        return nullptr;
    }
    if (!ConnectSocketDirectly(addr, *sock, (int)timeout.count(), /*manual_connection=*/true)) {
        error = strprintf("could not connect to %s:%d", host, port);
        return nullptr;
    }

    auto client = std::unique_ptr<WsClient>(new WsClient());
    client->m_sock = std::move(sock);

    // The client key is 16 random bytes, base64. It is not a secret and not a
    // authentication of anything -- it exists so the accept token proves the
    // server spoke WebSocket rather than replaying.
    unsigned char key_bytes[16];
    GetRandBytes(key_bytes, sizeof(key_bytes));
    const std::string key = EncodeBase64(Span<const unsigned char>(key_bytes, sizeof(key_bytes)));

    const std::string req =
        strprintf("GET %s HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Upgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "\r\n",
                  path, host, (int)port, key);
    if (!client->SendAll((const unsigned char*)req.data(), req.size(), error)) return nullptr;

    // Read the response head, one byte at a time up to the blank line. Small,
    // bounded, and it never over-reads into the first frame.
    std::string head;
    while (head.size() < 8192) {
        char c;
        if (!client->RecvExactly(&c, 1, timeout, error)) return nullptr;
        head.push_back(c);
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    if (head.find("\r\n\r\n") == std::string::npos) {
        error = "the relay's upgrade response never ended";
        return nullptr;
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0 && head.compare(0, 12, "HTTP/1.0 101") != 0) {
        const size_t eol = head.find("\r\n");
        error = "the relay refused the WebSocket upgrade: " + head.substr(0, eol == std::string::npos ? 0 : eol);
        return nullptr;
    }

    // Sec-WebSocket-Accept must be base64(sha1(key + GUID)).
    CSHA1 sha;
    const std::string to_hash = key + WS_GUID;
    sha.Write((const unsigned char*)to_hash.data(), to_hash.size());
    unsigned char digest[CSHA1::OUTPUT_SIZE];
    sha.Finalize(digest);
    const std::string expect = EncodeBase64(Span<const unsigned char>(digest, sizeof(digest)));

    const std::string lower = LowerCopy(head);
    const size_t at = lower.find("sec-websocket-accept:");
    if (at == std::string::npos) {
        error = "the relay's upgrade response carried no accept token";
        return nullptr;
    }
    size_t vs = at + strlen("sec-websocket-accept:");
    while (vs < head.size() && (head[vs] == ' ' || head[vs] == '\t')) ++vs;
    const size_t ve = head.find("\r\n", vs);
    const std::string got = head.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
    if (got != expect) {
        error = "the relay's accept token did not match; this is not a WebSocket endpoint";
        return nullptr;
    }
    return client;
}

bool WsClient::SendAll(const unsigned char* data, size_t len, std::string& error)
{
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = m_sock->Send(data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            error = "the relay connection dropped while sending";
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

bool WsClient::RecvExactly(void* buf, size_t len, std::chrono::milliseconds timeout, std::string& error)
{
    unsigned char* p = (unsigned char*)buf;
    size_t got = 0;
    while (got < len) {
        if (!m_sock->Wait(timeout, Sock::RECV)) {
            error = "the relay went quiet";
            return false;
        }
        const ssize_t n = m_sock->Recv(p + got, len - got, 0);
        if (n <= 0) {
            error = "the relay connection closed";
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

bool WsClient::SendFrame(uint8_t opcode, const unsigned char* payload, size_t len, std::string& error)
{
    std::vector<unsigned char> frame;
    frame.reserve(len + 14);
    frame.push_back((unsigned char)(0x80 | opcode));   // FIN + opcode

    // Every client frame is masked; the length form is the shortest that fits.
    if (len < 126) {
        frame.push_back((unsigned char)(0x80 | len));
    } else if (len <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back((unsigned char)((len >> 8) & 0xff));
        frame.push_back((unsigned char)(len & 0xff));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) frame.push_back((unsigned char)((((uint64_t)len) >> (8 * i)) & 0xff));
    }

    unsigned char mask[4];
    GetRandBytes(mask, sizeof(mask));
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < len; ++i) frame.push_back((unsigned char)(payload[i] ^ mask[i % 4]));

    return SendAll(frame.data(), frame.size(), error);
}

bool WsClient::SendText(const std::string& payload, std::string& error)
{
    if (m_closed) { error = "the connection is closed"; return false; }
    return SendFrame(OP_TEXT, (const unsigned char*)payload.data(), payload.size(), error);
}

bool WsClient::RecvText(std::string& out, std::chrono::milliseconds timeout, std::string& error)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (m_closed) { error = "the connection is closed"; return false; }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) { error = "timed out waiting for the maker"; return false; }

        unsigned char hdr[2];
        if (!RecvExactly(hdr, 2, left, error)) return false;
        const uint8_t opcode = hdr[0] & 0x0f;
        const bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7f;
        if (len == 126) {
            unsigned char e[2];
            if (!RecvExactly(e, 2, left, error)) return false;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (!RecvExactly(e, 8, left, error)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        if (len > MAX_FRAME) {
            error = "the relay announced an absurd frame";
            m_closed = true;
            return false;
        }
        unsigned char mask[4] = {0};
        if (masked) {
            // A server MUST NOT mask. Tolerate it rather than fail, but the
            // bytes still have to be unmasked to mean anything.
            if (!RecvExactly(mask, 4, left, error)) return false;
        }
        std::vector<unsigned char> payload((size_t)len);
        if (len && !RecvExactly(payload.data(), payload.size(), left, error)) return false;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
        }

        switch (opcode) {
        case OP_TEXT:
        case OP_BINARY:
        case OP_CONT:
            // The relay sends JSON, as text or binary depending on its mood;
            // both carry the same bytes and the caller parses either.
            out.assign(payload.begin(), payload.end());
            return true;
        case OP_PING: {
            std::string ignored;
            SendFrame(OP_PONG, payload.data(), payload.size(), ignored);
            break;   // keep waiting for something worth returning
        }
        case OP_PONG:
            break;
        case OP_CLOSE:
            m_closed = true;
            error = "the relay closed the connection";
            return false;
        default:
            break;   // an opcode we do not know is not ours to interpret
        }
    }
}

void WsClient::Close()
{
    if (m_closed || !m_sock) return;
    m_closed = true;
    std::string ignored;
    const unsigned char normal[2] = {0x03, 0xe8};   // 1000, going away cleanly
    SendFrame(OP_CLOSE, normal, sizeof(normal), ignored);
    m_sock.reset();
}
