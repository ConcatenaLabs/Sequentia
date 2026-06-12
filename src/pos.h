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
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

class CBlockIndex;

/** When set, the chain uses Proof-of-Stake leader election for block validity
 *  (layered on g_signed_blocks). */
extern bool g_con_pos;

/** Seconds per leader rank: the rank-r leader of a slot may only produce a
 *  block once this many seconds * r have elapsed since the parent block. */
extern int64_t g_pos_slot_interval;
static const int64_t DEFAULT_POS_SLOT_INTERVAL = 10;

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

/** The set of stakers eligible to be elected as block leaders, and their
 *  relative stake weights. For this PoC the set is chain configuration; a
 *  production system would track it in chainstate via staking transactions. */
class StakeRegistry
{
private:
    std::map<CPubKey, uint64_t> m_weights;

public:
    static StakeRegistry& GetInstance()
    {
        static StakeRegistry instance;
        return instance;
    }

    void Clear() { m_weights.clear(); }
    void SetStake(const CPubKey& pubkey, uint64_t weight) { m_weights[pubkey] = weight; }
    bool Empty() const { return m_weights.empty(); }
    size_t Size() const { return m_weights.size(); }
    uint64_t GetWeight(const CPubKey& pubkey) const
    {
        auto it = m_weights.find(pubkey);
        return it == m_weights.end() ? 0 : it->second;
    }
    const std::map<CPubKey, uint64_t>& Weights() const { return m_weights; }

    /** Parse a "<pubkeyhex>:<weight>" specification and add it to the registry.
     *  Returns false (with an error string) on malformed input. */
    bool AddFromSpec(const std::string& spec, std::string& error);
};

/** Derive the per-block election seed from the parent block and its Bitcoin
 *  anchor. Including the anchor ties the leader schedule to Bitcoin, as the
 *  paper specifies. */
uint256 ComputePosSeed(const uint256& parent_hash, const uint256& parent_anchor_hash, uint32_t height);

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
 *  "<leader> OP_CHECKSIGVERIFY <q> <c_1> ... <c_m> <m> OP_CHECKMULTISIG"
 *  with q = PosQuorum(m). With an empty committee this degrades to the plain
 *  leader-only challenge. */
CScript BuildPosBlockChallenge(const CPubKey& leader, const std::vector<CPubKey>& committee);

/** Extract the leader pubkey from a "<pubkey> OP_CHECKSIG" challenge, or
 *  nullopt if the script is not of that exact form. */
std::optional<CPubKey> PosChallengeToPubKey(const CScript& challenge);

/** The decoded parts of a PoS block challenge. For a leader-only challenge the
 *  committee is empty and quorum 0. */
struct PosChallengeParts {
    CPubKey leader;
    std::vector<CPubKey> committee;
    int quorum{0};
};

/** Parse either form of PoS block challenge (leader-only, or leader plus
 *  committee certification), or nullopt for any other script. */
std::optional<PosChallengeParts> ParsePosBlockChallenge(const CScript& challenge);

/** Compute the election seed for the block that would extend `pindexPrev`. */
uint256 PosSeedForChild(const CBlockIndex* pindexPrev);

#endif // BITCOIN_POS_H
