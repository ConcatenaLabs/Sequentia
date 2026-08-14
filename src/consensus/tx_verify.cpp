// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_verify.h>

#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <issuance.h>
#include <pegins.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/pegins.h>
#include <supervision.h>
#include <util/moneystr.h>

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;

    // Even if tx.nLockTime isn't satisfied by nBlockHeight/nBlockTime, a
    // transaction is still considered final if all inputs' nSequence ==
    // SEQUENCE_FINAL (0xffffffff), in which case nLockTime is ignored.
    //
    // Because of this behavior OP_CHECKLOCKTIMEVERIFY/CheckLockTime() will
    // also check that the spending input's nSequence != SEQUENCE_FINAL,
    // ensuring that an unsatisfied nLockTime value will actually cause
    // IsFinalTx() to return false here:
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    assert(prevHeights.size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Peg-ins have no output height
        if (txin.m_is_pegin) {
            continue;
        }

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            prevHeights[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = prevHeights[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight-1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        // Peg-in inputs are segwit-only
        if (tx.vin[i].m_is_pegin) {
            continue;
        }

        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, uint32_t flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    // Note that we only count segwit sigops for peg-in inputs
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        CScript scriptPubKey;
        if (tx.vin[i].m_is_pegin) {
            std::string err;
            // Make sure witness exists and has enough peg-in witness fields for
            // the claim_script
            if (tx.witness.vtxinwit.size() != tx.vin.size() ||
                    tx.witness.vtxinwit[i].m_pegin_witness.stack.size() < 4) {
                continue;
            }
            const auto pegin_witness = tx.witness.vtxinwit[i].m_pegin_witness;
            scriptPubKey = CScript(pegin_witness.stack[3].begin(), pegin_witness.stack[3].end());
        } else {
            const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
            assert(!coin.IsSpent());
            scriptPubKey = coin.out.scriptPubKey;
        }

        const CScriptWitness* pScriptWitness = tx.witness.vtxinwit.size() > i ? &tx.witness.vtxinwit[i].scriptWitness : NULL;
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, scriptPubKey, pScriptWitness, flags);
    }
    return nSigOps;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight, CAmountMap& fee_map, std::set<std::pair<uint256, COutPoint>>& setPeginsSpent, std::vector<CCheck*> *pvChecks, const bool cacheStore, bool fScriptChecks, const std::vector<std::pair<CScript, CScript>>& fedpegscripts)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent",
                         strprintf("%s: inputs missing/spent", __func__));
    }

    std::vector<CTxOut> spent_inputs;
    CAmount nValueIn = 0;
    // SEQUENTIA: set when any input, or any reissuance, carries a supervised
    // asset. Such a transaction must keep every output explicit, which is what
    // makes the explicit-only property inductive from issuance.
    bool touches_supervised = false;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        if (tx.vin[i].m_is_pegin) {
            // Check existence and validity of pegin witness
            std::string err;
            if (tx.witness.vtxinwit.size() <= i || !IsValidPeginWitness(tx.witness.vtxinwit[i].m_pegin_witness, fedpegscripts, prevout, err, true)) {
                return state.Invalid(TxValidationResult::TX_WITNESS_MUTATED, "bad-pegin-witness", err);
            }
            std::pair<uint256, COutPoint> pegin = std::make_pair(uint256(tx.witness.vtxinwit[i].m_pegin_witness.stack[2]), prevout);
            if (inputs.IsPeginSpent(pegin)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-double-pegin", strprintf("Double-pegin of %s:%d", prevout.hash.ToString(), prevout.n));
            }
            if (setPeginsSpent.count(pegin)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-double-pegin-in-obj",
                    strprintf("Double-pegin of %s:%d in single tx/block", prevout.hash.ToString(), prevout.n));
            }
            setPeginsSpent.insert(pegin);

            // Tally the input amount.
            spent_inputs.push_back(GetPeginOutputFromWitness(tx.witness.vtxinwit[i].m_pegin_witness));
            const CTxOut& out = spent_inputs.back();
            nValueIn += out.nValue.GetAmount(); // Non-explicit already filtered by IsValidPeginWitness
            if (!MoneyRange(out.nValue.GetAmount())) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputvalues-outofrange");
            }
        } else {
            const Coin& coin = inputs.AccessCoin(prevout);
            assert(!coin.IsSpent());

            // If prev is coinbase, check that it's matured. SEQUENTIA: the
            // required depth is chain- and height-dependent, because a maturity
            // counted in blocks protects for less and less wall-clock time as a
            // chain's cadence shortens (see CoinbaseMaturityAt, consensus.h).
            if (coin.IsCoinBase() && nSpendHeight - coin.nHeight < CoinbaseMaturityAt(nSpendHeight)) {
                return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-premature-spend-of-coinbase",
                    strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
            }
            spent_inputs.push_back(coin.out);
            if (coin.out.nValue.IsExplicit()) {
                nValueIn += coin.out.nValue.GetAmount();
            }

            // SEQUENTIA: supervised assets, spend side (src/supervision.h).
            if (SupervisionActive(nSpendHeight)) {
                const SupervisionRegistry& supervision = SupervisionRegistry::GetInstance();
                if (!supervision.Empty()) {
                    // Spending a freeze record is the unfreeze, and the record's
                    // own script deliberately does not authorise it -- see
                    // BuildSupervisionRecordScript. Consensus does, against the
                    // CURRENT operational key, which is the unfreeze trap: after
                    // a rotation the old key lifts nothing.
                    std::string err;
                    if (!CheckSupervisionRecordSpend(tx.vin[i], coin.out, supervision, err)) {
                        return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                             "bad-txns-supervision-unfreeze", err);
                    }

                    // IsExplicit() before GetAsset(): the latter asserts, so a
                    // blinded input would abort the node rather than be skipped.
                    // A supervised asset can never be in a blinded output --
                    // that is enforced from issuance onwards, below and in
                    // CheckSupervisedIssuance -- so a blinded input is simply
                    // some other asset.
                    if (coin.out.nAsset.IsExplicit()) {
                        const CAsset& asset = coin.out.nAsset.GetAsset();
                        if (supervision.IsSupervised(asset)) {
                            touches_supervised = true;
                        }
                        // Cheap test first: almost every supervised asset has no
                        // freezes at all, and almost every spend of one is not of
                        // a frozen script.
                        if (supervision.AnyFrozen() &&
                            supervision.IsFrozen(asset,
                                                 SupervisionTargetHash(coin.out.scriptPubKey)) &&
                            IsSingleOwnerSpend(tx, i, coin.out.scriptPubKey)) {
                            return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                "bad-txns-asset-frozen",
                                strprintf("input %u spends frozen %s", i, asset.GetHex()));
                        }
                    }
                }
            }
        }
    }

    if (g_con_elementsmode) {
        // Tally transaction fees
        if (!HasValidFee(tx)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-fee-outofrange");
        }

        // SEQUENTIA: supervised assets (src/supervision.h).
        //
        // Here, and not in VerifyAmounts, because this is the one function both
        // mempool acceptance (validation.cpp AcceptToMemoryPool) and ConnectBlock
        // pass through, and because the gate needs nSpendHeight, which is the
        // height of the block the transaction confirms in on both paths.
        //
        // Below the activation height a declaration is inert: the issuance
        // derives plainly, which is exactly what a node without this code does,
        // so old and new nodes agree about every block that already exists.
        const SupervisionDescriptor* supervision = nullptr;
        std::optional<SupervisionDeclaration> declaration;
        if (SupervisionActive(nSpendHeight)) {
            bool malformed = false;
            declaration = SupervisionFromTx(tx, malformed);
            if (malformed) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-supervision-multiple-declarations");
            }
            if (declaration) {
                uint256 entropy;
                std::string err;
                if (!CheckSupervisedIssuance(tx, *declaration, entropy, err)) {
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-supervision-declaration", err);
                }
                supervision = &declaration->descriptor;
            }

            // Freeze and rotation records. Admission is consensus, not policy:
            // a record that reached a block unsigned would freeze someone on
            // nobody's authority. The registry it is checked against is the one
            // applied through the PREVIOUS block -- ConnectTip updates it only
            // after the block validates -- which is what gives Alberto's
            // next-block effectiveness and keeps mempool acceptance and block
            // validation reading the same state.
            std::string err;
            if (!CheckSupervisionRecords(tx, SupervisionRegistry::GetInstance(), err)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                     "bad-txns-supervision-record", err);
            }

            // Reissuing a supervised asset counts as touching it, even though
            // no input carries it: the reissuance creates units out of the
            // token, and those units must land explicit like every other one.
            // The asset comes from the entropy the reissuance quotes, so it is
            // readable here without the spent output.
            const SupervisionRegistry& supervision = SupervisionRegistry::GetInstance();
            if (!supervision.Empty()) {
                for (const CTxIn& in : tx.vin) {
                    if (in.assetIssuance.IsNull() || in.assetIssuance.assetBlindingNonce.IsNull()) {
                        continue;
                    }
                    CAsset reissued;
                    CalculateAsset(reissued, in.assetIssuance.assetEntropy);
                    if (!supervision.IsSupervised(reissued)) continue;
                    touches_supervised = true;
                    if (!in.assetIssuance.nAmount.IsExplicit()) {
                        return state.Invalid(TxValidationResult::TX_CONSENSUS,
                            "bad-txns-supervised-blinded",
                            "a supervised asset may not be reissued into a blinded amount");
                    }
                }
            }

            // The induction step. Issuance starts a supervised asset in
            // explicit outputs (CheckSupervisedIssuance); this keeps it there
            // for ever after. Consensus cannot read a blinded output's asset,
            // so a supervised asset inside one would be unfreezable and
            // unauditable, and enforcing only at spend time would strand
            // whoever already held such an output, permanently.
            //
            // EVERY output, not merely the supervised ones, because which asset
            // a blinded output carries is exactly what cannot be determined.
            // The cost is real and lands on wallets rather than users: a
            // transaction that moves a supervised asset cannot blind its change
            // in some other asset either. See the wallet note in
            // doc/sequentia/supervised-assets-implementation.md.
            if (touches_supervised) {
                for (const CTxOut& out : tx.vout) {
                    if (!out.nAsset.IsExplicit() || !out.nValue.IsExplicit()) {
                        return state.Invalid(TxValidationResult::TX_CONSENSUS,
                            "bad-txns-supervised-blinded",
                            "a transaction moving a supervised asset must keep every output explicit");
                    }
                }
            }
        }

        // Verify that amounts add up.
        if (fScriptChecks && !VerifyAmounts(spent_inputs, tx, SupervisionActive(nSpendHeight), supervision, pvChecks, cacheStore)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-ne-out", "value in != value out");
        }
        fee_map += GetFeeMap(tx);
        if (!MoneyRange(fee_map)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-block-total-fee-outofrange");
        }
    } else {
        const CAmount value_out = tx.GetValueOutMap()[CAsset()];
        if (nValueIn < value_out) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-belowout",
                strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(value_out)));
        }

        // Tally transaction fees
        const CAmount txfee_aux = nValueIn - value_out;
        if (!MoneyRange(txfee_aux)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-fee-outofrange");
        }

        fee_map[CAsset()] += txfee_aux;
    }

    return true;
}
