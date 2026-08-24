// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SEQOBCOURIER_H
#define BITCOIN_SEQOBCOURIER_H

#include <consensus/amount.h>
#include <seqdexclient.h>
#include <wsclient.h>

#include <chrono>
#include <memory>
#include <string>

#include <univalue.h>

/**
 * SEQUENTIA: a courier session with a SeqDEX maker.
 *
 * The relay moves sealed bytes and nothing else. It stores no keys, holds no
 * funds, and cannot read a word of what passes through it: each side derives a
 * shared key by ECDH and seals every message under AES-256-GCM. What the relay
 * can do is refuse to carry a message, or stop carrying them halfway -- which
 * is why nothing in the protocol above this treats an unanswered message as a
 * commitment, and why every wait has a deadline.
 *
 * The one thing that must not be got wrong here: the shared key is derived from
 * the maker key in the SIGNED OFFER, never from the key the relay echoes back.
 * A relay that substitutes its own key would otherwise sit in the middle of a
 * conversation about who locks what, and the session would still look healthy.
 * The echo is checked against the offer and a mismatch aborts.
 */
class SeqobCourier
{
public:
    ~SeqobCourier();

    /** Open a lift session against `offer`.
     *
     *  `take_amount` is the size being taken, in the offer's own base units;
     *  the maker sizes its side to it. Returns nullptr with `error` set when
     *  the relay cannot be reached, will not open a lift, or answers with a
     *  maker key that is not the one the offer was signed with. */
    static std::unique_ptr<SeqobCourier> Open(const SeqobOffer& offer,
                                              CAmount take_amount,
                                              const std::string& fee_asset,
                                              std::chrono::milliseconds timeout,
                                              std::string& error);

    /** Seal `msg` and send it to the maker. */
    bool Send(const UniValue& msg, std::string& error);

    /** Wait for the maker's next message and open it.
     *
     *  A message that does not open under the shared key is DISCARDED, not
     *  reported as the maker's: it was not written by whoever holds that key.
     *  Returns false on timeout, on a closed connection, or on an explicit
     *  failure message from the far side. */
    bool Recv(UniValue& out, std::chrono::milliseconds timeout, std::string& error);

    /** Tell the maker this side is aborting, and why. Best effort: an abort
     *  that cannot be delivered is still an abort. */
    void SendFail(const std::string& code, const std::string& message);

    const std::string& SessionId() const { return m_session_id; }

    void Close();

private:
    SeqobCourier() = default;

    std::unique_ptr<WsClient> m_ws;
    std::string m_session_id;
    unsigned char m_key[32] = {0};
};

/** ECDH as the courier does it: multiply the peer's point by our scalar, take
 *  the x coordinate, hash it. Exposed for testing -- a key derivation the two
 *  sides disagree about produces a session where every message is discarded as
 *  unopenable, which looks exactly like a maker that never answers. */
bool SeqobCourierSharedKey(const std::vector<unsigned char>& peer_pubkey,
                           const unsigned char privkey[32],
                           unsigned char out_key[32]);

#endif // BITCOIN_SEQOBCOURIER_H
