// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: Proof-of-Stake leader election (proof of concept).
//
// Replaces Elements' fixed-federation signed-block challenge with a per-block
// challenge whose required signer is a stake-weighted leader, elected
// deterministically from a seed derived from the previous block and its Bitcoin
// anchor (see doc/sequentia/06-proof-of-stake.md). The block signature itself
// still rides the existing signed-block machinery (CheckProof), so PoS only
// changes *which* key must sign each block, not how the signature is verified.

#ifndef BITCOIN_POS_H
#define BITCOIN_POS_H

#include <pubkey.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

class CBlockIndex;

/** When set, the chain uses Proof-of-Stake leader election for block validity
 *  (layered on g_signed_blocks). */
extern bool g_con_pos;

/** Seconds per leader rank: the rank-r leader of a slot may only produce a
 *  block once this many seconds * r have elapsed since the parent block. Also
 *  the chain's nominal block time (one slot = one rank step), used to convert
 *  block-count locks to wall-clock (see PosRequiredUnbondingSeconds). Sequentia
 *  targets 30 s blocks; with nMaxBlockWeight = 200,000 the saturated chain grows
 *  at exactly Bitcoin's total rate (200,000 / 30 s == 4,000,000 / 600 s). */
extern int64_t g_pos_slot_interval;
static const int64_t DEFAULT_POS_SLOT_INTERVAL = 30;

/** Number of committee members that must countersign each block (the paper's
 *  blocksigners, principle 6). The committee is the first
 *  g_pos_committee_size entries of the slot's leader schedule, and a block is
 *  certified once a majority of the committee has signed it. 1 disables
 *  committee certification (leader-only signing). Bounded at 16 by the script
 *  smallint encoding; the paper's 100-member committee needs signature
 *  aggregation (future work). */
extern int g_pos_committee_size;
static const int DEFAULT_POS_COMMITTEE_SIZE = 1;
static const int MAX_POS_COMMITTEE_SIZE = 16;

/** When set (with g_con_pos), leader election uses *private* VRF sortition
 *  (doc/sequentia/07-vrf.md §4) instead of the public deterministic schedule:
 *  each block carries the leader's VRF proof over the slot seed in a tagged
 *  coinbase OP_RETURN, and the proof's output determines the leader's
 *  time-gated slot. Only the key holder can compute its slot in advance. */
extern bool g_pos_vrf;

/** When set (with g_pos_vrf), committee certification uses MuSig2 signature
 *  aggregation (doc/sequentia/07-vrf.md §6) instead of script multisig: the
 *  block challenge commits to the leader key plus one 32-byte aggregate of
 *  the committee member set (named by the coinbase SEQCMT commitments), and
 *  the block carries a single 64-byte BIP340 signature by all named members
 *  regardless of committee size — lifting the 16-member script-multisig cap
 *  to the paper's 100. */
extern bool g_pos_agg_committee;

/** Committee-size cap under MuSig2 aggregation (-posaggcommittee): the
 *  paper's 100-member committee. Without aggregation the cap is
 *  MAX_POS_COMMITTEE_SIZE (script multisig). */
static const int MAX_POS_AGG_COMMITTEE_SIZE = 100;

/** Upper bound on a VRF sortition slot, capping the time gate
 *  (slot * g_pos_slot_interval seconds after the parent block) regardless of
 *  how stake weights are scaled. */
static const uint64_t POS_VRF_MAX_SLOT = 1 << 20;

/** The set of stakers eligible to be elected as block leaders, and their
 *  stake weights. Two layers:
 *   - a *config* layer from chain configuration (-staker=<pubkeyhex>:<weight>),
 *     the bootstrap/genesis stake set; and
 *   - a *UTXO* layer derived from the chainstate: every unspent staking output
 *     (see BuildStakeScript) adds its amount (in policy-asset atoms) to its
 *     staker's weight. Rebuilt from the UTXO set at startup and mirrored
 *     incrementally on every tip connect/disconnect, so it is reorg-safe and a
 *     pure function of the active chain. Unbonding is the CSV-gated spend of
 *     the staking output, enforced by the script itself (the paper's stake
 *     locktime, principle 11).
 *  A staker's effective weight is the sum of both layers. */
class StakeRegistry
{
private:
    mutable Mutex m_mutex;
    std::map<CPubKey, uint64_t> m_config GUARDED_BY(m_mutex);
    std::map<CPubKey, uint64_t> m_utxo GUARDED_BY(m_mutex);

public:
    static StakeRegistry& GetInstance()
    {
        static StakeRegistry instance;
        return instance;
    }

    //! Clear the configuration layer (chain parameter (re)load).
    void Clear()
    {
        LOCK(m_mutex);
        m_config.clear();
        m_utxo.clear();
    }
    //! Set a configured staker's weight.
    void SetStake(const CPubKey& pubkey, uint64_t weight)
    {
        LOCK(m_mutex);
        m_config[pubkey] = weight;
    }
    //! Replace the UTXO-derived layer wholesale (startup rebuild).
    void SetUtxoStake(std::map<CPubKey, uint64_t>&& utxo)
    {
        LOCK(m_mutex);
        m_utxo = std::move(utxo);
    }
    //! A staking output entered the UTXO set.
    void AddUtxoStake(const CPubKey& pubkey, uint64_t amount)
    {
        LOCK(m_mutex);
        m_utxo[pubkey] += amount;
    }
    //! A staking output left the UTXO set (spent, or its creation reverted).
    void SubUtxoStake(const CPubKey& pubkey, uint64_t amount)
    {
        LOCK(m_mutex);
        auto it = m_utxo.find(pubkey);
        if (it == m_utxo.end() || it->second < amount) {
            // Should be unreachable: connect/disconnect are exact inverses
            // (PosApplyBlockStake / PosRevertBlockStake) and the startup
            // rebuild is a pure function of the UTXO set. If it ever happens
            // the registry is already inconsistent with consensus; leave the
            // stored weight untouched rather than silently discarding it.
            return;
        }
        it->second -= amount;
        if (it->second == 0) m_utxo.erase(it);
    }

    bool Empty() const
    {
        LOCK(m_mutex);
        return m_config.empty() && m_utxo.empty();
    }
    size_t Size() const
    {
        LOCK(m_mutex);
        std::map<CPubKey, uint64_t> merged = m_config;
        for (const auto& e : m_utxo) merged[e.first] += e.second;
        return merged.size();
    }
    uint64_t GetWeight(const CPubKey& pubkey) const
    {
        LOCK(m_mutex);
        uint64_t weight = 0;
        auto it = m_config.find(pubkey);
        if (it != m_config.end()) weight += it->second;
        auto it2 = m_utxo.find(pubkey);
        if (it2 != m_utxo.end()) weight += it2->second;
        return weight;
    }
    //! Effective (merged) weights.
    std::map<CPubKey, uint64_t> Weights() const
    {
        LOCK(m_mutex);
        std::map<CPubKey, uint64_t> merged = m_config;
        for (const auto& e : m_utxo) merged[e.first] += e.second;
        return merged;
    }
    //! UTXO-layer weight only (for introspection/tests).
    uint64_t GetUtxoWeight(const CPubKey& pubkey) const
    {
        LOCK(m_mutex);
        auto it = m_utxo.find(pubkey);
        return it == m_utxo.end() ? 0 : it->second;
    }

    /** Parse a "<pubkeyhex>:<weight>" specification and add it to the
     *  configuration layer. Returns false (with an error string) on
     *  malformed input. */
    bool AddFromSpec(const std::string& spec, std::string& error);
};

/** Derive the per-block election seed (whitepaper §3.5) from the parent's
 *  Bitcoin anchor hash and the height — both header fields, so the seed is
 *  identical on every node. Using the anchor (Bitcoin's PoW) instead of the SEQ
 *  block hash removes last-revealer grinding (a producer cannot bias the anchor;
 *  audit M1): its only freedom is which recent Bitcoin block to anchor to, which
 *  the anchor-freshness fork choice makes self-defeating. (An earlier attempt to
 *  chain the leader's VRF score here was reverted — that value is set after
 *  block-index creation and is not a reliable, node-consistent seed input.) */
uint256 ComputePosSeed(const uint256& parent_anchor_hash, uint32_t height);

/** Return the registered stakers ranked best-first (lowest weighted ticket
 *  first) for the given seed. */
std::vector<CPubKey> PosSchedule(const StakeRegistry& registry, const uint256& seed);

/** Rank of a staker in the schedule for a seed, or nullopt if not registered. */
std::optional<size_t> PosRank(const StakeRegistry& registry, const uint256& seed, const CPubKey& pubkey);

/** The committee for a slot: the first min(g_pos_committee_size, #stakers)
 *  entries of the slot's leader schedule, in schedule order. */
std::vector<CPubKey> PosCommittee(const StakeRegistry& registry, const uint256& seed);

/** Countersignature quorum for a committee of m members: a strict majority
 *  (the paper's 51-of-100). */
int PosQuorum(size_t committee_size);

/** Build the per-block challenge script for a leader: "<pubkey> OP_CHECKSIG". */
CScript BuildPosChallenge(const CPubKey& pubkey);

/** Build the per-block challenge for a leader plus committee certification:
 *  "<leader> OP_CHECKSIGVERIFY <q> <c_1> ... <c_m> <m> OP_CHECKMULTISIG".
 *  q defaults to PosQuorum(m) (the public-schedule mode, where the committee
 *  is exactly the schedule prefix); under VRF sortition the caller passes the
 *  chain's fixed quorum (a majority of the *expected* committee size), since
 *  the claimed member count varies. With an empty committee this degrades to
 *  the plain leader-only challenge. */
CScript BuildPosBlockChallenge(const CPubKey& leader, const std::vector<CPubKey>& committee, int quorum_override = 0);

/** Extract the leader pubkey from a "<pubkey> OP_CHECKSIG" challenge, or
 *  nullopt if the script is not of that exact form. */
std::optional<CPubKey> PosChallengeToPubKey(const CScript& challenge);

/** Build the aggregate-committee block challenge (-posaggcommittee):
 *      OP_1 <leader(33)> <agg_key(32)>
 *  where agg_key is the MuSig2 aggregate (x-only) of the committee member
 *  set. The script is a versioned commitment, not interpreter-executed:
 *  CheckProof verifies the leader's ECDSA signature and the single BIP340
 *  aggregate signature directly, and ContextualCheckBlock checks that
 *  agg_key matches the MuSig2 aggregate of exactly the members named (and
 *  proven sortition-eligible) by the coinbase SEQCMT commitments. */
CScript BuildPosAggChallenge(const CPubKey& leader, const std::vector<unsigned char>& agg_key32);

/** The decoded parts of a PoS block challenge. For a leader-only challenge the
 *  committee is empty and quorum 0. For the aggregate-committee form, agg_key
 *  holds the 32-byte MuSig2 aggregate and the committee list is empty (the
 *  member set is named by the block's coinbase SEQCMT commitments). */
struct PosChallengeParts {
    CPubKey leader;
    std::vector<CPubKey> committee;
    int quorum{0};
    std::vector<unsigned char> agg_key;
};

/** Parse any form of PoS block challenge (leader-only, leader plus committee
 *  multisig certification, or leader plus aggregate key), or nullopt for any
 *  other script. */
std::optional<PosChallengeParts> ParsePosBlockChallenge(const CScript& challenge);

/** Compute the election seed for the block that would extend `pindexPrev`. */
uint256 PosSeedForChild(const CBlockIndex* pindexPrev);

// --- VRF sortition (g_pos_vrf; doc/sequentia/07-vrf.md §4) ---

class CBlock;

/** Sum of all registered stake weights. */
uint64_t PosTotalWeight(const StakeRegistry& registry);

/** The stake-weighted sortition slot for a VRF output `beta` under stake
 *  `weight` (total stake `total_weight`):
 *      slot = min( floor( top64(beta / weight) * total_weight / 2^64 ),
 *                  POS_VRF_MAX_SLOT )
 *  Uniform beta gives slot ~ uniform in [0, total/weight): more stake means a
 *  statistically earlier slot, the continuous analogue of the public
 *  schedule's rank. The rank-r liveness gate becomes
 *  nTime >= parent.nTime + slot * g_pos_slot_interval. */
uint64_t PosVrfSlot(const uint256& beta, uint64_t weight, uint64_t total_weight);

/** Build the tagged coinbase OP_RETURN output script carrying the leader's
 *  VRF proof: OP_RETURN PUSH("SEQVRF" || proof). */
CScript BuildPosVrfCommitment(const std::vector<unsigned char>& proof);

/** Extract the VRF proof from a block's coinbase commitment, or nullopt if no
 *  (or a malformed) commitment is present. */
std::optional<std::vector<unsigned char>> ExtractPosVrfProof(const CBlock& block);

/** Committee membership under VRF sortition: with private sortition nobody can
 *  rank stakers (each beta is secret until published), so membership is
 *  threshold-based, Algorand-style: staker i with VRF output beta is a
 *  committee member for the slot iff
 *      PosVrfSlot(beta, weight, total_weight) < g_pos_committee_size.
 *  P(slot < T) = T * weight / total_weight, so the expected committee size is
 *  exactly g_pos_committee_size, distributed weight-proportionally. The
 *  certification quorum stays a strict majority of the *expected* size
 *  (PosQuorum(g_pos_committee_size)), the paper's 51-of-100. */
bool PosVrfIsCommitteeMember(const uint256& beta, uint64_t weight, uint64_t total_weight);

/** Escaping stall (whitepaper §3.8): the number of parent-chain (Bitcoin)
 *  blocks the chain must have advanced past the last certified block's anchor
 *  before a block may be certified below the committee quorum (the "h+3"
 *  rule — the new block references a Bitcoin block 3 past the parent's). */
static const uint32_t POS_ESCAPING_STALL_ANCHOR_GAP = 3;

/** Whether a block anchoring at `block_anchor_height` may be certified below
 *  the normal countersignature quorum, given its parent (last certified block)
 *  anchored at `parent_anchor_height`. True iff the parent chain has advanced
 *  at least POS_ESCAPING_STALL_ANCHOR_GAP blocks — a genuine stall, since a
 *  healthy chain re-anchors only gradually and cannot reference a Bitcoin
 *  block that does not yet exist. Computed purely from the SEQ-committed anchor
 *  heights, so every node agrees. See doc/sequentia/10-liveness-and-escaping-stall.md. */
inline bool PosEscapingStallAllowed(uint32_t parent_anchor_height, uint32_t block_anchor_height)
{
    // Subtraction (not addition) avoids any wraparound near UINT32_MAX.
    return block_anchor_height >= parent_anchor_height &&
           block_anchor_height - parent_anchor_height >= POS_ESCAPING_STALL_ANCHOR_GAP;
}

/** A committee member's eligibility claim carried in the block: its key and
 *  its VRF proof over the slot seed. */
struct PosVrfMember {
    CPubKey pubkey;
    std::vector<unsigned char> proof;
};

/** Build the tagged coinbase OP_RETURN output carrying one committee member's
 *  eligibility proof: OP_RETURN PUSH("SEQCMT" || pubkey(33) || proof). */
CScript BuildPosVrfMemberCommitment(const CPubKey& member, const std::vector<unsigned char>& proof);

/** Extract all committee-member eligibility commitments from a block's
 *  coinbase (malformed entries are skipped). */
std::vector<PosVrfMember> ExtractPosVrfMembers(const CBlock& block);

// --- On-chain stake registration (locked staking outputs) ---

class CTxOut;
class CBlockUndo;
class CCoinsView;

/** Minimum CSV unbonding delay (in blocks) for an output to count as stake;
 *  also the default delay for BuildStakeScript. */
extern uint32_t g_pos_unbonding_period;
static const uint32_t DEFAULT_POS_UNBONDING_PERIOD = 10;

/** SEQUENTIA: minimum stake weight (in policy-asset atoms) a key must hold to
 *  be an eligible blocksigner — leader or committee member (whitepaper §3.3:
 *  0.01% of supply = 40,000 SEQ). Stake below this is ignored by election,
 *  sortition, and the eligible-total denominator. 0 disables the floor (the
 *  default; the Sequentia chain sets it via -posminstake). */
extern uint64_t g_pos_min_stake;

/** Whether `weight` clears the eligibility floor: registered (>=1) and at least
 *  the configured minimum. The single chokepoint for stake eligibility. */
inline bool PosIsEligibleStake(uint64_t weight)
{
    return weight >= std::max<uint64_t>(g_pos_min_stake, 1);
}

/** The canonical staking output script:
 *      <csv_blocks> OP_CHECKSEQUENCEVERIFY OP_DROP <pubkey> OP_CHECKSIG
 *  A bare (unhashed) script so validators can recognize stake at output
 *  creation. Spending requires the staker's signature and csv_blocks of
 *  relative-height maturity (BIP112) — unbonding is the spend itself. */
CScript BuildStakeScript(const CPubKey& pubkey, uint32_t csv_blocks);

/** Parse a staking script, returning (pubkey, raw BIP68 CSV value), or nullopt
 *  if the script is not of the exact canonical form. The CSV value may be
 *  height-based or time-based (SEQUENCE_LOCKTIME_TYPE_FLAG set). */
std::optional<std::pair<CPubKey, uint32_t>> ParseStakeScript(const CScript& script);

/** The wall-clock lock duration (seconds) a staking CSV value enforces:
 *  time-based CSV in 512-second units, height-based CSV times the slot
 *  interval, so both encodings compare on one axis. nullopt if `csv` is not a
 *  valid relative lock (disable flag or stray bits). */
std::optional<int64_t> PosStakeLockSeconds(uint32_t csv);

/** The minimum unbonding lock the chain requires, in seconds
 *  (g_pos_unbonding_period blocks x slot interval). */
int64_t PosRequiredUnbondingSeconds();

/** If the output is a qualifying staking output — canonical script, explicit
 *  policy-asset amount, and an unbonding lock of at least
 *  PosRequiredUnbondingSeconds() — return its staker key and weight (atoms). */
std::optional<std::pair<CPubKey, uint64_t>> StakeFromTxOut(const CTxOut& out);

/** Mirror a connected block into the UTXO stake layer: outputs that create
 *  staking UTXOs add weight; inputs that spend them (from the block's undo
 *  data) subtract it. Must be called exactly once per tip connect. */
void PosApplyBlockStake(const CBlock& block, const CBlockUndo& undo);

/** Exact inverse of PosApplyBlockStake, for tip disconnects (reorgs). */
void PosRevertBlockStake(const CBlock& block, const CBlockUndo& undo);

/** Rebuild the UTXO stake layer by scanning the (flushed) UTXO set. */
bool RebuildUtxoStake(CCoinsView& view);

#endif // BITCOIN_POS_H
