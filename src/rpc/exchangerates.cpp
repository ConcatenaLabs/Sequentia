// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetsdir.h>
#include <exchangerates.h>
#include <referenceprices.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <txmempool.h>
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
                "\nSet the whitelist of assets and their exchange rates, for use by the mempool when valuating fee payments.\n",
                {
                    {"rates", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                        {
                            {"asset", RPCArg::Type::NUM, RPCArg::Optional::NO, CreateExchangeRatesDescription()}
                        },
                    },
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
    if (!exchangeRateMap.LoadFromJSON(jsonRates, errors)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Error loading rates from JSON: %s", MakeUnorderedList(errors)));
    }
    if (!exchangeRateMap.SaveToJSONFile(errors)) {
        throw JSONRPCError(RPC_DATABASE_ERROR, strprintf("Error saving exchange rates to JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors)));
    };
    EnsureAnyMemPool(request.context).RecomputeFees();
    return NullUniValue;
}
    };
}

static RPCHelpMan setdynamicfeerates()
{
    return RPCHelpMan{"setdynamicfeerates",
                "\nDEPRECATED alias for setfeeexchangerates intended for a price-server sidecar: it replaces the single "
                "fee-asset whitelist at runtime but, unlike setfeeexchangerates, does NOT persist to exchangerates.json "
                "(a running sidecar re-pushes after a restart). The node keeps ONE whitelist; there are no static/dynamic layers.\n",
                {
                    {"rates", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "",
                        {
                            {"asset", RPCArg::Type::NUM, RPCArg::Optional::NO, CreateExchangeRatesDescription()}
                        },
                    },
                    {"source", RPCArg::Type::STR, RPCArg::Default{"price-server"}, "Identifier of the publisher, recorded per entry."},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("setdynamicfeerates", "{\"b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23\": 100000000}")
                  + HelpExampleRpc("setdynamicfeerates", "{\"b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23\": 100000000}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue json = request.params[0].get_obj();
    std::map<std::string, UniValue> jsonRates;
    json.getObjMap(jsonRates);
    std::map<CAsset, CAmount> rates;
    std::vector<std::string> errors;
    for (const auto& rate : jsonRates) {
        CAsset asset = GetAssetFromString(rate.first);
        if (asset.IsNull()) {
            errors.push_back(strprintf("Unknown label and invalid asset hex: %s", rate.first));
            continue;
        }
        if (!rate.second.isNum()) {
            errors.push_back(strprintf("Rate for %s is not an integer", rate.first));
            continue;
        }
        CAmount value = rate.second.get_int64();
        // Rates are non-negative integers (atoms of the asset per reference unit).
        // 0 means "refuse this asset": ConvertAmountToValue treats scaled_value <= 0
        // as "not accepted" (no divide-by-zero). A negative rate is never valid (it
        // would saturate Convert and bypass fee valuation).
        if (value < 0) {
            errors.push_back(strprintf("Rate for %s must be a non-negative integer (0 = refuse the asset)", rate.first));
            continue;
        }
        rates[asset] = value;
    }
    if (!errors.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Error parsing dynamic rates: %s", MakeUnorderedList(errors)));
    }
    (void)request.params[1]; // 'source' accepted for backward compatibility, no longer recorded
    ExchangeRateMap::GetInstance().SetRates(rates);
    EnsureAnyMemPool(request.context).RecomputeFees();
    return NullUniValue;
}
    };
}

static RPCHelpMan getdynamicfeerates()
{
    return RPCHelpMan{"getdynamicfeerates",
                "\nDEPRECATED alias for getfeeexchangerates (the node keeps a single whitelist). Returns asset -> rate.\n",
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
                    HelpExampleCli("getdynamicfeerates", "")
                  + HelpExampleRpc("getdynamicfeerates", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        return ExchangeRateMap::GetInstance().ToJSON();
    }
    };
}

static RPCHelpMan cleardynamicfeerates()
{
    return RPCHelpMan{"cleardynamicfeerates",
                "\nEmpty the fee-asset whitelist (only the policy asset's 1:1 default remains). DEPRECATED alias; "
                "the node keeps a single whitelist with no separate dynamic layer.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("cleardynamicfeerates", "")
                  + HelpExampleRpc("cleardynamicfeerates", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ExchangeRateMap::GetInstance().ClearRates();
    EnsureAnyMemPool(request.context).RecomputeFees();
    return NullUniValue;
}
    };
}

static RPCHelpMan getfeeacceptancepolicy()
{
    return RPCHelpMan{"getfeeacceptancepolicy",
                "\nGet the fee-asset acceptance policy: every asset currently accepted for fee payment and its exchange rate.\n",
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

void RegisterExchangeRatesRPCCommands(CRPCTable &t)
{
// clang-format off

static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "exchangerates",      &getfeeexchangerates,                  },
    { "exchangerates",      &getreferenceprices,                   },
    { "exchangerates",      &setfeeexchangerates,                  },
    { "exchangerates",      &setdynamicfeerates,                   },
    { "exchangerates",      &getdynamicfeerates,                   },
    { "exchangerates",      &cleardynamicfeerates,                 },
    { "exchangerates",      &getfeeacceptancepolicy,               },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
