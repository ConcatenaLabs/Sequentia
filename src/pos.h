// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: Proof-of-Stake leader election (proof of concept).
//
// Replaces Elements' fixed-federation signed-block challenge with a per-block
// challenge whose required signer is a stake-weighted leader, elected
// deterministically from a seed derived from the previous block and its Bitcoin
// anchor (see doc/sequentia/04-proof-of-stake.md). The block signature itself
// still rides the existing signed-block machinery (CheckProof), so PoS only
// changes *which* key must sign each block, not how the signature is verified.

#ifndef BITCOIN_POS_H
#define BITCOIN_POS_H

#include <arith_uint256.h>
#include <pubkey.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

class CBlockIndex;
class CBlockHeader;
class CKey;

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
 *  (doc/sequentia/04-proof-of-stake.md §4) instead of the public deterministic schedule:
 *  each block carries the leader's VRF proof over the slot seed in a tagged
 *  coinbase OP_RETURN, and the proof's output determines the leader's
 *  time-gated slot. Only the key holder can compute its slot in advance. */
extern bool g_pos_vrf;

/** When set (with g_pos_vrf), committee certification uses MuSig2 signature
 *  aggregation (doc/sequentia/04-proof-of-stake.md §6) instead of script multisig: the
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

/** When set (with g_pos_vrf), committee certification uses non-interactive
 *  BLS12-381 aggregate signatures instead of MuSig2
 *  (doc/sequentia/proposals/autonomous-committee.md §7): each committee member
 *  signs the block hash with a BLS key derived from its staking key and
 *  commits its BLS public key plus a proof-of-possession in the coinbase; the
 *  block carries one 96-byte BLS aggregate signature, and the challenge commits
 *  to the 48-byte aggregate of the member BLS keys. Unlike MuSig2 (interactive,
 *  n-of-n), BLS shares are non-interactive, which is what lets a gossip
 *  committee aggregate "whichever members respond". Supersedes g_pos_agg_committee
 *  when both are set. */
extern bool g_pos_bls;

/** When set (requires g_pos_vrf + g_pos_bls), committee MEMBERSHIP is public
 *  and fixed-size (doc/sequentia/committee-public-selection-impl-spec.md,
 *  Option A): the certifying committee for a slot is the first
 *  min(#eligible stakers, g_pos_committee_size) entries of the deterministic
 *  public schedule (PosCommittee), and the quorum derives from that ACTUAL
 *  size (PosPublicQuorum). This restores quorum intersection — any two quorums
 *  overlap in at least 2 members at every size — so two disjoint quorums can
 *  never certify rival same-height blocks, which threshold VRF sortition
 *  cannot guarantee once the staker pool exceeds the committee target (its
 *  eligible count is a random variable while the quorum is fixed). Leader
 *  election stays private-VRF. Under this mode the per-member VRF eligibility
 *  proofs in the certificate are vestigial and are not verified. NETWORK-WIDE
 *  consensus rule: every node on a chain must agree on the value. */
extern bool g_pos_public_committee;

/** Sanity bound on -poscommitteesize under g_pos_public_committee. The
 *  recommended cap is 250 (quorum 126, the classical one-third Byzantine
 *  bound); the bound only guards against absurd configurations. */
static const int MAX_POS_PUBLIC_COMMITTEE_SIZE = 1000;

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
/** How a block producer's fee reward is paid out. */
enum class PosPayoutMode : uint8_t {
    LEADER = 0,   //!< default: the coinbase pays the elected leader (no record)
    DIRECT = 1,   //!< the coinbase pays a scriptPubKey the operator committed to
    LOTTERY = 2,  //!< the coinbase pays one participant, drawn by stake weight
};

/** A payout policy an operator has committed to, effective from `activation`. */
struct PosPayoutPolicy {
    PosPayoutMode mode{PosPayoutMode::LEADER};
    int64_t activation{0};      //!< block height from which this policy binds
    CScript script;             //!< DIRECT: the committed payout scriptPubKey
    uint32_t commission_bp{0};  //!< LOTTERY: basis points the operator keeps

    friend bool operator==(const PosPayoutPolicy& a, const PosPayoutPolicy& b)
    {
        return a.mode == b.mode && a.activation == b.activation &&
               a.script == b.script && a.commission_bp == b.commission_bp;
    }
};

/** Notice period, in blocks, before a newly announced payout policy may bind.
 *  A delegator can leave a pool unilaterally and instantly, so a mandatory delay
 *  between announcing a payout change and its taking effect is what makes
 *  "audit the pool before you commit" mean anything: an operator cannot flip the
 *  rewards to themselves and collect before their delegators can react. Without
 *  it, auditing is worthless -- you would inspect a pool, delegate, and be
 *  redirected on the very next block. It bounds the loss to zero for a delegator
 *  who is watching; it cannot help one who is not. */
extern uint32_t g_pos_payout_notice;
static const uint32_t DEFAULT_POS_PAYOUT_NOTICE = 2880; // ~1 day at 30s slots

/** Basis-point denominator for a LOTTERY operator commission. */
static const uint32_t POS_COMMISSION_DENOM = 10000;

class StakeRegistry
{
private:
    mutable Mutex m_mutex;
    std::map<CPubKey, uint64_t> m_config GUARDED_BY(m_mutex);
    std::map<CPubKey, uint64_t> m_utxo GUARDED_BY(m_mutex);
    //! Registered BLS public key per staker (impl spec Option A, phase 2). Under
    //! the public fixed-size committee with the bitfield certificate, a staker's
    //! BLS signing key must be registered so validators can aggregate and verify
    //! a certificate that names members only by a bitfield over the committee.
    //! The BLS pubkey is derived from the staking key (PosBlsSeedFromKey), so it
    //! is stable per staker, and its proof-of-possession is verified once at
    //! registration. Two layers, mirroring the weight: a config/genesis layer
    //! (SetBls) and a UTXO layer (m_bls_utxo) that a staking output carries and
    //! whose lifecycle is tied to the staker's UTXO weight (dropped when the
    //! last staking output is spent), so it is reorg-safe by construction. The
    //! config layer takes precedence when both are present.
    std::map<CPubKey, std::vector<unsigned char>> m_bls GUARDED_BY(m_mutex);
    std::map<CPubKey, std::vector<unsigned char>> m_bls_utxo GUARDED_BY(m_mutex);
    //! SEQUENTIA delegation: controller pubkey -> the signer that produces blocks
    //! with the controller's weight. Derived from unspent delegation-record
    //! outputs (BuildDelegationScript), so it is a pure function of the UTXO set,
    //! exactly like the weight and the UTXO BLS layer. A controller with no
    //! record signs for itself.
    //!
    //! The record lives in its OWN small output, never inside the staking output.
    //! That is what lets a staker re-point (or reclaim) its block-signing rights
    //! WITHOUT spending the stake -- essential once a staking output carries a
    //! multi-year vesting lock (liquid_locktime) and therefore cannot be spent at
    //! all. It also separates the hot block-signing key from the cold key that
    //! can actually move the coins.
    //!
    //! Resolution is ONE HOP, never chained: if A delegates to B and B delegates
    //! to C, A's weight goes to B and B's own weight goes to C. Chasing chains
    //! would admit cycles.
    std::map<CPubKey, CPubKey> m_deleg_utxo GUARDED_BY(m_mutex);
    //! SEQUENTIA payout policies: signer -> (activation height -> policy), from
    //! unspent payout records. A pure function of the UTXO set, like the layers
    //! above. Several records may coexist for one signer -- a pending one during
    //! its notice period, and the one it will replace -- so they are keyed by
    //! activation height and resolved by height at lookup, never by "latest
    //! seen". Consensus forbids two records sharing a (signer, activation).
    std::map<CPubKey, std::map<int64_t, PosPayoutPolicy>> m_payout_utxo GUARDED_BY(m_mutex);

    //! The signer for a controller (itself when it has delegated nothing).
    CPubKey SignerForLocked(const CPubKey& controller) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        auto it = m_deleg_utxo.find(controller);
        return it == m_deleg_utxo.end() ? controller : it->second;
    }
    //! config + utxo weight, keyed by CONTROLLER (before delegation is applied).
    std::map<CPubKey, uint64_t> MergedLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        std::map<CPubKey, uint64_t> merged = m_config;
        for (const auto& e : m_utxo) merged[e.first] += e.second;
        return merged;
    }
    //! Merged weight re-keyed onto signers (delegation applied, one hop).
    std::map<CPubKey, uint64_t> WeightsLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        std::map<CPubKey, uint64_t> out;
        for (const auto& e : MergedLocked()) out[SignerForLocked(e.first)] += e.second;
        return out;
    }

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
        m_bls.clear();
        m_bls_utxo.clear();
        m_deleg_utxo.clear();
        m_payout_utxo.clear();
    }
    //! Set a configured staker's weight.
    void SetStake(const CPubKey& pubkey, uint64_t weight)
    {
        LOCK(m_mutex);
        m_config[pubkey] = weight;
    }
    //! Register a CONFIG-layer staker's BLS public key (impl spec Option A,
    //! phase 2). The caller has verified its proof-of-possession.
    void SetBls(const CPubKey& pubkey, const std::vector<unsigned char>& bls_pubkey)
    {
        LOCK(m_mutex);
        m_bls[pubkey] = bls_pubkey;
    }
    //! A staker's registered BLS public key (config layer preferred, else UTXO
    //! layer), or an empty vector if none.
    std::vector<unsigned char> GetBls(const CPubKey& pubkey) const
    {
        LOCK(m_mutex);
        auto it = m_bls.find(pubkey);
        if (it != m_bls.end()) return it->second;
        auto it2 = m_bls_utxo.find(pubkey);
        return it2 == m_bls_utxo.end() ? std::vector<unsigned char>() : it2->second;
    }
    bool HasBls(const CPubKey& pubkey) const
    {
        LOCK(m_mutex);
        return m_bls.count(pubkey) > 0 || m_bls_utxo.count(pubkey) > 0;
    }
    //! Replace the UTXO-derived layers wholesale (startup rebuild): weights and
    //! the UTXO BLS registrations, both pure functions of the UTXO set.
    void SetUtxoStake(std::map<CPubKey, uint64_t>&& utxo,
                      std::map<CPubKey, std::vector<unsigned char>>&& bls_utxo = {},
                      std::map<CPubKey, CPubKey>&& deleg_utxo = {},
                      std::map<CPubKey, std::map<int64_t, PosPayoutPolicy>>&& payout_utxo = {})
    {
        LOCK(m_mutex);
        m_utxo = std::move(utxo);
        m_bls_utxo = std::move(bls_utxo);
        m_deleg_utxo = std::move(deleg_utxo);
        m_payout_utxo = std::move(payout_utxo);
    }
    //! A staking output entered the UTXO set. A non-empty bls_pubkey registers
    //! (or re-affirms) the staker's UTXO-layer committee BLS key; ConnectBlock
    //! has verified its PoP and that it does not conflict with an existing key.
    void AddUtxoStake(const CPubKey& pubkey, uint64_t amount,
                      const std::vector<unsigned char>& bls_pubkey = {})
    {
        LOCK(m_mutex);
        m_utxo[pubkey] += amount;
        if (!bls_pubkey.empty()) m_bls_utxo[pubkey] = bls_pubkey;
    }
    //! A staking output left the UTXO set (spent, or its creation reverted). The
    //! UTXO BLS key is tied to the staker having any UTXO weight: when the last
    //! staking output is gone, the registration goes with it.
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
        if (it->second == 0) {
            m_utxo.erase(it);
            m_bls_utxo.erase(pubkey);
        }
    }

    bool Empty() const
    {
        LOCK(m_mutex);
        return m_config.empty() && m_utxo.empty();
    }
    size_t Size() const
    {
        LOCK(m_mutex);
        return WeightsLocked().size();
    }
    //! The weight a SIGNER commands: its own, plus every controller's that has
    //! delegated to it. This is the number the leader election, the sortition and
    //! the eligibility floor all see, so a pool operator is ranked on the weight
    //! it has been lent -- while the coins themselves never move.
    uint64_t GetWeight(const CPubKey& pubkey) const
    {
        LOCK(m_mutex);
        uint64_t weight = 0;
        for (const auto& e : MergedLocked()) {
            if (SignerForLocked(e.first) == pubkey) weight += e.second;
        }
        return weight;
    }
    //! Effective weights, keyed by signer (delegation applied).
    std::map<CPubKey, uint64_t> Weights() const
    {
        LOCK(m_mutex);
        return WeightsLocked();
    }
    //! Raw weights keyed by controller, before delegation (introspection/tests).
    std::map<CPubKey, uint64_t> ControllerWeights() const
    {
        LOCK(m_mutex);
        return MergedLocked();
    }
    //! The signer a controller's weight currently counts for.
    CPubKey SignerFor(const CPubKey& controller) const
    {
        LOCK(m_mutex);
        return SignerForLocked(controller);
    }
    bool HasDelegation(const CPubKey& controller) const
    {
        LOCK(m_mutex);
        return m_deleg_utxo.count(controller) > 0;
    }
    std::map<CPubKey, CPubKey> Delegations() const
    {
        LOCK(m_mutex);
        return m_deleg_utxo;
    }
    //! A delegation record entered the UTXO set.
    void AddUtxoDelegation(const CPubKey& controller, const CPubKey& signer)
    {
        LOCK(m_mutex);
        m_deleg_utxo[controller] = signer;
    }
    //! A delegation record left the UTXO set. Erase only if it is still the
    //! record in force: a rotation spends the old record and creates a new one in
    //! the same transaction, and PosApplyBlockStake adds created outputs before
    //! subtracting spent ones. Without this guard the freshly-installed record
    //! would be erased by the removal of the one it replaced.
    void SubUtxoDelegation(const CPubKey& controller, const CPubKey& signer)
    {
        LOCK(m_mutex);
        auto it = m_deleg_utxo.find(controller);
        if (it != m_deleg_utxo.end() && it->second == signer) m_deleg_utxo.erase(it);
    }

    //! A payout record entered the UTXO set.
    void AddUtxoPayout(const CPubKey& signer, const PosPayoutPolicy& policy)
    {
        LOCK(m_mutex);
        m_payout_utxo[signer][policy.activation] = policy;
    }
    //! A payout record left the UTXO set. Erase only the exact policy, so that a
    //! record replaced within one block is not clobbered (see SubUtxoDelegation).
    void SubUtxoPayout(const CPubKey& signer, const PosPayoutPolicy& policy)
    {
        LOCK(m_mutex);
        auto it = m_payout_utxo.find(signer);
        if (it == m_payout_utxo.end()) return;
        auto jt = it->second.find(policy.activation);
        if (jt != it->second.end() && jt->second == policy) it->second.erase(jt);
        if (it->second.empty()) m_payout_utxo.erase(it);
    }
    //! Whether a record already exists for this exact (signer, activation).
    bool HasPayoutAt(const CPubKey& signer, int64_t activation) const
    {
        LOCK(m_mutex);
        auto it = m_payout_utxo.find(signer);
        return it != m_payout_utxo.end() && it->second.count(activation) > 0;
    }
    //! The policy binding a signer at `height`: the record with the greatest
    //! activation <= height. Nullopt when the signer has announced none yet, or
    //! when every announced policy is still inside its notice period.
    std::optional<PosPayoutPolicy> PayoutFor(const CPubKey& signer, int64_t height) const
    {
        LOCK(m_mutex);
        auto it = m_payout_utxo.find(signer);
        if (it == m_payout_utxo.end() || it->second.empty()) return std::nullopt;
        // upper_bound(height) is the first activation strictly after height.
        auto jt = it->second.upper_bound(height);
        if (jt == it->second.begin()) return std::nullopt; // all still pending
        --jt;
        return jt->second;
    }
    std::map<CPubKey, std::map<int64_t, PosPayoutPolicy>> Payouts() const
    {
        LOCK(m_mutex);
        return m_payout_utxo;
    }
    //! Every controller whose weight counts for `signer`, with that weight. The
    //! signer itself is included when it has not delegated its own stake away.
    //! This is the LOTTERY draw's participant set.
    std::map<CPubKey, uint64_t> ParticipantsFor(const CPubKey& signer) const
    {
        LOCK(m_mutex);
        std::map<CPubKey, uint64_t> out;
        for (const auto& e : MergedLocked()) {
            if (e.second > 0 && SignerForLocked(e.first) == signer) out[e.first] = e.second;
        }
        return out;
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

// --- Public fixed-size committee (g_pos_public_committee; impl spec Option A) ---

/** The ACTUAL committee size under the public fixed-size committee:
 *  min(#eligible stakers, g_pos_committee_size). Depends only on the registry
 *  (the seed decides WHO is in the committee, not how many). */
int PosPublicCommitteeSize(const StakeRegistry& registry);

/** Countersignature quorum for an actual committee of k members under the
 *  public fixed-size committee: a strict majority, plus one when k is odd, so
 *  that any two quorums overlap in at least 2 members at EVERY size (one
 *  tolerated equivocator; identical to PosQuorum at every even k, including
 *  the paper's 51-of-100 and the recommended 126-of-250). Never exceeds k. */
int PosPublicQuorum(int k);

/** The certification quorum for the current slot: PosPublicQuorum of the
 *  actual size under g_pos_public_committee, else the fixed
 *  PosQuorum(g_pos_committee_size) of the nominal size. */
int PosSlotQuorum(const StakeRegistry& registry);

/** The ordered public committee for a slot under g_pos_public_committee: the
 *  schedule prefix (PosSchedule order) restricted to BLS-registered stakers and
 *  capped at g_pos_committee_size. The ORDER is the bitfield index order of the
 *  certificate, so producer and validator must derive it identically. */
std::vector<CPubKey> PosPublicCommittee(const StakeRegistry& registry, const uint256& seed);

/** The public committee for a slot as a set, for membership checks. */
std::set<CPubKey> PosPublicCommitteeSet(const StakeRegistry& registry, const uint256& seed);

/** Cap on the number of committee members a certificate may name (and a node
 *  collects shares for): the configured committee size under the public
 *  fixed-size committee (the schedule prefix cannot exceed it), else the
 *  aggregate-committee hard cap (threshold sortition can legitimately draw
 *  above the expected size, but never above this). */
int PosMaxCommitteeMembers();

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

/** Build the BLS aggregate-committee block challenge (-posbls):
 *      OP_2 <leader(33)>
 *  Just the elected leader and the OP_2 version marker. Unlike the MuSig2 form,
 *  the committee's aggregate key is NOT in the challenge: the whole BLS
 *  certificate (the member set and the aggregate signature) lives in the block's
 *  proof *solution*, which is excluded from the block hash. That makes the hash
 *  the committee signs independent of which members sign — so members sign
 *  immediately and non-interactively and any node aggregates whatever shares
 *  arrive (doc/sequentia/proposals/autonomous-committee.md §7). CheckProof
 *  verifies the leader's ECDSA signature and the BLS aggregate (against the
 *  member keys in the solution); ContextualCheckBlock checks every member's
 *  sortition eligibility. */
CScript BuildPosBlsChallenge(const CPubKey& leader);

/** The decoded parts of a PoS block challenge. For a leader-only challenge the
 *  committee is empty and quorum 0. For the aggregate-committee form, agg_key
 *  holds the 32-byte MuSig2 aggregate and the committee list is empty (the
 *  member set is named by the block's coinbase SEQCMT commitments). */
struct PosChallengeParts {
    CPubKey leader;
    std::vector<CPubKey> committee;
    int quorum{0};
    std::vector<unsigned char> agg_key;  //!< 32-byte MuSig2 aggregate (OP_1 form)
    bool is_bls{false};                  //!< OP_2 BLS form (certificate is in the solution)
};

/** Parse any form of PoS block challenge (leader-only, leader plus committee
 *  multisig certification, or leader plus aggregate key), or nullopt for any
 *  other script. */
std::optional<PosChallengeParts> ParsePosBlockChallenge(const CScript& challenge);

/** The script a con_pos block's coinbase fee outputs must pay to: the elected
 *  leader's own key as a native-segwit (Bitcoin-format) P2WPKH output. Sequentia
 *  has no block subsidy, so a producer is paid only in the transaction fees of
 *  the block it leads; binding the coinbase to this script (consensus, gated by
 *  Consensus::Params::pos_coinbase_leader_height) makes fees unspendable by
 *  anyone but the elected leader. Producer and validator both derive it from the
 *  same leader pubkey so they agree byte-for-byte. */
CScript PosLeaderFeeScript(const CPubKey& leader);

/** Compute the election seed for the block that would extend `pindexPrev`. */
uint256 PosSeedForChild(const CBlockIndex* pindexPrev);

// --- VRF sortition (g_pos_vrf; doc/sequentia/04-proof-of-stake.md §4) ---

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

/** Exponential-race (weighted-sampling) sortition: a Sybil-neutral alternative
 *  to PosVrfSlot. score = -ln(beta / 2^256) / weight is Exponential(weight), so
 *  electing the lowest score is exactly stake-proportional AND unchanged by
 *  splitting a stake into many identities (min of exponentials is exponential
 *  with the summed rate). Fixed-point and deterministic (no floating point).
 *  PosVrfScoreExp returns the fine Q32 score used to ORDER candidates in the
 *  election; PosVrfSlotExp returns floor(score) (capped at POS_VRF_MAX_SLOT)
 *  for the time-gate nTime >= parent.nTime + slot * g_pos_slot_interval. */
arith_uint256 PosVrfScoreExp(const uint256& beta, uint64_t weight, uint64_t total_weight);
uint64_t PosVrfSlotExp(const uint256& beta, uint64_t weight, uint64_t total_weight);

namespace Consensus { struct Params; }
/** Whether the exponential-race sortition (PosVrfSlotExp / PosVrfScoreExp) is
 *  the active leader-election rule for a block at height `height`. Gated by
 *  consensus.pos_exprace_height: 0 keeps the legacy PosVrfSlot / raw-beta
 *  election; a positive H switches the whole network to the exp-race from
 *  height H onward (a coordinated hard fork). Every election site (time-gate in
 *  validation/miner/producer and the BackedForRound ordering) MUST gate on this
 *  single predicate so all nodes flip together at exactly the same height. */
bool PosExpRaceActive(const Consensus::Params& params, int height);

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
 *  heights, so every node agrees. See doc/sequentia/04-proof-of-stake.md.
 *
 *  CAVEAT (incident 2026-07-17): a height gap is NOT a time gap. During a
 *  testnet4 difficulty-1 block-storm the parent chain advances several heights
 *  in seconds, so this test alone passes with the chain fully alive (a
 *  leader-only block was minted 30 s after a quorum-certified parent, seeding
 *  a finality partition). Consensus therefore ALSO requires the parent-chain
 *  median-time-past gap between the two anchors (CheckEscapingStallMtpGap,
 *  anchor.h) whenever the sub-quorum relaxation is actually exercised. */
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

/** A BLS committee member, as carried in the block's proof solution: its staking
 *  (secp256k1) key and VRF proof (proving sortition eligibility, exactly as
 *  PosVrfMember), plus its 48-byte BLS public key and a 96-byte BLS
 *  proof-of-possession (binding the member to that BLS key and closing the
 *  rogue-key attack). */
struct PosBlsMember {
    CPubKey pubkey;
    std::vector<unsigned char> proof;       //!< VRF proof over the slot seed
    std::vector<unsigned char> bls_pubkey;  //!< 48-byte compressed BLS public key
    std::vector<unsigned char> bls_pop;     //!< 96-byte BLS proof-of-possession
};

/** The decoded BLS committee certificate carried in a block's proof solution. */
struct PosBlsCertificate {
    std::vector<unsigned char> leader_sig;  //!< the leader's ECDSA signature over the block hash
    std::vector<unsigned char> agg_sig;     //!< 96-byte BLS aggregate of the members' shares
    std::vector<PosBlsMember> members;       //!< the signing committee
};

/** Encode a BLS committee certificate into a block proof solution:
 *      <leader_sig> <agg_sig(96)> <member_1(257)> ... <member_m(257)>
 *  each member being secp_pubkey(33) || vrf_proof(80) || bls_pubkey(48) || bls_pop(96).
 *  The certificate lives in the solution (excluded from the block hash), so the
 *  hash the committee signs does not depend on who signs. */
CScript BuildPosBlsSolution(const std::vector<unsigned char>& leader_sig,
                            const std::vector<unsigned char>& agg_sig,
                            const std::vector<PosBlsMember>& members);

/** Decode a BLS committee certificate solution, or nullopt if malformed (wrong
 *  field sizes, an invalid member key, or more members than the committee cap). */
std::optional<PosBlsCertificate> ParsePosBlsSolution(const CScript& solution);

// --- Bitfield BLS certificate (g_pos_public_committee; impl spec Option A ph.2) ---

/** The BLS signing seed for a staking key: a domain-separated hash of the key,
 *  so a staker needs no separate BLS key to manage. Its public key
 *  (BlsDerivePubKey) is what a staker registers (StakeRegistry::SetBls). */
std::vector<unsigned char> PosBlsSeedFromKey(const CKey& key);

/** The decoded bitfield BLS certificate. Under the public fixed-size committee
 *  the member SET is public (the ordered PosCommittee schedule prefix), so the
 *  certificate names its signers by a bitfield over that order instead of
 *  carrying each member's key and proof — collapsing a ~257-byte-per-member
 *  certificate to the leader signature, one aggregate, and ~one bit per seat. */
struct PosBlsBitfieldCert {
    std::vector<unsigned char> leader_sig;  //!< the leader's ECDSA signature over the block hash
    std::vector<unsigned char> agg_sig;     //!< 96-byte BLS aggregate of the signers' shares
    std::vector<unsigned char> bitfield;    //!< bit i set == committee[i] signed (LSB-first)
};

/** Encode a bitfield BLS certificate into a block proof solution:
 *      <leader_sig> <agg_sig(96)> <bitfield(ceil(committee/8))>
 *  The signed block hash excludes the solution, so it is member-independent. */
CScript BuildPosBlsBitfieldSolution(const std::vector<unsigned char>& leader_sig,
                                    const std::vector<unsigned char>& agg_sig,
                                    const std::vector<unsigned char>& bitfield);

/** Decode a bitfield BLS certificate solution, or nullopt if malformed. */
std::optional<PosBlsBitfieldCert> ParsePosBlsBitfieldSolution(const CScript& solution);

/** Verify a bitfield BLS certificate's aggregate signature and membership
 *  against the registry at `pindexPrev`'s stake state (the global registry must
 *  mirror that tip; true in ConnectBlock and for a gossiped cert whose parent
 *  is the active tip). Recomputes the ordered public committee from the seed,
 *  resolves each set bit to that member's REGISTERED BLS key, rejects bits
 *  beyond the committee (phantom signers), and checks the one aggregate against
 *  all signer keys (the batch verification: one pairing check, not one per
 *  member). Returns the number of signers, or -1 on any failure (reason set to
 *  a bad-posbls-* string). Does NOT check the leader signature (self-contained,
 *  done in CheckProof) or the quorum policy (the caller applies it, since the
 *  escaping-stall floor differs from the pin-a-height full-quorum rule). */
int PosVerifyBitfieldCertificate(const CBlockHeader& header, const CBlockIndex* pindexPrev,
                                 const StakeRegistry& registry, std::string& reason);

/** Test bit i (LSB-first) of a signer bitfield. */
bool PosBitfieldTest(const std::vector<unsigned char>& bitfield, size_t i);
/** Set bit i (LSB-first) of a signer bitfield, growing it as needed. */
void PosBitfieldSet(std::vector<unsigned char>& bitfield, size_t i);
/** Count set bits in a signer bitfield. */
int PosBitfieldPopcount(const std::vector<unsigned char>& bitfield);

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
 *      <csv_blocks> OP_CHECKSEQUENCEVERIFY OP_DROP [<bls_pubkey(48)> OP_DROP
 *      <bls_pop(96)> OP_DROP] [<liquid_locktime> OP_CHECKLOCKTIMEVERIFY OP_DROP]
 *      <pubkey> OP_CHECKSIG
 *  A bare (unhashed) script so validators can recognize stake at output
 *  creation. Spending requires the staker's signature and csv_blocks of
 *  relative-height maturity (BIP112) — unbonding is the spend itself. When
 *  bls_pubkey/bls_pop are non-empty (impl spec Option A phase 2, runtime
 *  committee registration) they are pushed and OP_DROPed (spend semantics
 *  unchanged), so the BLS key rides in the UTXO and the registry's UTXO layer
 *  learns it as a pure function of the UTXO set (reorg-safe like the weight).
 *  The 48-byte BLS-pubkey push is unambiguous: a staking pubkey is 33 or 65
 *  bytes, never 48.
 *
 *  SEQUENTIA vesting: when liquid_locktime is non-zero the output additionally
 *  carries an ABSOLUTE timelock (BIP65), so it cannot be spent — and therefore
 *  cannot be sold or transferred — until that height/time. It still accrues
 *  stake weight the whole time, because weight is credited for a staking UTXO
 *  merely EXISTING (StakeFromTxOut); the coin is never moved or signed over to
 *  produce a block. That is what makes a "staking-only period" expressible: the
 *  tokens stake and earn fees while being unsellable. Non-transferability falls
 *  out of non-spendability, so no covenant is needed.
 *
 *  A relative (CSV) lock cannot serve this purpose: BIP68 locks are 16-bit, so
 *  the time-based encoding tops out at 65535*512s = 388 days. Absolute locks are
 *  32-bit and reach any calendar date. Prefer a time-based (unix-time)
 *  liquid_locktime over a height: block heights drift against a calendar over a
 *  multi-year vesting schedule. */
CScript BuildStakeScript(const CPubKey& pubkey, uint32_t csv_blocks,
                         const std::vector<unsigned char>& bls_pubkey = {},
                         const std::vector<unsigned char>& bls_pop = {},
                         int64_t liquid_locktime = 0);

/** The canonical DELEGATION-RECORD script:
 *      <"SEQDEL"> OP_DROP <signer_pubkey> OP_DROP <controller_pubkey> OP_CHECKSIG
 *
 *  A bare script, like the staking output, so validators recognize it at output
 *  creation and the delegation layer stays a pure function of the UTXO set.
 *  While this output is unspent, all of `controller`'s stake weight counts for
 *  `signer`, which is the key that must produce and sign blocks. The record is
 *  spendable by the CONTROLLER alone: only the owner of the stake may re-point
 *  or reclaim its block-signing rights.
 *
 *  Why the record lives in its own output rather than inside the staking script:
 *    - Rotation. Re-pointing to a new signer spends only this tiny output, never
 *      the stake. A staking output with a multi-year vesting lock cannot be spent
 *      at all, so a signer named inside it could never be changed -- a compromised
 *      operator key would be irrevocable for the whole lock.
 *    - Custody. The staking output's own pubkey never has to be online. Delegate
 *      to a hot signing key (your own, or a pool's) and keep the key that can
 *      actually move the coins offline. A signer can never spend the stake.
 *
 *  Rewards are unaffected by this file: the coinbase must pay the elected leader
 *  (the SIGNER), enforced in ConnectBlock -- unless that signer has committed a
 *  payout policy (BuildPayoutScript), which may redirect or share the reward. */
CScript BuildDelegationScript(const CPubKey& controller, const CPubKey& signer);


/** The canonical PAYOUT-RECORD script:
 *      <"SEQPAY"> OP_DROP <activation> OP_DROP <mode> OP_DROP <param> OP_DROP
 *      <signer_pubkey> OP_CHECKSIG
 *  param is the payout scriptPubKey (DIRECT) or the commission in basis points
 *  (LOTTERY). Bare, like the staking and delegation scripts, and spendable by
 *  the signer alone. Consensus requires activation >= creation_height +
 *  g_pos_payout_notice, and forbids two unspent records sharing one
 *  (signer, activation). The policy in force at height h is the record with the
 *  greatest activation <= h; older records linger harmlessly until spent. */
CScript BuildPayoutScript(const CPubKey& signer, const PosPayoutPolicy& policy);

/** The (signer, policy) a payout-record script names, or nullopt. */
std::optional<std::pair<CPubKey, PosPayoutPolicy>> ParsePayoutScript(const CScript& script);
std::optional<std::pair<CPubKey, PosPayoutPolicy>> PayoutFromTxOut(const CTxOut& out);

/** The scriptPubKey a block's coinbase MUST pay its fees to, given the elected
 *  leader, the block height, and the block's election seed. This is the single
 *  seam shared by the producer (which builds the coinbase) and ConnectBlock
 *  (which enforces it), so the two can never disagree.
 *
 *  LEADER  -> P2WPKH(leader): the default, and the pre-existing behaviour.
 *  DIRECT  -> the operator's committed script.
 *  LOTTERY -> P2WPKH of one participant, drawn deterministically from the seed,
 *             weighted by stake. The seed is SHA256(parent Bitcoin anchor hash ||
 *             height), supplied by Bitcoin's proof of work, so no operator can
 *             bias the draw. Over many blocks each delegator earns its exact
 *             proportional share with no per-delegator accounting -- at the cost
 *             of lumpy, infrequent payouts rather than a smoothed income. */
CScript PosRequiredCoinbaseScript(const CPubKey& leader, int64_t height, const uint256& seed);

/** The (controller, signer) a delegation-record script names, or nullopt if the
 *  script is not of the canonical form. */
std::optional<std::pair<CPubKey, CPubKey>> ParseDelegationScript(const CScript& script);

/** The (controller, signer) a txout registers, or nullopt if it is not a
 *  delegation record. Value and asset are unconstrained: the record carries no
 *  weight of its own, it only re-points weight the staking outputs already hold. */
std::optional<std::pair<CPubKey, CPubKey>> DelegationFromTxOut(const CTxOut& out);

/** A parsed staking script. bls_pubkey/bls_pop are empty when the output carries
 *  no committee registration (the old form, or a leader-only staker).
 *  liquid_locktime is 0 when the output carries no absolute vesting lock. */
struct ParsedStake {
    CPubKey pubkey;
    uint32_t csv{0};
    std::vector<unsigned char> bls_pubkey;
    std::vector<unsigned char> bls_pop;
    int64_t liquid_locktime{0};
};

/** Parse a staking script (either form), or nullopt if not of the canonical
 *  template. */
std::optional<ParsedStake> ParseStakeScriptFull(const CScript& script);

/** Parse a staking script, returning (pubkey, raw BIP68 CSV value), or nullopt
 *  if the script is not of the canonical form. The CSV value may be height-based
 *  or time-based (SEQUENCE_LOCKTIME_TYPE_FLAG set). */
std::optional<std::pair<CPubKey, uint32_t>> ParseStakeScript(const CScript& script);

/** The BLS committee registration (pubkey, proof-of-possession) carried by a
 *  staking output, or nullopt if it carries none. The PoP is NOT verified here
 *  (no crypto in libbitcoin_common); ConnectBlock verifies it in the node
 *  library and rejects a block whose new staking output has an invalid PoP. */
std::optional<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>>
ParseStakeBlsRegistration(const CScript& script);

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

/** Register the genesis block's staking output(s) before any chainstate
 *  activation, so PoS blocks re-validated during a reload/reindex are checked
 *  against a registry that already contains the genesis staker. */
void SeedGenesisStake(const CBlock& genesis);

// --- Operator-configured static checkpoints (-poscheckpoint=height:hash) ---
//
// A long-range-attack backstop supplied by the operator up front, so it protects
// a *fresh* sync before any block is downloaded: a block presented at a
// configured height must carry the configured hash, else it (and any branch
// built on it) is rejected in ContextualCheckBlockHeader. Reject-only — they
// never make a node seek or download a particular branch. These live in the
// common layer (here) rather than the node-layer anchor module because
// chainparams.cpp configures them and must link them into libbitcoin_common
// (and tools such as elements-tx), which does not link the node module.

/** Drop all configured checkpoints (chain-parameter (re)load). */
void ClearConfiguredPosCheckpoints();

/** Register a configured checkpoint. Fails on a negative height or a height
 *  already configured with a different hash. */
bool AddConfiguredPosCheckpoint(int height, const uint256& hash, std::string& error);

/** All configured checkpoints, keyed by Sequentia height. */
std::map<int, uint256> GetConfiguredPosCheckpoints();

#endif // BITCOIN_POS_H
