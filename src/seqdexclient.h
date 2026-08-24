// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SEQDEXCLIENT_H
#define BITCOIN_SEQDEXCLIENT_H

#include <asset.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

#include <univalue.h>

/**
 * SEQUENTIA: the node's client for a SeqDEX order-book relay.
 *
 * The relay stores signed offers, serves the book over REST, and couriers
 * end-to-end-sealed swap messages. It holds no wallet, no keys and no funds,
 * which is what makes talking to one safe: a book it lies about can deny a
 * trade or quote a bad price, but a covenant fill is verified against the coin
 * actually on chain before anything is spent, and the slippage cap bounds the
 * price. Nothing here trusts the relay with money.
 *
 * PLAIN HTTP, deliberately. This node links no TLS library -- Bitcoin Core
 * dropped OpenSSL and neither depends nor the release pipeline has replaced it
 * -- and adding one for a wallet convenience would be a dependency and a
 * reproducible-build change out of all proportion to the feature. So the relay
 * endpoint is configured (`-seqoburl`) and spoken to in the clear, exactly as
 * the node already speaks to its parent-chain daemon. What crosses the wire is
 * a signed public order book and sealed courier payloads; neither is a secret,
 * and neither is trusted.
 */

/** Where the relay is. Empty until -seqoburl is set. */
std::string SeqdexRelayUrl();

/** The configured relay, split up. False when -seqoburl is unset or is not a
 *  plain http:// URL this node can speak. */
bool SeqdexRelayEndpoint(std::string& host, uint16_t& port, std::string& path_prefix);

/** A JSON request to the relay. `method` is "GET" or "POST"; `body` is ignored
 *  for GET. Throws std::runtime_error on a transport or HTTP-status failure,
 *  which every caller must treat as "the book is unreadable right now" rather
 *  than as an answer. */
UniValue SeqdexHttpJson(const std::string& path, const std::string& method,
                        const UniValue& body = UniValue(UniValue::VOBJ));

/** The covenant an offer rests behind: a Taproot output whose script tree binds
 *  the fill to a rate, so anyone can take part of it and nobody can take it
 *  wrongly. Present only on offers that rest passively; an interactive offer
 *  needs its maker online and is not something this node can fill. */
struct SeqobCovenant {
    uint256 txid;
    uint32_t vout{0};
    CAsset asset_a;                        //!< what the covenant pays out
    CAsset asset_b;                        //!< what it must be paid in
    int64_t rate_num{0};
    int64_t rate_den{0};
    int64_t min_lot{0};
    std::vector<unsigned char> maker_prog; //!< 32-byte v1 payout program
    std::vector<unsigned char> maker_x;    //!< 32-byte x-only refund key
    std::vector<unsigned char> internal_key;
    std::vector<std::vector<unsigned char>> merkle_path;
    int64_t expiry_locktime{0};
};

/** One resting offer, as the book reports it.
 *
 *  A CROSS-CHAIN offer names native Bitcoin with the literal string "BTC"
 *  rather than an asset id, because on the parent chain it does not have one.
 *  That is the whole difference between the two rails as the book sees them,
 *  and a parser that insists on 32 bytes everywhere throws away exactly the
 *  offers a staker converting to Bitcoin needs. */
struct SeqobOffer {
    std::string offer_id;
    std::string maker_pubkey;
    CAsset offer_asset;      //!< what the maker gives; null when it gives BTC
    CAsset want_asset;       //!< what the maker wants; null when it wants BTC
    bool offer_is_btc{false};
    bool want_is_btc{false};
    CAmount offer_amount{0};
    CAmount want_amount{0};
    bool allow_partial{false};
    int64_t expires_at_unix{0};
    std::optional<SeqobCovenant> covenant;

    //! Price of this offer as want-per-offer, for ranking. Higher is better for
    //! whoever is PAYING want_asset; callers compare consistently or not at all.
    double PricePerOffer() const {
        return offer_amount > 0 ? (double)want_amount / (double)offer_amount : 0.0;
    }
};

/** Every offer resting on `base/quote`, both orientations merged and deduped.
 *
 *  The relay keys markets by exact base/quote order, so an offer selling A for
 *  B may rest under either heading depending on which maker posted it. Asking
 *  for only one orientation silently misses half the book. */
std::vector<SeqobOffer> SeqobFetchBook(const CAsset& base, const CAsset& quote);

/** Offers that BUY `asset` for native Bitcoin: the maker gives BTC and wants
 *  the asset, which is what a staker selling a reward for Bitcoin takes.
 *
 *  Both orientations again, since a cross offer rests under `<asset>/BTC` or
 *  `BTC/<asset>` depending on which maker posted it. Best price first, where
 *  "best" means the most Bitcoin per unit of the asset. */
std::vector<SeqobOffer> SeqobFetchBtcBids(const CAsset& asset);

/** The offers in one orderbook response. Exposed so the parsing can be tested
 *  against a captured real response rather than only against the live relay:
 *  the field names and their encodings (amounts as decimal strings, covenant
 *  bytes as base64) are the part most likely to drift, and the part a live-only
 *  test would notice last. */
std::vector<SeqobOffer> SeqobParseBook(const UniValue& orderbook_response);

/** What the book would pay to sell `atoms` of `sell` for `want`, having walked
 *  the crossable offers best price first.
 *
 *  Returns nullopt only when NOTHING crosses -- which is not an error: it is the
 *  ordinary state of a young pair, and the caller waits. A book that can fill
 *  part of the amount returns that part, in `sells`. `reference` prices the part
 *  that filled at the BEST offer, so the gap to `receives` is the slippage
 *  walking the book actually costs and not an artefact of a partial fill. */
struct SeqobWalkLeg {
    SeqobOffer offer;
    CAmount pay{0};      //!< atoms of the asset being sold that this leg costs
    CAmount receive{0};  //!< atoms of the target asset it delivers
};
struct SeqobWalk {
    CAmount receives{0};
    CAmount reference{0};
    //! How much of the asset being SOLD this walk actually consumes. A book
    //! that can only partly fill still converts what it can: refusing the whole
    //! sale because the last atom has no bid would leave a staker with a thin
    //! market converting nothing, forever.
    CAmount sells{0};
    //! The offers to take, in order.
    std::vector<SeqobWalkLeg> legs;
};
std::optional<SeqobWalk> SeqobWalkBook(const std::vector<SeqobOffer>& book,
                                       const CAsset& sell, const CAsset& want,
                                       CAmount atoms);

/** ceil(filled * num / den): what a covenant fill of `filled` must pay.
 *
 *  Byte-for-byte the arithmetic the covenant script itself enforces
 *  (`OP_MUL64`, `+den-1`, `OP_DIV64`), because a taker that rounds the other
 *  way builds a transaction consensus rejects. */
CAmount SeqobCovenantPrice(CAmount filled, int64_t rate_num, int64_t rate_den);

#endif // BITCOIN_SEQDEXCLIENT_H
