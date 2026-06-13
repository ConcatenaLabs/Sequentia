// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetsdir.h>
#include <exchangerates.h>
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
                "\nReplace the dynamic layer of the fee-asset whitelist, typically called by a locally run price server. "
                "Statically configured rates (setfeeexchangerates / exchangerates.json) always take precedence over dynamic "
                "rates for the same asset. Dynamic rates are dropped once older than -dynfeeratemaxage (if set).\n",
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
        // Rates are positive integers (atoms of the asset per reference unit);
        // a zero or negative rate would crash or bypass fee valuation.
        if (value <= 0) {
            errors.push_back(strprintf("Rate for %s must be a positive integer", rate.first));
            continue;
        }
        rates[asset] = value;
    }
    if (!errors.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Error parsing dynamic rates: %s", MakeUnorderedList(errors)));
    }
    std::string source = request.params[1].isNull() ? "price-server" : request.params[1].get_str();
    ExchangeRateMap::GetInstance().SetDynamicRates(rates, source, GetTime());
    EnsureAnyMemPool(request.context).RecomputeFees();
    return NullUniValue;
}
    };
}

static RPCHelpMan getdynamicfeerates()
{
    return RPCHelpMan{"getdynamicfeerates",
                "\nGet the dynamic layer of the fee-asset whitelist, with per-entry source and freshness metadata.\n",
                {},
                {
                    RPCResult{"rates", RPCResult::Type::OBJ, "", "",
                        {
                            RPCResult{RPCResult::Type::OBJ, "asset", "",
                                {
                                    RPCResult{RPCResult::Type::NUM, "rate", CreateExchangeRatesDescription()},
                                    RPCResult{RPCResult::Type::STR, "source", "Publisher identifier"},
                                    RPCResult{RPCResult::Type::NUM, "updated_at", "Unix time of last update"},
                                    RPCResult{RPCResult::Type::NUM, "age", "Seconds since last update"},
                                    RPCResult{RPCResult::Type::BOOL, "stale", "Whether the entry exceeds -dynfeeratemaxage"},
                                }
                            },
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
        return ExchangeRateMap::GetInstance().DynamicToJSON();
    }
    };
}

static RPCHelpMan cleardynamicfeerates()
{
    return RPCHelpMan{"cleardynamicfeerates",
                "\nDrop all dynamic fee-asset rates (e.g. on price server shutdown). Statically configured rates are unaffected.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("cleardynamicfeerates", "")
                  + HelpExampleRpc("cleardynamicfeerates", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ExchangeRateMap::GetInstance().ClearDynamicRates();
    EnsureAnyMemPool(request.context).RecomputeFees();
    return NullUniValue;
}
    };
}

static RPCHelpMan getfeeacceptancepolicy()
{
    return RPCHelpMan{"getfeeacceptancepolicy",
                "\nGet the effective fee-asset acceptance policy: every asset currently accepted for fee payment, "
                "its exchange rate, and whether the rate originates from the static or dynamic layer.\n",
                {},
                {
                    RPCResult{"policy", RPCResult::Type::OBJ, "", "",
                        {
                            RPCResult{RPCResult::Type::OBJ, "asset", "",
                                {
                                    RPCResult{RPCResult::Type::NUM, "rate", CreateExchangeRatesDescription()},
                                    RPCResult{RPCResult::Type::STR, "origin", "\"static\" or \"dynamic\""},
                                    RPCResult{RPCResult::Type::STR, "source", /*optional=*/true, "Publisher identifier (dynamic entries only)"},
                                    RPCResult{RPCResult::Type::NUM, "updated_at", /*optional=*/true, "Unix time of last update (dynamic entries only)"},
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

void RegisterExchangeRatesRPCCommands(CRPCTable &t)
{
// clang-format off

static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "exchangerates",      &getfeeexchangerates,                  },
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
