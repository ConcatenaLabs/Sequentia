#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Staking from a wallet, end to end, without a private key ever leaving it.

Two things the GUI Staking tab relies on, both of which used to fail silently
for descriptor wallets:

 - registerstake on a public-committee chain registers the staker's committee
   BLS key by itself when the wallet holds the staker key. Without that key a
   stake carries weight and can lead rounds, but the committee can never
   certify its blocks, so it produces nothing and the pool board shows it as
   not committee-ready.
 - startstaking turns block production on for the wallet's own staker keys by
   handing them to the node's producer in-process. The old path exported keys
   with dumpprivkey, which descriptor wallets refuse, so their stakes were
   registered and never produced. A running producer takes the keys live; the
   set is persisted for the next restart.

Topology: one node, a config-layer staker (with its BLS registration) running
the autonomous producer so the chain advances, and a descriptor wallet that
stakes on top of it.
"""

import json
import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosWalletStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()   # config-layer producer
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=4", "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-validatepegin=0",
        ]
        # Started once without the BLS spec (it is derived at runtime), then
        # restarted as a producer with it; see setup_network.
        self.extra_args = [self.common + ["-staker=%s:%d" % (self.a_pub, COIN)]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.setup_nodes()
        n0 = self.nodes[0]
        spec = n0.getblsregistration(self.a_wif)["spec"]
        self.producer_args = self.common + [
            "-staker=%s:%d%s" % (self.a_pub, COIN, spec),
            "-posproducer=1", "-posproducerkey=%s" % self.a_wif,
        ]
        self.restart_node(0, extra_args=self.producer_args)

    def run_test(self):
        n0 = self.nodes[0]
        self.wait_until(lambda: n0.getblockcount() >= 2, timeout=60)
        # The framework's default wallet holds the genesis free coins; the
        # wallets under test are fresh descriptor / legacy wallets funded from it.
        funder = n0.get_wallet_rpc(self.default_wallet_name)
        n0.createwallet(wallet_name="w", descriptors=True)
        w = n0.get_wallet_rpc("w")
        funder.sendtoaddress(address=w.getnewaddress(), amount=500, fee_asset_label="bitcoin")
        self.wait_until(lambda: w.getbalance().get("bitcoin", 0) > 200, timeout=60)

        self.log.info("registerstake from a descriptor wallet carries the committee BLS key by itself")
        addr = w.getnewaddress()
        pub = w.getaddressinfo(addr)["pubkey"]
        reg = w.registerstake(pub, 100)
        assert_equal(reg["committee_ready"], True)
        assert_equal(len(reg["blspubkey"]), 96)
        assert "note" not in reg
        self.wait_until(lambda: n0.getstakerinfo().get(pub) == 100 * COIN, timeout=60)
        assert_equal(n0.getstakerinfo(True)[pub]["blspubkey"], reg["blspubkey"])
        pool = n0.listpools(pub)["pools"][0]
        assert_equal(pool["committee_ready"], True)
        assert_equal(pool["eligible"], True)

        self.log.info("a staker key the wallet does not hold gets no BLS key, and says so")
        _, foreign_pub = make_staker()
        reg2 = w.registerstake(foreign_pub, 100)
        assert_equal(reg2["committee_ready"], False)
        assert "blspubkey" not in reg2
        assert "does not hold the staker key" in reg2["note"]

        self.log.info("startstaking hands the wallet's key to the RUNNING producer, live")
        before = n0.startposproducer([self.a_wif])   # a no-op that reports the current state
        assert_equal(before["producing"], True)
        assert_equal(before["added"], 0)
        assert_equal(before["started"], False)
        keys_before = before["keys"]
        res = w.startstaking()
        assert_equal(res["producing"], True)
        assert_equal(res["started"], False)          # it was already running
        assert_equal(res["added"], 1)
        assert_equal(res["keys"], keys_before + 1)
        assert_equal(res["persisted"], True)
        assert_equal(res["pubkeys"], [pub])
        again = w.startstaking([pub])
        assert_equal(again["added"], 0)
        assert_equal(again["keys"], keys_before + 1)
        assert_raises_rpc_error(-4, "does not hold the private key", w.startstaking, [foreign_pub])

        self.log.info("the merged key set is persisted for the next restart")
        settings = os.path.join(n0.datadir, self.chain, "settings.json")
        with open(settings) as f:
            saved = json.load(f)
        assert_equal(saved["posproducer"], True)
        assert_equal(len(saved["posproducerkey"]), keys_before + 1)

        self.log.info("the wallet's key keeps producing after a plain restart")
        h = n0.getblockcount()
        self.restart_node(0, extra_args=self.common + ["-staker=%s:%d%s" % (self.a_pub, COIN, n0.getblsregistration(self.a_wif)["spec"])])
        self.wait_until(lambda: n0.getblockcount() >= h + 2, timeout=60)
        state = n0.startposproducer([self.a_wif])
        assert_equal(state["producing"], True)
        assert_equal(state["keys"], keys_before + 1)

        if self.is_bdb_compiled():
            self.log.info("a legacy wallet gets the same treatment")
            funder = n0.get_wallet_rpc(self.default_wallet_name)   # fresh proxy after the restart
            n0.createwallet(wallet_name="l", descriptors=False)
            lw = n0.get_wallet_rpc("l")
            funder.sendtoaddress(address=lw.getnewaddress(), amount=500, fee_asset_label="bitcoin")
            self.wait_until(lambda: lw.getbalance().get("bitcoin", 0) > 200, timeout=60)
            lpub = lw.getaddressinfo(lw.getnewaddress())["pubkey"]
            lreg = lw.registerstake(lpub, 100)
            assert_equal(lreg["committee_ready"], True)
            lres = lw.startstaking()
            assert_equal(lres["added"], 1)
            assert_equal(lres["pubkeys"], [lpub])


if __name__ == '__main__':
    PosWalletStakingTest().main()
