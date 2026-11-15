// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_LOST_H
#define BITCOIN_POS_LOST_H

// SEQUENTIA: lost blocks (doc/sequentia/03-bitcoin-anchoring.md §3,
// "Transactions of a lost block").
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
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class BlockValidationState;
class CBlockIndex;
class CCoinsViewCache;
class CTxOut;
class ChainstateManager;
class CTransaction;
class CTxMemPool;
class UniValue;

//! The window, in Bitcoin blocks of the anchor: a lost block binds the blocks
//! anchored at most this many Bitcoin blocks above its own anchor.
static constexpr uint32_t DEFAULT_POS_LOST_WINDOW = 6;
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

//! A record of lost transactions, carried in a coinbase output
//! `OP_RETURN 'SQLR' <record>` (doc 03, "Transactions of a lost block").
struct LostRecordEntry {
    uint32_t k{0};                    //!< index into `lost`
    uint32_t pos{0};                  //!< position in that block
    std::vector<uint256> branch;      //!< merkle branch to its merkle root
    std::vector<unsigned char> tx;    //!< the transaction without its witness
    SERIALIZE_METHODS(LostRecordEntry, obj) { READWRITE(obj.k, obj.pos, obj.branch, obj.tx); }
};
struct LostRecord {
    uint8_t version{1};
    //! Bitcoin headers (80 bytes each), parents first, that make a lost
    //! block's anchor known to a daemon that never saw it.
    std::vector<std::vector<unsigned char>> btc_headers;
    //! The lost branch from the fork point's child, each header with its
    //! certificate.
    std::vector<std::vector<unsigned char>> lost;
    std::vector<LostRecordEntry> entries;
    SERIALIZE_METHODS(LostRecord, obj) { READWRITE(obj.version, obj.btc_headers, obj.lost, obj.entries); }
};
//! The most record bytes one block may carry.
static constexpr size_t LOST_RECORD_MAX_BYTES = 25000;
//! The due prefix's floor (doc 03, "The duty of a holder"): the weight held
//! back for the header and the coinbase, one coinbase fee output at its
//! largest payee script per fee asset of the prefix, and the signature
//! operations held back for the coinbase.
static constexpr int64_t LOST_RESERVED_WEIGHT = 8000;
static constexpr int64_t LOST_FEE_OUTPUT_WEIGHT = 4 * 160;
static constexpr int64_t LOST_RESERVED_SIGOPS = 4000;

//! The records a block's coinbase carries (malformed ones as nullopt).
std::vector<std::optional<LostRecord>> LostParseRecords(const CBlock& block);

//! Chain-alone validity, for every node: the records `block` carries, and
//! the bindings of the records in its chain (a later block anchored at most
//! W above a recorded transaction's lost block spends that transaction's
//! inputs only through it). cs_main held; `pindexPrev` is the active tip.
bool LostCheckRecords(const CBlock& block, const CBlockIndex* pindexPrev, bool just_check, BlockValidationState& state);

//! What a producer holding lost blocks puts first: the due lost transactions
//! (a prefix of them when they do not all fit, and then nothing else), and the
//! record outputs for the coinbase. cs_main held.
struct LostTemplateParts {
    std::vector<CTransactionRef> prefix;
    bool cut{false};
    std::vector<CTxOut> records;
};
LostTemplateParts LostForTemplate(const CBlockIndex* pindexPrev, uint32_t anchor_height, CCoinsViewCache& view);

//! Whether a refusal reason is one of the holders' duty (soft, re-judged).
bool LostIsRefusal(const std::string& reason);

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

//! The rule, for `block` built on `pindexPrev`, the active tip. Off-chain
//! evidence is not applied during initial block download, nor to a block that
//! `certified_descendant` says has a certified descendant. cs_main held.
bool LostCheckBlock(const CBlock& block, const CBlockIndex* pindexPrev, bool ibd,
                    const std::function<bool()>& certified_descendant, CCoinsViewCache& view,
                    BlockValidationState& state);

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
