// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/pos_control.h>

#include <chainparams.h>
#include <key_io.h>
#include <net.h>
#include <node/context.h>
#include <pos.h>
#include <pos_producer.h>
#include <txmempool.h>
#include <util/check.h>
#include <util/settings.h>
#include <univalue.h>
#include <util/system.h>
#include <validation.h>

#include <set>

namespace node {

bool StartPosProducerWithKeys(NodeContext& node, const std::vector<CKey>& keys,
                              interfaces::PosProducerStart& out, std::string& error)
{
    out = interfaces::PosProducerStart{};
    if (!g_con_pos) { error = "Proof-of-Stake (con_pos) is not enabled on this chain"; return false; }

    // De-duplicate by public key, and merge with whatever a running producer
    // already holds, so the PERSISTED set never drops a key this node was
    // already producing with.
    std::set<CPubKey> seen;
    std::vector<std::string> all_wifs;
    std::vector<CKey> new_keys;
    const bool already_running = (node.pos_producer != nullptr);
    if (already_running) {
        for (const CKey& k : node.pos_producer->Keys()) {
            if (seen.insert(k.GetPubKey()).second) all_wifs.push_back(EncodeSecret(k));
        }
    }
    for (const CKey& key : keys) {
        if (!key.IsValid()) { error = "Invalid staker private key"; return false; }
        if (seen.insert(key.GetPubKey()).second) { all_wifs.push_back(EncodeSecret(key)); new_keys.push_back(key); }
    }
    if (all_wifs.empty()) { error = "Provide at least one staker private key"; return false; }

    // A running producer takes the new keys live: net_processing reads the
    // active producer pointer locklessly on the message thread, so it must
    // never be rebuilt, but adding keys under the producer's key lock is safe
    // and lets a stake registered from the wallet produce without a restart.
    if (!already_running) {
        node.pos_producer = std::make_unique<PosProducer>(*Assert(node.chainman), *Assert(node.mempool), Params(),
                                                          Assert(node.connman.get()), new_keys);
        node.pos_producer->Start();
        out.started = true;
        out.added = (int)new_keys.size();
    } else {
        out.added = (int)node.pos_producer->AddKeys(new_keys);
    }

    // Reflect into the live args so status readers (GUI overview / staking page)
    // update, and persist to settings.json so the existing startup path
    // (AppInitMain: -posproducer + -posproducerkey) resumes production after a
    // restart with no manual config editing.
    gArgs.ForceSetArg("-posproducer", "1");
    {
        util::SettingsValue wif_arr(util::SettingsValue::VARR);
        for (const std::string& w : all_wifs) wif_arr.push_back(w);
        gArgs.LockSettings([&](util::Settings& settings) {
            settings.rw_settings["posproducer"] = true;
            settings.rw_settings["posproducerkey"] = wif_arr;
        });
        out.persisted = gArgs.WriteSettingsFile();
    }
    out.producing = node.pos_producer != nullptr;
    out.keys = node.pos_producer ? (int)node.pos_producer->Keys().size() : 0;
    return true;
}

} // namespace node
