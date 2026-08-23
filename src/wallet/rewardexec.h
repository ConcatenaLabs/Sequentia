// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_REWARDEXEC_H
#define BITCOIN_WALLET_REWARDEXEC_H

#include <seqdexclient.h>
#include <wallet/rewardconvert.h>

#include <string>
#include <vector>

#include <univalue.h>

namespace wallet {
class CWallet;

/**
 * SEQUENTIA reward auto-conversion, node side: remembering the staker's
 * instruction, and carrying it out.
 *
 * Layer 3 of doc/sequentia/reward-autoconvert-design.md. The policy above it
 * (wallet/rewardconvert.h) is pure and exhaustively tested; this is the part
 * that touches the network and spends coins, so it is deliberately thin: read
 * the book, ask policy, fill.
 */

/** One conversion this wallet has committed to.
 *
 *  `state` is the whole of the idempotence. `pending` and `done` both keep
 *  their inputs out of any further batch, because in both cases the coins may
 *  already be gone; only a DEFINITE refusal moves a record to `failed` and
 *  releases them. An executor that threw may have broadcast before it threw,
 *  and releasing there is how a wallet sells the same reward twice. */
struct RewardConversion {
    std::string id;
    std::string state;        //!< "pending", "done" or "failed"
    int64_t time{0};
    CAsset asset;
    CAmount value{0};
    std::optional<CAsset> target;
    CAmount expected{0};
    CAmount received{0};
    std::vector<COutPoint> inputs;
    std::string txid;
    std::string error;
};

RewardConvertSettings LoadRewardConvertSettings(const CWallet& wallet);
void StoreRewardConvertSettings(const CWallet& wallet, const RewardConvertSettings& s);

std::vector<RewardConversion> LoadRewardConversions(const CWallet& wallet);
void StoreRewardConversions(const CWallet& wallet, const std::vector<RewardConversion>& log);

/** Outpoints already committed to a conversion, pending or done. */
std::set<COutPoint> RewardConvertedOutpoints(const CWallet& wallet);

/** What one pass considered, and what became of it. */
struct RewardPassRow {
    RewardBatch batch;
    std::optional<RewardQuote> quote;
    RewardDecision decision;
    bool executed{false};
    std::string txid;
    std::string error;
};

struct RewardPassReport {
    bool ran{false};
    std::vector<RewardPassRow> considered;
    std::vector<std::string> errors;
};

/** Run one conversion pass over this wallet's matured rewards.
 *
 *  With `dry_run` nothing is spent and nothing is recorded: the report is what
 *  the wallet WOULD do, which is what the RPC and the GUI show. Without it, a
 *  batch the policy approves is sold.
 *
 *  Never throws for an ordinary "not now". No market, too small, and a price
 *  too far from reference are all WAITS: the coins stay where they are and the
 *  batch is reconsidered next pass. */
RewardPassReport RunRewardConversionPass(CWallet& wallet, bool dry_run);

/** The background pass, wired into the node's scheduler at startup: every
 *  loaded wallet whose staker has switched conversion on, on a slow tick.
 *  Rewards arrive at block pace at best, so looking more often only costs the
 *  relay a book read. */
void StartRewardConversionScheduler();

} // namespace wallet

#endif // BITCOIN_WALLET_REWARDEXEC_H
