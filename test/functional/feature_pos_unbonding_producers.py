#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A committee of autonomous producers crosses the two-step-unbonding
activation height with an old-form withdrawal in every mempool.

A staking output spent straight to an address is valid below -posunbondheight
and invalid from it. Such a withdrawal admitted below the height and still
resident when the tip reaches the block before it would be carried by every
producer's template into the first block under the rule; each template would
fail validation, every producer would skip its slot, and the chain would stop
at that height. Connecting the block before the height evicts it instead.

Here the withdrawal T is admitted well below the height but held out of every
template (-blockmintxfee) until each producer's tip is the block before the
height, standing in for a withdrawal that arrives just after that block's
proposal was built. No validity rule is touched by holding it back.

Committee of 4 autonomous BLS producers (nodes 0-3) plus a wallet node (4),
all with the debug categories off, as a default node runs.
"""
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.util import assert_equal

H = 20
UNBONDING = 5
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosUnbondingProducersTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(4)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-posblockspacing=3", "-posblockspacingheight=1",
            "-con_max_block_sig_size=4000", "-posunbonding=%d" % UNBONDING,
            "-posunbondheight=%d" % H,
            "-signblockscript=51", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1",
            "-anyonecanspendaremine=1", "-validatepegin=0", "-par=1", "-debug=none",
        ]
        common += ["-staker=%s:%d" % (pub, 10**6 * COIN) for _, pub in self.stakers]
        self.extra_args = [common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0], "-blockmintxfee=0.001"]
                           for i in range(4)]
        self.extra_args.append(list(common))

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        for a in range(5):
            for b in range(a + 1, 5):
                self.connect_nodes(a, b)

    def run_test(self):
        prod = self.nodes[:4]
        wn = self.nodes[4]
        self.wait_until(lambda: wn.getblockcount() >= 2, timeout=120)
        for wname in wn.listwallets():
            wn.unloadwallet(wname)
        wn.createwallet(wallet_name="legacy", descriptors=False)
        wn.rescanblockchain(0)
        pub = wn.getaddressinfo(wn.getnewaddress())["pubkey"]
        R = wn.registerstake(pub, 100)
        R = R["txid"] if isinstance(R, dict) else R
        for n in prod:
            n.prioritisetransaction(R, 0, 10**8)   # mine the registration despite -blockmintxfee
        self.wait_until(lambda: any(s["pubkey"] == pub and s["withdrawable"] for s in wn.liststakeutxos()), timeout=300)
        assert wn.getblockcount() <= H - 3, "stake matured too late for this test"

        self.log.info("Tip %d: a one-step withdrawal, admitted everywhere and held out of every template", wn.getblockcount())
        r = wn.withdrawstake(pub)
        assert "unbonding" not in r, r
        T = r["txid"]
        self.wait_until(lambda: all(T in n.getrawmempool() for n in prod), timeout=60)

        self.log.info("Each producer reaching H-1 = %d takes it out of its mempool", H - 1)
        pending = set(range(4))
        deadline = time.time() + 300
        while pending and time.time() < deadline:
            for i in list(pending):
                if prod[i].getblockcount() >= H - 1:
                    prod[i].prioritisetransaction(T, 0, 10**8)   # would put it into the next template
                    pending.discard(i)
            time.sleep(0.05)
        assert not pending

        self.log.info("The chain advances through the activation height")
        self.wait_until(lambda: all(n.getblockcount() >= H + 2 for n in self.nodes), timeout=120)
        self.sync_blocks()
        assert all(T not in n.getrawmempool() for n in self.nodes)
        for i, n in enumerate(self.nodes):
            with open(n.debug_log_path, encoding="utf-8") as f:
                log = f.read()
            assert "Evicting %s from the mempool: bad-unbond-required from height %d" % (T, H) in log, i
            assert "bad-unbond-required in tx" not in log, i
        assert_equal(wn.listunbonding()["active"], True)


if __name__ == '__main__':
    PosUnbondingProducersTest().main()
