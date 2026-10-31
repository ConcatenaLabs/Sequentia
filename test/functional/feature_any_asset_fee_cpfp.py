#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A child's fee counts for its parent by value, whatever asset each pays in.

CPFP (child pays for parent) is how a wallet speeds up a transaction it cannot
replace: it spends one of the stuck transaction's outputs in a child that pays
enough for both. On an open fee market the parent and the child may pay their
fees in different assets, so the producer must add the two fees by their value
in the reference unit, not by their atoms, when it ranks the pair for a block.

The node is started with a block floor (-blockmintxfee) ten times its relay
floor, so a transaction can sit in the mempool without ever being mined, which
is what "stuck" means. Then:

  - a parent paying its fee in an issued asset, below the block floor, is left
    out of a block; a child paying in another asset, enough for both, takes the
    parent into the next block with it;
  - the mempool's package figures (the parent's descendant fees, the child's
    ancestor fees) are sums of values, and stay so when a rate changes;
  - a coin of an asset no node accepts for fees is sped up by a child that adds
    a second input in an accepted asset to pay the fee; the same child paying
    its fee in the coin's own asset is refused;
  - a parent whose fee asset stops being accepted leaves the mempool with its
    child: a child cannot keep alive a parent the node will not mine.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

GASSET_RATE = 100000000
# The fee asset of the parents is worth half a gasset: the same value takes
# twice its atoms, so a sum of atoms and a sum of values differ.
ASSET_RATE = 50000000
# Reference atoms per vB.
RELAY_FLOOR = Decimal('0.1')
BLOCK_FLOOR = Decimal('1')


class AnyAssetFeeCpfpTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-defaultpeggedassetname=gasset",
            "-anyonecanspendaremine=1",
            "-txindex=1",
            "-minrelaytxfee=%s" % (RELAY_FLOOR / 100000),
            "-mintxfee=%s" % (RELAY_FLOOR / 100000),
            "-blockmintxfee=%s" % (BLOCK_FLOOR / 100000),
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_rates(self, asset_rate=ASSET_RATE):
        self.node.setfeeexchangerates({'gasset': GASSET_RATE, self.asset: asset_rate}, False)

    def send(self, inputs, outputs_asset, amount, fee_asset, fee_rate):
        """A transaction paying `amount` of `outputs_asset` to the wallet, its
        fee in `fee_asset` at `fee_rate` reference atoms per vB. `inputs` are
        spent first; the wallet adds what else the payment and the fee need."""
        to = self.node.getnewaddress()
        raw = self.node.createrawtransaction(
            inputs=inputs,
            outputs=[{to: amount, 'asset': outputs_asset}, {'fee': 0, 'fee_asset': fee_asset}])
        funded = self.node.fundrawtransaction(
            hexstring=raw, options={'fee_rate': fee_rate, 'fee_asset': fee_asset, 'add_inputs': True})['hex']
        signed = self.node.signrawtransactionwithwallet(funded)['hex']
        tx = self.node.decoderawtransaction(signed)
        vout = next(o['n'] for o in tx['vout']
                    if o.get('scriptPubKey', {}).get('address') == to)
        return signed, tx, vout

    def broadcast(self, signed):
        return self.node.sendrawtransaction(signed)

    def init(self):
        self.node = self.nodes[0]
        self.generate(self.node, COINBASE_MATURITY + 1)
        self.gasset = self.node.dumpassetlabels()['gasset']
        self.asset = self.node.issueasset(
            assetamount=Decimal('1000'), tokenamount=0, blind=False, fee_asset='gasset')['asset']
        # Never priced: no node takes it for fees.
        self.unpriced = self.node.issueasset(
            assetamount=Decimal('1000'), tokenamount=0, blind=False, fee_asset='gasset')['asset']
        self.generate(self.node, 1)
        self.set_rates()
        assert_equal(self.node.getrawmempool(), [])

    def test_child_pays_in_another_asset(self):
        self.log.info("A parent below the block floor is left out of a block")
        parent_hex, parent, vout = self.send([], self.asset, Decimal('5'), self.asset, RELAY_FLOOR * 3)
        parent_id = self.broadcast(parent_hex)
        self.generate(self.node, 2)
        assert parent_id in self.node.getrawmempool(), "the parent pays below the block floor"
        e = self.node.getmempoolentry(parent_id)
        assert_equal(e['fees']['asset'], self.asset)
        # Half a gasset an atom: the value is half the atoms.
        assert_equal(e['fees']['value'] * 2, e['fees']['base'])
        parent_value = e['fees']['value']
        parent_vsize = e['vsize']
        assert_greater_than(BLOCK_FLOOR * parent_vsize / 100000000, parent_value)

        self.log.info("A child paying in gasset, enough for both, takes it into the next block")
        child_hex, child, _ = self.send(
            [{'txid': parent_id, 'vout': vout}], self.asset, Decimal('4.9'), self.gasset, BLOCK_FLOOR * 6)
        # The child spends the parent's coin of the asset and adds gasset for the fee.
        assert {i['txid'] for i in child['vin']} - {parent_id}, "a second input pays the fee"
        child_id = self.broadcast(child_hex)
        pe = self.node.getmempoolentry(parent_id)
        ce = self.node.getmempoolentry(child_id)
        assert_equal(ce['fees']['asset'], self.gasset)
        child_value = ce['fees']['value']
        # The package figures add values, not atoms.
        assert_equal(pe['fees']['descendant'], parent_value + child_value)
        assert_equal(ce['fees']['ancestor'], parent_value + child_value)
        package_rate = (parent_value + child_value) * 100000000 / (parent_vsize + ce['vsize'])
        assert_greater_than(package_rate, BLOCK_FLOOR)
        self.log.info("parent %s: %s rfa in %d vB; child %s: %s rfa in %d vB; package %.3f rfa/vB",
                      parent_id[:16], parent_value * 100000000, parent_vsize,
                      child_id[:16], child_value * 100000000, ce['vsize'], package_rate)

        self.log.info("A rate change keeps the package figures in values")
        self.set_rates(ASSET_RATE * 2)
        pe = self.node.getmempoolentry(parent_id)
        ce = self.node.getmempoolentry(child_id)
        assert_equal(pe['fees']['value'], parent_value * 2)
        assert_equal(pe['fees']['descendant'], parent_value * 2 + child_value)
        assert_equal(ce['fees']['ancestor'], parent_value * 2 + child_value)
        self.set_rates()

        block = self.node.getblock(self.generate(self.node, 1)[0])
        assert parent_id in block['tx'] and child_id in block['tx'], "both are mined together"
        assert_equal(self.node.getrawmempool(), [])

    def test_unpriced_coin_sped_up_by_a_second_input(self):
        self.log.info("A coin of an asset no node takes for fees, its parent stuck")
        parent_hex, parent, vout = self.send([], self.unpriced, Decimal('7'), self.gasset, RELAY_FLOOR * 3)
        parent_id = self.broadcast(parent_hex)
        self.generate(self.node, 1)
        assert parent_id in self.node.getrawmempool()

        self.log.info("A child paying its fee in the coin's own asset is refused")
        to = self.node.getnewaddress()
        raw = self.node.createrawtransaction(
            inputs=[{'txid': parent_id, 'vout': vout}],
            outputs=[{to: Decimal('6.99'), 'asset': self.unpriced}, {'fee': Decimal('0.01'), 'fee_asset': self.unpriced}])
        bad = self.node.signrawtransactionwithwallet(raw)['hex']
        assert_raises_rpc_error(-26, "min relay fee not met", self.node.sendrawtransaction, bad)

        self.log.info("A second input in an accepted asset pays the fee instead")
        child_hex, child, _ = self.send(
            [{'txid': parent_id, 'vout': vout}], self.unpriced, Decimal('7'), self.gasset, BLOCK_FLOOR * 6)
        spent = {(i['txid'], i['vout']) for i in child['vin']}
        assert (parent_id, vout) in spent
        assert_greater_than(len(spent), 1)
        assert_equal(list(child['fee'].keys()), [self.gasset])
        child_id = self.broadcast(child_hex)
        block = self.node.getblock(self.generate(self.node, 1)[0])
        assert parent_id in block['tx'] and child_id in block['tx'], "both are mined together"

    def test_unaccepted_parent_leaves_with_its_child(self):
        self.log.info("A parent whose fee asset stops being accepted leaves with its child")
        parent_hex, parent, vout = self.send([], self.asset, Decimal('3'), self.asset, RELAY_FLOOR * 3)
        parent_id = self.broadcast(parent_hex)
        child_hex, _, _ = self.send(
            [{'txid': parent_id, 'vout': vout}], self.asset, Decimal('2.9'), self.gasset, BLOCK_FLOOR * 6)
        child_id = self.broadcast(child_hex)
        assert_equal(set(self.node.getrawmempool()), {parent_id, child_id})
        self.set_rates(0)
        assert_equal(self.node.getrawmempool(), [])
        self.set_rates()

    def run_test(self):
        self.init()
        self.test_child_pays_in_another_asset()
        self.test_unpriced_coin_sped_up_by_a_second_input()
        self.test_unaccepted_parent_leaves_with_its_child()


if __name__ == '__main__':
    AnyAssetFeeCpfpTest().main()
