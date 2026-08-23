// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_STAKINGREWARDS_H
#define BITCOIN_WALLET_STAKINGREWARDS_H

#include <asset.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/standard.h>
#include <sync.h>

#include <string>
#include <vector>

namespace wallet {
class CWallet;

/**
 * SEQUENTIA reward attribution, layer 1 of
 * doc/sequentia/reward-autoconvert-design.md: which of this wallet's coins
 * staking PAID it, as opposed to coins it merely holds.
 *
 * Two shapes, because there are exactly two ways the consensus rules pay a
 * staker: a coinbase output the wallet owns (its own block's reward, a pool's
 * committed direct payout, or a lottery draw landing on it), and an output paid
 * to P2WPKH(controller) by a pot claim under a split policy.
 */

//! One coin this wallet was PAID for staking.
struct StakingReward {
    COutPoint outpoint;
    CAsset asset;
    CAmount amount{0};
    std::string source;          //!< "solo", "direct", "lottery" or "split"
    CTxDestination dest;
    CPubKey controller;          //!< the staking key it was paid on, when it was one
    int height{0};               //!< 0 while unconfirmed
    int depth{0};
    int blocks_to_maturity{0};
    bool spent{false};

    bool Mature() const { return blocks_to_maturity == 0; }
};

/** Every staking reward this wallet has received, newest first.
 *
 *  Attribution is a function of WALLET data alone -- no chainstate lookup, no
 *  txindex -- because the light wallets have to reach the same verdict from the
 *  same facts, and a rule the node can only answer by reading a block is a rule
 *  they would have to approximate. */
std::vector<StakingReward> FindWalletStakingRewards(CWallet& wallet, bool include_spent)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

} // namespace wallet

#endif // BITCOIN_WALLET_STAKINGREWARDS_H
