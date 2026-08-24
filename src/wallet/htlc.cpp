// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/htlc.h>

#include <crypto/sha256.h>
#include <hash.h>
#include <pubkey.h>
#include <script/standard.h>

std::optional<CScript> BuildHtlcRedeemScript(const std::vector<unsigned char>& hash,
                                             const std::vector<unsigned char>& claim_pub,
                                             const std::vector<unsigned char>& refund_pub,
                                             uint32_t locktime)
{
    if (hash.size() != 32) return std::nullopt;
    // Compressed keys only: the daemon embeds 33-byte keys and the byte-match
    // depends on the OP_PUSHBYTES_33 that implies. A key that does not parse is
    // a key nobody can spend with, which is worth refusing before it is funded.
    for (const auto* pk : {&claim_pub, &refund_pub}) {
        if (pk->size() != 33) return std::nullopt;
        const CPubKey parsed(*pk);
        if (!parsed.IsFullyValid() || !parsed.IsCompressed()) return std::nullopt;
    }

    CScript s;
    s << OP_IF;
    s << OP_SIZE << CScriptNum(32) << OP_EQUALVERIFY;
    s << OP_SHA256 << hash << OP_EQUALVERIFY;
    s << claim_pub << OP_CHECKSIG;
    s << OP_ELSE;
    s << CScriptNum((int64_t)locktime) << OP_CHECKLOCKTIMEVERIFY << OP_DROP;
    s << refund_pub << OP_CHECKSIG;
    s << OP_ENDIF;
    return s;
}

CScript HtlcP2shSpk(const CScript& redeem)
{
    return GetScriptForDestination(ScriptHash(redeem));
}

CScript HtlcP2wshSpk(const CScript& witness_script)
{
    return GetScriptForDestination(WitnessV0ScriptHash(witness_script));
}

std::optional<HtlcTerms> ParseHtlcRedeemScript(const CScript& redeem)
{
    // Walk the script as a token sequence rather than pattern-matching bytes:
    // the shape is fixed, but the locktime's push width is not.
    std::vector<std::pair<opcodetype, std::vector<unsigned char>>> toks;
    CScript::const_iterator it = redeem.begin();
    while (it != redeem.end()) {
        opcodetype op;
        std::vector<unsigned char> data;
        if (!redeem.GetOp(it, op, data)) return std::nullopt;
        toks.emplace_back(op, std::move(data));
        if (toks.size() > 32) return std::nullopt;
    }
    if (toks.size() != 16) return std::nullopt;

    auto is_op = [&](size_t i, opcodetype op) { return toks[i].first == op && toks[i].second.empty(); };
    auto push_of = [&](size_t i, size_t len) { return toks[i].second.size() == len; };

    if (!is_op(0, OP_IF)) return std::nullopt;
    if (!is_op(1, OP_SIZE)) return std::nullopt;
    // <32>: a one-byte script number push.
    if (toks[2].second.size() != 1 || toks[2].second[0] != 32) return std::nullopt;
    if (!is_op(3, OP_EQUALVERIFY)) return std::nullopt;
    if (!is_op(4, OP_SHA256)) return std::nullopt;
    if (!push_of(5, 32)) return std::nullopt;
    if (!is_op(6, OP_EQUALVERIFY)) return std::nullopt;
    if (!push_of(7, 33)) return std::nullopt;
    if (!is_op(8, OP_CHECKSIG)) return std::nullopt;
    if (!is_op(9, OP_ELSE)) return std::nullopt;
    if (toks[10].second.empty()) return std::nullopt;   // the locktime push
    if (!is_op(11, OP_CHECKLOCKTIMEVERIFY)) return std::nullopt;
    if (!is_op(12, OP_DROP)) return std::nullopt;
    if (!push_of(13, 33)) return std::nullopt;
    if (!is_op(14, OP_CHECKSIG)) return std::nullopt;
    if (!is_op(15, OP_ENDIF)) return std::nullopt;

    HtlcTerms t;
    t.hash = toks[5].second;
    t.claim_pub = toks[7].second;
    t.refund_pub = toks[13].second;
    // The locktime is a script number, whose width depends on its value.
    try {
        t.locktime = (uint32_t)CScriptNum(toks[10].second, /*fRequireMinimal=*/true, 5).getint();
    } catch (const scriptnum_error&) {
        return std::nullopt;
    }
    return t;
}
