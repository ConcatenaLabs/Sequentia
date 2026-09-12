#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The standard fee-market acceptance suite: what gets confirmed, and by whom.

This is the test a change to fee policy, block assembly, the exchange-rate map or
the wallet's fee selection has to survive. It exists because every one of the
questions below has the same, useless answer on an idle chain -- yes, at the relay
floor -- and only becomes observable once the next block is full:

  1. an asset no producer prices                -> never confirms, anywhere
  2. a listed asset bidding under the wall      -> waits, and confirms in the gap
  3. above the relay floor, under the producer's own floor -> never confirms there
  4. RBF that raises the bid over the wall      -> confirms in the next block
  5. an asset only ONE producer prices          -> only that producer takes it,
                                                   and the other still accepts the block
  6. an asset both price, one higher            -> the richer valuation decides
  7. CPFP, same fee asset                       -> the child drags the parent in
  8. RBF switching fee asset                    -> value is what counts, not atoms
  9. CPFP switching fee asset                   -> same, through a package

The pair (5) and (6) carry the claim the whole open fee market rests on: which
assets a node accepts fees in is POLICY, not consensus (see the comment above the
single-fee-asset check in MemPoolAccept::PreChecks). Two producers may disagree
about what a fee is worth, or whether it is a fee at all, and must still agree on
the chain. A regression there does not show up as a rejected transaction; it shows
up as a fork, so (5) asserts the disagreeing node accepts the other's block.

HOW THE WALL IS BUILT. Blocks are shrunk with -blockmaxweight rather than filled
with real load: 8000 leaves 4000 of weight, about 1 kvB, for transactions, so a
handful of them is a full block and the auction is visible with no load on the
machine at all. The suite measures the size of its own filler transaction at
runtime and derives how many fit, so it does not hard-code a capacity that a
change in transaction shape would silently invalidate.

doc/sequentia/fee-market-testing.md explains the suite, and how much of it can be
reproduced against a shared testnet.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import BIP125_SEQUENCE_NUMBER
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

GASSET = 'b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23'

# Rates are "what one whole unit is worth", scaled by 1e8: the reference unit is
# an abstract numeraire, so 100000000 means one unit of the asset is worth one
# reference unit. Nothing is 1:1 by fiat, the policy asset included.
RATE_ONE = 100000000

# Fee rates below are in reference atoms per vB, which is what fundrawtransaction's
# fee_rate means on this chain (CURRENCY_ATOM is "rfa" under ANY_ASSET_FEES). They
# are values, not amounts: two transactions given the same fee_rate pay the same
# WORTH however many atoms of their own asset that takes.
WALL_RATE = 100        # what every filler transaction pays
UNDER_WALL = 30        # comfortably above the relay floor, comfortably under the wall
OVER_WALL = 400        # enough to jump the queue
UNDER_PRODUCER_FLOOR = 3   # above relay, below producer B's -blockmintxfee

# Producer B refuses anything under this, whatever the queue looks like.
# 0.00010000 RFU/kvB = 10000 rfa/kvB = 10 rfa/vB.
PRODUCER_B_FLOOR = '0.00010000'

BLOCK_MAX_WEIGHT = 8000
COINBASE_RESERVED_WEIGHT = 4000
WITNESS_SCALE_FACTOR = 4
USABLE_VSIZE = (BLOCK_MAX_WEIGHT - COINBASE_RESERVED_WEIGHT) // WITNESS_SCALE_FACTOR


class AnyAssetFeeMarketTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        common = [
            "-blindedaddresses=0",
            # A custom chain keeps the Elements default, so without this the
            # wallet hands out a CONFIDENTIAL change destination while the rest
            # of the transaction is explicit, and every raw send dies as
            # "output has nonce, but is not blinded" -- nothing to do with fees.
            "-con_default_blinded_addresses=0",
            "-initialfreecoins=1000000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-defaultpeggedassetname=gasset",
            "-txindex=1",
            # Low, or the wallet's own floor would lift every fee above the rates
            # set here and flatten the ordering the suite is trying to observe.
            "-minrelaytxfee=0.00000001",
        ]
        # node0 is producer A and holds the only funded wallet: it builds every
        # transaction under test. node1 is producer B -- a second opinion about
        # what a fee is worth, and about how little of it is worth having.
        self.extra_args = [
            common + ["-anyonecanspendaremine=1", "-blockmintxfee=0"],
            common + ["-blockmintxfee=%s" % PRODUCER_B_FLOOR],
        ]
        # Shrinking the block is a startup option, so the funding happens first at
        # full size. Funding under the small limit would congest the setup itself.
        self.small_block = ["-blockmaxweight=%d" % BLOCK_MAX_WEIGHT]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------ plumbing

    @property
    def a(self):
        """Producer A, and the wallet every transaction under test is built on."""
        return self.nodes[0]

    @property
    def b(self):
        """Producer B: same chain, different fee policy."""
        return self.nodes[1]

    def rates_a(self, extra=None):
        """Producer A's price list. Rebuilt and re-pushed rather than persisted,
        because that is what a price server does: setfeeexchangerates REPLACES the
        map every poll, and rates deliberately do not outlive a restart."""
        rates = {GASSET: RATE_ONE, self.listed: RATE_ONE // 2, self.split: RATE_ONE // 4}
        rates.update(extra or {})
        self.a.setfeeexchangerates(rates, False)

    def rates_b(self, extra=None):
        # B prices `split` at eight times A's valuation, and is the only one that
        # prices `solo` at all.
        rates = {GASSET: RATE_ONE, self.listed: RATE_ONE // 2,
                 self.split: RATE_ONE * 2, self.solo: RATE_ONE}
        rates.update(extra or {})
        self.b.setfeeexchangerates(rates, False)

    def plain_change(self, node):
        """An explicit change destination the wallet still recognises as change.

        Both halves matter. A receive address (getnewaddress) is not change, so
        bumpfee later cannot shrink that output and gives up with "no change
        destination provided for asset ..."; and the raw change address is
        confidential, which an otherwise explicit transaction cannot carry.
        """
        return node.getaddressinfo(node.getrawchangeaddress())['unconfidential']

    def take_confirmed(self, asset):
        """One CONFIRMED unspent output of `asset`, never handed out twice.

        Every transaction here must stand alone. Left to choose for itself the
        wallet will happily spend its own unconfirmed change, and a filler that
        does that becomes the CHILD of the transaction under test -- which the
        mempool then scores as one package and drags the under-bidder into a block
        as an accidental CPFP. The suite passed or failed by coin selection.
        """
        # Asked fresh every time, deliberately. A cached list goes stale: bumpfee
        # and the other wallet-driven calls here pick their own inputs, and one of
        # those can be an output still sitting in the cache, so handing it out
        # later builds a transaction on an already-spent input -- which surfaces
        # as bad-txns-inputs-missingorspent in whichever test happens to draw it,
        # intermittently. listunspent already excludes outputs spent in the
        # mempool, so a fresh query is the authoritative answer.
        for u in self.a.listunspent(1, 9999999):
            key = (u['txid'], u['vout'])
            if u['asset'] != asset or key in self.spent or u['amount'] < Decimal('0.5'):
                continue
            # Unblinded only. A blinded input forces the whole transaction to be
            # blinded, and every raw send then fails as "output has nonce, but is
            # not blinded" -- a confidentiality error dressed up as a fee one. The
            # wallet's own change from setup is where they come from, so this
            # cannot be avoided by only creating plain outputs.
            if u.get('amountblinder', '0' * 64) != '0' * 64:
                continue
            self.spent.add(key)
            return {'txid': key[0], 'vout': key[1]}
        raise AssertionError("out of confirmed %s outputs" % asset[:8])

    def build(self, fee_asset, fee_rate, amount=Decimal('0.001'), replaceable=False,
              node=None, spend=None, change_assets=None, funding_assets=None, to=None):
        """A signed, UNBROADCAST transaction paying `fee_rate` reference atoms/vB.

        Returned rather than sent, because half this suite is about what happens
        when the same bytes are offered to two producers who disagree about them.
        """
        node = node or self.a
        inputs = list(spend or [])
        for asset in (funding_assets if funding_assets is not None else [fee_asset]):
            inputs.append(self.take_confirmed(asset))
        if replaceable:
            # fundrawtransaction's `replaceable` only marks the inputs IT adds, and
            # here it adds none: the signalling has to go on the inputs named above
            # or bumpfee later refuses the transaction as not BIP125 replaceable.
            for i in inputs:
                i['sequence'] = BIP125_SEQUENCE_NUMBER
        # Paid OUT of this wallet unless a caller needs the output back. Every
        # payment to self leaves a 0.001 coin behind, and one of those is worth
        # EXACTLY the fee a later bump first estimates -- coin selection takes it as
        # a perfect match, the transaction then grows by an input and a change
        # output, and the bump dies as "Could not cover fee". The suite is
        # congesting a chain, not paying itself.
        raw = node.createrawtransaction(inputs, [
            {to if to is not None else self.sink: amount, 'asset': fee_asset},
            {'fee': 0, 'fee_asset': fee_asset}])
        # add_inputs=False so the funding above is the whole story: an extra input
        # the wallet chose could be unconfirmed, see take_confirmed.
        options = {'fee_rate': fee_rate, 'fee_asset': fee_asset, 'add_inputs': False}
        if replaceable:
            options['replaceable'] = True
        # Name a plain change destination per asset. The wallet's default change
        # address on a custom chain is confidential, and mixing it into an
        # otherwise explicit transaction fails the same way a blinded input does.
        # changeAddress takes a map for exactly the multi-asset case.
        assets = set(change_assets or []) | {fee_asset}
        options['changeAddress'] = {a: self.plain_change(node) for a in assets}
        funded = node.fundrawtransaction(hexstring=raw, options=options)['hex']
        return node.signrawtransactionwithwallet(funded)['hex']

    def txid_of(self, hexstr):
        return self.a.decoderawtransaction(hexstr)['txid']

    def vsize_of(self, hexstr):
        """The DISCOUNT size, which is what the mempool ranks and the assembler
        counts -- not the plain vsize."""
        d = self.a.decoderawtransaction(hexstr)
        return int(d.get('discountvsize', d['vsize']))

    # ------------------------------------------------------------------ the wall

    def top_up(self, vsize_target):
        """Queue filler at the wall rate until the mempool holds `vsize_target` vB.

        Filler is deliberately built from independent inputs: a chain would be
        ranked as one package, and the ordering under test would be the package's
        rather than each transaction's.
        """
        added = 0
        while added < vsize_target:
            hexstr = self.build(GASSET, WALL_RATE)
            self.a.sendrawtransaction(hexstr)
            added += self.vsize_of(hexstr)
        return added

    def mine_full_block(self, node=None):
        """Top the queue past one block, mine, and assert the block really filled.

        Asserting fullness is the point: a test that believes it is measuring an
        auction while the block has room is measuring nothing, and would keep
        passing after the auction stopped working.
        """
        node = node or self.a
        c = node.getmempoolcongestion()
        if c['bytes'] < USABLE_VSIZE * 2:
            self.top_up(USABLE_VSIZE * 2 - c['bytes'])
        assert_equal(node.getmempoolcongestion()['next_block_full'], True)
        block = node.getblock(self.generate(node, 1)[0])
        assert_greater_than(len(block['tx']), 1)
        return block

    def mine_until_it_lands(self, txid, limit=30):
        """Stop topping the queue up and mine until `txid` confirms.

        Returns the block it landed in, and whether that block was going to be
        full before it was mined. Both matter: the under-bidder must confirm (it
        was queued, not dropped) and it must confirm in a block that had ROOM --
        a chain of nothing but full blocks would never take it at all, and real
        congestion comes in waves. This is the quiet part of one.
        """
        for _ in range(limit):
            was_full = self.a.getmempoolcongestion()['next_block_full']
            block = self.a.getblock(self.generate(self.a, 1)[0])
            if txid in block['tx']:
                return block, was_full
        raise AssertionError("%s never confirmed in %d blocks" % (txid, limit))

    def drain(self, limit=60):
        """Empty the queue. Anything left behind leaks into the next assertion."""
        for _ in range(limit):
            if not self.a.getrawmempool():
                self.sync_all()
                return
            self.generate(self.a, 1)
        assert_equal(self.a.getrawmempool(), [])

    # --------------------------------------------------------------------- setup

    def init(self):
        # Outputs already handed out, so take_confirmed never repeats one.
        self.spent = set()
        # Somewhere outside this wallet for filler payments to land.
        self.sink = self.b.getnewaddress()
        self.generate(self.a, COINBASE_MATURITY + 1)

        # Four assets, because the interesting cases are all about disagreement:
        #   listed   -- both producers price it, identically
        #   unlisted -- neither prices it
        #   solo     -- only producer B prices it
        #   split    -- both price it, B eight times higher
        issued = {}
        for name in ('listed', 'unlisted', 'solo', 'split'):
            issued[name] = self.a.issueasset(
                assetamount=Decimal('10000'), tokenamount=1, blind=False,
                fee_asset='gasset')['asset']
            self.generate(self.a, 1)
        self.listed, self.unlisted = issued['listed'], issued['unlisted']
        self.solo, self.split = issued['solo'], issued['split']

        self.rates_a()
        self.rates_b()

        # A working set of small, independent UTXOs. The wall needs many roots
        # rather than one deep chain, and every transaction under test wants an
        # input the wallet can take without dragging others in.
        for asset in (GASSET, self.listed, self.solo, self.split, self.unlisted):
            for _ in range(6):
                addrs = [self.a.getnewaddress() for _ in range(25)]
                self.a.sendmany(dummy="", amounts={x: Decimal('1') for x in addrs},
                                output_assets={x: asset for x in addrs},
                                fee_asset='gasset')
                self.generate(self.a, 1)
        self.drain()

        # Only now does the block get small, on both producers.
        for i in (0, 1):
            self.restart_node(i, extra_args=self.extra_args[i] + self.small_block)
        self.connect_nodes(0, 1)
        self.rates_a()
        self.rates_b()
        assert_equal(self.a.getrawmempool(), [])

        filler = self.build(GASSET, WALL_RATE)
        self.filler_vsize = self.vsize_of(filler)
        self.per_block = USABLE_VSIZE // self.filler_vsize
        self.log.info("Block holds %d vB = about %d transactions of %d vB"
                      % (USABLE_VSIZE, self.per_block, self.filler_vsize))
        assert_greater_than(self.per_block, 1)

    # --------------------------------------------------------------------- tests

    def test_1_unpriced_asset_never_confirms(self):
        """An asset no producer prices is not cheap: it is not a fee at all.

        The wallet can build it -- its own price list said the asset was fine --
        and every producer values the fee at zero, so it never even reaches a
        mempool. This is the failure mode an operator will actually hit, because
        the wallet and the producers get their prices from different servers.
        """
        self.log.info("1. fee in an asset nobody prices -> never confirmed")
        # Build it while A still prices the asset, exactly as a wallet with a more
        # generous price server would.
        self.rates_a(extra={self.unlisted: RATE_ONE})
        hexstr = self.build(self.unlisted, OVER_WALL)
        self.rates_a()  # the producers' real price list: no such asset

        for node, who in ((self.a, 'A'), (self.b, 'B')):
            assert_raises_rpc_error(-26, "min relay fee not met",
                                    node.sendrawtransaction, hexstr)
        assert_equal(self.txid_of(hexstr) in self.a.getrawmempool(), False)
        assert_equal(self.txid_of(hexstr) in self.b.getrawmempool(), False)

    def test_2_underbid_waits_for_a_gap(self):
        """Under the wall is not rejected -- it is queued, and that is different.

        It sits in the mempool through every full block, and confirms in the first
        block that has room for it. A test that only asserted "not in the next
        block" would pass just as well against a node that had dropped it.
        """
        self.log.info("2. listed asset, bidding under the wall -> waits, then confirms")
        self.drain()
        hexstr = self.build(self.listed, UNDER_WALL)
        txid = self.a.sendrawtransaction(hexstr)

        for i in range(5):
            block = self.mine_full_block()
            assert_equal(txid in block['tx'], False)
            assert txid in self.a.getrawmempool(), "dropped rather than queued"
        self.log.info("   survived 5 full blocks unconfirmed")

        block, was_full = self.mine_until_it_lands(txid)
        assert_equal(was_full, False)
        self.log.info("   confirmed in the first block with room, at height %d" % block['height'])
        self.drain()

    def test_3_producer_floor_beats_an_empty_block(self):
        """-blockmintxfee is the producer's own reserve price, and it is absolute.

        Under it, a transaction is not merely outbid: the producer will leave the
        block half empty rather than take it. Nothing about the queue changes that,
        which is what separates this from case 2.
        """
        self.log.info("3. under the producer's own floor -> not even in an empty block")
        self.drain()
        hexstr = self.build(GASSET, UNDER_PRODUCER_FLOOR)
        txid = self.a.sendrawtransaction(hexstr)
        self.sync_mempools()
        assert txid in self.b.getrawmempool(), "relay uses minrelaytxfee, not blockmintxfee"

        # B has room to spare and still refuses it.
        assert_equal(self.b.getmempoolcongestion()['next_block_full'], False)
        block = self.b.getblock(self.generate(self.b, 1)[0])
        assert_equal(txid in block['tx'], False)
        assert txid in self.b.getrawmempool()

        # A, whose floor is zero, takes the very same transaction.
        block = self.a.getblock(self.generate(self.a, 1)[0])
        assert_equal(txid in block['tx'], True)
        self.drain()

    def test_4_rbf_over_the_wall(self):
        """Raising the bid in the same asset: confirmed in the next block."""
        self.log.info("4. RBF raising the bid over the wall -> next block")
        self.drain()
        hexstr = self.build(self.listed, UNDER_WALL, replaceable=True)
        txid = self.a.sendrawtransaction(hexstr)
        block = self.mine_full_block()
        assert_equal(txid in block['tx'], False)

        bumped = self.a.bumpfee(txid, {'fee_rate': OVER_WALL, 'fee_asset': self.listed})
        assert_equal(txid in self.a.getrawmempool(), False)
        block = self.mine_full_block()
        assert_equal(bumped['txid'] in block['tx'], True)
        self.drain()

    def test_5_asset_only_one_producer_prices(self):
        """Policy may diverge; consensus may not.

        A pays in an asset only B prices. A will not relay it and will not mine it;
        B does both. The assertion that matters is the last one: A accepts B's
        block. If fee-asset acceptance ever became consensus, this is where the
        chain would fork, and it would fork silently.
        """
        self.log.info("5. asset priced by producer B only -> only B mines it, "
                      "and A still accepts the block")
        self.drain()
        self.rates_a(extra={self.solo: RATE_ONE})
        hexstr = self.build(self.solo, OVER_WALL)
        self.rates_a()  # A does not price `solo`
        txid = self.txid_of(hexstr)

        assert_raises_rpc_error(-26, "min relay fee not met", self.a.sendrawtransaction, hexstr)
        assert_equal(self.b.sendrawtransaction(hexstr), txid)
        assert txid not in self.a.getrawmempool(), "A must not hold what it will not value"

        block_hash = self.generate(self.b, 1, sync_fun=self.no_op)[0]
        assert_equal(txid in self.b.getblock(block_hash)['tx'], True)

        self.sync_blocks()
        assert_equal(self.a.getbestblockhash(), block_hash)
        assert_equal(txid in self.a.getblock(block_hash)['tx'], True)
        self.log.info("   producer A accepted a block whose fee it does not price")
        self.drain()

    def test_6_divergent_prices_decide_differently(self):
        """The same atoms, two valuations, two answers -- on one chain.

        Built at a rate A values under the wall; B prices the asset eight times
        higher, so to B the identical transaction is a rich one. The transaction is
        in both mempools throughout: what differs is only who will mine it.
        """
        self.log.info("6. same transaction, two prices -> the richer valuation takes it")
        self.drain()
        hexstr = self.build(self.split, UNDER_WALL)
        txid = self.a.sendrawtransaction(hexstr)
        self.sync_mempools()
        assert txid in self.b.getrawmempool()

        block = self.mine_full_block(self.a)
        assert_equal(txid in block['tx'], False)
        assert txid in self.b.getrawmempool()

        # Same queue, other producer. Nothing about the transaction changed.
        c = self.b.getmempoolcongestion()
        if c['bytes'] < USABLE_VSIZE * 2:
            self.top_up(USABLE_VSIZE * 2 - c['bytes'])
        self.sync_mempools()
        block_hash = self.generate(self.b, 1, sync_fun=self.no_op)[0]
        assert_equal(txid in self.b.getblock(block_hash)['tx'], True)
        self.sync_blocks()
        assert_equal(self.a.getbestblockhash(), block_hash)
        self.drain()

    def test_7_cpfp_same_asset(self):
        """A child that pays for its parent, in the parent's own asset."""
        self.log.info("7. CPFP in the same asset -> parent and child land together")
        self.drain()
        parent_hex = self.build(self.listed, UNDER_WALL, to=self.a.getnewaddress())
        parent = self.a.sendrawtransaction(parent_hex)
        block = self.mine_full_block()
        assert_equal(parent in block['tx'], False)

        child = self.cpfp(parent_hex, self.listed, OVER_WALL * 3)
        block = self.mine_full_block()
        assert_equal(parent in block['tx'], True)
        assert_equal(child in block['tx'], True)
        self.drain()

    def test_8_rbf_changing_fee_asset(self):
        """Replacement in a DIFFERENT asset: the rule is value, not atom count.

        The replacement is worth more while it may well hold fewer atoms, which is
        the whole reason the mempool compares values rather than amounts.
        """
        self.log.info("8. RBF switching fee asset -> accepted on value")
        self.drain()
        hexstr = self.build(self.listed, UNDER_WALL, replaceable=True)
        txid = self.a.sendrawtransaction(hexstr)
        block = self.mine_full_block()
        assert_equal(txid in block['tx'], False)

        bumped = self.a.bumpfee(txid, {'fee_rate': OVER_WALL, 'fee_asset': GASSET})
        assert_equal(bumped['fee_asset'], GASSET)
        assert_equal(txid in self.a.getrawmempool(), False)
        block = self.mine_full_block()
        assert_equal(bumped['txid'] in block['tx'], True)
        self.drain()

    def test_9_cpfp_changing_fee_asset(self):
        """The child pays in another asset than the parent, and still lifts it."""
        self.log.info("9. CPFP switching fee asset -> the package is valued as one")
        self.drain()
        parent_hex = self.build(self.listed, UNDER_WALL, to=self.a.getnewaddress())
        parent = self.a.sendrawtransaction(parent_hex)
        block = self.mine_full_block()
        assert_equal(parent in block['tx'], False)

        child = self.cpfp(parent_hex, GASSET, OVER_WALL * 3)
        block = self.mine_full_block()
        assert_equal(parent in block['tx'], True)
        assert_equal(child in block['tx'], True)
        self.drain()

    # ------------------------------------------- confidential funds and fee bumps

    def mine_out(self, main, limit=30):
        """Mine until the queue is empty, without touching the bare RPC endpoint.

        self.drain() cannot be used once a second wallet is loaded: the framework's
        generate() asks the NODE for an address, and a node with two wallets has no
        default one to ask.
        """
        addr = main.getnewaddress()
        for _ in range(limit):
            if not self.a.getrawmempool():
                return
            self.a.generatetoaddress(1, addr, invalid_call=False)
        assert_equal(self.a.getrawmempool(), [])

    def descriptor_wallet(self, name, *, confidential, explicit):
        """A fresh wallet holding `listed` explicitly and GASSET in the given shapes.

        Confidential funding takes TWO outputs in one transaction on purpose. A
        single one cannot be balanced, so the wallet silently drops the blinding and
        hands back an explicit output (ignoreblindfail defaults to true) -- an
        earlier version of these cases funded that way and passed while testing the
        explicit path it meant to avoid.
        """
        main = self.a.get_wallet_rpc(self.default_wallet_name)
        self.a.createwallet(wallet_name=name, descriptors=True)
        w = self.a.get_wallet_rpc(name)
        addr = w.getnewaddress()
        conf = [w.getaddressinfo(w.getnewaddress())['confidential'] for _ in range(2)]
        main.sendtoaddress(address=addr, amount=Decimal('10'), assetlabel=self.listed,
                           fee_asset_label=GASSET, fee_rate=OVER_WALL)
        if confidential:
            main.sendmany(dummy="", amounts={conf[0]: Decimal('5'), conf[1]: Decimal('5')},
                          output_assets={c: GASSET for c in conf},
                          fee_asset=GASSET, fee_rate=OVER_WALL)
        if explicit:
            main.sendtoaddress(address=addr, amount=Decimal('10'), assetlabel=GASSET,
                               fee_asset_label=GASSET, fee_rate=OVER_WALL)
        self.mine_out(main)

        shapes = {False: 0, True: 0}
        for u in w.listunspent():
            if u['asset'] == GASSET:
                shapes[u.get('amountblinder', '0' * 64) != '0' * 64] += 1
        assert_equal(shapes[True] > 0, confidential)
        assert_equal(shapes[False] > 0, explicit)
        return w

    def underbid_from(self, w, dest):
        """A replaceable transaction paying its fee in `listed`, under the wall."""
        txid = w.sendtoaddress(address=dest, amount=Decimal('1'), assetlabel=self.listed,
                               fee_asset_label=self.listed, replaceable=True,
                               fee_rate=UNDER_WALL)
        assert txid in self.a.getrawmempool()
        return txid

    def test_10_bump_across_assets_takes_the_explicit_coin(self):
        """Switching fee asset must spend the explicit coin, not the confidential one.

        A bump rebuilds every recipient without a blinding key and asks for the new
        fee asset's change with add_blinding_key = false, so the replacement is
        entirely explicit -- the same invariant this path already enforces on the
        transaction being replaced. Coin selection has to respect it: a blinded coin
        reaches selection as a CInputCoin with its value and asset left at zero, so
        spending one funds a transaction with an input the amount accounting cannot
        see.

        Here the wallet holds the fee asset both ways, which is the ordinary state
        of a wallet that has been used. The replacement must be accepted, and every
        input it spends must be explicit.
        """
        self.log.info("10. bump across assets, wallet holding both shapes -> takes the explicit coin")
        self.drain()
        # No wall for these two: they ask whether the replacement is well formed at
        # all, which congestion does not change. Full-size blocks again, because the
        # wallet has to fund itself the way a wallet does and its transactions are
        # several times the size of the deliberately tiny block used above.
        self.restart_node(0, extra_args=self.extra_args[0])
        self.connect_nodes(0, 1)
        self.rates_a()
        main = self.a.get_wallet_rpc(self.default_wallet_name)
        dest = main.getnewaddress()

        w = self.descriptor_wallet('mixed', confidential=True, explicit=True)
        txid = self.underbid_from(w, dest)
        bumped = w.bumpfee(txid, {'fee_rate': OVER_WALL, 'fee_asset': GASSET})
        assert_equal(bumped['fee_asset'], GASSET)

        if bumped['txid'] not in self.a.getrawmempool():
            # Say WHY. "not in the mempool" alone sends the next reader looking at
            # relay or at BIP125 rules rather than at the transaction itself.
            reason = self.a.testmempoolaccept(
                [w.gettransaction(bumped['txid'], True)['hex']])[0].get('reject-reason', '?')
            raise AssertionError("the wallet's own node refused its replacement: %s" % reason)

        # And it got there by choosing correctly, not by luck.
        replacement = self.a.decoderawtransaction(w.gettransaction(bumped['txid'], True)['hex'])
        for vin in replacement['vin']:
            prev = self.a.getrawtransaction(vin['txid'], True)['vout'][vin['vout']]
            assert 'value' in prev, "the replacement spends a confidential output"
        self.a.unloadwallet('mixed')

    def test_11_bump_with_only_confidential_funds_is_refused(self):
        """And when there is no explicit coin, refuse -- clearly, and in time.

        This is the case that was failing in the field. The wallet would build the
        replacement anyway, record the bump, and hand the node a transaction it
        refused as bad-txns-in-ne-out: the user is left with a stuck transaction and
        a wallet that believes it replaced it.

        Refusing is the honest outcome, but only if the reason given is the true
        one. "Insufficient funds" would be a lie to someone looking at the balance
        in their own wallet -- the money is there, it just cannot be spent by a
        transaction that has to stay unblinded.
        """
        self.log.info("11. bump across assets, only confidential funds -> refused, and says why")
        main = self.a.get_wallet_rpc(self.default_wallet_name)
        dest = main.getnewaddress()
        w = self.descriptor_wallet('confonly', confidential=True, explicit=False)
        txid = self.underbid_from(w, dest)

        assert_raises_rpc_error(-4, "only in confidential outputs",
                                w.bumpfee, txid, {'fee_rate': OVER_WALL, 'fee_asset': GASSET})
        # The original is untouched: a refused bump must not leave the wallet in a
        # state where the transaction is neither replaced nor replaceable again.
        assert txid in self.a.getrawmempool()
        bumped = w.bumpfee(txid, {'fee_rate': OVER_WALL})
        assert bumped['txid'] in self.a.getrawmempool()
        self.a.unloadwallet('confonly')

    # ------------------------------------------------------------------ CPFP help

    def cpfp(self, parent_hex, fee_asset, fee_rate):
        """Spend the parent's first output, paying `fee_rate` in `fee_asset`.

        Spending the parent explicitly is what makes this CPFP rather than an
        unrelated rich transaction: coin selection left to itself would pick a
        confirmed input and lift nothing.
        """
        parent = self.a.decoderawtransaction(parent_hex)
        vout = next(i for i, o in enumerate(parent['vout'])
                    if o['scriptPubKey'].get('type') not in ('fee', 'nulldata')
                    and o.get('asset') == self.listed)
        spend = [{'txid': parent['txid'], 'vout': vout}]
        child_hex = self.build(fee_asset, fee_rate, amount=Decimal('0.0005'),
                               spend=spend, change_assets={fee_asset, self.listed},
                               funding_assets=[fee_asset])
        return self.a.sendrawtransaction(child_hex)

    # ---------------------------------------------------------------------- main

    def run_test(self):
        self.init()
        self.test_1_unpriced_asset_never_confirms()
        self.test_2_underbid_waits_for_a_gap()
        self.test_3_producer_floor_beats_an_empty_block()
        self.test_4_rbf_over_the_wall()
        self.test_5_asset_only_one_producer_prices()
        self.test_6_divergent_prices_decide_differently()
        self.test_7_cpfp_same_asset()
        self.test_8_rbf_changing_fee_asset()
        self.test_9_cpfp_changing_fee_asset()
        self.test_10_bump_across_assets_takes_the_explicit_coin()
        self.test_11_bump_with_only_confidential_funds_is_refused()


if __name__ == '__main__':
    AnyAssetFeeMarketTest().main()
