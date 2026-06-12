#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests opt-in confidential transactions (-con_default_blinded_addresses=0).

Sequentia inverts Liquid's blinded-by-default behavior: wallets hand out plain
Bitcoin-format addresses by default (so one address can serve both Bitcoin and
Sequentia in wallet apps), and confidential addresses — a visibly distinct
format — are opt-in per call (getnewaddress "" "blech32") or per node
(-blindedaddresses=1). See doc/sequentia/08-addresses-and-ct.md.

node0 runs with the Sequentia default (unblinded); node1 runs with an explicit
-blindedaddresses=1 override as the blinded control. (The test framework writes
blindedaddresses=0 into every node's config, so the historical chain-level
blinded *default* cannot be observed in-framework; it is covered by the
chain-parameter default in src/chainparams.cpp and was verified on live nodes.)
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class CtOptInTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = [
            "-con_blocksubsidy=5000000000",
            "-validatepegin=0",
            "-txindex=1",
        ]
        # node0: Sequentia behavior (CT opt-in); node1: blinded control.
        self.extra_args = [
            common + ["-con_default_blinded_addresses=0"],
            common + ["-blindedaddresses=1"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # --- node0 unblinded by default; node1 blinded (explicit override) ---
        a0 = n0.getnewaddress()
        info0 = n0.getaddressinfo(a0)
        assert_equal(info0['confidential_key'], '')
        assert a0.startswith('ert1')  # the plain bech32 form, not blech32

        a1 = n1.getnewaddress()
        info1 = n1.getaddressinfo(a1)
        assert info1['confidential_key'] != ''
        assert a1.startswith('el1')  # confidential blech32 form

        # getrawchangeaddress follows the same default
        c0 = n0.getaddressinfo(n0.getrawchangeaddress())
        assert_equal(c0['confidential_key'], '')
        c1 = n1.getaddressinfo(n1.getrawchangeaddress())
        assert c1['confidential_key'] != ''

        # --- Per-call opt-in on the unblinded-default node ---
        blinded = n0.getnewaddress("", "blech32")
        binfo = n0.getaddressinfo(blinded)
        assert binfo['confidential_key'] != ''
        assert blinded.startswith('el1')
        # And its unconfidential form is a plain address again
        assert binfo['unconfidential'].startswith('ert1')

        # --- Send semantics follow the destination ---
        # Fund node0 (its own coinbases)
        self.generatetoaddress(n0, 101, n0.getnewaddress(), sync_fun=self.no_op)

        # Payment to a plain address: explicit (public) value on the output
        plain_txid = n0.sendtoaddress(n0.getnewaddress(), 1.0)
        plain = n0.getrawtransaction(plain_txid, True)
        plain_outs = [o for o in plain['vout'] if o.get('value') == Decimal('1.00000000')]
        assert_equal(len(plain_outs), 1)
        assert 'valuecommitment' not in plain_outs[0]

        # Payment to the opt-in confidential address: blinded output
        ct_txid = n0.sendtoaddress(blinded, 1.0)
        ct = n0.getrawtransaction(ct_txid, True)
        blinded_outs = [o for o in ct['vout'] if 'valuecommitment' in o]
        assert len(blinded_outs) >= 1
        # The blinded payment confirms and is received in full
        self.generatetoaddress(n0, 1, n0.getnewaddress(), sync_fun=self.no_op)
        assert_equal(n0.getreceivedbyaddress(blinded)['bitcoin'], Decimal('1.00000000'))

        # --- Node-level opt-in: restarting node0 with -blindedaddresses=1
        # restores blind-by-default without changing the chain ---
        self.restart_node(0, extra_args=self.extra_args[0] + ["-blindedaddresses=1"])
        forced = n0.getaddressinfo(n0.getnewaddress())
        assert forced['confidential_key'] != ''


if __name__ == '__main__':
    CtOptInTest().main()
