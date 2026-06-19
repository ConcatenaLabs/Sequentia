// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_producer.h>

#include <block_proof.h>
#include <chainparams.h>
#include <logging.h>
#include <musig.h>
#include <node/miner.h>
#include <pos.h>
#include <primitives/block.h>
#include <script/generic.hpp>
#include <script/sign.h>
#include <timedata.h>
#include <txmempool.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>
#include <vrf.h>

#include <algorithm>
#include <map>
#include <memory>

using node::BlockAssembler;
using node::CBlockTemplate;
using node::IncrementExtraNonce;

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
            const int member_cap = g_pos_agg_committee ? MAX_POS_AGG_COMMITTEE_SIZE : MAX_POS_COMMITTEE_SIZE;
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
                vrf_commitments.push_back(BuildPosVrfMemberCommitment(member_pub, *member_proof));
                eligible++;
            }
            const int quorum = PosQuorum((size_t)g_pos_committee_size);
            if (eligible < quorum) {
                // Aggregate committees may certify below quorum under the
                // escaping-stall rule; script multisig always needs the quorum.
                if (!g_pos_agg_committee) {
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
    if (!parts->agg_key.empty()) {
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
                         const CChainParams& chainparams, std::vector<CKey> keys)
    : m_chainman(chainman), m_mempool(mempool), m_chainparams(chainparams),
      m_keys(std::move(keys)) {}

PosProducer::~PosProducer() { Stop(); }

void PosProducer::Start()
{
    if (m_running) return;
    m_running = true;
    RegisterValidationInterface(this);
    m_thread = std::thread(&util::TraceThread, "posproducer", [this] { ThreadLoop(); });
    LogPrintf("PoS producer: started with %d staking key(s)\n", (int)m_keys.size());
}

void PosProducer::Stop()
{
    if (!m_running) return;
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

    // Slot timing: the leader's slot opens slot*interval after the parent, and
    // we never produce faster than one interval since the parent — the paper's
    // soft lower-bound cadence floor (Principle 10; consensus only enforces the
    // slot gate). Wait until the later of the two.
    const int64_t slot_open = (int64_t)tip->nTime + (int64_t)best_slot * g_pos_slot_interval;
    const int64_t cadence_floor = (int64_t)tip->nTime + g_pos_slot_interval;
    const int64_t earliest = std::max(slot_open, cadence_floor);
    const int64_t now = GetAdjustedTime();
    if (now < earliest) {
        // Sleep until our slot, but cap the wait so a new tip or shutdown is
        // noticed promptly even if the cv wakeup is missed.
        const int64_t wait_ms = (earliest - now) * 1000;
        return std::clamp<int64_t>(wait_ms, 1, POS_PRODUCER_POLL_MS * 5);
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
