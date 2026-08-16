#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""SEQUENTIA: a supervised asset must be issuable with ZERO supply.

This is not a corner case, it is how a bridged stablecoin has to be issued.
Circle's Bridged USDC Standard wants supply that is exactly backed from the
first atom, so the bridge creates the asset with no units at all and exactly one
reissuance token, then mints against verified deposits. Compages does precisely
that (`assetamount: 0, tokenamount: 1`).

The risk this pins down: the supervised-issuance rule requires the issuance
amount to be EXPLICIT, which is there to keep a supervised asset out of blinded
outputs. A zero asset amount is encoded as a NULL amount rather than an explicit
zero, and null is not explicit, so a rule written carelessly forbids the one
issuance shape the bridge needs.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.key import ECKey, compute_xonly_pubkey
from test_framework.util import assert_equal
from decimal import Decimal


def make_key(seed):
    key = ECKey()
    key.set(seed.to_bytes(32, "big"), True)
    assert key.is_valid
    xonly, _ = compute_xonly_pubkey(key.get_bytes())
    return xonly.hex()


class SupervisedZeroSupplyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-con_default_blinded_addresses=0",
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-anyonecanspendaremine=1",
            "-txindex=1",
            "-supervisedassetsheight=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        policy = node.dumpassetlabels()["bitcoin"]

        op_pub = make_key(0xAAAA)
        rec_pub = make_key(0xBBBB)

        self.log.info("Issuing a supervised asset with zero supply, as a bridge does")
        r = node.issueasset(0, 1, False, None, policy, 6, None,
                            {"operationalkey": op_pub, "recoverykey": rec_pub, "pause": True})
        assert_equal(r["supervised"], True)
        self.generate(node, 1)

        entry = [a for a in node.getsupervisedassets() if a["asset"] == r["asset"]]
        assert_equal(len(entry), 1)
        assert_equal(entry[0]["pauseallowed"], True)
        self.log.info("  asset %s issued with no units and one reissuance token", r["asset"])

        # The reissuance token exists and is spendable, which is what makes the
        # supply mintable later against a verified deposit.
        assert_equal(node.getbalance()[r["token"]], Decimal("1"))
        assert r["asset"] not in node.getbalance()

        self.log.info("Reissuing against it, the way a deposit does")
        node.reissueasset(r["asset"], 250, policy)
        self.generate(node, 1)
        assert_equal(node.getbalance()[r["asset"]], Decimal("250"))
        # Still supervised, and still the same asset.
        assert_equal(len([a for a in node.getsupervisedassets() if a["asset"] == r["asset"]]), 1)


if __name__ == '__main__':
    SupervisedZeroSupplyTest().main()
