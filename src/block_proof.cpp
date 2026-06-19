// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <bls.h>
#include <chain.h>
#include <pos.h>
#include <primitives/block.h>
#include <script/interpreter.h>
#include <script/generic.hpp>

#include <set>

bool CheckChallenge(const CBlockHeader& block, const CBlockIndex& indexLast, const Consensus::Params& params)
{
    if (g_con_pos) {
        // SEQUENTIA PoS: the block's challenge must require the stake-weighted
        // leader elected for this slot and, when committee certification is
        // enabled (g_pos_committee_size > 1), a majority of the slot's
        // committee as countersigners (the paper's principle 6). The
        // signatures that satisfy the challenge are checked separately in
        // CheckProof. See doc/sequentia/04-proof-of-stake.md.
        const CScript& challenge = block.proof.challenge;
        std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(challenge);
        if (!parts) {
            return false; // challenge is not a recognized PoS challenge form
        }
        // The header check is STRUCTURAL ONLY, in every PoS mode. Anything
        // registry-dependent — leader registration/rank, slot time-gating,
        // sortition eligibility, exact committee membership — is validated in
        // ConnectBlock (CheckPosStakeRules): the stake registry mirrors the
        // *active tip's* UTXO set, and headers arrive far ahead of (or on a
        // different branch than) the active chain, where the registry would
        // be the wrong stake state.
        if (g_pos_vrf) {
            // BLS aggregate-committee certification (-posbls) takes precedence
            // over MuSig2 when both are enabled: the challenge must be the OP_2
            // <leader> form. The committee certificate is in the proof solution;
            // its members' sortition eligibility and quorum are validated at
            // connect time, the signatures (leader + BLS aggregate) in CheckProof.
            // The BLS form is invalid in any other mode.
            if (g_pos_bls && g_pos_committee_size > 1) {
                return parts->is_bls;
            }
            if (parts->is_bls) return false; // BLS form only valid under -posbls
            if (g_pos_agg_committee && g_pos_committee_size > 1) {
                // Aggregate-committee certification (doc 07 §6): the challenge
                // must be the OP_1 <leader> <agg_key> form. The member set it
                // aggregates is named by the coinbase SEQCMT commitments and
                // validated (eligibility, quorum, aggregate match) at connect
                // time; the signatures in CheckProof.
                return !parts->agg_key.empty();
            }
            if (!parts->agg_key.empty()) return false; // agg form only valid under -posaggcommittee
            if (g_pos_committee_size > 1) {
                // Committee certification with private sortition: the
                // challenge lists the *claimed* members (proven eligible at
                // connect time). Structurally: a fixed quorum (a strict
                // majority of the expected committee size), at least
                // quorum-many distinct members.
                if (parts->committee.empty()) return false;
                if (parts->quorum != PosQuorum((size_t)g_pos_committee_size)) return false;
                if ((int)parts->committee.size() < parts->quorum) return false;
                std::set<CPubKey> seen;
                for (const CPubKey& member : parts->committee) {
                    if (!seen.insert(member).second) return false; // duplicate
                }
            } else {
                if (!parts->committee.empty()) return false;
            }
            return true;
        }
        // Public-schedule mode: structurally just "neither aggregate form";
        // the elected leader/committee comparison happens at connect time.
        return parts->agg_key.empty() && !parts->is_bls;
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
            if (g_pos_agg_committee) {
                std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(block.proof.challenge);
                if (parts && !parts->agg_key.empty()) {
                    // Aggregate-committee form (doc 07 §6): the solution is two
                    // pushes — the leader's DER signature and one 64-byte
                    // BIP340 signature under the committee aggregate key —
                    // verified directly rather than through the script
                    // interpreter (OP_CHECKMULTISIG cannot express a single
                    // signature over an aggregate of up to 100 keys). That the
                    // aggregate key covers exactly the sortition-eligible
                    // members named in the coinbase is enforced in
                    // ContextualCheckBlock.
                    if (block.proof.solution.size() > params.max_block_signature_size) return false;
                    CScript::const_iterator pc = block.proof.solution.begin();
                    opcodetype opcode;
                    std::vector<unsigned char> leader_sig, agg_sig;
                    if (!block.proof.solution.GetOp(pc, opcode, leader_sig) || leader_sig.empty()) return false;
                    if (!block.proof.solution.GetOp(pc, opcode, agg_sig) || agg_sig.size() != 64) return false;
                    if (pc != block.proof.solution.end()) return false;
                    const uint256 hash = block.GetHash();
                    if (!parts->leader.Verify(hash, leader_sig)) return false;
                    const XOnlyPubKey agg_pubkey{Span<const unsigned char>(parts->agg_key)};
                    return agg_pubkey.VerifySchnorr(Span<const unsigned char>(hash.begin(), 32), agg_sig);
                }
            }
            if (g_pos_bls) {
                std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(block.proof.challenge);
                if (parts && parts->is_bls) {
                    // BLS aggregate-committee form (doc proposals/autonomous-committee
                    // §7): the whole certificate is in the proof solution — the
                    // leader's DER signature, one 96-byte BLS aggregate, and each
                    // member's (key, VRF proof, BLS key, proof-of-possession).
                    // Because the solution is excluded from the block hash, that
                    // hash is independent of which members signed, so the members
                    // could sign it non-interactively. Verify the leader ECDSA,
                    // every member's BLS PoP, and the aggregate against the member
                    // keys; sortition eligibility and quorum are enforced in
                    // ContextualCheckBlock.
                    if (block.proof.solution.size() > params.max_block_signature_size) return false;
                    std::optional<PosBlsCertificate> cert = ParsePosBlsSolution(block.proof.solution);
                    if (!cert || cert->members.empty()) return false;
                    const uint256 hash = block.GetHash();
                    if (!parts->leader.Verify(hash, cert->leader_sig)) return false;
                    std::vector<std::vector<unsigned char>> bls_pubkeys;
                    bls_pubkeys.reserve(cert->members.size());
                    for (const PosBlsMember& m : cert->members) {
                        if (!BlsVerifyPossession(m.bls_pubkey, m.bls_pop)) return false;
                        bls_pubkeys.push_back(m.bls_pubkey);
                    }
                    return BlsFastAggregateVerify(bls_pubkeys, Span<const unsigned char>(hash.begin(), 32), cert->agg_sig);
                }
            }
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
