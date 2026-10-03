#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Two equivocating committee members can split a majority-quorum committee;
the finality observation window heals it.

Committee of 4, quorum 3, so any two quorums overlap in exactly two members.
Two of the four staking keys are "Byzantine": each runs on TWO nodes, one in
each half of the network, so each key signs whatever block its half backs —
exactly an attacker that signs both of two competing blocks. The two honest
members are put on different halves and briefly disconnected, which stands in
for the timing split an attacker provokes with a late proposal (paper §6.2):
each half certifies a different block at the same height with one honest
signature plus the two double-signers. The halves are reconnected as soon as
both certificates exist, so every node sees both within a second.

The same scenario runs on two independent networks:
  - finality on connection (-posfinalitydelayms=0, the old rule): each half
    keeps the block it certified first, the finality gate refuses the other,
    and both branches keep growing — a permanent split;
  - the default observation window (3 s): both certificates meet inside the
    window, the deterministic comparator picks the same block everywhere, and
    the network converges on one chain.

A third network covers certificates of unequal size. The block hash excludes
the BLS certificate, so one block can carry certificates naming different
numbers of members, and each node keeps the count of the first it receives. On
a public BLS committee of 4 (quorum 3, the testnet's form) two nodes X and Y
each hold the same two sibling blocks A and B, X with 4 signatures on A and 3
on B, Y the reverse. The comparator never weighs the count, so both rank the
siblings alike, finalize the same one after the window, and stay on one chain.
"""

import os
import shutil
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript
from test_framework.util import assert_equal

GROUP = 6          # H1, H2, Z1a, Z2a, Z1b, Z2b
H1, H2, Z1A, Z2A, Z1B, Z2B = range(GROUP)
CERT = 0           # network 3 reuses network 1's first four nodes as X, Y, Z1, Z2
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosSplitEquivocationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2 * GROUP
        self.setup_clean_chain = True
        # Four staking keys: honest H1, H2 and Byzantine Z1, Z2. Equal weights,
        # committee 4, quorum 3.
        self.stakers = [make_staker() for _ in range(4)]
        key_of = {H1: 0, H2: 1, Z1A: 2, Z1B: 2, Z2A: 3, Z2B: 3}
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-posblockspacing=5", "-posblockspacingheight=1",
            "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = []
        for group, delay in ((0, 0), (1, 3000)):
            for role in range(GROUP):
                wif = self.stakers[key_of[role]][0]
                self.extra_args.append(common + [
                    "-posproducer", "-posproducerkey=%s" % wif,
                    "-posfinalitydelayms=%d" % delay,
                ])
        # Network 3, on network 1's nodes once it is done: a public BLS
        # committee of 4 with no producers; blocks are built with
        # generateposblock and a chosen set of committee keys.
        self.cert_stakers = [make_staker() for _ in range(4)]
        self.cert_common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-acceptnonstdtxn=1",
        ]

    def node(self, group, role):
        return self.nodes[group * GROUP + role]

    def setup_network(self):
        self.setup_nodes()
        for g in (0, 1):
            o = g * GROUP
            for a, b in ((H1, Z1A), (H1, Z2A), (Z1A, Z2A),      # half A
                         (H2, Z1B), (H2, Z2B), (Z1B, Z2B),      # half B
                         (H1, H2)):                             # the bridge
                self.connect_nodes(o + a, o + b)

    def provoke_split(self, g):
        """Partition the honest members, let each half certify a block at the
        same height, reconnect at once. Returns that height."""
        h1, h2 = self.node(g, H1), self.node(g, H2)
        # When a Byzantine key holds the best VRF, its two copies propose the
        # same block and both halves back it: no split. Like the attacker, just
        # try again at the next height.
        for attempt in range(1, 21):
            self.wait_until(lambda: h1.getblockcount() >= 3 and
                            h1.getbestblockhash() == h2.getbestblockhash(), timeout=180)
            self.disconnect_nodes(g * GROUP + H1, g * GROUP + H2)
            base = max(h1.getblockcount(), h2.getblockcount())
            target = base + 1
            self.wait_until(lambda: h1.getblockcount() >= target and h2.getblockcount() >= target, timeout=120)
            # Read both certified blocks BEFORE reconnecting: with the
            # observation window a split heals within a second of contact.
            a, b = h1.getblockhash(target), h2.getblockhash(target)
            if a != b:
                for n, name in ((h1, "H1"), (h2, "H2")):
                    hdr = n.getblockheader(n.getblockhash(target))
                    self.log.info("  %s certified %s at height %d with %s countersignatures",
                                  name, hdr["hash"][:16], target, hdr.get("poscountersigs"))
            self.connect_nodes(g * GROUP + H1, g * GROUP + H2)
            if a != b:
                self.log.info("  split obtained at attempt %d", attempt)
                break
            self.log.info("  attempt %d: both halves certified the same block, retrying", attempt)
        else:
            raise AssertionError("no split obtained in 20 attempts")
        return target

    def run_test(self):
        self.log.info("Network 1: finality on connection (old rule)")
        h_old = self.provoke_split(0)
        n1, n2 = self.node(0, H1), self.node(0, H2)
        self.wait_until(lambda: n1.getblockcount() >= h_old + 3 and n2.getblockcount() >= h_old + 3, timeout=180)
        assert n1.getblockhash(h_old) != n2.getblockhash(h_old)
        self.log.info("  split persists: both branches grew to %d and %d blocks, still different at height %d",
                      n1.getblockcount(), n2.getblockcount(), h_old)

        self.log.info("Network 2: 3-second finality observation window")
        h_new = self.provoke_split(1)
        m1, m2 = self.node(1, H1), self.node(1, H2)
        t0 = time.time()
        self.wait_until(lambda: m1.getblockhash(h_new) == m2.getblockhash(h_new), timeout=30)
        self.log.info("  converged on %s at height %d after %.1f s",
                      m1.getblockhash(h_new)[:16], h_new, time.time() - t0)
        target = h_new + 3
        self.wait_until(lambda: all(self.node(1, r).getblockcount() >= target for r in range(GROUP)), timeout=180)
        tips = {self.node(1, r).getblockhash(target) for r in range(GROUP)}
        assert len(tips) == 1, "network 2 did not stay on one chain"
        self.log.info("  all six nodes on one chain at height %d", target)

        self.log.info("Network 3: certificates of unequal size for the same two blocks")
        self.unequal_certificates()

    def cert_produce(self, node, leader, n_others):
        wifs = [w for w, _ in self.cert_stakers]
        others = [w for w in wifs if w != leader][:n_others]
        for _ in range(60):
            try:
                return node.generateposblock(leader, others)
            except JSONRPCException:
                time.sleep(0.5)
        raise AssertionError("could not produce as leader")

    def free_coin(self, node):
        g = node.getblock(node.getblockhash(0), 2)
        for tx in g['tx']:
            for vo in tx['vout']:
                if vo['scriptPubKey']['hex'] == '51' and vo.get('value', 0) > 0 and node.gettxout(tx['txid'], vo['n']):
                    return tx['txid'], vo['n'], int(vo['value'] * COIN)
        raise AssertionError("no free coin")

    def unequal_certificates(self):
        # A fresh chain on network 1's nodes: stop that network, wipe four of
        # its nodes' chains, and start them as a committee of 4.
        for a, b in ((H1, Z1A), (H1, Z2A), (Z1A, Z2A), (H2, Z1B), (H2, Z2B), (Z1B, Z2B), (H1, H2)):
            try:
                self.disconnect_nodes(a, b)
            except AssertionError:
                pass   # already apart
        for role in range(GROUP):
            self.nodes[role].stop_node(wait_until_stopped=False)
        for role in range(GROUP):
            self.nodes[role].wait_until_stopped()
        plain = self.cert_common + ["-staker=%s:1" % pub for _, pub in self.cert_stakers]
        for i in range(CERT, CERT + 4):
            shutil.rmtree(os.path.join(self.nodes[i].datadir, self.chain))
        self.start_node(CERT, extra_args=plain)
        specs = []
        for wif, pub in self.cert_stakers:
            specs.append("-staker=%s:1%s" % (pub, self.nodes[CERT].getblsregistration(wif)["spec"]))
        self.stop_node(CERT)
        shutil.rmtree(os.path.join(self.nodes[CERT].datadir, self.chain))
        for i in range(CERT, CERT + 4):
            self.start_node(i, extra_args=self.cert_common + specs)
        x, y, z1, z2 = self.nodes[CERT:CERT + 4]
        for a in range(CERT, CERT + 4):
            for b in range(a + 1, CERT + 4):
                self.connect_nodes(a, b)
        leader = self.cert_stakers[0][0]
        for _ in range(2):
            self.cert_produce(x, leader, 3)
        self.sync_blocks(self.nodes[CERT:CERT + 4])
        parent_h = x.getblockcount()
        for a in range(CERT, CERT + 4):
            for b in range(a + 1, CERT + 4):
                self.disconnect_nodes(a, b)

        # One block twice, with 4 signatures on X and 3 on Y, and a sibling
        # carrying one transaction, with 4 on Z1 and 3 on Z2. The two builds of
        # a block are equal only within one second; retry until they are.
        txid, n, amt = self.free_coin(z1)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(amt - 100000, CScript([0x51])), CTxOut(100000)]
        raw = tx.serialize().hex()
        z1.sendrawtransaction(raw)
        z2.sendrawtransaction(raw)
        for attempt in range(10):
            a4 = self.cert_produce(x, leader, 3)
            a3 = self.cert_produce(y, leader, 2)
            b4 = self.cert_produce(z1, leader, 3)
            b3 = self.cert_produce(z2, leader, 2)
            if a4["hash"] == a3["hash"] and b4["hash"] == b3["hash"]:
                break
            for node, res in ((x, a4), (y, a3), (z1, b4), (z2, b3)):
                node.invalidateblock(res["hash"])
            time.sleep(1)
        else:
            raise AssertionError("the two builds of a block never matched")
        A, B = a4["hash"], b4["hash"]
        assert A != B
        assert_equal((a4["countersignatures"], a3["countersignatures"]), (4, 3))
        assert_equal((b4["countersignatures"], b3["countersignatures"]), (4, 3))
        x.submitblock(z2.getblock(B, 0))   # X: A with 4, B with 3
        y.submitblock(z1.getblock(B, 0))   # Y: A with 3, B with 4
        for name, node, want in (("X", x, (4, 3)), ("Y", y, (3, 4))):
            got = (node.getblockheader(A)["poscountersigs"], node.getblockheader(B)["poscountersigs"])
            self.log.info("  %s holds A with %d signatures and B with %d; tip %s", name, got[0], got[1],
                          "A" if node.getbestblockhash() == A else "B")
            assert_equal(got, want)
        # Same leader, so equal VRF scores: the lower hash wins, in the order of
        # uint256::operator< (the stored bytes, i.e. the displayed hex reversed).
        winner = min(A, B, key=lambda h: bytes.fromhex(h)[::-1])
        assert_equal(x.getbestblockhash(), winner)
        assert_equal(y.getbestblockhash(), winner)
        self.wait_until(lambda: x.getposfinality()["finalized_height"] == parent_h + 1 and
                        y.getposfinality()["finalized_height"] == parent_h + 1, timeout=30)
        assert_equal(x.getposfinality()["finalized_hash"], winner)
        assert_equal(y.getposfinality()["finalized_hash"], winner)

        # Let them talk and extend: they stay on one chain.
        self.connect_nodes(CERT, CERT + 1)
        self.cert_produce(x, leader, 3)
        self.sync_blocks([x, y])
        assert_equal(x.getblockhash(parent_h + 1), winner)
        assert_equal(y.getblockhash(parent_h + 1), winner)
        self.log.info("  X and Y both finalized %s at height %d and extend one chain",
                      "A" if winner == A else "B", parent_h + 1)


if __name__ == '__main__':
    PosSplitEquivocationTest().main()
