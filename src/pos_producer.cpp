// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_producer.h>

#include <block_proof.h>
#include <bls.h>
#include <chainparams.h>
#include <crypto/sha256.h>
#include <logging.h>
#include <musig.h>
#include <net.h>
#include <netmessagemaker.h>
#include <node/miner.h>
#include <pos.h>
#include <primitives/block.h>
#include <protocol.h>
#include <script/generic.hpp>
#include <script/sign.h>
#include <timedata.h>
#include <txmempool.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>
#include <vrf.h>

#include <atomic>

#include <algorithm>
#include <map>
#include <memory>

using node::BlockAssembler;
using node::CBlockTemplate;
using node::IncrementExtraNonce;

namespace {
//! Derive a staker's BLS secret seed deterministically from its secp256k1
//! staking key (domain-separated by a tag), so a staker needs no separate BLS
//! key to manage. The matching BLS public key and proof-of-possession are
//! published per-block in the coinbase SEQBLS commitment; validators never
//! derive it (they read it from the block).
std::vector<unsigned char> PosBlsSeedFromKey(const CKey& key)
{
    static const std::string tag = "sequentia/pos-bls/v1";
    unsigned char out[32];
    CSHA256().Write(reinterpret_cast<const unsigned char*>(tag.data()), tag.size())
             .Write(key.begin(), key.size())
             .Finalize(out);
    return std::vector<unsigned char>(out, out + 32);
}

//! Build the leader's unsigned BLS committee block extending the active tip:
//! the OP_2 <leader> challenge, the leader's VRF commitment in the coinbase, an
//! empty solution (the certificate is assembled once members sign). Returns the
//! block and its height, or nullptr.
std::shared_ptr<CBlock> BuildUnsignedBlsBlock(ChainstateManager& chainman, CTxMemPool& mempool,
                                              const CChainParams& chainparams, const CKey& leader_key,
                                              int& out_height)
{
    const CPubKey pubkey = leader_key.GetPubKey();
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = chainman.ActiveChain().Tip();
    }
    if (!tip) return nullptr;
    const uint256 seed = PosSeedForChild(tip);
    auto proof = VrfProve(leader_key, Span<const unsigned char>(seed.begin(), 32));
    if (!proof) return nullptr;
    std::vector<CScript> vrf_commitments;
    vrf_commitments.push_back(BuildPosVrfCommitment(*proof));
    CScript feeDest = chainparams.GetConsensus().mandatory_coinbase_destination;
    if (feeDest == CScript()) feeDest = CScript() << OP_TRUE;
    std::unique_ptr<CBlockTemplate> tmpl(
        BlockAssembler(chainman.ActiveChainstate(), mempool, chainparams)
            .CreateNewBlock(feeDest, std::chrono::seconds(0), nullptr, &vrf_commitments, &pubkey, &*proof, nullptr));
    if (!tmpl) return nullptr;
    auto block = std::make_shared<CBlock>(tmpl->block);
    {
        LOCK(cs_main);
        unsigned int extra_nonce = 0;
        IncrementExtraNonce(block.get(), chainman.ActiveChain().Tip(), extra_nonce);
    }
    out_height = tip->nHeight + 1;
    return block;
}
} // namespace

//! The running producer, for net_processing to deliver gossip to.
static std::atomic<PosProducer*> g_active_producer{nullptr};
PosProducer* GetActivePosProducer() { return g_active_producer.load(); }

bool ProducePosBlock(ChainstateManager& chainman, CTxMemPool& mempool,
                     const CChainParams& chainparams, const CKey& leader_key,
                     const std::vector<CKey>& committee_keys,
                     PosProduceResult& result, std::string& error,
                     PosProduceError& err_kind)
{
    err_kind = PosProduceError::NONE;
    if (!leader_key.IsValid()) {
        error = "Invalid leader key";
        err_kind = PosProduceError::INVALID_KEY;
        return false;
    }
    const CPubKey pubkey = leader_key.GetPubKey();

    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = chainman.ActiveChain().Tip();
    }
    if (!tip) {
        error = "No active chain tip";
        err_kind = PosProduceError::INTERNAL;
        return false;
    }

    const uint256 seed = PosSeedForChild(tip);
    std::optional<size_t> rank = PosRank(StakeRegistry::GetInstance(), seed, pubkey);
    if (!rank) {
        error = "This key is not a registered staker for the current slot";
        err_kind = PosProduceError::NOT_STAKER;
        return false;
    }

    CScript feeDestinationScript = chainparams.GetConsensus().mandatory_coinbase_destination;
    if (feeDestinationScript == CScript()) feeDestinationScript = CScript() << OP_TRUE;

    // VRF sortition mode: compute this staker's sortition proof over the slot
    // seed and commit it in the coinbase. With committee certification, also
    // compute each provided committee key's eligibility proof; only
    // sortition-selected members may countersign (mirrors generateposblock).
    std::vector<unsigned char> vrf_proof;
    std::vector<CScript> vrf_commitments;
    std::vector<CPubKey> vrf_committee;
    std::map<CPubKey, CKey> candidates; // committee key candidates (leader + provided)
    std::vector<PosBlsMember> bls_members;                    // BLS committee (for the solution)
    std::vector<std::vector<unsigned char>> bls_member_seeds; // their BLS signing seeds (parallel)
    uint64_t vrf_slot = 0;
    uint256 vrf_output;
    if (g_pos_vrf) {
        auto proof = VrfProve(leader_key, Span<const unsigned char>(seed.begin(), 32));
        if (!proof) {
            error = "Failed to compute VRF sortition proof";
            err_kind = PosProduceError::MISC;
            return false;
        }
        vrf_proof = *proof;
        if (!VrfVerify(pubkey, Span<const unsigned char>(seed.begin(), 32), vrf_proof, vrf_output)) {
            error = "Produced VRF proof did not verify";
            err_kind = PosProduceError::INTERNAL;
            return false;
        }
        const StakeRegistry& registry = StakeRegistry::GetInstance();
        const uint64_t total_weight = PosTotalWeight(registry);
        vrf_slot = PosVrfSlot(vrf_output, registry.GetWeight(pubkey), total_weight);
        vrf_commitments.push_back(BuildPosVrfCommitment(vrf_proof));

        if (g_pos_committee_size > 1) {
            candidates.emplace(pubkey, leader_key);
            if ((int)committee_keys.size() > MAX_POS_AGG_COMMITTEE_SIZE) {
                error = strprintf("At most %d committee keys may be supplied", MAX_POS_AGG_COMMITTEE_SIZE);
                err_kind = PosProduceError::BAD_PARAM;
                return false;
            }
            for (const CKey& ckey : committee_keys) {
                if (!ckey.IsValid()) {
                    error = "Invalid committee private key";
                    err_kind = PosProduceError::INVALID_KEY;
                    return false;
                }
                candidates.emplace(ckey.GetPubKey(), ckey);
            }
            const int member_cap = (g_pos_agg_committee || g_pos_bls) ? MAX_POS_AGG_COMMITTEE_SIZE : MAX_POS_COMMITTEE_SIZE;
            int eligible = 0;
            for (const auto& [member_pub, member_key] : candidates) {
                if ((int)vrf_committee.size() >= member_cap) break;
                if (!PosIsEligibleStake(registry.GetWeight(member_pub))) continue;
                auto member_proof = VrfProve(member_key, Span<const unsigned char>(seed.begin(), 32));
                if (!member_proof) continue;
                uint256 member_beta;
                if (!VrfVerify(member_pub, Span<const unsigned char>(seed.begin(), 32), *member_proof, member_beta)) continue;
                if (!PosVrfIsCommitteeMember(member_beta, registry.GetWeight(member_pub), total_weight)) continue;
                vrf_committee.push_back(member_pub);
                if (g_pos_bls) {
                    // Each member derives its BLS key from its staking key; the
                    // member (key, VRF proof, BLS key, proof-of-possession) goes
                    // into the certificate carried in the proof solution, not the
                    // coinbase — so the signed block hash is member-independent.
                    std::vector<unsigned char> bls_seed = PosBlsSeedFromKey(member_key);
                    auto bls_pub = BlsDerivePubKey(bls_seed);
                    auto bls_pop = BlsProvePossession(bls_seed);
                    if (!bls_pub || !bls_pop) {
                        error = "Failed to derive committee member BLS key";
                        err_kind = PosProduceError::INTERNAL;
                        return false;
                    }
                    PosBlsMember m;
                    m.pubkey = member_pub;
                    m.proof = *member_proof;
                    m.bls_pubkey = *bls_pub;
                    m.bls_pop = *bls_pop;
                    bls_members.push_back(std::move(m));
                    bls_member_seeds.push_back(std::move(bls_seed));
                } else {
                    vrf_commitments.push_back(BuildPosVrfMemberCommitment(member_pub, *member_proof));
                }
                eligible++;
            }
            const int quorum = PosQuorum((size_t)g_pos_committee_size);
            if (eligible < quorum) {
                // Aggregate committees (MuSig2 or BLS) may certify below quorum
                // under the escaping-stall rule; script multisig always needs
                // the quorum.
                if (!g_pos_agg_committee && !g_pos_bls) {
                    error = strprintf("Only %d of the provided keys are sortition-selected committee members for this slot; quorum is %d of an expected committee of %d", eligible, quorum, g_pos_committee_size);
                    err_kind = PosProduceError::MISC;
                    return false;
                }
                if (eligible < 1) {
                    error = "No sortition-selected committee members available for this slot";
                    err_kind = PosProduceError::MISC;
                    return false;
                }
            }
        }
    }

    std::unique_ptr<CBlockTemplate> pblocktemplate(
        BlockAssembler(chainman.ActiveChainstate(), mempool, chainparams)
            .CreateNewBlock(feeDestinationScript, std::chrono::seconds(0), nullptr,
                            g_pos_vrf ? &vrf_commitments : nullptr, &pubkey,
                            g_pos_vrf ? &vrf_proof : nullptr,
                            (g_pos_vrf && !vrf_committee.empty()) ? &vrf_committee : nullptr));
    if (!pblocktemplate.get()) {
        error = "Could not create block template";
        err_kind = PosProduceError::INTERNAL;
        return false;
    }
    CBlock& block = pblocktemplate->block;

    {
        LOCK(cs_main);
        unsigned int extra_nonce = 0;
        IncrementExtraNonce(&block, chainman.ActiveChain().Tip(), extra_nonce);
    }

    // Collect all available signing keys (leader + any committee keys).
    FillableSigningProvider keystore;
    keystore.AddKey(leader_key);
    for (const CKey& ckey : committee_keys) {
        if (!ckey.IsValid()) {
            error = "Invalid committee private key";
            err_kind = PosProduceError::INVALID_KEY;
            return false;
        }
        keystore.AddKey(ckey);
    }

    // Sign the block: the same three challenge forms generateposblock handles —
    // aggregate-committee (leader ECDSA + one MuSig2 aggregate), leader-only
    // (generic P2PK), or script-multisig committee.
    SimpleSignatureCreator signature_creator(block.GetHash(), 0);
    std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(block.proof.challenge);
    if (!parts) {
        error = "Generated block challenge is not a recognized PoS challenge";
        err_kind = PosProduceError::INTERNAL;
        return false;
    }
    int countersigs = 0;
    if (parts->is_bls) {
        // BLS aggregate-committee form: the leader's ECDSA signature, the
        // aggregate of the members' shares, and the member set — all assembled
        // into the proof solution (excluded from the block hash, so the members
        // could sign it non-interactively; here one host holds all the keys).
        // Each member signs with the BLS key derived from its staking key.
        std::vector<unsigned char> leader_sig;
        if (!leader_key.Sign(block.GetHash(), leader_sig)) {
            error = "Failed to sign block as leader";
            err_kind = PosProduceError::MISC;
            return false;
        }
        const uint256 hash = block.GetHash();
        std::vector<std::vector<unsigned char>> shares;
        shares.reserve(bls_member_seeds.size());
        for (const std::vector<unsigned char>& member_seed : bls_member_seeds) {
            auto share = BlsSign(member_seed, Span<const unsigned char>(hash.begin(), 32));
            if (!share) {
                error = "Failed to produce a committee BLS signature share";
                err_kind = PosProduceError::MISC;
                return false;
            }
            shares.push_back(std::move(*share));
        }
        auto agg_sig = BlsAggregate(shares);
        if (!agg_sig) {
            error = "Failed to aggregate the committee BLS signatures";
            err_kind = PosProduceError::MISC;
            return false;
        }
        block.proof.solution = BuildPosBlsSolution(leader_sig, *agg_sig, bls_members);
        countersigs = (int)bls_members.size();
    } else if (!parts->agg_key.empty()) {
        std::vector<unsigned char> leader_sig;
        if (!leader_key.Sign(block.GetHash(), leader_sig)) {
            error = "Failed to sign block as leader";
            err_kind = PosProduceError::MISC;
            return false;
        }
        std::vector<CKey> member_keys;
        member_keys.reserve(vrf_committee.size());
        for (const CPubKey& member : vrf_committee) member_keys.push_back(candidates.at(member));
        const uint256 hash = block.GetHash();
        std::optional<std::vector<unsigned char>> agg_sig =
            MuSigSign(member_keys, vrf_committee, Span<const unsigned char>(hash.begin(), 32));
        if (!agg_sig) {
            error = "Failed to produce the committee's MuSig2 aggregate signature";
            err_kind = PosProduceError::MISC;
            return false;
        }
        CScript solution;
        solution << leader_sig << *agg_sig;
        block.proof.solution = solution;
        countersigs = (int)vrf_committee.size();
    } else if (parts->committee.empty()) {
        SignatureData sig_data;
        if (!ProduceSignature(keystore, signature_creator, block.proof.challenge, sig_data, SCRIPT_NO_SIGHASH_BYTE)) {
            error = "Failed to sign block as staker";
            err_kind = PosProduceError::MISC;
            return false;
        }
        block.proof.solution = sig_data.scriptSig;
    } else {
        std::vector<unsigned char> leader_sig;
        if (!signature_creator.CreateSig(keystore, leader_sig, parts->leader.GetID(), block.proof.challenge, SigVersion::BASE, 0)) {
            error = "Failed to sign block as leader";
            err_kind = PosProduceError::MISC;
            return false;
        }
        std::vector<std::vector<unsigned char>> committee_sigs;
        for (const CPubKey& member : parts->committee) {
            if ((int)committee_sigs.size() >= parts->quorum) break;
            std::vector<unsigned char> sig;
            if (signature_creator.CreateSig(keystore, sig, member.GetID(), block.proof.challenge, SigVersion::BASE, 0)) {
                committee_sigs.push_back(sig);
            }
        }
        if ((int)committee_sigs.size() < parts->quorum) {
            error = strprintf("Insufficient committee keys: %d countersignature(s) available, quorum is %d of %d", (int)committee_sigs.size(), parts->quorum, (int)parts->committee.size());
            err_kind = PosProduceError::MISC;
            return false;
        }
        CScript solution;
        solution << OP_0; // CHECKMULTISIG dummy
        for (const auto& sig : committee_sigs) solution << sig;
        solution << leader_sig;
        block.proof.solution = solution;
        countersigs = (int)committee_sigs.size();
    }

    if (!CheckProof(block, chainparams.GetConsensus())) {
        error = "Block signature(s) did not satisfy the leader/committee challenge";
        err_kind = PosProduceError::MISC;
        return false;
    }

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    if (!chainman.ProcessNewBlock(chainparams, shared_pblock, /*force_processing=*/true, nullptr)) {
        error = "ProcessNewBlock, block not accepted";
        err_kind = PosProduceError::INTERNAL;
        return false;
    }

    result.hash = block.GetHash();
    result.height = tip->nHeight + 1;
    result.rank = *rank;
    result.countersignatures = countersigs;
    result.vrf = g_pos_vrf;
    result.vrf_output = vrf_output;
    result.vrf_slot = vrf_slot;
    return true;
}

// --- The autonomous producer thread ---------------------------------------

//! Default re-evaluation cadence when there is nothing to do right now.
static constexpr int64_t POS_PRODUCER_POLL_MS = 1000;

PosProducer::PosProducer(ChainstateManager& chainman, CTxMemPool& mempool,
                         const CChainParams& chainparams, CConnman* connman, std::vector<CKey> keys)
    : m_chainman(chainman), m_mempool(mempool), m_chainparams(chainparams),
      m_connman(connman), m_keys(std::move(keys)) {}

PosProducer::~PosProducer() { Stop(); }

void PosProducer::Start()
{
    if (m_running) return;
    m_running = true;
    g_active_producer.store(this);
    RegisterValidationInterface(this);
    m_thread = std::thread(&util::TraceThread, "posproducer", [this] { ThreadLoop(); });
    LogPrintf("PoS producer: started with %d staking key(s)\n", (int)m_keys.size());
}

void PosProducer::Stop()
{
    if (!m_running) return;
    g_active_producer.store(nullptr);
    UnregisterValidationInterface(this);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void PosProducer::UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake = true;
    }
    m_cv.notify_all();
}

void PosProducer::ThreadLoop()
{
    while (true) {
        int64_t sleep_ms = Step();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stop) break;
        m_cv.wait_for(lock, std::chrono::milliseconds(sleep_ms),
                      [this] { return m_stop || m_wake; });
        if (m_stop) break;
        m_wake = false;
    }
}

int64_t PosProducer::Step()
{
    // Note: we deliberately do NOT gate on IsInitialBlockDownload(). A fresh
    // genesis-only chain is always "in IBD" until a recent block exists, so
    // gating here would prevent the producer from ever creating that first
    // block (the bootstrap this feature exists for). The producer always builds
    // on the best validated tip; a peer-aware "don't produce if a better chain
    // is being announced" guard is a later, gossip-aware refinement (Phase 2+).

    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip) return POS_PRODUCER_POLL_MS;

    const uint256 seed = PosSeedForChild(tip);
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const uint64_t total_weight = PosTotalWeight(registry);

    // Elect the best-ranked (lowest-slot) of our keys to lead this round. Under
    // VRF sortition the slot is private (computed from our own key); we cannot
    // see other stakers' slots, so we simply race at our slot time — a lower
    // slot elsewhere produces first and wakes us for the next height.
    int best_idx = -1;
    uint64_t best_slot = 0;
    int local_committee_eligible = 0; // how many of our keys are committee members this slot
    for (size_t i = 0; i < m_keys.size(); ++i) {
        const CPubKey pub = m_keys[i].GetPubKey();
        if (!PosIsEligibleStake(registry.GetWeight(pub))) continue;
        uint64_t slot;
        if (g_pos_vrf) {
            auto proof = VrfProve(m_keys[i], Span<const unsigned char>(seed.begin(), 32));
            if (!proof) continue;
            uint256 beta;
            if (!VrfVerify(pub, Span<const unsigned char>(seed.begin(), 32), *proof, beta)) continue;
            slot = PosVrfSlot(beta, registry.GetWeight(pub), total_weight);
            if (PosVrfIsCommitteeMember(beta, registry.GetWeight(pub), total_weight)) local_committee_eligible++;
        } else {
            std::optional<size_t> rank = PosRank(registry, seed, pub);
            if (!rank) continue;
            slot = (uint64_t)*rank;
        }
        if (best_idx < 0 || slot < best_slot) {
            best_idx = (int)i;
            best_slot = slot;
        }
    }
    if (best_idx < 0) return POS_PRODUCER_POLL_MS; // none of our keys is an eligible staker

    const int quorum = PosQuorum((size_t)g_pos_committee_size);

    // Slot timing: the leader's slot opens slot*interval after the parent, and
    // we never produce faster than one interval since the parent — the paper's
    // soft lower-bound cadence floor (Principle 10; consensus only enforces the
    // slot gate). A sub-second leader stagger (finer than the 1-second block
    // clock) on top lets the lowest-VRF-slot node propose first; staggering from
    // the later of the slot time and the current second keeps it effective during
    // catch-up (when the slot time is far in the past).
    static const int64_t POS_LEADER_STAGGER_MS = 300;
    const int64_t slot_open = (int64_t)tip->nTime + (int64_t)best_slot * g_pos_slot_interval;
    const int64_t cadence_floor = (int64_t)tip->nTime + g_pos_slot_interval;
    const int64_t earliest_sec = std::max(slot_open, cadence_floor);
    const int64_t now_ms = GetTimeMillis();
    const int64_t base_ms = std::max(earliest_sec * 1000, (now_ms / 1000) * 1000);
    const int64_t target_ms = base_ms + (int64_t)best_slot * POS_LEADER_STAGGER_MS;

    // BLS distributed committee (we lack a local quorum, e.g. one key per host):
    // drive the gossip round every poll — sign the lowest-VRF proposal once the
    // collection window closes, and assemble if our own proposal wins with a
    // quorum — and propose when our slot is due if no round has started yet.
    if (g_pos_bls && g_pos_committee_size > 1 && local_committee_eligible < quorum) {
        const int height = tip->nHeight + 1;
        int64_t round_poll = DriveRound();
        const bool connected = m_connman && m_connman->GetNodeCount(ConnectionDirection::Both) > 0;
        if (now_ms >= target_ms && connected) {
            bool start;
            {
                std::lock_guard<std::mutex> lock(m_gossip_mutex);
                start = (m_round_height < height); // no proposal seen yet this height
            }
            if (start) {
                ProposeGossip(m_keys[best_idx]);
                round_poll = 150;
            }
        }
        int64_t wait = std::min<int64_t>(round_poll, POS_PRODUCER_POLL_MS);
        // No active round and not yet due to propose: wake right at our slot
        // target (the stagger is sub-second, so we must not overshoot it with a
        // full poll interval).
        if (wait >= POS_PRODUCER_POLL_MS && now_ms < target_ms) {
            wait = std::clamp<int64_t>(target_ms - now_ms, 1, POS_PRODUCER_POLL_MS);
        }
        return wait;
    }

    if (now_ms < target_ms) {
        return std::clamp<int64_t>(target_ms - now_ms, 1, POS_PRODUCER_POLL_MS * 5);
    }

    // Due now: assemble, sign, submit. The remaining keys serve as committee
    // signers for the single-host committee case (none for committee = 1).
    std::vector<CKey> committee_keys;
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if ((int)i != best_idx) committee_keys.push_back(m_keys[i]);
    }
    PosProduceResult res;
    std::string err;
    PosProduceError kind = PosProduceError::NONE;
    if (ProducePosBlock(m_chainman, m_mempool, m_chainparams, m_keys[best_idx], committee_keys, res, err, kind)) {
        LogPrintf("PoS producer: created block %s at height %d (rank %d, %d countersignature(s))\n",
                  res.hash.GetHex(), res.height, (int)res.rank, res.countersignatures);
    } else {
        LogPrint(BCLog::VALIDATION, "PoS producer: no block this round: %s\n", err);
    }
    return POS_PRODUCER_POLL_MS;
}

// --- Gossip committee --------------------------------------------------------

void PosProducer::FloodProposal(const CBlock& block)
{
    if (!m_connman) return;
    m_connman->ForEachNode([&](CNode* pnode) {
        m_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetCommonVersion()).Make(NetMsgType::POSPROPOSAL, block));
    });
}

void PosProducer::Wake()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake = true;
    }
    m_cv.notify_all();
}

void PosProducer::FloodShare(const PosShare& share)
{
    if (!m_connman) return;
    m_connman->ForEachNode([&](CNode* pnode) {
        m_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetCommonVersion()).Make(NetMsgType::POSSHARE, share));
    });
}

std::vector<PosShare> PosProducer::MakeLocalShares(const CBlock& block)
{
    std::vector<PosShare> out;
    CBlockIndex* prev;
    {
        LOCK(cs_main);
        prev = m_chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
    }
    if (!prev) return out;
    const uint256 seed = PosSeedForChild(prev);
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    const uint64_t total = PosTotalWeight(reg);
    const uint256 hash = block.GetHash();
    for (const CKey& k : m_keys) {
        const CPubKey pub = k.GetPubKey();
        if (!PosIsEligibleStake(reg.GetWeight(pub))) continue;
        auto proof = VrfProve(k, Span<const unsigned char>(seed.begin(), 32));
        if (!proof) continue;
        uint256 beta;
        if (!VrfVerify(pub, Span<const unsigned char>(seed.begin(), 32), *proof, beta)) continue;
        if (!PosVrfIsCommitteeMember(beta, reg.GetWeight(pub), total)) continue;
        const std::vector<unsigned char> bls_seed = PosBlsSeedFromKey(k);
        auto bls_pub = BlsDerivePubKey(bls_seed);
        auto bls_pop = BlsProvePossession(bls_seed);
        auto share_sig = BlsSign(bls_seed, Span<const unsigned char>(hash.begin(), 32));
        if (!bls_pub || !bls_pop || !share_sig) continue;
        PosShare sh;
        sh.block_hash = hash;
        sh.pubkey = pub;
        sh.vrf_proof = *proof;
        sh.bls_pubkey = *bls_pub;
        sh.bls_pop = *bls_pop;
        sh.bls_share = *share_sig;
        out.push_back(std::move(sh));
    }
    return out;
}

void PosProducer::RecordProposal(const std::shared_ptr<const CBlock>& block, const uint256& leader_beta, int height)
{
    // m_gossip_mutex held. One round per height; within a round keep the
    // lowest-leader-VRF proposal (paper P6 step 6/8 convergence).
    if (height < m_round_height) return; // stale
    if (height > m_round_height) {
        m_round_height = height;
        m_round_start_ms = GetTimeMillis();
        m_best_proposal = block;
        m_best_beta = leader_beta;
        m_signed = false;
        m_proposal.reset();
        m_collected.clear();
    } else if (!m_best_proposal || leader_beta < m_best_beta) {
        m_best_proposal = block;
        m_best_beta = leader_beta;
    }
}

void PosProducer::ProposeGossip(const CKey& leader_key)
{
    int height = 0;
    auto block = BuildUnsignedBlsBlock(m_chainman, m_mempool, m_chainparams, leader_key, height);
    if (!block) {
        LogPrint(BCLog::VALIDATION, "PoS gossip: failed to build a proposal\n");
        return;
    }
    const uint256 hash = block->GetHash();
    uint256 own_beta;
    {
        CBlockIndex* prev;
        {
            LOCK(cs_main);
            prev = m_chainman.m_blockman.LookupBlockIndex(block->hashPrevBlock);
        }
        if (prev) {
            const uint256 seed = PosSeedForChild(prev);
            auto pr = VrfProve(leader_key, Span<const unsigned char>(seed.begin(), 32));
            if (pr) VrfVerify(leader_key.GetPubKey(), Span<const unsigned char>(seed.begin(), 32), *pr, own_beta);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        RecordProposal(block, own_beta, height);
        // Remember this is OUR proposal so we collect shares for it (and assemble
        // it if it wins the round).
        m_proposal = block;
        m_proposal_hash = hash;
    }
    LogPrintf("PoS gossip: proposing block %s at height %d\n", hash.GetHex(), height);
    FloodProposal(*block);
}

int64_t PosProducer::DriveRound()
{
    // Length of the proposal-collection window before we sign the lowest-VRF
    // proposal. Long enough for proposals to propagate, short for fast blocks.
    static const int64_t WINDOW_MS = 500;
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip) return POS_PRODUCER_POLL_MS;
    const int height = tip->nHeight + 1;

    // Once the window closes, sign the round's best proposal exactly once.
    std::shared_ptr<const CBlock> to_sign;
    bool sign_is_own = false;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height && !m_signed && m_best_proposal &&
            GetTimeMillis() >= m_round_start_ms + WINDOW_MS) {
            to_sign = m_best_proposal;
            m_signed = true;
            sign_is_own = m_proposal && (m_best_proposal->GetHash() == m_proposal_hash);
        }
    }
    if (to_sign) {
        std::vector<PosShare> shares = MakeLocalShares(*to_sign);
        if (sign_is_own) {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            for (PosShare& sh : shares) m_collected[sh.pubkey] = sh;
        } else {
            for (const PosShare& sh : shares) FloodShare(sh);
        }
    }

    // If our own proposal won the round and we hold a quorum, assemble + submit.
    std::shared_ptr<CBlock> final_block;
    uint256 final_hash;
    size_t members_n = 0;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height && m_proposal && m_best_proposal &&
            m_best_proposal->GetHash() == m_proposal_hash &&
            GetTimeMillis() >= m_round_start_ms + WINDOW_MS) {
            const int quorum = PosQuorum((size_t)g_pos_committee_size);
            if ((int)m_collected.size() >= quorum) {
                auto parts = ParsePosBlockChallenge(m_proposal->proof.challenge);
                CKey leader_key;
                if (parts) {
                    for (const CKey& k : m_keys) {
                        if (k.GetPubKey() == parts->leader) { leader_key = k; break; }
                    }
                }
                std::vector<unsigned char> leader_sig;
                if (parts && leader_key.IsValid() && leader_key.Sign(m_proposal->GetHash(), leader_sig)) {
                    std::vector<PosBlsMember> members;
                    std::vector<std::vector<unsigned char>> shares;
                    for (const auto& [pub, sh] : m_collected) {
                        PosBlsMember m;
                        m.pubkey = pub;
                        m.proof = sh.vrf_proof;
                        m.bls_pubkey = sh.bls_pubkey;
                        m.bls_pop = sh.bls_pop;
                        members.push_back(std::move(m));
                        shares.push_back(sh.bls_share);
                    }
                    auto agg = BlsAggregate(shares);
                    if (agg) {
                        final_block = std::make_shared<CBlock>(*m_proposal);
                        final_block->proof.solution = BuildPosBlsSolution(leader_sig, *agg, members);
                        final_hash = m_proposal->GetHash();
                        members_n = members.size();
                        m_proposal.reset(); // assembled; stop collecting
                    }
                }
            }
        }
    }
    if (final_block) {
        if (m_chainman.ProcessNewBlock(m_chainparams, final_block, /*force_processing=*/true, nullptr)) {
            LogPrintf("PoS gossip: certified block %s at height %d with %d committee member(s)\n",
                      final_hash.GetHex(), height, (int)members_n);
        } else {
            LogPrint(BCLog::VALIDATION, "PoS gossip: assembled block %s was not accepted\n", final_hash.GetHex());
        }
    }

    // Poll fast while a round for the current height is in progress.
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height && (m_proposal || !m_signed)) return 150;
    }
    return POS_PRODUCER_POLL_MS;
}

bool PosProducer::OnProposal(const std::shared_ptr<const CBlock>& block)
{
    const uint256 hash = block->GetHash();
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (!m_seen_proposals.insert(hash).second) return false; // already seen; don't relay
        if (m_seen_proposals.size() > 20000) m_seen_proposals.clear();
    }
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip || block->hashPrevBlock != tip->GetBlockHash()) return true; // relay; not on our tip
    const int height = tip->nHeight + 1;
    auto parts = ParsePosBlockChallenge(block->proof.challenge);
    if (!parts || !parts->is_bls) return true;
    const uint256 seed = PosSeedForChild(tip);
    auto leader_proof = ExtractPosVrfProof(*block);
    uint256 lbeta;
    if (!leader_proof || !VrfVerify(parts->leader, Span<const unsigned char>(seed.begin(), 32), *leader_proof, lbeta)) return true;
    if (StakeRegistry::GetInstance().GetWeight(parts->leader) == 0) return true;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        RecordProposal(block, lbeta, height);
    }
    Wake(); // let the worker drive this round promptly
    return true;
}

bool PosProducer::OnShare(const PosShare& share)
{
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (!m_seen_shares.insert({share.block_hash, share.pubkey}).second) return false;
        if (m_seen_shares.size() > 200000) m_seen_shares.clear();
    }
    // Crypto validation (registry-independent).
    if (share.bls_pubkey.size() != BLS_PK_SIZE || share.bls_pop.size() != BLS_SIG_SIZE || share.bls_share.size() != BLS_SIG_SIZE) return true;
    if (!BlsVerifyPossession(share.bls_pubkey, share.bls_pop)) return true;
    if (!BlsVerify(share.bls_pubkey, Span<const unsigned char>(share.block_hash.begin(), 32), share.bls_share)) return true;
    // Only collect shares for our own proposal.
    uint256 prev_hash;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (!m_proposal || share.block_hash != m_proposal_hash) return true; // not ours; relay
        prev_hash = m_proposal->hashPrevBlock;
    }
    // Member sortition eligibility for the proposal's slot.
    CBlockIndex* prev;
    {
        LOCK(cs_main);
        prev = m_chainman.m_blockman.LookupBlockIndex(prev_hash);
    }
    if (!prev) return true;
    const uint256 seed = PosSeedForChild(prev);
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    if (reg.GetWeight(share.pubkey) == 0) return true;
    uint256 beta;
    if (!VrfVerify(share.pubkey, Span<const unsigned char>(seed.begin(), 32), share.vrf_proof, beta)) return true;
    if (!PosVrfIsCommitteeMember(beta, reg.GetWeight(share.pubkey), PosTotalWeight(reg))) return true;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_proposal && share.block_hash == m_proposal_hash) m_collected[share.pubkey] = share;
    }
    Wake(); // a new share may complete our quorum
    return true;
}
