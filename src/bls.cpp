// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls.h>

#include <blst.hpp>

#include <string>

using blst::BLST_ERROR;
using blst::BLST_SUCCESS;

namespace {
//! IETF BLS "proof-of-possession" ciphersuite, min-pk (signatures in G2).
const std::string SIG_DST = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";
const std::string POP_DST = "BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

bool LoadSecret(Span<const unsigned char> sk, blst::SecretKey& out)
{
    if (sk.size() != BLS_SK_SIZE) return false;
    out.keygen(sk.data(), sk.size());
    return true;
}
} // namespace

std::optional<std::vector<unsigned char>> BlsDerivePubKey(Span<const unsigned char> sk)
{
    blst::SecretKey s;
    if (!LoadSecret(sk, s)) return std::nullopt;
    blst::P1 pk(s);
    std::vector<unsigned char> out(BLS_PK_SIZE);
    pk.compress(out.data());
    return out;
}

std::optional<std::vector<unsigned char>> BlsSign(Span<const unsigned char> sk,
                                                  Span<const unsigned char> msg32)
{
    if (msg32.size() != 32) return std::nullopt;
    blst::SecretKey s;
    if (!LoadSecret(sk, s)) return std::nullopt;
    blst::P2 sig;
    sig.hash_to(msg32.data(), msg32.size(), SIG_DST)->sign_with(s);
    std::vector<unsigned char> out(BLS_SIG_SIZE);
    sig.compress(out.data());
    return out;
}

bool BlsVerify(Span<const unsigned char> pk, Span<const unsigned char> msg32,
               Span<const unsigned char> sig)
{
    if (pk.size() != BLS_PK_SIZE || sig.size() != BLS_SIG_SIZE || msg32.size() != 32) return false;
    try {
        blst::P1_Affine pkaff(pk.data());
        blst::P2_Affine sigaff(sig.data());
        return sigaff.core_verify(pkaff, true, msg32.data(), msg32.size(), SIG_DST) == BLST_SUCCESS;
    } catch (const BLST_ERROR&) {
        return false;
    }
}

std::optional<std::vector<unsigned char>> BlsAggregate(
    const std::vector<std::vector<unsigned char>>& sigs)
{
    if (sigs.empty()) return std::nullopt;
    try {
        if (sigs[0].size() != BLS_SIG_SIZE) return std::nullopt;
        blst::P2 agg(sigs[0].data());
        for (size_t i = 1; i < sigs.size(); ++i) {
            if (sigs[i].size() != BLS_SIG_SIZE) return std::nullopt;
            agg.aggregate(blst::P2_Affine(sigs[i].data()));
        }
        std::vector<unsigned char> out(BLS_SIG_SIZE);
        agg.compress(out.data());
        return out;
    } catch (const BLST_ERROR&) {
        return std::nullopt;
    }
}

bool BlsFastAggregateVerify(const std::vector<std::vector<unsigned char>>& pks,
                            Span<const unsigned char> msg32,
                            Span<const unsigned char> aggsig)
{
    if (pks.empty() || aggsig.size() != BLS_SIG_SIZE || msg32.size() != 32) return false;
    try {
        if (pks[0].size() != BLS_PK_SIZE) return false;
        blst::P1 aggpk(pks[0].data());
        for (size_t i = 1; i < pks.size(); ++i) {
            if (pks[i].size() != BLS_PK_SIZE) return false;
            aggpk.aggregate(blst::P1_Affine(pks[i].data()));
        }
        blst::P2_Affine sigaff(aggsig.data());
        return sigaff.core_verify(aggpk.to_affine(), true, msg32.data(), msg32.size(), SIG_DST) == BLST_SUCCESS;
    } catch (const BLST_ERROR&) {
        return false;
    }
}

std::optional<std::vector<unsigned char>> BlsProvePossession(Span<const unsigned char> sk)
{
    blst::SecretKey s;
    if (!LoadSecret(sk, s)) return std::nullopt;
    unsigned char pkc[BLS_PK_SIZE];
    blst::P1(s).compress(pkc);
    blst::P2 pop;
    pop.hash_to(pkc, BLS_PK_SIZE, POP_DST)->sign_with(s);
    std::vector<unsigned char> out(BLS_SIG_SIZE);
    pop.compress(out.data());
    return out;
}

bool BlsVerifyPossession(Span<const unsigned char> pk, Span<const unsigned char> pop)
{
    if (pk.size() != BLS_PK_SIZE || pop.size() != BLS_SIG_SIZE) return false;
    try {
        blst::P1_Affine pkaff(pk.data());
        blst::P2_Affine popaff(pop.data());
        return popaff.core_verify(pkaff, true, pk.data(), pk.size(), POP_DST) == BLST_SUCCESS;
    } catch (const BLST_ERROR&) {
        return false;
    }
}
