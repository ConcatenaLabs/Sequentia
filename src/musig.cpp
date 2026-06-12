// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <musig.h>

#include <hash.h>
#include <random.h>
#include <sync.h>
#include <uint256.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

namespace {

const secp256k1_context* Ctx()
{
    static secp256k1_context* ctx = []() {
        secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        assert(c != nullptr);
        unsigned char seed[32];
        GetRandBytes(seed, 32);
        assert(secp256k1_context_randomize(c, seed));
        return c;
    }();
    return ctx;
}

//! Parse + canonically sort pubkeys (by compressed serialization) so the
//! aggregate depends only on the *set*, not the input order.
bool ParseSorted(const std::vector<CPubKey>& pubkeys, std::vector<secp256k1_pubkey>& out)
{
    if (pubkeys.empty()) return false;
    std::vector<std::vector<unsigned char>> sorted;
    sorted.reserve(pubkeys.size());
    for (const CPubKey& p : pubkeys) {
        if (!p.IsValid()) return false;
        sorted.emplace_back(p.begin(), p.end());
    }
    std::sort(sorted.begin(), sorted.end());
    out.resize(sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (!secp256k1_ec_pubkey_parse(Ctx(), &out[i], sorted[i].data(), sorted[i].size())) return false;
    }
    return true;
}

bool AggKey(const std::vector<CPubKey>& pubkeys, secp256k1_xonly_pubkey& agg_pk,
            secp256k1_musig_keyagg_cache& cache)
{
    std::vector<secp256k1_pubkey> parsed;
    if (!ParseSorted(pubkeys, parsed)) return false;
    std::vector<const secp256k1_pubkey*> ptrs(parsed.size());
    for (size_t i = 0; i < parsed.size(); ++i) ptrs[i] = &parsed[i];
    return secp256k1_musig_pubkey_agg(Ctx(), nullptr, &agg_pk, &cache, ptrs.data(), ptrs.size());
}

//! A fingerprint binding a signing session to its exact (set, message): the
//! sorted member set and msg32. Reusing a secret nonce with a different
//! message or set is catastrophic, so round 2 refuses any mismatch.
uint256 SessionFingerprint(const std::vector<CPubKey>& pubkeys, Span<const unsigned char> msg32)
{
    std::vector<std::vector<unsigned char>> sorted;
    for (const CPubKey& p : pubkeys) sorted.emplace_back(p.begin(), p.end());
    std::sort(sorted.begin(), sorted.end());
    CSHA256 sha;
    for (const auto& p : sorted) sha.Write(p.data(), p.size());
    sha.Write(msg32.data(), msg32.size());
    uint256 out;
    sha.Finalize(out.begin());
    return out;
}

//! Aggregate the members' 66-byte public nonces into one aggregate nonce.
bool AggregateNonces(const std::vector<std::vector<unsigned char>>& pubnonces,
                     secp256k1_musig_aggnonce& aggnonce)
{
    const secp256k1_context* ctx = Ctx();
    std::vector<secp256k1_musig_pubnonce> parsed(pubnonces.size());
    std::vector<const secp256k1_musig_pubnonce*> ptrs(pubnonces.size());
    for (size_t i = 0; i < pubnonces.size(); ++i) {
        if (pubnonces[i].size() != 66) return false;
        if (!secp256k1_musig_pubnonce_parse(ctx, &parsed[i], pubnonces[i].data())) return false;
        ptrs[i] = &parsed[i];
    }
    return secp256k1_musig_nonce_agg(ctx, &aggnonce, ptrs.data(), ptrs.size());
}

//! In-memory round-1 sessions, keyed by a caller-chosen id. Holds the live
//! (non-serialisable) secret nonce until round 2 consumes it exactly once.
struct PendingSession {
    secp256k1_musig_secnonce secnonce;
    uint256 fingerprint; //!< binds (member set, message); see SessionFingerprint
    int64_t created;     //!< steady-clock seconds, for TTL eviction
};
Mutex g_musig_sessions_mutex;
std::map<std::string, PendingSession> g_musig_sessions GUARDED_BY(g_musig_sessions_mutex);

//! Bounds so an unfinished round 1 (round 2 never called) cannot grow node
//! memory without limit: pending sessions expire, and their number is capped.
const int64_t MUSIG_SESSION_TTL_SECONDS = 600;   // abandoned after 10 minutes
const size_t MUSIG_SESSION_MAX = 10000;          // hard ceiling on live sessions

int64_t SteadySeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

//! Drop sessions older than the TTL. Caller holds g_musig_sessions_mutex.
void PurgeExpiredSessions() EXCLUSIVE_LOCKS_REQUIRED(g_musig_sessions_mutex)
{
    const int64_t now = SteadySeconds();
    for (auto it = g_musig_sessions.begin(); it != g_musig_sessions.end();) {
        if (now - it->second.created > MUSIG_SESSION_TTL_SECONDS) {
            it = g_musig_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

std::optional<std::vector<unsigned char>> MuSigAggregatePubkey(const std::vector<CPubKey>& pubkeys)
{
    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!AggKey(pubkeys, agg_pk, cache)) return std::nullopt;
    std::vector<unsigned char> out(32);
    if (!secp256k1_xonly_pubkey_serialize(Ctx(), out.data(), &agg_pk)) return std::nullopt;
    return out;
}

std::optional<std::vector<unsigned char>> MuSigSign(const std::vector<CKey>& keys,
                                                    const std::vector<CPubKey>& pubkeys,
                                                    Span<const unsigned char> msg32)
{
    if (msg32.size() != 32 || keys.empty() || keys.size() != pubkeys.size()) return std::nullopt;
    const secp256k1_context* ctx = Ctx();

    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!AggKey(pubkeys, agg_pk, cache)) return std::nullopt;

    const size_t n = keys.size();
    std::vector<secp256k1_keypair> keypairs(n);
    std::vector<secp256k1_pubkey> signer_pubkeys(n);
    std::vector<secp256k1_musig_secnonce> secnonces(n);
    std::vector<secp256k1_musig_pubnonce> pubnonces(n);
    std::vector<const secp256k1_musig_pubnonce*> pubnonce_ptrs(n);

    for (size_t i = 0; i < n; ++i) {
        if (!keys[i].IsValid()) return std::nullopt;
        unsigned char sk[32];
        std::memcpy(sk, keys[i].begin(), 32);
        if (!secp256k1_keypair_create(ctx, &keypairs[i], sk)) return std::nullopt;
        std::vector<unsigned char> pub_ser(pubkeys[i].begin(), pubkeys[i].end());
        if (!secp256k1_ec_pubkey_parse(ctx, &signer_pubkeys[i], pub_ser.data(), pub_ser.size())) return std::nullopt;

        unsigned char session_id[32];
        GetRandBytes(session_id, 32);
        if (!secp256k1_musig_nonce_gen(ctx, &secnonces[i], &pubnonces[i], session_id, sk,
                                       &signer_pubkeys[i], msg32.data(), &cache, nullptr)) {
            return std::nullopt;
        }
        pubnonce_ptrs[i] = &pubnonces[i];
    }

    secp256k1_musig_aggnonce aggnonce;
    if (!secp256k1_musig_nonce_agg(ctx, &aggnonce, pubnonce_ptrs.data(), n)) return std::nullopt;

    secp256k1_musig_session session;
    if (!secp256k1_musig_nonce_process(ctx, &session, &aggnonce, msg32.data(), &cache, nullptr)) return std::nullopt;

    std::vector<secp256k1_musig_partial_sig> partials(n);
    std::vector<const secp256k1_musig_partial_sig*> partial_ptrs(n);
    for (size_t i = 0; i < n; ++i) {
        if (!secp256k1_musig_partial_sign(ctx, &partials[i], &secnonces[i], &keypairs[i], &cache, &session)) {
            return std::nullopt;
        }
        partial_ptrs[i] = &partials[i];
    }

    std::vector<unsigned char> sig(64);
    if (!secp256k1_musig_partial_sig_agg(ctx, sig.data(), &session, partial_ptrs.data(), n)) return std::nullopt;

    // Self-check: the produced signature must verify under the aggregate key.
    if (!secp256k1_schnorrsig_verify(ctx, sig.data(), msg32.data(), 32, &agg_pk)) return std::nullopt;
    return sig;
}

bool MuSigVerify(const std::vector<CPubKey>& pubkeys, Span<const unsigned char> msg32,
                 Span<const unsigned char> sig64)
{
    if (msg32.size() != 32 || sig64.size() != 64) return false;
    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!AggKey(pubkeys, agg_pk, cache)) return false;
    return secp256k1_schnorrsig_verify(Ctx(), sig64.data(), msg32.data(), 32, &agg_pk);
}

std::optional<std::vector<unsigned char>> MuSigSessionNonce(
    const std::string& session_id, const CKey& key,
    const std::vector<CPubKey>& pubkeys, Span<const unsigned char> msg32,
    std::string& error)
{
    if (msg32.size() != 32) { error = "message must be 32 bytes"; return std::nullopt; }
    if (!key.IsValid()) { error = "invalid signing key"; return std::nullopt; }
    const secp256k1_context* ctx = Ctx();

    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!AggKey(pubkeys, agg_pk, cache)) { error = "could not aggregate the member set"; return std::nullopt; }

    unsigned char sk[32];
    std::memcpy(sk, key.begin(), 32);
    CPubKey cpub = key.GetPubKey();
    secp256k1_pubkey signer_pubkey;
    std::vector<unsigned char> pub_ser(cpub.begin(), cpub.end());
    if (!secp256k1_ec_pubkey_parse(ctx, &signer_pubkey, pub_ser.data(), pub_ser.size())) {
        error = "invalid signer public key"; return std::nullopt;
    }

    PendingSession sess;
    secp256k1_musig_pubnonce pubnonce;
    unsigned char session_secrand[32];
    GetRandBytes(session_secrand, 32);
    if (!secp256k1_musig_nonce_gen(ctx, &sess.secnonce, &pubnonce, session_secrand, sk,
                                   &signer_pubkey, msg32.data(), &cache, nullptr)) {
        error = "nonce generation failed"; return std::nullopt;
    }
    sess.fingerprint = SessionFingerprint(pubkeys, msg32);
    sess.created = SteadySeconds();

    {
        LOCK(g_musig_sessions_mutex);
        PurgeExpiredSessions();
        if (g_musig_sessions.count(session_id)) {
            error = "a signing session with this id already exists (refusing to reuse a secret nonce)";
            return std::nullopt;
        }
        if (g_musig_sessions.size() >= MUSIG_SESSION_MAX) {
            error = "too many pending signing sessions; retry later";
            return std::nullopt;
        }
        g_musig_sessions.emplace(session_id, sess);
    }

    std::vector<unsigned char> out(66);
    if (!secp256k1_musig_pubnonce_serialize(ctx, out.data(), &pubnonce)) {
        LOCK(g_musig_sessions_mutex);
        g_musig_sessions.erase(session_id);
        error = "could not serialize public nonce"; return std::nullopt;
    }
    return out;
}

std::optional<std::vector<unsigned char>> MuSigSessionPartialSign(
    const std::string& session_id, const CKey& key,
    const std::vector<CPubKey>& pubkeys,
    const std::vector<std::vector<unsigned char>>& pubnonces,
    Span<const unsigned char> msg32, std::string& error)
{
    if (msg32.size() != 32) { error = "message must be 32 bytes"; return std::nullopt; }
    if (!key.IsValid()) { error = "invalid signing key"; return std::nullopt; }
    const secp256k1_context* ctx = Ctx();

    // Look up and *remove* the round-1 session (single-use), after binding it
    // to the exact (member set, message) round 1 committed to.
    PendingSession sess;
    {
        LOCK(g_musig_sessions_mutex);
        auto it = g_musig_sessions.find(session_id);
        if (it == g_musig_sessions.end()) {
            error = "no round-1 session with this id (call the nonce step first; each session signs once)";
            return std::nullopt;
        }
        if (it->second.fingerprint != SessionFingerprint(pubkeys, msg32)) {
            // Do NOT consume: the caller may retry with the correct context.
            error = "member set or message does not match this session's round 1";
            return std::nullopt;
        }
        sess = it->second;
        g_musig_sessions.erase(it);
    }

    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!AggKey(pubkeys, agg_pk, cache)) { error = "could not aggregate the member set"; return std::nullopt; }

    secp256k1_musig_aggnonce aggnonce;
    if (!AggregateNonces(pubnonces, aggnonce)) { error = "could not aggregate the public nonces"; return std::nullopt; }

    secp256k1_musig_session musig_session;
    if (!secp256k1_musig_nonce_process(ctx, &musig_session, &aggnonce, msg32.data(), &cache, nullptr)) {
        error = "nonce processing failed"; return std::nullopt;
    }

    unsigned char sk[32];
    std::memcpy(sk, key.begin(), 32);
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, sk)) { error = "invalid keypair"; return std::nullopt; }

    secp256k1_musig_partial_sig partial;
    if (!secp256k1_musig_partial_sign(ctx, &partial, &sess.secnonce, &keypair, &cache, &musig_session)) {
        error = "partial signing failed"; return std::nullopt;
    }
    std::vector<unsigned char> out(32);
    if (!secp256k1_musig_partial_sig_serialize(ctx, out.data(), &partial)) {
        error = "could not serialize partial signature"; return std::nullopt;
    }
    return out;
}

std::optional<std::vector<unsigned char>> MuSigAggregatePartials(
    const std::vector<CPubKey>& pubkeys,
    const std::vector<std::vector<unsigned char>>& pubnonces,
    const std::vector<std::vector<unsigned char>>& partials,
    Span<const unsigned char> msg32)
{
    if (msg32.size() != 32) return std::nullopt;
    const secp256k1_context* ctx = Ctx();

    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!AggKey(pubkeys, agg_pk, cache)) return std::nullopt;

    secp256k1_musig_aggnonce aggnonce;
    if (!AggregateNonces(pubnonces, aggnonce)) return std::nullopt;

    secp256k1_musig_session musig_session;
    if (!secp256k1_musig_nonce_process(ctx, &musig_session, &aggnonce, msg32.data(), &cache, nullptr)) {
        return std::nullopt;
    }

    std::vector<secp256k1_musig_partial_sig> parsed(partials.size());
    std::vector<const secp256k1_musig_partial_sig*> ptrs(partials.size());
    for (size_t i = 0; i < partials.size(); ++i) {
        if (partials[i].size() != 32) return std::nullopt;
        if (!secp256k1_musig_partial_sig_parse(ctx, &parsed[i], partials[i].data())) return std::nullopt;
        ptrs[i] = &parsed[i];
    }

    std::vector<unsigned char> sig(64);
    if (!secp256k1_musig_partial_sig_agg(ctx, sig.data(), &musig_session, ptrs.data(), ptrs.size())) {
        return std::nullopt;
    }
    // Self-check against the aggregate key before returning.
    if (!secp256k1_schnorrsig_verify(ctx, sig.data(), msg32.data(), 32, &agg_pk)) return std::nullopt;
    return sig;
}

void MuSigSessionAbort(const std::string& session_id)
{
    LOCK(g_musig_sessions_mutex);
    g_musig_sessions.erase(session_id);
}
