// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_HTLC_H
#define BITCOIN_WALLET_HTLC_H

#include <script/script.h>
#include <uint256.h>

#include <optional>
#include <vector>

/**
 * SEQUENTIA: the hashed timelock both legs of a cross-chain swap are built on.
 *
 *     OP_IF
 *       OP_SIZE <32> OP_EQUALVERIFY          -- the preimage is exactly 32 bytes
 *       OP_SHA256 <H> OP_EQUALVERIFY
 *       <claim_pub> OP_CHECKSIG              -- claim, by revealing the secret
 *     OP_ELSE
 *       <locktime> OP_CHECKLOCKTIMEVERIFY OP_DROP
 *       <refund_pub> OP_CHECKSIG             -- refund, after the timelock
 *     OP_ENDIF
 *
 * The same script serves on Sequentia and on Bitcoin, which is the whole trick:
 * one secret unlocks both legs, so whoever reveals it to take one has revealed
 * it to the other. The `OP_SIZE <32>` guard is not decoration -- without it a
 * preimage of some other length can satisfy the hash on one chain and not the
 * other, and the atomicity quietly stops being atomic.
 *
 * Byte-for-byte the script SWK builds (`lwk_wollet::seqdex_htlc`) and the
 * daemon's makers build. A swap where the two sides disagree about a single
 * push encoding is a swap where one side's money is locked in a script the
 * other cannot spend, so this is pinned by a shared vector rather than trusted.
 */
std::optional<CScript> BuildHtlcRedeemScript(const std::vector<unsigned char>& hash,
                                             const std::vector<unsigned char>& claim_pub,
                                             const std::vector<unsigned char>& refund_pub,
                                             uint32_t locktime);

/** The P2SH scriptPubKey that pays a redeem script -- the Sequentia leg's form. */
CScript HtlcP2shSpk(const CScript& redeem);

/** The P2WSH scriptPubKey that pays a witness script -- the Bitcoin leg's form. */
CScript HtlcP2wshSpk(const CScript& witness_script);

/** Read the terms back out of a redeem script, or nullopt if it is not one.
 *
 *  Used to check that what a maker actually funded on the parent chain is the
 *  script it told us about -- the message is a claim, the coin is the fact. */
struct HtlcTerms {
    std::vector<unsigned char> hash;
    std::vector<unsigned char> claim_pub;
    std::vector<unsigned char> refund_pub;
    uint32_t locktime{0};
};
std::optional<HtlcTerms> ParseHtlcRedeemScript(const CScript& redeem);

#endif // BITCOIN_WALLET_HTLC_H
