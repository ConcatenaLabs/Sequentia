#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A competing quorum certificate holds finality until its block is judged.

The finality observation window (feature_pos_split_equivocation) only has to
let a ~300-byte certificate travel. The competing BLOCK can be large, full of
transactions nobody has seen, and slow to download and validate — and its
content is chosen by the attacker. So a node that has verified a full-quorum
certificate for a block at the same height as the one it is about to finalize
keeps that height (and everything above it) undecided until it has received and
judged the competing block, or until -posfinalityholdms expires.

Same split as feature_pos_split_equivocation (committee 4, two Byzantine keys
each on two nodes), plus an observer O that runs no producer and is connected
only to half A. The test hands O the certificate of half B's block through a
P2P connection:
  1. the body is served when O asks for it: O judges it at once and the height
     settles long before the hold would expire;
  2. the body is never served: O keeps the height undecided for the whole hold,
     even while half A extends the chain, then finalizes its own block.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework import p2p
from test_framework.p2p import P2PInterface
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal

H1, H2, Z1A, Z2A, Z1B, Z2B, OBS = range(7)
HOLD_MS = 15000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class msg_raw:
    """A message whose payload is already serialized (from RPC hex)."""
    __slots__ = ("msgtype", "data")

    def __init__(self, msgtype, data):
        self.msgtype = msgtype
        self.data = data

    def serialize(self):
        return self.data


class msg_pos_gossip:
    """Committee gossip relayed to the test peer, which ignores it."""
    __slots__ = ()

    def deserialize(self, f):
        f.read()


for _t in (b"poscert", b"posproposal", b"poscmpctprop", b"posshare", b"getposcert", b"getposprop"):
    p2p.MESSAGEMAP[_t] = type("msg_" + _t.decode(), (msg_pos_gossip,), {"msgtype": _t})


class CertPeer(P2PInterface):
    """Delivers a certificate; serves a block body only when allowed."""
    def __init__(self):
        super().__init__()
        self.body = {}      # hash -> raw block bytes this peer will serve
        self.asked = set()

    def on_message(self, message):
        if isinstance(message, msg_pos_gossip):
            return
        super().on_message(message)

    def on_getdata(self, message):
        for inv in message.inv:
            h = '%064x' % inv.hash
            self.asked.add(h)
            if h in self.body:
                self.send_message(msg_raw(b"block", self.body[h]))


class PosFinalityHoldTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 7
        self.setup_clean_chain = True
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
        for role in range(7):
            if role == OBS:
                self.extra_args.append(common + ["-posfinalityholdms=%d" % HOLD_MS])
            else:
                self.extra_args.append(common + ["-posproducer", "-posproducerkey=%s" % self.stakers[key_of[role]][0]])

    def setup_network(self):
        self.setup_nodes()
        for a, b in ((H1, Z1A), (H1, Z2A), (Z1A, Z2A), (H2, Z1B), (H2, Z2B), (Z1B, Z2B),
                     (H1, H2), (H1, OBS)):
            self.connect_nodes(a, b)

    def split(self):
        """Partition the honest members until each half certifies a different
        block at one height. Returns (height, half A's block, half B's block);
        the halves are left apart."""
        h1, h2 = self.nodes[H1], self.nodes[H2]
        for attempt in range(1, 21):
            self.wait_until(lambda: h1.getblockcount() >= 3 and
                            h1.getbestblockhash() == h2.getbestblockhash(), timeout=180)
            self.disconnect_nodes(H1, H2)
            target = max(h1.getblockcount(), h2.getblockcount()) + 1
            self.wait_until(lambda: h1.getblockcount() >= target and h2.getblockcount() >= target, timeout=120)
            a, b = h1.getblockhash(target), h2.getblockhash(target)
            if a != b:
                self.log.info("  split at height %d (attempt %d)", target, attempt)
                return target, a, b
            self.connect_nodes(H1, H2)
        raise AssertionError("no split obtained")

    def has_body(self, n, blockhash):
        try:
            self.nodes[n].getblock(blockhash, 0)
            return True
        except JSONRPCException:
            return False

    def realign(self, h, a, b):
        """Join the halves again on the block O chose at height h."""
        winner = self.nodes[OBS].getblockhash(h)
        loser = b if winner == a else a
        for r in ((H2, Z1B, Z2B) if loser == b else (H1, Z1A, Z2A)):
            self.nodes[r].invalidateblock(loser)
        self.connect_nodes(H1, H2)
        self.wait_until(lambda: self.nodes[H1].getblockhash(h) == self.nodes[H2].getblockhash(h) == winner, timeout=60)

    def give_certificate(self, peer, h, a, b):
        obs, h2 = self.nodes[OBS], self.nodes[H2]
        self.wait_until(lambda: obs.getblockcount() >= h and obs.getblockhash(h) == a, timeout=30)
        peer.send_message(msg_raw(b"poscert", bytes.fromhex(h2.getblockheader(b, False))))
        return time.time()

    def run_test(self):
        obs = self.nodes[OBS]

        self.log.info("1) The competing block arrives when O asks for it")
        peer = obs.add_p2p_connection(CertPeer())
        h, a, b = self.split()
        peer.body[b] = bytes.fromhex(self.nodes[H2].getblock(b, 0))
        t0 = self.give_certificate(peer, h, a, b)
        self.wait_until(lambda: b in peer.asked, timeout=10)
        # O now holds both blocks; the deterministic comparator decides and the
        # height settles well before the hold could expire.
        self.wait_until(lambda: obs.getposfinality()["finalized_height"] >= h and
                        not obs.getposfinality()["held_by"], timeout=10)
        known = {t["hash"] for t in obs.getchaintips()} | {obs.getblockhash(h)}
        assert a in known and b in known, "O did not end up holding both blocks"
        self.log.info("  O judged the competing block and settled height %d after %.1f s, on %s",
                      h, time.time() - t0, "A" if obs.getblockhash(h) == a else "B")
        # The halves themselves were kept apart beyond their own window, so each
        # finalized its own block (the straggler case, healed in production by
        # reconciliation). Realign them by hand on O's choice for the next round.
        self.realign(h, a, b)
        obs.disconnect_p2ps()

        self.log.info("2) The competing block never arrives")
        for attempt in range(1, 6):
            peer = obs.add_p2p_connection(CertPeer())
            h, a, b = self.split()
            t0 = self.give_certificate(peer, h, a, b)
            # A Byzantine key runs on a node in each half. When it leads half
            # B's block, its twin in half A builds the very same block, so half
            # A already holds the body and adopts it as soon as O relays the
            # certificate: the block does arrive, and this round proves nothing.
            self.wait_until(lambda: any(x["hash"] == b for x in obs.getposfinality()["held_by"]) or
                            self.has_body(OBS, b), timeout=5)
            if not self.has_body(OBS, b):
                break
            self.log.info("  half A already held block %s (attempt %d); splitting again", b[:16], attempt)
            self.wait_until(lambda: obs.getposfinality()["finalized_height"] >= h, timeout=30)
            self.realign(h, a, b)
            obs.disconnect_p2ps()
        else:
            raise AssertionError("half A held the competing block in every round")
        # Half A keeps extending the chain; finality must not move up to h.
        self.wait_until(lambda: obs.getblockcount() >= h + 1, timeout=60)
        time.sleep(max(0, t0 + 8 - time.time()))
        info = obs.getposfinality()
        assert info["finalized_height"] < h, info
        assert b in peer.asked, "O did not ask for the competing block"
        self.log.info("  after %.0f s O is still undecided at %d (finalized %d, tip %d)",
                      time.time() - t0, h, info["finalized_height"], obs.getblockcount())
        # The hold expires: O stops waiting and finalizes its own branch.
        self.wait_until(lambda: obs.getposfinality()["finalized_height"] >= h, timeout=HOLD_MS / 1000 + 15)
        assert_equal(obs.getblockhash(h), a)
        self.log.info("  hold expired after %.1f s: O finalized its own block", time.time() - t0)


if __name__ == '__main__':
    PosFinalityHoldTest().main()
