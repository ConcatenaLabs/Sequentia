// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/blockchain.h>

#include <assetsdir.h>
#include <blockfilter.h>
#include <chain.h>
#include <anchor.h>
#include <chainparams.h>
#include <key_io.h>
#include <pos.h>
#include <pos_producer.h>
#include <vrf.h>
#include <util/settings.h>
#include <set>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>
#include <fs.h>
#include <hash.h>
#include <index/blockfilterindex.h>
#include <index/coinstatsindex.h>
#include <logging/timer.h>
#include <net.h>
#include <net_processing.h>
#include <node/blockstorage.h>
#include <node/coinstats.h>
#include <node/context.h>
#include <node/utxo_snapshot.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <streams.h>
#include <sync.h>
#include <txdb.h>
#include <txmempool.h>
#include <undo.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbits.h>
#include <warnings.h>
#include <pegins.h>
#include <dynafed.h>

#include <stdint.h>

#include <univalue.h>

#include <condition_variable>
#include <memory>
#include <mutex>

using node::BlockManager;
using node::CCoinsStats;
using node::CoinStatsHashType;
using node::GetUTXOStats;
using node::IsBlockPruned;
using node::NodeContext;
using node::ReadBlockFromDisk;
using node::SnapshotMetadata;
using node::UndoReadFromDisk;

struct CUpdatedBlock
{
    uint256 hash;
    int height;
};

static Mutex cs_blockchange;
static std::condition_variable cond_blockchange;
static CUpdatedBlock latestblock GUARDED_BY(cs_blockchange);

/* Calculate the difficulty for a given block index.
 */
double GetDifficulty(const CBlockIndex* blockindex)
{
    CHECK_NONFATAL(blockindex);

    int nShift = (blockindex->nBits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

UniValue paramEntryToJSON(const DynaFedParamEntry& entry)
{
    UniValue result(UniValue::VOBJ);

    // set the type
    if (entry.m_serialize_type == 0) {
        result.pushKV("type", "null");
    } else if (entry.m_serialize_type == 1) {
        result.pushKV("type", "compact");
    } else if (entry.m_serialize_type == 2) {
        result.pushKV("type", "full");
    }

    // nothing more to do for null
    if (entry.m_serialize_type == 0) {
        return result;
    }

    // fields all params have
    result.pushKV("root", entry.CalculateRoot().GetHex());
    result.pushKV("signblockscript", HexStr(entry.m_signblockscript));
    result.pushKV("max_block_witness", (uint64_t)entry.m_signblock_witness_limit);

    // add the extra root which is stored for compact and calculated for full
    if (entry.m_serialize_type == 1) {
        // compact
        result.pushKV("extra_root", entry.m_elided_root.GetHex());
    } else if (entry.m_serialize_type == 2) {
        // full
        result.pushKV("extra_root", entry.CalculateExtraRoot().GetHex());
    }

    // some extra fields only present on full params
    if (entry.m_serialize_type == 2) {
        result.pushKV("fedpeg_program", HexStr(entry.m_fedpeg_program));
        result.pushKV("fedpegscript", HexStr(entry.m_fedpegscript));
        UniValue result_extension(UniValue::VARR);
        for (auto& item : entry.m_extension_space) {
            result_extension.push_back(HexStr(item));
        }
        result.pushKV("extension_space", result_extension);
    }

    return result;
}

UniValue dynaParamsToJSON(const DynaFedParams& dynafed_params)
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("current", paramEntryToJSON(dynafed_params.m_current));
    ret.pushKV("proposed", paramEntryToJSON(dynafed_params.m_proposed));
    return ret;
}

static int ComputeNextBlockAndDepth(const CBlockIndex* tip, const CBlockIndex* blockindex, const CBlockIndex*& next)
{
    next = tip->GetAncestor(blockindex->nHeight + 1);
    if (next && next->pprev == blockindex) {
        return tip->nHeight - blockindex->nHeight + 1;
    }
    next = nullptr;
    return blockindex == tip ? 1 : -1;
}

CBlockIndex* ParseHashOrHeight(const UniValue& param, ChainstateManager& chainman) {
    LOCK(::cs_main);
    CChain& active_chain = chainman.ActiveChain();

    if (param.isNum()) {
        const int height{param.get_int()};
        if (height < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Target block height %d is negative", height));
        }
        const int current_tip{active_chain.Height()};
        if (height > current_tip) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Target block height %d after current tip %d", height, current_tip));
        }

        return active_chain[height];
    } else {
        const uint256 hash{ParseHashV(param, "hash_or_height")};
        CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);

        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        return pindex;
    }
}

UniValue blockheaderToJSON(const CBlockIndex* tip, const CBlockIndex* blockindex_)
{
    // Serialize passed information without accessing chain state of the active chain!
    AssertLockNotHeld(cs_main); // For performance reasons

    CBlockIndex tmpBlockIndexFull;
    const CBlockIndex* blockindex;
    {
        LOCK(cs_main);
        blockindex = blockindex_->untrim_to(&tmpBlockIndexFull);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", blockindex->GetBlockHash().GetHex());
    const CBlockIndex* pnext;
    int confirmations = ComputeNextBlockAndDepth(tip, blockindex_, pnext);
    result.pushKV("confirmations", confirmations);
    result.pushKV("height", blockindex->nHeight);
    result.pushKV("version", blockindex->nVersion);
    result.pushKV("versionHex", strprintf("%08x", blockindex->nVersion));
    result.pushKV("merkleroot", blockindex->hashMerkleRoot.GetHex());
    result.pushKV("time", (int64_t)blockindex->nTime);
    result.pushKV("mediantime", (int64_t)blockindex->GetMedianTimePast());
    // SEQUENTIA: parent chain (Bitcoin) anchor
    if (g_con_bitcoin_anchor) {
        result.pushKV("anchorheight", (int64_t)blockindex->m_anchor_height);
        result.pushKV("anchorhash", blockindex->m_anchor_hash.GetHex());
    }
    // SEQUENTIA: PoS committee certification of THIS block. Cross-chain swap
    // safety wants a leg's block to be quorum-certified (immediately final), not
    // merely anchored: anchoring binds the leg to Bitcoin, quorum certification
    // means the committee finalized it. Exposed so an off-node swap driver
    // (VerifySeqLegSafe) and the wallet cross-maker can require both.
    if (g_con_pos) {
        const int pos_quorum = PosSlotQuorum(StakeRegistry::GetInstance());
        const int pos_countersigs = (int)blockindex->m_pos_countersigs;
        const bool pos_certified = pos_countersigs >= pos_quorum;
        result.pushKV("poscountersigs", pos_countersigs);
        result.pushKV("posquorum", pos_quorum);
        result.pushKV("poscertified", pos_certified);
    }
    if (!g_signed_blocks) {
        result.pushKV("nonce", (uint64_t)blockindex->nNonce);
        result.pushKV("bits", strprintf("%08x", blockindex->nBits));
        result.pushKV("difficulty", GetDifficulty(blockindex));
        result.pushKV("chainwork", blockindex->nChainWork.GetHex());
    } else {
        if (!blockindex->is_dynafed_block()) {
            if (blockindex->trimmed()) {
                result.pushKV("signblock_witness_asm", "<trimmed>");
                result.pushKV("signblock_witness_hex", "<trimmed>");
                result.pushKV("signblock_challenge", "<trimmed>");
                result.pushKV("warning", "Fields missing due to -trim_headers flag.");
            } else {
                result.pushKV("signblock_witness_asm", ScriptToAsmStr(blockindex->get_proof().solution));
                result.pushKV("signblock_witness_hex", HexStr(blockindex->get_proof().solution));
                result.pushKV("signblock_challenge", HexStr(blockindex->get_proof().challenge));
            }
        } else {
            if (blockindex->trimmed()) {
                result.pushKV("signblock_witness_hex", "<trimmed>");
                result.pushKV("dynamic_parameters", "<trimmed>");
                result.pushKV("warning", "Fields missing due to -trim_headers flag.");
            } else {
                result.pushKV("signblock_witness_hex", EncodeHexScriptWitness(blockindex->signblock_witness()));
                result.pushKV("dynamic_parameters", dynaParamsToJSON(blockindex->dynafed_params()));
            }
        }
    }
    result.pushKV("nTx", (uint64_t)blockindex->nTx);
    if (blockindex_->pprev)
        result.pushKV("previousblockhash", blockindex->pprev->GetBlockHash().GetHex());
    if (pnext)
        result.pushKV("nextblockhash", pnext->GetBlockHash().GetHex());
    return result;
}

UniValue blockToJSON(const CBlock& block, const CBlockIndex* tip, const CBlockIndex* blockindex, TxVerbosity verbosity)
{
    UniValue result = blockheaderToJSON(tip, blockindex);

    result.pushKV("strippedsize", (int)::GetSerializeSize(block, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS));
    result.pushKV("size", (int)::GetSerializeSize(block, PROTOCOL_VERSION));
    result.pushKV("weight", (int)::GetBlockWeight(block));
    UniValue txs(UniValue::VARR);

    switch (verbosity) {
        case TxVerbosity::SHOW_TXID:
            for (const CTransactionRef& tx : block.vtx) {
                txs.push_back(tx->GetHash().GetHex());
            }
            break;

        case TxVerbosity::SHOW_DETAILS:
        case TxVerbosity::SHOW_DETAILS_AND_PREVOUT:
            CBlockUndo blockUndo;
            const bool have_undo{WITH_LOCK(::cs_main, return !IsBlockPruned(blockindex) && UndoReadFromDisk(blockUndo, blockindex))};

            for (size_t i = 0; i < block.vtx.size(); ++i) {
                const CTransactionRef& tx = block.vtx.at(i);
                // coinbase transaction (i.e. i == 0) doesn't have undo data
                const CTxUndo* txundo = (have_undo && i > 0) ? &blockUndo.vtxundo.at(i - 1) : nullptr;
                UniValue objTx(UniValue::VOBJ);
                TxToUniv(*tx, uint256(), objTx, true, RPCSerializationFlags(), txundo, verbosity);
                txs.push_back(std::move(objTx));
            }
            break;
    }

    result.pushKV("tx", std::move(txs));

    return result;
}

static RPCHelpMan getblockcount()
{
    return RPCHelpMan{"getblockcount",
                "\nReturns the height of the most-work fully-validated chain.\n"
                "The genesis block has height 0.\n",
                {},
                RPCResult{
                    RPCResult::Type::NUM, "", "The current block count"},
                RPCExamples{
                    HelpExampleCli("getblockcount", "")
            + HelpExampleRpc("getblockcount", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return chainman.ActiveChain().Height();
},
    };
}

static RPCHelpMan getbestblockhash()
{
    return RPCHelpMan{"getbestblockhash",
                "\nReturns the hash of the best (tip) block in the most-work fully-validated chain.\n",
                {},
                RPCResult{
                    RPCResult::Type::STR_HEX, "", "the block hash, hex-encoded"},
                RPCExamples{
                    HelpExampleCli("getbestblockhash", "")
            + HelpExampleRpc("getbestblockhash", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return chainman.ActiveChain().Tip()->GetBlockHash().GetHex();
},
    };
}

void RPCNotifyBlockChange(const CBlockIndex* pindex)
{
    if(pindex) {
        LOCK(cs_blockchange);
        latestblock.hash = pindex->GetBlockHash();
        latestblock.height = pindex->nHeight;
    }
    cond_blockchange.notify_all();
}

static RPCHelpMan waitfornewblock()
{
    return RPCHelpMan{"waitfornewblock",
                "\nWaits for a specific new block and returns useful info about it.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitfornewblock", "1000")
            + HelpExampleRpc("waitfornewblock", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;
    if (!request.params[0].isNull())
        timeout = request.params[0].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        block = latestblock;
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&block]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        else
            cond_blockchange.wait(lock, [&block]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan waitforblock()
{
    return RPCHelpMan{"waitforblock",
                "\nWaits for a specific new block and returns useful info about it.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Block hash to wait for."},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\" 1000")
            + HelpExampleRpc("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\", 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;

    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    if (!request.params[1].isNull())
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&hash]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.hash == hash || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&hash]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.hash == hash || !IsRPCRunning(); });
        block = latestblock;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan waitforblockheight()
{
    return RPCHelpMan{"waitforblockheight",
                "\nWaits for (at least) block height and returns the height and hash\n"
                "of the current tip.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "Block height to wait for."},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitforblockheight", "100 1000")
            + HelpExampleRpc("waitforblockheight", "100, 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;

    int height = request.params[0].get_int();

    if (!request.params[1].isNull())
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&height]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height >= height || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&height]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height >= height || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan syncwithvalidationinterfacequeue()
{
    return RPCHelpMan{"syncwithvalidationinterfacequeue",
                "\nWaits for the validation interface queue to catch up on everything that was there when we entered this function.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("syncwithvalidationinterfacequeue","")
            + HelpExampleRpc("syncwithvalidationinterfacequeue","")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    SyncWithValidationInterfaceQueue();
    return NullUniValue;
},
    };
}

static RPCHelpMan getdifficulty()
{
    return RPCHelpMan{"getdifficulty",
                "\nReturns the proof-of-work difficulty as a multiple of the minimum difficulty.\n",
                {},
                RPCResult{
                    RPCResult::Type::NUM, "", "the proof-of-work difficulty as a multiple of the minimum difficulty."},
                RPCExamples{
                    HelpExampleCli("getdifficulty", "")
            + HelpExampleRpc("getdifficulty", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return GetDifficulty(chainman.ActiveChain().Tip());
},
    };
}

static std::vector<RPCResult> MempoolEntryDescription() { return {
    RPCResult{RPCResult::Type::NUM, "vsize", "virtual transaction size as defined in BIP 141. This is different from actual serialized size for witness transactions as witness data is discounted."},
    RPCResult{RPCResult::Type::NUM, "weight", "transaction weight as defined in BIP 141."},
    RPCResult{RPCResult::Type::STR_AMOUNT, "fee", /*optional=*/true,
              "transaction fee, denominated in " + CURRENCY_UNIT + " (DEPRECATED, returned only if config option -deprecatedrpc=fees is passed)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "modifiedfee", /*optional=*/true,
              "transaction fee with fee deltas used for mining priority, denominated in " + CURRENCY_UNIT +
                  " (DEPRECATED, returned only if config option -deprecatedrpc=fees is passed)"},
    RPCResult{RPCResult::Type::NUM_TIME, "time", "local time transaction entered pool in seconds since 1 Jan 1970 GMT"},
    RPCResult{RPCResult::Type::NUM, "height", "block height when transaction entered pool"},
    RPCResult{RPCResult::Type::NUM, "descendantcount", "number of in-mempool descendant transactions (including this one)"},
    RPCResult{RPCResult::Type::NUM, "descendantsize", "virtual transaction size of in-mempool descendants (including this one)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "descendantfees", /*optional=*/true,
              "transaction fees of in-mempool descendants (including this one) with fee deltas used for mining priority, denominated in " +
                  CURRENCY_ATOM + "s (DEPRECATED, returned only if config option -deprecatedrpc=fees is passed)"},
    RPCResult{RPCResult::Type::NUM, "ancestorcount", "number of in-mempool ancestor transactions (including this one)"},
    RPCResult{RPCResult::Type::NUM, "ancestorsize", "virtual transaction size of in-mempool ancestors (including this one)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "ancestorfees", /*optional=*/true,
              "transaction fees of in-mempool ancestors (including this one) with fee deltas used for mining priority, denominated in " +
                  CURRENCY_ATOM + "s (DEPRECATED, returned only if config option -deprecatedrpc=fees is passed)"},
    RPCResult{RPCResult::Type::STR_HEX, "wtxid", "hash of serialized transaction, including witness data"},
    RPCResult{RPCResult::Type::OBJ, "fees", "",
        {
            RPCResult{RPCResult::Type::STR_AMOUNT, "base", "transaction fee, denominated in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "modified", "transaction fee with fee deltas used for mining priority, denominated in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "ancestor", "transaction fees of in-mempool ancestors (including this one) with fee deltas used for mining priority, denominated in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "descendant", "transaction fees of in-mempool descendants (including this one) with fee deltas used for mining priority, denominated in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_HEX, "asset", /*optional=*/true, "asset used to pay transaction fee"},
            RPCResult{RPCResult::Type::STR_AMOUNT, "value", /*optional=*/true, "value of transaction fee according to current exchange rates, denominated in reference fee unit"},
        }},
    RPCResult{RPCResult::Type::ARR, "depends", "unconfirmed transactions used as inputs for this transaction",
        {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "parent transaction id"}}},
    RPCResult{RPCResult::Type::ARR, "spentby", "unconfirmed transactions spending outputs from this transaction",
        {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "child transaction id"}}},
    RPCResult{RPCResult::Type::BOOL, "bip125-replaceable", "Whether this transaction could be replaced due to BIP125 (replace-by-fee)"},
    RPCResult{RPCResult::Type::BOOL, "unbroadcast", "Whether this transaction is currently unbroadcast (initial broadcast not yet acknowledged by any peers)"},
};}

static void entryToJSON(const CTxMemPool& pool, UniValue& info, const CTxMemPoolEntry& e) EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    AssertLockHeld(pool.cs);

    info.pushKV("vsize", (int)e.GetTxSize());
    info.pushKV("weight", (int)e.GetTxWeight());
    // TODO: top-level fee fields are deprecated. deprecated_fee_fields_enabled blocks should be removed in v24
    const bool deprecated_fee_fields_enabled{IsDeprecatedRPCEnabled("fees")};
    if (deprecated_fee_fields_enabled) {
        info.pushKV("fee", ValueFromAmount(e.GetFee()));
        info.pushKV("modifiedfee", ValueFromAmount(e.GetModifiedFee()));
    }
    info.pushKV("time", count_seconds(e.GetTime()));
    info.pushKV("height", (int)e.GetHeight());
    info.pushKV("descendantcount", e.GetCountWithDescendants());
    info.pushKV("descendantsize", e.GetSizeWithDescendants());
    if (deprecated_fee_fields_enabled) {
        info.pushKV("descendantfees", e.GetModFeesWithDescendants().GetValue());
    }
    info.pushKV("ancestorcount", e.GetCountWithAncestors());
    info.pushKV("ancestorsize", e.GetSizeWithAncestors());
    if (deprecated_fee_fields_enabled) {
        info.pushKV("ancestorfees", e.GetModFeesWithAncestors().GetValue());
    }
    info.pushKV("wtxid", pool.vTxHashes[e.vTxHashesIdx].first.ToString());

    UniValue fees(UniValue::VOBJ);
    fees.pushKV("base", ValueFromAmount(e.GetFee()));
    fees.pushKV("modified", ValueFromAmount(e.GetModifiedFee()));
    fees.pushKV("ancestor", ValueFromAmount(e.GetModFeesWithAncestors().GetValue()));
    fees.pushKV("descendant", ValueFromAmount(e.GetModFeesWithDescendants().GetValue()));
    if (g_con_any_asset_fees) {
        fees.pushKV("asset", e.GetFeeAsset().GetHex());
        fees.pushKV("value", ValueFromAmount(e.GetFeeValue().GetValue()));
    }
    info.pushKV("fees", fees);

    const CTransaction& tx = e.GetTx();
    std::set<std::string> setDepends;
    for (const CTxIn& txin : tx.vin)
    {
        if (pool.exists(GenTxid::Txid(txin.prevout.hash)))
            setDepends.insert(txin.prevout.hash.ToString());
    }

    UniValue depends(UniValue::VARR);
    for (const std::string& dep : setDepends)
    {
        depends.push_back(dep);
    }

    info.pushKV("depends", depends);

    UniValue spent(UniValue::VARR);
    const CTxMemPool::txiter& it = pool.mapTx.find(tx.GetHash());
    const CTxMemPoolEntry::Children& children = it->GetMemPoolChildrenConst();
    for (const CTxMemPoolEntry& child : children) {
        spent.push_back(child.GetTx().GetHash().ToString());
    }

    info.pushKV("spentby", spent);

    // Add opt-in RBF status
    bool rbfStatus = false;
    RBFTransactionState rbfState = IsRBFOptIn(tx, pool);
    if (rbfState == RBFTransactionState::UNKNOWN) {
        throw JSONRPCError(RPC_MISC_ERROR, "Transaction is not in mempool");
    } else if (rbfState == RBFTransactionState::REPLACEABLE_BIP125) {
        rbfStatus = true;
    }

    info.pushKV("bip125-replaceable", rbfStatus);
    info.pushKV("unbroadcast", pool.IsUnbroadcastTx(tx.GetHash()));
}

UniValue MempoolToJSON(const CTxMemPool& pool, bool verbose, bool include_mempool_sequence)
{
    if (verbose) {
        if (include_mempool_sequence) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Verbose results cannot contain mempool sequence values.");
        }
        LOCK(pool.cs);
        UniValue o(UniValue::VOBJ);
        for (const CTxMemPoolEntry& e : pool.mapTx) {
            const uint256& hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(pool, info, e);
            // Mempool has unique entries so there is no advantage in using
            // UniValue::pushKV, which checks if the key already exists in O(N).
            // UniValue::__pushKV is used instead which currently is O(1).
            o.__pushKV(hash.ToString(), info);
        }
        return o;
    } else {
        uint64_t mempool_sequence;
        std::vector<uint256> vtxid;
        {
            LOCK(pool.cs);
            pool.queryHashes(vtxid);
            mempool_sequence = pool.GetSequence();
        }
        UniValue a(UniValue::VARR);
        for (const uint256& hash : vtxid)
            a.push_back(hash.ToString());

        if (!include_mempool_sequence) {
            return a;
        } else {
            UniValue o(UniValue::VOBJ);
            o.pushKV("txids", a);
            o.pushKV("mempool_sequence", mempool_sequence);
            return o;
        }
    }
}

static RPCHelpMan getrawmempool()
{
    return RPCHelpMan{"getrawmempool",
                "\nReturns all transaction ids in memory pool as a json array of string transaction ids.\n"
                "\nHint: use getmempoolentry to fetch a specific transaction from the mempool.\n",
                {
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                    {"mempool_sequence", RPCArg::Type::BOOL, RPCArg::Default{false}, "If verbose=false, returns a json object with transaction list and mempool sequence number attached."},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "", "The transaction id"},
                        }},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                    RPCResult{"for verbose = false and mempool_sequence = true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ARR, "txids", "",
                            {
                                {RPCResult::Type::STR_HEX, "", "The transaction id"},
                            }},
                            {RPCResult::Type::NUM, "mempool_sequence", "The mempool sequence value."},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getrawmempool", "true")
            + HelpExampleRpc("getrawmempool", "true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[0].isNull())
        fVerbose = request.params[0].get_bool();

    bool include_mempool_sequence = false;
    if (!request.params[1].isNull()) {
        include_mempool_sequence = request.params[1].get_bool();
    }

    return MempoolToJSON(EnsureAnyMemPool(request.context), fVerbose, include_mempool_sequence);
},
    };
}

static RPCHelpMan getmempoolancestors()
{
    return RPCHelpMan{"getmempoolancestors",
                "\nIf txid is in the mempool, returns all in-mempool ancestors.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-mempool ancestor transaction"}}},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getmempoolancestors", "\"mytxid\"")
            + HelpExampleRpc("getmempoolancestors", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    LOCK(mempool.cs);

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    CTxMemPool::setEntries setAncestors;
    uint64_t noLimit = std::numeric_limits<uint64_t>::max();
    std::string dummy;
    mempool.CalculateMemPoolAncestors(*it, setAncestors, noLimit, noLimit, noLimit, noLimit, dummy, false);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxMemPool::txiter ancestorIt : setAncestors) {
            o.push_back(ancestorIt->GetTx().GetHash().ToString());
        }
        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxMemPool::txiter ancestorIt : setAncestors) {
            const CTxMemPoolEntry &e = *ancestorIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(mempool, info, e);
            o.pushKV(_hash.ToString(), info);
        }
        return o;
    }
},
    };
}

static RPCHelpMan getmempooldescendants()
{
    return RPCHelpMan{"getmempooldescendants",
                "\nIf txid is in the mempool, returns all in-mempool descendants.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-mempool descendant transaction"}}},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getmempooldescendants", "\"mytxid\"")
            + HelpExampleRpc("getmempooldescendants", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    LOCK(mempool.cs);

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    CTxMemPool::setEntries setDescendants;
    mempool.CalculateDescendants(it, setDescendants);
    // CTxMemPool::CalculateDescendants will include the given tx
    setDescendants.erase(it);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxMemPool::txiter descendantIt : setDescendants) {
            o.push_back(descendantIt->GetTx().GetHash().ToString());
        }

        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxMemPool::txiter descendantIt : setDescendants) {
            const CTxMemPoolEntry &e = *descendantIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(mempool, info, e);
            o.pushKV(_hash.ToString(), info);
        }
        return o;
    }
},
    };
}

static RPCHelpMan getmempoolentry()
{
    return RPCHelpMan{"getmempoolentry",
                "\nReturns mempool data for given transaction\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "", MempoolEntryDescription()},
                RPCExamples{
                    HelpExampleCli("getmempoolentry", "\"mytxid\"")
            + HelpExampleRpc("getmempoolentry", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    LOCK(mempool.cs);

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    const CTxMemPoolEntry &e = *it;
    UniValue info(UniValue::VOBJ);
    entryToJSON(mempool, info, e);
    return info;
},
    };
}

static RPCHelpMan getblockfrompeer()
{
    return RPCHelpMan{
        "getblockfrompeer",
        "Attempt to fetch block from a given peer.\n\n"
        "We must have the header for this block, e.g. using submitheader.\n"
        "Subsequent calls for the same block and a new peer will cause the response from the previous peer to be ignored.\n\n"
        "Returns an empty JSON object if the request was successfully scheduled.",
        {
            {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash to try to fetch"},
            {"peer_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "The peer to fetch it from (see getpeerinfo for peer IDs)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", /*optional=*/false, "", {}},
        RPCExamples{
            HelpExampleCli("getblockfrompeer", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" 0")
            + HelpExampleRpc("getblockfrompeer", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" 0")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    PeerManager& peerman = EnsurePeerman(node);

    const uint256& block_hash{ParseHashV(request.params[0], "blockhash")};
    const NodeId peer_id{request.params[1].get_int64()};

    const CBlockIndex* const index = WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(block_hash););

    if (!index) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block header missing");
    }

    const bool block_has_data = WITH_LOCK(::cs_main, return index->nStatus & BLOCK_HAVE_DATA);
    if (block_has_data) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block already downloaded");
    }

    if (const auto err{peerman.FetchBlock(peer_id, *index)}) {
        throw JSONRPCError(RPC_MISC_ERROR, err.value());
    }
    return UniValue::VOBJ;
},
    };
}

static RPCHelpMan getblockhash()
{
    return RPCHelpMan{"getblockhash",
                "\nReturns hash of block in best-block-chain at height provided.\n",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The height index"},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "", "The block hash"},
                RPCExamples{
                    HelpExampleCli("getblockhash", "1000")
            + HelpExampleRpc("getblockhash", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();

    int nHeight = request.params[0].get_int();
    if (nHeight < 0 || nHeight > active_chain.Height())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");

    CBlockIndex* pblockindex = active_chain[nHeight];
    return pblockindex->GetBlockHash().GetHex();
},
    };
}

static RPCHelpMan getblockheader()
{
    return RPCHelpMan{"getblockheader",
                "\nIf verbose is false, returns a string that is serialized, hex-encoded data for blockheader 'hash'.\n"
                "If verbose is true, returns an Object with information about blockheader <hash>.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{true}, "true for a json object, false for the hex-encoded data"},
                },
                {
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "hash", "the block hash (same as provided)"},
                            {RPCResult::Type::NUM, "confirmations", "The number of confirmations, or -1 if the block is not on the main chain"},
                            {RPCResult::Type::NUM, "height", "The block height or index"},
                            {RPCResult::Type::NUM, "version", "The block version"},
                            {RPCResult::Type::STR_HEX, "versionHex", "The block version formatted in hexadecimal"},
                            {RPCResult::Type::STR_HEX, "merkleroot", "The merkle root"},
                            {RPCResult::Type::NUM_TIME, "time", "The block time expressed in " + UNIX_EPOCH_TIME},
                            {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                            {RPCResult::Type::NUM, "nonce", "The nonce"},
                            {RPCResult::Type::STR_HEX, "bits", "The bits"},
                            {RPCResult::Type::NUM, "difficulty", "The difficulty"},
                            {RPCResult::Type::STR_HEX, "chainwork", "Expected number of hashes required to produce the current chain"},
                            {RPCResult::Type::NUM, "nTx", "The number of transactions in the block"},
                            {RPCResult::Type::STR, "signblock_witness_asm", "ASM of sign block witness data"},
                            {RPCResult::Type::STR_HEX, "signblock_witness_hex", "Hex of sign block witness data"},
                            {RPCResult::Type::OBJ, "dynamic_parameters", "Dynamic federation parameters in the block, if any",
                            {
                                {RPCResult::Type::OBJ, "current", "enforced dynamic federation parameters. The signblockscript is published for each block, while others are published only at epoch start",
                                {
                                    {RPCResult::Type::STR_HEX, "signblockscript", "signblock script"},
                                    {RPCResult::Type::NUM, "max_block_witness", "Maximum serialized size of the block witness stack"},
                                    {RPCResult::Type::STR_HEX, "fedpegscript", "fedpeg script"},
                                    {RPCResult::Type::ARR, "extension_space", "array of hex-encoded strings",
                                    {
                                        {RPCResult::Type::ELISION, "", ""}
                                    }}
                                }},
                                {RPCResult::Type::OBJ, "proposed", "Proposed parameters. Uninforced. Must be published in full",
                                {
                                    {RPCResult::Type::ELISION, "", "same entries as current"}
                                }},
                            }},
                            {RPCResult::Type::STR_HEX, "previousblockhash", /*optional=*/true, "The hash of the previous block (if available)"},
                            {RPCResult::Type::STR_HEX, "nextblockhash", /*optional=*/true, "The hash of the next block (if available)"},
                        }},
                    RPCResult{"for verbose=false",
                        RPCResult::Type::STR_HEX, "", "A string that is serialized, hex-encoded data for block 'hash'"},
                },
                RPCExamples{
                    HelpExampleCli("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "hash"));

    bool fVerbose = true;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    CBlockIndex* pblockindex;
    const CBlockIndex* tip;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        tip = chainman.ActiveChain().Tip();
    }

    if (!pblockindex) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    }

    if (!fVerbose)
    {
        LOCK(cs_main);
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        CBlockIndex tmpBlockIndexFull;
        const CBlockIndex* pblockindexfull=pblockindex->untrim_to(&tmpBlockIndexFull);
        ssBlock << pblockindexfull->GetBlockHeader();
        std::string strHex = HexStr(ssBlock);
        return strHex;
    }

    return blockheaderToJSON(tip, pblockindex);
},
    };
}

static CBlock GetBlockChecked(const CBlockIndex* pblockindex) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);
    CBlock block;
    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block not available (pruned data)");
    }

    if (!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus())) {
        // Block not found on disk. This could be because we have the block
        // header in our index but not yet have the block or did not accept the
        // block.
        throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
    }

    return block;
}

static CBlockUndo GetUndoChecked(const CBlockIndex* pblockindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    AssertLockHeld(::cs_main);
    CBlockUndo blockUndo;
    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Undo data not available (pruned data)");
    }

    if (!UndoReadFromDisk(blockUndo, pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Can't read undo data from disk");
    }

    return blockUndo;
}

static RPCHelpMan getblock()
{
    return RPCHelpMan{"getblock",
                "\nIf verbosity is 0, returns a string that is serialized, hex-encoded data for block 'hash'.\n"
                "If verbosity is 1, returns an Object with information about block <hash>.\n"
                "If verbosity is 2, returns an Object with information about block <hash> and information about each transaction.\n"
                "If verbosity is 3, returns an Object with information about block <hash> and information about each transaction, including prevout information for inputs (only for unpruned blocks in the current best chain).\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"verbosity|verbose", RPCArg::Type::NUM, RPCArg::Default{1}, "0 for hex-encoded data, 1 for a JSON object, 2 for JSON object with transaction data, and 3 for JSON object with transaction data including prevout information for inputs"},
                },
                {
                    RPCResult{"for verbosity = 0",
                RPCResult::Type::STR_HEX, "", "A string that is serialized, hex-encoded data for block 'hash'"},
                    RPCResult{"for verbosity = 1",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "hash", "the block hash (same as provided)"},
                    {RPCResult::Type::NUM, "confirmations", "The number of confirmations, or -1 if the block is not on the main chain"},
                    {RPCResult::Type::NUM, "size", "The block size"},
                    {RPCResult::Type::NUM, "strippedsize", "The block size excluding witness data"},
                    {RPCResult::Type::NUM, "weight", "The block weight as defined in BIP 141"},
                    {RPCResult::Type::NUM, "height", "The block height or index"},
                    {RPCResult::Type::NUM, "version", "The block version"},
                    {RPCResult::Type::STR_HEX, "versionHex", "The block version formatted in hexadecimal"},
                    {RPCResult::Type::STR_HEX, "merkleroot", "The merkle root"},
                    {RPCResult::Type::ARR, "tx", "The transaction ids",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id"}}},
                    {RPCResult::Type::NUM_TIME, "time",       "The block time expressed in " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::NUM, "nonce", "The nonce"},
                    {RPCResult::Type::STR_HEX, "bits", "The bits"},
                    {RPCResult::Type::NUM, "difficulty", "The difficulty"},
                    {RPCResult::Type::STR_HEX, "chainwork", "Expected number of hashes required to produce the chain up to this block (in hex)"},
                    {RPCResult::Type::NUM, "nTx", "The number of transactions in the block"},
                    {RPCResult::Type::STR, "signblock_witness_asm", "ASM of sign block witness data"},
                    {RPCResult::Type::STR_HEX, "signblock_witness_hex", "Hex of sign block witness data"},
                    {RPCResult::Type::OBJ, "dynamic_parameters", "Dynamic federation parameters in the block, if any",
                    {
                        {RPCResult::Type::OBJ, "current", "enforced dynamic federation parameters. The signblockscript is published for each block, while others are published only at epoch start",
                        {
                            {RPCResult::Type::STR_HEX, "signblockscript", "signblock script"},
                            {RPCResult::Type::NUM, "max_block_witness", "Maximum serialized size of the block witness stack"},
                            {RPCResult::Type::STR_HEX, "fedpegscript", "fedpeg script"},
                            {RPCResult::Type::ARR, "extension_space", "array of hex-encoded strings",
                            {
                                {RPCResult::Type::ELISION, "", ""}
                            }},
                        }},
                        {RPCResult::Type::OBJ, "proposed", "Proposed parameters. Uninforced. Must be published in full",
                        {
                            {RPCResult::Type::ELISION, "", "same entries as current"}
                        }},
                    }},
                    {RPCResult::Type::STR_HEX, "previousblockhash", /*optional=*/true, "The hash of the previous block (if available)"},
                    {RPCResult::Type::STR_HEX, "nextblockhash", /*optional=*/true, "The hash of the next block (if available)"},
                }},
                    RPCResult{"for verbosity = 2",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ELISION, "", "Same output as verbosity = 1"},
                    {RPCResult::Type::ARR, "tx", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ELISION, "", "The transactions in the format of the getrawtransaction RPC. Different from verbosity = 1 \"tx\" result"},
                            {RPCResult::Type::NUM, "fee", "The transaction fee in " + CURRENCY_UNIT + ", omitted if block undo data is not available"},
                        }},
                    }},
                }},
                    RPCResult{"for verbosity = 3",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ELISION, "", "Same output as verbosity = 2"},
                    {RPCResult::Type::ARR, "tx", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ARR, "vin", "",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                {
                                    {RPCResult::Type::ELISION, "", "The same output as verbosity = 2"},
                                    {RPCResult::Type::OBJ, "prevout", "(Only if undo information is available)",
                                    {
                                        {RPCResult::Type::BOOL, "generated", "Coinbase or not"},
                                        {RPCResult::Type::NUM, "height", "The height of the prevout"},
                                        {RPCResult::Type::NUM, "value", "The value in " + CURRENCY_UNIT},
                                        {RPCResult::Type::OBJ, "scriptPubKey", "",
                                        {
                                            {RPCResult::Type::STR, "asm", "The asm"},
                                            {RPCResult::Type::STR, "hex", "The hex"},
                                            {RPCResult::Type::STR, "address", /* optional */ true, "The Bitcoin address (only if a well-defined address exists)"},
                                            {RPCResult::Type::STR, "type", "The type (one of: " + GetAllOutputTypes() + ")"},
                                        }},
                                    }},
                                }},
                            }},
                        }},
                    }},
                }},
        },
                RPCExamples{
                    HelpExampleCli("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    int verbosity = 1;
    if (!request.params[1].isNull()) {
        if (request.params[1].isBool()) {
            verbosity = request.params[1].get_bool() ? 1 : 0;
        } else {
            verbosity = request.params[1].get_int();
        }
    }

    CBlock block;
    const CBlockIndex* pblockindex;
    const CBlockIndex* tip;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        tip = chainman.ActiveChain().Tip();

        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        block = GetBlockChecked(pblockindex);
    }

    if (verbosity <= 0)
    {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssBlock << block;
        std::string strHex = HexStr(ssBlock);
        return strHex;
    }

    TxVerbosity tx_verbosity;
    if (verbosity == 1) {
        tx_verbosity = TxVerbosity::SHOW_TXID;
    } else if (verbosity == 2) {
        tx_verbosity = TxVerbosity::SHOW_DETAILS;
    } else {
        tx_verbosity = TxVerbosity::SHOW_DETAILS_AND_PREVOUT;
    }

    return blockToJSON(block, tip, pblockindex, tx_verbosity);
},
    };
}

static RPCHelpMan pruneblockchain()
{
    return RPCHelpMan{"pruneblockchain", "",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The block height to prune up to. May be set to a discrete height, or to a " + UNIX_EPOCH_TIME + "\n"
            "                  to prune blocks whose block time is at least 2 hours older than the provided timestamp."},
                },
                RPCResult{
                    RPCResult::Type::NUM, "", "Height of the last block pruned"},
                RPCExamples{
                    HelpExampleCli("pruneblockchain", "1000")
            + HelpExampleRpc("pruneblockchain", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!node::fPruneMode)
        throw JSONRPCError(RPC_MISC_ERROR, "Cannot prune blocks because node is not in prune mode.");

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    int heightParam = request.params[0].get_int();
    if (heightParam < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative block height.");

    // Height value more than a billion is too high to be a block height, and
    // too low to be a block time (corresponds to timestamp from Sep 2001).
    if (heightParam > 1000000000) {
        // Add a 2 hour buffer to include blocks which might have had old timestamps
        CBlockIndex* pindex = active_chain.FindEarliestAtLeast(heightParam - TIMESTAMP_WINDOW, 0);
        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Could not find block with at least the specified timestamp.");
        }
        heightParam = pindex->nHeight;
    }

    unsigned int height = (unsigned int) heightParam;
    unsigned int chainHeight = (unsigned int) active_chain.Height();
    if (chainHeight < Params().PruneAfterHeight())
        throw JSONRPCError(RPC_MISC_ERROR, "Blockchain is too short for pruning.");
    else if (height > chainHeight)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Blockchain is shorter than the attempted prune height.");
    else if (height > chainHeight - MIN_BLOCKS_TO_KEEP) {
        LogPrint(BCLog::RPC, "Attempt to prune blocks close to the tip.  Retaining the minimum number of blocks.\n");
        height = chainHeight - MIN_BLOCKS_TO_KEEP;
    }

    PruneBlockFilesManual(active_chainstate, height);
    const CBlockIndex* block = active_chain.Tip();
    CHECK_NONFATAL(block);
    while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
        block = block->pprev;
    }
    return uint64_t(block->nHeight);
},
    };
}

CoinStatsHashType ParseHashType(const std::string& hash_type_input)
{
    if (hash_type_input == "hash_serialized_2") {
        return CoinStatsHashType::HASH_SERIALIZED;
    } else if (hash_type_input == "muhash") {
        return CoinStatsHashType::MUHASH;
    } else if (hash_type_input == "none") {
        return CoinStatsHashType::NONE;
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("'%s' is not a valid hash_type", hash_type_input));
    }
}

static RPCHelpMan gettxoutsetinfo()
{
    return RPCHelpMan{"gettxoutsetinfo",
                "\nReturns statistics about the unspent transaction output set.\n"
                "Note this call may take some time if you are not using coinstatsindex.\n",
                {
                    {"hash_type", RPCArg::Type::STR, RPCArg::Default{"hash_serialized_2"}, "Which UTXO set hash should be calculated. Options: 'hash_serialized_2' (the legacy algorithm), 'muhash', 'none'."},
                    {"hash_or_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The block hash or height of the target height (only available with coinstatsindex).", "", {"", "string or numeric"}},
                    {"use_index", RPCArg::Type::BOOL, RPCArg::Default{true}, "Use coinstatsindex, if available."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "height", "The block height (index) of the returned statistics"},
                        {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at which these statistics are calculated"},
                        {RPCResult::Type::NUM, "txouts", "The number of unspent transaction outputs"},
                        {RPCResult::Type::NUM, "bogosize", "Database-independent, meaningless metric indicating the UTXO set size"},
                        {RPCResult::Type::STR_HEX, "hash_serialized_2", /*optional=*/true, "The serialized hash (only present if 'hash_serialized_2' hash_type is chosen)"},
                        {RPCResult::Type::STR_HEX, "muhash", /*optional=*/true, "The serialized hash (only present if 'muhash' hash_type is chosen)"},
                        {RPCResult::Type::NUM, "transactions", /*optional=*/true, "The number of transactions with unspent outputs (not available when coinstatsindex is used)"},
                        {RPCResult::Type::NUM, "disk_size", /*optional=*/true, "The estimated size of the chainstate on disk (not available when coinstatsindex is used)"},
                        {RPCResult::Type::STR_AMOUNT, "total_amount", "The total amount of coins in the UTXO set"},
                        {RPCResult::Type::STR_AMOUNT, "total_unspendable_amount", /*optional=*/true, "The total amount of coins permanently excluded from the UTXO set (only available if coinstatsindex is used)"},
                        {RPCResult::Type::OBJ, "block_info", /*optional=*/true, "Info on amounts in the block at this block height (only available if coinstatsindex is used)",
                        {
                            {RPCResult::Type::STR_AMOUNT, "prevout_spent", "Total amount of all prevouts spent in this block"},
                            {RPCResult::Type::STR_AMOUNT, "coinbase", "Coinbase subsidy amount of this block"},
                            {RPCResult::Type::STR_AMOUNT, "new_outputs_ex_coinbase", "Total amount of new outputs created by this block"},
                            {RPCResult::Type::STR_AMOUNT, "unspendable", "Total amount of unspendable outputs created in this block"},
                            {RPCResult::Type::OBJ, "unspendables", "Detailed view of the unspendable categories",
                            {
                                {RPCResult::Type::STR_AMOUNT, "genesis_block", "The unspendable amount of the Genesis block subsidy"},
                                {RPCResult::Type::STR_AMOUNT, "bip30", "Transactions overridden by duplicates (no longer possible with BIP30)"},
                                {RPCResult::Type::STR_AMOUNT, "scripts", "Amounts sent to scripts that are unspendable (for example OP_RETURN outputs)"},
                                {RPCResult::Type::STR_AMOUNT, "unclaimed_rewards", "Fee rewards that miners did not claim in their coinbase transaction"},
                            }}
                        }},
                    }},
                RPCExamples{
                    HelpExampleCli("gettxoutsetinfo", "") +
                    HelpExampleCli("gettxoutsetinfo", R"("none")") +
                    HelpExampleCli("gettxoutsetinfo", R"("none" 1000)") +
                    HelpExampleCli("gettxoutsetinfo", R"("none" '"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"')") +
                    HelpExampleRpc("gettxoutsetinfo", "") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none")") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none", 1000)") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none", "00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09")")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue ret(UniValue::VOBJ);

    CBlockIndex* pindex{nullptr};
    const CoinStatsHashType hash_type{request.params[0].isNull() ? CoinStatsHashType::HASH_SERIALIZED : ParseHashType(request.params[0].get_str())};
    CCoinsStats stats{hash_type};
    stats.index_requested = request.params[2].isNull() || request.params[2].get_bool();

    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    active_chainstate.ForceFlushStateToDisk();

    CCoinsView* coins_view;
    BlockManager* blockman;
    {
        LOCK(::cs_main);
        coins_view = &active_chainstate.CoinsDB();
        blockman = &active_chainstate.m_blockman;
        pindex = blockman->LookupBlockIndex(coins_view->GetBestBlock());
    }

    if (!request.params[1].isNull()) {
        if (!g_coin_stats_index) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Querying specific block heights requires coinstatsindex");
        }

        if (stats.m_hash_type == CoinStatsHashType::HASH_SERIALIZED) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "hash_serialized_2 hash type cannot be queried for a specific block");
        }

        pindex = ParseHashOrHeight(request.params[1], chainman);
    }

    if (stats.index_requested && g_coin_stats_index) {
        if (!g_coin_stats_index->BlockUntilSyncedToCurrentChain()) {
            const IndexSummary summary{g_coin_stats_index->GetSummary()};

            // If a specific block was requested and the index has already synced past that height, we can return the
            // data already even though the index is not fully synced yet.
            if (pindex->nHeight > summary.best_block_height) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Unable to get data because coinstatsindex is still syncing. Current height: %d", summary.best_block_height));
            }
        }
    }

    if (GetUTXOStats(coins_view, *blockman, stats, node.rpc_interruption_point, pindex)) {
        ret.pushKV("height", (int64_t)stats.nHeight);
        ret.pushKV("bestblock", stats.hashBlock.GetHex());
        ret.pushKV("txouts", (int64_t)stats.nTransactionOutputs);
        ret.pushKV("bogosize", (int64_t)stats.nBogoSize);
        if (hash_type == CoinStatsHashType::HASH_SERIALIZED) {
            ret.pushKV("hash_serialized_2", stats.hashSerialized.GetHex());
        }
        if (hash_type == CoinStatsHashType::MUHASH) {
            ret.pushKV("muhash", stats.hashSerialized.GetHex());
        }
        CHECK_NONFATAL(stats.total_amount.has_value());
        ret.pushKV("total_amount", ValueFromAmount(stats.total_amount.value()));
        if (!stats.index_used) {
            ret.pushKV("transactions", static_cast<int64_t>(stats.nTransactions));
            ret.pushKV("disk_size", stats.nDiskSize);
        } else {
            ret.pushKV("total_unspendable_amount", ValueFromAmount(stats.total_unspendable_amount));

            CCoinsStats prev_stats{hash_type};

            if (pindex->nHeight > 0) {
                GetUTXOStats(coins_view, *blockman, prev_stats, node.rpc_interruption_point, pindex->pprev);
            }

            UniValue block_info(UniValue::VOBJ);
            block_info.pushKV("prevout_spent", ValueFromAmount(stats.total_prevout_spent_amount - prev_stats.total_prevout_spent_amount));
            block_info.pushKV("coinbase", ValueFromAmount(stats.total_coinbase_amount - prev_stats.total_coinbase_amount));
            block_info.pushKV("new_outputs_ex_coinbase", ValueFromAmount(stats.total_new_outputs_ex_coinbase_amount - prev_stats.total_new_outputs_ex_coinbase_amount));
            block_info.pushKV("unspendable", ValueFromAmount(stats.total_unspendable_amount - prev_stats.total_unspendable_amount));

            UniValue unspendables(UniValue::VOBJ);
            unspendables.pushKV("genesis_block", ValueFromAmount(stats.total_unspendables_genesis_block - prev_stats.total_unspendables_genesis_block));
            unspendables.pushKV("bip30", ValueFromAmount(stats.total_unspendables_bip30 - prev_stats.total_unspendables_bip30));
            unspendables.pushKV("scripts", ValueFromAmount(stats.total_unspendables_scripts - prev_stats.total_unspendables_scripts));
            unspendables.pushKV("unclaimed_rewards", ValueFromAmount(stats.total_unspendables_unclaimed_rewards - prev_stats.total_unspendables_unclaimed_rewards));
            block_info.pushKV("unspendables", unspendables);

            ret.pushKV("block_info", block_info);
        }
    } else {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read UTXO set");
    }
    return ret;
},
    };
}

static RPCHelpMan gettxout()
{
    return RPCHelpMan{"gettxout",
        "\nReturns details about an unspent transaction output.\n",
        {
            {"txid", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction id"},
            {"n", RPCArg::Type::NUM, RPCArg::Optional::NO, "vout number"},
            {"include_mempool", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to include the mempool. Note that an unspent output that is spent in the mempool won't appear."},
        },
        {
            RPCResult{"If the UTXO was not found", RPCResult::Type::NONE, "", ""},
            RPCResult{"Otherwise", RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at the tip of the chain"},
                {RPCResult::Type::NUM, "confirmations", "The number of confirmations"},
                {RPCResult::Type::STR_AMOUNT, "value", "The transaction value in " + CURRENCY_UNIT},
                {RPCResult::Type::OBJ, "scriptPubKey", "", {
                    {RPCResult::Type::STR, "asm", ""},
                    {RPCResult::Type::STR, "desc", "Inferred descriptor for the output"},
                    {RPCResult::Type::STR_HEX, "hex", ""},
                    {RPCResult::Type::STR, "type", "The type, eg pubkeyhash"},
                    {RPCResult::Type::STR, "address", /*optional=*/true, "The Bitcoin address (only if a well-defined address exists)"},
                }},
                {RPCResult::Type::BOOL, "coinbase", "Coinbase or not"},
            }},
        },
        RPCExamples{
            "\nGet unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nView the details\n"
            + HelpExampleCli("gettxout", "\"txid\" 1") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("gettxout", "\"txid\", 1")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    LOCK(cs_main);

    UniValue ret(UniValue::VOBJ);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    int n = request.params[1].get_int();
    COutPoint out(hash, n);
    bool fMempool = true;
    if (!request.params[2].isNull())
        fMempool = request.params[2].get_bool();

    Coin coin;
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CCoinsViewCache* coins_view = &active_chainstate.CoinsTip();

    if (fMempool) {
        const CTxMemPool& mempool = EnsureMemPool(node);
        LOCK(mempool.cs);
        CCoinsViewMemPool view(coins_view, mempool);
        if (!view.GetCoin(out, coin) || mempool.isSpent(out)) {
            return NullUniValue;
        }
    } else {
        if (!coins_view->GetCoin(out, coin)) {
            return NullUniValue;
        }
    }

    const CBlockIndex* pindex = active_chainstate.m_blockman.LookupBlockIndex(coins_view->GetBestBlock());
    ret.pushKV("bestblock", pindex->GetBlockHash().GetHex());
    if (coin.nHeight == MEMPOOL_HEIGHT) {
        ret.pushKV("confirmations", 0);
    } else {
        ret.pushKV("confirmations", (int64_t)(pindex->nHeight - coin.nHeight + 1));
    }
    if (coin.out.nValue.IsExplicit()) {
        ret.pushKV("value", ValueFromAmount(coin.out.nValue.GetAmount()));
    } else {
        ret.pushKV("valuecommitment", coin.out.nValue.GetHex());
    }
    if (g_con_elementsmode) {
        if (coin.out.nAsset.IsExplicit()) {
            ret.pushKV("asset", coin.out.nAsset.GetAsset().GetHex());
        } else {
            ret.pushKV("assetcommitment", coin.out.nAsset.GetHex());
        }

        ret.pushKV("commitmentnonce", coin.out.nNonce.GetHex());
    }
    UniValue o(UniValue::VOBJ);
    ScriptPubKeyToUniv(coin.out.scriptPubKey, o, true);
    ret.pushKV("scriptPubKey", o);
    ret.pushKV("coinbase", (bool)coin.fCoinBase);

    return ret;
},
    };
}

static RPCHelpMan verifychain()
{
    return RPCHelpMan{"verifychain",
                "\nVerifies blockchain database.\n",
                {
                    {"checklevel", RPCArg::Type::NUM, RPCArg::DefaultHint{strprintf("%d, range=0-4", DEFAULT_CHECKLEVEL)},
                        strprintf("How thorough the block verification is:\n%s", MakeUnorderedList(CHECKLEVEL_DOC))},
                    {"nblocks", RPCArg::Type::NUM, RPCArg::DefaultHint{strprintf("%d, 0=all", DEFAULT_CHECKBLOCKS)}, "The number of blocks to check."},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Verified or not"},
                RPCExamples{
                    HelpExampleCli("verifychain", "")
            + HelpExampleRpc("verifychain", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const int check_level{request.params[0].isNull() ? DEFAULT_CHECKLEVEL : request.params[0].get_int()};
    const int check_depth{request.params[1].isNull() ? DEFAULT_CHECKBLOCKS : request.params[1].get_int()};

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);

    CChainState& active_chainstate = chainman.ActiveChainstate();
    return CVerifyDB().VerifyDB(
        active_chainstate, Params().GetConsensus(), active_chainstate.CoinsTip(), check_level, check_depth);
},
    };
}

static void SoftForkDescPushBack(const CBlockIndex* blockindex, UniValue& softforks, const Consensus::Params& params, Consensus::BuriedDeployment dep)
{
    // For buried deployments.

    if (!DeploymentEnabled(params, dep)) return;

    UniValue rv(UniValue::VOBJ);
    rv.pushKV("type", "buried");
    // getdeploymentinfo reports the softfork as active from when the chain height is
    // one below the activation height
    rv.pushKV("active", DeploymentActiveAfter(blockindex, params, dep));
    rv.pushKV("height", params.DeploymentHeight(dep));
    softforks.pushKV(DeploymentName(dep), rv);
}

static void SoftForkDescPushBack(const CBlockIndex* blockindex, UniValue& softforks, const Consensus::Params& consensusParams, Consensus::DeploymentPos id)
{
    // For BIP9 deployments.

    if (!DeploymentEnabled(consensusParams, id)) return;
    if (blockindex == nullptr) return;

    auto get_state_name = [](const ThresholdState state) -> std::string {
        switch (state) {
        case ThresholdState::DEFINED: return "defined";
        case ThresholdState::STARTED: return "started";
        case ThresholdState::LOCKED_IN: return "locked_in";
        case ThresholdState::ACTIVE: return "active";
        case ThresholdState::FAILED: return "failed";
        }
        return "invalid";
    };

    UniValue bip9(UniValue::VOBJ);

    const ThresholdState next_state = g_versionbitscache.State(blockindex, consensusParams, id);
    const ThresholdState current_state = g_versionbitscache.State(blockindex->pprev, consensusParams, id);

    const bool has_signal = (ThresholdState::STARTED == current_state || ThresholdState::LOCKED_IN == current_state);

    // BIP9 parameters
    if (has_signal) {
        bip9.pushKV("bit", consensusParams.vDeployments[id].bit);
    }
    bip9.pushKV("start_time", consensusParams.vDeployments[id].nStartTime);
    bip9.pushKV("timeout", consensusParams.vDeployments[id].nTimeout);
    bip9.pushKV("min_activation_height", consensusParams.vDeployments[id].min_activation_height);

    // BIP9 status
    bip9.pushKV("status", get_state_name(current_state));
    bip9.pushKV("since", g_versionbitscache.StateSinceHeight(blockindex->pprev, consensusParams, id));
    bip9.pushKV("status_next", get_state_name(next_state));

    // BIP9 signalling status, if applicable
    if (has_signal) {
        UniValue statsUV(UniValue::VOBJ);
        std::vector<bool> signals;
        BIP9Stats statsStruct = g_versionbitscache.Statistics(blockindex, consensusParams, id, &signals);
        statsUV.pushKV("period", statsStruct.period);
        statsUV.pushKV("elapsed", statsStruct.elapsed);
        statsUV.pushKV("count", statsStruct.count);
        if (ThresholdState::LOCKED_IN != current_state) {
            statsUV.pushKV("threshold", statsStruct.threshold);
            statsUV.pushKV("possible", statsStruct.possible);
        }
        bip9.pushKV("statistics", statsUV);

        std::string sig;
        sig.reserve(signals.size());
        for (const bool s : signals) {
            sig.push_back(s ? '#' : '-');
        }
        bip9.pushKV("signalling", sig);
    }

    UniValue rv(UniValue::VOBJ);
    rv.pushKV("type", "bip9");
    if (ThresholdState::ACTIVE == next_state) {
        rv.pushKV("height", g_versionbitscache.StateSinceHeight(blockindex, consensusParams, id));
    }
    rv.pushKV("active", ThresholdState::ACTIVE == next_state);
    rv.pushKV("bip9", bip9);

    softforks.pushKV(DeploymentName(id), rv);
}

namespace {
/* TODO: when -deprecatedrpc=softforks is removed, drop these */
UniValue DeploymentInfo(const CBlockIndex* tip, const Consensus::Params& consensusParams);
extern const std::vector<RPCResult> RPCHelpForDeployment;
}

// used by rest.cpp:rest_chaininfo, so cannot be static
RPCHelpMan getblockchaininfo()
{
    /* TODO: from v24, remove -deprecatedrpc=softforks */
    return RPCHelpMan{"getblockchaininfo",
                "Returns an object containing various state info regarding blockchain processing.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "chain", "current network name (main, test, signet, regtest, liquidv1, liquidv1test, liquidtestnet)"},
                        {RPCResult::Type::NUM, "blocks", "the height of the most-work fully-validated chain. The genesis block has height 0"},
                        {RPCResult::Type::NUM, "headers", "the current number of headers we have validated"},
                        {RPCResult::Type::STR, "bestblockhash", "the hash of the currently best block"},
                        {RPCResult::Type::NUM, "difficulty", "the current difficulty"},
                        {RPCResult::Type::NUM_TIME, "time", "The block time expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM, "verificationprogress", "estimate of verification progress [0..1]"},
                        {RPCResult::Type::BOOL, "initialblockdownload", "(debug information) estimate of whether this node is in Initial Block Download mode"},
                        {RPCResult::Type::STR_HEX, "chainwork", "total amount of work in active chain, in hexadecimal"},
                        {RPCResult::Type::NUM, "size_on_disk", "the estimated size of the block and undo files on disk"},
                        {RPCResult::Type::BOOL, "pruned", "if the blocks are subject to pruning"},
                        {RPCResult::Type::STR_HEX, "current_params_root", "the root of the currently active dynafed params"},
                        {RPCResult::Type::STR, "signblock_asm", "ASM of sign block challenge data from genesis block"},
                        {RPCResult::Type::STR_HEX, "signblock_hex", "Hex of sign block challenge data from genesis block"},
                        {RPCResult::Type::STR, "current_signblock_asm", "ASM of sign block challenge data enforced on the next block"},
                        {RPCResult::Type::STR_HEX, "current_signblock_hex", "Hex of sign block challenge data enforced on the next block"},
                        {RPCResult::Type::NUM, "max_block_witness", "maximum sized block witness serialized size for the next block"},
                        {RPCResult::Type::NUM, "epoch_length", "length of dynamic federations epoch, or signaling period"},
                        {RPCResult::Type::NUM, "total_valid_epochs", "number of epochs a given fedpscript is valid for, defined per chain"},
                        {RPCResult::Type::NUM, "epoch_age", "number of blocks into a dynamic federation epoch chain tip is. This number is between 0 to epoch_length-1"},
                        {RPCResult::Type::ARR, "extension_space", "array of extension fields in dynamic blockheader",
                        {
                            {RPCResult::Type::ELISION, "", ""}
                        }},
                        {RPCResult::Type::NUM, "pruneheight", /*optional=*/true, "lowest-height complete block stored (only present if pruning is enabled)"},
                        {RPCResult::Type::BOOL, "automatic_pruning", /*optional=*/true, "whether automatic pruning is enabled (only present if pruning is enabled)"},
                        {RPCResult::Type::NUM, "prune_target_size", /*optional=*/true, "the target size used by pruning (only present if automatic pruning is enabled)"},
                        {RPCResult::Type::OBJ_DYN, "softforks", "(DEPRECATED, returned only if config option -deprecatedrpc=softforks is passed) status of softforks",
                        {
                            {RPCResult::Type::OBJ, "xxxx", "name of the softfork",
                                RPCHelpForDeployment
                            },
                        }},
                        {RPCResult::Type::STR, "warnings", "any network and blockchain warnings"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblockchaininfo", "")
            + HelpExampleRpc("getblockchaininfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const ArgsManager& args{EnsureAnyArgsman(request.context)};
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();

    const CChainParams& chainparams = Params();
    const CBlockIndex* tip = active_chainstate.m_chain.Tip();
    CHECK_NONFATAL(tip);
    const int height = tip->nHeight;

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("chain",                 chainparams.NetworkIDString());
    obj.pushKV("blocks",                height);
    obj.pushKV("headers",               pindexBestHeader ? pindexBestHeader->nHeight : -1);
    obj.pushKV("bestblockhash",         tip->GetBlockHash().GetHex());
    if (!g_signed_blocks) {
        obj.pushKV("difficulty",            (double)GetDifficulty(tip));
    }
    obj.pushKV("time",                  (int64_t)tip->nTime);
    obj.pushKV("mediantime",            (int64_t)tip->GetMedianTimePast());
    obj.pushKV("verificationprogress",  GuessVerificationProgress(tip, Params().GetConsensus().nPowTargetSpacing));
    obj.pushKV("initialblockdownload",  active_chainstate.IsInitialBlockDownload());
    if (!g_signed_blocks) {
        obj.pushKV("chainwork", tip->nChainWork.GetHex());
    }
    obj.pushKV("size_on_disk", chainman.m_blockman.CalculateCurrentUsage());
    obj.pushKV("pruned",                node::fPruneMode);
    obj.pushKV("trim_headers",          node::fTrimHeaders); // ELEMENTS
    if (g_signed_blocks) {
        if (!DeploymentActiveAfter(tip, chainparams.GetConsensus(), Consensus::DEPLOYMENT_DYNA_FED)) {
            CScript sign_block_script = chainparams.GetConsensus().signblockscript;
            obj.pushKV("current_signblock_asm", ScriptToAsmStr(sign_block_script));
            obj.pushKV("current_signblock_hex", HexStr(sign_block_script));
            obj.pushKV("max_block_witness", (uint64_t)chainparams.GetConsensus().max_block_signature_size);
            UniValue arr(UniValue::VARR);
            for (const auto& extension : chainparams.GetConsensus().first_extension_space) {
                arr.push_back(HexStr(extension));
            }
            obj.pushKV("extension_space", arr);
        } else {
            const DynaFedParamEntry entry = ComputeNextBlockFullCurrentParameters(tip, chainparams.GetConsensus());
            obj.pushKV("current_params_root", entry.CalculateRoot().GetHex());
            obj.pushKV("current_signblock_asm", ScriptToAsmStr(entry.m_signblockscript));
            obj.pushKV("current_signblock_hex", HexStr(entry.m_signblockscript));
            obj.pushKV("max_block_witness", (uint64_t)entry.m_signblock_witness_limit);
            obj.pushKV("current_fedpeg_program", HexStr(entry.m_fedpeg_program));
            obj.pushKV("current_fedpeg_script", HexStr(entry.m_fedpegscript));
            UniValue arr(UniValue::VARR);
            for (const auto& extension : entry.m_extension_space) {
                arr.push_back(HexStr(extension));
            }
            obj.pushKV("extension_space", arr);
            obj.pushKV("epoch_length", (uint64_t)chainparams.GetConsensus().dynamic_epoch_length);
            obj.pushKV("total_valid_epochs", (uint64_t)chainparams.GetConsensus().total_valid_epochs);
            obj.pushKV("epoch_age", (uint64_t)(tip->nHeight % chainparams.GetConsensus().dynamic_epoch_length));
        }
    }

    if (node::fPruneMode) {
        const CBlockIndex* block = tip;
        CHECK_NONFATAL(block);
        while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
            block = block->pprev;
        }

        obj.pushKV("pruneheight",        block->nHeight);

        // if 0, execution bypasses the whole if block.
        bool automatic_pruning{args.GetIntArg("-prune", 0) != 1};
        obj.pushKV("automatic_pruning",  automatic_pruning);
        if (automatic_pruning) {
            obj.pushKV("prune_target_size",  node::nPruneTarget);
        }
    }

    if (IsDeprecatedRPCEnabled("softforks")) {
        const Consensus::Params& consensusParams = Params().GetConsensus();
        obj.pushKV("softforks", DeploymentInfo(tip, consensusParams));
    }

    obj.pushKV("warnings", GetWarnings(false).original);
    return obj;
},
    };
}

namespace {
const std::vector<RPCResult> RPCHelpForDeployment{
    {RPCResult::Type::STR, "type", "one of \"buried\", \"bip9\""},
    {RPCResult::Type::NUM, "height", /*optional=*/true, "height of the first block which the rules are or will be enforced (only for \"buried\" type, or \"bip9\" type with \"active\" status)"},
    {RPCResult::Type::BOOL, "active", "true if the rules are enforced for the mempool and the next block"},
    {RPCResult::Type::OBJ, "bip9", /*optional=*/true, "status of bip9 softforks (only for \"bip9\" type)",
    {
        {RPCResult::Type::NUM, "bit", /*optional=*/true, "the bit (0-28) in the block version field used to signal this softfork (only for \"started\" and \"locked_in\" status)"},
        {RPCResult::Type::NUM_TIME, "start_time", "the minimum median time past of a block at which the bit gains its meaning"},
        {RPCResult::Type::NUM_TIME, "timeout", "the median time past of a block at which the deployment is considered failed if not yet locked in"},
        {RPCResult::Type::NUM, "min_activation_height", "minimum height of blocks for which the rules may be enforced"},
        {RPCResult::Type::STR, "status", "status of deployment at specified block (one of \"defined\", \"started\", \"locked_in\", \"active\", \"failed\")"},
        {RPCResult::Type::NUM, "since", "height of the first block to which the status applies"},
        {RPCResult::Type::STR, "status_next", "status of deployment at the next block"},
        {RPCResult::Type::OBJ, "statistics", /*optional=*/true, "numeric statistics about signalling for a softfork (only for \"started\" and \"locked_in\" status)",
        {
            {RPCResult::Type::NUM, "period", "the length in blocks of the signalling period"},
            {RPCResult::Type::NUM, "threshold", /*optional=*/true, "the number of blocks with the version bit set required to activate the feature (only for \"started\" status)"},
            {RPCResult::Type::NUM, "elapsed", "the number of blocks elapsed since the beginning of the current period"},
            {RPCResult::Type::NUM, "count", "the number of blocks with the version bit set in the current period"},
            {RPCResult::Type::BOOL, "possible", /*optional=*/true, "returns false if there are not enough blocks left in this period to pass activation threshold (only for \"started\" status)"},
        }},
        {RPCResult::Type::STR, "signalling", "indicates blocks that signalled with a # and blocks that did not with a -"},
    }},
};

UniValue DeploymentInfo(const CBlockIndex* blockindex, const Consensus::Params& consensusParams)
{
    UniValue softforks(UniValue::VOBJ);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_HEIGHTINCB);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_DERSIG);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_CLTV);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_CSV);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_SEGWIT);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_DYNA_FED);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_TESTDUMMY);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_TAPROOT);
    SoftForkDescPushBack(blockindex, softforks, consensusParams, Consensus::DEPLOYMENT_SIMPLICITY);
    return softforks;
}
} // anon namespace

static RPCHelpMan getdeploymentinfo()
{
    return RPCHelpMan{"getdeploymentinfo",
        "Returns an object containing various state info regarding deployments of consensus changes.",
        {
            {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Default{"hash of current chain tip"}, "The block hash at which to query deployment state"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR, "hash", "requested block hash (or tip)"},
                {RPCResult::Type::NUM, "height", "requested block height (or tip)"},
                {RPCResult::Type::OBJ, "deployments", "", {
                    {RPCResult::Type::OBJ, "xxxx", "name of the deployment", RPCHelpForDeployment}
                }},
            }
        },
        RPCExamples{ HelpExampleCli("getdeploymentinfo", "") + HelpExampleRpc("getdeploymentinfo", "") },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const ChainstateManager& chainman = EnsureAnyChainman(request.context);
            LOCK(cs_main);
            const CChainState& active_chainstate = chainman.ActiveChainstate();

            const CBlockIndex* blockindex;
            if (request.params[0].isNull()) {
                blockindex = active_chainstate.m_chain.Tip();
                CHECK_NONFATAL(blockindex);
            } else {
                const uint256 hash(ParseHashV(request.params[0], "blockhash"));
                blockindex = chainman.m_blockman.LookupBlockIndex(hash);
                if (!blockindex) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
                }
            }

            const Consensus::Params& consensusParams = Params().GetConsensus();

            UniValue deploymentinfo(UniValue::VOBJ);
            deploymentinfo.pushKV("hash", blockindex->GetBlockHash().ToString());
            deploymentinfo.pushKV("height", blockindex->nHeight);
            deploymentinfo.pushKV("deployments", DeploymentInfo(blockindex, consensusParams));
            return deploymentinfo;
        },
    };
}

/** Comparison function for sorting the getchaintips heads.  */
struct CompareBlocksByHeight
{
    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const
    {
        /* Make sure that unequal blocks with the same height do not compare
           equal. Use the pointers themselves to make a distinction. */

        if (a->nHeight != b->nHeight)
          return (a->nHeight > b->nHeight);

        return a < b;
    }
};

static RPCHelpMan getchaintips()
{
    return RPCHelpMan{"getchaintips",
                "Return information about all known tips in the block tree,"
                " including the main chain as well as orphaned branches.\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {{RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "height", "height of the chain tip"},
                            {RPCResult::Type::STR_HEX, "hash", "block hash of the tip"},
                            {RPCResult::Type::NUM, "branchlen", "zero for main chain, otherwise length of branch connecting the tip to the main chain"},
                            {RPCResult::Type::STR, "status", "status of the chain, \"active\" for the main chain\n"
            "Possible values for status:\n"
            "1.  \"invalid\"               This branch contains at least one invalid block\n"
            "2.  \"headers-only\"          Not all blocks for this branch are available, but the headers are valid\n"
            "3.  \"valid-headers\"         All blocks are available for this branch, but they were never fully validated\n"
            "4.  \"valid-fork\"            This branch is not part of the active chain, but is fully validated\n"
            "5.  \"active\"                This is the tip of the active main chain, which is certainly valid"},
                        }}}},
                RPCExamples{
                    HelpExampleCli("getchaintips", "")
            + HelpExampleRpc("getchaintips", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChain& active_chain = chainman.ActiveChain();

    /*
     * Idea: The set of chain tips is the active chain tip, plus orphan blocks which do not have another orphan building off of them.
     * Algorithm:
     *  - Make one pass through BlockIndex(), picking out the orphan blocks, and also storing a set of the orphan block's pprev pointers.
     *  - Iterate through the orphan blocks. If the block isn't pointed to by another orphan, it is a chain tip.
     *  - Add the active chain tip
     */
    std::set<const CBlockIndex*, CompareBlocksByHeight> setTips;
    std::set<const CBlockIndex*> setOrphans;
    std::set<const CBlockIndex*> setPrevs;

    for (const std::pair<const uint256, CBlockIndex*>& item : chainman.BlockIndex()) {
        if (!active_chain.Contains(item.second)) {
            setOrphans.insert(item.second);
            setPrevs.insert(item.second->pprev);
        }
    }

    for (std::set<const CBlockIndex*>::iterator it = setOrphans.begin(); it != setOrphans.end(); ++it) {
        if (setPrevs.erase(*it) == 0) {
            setTips.insert(*it);
        }
    }

    // Always report the currently active tip.
    setTips.insert(active_chain.Tip());

    /* Construct the output array.  */
    UniValue res(UniValue::VARR);
    for (const CBlockIndex* block : setTips) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("height", block->nHeight);
        obj.pushKV("hash", block->phashBlock->GetHex());

        const int branchLen = block->nHeight - active_chain.FindFork(block)->nHeight;
        obj.pushKV("branchlen", branchLen);

        std::string status;
        if (active_chain.Contains(block)) {
            // This block is part of the currently active chain.
            status = "active";
        } else if (block->nStatus & BLOCK_FAILED_MASK) {
            // This block or one of its ancestors is invalid.
            status = "invalid";
        } else if (!block->HaveTxsDownloaded()) {
            // This block cannot be connected because full block data for it or one of its parents is missing.
            status = "headers-only";
        } else if (block->IsValid(BLOCK_VALID_SCRIPTS)) {
            // This block is fully validated, but no longer part of the active chain. It was probably the active block once, but was reorganized.
            status = "valid-fork";
        } else if (block->IsValid(BLOCK_VALID_TREE)) {
            // The headers for this block are valid, but it has not been validated. It was probably never part of the most-work chain.
            status = "valid-headers";
        } else {
            // No clue.
            status = "unknown";
        }
        obj.pushKV("status", status);

        res.push_back(obj);
    }

    return res;
},
    };
}

UniValue MempoolInfoToJSON(const CTxMemPool& pool)
{
    // Make sure this call is atomic in the pool.
    LOCK(pool.cs);
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("loaded", pool.IsLoaded());
    ret.pushKV("size", (int64_t)pool.size());
    ret.pushKV("bytes", (int64_t)pool.GetTotalTxSize());
    ret.pushKV("usage", (int64_t)pool.DynamicMemoryUsage());
    ret.pushKV("total_fee", ValueFromAmount(pool.GetTotalFee().GetValue()));
    size_t maxmempool = gArgs.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    ret.pushKV("maxmempool", (int64_t) maxmempool);
    ret.pushKV("mempoolminfee", ValueFromAmount(std::max(pool.GetMinFee(maxmempool), ::minRelayTxFee).GetFeePerK()));
    ret.pushKV("minrelaytxfee", ValueFromAmount(::minRelayTxFee.GetFeePerK()));
    ret.pushKV("unbroadcastcount", uint64_t{pool.GetUnbroadcastTxs().size()});
    return ret;
}

static RPCHelpMan getmempoolinfo()
{
    return RPCHelpMan{"getmempoolinfo",
                "\nReturns details on the active state of the TX memory pool.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "loaded", "True if the mempool is fully loaded"},
                        {RPCResult::Type::NUM, "size", "Current tx count"},
                        {RPCResult::Type::NUM, "bytes", "Sum of all virtual transaction sizes as defined in BIP 141. Differs from actual serialized size because witness data is discounted"},
                        {RPCResult::Type::NUM, "usage", "Total memory usage for the mempool"},
                        {RPCResult::Type::STR_AMOUNT, "total_fee", "Total fees for the mempool in " + CURRENCY_UNIT + ", ignoring modified fees through prioritisetransaction"},
                        {RPCResult::Type::NUM, "maxmempool", "Maximum memory usage for the mempool"},
                        {RPCResult::Type::STR_AMOUNT, "mempoolminfee", "Minimum fee rate in " + CURRENCY_UNIT + "/kvB for tx to be accepted. Is the maximum of minrelaytxfee and minimum mempool fee"},
                        {RPCResult::Type::STR_AMOUNT, "minrelaytxfee", "Current minimum relay fee for transactions"},
                        {RPCResult::Type::NUM, "unbroadcastcount", "Current number of transactions that haven't passed initial broadcast yet"}
                    }},
                RPCExamples{
                    HelpExampleCli("getmempoolinfo", "")
            + HelpExampleRpc("getmempoolinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return MempoolInfoToJSON(EnsureAnyMemPool(request.context));
},
    };
}

static RPCHelpMan preciousblock()
{
    return RPCHelpMan{"preciousblock",
                "\nTreats a block as if it were received before others with the same work.\n"
                "\nA later preciousblock call can override the effect of an earlier one.\n"
                "\nThe effects of preciousblock are not retained across restarts.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to mark as precious"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("preciousblock", "\"blockhash\"")
            + HelpExampleRpc("preciousblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    CBlockIndex* pblockindex;

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    {
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    BlockValidationState state;
    chainman.ActiveChainstate().PreciousBlock(state, pblockindex);

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan invalidateblock()
{
    return RPCHelpMan{"invalidateblock",
                "\nPermanently marks a block as invalid, as if it violated a consensus rule.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to mark as invalid"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("invalidateblock", "\"blockhash\"")
            + HelpExampleRpc("invalidateblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    BlockValidationState state;

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CBlockIndex* pblockindex;
    {
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }
    chainman.ActiveChainstate().InvalidateBlock(state, pblockindex);

    if (state.IsValid()) {
        chainman.ActiveChainstate().ActivateBestChain(state);
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan reconsiderblock()
{
    return RPCHelpMan{"reconsiderblock",
                "\nRemoves invalidity status of a block, its ancestors and its descendants, reconsider them for activation.\n"
                "This can be used to undo the effects of invalidateblock.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to reconsider"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("reconsiderblock", "\"blockhash\"")
            + HelpExampleRpc("reconsiderblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    {
        LOCK(cs_main);
        CBlockIndex* pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        chainman.ActiveChainstate().ResetBlockFailureFlags(pblockindex);
    }

    BlockValidationState state;
    chainman.ActiveChainstate().ActivateBestChain(state);

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan getchaintxstats()
{
    return RPCHelpMan{"getchaintxstats",
                "\nCompute statistics about the total number and rate of transactions in the chain.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::DefaultHint{"one month"}, "Size of the window in number of blocks"},
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"chain tip"}, "The hash of the block that ends the window."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM_TIME, "time", "The timestamp for the final block in the window, expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM, "txcount", "The total number of transactions in the chain up to that point"},
                        {RPCResult::Type::STR_HEX, "window_final_block_hash", "The hash of the final block in the window"},
                        {RPCResult::Type::NUM, "window_final_block_height", "The height of the final block in the window."},
                        {RPCResult::Type::NUM, "window_block_count", "Size of the window in number of blocks"},
                        {RPCResult::Type::NUM, "window_tx_count", /*optional=*/true, "The number of transactions in the window. Only returned if \"window_block_count\" is > 0"},
                        {RPCResult::Type::NUM, "window_interval", /*optional=*/true, "The elapsed time in the window in seconds. Only returned if \"window_block_count\" is > 0"},
                        {RPCResult::Type::NUM, "txrate", /*optional=*/true, "The average rate of transactions per second in the window. Only returned if \"window_interval\" is > 0"},
                    }},
                RPCExamples{
                    HelpExampleCli("getchaintxstats", "")
            + HelpExampleRpc("getchaintxstats", "2016")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const CBlockIndex* pindex;
    int blockcount = 30 * 24 * 60 * 60 / Params().GetConsensus().nPowTargetSpacing; // By default: 1 month

    if (request.params[1].isNull()) {
        LOCK(cs_main);
        pindex = chainman.ActiveChain().Tip();
    } else {
        uint256 hash(ParseHashV(request.params[1], "blockhash"));
        LOCK(cs_main);
        pindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        if (!chainman.ActiveChain().Contains(pindex)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Block is not in main chain");
        }
    }

    CHECK_NONFATAL(pindex != nullptr);

    if (request.params[0].isNull()) {
        blockcount = std::max(0, std::min(blockcount, pindex->nHeight - 1));
    } else {
        blockcount = request.params[0].get_int();

        if (blockcount < 0 || (blockcount > 0 && blockcount >= pindex->nHeight)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid block count: should be between 0 and the block's height - 1");
        }
    }

    const CBlockIndex* pindexPast = pindex->GetAncestor(pindex->nHeight - blockcount);
    int nTimeDiff = pindex->GetMedianTimePast() - pindexPast->GetMedianTimePast();
    int nTxDiff = pindex->nChainTx - pindexPast->nChainTx;

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("time", (int64_t)pindex->nTime);
    ret.pushKV("txcount", (int64_t)pindex->nChainTx);
    ret.pushKV("window_final_block_hash", pindex->GetBlockHash().GetHex());
    ret.pushKV("window_final_block_height", pindex->nHeight);
    ret.pushKV("window_block_count", blockcount);
    if (blockcount > 0) {
        ret.pushKV("window_tx_count", nTxDiff);
        ret.pushKV("window_interval", nTimeDiff);
        if (nTimeDiff > 0) {
            ret.pushKV("txrate", ((double)nTxDiff) / nTimeDiff);
        }
    }

    return ret;
},
    };
}

template<typename T>
static T CalculateTruncatedMedian(std::vector<T>& scores)
{
    size_t size = scores.size();
    if (size == 0) {
        return 0;
    }

    std::sort(scores.begin(), scores.end());
    if (size % 2 == 0) {
        return (scores[size / 2 - 1] + scores[size / 2]) / 2;
    } else {
        return scores[size / 2];
    }
}

void CalculatePercentilesByWeight(CAmount result[NUM_GETBLOCKSTATS_PERCENTILES], std::vector<std::pair<CAmount, int64_t>>& scores, int64_t total_weight)
{
    if (scores.empty()) {
        return;
    }

    std::sort(scores.begin(), scores.end());

    // 10th, 25th, 50th, 75th, and 90th percentile weight units.
    const double weights[NUM_GETBLOCKSTATS_PERCENTILES] = {
        total_weight / 10.0, total_weight / 4.0, total_weight / 2.0, (total_weight * 3.0) / 4.0, (total_weight * 9.0) / 10.0
    };

    int64_t next_percentile_index = 0;
    int64_t cumulative_weight = 0;
    for (const auto& element : scores) {
        cumulative_weight += element.second;
        while (next_percentile_index < NUM_GETBLOCKSTATS_PERCENTILES && cumulative_weight >= weights[next_percentile_index]) {
            result[next_percentile_index] = element.first;
            ++next_percentile_index;
        }
    }

    // Fill any remaining percentiles with the last value.
    for (int64_t i = next_percentile_index; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        result[i] = scores.back().first;
    }
}

template<typename T>
static inline bool SetHasKeys(const std::set<T>& set) {return false;}
template<typename T, typename Tk, typename... Args>
static inline bool SetHasKeys(const std::set<T>& set, const Tk& key, const Args&... args)
{
    return (set.count(key) != 0) || SetHasKeys(set, args...);
}

// outpoint (needed for the utxo index) + nHeight + fCoinBase
static constexpr size_t PER_UTXO_OVERHEAD = sizeof(COutPoint) + sizeof(uint32_t) + sizeof(bool);

static RPCHelpMan getblockstats()
{
    return RPCHelpMan{"getblockstats",
                "\nCompute per block statistics for a given window. All amounts are in " + CURRENCY_ATOM_FULL + "s.\n"
                "It won't work for some heights with pruning.\n",
                {
                    {"hash_or_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The block hash or height of the target block", "", {"", "string or numeric"}},
                    {"stats", RPCArg::Type::ARR, RPCArg::DefaultHint{"all values"}, "Values to plot (see result below)",
                        {
                            {"height", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selected statistic"},
                            {"time", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selected statistic"},
                        },
                        "stats"},
                   {"asset", RPCArg::Type::STR, RPCArg::DefaultHint{"policy asset"}, "asset for which the statistics will be computed"},
                },
                RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "asset", /*optional=*/true, "Asset for which the statistics are computed"},
                {RPCResult::Type::NUM, "avgfee", /*optional=*/true, "Average fee in the block"},
                {RPCResult::Type::NUM, "avgfeerate", /*optional=*/true, "Average feerate (in " + CURRENCY_ATOM_FULL + "s per virtual byte)"},
                {RPCResult::Type::NUM, "avgtxsize", /*optional=*/true, "Average transaction size"},
                {RPCResult::Type::STR_HEX, "blockhash", /*optional=*/true, "The block hash (to check for potential reorgs)"},
                {RPCResult::Type::ARR_FIXED, "feerate_percentiles", /*optional=*/true, "Feerates at the 10th, 25th, 50th, 75th, and 90th percentile weight unit (in " + CURRENCY_ATOM_FULL + " per virtual byte)",
                {
                    {RPCResult::Type::NUM, "10th_percentile_feerate", "The 10th percentile feerate"},
                    {RPCResult::Type::NUM, "25th_percentile_feerate", "The 25th percentile feerate"},
                    {RPCResult::Type::NUM, "50th_percentile_feerate", "The 50th percentile feerate"},
                    {RPCResult::Type::NUM, "75th_percentile_feerate", "The 75th percentile feerate"},
                    {RPCResult::Type::NUM, "90th_percentile_feerate", "The 90th percentile feerate"},
                }},
                {RPCResult::Type::NUM, "height", /*optional=*/true, "The height of the block"},
                {RPCResult::Type::NUM, "ins", /*optional=*/true, "The number of inputs (excluding coinbase)"},
                {RPCResult::Type::NUM, "maxfee", /*optional=*/true, "Maximum fee in the block"},
                {RPCResult::Type::NUM, "maxfeerate", /*optional=*/true, "Maximum feerate (in " + CURRENCY_ATOM_FULL + "s per virtual byte)"},
                {RPCResult::Type::NUM, "maxtxsize", /*optional=*/true, "Maximum transaction size"},
                {RPCResult::Type::NUM, "medianfee", /*optional=*/true, "Truncated median fee in the block"},
                {RPCResult::Type::NUM, "mediantime", /*optional=*/true, "The block median time past"},
                {RPCResult::Type::NUM, "mediantxsize", /*optional=*/true, "Truncated median transaction size"},
                {RPCResult::Type::NUM, "minfee", /*optional=*/true, "Minimum fee in the block"},
                {RPCResult::Type::NUM, "minfeerate", /*optional=*/true, "Minimum feerate (in " + CURRENCY_ATOM_FULL + "s per virtual byte)"},
                {RPCResult::Type::NUM, "mintxsize", /*optional=*/true, "Minimum transaction size"},
                {RPCResult::Type::NUM, "outs", /*optional=*/true, "The number of outputs"},
                {RPCResult::Type::NUM, "subsidy", /*optional=*/true, "The block subsidy"},
                {RPCResult::Type::NUM, "swtotal_size", /*optional=*/true, "Total size of all segwit transactions"},
                {RPCResult::Type::NUM, "swtotal_weight", /*optional=*/true, "Total weight of all segwit transactions"},
                {RPCResult::Type::NUM, "swtxs", /*optional=*/true, "The number of segwit transactions"},
                {RPCResult::Type::NUM, "time", /*optional=*/true, "The block time"},
                {RPCResult::Type::NUM, "total_out", /*optional=*/true, "Total amount in all outputs (excluding coinbase and thus reward [ie subsidy + totalfee])"},
                {RPCResult::Type::NUM, "total_size", /*optional=*/true, "Total size of all non-coinbase transactions"},
                {RPCResult::Type::NUM, "total_weight", /*optional=*/true, "Total weight of all non-coinbase transactions"},
                {RPCResult::Type::NUM, "totalfee", /*optional=*/true, "The fee total"},
                {RPCResult::Type::NUM, "txs", /*optional=*/true, "The number of transactions (including coinbase)"},
                {RPCResult::Type::NUM, "utxo_increase", /*optional=*/true, "The increase/decrease in the number of unspent outputs"},
                {RPCResult::Type::NUM, "utxo_size_inc", /*optional=*/true, "The increase/decrease in size for the utxo index (not discounting op_return and similar)"},
            }},
                RPCExamples{
                    HelpExampleCli("getblockstats", R"('"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"' '["minfeerate","avgfeerate"]')") +
                    HelpExampleCli("getblockstats", R"(1000 '["minfeerate","avgfeerate"]')") +
                    HelpExampleRpc("getblockstats", R"("00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09", ["minfeerate","avgfeerate"])") +
                    HelpExampleRpc("getblockstats", R"(1000, ["minfeerate","avgfeerate"])")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CBlockIndex* pindex{ParseHashOrHeight(request.params[0], chainman)};
    CHECK_NONFATAL(pindex != nullptr);

    std::set<std::string> stats;
    if (!request.params[1].isNull()) {
        const UniValue stats_univalue = request.params[1].get_array();
        for (unsigned int i = 0; i < stats_univalue.size(); i++) {
            const std::string stat = stats_univalue[i].get_str();
            stats.insert(stat);
        }
    }

    // ELEMENTS:
    CAsset asset = policyAsset;
    if (g_con_elementsmode && !request.params[2].isNull()) {
        std::string assetString = request.params[2].get_str();
        asset = GetAssetFromString(assetString);
        if (asset.IsNull()) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex: %s", assetString));
        }
    }

    const CBlock block = GetBlockChecked(pindex);
    const CBlockUndo blockUndo = GetUndoChecked(pindex);

    const bool do_all = stats.size() == 0; // Calculate everything if nothing selected (default)
    const bool do_mediantxsize = do_all || stats.count("mediantxsize") != 0;
    const bool do_medianfee = do_all || stats.count("medianfee") != 0;
    const bool do_feerate_percentiles = do_all || stats.count("feerate_percentiles") != 0;
    const bool loop_inputs = do_all || do_medianfee || do_feerate_percentiles ||
        SetHasKeys(stats, "utxo_size_inc", "totalfee", "avgfee", "avgfeerate", "minfee", "maxfee", "minfeerate", "maxfeerate");
    const bool loop_outputs = do_all || loop_inputs || stats.count("total_out");
    const bool do_calculate_size = do_mediantxsize ||
        SetHasKeys(stats, "total_size", "avgtxsize", "mintxsize", "maxtxsize", "swtotal_size");
    const bool do_calculate_weight = do_all || SetHasKeys(stats, "total_weight", "avgfeerate", "swtotal_weight", "avgfeerate", "feerate_percentiles", "minfeerate", "maxfeerate");
    const bool do_calculate_sw = do_all || SetHasKeys(stats, "swtxs", "swtotal_size", "swtotal_weight");

    CAmount maxfee = 0;
    CAmount maxfeerate = 0;
    CAmount minfee = MAX_MONEY;
    CAmount minfeerate = MAX_MONEY;
    CAmount total_out = 0;
    CAmount totalfee = 0;
    int64_t inputs = 0;
    int64_t maxtxsize = 0;
    int64_t mintxsize = MAX_BLOCK_SERIALIZED_SIZE;
    int64_t outputs = 0;
    int64_t swtotal_size = 0;
    int64_t swtotal_weight = 0;
    int64_t swtxs = 0;
    int64_t total_size = 0;
    int64_t total_weight = 0;
    int64_t utxo_size_inc = 0;
    std::vector<CAmount> fee_array;
    std::vector<std::pair<CAmount, int64_t>> feerate_array;
    std::vector<int64_t> txsize_array;

    for (size_t i = 0; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx.at(i);
        outputs += tx->vout.size();

        CAmount tx_total_out = 0;
        // ELEMENTS:
        CAmount elements_txfee = 0;
        if (g_con_elementsmode) {
            if (loop_outputs) {
                for (const CTxOut& out : tx->vout) {
                    if (out.IsFee() && out.nAsset.GetAsset() == asset) {
                        elements_txfee += out.nValue.GetAmount();
                    }
                    if (out.nValue.IsExplicit() && out.nAsset.IsExplicit() && out.nAsset.GetAsset() == asset) {
                        tx_total_out += out.nValue.GetAmount();
                    }
                    utxo_size_inc += GetSerializeSize(out, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
                }
            }
        } else {
            if (loop_outputs) {
                for (const CTxOut& out : tx->vout) {
                    tx_total_out += out.nValue.GetAmount();
                    utxo_size_inc += GetSerializeSize(out, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
                }
            }
        }

        if (tx->IsCoinBase()) {
            continue;
        }

        inputs += tx->vin.size(); // Don't count coinbase's fake input
        total_out += tx_total_out; // Don't count coinbase reward

        int64_t tx_size = 0;
        if (do_calculate_size) {

            tx_size = tx->GetTotalSize();
            if (do_mediantxsize) {
                txsize_array.push_back(tx_size);
            }
            maxtxsize = std::max(maxtxsize, tx_size);
            mintxsize = std::min(mintxsize, tx_size);
            total_size += tx_size;
        }

        int64_t weight = 0;
        if (do_calculate_weight) {
            weight = GetTransactionWeight(*tx);
            total_weight += weight;
        }

        if (do_calculate_sw && tx->HasWitness()) {
            ++swtxs;
            swtotal_size += tx_size;
            swtotal_weight += weight;
        }

        if (loop_inputs) {
            CAmount tx_total_in = 0;
            const auto& txundo = blockUndo.vtxundo.at(i - 1);
            for (const Coin& coin: txundo.vprevout) {
                const CTxOut& prevoutput = coin.out;

                tx_total_in += g_con_elementsmode ? 0 : prevoutput.nValue.GetAmount();
                utxo_size_inc -= GetSerializeSize(prevoutput, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
            }

            CAmount txfee = g_con_elementsmode ? elements_txfee : (tx_total_in - tx_total_out);
            CHECK_NONFATAL(MoneyRange(txfee));
            if (do_medianfee) {
                fee_array.push_back(txfee);
            }
            maxfee = std::max(maxfee, txfee);
            minfee = std::min(minfee, txfee);
            totalfee += txfee;

            // New feerate uses satoshis per virtual byte instead of per serialized byte
            CAmount feerate = weight ? (txfee * WITNESS_SCALE_FACTOR) / weight : 0;
            if (do_feerate_percentiles) {
                feerate_array.emplace_back(std::make_pair(feerate, weight));
            }
            maxfeerate = std::max(maxfeerate, feerate);
            minfeerate = std::min(minfeerate, feerate);
        }
    }

    CAmount feerate_percentiles[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };
    CalculatePercentilesByWeight(feerate_percentiles, feerate_array, total_weight);

    UniValue feerates_res(UniValue::VARR);
    for (int64_t i = 0; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        feerates_res.push_back(feerate_percentiles[i]);
    }

    UniValue ret_all(UniValue::VOBJ);
    ret_all.pushKV("asset", asset.GetHex());
    ret_all.pushKV("avgfee", (block.vtx.size() > 1) ? totalfee / (block.vtx.size() - 1) : 0);
    ret_all.pushKV("avgfeerate", total_weight ? (totalfee * WITNESS_SCALE_FACTOR) / total_weight : 0); // Unit: sat/vbyte
    ret_all.pushKV("avgtxsize", (block.vtx.size() > 1) ? total_size / (block.vtx.size() - 1) : 0);
    ret_all.pushKV("blockhash", pindex->GetBlockHash().GetHex());
    ret_all.pushKV("feerate_percentiles", feerates_res);
    ret_all.pushKV("height", (int64_t)pindex->nHeight);
    ret_all.pushKV("ins", inputs);
    ret_all.pushKV("maxfee", maxfee);
    ret_all.pushKV("maxfeerate", maxfeerate);
    ret_all.pushKV("maxtxsize", maxtxsize);
    ret_all.pushKV("medianfee", CalculateTruncatedMedian(fee_array));
    ret_all.pushKV("mediantime", pindex->GetMedianTimePast());
    ret_all.pushKV("mediantxsize", CalculateTruncatedMedian(txsize_array));
    ret_all.pushKV("minfee", (minfee == MAX_MONEY) ? 0 : minfee);
    ret_all.pushKV("minfeerate", (minfeerate == MAX_MONEY) ? 0 : minfeerate);
    ret_all.pushKV("mintxsize", mintxsize == MAX_BLOCK_SERIALIZED_SIZE ? 0 : mintxsize);
    ret_all.pushKV("outs", outputs);
    ret_all.pushKV("subsidy", GetBlockSubsidy(pindex->nHeight, Params().GetConsensus()));
    ret_all.pushKV("swtotal_size", swtotal_size);
    ret_all.pushKV("swtotal_weight", swtotal_weight);
    ret_all.pushKV("swtxs", swtxs);
    ret_all.pushKV("time", pindex->GetBlockTime());
    ret_all.pushKV("total_out", total_out);
    ret_all.pushKV("total_size", total_size);
    ret_all.pushKV("total_weight", total_weight);
    ret_all.pushKV("totalfee", totalfee);
    ret_all.pushKV("txs", (int64_t)block.vtx.size());
    ret_all.pushKV("utxo_increase", outputs - inputs);
    ret_all.pushKV("utxo_size_inc", utxo_size_inc);

    if (do_all) {
        return ret_all;
    }

    UniValue ret(UniValue::VOBJ);
    for (const std::string& stat : stats) {
        const UniValue& value = ret_all[stat];
        if (value.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid selected statistic '%s'", stat));
        }
        ret.pushKV(stat, value);
    }
    return ret;
},
    };
}

static RPCHelpMan savemempool()
{
    return RPCHelpMan{"savemempool",
                "\nDumps the mempool to disk. It will fail until the previous dump is fully loaded.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "filename", "the directory and file where the mempool was saved"},
                    }},
                RPCExamples{
                    HelpExampleCli("savemempool", "")
            + HelpExampleRpc("savemempool", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const ArgsManager& args{EnsureAnyArgsman(request.context)};
    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);

    if (!mempool.IsLoaded()) {
        throw JSONRPCError(RPC_MISC_ERROR, "The mempool was not loaded yet");
    }

    if (!DumpMempool(mempool)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Unable to dump mempool to disk");
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("filename", fs::path((args.GetDataDirNet() / "mempool.dat")).u8string());

    return ret;
},
    };
}

namespace {
//! Search for a given set of pubkey scripts
bool FindScriptPubKey(std::atomic<int>& scan_progress, const std::atomic<bool>& should_abort, int64_t& count, CCoinsViewCursor* cursor, const std::set<CScript>& needles, std::map<COutPoint, Coin>& out_results, std::function<void()>& interruption_point)
{
    scan_progress = 0;
    count = 0;
    while (cursor->Valid()) {
        COutPoint key;
        Coin coin;
        if (!cursor->GetKey(key) || !cursor->GetValue(coin)) return false;
        if (++count % 8192 == 0) {
            interruption_point();
            if (should_abort) {
                // allow to abort the scan via the abort reference
                return false;
            }
        }
        if (count % 256 == 0) {
            // update progress reference every 256 item
            uint32_t high = 0x100 * *key.hash.begin() + *(key.hash.begin() + 1);
            scan_progress = (int)(high * 100.0 / 65536.0 + 0.5);
        }
        if (needles.count(coin.out.scriptPubKey)) {
            out_results.emplace(key, coin);
        }
        cursor->Next();
    }
    scan_progress = 100;
    return true;
}
} // namespace

/** RAII object to prevent concurrency issue when scanning the txout set */
static std::atomic<int> g_scan_progress;
static std::atomic<bool> g_scan_in_progress;
static std::atomic<bool> g_should_abort_scan;
class CoinsViewScanReserver
{
private:
    bool m_could_reserve;
public:
    explicit CoinsViewScanReserver() : m_could_reserve(false) {}

    bool reserve() {
        CHECK_NONFATAL(!m_could_reserve);
        if (g_scan_in_progress.exchange(true)) {
            return false;
        }
        CHECK_NONFATAL(g_scan_progress == 0);
        m_could_reserve = true;
        return true;
    }

    ~CoinsViewScanReserver() {
        if (m_could_reserve) {
            g_scan_in_progress = false;
            g_scan_progress = 0;
        }
    }
};

static RPCHelpMan scantxoutset()
{
    // scriptPubKey corresponding to mainnet address 12cbQLTFMXRnSzktFkuoG3eHoMeFtpTu3S
    const std::string EXAMPLE_DESCRIPTOR_RAW = "raw(76a91411b366edfc0a8b66feebae5c2e25a7b6a5d1cf3188ac)#fm24fxxy";

    return RPCHelpMan{"scantxoutset",
        "\nScans the unspent transaction output set for entries that match certain output descriptors.\n"
        "Examples of output descriptors are:\n"
        "    addr(<address>)                      Outputs whose scriptPubKey corresponds to the specified address (does not include P2PK)\n"
        "    raw(<hex script>)                    Outputs whose scriptPubKey equals the specified hex scripts\n"
        "    combo(<pubkey>)                      P2PK, P2PKH, P2WPKH, and P2SH-P2WPKH outputs for the given pubkey\n"
        "    pkh(<pubkey>)                        P2PKH outputs for the given pubkey\n"
        "    sh(multi(<n>,<pubkey>,<pubkey>,...)) P2SH-multisig outputs for the given threshold and pubkeys\n"
        "\nIn the above, <pubkey> either refers to a fixed public key in hexadecimal notation, or to an xpub/xprv optionally followed by one\n"
        "or more path elements separated by \"/\", and optionally ending in \"/*\" (unhardened), or \"/*'\" or \"/*h\" (hardened) to specify all\n"
        "unhardened or hardened child keys.\n"
        "In the latter case, a range needs to be specified by below if different from 1000.\n"
        "For more information on output descriptors, see the documentation in the doc/descriptors.md file.\n",
        {
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "The action to execute\n"
                "\"start\" for starting a scan\n"
                "\"abort\" for aborting the current scan (returns true when abort was successful)\n"
                "\"status\" for progress report (in %) of the current scan"},
            {"scanobjects", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Array of scan objects. Required for \"start\" action\n"
                "Every scan object is either a string descriptor or an object:",
            {
                {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An output descriptor"},
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "An object with output descriptor and metadata",
                {
                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "An output descriptor"},
                    {"range", RPCArg::Type::RANGE, RPCArg::Default{1000}, "The range of HD chain indexes to explore (either end or [begin,end])"},
                }},
            },
                        "[scanobjects,...]"},
        },
        {
            RPCResult{"When action=='abort'", RPCResult::Type::BOOL, "", ""},
            RPCResult{"When action=='status' and no scan is in progress", RPCResult::Type::NONE, "", ""},
            RPCResult{"When action=='status' and scan is in progress", RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "progress", "The scan progress"},
            }},
            RPCResult{"When action=='start'", RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::BOOL, "success", "Whether the scan was completed"},
                {RPCResult::Type::NUM, "txouts", "The number of unspent transaction outputs scanned"},
                {RPCResult::Type::NUM, "height", "The current block height (index)"},
                {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at the tip of the chain"},
                {RPCResult::Type::ARR, "unspents", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::NUM, "vout", "The vout value"},
                        {RPCResult::Type::STR_HEX, "scriptPubKey", "The script key"},
                        {RPCResult::Type::STR, "desc", "A specialized descriptor for the matched scriptPubKey"},
                        {RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " of the unspent output"},
                        {RPCResult::Type::STR_HEX, "asset", "The asset ID"},
                        {RPCResult::Type::NUM, "height", "Height of the unspent transaction output"},
                    }},
                    {RPCResult::Type::STR_AMOUNT, "total_unblinded_bitcoin_amount", "The total amount of all found unspent unblinded outputs in " + CURRENCY_UNIT},
                }},
                {RPCResult::Type::STR_AMOUNT, "total_amount", "The total amount of all found unspent outputs in " + CURRENCY_UNIT},
            }},
        },
        RPCExamples{
            HelpExampleCli("scantxoutset", "start \'[\"" + EXAMPLE_DESCRIPTOR_RAW + "\"]\'") +
            HelpExampleCli("scantxoutset", "status") +
            HelpExampleCli("scantxoutset", "abort") +
            HelpExampleRpc("scantxoutset", "\"start\", [\"" + EXAMPLE_DESCRIPTOR_RAW + "\"]") +
            HelpExampleRpc("scantxoutset", "\"status\"") +
            HelpExampleRpc("scantxoutset", "\"abort\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR});

    UniValue result(UniValue::VOBJ);
    if (request.params[0].get_str() == "status") {
        CoinsViewScanReserver reserver;
        if (reserver.reserve()) {
            // no scan in progress
            return NullUniValue;
        }
        result.pushKV("progress", g_scan_progress);
        return result;
    } else if (request.params[0].get_str() == "abort") {
        CoinsViewScanReserver reserver;
        if (reserver.reserve()) {
            // reserve was possible which means no scan was running
            return false;
        }
        // set the abort flag
        g_should_abort_scan = true;
        return true;
    } else if (request.params[0].get_str() == "start") {
        CoinsViewScanReserver reserver;
        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Scan already in progress, use action \"abort\" or \"status\"");
        }

        if (request.params.size() < 2) {
            throw JSONRPCError(RPC_MISC_ERROR, "scanobjects argument is required for the start action");
        }

        std::set<CScript> needles;
        std::map<CScript, std::string> descriptors;
        CAmount total_in = 0;

        // loop through the scan objects
        for (const UniValue& scanobject : request.params[1].get_array().getValues()) {
            FlatSigningProvider provider;
            auto scripts = EvalDescriptorStringOrObject(scanobject, provider);
            for (const auto& script : scripts) {
                std::string inferred = InferDescriptor(script, provider)->ToString();
                needles.emplace(script);
                descriptors.emplace(std::move(script), std::move(inferred));
            }
        }

        // Scan the unspent transaction output set for inputs
        UniValue unspents(UniValue::VARR);
        std::vector<CTxOut> input_txos;
        std::map<COutPoint, Coin> coins;
        g_should_abort_scan = false;
        int64_t count = 0;
        std::unique_ptr<CCoinsViewCursor> pcursor;
        CBlockIndex* tip;
        NodeContext& node = EnsureAnyNodeContext(request.context);
        {
            ChainstateManager& chainman = EnsureChainman(node);
            LOCK(cs_main);
            CChainState& active_chainstate = chainman.ActiveChainstate();
            active_chainstate.ForceFlushStateToDisk();
            pcursor = active_chainstate.CoinsDB().Cursor();
            CHECK_NONFATAL(pcursor);
            tip = active_chainstate.m_chain.Tip();
            CHECK_NONFATAL(tip);
        }
        bool res = FindScriptPubKey(g_scan_progress, g_should_abort_scan, count, pcursor.get(), needles, coins, node.rpc_interruption_point);
        result.pushKV("success", res);
        result.pushKV("txouts", count);
        result.pushKV("height", tip->nHeight);
        result.pushKV("bestblock", tip->GetBlockHash().GetHex());

        if (!g_con_elementsmode) {
            for (const auto& it : coins) {
                const COutPoint& outpoint = it.first;
                const Coin& coin = it.second;
                const CTxOut& txo = coin.out;
                input_txos.push_back(txo);
                total_in += txo.nValue.GetAmount();

                UniValue unspent(UniValue::VOBJ);
                unspent.pushKV("txid", outpoint.hash.GetHex());
                unspent.pushKV("vout", (int32_t)outpoint.n);
                unspent.pushKV("scriptPubKey", HexStr(txo.scriptPubKey));
                unspent.pushKV("desc", descriptors[txo.scriptPubKey]);
                unspent.pushKV("amount", ValueFromAmount(txo.nValue.GetAmount()));
                unspent.pushKV("height", (int32_t)coin.nHeight);

                unspents.push_back(unspent);
            }
            result.pushKV("unspents", unspents);
            result.pushKV("total_amount", ValueFromAmount(total_in));
        } else {
            CAmount total_in_explicit_parent = 0;
            for (const auto& it : coins) {
                const COutPoint& outpoint = it.first;
                const Coin& coin = it.second;
                const CTxOut& txo = coin.out;
                input_txos.push_back(txo);
                if (txo.nValue.IsExplicit() && txo.nAsset.IsExplicit() && txo.nAsset.GetAsset() == Params().GetConsensus().pegged_asset) {
                    total_in_explicit_parent += txo.nValue.GetAmount();
                }

                UniValue unspent(UniValue::VOBJ);
                unspent.pushKV("txid", outpoint.hash.GetHex());
                unspent.pushKV("vout", (int32_t)outpoint.n);
                unspent.pushKV("scriptPubKey", HexStr(txo.scriptPubKey));
                unspent.pushKV("desc", descriptors[txo.scriptPubKey]);
                if (txo.nValue.IsExplicit()) {
                    unspent.pushKV("amount", ValueFromAmount(txo.nValue.GetAmount()));
                } else {
                    unspent.pushKV("amountcommitment", HexStr(txo.nValue.vchCommitment));
                }
                if (txo.nAsset.IsExplicit()) {
                    unspent.pushKV("asset", txo.nAsset.GetAsset().GetHex());
                } else {
                    unspent.pushKV("assetcommitment", HexStr(txo.nAsset.vchCommitment));
                }
                unspent.pushKV("height", (int32_t)coin.nHeight);

                unspents.push_back(unspent);
            }
            result.pushKV("unspents", unspents);
            result.pushKV("total_unblinded_bitcoin_amount", ValueFromAmount(total_in_explicit_parent));
        }
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid command");
    }
    return result;
},
    };
}

static RPCHelpMan getblockfilter()
{
    return RPCHelpMan{"getblockfilter",
                "\nRetrieve a BIP 157 content filter for a particular block.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the block"},
                    {"filtertype", RPCArg::Type::STR, RPCArg::Default{"basic"}, "The type name of the filter"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "filter", "the hex-encoded filter data"},
                        {RPCResult::Type::STR_HEX, "header", "the hex-encoded filter header"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblockfilter", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" \"basic\"") +
                    HelpExampleRpc("getblockfilter", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\", \"basic\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 block_hash = ParseHashV(request.params[0], "blockhash");
    std::string filtertype_name = "basic";
    if (!request.params[1].isNull()) {
        filtertype_name = request.params[1].get_str();
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(filtertype_name, filtertype)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown filtertype");
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        throw JSONRPCError(RPC_MISC_ERROR, "Index is not enabled for filtertype " + filtertype_name);
    }

    const CBlockIndex* block_index;
    bool block_was_connected;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        block_index = chainman.m_blockman.LookupBlockIndex(block_hash);
        if (!block_index) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        block_was_connected = block_index->IsValid(BLOCK_VALID_SCRIPTS);
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    BlockFilter filter;
    uint256 filter_header;
    if (!index->LookupFilter(block_index, filter) ||
        !index->LookupFilterHeader(block_index, filter_header)) {
        int err_code;
        std::string errmsg = "Filter not found.";

        if (!block_was_connected) {
            err_code = RPC_INVALID_ADDRESS_OR_KEY;
            errmsg += " Block was not connected to active chain.";
        } else if (!index_ready) {
            err_code = RPC_MISC_ERROR;
            errmsg += " Block filters are still in the process of being indexed.";
        } else {
            err_code = RPC_INTERNAL_ERROR;
            errmsg += " This error is unexpected and indicates index corruption.";
        }

        throw JSONRPCError(err_code, errmsg);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("filter", HexStr(filter.GetEncodedFilter()));
    ret.pushKV("header", filter_header.GetHex());
    return ret;
},
    };
}

/**
 * Serialize the UTXO set to a file for loading elsewhere.
 *
 * @see SnapshotMetadata
 */
static RPCHelpMan dumptxoutset()
{
    return RPCHelpMan{
        "dumptxoutset",
        "Write the serialized UTXO set to disk.",
        {
            {"path", RPCArg::Type::STR, RPCArg::Optional::NO, "Path to the output file. If relative, will be prefixed by datadir."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "coins_written", "the number of coins written in the snapshot"},
                    {RPCResult::Type::STR_HEX, "base_hash", "the hash of the base of the snapshot"},
                    {RPCResult::Type::NUM, "base_height", "the height of the base of the snapshot"},
                    {RPCResult::Type::STR, "path", "the absolute path that the snapshot was written to"},
                    {RPCResult::Type::STR_HEX, "txoutset_hash", "the hash of the UTXO set contents"},
                    {RPCResult::Type::NUM, "nchaintx", "the number of transactions in the chain up to and including the base block"},
                }
        },
        RPCExamples{
            HelpExampleCli("dumptxoutset", "utxo.dat")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const ArgsManager& args{EnsureAnyArgsman(request.context)};
    const fs::path path = fsbridge::AbsPathJoin(args.GetDataDirNet(), fs::u8path(request.params[0].get_str()));
    // Write to a temporary path and then move into `path` on completion
    // to avoid confusion due to an interruption.
    const fs::path temppath = fsbridge::AbsPathJoin(args.GetDataDirNet(), fs::u8path(request.params[0].get_str() + ".incomplete"));

    if (fs::exists(path)) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            path.u8string() + " already exists. If you are sure this is what you want, "
            "move it out of the way first");
    }

    FILE* file{fsbridge::fopen(temppath, "wb")};
    CAutoFile afile{file, SER_DISK, CLIENT_VERSION};
    NodeContext& node = EnsureAnyNodeContext(request.context);
    UniValue result = CreateUTXOSnapshot(
        node, node.chainman->ActiveChainstate(), afile, path, temppath);
    fs::rename(temppath, path);

    result.pushKV("path", path.u8string());
    return result;
},
    };
}

UniValue CreateUTXOSnapshot(
    NodeContext& node,
    CChainState& chainstate,
    CAutoFile& afile,
    const fs::path& path,
    const fs::path& temppath)
{
    std::unique_ptr<CCoinsViewCursor> pcursor;
    CCoinsStats stats{CoinStatsHashType::HASH_SERIALIZED};
    CBlockIndex* tip;

    {
        // We need to lock cs_main to ensure that the coinsdb isn't written to
        // between (i) flushing coins cache to disk (coinsdb), (ii) getting stats
        // based upon the coinsdb, and (iii) constructing a cursor to the
        // coinsdb for use below this block.
        //
        // Cursors returned by leveldb iterate over snapshots, so the contents
        // of the pcursor will not be affected by simultaneous writes during
        // use below this block.
        //
        // See discussion here:
        //   https://github.com/bitcoin/bitcoin/pull/15606#discussion_r274479369
        //
        LOCK(::cs_main);

        chainstate.ForceFlushStateToDisk();

        if (!GetUTXOStats(&chainstate.CoinsDB(), chainstate.m_blockman, stats, node.rpc_interruption_point)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read UTXO set");
        }

        pcursor = chainstate.CoinsDB().Cursor();
        tip = chainstate.m_blockman.LookupBlockIndex(stats.hashBlock);
        CHECK_NONFATAL(tip);
    }

    LOG_TIME_SECONDS(strprintf("writing UTXO snapshot at height %s (%s) to file %s (via %s)",
        tip->nHeight, tip->GetBlockHash().ToString(),
        fs::PathToString(path), fs::PathToString(temppath)));

    SnapshotMetadata metadata{tip->GetBlockHash(), stats.coins_count, tip->nChainTx};

    afile << metadata;

    COutPoint key;
    Coin coin;
    unsigned int iter{0};

    while (pcursor->Valid()) {
        if (iter % 5000 == 0) node.rpc_interruption_point();
        ++iter;
        if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
            afile << key;
            afile << coin;
        }

        pcursor->Next();
    }

    afile.fclose();

    UniValue result(UniValue::VOBJ);
    result.pushKV("coins_written", stats.coins_count);
    result.pushKV("base_hash", tip->GetBlockHash().ToString());
    result.pushKV("base_height", tip->nHeight);
    result.pushKV("path", path.u8string());
    result.pushKV("txoutset_hash", stats.hashSerialized.ToString());
    // Cast required because univalue doesn't have serialization specified for
    // `unsigned int`, nChainTx's type.
    result.pushKV("nchaintx", uint64_t{tip->nChainTx});
    return result;
}

//
// ELEMENTS:

static RPCHelpMan getsidechaininfo()
{
    return RPCHelpMan{"getsidechaininfo",
                "Returns an object containing various state info regarding sidechain functionality.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "fedpegscript", "The fedpegscript from genesis block"},
                        {RPCResult::Type::ARR, "current_fedpegscripts", "The currently-enforced fedpegscripts in hex. Peg-ins for any entries on this list are honored by consensus and policy. Newest first. Two total entries are possible",
                            {{RPCResult::Type::STR_HEX, "", "active fedpegscript"}}},
                        {RPCResult::Type::ARR, "current_fedpeg_programs", "The currently-enforced fedpegscript scriptPubKeys in hex. Prior to a transition this may be P2SH scriptpubkey, otherwise it will be a native segwit script. Results are paired in-order with current_fedpegscripts",
                            {{RPCResult::Type::STR_HEX, "", "active fedpegscript scriptPubKeys"}}},
                        {RPCResult::Type::STR_HEX, "pegged_asset", "Pegged asset type"},
                        {RPCResult::Type::STR, "min_peg_diff", "The minimum difficulty parent chain header target. Peg-in headers that have less work will be rejected as an anti-Dos measure"},
                        {RPCResult::Type::STR_HEX, "parent_blockhash", "The parent genesis blockhash as source of pegged-in funds"},
                        {RPCResult::Type::BOOL, "parent_chain_has_pow", "Whether parent chain has pow or signed blocks"},
                        {RPCResult::Type::STR, "parent_chain_signblockscript_asm", "If the parent chain has signed blocks, its signblockscript in ASM"},
                        {RPCResult::Type::STR_HEX, "parent_chain_signblockscript_hex", "If the parent chain has signed blocks, its signblockscript in hex"},
                        {RPCResult::Type::STR_HEX, "parent_pegged_asset", "If the parent chain has Confidential Assets, the asset id of the pegged asset in that chain"},
                        {RPCResult::Type::NUM, "pegin_confirmation_depth", "The number of mainchain confirmations required for a peg-in transaction to become valid"},
                        {RPCResult::Type::BOOL, "enforce_pak", "If peg-out authorization is being enforced"},
                    }},
                RPCExamples{
                    HelpExampleCli("getsidechaininfo", "")
                    + HelpExampleRpc("getsidechaininfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    LOCK(cs_main);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    const Consensus::Params& consensus = Params().GetConsensus();
    const uint256& parent_blockhash = Params().ParentGenesisBlockHash();

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("fedpegscript", HexStr(consensus.fedpegScript));
    // We use mempool_validation as true to show what is enforced for *next* block
    std::vector<std::pair<CScript, CScript>> fedpegscripts = GetValidFedpegScripts(chainman.ActiveChain().Tip(), consensus, true /* nextblock_validation */);
    UniValue fedpeg_prog_entries(UniValue::VARR);
    UniValue fedpeg_entries(UniValue::VARR);
    for (const auto& scripts : fedpegscripts) {
        fedpeg_prog_entries.push_back(HexStr(scripts.first));
        fedpeg_entries.push_back(HexStr(scripts.second));
    }
    obj.pushKV("current_fedpeg_programs", fedpeg_prog_entries);
    obj.pushKV("current_fedpegscripts", fedpeg_entries);
    obj.pushKV("pegged_asset", consensus.pegged_asset.GetHex());
    obj.pushKV("min_peg_diff", consensus.parentChainPowLimit.GetHex());
    obj.pushKV("parent_blockhash", parent_blockhash.GetHex());
    obj.pushKV("parent_chain_has_pow", consensus.ParentChainHasPow());
    obj.pushKV("enforce_pak", Params().GetEnforcePak());
    obj.pushKV("pegin_confirmation_depth", (uint64_t)consensus.pegin_min_depth);
    if (!consensus.ParentChainHasPow()) {
        obj.pushKV("parent_chain_signblockscript_asm", ScriptToAsmStr(consensus.parent_chain_signblockscript));
        obj.pushKV("parent_chain_signblockscript_hex", HexStr(consensus.parent_chain_signblockscript));
        obj.pushKV("parent_pegged_asset", consensus.parent_pegged_asset.GetHex());
    }
    return obj;
},
    };
}

// END ELEMENTS
//

// SEQUENTIA: report the chain's view of its parent chain (Bitcoin) anchor.
static RPCHelpMan getanchorstatus()
{
    return RPCHelpMan{"getanchorstatus",
                "\nReturns information about the chain tip's parent chain (Bitcoin) anchor and the connection to the parent chain daemon. Only available on chains with con_bitcoin_anchor enabled.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "validateanchor", "whether anchors are validated against the parent chain daemon"},
                        {RPCResult::Type::NUM, "tipheight", "height of this chain's tip"},
                        {RPCResult::Type::NUM, "anchorheight", "parent chain height referenced by the tip"},
                        {RPCResult::Type::STR_HEX, "anchorhash", "parent chain block hash referenced by the tip"},
                        {RPCResult::Type::STR, "anchorstatus", "result of checking the tip anchor against the parent chain daemon: \"ok\", \"not_found\", \"stale\", \"height_mismatch\", \"no_connection\" or \"not_validated\""},
                        {RPCResult::Type::NUM_TIME, "lastposfinalreject", "unix time of the most recent rival branch rejected at the PoS finality gate (0 = none since startup); recent rejections while the tip stands still signal a contested parent-chain fork"},
                        {RPCResult::Type::OBJ, "reconcile", /*optional=*/true, "state of the PoS finality reconciliation monitor (-posreconcile); only on PoS chains",
                        {
                            {RPCResult::Type::BOOL, "enabled", "whether the reconciliation monitor is on"},
                            {RPCResult::Type::STR, "state", "\"inactive\" (no rival certified branch), \"tracking\" (rival certified above the local finality; patience or anchor settling pending) or \"released\" (local finality released for the rival branch)"},
                            {RPCResult::Type::NUM, "rival_cert_height", /*optional=*/true, "height of the rival branch's highest quorum-certified block"},
                            {RPCResult::Type::STR_HEX, "rival_cert_hash", /*optional=*/true, "hash of that block"},
                            {RPCResult::Type::NUM, "patience_remaining", /*optional=*/true, "seconds of -posreconcilepatience still to elapse before a release can fire"},
                        }},
                    }},
                RPCExamples{
                    HelpExampleCli("getanchorstatus", "")
            + HelpExampleRpc("getanchorstatus", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_bitcoin_anchor) {
        throw JSONRPCError(RPC_MISC_ERROR, "Bitcoin anchoring (con_bitcoin_anchor) is not enabled on this chain");
    }
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    int tip_height;
    uint32_t anchor_height;
    uint256 anchor_hash;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        tip_height = tip->nHeight;
        anchor_height = tip->m_anchor_height;
        anchor_hash = tip->m_anchor_hash;
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("validateanchor", g_validate_anchor);
    result.pushKV("tipheight", tip_height);
    result.pushKV("anchorheight", (int64_t)anchor_height);
    result.pushKV("anchorhash", anchor_hash.GetHex());
    std::string status = "not_validated";
    if (g_validate_anchor && !anchor_hash.IsNull()) {
        switch (CheckMainchainAnchor(anchor_height, anchor_hash)) {
        case AnchorCheckResult::OK: status = "ok"; break;
        case AnchorCheckResult::NOT_FOUND: status = "not_found"; break;
        case AnchorCheckResult::STALE: status = "stale"; break;
        case AnchorCheckResult::HEIGHT_MISMATCH: status = "height_mismatch"; break;
        case AnchorCheckResult::NO_CONNECTION: status = "no_connection"; break;
        }
    }
    result.pushKV("anchorstatus", status);
    result.pushKV("lastposfinalreject", GetLastPosFinalForkRejectionTime());
    if (g_con_pos) {
        const PosReconcileStatus rec = GetPosReconcileStatus();
        UniValue reconcile(UniValue::VOBJ);
        reconcile.pushKV("enabled", g_pos_reconcile);
        reconcile.pushKV("state", rec.enabled ? rec.state : std::string("inactive"));
        if (rec.rival_cert_height >= 0) {
            reconcile.pushKV("rival_cert_height", rec.rival_cert_height);
            reconcile.pushKV("rival_cert_hash", rec.rival_cert_hash.GetHex());
        }
        if (rec.patience_remaining > 0) reconcile.pushKV("patience_remaining", rec.patience_remaining);
        result.pushKV("reconcile", reconcile);
    }
    return result;
},
    };
}

// SEQUENTIA PoS: the leader schedule for the slot extending the current tip.
static RPCHelpMan getposschedule()
{
    return RPCHelpMan{"getposschedule",
                "\nFor Proof-of-Stake chains: returns the stake-weighted leader schedule for the next block, "
                "best-ranked first. The schedule is derived deterministically from the current tip and its Bitcoin "
                "anchor, so every node computes the same ordering. See doc/sequentia/04-proof-of-stake.md.\n",
                {
                    {"count", RPCArg::Type::NUM, RPCArg::Default{10}, "Maximum number of ranked leaders to return."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "height", "height of the next block this schedule applies to"},
                        {RPCResult::Type::STR_HEX, "seed", "the election seed (H(tip || tip anchor || height))"},
                        {RPCResult::Type::NUM, "slot_interval", "seconds per rank (the liveness gate)"},
                        {RPCResult::Type::ARR, "schedule", "",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::NUM, "rank", "0 = primary leader"},
                                        {RPCResult::Type::STR_HEX, "pubkey", "staker public key"},
                                        {RPCResult::Type::NUM, "weight", "stake weight"},
                                        {RPCResult::Type::NUM, "slot_opens", "unix time at/after which this rank may produce"},
                                    }},
                            }},
                    }},
                RPCExamples{HelpExampleCli("getposschedule", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    size_t count = request.params[0].isNull() ? 10 : request.params[0].get_int();
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    int next_height;
    int64_t parent_time;
    uint256 seed;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        next_height = tip->nHeight + 1;
        parent_time = (int64_t)tip->nTime;
        seed = PosSeedForChild(tip);
    }
    std::vector<CPubKey> schedule = PosSchedule(registry, seed);
    UniValue arr(UniValue::VARR);
    for (size_t i = 0; i < schedule.size() && i < count; ++i) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("rank", (int)i);
        entry.pushKV("pubkey", HexStr(schedule[i]));
        entry.pushKV("weight", (uint64_t)registry.GetWeight(schedule[i]));
        entry.pushKV("slot_opens", parent_time + (int64_t)i * g_pos_slot_interval);
        arr.push_back(entry);
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("height", next_height);
    result.pushKV("seed", seed.GetHex());
    result.pushKV("slot_interval", g_pos_slot_interval);
    if (g_pos_vrf) {
        // Private sortition: slots depend on each staker's secret key, so no
        // public ordering exists. The "schedule" below is the legacy public
        // ranking and is NOT used by consensus in this mode.
        result.pushKV("sortition", "vrf");
        result.pushKV("total_weight", (uint64_t)PosTotalWeight(registry));
    }
    result.pushKV("schedule", arr);
    // Committee certification (principle 6): the first committee_size entries
    // of the schedule certify the block; a strict majority must countersign.
    std::vector<CPubKey> committee = PosCommittee(registry, seed);
    if (committee.size() > 1) {
        UniValue carr(UniValue::VARR);
        for (const CPubKey& member : committee) carr.push_back(HexStr(member));
        result.pushKV("committee", carr);
        result.pushKV("quorum", PosQuorum(committee.size()));
    }
    return result;
},
    };
}

// SEQUENTIA PoS: the full registered stake set.
static RPCHelpMan getstakerinfo()
{
    return RPCHelpMan{"getstakerinfo",
                "\nFor Proof-of-Stake chains: returns the registered stakers and their stake weights.\n",
                {
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, each value is an object with the stake weight and (impl spec Option A) the registered committee BLS public key, if any."},
                },
                {
                    RPCResult{"for verbose=false (default)",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {{RPCResult::Type::NUM, "pubkey", "stake weight, keyed by staker public key (hex)"}}},
                    RPCResult{"for verbose=true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {{RPCResult::Type::OBJ, "pubkey", "keyed by staker public key (hex)", {
                            {RPCResult::Type::NUM, "weight", "stake weight"},
                            {RPCResult::Type::STR_HEX, "blspubkey", "the registered committee BLS public key, or empty if none"},
                        }}}},
                },
                RPCExamples{HelpExampleCli("getstakerinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    const bool verbose = !request.params[0].isNull() && request.params[0].get_bool();
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    UniValue result(UniValue::VOBJ);
    for (const auto& entry : reg.Weights()) {
        if (!verbose) {
            result.pushKV(HexStr(entry.first), (uint64_t)entry.second);
        } else {
            UniValue o(UniValue::VOBJ);
            o.pushKV("weight", (uint64_t)entry.second);
            o.pushKV("blspubkey", HexStr(reg.GetBls(entry.first)));
            result.pushKV(HexStr(entry.first), o);
        }
    }
    return result;
},
    };
}

// SEQUENTIA PoS: this node's own sortition standing for the next block.
static RPCHelpMan getposslot()
{
    return RPCHelpMan{"getposslot",
                "\nFor Proof-of-Stake chains: this node's own standing in the election for the NEXT block.\n"
                "\nUnder VRF sortition (the Sequentia default) each staker draws a private slot from their own\n"
                "secret key, so no node can know another's slot in advance and no public ranking exists. This\n"
                "returns the draw of the keys THIS node produces with.\n"
                "\nA slot gates when a staker may PROPOSE, not who wins: every eligible staker proposes once its\n"
                "slot has opened (and never sooner than one slot interval after the parent), the committee\n"
                "collects the proposals for a short window, and then backs the one whose leader drew the\n"
                "lowest VRF output (after preferring the freshest Bitcoin anchor). So a low slot proposes\n"
                "early enough to be in the window, and the draw decides among those. A staker learns it led\n"
                "only when its proposal collects a quorum certificate.\n",
                {},
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::NUM, "height", "height of the next block these slots apply to"},
                    {RPCResult::Type::STR, "sortition", "\"vrf\" (private per-staker draw) or \"schedule\" (public ranking)"},
                    {RPCResult::Type::STR_HEX, "seed", "the election seed (from the tip and its Bitcoin anchor)"},
                    {RPCResult::Type::NUM, "slot_interval", "seconds a slot must wait per step"},
                    {RPCResult::Type::NUM, "total_weight", "total registered stake weight on the network"},
                    {RPCResult::Type::NUM, "stakers", "number of registered stakers"},
                    {RPCResult::Type::BOOL, "producing", "true if the autonomous producer is running on this node"},
                    {RPCResult::Type::NUM, "best_slot", "lowest slot among this node's keys (-1 if none is eligible)"},
                    {RPCResult::Type::NUM_TIME, "best_propose_at", "unix time at which the best slot actually proposes"},
                    {RPCResult::Type::ARR, "keys", "one entry per producing key holding an eligible stake",
                        {
                            {RPCResult::Type::OBJ, "", "",
                                {
                                    {RPCResult::Type::STR_HEX, "pubkey", "staker public key"},
                                    {RPCResult::Type::NUM, "weight", "this key's registered stake weight"},
                                    {RPCResult::Type::NUM, "share", "this key's share of the total stake (0..1)"},
                                    {RPCResult::Type::NUM, "slot", "the slot this key drew for the next block"},
                                    {RPCResult::Type::STR_HEX, "vrf", "this key's VRF output for the slot; the committee backs the LOWEST among the proposals it collects, so this is what decides who leads"},
                                    {RPCResult::Type::NUM_TIME, "slot_opens", "unix time from which this slot may produce (the consensus gate)"},
                                    {RPCResult::Type::NUM_TIME, "propose_at", "unix time this key actually proposes: the slot gate, but never sooner than one slot interval after the parent"},
                                    {RPCResult::Type::BOOL, "committee", "true if this key certifies this block as a committee member"},
                                }},
                        }},
                }},
                RPCExamples{HelpExampleCli("getposslot", "") + HelpExampleRpc("getposslot", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const StakeRegistry& registry = StakeRegistry::GetInstance();
    int next_height;
    int64_t parent_time;
    uint256 seed;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        next_height = tip->nHeight + 1;
        parent_time = (int64_t)tip->nTime;
        seed = PosSeedForChild(tip);
    }
    const uint64_t total_weight = PosTotalWeight(registry);

    UniValue result(UniValue::VOBJ);
    result.pushKV("height", next_height);
    result.pushKV("sortition", g_pos_vrf ? "vrf" : "schedule");
    result.pushKV("seed", seed.GetHex());
    result.pushKV("slot_interval", g_pos_slot_interval);
    result.pushKV("total_weight", total_weight);
    result.pushKV("stakers", (int)registry.Weights().size());

    // Only the running producer holds the secret keys the VRF draw needs; with no
    // producer there is nothing to report beyond the network-wide numbers above.
    PosProducer* producer = GetActivePosProducer();
    result.pushKV("producing", producer != nullptr);

    UniValue arr(UniValue::VARR);
    int64_t best_slot = -1;
    if (producer) {
        std::set<CPubKey> public_committee;
        if (g_pos_public_committee) public_committee = PosPublicCommitteeSet(registry, seed);
        for (const CKey& key : producer->Keys()) {
            const CPubKey pub = key.GetPubKey();
            const uint64_t weight = registry.GetWeight(pub);
            if (!PosIsEligibleStake(weight)) continue;
            uint64_t slot = 0;
            bool committee = false;
            uint256 beta;
            if (g_pos_vrf) {
                auto proof = VrfProve(key, Span<const unsigned char>(seed.begin(), 32));
                if (!proof) continue;
                if (!VrfVerify(pub, Span<const unsigned char>(seed.begin(), 32), *proof, beta)) continue;
                slot = PosVrfSlot(beta, weight, total_weight);
                committee = g_pos_public_committee ? public_committee.count(pub) > 0
                                                   : PosVrfIsCommitteeMember(beta, weight, total_weight);
            } else {
                std::optional<size_t> rank = PosRank(registry, seed, pub);
                if (!rank) continue;
                slot = (uint64_t)*rank;
                const std::vector<CPubKey> cmt = PosCommittee(registry, seed);
                committee = std::find(cmt.begin(), cmt.end(), pub) != cmt.end();
            }
            // The producer holds a cadence floor of one interval since the parent
            // (PosProducer::Step), so slots 0 and 1 both propose at the same time;
            // reporting the bare slot gate would promise a block that early.
            const int64_t slot_opens = parent_time + (int64_t)slot * g_pos_slot_interval;
            const int64_t propose_at = std::max(slot_opens, parent_time + g_pos_slot_interval);
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("pubkey", HexStr(pub));
            entry.pushKV("weight", weight);
            entry.pushKV("share", total_weight > 0 ? (double)weight / (double)total_weight : 0.0);
            entry.pushKV("slot", slot);
            if (g_pos_vrf) entry.pushKV("vrf", beta.GetHex());
            entry.pushKV("slot_opens", slot_opens);
            entry.pushKV("propose_at", propose_at);
            entry.pushKV("committee", committee);
            arr.push_back(entry);
            if (best_slot < 0 || (int64_t)slot < best_slot) best_slot = (int64_t)slot;
        }
    }
    result.pushKV("best_slot", best_slot);
    result.pushKV("best_propose_at", best_slot < 0 ? 0
        : std::max(parent_time + best_slot * g_pos_slot_interval, parent_time + g_pos_slot_interval));
    result.pushKV("keys", arr);
    return result;
},
    };
}

// SEQUENTIA PoS: who produced the recent blocks, and what they earned.
static RPCHelpMan getposrecentblocks()
{
    return RPCHelpMan{"getposrecentblocks",
                "\nFor Proof-of-Stake chains: the most recent blocks with the staker that produced each one\n"
                "and the fees it collected. Sequentia has no block subsidy, so a producer is paid exactly the\n"
                "fees of the block it leads: those are the block's coinbase outputs, listed here per asset.\n",
                {
                    {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many blocks back from the tip to report (max 1000)."},
                    {"producer", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Only report blocks produced by this staker public key."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::NUM, "scanned", "how many blocks were examined"},
                    {RPCResult::Type::NUM, "from_height", "lowest height examined"},
                    {RPCResult::Type::NUM, "to_height", "highest height examined (the tip)"},
                    {RPCResult::Type::ARR, "blocks", "newest first",
                        {
                            {RPCResult::Type::OBJ, "", "",
                                {
                                    {RPCResult::Type::NUM, "height", "block height"},
                                    {RPCResult::Type::STR_HEX, "hash", "block hash"},
                                    {RPCResult::Type::NUM_TIME, "time", "block time"},
                                    {RPCResult::Type::NUM, "wait", "seconds after the previous block that this one landed"},
                                    {RPCResult::Type::STR_HEX, "producer", "public key of the staker that produced it"},
                                    {RPCResult::Type::NUM, "txs", "transactions in the block, excluding the coinbase"},
                                    {RPCResult::Type::OBJ_DYN, "fees", "fees the producer collected, keyed by asset id",
                                        {{RPCResult::Type::STR_AMOUNT, "asset", "amount of that asset"}}},
                                }},
                        }},
                }},
                RPCExamples{HelpExampleCli("getposrecentblocks", "100") + HelpExampleRpc("getposrecentblocks", "100")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    int count = request.params[0].isNull() ? 100 : request.params[0].get_int();
    if (count < 1 || count > 1000) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be between 1 and 1000");
    }
    std::optional<CPubKey> filter;
    if (!request.params[1].isNull()) {
        const std::vector<unsigned char> raw = ParseHexV(request.params[1], "producer");
        CPubKey pub(raw);
        if (!pub.IsFullyValid()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "producer is not a valid public key");
        }
        filter = pub;
    }
    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    // Collect the indices under cs_main, then read the bodies without it: reading
    // up to 1000 blocks off disk should not hold up the whole node.
    std::vector<const CBlockIndex*> indices;
    int tip_height;
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = chainman.ActiveChain().Tip();
        tip_height = pindex->nHeight;
        for (int i = 0; i < count && pindex && pindex->nHeight > 0; ++i) {
            indices.push_back(pindex);
            pindex = pindex->pprev;
        }
    }

    UniValue arr(UniValue::VARR);
    for (const CBlockIndex* pindex : indices) {
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) continue;
        std::optional<PosChallengeParts> parts = ParsePosBlockChallenge(block.proof.challenge);
        if (!parts) continue;
        if (filter && parts->leader != *filter) continue;

        UniValue entry(UniValue::VOBJ);
        entry.pushKV("height", pindex->nHeight);
        entry.pushKV("hash", pindex->GetBlockHash().GetHex());
        entry.pushKV("time", (int64_t)pindex->nTime);
        entry.pushKV("wait", pindex->pprev ? (int64_t)pindex->nTime - (int64_t)pindex->pprev->nTime : 0);
        entry.pushKV("producer", HexStr(parts->leader));
        entry.pushKV("txs", (int)(block.vtx.empty() ? 0 : block.vtx.size() - 1));

        // The coinbase pays the leader one output per fee asset. Commitment
        // outputs (OP_RETURN markers, the VRF proof) carry no value, and blinded
        // amounts cannot be read here — both are simply skipped.
        UniValue fees(UniValue::VOBJ);
        if (!block.vtx.empty()) {
            CAmountMap collected;
            for (const CTxOut& out : block.vtx[0]->vout) {
                if (out.scriptPubKey.IsUnspendable()) continue;
                if (!out.nAsset.IsExplicit() || !out.nValue.IsExplicit()) continue;
                if (out.nValue.GetAmount() <= 0) continue;
                collected[out.nAsset.GetAsset()] += out.nValue.GetAmount();
            }
            for (const auto& it : collected) {
                fees.pushKV(it.first.GetHex(), ValueFromAmount(it.second));
            }
        }
        entry.pushKV("fees", fees);
        arr.push_back(entry);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("scanned", (int)indices.size());
    result.pushKV("from_height", indices.empty() ? tip_height : indices.back()->nHeight);
    result.pushKV("to_height", tip_height);
    result.pushKV("blocks", arr);
    return result;
},
    };
}

// SEQUENTIA PoS: enable autonomous block production at runtime, with no restart.
static RPCHelpMan startposproducer()
{
    return RPCHelpMan{"startposproducer",
                "\nEnable autonomous Proof-of-Stake block production at runtime, with no restart.\n"
                "Adds the given staker private key(s) to the running producer (creating it if this\n"
                "node was not producing yet), and persists the choice (settings.json in the datadir)\n"
                "so production resumes automatically after a restart. Each key must hold an eligible\n"
                "registered stake (see registerstake / getstakerinfo) to actually produce blocks.\n",
                {
                    {"keys", RPCArg::Type::ARR, RPCArg::Optional::NO, "Staker private key(s) in WIF. Kept secret in the datadir.",
                        {{"wif", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A staker private key (WIF)."}}},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::BOOL, "producing", "true if the autonomous producer is now running"},
                    {RPCResult::Type::NUM, "keys", "number of distinct producer keys now loaded"},
                    {RPCResult::Type::BOOL, "persisted", "true if the choice was saved for the next restart"},
                }},
                RPCExamples{HelpExampleCli("startposproducer", "'[\"cV…WIF…\"]'")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    NodeContext& node = EnsureAnyNodeContext(request.context);

    // Validate and de-duplicate the requested keys. Also gather the keys a running
    // producer already holds (merged with the new ones) so the PERSISTED set never
    // drops a key this node was already producing with.
    std::set<CPubKey> seen;
    std::vector<std::string> all_wifs;          // merged set, for persistence
    std::vector<CKey> new_keys;                 // newly requested keys not already loaded
    const bool already_running = (node.pos_producer != nullptr);
    if (already_running) {
        for (const CKey& k : node.pos_producer->Keys()) {
            if (seen.insert(k.GetPubKey()).second) all_wifs.push_back(EncodeSecret(k));
        }
    }
    const UniValue& arr = request.params[0].get_array();
    for (size_t i = 0; i < arr.size(); ++i) {
        CKey key = DecodeSecret(arr[i].get_str());
        if (!key.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid staker private key (expected a WIF)");
        if (seen.insert(key.GetPubKey()).second) { all_wifs.push_back(EncodeSecret(key)); new_keys.push_back(key); }
    }
    if (all_wifs.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Provide at least one staker private key (WIF)");

    // Start the autonomous producer if this node is not producing yet. We deliberately
    // do NOT rebuild a *running* producer here: net_processing reads the active producer
    // pointer locklessly on the message thread (GetActivePosProducer), so destroying a
    // live producer would be a use-after-free. When already running, the merged keys are
    // persisted instead and take effect on the next restart (handled below).
    bool started_now = false;
    if (!already_running) {
        std::vector<CKey> keys = new_keys;
        CTxMemPool& mempool = EnsureMemPool(node);
        ChainstateManager& chainman = EnsureChainman(node);
        CConnman& connman = EnsureConnman(node);
        node.pos_producer = std::make_unique<PosProducer>(chainman, mempool, Params(), &connman, std::move(keys));
        node.pos_producer->Start();
        started_now = true;
    }

    // Reflect into the live args so status readers (GUI overview / staking page) update,
    // and persist to settings.json so the existing startup path (AppInitMain:
    // -posproducer + -posproducerkey) resumes production after a restart with no manual
    // config editing.
    gArgs.ForceSetArg("-posproducer", "1");
    bool persisted = false;
    {
        UniValue wif_arr(UniValue::VARR);
        for (const std::string& w : all_wifs) wif_arr.push_back(w);
        gArgs.LockSettings([&](util::Settings& settings) {
            settings.rw_settings["posproducer"] = true;
            settings.rw_settings["posproducerkey"] = wif_arr;
        });
        persisted = gArgs.WriteSettingsFile();
    }

    const int live_keys = node.pos_producer ? (int)node.pos_producer->Keys().size() : 0;
    UniValue result(UniValue::VOBJ);
    result.pushKV("producing", node.pos_producer != nullptr);
    result.pushKV("keys", live_keys);                 // keys active in the running producer now
    result.pushKV("persisted", persisted);            // saved set (size all_wifs) applies next restart
    result.pushKV("started", started_now);            // false if it was already producing
    return result;
},
    };
}

// SEQUENTIA PoS: build the canonical staking-output script for a staker key.
static RPCHelpMan getdelegationscript()
{
    return RPCHelpMan{"getdelegationscript",
                "\nSEQUENTIA: returns the canonical delegation-record script, which lends a staker's block-signing\n"
                "rights to a signer (a staking-pool operator, or the staker's own online key) WITHOUT moving the\n"
                "stake. Fund this bare script with a small amount to activate it. While the output is unspent, all\n"
                "of the controller's stake weight counts for the signer, and the signer is the key that must\n"
                "produce and sign blocks (and which the coinbase pays).\n"
                "\nThe signer can NEVER spend the staked coins: only the controller key can. Spending this record\n"
                "(which only the controller can do) reclaims the rights; spending it and creating another in the\n"
                "same transaction rotates to a new signer. Because the record is a separate output, this works\n"
                "even while the staking output itself is frozen by a vesting lock (see getstakescript).\n",
                {
                    {"controller", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The staker public key that owns the stake (hex). It alone may spend this record."},
                    {"signer", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The public key that will produce blocks with the controller's weight (hex)."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "script", "the delegation-record scriptPubKey (hex)"},
                        {RPCResult::Type::STR_HEX, "controller", "the controller public key"},
                        {RPCResult::Type::STR_HEX, "signer", "the signer public key"},
                    }},
                RPCExamples{HelpExampleCli("getdelegationscript", "\"02aa...\" \"02bb...\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    CPubKey controller(ParseHexV(request.params[0], "controller"));
    CPubKey signer(ParseHexV(request.params[1], "signer"));
    if (!controller.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid controller public key");
    if (!signer.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid signer public key");

    UniValue result(UniValue::VOBJ);
    result.pushKV("script", HexStr(BuildDelegationScript(controller, signer)));
    result.pushKV("controller", HexStr(controller));
    result.pushKV("signer", HexStr(signer));
    return result;
},
    };
}

static RPCHelpMan getpayoutscript()
{
    return RPCHelpMan{"getpayoutscript",
                "\nSEQUENTIA: returns the canonical payout-record script, by which a block producer commits to how\n"
                "the fees it earns will be paid. Fund this bare script with a small amount to announce the policy.\n"
                "\nMODES:\n"
                "  direct  - the coinbase must pay `payout_script`. The operator cannot silently redirect the reward,\n"
                "            but the chain does not check that the destination shares anything with delegators.\n"
                "  lottery - the coinbase must pay ONE participant, drawn from everyone who delegated to this signer,\n"
                "            weighted by stake, from a seed derived from Bitcoin's proof of work (so it cannot be\n"
                "            biased). Each delegator earns its exact proportional share over time, with no accounting,\n"
                "            at the cost of rare lumpy payouts rather than smoothed income. `commission_bp` is the\n"
                "            share of blocks the operator keeps, in basis points (10000 = 100%).\n"
                "\nA policy may not take effect until `activation`, which must be at least the chain's notice period\n"
                "(-pospayoutnotice) beyond the block that announces it. That delay is what lets delegators audit a pool\n"
                "and leave (instantly, unilaterally) before a hostile change binds. Announcing does not cancel an\n"
                "earlier policy: the one in force at a height is the announced policy with the greatest activation at\n"
                "or below it. Inspect any producer's committed policies with getpayoutinfo before delegating.\n",
                {
                    {"signer", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block-producing public key this policy binds (hex). It alone may spend this record."},
                    {"activation", RPCArg::Type::NUM, RPCArg::Optional::NO, "Block height from which the policy binds."},
                    {"mode", RPCArg::Type::STR, RPCArg::Optional::NO, "\"direct\" or \"lottery\"."},
                    {"payout_script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "direct mode: the scriptPubKey the coinbase must pay (hex, at most 110 bytes)."},
                    {"commission_bp", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "lottery mode: basis points of blocks the operator keeps (0..10000, default 0)."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "script", "the payout-record scriptPubKey (hex)"},
                        {RPCResult::Type::STR, "mode", "\"direct\" or \"lottery\""},
                        {RPCResult::Type::NUM, "activation", "the height from which the policy binds"},
                        {RPCResult::Type::NUM, "notice_blocks", "the chain's minimum notice period, in blocks"},
                    }},
                RPCExamples{HelpExampleCli("getpayoutscript", "\"02bb...\" 5000 \"lottery\" null 500")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    CPubKey signer(ParseHexV(request.params[0], "signer"));
    if (!signer.IsFullyValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid signer public key");

    PosPayoutPolicy policy;
    policy.activation = request.params[1].get_int64();
    if (policy.activation <= 0 || policy.activation > 0xffffffffLL) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "activation must be between 1 and 4294967295");
    }
    const std::string mode = request.params[2].get_str();
    if (mode == "direct") {
        policy.mode = PosPayoutMode::DIRECT;
        if (request.params[3].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "direct mode requires payout_script");
        std::vector<unsigned char> spk = ParseHexV(request.params[3], "payout_script");
        if (spk.empty() || spk.size() > 110) throw JSONRPCError(RPC_INVALID_PARAMETER, "payout_script must be 1..110 bytes");
        policy.script = CScript(spk.begin(), spk.end());
    } else if (mode == "lottery") {
        policy.mode = PosPayoutMode::LOTTERY;
        int64_t bp = request.params[4].isNull() ? 0 : request.params[4].get_int64();
        if (bp < 0 || bp > (int64_t)POS_COMMISSION_DENOM) throw JSONRPCError(RPC_INVALID_PARAMETER, "commission_bp must be 0..10000");
        policy.commission_bp = (uint32_t)bp;
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "mode must be \"direct\" or \"lottery\"");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("script", HexStr(BuildPayoutScript(signer, policy)));
    result.pushKV("mode", mode);
    result.pushKV("activation", policy.activation);
    result.pushKV("notice_blocks", (int64_t)g_pos_payout_notice);
    return result;
},
    };
}

static RPCHelpMan getpayoutinfo()
{
    return RPCHelpMan{"getpayoutinfo",
                "\nSEQUENTIA: every payout policy committed on-chain, per block producer, derived from unspent payout\n"
                "records. This is the audit surface: before delegating to a pool, check what it has committed to, and\n"
                "watch it afterwards -- a change must be announced at least the notice period in advance, and you can\n"
                "leave a pool instantly and unilaterally. Producers absent here pay themselves (the default).\n",
                {
                    {"signer", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Restrict to one block-producing public key (hex)."},
                },
                RPCResult{RPCResult::Type::OBJ_DYN, "", "", {
                    {RPCResult::Type::ARR, "signer", "policies announced by this signer, by activation height", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::NUM, "activation", "height from which the policy binds"},
                            {RPCResult::Type::BOOL, "in_force", "whether it binds at the current tip"},
                            {RPCResult::Type::STR, "mode", "\"direct\" or \"lottery\""},
                            {RPCResult::Type::STR_HEX, "payout_script", /*optional=*/true, "direct: the committed payee"},
                            {RPCResult::Type::NUM, "commission_bp", /*optional=*/true, "lottery: operator's basis points"},
                        }},
                    }},
                }},
                RPCExamples{HelpExampleCli("getpayoutinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    std::string filter;
    if (!request.params[0].isNull()) filter = request.params[0].get_str();

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    int64_t height;
    {
        LOCK(cs_main);
        height = chainman.ActiveChain().Height();
    }

    StakeRegistry& registry = StakeRegistry::GetInstance();
    UniValue result(UniValue::VOBJ);
    for (const auto& signer_entry : registry.Payouts()) {
        const std::string signer_hex = HexStr(signer_entry.first);
        if (!filter.empty() && signer_hex != filter) continue;
        const auto in_force = registry.PayoutFor(signer_entry.first, height);
        UniValue arr(UniValue::VARR);
        for (const auto& e : signer_entry.second) {
            const PosPayoutPolicy& p = e.second;
            UniValue o(UniValue::VOBJ);
            o.pushKV("activation", p.activation);
            o.pushKV("in_force", in_force.has_value() && *in_force == p);
            o.pushKV("mode", p.mode == PosPayoutMode::DIRECT ? "direct" : "lottery");
            if (p.mode == PosPayoutMode::DIRECT) o.pushKV("payout_script", HexStr(p.script));
            else o.pushKV("commission_bp", (int64_t)p.commission_bp);
            arr.push_back(o);
        }
        result.pushKV(signer_hex, arr);
    }
    return result;
},
    };
}

static RPCHelpMan getdelegationinfo()
{
    return RPCHelpMan{"getdelegationinfo",
                "\nSEQUENTIA: the live delegation map, derived from unspent delegation records in the UTXO set.\n"
                "Each entry maps a controller (the key that owns the stake) to the signer currently producing\n"
                "blocks with that stake's weight. Stakers absent from this map sign for themselves.\n",
                {},
                RPCResult{RPCResult::Type::OBJ_DYN, "", "", {
                    {RPCResult::Type::STR_HEX, "controller", "the signer public key that this controller's weight counts for"},
                }},
                RPCExamples{HelpExampleCli("getdelegationinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    UniValue result(UniValue::VOBJ);
    for (const auto& e : StakeRegistry::GetInstance().Delegations()) {
        result.pushKV(HexStr(e.first), HexStr(e.second));
    }
    return result;
},
    };
}

static RPCHelpMan getstakescript()
{
    return RPCHelpMan{"getstakescript",
                "\nFor Proof-of-Stake chains: returns the canonical staking-output script for a staker public key. "
                "Sending an explicit Sequence (SEQ) amount to this (bare) script registers that amount as the key's "
                "on-chain stake while the output remains unspent. Spending it (unbonding) requires the staker's "
                "signature and csv_blocks of relative-height maturity, enforced by the script itself.\n",
                {
                    {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The staker public key (hex)."},
                    {"csv_blocks", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Height-based unbonding delay, in blocks (BIP68 relative-height CSV, max 65535)."},
                    {"csv_seconds", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Time-based unbonding delay, in seconds (BIP68 relative-time CSV, rounded up to 512s units). Use this when the minimum lock exceeds what a height CSV can express. Mutually exclusive with csv_blocks."},
                    {"blspubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "SEQUENTIA: committee BLS public key to register with this stake (from getblsregistration), so the staker can join the public fixed-size committee. Requires pop."},
                    {"pop", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "SEQUENTIA: the BLS proof-of-possession for blspubkey (from getblsregistration)."},
                    {"liquid_locktime", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "SEQUENTIA vesting: an absolute timelock (BIP65) before which the stake cannot be spent, and so cannot be sold or transferred. A unix time (>=500000000) or a block height (<500000000). The output still accrues stake weight for the whole period, which is what makes a \"staking-only period\" expressible. Prefer a unix time: block heights drift against a calendar over a multi-year vest."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "script", "the staking scriptPubKey (hex)"},
                        {RPCResult::Type::NUM, "csv", "the raw BIP68 CSV value encoded in the script"},
                        {RPCResult::Type::STR, "csv_type", "\"height\" or \"time\""},
                        {RPCResult::Type::NUM, "lock_seconds", "the unbonding lock the script enforces, in seconds"},
                        {RPCResult::Type::NUM, "min_unbonding_seconds", "the chain's minimum unbonding lock for stake to count, in seconds"},
                        {RPCResult::Type::NUM, "liquid_locktime", /*optional=*/true, "the absolute vesting locktime encoded in the script, if any"},
                    }},
                RPCExamples{HelpExampleCli("getstakescript", "\"02...\" 0 1209600")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    std::vector<unsigned char> pubkey_bytes = ParseHexV(request.params[0], "pubkey");
    CPubKey pubkey(pubkey_bytes);
    if (!pubkey.IsFullyValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid public key");
    }
    const bool has_blocks = !request.params[1].isNull();
    const bool has_seconds = !request.params[2].isNull();
    if (has_blocks && has_seconds) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Specify at most one of csv_blocks or csv_seconds");
    }
    uint32_t csv;
    if (has_seconds) {
        int64_t secs = request.params[2].get_int64();
        // Round up to 512-second units; BIP68 time CSV is 16-bit.
        int64_t units = (secs + (1 << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1) >> CTxIn::SEQUENCE_LOCKTIME_GRANULARITY;
        if (units < 1 || units > (int64_t)CTxIn::SEQUENCE_LOCKTIME_MASK) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "csv_seconds out of range (1..33553920 seconds)");
        }
        csv = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | (uint32_t)units;
    } else {
        int64_t blocks = has_blocks ? request.params[1].get_int64() : (int64_t)g_pos_unbonding_period;
        if (blocks < 1 || blocks > (int64_t)CTxIn::SEQUENCE_LOCKTIME_MASK) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "csv_blocks must be between 1 and 65535 (use csv_seconds for longer locks)");
        }
        csv = (uint32_t)blocks;
    }
    auto lock = PosStakeLockSeconds(csv);
    const int64_t required = PosRequiredUnbondingSeconds();
    if (!lock || *lock < required) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("the requested unbonding lock (%d s) is below the chain's minimum (%d s); the output would not count as stake", lock ? *lock : 0, required));
    }
    std::vector<unsigned char> bls_pubkey, bls_pop;
    const bool has_bls = !request.params[3].isNull() || !request.params[4].isNull();
    if (has_bls) {
        if (request.params[3].isNull() || request.params[4].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "blspubkey and pop must be given together");
        }
        bls_pubkey = ParseHexV(request.params[3], "blspubkey");
        bls_pop = ParseHexV(request.params[4], "pop");
        if (bls_pubkey.size() != 48 || bls_pop.size() != 96) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "blspubkey must be 48 bytes and pop 96 bytes (see getblsregistration)");
        }
    }
    int64_t liquid_locktime = 0;
    if (!request.params[5].isNull()) {
        liquid_locktime = request.params[5].get_int64();
        if (liquid_locktime <= 0 || liquid_locktime > 0xffffffffLL) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "liquid_locktime must be between 1 and 4294967295 (a unix time, or a block height below 500000000)");
        }
    }
    CScript script = BuildStakeScript(pubkey, csv, bls_pubkey, bls_pop, liquid_locktime);
    UniValue result(UniValue::VOBJ);
    result.pushKV("script", HexStr(script));
    result.pushKV("csv", (int64_t)csv);
    if (liquid_locktime > 0) result.pushKV("liquid_locktime", liquid_locktime);
    result.pushKV("csv_type", (csv & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) ? "time" : "height");
    result.pushKV("lock_seconds", *lock);
    result.pushKV("min_unbonding_seconds", required);
    return result;
},
    };
}

// SEQUENTIA PoS: the OP_RETURN payload to commit a Sequentia block into the
// parent chain as a long-range-attack checkpoint.
static RPCHelpMan getcheckpointpayload()
{
    return RPCHelpMan{"getcheckpointpayload",
                "\nFor Proof-of-Stake chains: returns the OP_RETURN payload (\"SEQCKPT\" || block hash || height) that "
                "commits a block of this chain into the parent chain as a checkpoint. Embed it in any parent-chain "
                "transaction (e.g. a `data` output of createrawtransaction). Once the commitment is buried "
                "-poscheckpointdepth deep, nodes that have the block on their active chain treat it as finalized "
                "and reject forks below it. See doc/sequentia/04-proof-of-stake.md.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"the chain tip"}, "The block to checkpoint."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "payload", "the checkpoint payload to embed in a parent-chain OP_RETURN"},
                        {RPCResult::Type::STR_HEX, "blockhash", "the committed block"},
                        {RPCResult::Type::NUM, "height", "its height"},
                    }},
                RPCExamples{HelpExampleCli("getcheckpointpayload", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const CBlockIndex* pindex;
    {
        LOCK(cs_main);
        if (request.params[0].isNull()) {
            pindex = chainman.ActiveChain().Tip();
        } else {
            uint256 hash(ParseHashV(request.params[0], "blockhash"));
            pindex = chainman.m_blockman.LookupBlockIndex(hash);
            if (!pindex) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
            if (!chainman.ActiveChain().Contains(pindex)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Block is not on the active chain");
            }
        }
    }
    std::vector<unsigned char> payload = BuildCheckpointPayload(pindex->GetBlockHash(), (uint32_t)pindex->nHeight);
    UniValue result(UniValue::VOBJ);
    result.pushKV("payload", HexStr(payload));
    result.pushKV("blockhash", pindex->GetBlockHash().GetHex());
    result.pushKV("height", pindex->nHeight);
    return result;
},
    };
}

// SEQUENTIA PoS: observed checkpoints and the current finality point.
static RPCHelpMan getcheckpointinfo()
{
    return RPCHelpMan{"getcheckpointinfo",
                "\nFor Proof-of-Stake chains: returns the checkpoints observed on the parent chain and the current "
                "finality point (the highest checkpointed-and-buried block on the active chain).\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "depth", "parent-chain confirmations a commitment needs to finalize"},
                        {RPCResult::Type::NUM, "finalized_height", "height of the finalized block, or -1"},
                        {RPCResult::Type::STR_HEX, "finalized_hash", /*optional=*/true, "hash of the finalized block"},
                        {RPCResult::Type::ARR, "checkpoints", "",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::STR_HEX, "blockhash", "checkpointed block"},
                                        {RPCResult::Type::NUM, "height", "its claimed height"},
                                        {RPCResult::Type::NUM, "btc_height", "parent-chain height of the commitment"},
                                        {RPCResult::Type::STR_HEX, "btc_hash", "parent-chain block containing it"},
                                    }},
                            }},
                        {RPCResult::Type::ARR, "conflicts", "buried checkpoints committing blocks NOT on this node's active chain at heights it has passed — a long-range-fork alarm",
                            {
                                {RPCResult::Type::ELISION, "", "same fields as checkpoints[]"},
                            }},
                        {RPCResult::Type::ARR, "configured", "operator-configured static checkpoints (-poscheckpoint)",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::NUM, "height", "the pinned height"},
                                        {RPCResult::Type::STR_HEX, "blockhash", "the required block hash at that height"},
                                    }},
                            }},
                    }},
                RPCExamples{HelpExampleCli("getcheckpointinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) {
        throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("depth", gArgs.GetIntArg("-poscheckpointdepth", DEFAULT_POS_CHECKPOINT_DEPTH));
    int fin_height = -1;
    uint256 fin_hash;
    if (GetPosFinalizedCheckpoint(fin_height, fin_hash)) {
        result.pushKV("finalized_height", fin_height);
        result.pushKV("finalized_hash", fin_hash.GetHex());
    } else {
        result.pushKV("finalized_height", -1);
    }
    UniValue arr(UniValue::VARR);
    for (const PosCheckpoint& ckpt : GetPosCheckpoints()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("blockhash", ckpt.seq_hash.GetHex());
        entry.pushKV("height", (int64_t)ckpt.seq_height);
        entry.pushKV("btc_height", ckpt.btc_height);
        entry.pushKV("btc_hash", ckpt.btc_hash.GetHex());
        arr.push_back(entry);
    }
    result.pushKV("checkpoints", arr);
    // Long-range-fork alarm: buried checkpoints whose blocks we do not have
    // at heights our chain already passed.
    UniValue conflicts(UniValue::VARR);
    for (const PosCheckpoint& ckpt : GetPosCheckpointConflicts()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("blockhash", ckpt.seq_hash.GetHex());
        entry.pushKV("height", (int64_t)ckpt.seq_height);
        entry.pushKV("btc_height", ckpt.btc_height);
        entry.pushKV("btc_hash", ckpt.btc_hash.GetHex());
        conflicts.push_back(entry);
    }
    result.pushKV("conflicts", conflicts);
    UniValue configured(UniValue::VARR);
    for (const auto& [height, hash] : GetConfiguredPosCheckpoints()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("height", height);
        entry.pushKV("blockhash", hash.GetHex());
        configured.push_back(entry);
    }
    result.pushKV("configured", configured);
    return result;
},
    };
}

void RegisterBlockchainRPCCommands(CRPCTable &t)
{
// clang-format off

static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "blockchain",         &getblockchaininfo,                  },
    { "blockchain",         &getchaintxstats,                    },
    { "blockchain",         &getblockstats,                      },
    { "blockchain",         &getbestblockhash,                   },
    { "blockchain",         &getblockcount,                      },
    { "blockchain",         &getblock,                           },
    { "blockchain",         &getblockfrompeer,                   },
    { "blockchain",         &getblockhash,                       },
    { "blockchain",         &getblockheader,                     },
    { "blockchain",         &getchaintips,                       },
    { "blockchain",         &getdifficulty,                      },
    { "blockchain",         &getdeploymentinfo,                  },
    { "blockchain",         &getmempoolancestors,                },
    { "blockchain",         &getmempooldescendants,              },
    { "blockchain",         &getmempoolentry,                    },
    { "blockchain",         &getmempoolinfo,                     },
    { "blockchain",         &getrawmempool,                      },
    { "blockchain",         &gettxout,                           },
    { "blockchain",         &gettxoutsetinfo,                    },
    { "blockchain",         &pruneblockchain,                    },
    { "blockchain",         &savemempool,                        },
    { "blockchain",         &verifychain,                        },

    { "blockchain",         &preciousblock,                      },
    { "blockchain",         &scantxoutset,                       },
    { "blockchain",         &getblockfilter,                     },

    // ELEMENTS:
    { "blockchain",         &getsidechaininfo,                   },

    // SEQUENTIA:
    { "blockchain",         &getanchorstatus,                    },
    { "blockchain",         &getposschedule,                     },
    { "blockchain",         &getposslot,                         },
    { "blockchain",         &getposrecentblocks,                 },
    { "blockchain",         &getstakerinfo,                      },
    { "blockchain",         &startposproducer,                   },
    { "blockchain",         &getstakescript,                     },
    { "blockchain",         &getdelegationscript,                },
    { "blockchain",         &getdelegationinfo,                  },
    { "blockchain",         &getpayoutscript,                    },
    { "blockchain",         &getpayoutinfo,                      },
    { "blockchain",         &getcheckpointpayload,               },
    { "blockchain",         &getcheckpointinfo,                  },

    /* Not shown in help */
    { "hidden",              &invalidateblock,                   },
    { "hidden",              &reconsiderblock,                   },
    { "hidden",              &waitfornewblock,                   },
    { "hidden",              &waitforblock,                      },
    { "hidden",              &waitforblockheight,                },
    { "hidden",              &syncwithvalidationinterfacequeue,  },
    { "hidden",              &dumptxoutset,                      },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
