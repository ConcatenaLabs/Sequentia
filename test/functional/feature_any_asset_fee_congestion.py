#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Block space is auctioned by fee VALUE, across assets, and getmempoolcongestion reports the price.

The claim the open fee market rests on is that a producer with a full block takes
the transactions that pay it the most, and that "the most" is comparable between
fees denominated in different assets. Nothing tested that, because nothing filled
a block: every existing fee test has room for everything it sends.

So this one shrinks the block until the queue does not fit, and then checks two
things that would both be invisible in an uncongested chain:

  - the winners are chosen by value, not by atom count. An asset priced at half a
    gasset needs twice as many atoms to pay the same value, so raw amounts rank
    the assets in the wrong order, and a node that compared them would auction
    block space by an asset's denomination rather than by what was paid.
  - getmempoolcongestion names the price of entry: what the cheapest transaction
    that still fits is paying. That figure is the whole point of the RPC, and it
    is only meaningful once the block is actually full -- while there is room, the
    honest answer is the relay floor, not an invented competition.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)

GASSET = 'b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23'
# The issued asset is priced at half a gasset, so one unit of value costs two of
# its atoms. Every "same value, different atoms" assertion below turns on this.
ASSET_RATE = 50000000
GASSET_RATE = 100000000


class AnyAssetFeeCongestionTest(BitcoinTestFramework):
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
            # Low, or the wallet's own floor would raise every fee above the rates
            # the test sets and flatten the ordering it is trying to observe.
            "-minrelaytxfee=0.00000001",
            "-blockmintxfee=0",
        ]]
        # The queue is shrunk by shrinking the block, which is a startup option --
        # so the funding happens first, on full-size blocks, and the node is
        # restarted with this before anything is measured. Funding under the small
        # limit would congest the setup itself.
        # 4000 of this is held back for the coinbase, leaving room for about one
        # transaction of the size this test builds -- fine enough that the order
        # of preference is visible block by block rather than lumped together.
        self.small_block_args = ["-blockmaxweight=6000"]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------ helpers

    def send_at_rate(self, asset, fee_rate, amount=Decimal('0.001')):
        """One transaction paying `fee_rate` reference atoms per vB, in `asset`.

        fee_rate is denominated in the REFERENCE unit whatever the fee asset, so
        two transactions given the same fee_rate pay the same value and differ
        only in how many atoms of their own asset that took.

        `asset` must be a 64-hex id: createrawtransaction takes no labels.
        """
        raw = self.node.createrawtransaction(outputs=[
            {self.node.getnewaddress(): amount, 'asset': asset},
            {'fee': 0, 'fee_asset': asset}])
        funded = self.node.fundrawtransaction(
            hexstring=raw, options={'fee_rate': fee_rate, 'fee_asset': asset})['hex']
        signed = self.node.signrawtransactionwithwallet(funded)['hex']
        txid = self.node.sendrawtransaction(signed)
        tx = self.node.decoderawtransaction(signed)
        return txid, tx['fee'][asset]

    def set_rates(self):
        self.node.setfeeexchangerates({'gasset': GASSET_RATE, self.asset: ASSET_RATE}, False)

    def drain(self, limit=40):
        """Generate until the queue is empty. With a block this small that takes
        several, and leaving anything behind would leak into the next assertion."""
        for _ in range(limit):
            if not self.node.getrawmempool():
                return
            self.generate(self.node, 1)
        assert_equal(self.node.getrawmempool(), [])

    # -------------------------------------------------------------------- setup

    def init(self):
        self.node = self.nodes[0]
        self.generate(self.node, COINBASE_MATURITY + 1)

        issuance = self.node.issueasset(
            assetamount=Decimal('1000'), tokenamount=1, blind=False, fee_asset='gasset')
        self.asset = issuance['asset']
        self.generate(self.node, 1)

        self.set_rates()

        # Independent inputs, so the transactions below are siblings rather than a
        # chain: a chain would be ranked as one package and the ordering under
        # test would be the package's, not each transaction's.
        for asset in ('gasset', self.asset):
            for _ in range(3):
                addresses = [self.node.getnewaddress() for _ in range(10)]
                self.node.sendmany(
                    dummy="",
                    amounts={a: Decimal('1') for a in addresses},
                    output_assets={a: asset for a in addresses},
                    fee_asset='gasset')
                self.generate(self.node, 1)
        self.drain()

        # Only now does the block get small.
        self.restart_node(0, extra_args=self.extra_args[0] + self.small_block_args)
        self.node = self.nodes[0]
        assert_equal(self.node.getrawmempool(), [])
        # The rates went with the restart, which is what persist=false means: a
        # price server re-pushes every poll, so its rates deliberately do not
        # outlive the node. Push them again rather than persisting, so that this
        # test exercises the same path a price server uses.
        self.set_rates()

    # -------------------------------------------------------------------- tests

    def test_quiet_mempool_reports_the_floor(self):
        """With room to spare there is no auction, and the RPC must not invent one."""
        assert_equal(self.node.getrawmempool(), [])
        c = self.node.getmempoolcongestion()
        assert_equal(c['size'], 0)
        assert_equal(c['next_block_full'], False)
        assert_equal(c['next_block_txs'], 0)
        assert_greater_than(1.0, c['backlog_blocks'])
        # No competition: the price of entry is just the floor to be relayed.
        assert_equal(c['next_block_min_feerate'], c['mempoolminfee'])

        # One cheap transaction still does not fill a block.
        self.send_at_rate(GASSET,2)
        c = self.node.getmempoolcongestion()
        assert_equal(c['size'], 1)
        assert_equal(c['next_block_txs'], 1)
        assert_equal(c['next_block_full'], False)
        assert_equal(c['next_block_min_feerate'], c['mempoolminfee'])
        self.drain()

    def mine_until_empty(self, limit=40):
        """Drain the queue a block at a time, returning {txid: which block it made}.

        Which block a transaction lands in is the observable form of "the producer
        preferred it", and unlike asserting membership of one particular block it
        does not depend on how many transactions happen to fit in each.
        """
        landed = {}
        for height in range(limit):
            if not self.node.getrawmempool():
                return landed
            block = self.node.getblock(self.generate(self.node, 1)[0])
            for txid in block['tx'][1:]:  # [0] is the coinbase
                landed.setdefault(txid, height)
        assert_equal(self.node.getrawmempool(), [])
        return landed

    def test_value_beats_atoms(self):
        """Two transactions, same atoms of fee, different value. Value wins."""
        # The issued asset is worth half a gasset, so `asset` at rate 5 and
        # `gasset` at rate 10 pay the SAME number of atoms -- and the gasset one
        # is worth twice as much. A node ranking by atoms would call them equal,
        # and one ranking by the asset's own denomination would get them backwards.
        cheap_txid, cheap_atoms = self.send_at_rate(self.asset, 5)
        rich_txid, rich_atoms = self.send_at_rate(GASSET,10)
        assert_equal(cheap_atoms, rich_atoms)  # the trap: identical amounts

        # The valuation itself, stated exactly. This is the mechanism the ordering
        # rests on, and unlike which block a transaction lands in it does not
        # depend on how many happen to fit: same atoms, half the value, because
        # the asset is worth half as much.
        cheap_fees = self.node.getmempoolentry(cheap_txid)['fees']
        rich_fees = self.node.getmempoolentry(rich_txid)['fees']
        assert_equal(cheap_fees['base'], rich_fees['base'])
        assert_equal(rich_fees['value'], 2 * cheap_fees['value'])
        assert_equal(rich_fees['asset'], GASSET)
        assert_equal(cheap_fees['asset'], self.asset)

        # Enough on top to guarantee the queue outlasts a block, so that there is
        # a preference to observe at all.
        for _ in range(6):
            self.send_at_rate(GASSET,50)

        c = self.node.getmempoolcongestion()
        assert_equal(c['next_block_full'], True)
        assert_greater_than(c['backlog_blocks'], 1.0)

        # And the end-to-end consequence: the producer takes the richer one first.
        landed = self.mine_until_empty()
        assert rich_txid in landed and cheap_txid in landed
        assert_greater_than(landed[cheap_txid], landed[rich_txid])

    def test_price_of_entry(self):
        """A full block makes the RPC quote the cheapest rate that still fits."""
        # A spread of rates, so the cut lands strictly inside it and the answer is
        # neither the floor nor the top bid.
        for rate in (100, 80, 60, 40, 20, 10, 5, 3):
            self.send_at_rate(GASSET,rate)
            self.send_at_rate(self.asset, rate)

        c = self.node.getmempoolcongestion()
        assert_equal(c['next_block_full'], True)
        assert_greater_than(c['size'], c['next_block_txs'])
        # The projection must respect the room the block actually has: the weight
        # limit less what the assembler holds back for the coinbase.
        assert_greater_than_or_equal(6000 - 4000, c['next_block_weight'])

        # The quoted price is a real cut, well above the floor that would apply if
        # nothing were competing. (It can be as high as the top bid -- when only
        # one transaction fits, the cheapest that fits IS the best bid -- so this
        # deliberately does not assert it is below the top.)
        cut = c['next_block_min_atoms_per_kvb']
        assert_greater_than(cut, int(c['mempoolminfee'] * Decimal(10**8)))
        assert_greater_than(cut, 3 * 1000)  # above the 3 atoms/vB bottom bid

        # The point of the number is that a wallet can act on it. Bid above it and
        # the next block takes you; the queue's cheapest transaction still waits.
        cheapest = min(self.node.getrawmempool(),
                       key=lambda t: self.node.getmempoolentry(t)['fees']['value'])
        winner, _ = self.send_at_rate(GASSET, 2 * (cut // 1000) + 1)
        block_txs = self.node.getblock(self.generate(self.node, 1)[0])['tx']
        assert winner in block_txs, "a bid above the quoted price was left out"
        assert cheapest not in block_txs, "the cheapest transaction was mined anyway"

        self.drain()

    def test_fee_asset_conversion(self):
        """The quoted price converted into an asset must use the whitelist rate."""
        self.send_at_rate(GASSET,40)
        base = self.node.getmempoolcongestion()
        in_gasset = self.node.getmempoolcongestion('gasset')
        in_asset = self.node.getmempoolcongestion(self.asset)

        assert_equal(in_gasset['accepted'], True)
        assert_equal(in_gasset['asset'], GASSET)
        # gasset is one reference unit per unit, so the figures coincide.
        assert_equal(in_gasset['next_block_min_asset_atoms_per_kvb'],
                     base['next_block_min_atoms_per_kvb'])
        # The issued asset is worth half as much, so it takes twice the atoms.
        assert_equal(in_asset['next_block_min_asset_atoms_per_kvb'],
                     2 * base['next_block_min_atoms_per_kvb'])

        # An asset this node refuses gets no conversion rather than a zero.
        self.node.setfeeexchangerates({'gasset': GASSET_RATE}, False)
        refused = self.node.getmempoolcongestion(self.asset)
        assert_equal(refused['accepted'], False)
        assert 'next_block_min_asset_atoms_per_kvb' not in refused
        self.node.setfeeexchangerates({'gasset': GASSET_RATE, self.asset: ASSET_RATE}, False)

        assert_raises_rpc_error(-5, "Unknown label and invalid asset hex: nope",
                                self.node.getmempoolcongestion, 'nope')
        self.generate(self.node, 2)

    def run_test(self):
        self.init()
        self.test_quiet_mempool_reports_the_floor()
        self.test_value_beats_atoms()
        self.test_price_of_entry()
        self.test_fee_asset_conversion()


if __name__ == '__main__':
    AnyAssetFeeCongestionTest().main()
