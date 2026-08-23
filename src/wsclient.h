// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WSCLIENT_H
#define BITCOIN_WSCLIENT_H

#include <util/sock.h>

#include <chrono>
#include <memory>
#include <string>

/**
 * SEQUENTIA: a minimal RFC 6455 WebSocket client.
 *
 * Just enough to hold one conversation with a SeqDEX relay: connect, upgrade,
 * exchange text frames, close. No extensions, no compression, no fragmentation
 * on send, no TLS -- the node links none, so this speaks ws:// only, for the
 * same reason and with the same consequences as the relay's REST client.
 *
 * It exists because a cross-chain conversion is a CONVERSATION. A covenant fill
 * is one transaction the node can build alone, but selling an asset for native
 * Bitcoin means agreeing terms with a maker, watching it lock the Bitcoin,
 * locking the asset, and claiming with the secret it reveals -- and the relay
 * carries those messages over a WebSocket. Without this the node could convert
 * rewards into any Sequentia asset and not into the one most stakers want.
 *
 * Blocking, with deadlines on every wait. It runs on a background thread that
 * has nothing else to do, and a conversation that stalls has to end rather than
 * hang: the counterparty is a stranger.
 */
class WsClient
{
public:
    ~WsClient();

    /** Connect to ws://host:port/path and complete the upgrade handshake.
     *  Returns nullptr and sets `error` on any failure -- including a server
     *  that answers something other than 101, or with an accept token that does
     *  not match the key we sent, which means we are not talking to a
     *  WebSocket endpoint whatever it claims. */
    static std::unique_ptr<WsClient> Connect(const std::string& host, uint16_t port,
                                             const std::string& path,
                                             std::chrono::milliseconds timeout,
                                             std::string& error);

    /** Send one text frame. Client frames are masked, as the RFC requires. */
    bool SendText(const std::string& payload, std::string& error);

    /** Receive the next text frame's payload.
     *
     *  Answers pings and skips anything that is not a text frame, so callers
     *  see a stream of messages rather than a stream of frames. Returns false
     *  on timeout, on close, or on a malformed frame. */
    bool RecvText(std::string& out, std::chrono::milliseconds timeout, std::string& error);

    /** Send a close frame. Best effort: a peer that has already gone is not an
     *  error worth reporting to anyone. */
    void Close();

private:
    WsClient() = default;
    bool RecvExactly(void* buf, size_t len, std::chrono::milliseconds timeout, std::string& error);
    bool SendAll(const unsigned char* data, size_t len, std::string& error);
    bool SendFrame(uint8_t opcode, const unsigned char* payload, size_t len, std::string& error);

    std::unique_ptr<Sock> m_sock;
    bool m_closed{false};
};

#endif // BITCOIN_WSCLIENT_H
