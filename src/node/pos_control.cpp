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
#include <sync.h>
#include <txmempool.h>
#include <util/check.h>
#include <util/settings.h>
#include <univalue.h>
#include <util/system.h>
#include <validation.h>

#include <set>

namespace node {

//! Serializes every runtime start (the startposproducer RPC and the wallet's
//! startstaking can run at once on different RPC threads), from reading
//! whether a producer runs to persisting the key set. Without it, two calls
//! could each see no producer and each build one; the one that lost its place
//! in the node context kept running, unstopped, past shutdown.
static Mutex g_pos_control_mutex;

bool StartPosProducerWithKeys(NodeContext& node, const std::vector<CKey>& keys,
                              interfaces::PosProducerStart& out, std::string& error)
{
    LOCK(g_pos_control_mutex);
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

    // A running producer takes the new keys live rather than being rebuilt (a
    // rebuild would drop the round state the gossip has built): adding keys
    // under the producer's key lock is safe and lets a stake registered from
    // the wallet produce without a restart.
    if (!already_running) {
        auto producer = std::make_shared<PosProducer>(*Assert(node.chainman), *Assert(node.mempool), Params(),
                                                      Assert(node.connman.get()), new_keys);
        if (!producer->Start()) { error = "Another proof-of-stake producer is already running"; return false; }
        node.pos_producer = std::move(producer);
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
