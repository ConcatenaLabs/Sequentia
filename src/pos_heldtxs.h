// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_HELDTXS_H
#define BITCOIN_POS_HELDTXS_H

// SEQUENTIA: the committee keeps certified transactions
// (doc/sequentia/04-proof-of-stake.md, "Held transactions").
//
// When the anchor watcher disconnects certified blocks because Bitcoin took
// their anchor away, this node keeps their transactions ("held"). A committee
// member then does not sign a block that spends an outpoint a held transaction
// spends, other than with that same transaction, in the block itself or in an
// ancestor above the disconnect point (arm one), and it keeps a sub-quorum
// block of its active chain that carries such a spend out of its fork choice
// for a while, so that it can sign a sibling in its place (arm two). The
// mempool refuses such spends too. A producer that holds mines held
// transactions from the record, whatever their fee, first in its block, up to
// POS_HELD_BLOCK_SHARE_PCT of it. Every hold ends within a bounded time.
// It is signing policy and local state, like the share-lock: no block is valid
// or invalid because of it, and nothing about a block is written to disk.

#include <primitives/transaction.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

class CBlock;
class CBlockIndex;
class ChainstateManager;
class CTxMemPool;

//! The liveness bound, in gossip rounds: once a member that holds (declined to
//! sign for a held transaction, or keeps a block set aside) has an honest
//! proposal to sign or to make, the chain may go this many rounds plus one
//! collection window without a new certified block.
//! Then the member releases everything it holds and signs as if it held
//! nothing, so the rule never pauses the chain longer than that.
static constexpr int POS_HELD_BOUND_ROUNDS = 12;
//! The backstop, as a multiple of the bound: whatever the member does or sees,
//! a hold (its first decline or set-aside) ends this many bounds after it
//! began. It is what limits how long a set-aside can keep a member off a branch.
static constexpr int POS_HELD_BACKSTOP_BOUNDS = 2;
//! The share of a block's weight (and of its signature operations) that a
//! producer gives to held transactions mined from the record, oldest first.
static constexpr int POS_HELD_BLOCK_SHARE_PCT = 50;
//! A held transaction is released at the latest once the active tip anchors
//! this many Bitcoin blocks above the highest anchor the disconnected blocks had.
static constexpr uint32_t POS_HELD_WINDOW = 144;

//! Whether the rule runs here: a Bitcoin-anchored PoS chain, not turned off
//! with the debug-only -posdebugignoreheld. Fixed by HeldTxsInit.
bool HeldTxsActive();

//! Read the switch and the record (<datadir>/heldtxs.dat). Called once at
//! startup, before the producer and the anchor watcher start.
void HeldTxsInit();

//! The anchor watcher disconnected `disconnected` (the old active chain from
//! the invalidated block up to the old tip, lowest first). Holds every
//! transaction of every block at or below the highest of them that carried its
//! parent's quorum. Called after the disconnect, never on its path.
void HeldTxsRecordDisconnect(ChainstateManager& chainman, const std::vector<const CBlockIndex*>& disconnected);

//! The anchor watcher is disconnecting blocks whose transactions the record
//! has not taken yet: a member signs nothing meanwhile (the window between the
//! disconnect and the fill, which follows it).
void HeldTxsSetFilling(bool filling);
bool HeldTxsFilling();

//! The held transaction `tx` conflicts with: one that spends an outpoint `tx`
//! spends, when `tx` is not that transaction.
std::optional<uint256> HeldTxsConflict(const CTransaction& tx);

//! Why a member must not sign `block` built on `prev`: one of its own
//! transactions, or of prev's and its ancestors' above the disconnect point,
//! conflicts with a held transaction. nullopt when it may sign. cs_main held.
std::optional<std::string> HeldTxsBlockConflict(const CBlock& block, const CBlockIndex* prev);

//! The liveness bound in ms: POS_HELD_BOUND_ROUNDS gossip rounds plus one
//! collection window (PosGossipRoundMs, PosGossipWindowMs). Fixed at startup.
int64_t HeldTxsBoundMs();

//! Release what is due (mined under a certified block, respent by a certified
//! block, invalid for good on the active chain, the liveness bound and the
//! backstop, the window), put held transactions back into the mempool for
//! relay, and with `park` (a committee member running the producer) set aside
//! the lowest sub-quorum block of the active chain that conflicts and end
//! set-asides that are due. The producer calls it at every pass and the anchor
//! watcher at every tick, so both clocks apply on every member, whichever way
//! it signs. Returns true when it changed the active chain. No lock held.
bool HeldTxsMaintain(ChainstateManager& chainman, CTxMemPool& mempool, bool park);

//! The held transactions, oldest first (parents before children), for a
//! producer to mine from the record (node::BlockAssembler).
std::vector<CTransactionRef> HeldTxsForMining();

//! `pindex` was just judged certified or not (validation.cpp PosSetCertified);
//! `newly` when this judgement is the one that found it to carry its parent's
//! quorum, which happens once per block and not again when it is re-connected.
//! Only such a block, judged after a hold began, ends the hold. A certified
//! block that builds on a set-aside block ends that set-aside at the next
//! maintenance pass. cs_main held.
void HeldTxsNoteCertified(const CBlockIndex* pindex, bool newly);

//! A member declined to sign or to propose for a held transaction.
void HeldTxsNoteDecline();
//! A member signs or makes a proposal that respends nothing held. If it is
//! holding (it declined something, or keeps a block set aside), the liveness
//! bound runs from now unless it already runs: waiting for an honest proposal
//! to exist is a pause an attacker that withholds its own causes anyway.
void HeldTxsNoteHold();

//! The set-aside block `pindex` is or descends from, if any (fork choice
//! skips such candidates). Memory only: nothing is marked failed, and a
//! restart forgets every set-aside. cs_main held.
const CBlockIndex* HeldTxsSetAsideAncestor(const CBlockIndex* pindex);

//! Release one held transaction (a caller that knows it can never be mined
//! again, such as a payment claim whose payment is gone for good).
void HeldTxsRelease(const uint256& txid, const std::string& why);

struct HeldTxsInfo {
    std::vector<uint256> txids;
    std::vector<uint256> parked;   //!< blocks set aside now (memory only)
    std::vector<uint256> given_up; //!< blocks set aside once and not to be set aside again (memory only)
    int64_t hold_ms{-1};           //!< how long the liveness bound has run, -1 when it does not
    int64_t held_for_ms{-1};       //!< how long the current hold has lasted (the backstop's clock), -1 when none
    int fork_height{-1};
    uint32_t release_anchor{0};
};
HeldTxsInfo HeldTxsGetInfo();

#endif // BITCOIN_POS_HELDTXS_H
