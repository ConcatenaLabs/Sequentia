// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/rewardconvert.h>

#include <tinyformat.h>

#include <algorithm>
#include <map>

namespace wallet {

int RewardQuote::SlippageBp() const
{
    if (reference <= 0 || receives >= reference) return 0;
    const int64_t shortfall = reference - receives;
    const int64_t bp = (shortfall * 10000) / reference;
    return (int)std::min<int64_t>(bp, 10000);
}

std::string RewardDecision::Reason() const
{
    switch (kind) {
    case RewardDecisionKind::Convert:
        return "converting";
    case RewardDecisionKind::Disabled:
        return "automatic conversion is switched off";
    case RewardDecisionKind::NotConverted:
        return "this asset is the one you convert into, or you chose to keep it";
    case RewardDecisionKind::NoMarket:
        return "no market for this pair right now; these rewards wait, and convert if one appears";
    case RewardDecisionKind::TooSmallToPrice:
        return "there is a market, but this much is worth less than one atom of the target; "
               "these rewards wait until there are more of them";
    case RewardDecisionKind::BelowFloor:
        return strprintf("not yet worth converting: these rewards would fetch %d, below your minimum of %d. "
                         "They wait for the next ones.", receives, floor);
    case RewardDecisionKind::SlippageTooHigh:
        return strprintf("the market is quoting %d.%02d%% off the reference price, past your %d.%02d%% limit; "
                         "these rewards wait for a better one",
                         slippage_bp / 100, slippage_bp % 100, cap_bp / 100, cap_bp % 100);
    }
    return "";
}

std::vector<RewardBatch> RewardBatches(const std::vector<RewardCoin>& rewards,
                                       const RewardConvertSettings& settings,
                                       const std::set<COutPoint>& already_converted)
{
    std::map<CAsset, RewardBatch> by_asset;

    for (const RewardCoin& r : rewards) {
        if (!r.Convertible()) continue;
        if (already_converted.count(r.outpoint)) continue;
        if (settings.IsTargetAsset(r.asset)) continue;
        if (settings.exclude.count(r.asset)) continue;

        RewardBatch& b = by_asset[r.asset];
        b.asset = r.asset;
        b.inputs.push_back(r.outpoint);
        b.value += r.amount;
    }

    std::vector<RewardBatch> out;
    out.reserve(by_asset.size());
    for (auto& e : by_asset) out.push_back(std::move(e.second));

    // Biggest first: if only some batches can be worked through in a pass, the
    // ones that matter most go first.
    std::sort(out.begin(), out.end(), [](const RewardBatch& a, const RewardBatch& b) {
        if (a.value != b.value) return a.value > b.value;
        return a.asset < b.asset;
    });
    return out;
}

RewardDecision DecideRewardConversion(const RewardBatch& batch,
                                      const std::optional<RewardQuote>& quote,
                                      const RewardConvertSettings& settings)
{
    RewardDecision d;
    if (!settings.enabled) {
        d.kind = RewardDecisionKind::Disabled;
        return d;
    }
    if (settings.IsTargetAsset(batch.asset) || settings.exclude.count(batch.asset)) {
        d.kind = RewardDecisionKind::NotConverted;
        return d;
    }
    if (!quote) {
        d.kind = RewardDecisionKind::NoMarket;
        return d;
    }
    if (quote->receives <= 0) {
        // A market that exists and a batch that prices to nothing are different
        // situations, and saying "no market" for the second sends a staker
        // looking for liquidity that is already there. The reference price is
        // what tells them apart: it is only set when there were offers.
        d.kind = quote->reference > 0 ? RewardDecisionKind::TooSmallToPrice
                                      : RewardDecisionKind::NoMarket;
        return d;
    }
    // Slippage before the floor: a batch quoted 40% away should say so, rather
    // than blame a floor it only misses because the price is wrong.
    const int slip = quote->SlippageBp();
    if (slip > settings.max_slippage_bp) {
        d.kind = RewardDecisionKind::SlippageTooHigh;
        d.slippage_bp = slip;
        d.cap_bp = settings.max_slippage_bp;
        return d;
    }
    if (quote->receives < settings.min_receive) {
        d.kind = RewardDecisionKind::BelowFloor;
        d.receives = quote->receives;
        d.floor = settings.min_receive;
        return d;
    }
    d.kind = RewardDecisionKind::Convert;
    d.receives = quote->receives;
    return d;
}

CAmount RewardSliceForWholeHtlc(CAmount offer_atoms, CAmount batch_atoms)
{
    if (offer_atoms <= 0 || batch_atoms <= 0) return 0;
    return std::min(offer_atoms, batch_atoms);
}

} // namespace wallet
