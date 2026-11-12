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
// ancestor above the disconnect point (arm one), and it sets aside a
// sub-quorum block of its active chain that carries such a spend, so that it
// can sign a sibling in its place (arm two). The mempool refuses such spends
// too. It is signing policy and local state, like the share-lock: no block is
// valid or invalid because of it.

#include <primitives/transaction.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

class CBlock;
class CBlockIndex;
class ChainstateManager;
class CTxMemPool;

//! How long a member keeps a block set aside (arm two) when no certified block
//! has displaced it, before it gives up and only refuses to sign on top of it.
static constexpr int64_t POS_HELD_PARK_MS = 120 * 1000;
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

//! Release what is due, put held transactions back into the mempool, and with
//! `park` (a committee member) set aside the lowest sub-quorum block of the
//! active chain that conflicts, and restore the blocks set aside earlier when
//! due. Returns true when it changed the active chain. No lock held.
bool HeldTxsMaintain(ChainstateManager& chainman, CTxMemPool& mempool, bool park);

struct HeldTxsInfo {
    std::vector<uint256> txids;
    std::vector<uint256> parked;   //!< blocks set aside now
    std::vector<uint256> given_up; //!< blocks set aside once and restored without a displacement
    int fork_height{-1};
    uint32_t release_anchor{0};
};
HeldTxsInfo HeldTxsGetInfo();

#endif // BITCOIN_POS_HELDTXS_H
