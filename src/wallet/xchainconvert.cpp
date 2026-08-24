// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/xchainconvert.h>

#include <chainparams.h>
#include <core_io.h>
#include <fs.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <mainchainrpc.h>
#include <primitives/bitcoin/transaction.h>
#include <random.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <seqobcourier.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/time.h>
#include <exchangerates.h>
#include <policy/policy.h>
#include <wallet/coincontrol.h>
#include <crypto/sha256.h>
#include <script/interpreter.h>
#include <wallet/htlc.h>
#include <wallet/rewardconvert.h>
#include <wallet/parentchain.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <optional>
#include <fstream>

namespace wallet {

UniValue MainChainPayload(const std::string& method, const UniValue& reply)
{
    // A JSON-RPC reply is an envelope: {"result": ..., "error": ..., "id": ...}.
    // Reaching for a payload field on the envelope itself finds nothing and says
    // nothing about why, so an unreadable reply has to be made loud here.
    const UniValue err = find_value(reply, "error");
    if (!err.isNull()) {
        throw std::runtime_error(strprintf("%s: %s", method, err.write()));
    }
    if (!reply.isObject() || !reply.exists("result")) {
        throw std::runtime_error(strprintf("%s: reply carried no result", method));
    }
    return find_value(reply, "result");
}

namespace {

//! How long to give the maker at each step. A live maker answers in seconds;
//! these are the points at which we stop waiting and (before funding) simply
//! try the next resting offer.
constexpr int TERMS_TIMEOUT_S = 30;
constexpr int SESSION_TIMEOUT_S = 20;

//! How long to wait for the maker's Bitcoin lock to confirm, and how often to
//! look. Bounded by the lock's own timelock in practice; this is the wall-clock
//! guard for a maker that funded nothing.
constexpr int BTC_CONF_POLL_S = 30;
constexpr int BTC_CONF_MAX_WAIT_S = 3 * 60 * 60;

//! When a swap that never committed anything is written off. It has to be
//! comfortably longer than the longest a live pass can hold one -- otherwise
//! the resume pass would retire a swap another thread is still working on, and
//! then that thread would fund against a record already marked dead.
constexpr int STRANDED_AFTER_S = BTC_CONF_MAX_WAIT_S + 60 * 60;

//! A parent-chain fee rate this node will not exceed no matter what
//! estimatesmartfee says. testnet4's estimator runs hot enough to eat a small
//! claim whole, and a claim that pays more than it collects is not a claim.
constexpr CAmount MAX_PARENT_FEERATE_SAT_VB = 50;

//! The claim transaction's size, near enough: one P2WSH input carrying a
//! signature, a 32-byte preimage and the script, and one P2WPKH output.
constexpr int64_t CLAIM_VSIZE = 175;

fs::path SwapsPath(const CWallet& wallet)
{
    return fs::PathFromString(wallet.GetDatabase().Filename()).parent_path() / "xchainswaps.json";
}

std::string Hex(const std::vector<unsigned char>& v) { return HexStr(v); }
std::vector<unsigned char> UnHex(const UniValue& v)
{
    if (!v.isStr() || !IsHex(v.get_str())) return {};
    return ParseHex(v.get_str());
}

const UniValue& F(const UniValue& o, const std::string& a, const std::string& b)
{
    static const UniValue null_value;
    if (!o.isObject()) return null_value;
    if (o.exists(a)) return o[a];
    if (o.exists(b)) return o[b];
    return null_value;
}

int64_t NumField(const UniValue& v)
{
    if (v.isNum()) return v.get_int64();
    if (v.isStr()) {
        const std::string s = v.get_str();
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return 0;
        return strtoll(s.c_str(), nullptr, 10);
    }
    return 0;
}

UniValue SwapToJson(const XchainSwap& s)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("state", s.state);
    o.pushKV("time", s.time);
    o.pushKV("offer_id", s.offer_id);
    o.pushKV("maker_pubkey", s.maker_pubkey);
    o.pushKV("asset", s.asset.GetHex());
    o.pushKV("seq_amount", s.seq_amount);
    o.pushKV("btc_amount", s.btc_amount);
    o.pushKV("hash_h", Hex(s.hash_h));
    o.pushKV("maker_seq_claim_pub", Hex(s.maker_seq_claim_pub));
    o.pushKV("maker_btc_refund_pub", Hex(s.maker_btc_refund_pub));
    o.pushKV("taker_seq_refund_pub", Hex(s.taker_seq_refund_pub));
    o.pushKV("taker_btc_claim_pub", Hex(s.taker_btc_claim_pub));
    o.pushKV("taker_seq_refund_priv", Hex(s.taker_seq_refund_priv));
    o.pushKV("taker_btc_claim_priv", Hex(s.taker_btc_claim_priv));
    o.pushKV("seq_locktime", (int64_t)s.seq_locktime);
    o.pushKV("btc_locktime", (int64_t)s.btc_locktime);
    o.pushKV("btc_leg_txid", s.btc_leg_txid);
    o.pushKV("btc_leg_vout", (int64_t)s.btc_leg_vout);
    o.pushKV("btc_leg_script", Hex(s.btc_leg_script));
    o.pushKV("btc_leg_amount", s.btc_leg_amount);
    o.pushKV("btc_leg_height", (int64_t)s.btc_leg_height);
    o.pushKV("seq_fund_txid", s.seq_fund_txid);
    o.pushKV("seq_fund_vout", (int64_t)s.seq_fund_vout);
    o.pushKV("seq_redeem", Hex(s.seq_redeem));
    o.pushKV("preimage", Hex(s.preimage));
    o.pushKV("btc_claim_txid", s.btc_claim_txid);
    o.pushKV("seq_refund_txid", s.seq_refund_txid);
    o.pushKV("error", s.error);
    return o;
}

XchainSwap SwapFromJson(const UniValue& o)
{
    XchainSwap s;
    s.state = F(o, "state", "state").isStr() ? o["state"].get_str() : "";
    s.time = NumField(F(o, "time", "time"));
    if (o.exists("offer_id") && o["offer_id"].isStr()) s.offer_id = o["offer_id"].get_str();
    if (o.exists("maker_pubkey") && o["maker_pubkey"].isStr()) s.maker_pubkey = o["maker_pubkey"].get_str();
    if (o.exists("asset") && o["asset"].isStr() && IsHex(o["asset"].get_str())) s.asset = CAsset(uint256S(o["asset"].get_str()));
    s.seq_amount = NumField(F(o, "seq_amount", "seq_amount"));
    s.btc_amount = NumField(F(o, "btc_amount", "btc_amount"));
    s.hash_h = UnHex(F(o, "hash_h", "hash_h"));
    s.maker_seq_claim_pub = UnHex(F(o, "maker_seq_claim_pub", "maker_seq_claim_pub"));
    s.maker_btc_refund_pub = UnHex(F(o, "maker_btc_refund_pub", "maker_btc_refund_pub"));
    s.taker_seq_refund_pub = UnHex(F(o, "taker_seq_refund_pub", "taker_seq_refund_pub"));
    s.taker_btc_claim_pub = UnHex(F(o, "taker_btc_claim_pub", "taker_btc_claim_pub"));
    s.taker_seq_refund_priv = UnHex(F(o, "taker_seq_refund_priv", "taker_seq_refund_priv"));
    s.taker_btc_claim_priv = UnHex(F(o, "taker_btc_claim_priv", "taker_btc_claim_priv"));
    s.seq_locktime = (uint32_t)NumField(F(o, "seq_locktime", "seq_locktime"));
    s.btc_locktime = (uint32_t)NumField(F(o, "btc_locktime", "btc_locktime"));
    if (o.exists("btc_leg_txid") && o["btc_leg_txid"].isStr()) s.btc_leg_txid = o["btc_leg_txid"].get_str();
    s.btc_leg_vout = (uint32_t)NumField(F(o, "btc_leg_vout", "btc_leg_vout"));
    s.btc_leg_script = UnHex(F(o, "btc_leg_script", "btc_leg_script"));
    s.btc_leg_amount = NumField(F(o, "btc_leg_amount", "btc_leg_amount"));
    s.btc_leg_height = (int)NumField(F(o, "btc_leg_height", "btc_leg_height"));
    if (o.exists("seq_fund_txid") && o["seq_fund_txid"].isStr()) s.seq_fund_txid = o["seq_fund_txid"].get_str();
    s.seq_fund_vout = (uint32_t)NumField(F(o, "seq_fund_vout", "seq_fund_vout"));
    s.seq_redeem = UnHex(F(o, "seq_redeem", "seq_redeem"));
    s.preimage = UnHex(F(o, "preimage", "preimage"));
    if (o.exists("btc_claim_txid") && o["btc_claim_txid"].isStr()) s.btc_claim_txid = o["btc_claim_txid"].get_str();
    if (o.exists("seq_refund_txid") && o["seq_refund_txid"].isStr()) s.seq_refund_txid = o["seq_refund_txid"].get_str();
    if (o.exists("error") && o["error"].isStr()) s.error = o["error"].get_str();
    return s;
}

void StoreSwaps(const CWallet& wallet, const std::vector<XchainSwap>& swaps)
{
    UniValue arr(UniValue::VARR);
    for (const XchainSwap& s : swaps) arr.push_back(SwapToJson(s));
    std::ofstream f(fs::PathToString(SwapsPath(wallet)), std::ios::trunc);
    f << arr.write(1) << std::endl;
}

//! Replace (or append) one swap by offer id, and write the file. The record is
//! the difference between a recoverable interruption and a stranded swap, so it
//! is written at every step rather than at the end.
void SaveSwap(const CWallet& wallet, const XchainSwap& s)
{
    auto all = LoadXchainSwaps(wallet);
    bool found = false;
    for (auto& e : all) {
        if (e.offer_id == s.offer_id && e.time == s.time) { e = s; found = true; break; }
    }
    if (!found) all.push_back(s);
    if (all.size() > 100) all.erase(all.begin(), all.begin() + (all.size() - 100));
    StoreSwaps(wallet, all);
}

// ---- parent chain -------------------------------------------------------

//! CallMainChainRPC hands back the whole JSON-RPC reply, not the result inside
//! it. Reading a payload field straight off that reply finds nothing, silently:
//! the fee estimate fell back to its default on every call, and the maker's
//! Bitcoin lock could never be verified at all, which would have aborted every
//! cross-chain conversion at its first look at the parent chain. Nothing here
//! reads the parent chain any other way.
UniValue MainChainResult(const std::string& method, const UniValue& params)
{
    return MainChainPayload(method, CallMainChainRPC(method, params));
}

//! One output on the parent chain, as the node's own Bitcoin daemon reports it.
struct ParentOut {
    bool found{false};
    CScript spk;
    CAmount value{0};
    int height{0};
    int confirmations{0};
};

ParentOut ReadParentOutput(const std::string& txid, uint32_t vout)
{
    ParentOut out;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(txid);
        params.push_back(true);
        const UniValue tx = MainChainResult("getrawtransaction", params);
        if (!tx.isObject() || !tx.exists("vout") || !tx["vout"].isArray()) return out;
        if (vout >= tx["vout"].size()) return out;
        const UniValue& o = tx["vout"][vout];
        if (!o.isObject()) return out;
        const UniValue& spk = o["scriptPubKey"];
        if (!spk.isObject() || !spk["hex"].isStr()) return out;
        const auto bytes = ParseHex(spk["hex"].get_str());
        out.spk = CScript(bytes.begin(), bytes.end());
        // Values arrive as decimal BTC; the atoms are what matters.
        out.value = AmountFromValue(o["value"]);
        out.confirmations = tx.exists("confirmations") ? (int)tx["confirmations"].get_int64() : 0;
        if (tx.exists("blockhash") && tx["blockhash"].isStr() && out.confirmations > 0) {
            UniValue bp(UniValue::VARR);
            bp.push_back(tx["blockhash"].get_str());
            const UniValue blk = MainChainResult("getblockheader", bp);
            if (blk.isObject() && blk.exists("height")) out.height = (int)blk["height"].get_int64();
        }
        out.found = true;
    } catch (const std::exception& e) {
        // Unreadable is not "absent": the caller must not treat a daemon that
        // is momentarily unavailable as a maker that funded nothing. Say why,
        // though -- the commonest cause is a parent daemon without -txindex,
        // which cannot look up a transaction it was not asked to watch, and
        // that is a five-second fix once somebody can see it.
        LogPrintf("[rewards] could not read %s:%d on the parent chain: %s\n", txid, vout, e.what());
        out.found = false;
    }
    return out;
}

//! What claiming on the parent chain costs per virtual byte.
//!
//! When the estimate cannot be had, the answer is the CEILING rather than the
//! floor, and never a cheerful default. Both callers are better served by
//! assuming the parent chain is expensive:
//!
//! - deciding whether a swap is worth making, a high guess declines the
//!   marginal ones, which is the safe way to be wrong;
//! - claiming, the asset has ALREADY been handed over -- the maker took it, and
//!   revealing the secret is how we learned to claim. Declining to claim
//!   because the fee is unknown would give up the Bitcoin and the asset both.
//!   Overpaying a claim already judged worth making is a far smaller loss.
//!
//! The old failure was worse than either: the estimate was never parsed at all,
//! so this quietly returned 2 sat/vB while the parent chain wanted fifty times
//! that.
CAmount ParentFeeratePerVb()
{
    try {
        UniValue params(UniValue::VARR);
        params.push_back(6);
        const UniValue r = MainChainResult("estimatesmartfee", params);
        if (r.isObject() && r.exists("feerate") && r["feerate"].isNum()) {
            // BTC/kvB -> sat/vB.
            const CAmount per_kvb = AmountFromValue(r["feerate"]);
            const CAmount per_vb = per_kvb / 1000;
            if (per_vb > 0) return std::min<CAmount>(per_vb, MAX_PARENT_FEERATE_SAT_VB);
        }
    } catch (const std::exception& e) {
        LogPrintf("[rewards] could not read the parent-chain fee rate (%s); "
                  "assuming the ceiling of %d sat/vB\n", e.what(), MAX_PARENT_FEERATE_SAT_VB);
        return MAX_PARENT_FEERATE_SAT_VB;
    }
    LogPrintf("[rewards] the parent chain gave no usable fee estimate; assuming the ceiling "
              "of %d sat/vB\n", MAX_PARENT_FEERATE_SAT_VB);
    return MAX_PARENT_FEERATE_SAT_VB;
}

//! This node's own Bitcoin anchor: the height the chain tip commits to. The
//! number the maker's gate is judged by, and the reason funding waits.
int TipAnchorHeight(CWallet& wallet)
{
    CBlock block;
    if (!wallet.chain().findBlock(wallet.GetLastBlockHash(), interfaces::FoundBlock().data(block))) return -1;
    return (int)block.m_anchor_height;
}

// ---- the asset leg ------------------------------------------------------

//! Fund the Sequentia side of the swap: `amount` of `asset` into the HTLC.
bool FundSeqHtlc(CWallet& wallet, const CScript& redeem, const CAsset& asset, CAmount amount,
                 std::string& txid_out, uint32_t& vout_out, std::string& error)
{
    const CScript spk = HtlcP2shSpk(redeem);

    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.vout.emplace_back(asset, amount, spk);

    CCoinControl cc;
    cc.m_add_inputs = true;
    CAmount fee = 0;
    int change_pos = -1;
    bilingual_str err;
    if (!FundTransaction(wallet, mtx, fee, change_pos, err, /*lockUnspents=*/true,
                         /*setSubtractFeeFromOutputs=*/{}, cc)) {
        error = "could not fund the asset leg: " + err.original;
        return false;
    }
    {
        LOCK(wallet.cs_wallet);
        std::map<int, bilingual_str> input_errors;
        if (!wallet.SignTransaction(mtx)) {
            error = "could not sign the asset leg";
            return false;
        }
    }
    CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string rejected;
    if (!wallet.chain().broadcastTransaction(tx, wallet.m_default_max_tx_fee, /*relay=*/true, rejected)) {
        error = "the asset leg was refused: " + rejected;
        return false;
    }
    txid_out = tx->GetHash().GetHex();
    for (size_t i = 0; i < tx->vout.size(); ++i) {
        if (tx->vout[i].scriptPubKey == spk) { vout_out = (uint32_t)i; break; }
    }
    return true;
}

//! Look for the maker's claim of our HTLC and read the secret out of it.
//!
//! The claim is an ordinary spend on this chain, so the node already has it --
//! no index and no explorer, just the blocks from our funding forward, which on
//! a minute-spaced chain is a handful even after an hour.
bool FindPreimageOnChain(CWallet& wallet, const XchainSwap& s, std::vector<unsigned char>& preimage)
{
    if (s.seq_fund_txid.empty()) return false;
    const uint256 fund_txid = uint256S(s.seq_fund_txid);
    const COutPoint target(fund_txid, s.seq_fund_vout);

    // Still unspent means the maker has not claimed.
    std::map<COutPoint, Coin> coins{{target, Coin()}};
    wallet.chain().findCoins(coins);
    if (!coins[target].IsSpent()) return false;

    int tip_height = wallet.GetLastBlockHeight();
    for (int h = tip_height; h >= 0 && h > tip_height - 2000; --h) {
        uint256 hash;
        if (!wallet.chain().findAncestorByHeight(wallet.GetLastBlockHash(), h,
                                                 interfaces::FoundBlock().hash(hash))) continue;
        CBlock block;
        if (!wallet.chain().findBlock(hash, interfaces::FoundBlock().data(block))) continue;
        for (const auto& tx : block.vtx) {
            for (size_t i = 0; i < tx->vin.size(); ++i) {
                if (tx->vin[i].prevout != target) continue;
                // The claim branch's scriptSig is <sig> <preimage> <1> <redeem>.
                // Take the 32-byte push that hashes to H and nothing else: a
                // push that merely looks the right size proves nothing.
                CScript::const_iterator it = tx->vin[i].scriptSig.begin();
                while (it != tx->vin[i].scriptSig.end()) {
                    opcodetype op;
                    std::vector<unsigned char> data;
                    if (!tx->vin[i].scriptSig.GetOp(it, op, data)) break;
                    if (data.size() != 32) continue;
                    unsigned char digest[CSHA256::OUTPUT_SIZE];
                    CSHA256().Write(data.data(), data.size()).Finalize(digest);
                    if (std::vector<unsigned char>(digest, digest + 32) == s.hash_h) {
                        preimage = data;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace

//! The parent chain's height, or nothing when it cannot be read.
//!
//! Exported because a swap's refund clock is measured in parent-chain blocks,
//! and telling a staker how long their asset stays locked means knowing where
//! that chain is. It lives here rather than at the call site so that every read
//! of the parent chain still goes through one door.
std::optional<int> ParentChainTip()
{
    try {
        const UniValue r = MainChainResult("getblockcount", UniValue(UniValue::VARR));
        if (r.isNum()) return r.get_int();
    } catch (const std::exception& e) {
        LogPrintf("[rewards] could not read the parent-chain height: %s\n", e.what());
    }
    return std::nullopt;
}

std::vector<XchainSwap> LoadXchainSwaps(const CWallet& wallet)
{
    std::vector<XchainSwap> out;
    std::ifstream f(fs::PathToString(SwapsPath(wallet)));
    if (!f.good()) return out;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue arr;
    if (!arr.read(data) || !arr.isArray()) return out;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].isObject()) out.push_back(SwapFromJson(arr[i]));
    }
    return out;
}


namespace {

//! A fresh keypair for one swap. Ephemeral, and persisted with the swap: a node
//! that forgot its refund key after a crash would have funded an asset it could
//! never reclaim, turning a recoverable interruption into a loss.
void NewSwapKey(std::vector<unsigned char>& priv, std::vector<unsigned char>& pub)
{
    CKey k;
    do { k.MakeNewKey(/*fCompressed=*/true); } while (!k.IsValid());
    priv.assign(k.begin(), k.end());
    const CPubKey p = k.GetPubKey();
    pub.assign(p.begin(), p.end());
}

CKey KeyFromBytes(const std::vector<unsigned char>& priv)
{
    CKey k;
    if (priv.size() == 32) k.Set(priv.begin(), priv.end(), /*fCompressedIn=*/true);
    return k;
}

//! What the maker must have locked for our slice: the offer's own ratio,
//! floored. Floor is the maker's favour, and it is the number the maker
//! independently recomputes -- agreeing on it is what makes a partial safe.
CAmount ProportionalBtcFloor(CAmount whole_btc, CAmount take, CAmount whole_asset)
{
    if (whole_asset <= 0 || take <= 0 || whole_btc <= 0) return 0;
    return (CAmount)(((__int128)whole_btc * take) / whole_asset);
}

//! Check the maker's Bitcoin lock against the PARENT CHAIN, not against what it
//! said. The message is a claim; the coin is the fact.
bool VerifyMakerBtcLeg(XchainSwap& s, std::string& why)
{
    const auto script_bytes = s.btc_leg_script;
    const CScript witness_script(script_bytes.begin(), script_bytes.end());
    const auto terms = ParseHtlcRedeemScript(witness_script);
    if (!terms) { why = "the maker's Bitcoin lock is not an HTLC"; return false; }
    if (terms->hash != s.hash_h) { why = "the maker's Bitcoin lock uses a different hash"; return false; }
    if (terms->claim_pub != s.taker_btc_claim_pub) {
        why = "the maker's Bitcoin lock cannot be claimed by this wallet";
        return false;
    }
    if (terms->locktime != s.btc_locktime) { why = "the maker's Bitcoin lock has a different timelock"; return false; }

    const ParentOut out = ReadParentOutput(s.btc_leg_txid, s.btc_leg_vout);
    if (!out.found) { why = "could not read the maker's Bitcoin lock from the parent chain"; return false; }
    if (out.spk != HtlcP2wshSpk(witness_script)) {
        why = "the output the maker pointed at does not pay the script it described";
        return false;
    }
    if (out.value < s.btc_amount) {
        why = strprintf("the maker locked %d satoshis, not the %d agreed", out.value, s.btc_amount);
        return false;
    }
    s.btc_leg_height = out.height;
    return true;
}

//! Spend the maker's Bitcoin HTLC with the revealed secret.
bool ClaimBtcLeg(CWallet& wallet, XchainSwap& s, std::string& error)
{
    if (s.preimage.size() != 32) { error = "no secret to claim with"; return false; }
    const CScript witness_script(s.btc_leg_script.begin(), s.btc_leg_script.end());

    // Where the Bitcoin goes: this wallet's own parent-chain address, which is
    // the same address it receives Bitcoin at anywhere else.
    CTxDestination dest;
    {
        LOCK(wallet.cs_wallet);
        bilingual_str err;
        if (!wallet.GetNewDestination(OutputType::BECH32, "reward conversion", dest, err)) {
            error = "could not get a Bitcoin address: " + err.original;
            return false;
        }
    }
    const CScript pay_to = GetScriptForDestination(dest);

    const CAmount fee = ParentFeeratePerVb() * CLAIM_VSIZE;
    if (s.btc_leg_amount <= fee) {
        error = strprintf("the Bitcoin leg (%d) does not cover the fee to claim it (%d)", s.btc_leg_amount, fee);
        return false;
    }

    Sidechain::Bitcoin::CMutableTransaction mtx;
    mtx.nVersion = 2;
    Sidechain::Bitcoin::CTxIn in;
    in.prevout = Sidechain::Bitcoin::COutPoint(uint256S(s.btc_leg_txid), s.btc_leg_vout);
    in.nSequence = 0xfffffffe;
    mtx.vin.push_back(in);
    Sidechain::Bitcoin::CTxOut out;
    out.nValue = s.btc_leg_amount - fee;
    out.scriptPubKey = pay_to;
    mtx.vout.push_back(out);

    const uint256 sighash = ParentBip143Sighash(mtx, 0, witness_script, s.btc_leg_amount);
    const CKey claim_key = KeyFromBytes(s.taker_btc_claim_priv);
    if (!claim_key.IsValid()) { error = "the claim key is unusable"; return false; }
    std::vector<unsigned char> sig;
    if (!claim_key.Sign(sighash, sig)) { error = "could not sign the claim"; return false; }
    sig.push_back(SIGHASH_ALL);

    // The claim branch: <sig> <preimage> <1> <script>. The 1 is what sends
    // OP_IF down the hashlock side.
    mtx.vin[0].scriptWitness.stack.clear();
    mtx.vin[0].scriptWitness.stack.push_back(sig);
    mtx.vin[0].scriptWitness.stack.push_back(s.preimage);
    mtx.vin[0].scriptWitness.stack.push_back({0x01});
    mtx.vin[0].scriptWitness.stack.emplace_back(witness_script.begin(), witness_script.end());

    const Sidechain::Bitcoin::CTransaction tx(mtx);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << tx;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(HexStr(ss));
        const UniValue r = MainChainResult("sendrawtransaction", params);
        s.btc_claim_txid = r.isStr() ? r.get_str() : tx.GetHash().GetHex();
    } catch (const std::exception& e) {
        error = std::string("the parent chain refused the claim: ") + e.what();
        return false;
    }
    return true;
}

//! Reclaim our own asset after the timelock, when the maker never claimed.
bool RefundSeqLeg(CWallet& wallet, XchainSwap& s, std::string& error)
{
    if (s.seq_fund_txid.empty()) { error = "nothing was funded"; return false; }
    const CScript redeem(s.seq_redeem.begin(), s.seq_redeem.end());
    const COutPoint outpoint(uint256S(s.seq_fund_txid), s.seq_fund_vout);

    std::map<COutPoint, Coin> coins{{outpoint, Coin()}};
    wallet.chain().findCoins(coins);
    const Coin& coin = coins[outpoint];
    if (coin.IsSpent()) { error = "already spent"; return false; }

    CTxDestination dest;
    {
        LOCK(wallet.cs_wallet);
        bilingual_str err;
        if (!wallet.GetNewDestination(OutputType::BECH32, "reward conversion refund", dest, err)) {
            error = "could not get a receiving address: " + err.original;
            return false;
        }
    }

    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = s.seq_locktime;           // the CLTV branch needs it
    CTxIn in(outpoint);
    in.nSequence = 0xfffffffe;                // and a sequence that lets locktime bind
    mtx.vin.push_back(in);

    // The HTLC output is explicit, so its value and asset are simply readable.
    if (!coin.out.nValue.IsExplicit() || !coin.out.nAsset.IsExplicit()) {
        error = "the funded leg is not explicit";
        return false;
    }
    const CAmount value = coin.out.nValue.GetAmount();
    const CAsset asset = coin.out.nAsset.GetAsset();

    // A refund pays its own fee out of the refunded amount: there is nothing
    // else in this transaction to pay it from.
    //
    // And it pays that fee in the asset being refunded, not the policy asset.
    // A refund of, say, GOLD holds nothing but GOLD, so a policy-asset fee
    // output would leave the transaction unbalanced in two assets at once --
    // short by the fee in the one it does hold, and conjuring the fee in one it
    // does not. Such a refund is rejected every time it is tried, which is the
    // worst possible moment to discover it: the refund is the promise that a
    // staker's asset comes back when a maker walks away. Sequentia's open fee
    // market is exactly what makes the fix the obvious one -- there is no asset
    // a fee has to be paid in.
    // Size the fee the way every fee on this chain is sized: a reference-unit
    // amount for the transaction's size, converted into whichever asset ends up
    // paying it. A flat atom count cannot be right for two assets at once --
    // the same 2000 atoms is dust in a cheap asset and, in a valuable one, a
    // fee so far above the going rate that the node refuses it outright. That
    // is not hypothetical on this chain: one live asset prices a whole staking
    // reward at single-digit atoms.
    //
    // Can the fee come out of the asset itself? Only if this node has a rate
    // for it, which is what "accepts it for fees" means where the fee market is
    // open. Usually it can, and for a pleasing reason: a staking reward IS a
    // fee somebody already paid, so a reward asset is one producers were
    // accepting. But rates come and go, and a refund that only works while a
    // rate happens to be listed is not the guarantee it is meant to be.
    const bool asset_pays_its_own_fee =
        g_con_any_asset_fees && ::minRelayTxFee.GetFee(1000, asset) > 0;
    const CAsset fee_asset = asset_pays_its_own_fee ? asset : ::policyAsset;

    // The fee follows the transaction's MEASURED size, not a guess at it.
    // Guessing high wastes a little; guessing low is unrecoverable in the way
    // that matters -- the refund is rejected, the asset stays locked, and the
    // next pass guesses exactly the same number and is rejected again. So the
    // transaction is built once to be measured and once to be sent. Elements
    // amounts are fixed-width, so the second build is the same size as the
    // first bar a byte of signature, which the margin covers.
    const uint32_t FIRST_GUESS_VSIZE = 380;
    const uint32_t SIG_WOBBLE = 2;
    CAmount fee = std::max<CAmount>(1, ::minRelayTxFee.GetFee(FIRST_GUESS_VSIZE, fee_asset));

    std::vector<COutput> fee_coins;
    CAmount fee_gathered = 0;
    if (asset_pays_its_own_fee) {
        if (value <= fee) { error = "the leg is too small to refund"; return false; }
    } else {
        // Fall back to the policy asset, out of the wallet's own coins. The
        // refunded asset then comes back whole, which is the better outcome
        // anyway -- it is the staker's asset, not the fee's.
        std::vector<COutput> avail;
        {
            LOCK(wallet.cs_wallet);
            AvailableCoins(wallet, avail, nullptr, 1, MAX_MONEY, MAX_MONEY, 0, &::policyAsset);
        }
        for (const COutput& c : avail) {
            if (fee_gathered >= fee) break;
            if (!c.fSpendable) continue;
            if (!(c.tx->GetOutputAsset(wallet, c.i) == ::policyAsset)) continue;
            if (!c.tx->tx->vout[c.i].nValue.IsExplicit()) continue;
            if (!c.tx->tx->vout[c.i].nAsset.IsExplicit()) continue;
            const CAmount v = c.tx->GetOutputValueOut(wallet, c.i);
            if (v <= 0) continue;
            fee_coins.push_back(c);
            fee_gathered += v;
        }
        if (fee_gathered < fee) {
            error = strprintf("this node does not take fees in %s, and the wallet has no %s to pay "
                              "the refund's fee with; the asset stays locked and refundable until "
                              "it does", asset.GetHex(), ::policyAsset.GetHex());
            return false;
        }
        for (const COutput& c : fee_coins) mtx.vin.emplace_back(COutPoint(c.tx->GetHash(), c.i));
    }

    CTxDestination change;
    if (!asset_pays_its_own_fee) {
        LOCK(wallet.cs_wallet);
        bilingual_str err;
        if (!wallet.GetNewDestination(OutputType::BECH32, "reward conversion refund", change, err)) {
            error = "could not get a change address: " + err.original;
            return false;
        }
    }

    const std::vector<CTxIn> vin_template = mtx.vin;
    for (int pass = 0; pass < 2; ++pass) {
    mtx.vin = vin_template;
    mtx.vout.clear();
    mtx.witness.SetNull();

    // A fee is not allowed to eat what it is paying to rescue. If sizing says
    // otherwise the rate is wrong, not the refund, and half is a limit that
    // gets the asset home either way.
    if (asset_pays_its_own_fee && fee > value / 2) fee = std::max<CAmount>(1, value / 2);
    if (asset_pays_its_own_fee && value <= fee) {
        error = "the leg is too small to refund";
        return false;
    }
    if (!asset_pays_its_own_fee && fee_gathered < fee) {
        error = strprintf("the wallet has only %d %s, and this refund needs %d for its fee",
                          fee_gathered, ::policyAsset.GetHex(), fee);
        return false;
    }

    if (asset_pays_its_own_fee) {
        mtx.vout.emplace_back(asset, value - fee, GetScriptForDestination(dest));
        mtx.vout.emplace_back(asset, fee, CScript());   // explicit fee output
    } else {
        mtx.vout.emplace_back(asset, value, GetScriptForDestination(dest));
        if (fee_gathered > fee) {
            mtx.vout.emplace_back(::policyAsset, fee_gathered - fee, GetScriptForDestination(change));
        }
        mtx.vout.emplace_back(::policyAsset, fee, CScript());
    }

    // The wallet's own fee inputs get signed first. That order is safe: the
    // timelock branch is signed with SIGHASH_ALL under the legacy rules, which
    // blank every other input's scriptSig, and the wallet's own signatures
    // commit to outpoints and outputs that are already final.
    //
    // The wallet is asked to sign a transaction one of whose inputs it can
    // never sign -- the contract is redeemed by a key held in the swap, not in
    // the keystore -- so a blanket failure from it means nothing here. What
    // matters is whether the inputs it WAS meant to sign came back signed, so
    // that is what gets checked.
    if (!fee_coins.empty()) {
        std::map<COutPoint, Coin> coins;
        for (const COutput& c : fee_coins) {
            Coin coin;
            coin.out = c.tx->tx->vout[c.i];
            coin.nHeight = 1;
            coins[COutPoint(c.tx->GetHash(), c.i)] = std::move(coin);
        }
        std::map<int, bilingual_str> input_errors;
        wallet.SignTransaction(mtx, coins, SIGHASH_ALL, input_errors);
        for (size_t i = 1; i < mtx.vin.size(); ++i) {
            const bool has_witness = i < mtx.witness.vtxinwit.size() &&
                                     !mtx.witness.vtxinwit[i].scriptWitness.IsNull();
            if (mtx.vin[i].scriptSig.empty() && !has_witness) {
                const auto it = input_errors.find((int)i);
                error = "could not sign the refund's fee input: " +
                        (it != input_errors.end() ? it->second.original : std::string("unknown"));
                return false;
            }
        }
    }

    const CKey refund_key = KeyFromBytes(s.taker_seq_refund_priv);
    if (!refund_key.IsValid()) { error = "the refund key is unusable"; return false; }

    const uint256 sighash = SignatureHash(redeem, mtx, 0, SIGHASH_ALL, coin.out.nValue,
                                          SigVersion::BASE, /*flags=*/0);
    std::vector<unsigned char> sig;
    if (!refund_key.Sign(sighash, sig)) { error = "could not sign the refund"; return false; }
    sig.push_back(SIGHASH_ALL);

    // The refund branch: <sig> <0> <redeem>. The 0 sends OP_IF down the
    // timelock side.
    mtx.vin[0].scriptSig = CScript() << sig << std::vector<unsigned char>()
                                     << std::vector<unsigned char>(redeem.begin(), redeem.end());

    if (pass == 0) {
        const uint32_t measured = GetVirtualTransactionSize(CTransaction(mtx)) + SIG_WOBBLE;
        fee = std::max<CAmount>(1, ::minRelayTxFee.GetFee(measured, fee_asset));
    }
    }   // end of the measure-then-send passes

    CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string rejected;
    if (!wallet.chain().broadcastTransaction(tx, wallet.m_default_max_tx_fee, /*relay=*/true, rejected)) {
        error = "the refund was refused: " + rejected;
        return false;
    }
    s.seq_refund_txid = tx->GetHash().GetHex();
    return true;
}

} // namespace

XchainOutcome RunXchainConversion(CWallet& wallet, const SeqobOffer& offer, CAmount slice)
{
    XchainOutcome out;

    if (!offer.offer_is_btc || offer.want_is_btc) {
        out.error = "that offer does not pay Bitcoin";
        return out;
    }
    const CAmount whole_asset = offer.want_amount;
    const CAmount whole_btc = offer.offer_amount;
    const CAmount take = RewardSliceForWholeHtlc(whole_asset, slice);
    const CAmount want_btc = ProportionalBtcFloor(whole_btc, take, whole_asset);
    if (take <= 0 || want_btc <= 0) {
        out.error = "that size does not fit this offer";
        return out;
    }

    // Will the Bitcoin be worth claiming? Claiming an HTLC costs a
    // parent-chain transaction, and if the leg is worth less than that fee the
    // swap ends with the asset committed and the proceeds unreachable --
    // recoverable only by waiting out the timelock. That check already exists
    // in ClaimBtcLeg, but by then the asset is gone, which is far too late for
    // it to be useful. So it is made here, before anything is spent.
    const CAmount claim_cost = ParentFeeratePerVb() * CLAIM_VSIZE;
    if (want_btc <= claim_cost * 2) {
        out.error = strprintf(
            "this would fetch %d satoshis, which does not cover the %d it costs to claim them. "
            "These rewards wait until there are more of them.", want_btc, claim_cost);
        return out;
    }

    XchainSwap s;
    s.state = "negotiating";
    s.time = GetTime();
    s.offer_id = offer.offer_id;
    s.maker_pubkey = offer.maker_pubkey;
    s.asset = offer.want_asset;
    s.seq_amount = take;
    s.btc_amount = want_btc;
    NewSwapKey(s.taker_seq_refund_priv, s.taker_seq_refund_pub);
    NewSwapKey(s.taker_btc_claim_priv, s.taker_btc_claim_pub);

    std::string error;
    auto courier = SeqobCourier::Open(offer, take, "", std::chrono::seconds(SESSION_TIMEOUT_S), error);
    if (!courier) { out.error = error; return out; }

    UniValue req(UniValue::VOBJ);
    req.pushKV("type", "terms_request");
    req.pushKV("taker_seq_refund_pub", Hex(s.taker_seq_refund_pub));
    req.pushKV("taker_btc_claim_pub", Hex(s.taker_btc_claim_pub));
    req.pushKV("seq_amount", (int64_t)take);
    if (!courier->Send(req, error)) { out.error = error; return out; }

    UniValue locked;
    if (!courier->Recv(locked, std::chrono::seconds(TERMS_TIMEOUT_S), error)) {
        out.error = "no maker responded: " + error;
        return out;
    }
    const UniValue& type = F(locked, "type", "type");
    if (type.isStr() && type.get_str() == "fail") {
        out.error = "the maker declined: " + F(locked, "message", "message").getValStr();
        return out;
    }

    const UniValue& leg = F(locked, "leg", "leg");
    s.hash_h = UnHex(F(locked, "hash_h", "hashH"));
    s.maker_seq_claim_pub = UnHex(F(locked, "maker_seq_claim_pub", "makerSeqClaimPub"));
    s.maker_btc_refund_pub = UnHex(F(locked, "maker_refund_pub", "makerRefundPub"));
    s.seq_locktime = (uint32_t)NumField(F(locked, "seq_locktime", "seqLocktime"));
    s.btc_locktime = (uint32_t)NumField(F(leg, "locktime", "locktime"));
    if (s.btc_locktime == 0) s.btc_locktime = (uint32_t)NumField(F(locked, "btc_locktime", "btcLocktime"));
    s.btc_leg_txid = F(leg, "txid", "txid").getValStr();
    s.btc_leg_vout = (uint32_t)NumField(F(leg, "vout", "vout"));
    s.btc_leg_script = UnHex(F(leg, "redeem_script", "redeemScript"));
    s.btc_leg_amount = NumField(F(leg, "amount", "amount"));

    // Bind the maker's terms to OUR slice before anything is spent. The maker
    // must have locked exactly the proportional Bitcoin, and sized the trade to
    // what we asked for -- in the terms AND in the coin it actually funded.
    const CAmount quoted_btc = NumField(F(locked, "btc_amount", "btcAmount"));
    const CAmount quoted_seq = NumField(F(locked, "seq_amount", "seqAmount"));
    if (s.hash_h.size() != 32 || s.maker_seq_claim_pub.size() != 33) {
        courier->SendFail("terms_mismatch", "incomplete terms");
        out.error = "the maker's terms were incomplete; nothing was spent";
        return out;
    }
    if (quoted_btc != want_btc || s.btc_leg_amount != want_btc) {
        courier->SendFail("terms_mismatch", "not the proportional Bitcoin for this slice");
        out.error = "the maker did not lock the proportional Bitcoin for this slice; nothing was spent";
        return out;
    }
    if (quoted_seq != take) {
        courier->SendFail("terms_mismatch", "sized differently from the requested slice");
        out.error = "the maker sized the trade differently; nothing was spent";
        return out;
    }

    std::string why;
    if (!VerifyMakerBtcLeg(s, why)) {
        courier->SendFail("btc_leg_invalid", why);
        out.error = why + "; nothing was spent";
        return out;
    }
    s.state = "btc_locked";
    SaveSwap(wallet, s);

    // Wait for the maker's lock to CONFIRM. Until it does there is nothing to
    // be safe about: an unconfirmed lock can be replaced.
    const int64_t give_up_at = GetTime() + BTC_CONF_MAX_WAIT_S;
    while (true) {
        const ParentOut po = ReadParentOutput(s.btc_leg_txid, s.btc_leg_vout);
        if (po.found && po.confirmations > 0 && po.height > 0) {
            s.btc_leg_height = po.height;
            break;
        }
        if (GetTime() >= give_up_at) {
            courier->SendFail("btc_leg_unconfirmed", "the Bitcoin lock never confirmed");
            out.error = "the maker's Bitcoin lock never confirmed; nothing was spent";
            return out;
        }
        UninterruptibleSleep(std::chrono::seconds(BTC_CONF_POLL_S));
    }
    SaveSwap(wallet, s);

    // THE GATE. Our funding's block commits to a Bitcoin anchor, and that
    // number freezes the moment it confirms -- so it has to be high enough
    // BEFORE we fund, and waiting afterwards can never repair it. Nothing of
    // ours has moved yet, so this wait is free; if we give up here the maker
    // simply refunds its own Bitcoin after its timelock.
    while (true) {
        const int anchor = TipAnchorHeight(wallet);
        if (anchor >= s.btc_leg_height) break;
        if (GetTime() >= give_up_at) {
            courier->SendFail("anchor_not_caught_up", "this node's Bitcoin anchor never reached the lock");
            out.error = "this node's Bitcoin anchor never reached the maker's lock; nothing was spent";
            return out;
        }
        UninterruptibleSleep(std::chrono::seconds(BTC_CONF_POLL_S));
    }

    // Look again, immediately before spending. A lock that was confirmed when
    // we checked can be gone by the end of a long wait: one parent-chain reorg
    // is all it takes, and funding against a dead lock is the one-sided loss
    // this whole sequence exists to prevent.
    if (!VerifyMakerBtcLeg(s, why)) {
        courier->SendFail("btc_leg_gone", why);
        out.error = "the maker's Bitcoin lock is no longer there; nothing was spent";
        return out;
    }

    const auto redeem = BuildHtlcRedeemScript(s.hash_h, s.maker_seq_claim_pub,
                                              s.taker_seq_refund_pub, s.seq_locktime);
    if (!redeem) {
        courier->SendFail("terms_mismatch", "the terms do not build an HTLC");
        out.error = "the maker's terms do not build a usable lock; nothing was spent";
        return out;
    }
    s.seq_redeem.assign(redeem->begin(), redeem->end());

    // From here our money is committed.
    std::string fund_error;
    if (!FundSeqHtlc(wallet, *redeem, s.asset, s.seq_amount, s.seq_fund_txid, s.seq_fund_vout, fund_error)) {
        courier->SendFail("seq_fund_failed", fund_error);
        out.error = fund_error;
        return out;
    }
    s.state = "seq_funded";
    SaveSwap(wallet, s);
    out.committed = true;

    UniValue funded(UniValue::VOBJ);
    funded.pushKV("type", "seq_leg_funded");
    UniValue fl(UniValue::VOBJ);
    fl.pushKV("txid", s.seq_fund_txid);
    fl.pushKV("vout", (int64_t)s.seq_fund_vout);
    fl.pushKV("amount", (int64_t)s.seq_amount);
    fl.pushKV("asset", s.asset.GetHex());
    fl.pushKV("redeem_script", Hex(s.seq_redeem));
    fl.pushKV("locktime", (int64_t)s.seq_locktime);
    funded.pushKV("leg", fl);
    std::string ignored;
    courier->Send(funded, ignored);
    courier->Close();

    LogPrintf("[rewards] cross-chain: funded %d of %s against the maker's %d satoshis (%s)\n",
              s.seq_amount, s.asset.GetHex(), s.btc_amount, s.offer_id);

    // The rest -- the maker claiming, the secret appearing, the Bitcoin being
    // taken -- happens on chain and is driven by the resume pass, so a node
    // that stops here still finishes.
    out.ok = true;
    out.received = s.btc_amount;
    return out;
}

void ResumeXchainSwaps(CWallet& wallet)
{
    auto swaps = LoadXchainSwaps(wallet);
    bool dirty = false;

    for (XchainSwap& s : swaps) {
        if (s.Terminal()) continue;

        if (s.state != "seq_funded") {
            // Nothing of ours was committed, so there is nothing to rescue --
            // but a swap left here by a node that stopped mid-negotiation would
            // otherwise sit unfinished for good, and an unfinished swap is
            // something a staker has to keep wondering about.
            //
            // It is NOT funded now instead. The maker's session ended when the
            // node did; by this point it has almost certainly taken its own
            // Bitcoin back, and funding into that would lock the staker's asset
            // until our own timelock for no possible gain.
            //
            // The wait is what makes this safe against the live pass, which
            // holds a swap in btc_locked while it waits for the maker's lock to
            // confirm. Past that window no pass can still be holding it, so
            // retiring it cannot pull the record out from under one.
            if (s.time > 0 && GetTime() - s.time > STRANDED_AFTER_S) {
                s.state = "failed";
                s.error = "this swap was interrupted before anything of yours was committed, and "
                          "too long ago to still be in progress; nothing was spent";
                dirty = true;
                LogPrintf("[rewards] cross-chain: retiring %s, interrupted at '%s' and long stale\n",
                          s.offer_id, s.state);
            }
            continue;
        }

        // 1. Has the maker taken the asset? Then the secret is on chain, and
        //    the Bitcoin is ours to take.
        if (s.preimage.size() != 32) {
            std::vector<unsigned char> found;
            if (FindPreimageOnChain(wallet, s, found)) {
                s.preimage = found;
                dirty = true;
                LogPrintf("[rewards] cross-chain: the maker revealed the secret for %s\n", s.offer_id);
            }
        }

        if (s.preimage.size() == 32 && s.btc_claim_txid.empty()) {
            std::string error;
            if (ClaimBtcLeg(wallet, s, error)) {
                s.state = "btc_claimed";
                LogPrintf("[rewards] cross-chain: claimed %d satoshis (%s)\n", s.btc_leg_amount, s.btc_claim_txid);
            } else {
                s.error = error;
                LogPrintf("[rewards] cross-chain: could not claim yet: %s\n", error);
            }
            dirty = true;
            continue;
        }

        // 2. The maker never claimed and the timelock has passed: take the
        //    asset back. The refund is a timelock, not a favour -- it needs
        //    nobody's cooperation.
        if (s.preimage.empty() && (uint32_t)wallet.GetLastBlockHeight() >= s.seq_locktime) {
            std::string error;
            if (RefundSeqLeg(wallet, s, error)) {
                s.state = "refunded";
                LogPrintf("[rewards] cross-chain: reclaimed the asset leg for %s\n", s.offer_id);
            } else {
                s.error = error;
            }
            dirty = true;
        }
    }

    if (dirty) StoreSwaps(wallet, swaps);
}

} // namespace wallet
