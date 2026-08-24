// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_XCHAINCONVERT_H
#define BITCOIN_WALLET_XCHAINCONVERT_H

#include <asset.h>
#include <consensus/amount.h>
#include <script/script.h>
#include <seqdexclient.h>
#include <uint256.h>

#include <string>
#include <vector>

namespace wallet {
class CWallet;

/**
 * SEQUENTIA: selling a staking reward for native Bitcoin.
 *
 * The maker locks Bitcoin behind a hashlock, the node locks the asset behind
 * the same hashlock, the maker takes the asset by revealing the secret, and the
 * node uses that secret to take the Bitcoin. One secret, two chains, and no
 * moment where one side can walk away with both -- provided the order is kept.
 *
 * THE ORDER IS THE SAFETY, so it is worth stating plainly:
 *
 *  1. Nothing of ours moves until the maker's Bitcoin is locked, confirmed, and
 *     verified against the parent chain by this node itself. Up to that point
 *     every failure is free.
 *  2. Our asset is funded only once this node's own Bitcoin anchor has reached
 *     the height the maker's lock confirmed at. The block that confirms our
 *     funding commits to an anchor, and that number is frozen the moment it
 *     confirms -- so waiting afterwards can never fix it. This is the gate the
 *     maker will apply to us, and satisfying it up front is what makes a
 *     cross-chain swap settle rather than stall.
 *  3. After funding, our asset is recoverable by timelock even if the maker
 *     vanishes, and the Bitcoin is claimable the moment the secret appears on
 *     the Sequentia chain. Both are driven by the resume pass, so a node that
 *     restarts mid-swap picks it up rather than stranding it.
 *
 * A swap that gets past step 2 and then fails is not lost, it is waiting: the
 * refund is a timelock, not a favour.
 */

/** One cross-chain conversion, persisted so a restart can finish or unwind it.
 *
 *  The secrets are here on purpose. A node that forgot the refund key after a
 *  crash would have funded an asset it could never reclaim, which turns a
 *  recoverable interruption into a loss. */
struct XchainSwap {
    //! "negotiating", "btc_locked", "seq_funded", "btc_claimed", "refunded", "failed".
    std::string state;
    int64_t time{0};
    std::string offer_id;
    std::string maker_pubkey;

    CAsset asset;
    CAmount seq_amount{0};
    CAmount btc_amount{0};

    std::vector<unsigned char> hash_h;
    std::vector<unsigned char> maker_seq_claim_pub;
    std::vector<unsigned char> maker_btc_refund_pub;
    std::vector<unsigned char> taker_seq_refund_pub;
    std::vector<unsigned char> taker_btc_claim_pub;
    std::vector<unsigned char> taker_seq_refund_priv;
    std::vector<unsigned char> taker_btc_claim_priv;

    uint32_t seq_locktime{0};
    uint32_t btc_locktime{0};

    //! The maker's Bitcoin lock, as this node found it on the parent chain.
    std::string btc_leg_txid;
    uint32_t btc_leg_vout{0};
    std::vector<unsigned char> btc_leg_script;
    CAmount btc_leg_amount{0};
    int btc_leg_height{0};

    //! Our asset lock.
    std::string seq_fund_txid;
    uint32_t seq_fund_vout{0};
    std::vector<unsigned char> seq_redeem;

    std::vector<unsigned char> preimage;   //!< once the maker reveals it
    std::string btc_claim_txid;
    std::string error;

    bool Terminal() const {
        return state == "btc_claimed" || state == "refunded" || state == "failed";
    }
};

struct XchainOutcome {
    bool ok{false};
    //! True when the asset has been committed, whatever happened after: the
    //! caller must NOT release those coins for another attempt.
    bool committed{false};
    std::string txid;
    CAmount received{0};
    std::string error;
};

/** Sell `slice` atoms of `asset` for native Bitcoin against `offer`.
 *
 *  Runs the whole conversation and both legs. Long: it waits on Bitcoin
 *  confirmations, so it belongs on a background thread and can take the better
 *  part of an hour. Returns with `committed` false only when nothing of ours
 *  was ever spent. */
XchainOutcome RunXchainConversion(CWallet& wallet, const SeqobOffer& offer, CAmount slice);

/** Push every unfinished swap one step further: claim the Bitcoin if the secret
 *  has appeared, refund the asset if the timelock has passed. Idempotent, and
 *  safe to call on a timer -- it is how a node that restarted mid-swap finishes
 *  one. */
void ResumeXchainSwaps(CWallet& wallet);

std::vector<XchainSwap> LoadXchainSwaps(const CWallet& wallet);

//! The payload of a parent-chain JSON-RPC reply.
//!
//! Separated from the call itself so it can be tested: the mistake it exists to
//! prevent -- reading a payload field straight off the reply that wraps it --
//! fails silently, finding nothing rather than erroring, and a silent nothing
//! is exactly what a test has to catch.
UniValue MainChainPayload(const std::string& method, const UniValue& reply);

//! The parent chain's height, or nothing when it cannot be read.
std::optional<int> ParentChainTip();

} // namespace wallet

#endif // BITCOIN_WALLET_XCHAINCONVERT_H
