// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: autonomous Proof-of-Stake block producer.
//
// The committee-certification cryptography and the produce-one-block logic are
// driven today by the generateposblock / getposblocktemplate+submitposblock
// RPCs (an external coordinator calls them each slot). This module adds the
// missing piece for a coordinator-free node: a background thread that, when the
// operator supplies one or more staking keys (-posproducerkey), detects its own
// eligibility each round, waits out the slot clock, and assembles, signs, and
// submits a block on its own — Phase 1 of the autonomous gossip-and-sign
// committee (doc/sequentia/proposals/autonomous-committee.md §12). It covers the
// leader-only / single-host cases; the peer-to-peer committee gossip and BLS
// aggregation are later phases.
//
// ProducePosBlock() is the shared produce-one-block core, factored out of
// generateposblock so the RPC and the thread run identical signing logic.

#ifndef BITCOIN_POS_PRODUCER_H
#define BITCOIN_POS_PRODUCER_H

#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>
#include <validationinterface.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CBlock;
class CChainParams;
class CConnman;
class CTxMemPool;
class ChainstateManager;

/** What a peer should do with a gossip message after the local producer has
 *  examined it (the producer is the only component with the round/registry
 *  context to judge a committee message). */
enum class PosGossipAction {
    Ignore,   //!< duplicate, off-tip, stale, or an equivocating duplicate — drop, no relay, no penalty
    Relay,    //!< new and valid — relay to our other peers
    Invalid,  //!< malformed, forged eligibility, or bad signature — drop and penalise the sender
};

/** SEQUENTIA: one committee member's contribution to a proposed block, gossiped
 *  as a `posshare`: its BLS signature share over the proposal's (member-
 *  independent) block hash, plus everything the leader needs to put the member
 *  into the certificate — its staking key, sortition-eligibility VRF proof, BLS
 *  public key, and BLS proof-of-possession. */
struct PosShare {
    uint256 block_hash;
    CPubKey pubkey;
    std::vector<unsigned char> vrf_proof;
    std::vector<unsigned char> bls_pubkey;
    std::vector<unsigned char> bls_pop;
    std::vector<unsigned char> bls_share;

    SERIALIZE_METHODS(PosShare, obj)
    {
        READWRITE(obj.block_hash, obj.pubkey, obj.vrf_proof, obj.bls_pubkey, obj.bls_pop, obj.bls_share);
    }
};

/** SEQUENTIA: the bandwidth-efficient form of a `posproposal` (BIP152-style). It
 *  carries the block header (which includes the proof, so the leader's staging
 *  signature rides along) and the coinbase in full — both unique to the proposer —
 *  plus the *ids* of the remaining transactions, which the receiver looks up in
 *  its mempool to rebuild the block. The header's merkle root verifies the
 *  reconstruction; a miss is repaired by fetching the full block (getposproposal).*/
struct PosCompactProposal {
    CBlockHeader header;
    CTransactionRef coinbase;
    std::vector<uint256> txids; //!< non-coinbase transaction ids, in block order

    SERIALIZE_METHODS(PosCompactProposal, obj)
    {
        READWRITE(obj.header, obj.coinbase, obj.txids);
    }
};

/** Build the compact form of an assembled/unsigned proposal block. */
PosCompactProposal MakePosCompactProposal(const CBlock& block);

/** Rebuild the full block from a compact proposal using `mempool`. Returns the
 *  block only if every referenced transaction is present AND the reconstructed
 *  merkle root matches the header; otherwise nullptr (the caller fetches the full
 *  block). */
std::shared_ptr<CBlock> ReconstructPosProposal(const PosCompactProposal& compact, CTxMemPool& mempool);

/** The outcome of producing one PoS block, for RPC result formatting. */
struct PosProduceResult {
    uint256 hash;
    int height{0};
    size_t rank{0};
    int countersignatures{0};
    bool vrf{false};
    uint256 vrf_output;
    uint64_t vrf_slot{0};
};

/** Failure category for ProducePosBlock, so an RPC caller can map it to the
 *  matching JSON-RPC error code while the node-layer helper stays free of any
 *  RPC dependency. */
enum class PosProduceError {
    NONE,        //!< success
    NOT_STAKER,  //!< the key is not a registered staker for this slot
    INVALID_KEY, //!< a supplied key is invalid
    BAD_PARAM,   //!< a caller parameter is out of range (e.g. too many keys)
    MISC,        //!< a recoverable failure (signing, sub-quorum committee, ...)
    INTERNAL,    //!< an internal error (template assembly, block not accepted)
};

/** Assemble, sign, and submit one PoS block extending the active tip, with
 *  `leader_key` as the elected proposer and `committee_keys` as any additional
 *  committee signing keys held locally (empty for leader-only / committee=1).
 *
 *  This is the shared core of generateposblock and the autonomous producer: it
 *  computes the slot seed, the leader's election rank/VRF sortition, the
 *  committee eligibility proofs, the block template, the leader (and committee)
 *  signatures, verifies the proof, and calls ProcessNewBlock. It does NOT wait
 *  for a slot to open — the caller is responsible for slot timing — but the
 *  block's own nTime is advanced to the proposer's slot so it is consensus-valid.
 *
 *  Returns true with `result` filled on success; false with a human-readable
 *  `error` and an `err_kind` category otherwise. */
bool ProducePosBlock(ChainstateManager& chainman, CTxMemPool& mempool,
                     const CChainParams& chainparams, const CKey& leader_key,
                     const std::vector<CKey>& committee_keys,
                     PosProduceResult& result, std::string& error,
                     PosProduceError& err_kind);

/** Background thread that produces PoS blocks autonomously from a set of staking
 *  keys, with no external coordinator. Reacts to new tips via the validation
 *  interface and runs the local slot clock; on each round it elects the
 *  best-ranked of its keys as leader and produces a block once that leader's
 *  slot opens (never faster than one slot interval since the parent — the
 *  paper's soft lower-bound cadence floor). */
class PosProducer final : public CValidationInterface
{
public:
    PosProducer(ChainstateManager& chainman, CTxMemPool& mempool,
                const CChainParams& chainparams, CConnman* connman, std::vector<CKey> keys);
    ~PosProducer();

    /** Register for tip notifications and start the worker thread. */
    void Start();
    /** Unregister, stop the worker thread, and join it. Idempotent. */
    void Stop();

    // --- Gossip committee (called from net_processing on the message thread) ---
    //
    //! Ingest a peer's `posproposal` (an elected leader's unsigned block). Records
    //! it into the current round if new and valid. Returns the relay/penalty action.
    PosGossipAction OnProposal(const std::shared_ptr<const CBlock>& block);
    //! Ingest a peer's `posshare`. Collects it if it matches a block we proposed
    //! and the signer is sortition-eligible. Returns the relay/penalty action.
    PosGossipAction OnShare(const PosShare& share);
    //! Return the full proposal block we hold for `hash` (a round candidate), to
    //! answer a peer's getposproposal, or nullptr.
    std::shared_ptr<const CBlock> GetProposalBlock(const uint256& hash);

protected:
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork,
                         bool fInitialDownload) override;

private:
    void ThreadLoop();
    //! Evaluate eligibility for the current tip and, if a slot is due, produce a
    //! block. Returns how long to sleep (ms) before the next evaluation.
    int64_t Step();

    //! Leader path for a distributed BLS committee: build the unsigned block,
    //! sign it, and flood it as a proposal. Every eligible member proposes, so
    //! the candidate set (and thus the round-robin leader order) is complete and
    //! common to all nodes.
    void ProposeGossip(const CKey& leader_key);
    //! Drive the active gossip round on the worker thread: sign the round's
    //! backed proposal (the round-robin leader), collect/flood shares, and — on a
    //! quorum — assemble and submit. The round index advances with time so a
    //! failed (equivocating/withholding) leader is deterministically excluded and
    //! the committee converges on the next-lowest-VRF leader (paper P6 §9).
    //! Returns the poll interval (short while a round is in progress).
    int64_t DriveRound();
    //! Record a proposal as a round candidate (resetting on a new height). The
    //! first block seen from a given leader wins, so an equivocating leader's
    //! later blocks are dropped. m_gossip_mutex held.
    void RecordCandidate(const std::shared_ptr<const CBlock>& block, const CPubKey& leader, const uint256& leader_beta, int height);
    //! The proposal backed in round `r`: the (r+1)-th lowest-leader-VRF candidate,
    //! or nullptr if fewer than r+1 candidates are known. m_gossip_mutex held.
    std::shared_ptr<const CBlock> BackedForRound(int r) const;
    //! Produce shares for every locally-held key that is sortition-eligible for
    //! `block`'s slot.
    std::vector<PosShare> MakeLocalShares(const CBlock& block);
    void FloodProposal(const CBlock& block);
    //! Flood the compact form of a proposal (the normal path; the full block goes
    //! only point-to-point as a getposproposal reply).
    void FloodCompactProposal(const CBlock& block);
    //! Send `a` to half our peers and `b` to the other half — used only by the
    //! regtest equivocation fault-injection to split the committee.
    void FloodProposalSplit(const CBlock& a, const CBlock& b);
    void FloodShare(const PosShare& share);
    //! Wake the worker thread (e.g. when a gossip message advances a round).
    void Wake();

    ChainstateManager& m_chainman;
    CTxMemPool& m_mempool;
    const CChainParams& m_chainparams;
    CConnman* const m_connman;
    const std::vector<CKey> m_keys;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop{false};
    bool m_wake{false};
    bool m_running{false};
    //! Regtest fault injection (-posbyzantineequivocate): when leading, propose
    //! two different blocks to disjoint halves of the committee and contribute no
    //! shares, to test that the round-robin routes around a Byzantine leader.
    bool m_byzantine_equivocate{false};

    //! One round candidate: a validated proposal and its leader's VRF.
    struct RoundCandidate {
        std::shared_ptr<const CBlock> block;
        uint256 beta;
    };

    // Gossip round state (one round per height). Every eligible member proposes,
    // so all nodes share the candidate set; the round-robin leader for round r is
    // the r-th lowest VRF, advanced by a time-derived index so all nodes exclude
    // the same failed leaders in lockstep. The leader ships its block signature
    // in the proposal, so any node that gathers a quorum assembles.
    std::mutex m_gossip_mutex;
    int m_round_height{0};                             //!< height we are running rounds for
    int64_t m_round_start_ms{0};                       //!< when this height's collection started
    std::map<CPubKey, RoundCandidate> m_candidates;    //!< leader -> its validated proposal this round
    std::set<CPubKey> m_equivocators;                  //!< leaders seen to propose two blocks this height (excluded)
    std::map<CPubKey, PosShare> m_collected;           //!< shares for the currently-backed proposal
    uint256 m_backed_hash;                             //!< hash of the proposal we are signing/collecting for
    int m_signed_round{-1};                            //!< highest round index we have signed for
    int m_proposed_height{0};                          //!< height we have already proposed our own block at
    std::set<uint256> m_seen_proposals;                //!< proposal dedup
    std::set<std::pair<uint256, CPubKey>> m_seen_shares; //!< share dedup
};

/** The running producer (for net_processing to deliver gossip to), or nullptr. */
PosProducer* GetActivePosProducer();

#endif // BITCOIN_POS_PRODUCER_H
