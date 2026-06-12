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


if __name__ == '__main__':
    AnchorSwapConsistencyTest().main()
