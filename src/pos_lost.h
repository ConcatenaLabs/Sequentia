// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_LOST_H
#define BITCOIN_POS_LOST_H

// SEQUENTIA: lost blocks (doc/sequentia/03-bitcoin-anchoring.md §3,
// "Transactions of a lost block").
//
// Two layers. Records and their bindings are validity: a function of the chain
// being judged and of the Bitcoin ancestry of the judged block's own anchor,
// identical on every node; what a node's Bitcoin daemon cannot settle yet is
// refused softly and retried, never stored as a verdict. The carry-over is a
// duty of the nodes that hold a lost block: they do not produce, sign or take
// as their tip a block that skips it, and they do not resist a block that has
// a descendant whose certificate verifies. Every certificate either layer
// trusts is verified in order from the fork point under the seat quorum, so no
// committee is seeded by a value the record's author chooses. A lost
// transaction stays protected for a candidate until that candidate's chain has
// rebuilt the lost block's height and W Bitcoin blocks have then passed.

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
//! certificate (the header's proof). Whether its anchor is off Bitcoin's best
//! chain is judged from the receiver's own view of that chain's ancestry.
struct LostEvidenceMsg {
    CBlock block;
    SERIALIZE_METHODS(LostEvidenceMsg, obj) { READWRITE(obj.block); }
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
    uint8_t version{2};
    //! The lost branch from the fork point's child, each header with its
    //! certificate, every one verified in order from the fork point.
    std::vector<std::vector<unsigned char>> lost;
    std::vector<LostRecordEntry> entries;
    SERIALIZE_METHODS(LostRecord, obj) { READWRITE(obj.version, obj.lost, obj.entries); }
};
//! The most record bytes one block may carry (a consensus value).
static constexpr size_t LOST_RECORD_MAX_BYTES = 25000;
//! The longest lost branch a record may carry (a consensus value): it bounds
//! how long a record can bind, and the walk that rebuilds bindings.
static constexpr int LOST_MAX_BRANCH = 256;
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
bool LostCheckRecords(const CBlock& block, const CBlockIndex* pindexPrev, bool just_check, BlockValidationState& state,
                      bool* has_record = nullptr);

//! What a producer holding lost blocks puts first: the due lost transactions
//! (a prefix of them when they do not all fit, and then nothing else), and the
//! record outputs for the coinbase. cs_main held.
struct LostTemplateParts {
    std::vector<CTransactionRef> prefix;
    bool cut{false};
    std::vector<CTxOut> records;
};
LostTemplateParts LostForTemplate(const CBlockIndex* pindexPrev, uint32_t anchor_height, const uint256& anchor_hash, CCoinsViewCache& view);

//! Whether a refusal reason is one of the holders' duty (soft, re-judged).
bool LostIsRefusal(const std::string& reason);

//! Whether this node enforces the rule: a PoS chain anchored to Bitcoin, the
//! anchor validated, and not turned off with the debug-only -posdebugignorelost.
bool LostActive();
//! LostActive(), and the chain's lost_blocks_height reached at `height`: whether
//! the rule applies to the block at that height (Consensus::Params).
bool LostActiveAt(int height);
uint32_t LostWindow();
//! Whether a transaction of a lost block at `lost_height`, anchored at
//! `lost_anchor_h`, still binds a block at `height` anchored at `anchor_h` on
//! `prev` (doc 03: until the chain rebuilt `lost_height`, then W Bitcoin blocks).
bool LostStillBinds(int lost_height, uint32_t lost_anchor_h, int height, uint32_t anchor_h, const CBlockIndex* prev);

//! Read the settings and the stored evidence (<datadir>/lostblocks.dat).
void LostInit();
//! Write the evidence.
void LostShutdown();

//! C2 (-poslostbindsigned): this member is about to release its signature on
//! `block` at `height`; held on disk before it does.
void LostNoteSigned(const CBlock& block, int height);

//! DisconnectTip took `pindex` off the active chain. cs_main held.
void LostNoteDisconnected(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block);

//! The rule, for `block` built on `pindexPrev`, the active tip. Off-chain
//! evidence is not applied during initial block download, nor to a block that
//! `certified_descendant` says has a certified descendant. cs_main held.
bool LostCheckBlock(const CBlock& block, const CBlockIndex* pindexPrev, bool ibd,
                    const std::function<bool()>& certified_descendant, CCoinsViewCache& view,
                    BlockValidationState& state);

//! A lost transaction `tx` conflicts with, for the mempool, while a child of
//! `tip` anchored like it is still bound.
std::optional<uint256> LostConflict(const CTransaction& tx, const CBlockIndex* tip);

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

//! getlostblocks. "active" is whether the rule applies to the block at
//! `next_height` (the tip's child), as getdeploymentinfo reports it.
UniValue LostGetInfo(int next_height);
std::optional<LostEvidenceMsg> LostGetEvidence(const uint256& hash);

#endif // BITCOIN_POS_LOST_H
