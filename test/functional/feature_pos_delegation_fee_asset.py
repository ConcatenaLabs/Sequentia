#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A delegation record pays its own fee in its own asset, at the node's rate.

Re-pointing and reclaiming a delegation take their fee out of the record,
which holds the policy asset. The node prices fees in reference fee units, so
the fee has to be converted into the policy asset at the node's exchange rate.
Paid unconverted, it is right only while the policy asset is worth exactly one
reference unit -- the default on regtest, and never the case on the public
testnet, where a re-pointing was refused with "min relay fee not met".

A refused broadcast must also leave the record free: a change that never left
the node is abandoned, so the delegation can be re-pointed again.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

UNBONDING = 5
COIN = 100_000_000
STAKE = 100  # SEQ
# The policy asset worth less than a reference unit, as on the public testnet.
POLICY_RATE = 17_525_620


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    return wif, k.get_pubkey().get_bytes().hex()


class PosDelegationFeeAssetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()
        self.base_args = [
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=%d" % UNBONDING,
            "-posslotinterval=1",
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, COIN),
            "-validatepegin=0",
            "-txindex=1",
        ]
        self.extra_args = [self.base_args]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.a_wif)

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        ctrl = w0.getaddressinfo(w0.getnewaddress())["pubkey"]
        w0.registerstake(ctrl, STAKE)
        self.mine(1)
        pool1 = w0.getaddressinfo(w0.getnewaddress())["pubkey"]
        pool2 = w0.getaddressinfo(w0.getnewaddress())["pubkey"]

        self.log.info("The policy asset is worth less than a reference fee unit")
        n0.setfeeexchangerates({"bitcoin": POLICY_RATE})

        w0.delegatestake(pool1)
        self.mine(1)
        assert_equal(n0.getdelegationinfo(), {ctrl: pool1})

        self.log.info("Re-pointing pays a fee the mempool accepts")
        res = w0.delegatestake(pool2)
        rot = n0.getrawtransaction(res["txid"], True)
        assert res["txid"] in n0.getrawmempool()
        assert_equal(len(rot["vin"]), 1)
        self.mine(1)
        assert_equal(n0.getdelegationinfo(), {ctrl: pool2})

        self.log.info("A change the node refuses to broadcast leaves the record free")
        self.restart_node(0, self.base_args + ["-maxtxfee=0.00001"])
        n0.setfeeexchangerates({"bitcoin": POLICY_RATE})
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        assert_raises_rpc_error(-4, "nothing was sent", w0.delegatestake, pool1)
        row = w0.listdelegations()[0]
        assert_equal(row["record"]["changing"], False)
        assert "changing_txid" not in row["record"]
        assert not any("waiting to confirm" in a for a in row["alerts"])

        self.restart_node(0, self.base_args)
        n0.setfeeexchangerates({"bitcoin": POLICY_RATE})
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        w0.delegatestake(pool1)
        self.mine(1)
        assert_equal(n0.getdelegationinfo(), {ctrl: pool1})


if __name__ == '__main__':
    PosDelegationFeeAssetTest().main()
