// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <supervision.h>

#include <hash.h>
#include <issuance.h>
#include <primitives/transaction.h>
#include <primitives/txwitness.h>
#include <streams.h>
#include <tinyformat.h>
#include <version.h>

#include <string>

int g_supervision_height = 0;

const std::vector<unsigned char> SUPERVISION_MARKER = {'S', 'E', 'Q', 'S', 'U', 'P'};

bool ValidateSupervisionDescriptor(const SupervisionDescriptor& desc, std::string& err)
{
    if (desc.version != SUPERVISION_VERSION) {
        err = strprintf("unknown supervision version %d", (int)desc.version);
        return false;
    }
    // Every defined bit is reserved and unimplemented. Refusing them keeps the
    // meanings free: an asset issued today claiming a capability nothing
    // enforces would be a permanent lie, since the bits are in the asset id.
    if (desc.feature_bits != 0) {
        err = "supervision feature bits are reserved and must be zero";
        return false;
    }
    if (!desc.operational_key.IsFullyValid()) {
        err = "operational key is not a valid x-only public key";
        return false;
    }
    if (!desc.recovery_key.IsFullyValid()) {
        err = "recovery key is not a valid x-only public key";
        return false;
    }
    // Equal keys would let a stolen operational key rotate itself, which is
    // exactly the race that having two keys exists to remove.
    if (desc.operational_key == desc.recovery_key) {
        err = "operational and recovery keys must differ";
        return false;
    }
    return true;
}

uint256 SupervisionDescriptorHash(const SupervisionDescriptor& desc)
{
    return SerializeHash(desc, SER_GETHASH, PROTOCOL_VERSION);
}

CScript BuildSupervisionScript(const CAsset& asset, const SupervisionDescriptor& desc)
{
    std::vector<unsigned char> payload;
    CVectorWriter(SER_NETWORK, PROTOCOL_VERSION, payload, 0, desc);

    CScript s;
    s << SUPERVISION_MARKER << OP_DROP;
    s << std::vector<unsigned char>(asset.begin(), asset.end()) << OP_DROP;
    s << payload << OP_DROP;
    s << OP_RETURN;
    return s;
}

std::optional<SupervisionDeclaration> ParseSupervisionScript(const CScript& script)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    if (!script.GetOp(pc, opcode, data) || data != SUPERVISION_MARKER) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || data.size() != 32) return std::nullopt;
    SupervisionDeclaration decl;
    decl.asset = CAsset(uint256(data));
    if (decl.asset.IsNull()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || data.size() != SUPERVISION_DESCRIPTOR_SIZE) {
        return std::nullopt;
    }
    try {
        SpanReader ss(SER_NETWORK, PROTOCOL_VERSION, data);
        ss >> decl.descriptor;
        // The width check above already forbids trailing bytes; this is the
        // belt to its braces, because the asset id commits to these bytes and a
        // second encoding of one descriptor would let one asset be described
        // two ways.
        if (!ss.empty()) return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return decl;
}

std::optional<SupervisionDeclaration> SupervisionFromTx(const CTransaction& tx, bool& malformed)
{
    malformed = false;
    std::optional<SupervisionDeclaration> found;
    for (const CTxOut& out : tx.vout) {
        auto decl = ParseSupervisionScript(out.scriptPubKey);
        if (!decl) continue;
        if (found) {
            malformed = true;
            return std::nullopt;
        }
        found = decl;
    }
    return found;
}

void GenerateSupervisedAssetEntropy(uint256& entropy, const COutPoint& prevout,
                                    const uint256& contracthash,
                                    const SupervisionDescriptor& desc)
{
    // E = H( H(I) || H(C) || H(D) )
    //
    // Same shape as GenerateAssetEntropy with the descriptor appended, so the
    // asset and its reissuance token both inherit the commitment without
    // touching CalculateAsset or CalculateReissuanceToken.
    std::vector<uint256> leaves;
    leaves.reserve(3);
    leaves.push_back(SerializeHash(prevout, SER_GETHASH, 0));
    leaves.push_back(contracthash);
    leaves.push_back(SupervisionDescriptorHash(desc));
    entropy = ComputeFastMerkleRoot(leaves);
}

bool CheckSupervisedIssuance(const CTransaction& tx, const SupervisionDeclaration& decl,
                             uint256& entropy, std::string& err)
{
    if (!ValidateSupervisionDescriptor(decl.descriptor, err)) {
        return false;
    }

    unsigned int issuances = 0;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const CAssetIssuance& issuance = tx.vin[i].assetIssuance;
        if (issuance.IsNull()) continue;
        // A non-null blinding nonce selects the reissuance branch, whose asset
        // id comes from the quoted entropy rather than from a derivation, so a
        // declaration has nothing to say about it.
        if (!issuance.assetBlindingNonce.IsNull()) continue;

        if (++issuances > 1) {
            err = "supervision declaration covers more than one issuance";
            return false;
        }

        if (!issuance.nAmount.IsExplicit()) {
            err = "supervised issuance amount must be explicit";
            return false;
        }
        if (!issuance.nInflationKeys.IsExplicit()) {
            err = "supervised reissuance token amount must be explicit";
            return false;
        }
        if (issuance.nInflationKeys.GetAmount() <= 0) {
            err = "supervised issuance must create reissuance tokens";
            return false;
        }

        GenerateSupervisedAssetEntropy(entropy, tx.vin[i].prevout, issuance.assetEntropy,
                                       decl.descriptor);
        CAsset derived;
        CalculateAsset(derived, entropy);
        if (derived != decl.asset) {
            err = "supervision declaration names an asset the issuance does not create";
            return false;
        }
    }

    if (issuances == 0) {
        err = "supervision declaration without an issuance";
        return false;
    }

    for (const CTxOut& out : tx.vout) {
        if (!out.nAsset.IsExplicit()) {
            err = "a supervised issuance may not create blinded-asset outputs";
            return false;
        }
        if (!out.nValue.IsExplicit()) {
            err = "a supervised issuance may not create blinded-value outputs";
            return false;
        }
        if (!ParseSupervisionScript(out.scriptPubKey)) continue;
        if (out.nAsset.GetAsset() != decl.asset || out.nValue.GetAmount() != 0) {
            err = "declaration output must carry zero of the declared asset";
            return false;
        }
    }

    return true;
}
