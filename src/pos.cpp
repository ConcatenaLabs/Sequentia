// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos.h>

#include <arith_uint256.h>
#include <chain.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <util/strencodings.h>

#include <algorithm>

bool g_con_pos = false;
int64_t g_pos_slot_interval = DEFAULT_POS_SLOT_INTERVAL;

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

CScript BuildPosChallenge(const CPubKey& pubkey)
{
    return CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
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

uint256 PosSeedForChild(const CBlockIndex* pindexPrev)
{
    if (pindexPrev == nullptr) return uint256();
    return ComputePosSeed(pindexPrev->GetBlockHash(), pindexPrev->m_anchor_hash,
                          (uint32_t)(pindexPrev->nHeight + 1));
}
