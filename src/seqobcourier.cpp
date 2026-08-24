// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <seqobcourier.h>

#include <crypto/aes_gcm.h>
#include <crypto/sha256.h>
#include <key.h>
#include <support/cleanse.h>
#include <pubkey.h>
#include <random.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <secp256k1.h>

#include <cstring>

namespace {

//! Base64, which is how the relay carries every byte string in its JSON.
std::string B64(const std::vector<unsigned char>& v)
{
    return EncodeBase64(Span<const unsigned char>(v.data(), v.size()));
}

std::vector<unsigned char> UnB64(const std::string& s, bool& ok)
{
    bool invalid = false;
    const std::string d = DecodeBase64(s, &invalid);
    ok = !invalid;
    return std::vector<unsigned char>(d.begin(), d.end());
}

//! The relay answers in either snake_case or camelCase depending on which
//! encoder produced the message. Reading only one is how a client decides a
//! healthy maker never replied.
const UniValue& F(const UniValue& o, const std::string& a, const std::string& b)
{
    static const UniValue null_value;
    if (!o.isObject()) return null_value;
    if (o.exists(a)) return o[a];
    if (o.exists(b)) return o[b];
    return null_value;
}

} // namespace

bool SeqobCourierSharedKey(const std::vector<unsigned char>& peer_pubkey,
                           const unsigned char privkey[32],
                           unsigned char out_key[32])
{
    if (peer_pubkey.size() != 33 && peer_pubkey.size() != 65) return false;
    const secp256k1_context* ctx = GetVerifyContext();
    if (!ctx) return false;

    secp256k1_pubkey point;
    if (!secp256k1_ec_pubkey_parse(ctx, &point, peer_pubkey.data(), peer_pubkey.size())) return false;
    // Plain point multiplication: the shared secret is the x coordinate of
    // priv * peer, hashed. Not a KDF with context binding, because the other
    // side is a Go daemon and a browser doing exactly this and no more.
    if (!secp256k1_ec_pubkey_tweak_mul(ctx, &point, privkey)) return false;

    unsigned char ser[33];
    size_t len = sizeof(ser);
    if (!secp256k1_ec_pubkey_serialize(ctx, ser, &len, &point, SECP256K1_EC_COMPRESSED)) return false;
    if (len != 33) return false;

    CSHA256().Write(ser + 1, 32).Finalize(out_key);   // drop the parity byte, keep x
    memory_cleanse(ser, sizeof(ser));
    return true;
}

SeqobCourier::~SeqobCourier()
{
    Close();
    memory_cleanse(m_key, sizeof(m_key));
}

std::unique_ptr<SeqobCourier> SeqobCourier::Open(const SeqobOffer& offer,
                                                 CAmount take_amount,
                                                 const std::string& fee_asset,
                                                 std::chrono::milliseconds timeout,
                                                 std::string& error)
{
    if (offer.maker_pubkey.empty() || !IsHex(offer.maker_pubkey)) {
        error = "the offer carries no usable maker key";
        return nullptr;
    }
    const std::vector<unsigned char> maker_pub = ParseHex(offer.maker_pubkey);
    if (maker_pub.size() != 33) {
        error = "the offer's maker key is not a compressed point";
        return nullptr;
    }

    std::string host, prefix;
    uint16_t port = 0;
    if (!SeqdexRelayEndpoint(host, port, prefix)) {
        error = "no SeqDEX relay configured: set -seqoburl=http://host:port";
        return nullptr;
    }

    auto ws = WsClient::Connect(host, port, prefix + "/v1/ws", timeout, error);
    if (!ws) return nullptr;

    auto c = std::unique_ptr<SeqobCourier>(new SeqobCourier());
    c->m_ws = std::move(ws);

    // An ephemeral session key, used for this conversation and nothing else. It
    // is never an on-chain key: a courier session leaks its public half to the
    // relay, and an on-chain identity is not a thing to hand over for that.
    unsigned char sess_priv[32];
    CPubKey sess_pub;
    {
        CKey k;
        do { k.MakeNewKey(/*fCompressed=*/true); } while (!k.IsValid());
        memcpy(sess_priv, k.begin(), 32);
        sess_pub = k.GetPubKey();
    }

    UniValue start(UniValue::VOBJ);
    UniValue sl(UniValue::VOBJ);
    sl.pushKV("offer_id", offer.offer_id);
    sl.pushKV("maker_pubkey", offer.maker_pubkey);
    sl.pushKV("take_amount", ToString(take_amount));
    sl.pushKV("taker_fee_asset", fee_asset);
    sl.pushKV("taker_session_pubkey", B64(std::vector<unsigned char>(sess_pub.begin(), sess_pub.end())));
    start.pushKV("start_lift", sl);

    if (!c->m_ws->SendText(start.write(), error)) {
        memory_cleanse(sess_priv, sizeof(sess_priv));
        return nullptr;
    }

    // Wait for lift_accepted, stepping over anything else the relay chats about.
    UniValue accepted;
    bool got = false;
    for (int i = 0; i < 8 && !got; ++i) {
        std::string raw;
        if (!c->m_ws->RecvText(raw, timeout, error)) {
            memory_cleanse(sess_priv, sizeof(sess_priv));
            return nullptr;
        }
        UniValue m;
        if (!m.read(raw) || !m.isObject()) continue;
        if (m.exists("error")) {
            const UniValue& e = m["error"];
            error = "relay: " + (e.isStr() ? e.get_str() : e.write());
            memory_cleanse(sess_priv, sizeof(sess_priv));
            return nullptr;
        }
        const UniValue& la = F(m, "lift_accepted", "liftAccepted");
        if (la.isObject()) { accepted = la; got = true; }
    }
    if (!got) {
        error = "the relay did not accept the lift";
        memory_cleanse(sess_priv, sizeof(sess_priv));
        return nullptr;
    }

    const UniValue& sid = F(accepted, "session_id", "sessionId");
    c->m_session_id = sid.isStr() ? sid.get_str() : "";
    if (c->m_session_id.empty()) {
        error = "the relay accepted the lift without a session";
        memory_cleanse(sess_priv, sizeof(sess_priv));
        return nullptr;
    }

    // The echo check. Derive the shared key from the key in the SIGNED OFFER,
    // and refuse if the relay echoed a different one: a substituted key is a
    // relay sitting in the middle of the conversation about who locks what,
    // and everything after this point would still look healthy.
    const UniValue& echo = F(accepted, "maker_session_pubkey", "makerSessionPubkey");
    if (echo.isStr() && !echo.get_str().empty()) {
        bool ok = false;
        const auto echoed = UnB64(echo.get_str(), ok);
        if (!ok || echoed != maker_pub) {
            error = "the relay returned a maker key the offer was not signed with; aborting";
            memory_cleanse(sess_priv, sizeof(sess_priv));
            return nullptr;
        }
    }

    if (!SeqobCourierSharedKey(maker_pub, sess_priv, c->m_key)) {
        error = "could not derive the session key";
        memory_cleanse(sess_priv, sizeof(sess_priv));
        return nullptr;
    }
    memory_cleanse(sess_priv, sizeof(sess_priv));
    return c;
}

bool SeqobCourier::Send(const UniValue& msg, std::string& error)
{
    if (!m_ws) { error = "the courier session is closed"; return false; }

    const std::string plain = msg.write();
    unsigned char nonce[AES_GCM_NONCE_SIZE];
    GetRandBytes(nonce, sizeof(nonce));
    const std::vector<unsigned char> body(plain.begin(), plain.end());
    const auto sealed = AES256GCMEncrypt(m_key, nonce, body);

    // nonce || ciphertext || tag, the layout the Go and browser sides produce.
    std::vector<unsigned char> wire;
    wire.reserve(sizeof(nonce) + sealed.size());
    wire.insert(wire.end(), nonce, nonce + sizeof(nonce));
    wire.insert(wire.end(), sealed.begin(), sealed.end());

    UniValue env(UniValue::VOBJ), sm(UniValue::VOBJ);
    sm.pushKV("session_id", m_session_id);
    sm.pushKV("ciphertext", B64(wire));
    env.pushKV("swap_msg", sm);
    return m_ws->SendText(env.write(), error);
}

bool SeqobCourier::Recv(UniValue& out, std::chrono::milliseconds timeout, std::string& error)
{
    if (!m_ws) { error = "the courier session is closed"; return false; }
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) { error = "timed out waiting for the maker"; return false; }

        std::string raw;
        if (!m_ws->RecvText(raw, left, error)) return false;
        UniValue m;
        if (!m.read(raw) || !m.isObject()) continue;
        if (m.exists("error")) {
            const UniValue& e = m["error"];
            error = "relay: " + (e.isStr() ? e.get_str() : e.write());
            return false;
        }
        const UniValue& sm = F(m, "swap_msg", "swapMsg");
        if (!sm.isObject()) continue;
        const UniValue& ct = F(sm, "ciphertext", "cipherText");
        if (!ct.isStr()) continue;

        bool ok = false;
        const auto wire = UnB64(ct.get_str(), ok);
        if (!ok || wire.size() <= AES_GCM_NONCE_SIZE) continue;

        unsigned char nonce[AES_GCM_NONCE_SIZE];
        memcpy(nonce, wire.data(), sizeof(nonce));
        const std::vector<unsigned char> sealed(wire.begin() + AES_GCM_NONCE_SIZE, wire.end());

        std::vector<unsigned char> plain;
        if (!AES256GCMDecrypt(m_key, nonce, sealed, {}, plain)) {
            // Not written by whoever holds the shared key. There is nothing in
            // it worth reading, and treating it as the maker's would be exactly
            // the mistake the sealing exists to prevent.
            continue;
        }
        UniValue parsed;
        if (!parsed.read(std::string(plain.begin(), plain.end()))) continue;
        out = parsed;
        return true;
    }
}

void SeqobCourier::SendFail(const std::string& code, const std::string& message)
{
    UniValue m(UniValue::VOBJ);
    m.pushKV("type", "fail");
    m.pushKV("code", code);
    m.pushKV("message", message);
    std::string ignored;
    Send(m, ignored);
}

void SeqobCourier::Close()
{
    if (m_ws) {
        m_ws->Close();
        m_ws.reset();
    }
}
