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
                "\nSet the whitelist of assets and their exchange rates the mempool uses when valuating fee payments.\n"
                "The given set replaces the whole whitelist (pass {} to clear it).\n"
                "\nThere is a single whitelist; \"static\" versus \"dynamic\" is only how it is operated, not a\n"
                "protocol distinction. By default the set persists to " + exchange_rates_config_file + " so a\n"
                "hand-configured (static) whitelist survives a restart. A price server that drives the whitelist\n"
                "automatically (dynamic operation) should pass persist=false: it re-pushes every poll, so persisting\n"
                "would only churn the file and, worse, leave a dead price server's last rates in force across a\n"
                "restart instead of failing back to the persisted static whitelist.\n",
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
    if (!exchangeRateMap.LoadFromJSON(jsonRates, errors)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Error loading rates from JSON: %s", MakeUnorderedList(errors)));
    }
    const bool persist = request.params[1].isNull() ? true : request.params[1].get_bool();
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
    { "exchangerates",      &getfeeacceptancepolicy,               },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
