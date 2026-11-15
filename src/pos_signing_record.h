// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: the signing record (possigned.dat) and the signing check.
//
// A committee member's knowledge of what it signed must survive any stop. The
// block index reaches the disk only at a chainstate flush or at the periodic
// write, and signing writes nothing, so a member killed, crashed or cut from
// power shortly after it signed could restart as if it never had: it would then
// sign a rival at a height it already signed, which is how one producer holding
// a quorum certifies two blocks at one height.
//
// So every path by which this node's keys sign a block goes through one gate
// (PosSigningGate): the gossip share, the gossip proposal, the leader-only
// producer, generateposblock, submitposblock, musigpartialsign and
// recertifyposblock. The gate first runs the signing check (the share-lock,
// below), then the signature is made, then it is written here as one entry and
// synced to the disk before it leaves the process: before the share is
// flooded, the proposal sent, the block submitted or the RPC answered. A write
// that fails means nothing leaves, and the node shuts down: a member that
// cannot record must not sign. The wallet's signblock does not sign
// proof-of-stake blocks.
//
// The signing check is the share-lock, kept per height and per signed block,
// for the record's entries loaded at the start and those made since alike.
// Before this node signs block B at height H, every other block it signed at H
// (a share, a certified or led block, or a MuSig2 partial signature whose
// block it knows) is judged against Bitcoin:
//  - its anchor is gone from Bitcoin's best chain: it binds nothing (Bitcoin
//    anchoring is supreme), and any escape it had is void;
//  - the parent chain's daemon gives no verdict: B is refused until it does;
//  - its anchor stands: it binds until its escape, which is the protocol's
//    own: the node asks its peers for that block's certificate and waits one
//    round; a certificate seen holds the height, silence frees it. The escape
//    counts only while the anchor stands unbroken: when Bitcoin replaces the
//    anchor and later returns to it, the block binds again and the next rival
//    waits for a new query and a new round.
// The same block again is always free, and blocks signed on two Bitcoin
// branches never refuse each other: that is following Bitcoin, not a fault.
// A block this node holds a verified full certificate for binds its height for
// good while its anchor stands, whether or not this member signed it, unless
// it is invalidated here (then it escapes as above). What binds a height is
// decided in RunCheck from one map fed by two sources: the record's entries
// (this member's signatures) and every verified certificate, which comes from
// the block index (read at the start for the record's window,
// PosSigningNoteIndexAtStart, then noted at each connect) and from the
// certificate message, all through PosSigningNoteCertificate. The record drops
// an entry only once the index on disk carries its block with a verified
// certificate, so no compaction weakens the check.
// All of this is local signing policy, never a validity rule.
//
// The file names its chain (its first entry carries the genesis hash), is
// append-only, one framed entry per signing event, and is rewritten without
// the dropped entries once they are more than half of it.

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
    UNREADABLE = 7, //!< stands for a last entry found whole but unreadable at a start (height: the floor of its hold)
    CHAIN = 8,      //!< the record's first entry: block_hash is the chain's genesis hash
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

    //! Whether this entry holds its height like the share-lock: a share, a
    //! block this node certified, led or recertified, or a MuSig2 partial
    //! signature whose block it knows. A proposal does not (every member
    //! proposes at every height; a proposal certifies only with others'
    //! shares), nor does a partial signature over a bare hash, whose height
    //! is unknown.
    bool Locks() const;
};

/** Build an entry for `block` at `height` signed by `signers`. */
PosSigningEntry PosSigningEntryFor(PosSignKind kind, const CBlock& block, int height, std::vector<CPubKey> signers);

/** Load possigned.dat from the data directory for the chain whose genesis is
 *  `genesis`. Returns false, with `error` set to what to do, when the file
 *  cannot be read, belongs to another chain or names none, was written by a
 *  newer format, or is damaged before its last entry. A last entry cut short
 *  is a write that never completed, and is dropped. A last entry whole in
 *  length but unreadable cannot be told apart from one that rotted after it
 *  was synced: the node starts, keeps a copy of the file beside it, and holds
 *  the next height it would sign at (see PosSigningDamagedHold). */
bool PosSigningRecordLoad(const uint256& genesis, std::string& error);

/** Append `entry` and sync it to the disk. Returns true only once it is
 *  durable; on failure (now or at any earlier write) returns false, and the
 *  node is shutting down (AbortNode, "Failed to write the signing record").
 *  The caller then releases nothing. Signing paths use PosSigningGate, which
 *  calls this. Takes the record's own lock; call it with no producer lock held
 *  (cs_main may be held). */
bool PosSigningRecordAppend(const PosSigningEntry& entry);

/** The error a signing RPC returns when the record cannot be written. */
extern const char* const POS_SIGNING_RECORD_WRITE_ERROR;

/** What the parent chain says of a signed block's Bitcoin anchor. */
enum class PosAnchorStanding {
    STANDS,     //!< on Bitcoin's best chain
    GONE,       //!< replaced: off Bitcoin's best chain (or unknown to a synced daemon)
    UNDECIDED,  //!< no verdict: the daemon does not answer, or has not synced that far
};

/** How the signing check judges anchors, and how it reaches the network.
 *  Set at the start (init) and cleared at shutdown. The judge answers nothing
 *  (nullopt) while the node does not watch Bitcoin (-validateanchor=0); with
 *  no answer on an anchored chain, entries made in this run stand, and entries
 *  loaded from the disk bind nothing: such a node cannot tell a replaced
 *  anchor from a standing one. */
using PosAnchorJudge = std::function<std::optional<PosAnchorStanding>(uint32_t anchor_height, const uint256& anchor_hash)>;
void PosSigningSetAnchorJudge(PosAnchorJudge judge);
void PosSigningSetNetwork(std::function<void(const uint256& block_hash)> query_certificate, std::function<int()> peer_count);

/** The parent chain moved (the anchor watcher): every anchor above
 *  `unchanged_height` may have changed side (a negative value: any anchor),
 *  so a block anchored there loses its escape. */
void PosSigningParentMoved(int unchanged_height);

/** A block this node holds a verified full certificate for. */
struct PosCertifiedBlock {
    uint256 hash;
    int height{-1};
    uint32_t anchor_height{0};
    uint256 anchor_hash;
};

/** How a verified full certificate reached this node. */
enum class PosCertRoute {
    CONNECTED,              //!< its block connected here (ConnectBlock checked the certificate): any block route, RPC or peer
    CERTIFICATE,            //!< a certificate message, verified (PosVerifyCertificate); the block may not be here yet
    UPGRADE,                //!< a fuller certificate adopted for a block that connected here
    INDEX_AT_START,         //!< the block index at the start: connected here once, now off the active chain
    INDEX_AT_START_ACTIVE,  //!< the block index at the start: on the active chain
    INDEX_AT_START_INVALID, //!< the block index at the start: invalid here (invalidateblock, or below such a block)
};
const char* PosCertRouteName(PosCertRoute route);

/** The one function by which every verified full certificate reaches the
 *  signing check, whatever its route: the check then holds the block as
 *  certified, which binds its height for as long as its anchor stands (on a
 *  node that does not watch Bitcoin: while it is on the active chain), unless
 *  it is invalidated here. A certificate that has not been verified (a header,
 *  a body not yet connected) is not passed here: binding on one would let any
 *  leader freeze a height with a fake aggregate. Takes the record's lock;
 *  cs_main may be held. */
void PosSigningNoteCertificate(const PosCertifiedBlock& block, PosCertRoute route);

/** A certified block left this node's active chain (DisconnectTip). */
void PosSigningNoteDisconnected(const uint256& block_hash, int height);

/** The operator invalidated these held blocks on this node (invalidateblock,
 *  the block or an ancestor), or revived them (reconsiderblock). A certified
 *  block binds its height for good, wherever its certificate came from, unless
 *  it was invalidated here: invalidating is a recovery step that must still
 *  work, so the member is then released after one round as for any block, and
 *  the escape says in words that it signs a rival of a certified block. */
void PosSigningNoteInvalidated(const std::vector<uint256>& hashes, bool invalidated);

/** The answer of the signing check. */
struct PosSignCheck {
    bool ok{true};
    uint256 bound;     //!< the block that holds the height, when refused for one
    int height{-1};
    std::string why;   //!< the refusal, in words an RPC can return
};

/** The gate every signing path passes: it serialises check, signature and
 *  record across every path, so two callers cannot both pass the check for
 *  rivals. Construct it before making any signature; sign only if Allowed();
 *  then Record() before the signature leaves. For an entry whose block is not
 *  known (a bare hash, height -1), the caller passes the active tip. */
class PosSigningGate
{
public:
    PosSigningGate(PosSigningEntry entry, int64_t now_ms, int tip_height = -1, const uint256& tip_hash = uint256());
    ~PosSigningGate();
    PosSigningGate(const PosSigningGate&) = delete;
    PosSigningGate& operator=(const PosSigningGate&) = delete;
    bool Allowed() const { return m_check.ok; }
    const PosSignCheck& Check() const { return m_check; }
    PosSigningEntry& Entry() { return m_entry; }
    //! Append the entry (PosSigningRecordAppend); false: release nothing.
    bool Record();

private:
    PosSigningEntry m_entry;
    PosSignCheck m_check;
    bool m_locked{false};
};

/** The signing check alone, with its side effects (a grace started, a query
 *  sent), for a caller that wants to know before it does costly work. The gate
 *  runs it again. */
PosSignCheck PosSigningCheckNow(const PosSigningEntry& entry, int64_t now_ms, int tip_height = -1, const uint256& tip_hash = uint256());

/** A block this node signed at `height`, other than `except`, that binds now
 *  (anchor standing or undecided, not escaped). Starts its grace and its
 *  certificate query when they have not started. For the producer's ancestry
 *  arm. */
struct PosBoundBlock {
    uint256 hash;
    int64_t grace_start_ms{0};
    bool undecided{false};
    bool certified{false};
};
std::optional<PosBoundBlock> PosSigningBoundAt(int height, const uint256& except, int64_t now_ms);

/** Whether this node signed a block at `height` whose anchor has not been
 *  replaced (escaped or not): it then never produces alone there. */
bool PosSigningSignedAt(int height);

/** A gossip collection restarted at `height`: the grace of every block that
 *  binds there starts again now, so that every locked member's grace ends
 *  together inside the re-seeded round. Returns the blocks to query again
 *  (PosSigningSendQueries), since the caller holds its own lock. */
std::vector<uint256> PosSigningRestartGrace(int height, int64_t now_ms);

/** Ask the peers for these blocks' certificates (outside any producer lock). */
void PosSigningSendQueries(const std::vector<uint256>& hashes);

/** The round and window the escape and the damaged-entry hold are measured in
 *  (-posroundms, -poswindowms, or the per-member formula). */
int64_t PosSigningRoundMs();
int64_t PosSigningWindowMs();

/** A last entry found whole but unreadable at the start: it may name a block
 *  whose signature left before the node stopped, and no query can name it. The
 *  node signs nothing at the first height at or above `floor` it is asked to
 *  sign at, until that height is decided by the others (a certificate for it
 *  seen, or a signature asked at a greater height), or until one window and two
 *  rounds have passed there with a peer connected and no certificate. */
struct PosSigningDamagedHold {
    bool active{false};
    int floor{0};
    int height{-1};          //!< the height held, -1 until the node is first asked to sign at or above floor
    int64_t bound_start_ms{0}; //!< when the bound began (a peer connected at that height), 0 if not yet
    int64_t bound_ms{0};
    uint64_t bytes{0};       //!< the unreadable entry's size
    std::string copy;        //!< the copy of the file kept beside the record
};
PosSigningDamagedHold PosSigningGetDamagedHold();

/** The block index has just been written (cs_main held): drop every entry whose
 *  block the index carries with a verified full certificate (the check holds
 *  it as certified, and the scan at the next start finds it in the index
 *  again), and every entry more than POS_SIGNING_RECORD_WINDOW Bitcoin blocks
 *  (or, with no anchoring, Sequentia blocks) below the tip; forget certified
 *  blocks past the same window; rewrite the file once dropped entries are more
 *  than half of it. Nothing else is dropped: an entry is dropped only where
 *  something else answers for it, so compaction never weakens the check. */
//! Whether the block index carries `hash` with a verified full certificate
//! (connected here at least once, carrying its parent's quorum).
using PosIndexHoldsCertificate = std::function<bool(const uint256& hash)>;
void PosSigningRecordIndexWritten(int tip_height, uint32_t tip_anchor_height, const PosIndexHoldsCertificate& index_certifies);

/** How far below the tip an entry is kept at most, in Bitcoin blocks (one day). */
static constexpr uint32_t POS_SIGNING_RECORD_WINDOW = 144;

/** For getsigningrecord and the tests: the entries now in the record. */
std::vector<PosSigningEntry> PosSigningRecordEntries();

/** For getsigningrecord: the genesis hash the record names. */
uint256 PosSigningRecordChain();

/** For getsigningrecord: each block the check holds, with its state at the
 *  last judgement ("binds", "escaped", "anchor gone", "undecided", "certified",
 *  or "not judged" before the first check). */
struct PosSigningHeld {
    uint256 hash;
    int height{0};
    uint32_t anchor_height{0};
    uint256 anchor_hash;
    bool from_disk{false};
    bool signed_here{false};  //!< a record entry of this member's signature holds it
    bool certified{false};    //!< this node holds a verified full certificate for it
    bool on_active{false};
    bool invalidated{false};
    std::string state;
};
std::vector<PosSigningHeld> PosSigningHeldBlocks();

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
