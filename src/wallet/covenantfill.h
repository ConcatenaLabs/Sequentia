// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_COVENANTFILL_H
#define BITCOIN_WALLET_COVENANTFILL_H

#include <asset.h>
#include <consensus/amount.h>
#include <script/script.h>
#include <seqdexclient.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

/**
 * SEQUENTIA: taking a resting SeqDEX covenant offer, from the node.
 *
 * A covenant offer is a Taproot output whose script tree contains a FILL leaf.
 * That leaf spends nothing and signs nothing: it only INSPECTS the transaction
 * spending it, and passes when the transaction pays the maker at the committed
 * rate and returns any remainder to an identical output. So anyone can take
 * part of the offer, at any time, with the maker offline -- and nobody can take
 * it on any other terms, because the script is the entire spending condition.
 *
 * That is why the node can trade at all. It has no WebSocket client and cannot
 * hold an interactive conversation with a maker, but it does not need to: it
 * builds one transaction and broadcasts it.
 *
 * These builders reproduce the leaf the maker committed to, BYTE FOR BYTE. They
 * are not a re-interpretation of it. The offer supplies the terms; if the leaf
 * built from those terms does not hash into the Taproot output actually sitting
 * on chain, the offer is a lie and nothing is spent -- which is the whole of
 * the trust model, and why VerifyAgainstOnChain exists.
 */

/** Elements/Sequentia tapscript leaf version (Bitcoin uses 0xc0). */
static constexpr uint8_t SEQOB_LEAF_VERSION = 0xc4;

/** The FILL leaf: the maker's committed terms, as consensus reads them.
 *
 *  `asset_a` (what the covenant pays out) and `asset_b` (what it must be paid
 *  in) go in as the leaf bakes them -- 32 raw bytes in INTERNAL order. The
 *  price the leaf enforces is ceil(filled * num / den), and no fill may leave
 *  either the filled amount or the remainder below `min_lot`. */
CScript BuildSeqobFillLeaf(const CAsset& asset_a, const CAsset& asset_b,
                           int64_t rate_num, int64_t rate_den, int64_t min_lot,
                           const std::vector<unsigned char>& maker_prog);

/** The REFUND leaf: the maker reclaims after `expiry_locktime`. Reproduced only
 *  so the script tree can be rebuilt when the relay does not supply the path. */
CScript BuildSeqobRefundLeaf(int64_t expiry_locktime,
                             const std::vector<unsigned char>& maker_x);

/** The control block a fill spend needs: leaf version + parity, the internal
 *  key, and the merkle path to the fill leaf. */
std::vector<unsigned char> BuildSeqobControlBlock(const std::vector<unsigned char>& internal_key,
                                                  const std::vector<std::vector<unsigned char>>& merkle_path,
                                                  bool parity);

/** The scriptPubKey a covenant with these terms MUST have, and the control
 *  block that spends its fill leaf.
 *
 *  Returns nullopt if the terms are not self-consistent. The caller then
 *  compares `spk` against the output actually on chain: equal means the relay
 *  told the truth about this offer, and unequal means it did not. */
struct SeqobFillScripts {
    CScript fill_leaf;
    CScript spk;                                //!< the covenant's own scriptPubKey
    std::vector<unsigned char> control_block;
};
std::optional<SeqobFillScripts> BuildSeqobFillScripts(const SeqobCovenant& cov);

/** How a fill of `filled` atoms of asset A splits.
 *
 *  `credit` is what the maker must be paid, ceil-rounded exactly as the script
 *  computes it; `remainder` is what goes back into an identical covenant. A
 *  fill is only valid when both the filled amount and any remainder clear
 *  `min_lot`, which is why a partial that would strand the rest below the
 *  minimum is not a fill this node will plan. */
struct SeqobFillPlan {
    CAmount filled{0};
    CAmount credit{0};
    CAmount remainder{0};
    bool partial{false};
};
std::optional<SeqobFillPlan> PlanSeqobFill(const SeqobCovenant& cov, CAmount locked, CAmount filled);

#endif // BITCOIN_WALLET_COVENANTFILL_H
