// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <anchor.h>
#include <assetsdir.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <exchangerates.h>
#include <interfaces/chain.h>
#include <issuance.h>
#include <key_io.h>
#include <mainchainrpc.h>
#include <policy/policy.h>
#include <primitives/bitcoin/transaction.h>
#include <fstream>
#include <wallet/scriptpubkeyman.h>
#include <policy/settings.h>
#include <pos.h>
#include <rpc/rawtransaction_util.h>
#include <rpc/util.h>
#include <script/interpreter.h>
#include <script/pegins.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/time.h>
#include <util/translation.h>
#include <util/vector.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/receive.h>
#include <wallet/rpc/util.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/spend.h>
#include <wallet/parentchain.h>
#include <wallet/stakingrewards.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <map>

using wallet::CRecipient;

namespace wallet {
static void ParseRecipients(const UniValue& address_amounts, const UniValue& address_assets, const UniValue& subtract_fee_outputs, std::vector<CRecipient> &recipients) {
    std::set<CTxDestination> destinations;
    int i = 0;
    for (const std::string& address: address_amounts.getKeys()) {
        CAsset asset = Params().GetConsensus().pegged_asset;
        if (!address_assets.isNull() && address_assets[address].isStr()) {
            std::string strasset = address_assets[address].get_str();
            asset = GetAssetFromString(strasset);
        }
        if (asset.IsNull() && g_con_elementsmode) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex: %s", asset.GetHex()));
        }

        CTxDestination dest = DecodeDestination(address);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Bitcoin address: ") + address);
        }

        if (destinations.count(dest)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + address);
        }
        destinations.insert(dest);

        CScript script_pub_key = GetScriptForDestination(dest);
        CAmount amount = AmountFromValue(address_amounts[i++], asset == Params().GetConsensus().pegged_asset);

        bool subtract_fee = false;
        for (unsigned int idx = 0; idx < subtract_fee_outputs.size(); idx++) {
            const UniValue& addr = subtract_fee_outputs[idx];
            if (addr.get_str() == address) {
                subtract_fee = true;
            }
        }

        CRecipient recipient = {script_pub_key, amount, asset, GetDestinationBlindingKey(dest), subtract_fee};
        recipients.push_back(recipient);
    }
}

/** SEQUENTIA: the assets of the recipients the fee is being subtracted from. */
static std::vector<CAsset> SubtractFeeFromAssets(const std::vector<CRecipient>& recipients)
{
    std::vector<CAsset> assets;
    for (const CRecipient& recipient : recipients) {
        if (recipient.fSubtractFeeFromAmount) assets.push_back(recipient.asset);
    }
    return assets;
}

UniValue SendMoney(CWallet& wallet, const CCoinControl &coin_control, std::vector<CRecipient> &recipients, mapValue_t map_value, bool verbose, bool ignore_blind_fail)
{
    EnsureWalletIsUnlocked(wallet);

    // This function is only used by sendtoaddress and sendmany.
    // This should always try to sign, if we don't have private keys, don't try to do anything here.
    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    // Shuffle recipient list
    std::shuffle(recipients.begin(), recipients.end(), FastRandomContext());

    // Send
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    bilingual_str error;
    CTransactionRef tx;
    FeeCalculation fee_calc_out;
    auto blind_details = g_con_elementsmode ? std::make_unique<BlindDetails>() : nullptr;
    if (blind_details) blind_details->ignore_blind_failure = ignore_blind_fail;
    const bool fCreated = CreateTransaction(wallet, recipients, tx, nFeeRequired, nChangePosRet, error, coin_control, fee_calc_out, true, blind_details.get());
    if (!fCreated) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error.original);
    }
    wallet.CommitTransaction(tx, std::move(map_value), {} /* orderForm */, blind_details.get());
    if (verbose) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", tx->GetHash().GetHex());
        entry.pushKV("fee_reason", StringForFeeReason(fee_calc_out.reason));
        return entry;
    }
    return tx->GetHash().GetHex();
}

RPCHelpMan registerstake()
{
    return RPCHelpMan{"registerstake",
                "\nRegister an amount of Sequence (SEQ) as proof-of-stake for a staker public key, by funding the\n"
                "canonical staking output (see getstakescript) from this wallet. The amount counts as the\n"
                "key's on-chain stake while the output stays unspent; spending it (unbonding) requires the\n"
                "staker key and the script's CSV maturity. Get a staker pubkey with getnewaddress followed\n"
                "by getaddressinfo. To then produce blocks, call startposproducer with the staker key's WIF\n"
                "(no restart needed; it persists across restarts) — or start the node with -posproducer and\n"
                "-posproducerkey.\n",
                {
                    {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The staker public key (hex)."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Amount of Sequence (SEQ) to stake (at or above the chain's minimum stake)."},
                    {"csv_blocks", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Height-based unbonding delay in blocks (default: the chain minimum)."},
                    {"csv_seconds", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Time-based unbonding delay in seconds (mutually exclusive with csv_blocks)."},
                    {"blspubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "SEQUENTIA: committee BLS public key to register with this stake (from getblsregistration), so the staker can join the public fixed-size committee. Requires pop."},
                    {"pop", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "SEQUENTIA: the BLS proof-of-possession for blspubkey (from getblsregistration)."},
                    {"liquid_locktime", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "SEQUENTIA vesting: an absolute timelock (BIP65) before which the stake cannot be spent, sold, or transferred, while still accruing stake weight throughout (a \"staking-only period\"). A unix time (>=500000000) or a block height (<500000000)."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the registration transaction id"},
                    {RPCResult::Type::STR_HEX, "script", "the staking scriptPubKey that was funded"},
                    {RPCResult::Type::NUM, "csv", "the BIP68 CSV value encoded in the script"},
                    {RPCResult::Type::NUM, "unbonding_seconds", "the unbonding lock in seconds before the stake can be withdrawn"},
                    {RPCResult::Type::NUM, "liquid_locktime", /*optional=*/true, "the absolute vesting locktime encoded in the script, if any"},
                }},
                RPCExamples{HelpExampleCli("registerstake", "\"02abc...\" 50000")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    std::vector<unsigned char> pubkey_bytes = ParseHexV(request.params[0], "pubkey");
    CPubKey pubkey(pubkey_bytes);
    if (!pubkey.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid public key");

    const bool has_blocks = !request.params[2].isNull();
    const bool has_seconds = !request.params[3].isNull();
    if (has_blocks && has_seconds) throw JSONRPCError(RPC_INVALID_PARAMETER, "Specify at most one of csv_blocks or csv_seconds");
    uint32_t csv;
    if (has_seconds) {
        int64_t secs = request.params[3].get_int64();
        int64_t units = (secs + (1 << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1) >> CTxIn::SEQUENCE_LOCKTIME_GRANULARITY;
        if (units < 1 || units > (int64_t)CTxIn::SEQUENCE_LOCKTIME_MASK) throw JSONRPCError(RPC_INVALID_PARAMETER, "csv_seconds out of range");
        csv = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | (uint32_t)units;
    } else {
        int64_t blocks = has_blocks ? request.params[2].get_int64() : (int64_t)g_pos_unbonding_period;
        if (blocks < 1 || blocks > (int64_t)CTxIn::SEQUENCE_LOCKTIME_MASK) throw JSONRPCError(RPC_INVALID_PARAMETER, "csv_blocks must be between 1 and 65535");
        csv = (uint32_t)blocks;
    }
    auto lock = PosStakeLockSeconds(csv);
    const int64_t required = PosRequiredUnbondingSeconds();
    if (!lock || *lock < required) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("the unbonding lock (%d s) is below the chain's minimum (%d s); it would not count as stake", lock ? *lock : 0, required));

    // Optional committee BLS registration (impl spec Option A phase 2): the key
    // rides in the staking output, so the registry learns it as a pure function
    // of the UTXO set. Its PoP is verified when the funding block connects.
    std::vector<unsigned char> bls_pubkey, bls_pop;
    const bool has_bls = !request.params[4].isNull() || !request.params[5].isNull();
    if (has_bls) {
        if (request.params[4].isNull() || request.params[5].isNull())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "blspubkey and pop must be given together");
        bls_pubkey = ParseHexV(request.params[4], "blspubkey");
        bls_pop = ParseHexV(request.params[5], "pop");
        if (bls_pubkey.size() != 48 || bls_pop.size() != 96)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "blspubkey must be 48 bytes and pop 96 bytes (see getblsregistration)");
    }
    int64_t liquid_locktime = 0;
    if (!request.params[6].isNull()) {
        liquid_locktime = request.params[6].get_int64();
        if (liquid_locktime <= 0 || liquid_locktime > 0xffffffffLL)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "liquid_locktime must be between 1 and 4294967295 (a unix time, or a block height below 500000000)");
    }
    CScript stake_script = BuildStakeScript(pubkey, csv, bls_pubkey, bls_pop, liquid_locktime);
    CAmount amount = AmountFromValue(request.params[1], true);
    // Enforce the chain's minimum-stake floor: a sub-floor output is silently
    // dropped from the schedule/committee by PosIsEligibleStake and would never
    // count, so refuse it here rather than fund a stake that does nothing.
    if (g_pos_min_stake > 0 && (uint64_t)amount < g_pos_min_stake)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("amount is below the chain's minimum stake of %d SEQ; it would not count as stake", g_pos_min_stake / 100000000ULL));

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    CRecipient recipient = {stake_script, amount, Params().GetConsensus().pegged_asset, CPubKey(), false};
    std::vector<CRecipient> recipients = {recipient};
    CCoinControl coin_control;
    mapValue_t mapValue;
    UniValue txid = SendMoney(*pwallet, coin_control, recipients, mapValue, /*verbose=*/false, /*ignore_blind_fail=*/true);

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid);
    result.pushKV("script", HexStr(stake_script));
    result.pushKV("csv", (int64_t)csv);
    if (lock) result.pushKV("unbonding_seconds", (int64_t)*lock);
    if (liquid_locktime > 0) result.pushKV("liquid_locktime", liquid_locktime);
    return result;
},
    };
}

// --- SEQUENTIA unstake: find, list and withdraw this wallet's staking outputs ---

namespace {

//! One of this wallet's staking UTXOs, with everything needed to judge its
//! maturity and to spend it (withdrawstake), plus the numbers liststakeutxos
//! reports.
struct StakeUtxo {
    COutPoint outpoint;
    CTxOut txout;
    ParsedStake parsed;          //!< staker pubkey, CSV, BLS registration, vesting lock
    CAmount amount{0};           //!< explicit policy-asset amount (the stake weight)
    int fund_height{-1};         //!< height the funding output confirmed at (-1 while unconfirmed)
    bool unconfirmed{false};     //!< the funding transaction is still in the mempool
    bool withdrawing{false};     //!< already spent by a withdrawal that has not confirmed yet
    bool csv_mature{false};      //!< unbonding (BIP68) served, judged against the tip
    bool vesting_mature{false};  //!< liquid_locktime (BIP65) passed, or none carried
    int spendable_height{-1};    //!< height-based CSV: first block that may contain the spend
    int64_t spendable_time{0};   //!< time-based CSV: earliest spend MTP; 0 when height-based
    bool Mature() const { return csv_mature && vesting_mature; }
};

//! Does this wallet hold the private key for `pubkey`? Judged through IsMine on
//! the key's standard destinations, so it answers correctly even while the
//! wallet is locked (the staking flow hands out staker keys via getnewaddress,
//! so the key lives in the wallet as an ordinary address key).
bool WalletControlsStakerKey(const CWallet& wallet, const CPubKey& pubkey) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (wallet.IsMine(GetScriptForDestination(WitnessV0KeyHash(pubkey))) & ISMINE_SPENDABLE) return true;
    return (wallet.IsMine(GetScriptForDestination(PKHash(pubkey))) & ISMINE_SPENDABLE) != 0;
}

//! The staker private key for `pubkey`, from whichever ScriptPubKeyMan holds
//! it. The wallet must be unlocked.
bool GetStakerKey(const CWallet& wallet, const CPubKey& pubkey, CKey& key_out)
{
    const CKeyID keyid = pubkey.GetID();
    for (ScriptPubKeyMan* spk_man : wallet.GetAllScriptPubKeyMans()) {
        if (auto* legacy = dynamic_cast<LegacyScriptPubKeyMan*>(spk_man)) {
            if (legacy->GetKey(keyid, key_out)) return true;
        } else if (auto* desc = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
            if (desc->GetStakingKey(pubkey, key_out)) return true;
        }
    }
    return false;
}

//! Find this wallet's unspent staking outputs: outputs of the wallet's own
//! transactions paying the canonical staking script (BuildStakeScript) for a
//! staker key this wallet controls, still present in the UTXO set.
//! registerstake funds the output from this wallet, so the funding transaction
//! is always a wallet transaction; a staking output funded by a *different*
//! wallet toward one of our keys is out of scope (finding it would take a full
//! UTXO-set scan). Maturity is judged against the current tip: the spend would
//! land in block tip+1.
std::vector<StakeUtxo> FindWalletStakeUtxos(CWallet& wallet, const std::optional<CPubKey>& only_pubkey) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    // Who spends what, so a staking output that is already spent can be told
    // apart: spent by a CONFIRMED transaction means the stake is gone, spent by
    // one still waiting means a withdrawal is in flight — and the stake registry
    // keeps crediting its weight until that transaction confirms.
    // An outpoint can have SEVERAL spenders in the wallet once a withdrawal has
    // been replaced by fee: the replacement and the transaction it replaced both
    // sit here, the latter conflicted (negative depth). Only the live one says
    // anything about the stake, so collect them all and pick it deliberately —
    // taking whichever came first would depend on map order and answer
    // differently from one run to the next.
    std::multimap<COutPoint, const CWalletTx*> spenders;
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        for (const CTxIn& in : wtx.tx->vin) spenders.emplace(in.prevout, &wtx);
    }
    // The spender that still counts: confirmed, or waiting in the mempool.
    // Conflicted and abandoned ones are dead and must not be reported at all.
    const auto live_spender = [&](const COutPoint& op) -> const CWalletTx* {
        const auto range = spenders.equal_range(op);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second->isAbandoned()) continue;
            if (wallet.GetTxDepthInMainChain(*it->second) < 0) continue; // replaced
            return it->second;
        }
        return nullptr;
    };

    std::vector<StakeUtxo> found;
    for (const auto& [wtxid, wtx] : wallet.mapWallet) {
        for (uint32_t n = 0; n < wtx.tx->vout.size(); ++n) {
            const CTxOut& out = wtx.tx->vout[n];
            if (!StakeFromTxOut(out)) continue; // qualifying stake: explicit policy asset, lock >= chain minimum
            auto parsed = ParseStakeScriptFull(out.scriptPubKey);
            if (!parsed) continue;
            if (only_pubkey && parsed->pubkey != *only_pubkey) continue;
            if (!WalletControlsStakerKey(wallet, parsed->pubkey)) continue;
            StakeUtxo s;
            s.outpoint = COutPoint(wtxid, n);
            if (wallet.IsSpent(wtxid, n)) {
                // Report it only while the spend is still pending, so the caller
                // can say "a withdrawal is on its way" instead of contradicting
                // the stake weight the registry still shows.
                const CWalletTx* sp = live_spender(s.outpoint);
                if (!sp) continue;
                if (wallet.GetTxDepthInMainChain(*sp) > 0) continue; // confirmed: really gone
                s.withdrawing = true;
            }
            s.txout = out;
            s.parsed = *parsed;
            s.amount = out.nValue.GetAmount();
            found.push_back(std::move(s));
        }
    }
    if (found.empty()) return found;

    // Which candidates are still unspent, and at what height each was funded:
    // both read from the UTXO set in one call.
    std::map<COutPoint, Coin> coins;
    for (const StakeUtxo& s : found) coins[s.outpoint];
    wallet.chain().findCoins(coins);

    const int tip_height = wallet.GetLastBlockHeight();
    const uint256 tip_hash = wallet.GetLastBlockHash();
    int64_t tip_mtp = 0;
    wallet.chain().findBlock(tip_hash, interfaces::FoundBlock().mtpTime(tip_mtp));

    std::vector<StakeUtxo> live;
    for (StakeUtxo& s : found) {
        // A withdrawal already sent is no longer in the UTXO set, so there is no
        // coin to read and no maturity left to judge: it is on its way out, and
        // only worth reporting so the caller can explain the wait.
        if (s.withdrawing) {
            s.csv_mature = false;
            s.vesting_mature = false;
            live.push_back(std::move(s));
            continue;
        }
        const Coin& coin = coins[s.outpoint];
        if (coin.out.IsNull()) continue; // spent

        // findCoins reports mempool outputs too, with nHeight set to the
        // MEMPOOL_HEIGHT sentinel (INT_MAX). An unconfirmed staking output has
        // not started its unbonding clock at all — BIP68 counts from the block
        // that CONFIRMS the funding — so it can never be withdrawable yet.
        // Detect it by the height being beyond the tip, which also keeps the
        // fund_height + csv arithmetic below from overflowing (that overflow
        // wrapped negative and made an unconfirmed stake look mature).
        if (coin.nHeight > (uint32_t)tip_height) {
            s.fund_height = -1;
            s.unconfirmed = true;
            s.csv_mature = false;
            s.vesting_mature = false;
            live.push_back(std::move(s));
            continue;
        }
        s.fund_height = (int)coin.nHeight;

        // Unbonding (BIP68 relative lock): height-based counts blocks from the
        // funding height; time-based counts 512-second units from the median
        // time of the funding block's parent.
        const uint32_t csv_value = s.parsed.csv & CTxIn::SEQUENCE_LOCKTIME_MASK;
        if (s.parsed.csv & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            const int64_t lock_secs = (int64_t)csv_value << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY;
            int64_t fund_parent_mtp = 0;
            wallet.chain().findAncestorByHeight(tip_hash, s.fund_height > 0 ? s.fund_height - 1 : 0,
                                                interfaces::FoundBlock().mtpTime(fund_parent_mtp));
            s.spendable_time = fund_parent_mtp + lock_secs;
            s.csv_mature = tip_mtp >= s.spendable_time;
        } else {
            s.spendable_height = s.fund_height + (int)csv_value;
            s.csv_mature = tip_height + 1 >= s.spendable_height;
        }

        // Vesting (BIP65 absolute lock), when the output carries one.
        if (s.parsed.liquid_locktime > 0) {
            if (s.parsed.liquid_locktime < (int64_t)LOCKTIME_THRESHOLD) {
                s.vesting_mature = tip_height >= s.parsed.liquid_locktime;
            } else {
                s.vesting_mature = tip_mtp > s.parsed.liquid_locktime;
            }
        } else {
            s.vesting_mature = true;
        }
        live.push_back(std::move(s));
    }
    return live;
}

//! Rough wall-clock seconds per block, for translating a height wait into a
//! calendar date (the slot interval itself is not exported by pos.h).
int64_t ApproxSecondsPerBlock()
{
    // The CADENCE, which is pos_block_spacing -- not the slot interval, and not
    // PosRequiredUnbondingSeconds()/g_pos_unbonding_period, which reduces to the
    // slot interval. Those are two different numbers now: the slot interval
    // scales the leader time-gate (30 s), while the chain actually produces a
    // block every pos_block_spacing (60 s). Using the gate unit here halved
    // every date this function produces -- it would have promised a stake locked
    // for 43,200 blocks that it unlocks in 15 days when it really takes 30.
    const int64_t spacing = Params().GetConsensus().pos_block_spacing;
    if (spacing > 0) return spacing;
    return g_pos_slot_interval > 0 ? g_pos_slot_interval : 30;
}

//! Why an immature staking output cannot be withdrawn yet, with when it can:
//! "unbonding until block 45120 (around 2026-08-06T10:00:00Z)".
std::string DescribeImmaturity(const StakeUtxo& s, int tip_height, int64_t tip_time)
{
    // Already withdrawn, just not yet confirmed — the stake registry still
    // credits its weight until it is.
    if (s.withdrawing) {
        return "a withdrawal of this stake is waiting to confirm";
    }
    // Nothing to date yet: the unbonding clock starts at the confirming block.
    if (s.unconfirmed) {
        return "waiting for the stake registration to confirm; the unbonding delay starts from that block";
    }
    if (!s.csv_mature) {
        if (s.spendable_height >= 0) {
            const int64_t eta = tip_time + (int64_t)(s.spendable_height - (tip_height + 1)) * ApproxSecondsPerBlock();
            return strprintf("unbonding until block %d (around %s)", s.spendable_height, FormatISO8601DateTime(eta));
        }
        return strprintf("unbonding until %s", FormatISO8601DateTime(s.spendable_time));
    }
    if (s.parsed.liquid_locktime >= (int64_t)LOCKTIME_THRESHOLD) {
        return strprintf("vesting-locked until %s", FormatISO8601DateTime(s.parsed.liquid_locktime));
    }
    const int64_t eta = tip_time + (s.parsed.liquid_locktime - tip_height) * ApproxSecondsPerBlock();
    return strprintf("vesting-locked until block %d (around %s)", (int)s.parsed.liquid_locktime, FormatISO8601DateTime(eta));
}

} // namespace

RPCHelpMan liststakeutxos()
{
    return RPCHelpMan{"liststakeutxos",
                "\nList this wallet's staking outputs (see registerstake): amount, staker key, and whether each\n"
                "is withdrawable right now. The unbonding lock counts from the block that funded the stake, so\n"
                "a stake older than the unbonding period can be withdrawn immediately with withdrawstake.\n",
                {
                    {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Only staking outputs registered to this staker public key (hex)."},
                },
                RPCResult{RPCResult::Type::ARR, "", "", {
                    {RPCResult::Type::OBJ, "", "", {
                        {RPCResult::Type::STR_HEX, "txid", "the funding transaction id"},
                        {RPCResult::Type::NUM, "vout", "the funding output index"},
                        {RPCResult::Type::STR_AMOUNT, "amount", "the staked amount"},
                        {RPCResult::Type::STR_HEX, "pubkey", "the staker public key"},
                        {RPCResult::Type::NUM, "funded_height", /*optional=*/true, "height the stake confirmed at (absent while unconfirmed)"},
                        {RPCResult::Type::BOOL, "confirmed", "whether the registration has confirmed; the unbonding delay only starts then"},
                        {RPCResult::Type::BOOL, "withdrawing", "a withdrawal of this stake has been sent and is waiting to confirm; it still counts as stake until then"},
                        {RPCResult::Type::BOOL, "withdrawable", "whether the stake can be withdrawn right now"},
                        {RPCResult::Type::NUM, "spendable_height", /*optional=*/true, "first block that could contain the withdrawal (height-locked stakes)"},
                        {RPCResult::Type::NUM_TIME, "spendable_time", /*optional=*/true, "earliest withdrawal time (time-locked stakes)"},
                        {RPCResult::Type::NUM, "liquid_locktime", /*optional=*/true, "the vesting lock (BIP65) the output carries, if any"},
                        {RPCResult::Type::STR, "status", "human-readable maturity"},
                    }},
                }},
                RPCExamples{HelpExampleCli("liststakeutxos", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    std::optional<CPubKey> only;
    if (!request.params[0].isNull()) {
        CPubKey pk(ParseHexV(request.params[0], "pubkey"));
        if (!pk.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid public key");
        only = pk;
    }

    LOCK(pwallet->cs_wallet);
    const int tip_height = pwallet->GetLastBlockHeight();
    int64_t tip_time = 0;
    pwallet->chain().findBlock(pwallet->GetLastBlockHash(), interfaces::FoundBlock().time(tip_time));

    UniValue result(UniValue::VARR);
    for (const StakeUtxo& s : FindWalletStakeUtxos(*pwallet, only)) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", s.outpoint.hash.GetHex());
        o.pushKV("vout", (int64_t)s.outpoint.n);
        o.pushKV("amount", ValueFromAmount(s.amount));
        o.pushKV("pubkey", HexStr(s.parsed.pubkey));
        if (s.fund_height >= 0) o.pushKV("funded_height", s.fund_height);
        o.pushKV("confirmed", !s.unconfirmed);
        o.pushKV("withdrawing", s.withdrawing);
        o.pushKV("withdrawable", s.Mature());
        if (s.spendable_height >= 0) o.pushKV("spendable_height", s.spendable_height);
        if (s.spendable_time > 0) o.pushKV("spendable_time", s.spendable_time);
        if (s.parsed.liquid_locktime > 0) o.pushKV("liquid_locktime", s.parsed.liquid_locktime);
        o.pushKV("status", s.Mature() ? "withdrawable now" : DescribeImmaturity(s, tip_height, tip_time));
        result.push_back(o);
    }
    return result;
},
    };
}

RPCHelpMan withdrawstake()
{
    return RPCHelpMan{"withdrawstake",
                "\nWithdraw (unstake) Sequence (SEQ) this wallet registered with registerstake, by spending the\n"
                "staking output(s) back to a fresh receiving address of this wallet. A staking output can be\n"
                "withdrawn once its unbonding lock has been served, counted from the block that FUNDED it; the\n"
                "withdrawal itself needs no further delay — the coins are spendable as soon as the withdrawal\n"
                "confirms, and the key's registered stake weight drops at that same confirmation. See\n"
                "liststakeutxos for what is withdrawable and when.\n"
                "\nStaking outputs are whole coins. To withdraw part of one, the remainder is re-staked into a\n"
                "fresh staking output for the same key — which restarts the remainder's unbonding clock.\n",
                {
                    {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Withdraw only stake registered to this staker public key (hex). Required for a partial withdrawal when the wallet stakes with more than one key."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "Amount of SEQ to remove from the stake (default: all withdrawable stake). The network fee is paid out of this amount."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Destination address (default: a fresh address of this wallet). The withdrawal output is explicit (not confidential)."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the withdrawal transaction id"},
                    {RPCResult::Type::STR_AMOUNT, "amount", "SEQ arriving at the destination (the withdrawn amount minus the fee)"},
                    {RPCResult::Type::STR_AMOUNT, "fee", "the network fee, paid out of the withdrawn amount"},
                    {RPCResult::Type::STR, "destination", "the receiving address"},
                    {RPCResult::Type::STR_AMOUNT, "unstaked", "stake weight removed from the staker key (amount + fee)"},
                    {RPCResult::Type::STR_AMOUNT, "restaked", /*optional=*/true, "remainder re-staked into a fresh output (its unbonding clock restarts)"},
                    {RPCResult::Type::ARR, "withdrawn_outputs", "the staking outputs this withdrawal spends", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "txid", "funding transaction id"},
                            {RPCResult::Type::NUM, "vout", "funding output index"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "staked amount"},
                            {RPCResult::Type::STR_HEX, "pubkey", "staker public key"},
                        }},
                    }},
                    {RPCResult::Type::NUM, "stake_before", "this wallet's registered stake weight before the withdrawal (atoms)"},
                    {RPCResult::Type::NUM, "stake_after", "this wallet's stake weight once the withdrawal confirms (atoms)"},
                    {RPCResult::Type::NUM, "network_stake_before", "total registered stake on the network (atoms)"},
                    {RPCResult::Type::NUM, "network_stake_after", "total stake once the withdrawal confirms (atoms)"},
                    {RPCResult::Type::NUM, "share_before", /*optional=*/true, "this wallet's share of the network stake now (0..1)"},
                    {RPCResult::Type::NUM, "share_after", /*optional=*/true, "this wallet's share once the withdrawal confirms (0..1)"},
                }},
                RPCExamples{HelpExampleCli("withdrawstake", "") + HelpExampleCli("withdrawstake", "\"02abc...\" 10000")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    std::optional<CPubKey> only;
    if (!request.params[0].isNull()) {
        CPubKey pk(ParseHexV(request.params[0], "pubkey"));
        if (!pk.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid public key");
        only = pk;
    }
    std::optional<CAmount> want;
    if (!request.params[1].isNull()) {
        want = AmountFromValue(request.params[1], true);
        if (*want <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "amount must be positive");
    }

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    const int tip_height = pwallet->GetLastBlockHeight();
    int64_t tip_time = 0;
    pwallet->chain().findBlock(pwallet->GetLastBlockHash(), interfaces::FoundBlock().time(tip_time));

    // 1) The wallet's live staking outputs, split by maturity.
    std::vector<StakeUtxo> stakes = FindWalletStakeUtxos(*pwallet, only);
    if (stakes.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, only
            ? strprintf("this wallet has no staking outputs for key %s", HexStr(*only))
            : std::string("this wallet has no staking outputs (see registerstake)"));
    }
    std::vector<StakeUtxo> mature;
    CAmount mature_total = 0, immature_total = 0, withdrawing_total = 0;
    const StakeUtxo* soonest = nullptr;
    for (const StakeUtxo& s : stakes) {
        if (s.withdrawing) {
            // Already on its way out: not withdrawable again, and not something
            // the caller is waiting to unlock either.
            withdrawing_total += s.amount;
        } else if (s.Mature()) {
            mature_total += s.amount;
            mature.push_back(s);
        } else {
            immature_total += s.amount;
            // The stake that unlocks first, for the "try again when" message. A
            // still-unconfirmed stake is the worst candidate, not the best: its
            // unbonding clock has not started, so it can only ever unlock after
            // every confirmed one. Prefer a confirmed stake whenever there is one.
            if (!soonest) {
                soonest = &s;
            } else if (soonest->unconfirmed != s.unconfirmed) {
                if (soonest->unconfirmed) soonest = &s; // a confirmed one always wins
            } else if (!s.unconfirmed) {
                const bool by_height = s.spendable_height >= 0 && soonest->spendable_height >= 0;
                if (by_height ? s.spendable_height < soonest->spendable_height
                              : s.spendable_time < soonest->spendable_time) {
                    soonest = &s;
                }
            }
        }
    }
    if (mature.empty()) {
        if (!soonest) {
            // Everything this wallet had staked is already being withdrawn.
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                "a withdrawal of %s SEQ has already been sent and is waiting to confirm; there is nothing "
                "left to withdraw", FormatMoney(withdrawing_total)));
        }
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "none of the %s SEQ this wallet has staked is withdrawable yet; the soonest is %s",
            FormatMoney(immature_total), DescribeImmaturity(*soonest, tip_height, tip_time)));
    }

    // 2) What to withdraw. The requested amount is the stake weight that stops
    //    staking; the network fee is paid out of it.
    const CAmount want_amt = want.value_or(mature_total);
    if (want_amt > mature_total) {
        throw JSONRPCError(RPC_WALLET_ERROR, immature_total > 0
            ? strprintf("only %s of the staked %s SEQ is withdrawable right now — the rest is still unbonding (see liststakeutxos)",
                        FormatMoney(mature_total), FormatMoney(mature_total + immature_total))
            : strprintf("only %s SEQ is withdrawable", FormatMoney(mature_total)));
    }
    // A partial withdrawal across several staker keys is well defined: the
    // selection below takes whole outputs largest-first, so at most ONE output
    // is split, and its remainder goes back to that same output's key. Pass
    // `pubkey` to confine the withdrawal to one staker.

    // Spend as few staking outputs as possible: largest first until covered.
    std::sort(mature.begin(), mature.end(),
              [](const StakeUtxo& a, const StakeUtxo& b) { return a.amount > b.amount; });
    std::vector<StakeUtxo> selected;
    CAmount selected_total = 0;
    for (const StakeUtxo& s : mature) {
        if (selected_total >= want_amt) break;
        selected.push_back(s);
        selected_total += s.amount;
    }
    // Whatever the selection overshoots stays staked: it goes back into a fresh
    // staking output for the same key (with a fresh unbonding clock — the only
    // way to split a coin the chain knows only as a whole).
    const CAmount restake_amt = selected_total - want_amt;
    const CAmount stake_floor = (CAmount)std::max<uint64_t>(g_pos_min_stake, 1);
    if (restake_amt > 0 && restake_amt < stake_floor) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
            "withdrawing %s SEQ would leave %s SEQ re-staked, below the chain's minimum stake of %s SEQ; "
            "withdraw the whole %s SEQ, or a smaller amount that leaves at least the minimum staked",
            FormatMoney(want_amt), FormatMoney(restake_amt), FormatMoney(stake_floor), FormatMoney(selected_total)));
    }

    // 3) The destination: a fresh address of this wallet unless one was given.
    CTxDestination dest;
    if (!request.params[2].isNull()) {
        dest = DecodeDestination(request.params[2].get_str());
        if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    } else {
        bilingual_str dest_error;
        if (!pwallet->GetNewDestination(pwallet->m_default_address_type, "", dest, dest_error)) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, dest_error.original);
        }
    }
    // The withdrawal output is explicit, so report the unconfidential form of
    // the address — that is what the chain will show.
    std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
    const CScript dest_script = GetScriptForDestination(dest);
    const std::string dest_str = EncodeDestination(dest);

    // 4) Build the spend. nVersion 2 activates BIP68; each input's nSequence
    //    must encode a relative lock at least as long as its script's CSV value
    //    (the exact value is both necessary and, the coin being mature,
    //    sufficient). nLockTime serves any vesting lock (BIP65) among the
    //    selected outputs — height and time locks cannot share a transaction.
    int64_t height_lock = 0, time_lock = 0;
    for (const StakeUtxo& s : selected) {
        if (s.parsed.liquid_locktime <= 0) continue;
        if (s.parsed.liquid_locktime < (int64_t)LOCKTIME_THRESHOLD) {
            height_lock = std::max(height_lock, s.parsed.liquid_locktime);
        } else {
            time_lock = std::max(time_lock, s.parsed.liquid_locktime);
        }
    }
    if (height_lock > 0 && time_lock > 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "cannot mix height-vested and time-vested stakes in one withdrawal; withdraw them separately (pass pubkey and amount)");
    }

    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = time_lock > 0 ? (uint32_t)time_lock : (uint32_t)tip_height;
    for (const StakeUtxo& s : selected) {
        CTxIn in(s.outpoint);
        in.nSequence = s.parsed.csv;
        mtx.vin.push_back(in);
    }
    const CAsset& asset = Params().GetConsensus().pegged_asset;
    mtx.vout.push_back(CTxOut(asset, want_amt, dest_script)); // fee patched out below
    if (restake_amt > 0) {
        const StakeUtxo& src = selected.back(); // the output whose split created the remainder
        // Same key, same unbonding delay, same BLS registration (a consensus
        // rule requires all of a staker's outputs to carry the same BLS key).
        // No vesting lock: it was already served, or there was none.
        mtx.vout.push_back(CTxOut(asset, restake_amt,
            BuildStakeScript(src.parsed.pubkey, src.parsed.csv, src.parsed.bls_pubkey, src.parsed.bls_pop, 0)));
    }
    mtx.vout.push_back(CTxOut(asset, 0, CScript())); // the explicit fee output, patched below

    // 5) The fee, from the final transaction's size with worst-case signatures
    //    (a staking spend's scriptSig is a single ECDSA signature push).
    CAmount fee = 0;
    {
        CMutableTransaction sizing = mtx;
        for (CTxIn& in : sizing.vin) in.scriptSig = CScript() << std::vector<unsigned char>(73);
        CCoinControl coin_control;
        fee = GetMinimumFeeRate(*pwallet, coin_control, nullptr).GetFee(GetVirtualTransactionSize(CTransaction(sizing)));
    }
    if (want_amt <= fee) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
            "the withdrawal (%s SEQ) would not cover its own network fee (%s SEQ)", FormatMoney(want_amt), FormatMoney(fee)));
    }
    mtx.vout.front().nValue = want_amt - fee;
    mtx.vout.back().nValue = fee;

    // 6) Sign each staking input with its staker key. The spend is a bare
    //    (pre-segwit) script, so this is a legacy signature over the staking
    //    script itself; the scriptSig is just the signature push.
    for (size_t i = 0; i < selected.size(); ++i) {
        const StakeUtxo& s = selected[i];
        CKey key;
        if (!GetStakerKey(*pwallet, s.parsed.pubkey, key)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("the private key for staker %s is not available in this wallet", HexStr(s.parsed.pubkey)));
        }
        FlatSigningProvider provider;
        provider.keys[s.parsed.pubkey.GetID()] = key;
        std::vector<unsigned char> sig;
        MutableTransactionSignatureCreator creator(&mtx, i, s.txout.nValue, SIGHASH_ALL);
        if (!creator.CreateSig(provider, sig, s.parsed.pubkey.GetID(), s.txout.scriptPubKey, SigVersion::BASE, /*flags=*/0)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "failed to sign the staking spend");
        }
        mtx.vin[i].scriptSig = CScript() << sig;
    }

    // 7) Self-check before anything leaves the wallet: every input must satisfy
    //    its staking script (signature, CSV, vesting) exactly as a validating
    //    node will judge it.
    for (size_t i = 0; i < selected.size(); ++i) {
        ScriptError serror = SCRIPT_ERR_OK;
        MutableTransactionSignatureChecker checker(&mtx, i, selected[i].txout.nValue, MissingDataBehavior::FAIL);
        if (!VerifyScript(mtx.vin[i].scriptSig, selected[i].txout.scriptPubKey, nullptr,
                          SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY,
                          checker, &serror)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("constructed an invalid staking spend (%s); nothing was sent", ScriptErrorString(serror)));
        }
    }

    // The wallet's standing before/after, for the caller's confirmation UI.
    // The registry moves only when the withdrawal confirms.
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const uint64_t total_before = PosTotalWeight(registry);
    uint64_t mine_before = 0;
    for (const auto& entry : registry.Weights()) {
        if (WalletControlsStakerKey(*pwallet, entry.first)) mine_before += entry.second;
    }
    const CAmount unstaked = selected_total - restake_amt;
    const uint64_t mine_after = mine_before > (uint64_t)unstaked ? mine_before - (uint64_t)unstaked : 0;
    const uint64_t total_after = total_before > (uint64_t)unstaked ? total_before - (uint64_t)unstaked : 0;

    // 8) Broadcast.
    const CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string err_string;
    if (!pwallet->chain().broadcastTransaction(tx, pwallet->m_default_max_tx_fee, /*relay=*/true, err_string)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("failed to broadcast the withdrawal: %s", err_string));
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount", ValueFromAmount(want_amt - fee));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("destination", dest_str);
    result.pushKV("unstaked", ValueFromAmount(unstaked));
    if (restake_amt > 0) result.pushKV("restaked", ValueFromAmount(restake_amt));
    UniValue ins(UniValue::VARR);
    for (const StakeUtxo& s : selected) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", s.outpoint.hash.GetHex());
        o.pushKV("vout", (int64_t)s.outpoint.n);
        o.pushKV("amount", ValueFromAmount(s.amount));
        o.pushKV("pubkey", HexStr(s.parsed.pubkey));
        ins.push_back(o);
    }
    result.pushKV("withdrawn_outputs", ins);
    result.pushKV("stake_before", mine_before);
    result.pushKV("stake_after", mine_after);
    result.pushKV("network_stake_before", total_before);
    result.pushKV("network_stake_after", total_after);
    if (total_before > 0) result.pushKV("share_before", (double)mine_before / (double)total_before);
    if (total_after > 0) result.pushKV("share_after", (double)mine_after / (double)total_after);
    return result;
},
    };
}

RPCHelpMan bumpwithdrawstakefee()
{
    return RPCHelpMan{"bumpwithdrawstakefee",
                "\nRe-send a pending stake withdrawal with a higher network fee (BIP125 replace-by-fee), for when\n"
                "the original is taking too long to confirm.\n"
                "\nThe wallet's own bumpfee cannot do this: it requires every input to be one the wallet\n"
                "recognises as its own, and a staking output is a bare script that IsMine does not match — nor\n"
                "could the generic signer re-sign it. This rebuilds the very same withdrawal — same staking\n"
                "inputs, same destination, same re-staked remainder — and only moves value from the withdrawn\n"
                "amount to the fee, so the replacement is the original transaction paying more.\n",
                {
                    {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "Fee rate in " + CURRENCY_ATOM + "/vB for the replacement (default: the smallest increase the network will accept)."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the replacement transaction id"},
                    {RPCResult::Type::STR_HEX, "replaced_txid", "the transaction it replaces"},
                    {RPCResult::Type::STR_AMOUNT, "old_fee", "the fee the original paid"},
                    {RPCResult::Type::STR_AMOUNT, "fee", "the fee the replacement pays"},
                    {RPCResult::Type::STR_AMOUNT, "amount", "SEQ now arriving at the destination (the extra fee comes out of it)"},
                    {RPCResult::Type::STR, "destination", "the receiving address (unchanged)"},
                }},
                RPCExamples{HelpExampleCli("bumpwithdrawstakefee", "") + HelpExampleCli("bumpwithdrawstakefee", "2")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    // The pending withdrawal is the unconfirmed transaction spending this
    // wallet's staking outputs — exactly what liststakeutxos flags "withdrawing".
    std::vector<StakeUtxo> stakes = FindWalletStakeUtxos(*pwallet, std::nullopt);
    std::vector<StakeUtxo> spent;
    for (const StakeUtxo& s : stakes) {
        if (s.withdrawing) spent.push_back(s);
    }
    if (spent.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "there is no pending stake withdrawal to re-send");
    }
    // Every input of one withdrawal shares its spending transaction. Skip the
    // dead ones: after an earlier bump the transaction that was replaced is
    // still in the wallet, conflicted, and picking it would try to replace a
    // transaction that no longer exists anywhere.
    std::set<uint256> spender_ids;
    const CWalletTx* original = nullptr;
    for (const StakeUtxo& s : spent) {
        for (const auto& [txid, wtx] : pwallet->mapWallet) {
            if (wtx.isAbandoned() || pwallet->GetTxDepthInMainChain(wtx) != 0) continue;
            for (const CTxIn& in : wtx.tx->vin) {
                if (in.prevout == s.outpoint) { spender_ids.insert(txid); original = &wtx; }
            }
        }
    }
    if (!original) throw JSONRPCError(RPC_WALLET_ERROR, "could not find the pending withdrawal transaction");
    if (spender_ids.size() > 1) {
        throw JSONRPCError(RPC_WALLET_ERROR, "more than one pending stake withdrawal; wait for them to confirm");
    }

    // Rebuild it: same inputs, same outputs, only the split between the
    // destination and the fee changes.
    CMutableTransaction mtx(*original->tx);
    int dest_idx = -1, fee_idx = -1;
    for (size_t i = 0; i < mtx.vout.size(); ++i) {
        if (mtx.vout[i].IsFee()) { fee_idx = (int)i; continue; }
        // The re-staked remainder must not shrink — that would silently change
        // how much stays staked. Only the payout to ourselves absorbs the fee.
        if (ParseStakeScript(mtx.vout[i].scriptPubKey)) continue;
        if (dest_idx < 0) dest_idx = (int)i;
    }
    if (dest_idx < 0 || fee_idx < 0) {
        throw JSONRPCError(RPC_WALLET_ERROR, "the pending withdrawal does not look like one this wallet built");
    }
    const CAmount old_fee = mtx.vout[fee_idx].nValue.GetAmount();

    // What the replacement must pay: BIP125 wants the old fee rate plus one
    // incremental relay fee over the new size, and the caller may ask for more.
    const int64_t vsize = GetVirtualTransactionSize(CTransaction(mtx));
    const CFeeRate incremental = std::max(pwallet->chain().relayIncrementalFee(), CFeeRate(WALLET_INCREMENTAL_RELAY_FEE));
    CAmount new_fee = old_fee + incremental.GetFee(vsize);
    if (!request.params[0].isNull()) {
        const CFeeRate asked{AmountFromValue(request.params[0], /*is_policy_asset=*/true, /*decimals=*/3) * 1000};
        const CAmount wanted = asked.GetFee(vsize);
        if (wanted <= new_fee) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "fee_rate is too low to replace the pending withdrawal: it would pay %s SEQ, and the network "
                "requires at least %s SEQ (the original paid %s)",
                FormatMoney(wanted), FormatMoney(new_fee), FormatMoney(old_fee)));
        }
        new_fee = wanted;
    }
    const CAmount extra = new_fee - old_fee;
    if (mtx.vout[dest_idx].nValue.GetAmount() <= extra) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "the withdrawn amount (%s SEQ) cannot absorb a fee increase of %s SEQ",
            FormatMoney(mtx.vout[dest_idx].nValue.GetAmount()), FormatMoney(extra)));
    }
    mtx.vout[dest_idx].nValue = mtx.vout[dest_idx].nValue.GetAmount() - extra;
    mtx.vout[fee_idx].nValue = new_fee;
    if (IsDust(mtx.vout[dest_idx], pwallet->chain().relayDustFee())) {
        throw JSONRPCError(RPC_WALLET_ERROR, "raising the fee that far would leave a dust output");
    }

    // Re-sign every staking input over the new amounts.
    for (size_t i = 0; i < mtx.vin.size(); ++i) {
        const StakeUtxo* s = nullptr;
        for (const StakeUtxo& c : spent) {
            if (c.outpoint == mtx.vin[i].prevout) { s = &c; break; }
        }
        if (!s) throw JSONRPCError(RPC_WALLET_ERROR, "the pending withdrawal spends an input this wallet cannot re-sign");
        CKey key;
        if (!GetStakerKey(*pwallet, s->parsed.pubkey, key)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("the private key for staker %s is not available in this wallet", HexStr(s->parsed.pubkey)));
        }
        FlatSigningProvider provider;
        provider.keys[s->parsed.pubkey.GetID()] = key;
        std::vector<unsigned char> sig;
        MutableTransactionSignatureCreator creator(&mtx, i, s->txout.nValue, SIGHASH_ALL);
        if (!creator.CreateSig(provider, sig, s->parsed.pubkey.GetID(), s->txout.scriptPubKey, SigVersion::BASE, /*flags=*/0)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "failed to re-sign the staking spend");
        }
        mtx.vin[i].scriptSig = CScript() << sig;
    }
    for (size_t i = 0; i < mtx.vin.size(); ++i) {
        const StakeUtxo* s = nullptr;
        for (const StakeUtxo& c : spent) if (c.outpoint == mtx.vin[i].prevout) { s = &c; break; }
        ScriptError serror = SCRIPT_ERR_OK;
        MutableTransactionSignatureChecker checker(&mtx, i, s->txout.nValue, MissingDataBehavior::FAIL);
        if (!VerifyScript(mtx.vin[i].scriptSig, s->txout.scriptPubKey, nullptr,
                          SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY,
                          checker, &serror)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("constructed an invalid replacement (%s); nothing was sent", ScriptErrorString(serror)));
        }
    }

    const CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string err_string;
    if (!pwallet->chain().broadcastTransaction(tx, pwallet->m_default_max_tx_fee, /*relay=*/true, err_string)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("failed to broadcast the replacement: %s", err_string));
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("replaced_txid", original->GetHash().GetHex());
    result.pushKV("old_fee", ValueFromAmount(old_fee));
    result.pushKV("fee", ValueFromAmount(new_fee));
    result.pushKV("amount", ValueFromAmount(tx->vout[dest_idx].nValue.GetAmount()));
    CTxDestination dest;
    if (ExtractDestination(tx->vout[dest_idx].scriptPubKey, dest)) {
        result.pushKV("destination", EncodeDestination(dest));
    }
    return result;
},
    };
}


// --- SEQUENTIA staking pools: delegate, reclaim, and watch a pool ---
//
// The consensus primitive is a delegation record (BuildDelegationScript): a
// small bare output naming a controller and a signer. While it is unspent the
// controller's whole stake weight counts for the signer, who must produce and
// sign the blocks -- and who can never spend the staked coins, because only the
// controller's key appears in the record's OP_CHECKSIG. The coins never move.
//
// What was missing was any way to USE it from a wallet: the primitive shipped
// with getdelegationscript, which hands back a script and leaves the caller to
// hand-build a transaction paying a bare output. The RPCs below are that
// missing layer -- fund a record, reclaim it, and watch what the pool you
// delegated to has committed to.

// Moved out of the anonymous namespace so the cross-chain conversion path can
// use them: a second copy of a SIGHASH is exactly the kind of thing that
// drifts, and a drifted sighash is a signature nobody can verify.
//! BIP143 (segwit v0) signature hash over the PARENT-chain transaction form.
uint256 ParentBip143Sighash(const Sidechain::Bitcoin::CMutableTransaction& tx, unsigned int in_pos,
                            const CScript& script_code, CAmount amount)
{
    CHashWriter hp(SER_GETHASH, 0), hs(SER_GETHASH, 0), ho(SER_GETHASH, 0);
    for (const auto& in : tx.vin) hp << in.prevout;
    for (const auto& in : tx.vin) hs << in.nSequence;
    for (const auto& o : tx.vout) ho << o;
    CHashWriter ss(SER_GETHASH, 0);
    ss << tx.nVersion << hp.GetHash() << hs.GetHash()
       << tx.vin[in_pos].prevout << script_code << amount << tx.vin[in_pos].nSequence
       << ho.GetHash() << tx.nLockTime << int32_t{SIGHASH_ALL};
    return ss.GetHash();
}

//! The wallet key behind a P2WPKH scriptPubKey, wherever the wallet keeps it.
//! The pubkey comes from the public solving provider; the private key through
//! the same by-pubkey accessor the staking spends use (GetStakerKey above),
//! which is the sanctioned raw-key door for descriptor wallets in this tree.
bool GetWalletKeyForP2WPKH(const CWallet& wallet, const CScript& spk, CKey& key, CPubKey& pubkey)
{
    int version = 0;
    std::vector<unsigned char> program;
    if (!spk.IsWitnessProgram(version, program) || version != 0 || program.size() != 20) return false;
    const CKeyID keyid{uint160(program)};
    const auto provider = wallet.GetSolvingProvider(spk);
    if (!provider || !provider->GetPubKey(keyid, pubkey)) return false;
    return GetStakerKey(wallet, pubkey, key);
}

namespace {

//! One of this wallet's unspent delegation records, with everything needed to
//! spend it. The record is a bare output, which IsMine does not match, so -- as
//! with staking outputs (FindWalletStakeUtxos) -- it is found by walking the
//! wallet's own transactions. delegatestake funds the record from this wallet,
//! so the funding transaction is always a wallet transaction; a record funded
//! by some other wallet toward one of our keys is out of scope, and would take
//! a full UTXO-set scan to find.
struct DelegationUtxo {
    COutPoint outpoint;
    CTxOut txout;
    CPubKey controller;
    CPubKey signer;
    CAmount amount{0};
    bool unconfirmed{false};  //!< the funding transaction is still in the mempool
    bool spending{false};     //!< a reclaim or rotation is sent but not yet confirmed
};

std::vector<DelegationUtxo> FindWalletDelegationUtxos(CWallet& wallet, const std::optional<CPubKey>& only_controller) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    // Which wallet transaction, if any, still spends an outpoint. Conflicted
    // and abandoned spenders are dead and must not count -- the same reasoning
    // (and the same replace-by-fee case) as in FindWalletStakeUtxos.
    std::multimap<COutPoint, const CWalletTx*> spenders;
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        for (const CTxIn& in : wtx.tx->vin) spenders.emplace(in.prevout, &wtx);
    }
    const auto live_spender = [&](const COutPoint& op) -> const CWalletTx* {
        const auto range = spenders.equal_range(op);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second->isAbandoned()) continue;
            if (wallet.GetTxDepthInMainChain(*it->second) < 0) continue; // replaced
            return it->second;
        }
        return nullptr;
    };

    std::vector<DelegationUtxo> found;
    for (const auto& [wtxid, wtx] : wallet.mapWallet) {
        for (uint32_t n = 0; n < wtx.tx->vout.size(); ++n) {
            const CTxOut& out = wtx.tx->vout[n];
            auto parsed = DelegationFromTxOut(out);
            if (!parsed) continue;
            if (only_controller && parsed->first != *only_controller) continue;
            // Only a record this wallet can actually spend is ours to manage.
            if (!WalletControlsStakerKey(wallet, parsed->first)) continue;
            DelegationUtxo d;
            d.outpoint = COutPoint(wtxid, n);
            if (wallet.IsSpent(wtxid, n)) {
                const CWalletTx* sp = live_spender(d.outpoint);
                if (!sp) continue;
                if (wallet.GetTxDepthInMainChain(*sp) > 0) continue; // confirmed: really gone
                d.spending = true;
            }
            d.txout = out;
            d.controller = parsed->first;
            d.signer = parsed->second;
            d.amount = out.nValue.IsExplicit() ? out.nValue.GetAmount() : 0;
            found.push_back(std::move(d));
        }
    }
    if (found.empty()) return found;

    std::map<COutPoint, Coin> coins;
    for (const DelegationUtxo& d : found) coins[d.outpoint];
    wallet.chain().findCoins(coins);
    const int tip_height = wallet.GetLastBlockHeight();

    std::vector<DelegationUtxo> live;
    for (DelegationUtxo& d : found) {
        if (d.spending) { live.push_back(std::move(d)); continue; }
        const Coin& coin = coins[d.outpoint];
        if (coin.out.IsNull()) continue; // spent
        // findCoins reports mempool outputs with the MEMPOOL_HEIGHT sentinel.
        if (coin.nHeight > (uint32_t)tip_height) d.unconfirmed = true;
        live.push_back(std::move(d));
    }
    return live;
}

//! The value to put in a delegation record by default. The record must clear
//! the dust floor to relay at all, and every re-pointing pays its fee out of
//! the record itself (a rotation is self-contained: it spends the old record
//! and creates the new one in ONE transaction, which is what keeps a block from
//! ever holding two records for one controller). So the default is the dust
//! floor plus room for ten rotations, and the record shrinks by a fee each time
//! it is re-pointed.
//! The network fee for spending a delegation record: one bare input, one output
//! and the explicit fee output, about 250 vB once the worst-case signature is
//! counted. Every reclaim and every re-pointing pays this out of the record.
CAmount DelegationSpendFee(const CWallet& wallet)
{
    CCoinControl coin_control;
    return GetMinimumFeeRate(wallet, coin_control, nullptr).GetFee(250);
}

//! The least a delegation record may hold and still be RECLAIMABLE: the dust
//! floor for its output, plus the fee to spend it.
//!
//! The dust floor alone is not enough, and on this chain is not even close: dust
//! is tens of atoms while the spend fee is thousands. A record funded between
//! the two would relay, delegate correctly, and then be impossible to reclaim,
//! because it could not pay for its own spend. That is a trap, so it is refused
//! at the point of creation rather than discovered later.
CAmount MinDelegationRecordAmount(const CWallet& wallet, const CScript& record)
{
    const CAsset& asset = Params().GetConsensus().pegged_asset;
    return GetDustThreshold(CTxOut(asset, 1, record), ::dustRelayFee) + DelegationSpendFee(wallet);
}

//! The value to put in a record by default: enough to be reclaimed, plus room
//! for ten re-pointings, since each takes its fee out of the record.
CAmount DefaultDelegationRecordAmount(const CWallet& wallet, const CScript& record)
{
    return MinDelegationRecordAmount(wallet, record) + 10 * DelegationSpendFee(wallet);
}

//! Spend delegation records back to `dest_script`, signing each with its
//! controller key, or -- when `rotate_to` is given -- re-creating the single
//! record it names pointed at a new signer.
//!
//! A rotation must share a transaction with the spend it replaces. Consensus
//! allows at most one unspent record per controller, and a block holding a
//! second one is invalid -- so "reclaim, then delegate again" as two loose
//! transactions could be mined in the wrong order and take the whole block down
//! with it. One transaction cannot be mis-ordered against itself.
CTransactionRef SpendDelegationRecords(CWallet& wallet, const std::vector<DelegationUtxo>& records,
                                       const CScript& dest_script,
                                       const CScript* rotate_to, CAmount* rotate_value_out,
                                       CAmount* fee_out, CAmount* reclaimed_out) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const CAsset& asset = Params().GetConsensus().pegged_asset;
    CAmount total = 0;
    for (const DelegationUtxo& d : records) total += d.amount;

    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = (uint32_t)wallet.GetLastBlockHeight();
    for (const DelegationUtxo& d : records) {
        CTxIn in(d.outpoint);
        in.nSequence = MAX_BIP125_RBF_SEQUENCE; // no relative lock on a record; stay replaceable
        mtx.vin.push_back(in);
    }
    // Output 0 is what the records' value becomes: the new record when
    // re-pointing, otherwise the reclaimed coins coming back to the wallet.
    mtx.vout.push_back(CTxOut(asset, total, rotate_to ? *rotate_to : dest_script));
    mtx.vout.push_back(CTxOut(asset, 0, CScript())); // explicit fee output, patched below

    CAmount fee = 0;
    {
        CMutableTransaction sizing = mtx;
        for (CTxIn& in : sizing.vin) in.scriptSig = CScript() << std::vector<unsigned char>(73);
        CCoinControl coin_control;
        fee = GetMinimumFeeRate(wallet, coin_control, nullptr).GetFee(GetVirtualTransactionSize(CTransaction(sizing)));
    }
    if (total <= fee) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "the delegation record holds %s SEQ, which does not cover the network fee (%s SEQ) to spend it",
            FormatMoney(total), FormatMoney(fee)));
    }
    const CAmount out_value = total - fee;
    // Whatever the remainder becomes -- the new record when re-pointing, the
    // coins coming home when reclaiming -- it has to clear the dust floor for
    // that script, or the transaction simply will not relay. This is reachable:
    // a record funded with exactly the dust minimum has nothing left over after
    // one fee.
    {
        const CScript& out_script = rotate_to ? *rotate_to : dest_script;
        const CAmount dust = GetDustThreshold(CTxOut(asset, out_value, out_script), ::dustRelayFee);
        if (out_value < dust) {
            throw JSONRPCError(RPC_WALLET_ERROR, rotate_to
                ? strprintf("re-pointing this delegation would leave %s SEQ in the record, below the %s SEQ the "
                            "network requires; reclaim it with undelegatestake and delegate again with a larger amount",
                            FormatMoney(out_value), FormatMoney(dust))
                : strprintf("reclaiming this delegation would return %s SEQ, below the %s SEQ the network will "
                            "relay; the record cannot pay its own fee and leave a spendable amount. The delegation "
                            "itself is unaffected, and the stake was never at risk",
                            FormatMoney(out_value), FormatMoney(dust)));
        }
    }
    mtx.vout.front().nValue = out_value;
    mtx.vout.back().nValue = fee;

    // Sign each record with its controller key. A record is a bare
    // (pre-segwit) script, so this is a legacy signature over the script itself
    // and the scriptSig is just the signature push -- the same shape as a
    // staking spend (withdrawstake).
    for (size_t i = 0; i < records.size(); ++i) {
        const DelegationUtxo& d = records[i];
        CKey key;
        if (!GetStakerKey(wallet, d.controller, key)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                "the private key for controller %s is not available in this wallet", HexStr(d.controller)));
        }
        FlatSigningProvider provider;
        provider.keys[d.controller.GetID()] = key;
        std::vector<unsigned char> sig;
        MutableTransactionSignatureCreator creator(&mtx, i, d.txout.nValue, SIGHASH_ALL);
        if (!creator.CreateSig(provider, sig, d.controller.GetID(), d.txout.scriptPubKey, SigVersion::BASE, /*flags=*/0)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "failed to sign the delegation-record spend");
        }
        mtx.vin[i].scriptSig = CScript() << sig;
    }

    // Self-check before anything leaves the wallet: every input must satisfy
    // its record script exactly as a validating node will judge it.
    for (size_t i = 0; i < records.size(); ++i) {
        ScriptError serror = SCRIPT_ERR_OK;
        MutableTransactionSignatureChecker checker(&mtx, i, records[i].txout.nValue, MissingDataBehavior::FAIL);
        if (!VerifyScript(mtx.vin[i].scriptSig, records[i].txout.scriptPubKey, nullptr,
                          SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY,
                          checker, &serror)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                "constructed an invalid delegation-record spend (%s); nothing was sent", ScriptErrorString(serror)));
        }
    }

    const CTransactionRef tx = MakeTransactionRef(std::move(mtx));

    // Record it in the wallet BEFORE broadcasting. A re-pointing pays nothing
    // the wallet recognises as its own -- its only outputs are the new bare
    // record and the fee -- so nothing would ever add it, and the wallet would
    // lose sight of the very record it just created: unable to re-point it
    // again, and unable to reclaim the coins in it. Adding it first also means
    // a broadcast that fails after the transaction has already propagated
    // leaves the record tracked rather than orphaned, which is the safe way for
    // that race to go.
    wallet.AddToWallet(tx, TxStateInactive{}, [](CWalletTx& wtx, bool /*new_tx*/) {
        wtx.fTimeReceivedIsTxTime = true;
        wtx.fFromMe = true;
        return true;
    });

    std::string err_string;
    if (!wallet.chain().broadcastTransaction(tx, wallet.m_default_max_tx_fee, /*relay=*/true, err_string)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("failed to broadcast the delegation change: %s", err_string));
    }
    if (fee_out) *fee_out = fee;
    if (rotate_value_out) *rotate_value_out = rotate_to ? out_value : 0;
    if (reclaimed_out) *reclaimed_out = rotate_to ? 0 : out_value;
    return tx;
}

//! The staker keys this wallet controls that have stake registered, in a stable
//! order. These are the keys worth delegating -- a controller with no stake can
//! hold a record, but it lends nothing.
std::vector<CPubKey> WalletStakerKeys(CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    std::set<CPubKey> keys;
    for (const StakeUtxo& s : FindWalletStakeUtxos(wallet, std::nullopt)) {
        if (!s.withdrawing) keys.insert(s.parsed.pubkey);
    }
    return std::vector<CPubKey>(keys.begin(), keys.end());
}

} // namespace

RPCHelpMan delegatestake()
{
    return RPCHelpMan{"delegatestake",
                "\nSEQUENTIA staking pools: lend this wallet's stake weight to a signer -- a pool operator, or\n"
                "this staker's own online key -- WITHOUT moving the staked coins. This funds a delegation record\n"
                "(see getdelegationscript) naming the controller and the signer. While it is unspent the\n"
                "controller's whole stake weight counts for the signer, who must produce and sign the blocks.\n"
                "\nThe signer can NEVER spend the staked coins: only the controller's key appears in the record's\n"
                "signature check, and the staking outputs are not touched at all. Because the record is a separate\n"
                "output, this works even while the stake itself is frozen by a vesting lock, and it lets the key\n"
                "that can move the coins stay offline while a hot key produces blocks.\n"
                "\nCalling this again with a different signer RE-POINTS the delegation: the old record is spent and\n"
                "the new one created in the SAME transaction, which is what stops a block from ever holding two\n"
                "records for one controller. The re-pointing fee comes out of the record. Reclaim the rights (and\n"
                "the record's coins) at any time, unilaterally, with undelegatestake.\n"
                "\nYOU DO NOT NEED ENOUGH TO STAKE ALONE. Pass `amount` and this bonds that much SEQ and lends it\n"
                "in one transaction, with NO minimum: the chain applies its minimum-stake floor to what a SIGNER\n"
                "commands in total, after delegation is resolved, not to each delegator separately. Pooling small\n"
                "holdings is precisely what that arithmetic is for, and it is why delegation exists. Omit `amount`\n"
                "to lend stake this wallet has already bonded instead.\n"
                "\nStaking alone is the other path, and the one with a floor: registerstake refuses an amount that\n"
                "could never win a block by itself. Delegating has no such limit.\n"
                "\nCheck what a pool has committed to before delegating, and watch it afterwards: see listpools\n"
                "and listdelegations. A pool's payout policy cannot change without a notice period.\n",
                {
                    {"signer", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The public key that will produce blocks with this stake's weight (hex). Pool operators publish theirs; the pool board lists them."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "SEQ to bond and lend, in one transaction. No minimum: delegated weight counts toward the pool's total, not your own. Omit to lend stake this wallet has already bonded."},
                    {"controller", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The staker public key to bond to, or whose already-bonded weight to lend (hex). Defaults to a fresh key when `amount` is given, and to this wallet's staker key when it has exactly one."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the transaction that funded (or re-pointed) the record"},
                    {RPCResult::Type::STR_HEX, "controller", "the staker key whose weight is now lent"},
                    {RPCResult::Type::STR_HEX, "signer", "the key that now produces blocks with it"},
                    {RPCResult::Type::STR_HEX, "previous_signer", /*optional=*/true, "the signer this replaced, when re-pointing"},
                    {RPCResult::Type::STR_AMOUNT, "staked", /*optional=*/true, "SEQ bonded by this call, when `amount` was given"},
                    {RPCResult::Type::STR_AMOUNT, "record_amount", "SEQ held in the delegation record"},
                    {RPCResult::Type::STR_AMOUNT, "fee", /*optional=*/true, "the network fee taken out of the record, when re-pointing"},
                    {RPCResult::Type::NUM, "delegated_weight", "stake weight this lends to the signer, once the record confirms (atoms)"},
                    {RPCResult::Type::STR, "note", /*optional=*/true, "anything worth knowing about what was just done"},
                }},
                RPCExamples{HelpExampleCli("delegatestake", "\"02bb...\" 1000")
                            + HelpExampleCli("delegatestake", "\"02bb...\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    CPubKey signer(ParseHexV(request.params[0], "signer"));
    if (!signer.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid signer public key");

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    // 1) Which staker key is being delegated. Only a key this wallet controls
    //    can be a controller: the controller is the only key that can ever
    //    reclaim the record, so delegating on behalf of a key we do not hold
    //    would hand the rights away irrevocably.
    // `amount` given means "bond this much and lend it", which is the path for
    // anyone who does not hold enough to stake alone -- most delegators, by
    // definition.
    std::optional<CAmount> stake_amount;
    if (!request.params[1].isNull()) {
        stake_amount = AmountFromValue(request.params[1], true);
        if (*stake_amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "amount must be positive");
    }

    CPubKey controller;
    if (!request.params[2].isNull()) {
        controller = CPubKey(ParseHexV(request.params[2], "controller"));
        if (!controller.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid controller public key");
        if (!WalletControlsStakerKey(*pwallet, controller)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                "this wallet does not hold the private key for controller %s; only the controller can reclaim a "
                "delegation, so delegating on its behalf would give the rights away for good", HexStr(controller)));
        }
    } else if (stake_amount) {
        // Bonding fresh: a key of this wallet's own, so only this wallet can
        // ever unbond it or take the delegation back.
        CTxDestination dest;
        bilingual_str dest_error;
        if (!pwallet->GetNewDestination(pwallet->m_default_address_type, "", dest, dest_error)) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, dest_error.original);
        }
        CKeyID keyid;
        if (const PKHash* pkh = std::get_if<PKHash>(&dest)) keyid = ToKeyID(*pkh);
        else if (const WitnessV0KeyHash* wpkh = std::get_if<WitnessV0KeyHash>(&dest)) keyid = ToKeyID(*wpkh);
        // The staking script names a PUBLIC key, so the destination has to be a
        // plain key one; the wallet's solving provider is what maps it back.
        std::unique_ptr<SigningProvider> provider = pwallet->GetSolvingProvider(GetScriptForDestination(dest));
        if (keyid.IsNull() || !provider || !provider->GetPubKey(keyid, controller)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                "could not derive a staking key from this wallet; pass `controller` with a key it holds");
        }
    } else {
        const std::vector<CPubKey> keys = WalletStakerKeys(*pwallet);
        if (keys.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                "this wallet has nothing staked to lend. Pass `amount` to bond some SEQ and delegate it in one "
                "step -- there is no minimum for that, because a pool's eligibility is judged on what it "
                "commands in total, not on what each delegator brings.");
        }
        if (keys.size() > 1) {
            std::string list;
            for (const CPubKey& k : keys) list += (list.empty() ? "" : ", ") + HexStr(k);
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "this wallet stakes with more than one key, so `controller` is required. Its staker keys are: %s", list));
        }
        controller = keys.front();
    }

    if (signer == controller) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "delegating to the controller itself is what already happens with no record at all; to stop "
            "delegating, reclaim the record with undelegatestake");
    }

    // 2) An existing record for this controller must be re-pointed, never
    //    duplicated: consensus permits at most one unspent record per
    //    controller (two would make the leader election node-dependent), and a
    //    block containing a second one is invalid.
    StakeRegistry& registry = StakeRegistry::GetInstance();
    std::vector<DelegationUtxo> existing = FindWalletDelegationUtxos(*pwallet, controller);
    const DelegationUtxo* live_record = nullptr;
    for (const DelegationUtxo& d : existing) {
        if (d.spending) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                "a change to this delegation has already been sent and is waiting to confirm; wait for it "
                "before re-pointing %s again", HexStr(controller)));
        }
        live_record = &d;
    }
    if (!live_record && registry.HasDelegation(controller)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "controller %s already has a delegation record (to signer %s) that this wallet did not fund and "
            "cannot find, so it cannot be re-pointed from here. Spend that record first; a second record for "
            "the same controller would make any block containing it invalid.",
            HexStr(controller), HexStr(registry.SignerFor(controller))));
    }
    if (live_record && live_record->signer == signer) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
            "controller %s already delegates to signer %s", HexStr(controller), HexStr(signer)));
    }

    const CScript record_script = BuildDelegationScript(controller, signer);
    const std::map<CPubKey, uint64_t> controller_weights = registry.ControllerWeights();
    const auto weight_it = controller_weights.find(controller);
    const uint64_t weight = weight_it == controller_weights.end() ? 0 : weight_it->second;

    UniValue result(UniValue::VOBJ);
    std::string note;

    if (live_record) {
        // 3a) Re-point: spend the old record and create the new one in one
        //     transaction. The fee comes out of the record's own value.
        //
        //     Which means `amount` has nothing to act on here. Silently ignoring
        //     it would let someone believe they had topped the record up, so
        //     refuse instead and say where the value actually comes from.
        if (stake_amount) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "amount cannot be set when moving an existing delegation to another pool: the new record is "
                "funded by the old one, and the stake itself is not touched. Controller %s already lends its "
                "weight. To bond MORE and lend that too, delegate again with a different controller key.",
                HexStr(controller)));
        }
        const CPubKey previous = live_record->signer;
        const std::vector<DelegationUtxo> spend_these{*live_record};
        CAmount fee = 0, new_value = 0;
        const CTransactionRef tx = SpendDelegationRecords(*pwallet, spend_these, CScript(), &record_script,
                                                          &new_value, &fee, nullptr);
        result.pushKV("txid", tx->GetHash().GetHex());
        result.pushKV("previous_signer", HexStr(previous));
        result.pushKV("record_amount", ValueFromAmount(new_value));
        result.pushKV("fee", ValueFromAmount(fee));
        note = "the delegation moves the moment this transaction confirms; the old signer stops commanding "
               "this weight at that same confirmation";
    } else {
        // 3b) First delegation for this controller. The record is a small bare
        //     output the wallet funds; when `amount` was given, the staking
        //     output it lends is funded by the SAME transaction, so bonding and
        //     lending are one step and one confirmation rather than two.
        const CAsset& asset = Params().GetConsensus().pegged_asset;
        const CAmount record_amount = DefaultDelegationRecordAmount(*pwallet, record_script);
        std::vector<CRecipient> recipients;

        if (stake_amount) {
            // Deliberately NO minimum-stake check. registerstake refuses a
            // sub-floor amount because a stake that small could never win a
            // block on its own -- but this one is not on its own. Consensus
            // applies the floor to what a SIGNER commands once delegation is
            // resolved (PosIsEligibleStake over registry.Weights()), so a small
            // holding pooled behind an eligible signer counts in full. Refusing
            // it here would deny delegation to exactly the people it is for.
            const uint32_t csv = (uint32_t)g_pos_unbonding_period;
            const auto lock = PosStakeLockSeconds(csv);
            const int64_t required = PosRequiredUnbondingSeconds();
            if (!lock || *lock < required) {
                throw JSONRPCError(RPC_MISC_ERROR, strprintf(
                    "this chain's default unbonding delay (%d blocks) does not meet its own minimum (%d s); "
                    "bond with registerstake and pass csv_seconds, then delegate without an amount",
                    (int)csv, (int)required));
            }
            const CScript stake_script = BuildStakeScript(controller, csv, {}, {}, 0);
            const CAmount stake_dust = GetDustThreshold(CTxOut(asset, *stake_amount, stake_script), ::dustRelayFee);
            if (*stake_amount < stake_dust) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "amount is below the %s SEQ dust floor; the network would not relay it", FormatMoney(stake_dust)));
            }
            recipients.push_back({stake_script, *stake_amount, asset, CPubKey(), false});
        }
        recipients.push_back({record_script, record_amount, asset, CPubKey(), false});

        CCoinControl coin_control;
        mapValue_t mapValue;
        UniValue txid = SendMoney(*pwallet, coin_control, recipients, mapValue, /*verbose=*/false, /*ignore_blind_fail=*/true);
        result.pushKV("txid", txid);
        if (stake_amount) result.pushKV("staked", ValueFromAmount(*stake_amount));
        result.pushKV("record_amount", ValueFromAmount(record_amount));
    }

    result.pushKV("controller", HexStr(controller));
    result.pushKV("signer", HexStr(signer));
    // What this lends once the transaction confirms: weight already registered
    // to the controller, plus anything this call is bonding.
    result.pushKV("delegated_weight", weight + (stake_amount ? (uint64_t)*stake_amount : 0));
    if (stake_amount) {
        if (!note.empty()) note += ". Also: ";
        note += "the stake and the delegation are in one transaction, so this weight counts for the pool from "
                "the block that confirms it. Your coins do not move again: leaving spends only the record";
    } else if (weight == 0) {
        // Both notes can apply at once (re-pointing a key whose stake has since
        // been withdrawn), and the one about lending nothing is the one the
        // caller most needs, so add rather than replace.
        if (!note.empty()) note += ". Also: ";
        note += "this controller has no registered stake, so the record lends nothing yet; it will lend whatever "
                "is staked to this key from the moment it is registered";
    }
    if (!note.empty()) result.pushKV("note", note);
    return result;
},
    };
}

RPCHelpMan undelegatestake()
{
    return RPCHelpMan{"undelegatestake",
                "\nSEQUENTIA staking pools: stop delegating -- reclaim this wallet's block-signing rights from the\n"
                "signer they were lent to, and the coins held in the delegation record with them. Spending the\n"
                "record is unilateral and needs nobody's cooperation: only the controller's key can spend it, and\n"
                "there is no lock and no notice period on leaving. From that confirmation on, the controller's\n"
                "stake weight counts for the controller itself again.\n"
                "\nThis does NOT unstake. The staked coins were never moved by delegating and are not moved by\n"
                "reclaiming; use withdrawstake to unstake.\n",
                {
                    {"controller", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Reclaim only this staker key's delegation (hex). Default: every delegation this wallet holds."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Where the record's coins go (default: a fresh address of this wallet). The output is explicit, not confidential."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the reclaim transaction id"},
                    {RPCResult::Type::STR_AMOUNT, "amount", "SEQ arriving at the destination (the records' value minus the fee)"},
                    {RPCResult::Type::STR_AMOUNT, "fee", "the network fee, paid out of the reclaimed coins"},
                    {RPCResult::Type::STR, "destination", "the receiving address"},
                    {RPCResult::Type::ARR, "reclaimed", "the delegations this ends", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "controller", "the staker key taking its rights back"},
                            {RPCResult::Type::STR_HEX, "signer", "the signer losing them"},
                            {RPCResult::Type::NUM, "weight", "stake weight returning to the controller (atoms)"},
                        }},
                    }},
                }},
                RPCExamples{HelpExampleCli("undelegatestake", "") + HelpExampleCli("undelegatestake", "\"02aa...\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    std::optional<CPubKey> only;
    if (!request.params[0].isNull()) {
        CPubKey pk(ParseHexV(request.params[0], "controller"));
        if (!pk.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid controller public key");
        only = pk;
    }

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    const std::vector<DelegationUtxo> all = FindWalletDelegationUtxos(*pwallet, only);
    std::vector<DelegationUtxo> records;
    bool any_pending = false, any_unconfirmed = false;
    for (const DelegationUtxo& d : all) {
        if (d.spending) { any_pending = true; continue; }
        // Nothing to reclaim from a record that has not confirmed: the
        // delegation is not in force until its funding block lands.
        if (d.unconfirmed) { any_unconfirmed = true; continue; }
        records.push_back(d);
    }
    if (records.empty()) {
        if (any_pending) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                "a change to this wallet's delegations has already been sent and is waiting to confirm");
        }
        if (any_unconfirmed) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                "the delegation is still waiting to confirm; it is not in force yet, and can be reclaimed "
                "once it is");
        }
        throw JSONRPCError(RPC_WALLET_ERROR, only
            ? strprintf("this wallet holds no delegation for controller %s", HexStr(*only))
            : std::string("this wallet is not delegating any stake (see delegatestake)"));
    }

    CTxDestination dest;
    if (!request.params[1].isNull()) {
        dest = DecodeDestination(request.params[1].get_str());
        if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    } else {
        bilingual_str dest_error;
        if (!pwallet->GetNewDestination(pwallet->m_default_address_type, "", dest, dest_error)) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, dest_error.original);
        }
    }
    std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
    const CScript dest_script = GetScriptForDestination(dest);

    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const std::map<CPubKey, uint64_t> controller_weights = registry.ControllerWeights();

    CAmount fee = 0, reclaimed = 0;
    const CTransactionRef tx = SpendDelegationRecords(*pwallet, records, dest_script, nullptr,
                                                      nullptr, &fee, &reclaimed);

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount", ValueFromAmount(reclaimed));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("destination", EncodeDestination(dest));
    UniValue arr(UniValue::VARR);
    for (const DelegationUtxo& d : records) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("controller", HexStr(d.controller));
        o.pushKV("signer", HexStr(d.signer));
        const auto it = controller_weights.find(d.controller);
        o.pushKV("weight", it == controller_weights.end() ? (uint64_t)0 : it->second);
        arr.push_back(o);
    }
    result.pushKV("reclaimed", arr);
    return result;
},
    };
}


RPCHelpMan announcepayout()
{
    return RPCHelpMan{"announcepayout",
                "\nSEQUENTIA staking pools, operator side: commit on-chain to how the blocks this wallet's signer\n"
                "produces will pay out. This funds a payout record (see getpayoutscript) that binds the signer\n"
                "from `activation` onward, and which anyone can read with listpools or getpayoutinfo.\n"
                "\nMODES:\n"
                "  direct  - every block's coinbase must pay a committed script: an address, or raw bytes via\n"
                "            `payout_script` when the arrangement is something an address cannot express. This stops the operator\n"
                "            redirecting the reward silently; it does NOT make the chain check that the address\n"
                "            shares anything with delegators. Trust-minimised, not trustless.\n"
                "  split   - every block's coinbase pays the pool's POT, and anyone may broadcast a CLAIM that\n"
                "            distributes the pot to every delegator in exact proportion to the weight each had\n"
                "            lent when the reward was earned (see claimpoolrewards). Shares too small to pay roll\n"
                "            into the next claim instead of being rounded away. Commission is a bp/10000 chance,\n"
                "            drawn from Bitcoin's proof of work, that a block pays the operator instead: exact in\n"
                "            expectation. The proportional payout most delegators expect.\n"
                "  lottery - every block's coinbase must pay ONE delegator, drawn by stake weight from a seed\n"
                "            derived from Bitcoin's proof of work, so the draw cannot be biased. Each delegator\n"
                "            earns its exact proportional share over time with no accounting at all -- but in\n"
                "            rare lumps, not smoothed: 1% of a pool is 100% of one block in a hundred.\n"
                "            `commission_bp` is the share of blocks the operator keeps, in basis points.\n"
                "\nA policy cannot bind before the chain's notice period has passed since the block that ANNOUNCES\n"
                "it, which is what gives delegators time to audit the change and leave. Announcing does not cancel\n"
                "an earlier policy: the one in force at a height is the announced policy with the greatest\n"
                "activation at or below it, so a pending change and the current rule coexist until the switch.\n"
                "\nThe record is spendable by the signer, so this wallet must hold the signer's key -- otherwise the\n"
                "announcement could never be withdrawn.\n",
                {
                    {"mode", RPCArg::Type::STR, RPCArg::Optional::NO, "\"direct\", \"lottery\" or \"split\"."},
                    {"signer", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The block-producing public key this binds (hex). Defaults to this wallet's staker key when it has exactly one."},
                    {"activation", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Block height from which the policy binds (default: comfortably past the notice period)."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "direct mode: the address every coinbase must pay (default: a fresh address of this wallet)."},
                    {"commission_bp", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "lottery or split mode: basis points of blocks the operator keeps (0..10000, default 0). At 0 the operator still earns on its own stake, as one participant among the rest."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "SEQ to put in the payout record (default: just over the dust floor). Recoverable by spending the record with the signer key."},
                    {"payout_script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "direct mode: the exact scriptPubKey every coinbase must pay (hex, 1..110 bytes), instead of `address`. For committing to something an address cannot express -- a multisig, a covenant, a contract that splits the reward. The chain compares the coinbase output against these bytes and nothing else, so it is on you that they are spendable by whoever should receive them."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the announcement transaction id"},
                    {RPCResult::Type::STR_HEX, "signer", "the signer the policy binds"},
                    {RPCResult::Type::STR, "mode", "\"direct\", \"lottery\" or \"split\""},
                    {RPCResult::Type::NUM, "activation", "height from which it binds"},
                    {RPCResult::Type::NUM, "notice_blocks", "the chain's minimum notice period, in blocks"},
                    {RPCResult::Type::NUM, "earliest_activation", "the lowest activation this announcement could have used, if it confirms in the next block"},
                    {RPCResult::Type::STR, "address", /*optional=*/true, "direct: the committed payee, when the script corresponds to an address"},
                    {RPCResult::Type::STR_HEX, "payout_script", /*optional=*/true, "direct: the exact bytes every coinbase must pay"},
                    {RPCResult::Type::NUM, "commission_bp", /*optional=*/true, "lottery: the operator's basis points"},
                    {RPCResult::Type::STR_HEX, "script", "the payout-record scriptPubKey that was funded"},
                    {RPCResult::Type::STR, "note", "what delegators will see, and when"},
                }},
                RPCExamples{HelpExampleCli("announcepayout", "\"lottery\"") + HelpExampleCli("announcepayout", "\"lottery\" \"02bb...\" null null 500")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    const std::string mode = request.params[0].get_str();
    if (mode != "direct" && mode != "lottery" && mode != "split") {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "mode must be \"direct\", \"lottery\" or \"split\"");
    }

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    // The signer whose blocks this binds. The record is spendable by the signer
    // alone, so a wallet that does not hold that key could announce a policy it
    // could never withdraw.
    CPubKey signer;
    if (!request.params[1].isNull()) {
        signer = CPubKey(ParseHexV(request.params[1], "signer"));
        if (!signer.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid signer public key");
    } else {
        const std::vector<CPubKey> keys = WalletStakerKeys(*pwallet);
        if (keys.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                "this wallet has no staking outputs, so it has no signer key to announce for; pass `signer`");
        }
        if (keys.size() > 1) {
            std::string list;
            for (const CPubKey& k : keys) list += (list.empty() ? "" : ", ") + HexStr(k);
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "this wallet stakes with more than one key, so `signer` is required. Its staker keys are: %s", list));
        }
        signer = keys.front();
    }
    if (!WalletControlsStakerKey(*pwallet, signer)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "this wallet does not hold the private key for signer %s; only that key can ever spend the payout "
            "record, so the announcement could never be withdrawn", HexStr(signer)));
    }

    const int tip_height = pwallet->GetLastBlockHeight();
    // Consensus measures the notice period from the block that CONFIRMS the
    // announcement, which is at best the next one. Anything below this is
    // certain to be rejected as bad-payout-notice.
    const int64_t earliest = (int64_t)tip_height + 1 + (int64_t)g_pos_payout_notice;

    PosPayoutPolicy policy;
    if (!request.params[2].isNull()) {
        policy.activation = request.params[2].get_int64();
        if (policy.activation <= 0 || policy.activation > 0xffffffffLL) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "activation must be between 1 and 4294967295");
        }
        if (policy.activation < earliest) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "activation %d is inside the notice period: this chain requires %d blocks between the block that "
                "announces a policy and the block it binds at, so the earliest this announcement could use is %d. "
                "The notice is what gives delegators time to audit the change and leave.",
                (int)policy.activation, (int)g_pos_payout_notice, (int)earliest));
        }
    } else {
        // A margin over the floor, because the announcement may wait in the
        // mempool: every block it waits pushes its own announce height up.
        policy.activation = earliest + 10;
    }

    std::string address_str;
    if (mode == "direct") {
        policy.mode = PosPayoutMode::DIRECT;
        const bool has_script = !request.params[6].isNull();
        if (has_script && !request.params[3].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "give either address or payout_script, not both: they would commit to different bytes");
        }
        if (has_script) {
            // The raw form. Consensus compares the coinbase output against these
            // bytes and asks nothing else of them, so anything expressible as a
            // scriptPubKey is a valid commitment -- a multisig, a covenant, a
            // contract that splits the reward among several parties. It is
            // equally on the operator that they are spendable at all: a script
            // nobody can satisfy burns every reward this key ever earns, and the
            // chain will enforce that just as faithfully.
            const std::vector<unsigned char> raw = ParseHexV(request.params[6], "payout_script");
            if (raw.empty() || raw.size() > 110) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "payout_script must be 1..110 bytes");
            }
            policy.script = CScript(raw.begin(), raw.end());
            CTxDestination parsed;
            if (ExtractDestination(policy.script, parsed)) address_str = EncodeDestination(parsed);
        } else {
            CTxDestination dest;
            if (!request.params[3].isNull()) {
                dest = DecodeDestination(request.params[3].get_str());
                if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            } else {
                bilingual_str dest_error;
                if (!pwallet->GetNewDestination(pwallet->m_default_address_type, "", dest, dest_error)) {
                    throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, dest_error.original);
                }
            }
            // The coinbase output the chain will compare against is explicit, so
            // commit to the unconfidential form of the address.
            std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
            policy.script = GetScriptForDestination(dest);
            address_str = EncodeDestination(dest);
            if (policy.script.empty() || policy.script.size() > 110) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "the committed script must be 1..110 bytes");
            }
        }
        if (!request.params[4].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "commission_bp applies to lottery mode only");
        }
    } else {
        if (!request.params[6].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "payout_script applies to direct mode only; a lottery pays whichever delegator the draw picks");
        }
        if (!request.params[3].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "address applies to direct mode only; a lottery pays whichever delegator the draw picks");
        }
        if (mode == "split") {
            // The mode is behind a flag day: a node without it treats a split
            // record as an inert output and would accept a coinbase the new
            // rule rejects. Announcing before the flag would create a record
            // this chain does not recognise yet.
            if (g_split_payout_height > 0 && tip_height + 1 < g_split_payout_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "the split payout mode activates at height %d on this chain (the tip is %d); announce it "
                    "after that, or use lottery until then", g_split_payout_height, tip_height));
            }
            policy.mode = PosPayoutMode::SPLIT;
        } else {
            policy.mode = PosPayoutMode::LOTTERY;
        }
        const int64_t bp = request.params[4].isNull() ? 0 : request.params[4].get_int64();
        if (bp < 0 || bp > (int64_t)POS_COMMISSION_DENOM) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "commission_bp must be between 0 and 10000");
        }
        policy.commission_bp = (uint32_t)bp;
    }

    // Consensus forbids two records sharing a (signer, activation): the second
    // would be rejected as bad-payout-exists, taking its block down with it.
    StakeRegistry& registry = StakeRegistry::GetInstance();
    if (registry.HasPayoutAt(signer, policy.activation)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
            "signer %s has already announced a policy activating at height %d; pick another activation",
            HexStr(signer), (int)policy.activation));
    }

    const CScript record_script = BuildPayoutScript(signer, policy);
    const CAsset& asset = Params().GetConsensus().pegged_asset;
    CAmount amount;
    if (!request.params[5].isNull()) {
        amount = AmountFromValue(request.params[5], true);
        if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "amount must be positive");
        const CAmount dust = GetDustThreshold(CTxOut(asset, amount, record_script), ::dustRelayFee);
        if (amount < dust) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "amount is below the %s SEQ dust floor for a payout record; the network would not relay it",
                FormatMoney(dust)));
        }
    } else {
        amount = 2 * GetDustThreshold(CTxOut(asset, 1, record_script), ::dustRelayFee);
    }

    CRecipient recipient = {record_script, amount, asset, CPubKey(), false};
    std::vector<CRecipient> recipients = {recipient};
    CCoinControl coin_control;
    mapValue_t mapValue;
    UniValue txid = SendMoney(*pwallet, coin_control, recipients, mapValue, /*verbose=*/false, /*ignore_blind_fail=*/true);

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid);
    result.pushKV("signer", HexStr(signer));
    result.pushKV("mode", mode);
    result.pushKV("activation", policy.activation);
    result.pushKV("notice_blocks", (int64_t)g_pos_payout_notice);
    result.pushKV("earliest_activation", earliest);
    if (mode == "direct") {
        // A raw script need not correspond to any address, so report the bytes
        // always and the address only when one exists.
        result.pushKV("payout_script", HexStr(policy.script));
        if (!address_str.empty()) result.pushKV("address", address_str);
    }
    else result.pushKV("commission_bp", (int64_t)policy.commission_bp);
    result.pushKV("script", HexStr(record_script));
    result.pushKV("note", strprintf(
        "delegators see this as a pending policy from the moment it confirms until it binds at height %d, and can "
        "leave at any time until then (and after). It does not replace the current policy before that height.",
        (int)policy.activation));
    return result;
},
    };
}

RPCHelpMan claimpoolrewards()
{
    return RPCHelpMan{"claimpoolrewards",
                "\nSEQUENTIA split payouts: sweep a pool's POT and pay every delegator its exact share, in one\n"
                "transaction. Anyone may do this for any pool -- the distribution is fully determined by the\n"
                "chain, so there is nothing to trust the claimer with, and nobody's payout depends on the\n"
                "operator staying interested. The network fee comes out of the pot, capped (with any claimer's\n"
                "margin) at 1/99 of what the delegators receive; whatever margin the fee does not use is paid to\n"
                "this wallet, which is the incentive to be the one who claims.\n"
                "\nEach delegator is paid only from pot outputs created after its delegation (and its stake) "
                "existed, so joining a pool just before a claim earns exactly nothing from it. Shares below the\n"
                "minimum payout roll into a fresh pot and accumulate for the next claim.\n",
                {
                    {"signer", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The pool to claim for (hex). Defaults to the pool this wallet's stake is delegated to."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "the claim transaction id"},
                    {RPCResult::Type::NUM, "pot_outputs", "pot outputs swept"},
                    {RPCResult::Type::OBJ_DYN, "distributed", "paid to delegators, keyed by asset id", {
                        {RPCResult::Type::STR_AMOUNT, "asset", "amount"}}},
                    {RPCResult::Type::OBJ_DYN, "repotted", "rolled into the fresh pot (sub-minimum shares and remainders), keyed by asset id", {
                        {RPCResult::Type::STR_AMOUNT, "asset", "amount"}}},
                    {RPCResult::Type::STR_AMOUNT, "fee", "the network fee, paid out of the pot"},
                    {RPCResult::Type::STR_AMOUNT, "margin", "the claimer's cut, paid to this wallet"},
                    {RPCResult::Type::NUM, "delegators_paid", "how many delegators received a payout"},
                }},
                RPCExamples{HelpExampleCli("claimpoolrewards", "") + HelpExampleCli("claimpoolrewards", "\"02bb...\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    StakeRegistry& registry = StakeRegistry::GetInstance();

    LOCK(pwallet->cs_wallet);

    // 1) Which pool. Explicit, or the one this wallet's stake is lent to.
    CPubKey signer;
    if (!request.params[0].isNull()) {
        signer = CPubKey(ParseHexV(request.params[0], "signer"));
        if (!signer.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid signer public key");
    } else {
        const std::vector<CPubKey> keys = WalletStakerKeys(*pwallet);
        for (const CPubKey& k : keys) {
            const CPubKey s = registry.SignerFor(k);
            if (s != k) { signer = s; break; }
        }
        if (!signer.IsFullyValid()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "this wallet's stake is not delegated to a pool; pass the pool's signer key explicitly");
        }
    }

    // 2) The pot, and what a claim of it must pay. Both are pure functions of
    //    the UTXO set, computed by the SAME code every validator runs.
    const std::map<COutPoint, PosPotRef> all_pots = registry.PotsFor(signer);
    if (all_pots.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "pool %s has no pot to claim: either it is not a split pool, or everything accrued has already "
            "been distributed", HexStr(signer)));
    }
    // Pot outputs created by a coinbase are subject to coinbase maturity, like
    // any other coinbase value: a claim can only sweep rewards at least
    // COINBASE_MATURITY blocks deep. (A previous claim's own re-pot output is
    // an ordinary transaction output and sweeps immediately, but sweeping it
    // alone rarely clears the fee cap, so immature pots are simply left for
    // the next claim rather than special-cased.)
    const int tip_for_maturity = pwallet->GetLastBlockHeight();
    std::map<COutPoint, PosPotRef> pots;
    int immature = 0;
    for (const auto& e : all_pots) {
        if (e.second.height > 0 && tip_for_maturity - e.second.height + 1 < COINBASE_MATURITY) {
            ++immature;
            continue;
        }
        pots[e.first] = e.second;
    }
    if (pots.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "pool %s has %d pot output(s), all still inside coinbase maturity (%d blocks); claim once they mature",
            HexStr(signer), immature, COINBASE_MATURITY));
    }
    std::vector<std::tuple<CAsset, int64_t, int>> pot_inputs;
    for (const auto& e : pots) pot_inputs.emplace_back(e.second.asset, e.second.value, e.second.height);
    const PosPotShares shares = PosComputePotShares(signer, pot_inputs);

    // 3) Assemble. Inputs: every pot output (empty scriptSig satisfies the pot
    //    script; the consensus overlay is the whole spend condition). Outputs:
    //    each delegator's exact share where it clears the minimum, a fresh pot
    //    per asset for what does not, the fee, and this wallet's margin.
    const int tip_height = pwallet->GetLastBlockHeight();
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = (uint32_t)tip_height;
    for (const auto& e : pots) {
        CTxIn in(e.first);
        in.nSequence = MAX_BIP125_RBF_SEQUENCE;
        mtx.vin.push_back(in);
    }

    std::map<CAsset, int64_t> paid, repot;
    int64_t delegators_paid = 0;
    for (const auto& d : shares.owed) {
        for (const auto& a : d.second) {
            if (a.second < POS_SPLIT_MIN_PAYOUT) continue;
            mtx.vout.push_back(CTxOut(a.first, a.second,
                                      GetScriptForDestination(WitnessV0KeyHash(d.first.GetID()))));
            paid[a.first] += a.second;
            ++delegators_paid;
        }
    }
    if (paid.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "pool %s has accrued %d pot output(s), but no delegator's share clears the minimum payout yet; "
            "claim again once more has accrued", HexStr(signer), (int)pots.size()));
    }

    // The fee is paid in the swept asset with the largest distribution, which
    // maximises the room under the withhold cap. (On this chain any accepted
    // asset can pay a fee: the open fee market.)
    CAsset fee_asset;
    int64_t fee_room = -1;
    for (const auto& e : paid) {
        const int64_t room = e.second / POS_SPLIT_WITHHOLD_RATIO;
        if (room > fee_room) { fee_room = room; fee_asset = e.first; }
    }

    // Fresh pots first (so sizing includes them), fee + margin after.
    const CScript pot_script = BuildPotScript(signer);
    for (const auto& sw : shares.swept) {
        const int64_t leftover = sw.second - (paid.count(sw.first) ? paid.at(sw.first) : 0);
        if (leftover > 0 && sw.first != fee_asset) {
            mtx.vout.push_back(CTxOut(sw.first, leftover, pot_script));
            repot[sw.first] += leftover;
        }
    }

    // Size the fee against the finished shape: everything above, plus at most a
    // repot output, a margin output and the fee output in the fee asset.
    CCoinControl coin_control;
    const CAmount fee_rate_estimate = GetMinimumFeeRate(*pwallet, coin_control, nullptr)
                                          .GetFee(GetVirtualTransactionSize(CTransaction(mtx)) + 3 * 70);
    if (fee_rate_estimate > fee_room) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "the pot is not yet worth claiming: the network fee (%s) would exceed 1/%d of what the delegators "
            "would receive (%s). Claim again once more has accrued.",
            FormatMoney(fee_rate_estimate), (int)POS_SPLIT_WITHHOLD_RATIO, FormatMoney(fee_room)));
    }

    // This wallet's margin: whatever the cap allows beyond the fee. Below the
    // dust floor it is not worth an output; it stays in the pot.
    bilingual_str dest_error;
    CTxDestination margin_dest;
    if (!pwallet->GetNewDestination(pwallet->m_default_address_type, "", margin_dest, dest_error)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, dest_error.original);
    }
    std::visit(SetBlindingPubKeyVisitor(CPubKey()), margin_dest);
    const CScript margin_script = GetScriptForDestination(margin_dest);
    int64_t margin = fee_room - fee_rate_estimate;
    const CAmount margin_dust = GetDustThreshold(CTxOut(fee_asset, 1, margin_script), ::dustRelayFee);
    if (margin < margin_dust) margin = 0;

    const int64_t fee_swept = shares.swept.count(fee_asset) ? shares.swept.at(fee_asset) : 0;
    const int64_t fee_leftover = fee_swept - (paid.count(fee_asset) ? paid.at(fee_asset) : 0)
                                 - fee_rate_estimate - margin;
    if (fee_leftover < 0) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            "the pot cannot cover its own claim fee yet; claim again once more has accrued");
    }
    if (margin > 0) mtx.vout.push_back(CTxOut(fee_asset, margin, margin_script));
    if (fee_leftover > 0) {
        mtx.vout.push_back(CTxOut(fee_asset, fee_leftover, pot_script));
        repot[fee_asset] += fee_leftover;
    }
    mtx.vout.push_back(CTxOut(fee_asset, fee_rate_estimate, CScript())); // the explicit fee output

    // 4) Self-check with the SAME function every validator runs, before
    //    anything leaves this node: a claim this check rejects would poison the
    //    mempool for every producer.
    std::vector<Coin> spent_coins;
    for (const auto& e : pots) {
        CTxOut out(e.second.asset, e.second.value, pot_script);
        spent_coins.emplace_back(std::move(out), e.second.height, /*fCoinBaseIn=*/false);
    }
    std::string claim_reason;
    if (!CheckPosPotClaim(CTransaction(mtx), spent_coins, claim_reason)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "constructed an invalid claim (%s); nothing was sent", claim_reason));
    }

    // 5) Broadcast. Nothing to sign: the pot script is anyone-can-spend, and
    //    the consensus overlay is the entire spend condition.
    const CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string err_string;
    if (!pwallet->chain().broadcastTransaction(tx, pwallet->m_default_max_tx_fee, /*relay=*/true, err_string)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("failed to broadcast the claim: %s", err_string));
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("pot_outputs", (int64_t)pots.size());
    UniValue dist(UniValue::VOBJ), rep(UniValue::VOBJ);
    for (const auto& e : paid) dist.pushKV(e.first.GetHex(), ValueFromAmount(e.second));
    for (const auto& e : repot) rep.pushKV(e.first.GetHex(), ValueFromAmount(e.second));
    result.pushKV("distributed", dist);
    result.pushKV("repotted", rep);
    result.pushKV("fee", ValueFromAmount(fee_rate_estimate));
    result.pushKV("margin", ValueFromAmount(margin));
    result.pushKV("delegators_paid", delegators_paid);
    return result;
},
    };
}

RPCHelpMan listdelegations()
{
    return RPCHelpMan{"listdelegations",
                "\nSEQUENTIA staking pools: what this wallet's stake is doing, and what the pools it is delegated\n"
                "to have committed to. One entry per staker key, whether or not it delegates.\n"
                "\nThis is the delegator's watch. A pool's payout policy cannot change without announcing it at\n"
                "least the chain's notice period in advance, and leaving is instant and unilateral -- but that\n"
                "only protects a delegator who SEES the notice. Any announced change that has not bound yet is\n"
                "reported here as a pending policy, with how long is left to act, and summarised in `alerts`.\n",
                {
                    {"controller", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Only this staker public key (hex)."},
                },
                RPCResult{RPCResult::Type::ARR, "", "", {
                    {RPCResult::Type::OBJ, "", "", {
                        {RPCResult::Type::STR_HEX, "controller", "the staker key"},
                        {RPCResult::Type::STR_HEX, "signer", "the key producing blocks with this weight (the controller itself when not delegating)"},
                        {RPCResult::Type::BOOL, "delegated", "whether the weight is lent to someone else"},
                        {RPCResult::Type::NUM, "weight", "this key's stake weight (atoms)"},
                        {RPCResult::Type::NUM, "signer_weight", "total weight the signer commands, including everyone else's (atoms)"},
                        {RPCResult::Type::NUM, "pool_share", /*optional=*/true, "this key's share of the signer's total weight (0..1)"},
                        {RPCResult::Type::OBJ, "record", /*optional=*/true, "the delegation record, when delegating", {
                            {RPCResult::Type::STR_HEX, "txid", "the funding transaction id"},
                            {RPCResult::Type::NUM, "vout", "the funding output index"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "SEQ held in the record, reclaimable with undelegatestake"},
                            {RPCResult::Type::BOOL, "confirmed", "whether the record has confirmed; the delegation is not in force until it has"},
                            {RPCResult::Type::BOOL, "changing", "a reclaim or re-pointing has been sent and is waiting to confirm"},
                        }},
                        {RPCResult::Type::OBJ, "policy_in_force", /*optional=*/true, "the payout policy binding the signer right now, if it has committed to one", {
                            {RPCResult::Type::NUM, "activation", "height from which it binds"},
                            {RPCResult::Type::STR, "mode", "\"direct\", \"lottery\" or \"split\""},
                            {RPCResult::Type::STR_HEX, "payout_script", /*optional=*/true, "direct: the committed payee"},
                            {RPCResult::Type::NUM, "commission_bp", /*optional=*/true, "lottery: the operator's basis points"},
                        }},
                        {RPCResult::Type::ARR, "policy_pending", "announced policies that have not bound yet -- the notice window", {
                            {RPCResult::Type::OBJ, "", "", {
                                {RPCResult::Type::NUM, "activation", "height from which it will bind"},
                                {RPCResult::Type::NUM, "blocks_away", "blocks left before it binds"},
                                {RPCResult::Type::NUM_TIME, "binds_around", "approximate time it binds"},
                                {RPCResult::Type::STR, "mode", "\"direct\", \"lottery\" or \"split\""},
                                {RPCResult::Type::STR_HEX, "payout_script", /*optional=*/true, "direct: the committed payee"},
                                {RPCResult::Type::NUM, "commission_bp", /*optional=*/true, "lottery: the operator's basis points"},
                            }},
                        }},
                        {RPCResult::Type::ARR, "alerts", "what a delegator should know about this pool right now", {
                            {RPCResult::Type::STR, "", "a plain-language warning"},
                        }},
                    }},
                }},
                RPCExamples{HelpExampleCli("listdelegations", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    std::optional<CPubKey> only;
    if (!request.params[0].isNull()) {
        CPubKey pk(ParseHexV(request.params[0], "controller"));
        if (!pk.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid controller public key");
        only = pk;
    }

    LOCK(pwallet->cs_wallet);
    const int tip_height = pwallet->GetLastBlockHeight();
    int64_t tip_time = 0;
    pwallet->chain().findBlock(pwallet->GetLastBlockHash(), interfaces::FoundBlock().time(tip_time));

    const StakeRegistry& registry = StakeRegistry::GetInstance();
    const std::map<CPubKey, uint64_t> controller_weights = registry.ControllerWeights();
    const std::map<CPubKey, uint64_t> signer_weights = registry.Weights();
    const auto payouts = registry.Payouts();

    // Every staker key this wallet has a reason to report on: one it stakes
    // with, and one it holds a delegation record for (which may have no stake
    // registered against it yet).
    std::map<CPubKey, const DelegationUtxo*> records;
    const std::vector<DelegationUtxo> record_list = FindWalletDelegationUtxos(*pwallet, only);
    for (const DelegationUtxo& d : record_list) records[d.controller] = &d;

    std::set<CPubKey> controllers;
    for (const CPubKey& k : WalletStakerKeys(*pwallet)) {
        if (!only || k == *only) controllers.insert(k);
    }
    for (const auto& e : records) controllers.insert(e.first);

    UniValue result(UniValue::VARR);
    for (const CPubKey& controller : controllers) {
        const auto rec_it = records.find(controller);
        const DelegationUtxo* rec = rec_it == records.end() ? nullptr : rec_it->second;
        // The registry is the authority on who signs: it reflects what the
        // whole network sees, including a record this wallet did not fund.
        const CPubKey signer = registry.SignerFor(controller);
        const bool delegated = signer != controller;

        const auto cw = controller_weights.find(controller);
        const uint64_t weight = cw == controller_weights.end() ? 0 : cw->second;
        const auto sw = signer_weights.find(signer);
        const uint64_t signer_weight = sw == signer_weights.end() ? 0 : sw->second;

        UniValue o(UniValue::VOBJ);
        o.pushKV("controller", HexStr(controller));
        o.pushKV("signer", HexStr(signer));
        o.pushKV("delegated", delegated);
        o.pushKV("weight", weight);
        o.pushKV("signer_weight", signer_weight);
        if (signer_weight > 0) o.pushKV("pool_share", (double)weight / (double)signer_weight);

        if (rec) {
            UniValue r(UniValue::VOBJ);
            r.pushKV("txid", rec->outpoint.hash.GetHex());
            r.pushKV("vout", (int64_t)rec->outpoint.n);
            r.pushKV("amount", ValueFromAmount(rec->amount));
            r.pushKV("confirmed", !rec->unconfirmed);
            r.pushKV("changing", rec->spending);
            o.pushKV("record", r);
        }

        UniValue alerts(UniValue::VARR);
        UniValue pending(UniValue::VARR);

        // What the signer has committed to. Only worth reporting for a pool:
        // when signing for yourself the coinbase is yours by default anyway.
        const auto describe = [&](const PosPayoutPolicy& p, UniValue& into) {
            into.pushKV("activation", p.activation);
            into.pushKV("mode", PosPayoutModeName(p.mode));
            if (p.mode == PosPayoutMode::DIRECT) into.pushKV("payout_script", HexStr(p.script));
            else into.pushKV("commission_bp", (int64_t)p.commission_bp);
        };

        const auto in_force = registry.PayoutFor(signer, tip_height);
        if (in_force) {
            UniValue p(UniValue::VOBJ);
            describe(*in_force, p);
            o.pushKV("policy_in_force", p);
        }
        const auto sp = payouts.find(signer);
        if (sp != payouts.end()) {
            for (const auto& e : sp->second) {
                const PosPayoutPolicy& p = e.second;
                if (p.activation <= tip_height) continue; // already binding, or superseded
                UniValue po(UniValue::VOBJ);
                describe(p, po);
                const int64_t away = p.activation - tip_height;
                po.pushKV("blocks_away", away);
                po.pushKV("binds_around", tip_time + away * ApproxSecondsPerBlock());
                pending.push_back(po);
                if (delegated) {
                    alerts.push_back(strprintf(
                        "pool %s has announced a NEW payout policy (%s) binding at block %d, %d blocks from now "
                        "(around %s). If you do not accept it, reclaim your stake's signing rights with "
                        "undelegatestake before then -- leaving is instant and needs nobody's permission.",
                        HexStr(signer), PosPayoutModeName(p.mode),
                        (int)p.activation, (int)away, FormatISO8601DateTime(tip_time + away * ApproxSecondsPerBlock())));
                }
            }
        }
        o.pushKV("policy_pending", pending);

        if (delegated) {
            if (!in_force) {
                alerts.push_back(strprintf(
                    "pool %s has committed to no payout policy, so by default it keeps everything the blocks it "
                    "produces earn. Nothing on-chain obliges it to pay you.", HexStr(signer)));
            } else if (in_force->mode == PosPayoutMode::DIRECT) {
                alerts.push_back(
                    "this pool pays a committed address (direct mode). The chain stops the operator redirecting "
                    "the reward silently, but does NOT check that the destination shares anything with you: "
                    "check who that address belongs to.");
            }
            if (rec && rec->unconfirmed) {
                alerts.push_back("this delegation has not confirmed yet, so the weight still counts for you.");
            }
            if (rec && rec->spending) {
                alerts.push_back("a change to this delegation has been sent and is waiting to confirm.");
            }
            if (!rec) {
                alerts.push_back(strprintf(
                    "the delegation record for %s was not funded by this wallet, so it cannot be re-pointed or "
                    "reclaimed from here.", HexStr(controller)));
            }
            if (weight == 0) {
                alerts.push_back("this key has no registered stake, so the delegation lends nothing.");
            }
        }
        o.pushKV("alerts", alerts);
        result.push_back(o);
    }
    return result;
},
    };
}

/** Every staking reward this wallet has received, newest first. Declared in
 *  wallet/stakingrewards.h, where the two shapes are spelled out.
 *
 *      coinbase && IsMine(out)                    -> solo | direct | lottery
 *      !coinbase && !IsFromMe && IsMine(out)
 *          && out pays P2WPKH(a staking key)      -> split (a pot claim)
 *
 *  The second rule is narrow on purpose. A staking key is never handed out as a
 *  receive address, and requiring the transaction not to be ours excludes a
 *  delegator's own withdrawal or re-pointing, which also pay back to it.
 */
std::vector<StakingReward> FindWalletStakingRewards(CWallet& wallet, bool include_spent)
{
    const StakeRegistry& registry = StakeRegistry::GetInstance();

    // The keys a reward can be paid on. Three sources, because a staker's key
    // reaches the wallet by three different routes:
    //  - every key the REGISTRY knows as a staker whose P2WPKH this wallet can
    //    spend. This is the one that catches a staker configured with -staker=,
    //    which holds weight without the wallet holding a stake output at all --
    //    how the committee nodes run;
    //  - every key this wallet holds a stake output for, including stake being
    //    withdrawn: a reward already earned is still a reward;
    //  - every controller this wallet holds a delegation record for, which may
    //    have no registered weight yet.
    std::map<CScript, CPubKey> staking_scripts;
    const auto consider = [&](const CPubKey& k) {
        const CScript spk = PosLeaderFeeScript(k);
        if (wallet.IsMine(spk) & ISMINE_SPENDABLE) staking_scripts.emplace(spk, k);
    };
    for (const auto& e : registry.ControllerWeights()) consider(e.first);
    for (const auto& e : registry.Weights()) consider(e.first);
    for (const StakeUtxo& s : FindWalletStakeUtxos(wallet, std::nullopt)) {
        staking_scripts.emplace(PosLeaderFeeScript(s.parsed.pubkey), s.parsed.pubkey);
    }
    for (const DelegationUtxo& d : FindWalletDelegationUtxos(wallet, std::nullopt)) {
        staking_scripts.emplace(PosLeaderFeeScript(d.controller), d.controller);
    }

    std::vector<StakingReward> rewards;
    for (const auto& entry : wallet.mapWallet) {
        const CWalletTx& wtx = entry.second;
        const bool coinbase = wtx.tx->IsCoinBase();
        // Our own spending is never a reward. A coinbase is never ours to send,
        // so the test is only meaningful (and only applied) off the coinbase path.
        if (!coinbase && CachedTxIsFromMe(wallet, wtx, ISMINE_ALL)) continue;

        const int depth = wallet.GetTxDepthInMainChain(wtx);
        if (depth < 0) continue;  // conflicted with a block: not money
        int height = 0;
        if (const auto* conf = wtx.state<TxStateConfirmed>()) height = conf->confirmed_block_height;

        for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i) {
            const CTxOut& txout = wtx.tx->vout[i];
            if (txout.IsFee()) continue;
            if (!(wallet.IsMine(txout) & ISMINE_SPENDABLE)) continue;

            const auto sk = staking_scripts.find(txout.scriptPubKey);
            const bool on_staking_key = sk != staking_scripts.end();
            if (!coinbase && !on_staking_key) continue;

            StakingReward r;
            r.outpoint = COutPoint(wtx.GetHash(), i);
            r.spent = wallet.IsSpent(r.outpoint.hash, r.outpoint.n);
            if (r.spent && !include_spent) continue;

            r.amount = wtx.GetOutputValueOut(wallet, i);
            r.asset = wtx.GetOutputAsset(wallet, i);
            // A reward we cannot unblind is not a reward we can convert or even
            // count. Consensus only ever creates explicit coinbase and pot-claim
            // outputs, so this skips nothing real.
            if (r.amount <= 0 || r.asset.IsNull()) continue;

            // Which of the four sources. Note that the payout policy consulted
            // here is the one in force NOW, not at the reward's height: the
            // registry is a function of the current UTXO set. The label is
            // informational -- nothing downstream, conversion included, depends
            // on which of the coinbase sources a reward came from.
            if (!coinbase) {
                r.source = "split";
                r.controller = sk->second;
            } else if (on_staking_key) {
                r.controller = sk->second;
                // Paid on our own staking key: either we were the elected leader
                // (not delegating), or a pool's lottery draw landed on us.
                r.source = registry.SignerFor(sk->second) == sk->second ? "solo" : "lottery";
            } else {
                // A coinbase paying some other script of ours is a pool paying
                // the address it committed to under a direct policy.
                r.source = "direct";
            }

            r.height = height;
            r.depth = depth;
            r.blocks_to_maturity = coinbase ? wallet.GetTxBlocksToMaturity(wtx) : 0;
            ExtractDestination(txout.scriptPubKey, r.dest);
            rewards.push_back(std::move(r));
        }
    }

    // Newest first, with a total order so the listing is stable across calls.
    std::sort(rewards.begin(), rewards.end(), [](const StakingReward& a, const StakingReward& b) {
        if (a.height != b.height) return a.height > b.height;
        if (a.outpoint.hash != b.outpoint.hash) return a.outpoint.hash < b.outpoint.hash;
        return a.outpoint.n < b.outpoint.n;
    });
    return rewards;
}

RPCHelpMan liststakingrewards()
{
    return RPCHelpMan{"liststakingrewards",
                "\nSEQUENTIA staking: every coin this wallet was PAID for staking, in whatever asset it was\n"
                "paid in. Sequentia has no block subsidy, so a staker earns the transaction fees of the blocks\n"
                "it produces -- and under the open fee market those arrive in whichever assets the payers chose,\n"
                "one coinbase output per asset. A pool delegator is paid the same way, through its pool.\n"
                "\nFour sources, which are the four ways the rules pay a staker:\n"
                "  solo     this wallet's own key was the elected leader and the coinbase paid it\n"
                "  lottery  a pool's per-block draw landed on this wallet's stake\n"
                "  direct   a pool paid the address it committed to under a direct payout policy\n"
                "  split    a share of a pool's pot, distributed by claimpoolrewards\n"
                "\nCoinbase rewards are spendable only once they mature (100 blocks); immature ones are listed\n"
                "with the blocks left to wait. `totals` adds each asset up, which is what a wallet offering to\n"
                "convert rewards needs: see doc/sequentia/reward-autoconvert-design.md.\n",
                {
                    {"include_spent", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also list rewards that have already been spent."},
                    {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only this asset (hex id or label)."},
                    {"count", RPCArg::Type::NUM, RPCArg::Default{0}, "Return at most this many rewards (0 = all). Totals always cover everything."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::ARR, "rewards", "the reward outputs, newest first", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "txid", "the transaction that paid it"},
                            {RPCResult::Type::NUM, "vout", "the output index"},
                            {RPCResult::Type::STR, "source", "\"solo\", \"lottery\", \"direct\" or \"split\""},
                            {RPCResult::Type::STR_HEX, "asset", "the asset it was paid in"},
                            {RPCResult::Type::STR, "assetlabel", /*optional=*/true, "the asset's label, when it has one"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "how much"},
                            {RPCResult::Type::STR, "address", /*optional=*/true, "where it was paid"},
                            {RPCResult::Type::STR_HEX, "controller", /*optional=*/true, "the staking key it was paid on, when it was paid on one"},
                            {RPCResult::Type::NUM, "height", "the height it confirmed at, 0 while unconfirmed"},
                            {RPCResult::Type::NUM, "confirmations", "confirmations"},
                            {RPCResult::Type::BOOL, "mature", "whether it is spendable yet (coinbase value matures at 100 blocks)"},
                            {RPCResult::Type::NUM, "blocks_to_maturity", "blocks left before it is spendable, 0 when it already is"},
                            {RPCResult::Type::BOOL, "spent", "whether this wallet has already spent it"},
                        }},
                    }},
                    {RPCResult::Type::ARR, "totals", "what has been earned, per asset, over every reward listed", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "asset", "the asset"},
                            {RPCResult::Type::STR, "assetlabel", /*optional=*/true, "its label, when it has one"},
                            {RPCResult::Type::STR_AMOUNT, "mature", "spendable now"},
                            {RPCResult::Type::STR_AMOUNT, "immature", "earned, still inside coinbase maturity"},
                            {RPCResult::Type::NUM, "outputs", "how many reward outputs"},
                        }},
                    }},
                }},
                RPCExamples{HelpExampleCli("liststakingrewards", "")
                          + HelpExampleCli("liststakingrewards", "false \"GOLD\"")
                          + HelpExampleRpc("liststakingrewards", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    const bool include_spent = request.params[0].isNull() ? false : request.params[0].get_bool();
    std::optional<CAsset> only_asset;
    // An empty string means "every asset", not "an asset called nothing". A
    // positional call has no other way to skip this argument.
    if (!request.params[1].isNull() && !request.params[1].get_str().empty()) {
        const CAsset a = GetAssetFromString(request.params[1].get_str());
        if (a.IsNull()) throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Unknown label and invalid asset hex: " + request.params[1].get_str());
        only_asset = a;
    }
    const int count = request.params[2].isNull() ? 0 : request.params[2].get_int();
    if (count < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "count cannot be negative");

    LOCK(pwallet->cs_wallet);
    const std::vector<StakingReward> rewards = FindWalletStakingRewards(*pwallet, include_spent);

    struct Total { CAmount mature{0}; CAmount immature{0}; int outputs{0}; };
    std::map<CAsset, Total> totals;

    UniValue list(UniValue::VARR);
    for (const StakingReward& r : rewards) {
        if (only_asset && r.asset != *only_asset) continue;

        Total& t = totals[r.asset];
        ++t.outputs;
        (r.blocks_to_maturity > 0 ? t.immature : t.mature) += r.amount;

        if (count > 0 && (int)list.size() >= count) continue;  // totals still cover everything

        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", r.outpoint.hash.GetHex());
        o.pushKV("vout", (int64_t)r.outpoint.n);
        o.pushKV("source", r.source);
        o.pushKV("asset", r.asset.GetHex());
        const std::string label = gAssetsDir.GetLabel(r.asset);
        if (!label.empty()) o.pushKV("assetlabel", label);
        o.pushKV("amount", ValueFromAmount(r.amount));
        if (IsValidDestination(r.dest)) o.pushKV("address", EncodeDestination(r.dest));
        if (r.controller.IsValid()) o.pushKV("controller", HexStr(r.controller));
        o.pushKV("height", (int64_t)r.height);
        o.pushKV("confirmations", (int64_t)r.depth);
        o.pushKV("mature", r.blocks_to_maturity == 0);
        o.pushKV("blocks_to_maturity", (int64_t)r.blocks_to_maturity);
        o.pushKV("spent", r.spent);
        list.push_back(o);
    }

    UniValue totals_arr(UniValue::VARR);
    for (const auto& e : totals) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("asset", e.first.GetHex());
        const std::string label = gAssetsDir.GetLabel(e.first);
        if (!label.empty()) o.pushKV("assetlabel", label);
        o.pushKV("mature", ValueFromAmount(e.second.mature));
        o.pushKV("immature", ValueFromAmount(e.second.immature));
        o.pushKV("outputs", (int64_t)e.second.outputs);
        totals_arr.push_back(o);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("rewards", list);
    result.pushKV("totals", totals_arr);
    return result;
},
    };
}
namespace {
// bitcoind runs one scantxoutset at a time for the whole process, and this wallet
// has several reasons to want one: every loaded wallet refreshes its Bitcoin balance
// on its own timer, and a Bitcoin send scans before it spends. Two wallets open in
// one GUI is enough -- their timers start together and collide once a minute, for
// ever.
//
// So wait for the slot rather than returning bitcoind refusal to a caller who did
// nothing wrong. The wait is a DEADLINE, not a number of tries: a scan of testnet4
// measured 18.8 s over 14.2 million outputs, and a retry budget shorter than one
// scan means whoever comes second always gives up just before the slot frees --
// which is exactly how "Scan already in progress" ended up parked under the
// balances. The deadline is generous enough for a scan several times that size and
// still bounded, so an abandoned scan cannot hang a wallet for ever.
UniValue ScanParentChainUtxoSet(const UniValue& params)
{
    constexpr auto wait_budget = std::chrono::seconds{90};
    const auto deadline = std::chrono::steady_clock::now() + wait_budget;
    UniValue reply;
    for (;;) {
        reply = CallMainChainRPC("scantxoutset", params);
        const bool busy = reply.exists("error") && !reply["error"].isNull() &&
            reply["error"].isObject() && reply["error"].exists("message") &&
            reply["error"]["message"].get_str().find("in progress") != std::string::npos;
        if (!busy || std::chrono::steady_clock::now() >= deadline) break;
        UninterruptibleSleep(std::chrono::milliseconds{1500});
    }
    return reply;
}
} // namespace

RPCHelpMan getbtcscanprogress()
{
    return RPCHelpMan{"getbtcscanprogress",
        "\nHow far along the parent-chain scan behind getbtcbalance is, if one is running.\n"
        "\nThe first reading of a wallet Bitcoin balance walks the whole parent-chain UTXO set,\n"
        "which takes seconds on testnet4 and minutes on a mainnet-sized set. A caller that shows\n"
        "the balance can poll this to say how far it has got instead of showing nothing at all.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "scanning", "whether a parent-chain scan is running right now"},
            {RPCResult::Type::NUM, "progress", "percent complete, 0-100; absent when nothing is running"},
        }},
        RPCExamples{HelpExampleCli("getbtcscanprogress", "") + HelpExampleRpc("getbtcscanprogress", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue result(UniValue::VOBJ);
    UniValue params(UniValue::VARR);
    params.push_back("status");
    // A scan holds bitcoind single scan slot, and asking for its status does not
    // queue behind it -- that is the whole reason this can be polled while one runs.
    UniValue reply;
    try {
        reply = CallMainChainRPC("scantxoutset", params);
    } catch (const std::exception&) {
        result.pushKV("scanning", false);
        return result;
    }
    const UniValue& res = reply.exists("result") ? reply["result"] : NullUniValue;
    // bitcoind answers null when no scan is running.
    if (!res.isObject()) {
        result.pushKV("scanning", false);
        return result;
    }
    result.pushKV("scanning", true);
    if (res.exists("progress") && res["progress"].isNum()) {
        result.pushKV("progress", res["progress"].get_int());
    }
    return result;
}};
}

namespace {

//! SEQUENTIA: the wallet's own record of its parent-chain coins.
//!
//! Without it, every question about Bitcoin -- what is my balance, what can I
//! spend -- was answered by rebuilding the answer from nothing: a scantxoutset
//! over the whole parent UTXO set, measured at 18.8 s across 14.2 million
//! outputs on testnet4 and minutes on a mainnet-sized set. That is the reason a
//! balance took twenty seconds and a send froze the window twice.
//!
//! Every serious wallet instead KEEPS what it owns and edits it as blocks
//! arrive. This is that record: filled once by a full scan, then moved forward
//! block by block. A balance becomes a sum of local rows, and coin selection a
//! walk over them -- the same thing the wallet already does for Sequentia
//! assets, which is why those were always instant.
struct ParentCoin {
    uint256 txid;
    uint32_t vout{0};
    CAmount amount{0};
    int height{0};              //!< 0 = ours but not yet in a block (our own change)
    int64_t time{0};            //!< block time, so a list can say when it arrived
    std::string script_hex;     //!< the scriptPubKey, so spending never has to guess it
    std::string address;
    std::string spent_by;       //!< txid of our spend, while that spend is unconfirmed
};

struct ParentCoinSet {
    int scanned_height{0};      //!< every block up to here has been applied
    std::string scanned_hash;   //!< ...and this was its hash, so a reorg is visible
    int64_t full_scan_ms{0};    //!< what one full scan cost here, measured, not guessed
    std::vector<ParentCoin> coins;
    bool loaded{false};         //!< false = no record yet, a full scan is owed
};

fs::path ParentCoinsPath(const CWallet& wallet)
{
    return fs::PathFromString(wallet.GetDatabase().Filename()).parent_path() / "parent_coins.json";
}

//! Read the record as it is on disk, usable or not. A record can be unusable for
//! spending and still hold the only copy of something: which coins are committed to
//! a spend of ours, and the change of that spend, neither of which the parent chain
//! will admit to until the transaction confirms.
ParentCoinSet LoadParentCoinsRaw(const CWallet& wallet)
{
    ParentCoinSet out;
    std::ifstream f(fs::PathToString(ParentCoinsPath(wallet)));
    if (!f.good()) return out;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue parsed;
    if (!parsed.read(data) || !parsed.isObject()) return out;
    out.scanned_height = parsed.exists("scanned_height") && parsed["scanned_height"].isNum()
                         ? parsed["scanned_height"].get_int() : 0;
    out.scanned_hash = parsed.exists("scanned_hash") ? parsed["scanned_hash"].getValStr() : "";
    out.full_scan_ms = parsed.exists("full_scan_ms") && parsed["full_scan_ms"].isNum()
                       ? parsed["full_scan_ms"].get_int64() : 0;
    if (parsed.exists("coins") && parsed["coins"].isArray()) {
        const UniValue& arr = parsed["coins"];
        for (size_t i = 0; i < arr.size(); ++i) {
            const UniValue& c = arr[i];
            if (!c.isObject() || !c["txid"].isStr()) continue;
            ParentCoin coin;
            coin.txid = uint256S(c["txid"].get_str());
            coin.vout = c["vout"].isNum() ? (uint32_t)c["vout"].get_int() : 0;
            int64_t parsed_amt = 0;
            if (c["btc"].isStr() && ParseFixedPoint(c["btc"].get_str(), 8, &parsed_amt)) coin.amount = parsed_amt;
            coin.height = c["height"].isNum() ? c["height"].get_int() : 0;
            coin.time = c["time"].isNum() ? c["time"].get_int64() : 0;
            coin.script_hex = c["script"].getValStr();
            coin.address = c["address"].getValStr();
            coin.spent_by = c["spent_by"].getValStr();
            out.coins.push_back(std::move(coin));
        }
    }
    out.loaded = true;
    return out;
}

//! The record, if it is one this build can spend from. A coin without its script
//! cannot be spent, and this chain cannot recover the script from the address it
//! was paid to -- parent-chain addresses do not decode here. A record written
//! before the script was kept is therefore not a usable record: report none, and
//! let the caller rebuild it once rather than carry coins it can only look at.
ParentCoinSet LoadParentCoins(const CWallet& wallet)
{
    ParentCoinSet out = LoadParentCoinsRaw(wallet);
    if (!out.loaded) return out;
    for (const ParentCoin& c : out.coins) {
        if (c.script_hex.empty() && c.height > 0) return ParentCoinSet{};
    }
    return out;
}

void StoreParentCoins(const CWallet& wallet, const ParentCoinSet& set)
{
    UniValue root(UniValue::VOBJ);
    root.pushKV("scanned_height", set.scanned_height);
    root.pushKV("scanned_hash", set.scanned_hash);
    root.pushKV("full_scan_ms", set.full_scan_ms);
    UniValue arr(UniValue::VARR);
    for (const ParentCoin& c : set.coins) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", c.txid.GetHex());
        o.pushKV("vout", (int)c.vout);
        o.pushKV("btc", ValueFromAmount(c.amount).getValStr());
        o.pushKV("height", c.height);
        o.pushKV("time", c.time);
        if (!c.script_hex.empty()) o.pushKV("script", c.script_hex);
        o.pushKV("address", c.address);
        if (!c.spent_by.empty()) o.pushKV("spent_by", c.spent_by);
        arr.push_back(o);
    }
    root.pushKV("coins", arr);
    std::ofstream f(fs::PathToString(ParentCoinsPath(wallet)), std::ios::trunc);
    f << root.write(1) << std::endl;
}

//! What the wallet can spend right now: ours, and not already committed to a
//! spend of ours that has yet to confirm. Spending a coin twice is how the
//! second send of a session became a replacement its own predecessor refused.
CAmount SpendableParentBalance(const ParentCoinSet& set)
{
    CAmount total = 0;
    for (const ParentCoin& c : set.coins) {
        if (c.spent_by.empty()) total += c.amount;
    }
    return total;
}

//! This wallet's parent-chain scripts, by hex, with the address each belongs to.
//! The unblinded scriptPubKey is byte-identical to what Bitcoin sees, which is the
//! whole reason a Sequentia wallet can hold Bitcoin at its own addresses.
std::map<std::string, std::string> WalletParentScripts(const CWallet& wallet)
{
    std::map<std::string, std::string> out;
    LOCK(wallet.cs_wallet);
    for (const auto& item : wallet.m_address_book) {
        CTxDestination dest = item.first;
        std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
        const std::string addr = EncodeDestination(dest);
        if (addr.empty()) continue;
        const CScript spk = GetScriptForDestination(dest);
        // The address book holds every address this wallet has WRITTEN DOWN,
        // which includes the ones it pays: a saved payee is in there beside our
        // own receiving addresses. Counting coins at those as ours inflates the
        // balance with somebody else money -- visible the moment a send refuses
        // to spend what the overview claims is there.
        if (!wallet.IsMine(spk)) continue;
        out[HexStr(spk)] = addr;
    }
    return out;
}

//! Apply one parent block to the record: coins of ours it creates, coins of ours
//! it spends. Both directions matter -- a wallet that only watched for money
//! arriving would keep offering coins it no longer owns.
void ApplyParentBlock(const UniValue& block, int height, int64_t block_time,
                      const std::map<std::string, std::string>& mine,
                      ParentCoinSet& set)
{
    const UniValue& txs = find_value(block, "tx");
    if (!txs.isArray()) return;
    for (size_t i = 0; i < txs.size(); ++i) {
        const UniValue& tx = txs[i];
        const std::string txid_hex = find_value(tx, "txid").getValStr();

        // Spent: drop ours that this block consumes, whoever spent them. Our own
        // pending spend confirming lands here too, which is how a coin marked
        // spent_by finally leaves the record.
        const UniValue& vins = find_value(tx, "vin");
        if (vins.isArray()) {
            for (size_t k = 0; k < vins.size(); ++k) {
                const UniValue& pt = find_value(vins[k], "txid");
                const UniValue& pv = find_value(vins[k], "vout");
                if (!pt.isStr() || !pv.isNum()) continue; // coinbase
                const uint256 ptxid = uint256S(pt.get_str());
                const uint32_t pvout = (uint32_t)pv.get_int();
                set.coins.erase(std::remove_if(set.coins.begin(), set.coins.end(),
                    [&](const ParentCoin& c) { return c.txid == ptxid && c.vout == pvout; }),
                    set.coins.end());
            }
        }

        // Received: outputs paying one of our scripts.
        const UniValue& vouts = find_value(tx, "vout");
        if (!vouts.isArray()) continue;
        for (size_t j = 0; j < vouts.size(); ++j) {
            const UniValue& spk = find_value(vouts[j], "scriptPubKey");
            if (!spk.isObject()) continue;
            const auto it = mine.find(find_value(spk, "hex").getValStr());
            if (it == mine.end()) continue;
            const uint256 txid = uint256S(txid_hex);
            const uint32_t vout = (uint32_t)find_value(vouts[j], "n").get_int();
            // Our own change, recorded at height 0 when we sent it, is the same coin:
            // confirm it in place rather than listing it twice.
            auto existing = std::find_if(set.coins.begin(), set.coins.end(),
                [&](const ParentCoin& c) { return c.txid == txid && c.vout == vout; });
            if (existing != set.coins.end()) {
                existing->height = height;
                existing->time = block_time;
                continue;
            }
            ParentCoin coin;
            coin.txid = txid;
            coin.vout = vout;
            coin.amount = AmountFromValue(find_value(vouts[j], "value"));
            coin.height = height;
            coin.time = block_time;
            coin.script_hex = it->first;
            coin.address = it->second;
            set.coins.push_back(std::move(coin));
        }
    }
}

//! Move the record forward to the parent tip by reading only the blocks that
//! arrived since last time. Returns false when the record cannot be advanced and
//! a full scan is owed instead -- the caller decides whether to pay for one.
bool AdvanceParentCoins(const CWallet& wallet, ParentCoinSet& set, std::string& err)
{
    if (!set.loaded) { err = "no record yet"; return false; }
    int tip_height = 0;
    try {
        UniValue r = CallMainChainRPC("getblockcount", UniValue(UniValue::VARR));
        if (!r.exists("result") || !r["result"].isNum()) { err = "parent chain unreachable"; return false; }
        tip_height = r["result"].get_int();
    } catch (const std::exception& e) {
        err = std::string("parent chain unreachable: ") + e.what();
        return false;
    }
    if (tip_height <= set.scanned_height) return true; // already current

    // A record describes a history. If the block it stopped at is no longer the
    // one at that height, the parent chain reorganised under us and every coin
    // after it is in doubt: rescan rather than guess.
    if (!set.scanned_hash.empty()) {
        try {
            UniValue hp(UniValue::VARR);
            hp.push_back(set.scanned_height);
            UniValue hr = CallMainChainRPC("getblockhash", hp);
            if (hr.exists("result") && hr["result"].isStr() && hr["result"].get_str() != set.scanned_hash) {
                err = "parent chain reorganised past the recorded point";
                return false;
            }
        } catch (const std::exception&) {
            err = "parent chain unreachable";
            return false;
        }
    }

    // Past some gap, one full scan is simply faster than reading block by block.
    // Where that point lies is not a constant: it depends on how long a scan takes
    // here (seconds on testnet4, minutes on a mainnet-sized UTXO set) against what a
    // block costs, which is dominated by the connection rather than the block --
    // every parent RPC opens a fresh TCP connection and authenticates again.
    // Measured on 2026-08-25: 38 blocks took 33.5 s, about 0.9 s each, against 33.6 s
    // for a full scan of the same wallet. So the crossing point was around forty
    // blocks, not the five hundred first assumed -- which would have meant seven
    // minutes of catching up to avoid half a minute of scanning.
    //
    // Compare the two costs instead of hardcoding a winner, using the scan time this
    // wallet actually saw. Batched parent RPC (upstream 24.7.5) will cut the per
    // block cost sharply, and this arithmetic will follow it without being touched.
    constexpr int64_t ms_per_block_estimate = 900;
    const int behind = tip_height - set.scanned_height;

    // Speed is not the only reason to walk blocks. A scan sees the confirmed chain
    // and nothing else, so it silently drops what only the record knows: the coins
    // committed to a send still in a mempool, and that send's change. Walking keeps
    // them. So a short gap is walked whatever the clock says -- on a small chain a
    // scan can be milliseconds, and a wallet that always rescans because rescanning
    // is quick would keep forgetting its own pending sends.
    constexpr int always_walk_below = 20;
    const int64_t known_scan_ms = set.full_scan_ms > 0 ? set.full_scan_ms : 30000;
    if (behind > always_walk_below && (int64_t)behind * ms_per_block_estimate > known_scan_ms) {
        err = strprintf("%d blocks behind: a full scan (%d ms last time) is faster than catching up",
                        behind, known_scan_ms);
        return false;
    }

    const std::map<std::string, std::string> mine = WalletParentScripts(wallet);
    std::string last_hash = set.scanned_hash;

    // Follow nextblockhash from the block we stopped at rather than asking for each
    // height. Every parent RPC opens its own TCP connection and authenticates again
    // -- fine once, ruinous by the thousand -- so halving the calls halves the real
    // cost of catching up. A block already names its successor; asking for what we
    // have been told is a round trip spent on nothing.
    std::string next_hash;
    {
        try {
            UniValue hp(UniValue::VARR);
            hp.push_back(set.scanned_height + 1);
            UniValue hr = CallMainChainRPC("getblockhash", hp);
            if (!hr.exists("result") || !hr["result"].isStr()) { err = "parent chain unreachable"; return false; }
            next_hash = hr["result"].get_str();
        } catch (const std::exception& e) {
            err = std::string("parent chain unreachable: ") + e.what();
            return false;
        }
    }

    for (int h = set.scanned_height + 1; h <= tip_height && !next_hash.empty(); ++h) {
        try {
            const std::string bhash = next_hash;
            UniValue bp(UniValue::VARR);
            bp.push_back(bhash);
            bp.push_back(2);
            UniValue br = CallMainChainRPC("getblock", bp);
            if (!br.exists("result") || !br["result"].isObject()) { err = "parent chain unreachable"; return false; }
            const UniValue& btime = find_value(br["result"], "time");
            ApplyParentBlock(br["result"], h, btime.isNum() ? btime.get_int64() : 0, mine, set);
            const UniValue& nxt = find_value(br["result"], "nextblockhash");
            next_hash = nxt.isStr() ? nxt.get_str() : std::string();
            last_hash = bhash;
        } catch (const std::exception& e) {
            // Stop where we got to: the record stays consistent at scanned_height,
            // and the next call picks up from there.
            err = std::string("parent chain unreachable: ") + e.what();
            set.scanned_hash = last_hash;
            StoreParentCoins(wallet, set);
            return false;
        }
        set.scanned_height = h;
        set.scanned_hash = last_hash;
    }
    StoreParentCoins(wallet, set);
    return true;
}

//! Dates for coins that have none. A full scan resolves only a bounded number of
//! block times -- two parent RPCs each -- so an old coin can land in the record
//! dateless. Fill a few per call and keep them: the cost is paid once per coin,
//! ever, instead of every time a list is drawn.
bool FillMissingCoinTimes(ParentCoinSet& set)
{
    int budget = 8;
    bool changed = false;
    std::map<int, int64_t> resolved;
    for (ParentCoin& c : set.coins) {
        if (c.time != 0 || c.height <= 0) continue;
        const auto known = resolved.find(c.height);
        if (known != resolved.end()) { c.time = known->second; changed = true; continue; }
        if (budget-- <= 0) break;
        try {
            UniValue hp(UniValue::VARR);
            hp.push_back(c.height);
            UniValue hr = CallMainChainRPC("getblockhash", hp);
            if (!hr.exists("result") || !hr["result"].isStr()) continue;
            UniValue bp(UniValue::VARR);
            bp.push_back(hr["result"].get_str());
            UniValue br = CallMainChainRPC("getblockheader", bp);
            if (br.exists("result") && br["result"].isObject() && br["result"]["time"].isNum()) {
                c.time = br["result"]["time"].get_int64();
                resolved[c.height] = c.time;
                changed = true;
            }
        } catch (const std::exception&) {
            break; // parent unreachable: the dates can wait, the balance cannot
        }
    }
    return changed;
}

//! Defined below: the list of Bitcoin sends this wallet has made.
UniValue LoadParentSends(const CWallet& wallet);

//! Re-derive our unconfirmed spends from the sends we recorded, by asking the
//! parent chain what those transactions actually spend.
//!
//! The record is the usual source for this, but it cannot be the only one: it is
//! rebuilt from a scan when it is missing, unusable or too far behind, and a scan
//! sees only the confirmed chain. A wallet whose record was rebuilt in the minutes
//! after a send would offer that send's coins again -- exactly the double spend
//! the marking exists to stop. The list of our own sends survives all of that, and
//! a transaction still in a mempool will tell us its inputs.
void RecoverPendingSpendsFromSends(const CWallet& wallet, ParentCoinSet& record)
{
    const UniValue sends = LoadParentSends(wallet);
    for (size_t i = 0; i < sends.size(); ++i) {
        const UniValue& s = sends[i];
        if (!s.isObject() || !s["txid"].isStr()) continue;
        const std::string txid_hex = s["txid"].get_str();

        // A send we already marked settled has nothing left to say: its coins are
        // spent on the confirmed chain and its change is in the record. Skipping
        // it is what keeps a rebuild from costing one parent call per send ever
        // made -- which, on a wallet with a long history, is the whole cost.
        if (s["settled"].isBool() && s["settled"].get_bool()) continue;

        // Only transactions still waiting matter. A confirmed one has already had
        // its effect applied by the scan or the block walk.
        UniValue tx;
        try {
            UniValue p(UniValue::VARR);
            p.push_back(txid_hex);
            p.push_back(true);
            UniValue r = CallMainChainRPC("getrawtransaction", p);
            if (!r.exists("result") || !r["result"].isObject()) continue;
            tx = r["result"];
        } catch (const std::exception&) {
            continue; // unreachable, or unknown without a transaction index
        }
        const UniValue& confs = find_value(tx, "confirmations");
        if (confs.isNum() && confs.get_int() > 0) continue;

        const UniValue& vins = find_value(tx, "vin");
        if (vins.isArray()) {
            for (size_t k = 0; k < vins.size(); ++k) {
                const UniValue& pt = find_value(vins[k], "txid");
                const UniValue& pv = find_value(vins[k], "vout");
                if (!pt.isStr() || !pv.isNum()) continue;
                const uint256 ptxid = uint256S(pt.get_str());
                const uint32_t pvout = (uint32_t)pv.get_int();
                for (ParentCoin& c : record.coins) {
                    if (c.txid == ptxid && c.vout == pvout) c.spent_by = txid_hex;
                }
            }
        }

        // The change of that send is ours and is not in the confirmed set either.
        const int change_vout = (s["change_vout"].isNum()) ? s["change_vout"].get_int() : -1;
        if (change_vout < 0) continue;
        const UniValue& vouts = find_value(tx, "vout");
        if (!vouts.isArray() || (size_t)change_vout >= vouts.size()) continue;
        const UniValue& out = vouts[change_vout];
        const uint256 txid = uint256S(txid_hex);
        const bool already = std::any_of(record.coins.begin(), record.coins.end(),
            [&](const ParentCoin& c) { return c.txid == txid && c.vout == (uint32_t)change_vout; });
        if (already) continue;
        ParentCoin c;
        c.txid = txid;
        c.vout = (uint32_t)change_vout;
        c.amount = AmountFromValue(find_value(out, "value"));
        c.height = 0;
        c.time = s["time"].isNum() ? s["time"].get_int64() : GetTime();
        const UniValue& spk = find_value(out, "scriptPubKey");
        if (spk.isObject() && find_value(spk, "hex").isStr()) c.script_hex = find_value(spk, "hex").get_str();
        c.address = s["change_address"].getValStr();
        if (!c.script_hex.empty()) record.coins.push_back(std::move(c));
    }
}

//! The getbtcbalance answer, built from the record instead of from a scan.
UniValue ParentBalanceReply(const ParentCoinSet& set, int naddr)
{
    std::vector<const ParentCoin*> spendable;
    for (const ParentCoin& c : set.coins) {
        if (c.spent_by.empty()) spendable.push_back(&c);
    }
    std::sort(spendable.begin(), spendable.end(),
              [](const ParentCoin* a, const ParentCoin* b) { return a->height > b->height; });

    CAmount total = 0;
    UniValue utxos(UniValue::VARR);
    for (const ParentCoin* c : spendable) {
        total += c->amount;
        if (utxos.size() >= 200) continue; // the list feeds a GUI, the total does not
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", c->txid.GetHex());
        o.pushKV("vout", (int)c->vout);
        o.pushKV("btc", ValueFromAmount(c->amount));
        o.pushKV("height", c->height);
        o.pushKV("confirmations", (c->height > 0 && set.scanned_height >= c->height)
                                  ? set.scanned_height - c->height + 1 : 0);
        o.pushKV("time", c->time);
        o.pushKV("address", c->address);
        utxos.push_back(o);
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("btc", ValueFromAmount(total));
    result.pushKV("addresses", naddr);
    result.pushKV("parent_height", set.scanned_height);
    result.pushKV("utxos", utxos);
    result.pushKV("error", "");
    return result;
}

//! Tell the node what to watch, from the record.
void RegisterWatchFromRecord(const CWallet& wallet, const ParentCoinSet& set)
{
    std::set<std::vector<unsigned char>> scripts;
    for (const auto& item : WalletParentScripts(wallet)) {
        const std::vector<unsigned char> raw = ParseHex(item.first);
        scripts.insert(raw);
    }
    std::set<std::pair<uint256, uint32_t>> outpoints;
    for (const ParentCoin& c : set.coins) outpoints.insert(std::make_pair(c.txid, c.vout));
    SetWatchedParentOutputs(std::move(scripts), std::move(outpoints));
}

} // namespace

RPCHelpMan getbtcbalance()
{
    return RPCHelpMan{"getbtcbalance",
                "\nReturn the Bitcoin (parent-chain) balance held at this wallet's receiving addresses.\n"
                "Sequentia addresses are Bitcoin-identical, so each receiving address is also valid on\n"
                "the Bitcoin parent chain; this scans the parent chain's UTXO set (via the configured\n"
                "-mainchainrpc connection) for unspent outputs paying those addresses. Read-only: the\n"
                "wallet holds the keys, but sending BTC requires a full (non read-only) Bitcoin node.\n",
                {},
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_AMOUNT, "btc", "total unspent parent-chain balance across this wallet's addresses"},
                    {RPCResult::Type::NUM, "addresses", "number of wallet receiving addresses scanned"},
                    {RPCResult::Type::NUM, "parent_height", "the parent-chain height the scan covered"},
                    {RPCResult::Type::ARR, "utxos", "the unspent outputs found, newest first (capped at 200)", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "txid", "the parent-chain transaction id"},
                            {RPCResult::Type::NUM, "vout", "the output index"},
                            {RPCResult::Type::STR_AMOUNT, "btc", "the output's amount"},
                            {RPCResult::Type::NUM, "height", "the parent-chain height it confirmed at"},
                            {RPCResult::Type::NUM, "confirmations", "parent-chain confirmations"},
                            {RPCResult::Type::NUM_TIME, "time", "the block time of that height, 0 when not resolved"},
                            {RPCResult::Type::STR, "address", "the receiving address it paid"},
                        }},
                    }},
                    {RPCResult::Type::STR, "error", "non-empty if the parent chain could not be queried"},
                }},
                RPCExamples{HelpExampleCli("getbtcbalance", "") + HelpExampleRpc("getbtcbalance", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    // Gather this wallet's receiving addresses (Bitcoin-identical) as scan descriptors.
    UniValue descriptors(UniValue::VARR);
    std::set<std::vector<unsigned char>> watched_scripts;
    int naddr = 0;
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& item : pwallet->m_address_book) {
            if (item.second.IsChange()) continue;
            // The parent chain is Bitcoin (testnet4); it cannot parse a Sequentia
            // CONFIDENTIAL address (blech32 "tsqb..."), which Elements hands out by
            // default — scantxoutset would reject the descriptor with "Address is not
            // valid". Strip the blinding pubkey so we encode the UNCONFIDENTIAL,
            // Bitcoin-identical form (bech32 "tb1...", base58 m/n/2...) the parent
            // accepts. The scriptPubKey is identical, so the scan still finds the funds.
            CTxDestination dest = item.first;
            std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
            const std::string addr = EncodeDestination(dest);
            if (addr.empty()) continue;
            descriptors.push_back("addr(" + addr + ")");
            // The same addresses, as raw scripts, for the node to watch new parent
            // blocks with. Unblinded, so byte-identical to what Bitcoin sees.
            const CScript spk = GetScriptForDestination(dest);
            watched_scripts.insert(std::vector<unsigned char>(spk.begin(), spk.end()));
            ++naddr;
        }
    }

    // The fast path, and after the first time the only one: read what we already
    // know, move it forward over the handful of blocks that arrived since, answer.
    // A full scan is what this exists to avoid -- it stays below, for the first
    // reading of a wallet and for a record that cannot be moved forward.
    {
        ParentCoinSet record = LoadParentCoins(*pwallet);
        std::string ferr;
        if (record.loaded && AdvanceParentCoins(*pwallet, record, ferr)) {
            if (FillMissingCoinTimes(record)) StoreParentCoins(*pwallet, record);
            RegisterWatchFromRecord(*pwallet, record);
            return ParentBalanceReply(record, naddr);
        }
        if (record.loaded) {
            // Bitcoin being unreachable is not a reason to report zero. The record
            // holds coins that exist whether or not a daemon is answering right
            // now, and a wallet that shows 0.00 because it cannot ask is worse than
            // one that shows what it last knew: the first looks like theft, the
            // second like weather. Say the balance, and say it may have moved --
            // a full scan below would fail for the same reason anyway.
            if (ferr.find("unreachable") != std::string::npos) {
                UniValue stale = ParentBalanceReply(record, naddr);
                stale.pushKV("stale", true);
                return stale;
            }
            LogPrintf("Bitcoin balance: falling back to a full parent-chain scan: %s\n", ferr);
        }
    }

    UniValue result(UniValue::VOBJ);
    if (naddr == 0) {
        // No receiving addresses handed out yet means the parent-chain balance is a
        // plain zero, not a failure: "error" is reserved for the parent chain being
        // unreachable, and callers (the GUI Overview among them) surface it as such.
        result.pushKV("btc", ValueFromAmount(0));
        result.pushKV("addresses", 0);
        result.pushKV("parent_height", 0);
        result.pushKV("utxos", UniValue(UniValue::VARR));
        return result;
    }

    // Scan the parent chain's UTXO set for those addresses. The cs_wallet lock is
    // released above, so the slow scantxoutset HTTP call does not block the wallet.
    UniValue params(UniValue::VARR);
    params.push_back("start");
    params.push_back(descriptors);
    CAmount total = 0;
    int parent_height = 0;
    std::string err;
    UniValue utxos(UniValue::VARR);
    std::vector<ParentCoin> scanned_coins;
    const int64_t scan_started_ms = GetTimeMillis();
    try {
        UniValue reply = ScanParentChainUtxoSet(params);
        if (reply.exists("error") && !reply["error"].isNull()) {
            const UniValue& e = reply["error"];
            err = (e.isObject() && e.exists("message")) ? e["message"].get_str() : e.write();
        } else if (reply.exists("result") && reply["result"].isObject()) {
            const UniValue& res = reply["result"];
            // The total is summed from the coins below, not read from a field. The
            // parent daemon reports its own total under a name that depends on what
            // it is: bitcoind says total_amount, an Elements-style daemon says
            // total_unblinded_bitcoin_amount, and reading the wrong one yields a
            // silent zero next to a list of coins that are plainly there. Summing
            // what we are about to record also makes the two impossible to
            // disagree -- a balance that does not match its own coins is worse
            // than one that is late.
            if (res.exists("height") && res["height"].isNum()) parent_height = res["height"].get_int();

            // The individual outputs, so a wallet can show WHAT arrived and when, not just
            // the sum. This is the UTXO set, not history: an output disappears from here
            // the moment it is spent, and unconfirmed outputs never appear at all.
            if (res.exists("unspents") && res["unspents"].isArray()) {
                struct Found { std::string txid; int vout; UniValue amount; int height; std::string address; std::string script; };
                std::vector<Found> found;
                for (size_t i = 0; i < res["unspents"].size(); ++i) {
                    const UniValue& u = res["unspents"][i];
                    if (!u.isObject()) continue;
                    Found f;
                    f.txid = (u.exists("txid") && u["txid"].isStr()) ? u["txid"].get_str() : "";
                    f.vout = (u.exists("vout") && u["vout"].isNum()) ? u["vout"].get_int() : 0;
                    if (u.exists("amount")) f.amount = u["amount"];
                    f.height = (u.exists("height") && u["height"].isNum()) ? u["height"].get_int() : 0;
                    // The scriptPubKey, kept because it is the only handle that
                    // survives: the parent chain writes its addresses in Bitcoin
                    // form (tb1...), which this chain's decoder rejects outright,
                    // so an address is not something a spend can work back from.
                    if (u.exists("scriptPubKey") && u["scriptPubKey"].isStr()) f.script = u["scriptPubKey"].get_str();
                    // scantxoutset echoes the matching descriptor as "addr(<address>)#checksum";
                    // recover the plain address from it.
                    if (u.exists("desc") && u["desc"].isStr()) {
                        const std::string d = u["desc"].get_str();
                        const size_t open = d.find("addr(");
                        const size_t close = (open == std::string::npos) ? std::string::npos : d.find(')', open);
                        if (close != std::string::npos) f.address = d.substr(open + 5, close - open - 5);
                    }
                    found.push_back(std::move(f));
                }
                std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.height > b.height; });
                // The record gets every coin, before the list is cut down: a balance
                // built from a truncated record would simply be wrong.
                for (const Found& f : found) {
                    ParentCoin c;
                    c.txid = uint256S(f.txid);
                    c.vout = (uint32_t)f.vout;
                    int64_t amt = 0;
                    if (ParseFixedPoint(f.amount.getValStr(), 8, &amt)) c.amount = amt;
                    c.height = f.height;
                    c.script_hex = f.script;
                    c.address = f.address;
                    scanned_coins.push_back(std::move(c));
                }
                for (const ParentCoin& c : scanned_coins) total += c.amount;
                if (found.size() > 200) found.resize(200); // this feeds a GUI list, not an accounting export
                // Resolve block times for the heights involved (two parent RPCs per distinct
                // height, capped so a wallet with many old outputs cannot stall the call).
                std::map<int, int64_t> height_time;
                for (const Found& f : found) {
                    if (f.height <= 0 || height_time.count(f.height) || height_time.size() >= 24) continue;
                    try {
                        UniValue hp(UniValue::VARR);
                        hp.push_back(f.height);
                        UniValue hr = CallMainChainRPC("getblockhash", hp);
                        if (!hr.exists("result") || !hr["result"].isStr()) continue;
                        UniValue bp(UniValue::VARR);
                        bp.push_back(hr["result"].get_str());
                        UniValue br = CallMainChainRPC("getblockheader", bp);
                        if (br.exists("result") && br["result"].isObject() && br["result"]["time"].isNum()) {
                            height_time[f.height] = br["result"]["time"].get_int64();
                        }
                    } catch (...) {
                        // A missing timestamp is not worth failing the whole scan over.
                    }
                }
                // The record was filled before these were known.
                for (ParentCoin& c : scanned_coins) {
                    const auto it = height_time.find(c.height);
                    if (it != height_time.end()) c.time = it->second;
                }
                for (const Found& f : found) {
                    UniValue o(UniValue::VOBJ);
                    o.pushKV("txid", f.txid);
                    o.pushKV("vout", f.vout);
                    o.pushKV("btc", f.amount);
                    o.pushKV("height", f.height);
                    o.pushKV("confirmations", (f.height > 0 && parent_height >= f.height) ? parent_height - f.height + 1 : 0);
                    const auto it = height_time.find(f.height);
                    o.pushKV("time", it != height_time.end() ? it->second : int64_t{0});
                    o.pushKV("address", f.address);
                    utxos.push_back(o);
                }
            }
        }
    } catch (const std::exception& e) {
        err = std::string("parent chain unreachable: ") + e.what();
    } catch (...) {
        err = "parent chain query failed";
    }

    // Hand the node what to watch, so the NEXT balance does not need a scan at all:
    // it walks every new parent block already, and can tell us when one pays or
    // spends something of ours. Only after a scan that actually succeeded -- a
    // failed one knows nothing, and must not replace what we knew before.
    //
    // The outpoints come from the same list the GUI shows, which is capped; a wallet
    // above that cap still gets every RECEIVE noticed by script, and its spends are
    // caught by the periodic safety refresh.
    if (err.empty()) {
        // Now there is a record, so the next reading costs blocks instead of the
        // whole UTXO set. The hash pins the height: without it a later reorg would
        // be invisible and the record would quietly describe a chain nobody is on.
        ParentCoinSet record;
        record.loaded = true;
        record.scanned_height = parent_height;
        record.coins = std::move(scanned_coins);
        record.full_scan_ms = GetTimeMillis() - scan_started_ms;

        // A scan sees the CONFIRMED chain, so it re-offers every coin we have
        // already committed to a send that is still in the mempool, and knows
        // nothing of that send's change. Carrying both across a rebuild is the
        // whole point of keeping a record: without it, a rebuild -- a reorg, a
        // migration, any riallineamento -- silently re-arms the double spend the
        // record exists to prevent.
        const ParentCoinSet previous = LoadParentCoinsRaw(*pwallet);
        if (previous.loaded) {
            for (const ParentCoin& old : previous.coins) {
                if (!old.spent_by.empty()) {
                    for (ParentCoin& c : record.coins) {
                        if (c.txid == old.txid && c.vout == old.vout) c.spent_by = old.spent_by;
                    }
                } else if (old.height == 0) {
                    // Our own unconfirmed change: the scan cannot see it, and if it
                    // has confirmed meanwhile the scan already listed it.
                    const bool already = std::any_of(record.coins.begin(), record.coins.end(),
                        [&](const ParentCoin& c) { return c.txid == old.txid && c.vout == old.vout; });
                    if (!already) record.coins.push_back(old);
                }
            }
        }
        try {
            UniValue hp(UniValue::VARR);
            hp.push_back(parent_height);
            UniValue hr = CallMainChainRPC("getblockhash", hp);
            if (hr.exists("result") && hr["result"].isStr()) record.scanned_hash = hr["result"].get_str();
        } catch (const std::exception&) {
            // No hash means the next call cannot trust the height and will rescan.
        }
        // ...and for what no record could know, because it was rebuilt after the
        // send rather than before it.
        RecoverPendingSpendsFromSends(*pwallet, record);
        if (!record.scanned_hash.empty()) StoreParentCoins(*pwallet, record);

        std::set<std::pair<uint256, uint32_t>> watched_outpoints;
        for (size_t i = 0; i < utxos.size(); ++i) {
            const UniValue& u = utxos[i];
            if (!u.isObject() || !u["txid"].isStr() || !u["vout"].isNum()) continue;
            watched_outpoints.insert(std::make_pair(uint256S(u["txid"].get_str()),
                                                    (uint32_t)u["vout"].get_int()));
        }
        SetWatchedParentOutputs(std::move(watched_scripts), std::move(watched_outpoints));
    }

    result.pushKV("btc", ValueFromAmount(total));
    result.pushKV("addresses", naddr);
    result.pushKV("parent_height", parent_height);
    result.pushKV("utxos", utxos);
    result.pushKV("error", err);
    return result;
},
    };
}

// SEQUENTIA parent-chain (native Bitcoin) spending. The wallet's receiving keys are
// Bitcoin keys and its unblinded scriptPubKeys are byte-identical on the parent
// chain, so the wallet can spend the Bitcoin found at its own addresses: scan the
// parent UTXO set, build a plain Bitcoin transaction, sign it with the wallet's own
// keys, and hand it to the configured -mainchainrpc node for broadcast. Nothing
// here is a peg: the coins start and stay on Bitcoin.
namespace {

struct ParentUtxo {
    uint256 txid;
    int vout{0};
    CAmount amount{0};
    int height{0};
    int64_t time{0};
    CScript script;
    std::string address;
};

//! Scan the parent chain's UTXO set for this wallet's receiving addresses.
//! Returns false with `err` set when the parent chain cannot be queried.
bool ScanParentUtxos(const CWallet& wallet, std::vector<ParentUtxo>& out, int& parent_height, std::string& err)
{
    UniValue descriptors(UniValue::VARR);
    {
        LOCK(wallet.cs_wallet);
        for (const auto& item : wallet.m_address_book) {
            CTxDestination dest = item.first;
            std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
            const std::string addr = EncodeDestination(dest);
            if (addr.empty()) continue;
            descriptors.push_back("addr(" + addr + ")");
        }
    }
    if (descriptors.empty()) { err = "no receiving addresses yet"; return false; }

    UniValue params(UniValue::VARR);
    params.push_back("start");
    params.push_back(descriptors);
    try {
        // One scan at a time per bitcoind: the balance refresh and a send that
        // both scan can collide. The other scan finishes in seconds, so wait
        // for it rather than bouncing the error to whoever came second.
        UniValue reply = ScanParentChainUtxoSet(params);
        if (reply.exists("error") && !reply["error"].isNull()) {
            const UniValue& e = reply["error"];
            err = (e.isObject() && e.exists("message")) ? e["message"].get_str() : e.write();
            return false;
        }
        if (!reply.exists("result") || !reply["result"].isObject()) { err = "unexpected parent chain response"; return false; }
        const UniValue& res = reply["result"];
        parent_height = (res.exists("height") && res["height"].isNum()) ? res["height"].get_int() : 0;
        if (!res.exists("unspents") || !res["unspents"].isArray()) return true;
        for (size_t i = 0; i < res["unspents"].size(); ++i) {
            const UniValue& u = res["unspents"][i];
            if (!u.isObject()) continue;
            ParentUtxo x;
            if (u.exists("txid") && u["txid"].isStr()) x.txid = uint256S(u["txid"].get_str());
            if (u.exists("vout") && u["vout"].isNum()) x.vout = u["vout"].get_int();
            if (u.exists("amount")) x.amount = AmountFromValue(u["amount"]);
            if (u.exists("height") && u["height"].isNum()) x.height = u["height"].get_int();
            if (u.exists("scriptPubKey") && u["scriptPubKey"].isStr()) {
                const std::vector<unsigned char> raw = ParseHex(u["scriptPubKey"].get_str());
                x.script = CScript(raw.begin(), raw.end());
            }
            if (u.exists("desc") && u["desc"].isStr()) {
                const std::string d = u["desc"].get_str();
                const size_t open = d.find("addr(");
                const size_t close = (open == std::string::npos) ? std::string::npos : d.find(')', open);
                if (close != std::string::npos) x.address = d.substr(open + 5, close - open - 5);
            }
            if (x.script.empty() && !x.address.empty()) {
                // Older parent nodes omit scriptPubKey from scan results; the address
                // is Bitcoin-identical, so our own decoder reconstructs the script.
                CTxDestination d = DecodeDestination(x.address);
                if (IsValidDestination(d)) x.script = GetScriptForDestination(d);
            }
            out.push_back(std::move(x));
        }
        std::sort(out.begin(), out.end(), [](const ParentUtxo& a, const ParentUtxo& b) { return a.amount > b.amount; });
        return true;
    } catch (const std::exception& e) {
        err = std::string("parent chain unreachable: ") + e.what();
        return false;
    }
}

fs::path ParentSendsPath(const CWallet& wallet)
{
    return fs::PathFromString(wallet.GetDatabase().Filename()).parent_path() / "parent_sends.json";
}

UniValue LoadParentSends(const CWallet& wallet)
{
    UniValue arr(UniValue::VARR);
    std::ifstream f(fs::PathToString(ParentSendsPath(wallet)));
    if (!f.good()) return arr;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue parsed;
    if (parsed.read(data) && parsed.isArray()) arr = parsed;
    return arr;
}

void StoreParentSends(const CWallet& wallet, const UniValue& arr)
{
    std::ofstream f(fs::PathToString(ParentSendsPath(wallet)), std::ios::trunc);
    f << arr.write(1) << std::endl;
}

} // namespace

//! The coins a send can choose from, taken from the record. Falls back to a full
//! scan only when there is no record to move forward -- the first send of a wallet,
//! or after a reorg deep enough to throw the record away.
bool ParentUtxosForSpending(const CWallet& wallet, std::vector<ParentUtxo>& out,
                            int& parent_height, std::string& err)
{
    ParentCoinSet record = LoadParentCoins(wallet);
    if (!record.loaded || !AdvanceParentCoins(wallet, record, err)) {
        return ScanParentUtxos(wallet, out, parent_height, err);
    }
    parent_height = record.scanned_height;
    for (const ParentCoin& c : record.coins) {
        // A coin already committed to a send of ours is not available. Offering it
        // again is how a second send became a replacement of the first, refused for
        // not paying more than the transaction it was unknowingly replacing.
        if (!c.spent_by.empty()) continue;
        ParentUtxo u;
        u.txid = c.txid;
        u.vout = (int)c.vout;
        u.amount = c.amount;
        u.height = c.height;
        u.time = c.time;
        u.address = c.address;
        if (!c.script_hex.empty()) {
            const std::vector<unsigned char> raw = ParseHex(c.script_hex);
            u.script = CScript(raw.begin(), raw.end());
        } else {
            // Cannot happen with a record this build wrote, and cannot be repaired
            // here: parent-chain addresses do not decode on this chain.
            continue;
        }
        out.push_back(std::move(u));
    }
    return true;
}

//! After a broadcast: the coins are committed, and the change is ours already.
//! Recording both is what lets the next send happen immediately instead of waiting
//! for a confirmation -- and what stops it colliding with this one.
void RecordParentSpend(const CWallet& wallet,
                       const std::vector<std::pair<uint256, uint32_t>>& spent,
                       const std::string& txid,
                       bool with_change, const std::string& change_address,
                       CAmount change_amount, const CScript& change_script,
                       uint32_t change_vout)
{
    ParentCoinSet record = LoadParentCoins(wallet);
    if (!record.loaded) return; // no record yet: the next balance will build one
    for (ParentCoin& c : record.coins) {
        for (const auto& sp : spent) {
            if (c.txid == sp.first && c.vout == sp.second) c.spent_by = txid;
        }
    }
    // Nothing on the parent chain has happened that a block watcher could see: the
    // send is in a mempool. Ring the same bell anyway, so the overview re-reads the
    // balance on its next tick instead of showing the spent coin until a block
    // arrives -- or, worse, until the half-hour safety refresh.
    NoteParentWatchTouch();
    if (with_change && change_amount > 0) {
        ParentCoin c;
        c.txid = uint256S(txid);
        c.vout = change_vout; // destinations first, change after them
        c.amount = change_amount;
        c.height = 0; // ours, not yet in a block
        c.time = GetTime();
        c.script_hex = HexStr(change_script);
        c.address = change_address;
        record.coins.push_back(std::move(c));
    }
    StoreParentCoins(wallet, record);
}

RPCHelpMan getbtcfeerate()
{
    return RPCHelpMan{"getbtcfeerate",
        "\nThe fee rate a Bitcoin send would use if you do not choose one.\n"
        "\nThis is the parent chain's own estimatesmartfee, in sat/vB, floored at 1. It is worth\n"
        "showing rather than applying silently: an estimator can quote an absurd rate -- testnet4\n"
        "quoted 362 sat/vB on 2026-08-25, which is 0.0005 BTC of fee on a 0.01 BTC send -- and a\n"
        "fee nobody was shown is a fee nobody agreed to.\n",
        {
            {"conf_target", RPCArg::Type::NUM, RPCArg::Default{6}, "Blocks to target."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::NUM, "sat_vb", "the rate a send would use, in sat/vB"},
            {RPCResult::Type::BOOL, "estimated", "true if the parent chain answered; false if this is the 1 sat/vB floor"},
        }},
        RPCExamples{HelpExampleCli("getbtcfeerate", "") + HelpExampleRpc("getbtcfeerate", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int conf_target = 6;
    if (!request.params[0].isNull()) conf_target = request.params[0].get_int();
    if (conf_target < 1) conf_target = 1;

    CAmount sat_per_vb = 0;
    bool estimated = false;
    try {
        UniValue p(UniValue::VARR);
        p.push_back(conf_target);
        UniValue r = CallMainChainRPC("estimatesmartfee", p);
        if (r.exists("result") && r["result"].isObject() && r["result"].exists("feerate")) {
            const CAmount per_kvb = AmountFromValue(r["result"]["feerate"]);
            sat_per_vb = per_kvb / 1000;
            estimated = sat_per_vb > 0;
        }
    } catch (const std::exception&) {
        // Unreachable parent: the floor is still a usable answer, and the caller
        // learns it was not an estimate.
    }
    if (sat_per_vb < 1) sat_per_vb = 1;

    UniValue result(UniValue::VOBJ);
    result.pushKV("sat_vb", (int64_t)sat_per_vb);
    result.pushKV("estimated", estimated);
    return result;
}};
}

RPCHelpMan sendbtctoaddress()
{
    return RPCHelpMan{"sendbtctoaddress",
        "\nSend native Bitcoin (parent-chain) from this wallet's addresses.\n"
        "The wallet's receiving addresses are Bitcoin-identical, so Bitcoin received at them is\n"
        "spendable with the wallet's own keys. This scans the parent chain for those coins, builds\n"
        "and signs an ordinary Bitcoin transaction, and broadcasts it through the configured\n"
        "-mainchainrpc node. This is native Bitcoin on its own chain, not a peg and not a\n"
        "Sequentia asset; fees are paid in bitcoin out of the coins being spent.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination, a bech32 (segwit) address. Sequentia and Bitcoin share the address format."},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in BTC to send."},
            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED_NAMED_ARG, "Fee rate in sat/vB. Defaults to the parent chain's estimatesmartfee, floor 1 sat/vB."},
            {"subtractfeefromamount", RPCArg::Type::BOOL, RPCArg::Default{false}, "Deduct the fee from the amount instead of adding it on top."},
            {"estimate_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Select coins and compute the fee, then stop: nothing is signed, broadcast, or recorded. For showing the fee before sending."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "txid", "the parent-chain transaction id (absent with estimate_only)"},
            {RPCResult::Type::STR_AMOUNT, "fee", "the bitcoin fee paid (or, with estimate_only, the fee that would be)"},
            {RPCResult::Type::NUM, "inputs", "how many parent-chain coins were spent"},
            {RPCResult::Type::NUM, "fee_rate", "the sat/vB rate used"},
        }},
        RPCExamples{HelpExampleCli("sendbtctoaddress", "\"tb1q...\" 0.01") + HelpExampleRpc("sendbtctoaddress", "\"tb1q...\", 0.01")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    EnsureWalletIsUnlocked(*pwallet);

    // Destinations: the unconfidential, Bitcoin-identical form only. Confidential
    // addresses have no meaning on the parent chain, and legacy base58 is not
    // something this wallet ever hands out.
    //
    // One transaction may pay several destinations -- that is ordinary on Bitcoin,
    // and it is one transaction rather than one each, so it costs one fee and one
    // set of inputs instead of N.
    struct ParentRecipient { CScript script; std::string address; CAmount amount{0}; };
    std::vector<ParentRecipient> recipients;

    auto add_recipient = [&](const std::string& addr_str, const UniValue& amount_val) {
        CTxDestination dest = DecodeDestination(addr_str);
        std::visit(SetBlindingPubKeyVisitor(CPubKey()), dest);
        if (!IsValidDestination(dest) ||
            (!std::holds_alternative<WitnessV0KeyHash>(dest) && !std::holds_alternative<WitnessV0ScriptHash>(dest) &&
             !std::holds_alternative<WitnessV1Taproot>(dest))) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Bitcoin travels to a bech32 address (tb1.../bc1...); give one of those");
        }
        const CAmount amt = AmountFromValue(amount_val);
        if (amt <= 0) throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
        recipients.push_back({GetScriptForDestination(dest), EncodeDestination(dest), amt});
    };

    if (request.params[0].isArray()) {
        const UniValue& list = request.params[0];
        if (list.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "No destinations given");
        for (size_t i = 0; i < list.size(); ++i) {
            const UniValue& o = list[i];
            if (!o.isObject() || !o["address"].isStr() || o["amount"].isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Each destination needs an address and an amount");
            }
            add_recipient(o["address"].get_str(), o["amount"]);
        }
    } else {
        if (request.params[1].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "No amount given");
        add_recipient(request.params[0].get_str(), request.params[1]);
    }

    CAmount send_amount = 0;
    for (const ParentRecipient& r : recipients) send_amount += r.amount;

    const bool subtract_fee = !request.params[3].isNull() && request.params[3].get_bool();
    // Deducting the fee from an amount is unambiguous with one destination and a
    // choice with several -- whose payment shrinks? Rather than decide for the
    // caller and move somebody money quietly, refuse and let them say.
    if (subtract_fee && recipients.size() > 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "subtractfeefromamount needs one destination: with several, say which amounts to reduce by lowering them yourself");
    }

    // Fee rate, sat/vB: caller's, else the parent chain's own estimate, floor 1.
    CAmount sat_per_vb = 0;
    if (!request.params[2].isNull()) {
        sat_per_vb = request.params[2].get_int64();
        if (sat_per_vb < 1) throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_rate must be at least 1 sat/vB");
    } else {
        try {
            UniValue p(UniValue::VARR);
            p.push_back(6);
            UniValue r = CallMainChainRPC("estimatesmartfee", p);
            if (r.exists("result") && r["result"].isObject() && r["result"].exists("feerate")) {
                const CAmount per_kvb = AmountFromValue(r["result"]["feerate"]);
                sat_per_vb = std::max<CAmount>(1, per_kvb / 1000);
            }
        } catch (...) {}
        if (sat_per_vb < 1) sat_per_vb = 1;
    }

    std::vector<ParentUtxo> coins;
    int parent_height = 0;
    std::string err;
    if (!ParentUtxosForSpending(*pwallet, coins, parent_height, err)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Cannot reach the Bitcoin parent chain: " + err);
    }

    // Spendable = confirmed P2WPKH coins whose key the wallet actually holds. Coins the
    // scan found but this wallet cannot sign for (watch-only entries) stay untouched.
    struct Selected { ParentUtxo utxo; CKey key; CPubKey pubkey; };
    std::vector<Selected> spendable;
    CAmount available = 0;
    CAmount unconfirmed_own = 0;
    for (const ParentUtxo& u : coins) {
        if (u.amount <= 0) continue;
        // Height 0 means our own change, from a send of ours that has not been
        // mined yet -- nothing else can get into the record without a block. It is
        // ours and it is spendable: refusing it would make every send wait for a
        // confirmation, which is exactly the wait this record was built to remove.
        // The transaction that spends it simply confirms after its parent, which is
        // ordinary on Bitcoin.
        CKey key; CPubKey pubkey;
        if (!GetWalletKeyForP2WPKH(*pwallet, u.script, key, pubkey)) continue;
        if (u.height == 0) unconfirmed_own += u.amount;
        spendable.push_back({u, key, pubkey});
        available += u.amount;
    }
    if (available < send_amount) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
            strprintf("Insufficient Bitcoin: %s available at this wallet's addresses%s, %s requested",
                      FormatMoney(available),
                      unconfirmed_own > 0 ? strprintf(" (%s of it change not yet mined)", FormatMoney(unconfirmed_own)) : "",
                      FormatMoney(send_amount)));
    }

    // Largest-first selection with an iterated fee: vsize ~= 11 + 68 per input + 31 per
    // output. Change below dust (546 sat) folds into the fee rather than creating an
    // output the parent chain would refuse to relay.
    static constexpr CAmount PARENT_DUST{546};
    std::vector<Selected> picked;
    CAmount in_sum = 0, fee = 0, change = 0;
    bool with_change = false;
    for (const Selected& s : spendable) {
        picked.push_back(s);
        in_sum += s.utxo.amount;
        const int outs_max = (int)recipients.size() + 1;   // destinations plus change
        for (int outs = outs_max; outs >= outs_max - 1; --outs) {
            const CAmount f = sat_per_vb * (11 + 68 * (CAmount)picked.size() + 31 * outs);
            const CAmount need = subtract_fee ? send_amount : send_amount + f;
            if (in_sum >= need) {
                fee = f;
                with_change = (outs == outs_max);
                change = with_change ? in_sum - need : 0;
                if (with_change && change < PARENT_DUST) { with_change = false; continue; }
                goto selected;
            }
        }
    }
    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
        strprintf("Insufficient Bitcoin once the fee is counted: %s available, %s requested at %d sat/vB",
                  FormatMoney(available), FormatMoney(send_amount), sat_per_vb));
selected:

    const CAmount to_dest = subtract_fee ? send_amount - fee : send_amount;
    if (to_dest <= PARENT_DUST) throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount after the fee would be dust on the parent chain");
    // With one destination the deduction lands on it; with several there is no
    // deduction at all (refused above), so each is paid exactly what was asked.
    if (subtract_fee) recipients.front().amount = to_dest;

    // Bitcoin's safety ceiling, kept for Bitcoin. Sequentia removed the default fee
    // ceiling because fees there can be paid in any asset, where one number cannot
    // mean anything -- but this fee is bitcoin, priced by the parent chain's own
    // estimator, and that estimator can return absurdities: testnet4 was quoting 362
    // sat/vB on 2026-08-25, which turned a 0.01 BTC send into a 0.0005 BTC fee. A
    // ceiling is the difference between an expensive send and a lost coin.
    if (fee > DEFAULT_TRANSACTION_MAXFEE) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Fee of %s BTC is above the %s BTC safety ceiling for Bitcoin sends "
                      "(the parent chain estimated %d sat/vB). Pass fee_rate to choose the rate yourself.",
                      FormatMoney(fee), FormatMoney(DEFAULT_TRANSACTION_MAXFEE), sat_per_vb));
    }
    if (!with_change) fee = in_sum - to_dest; // no change output: the remainder is the fee

    // The estimate stops here: selection and fee are decided, nothing has been
    // signed, no change address consumed, nothing recorded or broadcast.
    if (!request.params[4].isNull() && request.params[4].get_bool()) {
        UniValue est(UniValue::VOBJ);
        est.pushKV("fee", ValueFromAmount(fee));
        est.pushKV("inputs", (int)picked.size());
        est.pushKV("fee_rate", sat_per_vb);
        return est;
    }

    // The change returns to a fresh address of this wallet, which the parent-chain scan
    // covers, so the remainder reappears in the balance and the send's confirmations can
    // be read off it.
    CScript change_script;
    std::string change_address;
    if (with_change) {
        CTxDestination change_dest;
        bilingual_str dest_err;
        if (!pwallet->GetNewDestination(OutputType::BECH32, "", change_dest, dest_err)) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, dest_err.original);
        }
        std::visit(SetBlindingPubKeyVisitor(CPubKey()), change_dest);
        change_script = GetScriptForDestination(change_dest);
        change_address = EncodeDestination(change_dest);
    }

    Sidechain::Bitcoin::CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = 0;
    for (const Selected& s : picked) {
        Sidechain::Bitcoin::CTxIn in;
        in.prevout = Sidechain::Bitcoin::COutPoint(s.utxo.txid, s.utxo.vout);
        in.nSequence = 0xfffffffd; // opt-in RBF, as every wallet send
        mtx.vin.push_back(in);
    }
    for (const ParentRecipient& r : recipients) {
        Sidechain::Bitcoin::CTxOut out;
        out.nValue = r.amount;
        out.scriptPubKey = r.script;
        mtx.vout.push_back(out);
    }
    if (with_change) {
        Sidechain::Bitcoin::CTxOut out;
        out.nValue = change;
        out.scriptPubKey = change_script;
        mtx.vout.push_back(out);
    }

    for (size_t i = 0; i < picked.size(); ++i) {
        const Selected& s = picked[i];
        int version = 0;
        std::vector<unsigned char> program;
        s.utxo.script.IsWitnessProgram(version, program);
        CScript script_code;
        script_code << OP_DUP << OP_HASH160 << program << OP_EQUALVERIFY << OP_CHECKSIG;
        const uint256 sighash = ParentBip143Sighash(mtx, i, script_code, s.utxo.amount);
        std::vector<unsigned char> sig;
        if (!s.key.Sign(sighash, sig)) throw JSONRPCError(RPC_WALLET_ERROR, "Signing the parent-chain transaction failed");
        sig.push_back((unsigned char)SIGHASH_ALL);
        mtx.vin[i].scriptWitness.stack = {sig, std::vector<unsigned char>(s.pubkey.begin(), s.pubkey.end())};
    }

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << mtx;
    const std::string tx_hex = HexStr(ssTx);

    std::string txid;
    try {
        UniValue p(UniValue::VARR);
        p.push_back(tx_hex);
        UniValue r = CallMainChainRPC("sendrawtransaction", p);
        if (r.exists("error") && !r["error"].isNull()) {
            const UniValue& e = r["error"];
            throw JSONRPCError(RPC_MISC_ERROR, "The Bitcoin node refused the transaction: " +
                ((e.isObject() && e.exists("message")) ? e["message"].get_str() : e.write()));
        }
        if (!r.exists("result") || !r["result"].isStr()) throw JSONRPCError(RPC_MISC_ERROR, "Unexpected parent chain response to broadcast");
        txid = r["result"].get_str();
    } catch (const UniValue& e) {
        throw;
    } catch (const std::exception& e) {
        throw JSONRPCError(RPC_MISC_ERROR, std::string("Cannot reach the Bitcoin parent chain to broadcast: ") + e.what());
    }

    // The record, immediately: these coins are committed and this change is ours.
    // Until this existed, the wallet asked the parent chain what it owned and the
    // parent chain answered with CONFIRMED coins only -- so a second send within the
    // same block chose the same coins again and Bitcoin refused it as a replacement
    // that paid no more than what it replaced.
    {
        std::vector<std::pair<uint256, uint32_t>> spent;
        for (const Selected& sel : picked) {
            spent.push_back(std::make_pair(sel.utxo.txid, (uint32_t)sel.utxo.vout));
        }
        RecordParentSpend(*pwallet, spent, txid, with_change, change_address, change, change_script, (uint32_t)recipients.size());
    }

    // The send becomes part of this wallet's Bitcoin history. The scan alone cannot
    // reconstruct it (spent coins leave the UTXO set), so it is recorded beside the
    // wallet; listbtctransactions folds these records into the unified history.
    {
        UniValue sends = LoadParentSends(*pwallet);
        UniValue rec(UniValue::VOBJ);
        rec.pushKV("txid", txid);
        rec.pushKV("time", GetTime());
        // Every destination, not just the first: a transaction that paid three
        // people is three lines of history, and a record that keeps one of them
        // loses the other two for good -- the parent chain cannot give them back
        // once the outputs are spent.
        rec.pushKV("address", recipients.front().address);
        rec.pushKV("btc", ValueFromAmount(to_dest));
        if (recipients.size() > 1) {
            UniValue dests(UniValue::VARR);
            for (const ParentRecipient& r : recipients) {
                UniValue d(UniValue::VOBJ);
                d.pushKV("address", r.address);
                d.pushKV("btc", ValueFromAmount(r.amount));
                dests.push_back(d);
            }
            rec.pushKV("destinations", dests);
        }
        rec.pushKV("fee", ValueFromAmount(fee));
        rec.pushKV("change_vout", with_change ? (int)recipients.size() : -1);
        rec.pushKV("change_address", change_address);
        sends.push_back(rec);
        StoreParentSends(*pwallet, sends);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid);
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("inputs", (int)picked.size());
    result.pushKV("fee_rate", sat_per_vb);
    return result;
},
    };
}

RPCHelpMan listbtctransactions()
{
    return RPCHelpMan{"listbtctransactions",
        "\nThe wallet's Bitcoin (parent-chain) history: coins received at its addresses and\n"
        "sends made with sendbtctoaddress, newest first. Receives come from scanning the\n"
        "parent UTXO set, so a received coin later spent elsewhere leaves the list; sends\n"
        "are recorded when made and their confirmations read from the parent chain.\n",
        {
            {"scan", RPCArg::Type::BOOL, RPCArg::Default{true}, "Scan the parent chain for receives. false returns only the recorded sends, cheaply, for a caller that already holds a fresh scan."},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {
            {RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR, "category", "\"receive\" or \"send\""},
                {RPCResult::Type::STR_HEX, "txid", "the parent-chain transaction id"},
                {RPCResult::Type::STR, "address", "the address involved"},
                {RPCResult::Type::STR_AMOUNT, "btc", "the amount (positive for receive, the amount sent for send)"},
                {RPCResult::Type::STR_AMOUNT, "fee", "the fee paid (sends only)"},
                {RPCResult::Type::NUM_TIME, "time", "block time for receives, broadcast time for sends"},
                {RPCResult::Type::NUM, "confirmations", "parent-chain confirmations; 0 in the mempool, -1 unknown"},
            }},
        }},
        RPCExamples{HelpExampleCli("listbtctransactions", "") + HelpExampleRpc("listbtctransactions", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    UniValue rows(UniValue::VARR);
    const bool want_scan = request.params[0].isNull() || request.params[0].get_bool();
    std::vector<ParentUtxo> coins;
    int parent_height = 0;
    std::string err;
    const bool scanned = want_scan && ScanParentUtxos(*pwallet, coins, parent_height, err);

    const UniValue sends = LoadParentSends(*pwallet);
    // Read once, not once per send. Inside the loop this re-parsed the whole
    // record for every transaction whose outputs were already spent -- which,
    // given time, is nearly all of them -- so the cost grew with the SQUARE of
    // the history. A thousand sends meant a thousand reads of the same file.
    const ParentCoinSet record_for_confs = LoadParentCoinsRaw(*pwallet);
    std::map<size_t, int> newly_settled;   // index -> the height it settled at
    std::set<std::string> send_txids;
    for (size_t i = 0; i < sends.size(); ++i) {
        if (sends[i].isObject() && sends[i].exists("txid")) send_txids.insert(sends[i]["txid"].get_str());
    }

    if (scanned) {
        // Times for receive rows, as getbtcbalance resolves them (capped).
        std::map<int, int64_t> height_time;
        for (const ParentUtxo& u : coins) {
            if (u.height <= 0 || height_time.count(u.height) || height_time.size() >= 24) continue;
            try {
                UniValue hp(UniValue::VARR);
                hp.push_back(u.height);
                UniValue hr = CallMainChainRPC("getblockhash", hp);
                if (!hr.exists("result") || !hr["result"].isStr()) continue;
                UniValue bp(UniValue::VARR);
                bp.push_back(hr["result"].get_str());
                UniValue br = CallMainChainRPC("getblockheader", bp);
                if (br.exists("result") && br["result"].isObject() && br["result"]["time"].isNum()) {
                    height_time[u.height] = br["result"]["time"].get_int64();
                }
            } catch (...) {}
        }
        for (const ParentUtxo& u : coins) {
            // A send's change lands back at our own address; the scan sees it as a fresh
            // coin, but as HISTORY it is part of the send, not a receive of its own.
            if (send_txids.count(u.txid.GetHex())) continue;
            UniValue o(UniValue::VOBJ);
            o.pushKV("category", "receive");
            o.pushKV("txid", u.txid.GetHex());
            o.pushKV("address", u.address);
            o.pushKV("btc", ValueFromAmount(u.amount));
            const auto it = height_time.find(u.height);
            o.pushKV("time", it != height_time.end() ? it->second : int64_t{0});
            o.pushKV("confirmations", (u.height > 0 && parent_height >= u.height) ? parent_height - u.height + 1 : 0);
            rows.push_back(o);
        }
    }

    // Sends, with confirmations read from the parent chain via the change output
    // (gettxout answers for unspent outputs, mempool included).
    for (size_t i = 0; i < sends.size(); ++i) {
        if (!sends[i].isObject()) continue;
        const UniValue& s = sends[i];
        UniValue o(UniValue::VOBJ);
        o.pushKV("category", "send");
        o.pushKV("txid", s.exists("txid") ? s["txid"].get_str() : "");
        o.pushKV("address", s.exists("address") ? s["address"].get_str() : "");
        o.pushKV("btc", s.exists("btc") ? s["btc"] : UniValue(UniValue::VNUM));
        if (s.exists("fee")) o.pushKV("fee", s["fee"]);
        o.pushKV("time", s.exists("time") ? s["time"].get_int64() : int64_t{0});
        // Confirmations come from whichever of the send's outputs is still
        // unspent: the change when it exists and has not itself been spent,
        // else the destination (vout 0), which stays unspent until the
        // recipient moves it. A send with every output spent is by then deep
        // in the chain; report it as confirmed rather than unknown.
        int confs = -1;

        // A settled send is never asked about again. Marking it was only half the
        // saving: the calls have to stop as well, and for that its confirmations
        // must be computable without asking -- hence the height it settled at.
        const bool settled = s["settled"].isBool() && s["settled"].get_bool();
        if (settled) {
            // Buried is buried, with or without the height: a record written before
            // the height was kept must not go back to costing a call per read.
            const int h = s["settled_height"].isNum() ? s["settled_height"].get_int() : 0;
            confs = (h > 0 && record_for_confs.scanned_height >= h)
                    ? record_for_confs.scanned_height - h + 1 : 6;
        } else if (s.exists("txid")) {
            const int change_vout = (s.exists("change_vout") && s["change_vout"].isNum()) ? s["change_vout"].get_int() : -1;
            std::vector<int> candidates;
            if (change_vout >= 0) candidates.push_back(change_vout);
            candidates.push_back(0);
            for (int vout : candidates) {
                try {
                    UniValue p(UniValue::VARR);
                    p.push_back(s["txid"].get_str());
                    p.push_back(vout);
                    p.push_back(true);
                    UniValue r = CallMainChainRPC("gettxout", p);
                    if (r.exists("result") && r["result"].isObject() && r["result"].exists("confirmations")) {
                        confs = r["result"]["confirmations"].get_int();
                        break;
                    }
                } catch (...) {}
            }
            // gettxout only answers about outputs that still EXIST. Once a later
            // send spends them, this says nothing about the earlier one -- which
            // is how a transaction with 42 confirmations came to be reported as
            // unknown, minutes after its change was spent. The record kept the
            // height of every coin it ever saw, including the ones now spent.
            if (confs < 0) {
                const ParentCoinSet& rec = record_for_confs;
                if (rec.loaded && rec.scanned_height > 0) {
                    const uint256 txid = uint256S(s["txid"].get_str());
                    for (const ParentCoin& c : rec.coins) {
                        if (c.txid != txid) continue;
                        confs = (c.height > 0) ? rec.scanned_height - c.height + 1 : 0;
                        break;
                    }
                }
            }
        }
        // Six confirmations is buried: the answer will not change again. Note which
        // sends reached it and write them down ONCE after the loop -- without this,
        // opening the tab cost two parent calls per send for ever.
        if (confs >= 6 && !settled) {
            newly_settled[i] = record_for_confs.scanned_height > 0
                               ? record_for_confs.scanned_height - confs + 1 : 0;
        }
        o.pushKV("confirmations", confs);

        // One row per destination. The fee belongs to the transaction, not to any
        // one payment, so it rides on the first row only and is not counted three
        // times by anyone adding the column up.
        if (s.exists("destinations") && s["destinations"].isArray() && s["destinations"].size() > 1) {
            const UniValue& dests = s["destinations"];
            for (size_t d = 0; d < dests.size(); ++d) {
                if (!dests[d].isObject()) continue;
                UniValue row = o;
                row.pushKV("address", dests[d]["address"].getValStr());
                row.pushKV("btc", dests[d]["btc"].getValStr());
                if (d > 0) row.pushKV("fee", "0.00000000");
                rows.push_back(row);
            }
            continue;
        }
        rows.push_back(o);
    }

    // One write for however many sends became buried during this read. UniValue
    // has no in-place element assignment, so the array is rebuilt -- which is
    // still one pass and one write, rather than one file write per send.
    if (!newly_settled.empty()) {
        UniValue updated(UniValue::VARR);
        for (size_t k = 0; k < sends.size(); ++k) {
            const auto it_settled = newly_settled.find(k);
            if (it_settled != newly_settled.end() && sends[k].isObject()) {
                UniValue r = sends[k];
                r.pushKV("settled", true);
                if (it_settled->second > 0) r.pushKV("settled_height", it_settled->second);
                updated.push_back(r);
            } else {
                updated.push_back(sends[k]);
            }
        }
        StoreParentSends(*pwallet, updated);
    }

    // Newest first, whatever chain the clock came from.
    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&rows](size_t a, size_t b) {
        const int64_t ta = rows[a].exists("time") ? rows[a]["time"].get_int64() : 0;
        const int64_t tb = rows[b].exists("time") ? rows[b]["time"].get_int64() : 0;
        return ta > tb;
    });
    UniValue sorted(UniValue::VARR);
    for (size_t i : order) sorted.push_back(rows[i]);
    return sorted;
},
    };
}


/**
 * Update coin control with fee estimation based on the given parameters
 *
 * @param[in]     wallet            Wallet reference
 * @param[in,out] cc                Coin control to be updated
 * @param[in]     conf_target       UniValue integer; confirmation target in blocks, values between 1 and 1008 are valid per policy/fees.h;
 * @param[in]     estimate_mode     UniValue string; fee estimation mode, valid values are "unset", "economical" or "conservative";
 * @param[in]     fee_rate          UniValue real; fee rate in sat/vB;
 *                                      if present, both conf_target and estimate_mode must either be null, or "unset"
 * @param[in]     override_min_fee  bool; whether to set fOverrideFeeRate to true to disable minimum fee rate checks and instead
 *                                      verify only that fee_rate is greater than 0
 * @throws a JSONRPCError if conf_target, estimate_mode, or fee_rate contain invalid values or are in conflict
 */
static void SetFeeEstimateMode(const CWallet& wallet, CCoinControl& cc, const UniValue& conf_target, const UniValue& estimate_mode, const UniValue& fee_rate, bool override_min_fee)
{
    if (!fee_rate.isNull()) {
        if (!conf_target.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and fee_rate. Please provide either a confirmation target in blocks for automatic fee estimation, or an explicit fee rate.");
        }
        if (!estimate_mode.isNull() && estimate_mode.get_str() != "unset") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and fee_rate");
        }
        // Fee rates in sat/vB cannot represent more than 3 significant digits.
        cc.m_feerate = CFeeRate{AmountFromValue(fee_rate, /*check_range=*/true, /*decimals=*/3)};
        if (override_min_fee) cc.fOverrideFeeRate = true;
        // Default RBF to true for explicit fee_rate, if unset.
        if (!cc.m_signal_bip125_rbf) cc.m_signal_bip125_rbf = true;
        return;
    }
    if (!estimate_mode.isNull() && !FeeModeFromString(estimate_mode.get_str(), cc.m_fee_mode)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, InvalidEstimateModeErrorMessage());
    }
    if (!conf_target.isNull()) {
        cc.m_confirm_target = ParseConfirmTarget(conf_target, wallet.chain().estimateMaxBlocks());
    }
}

RPCHelpMan sendtoaddress()
{
    return RPCHelpMan{"sendtoaddress",
                "\nSend an amount to a given address." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address to send to."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment used to store what the transaction is for.\n"
                                         "This is not part of the transaction, just kept in your wallet."},
                    {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment to store the name of the person or organization\n"
                                         "to which you're sending the transaction. This is not part of the \n"
                                         "transaction, just kept in your wallet."},
                    {"subtractfeefromamount", RPCArg::Type::BOOL, RPCArg::Default{false}, "The fee will be deducted from the amount being sent.\n"
                                         "The recipient will receive less bitcoins than you enter in the amount field."},
                    {"replaceable", RPCArg::Type::BOOL, RPCArg::DefaultHint{"wallet default"}, "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks"},
                    {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, std::string() + "The fee estimate mode, must be one of (case insensitive):\n"
            "       \"" + FeeModes("\"\n\"") + "\""},
                    {"avoid_reuse", RPCArg::Type::BOOL, RPCArg::Default{true}, "(only available if avoid_reuse wallet flag is set) Avoid spending from dirty addresses; addresses are considered\n"
                                         "dirty if they have previously been used in a transaction. If true, this also activates avoidpartialspends, grouping outputs by their addresses."},
                    {"assetlabel", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Hex asset id or asset label for balance."},
                    {"ignoreblindfail", RPCArg::Type::BOOL, RPCArg::Default{true}, "Return a transaction even when a blinding attempt fails due to number of blinded inputs/outputs."},
                    {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                    {"fee_asset_label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Hex asset id or asset label for fee payment. On a chain with the open fee market (con_any_asset_fees) the fee asset must be named, because nothing is defaulted or inferred -- unless subtractfeefromamount is set, which takes the fee out of that output and so already determines it, in which case this must be omitted."},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, return extra information about the transaction."},
                },
                {
                    RPCResult{"if verbose is not set or set to false",
                        RPCResult::Type::STR_HEX, "txid", "The transaction id."
                    },
                    RPCResult{"if verbose is set to true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "txid", "The transaction id."},
                            {RPCResult::Type::STR, "fee_reason", "The transaction fee reason."}
                        },
                    },
                },
                RPCExamples{
                    "\nSend 0.1 BTC\n"
                    + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 0.1") +
                    "\nSend 0.1 BTC with a confirmation target of 6 blocks in economical fee estimate mode using positional arguments\n"
                    + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 0.1 \"donation\" \"sean's outpost\" false true 6 economical") +
                    "\nSend 0.1 BTC with a fee rate of 1.1 " + CURRENCY_ATOM + "/vB, subtract fee from amount, BIP125-replaceable, using positional arguments\n"
                    + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 0.1 \"drinks\" \"room77\" true true null \"unset\" null 1.1") +
                    "\nSend 0.2 BTC with a confirmation target of 6 blocks in economical fee estimate mode using named arguments\n"
                    + HelpExampleCli("-named sendtoaddress", "address=\"" + EXAMPLE_ADDRESS[0] + "\" amount=0.2 conf_target=6 estimate_mode=\"economical\"") +
                    "\nSend 0.5 BTC with a fee rate of 25 " + CURRENCY_ATOM + "/vB using named arguments\n"
                    + HelpExampleCli("-named sendtoaddress", "address=\"" + EXAMPLE_ADDRESS[0] + "\" amount=0.5 fee_rate=25")
                    + HelpExampleCli("-named sendtoaddress", "address=\"" + EXAMPLE_ADDRESS[0] + "\" amount=0.5 fee_rate=25 subtractfeefromamount=false replaceable=true avoid_reuse=true comment=\"2 pizzas\" comment_to=\"jeremy\" verbose=true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    // Wallet comments
    mapValue_t mapValue;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["to"] = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (!request.params[4].isNull()) {
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    coin_control.m_avoid_address_reuse = GetAvoidReuseFlag(*pwallet, request.params[8]);
    // We also enable partial spend avoidance if reuse avoidance is set.
    coin_control.m_avoid_partial_spends |= coin_control.m_avoid_address_reuse;

    std::string strasset = Params().GetConsensus().pegged_asset.GetHex();
    if (request.params.size() > 9 && request.params[9].isStr() && !request.params[9].get_str().empty()) {
        strasset = request.params[9].get_str();
    }
    CAsset asset = GetAssetFromString(strasset);
    if (asset.IsNull() && g_con_elementsmode) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex: %s", asset.GetHex()));
    }

    bool ignore_blind_fail = true;
    if (!request.params[10].isNull()) {
        ignore_blind_fail = request.params[10].get_bool();
    }

    SetFeeEstimateMode(*pwallet, coin_control, /* conf_target */ request.params[6], /* estimate_mode */ request.params[7], /* fee_rate */ request.params[11], /* override_min_fee */ false);

    // SEQUENTIA: the fee asset is the caller's choice and is never inferred from
    // what the transaction happens to send. This used to default to `asset`, the
    // asset being sent; a silent default is a policy decision made out of the
    // caller's sight, and any default at all makes some asset the privileged fee
    // currency. See ResolveFeeAsset (wallet/rpc/util.h) for the settled rule.
    const std::optional<CAsset> explicit_fee_asset =
        g_con_any_asset_fees && request.params.size() > 12 ? ParseFeeAssetArg(request.params[12]) : std::nullopt;

    EnsureWalletIsUnlocked(*pwallet);

    UniValue address_amounts(UniValue::VOBJ);
    UniValue address_assets(UniValue::VOBJ);
    const std::string address = request.params[0].get_str();
    address_amounts.pushKV(address, request.params[1]);
    address_assets.pushKV(address, asset.GetHex());
    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (fSubtractFeeFromAmount) {
        subtractFeeFromAmount.push_back(address);
    }

    std::vector<CRecipient> recipients;
    ParseRecipients(address_amounts, address_assets, subtractFeeFromAmount, recipients);
    if (g_con_any_asset_fees) {
        coin_control.m_fee_asset = ResolveFeeAsset(explicit_fee_asset, DeterminedBySubtractFee(SubtractFeeFromAssets(recipients)), "fee_asset_label");
    }
    bool verbose = request.params[13].isNull() ? false: request.params[13].get_bool();

    return SendMoney(*pwallet, coin_control, recipients, mapValue, verbose, ignore_blind_fail);
},
    };
}

RPCHelpMan sendmany()
{
    return RPCHelpMan{"sendmany",
                "\nSend multiple times. Amounts are double-precision floating point numbers." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::NO, "Must be set to \"\" for backwards compatibility.", "\"\""},
                    {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"minconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "Ignored dummy value"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment"},
                    {"subtractfeefrom", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "The addresses.\n"
                                       "The fee will be equally deducted from the amount of each selected address.\n"
                                       "Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
                                       "If no addresses are specified here, the sender pays the fee.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Subtract fee from this address"},
                        },
                    },
                    {"replaceable", RPCArg::Type::BOOL, RPCArg::DefaultHint{"wallet default"}, "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks"},
                    {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, std::string() + "The fee estimate mode, must be one of (case insensitive):\n"
            "       \"" + FeeModes("\"\n\"") + "\""},
                    {"output_assets", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "A json object of addresses to assets.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "A key-value pair where the key is the address used and the value is an asset label or hex asset ID."},
                        },
                    },
                    {"ignoreblindfail", RPCArg::Type::BOOL, RPCArg::Default{true}, "Return a transaction even when a blinding attempt fails due to number of blinded inputs/outputs."},
                    {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                    {"fee_asset", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED_NAMED_ARG, "Label or hex ID of the asset used for fees. On a chain with the open fee market (con_any_asset_fees) the fee asset must be named, because nothing is defaulted or inferred -- unless subtractfeefrom is used, which takes the fee out of those outputs and so already determines it, in which case this must be omitted."},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, return extra information about the transaction."},
                },
                {
                    RPCResult{"if verbose is not set or set to false",
                        RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                "the number of addresses."
                    },
                    RPCResult{"if verbose is set to true",
                        RPCResult::Type::OBJ, "", "",
                        {
                                {RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                "the number of addresses."},
                            {RPCResult::Type::STR, "fee_reason", "The transaction fee reason."}
                        },
                    },
                },
                RPCExamples{
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\" 6 \"testing\"") +
            "\nSend two amounts to two different addresses, subtract fee from amount:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\" 1 \"\" \"[\\\"" + EXAMPLE_ADDRESS[0] + "\\\",\\\"" + EXAMPLE_ADDRESS[1] + "\\\"]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendmany", "\"\", {\"" + EXAMPLE_ADDRESS[0] + "\":0.01,\"" + EXAMPLE_ADDRESS[1] + "\":0.02}, 6, \"testing\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }
    UniValue sendTo = request.params[1].get_obj();

    mapValue_t mapValue;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    SetFeeEstimateMode(*pwallet, coin_control, /* conf_target */ request.params[6], /* estimate_mode */ request.params[7], /* fee_rate */ request.params[10], /* override_min_fee */ false);

    UniValue assets;
    if (!request.params[8].isNull()) {
        if (!g_con_elementsmode) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Asset argument cannot be given for Bitcoin serialization.");
        }
        assets = request.params[8].get_obj();
    }

    bool ignore_blind_fail = true;
    if (!request.params[9].isNull()) {
        ignore_blind_fail = request.params[9].get_bool();
    }

    std::vector<CRecipient> recipients;
    ParseRecipients(sendTo, assets, subtractFeeFromAmount, recipients);
    if (g_con_any_asset_fees) {
        // Never inferred from the recipients (see sendtoaddress). The one thing
        // derived is the subtract-fee constraint, and only because the fee comes
        // out of those outputs. (This used to skip an empty recipient list, which
        // left the fee asset unset and so silently on the policy asset.)
        const std::optional<CAsset> explicit_fee_asset =
            request.params.size() > 11 ? ParseFeeAssetArg(request.params[11]) : std::nullopt;
        coin_control.m_fee_asset = ResolveFeeAsset(explicit_fee_asset, DeterminedBySubtractFee(SubtractFeeFromAssets(recipients)), "fee_asset");
    }
    bool verbose = request.params[12].isNull() ? false : request.params[12].get_bool();

    return SendMoney(*pwallet, coin_control, recipients, std::move(mapValue), verbose, ignore_blind_fail);
},
    };
}

RPCHelpMan settxfee()
{
    return RPCHelpMan{"settxfee",
                "\nSet the transaction fee rate in " + CURRENCY_UNIT + "/kvB for this wallet. Overrides the global -paytxfee command line parameter.\n"
                "Can be deactivated by passing 0 as the fee. In that case automatic fee selection will be used by default.\n",
                {
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The transaction fee rate in " + CURRENCY_UNIT + "/kvB"},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Returns true if successful"
                },
                RPCExamples{
                    HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    LOCK(pwallet->cs_wallet);

    CAmount nAmount = AmountFromValue(request.params[0]);
    CFeeRate tx_fee_rate(nAmount, 1000);
    CFeeRate max_tx_fee_rate(pwallet->m_default_max_tx_fee, 1000);
    if (tx_fee_rate == CFeeRate(0)) {
        // automatic selection
    } else if (tx_fee_rate < pwallet->chain().relayMinFee()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than min relay tx fee (%s)", pwallet->chain().relayMinFee().ToString()));
    } else if (tx_fee_rate < pwallet->m_min_fee) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than wallet min fee (%s)", pwallet->m_min_fee.ToString()));
    } else if (tx_fee_rate > max_tx_fee_rate) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be more than wallet max tx fee (%s)", max_tx_fee_rate.ToString()));
    }

    pwallet->m_pay_tx_fee = tx_fee_rate;
    return true;
},
    };
}


// Only includes key documentation where the key is snake_case in all RPC methods. MixedCase keys can be added later.
static std::vector<RPCArg> FundTxDoc()
{
    return {
        {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks"},
        {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, std::string() + "The fee estimate mode, must be one of (case insensitive):\n"
            "         \"" + FeeModes("\"\n\"") + "\""},
        {"replaceable", RPCArg::Type::BOOL, RPCArg::DefaultHint{"wallet default"}, "Marks this transaction as BIP125-replaceable.\n"
            "Allows this transaction to be replaced by a transaction with higher fees"},
        {"solving_data", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "Keys and scripts needed for producing a final transaction with a dummy signature.\n"
            "Used for fee estimation during coin selection.",
         {
             {"pubkeys", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Public keys involved in this transaction.",
             {
                 {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A public key"},
             }},
             {"scripts", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Scripts involved in this transaction.",
             {
                 {"script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A script"},
             }},
             {"descriptors", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Descriptors that provide solving data for this transaction.",
             {
                 {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A descriptor"},
             }},
         }},
        {"ignoreblindfail", RPCArg::Type::BOOL, RPCArg::Default{true}, "Return a transaction even when a blinding attempt fails due to number of blinded inputs/outputs.\n"
            "When false, such a transaction is an error instead of one with an output silently left explicit."},
    };
}

/** SEQUENTIA: the privacy the wallet was asked for and could not deliver, as an RPC field. */
static RPCResult FundTxWarningsDoc()
{
    return {RPCResult::Type::ARR, "warnings", /*optional=*/true, "Warnings about the funded transaction, e.g. an output that had to be left explicit",
        {
            {RPCResult::Type::STR, "", ""},
        }};
}

static void PushFundTxWarnings(UniValue& result, const std::vector<bilingual_str>& warnings)
{
    if (warnings.empty()) return;
    UniValue arr(UniValue::VARR);
    for (const bilingual_str& warning : warnings) {
        arr.push_back(warning.original);
    }
    result.pushKV("warnings", arr);
}

void FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, const UniValue& options, CCoinControl& coinControl, bool override_min_fee, std::vector<bilingual_str>* warnings = nullptr)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    change_position = -1;
    bool lockUnspents = false;
    // ELEMENTS/SEQUENTIA: same meaning and same default as sendtoaddress/sendmany.
    bool ignore_blind_fail = true;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;
    // SEQUENTIA: no fee asset is inferred from the transaction. This used to be
    // `coinControl.m_fee_asset = tx.vout[0].nAsset.GetAsset()` -- the FIRST
    // output's asset, which has nothing to do with which asset can pay a fee, and
    // made funding fail outright whenever that asset had no exchange rate here.
    // What the transaction does state is read below: an explicit fee output, and
    // the subtract_fee_from_outputs positions. The caller's options.fee_asset goes
    // into `explicit_fee_asset`; ResolveFeeAsset settles them, or refuses when
    // nothing says anything. Nothing here treats the policy asset differently.
    std::optional<CAsset> explicit_fee_asset;
    // A single change address names one destination for the FEE asset, which is
    // only settled below; hold it until then rather than reading a half-settled
    // m_fee_asset.
    std::optional<CTxDestination> fee_asset_change_dest;

    if (!options.isNull()) {
      if (options.type() == UniValue::VBOOL) {
        // backward compatibility bool only fallback
        coinControl.fAllowWatchOnly = options.get_bool();
      }
      else {
        RPCTypeCheckArgument(options, UniValue::VOBJ);

        RPCTypeCheckObj(options,
            {
                {"add_inputs", UniValueType(UniValue::VBOOL)},
                {"include_unsafe", UniValueType(UniValue::VBOOL)},
                {"add_to_wallet", UniValueType(UniValue::VBOOL)},
                {"changeAddress", UniValueType()}, // will be checked below
                {"change_address", UniValueType()}, // will be checked below
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"change_position", UniValueType(UniValue::VNUM)},
                {"change_type", UniValueType(UniValue::VSTR)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"include_watching", UniValueType(UniValue::VBOOL)},
                {"inputs", UniValueType(UniValue::VARR)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"lock_unspents", UniValueType(UniValue::VBOOL)},
                {"locktime", UniValueType(UniValue::VNUM)},
                {"fee_rate", UniValueType()}, // will be checked by AmountFromValue() in SetFeeEstimateMode()
                {"feeRate", UniValueType()}, // will be checked by AmountFromValue() below
                {"fee_asset", UniValueType(UniValue::VSTR)},
                {"psbt", UniValueType(UniValue::VBOOL)},
                {"solving_data", UniValueType(UniValue::VOBJ)},
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                {"subtract_fee_from_outputs", UniValueType(UniValue::VARR)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"conf_target", UniValueType(UniValue::VNUM)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
                {"include_explicit", UniValueType(UniValue::VBOOL)},
                {"input_weights", UniValueType(UniValue::VARR)},
                {"ignoreblindfail", UniValueType(UniValue::VBOOL)},
            },
            true, true);

        if (options.exists("add_inputs") ) {
            coinControl.m_add_inputs = options["add_inputs"].get_bool();
        }

        if (options.exists("ignoreblindfail")) {
            ignore_blind_fail = options["ignoreblindfail"].get_bool();
        }

        if (g_con_any_asset_fees && options.exists("fee_asset")) {
            explicit_fee_asset = ParseFeeAssetArg(options["fee_asset"]);
        }

        if (options.exists("changeAddress") || options.exists("change_address")) {
            const UniValue& change_address  = options.exists("change_address") ? options["change_address"] : options["changeAddress"];
            std::map<CAsset, CTxDestination> destinations;

            if (change_address.isStr()) {
                // Single destination for fee asset.
                CTxDestination dest = DecodeDestination(change_address.get_str());
                if (!IsValidDestination(dest)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Change address must be a valid address");
                }
                fee_asset_change_dest = dest;
            } else if (change_address.isObject()) {
                // Map of assets to destinations.
                std::map<std::string, UniValue> kvMap;
                change_address.getObjMap(kvMap);

                for (const auto& kv : kvMap) {
                    CAsset asset = GetAssetFromString(kv.first);
                    if (asset.IsNull()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Change address key must be a valid asset label or hex");
                    }

                    CTxDestination dest = DecodeDestination(kv.second.get_str());
                    if (!IsValidDestination(dest)) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Change address must be a valid address");
                    }

                    destinations[asset] = dest;
                }
            } else {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Change address must be either a map or a string");
            }

            coinControl.destChange = destinations;
        }

        if (options.exists("changePosition") || options.exists("change_position")) {
            change_position = (options.exists("change_position") ? options["change_position"] : options["changePosition"]).get_int();
        }

        if (options.exists("change_type")) {
            if (options.exists("changeAddress") || options.exists("change_address")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both change address and address type options");
            }
            if (std::optional<OutputType> parsed = ParseOutputType(options["change_type"].get_str())) {
                coinControl.m_change_type.emplace(parsed.value());
            } else {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
            }
        }

        const UniValue include_watching_option = options.exists("include_watching") ? options["include_watching"] : options["includeWatching"];
        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(include_watching_option, wallet);

        if (options.exists("lockUnspents") || options.exists("lock_unspents")) {
            lockUnspents = (options.exists("lock_unspents") ? options["lock_unspents"] : options["lockUnspents"]).get_bool();
        }

        if (options.exists("include_unsafe")) {
            coinControl.m_include_unsafe_inputs = options["include_unsafe"].get_bool();
        }

        if (options.exists("feeRate")) {
            if (options.exists("fee_rate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both fee_rate (" + CURRENCY_ATOM + "/vB) and feeRate (" + CURRENCY_UNIT + "/kvB)");
            }
            if (options.exists("conf_target")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate. Please provide either a confirmation target in blocks for automatic fee estimation, or an explicit fee rate.");
            }
            if (options.exists("estimate_mode")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
            }
            coinControl.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
            coinControl.fOverrideFeeRate = true;
        }

        if (options.exists("subtractFeeFromOutputs") || options.exists("subtract_fee_from_outputs") )
            subtractFeeFromOutputs = (options.exists("subtract_fee_from_outputs") ? options["subtract_fee_from_outputs"] : options["subtractFeeFromOutputs"]).get_array();

        if (options.exists("replaceable")) {
            coinControl.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        SetFeeEstimateMode(wallet, coinControl, options["conf_target"], options["estimate_mode"], options["fee_rate"], override_min_fee);
      }
    } else {
        // if options is null and not a bool
        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(NullUniValue, wallet);
    }

    if (options.exists("solving_data")) {
        const UniValue solving_data = options["solving_data"].get_obj();
        if (solving_data.exists("pubkeys")) {
            for (const UniValue& pk_univ : solving_data["pubkeys"].get_array().getValues()) {
                const std::string& pk_str = pk_univ.get_str();
                if (!IsHex(pk_str)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not hex", pk_str));
                }
                const std::vector<unsigned char> data(ParseHex(pk_str));
                const CPubKey pubkey(data.begin(), data.end());
                if (!pubkey.IsFullyValid()) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not a valid public key", pk_str));
                }
                coinControl.m_external_provider.pubkeys.emplace(pubkey.GetID(), pubkey);
                // Add witness script for pubkeys
                const CScript wit_script = GetScriptForDestination(WitnessV0KeyHash(pubkey));
                coinControl.m_external_provider.scripts.emplace(CScriptID(wit_script), wit_script);
            }
        }

        if (solving_data.exists("scripts")) {
            for (const UniValue& script_univ : solving_data["scripts"].get_array().getValues()) {
                const std::string& script_str = script_univ.get_str();
                if (!IsHex(script_str)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not hex", script_str));
                }
                std::vector<unsigned char> script_data(ParseHex(script_str));
                const CScript script(script_data.begin(), script_data.end());
                coinControl.m_external_provider.scripts.emplace(CScriptID(script), script);
            }
        }

        if (solving_data.exists("descriptors")) {
            for (const UniValue& desc_univ : solving_data["descriptors"].get_array().getValues()) {
                const std::string& desc_str  = desc_univ.get_str();
                FlatSigningProvider desc_out;
                std::string error;
                std::vector<CScript> scripts_temp;
                std::unique_ptr<Descriptor> desc = Parse(desc_str, desc_out, error, true);
                if (!desc) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unable to parse descriptor '%s': %s", desc_str, error));
                }
                desc->Expand(0, desc_out, scripts_temp, desc_out);
                coinControl.m_external_provider = Merge(coinControl.m_external_provider, desc_out);
            }
        }
    }

    if (options.exists("input_weights")) {
        for (const UniValue& input : options["input_weights"].get_array().getValues()) {
            uint256 txid = ParseHashO(input, "txid");

            const UniValue& vout_v = find_value(input, "vout");
            if (!vout_v.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
            }
            int vout = vout_v.get_int();
            if (vout < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");
            }

            const UniValue& weight_v = find_value(input, "weight");
            if (!weight_v.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing weight key");
            }
            int64_t weight = weight_v.get_int64();
            CMutableTransaction mtx;
            mtx.vin.resize(1);
            mtx.witness.vtxinwit.resize(1);
            const int64_t min_input_weight = GetTransactionInputWeight(CTransaction(mtx), 0);
            CHECK_NONFATAL(min_input_weight == 165);
            if (weight < min_input_weight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, weight cannot be less than 165 (41 bytes (size of outpoint + sequence + empty scriptSig) * 4 (witness scaling factor)) + 1 (empty witness)");
            }
            if (weight > MAX_STANDARD_TX_WEIGHT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, weight cannot be greater than the maximum standard tx weight of %d", MAX_STANDARD_TX_WEIGHT));
            }

            coinControl.SetInputWeight(COutPoint(txid, vout), weight);
        }
    }

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (change_position != -1 && (change_position < 0 || (unsigned int)change_position > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    // SEQUENTIA: settle the fee asset now that the caller's choice, the
    // transaction's own fee outputs and the subtract-fee positions are all known.
    // The transaction states the answer in two ways -- a fee output names the
    // asset the fee is paid in, and subtracting the fee from an output means it
    // comes out of that output's amount -- and where it does, there is nothing
    // for the caller to choose. Uniform across every asset, with no policy-asset
    // branch: subtracting the fee from an output previously only happened to work
    // when that output was the policy asset, because the silent fallback
    // coincided with it.
    if (g_con_any_asset_fees) {
        std::vector<CAsset> fee_output_assets;
        for (const CTxOut& out : tx.vout) {
            if (out.IsFee()) fee_output_assets.push_back(out.nAsset.GetAsset());
        }
        std::vector<CAsset> subtract_from_assets;
        for (const int pos : setSubtractFeeFromOutputs) {
            if (!tx.vout[pos].nAsset.IsExplicit()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "Cannot subtract the fee from output %d: its asset is blinded, so the asset the fee would come out of is not known here.", pos));
            }
            subtract_from_assets.push_back(tx.vout[pos].nAsset.GetAsset());
        }
        coinControl.m_fee_asset = ResolveFeeAsset(
            explicit_fee_asset,
            CombineFeeAssetDeterminations(DeterminedByFeeOutputs(fee_output_assets),
                                          DeterminedBySubtractFee(subtract_from_assets)),
            "options.fee_asset");
    }
    if (fee_asset_change_dest) {
        coinControl.destChange[coinControl.m_fee_asset.value_or(::policyAsset)] = *fee_asset_change_dest;
    }

    // Check any existing inputs for peg-in data and add to external txouts if so
    // Fetch specified UTXOs from the UTXO set to get the scriptPubKeys and values of the outputs being selected
    // and to match with the given solving_data. Only used for non-wallet outputs.
    const auto& fedpegscripts = GetValidFedpegScripts(wallet.chain().getTip(), Params().GetConsensus(), true /* nextblock_validation */);
    std::map<COutPoint, Coin> coins;
    for (unsigned int i = 0; i < tx.vin.size(); ++i ) {
        const CTxIn& txin = tx.vin[i];
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
        if (txin.m_is_pegin) {
            std::string err;
            if (tx.witness.vtxinwit.size() != tx.vin.size() || !IsValidPeginWitness(tx.witness.vtxinwit[i].m_pegin_witness, fedpegscripts, txin.prevout, err, false)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Transaction contains invalid peg-in input: %s", err));
            }
            CScriptWitness& pegin_witness = tx.witness.vtxinwit[i].m_pegin_witness;
            CTxOut txout = GetPeginOutputFromWitness(pegin_witness);
            coinControl.SelectExternal(txin.prevout, txout);
        }
    }
    wallet.chain().findCoins(coins);
    for (const auto& coin : coins) {
        if (!coin.second.out.IsNull()) {
            coinControl.SelectExternal(coin.first, coin.second.out);
        }
    }

    bilingual_str error;

    if (!FundTransaction(wallet, tx, fee_out, change_position, error, lockUnspents, setSubtractFeeFromOutputs, coinControl, ignore_blind_fail, warnings)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error.original);
    }
}

static void SetOptionsInputWeights(const UniValue& inputs, UniValue& options)
{
    if (options.exists("input_weights")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Input weights should be specified in inputs rather than in options.");
    }
    if (inputs.size() == 0) {
        return;
    }
    UniValue weights(UniValue::VARR);
    for (const UniValue& input : inputs.getValues()) {
        if (input.exists("weight")) {
            weights.push_back(input);
        }
    }
    options.pushKV("input_weights", weights);
}

RPCHelpMan fundrawtransaction()
{
    return RPCHelpMan{"fundrawtransaction",
                "\nIf the transaction has no inputs, they will be automatically selected to meet its out value.\n"
                "It will add at most one change output to the outputs.\n"
                "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
                "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                "The inputs added will not be signed, use signrawtransactionwithkey\n"
                "or signrawtransactionwithwallet for that.\n"
                "All existing inputs must either have their previous output transaction be in the wallet\n"
                "or be in the UTXO set. Solving data must be provided for non-wallet inputs.\n"
                "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
                "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
                "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n",
                {
                    {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}",
                        Cat<std::vector<RPCArg>>(
                        {
                            {"add_inputs", RPCArg::Type::BOOL, RPCArg::Default{true}, "For a transaction with existing inputs, automatically include more if they are not enough."},
                            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                            {"changeAddress", RPCArg::Type::STR, RPCArg::DefaultHint{"pool address"}, "The address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch only.\n"
                                                          "Only solvable inputs can be used. Watch-only destinations are solvable if the public key and/or output script was imported,\n"
                                                          "e.g. with 'importpubkey' or 'importmulti' with the 'pubkeys' or 'desc' field."},
                            {"lockUnspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                            {"feeRate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_UNIT + "/kvB."},
                            {"fee_asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Label or hex ID of the asset used to pay the fee. On a chain with the open fee market (con_any_asset_fees) the fee asset must be named, because nothing is defaulted or inferred -- unless the transaction already determines it, which it does when it carries an explicit fee output (that output names the asset) or when subtract_fee_from_outputs is used (the fee comes out of those outputs, so it is denominated in theirs). Where it is determined, this must be omitted."},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "The integers.\n"
                                                          "The fee will be equally deducted from the amount of each specified output.\n"
                                                          "Those recipients will receive less coins than you enter in their corresponding amount field.\n"
                                                          "If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"input_weights", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "Inputs and their corresponding weights",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output index"},
                                    {"weight", RPCArg::Type::NUM, RPCArg::Optional::NO, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that serialized signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                                },
                             },
                        },
                        FundTxDoc()),
                        "options"},
                    {"iswitness", RPCArg::Type::BOOL, RPCArg::DefaultHint{"depends on heuristic tests"}, "Whether the transaction hex is a serialized witness transaction.\n"
                        "If iswitness is not present, heuristic tests will be used in decoding.\n"
                        "If true, only witness deserialization will be tried.\n"
                        "If false, only non-witness deserialization will be tried.\n"
                        "This boolean should reflect whether the transaction has inputs\n"
                        "(e.g. fully valid, or on-chain transactions), if known by the caller."
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The resulting raw transaction (hex-encoded string)"},
                        {RPCResult::Type::STR_AMOUNT, "fee", "Fee in " + CURRENCY_UNIT + " the resulting transaction pays"},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                        FundTxWarningsDoc(),
                    }
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransactionwithwallet", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValueType(), UniValue::VBOOL});

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    CAmount fee;
    int change_position;
    CCoinControl coin_control;
    // Automatically select (additional) coins. Can be overridden by options.add_inputs.
    coin_control.m_add_inputs = true;
    std::vector<bilingual_str> warnings;
    FundTransaction(*pwallet, tx, fee, change_position, request.params[1], coin_control, /* override_min_fee */ true, &warnings);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    result.pushKV("fee", ValueFromAmount(fee));
    if (g_con_any_asset_fees) {
        result.pushKV("fee_asset", coin_control.m_fee_asset.value_or(::policyAsset).GetHex());
    }
    result.pushKV("changepos", change_position);
    PushFundTxWarnings(result, warnings);

    return result;
},
    };
}

RPCHelpMan signrawtransactionwithwallet()
{
    return RPCHelpMan{"signrawtransactionwithwallet",
                "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
                "The second optional argument (may be null) is an array of previous transaction outputs that\n"
                "this transaction depends on but may not yet be in the block chain." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {"prevtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "The previous dependent transaction outputs",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"scriptPubKey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "script key"},
                                    {"redeemScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2SH) redeem script"},
                                    {"witnessScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2WSH or P2SH-P2WSH) witness script"},
                                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The amount spent (required if non-confidential segwit output)"},
                                    {"amountcommitment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The amount commitment spent (required if confidential segwit output)"},
                                },
                            },
                        },
                    },
                    {"sighashtype", RPCArg::Type::STR, RPCArg::Default{"DEFAULT for Taproot, ALL otherwise"}, "The signature hash type. Must be one of\n"
            "       \"DEFAULT\"\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The hex-encoded raw transaction with signature(s)"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                        {RPCResult::Type::ARR, "errors", /*optional=*/true, "Script verification errors (if there are any)",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The hash of the referenced, previous transaction"},
                                {RPCResult::Type::NUM, "vout", "The index of the output to spent and used as input"},
                                {RPCResult::Type::ARR, "witness", "",
                                {
                                    {RPCResult::Type::STR_HEX, "witness", ""},
                                }},
                                {RPCResult::Type::STR_HEX, "scriptSig", "The hex-encoded signature script"},
                                {RPCResult::Type::NUM, "sequence", "Script sequence number"},
                                {RPCResult::Type::STR, "error", "Verification or signing error related to the input"},
                            }},
                        }},
                        {RPCResult::Type::STR, "warning", "Warning that a peg-in input signed may be immature. This could mean lack of connectivity to or misconfiguration of the daemon."},
                    }
                },
                RPCExamples{
                    HelpExampleCli("signrawtransactionwithwallet", "\"myhex\"")
            + HelpExampleRpc("signrawtransactionwithwallet", "\"myhex\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR, UniValue::VSTR}, true);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed. Make sure the tx has at least one input.");
    }

    // Sign the transaction
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    // Fetch previous transactions (inputs):
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : mtx.vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    pwallet->chain().findCoins(coins);

    // Parse the prevtxs array
    ParsePrevouts(request.params[1], nullptr, coins);

    int nHashType = ParseSighashString(request.params[2]);

    // Script verification errors
    std::map<int, bilingual_str> input_errors;

    bool immature_pegin = ValidateTransactionPeginInputs(mtx, pwallet->chain().getTip(), input_errors);
    bool complete = pwallet->SignTransaction(mtx, coins, nHashType, input_errors);
    UniValue result(UniValue::VOBJ);
    SignTransactionResultToJSON(mtx, complete, coins, input_errors, immature_pegin, result);
    return result;
},
    };
}

static RPCHelpMan bumpfee_helper(std::string method_name)
{
    const bool want_psbt = method_name == "psbtbumpfee";
    const std::string incremental_fee{CFeeRate(DEFAULT_INCREMENTAL_RELAY_FEE).ToString(FeeEstimateMode::SAT_VB)};

    return RPCHelpMan{method_name,
        "\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n"
        + std::string(want_psbt ? "Returns a PSBT instead of creating and signing a new transaction.\n" : "") +
        "An opt-in RBF transaction with the given txid must be in the wallet.\n"
        "The command will pay the additional fee by reducing change outputs or adding inputs when necessary.\n"
        "It may add a new change output if one does not already exist.\n"
        "All inputs in the original transaction will be included in the replacement transaction.\n"
        "The command will fail if the wallet or mempool contains a transaction that spends one of T's outputs.\n"
        "By default, the new fee will be calculated automatically using the estimatesmartfee RPC.\n"
        "The user can specify a confirmation target for estimatesmartfee.\n"
        "Alternatively, the user can specify a fee rate in " + CURRENCY_ATOM + "/vB for the new transaction.\n"
        "At a minimum, the new fee rate must be high enough to pay an additional new relay fee (incrementalfee\n"
        "returned by getnetworkinfo) to enter the node's mempool.\n"
        "* WARNING: before version 0.21, fee_rate was in " + CURRENCY_UNIT + "/kvB. As of 0.21, fee_rate is in " + CURRENCY_ATOM + "/vB. *\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The txid to be bumped"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                {
                    {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks\n"},
                    {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"},
                             "\nSpecify a fee rate in " + CURRENCY_ATOM + "/vB instead of relying on the built-in fee estimator.\n"
                             "Must be at least " + incremental_fee + " higher than the current transaction fee rate.\n"
                             "WARNING: before version 0.21, fee_rate was in " + CURRENCY_UNIT + "/kvB. As of 0.21, fee_rate is in " + CURRENCY_ATOM + "/vB.\n"},
                    {"fee_asset", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"not set, fall back to fee asset in existing transaction"}, "Asset to use to pay fees\n"},
                    {"replaceable", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether the new transaction should still be\n"
                             "marked bip-125 replaceable. If true, the sequence numbers in the transaction will\n"
                             "be left unchanged from the original. If false, any input sequence numbers in the\n"
                             "original transaction that were less than 0xfffffffe will be increased to 0xfffffffe\n"
                             "so the new transaction will not be explicitly bip-125 replaceable (though it may\n"
                             "still be replaceable in practice, for example if it has unconfirmed ancestors which\n"
                             "are replaceable).\n"},
                    {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, "The fee estimate mode, must be one of (case insensitive):\n"
                             "\"" + FeeModes("\"\n\"") + "\""},
                },
                "options"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "", Cat(
                want_psbt ?
                std::vector<RPCResult>{{RPCResult::Type::STR, "psbt", "The base64-encoded unsigned PSBT of the new transaction."}} :
                std::vector<RPCResult>{{RPCResult::Type::STR_HEX, "txid", "The id of the new transaction."}},
            {
                {RPCResult::Type::STR_AMOUNT, "origfee", "The fee of the replaced transaction."},
                {RPCResult::Type::STR_AMOUNT, "fee", "The fee of the new transaction."},
                {RPCResult::Type::STR_HEX, "fee_asset", /* optional */ g_con_any_asset_fees, "The asset being used to pay fees."},
                {RPCResult::Type::ARR, "errors", "Errors encountered during processing (may be empty).",
                {
                    {RPCResult::Type::STR, "", ""},
                }},
            })
        },
        RPCExamples{
    "\nBump the fee, get the new transaction\'s " + std::string(want_psbt ? "psbt" : "txid") + "\n" +
            HelpExampleCli(method_name, "<txid>")
        },
        [want_psbt](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && !want_psbt) {
        throw JSONRPCError(RPC_WALLET_ERROR, "bumpfee is not available with wallets that have private keys disabled. Use psbtbumpfee instead.");
    }

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    uint256 hash(ParseHashV(request.params[0], "txid"));

    CCoinControl coin_control;
    coin_control.fAllowWatchOnly = pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    // optional parameters
    coin_control.m_signal_bip125_rbf = true;
    CAsset fee_asset = ::policyAsset;

    if (!request.params[1].isNull()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"conf_target", UniValueType(UniValue::VNUM)},
                {"fee_rate", UniValueType()}, // will be checked by AmountFromValue() in SetFeeEstimateMode()
                {"fee_asset", UniValueType(UniValue::VSTR)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);

        if (options.exists("confTarget") && options.exists("conf_target")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget and conf_target options should not both be set. Use conf_target (confTarget is deprecated).");
        }

        auto conf_target = options.exists("confTarget") ? options["confTarget"] : options["conf_target"];

        if (options.exists("replaceable")) {
            coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }

        if (g_con_any_asset_fees && options.exists("fee_asset")) {
            std::string feeAssetString = options["fee_asset"].get_str();
            fee_asset = GetAssetFromString(feeAssetString);
            if (fee_asset.IsNull()) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex for fee: %s", feeAssetString));
            }
            coin_control.m_fee_asset = fee_asset;
        }
        SetFeeEstimateMode(*pwallet, coin_control, conf_target, options["estimate_mode"], options["fee_rate"], /* override_min_fee */ false);
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(*pwallet);


    std::vector<bilingual_str> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    feebumper::Result res;
    // Targeting feerate bump.
    res = feebumper::CreateRateBumpTransaction(*pwallet, hash, coin_control, errors, old_fee, new_fee, mtx);
    if (res != feebumper::Result::OK) {
        switch(res) {
            case feebumper::Result::INVALID_ADDRESS_OR_KEY:
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errors[0].original);
                break;
            case feebumper::Result::INVALID_REQUEST:
                throw JSONRPCError(RPC_INVALID_REQUEST, errors[0].original);
                break;
            case feebumper::Result::INVALID_PARAMETER:
                throw JSONRPCError(RPC_INVALID_PARAMETER, errors[0].original);
                break;
            case feebumper::Result::WALLET_ERROR:
                throw JSONRPCError(RPC_WALLET_ERROR, errors[0].original);
                break;
            default:
                throw JSONRPCError(RPC_MISC_ERROR, errors[0].original);
                break;
        }
    }

    // SEQUENTIA: report the asset the bump ACTUALLY pays in. Absent an explicit
    // options.fee_asset, feebumper inherits the replaced transaction's fee asset
    // (a derivation from the tx being replaced, not a default), so reporting
    // ::policyAsset here claimed the bump was paid in SEQ whenever the original
    // was not. Read it off the built transaction, before mtx is moved away.
    if (g_con_any_asset_fees) {
        fee_asset = CTransaction(mtx).GetFeeAsset(::policyAsset);
    }

    UniValue result(UniValue::VOBJ);

    // For bumpfee, return the new transaction id.
    // For psbtbumpfee, return the base64-encoded unsigned PSBT of the new transaction.
    if (!want_psbt) {
        if (!feebumper::SignTransaction(*pwallet, mtx)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Can't sign transaction.");
        }

        uint256 txid;
        if (feebumper::CommitTransaction(*pwallet, hash, std::move(mtx), errors, txid) != feebumper::Result::OK) {
            throw JSONRPCError(RPC_WALLET_ERROR, errors[0].original);
        }

        result.pushKV("txid", txid.GetHex());
    } else {
        PartiallySignedTransaction psbtx(mtx, 2 /* version */);
        bool complete = false;
        const TransactionError err = pwallet->FillPSBT(psbtx, complete, SIGHASH_DEFAULT, false /* sign */, true /* bip32derivs */);
        CHECK_NONFATAL(err == TransactionError::OK);
        CHECK_NONFATAL(!complete);
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }

    result.pushKV("origfee", ValueFromAmount(old_fee));
    result.pushKV("fee", ValueFromAmount(new_fee));
    result.pushKV("fee_asset", fee_asset.GetHex());
    UniValue result_errors(UniValue::VARR);
    for (const bilingual_str& error : errors) {
        result_errors.push_back(error.original);
    }
    result.pushKV("errors", result_errors);

    return result;
},
    };
}

RPCHelpMan bumpfee() { return bumpfee_helper("bumpfee"); }
RPCHelpMan psbtbumpfee() { return bumpfee_helper("psbtbumpfee"); }

RPCHelpMan send()
{
    return RPCHelpMan{"send",
        "\nEXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSend a transaction.\n",
        {
            {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs (key-value pairs), where none of the keys are duplicated.\n"
                    "That is, each address can only appear once and there can only be one 'data' object.\n"
                    "For convenience, a dictionary, which holds the key-value pairs directly, is also accepted.",
                {
                    {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                        },
                        },
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                        },
                    },
                },
            },
            {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks"},
            {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, std::string() + "The fee estimate mode, must be one of (case insensitive):\n"
                        "       \"" + FeeModes("\"\n\"") + "\""},
            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                Cat<std::vector<RPCArg>>(
                {
                    {"add_inputs", RPCArg::Type::BOOL, RPCArg::Default{false}, "If inputs are specified, automatically include more if they are not enough."},
                    {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                    {"add_to_wallet", RPCArg::Type::BOOL, RPCArg::Default{true}, "When false, returns a serialized transaction which will not be added to the wallet or broadcast"},
                    {"change_address", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"pool address"}, "The address to receive the change"},
                    {"change_position", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                    {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if change_address is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                    {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                    {"fee_asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Label or hex ID of the asset used to pay the fee. On a chain with the open fee market (con_any_asset_fees) the fee asset must be named, because nothing is defaulted or inferred -- unless the transaction already determines it, which it does when it carries an explicit fee output (that output names the asset) or when subtract_fee_from_outputs is used (the fee comes out of those outputs, so it is denominated in theirs). Where it is determined, this must be omitted."},
                    {"include_watching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch only.\n"
                                          "Only solvable inputs can be used. Watch-only destinations are solvable if the public key and/or output script was imported,\n"
                                          "e.g. with 'importpubkey' or 'importmulti' with the 'pubkeys' or 'desc' field."},
                    {"inputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Specify inputs instead of adding them automatically. A JSON array of JSON objects",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                            {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                            {"weight", RPCArg::Type::NUM, RPCArg::DefaultHint{"Calculated from wallet and solving data"}, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                    {"psbt", RPCArg::Type::BOOL,  RPCArg::DefaultHint{"automatic"}, "Always return a PSBT, implies add_to_wallet=false."},
                    {"subtract_fee_from_outputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Outputs to subtract the fee from, specified as integer indices.\n"
                    "The fee will be equally deducted from the amount of each specified output.\n"
                    "Those recipients will receive less coins than you enter in their corresponding amount field.\n"
                    "If no outputs are specified here, the sender pays the fee.",
                        {
                            {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                        },
                    },
                },
                FundTxDoc()),
                "options"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The transaction id for the send. Only 1 transaction is created regardless of the number of addresses."},
                    {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "If add_to_wallet is false, the hex-encoded raw transaction with signature(s)"},
                    {RPCResult::Type::STR, "psbt", /*optional=*/true, "If more signatures are needed, or if add_to_wallet is false, the base64-encoded (partially) signed transaction"},
                    FundTxWarningsDoc()
                }
        },
        RPCExamples{""
        "\nSend 0.1 BTC with a confirmation target of 6 blocks in economical fee estimate mode\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.1}' 6 economical\n") +
        "Send 0.2 BTC with a fee rate of 1.1 " + CURRENCY_ATOM + "/vB using positional arguments\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.2}' null \"unset\" 1.1\n") +
        "Send 0.2 BTC with a fee rate of 1 " + CURRENCY_ATOM + "/vB using the options argument\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.2}' null \"unset\" null '{\"fee_rate\": 1}'\n") +
        "Send 0.3 BTC with a fee rate of 25 " + CURRENCY_ATOM + "/vB using named arguments\n"
        + HelpExampleCli("-named send", "outputs='{\"" + EXAMPLE_ADDRESS[0] + "\": 0.3}' fee_rate=25\n") +
        "Create a transaction that should confirm the next block, with a specific input, and return result without adding to wallet or broadcasting to the network\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.1}' 1 economical '{\"add_to_wallet\": false, \"inputs\": [{\"txid\":\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\", \"vout\":1}]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            RPCTypeCheck(request.params, {
                UniValueType(), // outputs (ARR or OBJ, checked later)
                UniValue::VNUM, // conf_target
                UniValue::VSTR, // estimate_mode
                UniValueType(), // fee_rate, will be checked by AmountFromValue() in SetFeeEstimateMode()
                UniValue::VOBJ, // options
                }, true
            );

            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return NullUniValue;

            UniValue options{request.params[4].isNull() ? UniValue::VOBJ : request.params[4]};
            if (options.exists("conf_target") || options.exists("estimate_mode")) {
                if (!request.params[1].isNull() || !request.params[2].isNull()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Pass conf_target and estimate_mode either as arguments or in the options object, but not both");
                }
            } else {
                options.pushKV("conf_target", request.params[1]);
                options.pushKV("estimate_mode", request.params[2]);
            }
            if (options.exists("fee_rate")) {
                if (!request.params[3].isNull()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Pass the fee_rate either as an argument, or in the options object, but not both");
                }
            } else {
                options.pushKV("fee_rate", request.params[3]);
            }
            if (!options["conf_target"].isNull() && (options["estimate_mode"].isNull() || (options["estimate_mode"].get_str() == "unset"))) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Specify estimate_mode");
            }
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use fee_rate (" + CURRENCY_ATOM + "/vB) instead of feeRate");
            }
            if (options.exists("changeAddress")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_address");
            }
            if (options.exists("changePosition")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_position");
            }
            if (options.exists("includeWatching")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use include_watching");
            }
            if (options.exists("lockUnspents")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use lock_unspents");
            }
            if (options.exists("subtractFeeFromOutputs")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use subtract_fee_from_outputs");
            }

            const bool psbt_opt_in = options.exists("psbt") && options["psbt"].get_bool();

            CAmount fee;
            int change_position;
            bool rbf = pwallet->m_signal_rbf;
            if (options.exists("replaceable")) {
                rbf = options["replaceable"].get_bool();
            }
            CMutableTransaction rawTx = ConstructTransaction(options["inputs"], request.params[0], options["locktime"], rbf, pwallet->chain().getTip(), nullptr /* output_pubkey_out */, true /* allow_peg_in */);
            CCoinControl coin_control;
            // Automatically select coins, unless at least one is manually selected. Can
            // be overridden by options.add_inputs.
            coin_control.m_add_inputs = rawTx.vin.size() == 0;
            SetOptionsInputWeights(options["inputs"], options);
            std::vector<bilingual_str> warnings;
            FundTransaction(*pwallet, rawTx, fee, change_position, options, coin_control, /* override_min_fee */ false, &warnings);

            bool add_to_wallet = true;
            if (options.exists("add_to_wallet")) {
                add_to_wallet = options["add_to_wallet"].get_bool();
            }

            // Make a blank psbt
            PartiallySignedTransaction psbtx(rawTx, 2 /* version */);

            // First fill transaction with our data without signing,
            // so external signers are not asked sign more than once.
            bool complete;
            pwallet->FillPSBT(psbtx, complete, SIGHASH_DEFAULT, false, true, true);
            const TransactionError err = pwallet->FillPSBT(psbtx, complete, SIGHASH_DEFAULT, true, false, true);
            if (err != TransactionError::OK) {
                throw JSONRPCTransactionError(err);
            }

            CMutableTransaction mtx;
            complete = FinalizeAndExtractPSBT(psbtx, mtx);

            UniValue result(UniValue::VOBJ);

            if (psbt_opt_in || !complete || !add_to_wallet) {
                // Serialize the PSBT
                CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
                ssTx << psbtx;
                result.pushKV("psbt", EncodeBase64(ssTx.str()));
            }

            if (complete) {
                std::string err_string;
                std::string hex = EncodeHexTx(CTransaction(mtx));
                CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
                result.pushKV("txid", tx->GetHash().GetHex());
                if (add_to_wallet && !psbt_opt_in) {
                    pwallet->CommitTransaction(tx, {}, {} /* orderForm */);
                } else {
                    result.pushKV("hex", hex);
                }
            }
            result.pushKV("complete", complete);
            PushFundTxWarnings(result, warnings);

            return result;
        }
    };
}

RPCHelpMan walletprocesspsbt()
{
    return RPCHelpMan{"walletprocesspsbt",
                "\nUpdate a PSBT with input information from our wallet and then sign inputs\n"
                "that we can sign for." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction base64 string"},
                    {"sign", RPCArg::Type::BOOL, RPCArg::Default{true}, "Also sign the transaction when updating (requires wallet to be unlocked)"},
                    {"sighashtype", RPCArg::Type::STR, RPCArg::Default{"DEFAULT for Taproot, ALL otherwise"}, "The signature hash type to sign with if not specified by the PSBT. Must be one of\n"
            "       \"DEFAULT\"\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                    {"bip32derivs", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include BIP 32 derivation paths for public keys if we know them"},
                    {"finalize", RPCArg::Type::BOOL, RPCArg::Default{true}, "Also finalize inputs if possible"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psbt", "The base64-encoded partially signed transaction"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    },
                },
                RPCExamples{
                    HelpExampleCli("walletprocesspsbt", "\"psbt\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_elementsmode)
        throw std::runtime_error("PSBT operations are disabled when not in elementsmode.\n");

    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    const CWallet& wallet{*pwallet};
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    RPCTypeCheck(request.params, {UniValue::VSTR});

    // Unserialize the transaction
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodeBase64PSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("TX decode failed %s", error));
    }

    // Get the sighash type
    int nHashType = ParseSighashString(request.params[2]);

    // Don't sign, just fill data.
    bool bip32derivs = request.params[3].isNull() ? true : request.params[3].get_bool();
    bool finalize = request.params[4].isNull() ? true : request.params[4].get_bool();
    bool complete = true;


    const TransactionError err{wallet.FillPSBT(psbtx, complete, nHashType, false, bip32derivs, true, nullptr, true, false)};
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    // If not blinded but needs blinding, blind
    bool needs_blinding = false;
    for (const PSBTOutput& output : psbtx.outputs) {
        if (output.IsBlinded() && !output.IsFullyBlinded()) {
            needs_blinding = true;
            break;
        }
    }
    if (needs_blinding) {
        BlindingStatus status = pwallet->WalletBlindPSBT(psbtx);
        // Fail if we couldn't blind, but only if it is for reasons other than needing UTXOs
        if (status != BlindingStatus::OK && status != BlindingStatus::NEEDS_UTXOS) {
            throw JSONRPCError(RPC_WALLET_ERROR, GetBlindingStatusError(status));
        }
    }

    // If fully blinded, sign if we want to
    if (psbtx.IsFullyBlinded()) {
        bool sign = request.params[1].isNull() ? true : request.params[1].get_bool();
        if (sign) {
            EnsureWalletIsUnlocked(*pwallet);
            const TransactionError err = pwallet->FillPSBT(psbtx, complete, nHashType, sign, bip32derivs, true, nullptr, true, finalize);
            if (err != TransactionError::OK) {
                throw JSONRPCTransactionError(err);
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("complete", complete);

    return result;
},
    };
}

RPCHelpMan walletcreatefundedpsbt()
{
    return RPCHelpMan{"walletcreatefundedpsbt",
                "\nCreates and funds a transaction in the Partially Signed Transaction format.\n"
                "Implements the Creator and Updater roles.\n"
                "All existing inputs must either have their previous output transaction be in the wallet\n"
                "or be in the UTXO set. Solving data must be provided for non-wallet inputs.\n",
                {
                    {"inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "Leave empty to add inputs automatically. See add_inputs option.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::DefaultHint{"depends on the value of the 'locktime' and 'options.replaceable' arguments"}, "The sequence number"},
                                    {"pegin_bitcoin_tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The raw bitcoin transaction (in hex) depositing bitcoin to the mainchain_address generated by getpeginaddress"},
                                    {"pegin_txout_proof", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A rawtxoutproof (in hex) generated by the mainchain daemon's `gettxoutproof` containing a proof of only bitcoin_tx"},
                                    {"pegin_claim_script", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The witness program generated by getpeginaddress."},
                                    {"issuance_amount", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The amount to be issued"},
                                    {"issuance_tokens", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The number of asset issuance tokens to generate"},
                                    {"asset_entropy", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "For new asset issuance, this is any additional entropy to be used in the asset tag calculation. For reissuance, this is the original asaset entropy"},
                                    {"asset_blinding_nonce", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Do not set for new asset issuance. For reissuance, this is the blinding factor for reissuance token output for the asset being reissued"},
                                    {"blind_reissuance",  RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to mark the issuance input for blinding or not. Only affects issuances with re-issuance tokens."},
                                    {"weight", RPCArg::Type::NUM, RPCArg::DefaultHint{"Calculated from wallet and solving data"}, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                                },
                            },
                        },
                        },
                    {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs (key-value pairs), where none of the keys are duplicated.\n"
                            "That is, each address can only appear once and there can only be one 'data' object.\n"
                            "For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
                            "accepted as second parameter.",
                        {
                            {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                                {
                                    {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                                    {"blinder_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The index of the input whose signer will blind this output. Must be provided if this output is to be blinded"},
                                    {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The asset tag for this output if it is not the main chain asset"},
                                },
                                },
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                                },
                            },
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        Cat<std::vector<RPCArg>>(
                        {
                            {"add_inputs", RPCArg::Type::BOOL, RPCArg::Default{false}, "If inputs are specified, automatically include more if they are not enough."},
                            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                            {"changeAddress", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"pool address"}, "The address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch only"},
                            {"lockUnspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                            {"feeRate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_UNIT + "/kvB."},
                            {"fee_asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Label or hex ID of the asset used to pay the fee. On a chain with the open fee market (con_any_asset_fees) the fee asset must be named, because nothing is defaulted or inferred -- unless the transaction already determines it, which it does when it carries an explicit fee output (that output names the asset) or when subtract_fee_from_outputs is used (the fee comes out of those outputs, so it is denominated in theirs). Where it is determined, this must be omitted."},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "The outputs to subtract the fee from.\n"
                                                          "The fee will be equally deducted from the amount of each specified output.\n"
                                                          "Those recipients will receive less coins than you enter in their corresponding amount field.\n"
                                                          "If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                        },
                        FundTxDoc()),
                        "options"},
                    {"bip32derivs", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include BIP 32 derivation paths for public keys if we know them"},
                    {"psbt_version", RPCArg::Type::NUM, RPCArg::Default{2}, "The PSBT version number to use."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psbt", "The resulting raw transaction (base64-encoded string)"},
                        {RPCResult::Type::STR_AMOUNT, "fee", g_con_any_asset_fees ? "Fee that the resulting transaction pays, denominated in the asset specified by 'fee_asset'" : "Fee in " + CURRENCY_UNIT + " the resulting transaction pays"},
                        {RPCResult::Type::STR_AMOUNT, "fee_asset", /* optional */ g_con_any_asset_fees, "Asset that the fee is paid with"},
                        {RPCResult::Type::STR_AMOUNT, "fee_value", /* optional */ g_con_any_asset_fees, "Fee that the resulting transaction pays, denominated in " + CURRENCY_UNIT},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                        FundTxWarningsDoc(),
                    }
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("walletcreatefundedpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
                                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_elementsmode)
        throw std::runtime_error("PSBT operations are disabled when not in elementsmode.\n");

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    CWallet& wallet{*pwallet};
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    RPCTypeCheck(request.params, {
        UniValue::VARR,
        UniValueType(), // ARR or OBJ, checked later
        UniValue::VNUM,
        UniValue::VOBJ,
        UniValue::VBOOL,
        UniValue::VNUM,
        }, true
    );

    UniValue options = request.params[3];

    CAmount fee;
    int change_position;
    bool rbf{wallet.m_signal_rbf};
    const UniValue &replaceable_arg = options["replaceable"];
    if (!replaceable_arg.isNull()) {
        RPCTypeCheckArgument(replaceable_arg, UniValue::VBOOL);
        rbf = replaceable_arg.isTrue();
    }
    // It's hard to control the behavior of FundTransaction, so we will wait
    //   until after it's done, then extract the blinding keys from the output
    //   nonces.
    std::map<CTxOut, PSBTOutput> psbt_outs;
    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2], rbf, wallet.chain().getTip(), &psbt_outs, true /* allow_peg_in */, true /* allow_issuance */);

    // Make a blank psbt
    uint32_t psbt_version = 2;
    if (!request.params[5].isNull()) {
        psbt_version = request.params[5].get_int();
    }
    if (psbt_version != 2) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "The PSBT version can only be 2");
    }

    // Make a blank psbt
    std::set<uint256> new_assets;
    std::set<uint256> new_reissuance;
    for (unsigned int i = 0; i < rawTx.vin.size(); ++i) {
        if (!rawTx.vin[i].assetIssuance.IsNull()) {
            const UniValue& blind_reissuance_v = find_value(request.params[0].get_array()[i].get_obj(), "blind_reissuance");
            bool blind_reissuance = blind_reissuance_v.isNull() ? true : blind_reissuance_v.get_bool();
            uint256 entropy;
            CAsset asset;
            CAsset token;

            if (rawTx.vin[i].assetIssuance.assetBlindingNonce.IsNull()) {
                // New issuance, calculate the final entropy
                GenerateAssetEntropy(entropy, rawTx.vin[i].prevout, rawTx.vin[i].assetIssuance.assetEntropy);
            } else {
                // Reissuance, use original entropy set in assetEntropy
                entropy = rawTx.vin[i].assetIssuance.assetEntropy;
            }

            CalculateAsset(asset, entropy);
            new_assets.insert(asset.id);

            if (!rawTx.vin[i].assetIssuance.nInflationKeys.IsNull()) {
                // Calculate reissuance asset tag if there will be reissuance tokens
                CalculateReissuanceToken(token, entropy, blind_reissuance);
                new_reissuance.insert(token.id);
            }
        }
    }
    CCoinControl coin_control;
    // Automatically select coins, unless at least one is manually selected. Can
    // be overridden by options.add_inputs.
    coin_control.m_add_inputs = rawTx.vin.size() == 0;
    // FundTransaction expects blinding keys, if present, to appear in the output nonces
    for (CTxOut& txout : rawTx.vout) {
        auto search_it = psbt_outs.find(txout);
        CHECK_NONFATAL (search_it != psbt_outs.end());
        CPubKey& blind_pub = search_it->second.m_blinding_pubkey;
        if (blind_pub.IsFullyValid()) {
            txout.nNonce.vchCommitment = std::vector<unsigned char>(blind_pub.begin(), blind_pub.end());
        }
    }
    SetOptionsInputWeights(request.params[0], options);
    std::vector<bilingual_str> warnings;
    FundTransaction(wallet, rawTx, fee, change_position, options, coin_control, /* override_min_fee */ true, &warnings);
    // Find an input that is ours
    unsigned int blinder_index = 0;
    {
        LOCK(wallet.cs_wallet);
        for (; blinder_index < rawTx.vin.size(); ++blinder_index) {
            const CTxIn& txin = rawTx.vin[blinder_index];
            if (InputIsMine(wallet, txin) != ISMINE_NO) {
                break;
            }
        }
    }
    CHECK_NONFATAL (blinder_index < rawTx.vin.size()); // We added inputs, or existing inputs are ours, we should have a blinder index at this point.
    // It may add outputs (change, and in some edge case OP_RETURN) which need to be
    // blinded. So pull these into `psbt_outs`.
    for (const CTxOut& txout : rawTx.vout) {
        if (!txout.nNonce.IsNull() && !psbt_outs.count(txout)) {
            PSBTOutput new_out{2}; // psbtv2 output
            new_out.m_blinding_pubkey.Set(txout.nNonce.vchCommitment.begin(), txout.nNonce.vchCommitment.end());
            new_out.m_blinder_index = blinder_index;
            psbt_outs.insert(std::make_pair(txout, new_out));
        }
    }
    PartiallySignedTransaction psbtx(rawTx, psbt_version);
    for (unsigned int i = 0; i < rawTx.vout.size(); ++i) {
        PSBTOutput& output = psbtx.outputs[i];
        auto it = psbt_outs.find(rawTx.vout.at(i));
        if (it != psbt_outs.end()) {
            PSBTOutput& construct_psbt_out = it->second;

            output.m_blinding_pubkey = construct_psbt_out.m_blinding_pubkey;
            output.m_blinder_index = construct_psbt_out.m_blinder_index;
        }

        if (output.m_blinder_index == std::nullopt) {
            output.m_blinder_index = blinder_index;
        }

        // Check the asset
        if (new_assets.count(output.m_asset) > 0) {
            new_assets.erase(output.m_asset);
        }
        if (new_reissuance.count(output.m_asset) > 0) {
            new_reissuance.erase(output.m_asset);
        }
    }

    // Make sure all newly issued assets and reissuance tokens had outputs
    if (new_assets.size() > 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing output for new assets");
    }
    if (new_reissuance.size() > 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing output for reissuance tokens");
    }

    // Determine whether to include explicit values
    bool include_explicit = request.params[3].exists("include_explicit") && request.params[3]["include_explicit"].get_bool();

    // Fill transaction with out data but don't sign
    bool bip32derivs = request.params[4].isNull() ? true : request.params[4].get_bool();
    bool complete = true;
    const TransactionError err{wallet.FillPSBT(psbtx, complete, 1, false, bip32derivs, true, nullptr, include_explicit)};
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;

    UniValue result(UniValue::VOBJ);
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("fee", ValueFromAmount(fee));
    if (g_con_any_asset_fees) {
        CAsset fee_asset = coin_control.m_fee_asset.value_or(::policyAsset);
        CValue fee_value = ExchangeRateMap::GetInstance().ConvertAmountToValue(fee, fee_asset);
        result.pushKV("fee_asset", fee_asset.GetHex());
        result.pushKV("fee_value", ValueFromAmount(fee_value.GetValue()));
    }
    result.pushKV("changepos", change_position);
    PushFundTxWarnings(result, warnings);
    return result;
},
    };
}
} // namespace wallet
