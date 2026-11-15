// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_LOST_H
#define BITCOIN_POS_LOST_H

// SEQUENTIA: lost blocks (doc/sequentia/04-proof-of-stake.md, "Lost blocks").
//
// A lost block is a block that carries its parent's full certification
// quorum, left the active chain because Bitcoin replaced an anchor on its
// branch, and forked from the active chain at a known block. While a block's
// anchor is at most the window above a lost block's anchor, and the lost
// block's fork point is an ancestor of it, the block is not valid if one of
// its transactions spends an outpoint that a transaction of the lost block
// spends, unless it is that same transaction. The lost block itself (with its
// certificate, and the Bitcoin headers of its stale anchor) is the evidence:
// a node that has it applies the rule, a node that does not cannot, so every
// node relays what it holds to every peer. A refusal is soft: no peer is
// punished, and the block is judged again whenever the evidence or the node's
// view of Bitcoin changes.

#include <primitives/block.h>
#include <serialize.h>
#include <uint256.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class BlockValidationState;
class CBlockIndex;
class ChainstateManager;
class CTransaction;
class CTxMemPool;
class UniValue;

//! The window, in Bitcoin blocks of the anchor: a lost block binds the blocks
//! anchored at most this many Bitcoin blocks above its own anchor.
static constexpr uint32_t DEFAULT_POS_LOST_WINDOW = 6;
//! Evidence is kept (and served) until the active tip anchors this many
//! windows above the lost block's anchor.
static constexpr uint32_t POS_LOST_KEEP_WINDOWS = 2;
//! At most this many blocks are kept as evidence; more is refused.
static constexpr size_t POS_LOST_MAX_BLOCKS = 2000;

//! What one evidence message carries: a block of a lost branch with its
//! certificate (the header's proof), and the Bitcoin headers from the first
//! one off Bitcoin's best chain up to the block's anchor, parents first, so a
//! node whose Bitcoin daemon never saw the stale block can learn it.
struct LostEvidenceMsg {
    CBlock block;
    std::vector<std::vector<unsigned char>> btc_headers;
    SERIALIZE_METHODS(LostEvidenceMsg, obj) { READWRITE(obj.block, obj.btc_headers); }
};

//! Whether this node enforces the rule: a PoS chain anchored to Bitcoin, the
//! anchor validated, and not turned off with the debug-only -posdebugignorelost.
bool LostActive();
uint32_t LostWindow();

//! Read the settings and the stored evidence (<datadir>/lostblocks.dat).
void LostInit();
//! Write the evidence.
void LostShutdown();

//! DisconnectTip took `pindex` off the active chain. cs_main held.
void LostNoteDisconnected(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block);

//! The rule, for `block` built on `pindexPrev`. cs_main held.
bool LostCheckBlock(const CBlock& block, const CBlockIndex* pindexPrev, BlockValidationState& state);

//! A lost transaction `tx` conflicts with, for the mempool, while the next
//! block (anchored at least at `tip_anchor_height`) is still bound.
std::optional<uint256> LostConflict(const CTransaction& tx, uint32_t tip_anchor_height);

//! ConnectTip refused `pindex` under the rule and marked it failed. cs_main held.
void LostNoteRefused(const CBlockIndex* pindex);

//! Evidence received from a peer, judged on the maintenance pass.
void LostQueueEvidence(int64_t peer, LostEvidenceMsg msg);
//! Judge one piece of evidence now. Empty when it was taken, else why not.
//! No lock held.
std::string LostAcceptEvidence(ChainstateManager& chainman, const LostEvidenceMsg& msg);
//! Evidence `peer` has not been sent yet (parents first, a few at a time).
std::vector<LostEvidenceMsg> LostToSend(int64_t peer);
void LostPeerGone(int64_t peer);

//! One maintenance pass (the anchor watcher's thread): take what DisconnectTip
//! left, judge queued evidence and every anchor, rebuild the bound outpoints,
//! and when they change, clean the mempool, put lost transactions back into
//! it, and judge again the active chain and every refused block.
void LostMaintain(ChainstateManager& chainman, CTxMemPool* mempool);

UniValue LostGetInfo();
std::optional<LostEvidenceMsg> LostGetEvidence(const uint256& hash);

#endif // BITCOIN_POS_LOST_H
