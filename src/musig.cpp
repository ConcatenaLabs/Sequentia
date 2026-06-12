// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <musig.h>

#include <random.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <cstring>

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
