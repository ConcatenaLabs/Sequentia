// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: the signing record (possigned.dat).
//
// A committee member's knowledge of what it signed must survive any stop. The
// block index reaches the disk only at a chainstate flush or at the periodic
// write, and signing writes nothing, so a member killed, crashed or cut from
// power shortly after it signed could restart as if it never had: it would then
// sign a rival at a height it already signed, which is how one producer holding
// a quorum certifies two blocks at one height.
//
// So every signature a member's keys make on a block (a gossip share, a
// proposal, a self-certified block, a leader signature, a MuSig2 partial
// signature) is first written here, as one entry, and the entry is synced to
// the disk before the signature leaves the process: before the share is
// flooded, the proposal sent, the block submitted or the RPC answered. A write
// that fails means nothing leaves, and the node shuts down: a member that
// cannot record must not sign.
//
// At a restart the entries rebuild the share-lock (PosProducer): a member does
// not sign a rival at a height it signed while that block's Bitcoin anchor
// stands, and the escape stays the protocol's own (the certificate query and
// the grace). Bitcoin anchoring is supreme: a signed block whose anchor Bitcoin
// replaced never blocks a new block at its height.
//
// The file is append-only, one framed entry per signing event, and is
// rewritten without the dropped entries once they are more than half of it.

#ifndef BITCOIN_POS_SIGNING_RECORD_H
#define BITCOIN_POS_SIGNING_RECORD_H

#include <primitives/transaction.h>
#include <pubkey.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CBlock;

/** What a signing record entry's signature was. */
enum class PosSignKind : uint8_t {
    SHARE = 1,      //!< a committee member's gossip share (BLS) on a proposal
    PROPOSAL = 2,   //!< a leader's signature on its own gossip proposal
    CERTIFIED = 3,  //!< a block this node signed whole and submitted (leader and committee keys)
    LEADER = 4,     //!< a leader's signature on a block whose committee signed elsewhere (submitposblock)
    PARTIAL = 5,    //!< a MuSig2 partial signature (musigpartialsign)
    RECERTIFY = 6,  //!< a new certificate for an existing block (recertifyposblock, test chains only)
};

/** One signing event: what was signed, by which keys, and what the block spends. */
struct PosSigningEntry {
    uint8_t kind{0};
    uint256 block_hash;
    int32_t height{-1};          //!< -1: the block is not known to this node (a partial signature over a bare hash)
    uint256 parent_hash;
    uint32_t anchor_height{0};   //!< the block's Bitcoin anchor; for an unknown block, the parent chain height known when signing
    uint256 anchor_hash;         //!< null when the block or its anchor is unknown
    int64_t time{0};             //!< when it was signed (seconds)
    std::vector<CPubKey> signers;
    //! What the block spends: each non-coinbase input's outpoint and the spending transaction's id.
    std::vector<std::pair<COutPoint, uint256>> spends;

    SERIALIZE_METHODS(PosSigningEntry, obj)
    {
        READWRITE(obj.kind, obj.block_hash, obj.height, obj.parent_hash, obj.anchor_height, obj.anchor_hash,
                  obj.time, obj.signers, obj.spends);
    }

    //! Whether this entry holds its height like the share-lock: a share or a
    //! block this node certified or led. A proposal does not (every member
    //! proposes at every height), nor does a partial signature or a test
    //! recertification.
    bool Locks() const;
};

/** Build an entry for `block` at `height` signed by `signers`. */
PosSigningEntry PosSigningEntryFor(PosSignKind kind, const CBlock& block, int height, std::vector<CPubKey> signers);

/** Load possigned.dat from the data directory. Returns false, with `error`
 *  set, when the file exists and cannot be read or is corrupt before its last
 *  entry (a torn last entry is a write that never completed, and is dropped). */
bool PosSigningRecordLoad(std::string& error);

/** Append `entry` and sync it to the disk. Returns true only once it is
 *  durable; on failure (now or at any earlier write) returns false, and the
 *  node is shutting down (AbortNode, "Failed to write the signing record").
 *  The caller then releases nothing. Takes the record's own lock; call it with
 *  no producer lock held (cs_main may be held). */
bool PosSigningRecordAppend(const PosSigningEntry& entry);

/** The error a signing RPC returns when the record cannot be written. */
extern const char* const POS_SIGNING_RECORD_WRITE_ERROR;

/** A share-lock rebuilt from the record at the start: the latest entry that
 *  locks its height, loaded from the disk (entries appended by this process
 *  are the in-memory share-lock's, PosProducer). */
struct PosRestoredLock {
    uint256 block_hash;
    int height{0};
    uint32_t anchor_height{0};
    uint256 anchor_hash;
};

/** The restored lock at `height`, if any. */
std::optional<PosRestoredLock> PosSigningRecordRestoredLock(int height);

/** Forget the restored lock at `height` (its anchor was replaced, or the
 *  in-memory share-lock now holds the height). */
void PosSigningRecordClearRestoredLock(int height);

/** The block index has just been written (cs_main held): drop every entry whose
 *  block it now carries on the active chain, every entry superseded at its
 *  height by a quorum-certified block the index carries on the active chain,
 *  and every entry more than POS_SIGNING_RECORD_WINDOW Bitcoin blocks (or, with
 *  no anchoring, Sequentia blocks) below the tip; rewrite the file once dropped
 *  entries are more than half of it. */
//! What the active chain holds at a height: its block and whether that block
//! carries a quorum certificate; nullopt above the tip.
using PosActiveAt = std::function<std::optional<std::pair<uint256, bool>>(int height)>;
void PosSigningRecordIndexWritten(int tip_height, uint32_t tip_anchor_height, const PosActiveAt& active_at);

/** How far below the tip an entry is kept at most, in Bitcoin blocks (one day). */
static constexpr uint32_t POS_SIGNING_RECORD_WINDOW = 144;

/** For getsigningrecord and the tests: the entries now in the record. */
std::vector<PosSigningEntry> PosSigningRecordEntries();

/** Statistics: entries, bytes on disk, and the cost of the last and the slowest append (microseconds). */
struct PosSigningRecordStats {
    size_t entries{0};
    uint64_t file_bytes{0};
    uint64_t live_bytes{0};
    uint64_t appends{0};
    int64_t last_append_us{0};
    int64_t max_append_us{0};
    int64_t total_append_us{0};
    uint64_t rewrites{0};
};
PosSigningRecordStats PosSigningRecordGetStats();

/** Unit tests: forget everything in memory (the file is untouched). */
void PosSigningRecordResetForTesting();

#endif // BITCOIN_POS_SIGNING_RECORD_H
