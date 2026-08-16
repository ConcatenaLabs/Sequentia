// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FEEASSETS_H
#define BITCOIN_FEEASSETS_H

#include <asset.h>
#include <assetsdir.h>
#include <consensus/amount.h>

#include <string>
#include <vector>

class CScheduler;
class CTxMemPool;

/**
 * SEQUENTIA: what a wallet needs in order to judge a candidate fee asset.
 *
 * Three independent facts decide whether paying a fee in some asset is a good
 * idea, and they come from three different places, which is why they used to be
 * confused for one another:
 *
 *  - the FEE WHITELIST (exchangerates.h) says whether THIS node accepts the
 *    asset at all. An asset absent from it, or listed at rate 0, is refused by
 *    this node's own mempool: a transaction paying its fee in that asset is not
 *    merely unlikely to confirm, it never leaves the wallet's node.
 *  - the ASSET REGISTRY (assetregistry.h) says whether the asset is published
 *    to the network. Price servers discover their asset universe from it, so an
 *    unlisted asset is one other producers' whitelists are unlikely to contain
 *    even when this node's does.
 *  - the REFERENCE PRICE FEED (referenceprices.h) says whether there is a market
 *    price for it. It is display-only and settles nothing about acceptance, but
 *    an asset nobody quotes is one nobody can value.
 *
 * Only the first is decisive. Reading either of the other two as if it were —
 * warning about a fee asset because the display feed has no price for it, say —
 * both misses the case that actually fails and cries wolf on the case that
 * works, so the three stay separate fields here and are never collapsed.
 */
struct FeeAssetInfo {
    CAsset asset;
    //! The asset's label, or its hex id when it has no label.
    std::string identifier;
    //! Present in this node's fee whitelist, whatever the rate.
    bool listed{false};
    //! Present AND priced above zero: this node will actually value a fee in it.
    //! `listed` without `accepted` is an explicit refusal (rate 0), which is a
    //! deliberate operator statement rather than an omission.
    bool accepted{false};
    //! Atoms of the asset equal to one reference fee atom. 0 when not listed.
    CAmount rate{0};
    //! Published by the Asset Registry this node reads.
    bool registry_listed{false};
    //! The display price feed quotes this asset above zero.
    bool has_market_price{false};
    //! That price, in the feed's base unit (USD today). 0 when unquoted.
    double market_price{0.0};
    //! Decimal places, for formatting amounts.
    uint8_t precision{DEFAULT_ASSET_PRECISION};
};

/** The reference price feed's key for an asset. The feed names the native asset
 *  "SEQ" whatever the chain-aware display ticker is (tSEQ on testnet); every
 *  other asset is keyed by its registry ticker, upper-cased. */
std::string FeeAssetFeedTicker(const CAsset& asset);

/** Assemble the three facts above for one asset. Cheap: no I/O, no chain
 *  access — the whitelist, the asset directory and the price cache are all
 *  in-memory. Safe to call per keystroke from the GUI. */
FeeAssetInfo GetFeeAssetInfo(const CAsset& asset);

/** The same for every asset worth reporting: the union of this node's fee
 *  whitelist and the assets its directory knows (registry entries, operator
 *  -assetdir entries, the native asset). The union rather than the whitelist
 *  alone because the interesting answer is often about an asset that is NOT
 *  whitelisted. Sorted by identifier. */
std::vector<FeeAssetInfo> GetAllFeeAssetInfo();

/** SEQUENTIA: price every asset the reference feed quotes into the fee whitelist,
 *  so that a fee can be paid in any of them and not only in the one the whitelist
 *  is seeded with. Leaves rates an operator set alone.
 *  @return how many rates changed. */
int ApplyFeedDerivedFeeRates();

/** Schedule the above shortly after startup and on every price poll. No-op when
 *  no price feed is configured, which leaves the whitelist entirely to the
 *  operator and any price-server sidecar, as before. */
void StartFeedDerivedFeeRates(CScheduler& scheduler, CTxMemPool* mempool);

#endif // BITCOIN_FEEASSETS_H
