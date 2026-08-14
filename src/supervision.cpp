// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <supervision.h>

#include <coins.h>
#include <hash.h>
#include <issuance.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <primitives/txwitness.h>
#include <script/standard.h>
#include <streams.h>
#include <tinyformat.h>
#include <undo.h>
#include <util/strencodings.h>
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

const std::vector<unsigned char> SUPERVISION_RECORD_MARKER = {'S', 'E', 'Q', 'F', 'R', 'Z'};

//! The signature's fixed width. BIP340 signatures are always 64 bytes; there is
//! no sighash byte, because a record signs a message this file defines rather
//! than a transaction.
static const size_t SUPERVISION_SIG_SIZE = 64;

uint256 SupervisionTargetHash(const CScript& script)
{
    return Hash(script);
}

uint256 SupervisionRecordSigHash(const SupervisionRecord& record, const COutPoint& first_input)
{
    CHashWriter ss = TaggedHash("Sequentia/SupervisionRecord");
    ss << (uint8_t)record.kind;
    ss << record.asset;
    ss << record.target;
    if (record.IsRotation()) {
        ss.write(MakeByteSpan(record.old_key));
    }
    ss << first_input;
    return ss.GetSHA256();
}

uint256 SupervisionUnfreezeSigHash(const COutPoint& record_outpoint, const CAsset& asset,
                                   const uint256& target)
{
    CHashWriter ss = TaggedHash("Sequentia/SupervisionUnfreeze");
    ss << record_outpoint;
    ss << asset;
    ss << target;
    return ss.GetSHA256();
}

CScript BuildSupervisionRecordScript(const SupervisionRecord& record)
{
    CScript s;
    s << SUPERVISION_RECORD_MARKER << OP_DROP;
    // A small-integer opcode, not a one-byte push: SCRIPT_VERIFY_MINIMALDATA
    // rejects a push of a single byte in 1..16, so the obvious encoding would
    // make every record non-standard and unrelayable.
    s << (int64_t)record.kind << OP_DROP;
    s << std::vector<unsigned char>(record.asset.begin(), record.asset.end()) << OP_DROP;
    s << std::vector<unsigned char>(record.target.begin(), record.target.end()) << OP_DROP;
    if (record.IsRotation()) {
        s << std::vector<unsigned char>(record.old_key.begin(), record.old_key.end()) << OP_DROP;
    }
    // OP_2DROP, not OP_DROP: it removes the admission signature AND the single
    // item the spender pushes, leaving exactly OP_TRUE on the stack. Anything
    // else fails SCRIPT_VERIFY_CLEANSTACK, so an unfreeze would be valid by
    // consensus and never relay, which is the same as not working.
    s << record.signature << OP_2DROP;
    s << OP_TRUE;
    return s;
}

std::optional<SupervisionRecord> ParseSupervisionRecordScript(const CScript& script)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    if (!script.GetOp(pc, opcode, data) || data != SUPERVISION_RECORD_MARKER) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    SupervisionRecord record;
    if (!script.GetOp(pc, opcode, data) || opcode < OP_1 || opcode > OP_16) return std::nullopt;
    switch ((unsigned char)(opcode - (OP_1 - 1))) {
    case (unsigned char)SupervisionRecordKind::FREEZE:
    case (unsigned char)SupervisionRecordKind::ROTATE_OPERATIONAL:
    case (unsigned char)SupervisionRecordKind::ROTATE_RECOVERY:
        record.kind = (SupervisionRecordKind)(opcode - (OP_1 - 1));
        break;
    default:
        // An unknown kind is not forward compatibility, it is a record whose
        // meaning this node cannot know. Refusing to parse it means the
        // registry never acts on it, and consensus refuses to admit it.
        return std::nullopt;
    }
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || data.size() != 32) return std::nullopt;
    record.asset = CAsset(uint256(data));
    if (record.asset.IsNull()) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || data.size() != 32) return std::nullopt;
    record.target = uint256(data);
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;

    if (record.IsRotation()) {
        if (!script.GetOp(pc, opcode, data) || data.size() != 32) return std::nullopt;
        record.old_key = XOnlyPubKey(data);
        if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return std::nullopt;
    }

    if (!script.GetOp(pc, opcode, data) || data.size() != SUPERVISION_SIG_SIZE) return std::nullopt;
    record.signature = data;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_2DROP) return std::nullopt;

    if (!script.GetOp(pc, opcode, data) || opcode != OP_TRUE) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return record;
}

bool CheckSupervisionRecords(const CTransaction& tx, const SupervisionRegistry& registry,
                             std::string& err)
{
    if (tx.vin.empty()) {
        err = "supervision record in a transaction with no inputs";
        return false;
    }

    for (const CTxOut& out : tx.vout) {
        const auto record = ParseSupervisionRecordScript(out.scriptPubKey);
        if (!record) continue;

        if (!registry.IsSupervised(record->asset)) {
            err = strprintf("supervision record names unsupervised asset %s",
                            record->asset.GetHex());
            return false;
        }

        // Which key may sign is the whole of the two-key design: freezes are
        // day-to-day and signed by the operational key, rotations are the
        // incident response and signed by the recovery key. The operational key
        // cannot rotate anything, not even itself, so a thief who has it can
        // grief but can never take the authority away from its owner.
        const auto signer = record->IsRotation() ? registry.RecoveryKey(record->asset)
                                                 : registry.OperationalKey(record->asset);
        if (!signer) {
            err = "supervision record names an asset with no current key";
            return false;
        }

        if (record->IsRotation()) {
            const bool operational_role =
                record->kind == SupervisionRecordKind::ROTATE_OPERATIONAL;
            const auto current = operational_role ? registry.OperationalKey(record->asset)
                                                  : registry.RecoveryKey(record->asset);
            // Naming the key it replaces is what lets the registry replay
            // rotations out of the UTXO set in any order, and it also rejects a
            // stale rotation signed against a key some earlier rotation has
            // already retired.
            if (!current || !(*current == record->old_key)) {
                err = "rotation record does not name the key it replaces";
                return false;
            }
            const XOnlyPubKey new_key = record->NewKey();
            if (!new_key.IsFullyValid()) {
                err = "rotation record installs an invalid key";
                return false;
            }
            if (new_key == record->old_key) {
                err = "rotation record installs the key it replaces";
                return false;
            }
            // The two roles must stay distinct for the same reason the
            // descriptor refuses equal keys at issuance: one key that both uses
            // and rotates is the self-signed-rotation race.
            const auto other = operational_role ? registry.RecoveryKey(record->asset)
                                                : registry.OperationalKey(record->asset);
            if (other && new_key == *other) {
                err = "rotation would make the operational and recovery keys equal";
                return false;
            }
        }

        const uint256 sighash = SupervisionRecordSigHash(*record, tx.vin[0].prevout);
        if (!signer->VerifySchnorr(sighash, record->signature)) {
            err = "supervision record is not signed by the asset's current key";
            return false;
        }

        // Zero of the asset it governs, like a declaration. Two reasons, and
        // the second is the one that bites: an issuer cannot burn value into an
        // output only consensus can release, and the output escapes the dust
        // rule, which applies only to the fee asset -- a record denominated in
        // the policy asset would be dust and would never relay.
        if (!out.nAsset.IsExplicit() || !out.nValue.IsExplicit() ||
            out.nAsset.GetAsset() != record->asset || out.nValue.GetAmount() != 0) {
            err = "supervision record must carry zero of the asset it governs";
            return false;
        }
    }

    return true;
}

bool CheckSupervisionRecordSpend(const CTxIn& input, const CTxOut& record_out,
                                 const SupervisionRegistry& registry, std::string& err)
{
    const auto record = ParseSupervisionRecordScript(record_out.scriptPubKey);
    if (!record) return true; // not a record; nothing to authorise

    if (record->IsRotation()) {
        // A rotation is a statement about the past, not a switch to be flipped
        // back. Spending one would erase it from the UTXO set and take the key
        // change with it, so the registry could no longer be rebuilt.
        err = "a rotation record cannot be spent";
        return false;
    }

    const auto operational = registry.OperationalKey(record->asset);
    if (!operational) {
        err = "freeze record names an asset that is no longer supervised";
        return false;
    }

    // The signature rides in the scriptSig, which the record's own script
    // ignores. Exactly one push of exactly a BIP340 signature: anything else is
    // refused rather than searched, so there is one encoding of an unfreeze.
    CScript::const_iterator pc = input.scriptSig.begin();
    opcodetype opcode;
    std::vector<unsigned char> sig;
    if (!input.scriptSig.GetOp(pc, opcode, sig) || sig.size() != SUPERVISION_SIG_SIZE ||
        pc != input.scriptSig.end()) {
        err = "unfreeze must carry exactly one signature";
        return false;
    }

    const uint256 sighash =
        SupervisionUnfreezeSigHash(input.prevout, record->asset, record->target);
    if (!operational->VerifySchnorr(sighash, sig)) {
        err = "unfreeze is not signed by the asset's current operational key";
        return false;
    }
    return true;
}

bool IsSingleOwnerSpend(const CTransaction& tx, unsigned int n, const CScript& script_pubkey)
{
    if (n >= tx.vin.size()) return false;

    std::vector<std::vector<unsigned char>> solutions;
    const TxoutType type = Solver(script_pubkey, solutions);

    const std::vector<std::vector<unsigned char>>* witness = nullptr;
    if (tx.witness.vtxinwit.size() > n) {
        witness = &tx.witness.vtxinwit[n].scriptWitness.stack;
    }

    switch (type) {
    case TxoutType::PUBKEY:
    case TxoutType::PUBKEYHASH:
        // One key named in the output itself. Legacy forms, rare on a chain
        // whose default address is bech32, but they are unambiguous.
        return true;

    case TxoutType::WITNESS_V0_KEYHASH:
        // P2WPKH: <sig> <pubkey>.
        return witness && witness->size() == 2;

    case TxoutType::WITNESS_V1_TAPROOT: {
        // A taproot KEY-path spend is one witness element, plus an optional
        // annex. A script-path spend carries a control block, so it is longer,
        // and is not single-owner: the script could be anything, including a
        // channel or a covenant.
        if (!witness) return false;
        size_t size = witness->size();
        if (size >= 2 && !witness->back().empty() && witness->back()[0] == ANNEX_TAG) size--;
        return size == 1;
    }

    case TxoutType::SCRIPTHASH: {
        // P2SH is only single-owner when it wraps P2WPKH, which the spend
        // reveals: the scriptSig is exactly a push of the 22-byte v0 program
        // and the witness is the P2WPKH pair. Covered rather than skipped
        // because leaving it out would make a legacy-wrapped address an
        // evasion route, and because a wrapped P2WPKH cannot be shared with
        // anyone.
        if (!witness || witness->size() != 2) return false;
        CScript::const_iterator pc = tx.vin[n].scriptSig.begin();
        opcodetype opcode;
        std::vector<unsigned char> redeem;
        if (!tx.vin[n].scriptSig.GetOp(pc, opcode, redeem)) return false;
        if (pc != tx.vin[n].scriptSig.end()) return false;
        const CScript redeem_script(redeem.begin(), redeem.end());
        int version;
        std::vector<unsigned char> program;
        return redeem_script.IsWitnessProgram(version, program) && version == 0 &&
               program.size() == 20;
    }

    default:
        // Bare multisig, P2WSH, unknown witness versions, non-standard scripts:
        // any of them may be shared, so none of them is freezable.
        return false;
    }
}

bool IsSupervisionOutput(const CTxOut& out)
{
    if (!out.nAsset.IsExplicit()) return false;
    if (const auto decl = ParseSupervisionScript(out.scriptPubKey)) {
        return out.nAsset.GetAsset() == decl->asset;
    }
    if (const auto record = ParseSupervisionRecordScript(out.scriptPubKey)) {
        return out.nAsset.GetAsset() == record->asset;
    }
    return false;
}

bool SupervisionInvalidates(const CTransaction& tx, CCoinsView& view,
                            const SupervisionRegistry& registry)
{
    if (registry.Empty()) return false;

    // A record this transaction creates, whose admission signature was written
    // against a key that has since been rotated away.
    std::string err;
    if (!CheckSupervisionRecords(tx, registry, err)) return true;

    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        if (tx.vin[i].m_is_pegin) continue;
        Coin coin;
        if (!view.GetCoin(tx.vin[i].prevout, coin) || coin.IsSpent()) continue;

        // An unfreeze signed by a key a rotation has since retired.
        if (!CheckSupervisionRecordSpend(tx.vin[i], coin.out, registry, err)) return true;

        if (!coin.out.nAsset.IsExplicit()) continue;
        if (!registry.IsFrozen(coin.out.nAsset.GetAsset(),
                               SupervisionTargetHash(coin.out.scriptPubKey))) {
            continue;
        }
        if (IsSingleOwnerSpend(tx, i, coin.out.scriptPubKey)) return true;
    }
    return false;
}

bool IsSupervisionRejection(const std::string& reject_reason)
{
    return reject_reason == "bad-txns-asset-frozen" ||
           reject_reason == "bad-txns-supervision-unfreeze" ||
           reject_reason == "bad-txns-supervision-record" ||
           reject_reason == "bad-txns-supervised-blinded";
}

void SupervisionApplyBlock(const CBlock& block, const CBlockUndo& undo)
{
    SupervisionRegistry& registry = SupervisionRegistry::GetInstance();

    // Created outputs first, spent ones after, matching PosApplyBlockStake. The
    // order matters for a transaction that both creates and spends records.
    for (const CTransactionRef& tx : block.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto decl = ParseSupervisionScript(out.scriptPubKey)) {
                registry.AddAsset(*decl);
                LogPrintf("Supervision: asset %s is supervised\n", decl->asset.GetHex());
                continue;
            }
            if (auto record = ParseSupervisionRecordScript(out.scriptPubKey)) {
                switch (record->kind) {
                case SupervisionRecordKind::FREEZE:
                    registry.AddFreeze(record->asset, record->target);
                    LogPrintf("Supervision: %s freezes script %s\n", record->asset.GetHex(),
                              record->target.GetHex());
                    break;
                case SupervisionRecordKind::ROTATE_OPERATIONAL:
                case SupervisionRecordKind::ROTATE_RECOVERY:
                    registry.Rotate(record->asset,
                                    record->kind == SupervisionRecordKind::ROTATE_OPERATIONAL,
                                    record->old_key, record->NewKey());
                    LogPrintf("Supervision: %s rotates a key\n", record->asset.GetHex());
                    break;
                }
            }
        }
    }

    for (const CTxUndo& txundo : undo.vtxundo) {
        for (const Coin& coin : txundo.vprevout) {
            if (auto record = ParseSupervisionRecordScript(coin.out.scriptPubKey)) {
                if (record->kind == SupervisionRecordKind::FREEZE) {
                    registry.SubFreeze(record->asset, record->target);
                    LogPrintf("Supervision: %s unfreezes script %s\n", record->asset.GetHex(),
                              record->target.GetHex());
                }
            }
        }
    }
}

void SupervisionRevertBlock(const CBlock& block, const CBlockUndo& undo)
{
    SupervisionRegistry& registry = SupervisionRegistry::GetInstance();

    // Exact inverse, undone in reverse order: re-add the freezes this block
    // lifted before removing the ones it created, so no count passes through a
    // state the forward pass never produced.
    for (const CTxUndo& txundo : undo.vtxundo) {
        for (const Coin& coin : txundo.vprevout) {
            if (auto record = ParseSupervisionRecordScript(coin.out.scriptPubKey)) {
                if (record->kind == SupervisionRecordKind::FREEZE) {
                    registry.AddFreeze(record->asset, record->target);
                }
            }
        }
    }

    for (const CTransactionRef& tx : block.vtx) {
        for (const CTxOut& out : tx->vout) {
            if (auto decl = ParseSupervisionScript(out.scriptPubKey)) {
                registry.RemoveAsset(decl->asset);
                continue;
            }
            if (auto record = ParseSupervisionRecordScript(out.scriptPubKey)) {
                switch (record->kind) {
                case SupervisionRecordKind::FREEZE:
                    registry.SubFreeze(record->asset, record->target);
                    break;
                case SupervisionRecordKind::ROTATE_OPERATIONAL:
                case SupervisionRecordKind::ROTATE_RECOVERY:
                    // Backwards: put back the key this rotation replaced.
                    registry.Rotate(record->asset,
                                    record->kind == SupervisionRecordKind::ROTATE_OPERATIONAL,
                                    record->NewKey(), record->old_key);
                    break;
                }
            }
        }
    }
}

bool RebuildSupervisionRegistry(CCoinsView& view)
{
    SupervisionRegistry& registry = SupervisionRegistry::GetInstance();
    registry.Clear();

    // Two passes, because a rotation is meaningless until its asset is known,
    // and the UTXO set has no order. Declarations first, then the records.
    std::vector<SupervisionRecord> records;
    {
        std::unique_ptr<CCoinsViewCursor> cursor(view.Cursor());
        if (!cursor) return false;
        for (; cursor->Valid(); cursor->Next()) {
            COutPoint key;
            Coin coin;
            if (!cursor->GetKey(key) || !cursor->GetValue(coin)) return false;
            if (coin.IsSpent()) continue;
            // Below the activation height a declaration is inert data: the
            // asset it names was never derived over it, so admitting it here
            // would supervise an asset the chain does not think is supervised.
            // The apply path skips those blocks for the same reason, and this
            // is how a rebuild reproduces the same answer.
            if (!SupervisionActive((int)coin.nHeight)) continue;
            if (auto decl = ParseSupervisionScript(coin.out.scriptPubKey)) {
                registry.AddAsset(*decl);
                continue;
            }
            if (auto record = ParseSupervisionRecordScript(coin.out.scriptPubKey)) {
                records.push_back(*record);
            }
        }
    }

    // Freezes are counted, so their order does not matter. Rotations do have an
    // order, and the UTXO set does not record it, which is why each rotation
    // names the key it replaces: they form a chain from the descriptor, and
    // following it recovers the sequence without a timestamp.
    std::vector<SupervisionRecord> rotations;
    for (const SupervisionRecord& record : records) {
        if (record.kind == SupervisionRecordKind::FREEZE) {
            registry.AddFreeze(record.asset, record.target);
        } else {
            rotations.push_back(record);
        }
    }

    bool progress = true;
    while (progress && !rotations.empty()) {
        progress = false;
        for (auto it = rotations.begin(); it != rotations.end();) {
            const bool operational_role = it->kind == SupervisionRecordKind::ROTATE_OPERATIONAL;
            if (registry.Rotate(it->asset, operational_role, it->old_key, it->NewKey())) {
                it = rotations.erase(it);
                progress = true;
            } else {
                ++it;
            }
        }
    }
    if (!rotations.empty()) {
        // Every rotation in the UTXO set was admitted by consensus against the
        // then-current key, so the chain must close. A leftover means the UTXO
        // set and the rules that produced it disagree, and continuing would run
        // a node whose freeze authority is not the chain's.
        LogPrintf("Supervision: %u rotation records do not chain; registry cannot be rebuilt\n",
                  (unsigned)rotations.size());
        return false;
    }
    return true;
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
