// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetsdir.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <exchangerates.h>
#include <interfaces/chain.h>
#include <issuance.h>
#include <key_io.h>
#include <mainchainrpc.h>
#include <policy/policy.h>
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
#include <wallet/wallet.h>

#include <univalue.h>

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
    int fund_height{-1};         //!< height the funding output confirmed at
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
            std::unique_ptr<FlatSigningProvider> provider = desc->GetSigningProvider(pubkey);
            if (provider && provider->GetKey(keyid, key_out)) return true;
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
    std::vector<StakeUtxo> found;
    for (const auto& [wtxid, wtx] : wallet.mapWallet) {
        for (uint32_t n = 0; n < wtx.tx->vout.size(); ++n) {
            const CTxOut& out = wtx.tx->vout[n];
            if (!StakeFromTxOut(out)) continue; // qualifying stake: explicit policy asset, lock >= chain minimum
            auto parsed = ParseStakeScriptFull(out.scriptPubKey);
            if (!parsed) continue;
            if (only_pubkey && parsed->pubkey != *only_pubkey) continue;
            if (!WalletControlsStakerKey(wallet, parsed->pubkey)) continue;
            if (wallet.IsSpent(wtxid, n)) continue; // e.g. an earlier withdrawal still in the mempool
            StakeUtxo s;
            s.outpoint = COutPoint(wtxid, n);
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
        const Coin& coin = coins[s.outpoint];
        if (coin.out.IsNull()) continue; // spent, or not yet confirmed
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
    return g_pos_unbonding_period > 0 ? PosRequiredUnbondingSeconds() / (int64_t)g_pos_unbonding_period : 30;
}

//! Why an immature staking output cannot be withdrawn yet, with when it can:
//! "unbonding until block 45120 (around 2026-08-06T10:00:00Z)".
std::string DescribeImmaturity(const StakeUtxo& s, int tip_height, int64_t tip_time)
{
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
                        {RPCResult::Type::NUM, "funded_height", "height the stake confirmed at"},
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
        o.pushKV("funded_height", s.fund_height);
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
    CAmount mature_total = 0, immature_total = 0;
    const StakeUtxo* soonest = nullptr;
    for (const StakeUtxo& s : stakes) {
        if (s.Mature()) {
            mature_total += s.amount;
            mature.push_back(s);
        } else {
            immature_total += s.amount;
            // The stake that unlocks first, for the "try again when" message.
            if (!soonest || (s.spendable_height >= 0 && soonest->spendable_height >= 0
                                 ? s.spendable_height < soonest->spendable_height
                                 : s.spendable_time < soonest->spendable_time)) {
                soonest = &s;
            }
        }
    }
    if (mature.empty()) {
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
    if (want && *want < mature_total && !only) {
        std::set<CPubKey> keys;
        for (const StakeUtxo& s : mature) keys.insert(s.parsed.pubkey);
        if (keys.size() > 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "this wallet stakes with more than one key; pass the staker pubkey to withdraw a specific amount");
        }
    }

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
            ++naddr;
        }
    }

    UniValue result(UniValue::VOBJ);
    if (naddr == 0) {
        result.pushKV("btc", ValueFromAmount(0));
        result.pushKV("addresses", 0);
        result.pushKV("parent_height", 0);
        result.pushKV("error", "no receiving addresses yet - create one on the Receive tab");
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
    try {
        UniValue reply = CallMainChainRPC("scantxoutset", params);
        if (reply.exists("error") && !reply["error"].isNull()) {
            const UniValue& e = reply["error"];
            err = (e.isObject() && e.exists("message")) ? e["message"].get_str() : e.write();
        } else if (reply.exists("result") && reply["result"].isObject()) {
            const UniValue& res = reply["result"];
            if (res.exists("total_amount")) total = AmountFromValue(res["total_amount"]);
            if (res.exists("height") && res["height"].isNum()) parent_height = res["height"].get_int();
        }
    } catch (const std::exception& e) {
        err = std::string("parent chain unreachable: ") + e.what();
    } catch (...) {
        err = "parent chain query failed";
    }

    result.pushKV("btc", ValueFromAmount(total));
    result.pushKV("addresses", naddr);
    result.pushKV("parent_height", parent_height);
    result.pushKV("error", err);
    return result;
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
                    {"fee_asset_label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Hex asset id or asset label for fee payment."},
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

    if (g_con_any_asset_fees) {
        // Default to using the same asset being sent in the transaction
        CAsset feeAsset = asset;
        if (request.params.size() > 12 && request.params[12].isStr() && !request.params[12].get_str().empty()) {
            std::string strFeeAsset = request.params[12].get_str();
            feeAsset = GetAssetFromString(strFeeAsset);
            if (feeAsset.IsNull()) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex for fee: %s", feeAsset.GetHex()));
            }
        }
        coin_control.m_fee_asset = feeAsset;
    }

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
                    {"fee_asset", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"not set, fall back to asset being sent"}, "label or hex ID of asset used for fees"},                    
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
    if (g_con_any_asset_fees && !recipients.empty()) {
        CAsset feeAsset = recipients[0].asset;
        if (request.params.size() > 11) {
            std::string strFeeAsset = request.params[11].get_str();
            feeAsset = GetAssetFromString(strFeeAsset);
            if (feeAsset.IsNull()) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex for fee: %s", feeAsset.GetHex()));
            }
        }
        coin_control.m_fee_asset = feeAsset;
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
    };
}

void FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, const UniValue& options, CCoinControl& coinControl, bool override_min_fee)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    change_position = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;
    if (g_con_any_asset_fees && !tx.vout.empty()) {
        coinControl.m_fee_asset = tx.vout[0].nAsset.GetAsset();
    }

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
            },
            true, true);

        if (options.exists("add_inputs") ) {
            coinControl.m_add_inputs = options["add_inputs"].get_bool();
        }

        if (g_con_any_asset_fees) {
            if (options.exists("fee_asset")) {
                std::string strFeeAsset = options["fee_asset"].get_str();
                CAsset feeAsset = GetAssetFromString(strFeeAsset);
                if (feeAsset.IsNull()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex for fee: %s", feeAsset.GetHex()));
                }
                coinControl.m_fee_asset = feeAsset;
            }
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
                destinations[coinControl.m_fee_asset.value_or(::policyAsset)] = dest;
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

    if (!FundTransaction(wallet, tx, fee_out, change_position, error, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
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
                            {"fee_asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "The hex id or label of asset used for fee payment."},
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
    FundTransaction(*pwallet, tx, fee, change_position, request.params[1], coin_control, /* override_min_fee */ true);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    result.pushKV("fee", ValueFromAmount(fee));
    if (g_con_any_asset_fees) {
        result.pushKV("fee_asset", coin_control.m_fee_asset.value_or(::policyAsset).GetHex());
    }
    result.pushKV("changepos", change_position);

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
                    {RPCResult::Type::STR, "psbt", /*optional=*/true, "If more signatures are needed, or if add_to_wallet is false, the base64-encoded (partially) signed transaction"}
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
            FundTransaction(*pwallet, rawTx, fee, change_position, options, coin_control, /* override_min_fee */ false);

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
    FundTransaction(wallet, rawTx, fee, change_position, options, coin_control, /* override_min_fee */ true);
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
    return result;
},
    };
}
} // namespace wallet
