// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/covenantfill.h>

#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script.h>

#include <limits>

namespace {

//! 64-bit little-endian operand, as the OP_*64 arithmetic opcodes read them.
std::vector<unsigned char> Le8(int64_t n)
{
    std::vector<unsigned char> out(8);
    uint64_t v = (uint64_t)n;
    for (int i = 0; i < 8; ++i) { out[i] = (unsigned char)(v & 0xff); v >>= 8; }
    return out;
}

std::vector<unsigned char> AssetRawBytes(const CAsset& a)
{
    const uint256 id = a.id;
    return std::vector<unsigned char>(id.begin(), id.end());
}

//! The covenant input at consensus index k credits the maker at output 2k and
//! re-pays its remainder at output 2k+1. Recomputed from the input index each
//! time, so the leaf carries no per-spend state.
void PushCreditIdx(CScript& s)
{
    s << OP_PUSHCURRENTINPUTINDEX << OP_DUP << OP_ADD;
}

void PushRemIdx(CScript& s)
{
    s << OP_PUSHCURRENTINPUTINDEX << OP_DUP << OP_ADD << OP_1ADD;
}

} // namespace

CScript BuildSeqobFillLeaf(const CAsset& asset_a, const CAsset& asset_b,
                           int64_t rate_num, int64_t rate_den, int64_t min_lot,
                           const std::vector<unsigned char>& maker_prog)
{
    CScript s;
    const std::vector<unsigned char> a = AssetRawBytes(asset_a);
    const std::vector<unsigned char> b = AssetRawBytes(asset_b);

    // locked = this covenant input's own value (must be explicit).
    s << OP_PUSHCURRENTINPUTINDEX << OP_INSPECTINPUTVALUE;
    s << OP_1 << OP_EQUALVERIFY;

    // remainder = asset A re-paid to output 2k+1 (0 for a full fill).
    PushRemIdx(s);
    s << OP_INSPECTNUMOUTPUTS << OP_LESSTHAN;
    s << OP_IF;
        PushRemIdx(s);
        s << OP_INSPECTOUTPUTASSET;
        s << OP_1 << OP_EQUALVERIFY;                 // explicit
        s << a << OP_EQUAL;
        s << OP_IF;
            // asset A at 2k+1 -> remainder: self-replicate the spk, floor it,
            // then read the value.
            PushRemIdx(s);
            s << OP_INSPECTOUTPUTSCRIPTPUBKEY;
            s << OP_PUSHCURRENTINPUTINDEX << OP_INSPECTINPUTSCRIPTPUBKEY;
            s << OP_ROT << OP_EQUALVERIFY << OP_EQUALVERIFY;
            PushRemIdx(s);
            s << OP_INSPECTOUTPUTVALUE << OP_1 << OP_EQUALVERIFY;
            s << OP_DUP << Le8(min_lot) << OP_GREATERTHANOREQUAL64 << OP_VERIFY;
        s << OP_ELSE;
            s << Le8(0);                             // remainder = 0
        s << OP_ENDIF;
    s << OP_ELSE;
        s << Le8(0);                                 // full fill, remainder = 0
    s << OP_ENDIF;

    // filled = locked - remainder, floored by min_lot.
    s << OP_SUB64 << OP_VERIFY;
    s << OP_DUP << Le8(min_lot) << OP_GREATERTHANOREQUAL64 << OP_VERIFY;

    // required_B = ceil(filled*num/den) = floor((filled*num + den-1)/den).
    s << Le8(rate_num) << OP_MUL64 << OP_VERIFY;
    s << Le8(rate_den - 1) << OP_ADD64 << OP_VERIFY;
    s << Le8(rate_den) << OP_DIV64 << OP_VERIFY << OP_NIP;

    // credit output at 2k: asset == B, spk == maker, value >= required.
    PushCreditIdx(s);
    s << OP_INSPECTOUTPUTASSET << OP_1 << OP_EQUALVERIFY;
    s << b << OP_EQUALVERIFY;
    PushCreditIdx(s);
    s << OP_INSPECTOUTPUTSCRIPTPUBKEY << OP_1 << OP_EQUALVERIFY;
    s << maker_prog << OP_EQUALVERIFY;
    PushCreditIdx(s);
    s << OP_INSPECTOUTPUTVALUE << OP_1 << OP_EQUALVERIFY;
    s << OP_SWAP << OP_GREATERTHANOREQUAL64;
    return s;
}

CScript BuildSeqobRefundLeaf(int64_t expiry_locktime, const std::vector<unsigned char>& maker_x)
{
    CScript s;
    s << CScriptNum(expiry_locktime);
    s << OP_CHECKLOCKTIMEVERIFY << OP_DROP;
    s << maker_x << OP_CHECKSIG;
    return s;
}

std::vector<unsigned char> BuildSeqobControlBlock(const std::vector<unsigned char>& internal_key,
                                                  const std::vector<std::vector<unsigned char>>& merkle_path,
                                                  bool parity)
{
    std::vector<unsigned char> cb;
    cb.reserve(33 + 32 * merkle_path.size());
    cb.push_back((unsigned char)(SEQOB_LEAF_VERSION | (parity ? 1 : 0)));
    cb.insert(cb.end(), internal_key.begin(), internal_key.end());
    for (const auto& h : merkle_path) cb.insert(cb.end(), h.begin(), h.end());
    return cb;
}

std::optional<SeqobFillScripts> BuildSeqobFillScripts(const SeqobCovenant& cov)
{
    if (cov.internal_key.size() != 32 || cov.maker_prog.size() != 32) return std::nullopt;
    if (cov.rate_num <= 0 || cov.rate_den <= 0 || cov.min_lot <= 0) return std::nullopt;
    for (const auto& h : cov.merkle_path) if (h.size() != 32) return std::nullopt;

    SeqobFillScripts out;
    out.fill_leaf = BuildSeqobFillLeaf(cov.asset_a, cov.asset_b, cov.rate_num,
                                       cov.rate_den, cov.min_lot, cov.maker_prog);
    const uint256 leaf_hash = ComputeTapleafHash(SEQOB_LEAF_VERSION, out.fill_leaf);

    // The merkle root does not depend on the parity bit, so compute it from a
    // provisional control block and then settle the parity from the tweak.
    const std::vector<unsigned char> provisional = BuildSeqobControlBlock(cov.internal_key, cov.merkle_path, false);
    const uint256 merkle_root = ComputeTaprootMerkleRoot(provisional, leaf_hash);

    const XOnlyPubKey internal(Span<const unsigned char>(cov.internal_key.data(), 32));
    const auto tweaked = internal.CreateTapTweak(&merkle_root);
    if (!tweaked) return std::nullopt;

    out.control_block = BuildSeqobControlBlock(cov.internal_key, cov.merkle_path, tweaked->second);
    out.spk = CScript() << OP_1 << std::vector<unsigned char>(tweaked->first.begin(), tweaked->first.end());
    return out;
}

std::optional<SeqobFillPlan> PlanSeqobFill(const SeqobCovenant& cov, CAmount locked, CAmount filled)
{
    if (locked <= 0 || filled <= 0 || filled > locked) return std::nullopt;
    if (cov.min_lot <= 0 || cov.rate_num <= 0 || cov.rate_den <= 0) return std::nullopt;

    SeqobFillPlan p;
    p.filled = filled;
    p.remainder = locked - filled;
    p.partial = p.remainder > 0;

    // The leaf floors BOTH the fill and any remainder at min_lot. A partial that
    // would strand the rest below the minimum is not a fill the script accepts,
    // so it is not one this node plans: better to take nothing than to broadcast
    // a transaction consensus rejects.
    if (p.filled < cov.min_lot) return std::nullopt;
    if (p.partial && p.remainder < cov.min_lot) return std::nullopt;

    p.credit = SeqobCovenantPrice(p.filled, cov.rate_num, cov.rate_den);
    if (p.credit <= 0) return std::nullopt;
    return p;
}
