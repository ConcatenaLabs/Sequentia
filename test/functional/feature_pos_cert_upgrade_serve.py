#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A fuller certificate adopted for a connected block is stored and served (#387).

A node that holds a block with a thin certificate and later adopts a fuller
one (PosUpgradeCertificate, #282) keeps it in the block's index entry. The
stored body keeps the certificate it arrived with, so every read of the block
must put the adopted one in its place: block relay, getposcert, and a restart
in between. Before #387 a block that had been connected kept serving the thin
body, and a fresh node syncing from it stayed uncertified.

X connects S with two signatures, moves to the certified sibling B, then adopts
S's four-member certificate from gossip and moves back to S. Forged
certificates do not upgrade it. After a restart X still serves the four-member
certificate, as a block to a fresh follower and in answer to getposcert.

Based on the reproduction in #387 (review of #375).
"""

import time

from feature_pos_cert_gossip import msg_getposcert
from feature_pos_stall_cert_upgrade import (
    CertPeer,
    PosStallCertUpgradeTest,
    hash_order,
    msg_poscert,
)
from test_framework.p2p import MESSAGEMAP
from test_framework.util import assert_equal


class PosCertUpgradeServeTest(PosStallCertUpgradeTest):
    def set_test_params(self):
        super().set_test_params()

    def forged_header(self, node, block_hash, source_hash, mutation=None):
        raw = bytearray.fromhex(node.getblockheader(block_hash, False))
        original = bytearray.fromhex(node.getblockheader(block_hash)["signblock_witness_hex"])
        offset = raw.find(original)
        solution = bytearray.fromhex(node.getblockheader(source_hash)["signblock_witness_hex"])
        if mutation == "aggregate":
            leader_len = solution[0]
            solution[1 + leader_len + 1] ^= 1
        elif mutation == "phantom":
            solution[-2] |= 0x10
        assert offset >= 0
        raw[offset:offset + len(solution)] = solution
        return bytes(raw)

    def run_test(self):
        MESSAGEMAP[b"poscert"] = msg_poscert
        _, x, y, z, fresh = self.nodes
        leader = self.stakers[0][0]
        all_keys = [wif for wif, _ in self.stakers]

        self.advance_parent(12)
        for _ in range(2):
            self.produce(z, 3, leader)
        parent = z.getbestblockhash()
        parent_height = z.getblockcount()
        for node in (x, y, fresh):
            for height in range(1, parent_height + 1):
                assert node.submitblock(z.getblock(z.getblockhash(height), 0)) in (None, "duplicate")
        self.advance_parent(3)

        candidates = []
        for _ in range(20):
            b = self.produce(z, 3, leader)
            b_hex = z.getblock(b["hash"], 0)
            z.invalidateblock(b["hash"])
            s = self.produce(z, 1, leader)
            s2_hex = z.getblock(s["hash"], 0)
            z.invalidateblock(s["hash"])
            if hash_order(s["hash"]) < hash_order(b["hash"]):
                candidates = (b, b_hex, s, s2_hex)
                break
            time.sleep(1.1)
        assert candidates
        b, block_b, s, block_s2 = candidates
        B, S = b["hash"], s["hash"]
        full = z.recertifyposblock(block_s2, leader, all_keys)
        block_s4 = full["hex"]
        assert_equal(full["countersignatures"], 4)

        self.log.info("Y holds S with four signers; X connects S with two, then the certified sibling B")
        assert_equal(y.submitblock(block_s4), None)
        cert = bytes.fromhex(y.getblockheader(S, False))
        assert_equal(y.submitblock(block_b), "inconclusive")
        assert_equal(x.submitblock(block_s2), None)
        assert_equal(x.submitblock(block_b), None)
        assert_equal(x.getbestblockhash(), B)
        assert_equal(x.getblockheader(S)["poscountersigs"], 2)
        peer = x.add_p2p_connection(CertPeer())

        self.log.info("Invalid aggregate, phantom signer, and another height's certificate do not upgrade")
        for forged in (self.forged_header(y, S, S, "aggregate"),
                       self.forged_header(y, S, S, "phantom"),
                       self.forged_header(y, S, parent)):
            peer.send_message(msg_poscert(forged))
            time.sleep(0.3)
            assert_equal(x.getblockheader(S)["poscountersigs"], 2)
            assert_equal(x.getblockheader(S)["poscertified"], False)

        self.log.info("The genuine fuller certificate upgrades X, which moves back to S and serves it")
        peer.send_message(msg_poscert(cert))
        self.wait_until(lambda: x.getblockheader(S)["poscountersigs"] == 4, timeout=10)
        assert_equal(x.getbestblockhash(), S)
        assert_equal(x.getblockheader(S)["poscertified"], True)
        assert_equal(x.getblockheader(S, False), cert.hex())
        assert_equal(x.getblock(S, 0), block_s4)

        self.log.info("After a restart X still serves the fuller certificate, as a block and on getposcert")
        self.restart_node(1, extra_args=self.anchored_args(1) + self.specs)
        assert_equal(x.getblockheader(S)["poscountersigs"], 4)
        assert_equal(x.getblock(S, 0), block_s4)
        asker = x.add_p2p_connection(CertPeer())
        asker.send_message(msg_getposcert(bytes.fromhex(S)[::-1]))
        self.wait_until(lambda: len(asker.certs) > 0, timeout=10)
        assert_equal(asker.certs[-1], cert)

        self.log.info("A fresh follower syncing only from X gets S certified")
        self.connect_nodes(1, 4)
        self.sync_blocks([x, fresh])
        assert_equal(fresh.getbestblockhash(), S)
        assert_equal(fresh.getblockheader(S)["poscountersigs"], 4)
        assert_equal(fresh.getblockheader(S, False), cert.hex())


if __name__ == "__main__":
    PosCertUpgradeServeTest().main()
