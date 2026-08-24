// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_REWARDCONVERT_H
#define BITCOIN_WALLET_REWARDCONVERT_H

#include <asset.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wallet {

/**
 * SEQUENTIA reward auto-conversion, node side: the policy that decides which
 * staking rewards to sell, and for what.
 *
 * This is layer 2 of doc/sequentia/reward-autoconvert-design.md. Layer 1
 * (which coins are rewards) is `liststakingrewards` in wallet/rpc/spend.cpp;
 * layer 3 (actually selling) is wallet/rewardexec.{h,cpp}.
 *
 * Everything here is a PURE function of a batch plus a quote, with no wallet,
 * no chain and no network in sight, for the same reason the light wallets keep
 * their copy pure: the decision that sells someone's coins is the one that has
 * to be exhaustively testable. It mirrors SWK's `lwk_wollet::staking_rewards`
 * rule for rule, and `rewardconvert_tests.cpp` pins the cases both must agree
 * on -- the two implementations are how a node wallet and a light wallet
 * watching the SAME keys would otherwise drift apart.
 */

/** What rewards are converted into.
 *
 *  Native parent-chain BTC is the default and the top of every picker, but it
 *  is not the only choice: outside staking no asset is privileged, and a staker
 *  who wants USDX, or GOLD, or more SEQ to stake with is doing the same thing
 *  for the same reason. `target` unset means native BTC -- never SBTC, which is
 *  a narrow opt-in peg a staker can still choose explicitly, as an asset. */
struct RewardConvertSettings {
    //! Opt-in, always: converting rewards is irreversible.
    bool enabled{false};
    //! std::nullopt = native parent-chain BTC.
    std::optional<CAsset> target;
    //! Assets to keep as they are, on top of the target itself.
    std::set<CAsset> exclude;
    //! The floor a batch must clear, in the TARGET asset's atoms.
    CAmount min_receive{10000};
    //! How far below the reference price a fill may land, in basis points.
    int max_slippage_bp{200};

    bool IsTargetAsset(const CAsset& asset) const { return target.has_value() && *target == asset; }
    //! Native BTC settles cross-chain; a Sequentia asset settles same-chain.
    //! Auto-conversion adds no settlement primitive of its own -- which one
    //! runs follows from the target alone.
    bool TargetIsNativeBtc() const { return !target.has_value(); }
};

/** One reward coin, as policy needs to see it. */
struct RewardCoin {
    COutPoint outpoint;
    CAsset asset;
    CAmount amount{0};
    bool mature{false};
    bool spent{false};

    //! Matured, unspent, still ours: eligible to be converted.
    bool Convertible() const { return mature && !spent; }
};

/** Reward coins of one asset, gathered until they are worth converting. */
struct RewardBatch {
    CAsset asset;
    //! The coins this batch accounts for. What is BOUND is the amount, not the
    //! identity of these outputs: an asset is fungible, so any coins of it
    //! satisfy the sale, and the wallet's stake, delegation and payout records
    //! are bare scripts its coin selection cannot spend at all.
    std::vector<COutPoint> inputs;
    CAmount value{0};
};

/** What the book is offering for a batch right now. */
struct RewardQuote {
    //! Target atoms the book would actually deliver for the whole batch,
    //! having walked the levels, net of the swap's own costs.
    CAmount receives{0};
    //! Target atoms the batch is worth at the reference price, ignoring depth.
    CAmount reference{0};

    //! How far below the reference price this fill lands, in basis points.
    //! A fill BETTER than reference is zero, never negative.
    int SlippageBp() const;
};

/** Whether a batch converts now, and if not, why not.
 *
 *  Every "not now" is a WAIT, never an error: the coins stay exactly where they
 *  are and the batch is reconsidered when the next reward in that asset lands,
 *  or when the book changes. */
enum class RewardDecisionKind {
    Convert,
    Disabled,          //!< the setting is off
    NotConverted,      //!< the asset IS the target, or the staker excluded it
    NoMarket,          //!< no market for the pair at all
    TooSmallToPrice,   //!< a market exists, but this batch prices to nothing
    BelowFloor,        //!< the proceeds would not clear the minimum
    SlippageTooHigh,   //!< a market that exists, quoted too far from reference
};

struct RewardDecision {
    RewardDecisionKind kind{RewardDecisionKind::Disabled};
    CAmount receives{0};
    CAmount floor{0};
    int slippage_bp{0};
    int cap_bp{0};

    bool Converts() const { return kind == RewardDecisionKind::Convert; }
    //! A sentence for the RPC and the GUI to print as-is.
    std::string Reason() const;
};

/** Group convertible rewards into one batch per asset, biggest first.
 *
 *  Skips the target itself and anything excluded, anything not yet matured or
 *  already spent, and anything in `already_converted` -- the outpoints a
 *  conversion has already committed to, which is the whole of the idempotence
 *  that stops a restart selling the same reward twice. */
std::vector<RewardBatch> RewardBatches(const std::vector<RewardCoin>& rewards,
                                       const RewardConvertSettings& settings,
                                       const std::set<COutPoint>& already_converted);

/** Whether one batch converts, given what the book offers. `quote` unset means
 *  there is no market for the pair, or none with depth to fill the batch. */
RewardDecision DecideRewardConversion(const RewardBatch& batch,
                                      const std::optional<RewardQuote>& quote,
                                      const RewardConvertSettings& settings);

/** How much of a batch a WHOLE-HTLC offer may take.
 *
 *  The cross-chain rail rests whole offers and the one picked is the smallest
 *  that COVERS the request, which can be far larger than the batch. Taking it
 *  whole would sell coins staking never paid, so the slice is clamped. Selling
 *  LESS is normal: the remainder waits for the next pass. Returns 0 when either
 *  side has nothing to trade, which the caller must read as "no fill" and never
 *  as "take everything". */
CAmount RewardSliceForWholeHtlc(CAmount offer_atoms, CAmount batch_atoms);

} // namespace wallet

#endif // BITCOIN_WALLET_REWARDCONVERT_H
