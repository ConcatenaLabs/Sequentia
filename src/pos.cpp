// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <vrf.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <util/strencodings.h>

#include <algorithm>

bool g_con_pos = false;
int64_t g_pos_slot_interval = DEFAULT_POS_SLOT_INTERVAL;
int g_pos_committee_size = DEFAULT_POS_COMMITTEE_SIZE;
bool g_pos_vrf = false;

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

uint256 ComputePosSeed(const uint256& parent_hash, const uint256& parent_anchor_hash, uint32_t height)
{
    CSHA256 sha;
    sha.Write(parent_hash.begin(), parent_hash.size());
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
    if (registry.GetWeight(pubkey) == 0) return std::nullopt;
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

CScript BuildPosBlockChallenge(const CPubKey& leader, const std::vector<CPubKey>& committee)
{
    // A committee of one adds nothing over the leader's own signature, so the
    // committee form is only used from size two upward.
    if (committee.size() <= 1) {
        return BuildPosChallenge(leader);
    }
    CScript script;
    script << ToByteVector(leader) << OP_CHECKSIGVERIFY;
    script << (int64_t)PosQuorum(committee.size());
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

std::optional<PosChallengeParts> ParsePosBlockChallenge(const CScript& challenge)
{
    // Leader-only form.
    if (auto leader = PosChallengeToPubKey(challenge)) {
        PosChallengeParts parts;
        parts.leader = *leader;
        return parts;
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
    return ComputePosSeed(pindexPrev->GetBlockHash(), pindexPrev->m_anchor_hash,
                          (uint32_t)(pindexPrev->nHeight + 1));
}

// --- VRF sortition (doc/sequentia/07-vrf.md §4) ---

namespace {
const unsigned char POS_VRF_TAG[6] = {'S', 'E', 'Q', 'V', 'R', 'F'};
} // namespace

uint64_t PosTotalWeight(const StakeRegistry& registry)
{
    uint64_t total = 0;
    for (const auto& entry : registry.Weights()) {
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
