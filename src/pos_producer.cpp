// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_producer.h>

#include <anchor.h>
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
#include <chrono>
#include <map>
#include <memory>

using node::BlockAssembler;
using node::CBlockTemplate;
using node::IncrementExtraNonce;

//! Derive a staker's BLS secret seed deterministically from its secp256k1
//! staking key (domain-separated by a tag), so a staker needs no separate BLS
//! key to manage. Declared in pos.h (global) so the config layer and the
//! getblsregistration RPC can derive a staker's committee registration; the
//! registered BLS public key MUST equal BlsDerivePubKey of this seed.
std::vector<unsigned char> PosBlsSeedFromKey(const CKey& key)
{
    static const std::string tag = "sequentia/pos-bls/v1";
    unsigned char out[32];
    CSHA256().Write(reinterpret_cast<const unsigned char*>(tag.data()), tag.size())
             .Write(key.begin(), key.size())
             .Finalize(out);
    return std::vector<unsigned char>(out, out + 32);
}

namespace {
//! Build the signer bitfield for a bitfield BLS certificate (impl spec Option A
//! phase 2): bit i is set iff the i-th member of the ordered public committee
//! is in `signers`. Producer and validator derive the committee identically, so
//! the certificate needs no member keys — the validator reads them from the
//! registry by committee position.
std::vector<unsigned char> BuildSignerBitfield(const std::vector<CPubKey>& committee,
                                               const std::set<CPubKey>& signers)
{
    std::vector<unsigned char> bf;
    for (size_t i = 0; i < committee.size(); ++i) {
        if (signers.count(committee[i])) PosBitfieldSet(bf, i);
    }
    return bf;
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
    // SEQUENTIA PoS: pay the block's fees to the elected leader's own key (the
    // producer is paid only in fees; there is no subsidy). Consensus binds this
    // from pos_coinbase_leader_height onward, so a leader that pays elsewhere is
    // rejected. Fall back to the legacy anyone-can-spend only for non-PoS chains.
    // The payee is the leader unless it has committed a payout policy (DIRECT
    // redirect, or a LOTTERY draw among its delegators). ConnectBlock enforces
    // exactly this function, so a producer that pays elsewhere is rejected.
    if (feeDest == CScript()) feeDest = g_con_pos ? PosRequiredCoinbaseScript(pubkey, tip->nHeight + 1, seed) : (CScript() << OP_TRUE);
    std::unique_ptr<CBlockTemplate> tmpl;
    try {
        tmpl = BlockAssembler(chainman.ActiveChainstate(), mempool, chainparams)
            .CreateNewBlock(feeDest, std::chrono::seconds(0), nullptr, &vrf_commitments, &pubkey, &*proof, nullptr);
    } catch (const std::exception& e) {
        // CreateNewBlock throws if its final TestBlockValidity fails — e.g. the tip
        // advanced between computing our VRF proof (against the old parent) and
        // assembly, so the proof no longer matches. That is a transient race, not a
        // fault: skip this slot and the next poll re-proposes on the current tip.
        // A staker must never abort the daemon over one unbuildable slot.
        LogPrint(BCLog::VALIDATION, "PoS gossip: skipping proposal, block assembly failed: %s\n", e.what());
        return nullptr;
    }
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
    // SEQUENTIA PoS: fees are paid to the elected leader's own key (see
    // PosLeaderFeeScript); consensus enforces it from pos_coinbase_leader_height.
    if (feeDestinationScript == CScript()) feeDestinationScript = g_con_pos ? PosRequiredCoinbaseScript(pubkey, tip->nHeight + 1, seed) : (CScript() << OP_TRUE);

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
        vrf_slot = PosExpRaceActive(chainparams.GetConsensus(), tip->nHeight + 1)
                       ? PosVrfSlotExp(vrf_output, registry.GetWeight(pubkey), total_weight)
                       : PosVrfSlot(vrf_output, registry.GetWeight(pubkey), total_weight);
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
            const int member_cap = g_pos_public_committee ? PosMaxCommitteeMembers() :
                (g_pos_agg_committee || g_pos_bls) ? MAX_POS_AGG_COMMITTEE_SIZE : MAX_POS_COMMITTEE_SIZE;
            // Under the public fixed-size committee, membership is the
            // deterministic schedule prefix; the VRF proof is still produced
            // (the certificate format carries it) but no longer decides
            // membership and is not verified by validators in this mode.
            std::set<CPubKey> public_committee;
            if (g_pos_public_committee) public_committee = PosPublicCommitteeSet(registry, seed);
            int eligible = 0;
            for (const auto& [member_pub, member_key] : candidates) {
                if ((int)vrf_committee.size() >= member_cap) break;
                if (!PosIsEligibleStake(registry.GetWeight(member_pub))) continue;
                if (g_pos_public_committee && !public_committee.count(member_pub)) continue;
                auto member_proof = VrfProve(member_key, Span<const unsigned char>(seed.begin(), 32));
                if (!member_proof) continue;
                uint256 member_beta;
                if (!VrfVerify(member_pub, Span<const unsigned char>(seed.begin(), 32), *member_proof, member_beta)) continue;
                if (!g_pos_public_committee && !PosVrfIsCommitteeMember(member_beta, registry.GetWeight(member_pub), total_weight)) continue;
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
            const int quorum = PosSlotQuorum(registry);
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

    std::unique_ptr<CBlockTemplate> pblocktemplate;
    try {
        pblocktemplate = BlockAssembler(chainman.ActiveChainstate(), mempool, chainparams)
            .CreateNewBlock(feeDestinationScript, std::chrono::seconds(0), nullptr,
                            g_pos_vrf ? &vrf_commitments : nullptr, &pubkey,
                            g_pos_vrf ? &vrf_proof : nullptr,
                            (g_pos_vrf && !vrf_committee.empty()) ? &vrf_committee : nullptr);
    } catch (const std::exception& e) {
        // CreateNewBlock throws if its final TestBlockValidity fails (e.g. the tip
        // advanced between computing our VRF proof and assembly, leaving the proof
        // bound to a stale parent). Treat it as a transient, recoverable skip — a
        // staker must not abort the daemon over one unbuildable slot.
        error = strprintf("Block assembly failed: %s", e.what());
        err_kind = PosProduceError::MISC;  // recoverable: a stale-tip race or, via RPC, too few committee keys
        return false;
    }
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
        if (g_pos_public_committee) {
            // Bitfield certificate (impl spec Option A phase 2): name the
            // signers by a bitfield over the ordered public committee instead
            // of carrying each member's key and proof.
            const StakeRegistry& reg = StakeRegistry::GetInstance();
            std::vector<CPubKey> committee = PosPublicCommittee(reg, seed);
            std::set<CPubKey> signers;
            for (const PosBlsMember& m : bls_members) signers.insert(m.pubkey);
            block.proof.solution = BuildPosBlsBitfieldSolution(leader_sig, *agg_sig, BuildSignerBitfield(committee, signers));
        } else {
            block.proof.solution = BuildPosBlsSolution(leader_sig, *agg_sig, bls_members);
        }
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
//! How long a node holds production at a height for which it knows a quorum
//! certificate but lacks the block body (3A Tier 1). Long enough for any peer
//! holding the ~few-hundred-byte-certified block to serve it (one
//! getposproposal round-trip), short enough that a certificate whose body is
//! deliberately withheld costs a bounded pause, never a deadlock.
static constexpr int64_t POS_CERT_HOLD_MS = 30000;

PosProducer::PosProducer(ChainstateManager& chainman, CTxMemPool& mempool,
                         const CChainParams& chainparams, CConnman* connman, std::vector<CKey> keys)
    : m_chainman(chainman), m_mempool(mempool), m_chainparams(chainparams),
      m_connman(connman), m_keys(std::move(keys))
{
    m_byzantine_equivocate = gArgs.GetBoolArg("-posbyzantineequivocate", false);
    m_byzantine_invalid = gArgs.GetBoolArg("-posbyzantineinvalid", false);
    m_debug_round_skew_ms = gArgs.GetIntArg("-posdebugroundskewms", 0);
    m_recovery_wait_ms = std::clamp<int64_t>(gArgs.GetIntArg("-posanchorrecoverywait", 30), 0, 3600) * 1000;
    // Local liveness timings, NOT consensus rules (every node derives the
    // round index from the same global anchor, so nodes with different
    // values still agree on the round): an operator on weak hardware or a
    // slow link can lengthen them without asking anyone. 0 = the per-member
    // formula default (500+25/member window, 700+35/member rounds).
    m_window_override_ms = std::clamp<int64_t>(gArgs.GetIntArg("-poswindowms", 0), 0, 60000);
    m_round_override_ms = std::clamp<int64_t>(gArgs.GetIntArg("-posroundms", 0), 0, 60000);
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

    // A parent-chain (Bitcoin) reorganization can roll our anchored tip BACKWARD
    // (validation.cpp invalidates every block whose anchor left the parent's best
    // chain, rolling back to the last anchor-canonical height) or replace the
    // block at the current height. Our per-height round high-water marks
    // (m_round_height, m_proposed_height) are monotonic, so after a backward move
    // they sit ABOVE the now-current height: no leader would propose again at
    // that height (Step()'s `m_proposed_height < height` gate) and receivers
    // would reject any lower-height proposal (OnProposal's `height < m_round_height`
    // gate) — a permanent stall, even though a block extending the rolled-back
    // tip with a fresh anchor is fully consensus-valid. Detect a non-forward tip
    // change and reset the round state so the committee resumes on the new tip. A
    // pure forward advance leaves the marks below next_height and needs no reset.
    {
        const uint256 tip_hash = tip->GetBlockHash();
        const int next_height = tip->nHeight + 1;
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (tip_hash != m_last_tip) {
            if (m_round_height >= next_height || m_proposed_height >= next_height) {
                LogPrintf("PoS producer: tip moved to %s (height %d) at/below tracked round height "
                          "%d/%d; resetting round state (parent-chain reorg recovery)\n",
                          tip_hash.ToString(), tip->nHeight, m_round_height, m_proposed_height);
                m_round_height = 0;
                m_proposed_height = 0;
                m_candidates.clear();
                m_collected.clear();
                m_excluded.clear();
                m_backed_hash.SetNull();
                m_signed_round = -1;
            }
            m_last_tip = tip_hash;
        }
    }

    // Committee-equivocation prevention (Change 4a). After an anchor rollback
    // this producer would otherwise treat the rolled-back height as vacant
    // IMMEDIATELY (the reset above cleared the round marks and the slot
    // deadlines are long past), while restoring the original quorum-certified
    // block takes the anchor watcher at least one poll tick plus a live parent
    // check. Losing that race mints a rival that part of the committee then
    // certifies: two quorum-certified siblings at one height, both anchored to
    // the returned parent chain — a committee equivocation immediate finality
    // freezes on different nodes (the live 96/4 split) and anchoring cannot
    // arbitrate. So while the watcher holds a quorum-certified child of our tip
    // that is not confirmed off the parent's best chain, neither propose nor
    // drive rounds (no rival backed or signed) at this height. The hold
    // resolves by verdict — the block is restored (tip advances past it) or its
    // anchor is confirmed stale (a genuine parent departure; produce as usual,
    // which is why a normal forward reorg never holds: the invalidation walk
    // caches the stale verdict before the producer ever sees the rollback) —
    // and is bounded by -posanchorrecoverywait so an unreachable parent daemon
    // can only ever delay production, never deadlock it. Rival proposals and
    // shares arriving meanwhile are still recorded and relayed (OnProposal is
    // deliberately untouched): the hold withholds only OUR proposal and OUR
    // signature; relaying keeps the node a good gossip citizen and the
    // recorded candidates stay usable if the hold expires.
    if (m_recovery_wait_ms > 0) {
        const uint256 tip_hash = tip->GetBlockHash();
        const std::optional<uint256> pending =
            AnchorCertifiedSiblingPending(m_chainman, tip_hash, tip->nHeight + 1);
        if (pending) {
            // Steady clock: an NTP step must not stretch or shrink the
            // bounded patience.
            const int64_t now_hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (m_recovery_hold_tip != tip_hash) {
                m_recovery_hold_tip = tip_hash;
                m_recovery_hold_since_ms = now_hold_ms;
                m_recovery_hold_expired_logged = false;
                LogPrintf("PoS producer: holding production at height %d: quorum-certified block %s awaits anchor recovery\n",
                          tip->nHeight + 1, pending->ToString());
            }
            if (now_hold_ms - m_recovery_hold_since_ms < m_recovery_wait_ms) {
                return 500; // re-evaluate shortly; recovery normally lands within one watcher tick
            }
            if (!m_recovery_hold_expired_logged) {
                m_recovery_hold_expired_logged = true;
                LogPrintf("PoS producer: anchor-recovery hold at height %d expired after %ds; resuming production\n",
                          tip->nHeight + 1, (int)(m_recovery_wait_ms / 1000));
            }
        } else if (!m_recovery_hold_tip.IsNull()) {
            m_recovery_hold_tip.SetNull();
        }
    }

    // Certificate pin (honest-splits fix 3A, Tier 1): a verified quorum
    // certificate for the next height means that height is TAKEN — proposing,
    // backing or signing a rival there (including via the escaping-stall
    // valve) is exactly the asymmetric-finality split. Try to complete the
    // certified block from a held proposal body; otherwise hold production at
    // this height while the body is fetched. The hold is time-bounded so a
    // deliberately withheld body degrades into a bounded pause, not a
    // deadlock (the share-lock is where the stronger guarantee lands).
    {
        bool held = false;
        {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            // Prune certificates for heights the chain has passed.
            while (!m_certified_heights.empty() && m_certified_heights.begin()->first <= tip->nHeight) {
                m_certified.erase(m_certified_heights.begin()->second);
                m_certified_heights.erase(m_certified_heights.begin());
            }
            held = m_certified_heights.count(tip->nHeight + 1) > 0;
        }
        if (held) {
            if (TryConnectCertified()) return 100; // connected; re-evaluate at the new tip
            const int64_t now_hold = GetTimeMillis();
            {
                std::lock_guard<std::mutex> lock(m_gossip_mutex);
                if (m_cert_hold_height != tip->nHeight + 1) {
                    m_cert_hold_height = tip->nHeight + 1;
                    m_cert_hold_since_ms = now_hold;
                    m_cert_hold_logged = false;
                }
                if (now_hold - m_cert_hold_since_ms < POS_CERT_HOLD_MS) {
                    if (!m_cert_hold_logged) {
                        m_cert_hold_logged = true;
                        LogPrintf("PoS producer: holding production at height %d: a quorum certificate is known; awaiting the block body\n",
                                  tip->nHeight + 1);
                    }
                    held = true;
                } else {
                    held = false; // patience expired: resume (bounded liveness)
                }
            }
            if (held) return 500;
        }
    }

    const uint256 seed = PosSeedForChild(tip);
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const uint64_t total_weight = PosTotalWeight(registry);

    // Elect the best-ranked (lowest-slot) of our keys to lead this round. Under
    // VRF sortition the slot is private (computed from our own key); we cannot
    // see other stakers' slots, so we simply race at our slot time — a lower
    // slot elsewhere produces first and wakes us for the next height.
    // Under the public fixed-size committee, MEMBERSHIP is the deterministic
    // schedule prefix (leader election stays private-VRF).
    std::set<CPubKey> public_committee;
    if (g_pos_public_committee) public_committee = PosPublicCommitteeSet(registry, seed);
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
            slot = PosExpRaceActive(m_chainparams.GetConsensus(), tip->nHeight + 1)
                       ? PosVrfSlotExp(beta, registry.GetWeight(pub), total_weight)
                       : PosVrfSlot(beta, registry.GetWeight(pub), total_weight);
            if (g_pos_public_committee ? public_committee.count(pub) > 0
                                       : PosVrfIsCommitteeMember(beta, registry.GetWeight(pub), total_weight)) {
                local_committee_eligible++;
            }
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

    const int quorum = PosSlotQuorum(registry);

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
    // Under the public fixed-size committee, membership is the deterministic
    // schedule prefix; the VRF proof still rides in the share (certificate
    // format) but no longer decides membership.
    std::set<CPubKey> public_committee;
    if (g_pos_public_committee) public_committee = PosPublicCommitteeSet(reg, seed);
    for (const CKey& k : m_keys) {
        const CPubKey pub = k.GetPubKey();
        if (!PosIsEligibleStake(reg.GetWeight(pub))) continue;
        if (g_pos_public_committee && !public_committee.count(pub)) continue;
        auto proof = VrfProve(k, Span<const unsigned char>(seed.begin(), 32));
        if (!proof) continue;
        uint256 beta;
        if (!VrfVerify(pub, Span<const unsigned char>(seed.begin(), 32), *proof, beta)) continue;
        if (!g_pos_public_committee && !PosVrfIsCommitteeMember(beta, reg.GetWeight(pub), total)) continue;
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
    // then (2) the leader's election key; the round-r leader is the (r+1)-th in
    // that order. The anchor-freshness preference (paper Principle 7, rule III)
    // keeps the tip tracking Bitcoin's tip: a proposal referencing a more recent
    // Bitcoin block is backed over a staler one, so producers are incentivised to
    // anchor fresh. In the common case all proposals carry the same (freshest)
    // anchor, so it reduces to a pure election among equally-fresh proposals.
    //
    // The election key is the fork-gated sortition: from pos_exprace_height the
    // exponential-race score (split-neutral, exactly stake-proportional); below
    // it, the legacy raw leader VRF (beta). Every node gates on the same height
    // (m_round_height, the block's own height) so all honest nodes converge on
    // the identical leader — a divergence here would split the committee.
    if (r < 0 || (size_t)r >= m_candidates.size()) return nullptr;

    const bool exprace = PosExpRaceActive(m_chainparams.GetConsensus(), m_round_height);
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const uint64_t total_weight = exprace ? PosTotalWeight(registry) : 0;

    struct Entry { const RoundCandidate* cand; arith_uint256 score; };
    std::vector<Entry> ordered;
    ordered.reserve(m_candidates.size());
    for (const auto& [leader, cand] : m_candidates) {
        arith_uint256 score;
        if (exprace) score = PosVrfScoreExp(cand.beta, registry.GetWeight(leader), total_weight);
        ordered.push_back({&cand, score});
    }
    std::sort(ordered.begin(), ordered.end(),
              [exprace](const Entry& a, const Entry& b) {
                  if (a.cand->block->m_anchor_height != b.cand->block->m_anchor_height)
                      return a.cand->block->m_anchor_height > b.cand->block->m_anchor_height; // fresher anchor first
                  if (exprace) return a.score < b.score;   // exp-race: lowest weighted score
                  return a.cand->beta < b.cand->beta;       // legacy: lowest raw VRF
              });
    return ordered[r].cand->block;
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
    // round-robin re-vote (P6 §9).
    //
    // The round index MUST be derived from a network-global reference, not each
    // node's local round-start: anchoring to a local arrival time lets gossip
    // latency put two honest nodes in different rounds at the same instant, so
    // they sign different leaders' blocks at the same height — two sub-quorum
    // halves, or, worse, two competing quorums and a fork. We anchor instead to
    // the round-0 leader's own block timestamp: the leader stamps it at ~real
    // production time, and every node reads the identical value off the same
    // gossiped block, so all honest nodes compute the same round_index together.
    // (Requires only loose clock agreement, well within one slot — the same
    // assumption the slot schedule already makes.)
    // Scale the collection window and round length with committee size: at large
    // N, every node's proposal (to agree on the lowest-VRF leader) and a quorum of
    // BLS shares must propagate across a multi-hop gossip mesh and be verified
    // within the window/round, which the small-committee 500/700 ms values are far
    // too tight for (a 100-member committee stalls — nodes never converge on one
    // leader with a quorum). These are local liveness timings, identical on every
    // node, not consensus rules, so enlarging them is safe. ~25 ms/member of
    // window and ~35 ms/member of round keeps small committees near the old values
    // while giving a 100-member committee ~3 s to collect.
    const int64_t WINDOW_MS = m_window_override_ms > 0 ? m_window_override_ms
        : 500 + 25 * (int64_t)g_pos_committee_size;
    const int64_t ROUND_MS = m_round_override_ms > 0 ? m_round_override_ms
        : 700 + 35 * (int64_t)g_pos_committee_size;
    CBlockIndex* tip;
    {
        LOCK(cs_main);
        tip = m_chainman.ActiveChain().Tip();
    }
    if (!tip) return POS_PRODUCER_POLL_MS;
    const int height = tip->nHeight + 1;
    // m_debug_round_skew_ms is a DEBUG-ONLY injection of inter-node clock skew
    // (-posdebugroundskewms), used to measure how much synchrony loss the
    // global-clock round anchor tolerates before rounds desync. Always 0 in
    // production.
    const int64_t now = GetTimeMillis() + m_debug_round_skew_ms;

    // Certificate pin (3A Tier 1): once a quorum certificate for this height
    // is known, the round is OVER — sign nothing more here (this is also the
    // 3B round-advance re-vote cut short: a certificate that arrives near a
    // round boundary stops the re-vote instead of racing it). Complete the
    // block from a held proposal body if we can.
    {
        bool certified_here = false;
        {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            certified_here = m_certified_heights.count(height) > 0;
        }
        if (certified_here) {
            TryConnectCertified();
            return 150; // poll while the body is fetched / the tip advances
        }
    }

    // Determine the current round index and the proposal it backs.
    int round_index;
    std::shared_ptr<const CBlock> backed;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_round_height != height) return POS_PRODUCER_POLL_MS; // no active round yet
        std::shared_ptr<const CBlock> round0 = BackedForRound(0);
        if (!round0) return 150;                                    // no candidates yet, still collecting
        const int64_t anchor_ms = (int64_t)round0->nTime * 1000;    // network-global schedule origin
        const int64_t elapsed = now - anchor_ms;
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
            // Re-arm our own proposal: restarting collection only helps if
            // producers actually re-seed, and Step()'s once-per-height gate
            // (m_proposed_height) would otherwise keep every node mute at this
            // height forever — no round ever forms again and the escaping-stall
            // valve, being only a quorum relaxation on an existing proposal,
            // has nothing to fire on: a permanent liveness deadlock. The
            // re-proposal is safe: the share-lock arms still gate what we SIGN,
            // and RecordCandidate re-clears the per-height exclusions when the
            // re-seeded round arrives.
            m_proposed_height = 0;
            return 150;
        }
    }

    // Share-lock, ancestry arm (3A residual): our current parent is a
    // same-height RIVAL of a block we share-signed. Signing on top of it
    // would help certify a chain that orphans a block that may have quietly
    // certified elsewhere. Hold until the escaping-stall anchor gap passes
    // (the same valve the whitepaper uses for a genuine stall: a partition
    // hiding X's certificate can outlast any number of rounds, so the
    // release is keyed on parent-chain progress, not the clock) — or, when
    // anchoring is off, a bounded grace of two rounds.
    {
        bool ancestry_hold = false;
        uint256 locked;
        {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            if (m_lock_height == tip->nHeight && !m_lock_hash.IsNull() &&
                m_lock_hash != tip->GetBlockHash()) {
                if (m_lock_grace_start_ms == 0) m_lock_grace_start_ms = now;
                locked = m_lock_hash;
                // The release is keyed on the same evidence consensus accepts
                // for escaping a stall: parent-chain height gap AND real-time
                // MTP gap (anchor.h, incident 2026-07-17) — a height race
                // during a parent block-storm must not unlock the hold.
                const bool gap_passed = g_con_bitcoin_anchor && tip->pprev &&
                    PosEscapingStallAllowed(tip->pprev->m_anchor_height, backed->m_anchor_height) &&
                    CheckEscapingStallMtpGap(tip->pprev->m_anchor_hash, backed->m_anchor_hash) == EscapeStallTimeVerdict::ALLOWED;
                const bool grace_passed = !g_con_bitcoin_anchor &&
                    (now - m_lock_grace_start_ms >= 2 * ROUND_MS);
                ancestry_hold = !(gap_passed || grace_passed);
            }
        }
        if (ancestry_hold) {
            SendCertQueryOnce(locked);
            return 150; // keep polling: a certificate for X releases via the pin, the gap/grace via time
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
        // Share-lock, same-height arm (3B): we already share-signed a
        // DIFFERENT block at this height. The round schedule says re-vote,
        // but our shares for the first block are still live and assemblable:
        // signing the rival now is exactly the protocol-induced double-sign
        // that lets two blocks certify at one height. Ask the network for a
        // certificate on the first block (a certificate anywhere is one
        // ~300-byte round-trip away) and wait ONE grace round; only silence
        // releases the re-vote. With every signer of X held this way, a
        // rival cannot reach quorum while X can still certify.
        if (want_sign && !m_lock_hash.IsNull() && m_lock_height == height &&
            m_lock_hash != backed->GetHash()) {
            if (m_lock_grace_start_ms == 0) {
                m_lock_grace_start_ms = now;
                want_sign = false;
            } else if (now - m_lock_grace_start_ms < ROUND_MS) {
                want_sign = false;
            }
            // else: grace expired with no certificate — the re-vote proceeds.
        }
    }
    {
        bool need_query = false;
        uint256 locked;
        {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            if (!want_sign && !m_lock_queried && m_lock_grace_start_ms != 0 && !m_lock_hash.IsNull()) {
                need_query = true;
                locked = m_lock_hash;
            }
        }
        if (need_query) SendCertQueryOnce(locked);
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
                // Share-lock: remember what we signed here. Signing anything
                // ELSE at this height (or on a rival parent) is now gated on
                // the certificate query + grace above.
                m_lock_hash = backed->GetHash();
                m_lock_height = height;
                m_lock_grace_start_ms = 0;
                m_lock_queried = false;
                for (PosShare& sh : shares) {
                    if (m_collected.size() < (size_t)PosMaxCommitteeMembers()) m_collected[sh.pubkey] = sh;
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
        // Escaping stall (paper §i.5): when the chain has stalled — the parent
        // anchor advanced >= POS_ESCAPING_STALL_ANCHOR_GAP since the backed
        // block's parent — consensus accepts a sub-quorum (down to 1-member)
        // block (validation.cpp). Honor the same relaxation here so the
        // autonomous producer can self-certify under stall, including a lone
        // genesis founder bringing up the chain from a single staking output
        // (otherwise it would be stranded in the gossip path, unable to reach a
        // full quorum). In steady state Bitcoin is slower than the slot, so the
        // gap is never met and this stays a strict quorum.
        const int quorum = PosSlotQuorum(StakeRegistry::GetInstance());
        bool escaping_stall = g_con_bitcoin_anchor &&
            PosEscapingStallAllowed(tip->m_anchor_height, backed->m_anchor_height);
        // Escaping-stall real-time evidence (anchor.h, incident 2026-07-17):
        // consensus additionally requires a parent-chain MTP gap for sub-quorum
        // blocks, so honor it here too or the produced block is rejected
        // network-wide. Only consulted when the relaxation would actually be
        // used (shares below quorum) — a rare, genuinely-stalled situation —
        // and the MTP lookups are cached, so the parent-daemon RPC under the
        // gossip lock is a one-shot.
        if (escaping_stall && (int)m_collected.size() < quorum &&
            CheckEscapingStallMtpGap(tip->m_anchor_hash, backed->m_anchor_hash) != EscapeStallTimeVerdict::ALLOWED) {
            escaping_stall = false;
        }
        const int min_members = escaping_stall ? 1 : quorum;
        if (m_round_height == height && m_backed_hash == backed->GetHash() &&
            (int)m_collected.size() >= min_members) {
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
                    if (g_pos_public_committee) {
                        // Bitfield certificate (impl spec Option A phase 2).
                        const StakeRegistry& reg = StakeRegistry::GetInstance();
                        std::vector<CPubKey> committee = PosPublicCommittee(reg, PosSeedForChild(tip));
                        std::set<CPubKey> signers;
                        for (const PosBlsMember& m : members) signers.insert(m.pubkey);
                        final_block->proof.solution = BuildPosBlsBitfieldSolution(leader_sig, *agg, BuildSignerBitfield(committee, signers));
                    } else {
                        final_block->proof.solution = BuildPosBlsSolution(leader_sig, *agg, members);
                    }
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
            // Flood the certificate (the certified header) as its own small
            // object (3A Tier 1): the full block can be lost to a partition
            // or a slow flood; the ~few-hundred-byte certificate crosses on
            // any surviving path and pins every member that share-signed
            // the proposal — they already hold the body and connect on
            // receipt, so no rival is ever minted at this height.
            {
                std::lock_guard<std::mutex> lock(m_gossip_mutex);
                m_recent_certs[final_hash] = final_block->GetBlockHeader();
                if (m_recent_certs.size() > 100) m_recent_certs.erase(m_recent_certs.begin());
            }
            BroadcastCertificate(final_block->GetBlockHeader());
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
    // A body arriving for a block we hold a quorum certificate for (3A): it
    // is not a round candidate — attach the certificate and connect it. The
    // block hash excludes the proof solution, so the fetched body's hash
    // matches the certified header's; ProcessNewBlock still fully validates
    // (a body whose transactions do not match the merkle root is rejected).
    // Checked before the dedup so a re-fetched body always completes.
    {
        CBlockHeader cert_header;
        bool have_cert = false;
        {
            std::lock_guard<std::mutex> lock(m_gossip_mutex);
            auto it = m_certified.find(hash);
            if (it != m_certified.end()) {
                cert_header = it->second;
                have_cert = true;
            }
        }
        if (have_cert) {
            auto full = std::make_shared<CBlock>(*block);
            full->proof.solution = cert_header.proof.solution;
            if (m_chainman.ProcessNewBlock(m_chainparams, full, /*force_processing=*/true, nullptr)) {
                LogPrintf("PoS gossip: adopted certified block %s at height %d (body fetched after certificate)\n",
                          hash.GetHex(), (int)full->block_height);
            }
            return PosGossipAction::Ignore;
        }
    }
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
    // A member's proof-of-possession is the same bytes every block, so its
    // (costly) pairing check is cached after the first success — measured to
    // halve the per-share cost at large committees. The signature share is
    // per-block and always verified.
    uint256 pop_key;
    {
        CSHA256 hasher;
        hasher.Write(share.bls_pubkey.data(), share.bls_pubkey.size());
        hasher.Write(share.bls_pop.data(), share.bls_pop.size());
        hasher.Finalize(pop_key.begin());
    }
    bool pop_cached;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        pop_cached = m_pop_verified.count(pop_key) > 0;
    }
    if (!pop_cached) {
        if (!BlsVerifyPossession(share.bls_pubkey, share.bls_pop)) return PosGossipAction::Invalid;
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_pop_verified.size() > 4096) m_pop_verified.clear();
        m_pop_verified.insert(pop_key);
    }
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
    if (g_pos_public_committee) {
        // Membership is the deterministic schedule prefix; the share's VRF
        // proof is vestigial in this mode and not verified. The share must be
        // signed with the member's REGISTERED BLS key, since the certificate's
        // aggregate is verified against registry keys — a share under any other
        // key would only assemble into a certificate that fails validation.
        if (!PosPublicCommitteeSet(reg, seed).count(share.pubkey)) return PosGossipAction::Invalid;
        if (reg.GetBls(share.pubkey) != share.bls_pubkey) return PosGossipAction::Invalid;
    } else {
    uint256 beta;
    if (!VrfVerify(share.pubkey, Span<const unsigned char>(seed.begin(), 32), share.vrf_proof, beta)) return PosGossipAction::Invalid;
    if (!PosVrfIsCommitteeMember(beta, weight, PosTotalWeight(reg))) return PosGossipAction::Invalid;
    }
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_backed_hash == share.block_hash && m_collected.size() < (size_t)PosMaxCommitteeMembers()) {
            m_collected[share.pubkey] = share;
        }
    }
    Wake(); // a new share may complete the quorum
    return PosGossipAction::Relay;
}

void PosProducer::BroadcastCertificate(const CBlockHeader& header)
{
    if (!m_connman) return;
    m_connman->ForEachNode([&](CNode* pnode) {
        m_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetCommonVersion()).Make(NetMsgType::POSCERT, header));
    });
}

void PosProducer::SendCertQueryOnce(const uint256& hash)
{
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (m_lock_queried || m_lock_hash != hash) return;
        m_lock_queried = true;
    }
    if (!m_connman) return;
    LogPrintf("PoS gossip: share-lock at %s — querying peers for a certificate\n", hash.GetHex());
    m_connman->ForEachNode([&](CNode* pnode) {
        m_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetCommonVersion()).Make(NetMsgType::GETPOSCERT, hash));
    });
}

std::optional<CBlockHeader> PosProducer::GetCertificate(const uint256& hash)
{
    std::lock_guard<std::mutex> lock(m_gossip_mutex);
    auto it = m_recent_certs.find(hash);
    if (it == m_recent_certs.end()) return std::nullopt;
    return it->second;
}

bool PosProducer::TryConnectCertified()
{
    // A committee member that share-signed the proposal (or collected it as a
    // round candidate) already validated and holds the FULL block body; the
    // certificate was the only missing piece. Attach it and connect.
    uint256 hash;
    CBlockHeader header;
    std::shared_ptr<const CBlock> body;
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        for (const auto& [h, hdr] : m_certified) {
            for (const auto& [leader, cand] : m_candidates) {
                if (cand.block->GetHash() == h) {
                    hash = h;
                    header = hdr;
                    body = cand.block;
                    break;
                }
            }
            if (body) break;
        }
    }
    if (!body) return false;
    auto full = std::make_shared<CBlock>(*body);
    full->proof.solution = header.proof.solution;
    if (m_chainman.ProcessNewBlock(m_chainparams, full, /*force_processing=*/true, nullptr)) {
        LogPrintf("PoS gossip: adopted certified block %s at height %d from certificate gossip\n",
                  hash.GetHex(), (int)full->block_height);
        return true;
    }
    return false;
}

PosGossipAction PosProducer::OnCertificate(const CBlockHeader& header)
{
    const uint256 hash = header.GetHash();
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        if (!m_seen_certs.insert(hash).second) return PosGossipAction::Ignore;
        if (m_seen_certs.size() > 20000) m_seen_certs.clear();
    }
    // Certificates exist only under the BLS committee (the certificate is the
    // header's proof solution, member-independent block hash).
    if (!g_pos_bls || g_pos_committee_size <= 1) return PosGossipAction::Ignore;
    // Structural: it must be the BLS challenge form.
    std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(header.proof.challenge);
    if (!parts || !parts->is_bls) return PosGossipAction::Invalid;
    // Parent lookup: gives the height and the slot seed / registry context. A
    // certificate on an unknown branch cannot be verified — neither pin nor relay.
    const CBlockIndex* parent;
    bool already_have = false;
    {
        LOCK(cs_main);
        const CBlockIndex* self = m_chainman.m_blockman.LookupBlockIndex(hash);
        already_have = self && (self->nStatus & BLOCK_HAVE_DATA);
        parent = m_chainman.m_blockman.LookupBlockIndex(header.hashPrevBlock);
    }
    if (already_have) return PosGossipAction::Ignore; // nothing new: the block itself already arrived
    if (!parent) return PosGossipAction::Ignore;
    const int height = parent->nHeight + 1;
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    // The leader signature (and structural size) is self-contained and objective,
    // so provable garbage is penalised. Under the bitfield form this is all
    // CheckProof verifies; the aggregate is registry-dependent (below).
    if (!CheckProof(header, m_chainparams.GetConsensus())) return PosGossipAction::Invalid;
    // Only a FULL-quorum certificate pins anyone: a sub-quorum escaping-stall
    // block is deliberately second-class (never immediately final, loses
    // fork-choice to quorum siblings) and must not suppress production.
    int signer_count = 0;
    if (g_pos_public_committee) {
        // Bitfield certificate: aggregate + membership verified against the
        // registry via the SAME helper as ConnectBlock (so gossip-accept and
        // block-validation cannot diverge). Registry-dependent failures are
        // subjective (Ignore, our tip may not be the cert's branch); only a
        // malformed structure is objective (Invalid).
        std::string reason;
        const int signers = PosVerifyBitfieldCertificate(header, parent, reg, reason);
        if (signers < 0) {
            return reason == "bad-posbls-bitfield-malformed" ? PosGossipAction::Invalid : PosGossipAction::Ignore;
        }
        if (signers < PosSlotQuorum(reg)) return PosGossipAction::Ignore;
        signer_count = signers;
    } else {
        // Full-member certificate: members carry their own keys and VRF proofs;
        // membership and quorum mirror ConnectBlock, registry-dependent so
        // failures are ignored (not penalised).
        std::optional<PosBlsCertificate> cert = ParsePosBlsSolution(header.proof.solution);
        if (!cert || cert->members.empty()) return PosGossipAction::Invalid; // an unsigned header is not a certificate
        std::set<CPubKey> named;
        for (const PosBlsMember& m : cert->members) {
            if (!named.insert(m.pubkey).second) return PosGossipAction::Invalid; // duplicate member
        }
        if ((int)named.size() < PosSlotQuorum(reg)) return PosGossipAction::Ignore;
        if ((int)named.size() > PosMaxCommitteeMembers()) return PosGossipAction::Ignore;
        const uint256 seed = PosSeedForChild(parent);
        const uint64_t total = PosTotalWeight(reg);
        for (const PosBlsMember& m : cert->members) {
            if (reg.GetWeight(m.pubkey) == 0) return PosGossipAction::Ignore;
            uint256 beta;
            if (!VrfVerify(m.pubkey, Span<const unsigned char>(seed.begin(), 32), m.proof, beta)) return PosGossipAction::Invalid;
            if (!PosVrfIsCommitteeMember(beta, reg.GetWeight(m.pubkey), total)) return PosGossipAction::Ignore;
        }
        signer_count = (int)named.size();
    }
    // A verified quorum certificate: pin this height. No rival will be
    // proposed, backed or signed here (Step/DriveRound), and the block is
    // completed on the spot if we hold its validated proposal body.
    {
        std::lock_guard<std::mutex> lock(m_gossip_mutex);
        m_certified[hash] = header;
        m_certified_heights[height] = hash;
        m_recent_certs[hash] = header; // answers getposcert share-lock queries
        // Bound the maps (certificates for live heights only; stale entries
        // are pruned as the tip advances in Step).
        if (m_certified.size() > 100) {
            m_certified.erase(m_certified.begin());
        }
        if (m_certified_heights.size() > 100) {
            m_certified_heights.erase(m_certified_heights.begin());
        }
        if (m_recent_certs.size() > 100) {
            m_recent_certs.erase(m_recent_certs.begin());
        }
    }
    LogPrintf("PoS gossip: certificate received for block %s at height %d (%d members)\n",
              hash.GetHex(), height, signer_count);
    TryConnectCertified();
    Wake(); // re-evaluate holds and rounds immediately
    return PosGossipAction::Relay;
}
