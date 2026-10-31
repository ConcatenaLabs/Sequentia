#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A transaction of ours, broadcast again, reaches peers that saw it before.

A node remembers, per connection, which transactions the peer has seen, and
never announced those to it again. The memory only fills: a peer that drops a
transaction it saw keeps it "known" for the life of the connection. Every node
drops a transaction whose lock time a reorg leaves in the future, as a reorg
that loses height -- an anchor reorg whenever the parent chain orphans the
blocks the newest blocks relied on -- does to one locked at the height it was
made at. Resubmitted once the chain grew back, such a transaction sat in its
sender's mempool, offered to no peer, on long-lived connections forever.

Checked here: a wallet payment carries no lock time, so a reorg that loses
height leaves it final; local broadcasts and the retry of a local transaction
no peer has fetched are offered to every peer again; the wallet hands a
payment back to the node as soon as the node stops refusing it, and reports
one the node refuses ("refused") and one no peer has taken ("unbroadcast").
"""

import time

from test_framework.messages import MSG_TX, MSG_WTX
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

MAX_INITIAL_BROADCAST_DELAY = 15 * 60


class InvCounter(P2PTxInvStore):
    """Counts announcements and never asks for the transaction, so the sender
    keeps it unbroadcast."""
    def on_inv(self, message):
        for i in message.inv:
            if i.type in (MSG_TX, MSG_WTX):
                self.tx_invs_received[i.hash] += 1

    def count(self, tx):
        # Called inside wait_until, which holds p2p_lock already.
        return sum(self.tx_invs_received[int(h, 16)] for h in {tx["txid"], tx["wtxid"]})


class ReannounceLocalTxTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.test_wallet_payment_survives_height_loss()
        self.test_reorg_resubmit()
        self.test_unbroadcast_retry()

    def test_wallet_payment_survives_height_loss(self):
        self.log.info("A wallet payment carries no lock time, so a reorg that loses height leaves it final")
        n0, n1 = self.nodes
        txid = n0.sendtoaddress(address=n1.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        assert_equal(n0.getrawtransaction(txid, True)["locktime"], 0)
        self.sync_mempools()
        h = n0.getblockcount()
        self.generate(n0, 1)
        assert_equal(n0.gettransaction(txid)["confirmations"], 1)
        orphaned = n0.getblockhash(h)
        for n in self.nodes:
            n.invalidateblock(orphaned)
            assert_equal(n.getblockcount(), h - 1)
        self.log.info("Back in both mempools at once, and in the next block")
        for n in self.nodes:
            self.wait_until(lambda: txid in n.getrawmempool(), timeout=30)
        assert "refused" not in n0.gettransaction(txid)
        self.generate(n0, 1)
        assert_equal(n1.gettransaction(txid)["confirmations"], 1)

    def test_reorg_resubmit(self):
        self.log.info("A payment with a caller's lock time, dropped by a reorg that loses height, reaches the peer once resubmitted")
        n0, n1 = self.nodes
        h = n0.getblockcount()
        # A lock time at the current height, as Bitcoin Core's wallet sets one.
        # Paid to the sender itself: the peer's wallet must not know it, or the
        # peer would resubmit it on its own and prove nothing about relay.
        raw = n0.createrawtransaction([], [{n0.getnewaddress(): 1}], h)
        funded = n0.fundrawtransaction(raw, {"fee_asset": "bitcoin"})
        signed = n0.signrawtransactionwithwallet(funded["hex"])["hex"]
        decoded = n0.decoderawtransaction(signed)
        assert_equal(decoded["locktime"], h)
        assert any(i["sequence"] < 0xffffffff for i in decoded["vin"])
        txid = n0.sendrawtransaction(signed)
        self.sync_mempools()
        assert txid in n1.getrawmempool()

        self.log.info("Mined, then a reorg on both nodes back below its lock time")
        self.generate(n0, 1)
        assert_equal(n0.gettransaction(txid)["confirmations"], 1)
        orphaned = n0.getblockhash(h)
        for n in self.nodes:
            n.invalidateblock(orphaned)
            assert_equal(n.getblockcount(), h - 1)
            assert txid not in n.getrawmempool()
        assert_equal(n0.testmempoolaccept([signed])[0]["reject-reason"], "non-final")
        self.log.info("The wallet says the node refuses it, and why")
        self.wait_until(lambda: n0.gettransaction(txid).get("refused") == "non-final", timeout=60)

        self.log.info("The chain grows back past its lock time; the wallet resubmits it at once")
        self.generate(n0, 2, sync_fun=self.sync_blocks)
        assert n0.testmempoolaccept([signed])[0]["allowed"]
        self.wait_until(lambda: txid in n0.getrawmempool(), timeout=60)
        assert "refused" not in n0.gettransaction(txid)

        self.log.info("It reaches the peer over the same connection")
        self.wait_until(lambda: txid in n1.getrawmempool(), timeout=60)
        self.wait_until(lambda: not n0.getmempoolentry(txid)["unbroadcast"], timeout=60)
        assert "unbroadcast" not in n0.gettransaction(txid)
        self.generate(n0, 1)
        assert txid in n1.getblock(n1.getbestblockhash())["tx"]

    def test_unbroadcast_retry(self):
        self.log.info("A local transaction no peer has fetched is offered again to the peers that saw it")
        n0, n1 = self.nodes
        self.disconnect_nodes(0, 1)
        peer = n0.add_p2p_connection(InvCounter())
        txid = n0.sendtoaddress(address=n0.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        sent = time.time()
        tx = {"txid": txid, "wtxid": n0.getmempoolentry(txid)["wtxid"]}
        assert "unbroadcast" not in n0.gettransaction(txid)  # a fresh payment does not read as stuck
        peer.wait_until(lambda: peer.count(tx) == 1, timeout=60)
        assert n0.getmempoolentry(txid)["unbroadcast"]

        self.log.info("The 10-15 minute retry announces it again")
        n0.mockscheduler(MAX_INITIAL_BROADCAST_DELAY)
        peer.wait_until(lambda: peer.count(tx) == 2, timeout=60)

        self.log.info("So does sending it again by hand")
        n0.sendrawtransaction(n0.gettransaction(txid)["hex"])
        peer.wait_until(lambda: peer.count(tx) == 3, timeout=60)

        self.log.info("A minute on, unfetched, the wallet says no peer has taken it")
        time.sleep(max(0, sent + 62 - time.time()))
        wtx = n0.gettransaction(txid)
        assert_equal(wtx["unbroadcast"], True)
        assert "refused" not in wtx

        self.log.info("Once a peer fetches it, it is broadcast and the wallet stops saying otherwise")
        self.connect_nodes(0, 1)
        n0.mockscheduler(MAX_INITIAL_BROADCAST_DELAY)
        self.wait_until(lambda: txid in n1.getrawmempool(), timeout=60)
        self.wait_until(lambda: not n0.getmempoolentry(txid)["unbroadcast"], timeout=60)
        assert "unbroadcast" not in n0.gettransaction(txid)


if __name__ == "__main__":
    ReannounceLocalTxTest().main()
