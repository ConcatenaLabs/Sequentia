// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos.h>

#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <logging.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <undo.h>
#include <vrf.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <util/strencodings.h>

#include <algorithm>

bool g_con_pos = false;
uint32_t g_pos_unbonding_period = DEFAULT_POS_UNBONDING_PERIOD;
int64_t g_pos_slot_interval = DEFAULT_POS_SLOT_INTERVAL;
int g_pos_committee_size = DEFAULT_POS_COMMITTEE_SIZE;
bool g_pos_vrf = false;
bool g_pos_agg_committee = false;
uint64_t g_pos_min_stake = 0;

bool StakeRegistry::AddFromSpec(const std::string& spec, std::string& error)
{
    size_t colon = spec.rfind(':');
    if (colon == std::string::npos) {
        error = strprintf("staker spec '%s' must be <pubkeyhex>:<weight>", spec);
        return false;
    }
    std::string pubkey_hex = spec.substr(0, colon);
    std::string weight_str = spec.substr(colon + 1);
    if (!IsHex(pubkey_hex)) {
        error = strprintf("staker pubkey '%s' is not valid hex", pubkey_hex);
        return false;
    }
    std::vector<unsigned char> pubkey_bytes = ParseHex(pubkey_hex);
    CPubKey pubkey(pubkey_bytes);
    // Note: use the context-free IsValid() (structural: header byte + length),
    // not IsFullyValid(), because the stake registry is loaded while parsing
    // chain parameters, before the secp256k1 verification context is started.
    // Full on-curve validity is exercised later during election/signature
    // verification, after ECC_Start().
    if (!pubkey.IsValid()) {
        error = strprintf("staker pubkey '%s' is not a structurally valid public key", pubkey_hex);
        return false;
    }
    int64_t weight = 0;
    if (!ParseInt64(weight_str, &weight) || weight <= 0) {
        error = strprintf("staker weight '%s' must be a positive integer", weight_str);
        return false;
    }
    SetStake(pubkey, (uint64_t)weight);
    return true;
}

uint256 ComputePosSeed(uint64_t parent_vrf_score, const uint256& parent_anchor_hash, uint32_t height)
{
    CSHA256 sha;
    unsigned char score_le[8];
    for (int i = 0; i < 8; ++i) score_le[i] = (unsigned char)((parent_vrf_score >> (8 * i)) & 0xff);
    sha.Write(score_le, 8);
    sha.Write(parent_anchor_hash.begin(), parent_anchor_hash.size());
    unsigned char height_le[4];
    height_le[0] = (unsigned char)(height & 0xff);
    height_le[1] = (unsigned char)((height >> 8) & 0xff);
    height_le[2] = (unsigned char)((height >> 16) & 0xff);
    height_le[3] = (unsigned char)((height >> 24) & 0xff);
    sha.Write(height_le, 4);
    uint256 out;
    sha.Finalize(out.begin());
    return out;
}

namespace {
//! The weighted ticket of a staker: H(seed || pubkey) interpreted as a 256-bit
//! integer, divided by the staker's stake weight. Lower is better, so more
//! stake (larger divisor) statistically yields a lower ticket and thus a
//! better (lower) rank.
arith_uint256 WeightedTicket(const uint256& seed, const CPubKey& pubkey, uint64_t weight)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss.write(MakeByteSpan(seed));
    ss.write(MakeByteSpan(pubkey));
    uint256 ticket = ss.GetSHA256();
    arith_uint256 value = UintToArith256(ticket);
    if (weight == 0) {
        return value; // unreachable for registered stakers (weight > 0)
    }
    value /= arith_uint256(weight);
    return value;
}
} // namespace

std::vector<CPubKey> PosSchedule(const StakeRegistry& registry, const uint256& seed)
{
    std::vector<std::pair<arith_uint256, CPubKey>> ranked;
    ranked.reserve(registry.Size());
    for (const auto& entry : registry.Weights()) {
        if (!PosIsEligibleStake(entry.second)) continue; // below the min-stake floor
        ranked.emplace_back(WeightedTicket(seed, entry.first, entry.second), entry.first);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second; // deterministic tie-break by pubkey
    });
    std::vector<CPubKey> schedule;
    schedule.reserve(ranked.size());
    for (const auto& r : ranked) schedule.push_back(r.second);
    return schedule;
}

std::optional<size_t> PosRank(const StakeRegistry& registry, const uint256& seed, const CPubKey& pubkey)
{
    if (!PosIsEligibleStake(registry.GetWeight(pubkey))) return std::nullopt;
    std::vector<CPubKey> schedule = PosSchedule(registry, seed);
    for (size_t i = 0; i < schedule.size(); ++i) {
        if (schedule[i] == pubkey) return i;
    }
    return std::nullopt;
}

std::vector<CPubKey> PosCommittee(const StakeRegistry& registry, const uint256& seed)
{
    std::vector<CPubKey> schedule = PosSchedule(registry, seed);
    size_t m = std::min<size_t>(schedule.size(), (size_t)std::max(g_pos_committee_size, 0));
    schedule.resize(m);
    return schedule;
}

int PosQuorum(size_t committee_size)
{
    if (committee_size == 0) return 0;
    return (int)(committee_size / 2) + 1;
}

CScript BuildPosChallenge(const CPubKey& pubkey)
{
    return CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
}

CScript BuildPosBlockChallenge(const CPubKey& leader, const std::vector<CPubKey>& committee, int quorum_override)
{
    // A committee of one adds nothing over the leader's own signature, so the
    // committee form is only used from size two upward.
    if (committee.size() <= 1) {
        return BuildPosChallenge(leader);
    }
    int quorum = quorum_override > 0 ? quorum_override : PosQuorum(committee.size());
    CScript script;
    script << ToByteVector(leader) << OP_CHECKSIGVERIFY;
    script << (int64_t)quorum;
    for (const CPubKey& member : committee) {
        script << ToByteVector(member);
    }
    script << (int64_t)committee.size() << OP_CHECKMULTISIG;
    return script;
}

std::optional<CPubKey> PosChallengeToPubKey(const CScript& challenge)
{
    // Expect exactly: PUSH(33|65 bytes) <pubkey> OP_CHECKSIG
    CScript::const_iterator pc = challenge.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!challenge.GetOp(pc, opcode, data)) return std::nullopt;
    if (data.empty()) return std::nullopt; // not a push of pubkey bytes
    CPubKey pubkey(data);
    if (!pubkey.IsFullyValid()) return std::nullopt;
    if (!challenge.GetOp(pc, opcode, data)) return std::nullopt;
    if (opcode != OP_CHECKSIG) return std::nullopt;
    if (pc != challenge.end()) return std::nullopt; // trailing data
    return pubkey;
}

namespace {
//! Decode an OP_1..OP_16 smallint, or -1 for anything else.
int DecodeSmallInt(opcodetype opcode)
{
    if (opcode >= OP_1 && opcode <= OP_16) {
        return (int)opcode - (int)(OP_1 - 1);
    }
    return -1;
}
} // namespace

CScript BuildPosAggChallenge(const CPubKey& leader, const std::vector<unsigned char>& agg_key32)
{
    return CScript() << OP_1 << ToByteVector(leader) << agg_key32;
}

std::optional<PosChallengeParts> ParsePosBlockChallenge(const CScript& challenge)
{
    // Leader-only form.
    if (auto leader = PosChallengeToPubKey(challenge)) {
        PosChallengeParts parts;
        parts.leader = *leader;
        return parts;
    }

    // Aggregate-committee form: OP_1 <leader> <agg_key(32)>. The leading OP_1
    // is a version marker that cannot start either other form (both begin
    // with a pubkey push), so parsing is unambiguous.
    {
        CScript::const_iterator pc = challenge.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;
        if (challenge.GetOp(pc, opcode, data) && opcode == OP_1) {
            PosChallengeParts parts;
            if (!challenge.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
            parts.leader = CPubKey(data);
            if (!parts.leader.IsFullyValid()) return std::nullopt;
            if (!challenge.GetOp(pc, opcode, data) || data.size() != 32) return std::nullopt;
            parts.agg_key = data;
            if (pc != challenge.end()) return std::nullopt; // trailing data
            return parts;
        }
    }

    // Committee form:
    // <leader> OP_CHECKSIGVERIFY <q> <c_1> ... <c_m> <m> OP_CHECKMULTISIG
    PosChallengeParts parts;
    CScript::const_iterator pc = challenge.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    if (!challenge.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    parts.leader = CPubKey(data);
    if (!parts.leader.IsFullyValid()) return std::nullopt;
    if (!challenge.GetOp(pc, opcode, data) || opcode != OP_CHECKSIGVERIFY) return std::nullopt;

    if (!challenge.GetOp(pc, opcode, data)) return std::nullopt;
    parts.quorum = DecodeSmallInt(opcode);
    if (parts.quorum < 1) return std::nullopt;

    // Pubkeys until the trailing <m> smallint.
    while (true) {
        if (!challenge.GetOp(pc, opcode, data)) return std::nullopt;
        if (data.empty()) {
            // Should be the committee size smallint.
            int m = DecodeSmallInt(opcode);
            if (m < 1 || (size_t)m != parts.committee.size()) return std::nullopt;
            break;
        }
        CPubKey member(data);
        if (!member.IsFullyValid()) return std::nullopt;
        if (parts.committee.size() >= (size_t)MAX_POS_COMMITTEE_SIZE) return std::nullopt;
        parts.committee.push_back(member);
    }
    if (parts.quorum > (int)parts.committee.size()) return std::nullopt;
    if (!challenge.GetOp(pc, opcode, data) || opcode != OP_CHECKMULTISIG) return std::nullopt;
    if (pc != challenge.end()) return std::nullopt; // trailing data
    return parts;
}

uint256 PosSeedForChild(const CBlockIndex* pindexPrev)
{
    if (pindexPrev == nullptr) return uint256();
    // m_pos_vrf_score is the parent leader's VRF beta prefix, set at acceptance
    // (SetPosForkChoiceKeys) and persisted; for genesis / non-VRF parents it is
    // the UINT64_MAX default, which is fine — the seed only needs determinism.
    return ComputePosSeed(pindexPrev->m_pos_vrf_score, pindexPrev->m_anchor_hash,
                          (uint32_t)(pindexPrev->nHeight + 1));
}

// --- VRF sortition (doc/sequentia/07-vrf.md §4) ---

namespace {
const unsigned char POS_VRF_TAG[6] = {'S', 'E', 'Q', 'V', 'R', 'F'};
const unsigned char POS_CMT_TAG[6] = {'S', 'E', 'Q', 'C', 'M', 'T'};
} // namespace

uint64_t PosTotalWeight(const StakeRegistry& registry)
{
    uint64_t total = 0;
    for (const auto& entry : registry.Weights()) {
        if (!PosIsEligibleStake(entry.second)) continue; // only eligible stake counts
        total += entry.second;
    }
    return total;
}

uint64_t PosVrfSlot(const uint256& beta, uint64_t weight, uint64_t total_weight)
{
    if (weight == 0 || total_weight == 0) return POS_VRF_MAX_SLOT;
    arith_uint256 q = UintToArith256(beta);
    q /= arith_uint256(weight);
    // top 64 bits of q, scaled by the total weight: fits in 128 bits.
    arith_uint256 slot_a = (q >> 192) * arith_uint256(total_weight);
    slot_a >>= 64;
    uint64_t slot = slot_a.GetLow64();
    return std::min<uint64_t>(slot, POS_VRF_MAX_SLOT);
}

CScript BuildPosVrfCommitment(const std::vector<unsigned char>& proof)
{
    std::vector<unsigned char> data(POS_VRF_TAG, POS_VRF_TAG + sizeof(POS_VRF_TAG));
    data.insert(data.end(), proof.begin(), proof.end());
    return CScript() << OP_RETURN << data;
}

std::optional<std::vector<unsigned char>> ExtractPosVrfProof(const CBlock& block)
{
    if (block.vtx.empty()) return std::nullopt;
    for (const CTxOut& out : block.vtx[0]->vout) {
        const CScript& spk = out.scriptPubKey;
        // Expect exactly: OP_RETURN PUSH(tag || proof)
        CScript::const_iterator pc = spk.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;
        if (!spk.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;
        if (!spk.GetOp(pc, opcode, data)) continue;
        if (data.size() != sizeof(POS_VRF_TAG) + VRF_PROOF_SIZE) continue;
        if (!std::equal(POS_VRF_TAG, POS_VRF_TAG + sizeof(POS_VRF_TAG), data.begin())) continue;
        return std::vector<unsigned char>(data.begin() + sizeof(POS_VRF_TAG), data.end());
    }
    return std::nullopt;
}

bool PosVrfIsCommitteeMember(const uint256& beta, uint64_t weight, uint64_t total_weight)
{
    if (!PosIsEligibleStake(weight)) return false; // below the min-stake floor
    return PosVrfSlot(beta, weight, total_weight) < (uint64_t)std::max(g_pos_committee_size, 0);
}

CScript BuildPosVrfMemberCommitment(const CPubKey& member, const std::vector<unsigned char>& proof)
{
    std::vector<unsigned char> data(POS_CMT_TAG, POS_CMT_TAG + sizeof(POS_CMT_TAG));
    data.insert(data.end(), member.begin(), member.end());
    data.insert(data.end(), proof.begin(), proof.end());
    return CScript() << OP_RETURN << data;
}

std::vector<PosVrfMember> ExtractPosVrfMembers(const CBlock& block)
{
    std::vector<PosVrfMember> members;
    if (block.vtx.empty()) return members;
    for (const CTxOut& out : block.vtx[0]->vout) {
        const CScript& spk = out.scriptPubKey;
        CScript::const_iterator pc = spk.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;
        if (!spk.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;
        if (!spk.GetOp(pc, opcode, data)) continue;
        if (data.size() != sizeof(POS_CMT_TAG) + CPubKey::COMPRESSED_SIZE + VRF_PROOF_SIZE) continue;
        if (!std::equal(POS_CMT_TAG, POS_CMT_TAG + sizeof(POS_CMT_TAG), data.begin())) continue;
        PosVrfMember member;
        member.pubkey = CPubKey(data.begin() + sizeof(POS_CMT_TAG),
                                data.begin() + sizeof(POS_CMT_TAG) + CPubKey::COMPRESSED_SIZE);
        if (!member.pubkey.IsValid()) continue;
        member.proof.assign(data.begin() + sizeof(POS_CMT_TAG) + CPubKey::COMPRESSED_SIZE, data.end());
        members.push_back(member);
    }
    return members;
}

// --- On-chain stake registration (locked staking outputs) ---

CScript BuildStakeScript(const CPubKey& pubkey, uint32_t csv_blocks)
{
    return CScript() << (int64_t)csv_blocks << OP_CHECKSEQUENCEVERIFY << OP_DROP
                     << ToByteVector(pubkey) << OP_CHECKSIG;
}

std::optional<std::pair<CPubKey, uint32_t>> ParseStakeScript(const CScript& script)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    // <csv>: an OP_1..OP_16 smallint or a minimal CScriptNum push, carrying a
    // BIP68 relative-locktime value — either height-based (no type flag) or
    // time-based (CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG set, 512-second units).
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    int64_t csv = -1;
    if (opcode >= OP_1 && opcode <= OP_16) {
        csv = (int)opcode - (int)(OP_1 - 1);
    } else if (!data.empty() && data.size() <= 4) {
        try {
            csv = CScriptNum(data, /*fRequireMinimal=*/true).getint();
        } catch (const scriptnum_error&) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (csv < 1) return std::nullopt; // must be a positive relative lock
    // Canonical for staking: only the type flag and the 16-bit value may be
    // set (no disable flag, no stray reserved bits) — so the encoding is
    // unambiguous across nodes.
    const uint32_t allowed = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | CTxIn::SEQUENCE_LOCKTIME_MASK;
    if ((uint64_t)csv & ~(uint64_t)allowed) return std::nullopt;
    if ((csv & CTxIn::SEQUENCE_LOCKTIME_MASK) == 0) return std::nullopt; // value must be non-zero

    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSEQUENCEVERIFY) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    CPubKey pubkey(data);
    if (!pubkey.IsFullyValid()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSIG) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return std::make_pair(pubkey, (uint32_t)csv);
}

//! The effective slot interval in seconds (>=1), used to put height- and
//! time-based locks on one comparable axis.
static int64_t EffectiveSlotSeconds()
{
    return g_pos_slot_interval > 0 ? g_pos_slot_interval : 1;
}

std::optional<int64_t> PosStakeLockSeconds(uint32_t csv)
{
    if (csv & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) return std::nullopt; // not a relative lock
    const uint32_t allowed = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | CTxIn::SEQUENCE_LOCKTIME_MASK;
    if (csv & ~allowed) return std::nullopt; // stray reserved bits
    uint32_t value = csv & CTxIn::SEQUENCE_LOCKTIME_MASK;
    if (value == 0) return std::nullopt;
    if (csv & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
        // Time-based: 512-second units (the only form that can express a lock
        // beyond the 16-bit height cap, e.g. the whitepaper's >2016-BTC-block
        // unbonding at fast slot intervals).
        return (int64_t)value << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY;
    }
    return (int64_t)value * EffectiveSlotSeconds(); // height-based
}

int64_t PosRequiredUnbondingSeconds()
{
    return (int64_t)g_pos_unbonding_period * EffectiveSlotSeconds();
}

std::optional<std::pair<CPubKey, uint64_t>> StakeFromTxOut(const CTxOut& out)
{
    // Stake must be a transparent amount of the policy asset: confidential
    // outputs hide the amount, so they cannot carry verifiable weight.
    if (!out.nValue.IsExplicit() || !out.nAsset.IsExplicit()) return std::nullopt;
    if (out.nAsset.GetAsset() != ::policyAsset) return std::nullopt;
    CAmount amount = out.nValue.GetAmount();
    if (amount <= 0) return std::nullopt;
    auto parsed = ParseStakeScript(out.scriptPubKey);
    if (!parsed) return std::nullopt;
    // The unbonding lock must be at least the chain minimum, compared as a
    // wall-clock duration so height- and time-based CSV are judged uniformly.
    auto lock = PosStakeLockSeconds(parsed->second);
    if (!lock || *lock < PosRequiredUnbondingSeconds()) return std::nullopt;
    return std::make_pair(parsed->first, (uint64_t)amount);
}

void PosApplyBlockStake(const CBlock& block, const CBlockUndo& undo)
{
    StakeRegistry& registry = StakeRegistry::GetInstance();
    // New staking outputs add weight.
    for (const CTransactionRef& tx : block.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto stake = StakeFromTxOut(out)) {
                registry.AddUtxoStake(stake->first, stake->second);
                LogPrintf("PoS: staking output adds %llu to %s\n", (unsigned long long)stake->second, HexStr(stake->first));
            }
        }
    }
    // Spent staking outputs (recorded in the block's undo data) remove weight.
    for (const CTxUndo& txundo : undo.vtxundo) {
        for (const Coin& coin : txundo.vprevout) {
            if (auto stake = StakeFromTxOut(coin.out)) {
                registry.SubUtxoStake(stake->first, stake->second);
                LogPrintf("PoS: staking output spend removes %llu from %s\n", (unsigned long long)stake->second, HexStr(stake->first));
            }
        }
    }
}

void PosRevertBlockStake(const CBlock& block, const CBlockUndo& undo)
{
    StakeRegistry& registry = StakeRegistry::GetInstance();
    // Exact inverse of PosApplyBlockStake, which added created outputs then
    // subtracted spent ones. Undoing in reverse order — re-add the spent
    // outputs FIRST, then subtract the created ones — keeps every per-pubkey
    // running total non-negative, so it is a true inverse even for a staking
    // output created and spent within this same block (where the pubkey's
    // post-block weight is zero and its map entry was erased). Doing the
    // subtraction first there would hit SubUtxoStake's underflow guard and
    // corrupt the registry.
    for (const CTxUndo& txundo : undo.vtxundo) {
        for (const Coin& coin : txundo.vprevout) {
            if (auto stake = StakeFromTxOut(coin.out)) {
                registry.AddUtxoStake(stake->first, stake->second);
            }
        }
    }
    for (const CTransactionRef& tx : block.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto stake = StakeFromTxOut(out)) {
                registry.SubUtxoStake(stake->first, stake->second);
            }
        }
    }
}

bool RebuildUtxoStake(CCoinsView& view)
{
    std::map<CPubKey, uint64_t> utxo_stake;
    std::unique_ptr<CCoinsViewCursor> pcursor(view.Cursor());
    if (!pcursor) return false;
    while (pcursor->Valid()) {
        COutPoint key;
        Coin coin;
        if (!pcursor->GetKey(key) || !pcursor->GetValue(coin)) return false;
        // Count ALL staking outputs, including genesis (height 0) ones. This is
        // what lets a chain bootstrap from a genesis-seeded staking output with
        // no -staker config layer (the founder's pre-mined stake): the registry
        // is a pure function of the UTXO set. Consistency across fresh / IBD /
        // restarted nodes holds because the UTXO layer is *always* re-derived by
        // this scan at init (SetUtxoStake replaces it wholesale), and the
        // incremental path (PosApply/RevertBlockStake) never adds genesis (it is
        // special-cased out of ConnectBlock) — it only ever *spends* a genesis
        // staking output, which this scan also reflects (the coin is gone).
        if (auto stake = StakeFromTxOut(coin.out)) {
            utxo_stake[stake->first] += stake->second;
        }
        pcursor->Next();
    }
    StakeRegistry::GetInstance().SetUtxoStake(std::move(utxo_stake));
    return true;
}
