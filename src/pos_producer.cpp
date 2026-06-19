// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_producer.h>

#include <block_proof.h>
#include <bls.h>
#include <chainparams.h>
#include <crypto/sha256.h>
#include <logging.h>
#include <consensus/merkle.h>
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
#include <util/strencodings.h>
#include <util/system.h>
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

PosCompactProposal MakePosCompactProposal(const CBlock& block)
{
    PosCompactProposal c;
    c.header = block; // slice CBlock -> CBlockHeader (header incl. the proof)
    if (!block.vtx.empty()) c.coinbase = block.vtx[0];
    for (size_t i = 1; i < block.vtx.size(); ++i) c.txids.push_back(block.vtx[i]->GetHash());
    return c;
}

std::shared_ptr<CBlock> ReconstructPosProposal(const PosCompactProposal& compact, CTxMemPool& mempool)
{
    if (!compact.coinbase) return nullptr;
    auto block = std::make_shared<CBlock>();
    *static_cast<CBlockHeader*>(block.get()) = compact.header;
    block->vtx.push_back(compact.coinbase);
    {
        LOCK(mempool.cs);
        for (const uint256& txid : compact.txids) {
            CTransactionRef tx = mempool.get(txid);
            if (!tx) return nullptr; // a referenced transaction is not in our mempool
            block->vtx.push_back(std::move(tx));
        }
    }
    // The header's merkle root verifies the reconstruction: if it matches, the
    // looked-up transactions are exactly the ones the proposer committed to.
    bool mutated = false;
    if (BlockMerkleRoot(*block, &mutated) != block->hashMerkleRoot || mutated) return nullptr;
    return block;
}

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
      m_connman(connman), m_keys(std::move(keys))
{
    m_byzantine_equivocate = gArgs.GetBoolArg("-posbyzantineequivocate", false);
    m_byzantine_invalid = gArgs.GetBoolArg("-posbyzantineinvalid", false);
}

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
    // slot gate). We propose as soon as that opens and do NOT add a sub-second
    // per-slot leader stagger: the gossip collection window plus lowest-VRF
    // convergence already resolve multiple simultaneous proposers, whereas a
    // stagger wider than the window would let an early (low-slot) proposer sign
    // before higher-slot proposals arrive — splitting shares and stalling the
    // round at larger committee sizes. Anchoring to the current second keeps the
    // round starts of all nodes aligned during catch-up (past slot times).
    const int64_t slot_open = (int64_t)tip->nTime + (int64_t)best_slot * g_pos_slot_interval;
    const int64_t cadence_floor = (int64_t)tip->nTime + g_pos_slot_interval;
    const int64_t earliest_sec = std::max(slot_open, cadence_floor);
    const int64_t now_ms = GetTimeMillis();
    const int64_t target_ms = std::max(earliest_sec * 1000, (now_ms / 1000) * 1000);

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
                // Every eligible member proposes once per height (regardless of
                // having seen others' proposals), so the candidate set — and thus
                // the round-robin leader order — is complete and common to all.
                start = (m_proposed_height < height);
                if (start) m_proposed_height = height;
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

void PosProducer::FloodCompactProposal(const CBlock& block)
{
    if (!m_connman) return;
    PosCompactProposal compact = MakePosCompactProposal(block);
    m_connman->ForEachNode([&](CNode* pnode) {
        m_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetCommonVersion()).Make(NetMsgType::POSCMPCTPROPOSAL, compact));
    });
}

std::shared_ptr<const CBlock> PosProducer::GetProposalBlock(const uint256& hash)
{
    std::lock_guard<std::mutex> lock(m_gossip_mutex);
    for (const auto& [leader, cand] : m_candidates) {
        if (cand.block->GetHash() == hash) return cand.block;
    }
    return nullptr;
}

void PosProducer::FloodProposalSplit(const CBlock& a, const CBlock& b)
{
    if (!m_connman) return;
    int i = 0;
    m_connman->ForEachNode([&](CNode* pnode) {
        const CBlock& which = (i++ % 2 == 0) ? a : b;
        m_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetCommonVersion()).Make(NetMsgType::POSPROPOSAL, which));
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

void PosProducer::RecordCandidate(const std::shared_ptr<const CBlock>& block, const CPubKey& leader, const uint256& leader_beta, int height)
{
    // m_gossip_mutex held. One round set per height.
    if (height < m_round_height) return; // stale
    if (height > m_round_height) {
        m_round_height = height;
        m_round_start_ms = GetTimeMillis();
        m_candidates.clear();
        m_excluded.clear();
        m_collected.clear();
        m_backed_hash.SetNull();
        m_signed_round = -1;
    }
    // A leader excluded for equivocation (proposing two blocks this height) is
    // never recorded — the committee backs neither of its blocks.
    if (m_excluded.count(leader)) return;
    auto it = m_candidates.find(leader);
    if (it != m_candidates.end()) return;                       // already have this leader's block
    if (m_candidates.size() >= (size_t)MAX_POS_AGG_COMMITTEE_SIZE) return;
    m_candidates.emplace(leader, RoundCandidate{block, leader_beta});
}

std::shared_ptr<const CBlock> PosProducer::BackedForRound(int r) const
{
    // m_gossip_mutex held. Candidates are ordered by (1) freshest Bitcoin anchor,
    // then (2) lowest leader VRF; the round-r leader is the (r+1)-th in that order.
    // The anchor-freshness preference (paper Principle 7, rule III) keeps the tip
    // tracking Bitcoin's tip: a proposal referencing a more recent Bitcoin block
    // is backed over a staler one, so producers are incentivised to anchor fresh.
    // In the common case all proposals carry the same (freshest) anchor, so it
    // reduces to pure lowest-VRF election; it only bites when a producer is stale.
    if (r < 0 || (size_t)r >= m_candidates.size()) return nullptr;
    std::vector<const RoundCandidate*> ordered;
    ordered.reserve(m_candidates.size());
    for (const auto& [leader, cand] : m_candidates) ordered.push_back(&cand);
    std::sort(ordered.begin(), ordered.end(),
              [](const RoundCandidate* a, const RoundCandidate* b) {
                  if (a->block->m_anchor_height != b->block->m_anchor_height)
                      return a->block->m_anchor_height > b->block->m_anchor_height; // fresher anchor first
                  return a->beta < b->beta;                                          // then lowest VRF
              });
    return ordered[r]->block;
}

void PosProducer::ProposeGossip(const CKey& leader_key)
{
    int height = 0;
    auto block = BuildUnsignedBlsBlock(m_chainman, m_mempool, m_chainparams, leader_key, height);
    if (!block) {
        LogPrint(BCLog::VALIDATION, "PoS gossip: failed to build a proposal\n");
        return;
    }
    // The leader authorises its block by signing the (member-independent) hash,
    // and ships that signature inside the proposal — staged in the otherwise-empty
    // proof solution, which is excluded from the hash — so any node that later
    // gathers a quorum of shares can assemble the certificate without us.
    const uint256 hash = block->GetHash();
    std::vector<unsigned char> leader_sig;
    if (!leader_key.Sign(hash, leader_sig)) {
        LogPrint(BCLog::VALIDATION, "PoS gossip: failed to sign proposal\n");
        return;
    }
    block->proof.solution = CScript() << leader_sig;
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
        RecordCandidate(block, leader_key.GetPubKey(), own_beta, height);
    }

    if (m_byzantine_equivocate) {
        // Fault injection: build a second, different valid block (later nTime →
        // different hash, same leader/VRF), re-sign it, and send the two blocks to
        // disjoint halves of the committee to split it. We also contribute no
        // shares (below), so neither half can reach a quorum — exactly the
        // present-but-failing leader the round-robin must route around.
        auto block_b = std::make_shared<CBlock>(*block);
        block_b->nTime += 1;
        block_b->proof.solution = CScript();
        std::vector<unsigned char> sig_b;
        if (leader_key.Sign(block_b->GetHash(), sig_b)) {
            block_b->proof.solution = CScript() << sig_b;
            LogPrintf("PoS gossip: BYZANTINE equivocating %s / %s at height %d\n",
                      hash.GetHex(), block_b->GetHash().GetHex(), height);
            FloodProposalSplit(*block, *block_b);
            return;
        }
    }

    if (m_byzantine_invalid) {
        // Fault injection: propose a consensus-invalid block (a second coinbase →
        // bad-cb-multiple). Cheap checks (VRF, leader signature) pass, so it is
        // recorded as a candidate; lazy validation must reject it at sign time and
        // exclude the leader.
        auto bad = std::make_shared<CBlock>(*block);
        bad->vtx.push_back(bad->vtx[0]);
        bad->hashMerkleRoot = BlockMerkleRoot(*bad);
        bad->proof.solution = CScript();
        std::vector<unsigned char> sig_bad;
        if (leader_key.Sign(bad->GetHash(), sig_bad)) {
            bad->proof.solution = CScript() << sig_bad;
            LogPrintf("PoS gossip: BYZANTINE proposing invalid block %s at height %d\n", bad->GetHash().GetHex(), height);
            FloodCompactProposal(*bad);
            return;
        }
    }

    LogPrintf("PoS gossip: proposing block %s at height %d\n", hash.GetHex(), height);
    FloodCompactProposal(*block);
}

int64_t PosProducer::DriveRound()
{
    // Proposals are collected for WINDOW_MS, then we sign round 0's backed leader
    // (the lowest-VRF candidate). Each subsequent ROUND_MS advances the round
    // index: the backed leader becomes the next-lowest-VRF candidate, so a leader
    // whose round failed to certify (it withheld, equivocated into a sub-quorum
    // split, or too few of its backers were online) is deterministically excluded
    // and the committee converges on the next leader in lockstep — the paper's
    // round-robin re-vote (P6 §9). The index is derived from the (aligned) clock,
    // so all nodes step through the same leader order together.
    static const int64_t WINDOW_MS = 500;
    static const int64_t ROUND_MS = 700;
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip) return POS_PRODUCER_POLL_MS;
    const int height = tip->nHeight + 1;
    const int64_t now = GetTimeMillis();

    // Determine the current round index and the proposal it backs.
    int round_index;
    std::shared_ptr<const CBlock> backed;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height != height) return POS_PRODUCER_POLL_MS; // no active round yet
        const int64_t elapsed = now - m_round_start_ms;
        if (elapsed < WINDOW_MS) return 150;                        // still collecting proposals
        round_index = (int)((elapsed - WINDOW_MS) / ROUND_MS);
        backed = BackedForRound(round_index);
        if (!backed) {
            // We have excluded every candidate we know without certifying. This
            // needs more than (#candidates) leaders to have failed — beyond our
            // fault assumption. Restart collection so producers re-seed candidates.
            m_round_height = 0;
            m_candidates.clear();
            m_collected.clear();
            m_backed_hash.SetNull();
            m_signed_round = -1;
            return 150;
        }
    }

    // Decide whether this is a new proposal to sign — the round advanced, or the
    // backed leader changed (the one we were backing was excluded). Don't commit
    // until we've validated it.
    bool want_sign = false;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height && (m_signed_round < round_index || m_backed_hash != backed->GetHash())) {
            want_sign = true;
        }
    }
    if (want_sign && !m_byzantine_equivocate) {
        // Lazy validation (paper P6 step 10): validate the backed block only now,
        // when we are about to sign it — not every proposal on the message thread.
        // An invalid block excludes its leader; next pass we back the next-lowest
        // valid candidate.
        bool valid = false;
        {
            LOCK(cs_main);
            CBlockIndex* tip2 = m_chainman.ActiveChain().Tip();
            if (tip2 && backed->hashPrevBlock == tip2->GetBlockHash()) {
                BlockValidationState state;
                valid = TestBlockValidity(state, m_chainparams, m_chainman.ActiveChainstate(), *backed, tip2,
                                          /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true);
            }
        }
        if (!valid) {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            if (auto parts = ParsePosBlockChallenge(backed->proof.challenge)) {
                m_excluded.insert(parts->leader);
                m_candidates.erase(parts->leader);
            }
            return 150; // re-pick the next-lowest valid leader promptly
        }
        // Valid: commit, collect our own shares (we are a potential aggregator)
        // and flood them (so every node is too).
        std::vector<PosShare> shares = MakeLocalShares(*backed);
        {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            if (m_round_height == height) {
                m_signed_round = round_index;
                if (m_backed_hash != backed->GetHash()) {
                    m_backed_hash = backed->GetHash();
                    m_collected.clear(); // shares were for the previous backed leader
                }
                for (PosShare& sh : shares) {
                    if (m_collected.size() < (size_t)MAX_POS_AGG_COMMITTEE_SIZE) m_collected[sh.pubkey] = sh;
                }
            }
        }
        for (const PosShare& sh : shares) FloodShare(sh);
    }

    // Any node holding a quorum of shares for the backed proposal assembles the
    // certificate and submits — the leader's authorising signature is in the
    // proposal, so no single aggregator can stall the round. Competing assemblies
    // share the same block hash, so duplicates are dropped by ProcessNewBlock.
    std::shared_ptr<CBlock> final_block;
    uint256 final_hash;
    size_t members_n = 0;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height && m_backed_hash == backed->GetHash() &&
            (int)m_collected.size() >= PosQuorum((size_t)g_pos_committee_size)) {
            CScript::const_iterator pc = backed->proof.solution.begin();
            opcodetype op;
            std::vector<unsigned char> leader_sig;
            if (backed->proof.solution.GetOp(pc, op, leader_sig) && !leader_sig.empty()) {
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
                    final_block = std::make_shared<CBlock>(*backed);
                    final_block->proof.solution = BuildPosBlsSolution(leader_sig, *agg, members);
                    final_hash = backed->GetHash();
                    members_n = members.size();
                    m_collected.clear();   // assembled; do not re-assemble until the tip advances
                    m_backed_hash.SetNull();
                }
            }
        }
    }
    if (final_block) {
        if (m_chainman.ProcessNewBlock(m_chainparams, final_block, /*force_processing=*/true, nullptr)) {
            LogPrintf("PoS gossip: certified block %s at height %d (round %d, %d committee member(s))\n",
                      final_hash.GetHex(), height, round_index, (int)members_n);
        } else {
            LogPrint(BCLog::VALIDATION, "PoS gossip: assembled block %s was not accepted\n", final_hash.GetHex());
        }
    }

    // Poll fast while a round for the current height is still in progress.
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height) return 150;
    }
    return POS_PRODUCER_POLL_MS;
}

PosGossipAction PosProducer::OnProposal(const std::shared_ptr<const CBlock>& block)
{
    const uint256 hash = block->GetHash();
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (!m_seen_proposals.insert(hash).second) return PosGossipAction::Ignore; // already seen
        if (m_seen_proposals.size() > 20000) m_seen_proposals.clear();
    }
    // A posproposal that is not even the BLS committee form is malformed: no
    // honest node would originate or relay it.
    auto parts = ParsePosBlockChallenge(block->proof.challenge);
    if (!parts || !parts->is_bls) return PosGossipAction::Invalid;
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip) return PosGossipAction::Ignore;
    if (block->hashPrevBlock != tip->GetBlockHash()) return PosGossipAction::Ignore; // not on our tip (benign race)
    const int height = tip->nHeight + 1;
    // The leader must prove sortition eligibility (objective: an honest relayer
    // checked this, so a failure means the sender sent garbage). Reject a
    // non-staker leader with the cheap registry lookup before the VRF verify, so
    // forged proposals cost an attacker as little of our CPU as possible.
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    const uint64_t weight = reg.GetWeight(parts->leader);
    if (weight == 0) return PosGossipAction::Invalid;
    const uint256 seed = PosSeedForChild(tip);
    auto leader_proof = ExtractPosVrfProof(*block);
    uint256 lbeta;
    if (!leader_proof || !VrfVerify(parts->leader, Span<const unsigned char>(seed.begin(), 32), *leader_proof, lbeta)) return PosGossipAction::Invalid;
    if (!PosVrfIsCommitteeMember(lbeta, weight, PosTotalWeight(reg))) return PosGossipAction::Invalid; // leader not sortitioned
    // The leader's authorising signature rides in the proposal's staging solution
    // (a single push). It must verify against the block hash so any node can later
    // assemble using it.
    {
        CScript::const_iterator pc = block->proof.solution.begin();
        opcodetype op;
        std::vector<unsigned char> leader_sig;
        if (!block->proof.solution.GetOp(pc, op, leader_sig) || leader_sig.empty() ||
            !parts->leader.Verify(hash, leader_sig)) {
            return PosGossipAction::Invalid;
        }
    }
    // Equivocation handling BEFORE the costly full validation (the leader's
    // signature already proves authorship of this hash). Two cases, both cheap:
    //   - The leader is already excluded for equivocation → drop silently.
    //   - The leader already has a *different* block this round → this is the
    //     equivocation: exclude the leader (the committee backs neither of its
    //     blocks, paper Liveness theorem 1) and RELAY this block as evidence, so
    //     every honest node detects it and converges on the next-lowest valid
    //     leader instead of splitting on the two. Bounds validation too: an
    //     equivocator's later blocks never reach TestBlockValidity.
    bool equivocation_evidence = false;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height == height) {
            if (m_excluded.count(parts->leader)) return PosGossipAction::Ignore;
            auto it = m_candidates.find(parts->leader);
            if (it != m_candidates.end()) {
                if (it->second.block->GetHash() != hash) {
                    m_excluded.insert(parts->leader); // exclude the equivocator
                    m_candidates.erase(it);               // drop the block we had backed too
                    equivocation_evidence = true;
                } else {
                    return PosGossipAction::Ignore;       // same block, duplicate
                }
            }
        }
    }
    if (equivocation_evidence) {
        LogPrintf("PoS gossip: leader %s equivocated at height %d — excluding it\n",
                  HexStr(parts->leader).substr(0, 16), height);
        Wake();                              // re-pick the backed leader promptly
        return PosGossipAction::Relay;       // propagate the evidence so all nodes exclude it
    }
    // Record the proposal as a candidate after only the cheap, objective checks
    // (sortition eligibility, leader signature, equivocation). Full block
    // validation (TestBlockValidity) is deferred to the moment we are about to
    // *sign* the backed proposal — paper Principle 6 step 10, "verify only after
    // the first vote, to reduce the effort of verifying more than one block." So
    // we validate ~one block per round instead of every proposal on the message
    // thread; an invalid backed block is caught at sign time and its leader is
    // excluded (the committee routes to the next valid leader).
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        RecordCandidate(block, parts->leader, lbeta, height);
    }
    Wake(); // let the worker drive this round promptly
    return PosGossipAction::Relay;
}

PosGossipAction PosProducer::OnShare(const PosShare& share)
{
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (!m_seen_shares.insert({share.block_hash, share.pubkey}).second) return PosGossipAction::Ignore;
        if (m_seen_shares.size() > 200000) m_seen_shares.clear();
    }
    // Crypto validation (registry-independent, objective): malformed sizes, a bad
    // proof-of-possession, or a signature that does not verify is provable garbage.
    if (share.bls_pubkey.size() != BLS_PK_SIZE || share.bls_pop.size() != BLS_SIG_SIZE ||
        share.bls_share.size() != BLS_SIG_SIZE || share.vrf_proof.size() != VRF_PROOF_SIZE || !share.pubkey.IsValid()) {
        return PosGossipAction::Invalid;
    }
    if (!BlsVerifyPossession(share.bls_pubkey, share.bls_pop)) return PosGossipAction::Invalid;
    if (!BlsVerify(share.bls_pubkey, Span<const unsigned char>(share.block_hash.begin(), 32), share.bls_share)) return PosGossipAction::Invalid;
    // Classify the share: for the proposal we are backing this round, for some
    // other known candidate (a different round's leader — relay so its aggregators
    // get it), or for nothing we know (junk — drop, do not amplify).
    bool is_backed = false, is_candidate = false;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        is_backed = (!m_backed_hash.IsNull() && share.block_hash == m_backed_hash);
        if (!is_backed) {
            for (const auto& [leader, cand] : m_candidates) {
                if (cand.block->GetHash() == share.block_hash) { is_candidate = true; break; }
            }
        }
    }
    if (!is_backed) return is_candidate ? PosGossipAction::Relay : PosGossipAction::Ignore;
    // For the backed proposal we can (and must) check the signer's sortition. The
    // proposal extends the active tip, so the slot seed is that of the tip.
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip) return PosGossipAction::Relay;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height != tip->nHeight + 1 || m_backed_hash != share.block_hash) return PosGossipAction::Relay; // round moved on
    }
    const uint256 seed = PosSeedForChild(tip);
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    const uint64_t weight = reg.GetWeight(share.pubkey);
    if (weight == 0) return PosGossipAction::Invalid;
    uint256 beta;
    if (!VrfVerify(share.pubkey, Span<const unsigned char>(seed.begin(), 32), share.vrf_proof, beta)) return PosGossipAction::Invalid;
    if (!PosVrfIsCommitteeMember(beta, weight, PosTotalWeight(reg))) return PosGossipAction::Invalid;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_backed_hash == share.block_hash && m_collected.size() < (size_t)MAX_POS_AGG_COMMITTEE_SIZE) {
            m_collected[share.pubkey] = share;
        }
    }
    Wake(); // a new share may complete the quorum
    return PosGossipAction::Relay;
}
