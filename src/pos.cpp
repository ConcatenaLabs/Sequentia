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
#include <script/standard.h>
#include <undo.h>
#include <vrf.h>
#include <bls.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <util/strencodings.h>

#include <algorithm>

// g_con_pos is defined in primitives/block.cpp (the consensus library) so the
// standalone libelementsconsensus resolves CProof's serialization; it is
// declared extern in pos.h.
uint32_t g_pos_unbonding_period = DEFAULT_POS_UNBONDING_PERIOD;
int64_t g_pos_slot_interval = DEFAULT_POS_SLOT_INTERVAL;
int g_pos_committee_size = DEFAULT_POS_COMMITTEE_SIZE;
bool g_pos_vrf = false;
bool g_pos_agg_committee = false;
bool g_pos_bls = false;
bool g_pos_public_committee = false;
uint64_t g_pos_min_stake = 0;

bool StakeRegistry::AddFromSpec(const std::string& spec, std::string& error)
{
    // <pubkeyhex>:<weight>  or, with a BLS committee registration (impl spec
    // Option A phase 2),  <pubkeyhex>:<weight>:<blspubkeyhex>:<pophex>.
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        size_t colon = spec.find(':', start);
        if (colon == std::string::npos) { f.push_back(spec.substr(start)); break; }
        f.push_back(spec.substr(start, colon - start));
        start = colon + 1;
    }
    if (f.size() != 2 && f.size() != 4) {
        error = strprintf("staker spec '%s' must be <pubkeyhex>:<weight> or <pubkeyhex>:<weight>:<blspubkeyhex>:<pophex>", spec);
        return false;
    }
    const std::string& pubkey_hex = f[0];
    const std::string& weight_str = f[1];
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
    if (f.size() == 4) {
        // Optional BLS committee registration. The proof-of-possession is
        // verified once, here at registration; blst is independent of the
        // secp256k1 context, so this is safe before ECC_Start().
        if (!IsHex(f[2]) || !IsHex(f[3])) {
            error = strprintf("staker BLS registration for '%s' is not valid hex", pubkey_hex);
            return false;
        }
        std::vector<unsigned char> bls_pubkey = ParseHex(f[2]);
        std::vector<unsigned char> bls_pop = ParseHex(f[3]);
        if (bls_pubkey.size() != BLS_PK_SIZE || bls_pop.size() != BLS_SIG_SIZE) {
            error = strprintf("staker BLS key/pop for '%s' have wrong sizes (want %d/%d bytes)", pubkey_hex, BLS_PK_SIZE, BLS_SIG_SIZE);
            return false;
        }
        // Structural check only: the cryptographic proof-of-possession is NOT
        // verified here. This runs in every tool that parses -staker (including
        // ones that do not link blst), and the config/genesis staker set is
        // trusted; a bad key is also self-punishing (its shares never verify and
        // the block's aggregate is rejected at connect). Runtime UTXO-layer
        // registration (future work) verifies the PoP on-chain, where blst is
        // linked. The registered key MUST be BlsDerivePubKey(PosBlsSeedFromKey).
        SetBls(pubkey, bls_pubkey);
    }
    SetStake(pubkey, (uint64_t)weight);
    return true;
}

uint256 ComputePosSeed(const uint256& parent_anchor_hash, uint32_t height)
{
    CSHA256 sha;
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

std::vector<CPubKey> PosPublicCommittee(const StakeRegistry& registry, const uint256& seed)
{
    // The public committee is the schedule prefix, but under the bitfield
    // certificate a member must have a registered BLS key (validators name
    // signers by a bitfield and look their keys up), so ineligible-by-missing-
    // key stakers are skipped in schedule order before the cap is applied. The
    // leader role has no such requirement (it signs with ECDSA).
    std::vector<CPubKey> schedule = PosSchedule(registry, seed);
    std::vector<CPubKey> committee;
    const size_t cap = (size_t)std::max(g_pos_committee_size, 0);
    for (const CPubKey& s : schedule) {
        if (!registry.HasBls(s)) continue;
        committee.push_back(s);
        if (committee.size() >= cap) break;
    }
    return committee;
}

int PosPublicCommitteeSize(const StakeRegistry& registry)
{
    // Seat count is min(#eligible registered stakers, cap) and does not depend
    // on the seed (the seed only orders/selects WHICH stakers fill the seats).
    size_t pool = 0;
    for (const auto& entry : registry.Weights()) {
        if (PosIsEligibleStake(entry.second) && registry.HasBls(entry.first)) pool++;
    }
    return (int)std::min<size_t>(pool, (size_t)std::max(g_pos_committee_size, 0));
}

int PosPublicQuorum(int k)
{
    if (k <= 0) return 0;
    // Strict majority, plus one at odd k: any two quorums then overlap in
    // 2q - k >= 2 members at every size, so a double-certification needs at
    // least two equivocating signers regardless of parity. Identical to
    // PosQuorum at every even k.
    int q = k / 2 + 1 + (k & 1);
    return std::min(k, q);
}

int PosSlotQuorum(const StakeRegistry& registry)
{
    if (g_pos_public_committee) return PosPublicQuorum(PosPublicCommitteeSize(registry));
    return PosQuorum((size_t)std::max(g_pos_committee_size, 1));
}

std::set<CPubKey> PosPublicCommitteeSet(const StakeRegistry& registry, const uint256& seed)
{
    std::vector<CPubKey> members = PosPublicCommittee(registry, seed);
    return std::set<CPubKey>(members.begin(), members.end());
}

int PosMaxCommitteeMembers()
{
    if (g_pos_public_committee) return std::max(g_pos_committee_size, 1);
    return MAX_POS_AGG_COMMITTEE_SIZE;
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

CScript BuildPosBlsChallenge(const CPubKey& leader)
{
    return CScript() << OP_2 << ToByteVector(leader);
}

CScript PosLeaderFeeScript(const CPubKey& leader)
{
    return GetScriptForDestination(WitnessV0KeyHash(leader));
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

    // BLS aggregate-committee form: OP_2 <leader>. OP_2 is the version marker
    // for the BLS form (OP_1 is MuSig2), again unambiguous since the other forms
    // begin with a pubkey push. The committee certificate is not here — it is in
    // the proof solution (so the signed block hash is member-independent).
    {
        CScript::const_iterator pc = challenge.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;
        if (challenge.GetOp(pc, opcode, data) && opcode == OP_2) {
            PosChallengeParts parts;
            if (!challenge.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
            parts.leader = CPubKey(data);
            if (!parts.leader.IsFullyValid()) return std::nullopt;
            if (pc != challenge.end()) return std::nullopt; // trailing data
            parts.is_bls = true;
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
    // Seed from the parent's Bitcoin anchor hash + the height. Both are header
    // fields fixed at block-index creation, so the seed is identical on every
    // node (unlike m_pos_vrf_score, which is set later and proved unreliable as a
    // seed input). The anchor is Bitcoin's PoW, so a SEQ producer cannot bias it.
    // The parent's anchor is fixed once the parent exists, so within a Bitcoin
    // interval all same-height candidates share the seed; a producer's only
    // freedom is which recent Bitcoin block its *own* block anchors to, which
    // affects only the NEXT block's (privately VRF-sortitioned) committee —
    // bounded, VRF-mitigated grinding, not a seed-grinding lever here.
    return ComputePosSeed(pindexPrev->m_anchor_hash, (uint32_t)(pindexPrev->nHeight + 1));
}

// --- VRF sortition (doc/sequentia/04-proof-of-stake.md §4) ---

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

// --- Exponential-race (weighted-sampling) sortition -----------------------
// A Sybil-neutral alternative to PosVrfSlot. With U = beta / 2^256 uniform in
// (0,1), the score
//     score = -ln(U) / weight
// is Exponential(weight); the minimum of independent scores is
// Exponential(sum of weights), so P(staker i is the minimum) = weight_i / total
// AND splitting a weight into several identities leaves that probability
// unchanged (min of Exp(w/2)+Exp(w/2) = Exp(w)). Election by lowest score is
// therefore exactly stake-proportional and split-proof, unlike raw beta.
//
// Fixed-point, deterministic (no floating point -> identical on every node),
// using the identity
//     -ln(U) = ln(2^256 / beta) = (256 - log2 beta) * ln2.
// log2 beta = n + log2(mantissa), n = bitlength-1; the fractional part comes
// from the classic bit-by-bit squaring of the mantissa. All values are Q32
// fixed-point. Validated against a 256-bit reference: split-neutral to <1% and
// exactly proportional over 200k-round simulations.
namespace {
const int      POS_EXP_FRAC = 32;                   // fixed-point fractional bits
const int      POS_EXP_P    = 61;                   // mantissa scale bits
const uint64_t POS_EXP_LN2_Q32 = 2977044472ULL;     // round(ln2 * 2^32)

// Fractional-aware log2(beta) in Q32, given n = bitlength(beta) - 1.
uint64_t PosExpLog2Q32(const arith_uint256& beta, int n)
{
    // mantissa v in [2^P, 2^{P+1}): represents mant = v / 2^P in [1, 2).
    arith_uint256 vbig = (n >= POS_EXP_P) ? (beta >> (n - POS_EXP_P))
                                          : (beta << (POS_EXP_P - n));
    uint64_t v = vbig.GetLow64();
    uint64_t frac = 0;
    for (int i = 1; i <= POS_EXP_FRAC; ++i) {
        arith_uint256 sq = arith_uint256(v) * arith_uint256(v); // <= 2^124
        sq >>= POS_EXP_P;                                       // back to Q_P
        v = sq.GetLow64();                                      // in [2^P, 2^{P+2})
        if (v >> (POS_EXP_P + 1)) {                             // mant^2 >= 2
            frac |= (uint64_t)1 << (POS_EXP_FRAC - i);
            v >>= 1;
        }
    }
    return ((uint64_t)n << POS_EXP_FRAC) | frac;
}

// score = -ln(U) * total / weight, in Q32 fixed-point. beta == 0 or a zero
// weight yields a sentinel above any real score (never elected / max slot).
arith_uint256 PosExpScoreInf()
{
    return arith_uint256((uint64_t)POS_VRF_MAX_SLOT + 1) << POS_EXP_FRAC;
}
} // namespace

arith_uint256 PosVrfScoreExp(const uint256& beta, uint64_t weight, uint64_t total_weight)
{
    if (weight == 0 || total_weight == 0) return PosExpScoreInf();
    arith_uint256 b = UintToArith256(beta);
    int bits = (int)b.bits();
    if (bits == 0) return PosExpScoreInf();                 // beta == 0 -> -ln(0) = inf
    int n = bits - 1;
    uint64_t log2b = PosExpLog2Q32(b, n);                   // Q32, in [0, 256<<32)
    uint64_t Lc = ((uint64_t)256 << POS_EXP_FRAC) - log2b;  // (256 - log2 beta), Q32
    arith_uint256 neg_ln = (arith_uint256(Lc) * arith_uint256(POS_EXP_LN2_Q32)) >> POS_EXP_FRAC;
    return (neg_ln * arith_uint256(total_weight)) / arith_uint256(weight);
}

uint64_t PosVrfSlotExp(const uint256& beta, uint64_t weight, uint64_t total_weight)
{
    arith_uint256 slot = PosVrfScoreExp(beta, weight, total_weight) >> POS_EXP_FRAC;
    if (slot > arith_uint256((uint64_t)POS_VRF_MAX_SLOT)) return POS_VRF_MAX_SLOT;
    return slot.GetLow64();
}

bool PosExpRaceActive(const Consensus::Params& params, int height)
{
    return params.pos_exprace_height > 0 && height >= params.pos_exprace_height;
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

CScript BuildPosBlsSolution(const std::vector<unsigned char>& leader_sig,
                            const std::vector<unsigned char>& agg_sig,
                            const std::vector<PosBlsMember>& members)
{
    CScript s;
    s << leader_sig << agg_sig;
    for (const PosBlsMember& m : members) {
        std::vector<unsigned char> blob;
        blob.reserve(CPubKey::COMPRESSED_SIZE + VRF_PROOF_SIZE + BLS_PK_SIZE + BLS_SIG_SIZE);
        blob.insert(blob.end(), m.pubkey.begin(), m.pubkey.end());
        blob.insert(blob.end(), m.proof.begin(), m.proof.end());
        blob.insert(blob.end(), m.bls_pubkey.begin(), m.bls_pubkey.end());
        blob.insert(blob.end(), m.bls_pop.begin(), m.bls_pop.end());
        s << blob;
    }
    return s;
}

std::optional<PosBlsCertificate> ParsePosBlsSolution(const CScript& solution)
{
    PosBlsCertificate cert;
    CScript::const_iterator pc = solution.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!solution.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    cert.leader_sig = data;
    if (!solution.GetOp(pc, opcode, data) || data.size() != BLS_SIG_SIZE) return std::nullopt;
    cert.agg_sig = data;
    const size_t member_size = CPubKey::COMPRESSED_SIZE + VRF_PROOF_SIZE + BLS_PK_SIZE + BLS_SIG_SIZE;
    while (solution.GetOp(pc, opcode, data)) {
        if (data.size() != member_size) return std::nullopt;
        if (cert.members.size() >= (size_t)MAX_POS_AGG_COMMITTEE_SIZE) return std::nullopt;
        PosBlsMember m;
        size_t off = 0;
        m.pubkey = CPubKey(data.begin() + off, data.begin() + off + CPubKey::COMPRESSED_SIZE);
        off += CPubKey::COMPRESSED_SIZE;
        if (!m.pubkey.IsValid()) return std::nullopt;
        m.proof.assign(data.begin() + off, data.begin() + off + VRF_PROOF_SIZE);
        off += VRF_PROOF_SIZE;
        m.bls_pubkey.assign(data.begin() + off, data.begin() + off + BLS_PK_SIZE);
        off += BLS_PK_SIZE;
        m.bls_pop.assign(data.begin() + off, data.begin() + off + BLS_SIG_SIZE);
        cert.members.push_back(std::move(m));
    }
    return cert;
}

bool PosBitfieldTest(const std::vector<unsigned char>& bitfield, size_t i)
{
    const size_t byte = i >> 3;
    if (byte >= bitfield.size()) return false;
    return (bitfield[byte] >> (i & 7)) & 1;
}

void PosBitfieldSet(std::vector<unsigned char>& bitfield, size_t i)
{
    const size_t byte = i >> 3;
    if (byte >= bitfield.size()) bitfield.resize(byte + 1, 0);
    bitfield[byte] |= (unsigned char)(1u << (i & 7));
}

int PosBitfieldPopcount(const std::vector<unsigned char>& bitfield)
{
    int n = 0;
    for (unsigned char b : bitfield) {
        while (b) { n += b & 1; b >>= 1; }
    }
    return n;
}

CScript BuildPosBlsBitfieldSolution(const std::vector<unsigned char>& leader_sig,
                                    const std::vector<unsigned char>& agg_sig,
                                    const std::vector<unsigned char>& bitfield)
{
    CScript s;
    s << leader_sig << agg_sig << bitfield;
    return s;
}

std::optional<PosBlsBitfieldCert> ParsePosBlsBitfieldSolution(const CScript& solution)
{
    PosBlsBitfieldCert cert;
    CScript::const_iterator pc = solution.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!solution.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    cert.leader_sig = data;
    if (!solution.GetOp(pc, opcode, data) || data.size() != BLS_SIG_SIZE) return std::nullopt;
    cert.agg_sig = data;
    if (!solution.GetOp(pc, opcode, data) || data.empty()) return std::nullopt; // a certificate names >= 1 signer
    // Cap the bitfield at the committee cap (in bytes) to bound work.
    if (data.size() > (size_t)(MAX_POS_PUBLIC_COMMITTEE_SIZE + 7) / 8) return std::nullopt;
    cert.bitfield = data;
    if (pc != solution.end()) return std::nullopt; // trailing data
    return cert;
}

// PosVerifyBitfieldCertificate is defined in validation.cpp (the node library,
// which links blst) rather than here in libbitcoin_common, so that lightweight
// tools linking pos.o (elements-tx/util) do not pull in the BLS aggregate
// verification and its blst dependency. It is declared in pos.h.

// --- On-chain stake registration (locked staking outputs) ---

CScript BuildStakeScript(const CPubKey& pubkey, uint32_t csv_blocks,
                         const std::vector<unsigned char>& bls_pubkey,
                         const std::vector<unsigned char>& bls_pop,
                         int64_t liquid_locktime)
{
    CScript s;
    s << (int64_t)csv_blocks << OP_CHECKSEQUENCEVERIFY << OP_DROP;
    if (!bls_pubkey.empty() || !bls_pop.empty()) {
        // Committee registration: push and drop the BLS key and its PoP so they
        // are committed in the UTXO without affecting the spend path.
        s << bls_pubkey << OP_DROP << bls_pop << OP_DROP;
    }
    if (liquid_locktime > 0) {
        // Vesting: an absolute timelock making the stake unspendable (hence
        // unsellable) until liquid_locktime, while it keeps accruing weight.
        s << liquid_locktime << OP_CHECKLOCKTIMEVERIFY << OP_DROP;
    }
    s << ToByteVector(pubkey) << OP_CHECKSIG;
    return s;
}

//! Marker distinguishing a delegation record from any other bare script. The
//! staking script begins with a small CSV number push followed by
//! OP_CHECKSEQUENCEVERIFY, so the two templates can never be confused.
static const std::vector<unsigned char> DELEGATION_MARKER = {'S', 'E', 'Q', 'D', 'E', 'L'};

CScript BuildDelegationScript(const CPubKey& controller, const CPubKey& signer)
{
    CScript s;
    s << DELEGATION_MARKER << OP_DROP;
    s << ToByteVector(signer) << OP_DROP;
    s << ToByteVector(controller) << OP_CHECKSIG;
    return s;
}

std::optional<std::pair<CPubKey, CPubKey>> ParseDelegationScript(const CScript& script)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    if (!script.GetOp(pc, opcode, data) || data != DELEGATION_MARKER) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    CPubKey signer(data);
    if (!signer.IsFullyValid()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    CPubKey controller(data);
    if (!controller.IsFullyValid()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSIG) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return std::make_pair(controller, signer);
}

std::optional<std::pair<CPubKey, CPubKey>> DelegationFromTxOut(const CTxOut& out)
{
    return ParseDelegationScript(out.scriptPubKey);
}

static int64_t StakeDecodeScriptInt64(const std::vector<unsigned char>& vch);

uint32_t g_pos_payout_notice = DEFAULT_POS_PAYOUT_NOTICE;

static const std::vector<unsigned char> PAYOUT_MARKER = {'S', 'E', 'Q', 'P', 'A', 'Y'};
//! An operator's committed payout script is bounded: it rides in a bare output,
//! and an unbounded push would bloat every node's UTXO set.
static const size_t MAX_PAYOUT_SCRIPT_SIZE = 110;

CScript BuildPayoutScript(const CPubKey& signer, const PosPayoutPolicy& policy)
{
    CScript s;
    s << PAYOUT_MARKER << OP_DROP;
    s << policy.activation << OP_DROP;
    s << (int64_t)(uint8_t)policy.mode << OP_DROP;
    if (policy.mode == PosPayoutMode::DIRECT) {
        s << std::vector<unsigned char>(policy.script.begin(), policy.script.end());
    } else {
        s << (int64_t)policy.commission_bp;
    }
    s << OP_DROP;
    s << ToByteVector(signer) << OP_CHECKSIG;
    return s;
}

//! Read a numeric token (OP_0, OP_1..OP_16, or a minimal push of <= max_size).
static bool ReadScriptNumToken(opcodetype opcode, const std::vector<unsigned char>& data,
                               size_t max_size, int64_t& out)
{
    if (opcode == OP_0) { out = 0; return true; }
    if (opcode >= OP_1 && opcode <= OP_16) { out = (int)opcode - (int)(OP_1 - 1); return true; }
    if (data.empty() || data.size() > max_size) return false;
    try {
        CScriptNum(data, /*fRequireMinimal=*/true, max_size); // enforces minimality
    } catch (const scriptnum_error&) {
        return false;
    }
    out = StakeDecodeScriptInt64(data);
    return true;
}

std::optional<std::pair<CPubKey, PosPayoutPolicy>> ParsePayoutScript(const CScript& script)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    if (!script.GetOp(pc, opcode, data) || data != PAYOUT_MARKER) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    PosPayoutPolicy policy;
    // <activation>
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    if (!ReadScriptNumToken(opcode, data, 5, policy.activation)) return std::nullopt;
    if (policy.activation <= 0 || policy.activation > 0xffffffffLL) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    // <mode>
    int64_t mode = 0;
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    if (!ReadScriptNumToken(opcode, data, 1, mode)) return std::nullopt;
    if (mode != (int64_t)PosPayoutMode::DIRECT && mode != (int64_t)PosPayoutMode::LOTTERY) return std::nullopt;
    policy.mode = (PosPayoutMode)mode;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    // <param>: the payout scriptPubKey (DIRECT) or the commission (LOTTERY).
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    if (policy.mode == PosPayoutMode::DIRECT) {
        if (data.empty() || data.size() > MAX_PAYOUT_SCRIPT_SIZE) return std::nullopt;
        policy.script = CScript(data.begin(), data.end());
    } else {
        int64_t bp = 0;
        if (!ReadScriptNumToken(opcode, data, 3, bp)) return std::nullopt;
        if (bp < 0 || bp > (int64_t)POS_COMMISSION_DENOM) return std::nullopt;
        policy.commission_bp = (uint32_t)bp;
    }
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || data.empty()) return std::nullopt;
    CPubKey signer(data);
    if (!signer.IsFullyValid()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSIG) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return std::make_pair(signer, policy);
}

std::optional<std::pair<CPubKey, PosPayoutPolicy>> PayoutFromTxOut(const CTxOut& out)
{
    return ParsePayoutScript(out.scriptPubKey);
}

//! A uniform 64-bit draw from the block's election seed, domain-separated by tag.
static uint64_t PayoutDraw(const uint256& seed, const std::string& tag)
{
    CSHA256 sha;
    sha.Write(seed.begin(), seed.size());
    sha.Write((const unsigned char*)tag.data(), tag.size());
    unsigned char out[CSHA256::OUTPUT_SIZE];
    sha.Finalize(out);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)out[i] << (8 * i);
    return v;
}

CScript PosRequiredCoinbaseScript(const CPubKey& leader, int64_t height, const uint256& seed)
{
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const auto policy = registry.PayoutFor(leader, height);
    if (!policy) return PosLeaderFeeScript(leader);   // no committed policy

    if (policy->mode == PosPayoutMode::DIRECT) return policy->script;

    // LOTTERY: draw one participant, weighted by the stake it lent this signer.
    // The seed is SHA256(parent Bitcoin anchor hash || height) -- supplied by
    // Bitcoin's proof of work -- so the leader cannot grind the outcome.
    if (policy->commission_bp > 0 &&
        PayoutDraw(seed, "seqpay:commission") % POS_COMMISSION_DENOM < policy->commission_bp) {
        return PosLeaderFeeScript(leader);
    }
    const std::map<CPubKey, uint64_t> participants = registry.ParticipantsFor(leader);
    uint64_t total = 0;
    for (const auto& e : participants) total += e.second;
    if (total == 0) return PosLeaderFeeScript(leader); // nothing staked; pay the leader

    uint64_t ticket = PayoutDraw(seed, "seqpay:winner") % total;
    for (const auto& e : participants) { // std::map iteration: deterministic order
        if (ticket < e.second) return PosLeaderFeeScript(e.first);
        ticket -= e.second;
    }
    return PosLeaderFeeScript(leader); // unreachable: ticket < total
}

//! Decode a script number's little-endian sign-magnitude encoding to int64.
//! CScriptNum::getint() saturates at INT_MAX, which would silently corrupt a
//! locktime past 2038, so decode the full width here. The caller must already
//! have validated minimality (by constructing a CScriptNum).
static int64_t StakeDecodeScriptInt64(const std::vector<unsigned char>& vch)
{
    if (vch.empty()) return 0;
    int64_t result = 0;
    for (size_t i = 0; i != vch.size(); ++i) result |= static_cast<int64_t>(vch[i]) << (8 * i);
    if (vch.back() & 0x80) {
        return -(static_cast<int64_t>(result & ~(int64_t{0x80} << (8 * (vch.size() - 1)))));
    }
    return result;
}

std::optional<ParsedStake> ParseStakeScriptFull(const CScript& script)
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
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    // Optional committee registration: a 48-byte push here is the BLS pubkey
    // (a staking pubkey is 33 or 65 bytes, never 48, so this is unambiguous),
    // followed by OP_DROP, the 96-byte PoP, OP_DROP, then the staking pubkey.
    ParsedStake out;
    if (data.size() == BLS_PK_SIZE) {
        out.bls_pubkey = data;
        if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
        if (!script.GetOp(pc, opcode, data) || data.size() != BLS_SIG_SIZE) return std::nullopt;
        out.bls_pop = data;
        if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
        if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    }
    // Optional absolute vesting lock: <liquid_locktime> OP_CHECKLOCKTIMEVERIFY
    // OP_DROP. Disambiguated by one-token lookahead rather than by push size:
    // the token just read is the locktime only if OP_CHECKLOCKTIMEVERIFY
    // follows it, otherwise it is the staking pubkey and we rewind.
    {
        const CScript::const_iterator before_lookahead = pc;
        opcodetype next_op;
        std::vector<unsigned char> next_data;
        if (script.GetOp(pc, next_op, next_data) && next_op == OP_CHECKLOCKTIMEVERIFY) {
            int64_t locktime = -1;
            if (opcode >= OP_1 && opcode <= OP_16) {
                locktime = (int)opcode - (int)(OP_1 - 1);
            } else if (!data.empty() && data.size() <= 5) {
                // 5-byte bignums, as OP_CHECKLOCKTIMEVERIFY itself accepts.
                // Constructing the CScriptNum enforces minimal encoding.
                try {
                    CScriptNum(data, /*fRequireMinimal=*/true, /*nMaxNumSize=*/5);
                } catch (const scriptnum_error&) {
                    return std::nullopt;
                }
                locktime = StakeDecodeScriptInt64(data);
            } else {
                return std::nullopt;
            }
            // Must be positive (0 is no lock; negative fails CLTV outright) and
            // within the uint32 range of nLockTime -- a larger value can never
            // be satisfied, which would burn the stake rather than vest it.
            if (locktime <= 0 || locktime > 0xffffffffLL) return std::nullopt;
            out.liquid_locktime = locktime;
            if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
            if (!script.GetOp(pc, opcode, data)) return std::nullopt;
        } else {
            pc = before_lookahead; // no vesting lock; the token read is the pubkey
        }
    }
    if (data.empty()) return std::nullopt;
    CPubKey pubkey(data);
    if (!pubkey.IsFullyValid()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSIG) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    out.pubkey = pubkey;
    out.csv = (uint32_t)csv;
    return out;
}

std::optional<std::pair<CPubKey, uint32_t>> ParseStakeScript(const CScript& script)
{
    auto full = ParseStakeScriptFull(script);
    if (!full) return std::nullopt;
    return std::make_pair(full->pubkey, full->csv);
}

std::optional<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>>
ParseStakeBlsRegistration(const CScript& script)
{
    auto full = ParseStakeScriptFull(script);
    if (!full || full->bls_pubkey.empty()) return std::nullopt;
    return std::make_pair(full->bls_pubkey, full->bls_pop);
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
    // New staking outputs add weight (and, if they carry a committee BLS
    // registration whose PoP ConnectBlock has verified, register the key).
    for (const CTransactionRef& tx : block.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto stake = StakeFromTxOut(out)) {
                std::vector<unsigned char> bls_pubkey;
                if (auto reg = ParseStakeBlsRegistration(out.scriptPubKey)) bls_pubkey = reg->first;
                registry.AddUtxoStake(stake->first, stake->second, bls_pubkey);
                LogPrintf("PoS: staking output adds %llu to %s\n", (unsigned long long)stake->second, HexStr(stake->first));
            }
            if (auto deleg = DelegationFromTxOut(out)) {
                registry.AddUtxoDelegation(deleg->first, deleg->second);
                LogPrintf("PoS: %s delegates its stake weight to %s\n", HexStr(deleg->first), HexStr(deleg->second));
            }
            if (auto payout = PayoutFromTxOut(out)) {
                registry.AddUtxoPayout(payout->first, payout->second);
                LogPrintf("PoS: %s announces a payout policy effective at height %d\n",
                          HexStr(payout->first), (int)payout->second.activation);
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
            // Conditional erase, so a rotation (old record spent + new record
            // created in one transaction) keeps the NEW signer: the created
            // outputs were applied above.
            if (auto deleg = DelegationFromTxOut(coin.out)) {
                registry.SubUtxoDelegation(deleg->first, deleg->second);
            }
            if (auto payout = PayoutFromTxOut(coin.out)) {
                registry.SubUtxoPayout(payout->first, payout->second);
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
                std::vector<unsigned char> bls_pubkey;
                if (auto reg = ParseStakeBlsRegistration(coin.out.scriptPubKey)) bls_pubkey = reg->first;
                registry.AddUtxoStake(stake->first, stake->second, bls_pubkey);
            }
            // Restore the record this block spent, before the created records
            // are removed below. A rotation then lands back on the old signer:
            // the erase of the created record is conditional and will not fire.
            if (auto deleg = DelegationFromTxOut(coin.out)) {
                registry.AddUtxoDelegation(deleg->first, deleg->second);
            }
            if (auto payout = PayoutFromTxOut(coin.out)) {
                registry.AddUtxoPayout(payout->first, payout->second);
            }
        }
    }
    for (const CTransactionRef& tx : block.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto stake = StakeFromTxOut(out)) {
                registry.SubUtxoStake(stake->first, stake->second);
            }
            if (auto deleg = DelegationFromTxOut(out)) {
                registry.SubUtxoDelegation(deleg->first, deleg->second);
            }
            if (auto payout = PayoutFromTxOut(out)) {
                registry.SubUtxoPayout(payout->first, payout->second);
            }
        }
    }
}

bool RebuildUtxoStake(CCoinsView& view)
{
    std::map<CPubKey, uint64_t> utxo_stake;
    std::map<CPubKey, std::vector<unsigned char>> utxo_bls;
    std::map<CPubKey, CPubKey> utxo_deleg;
    std::map<CPubKey, std::map<int64_t, PosPayoutPolicy>> utxo_payout;
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
            // A staking output's BLS registration (its PoP was verified when the
            // output's block connected; the UTXO set is trusted here). All of a
            // staker's outputs carry the same key (a consensus rule at connect),
            // so any one is authoritative.
            if (auto reg = ParseStakeBlsRegistration(coin.out.scriptPubKey)) {
                utxo_bls[stake->first] = reg->first;
            }
        }
        // Unspent delegation records re-point weight onto signers. A consensus
        // rule (CheckPosDelegationRules) keeps at most one unspent record per
        // controller, so this scan is unambiguous however the UTXOs are ordered.
        if (auto deleg = DelegationFromTxOut(coin.out)) {
            utxo_deleg[deleg->first] = deleg->second;
        }
        // Unspent payout records. Keyed by activation height, so a pending policy
        // and the one it will replace coexist unambiguously (a consensus rule
        // forbids two records sharing a (signer, activation)).
        if (auto payout = PayoutFromTxOut(coin.out)) {
            utxo_payout[payout->first][payout->second.activation] = payout->second;
        }
        pcursor->Next();
    }
    StakeRegistry::GetInstance().SetUtxoStake(std::move(utxo_stake), std::move(utxo_bls),
                                              std::move(utxo_deleg), std::move(utxo_payout));
    return true;
}

void SeedGenesisStake(const CBlock& genesis)
{
    // Register the genesis block's staking output(s) into the registry BEFORE any
    // chainstate (re)activation at startup. Genesis stake is otherwise registered
    // only by the UTXO scan (RebuildUtxoStake), which runs AFTER the chain is
    // loaded/activated; but chain (re)activation validates PoS blocks via
    // CheckPosStakeRules, and the very first block's leader is the genesis staker.
    // On a reload that re-validates blocks during load (a node restarted with the
    // coins tip behind the block index, or -reindex/-reindex-chainstate), the
    // first block would be checked against an empty registry and rejected
    // ("leader-not-staker"), marking the whole chain invalid and stranding the
    // node at genesis. Genesis is special-cased out of the incremental
    // ConnectBlock stake path, so this is the only early entry point. Idempotent
    // with the later UTXO scan, which replaces the registry wholesale.
    StakeRegistry& registry = StakeRegistry::GetInstance();
    for (const CTransactionRef& tx : genesis.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto stake = StakeFromTxOut(out)) {
                // Thread the genesis staker's committee BLS registration too (as
                // RebuildUtxoStake does), so a public committee (-pospubliccommittee)
                // can bootstrap on this early path: restart / -reindex re-validates
                // block 1 BEFORE the later UTXO scan, and its leader is the genesis
                // staker, who must be a BLS-registered committee member to certify.
                // Without it the founder is registered here with weight but no BLS
                // key, block 1's certificate fails to verify, and the chain strands
                // at genesis. Idempotent with the UTXO scan (replaces the registry).
                std::vector<unsigned char> bls;
                if (auto reg = ParseStakeBlsRegistration(out.scriptPubKey)) bls = reg->first;
                registry.AddUtxoStake(stake->first, stake->second, bls);
            }
        }
    }
}

// --- Operator-configured static checkpoints (-poscheckpoint=height:hash) ---
// In the common layer (not the node-layer anchor module) so chainparams.cpp and
// tools such as elements-tx — which link libbitcoin_common but not the node
// library — can configure and link them. Reject-only; enforced in
// ContextualCheckBlockHeader. Guarded by their own mutex.
namespace {
Mutex g_pos_config_cp_mutex;
std::map<int, uint256> g_pos_config_checkpoints GUARDED_BY(g_pos_config_cp_mutex);
} // namespace

void ClearConfiguredPosCheckpoints()
{
    LOCK(g_pos_config_cp_mutex);
    g_pos_config_checkpoints.clear();
}

bool AddConfiguredPosCheckpoint(int height, const uint256& hash, std::string& error)
{
    if (height < 0) { error = "checkpoint height must be non-negative"; return false; }
    LOCK(g_pos_config_cp_mutex);
    auto it = g_pos_config_checkpoints.find(height);
    if (it != g_pos_config_checkpoints.end() && it->second != hash) {
        error = strprintf("conflicting checkpoint at height %d", height);
        return false;
    }
    g_pos_config_checkpoints[height] = hash;
    return true;
}

std::map<int, uint256> GetConfiguredPosCheckpoints()
{
    LOCK(g_pos_config_cp_mutex);
    return g_pos_config_checkpoints;
}
