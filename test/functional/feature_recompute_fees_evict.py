#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regression test for CTxMemPool::RecomputeFees() (open fee market).

De-pricing a non-policy fee asset (setfeeexchangerates rate -> 0) while the
mempool holds several transactions that pay their fee in that asset must:
  * NOT crash the daemon, and
  * evict exactly those transactions (their reference fee value drops to 0,
    below the relay minimum that gated their entry), leaving policy-asset
    transactions untouched.

Historically RecomputeFees() iterated the boost multi_index `mapTx` by value
while calling mapTx.modify(...) on entries inside the same loop (the current
entry plus its ancestors/descendants). Mutating a multi_index mid-iteration is
undefined behaviour and silently killed producer nodes. See
src/txmempool.cpp::RecomputeFees().
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)


class RecomputeFeesEvictTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-defaultpeggedassetname=gasset",
            "-walletrbf=1",
            "-minrelaytxfee=0.0000001",
            "-anyonecanspendaremine=1",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generatetoaddress(node, COINBASE_MATURITY + 1, node.getnewaddress())

        gasset = node.dumpassetlabels()['gasset']

        # Issue a test asset and confirm it.
        issuance = node.issueasset(Decimal('1000'), 1, False)
        asset = issuance['asset']
        self.generate(node, 1)

        # Price both assets so fee-in-asset transactions are admitted.
        rates = {"gasset": 100000000, asset: 100000000}
        node.setfeeexchangerates(rates)
        assert_equal(node.getfeeexchangerates(), rates)

        addr = node.getnewaddress()

        # A control transaction whose fee is paid in the policy asset (gasset).
        # It must survive the de-pricing untouched. Built FIRST on purpose:
        # admitting a transaction runs IsStandardTx, and an earlier bug there
        # (CTransaction::GetFeeAsset clobbering the ::policyAsset global with the
        # tx's fee asset) made the *last* admitted fee asset masquerade as the
        # policy asset. By admitting the policy-asset tx first and the
        # non-policy-fee txs last, ::policyAsset (if corruptible) would be left
        # pointing at the asset we de-price -- which is exactly the state under
        # which RecomputeFees fails to evict. A correct node evicts regardless.
        gasset_txid = node.sendtoaddress(
            address=addr,
            amount=Decimal('0.1'),
            assetlabel='gasset',
            fee_asset_label='gasset')

        # Several transactions pay their fee in the (priced) non-policy asset;
        # a chain (unconfirmed parent -> child) creates an ancestor/descendant
        # relationship so RecomputeFees() exercises its ancestor and descendant
        # modify() paths while mutating mapTx.
        asset_fee_txids = []
        for _ in range(5):
            txid = node.sendtoaddress(
                address=addr,
                amount=Decimal('0.1'),
                assetlabel=asset,
                fee_asset_label=asset)
            asset_fee_txids.append(txid)

        unspent = next(u for u in node.listunspent(0)
                       if u['asset'] == asset and u['amount'] > Decimal('1'))
        raw = node.createrawtransaction(
            [{"txid": unspent['txid'], "vout": unspent['vout']}],
            [{addr: Decimal('0.2'), 'asset': asset},
             {"fee": Decimal('0.00001'), 'fee_asset': asset},
             {node.getrawchangeaddress(): unspent['amount'] - Decimal('0.2') - Decimal('0.00001'), 'asset': asset}])
        signed = node.signrawtransactionwithwallet(raw)
        child_txid = node.sendrawtransaction(signed['hex'])
        asset_fee_txids.append(child_txid)

        mempool_before = set(node.getrawmempool())
        for txid in asset_fee_txids:
            assert txid in mempool_before, f"{txid} missing from mempool"
        assert gasset_txid in mempool_before
        self.log.info("Mempool holds %d transactions before de-pricing", len(mempool_before))

        height_before = node.getblockcount()

        # De-price the non-policy fee asset. This drives RecomputeFees() over a
        # NON-EMPTY mempool: the historical bug crashed the node here.
        node.setfeeexchangerates({"gasset": 100000000, asset: 0})

        # Node must still be alive and responsive (a crash would make this RPC
        # fail / the connection drop).
        assert_equal(node.getblockcount(), height_before)
        self.log.info("Node survived de-pricing; mempool now: %d txs",
                      len(node.getrawmempool()))

        # Every transaction that paid its fee in the now-unpriced asset must be
        # evicted; the policy-asset transaction must remain.
        mempool_after = set(node.getrawmempool())
        for txid in asset_fee_txids:
            assert txid not in mempool_after, f"{txid} should have been evicted"
        assert gasset_txid in mempool_after, "policy-asset tx must survive"

        # The node continues to function: mine a block and accept new work.
        self.generate(node, 1)
        assert gasset_txid not in node.getrawmempool()
        assert_equal(node.gettransaction(gasset_txid)['confirmations'], 1)
        self.log.info("RecomputeFees evicted unpriced-fee txs and kept the node alive")


if __name__ == '__main__':
    RecomputeFeesEvictTest().main()
