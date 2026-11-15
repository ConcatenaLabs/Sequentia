#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A sibling's fuller certificate is judged against its parent's registry (#277, #386).

A certificate is verified against the committee, keys, seat weights and quorum
its parent leaves. A node whose active tip is a sibling that changed the
registry used to verify it against the tip's state instead: a genuine
certificate was ignored, and two nodes finalized different blocks for good.

A registers a fifth committee member; S is its sibling below it by hash. X has
A active and S with two signatures; Y has S with four, from the parent's
registry. S's four-member certificate reaches X, which must adopt it, move to S
and finalize it, like Y. V is X a second time, restarted before the
certificate arrives, so the registry at the parent is rebuilt from the tip
rather than kept from when A connected. W syncs from scratch from X and must
end on S with its four-member certificate.

Based on the reproduction in #386 (review of #375).
"""

import time

from feature_pos_certified_stall_quorum import PosCertifiedStallQuorumTest, FEE, hash_order
from feature_pos_stall_cert_upgrade import CertPeer, msg_poscert
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.p2p import MESSAGEMAP
from test_framework.script import CScript, OP_TRUE
from test_framework.util import assert_equal

V, W = 5, 6
REBUILT = "stake registry after %s rebuilt by undoing the tip"


class PosCertUpgradeRegistryTest(PosCertifiedStallQuorumTest):
    def set_test_params(self):
        super().set_test_params()
        self.num_nodes = 7

    def setup_network(self, split=False):
        super().setup_network(split)
        chain = "elementsregtest"
        self.add_nodes(2, [self.anchored_args(i) for i in (V, W)], chain=[chain] * 2)
        for i in (V, W):
            self.start_node(i, extra_args=self.anchored_args(i) + self.specs + ["-debug=validation"])

    def log_has(self, node, text):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return any(text in line for line in f)

    def run_test(self):
        MESSAGEMAP[b"poscert"] = msg_poscert
        _, x, y, z1, z2, v, w = self.nodes
        leader = self.stakers[0][0]
        all_keys = [wif for wif, _ in self.stakers]

        self.advance_parent(12)
        for _ in range(2):
            self.produce(x, 3, leaders=[leader])
        self.sync_blocks(self.nodes[1:5])
        parent = z2.getbestblockhash()
        height = z2.getblockcount() + 1
        for h in range(1, height):
            assert v.submitblock(z2.getblock(z2.getblockhash(h), 0)) in (None, "duplicate")
        self.advance_parent(3)
        self.disconnect_all()

        txid, n, amount, asset = self.free_coin(z1)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(2 * COIN, bytes.fromhex(self.e_stake), asset),
                   CTxOut(amount - 2 * COIN - FEE, CScript([OP_TRUE]), asset), CTxOut(FEE, b'', asset)]
        z1.sendrawtransaction(tx.serialize().hex())

        chosen = None
        for _ in range(20):
            a = self.produce(z1, 3, leaders=[leader])
            a_hex = z1.getblock(a["hash"], 0)
            z1.invalidateblock(a["hash"])
            s = self.produce(z2, 1, leaders=[leader])
            s2_hex = z2.getblock(s["hash"], 0)
            z2.invalidateblock(s["hash"])
            if hash_order(s["hash"]) < hash_order(a["hash"]):
                chosen = a, a_hex, s, s2_hex
                break
            time.sleep(1.1)
        assert chosen
        a, block_a, s, block_s2 = chosen
        A, S = a["hash"], s["hash"]
        full = z2.recertifyposblock(block_s2, leader, all_keys)
        block_s4 = full["hex"]
        assert_equal(full["countersignatures"], 4)

        self.log.info("X and V have A active, which changes the committee, and S with two signatures; Y has S with four")
        for node in (x, v):
            assert_equal(node.submitblock(block_a), None)
            assert_equal(node.submitblock(block_s2), "inconclusive")
            assert_equal(node.getbestblockhash(), A)
            assert_equal(node.getblockheader(S)["poscountersigs"], 2)
        assert_equal(y.submitblock(block_s4), None)
        cert = bytes.fromhex(y.getblockheader(S, False))
        assert_equal(y.submitblock(block_a), "inconclusive")
        assert_equal(y.getbestblockhash(), S)

        self.log.info("V restarts: no registry snapshot survives")
        self.restart_node(V, extra_args=self.anchored_args(V) + self.specs + ["-debug=validation"])
        assert_equal(v.getbestblockhash(), A)

        self.log.info("S's genuine four-member certificate reaches X and V; both adopt it and finalize S, like Y")
        for node in (x, v):
            peer = node.add_p2p_connection(CertPeer())
            peer.send_message(msg_poscert(cert))
        for name, node in (("X", x), ("V", v), ("Y", y)):
            self.wait_until(lambda: node.getblockheader(S)["poscountersigs"] == 4, timeout=10)
            self.wait_until(lambda: node.getbestblockhash() == S, timeout=10)
            try:
                self.wait_until(lambda: node.getposfinality().get("finalized_hash") == S, timeout=30)
            except AssertionError:
                raise AssertionError("%s did not finalize S: %s" % (name, node.getposfinality()))
        assert self.log_has(v, REBUILT % parent), "V did not rebuild the parent's registry"

        self.log.info("W syncs from scratch from X and ends on S, certified")
        self.connect_nodes(1, W)
        self.sync_blocks([x, w])
        assert_equal(w.getblockhash(height), S)
        assert_equal(w.getblockheader(S)["poscountersigs"], 4)
        assert_equal(w.getblockheader(S, False), cert.hex())


if __name__ == "__main__":
    PosCertUpgradeRegistryTest().main()
