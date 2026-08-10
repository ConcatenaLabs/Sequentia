#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Demonstrates cross-chain swap consistency under Bitcoin anchoring.

The motivation for anchoring (doc/sequentia/03-bitcoin-anchoring.md, paper
principles 5-7) is that a cross-chain atomic swap needs no extra
reorg-protection timelocks: if the Bitcoin leg of a swap is reorganized away,
the Sequentia blocks containing the corresponding Sequentia leg are
reorganized away *with it*, because they anchor to the discarded Bitcoin
block (or a descendant of it).

This test walks that exact scenario with plain payments standing in for the
two legs of a swap (the property is independent of the locking script, so an
HTLC rides on top unchanged):

  1. Alice pays Bob on the parent chain ("the BTC leg"), confirmed in parent
     block P.
  2. Following paper principle 7, the Sequentia leg is broadcast only after
     the BTC leg is on-chain: Bob pays Alice on the anchored chain ("the SEQ
     leg"), confirmed in a Sequentia block S anchored at a height >= P's.
  3. The parent chain reorganizes: P is replaced by a branch in which the
     BTC leg is double-spent away.
  4. The anchor watcher reorganizes the Sequentia chain: S is disconnected
     because its anchor is no longer canonical. Both legs of the swap have
     now been reverted together — neither party is left having paid without
     being paid.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_auth_cookie,
    get_datadir_path,
    rpc_port,
    p2p_port,
)


class AnchorSwapConsistencyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []

        parent_chain = "elementsregtest"
        parent_args = [
            "-port=" + str(p2p_port(0)),
            "-rpcport=" + str(rpc_port(0)),
            "-validatepegin=0",
            "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-blindedaddresses=0",
            "-fallbackfee=0.0001",
            "-walletrbf=1",
            "-txindex=1",
        ]
        self.add_nodes(1, [parent_args], chain=[parent_chain])
        self.start_node(0)
        self.parentgenesisblockhash = self.nodes[0].getblockhash(0)

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        anchored_args = [
            "-port=" + str(p2p_port(1)),
            "-rpcport=" + str(rpc_port(1)),
            "-validatepegin=0",
            "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-blindedaddresses=0",
            "-fallbackfee=0.0001",
            "-con_bitcoin_anchor=1",
            "-validateanchor=1",
            "-anchorpollinterval=1",
            "-mainchainrpchost=127.0.0.1",
            "-mainchainrpcport=%s" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u,
            "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesisblockhash,
        ]
        self.add_nodes(1, [anchored_args], chain=["elementsregtest"])
        self.start_node(1)

        for node in self.nodes:
            node.createwallet(wallet_name="w", descriptors=True)

    def wait_for_tip_change(self, node, old_tip, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if node.getbestblockhash() != old_tip:
                return
            time.sleep(0.25)
        raise AssertionError("chain did not reorganize within %ds" % timeout)

    def mine_anchored_at_least(self, node, addr, height, timeout=30):
        """Mine blocks on the anchored chain until one anchors at/above `height`.

        The committee's chosen anchor can lag the parent tip (on a live network
        -anchoravoidcontested deliberately backs it down to the last uncontested
        parent height), so a caller that needs a block anchored at a given height
        must WAIT for it rather than assume the next block will carry it.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.generatetoaddress(node, 1, addr, sync_fun=self.no_op)
            block = node.getbestblockhash()
            if node.getblockheader(block)['anchorheight'] >= height:
                return block
            time.sleep(0.25)
        raise AssertionError("no block anchored at/above parent height %d within %ds" % (height, timeout))

    def double_spend_away(self, parent, txid, fork_height, extend=3):
        """Reorg the parent chain from `fork_height`, double-spending `txid` away.

        Replaces the branch from fork_height upward with a longer one in which
        txid's inputs are spent elsewhere, so txid can never re-confirm.
        """
        parent.invalidateblock(parent.getblockhash(fork_height))
        assert parent.gettransaction(txid)['confirmations'] <= 0
        leg_dec = parent.decoderawtransaction(parent.getrawtransaction(txid))
        inputs = []
        in_total = 0
        for vin in leg_dec['vin']:
            prev = parent.decoderawtransaction(parent.getrawtransaction(vin['txid']))
            in_total += prev['vout'][vin['vout']]['value']
            inputs.append({"txid": vin['txid'], "vout": vin['vout']})
        fee = 0.001  # comfortably above the BIP125 replacement fee floor
        raw = parent.createrawtransaction(inputs, [
            {parent.getnewaddress(): round(float(in_total) - fee, 8)},
            {"fee": fee},
        ])
        signed = parent.signrawtransactionwithwallet(raw)
        assert signed['complete']
        double_spend = parent.sendrawtransaction(signed['hex'])
        assert txid not in parent.getrawmempool()
        self.generatetoaddress(parent, extend, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.gettransaction(txid)['confirmations'] <= 0
        assert parent.gettransaction(double_spend)['confirmations'] >= 1
        return double_spend

    def run_test(self):
        parent = self.nodes[0]   # stands in for Bitcoin
        seq = self.nodes[1]      # the anchored (Sequentia) chain

        # Fund both sides
        parent_mine = parent.getnewaddress()
        seq_mine = seq.getnewaddress()
        self.generatetoaddress(parent, 101, parent_mine, sync_fun=self.no_op)
        self.generatetoaddress(seq, 101, seq_mine, sync_fun=self.no_op)

        # --- Step 1: the BTC leg. Alice pays Bob on the parent chain. ---
        bob_parent = parent.getnewaddress()
        btc_leg = parent.sendtoaddress(address=bob_parent, amount=10.0, replaceable=True)
        self.generatetoaddress(parent, 1, parent_mine, sync_fun=self.no_op)
        block_p_height = parent.getblockcount()
        assert_equal(parent.gettransaction(btc_leg)['confirmations'], 1)

        # --- Step 2: the SEQ leg, broadcast only now that the BTC leg is
        # on-chain (paper principle 7), so the Sequentia block containing it
        # anchors at a height >= P's. ---
        alice_seq = seq.getnewaddress()
        seq_leg = seq.sendtoaddress(address=alice_seq, amount=10.0)
        self.generatetoaddress(seq, 1, seq_mine, sync_fun=self.no_op)
        block_s = seq.getbestblockhash()
        header_s = seq.getblockheader(block_s)
        assert header_s['anchorheight'] >= block_p_height
        assert_equal(seq.gettransaction(seq_leg)['confirmations'], 1)
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')

        # --- Step 3: the parent chain reorganizes and the BTC leg is
        # double-spent away on the new branch. ---
        block_p = parent.getblockhash(block_p_height)
        parent.invalidateblock(block_p)
        # The BTC leg is back in the parent mempool; replace it (BIP125) with
        # a conflicting spend of the same inputs back to Alice, so the new
        # branch cannot re-confirm it.
        assert parent.gettransaction(btc_leg)['confirmations'] <= 0
        leg_dec = parent.decoderawtransaction(parent.getrawtransaction(btc_leg))
        inputs = []
        in_total = 0
        for vin in leg_dec['vin']:
            prev = parent.decoderawtransaction(parent.getrawtransaction(vin['txid']))
            in_total += prev['vout'][vin['vout']]['value']
            inputs.append({"txid": vin['txid'], "vout": vin['vout']})
        fee = 0.001  # comfortably above the BIP125 replacement fee floor
        raw = parent.createrawtransaction(inputs, [
            {parent.getnewaddress(): round(float(in_total) - fee, 8)},
            {"fee": fee},
        ])
        signed = parent.signrawtransactionwithwallet(raw)
        assert signed['complete']
        double_spend = parent.sendrawtransaction(signed['hex'])
        assert btc_leg not in parent.getrawmempool()
        # Mine the competing branch past the original height
        self.generatetoaddress(parent, 2, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockcount() > block_p_height
        # The BTC leg is gone from the parent's best chain
        assert parent.gettransaction(btc_leg)['confirmations'] <= 0
        assert parent.gettransaction(double_spend)['confirmations'] >= 1

        # --- Step 4: the anchored chain follows: block S (whose anchor was
        # reorganized away) is disconnected, reverting the SEQ leg too. ---
        self.wait_for_tip_change(seq, block_s)
        assert seq.getbestblockhash() != block_s
        assert_equal(seq.getblockheader(block_s)['confirmations'], -1)
        # The SEQ leg is unconfirmed again — back in the mempool, exactly as
        # if the swap's second leg had never settled.
        assert seq.gettransaction(seq_leg)['confirmations'] <= 0
        assert seq_leg in seq.getrawmempool()

        # --- Aftermath: production resumes on the new parent branch. The SEQ
        # leg may re-confirm from the mempool (on Sequentia it is still a
        # valid payment); in a real HTLC the parties now act on the reverted
        # state (refund/retry) before any timelock pressure, which is the
        # point of anchoring. ---
        self.generatetoaddress(seq, 1, seq_mine, sync_fun=self.no_op)
        new_tip = seq.getblockheader(seq.getbestblockhash())
        assert new_tip['anchorheight'] >= parent.getblockcount() - 1
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')

        self.test_burying_block_does_not_rescue(parent, seq, parent_mine, seq_mine)

    def test_burying_block_does_not_rescue(self, parent, seq, parent_mine, seq_mine):
        """A well-anchored block BURYING a leg does not make that leg safe.

        This is the counterexample to the tempting relaxation of the cross-chain
        claim gate. When the block holding the Sequentia leg anchors BELOW the
        BTC-leg height, the gate refuses and no amount of waiting changes it
        (anchorheight is a committed header field). The apparent shortcut is to
        gate on a LATER block instead — one that buries the leg and anchors high
        enough — since that value does advance, typically within a block or two.

        It is not a shortcut, it is the removal of the protection. Invalidation
        propagates to a block's DESCENDANTS, never to its ANCESTORS: orphaning the
        burying block's anchor discards the burying block and everything above it
        while the leg's own block stays CONNECTED and its output stays spendable.
        Meanwhile the counterparty's BTC leg, confirmed above the fork point, is
        gone and can be double-spent. The party that gave the asset loses both
        legs.

        The scenario below is the one measured live on 2026-07-25 (leg anchored
        145607, BTC leg at 145609, buried 90s later by a block anchored 145609):

          leg block   anchored at X            <- must SURVIVE the reorg
          BTC leg     confirmed at X+2         <- must DIE in the reorg
          bury block  anchored at X+2          <- must DIE with it
          reorg fork point P = X+1, i.e. X < P <= X+2
        """
        self.log.info("burying-block counterexample: a later well-anchored block must NOT rescue an under-anchored leg")

        # 1. The asset leg confirms in a Sequentia block anchored at the CURRENT
        #    parent height X (at most: the anchor may legitimately lag).
        x = parent.getblockcount()
        leg_txid = seq.sendtoaddress(address=seq.getnewaddress(), amount=1.0)
        self.generatetoaddress(seq, 1, seq_mine, sync_fun=self.no_op)
        block_leg = seq.getbestblockhash()
        leg_anchor = seq.getblockheader(block_leg)['anchorheight']
        assert leg_anchor <= x
        assert_equal(seq.gettransaction(leg_txid)['confirmations'], 1)

        # 2. The BTC leg confirms TWO parent blocks later, so the asset leg's
        #    block is anchored strictly BELOW it: the gate must refuse this leg.
        self.generatetoaddress(parent, 1, parent_mine, sync_fun=self.no_op)      # X+1
        btc_leg = parent.sendtoaddress(address=parent.getnewaddress(), amount=10.0, replaceable=True)
        self.generatetoaddress(parent, 1, parent_mine, sync_fun=self.no_op)      # X+2 confirms it
        h_btc = parent.getblockcount()
        assert_equal(h_btc, x + 2)
        assert_equal(parent.gettransaction(btc_leg)['confirmations'], 1)
        assert leg_anchor < h_btc, "the leg must be UNDER-anchored for this test to mean anything"

        # 3. A later Sequentia block buries the leg and anchors at/above the
        #    BTC-leg height. A gate that consulted THIS block would pass.
        block_bury = self.mine_anchored_at_least(seq, seq_mine, h_btc)
        assert block_bury != block_leg
        assert seq.getblockheader(block_bury)['anchorheight'] >= h_btc
        assert seq.getblockheader(block_leg)['confirmations'] >= 2   # the leg is buried

        # 4. Bitcoin reorgs from P = X+1: strictly above the leg block's anchor
        #    and at/below the BTC leg's height. Routine — a 1-2 block reorg.
        self.double_spend_away(parent, btc_leg, fork_height=x + 1)

        # 5. THE POINT. The burying block dies with its orphaned anchor; the leg's
        #    OWN block survives, because its anchor is still canonical — and so
        #    does the funded output a claimant would spend. The BTC leg is gone.
        self.wait_for_tip_change(seq, block_bury)
        assert_equal(seq.getblockheader(block_bury)['confirmations'], -1)
        assert seq.getblockheader(block_leg)['confirmations'] >= 1, \
            "the leg's block must SURVIVE: invalidation never propagates to ancestors"
        assert_equal(seq.gettransaction(leg_txid)['confirmations'] >= 1, True)
        assert parent.gettransaction(btc_leg)['confirmations'] <= 0

        self.log.info("  leg block %s (anchor %d) SURVIVED; burying block %s (anchor >= %d) was invalidated",
                      block_leg[:16], leg_anchor, block_bury[:16], h_btc)
        self.log.info("  => had the claim gate consulted the burying block, the claimant would have taken the")
        self.log.info("     asset from a still-confirmed output while its own BTC leg was reorged away and")
        self.log.info("     double-spent. The gate MUST read the block that confirmed the leg.")


if __name__ == '__main__':
    AnchorSwapConsistencyTest().main()
