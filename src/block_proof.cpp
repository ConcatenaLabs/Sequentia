// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <chain.h>
#include <pos.h>
#include <primitives/block.h>
#include <script/interpreter.h>
#include <script/generic.hpp>

bool CheckChallenge(const CBlockHeader& block, const CBlockIndex& indexLast, const Consensus::Params& params)
{
    if (g_con_pos) {
        // SEQUENTIA PoS: the block's challenge must require the stake-weighted
        // leader elected for this slot and, when committee certification is
        // enabled (g_pos_committee_size > 1), a majority of the slot's
        // committee as countersigners (the paper's principle 6). The
        // signatures that satisfy the challenge are checked separately in
        // CheckProof. See doc/sequentia/06-proof-of-stake.md.
        const CScript& challenge = block.proof.challenge;
        std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(challenge);
        if (!parts) {
            return false; // challenge is not a recognized PoS challenge form
        }
        const StakeRegistry& registry = StakeRegistry::GetInstance();
        uint256 seed = PosSeedForChild(&indexLast);
        std::optional<size_t> rank = PosRank(registry, seed, parts->leader);
        if (!rank) {
            return false; // proposer is not a registered staker for this slot
        }
        // Liveness gating: the rank-r leader may only produce once r slot
        // intervals have elapsed since the parent block, so a higher-ranked
        // (worse) leader cannot pre-empt a lower-ranked (better) one.
        if ((int64_t)block.nTime < (int64_t)indexLast.nTime + (int64_t)(*rank) * g_pos_slot_interval) {
            return false;
        }
        // Committee certification: the challenge's committee and quorum must
        // be exactly the elected committee for this slot and its majority.
        std::vector<CPubKey> expected_committee = PosCommittee(registry, seed);
        if (expected_committee.size() <= 1) {
            // Committee certification disabled: only the leader-only form is valid.
            if (!parts->committee.empty()) return false;
        } else {
            if (parts->committee != expected_committee) return false;
            if (parts->quorum != PosQuorum(expected_committee.size())) return false;
        }
        return true;
    } else if (g_signed_blocks) {
        return block.proof.challenge == indexLast.get_proof().challenge;
    } else {
        return block.nBits == GetNextWorkRequired(&indexLast, &block, params);
    }
}

static bool CheckProofGeneric(const CBlockHeader& block, const uint32_t max_block_signature_size, const CScript& challenge, const CScript& scriptSig, const CScriptWitness& witness)
{
    // Legacy blocks have empty witness, dynafed blocks have empty scriptSig
    bool is_dyna = !witness.stack.empty();

    // Check signature limits for blocks
    if (scriptSig.size() > max_block_signature_size) {
        assert(!is_dyna);
        return false;
    } else if (witness.GetSerializedSize() > max_block_signature_size) {
        assert(is_dyna);
        return false;
    }

    // Some anti-DoS flags, though max_block_signature_size caps the possible
    // danger in malleation of the block witness data.
    unsigned int proof_flags = SCRIPT_VERIFY_P2SH // For cleanstack evaluation under segwit flag
        | SCRIPT_VERIFY_STRICTENC // Minimally-sized DER sigs
        | SCRIPT_VERIFY_NULLDUMMY // No extra data stuffed into OP_CMS witness
        | SCRIPT_VERIFY_CLEANSTACK // No extra pushes leftover in witness
        | SCRIPT_VERIFY_MINIMALDATA // Pushes are minimally-sized
        | SCRIPT_VERIFY_SIGPUSHONLY // Witness is push-only
        | SCRIPT_VERIFY_LOW_S // Stop easiest signature fiddling
        | SCRIPT_VERIFY_WITNESS // Witness and to enforce cleanstack
        | (is_dyna ? SCRIPT_VERIFY_NONE : SCRIPT_NO_SIGHASH_BYTE); // Non-dynafed blocks do not have sighash byte
    return GenericVerifyScript(scriptSig, witness, challenge, proof_flags, block);
}

bool CheckProof(const CBlockHeader& block, const Consensus::Params& params)
{
    if (g_signed_blocks) {
        const DynaFedParams& dynafed_params = block.m_dynafed_params;
        if (g_con_pos && dynafed_params.IsNull()) {
            // SEQUENTIA PoS: verify the block signature against the per-block
            // leader challenge carried in the header (validated as the correct
            // elected leader by CheckChallenge), not the chain's fixed script.
            return CheckProofGeneric(block, params.max_block_signature_size, block.proof.challenge, block.proof.solution, CScriptWitness());
        } else if (dynafed_params.IsNull()) {
            return CheckProofGeneric(block, params.max_block_signature_size, params.signblockscript, block.proof.solution, CScriptWitness());
        } else {
            return CheckProofGeneric(block, dynafed_params.m_current.m_signblock_witness_limit, dynafed_params.m_current.m_signblockscript, CScript(), block.m_signblock_witness);
        }
    } else {
        return CheckProofOfWork(block.GetHash(), block.nBits, params);
    }
}

bool CheckProofSignedParent(const CBlockHeader& block, const Consensus::Params& params)
{
    const DynaFedParams& dynafed_params = block.m_dynafed_params;
    if (dynafed_params.IsNull()) {
        return CheckProofGeneric(block, params.max_block_signature_size, params.parent_chain_signblockscript, block.proof.solution, CScriptWitness());
    } else {
        // Dynamic federations means we cannot validate the signer set
        // at least without tracking the parent chain more directly.
        // Note that we do not even serialize dynamic federation block witness data
        // currently for merkle proofs which is the only context in which
        // this function is currently used.
        return true;
    }
}
