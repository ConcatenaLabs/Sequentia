// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetsdir.h>
#include <exchangerates.h>
#include <feeassets.h>
#include <feepriceserver.h>
#include <referenceprices.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/time.h>

using node::NodeContext;

static std::string CreateExchangeRatesDescription() {
    return "A key-value pair. The key (string) is the asset hex, the value (integer) represents how many atoms of "
           "the asset are equal to " + strprintf("1 %s or %d %ss", CURRENCY_UNIT, COIN, CURRENCY_ATOM_FULL) + ".";
}

static RPCHelpMan getfeeexchangerates()
{
    return RPCHelpMan{"getfeeexchangerates",
                "\nGet the whitelist of assets and their current exchange rates, for use by the mempool when valuating fee payments.\n",
                {},
                {
                    RPCResult{"rates", RPCResult::Type::OBJ, "", "",
                        {
                            RPCResult{RPCResult::Type::NUM, "asset", CreateExchangeRatesDescription()},
                            RPCResult{RPCResult::Type::ELISION, "", ""}
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getfeeexchangerates", "")
                  + HelpExampleRpc("getfeeexchangerates", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        return ExchangeRateMap::GetInstance().ToJSON();
    }
    };
}

static RPCHelpMan setfeeexchangerates()
{
    return RPCHelpMan{"setfeeexchangerates",
                "\nSet the whitelist of assets and their exchange rates the mempool uses when valuating fee payments.\n"
                "The given set replaces the whole whitelist. An asset left out of the set is not accepted, exactly\n"
                "as if it were listed at rate 0, and that applies to every asset including the fee/policy asset:\n"
                "the reference unit is an abstract factor, never a token, so no asset is valued by default. Passing\n"
                "{} therefore leaves this node accepting NO fee asset at all, which will empty its mempool.\n"
                "\nThere is a single whitelist. It is static when written by hand, and dynamic when a price server\n"
                "updates it. By default the set persists to " + exchange_rates_config_file + " so a static\n"
                "whitelist survives a restart. A price server that pushes to this node should pass persist=false:\n"
                "it re-pushes every poll, so persisting would only churn the file and, worse, leave a dead price\n"
                "server's last rates in force across a restart instead of the static whitelist.\n"
                "\nIf this node is connected to a price server (getfeepriceserver), that server is the whitelist's\n"
                "only writer, so this call disconnects it, for good: the node records that in settings.json and\n"
                "from then on the whitelist is what is written to it. setfeepriceserver connects it again.\n",
                {
                    {"rates", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "",
                        {
                            {"asset", RPCArg::Type::NUM, RPCArg::Optional::NO, CreateExchangeRatesDescription()}
                        },
                    },
                    {"persist", RPCArg::Type::BOOL, RPCArg::Default{true}, "Write the whitelist to " + exchange_rates_config_file + " so it survives a restart. Pass false for automated (price-server) pushes that are re-sent each poll."},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("setfeeexchangerates", "{\"b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23\": 100000000}")
                  + HelpExampleRpc("setfeeexchangerates", "{\"b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23\": 100000000}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue json = request.params[0].get_obj();
    std::map<std::string, UniValue> jsonRates;
    json.getObjMap(jsonRates);
    auto& exchangeRateMap = ExchangeRateMap::GetInstance();
    std::vector<std::string> errors;
    std::map<CAsset, CAmount> rates;
    // Checked before anything changes, so a mistyped list neither writes
    // nor disconnects the node from its price server.
    if (!ExchangeRateMap::ParseJSON(jsonRates, rates, errors)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Error loading rates from JSON: %s", MakeUnorderedList(errors)));
    }
    const bool persist = request.params[1].isNull() ? true : request.params[1].get_bool();
    // One writer: anyone writing the whitelist ends the node's connection to
    // a price server before the write lands, so a list in flight from that
    // server can never overwrite this one.
    NoteOperatorFeeListWrite(persist ? "hand (setfeeexchangerates)"
                                     : "a price server pushing to this node (setfeeexchangerates, persist=false)");
    exchangeRateMap.SetRates(rates);
    if (persist && !exchangeRateMap.SaveToJSONFile(errors)) {
        throw JSONRPCError(RPC_DATABASE_ERROR, strprintf("Error saving exchange rates to JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors)));
    };
    EnsureAnyMemPool(request.context).RecomputeFees();
    return NullUniValue;
}
    };
}

static RPCHelpMan getfeeacceptancepolicy()
{
    return RPCHelpMan{"getfeeacceptancepolicy",
                "\nGet the fee-asset acceptance policy: every asset currently accepted for fee payment and its exchange rate.\n"
                "This is the complete accept set. There is no implied entry: an asset absent from the result is not\n"
                "accepted, and an asset present with rate 0 is explicitly refused.\n",
                {},
                {
                    RPCResult{"policy", RPCResult::Type::OBJ, "", "",
                        {
                            RPCResult{RPCResult::Type::OBJ, "asset", "",
                                {
                                    RPCResult{RPCResult::Type::NUM, "rate", CreateExchangeRatesDescription()},
                                }
                            },
                            RPCResult{RPCResult::Type::ELISION, "", ""}
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getfeeacceptancepolicy", "")
                  + HelpExampleRpc("getfeeacceptancepolicy", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        return ExchangeRateMap::GetInstance().AcceptancePolicyToJSON();
    }
    };
}

static UniValue FeeAssetInfoToJSON(const FeeAssetInfo& info)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("asset", info.asset.GetHex());
    entry.pushKV("label", info.identifier);
    entry.pushKV("precision", (int)info.precision);
    entry.pushKV("listed", info.listed);
    entry.pushKV("accepted", info.accepted);
    if (info.listed) entry.pushKV("rate", info.rate);
    entry.pushKV("registry_listed", info.registry_listed);
    if (info.has_market_price) entry.pushKV("market_price", UniValue(info.market_price));
    return entry;
}

static RPCHelpMan getfeeassetinfo()
{
    return RPCHelpMan{"getfeeassetinfo",
                "\nEverything needed to judge whether a fee can be paid in an asset: whether this node\n"
                "accepts it, whether the Asset Registry publishes it, and whether the reference price\n"
                "feed quotes it. The three are independent and only the first is decisive.\n"
                "\n\"accepted\" is this node's own answer: false means this node's mempool refuses the\n"
                "transaction outright, so it never even reaches a block producer. \"registry_listed\"\n"
                "and \"market_price\" are about what OTHER nodes are likely to do — price servers\n"
                "discover assets from the registry and price them from a feed — so an accepted asset\n"
                "that is neither published nor quoted still risks confirming nowhere but here.\n"
                "\nWith no argument, reports every asset in the whitelist plus every asset the node's\n"
                "asset directory knows.\n",
                {
                    {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Label or hex id of a single asset to report on."},
                },
                RPCResult{RPCResult::Type::OBJ_DYN, "", "keyed by asset hex id",
                    {
                        {RPCResult::Type::OBJ, "asset_id", "",
                            {
                                {RPCResult::Type::STR_HEX, "asset", "The asset's hex id"},
                                {RPCResult::Type::STR, "label", "The asset's label, or its hex id when it has none"},
                                {RPCResult::Type::NUM, "precision", "Decimal places"},
                                {RPCResult::Type::BOOL, "listed", "Present in this node's fee whitelist, whatever the rate"},
                                {RPCResult::Type::BOOL, "accepted", "Listed AND priced above zero: this node values a fee in it"},
                                {RPCResult::Type::NUM, "rate", /*optional=*/true, CreateExchangeRatesDescription() + " Only present when listed; 0 means explicitly refused."},
                                {RPCResult::Type::BOOL, "registry_listed", "Published by the Asset Registry this node reads"},
                                {RPCResult::Type::NUM, "market_price", /*optional=*/true, "Price of one whole unit in the reference feed's base unit (USD today). Absent when unquoted."},
                            }
                        },
                    }
                },
                RPCExamples{
                    HelpExampleCli("getfeeassetinfo", "")
                  + HelpExampleCli("getfeeassetinfo", "USDX")
                  + HelpExampleRpc("getfeeassetinfo", "\"USDX\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        UniValue out(UniValue::VOBJ);
        if (!request.params[0].isNull()) {
            const std::string assetstr = request.params[0].get_str();
            const CAsset asset = GetAssetFromString(assetstr);
            if (asset.IsNull()) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown label and invalid asset hex: %s", assetstr));
            }
            // An asset nothing knows about is a legitimate answer here, not an
            // error: "no, this node does not accept it" is exactly what the
            // caller asked, and refusing to say so would make the caller guess.
            out.pushKV(asset.GetHex(), FeeAssetInfoToJSON(GetFeeAssetInfo(asset)));
            return out;
        }
        for (const FeeAssetInfo& info : GetAllFeeAssetInfo()) {
            out.pushKV(info.asset.GetHex(), FeeAssetInfoToJSON(info));
        }
        return out;
    }
    };
}

static RPCHelpMan getreferenceprices()
{
    return RPCHelpMan{"getreferenceprices",
                "\nGet the cached per-asset USD reference prices. DISPLAY ONLY: the node GUI uses these to\n"
                "value amounts in a user-chosen reference currency (USD, BTC, or any priced asset). This\n"
                "never affects consensus, fees or the mempool. Empty unless -referencepricesurl is set.\n",
                {},
                {
                    RPCResult{"prices", RPCResult::Type::OBJ, "", "",
                        {
                            RPCResult{RPCResult::Type::NUM, "TICKER", "USD price of one whole unit of the asset"},
                            RPCResult{RPCResult::Type::ELISION, "", ""}
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getreferenceprices", "")
                  + HelpExampleRpc("getreferenceprices", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        UniValue out(UniValue::VOBJ);
        for (const auto& [ticker, price] : GetReferencePrices()) {
            out.pushKV(ticker, UniValue(price));
        }
        return out;
    }
    };
}

static UniValue FeePriceServerToJSON()
{
    const FeeListSource src = GetFeeListSource();
    UniValue out(UniValue::VOBJ);
    out.pushKV("connected", src.connected);
    out.pushKV("whitelist", src.connected ? "dynamic" : "static");
    if (src.connected) {
        out.pushKV("url", src.url);
        out.pushKV("default", src.by_default);
        out.pushKV("has_list", src.have_list);
        out.pushKV("list_written", src.last_written);
        out.pushKV("list_published", src.list_published);
        out.pushKV("last_attempt", src.last_attempt);
        out.pushKV("last_error", src.last_error);
    } else if (!src.ended.empty()) {
        out.pushKV("disconnected_because", src.ended);
    }
    out.pushKV("network_price_server", DefaultFeePriceServer(gArgs));
    out.pushKV("assets", (int64_t)ExchangeRateMap::GetInstance().GetRates().size());
    return out;
}

static const std::vector<RPCResult> FEE_PRICE_SERVER_RESULT{
    {RPCResult::Type::BOOL, "connected", "Whether this node is connected to a price server, which is then its whitelist's only writer"},
    {RPCResult::Type::STR, "whitelist", "\"dynamic\" when connected to a price server; \"static\" otherwise, meaning written by hand, or by a price server pushing to this node over setfeeexchangerates"},
    {RPCResult::Type::STR, "url", /*optional=*/true, "The address of the price server's published list"},
    {RPCResult::Type::BOOL, "default", /*optional=*/true, "Whether the address is the network's default rather than one the operator chose"},
    {RPCResult::Type::BOOL, "has_list", /*optional=*/true, "Whether a list from this price server is in force"},
    {RPCResult::Type::NUM_TIME, "list_written", /*optional=*/true, "When the list in force was taken from the price server (" + UNIX_EPOCH_TIME + "), 0 if never"},
    {RPCResult::Type::NUM_TIME, "list_published", /*optional=*/true, "When the price server says it published that list (" + UNIX_EPOCH_TIME + ")"},
    {RPCResult::Type::NUM_TIME, "last_attempt", /*optional=*/true, "When this node last asked the price server (" + UNIX_EPOCH_TIME + ")"},
    {RPCResult::Type::STR, "last_error", /*optional=*/true, "Why the last answer was refused or did not arrive; empty after a list was taken. The list in force is kept meanwhile"},
    {RPCResult::Type::STR, "disconnected_because", /*optional=*/true, "Why the node disconnected from its price server, when it did since it started"},
    {RPCResult::Type::STR, "network_price_server", "The network's price server for this chain (empty where there is none, as on the mainnet)"},
    {RPCResult::Type::NUM, "assets", "How many assets the whitelist lists"},
};

static RPCHelpMan getfeepriceserver()
{
    return RPCHelpMan{"getfeepriceserver",
                "\nWhether this node is connected to a price server, which one, and when its fee whitelist was last written.\n"
                "A whitelist is static when written by hand, and dynamic when a price server updates it.\n",
                {},
                RPCResult{RPCResult::Type::OBJ, "", "", FEE_PRICE_SERVER_RESULT},
                RPCExamples{
                    HelpExampleCli("getfeepriceserver", "")
                  + HelpExampleRpc("getfeepriceserver", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        return FeePriceServerToJSON();
    }
    };
}

static RPCHelpMan setfeepriceserver()
{
    return RPCHelpMan{"setfeepriceserver",
                "\nConnect this node to a price server, its operator's own or another operator's, or disconnect it.\n"
                "Connected, the node's fee whitelist is dynamic: the price server is its only writer, and the node\n"
                "takes the list it publishes now and every -feepriceserverpoll seconds. The node asks first and\n"
                "connects only if the answer is a price server's published list; a price feed API's answer, or\n"
                "anything else, is refused and changes nothing.\n"
                "\nAn empty address disconnects: the list in force is kept, as a static whitelist saved to\n"
                + exchange_rates_config_file + ". Either way the choice is recorded in settings.json.\n",
                {
                    {"url", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the price server's published list (http only), e.g. its /api/whitelist; \"\" to disconnect"},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", FEE_PRICE_SERVER_RESULT},
                RPCExamples{
                    HelpExampleCli("setfeepriceserver", "\"http://sequentiatestnet.com/feewhitelist\"")
                  + HelpExampleCli("setfeepriceserver", "\"\"")
                  + HelpExampleRpc("setfeepriceserver", "\"http://127.0.0.1:8090/api/whitelist\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        std::string error;
        if (!SetFeePriceServer(request.params[0].get_str(), error)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
        }
        EnsureAnyMemPool(request.context).RecomputeFees();
        return FeePriceServerToJSON();
    }
    };
}

void RegisterExchangeRatesRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "exchangerates",      &getfeeexchangerates,                  },
    { "exchangerates",      &getreferenceprices,                   },
    { "exchangerates",      &setfeeexchangerates,                  },
    { "exchangerates",      &getfeepriceserver,                    },
    { "exchangerates",      &setfeepriceserver,                    },
    { "exchangerates",      &getfeeacceptancepolicy,               },
    { "exchangerates",      &getfeeassetinfo,                      },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
