#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A gossip collection whose proposals all lose their anchor still runs out and restarts.

node0 is the parent chain ("Bitcoin"). nodes 1-4 are a gossip committee of four
seats, one each, quorum 3. Nodes 3 and 4 stop, so no round at height H
certifies, and node1 (M) and node2 keep proposing and signing there, every
proposal anchored on the parent's tip. Then the parent replaces its top two
blocks: every proposal held at H is anchored on a block Bitcoin no longer has,
and is left aside.

The collection must still run out and restart, so that M proposes and signs a
new block at H on the parent's new branch without waiting for another parent
block (the parent does not move while the test waits). Before the fix the
collection had no schedule once no candidate could be backed: every member
stayed silent at H until the next parent block (Sequentia #411). Then nodes 3
and 4 return and H is certified, with the parent still unmoved.
"""

import os
import struct
import time
from io import BytesIO

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import get_auth_cookie, get_datadir_path, p2p_port, rpc_port

M = 1
COMMITTEE = 4
ROUND_MS = 700 + 35 * COMMITTEE
COLLECTION_S = (500 + 25 * COMMITTEE + COMMITTEE * ROUND_MS) / 1000.0


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def read_compact_size(f):
    n = f.read(1)[0]
    if n < 253:
        return n
    if n == 253:
        return struct.unpack("<H", f.read(2))[0]
    if n == 254:
        return struct.unpack("<I", f.read(4))[0]
    return struct.unpack("<Q", f.read(8))[0]


class ShareCollector(P2PInterface):
    """A peer that frames every message and decodes only the handshake's, pings and `posshare`
    (block hash, then the signer's key): the test framework cannot parse Sequentia's other gossip."""
    KEEP = {b"version", b"verack", b"ping", b"pong", b"sendheaders", b"wtxidrelay", b"sendaddrv2",
            b"feefilter", b"sendcmpct"}

    def __init__(self):
        super().__init__()
        self.shares = []

    def _on_data(self):
        while len(self.recvbuf) >= 24:
            msgtype = self.recvbuf[4:16].split(b"\x00", 1)[0]
            msglen = struct.unpack("<i", self.recvbuf[16:20])[0]
            if len(self.recvbuf) < 24 + msglen:
                return
            msg = self.recvbuf[24:24 + msglen]
            self.recvbuf = self.recvbuf[24 + msglen:]
            if msgtype == b"posshare":
                f = BytesIO(msg)
                block_hash = f.read(32)[::-1].hex()
                pubkey = f.read(read_compact_size(f)).hex()
                self.shares.append((block_hash, pubkey))
            elif msgtype in self.KEEP and msgtype in MESSAGEMAP:
                t = MESSAGEMAP[msgtype]()
                t.deserialize(BytesIO(msg))
                self.on_message(t)


class PosGossipAnchorReorgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(COMMITTEE)]

    def setup_network(self):
        chain = "elementsregtest"
        self.chain_name = chain
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-signblockscript=51"]], chain=[chain])
        self.start_node(0)
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        common = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posexpraceheight=1", "-posslotinterval=1",
            "-con_max_block_sig_size=8000", "-con_blocksubsidy=5000000000", "-par=1", "-debug=validation",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            # Keep the escaping stall (a sub-quorum block after a parent median-time gap) out of reach.
            "-posescapestallmtpgap=1000000", "-mainchainrpctimeout=5",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.nodes[0].getblockhash(0),
        ]
        base = common + ["-staker=%s:1" % p for _, p in self.stakers]
        self.add_nodes(COMMITTEE, [base + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)] for i in range(1, 5)],
                       chain=[chain] * COMMITTEE)
        self.start_node(1)
        specs = ["-staker=%s:1%s" % (p, self.nodes[1].getblsregistration(w)["spec"]) for w, p in self.stakers]
        self.full = common + specs
        self.stop_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_mine(3)
        for i in range(1, 5):
            self.start_node(i, extra_args=self.args_for(i))
        self.mesh()

    def args_for(self, i):
        return self.full + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                            "-posproducer=1", "-posproducerkey=%s" % self.stakers[i - 1][0]]

    def parent_mine(self, n):
        p = self.nodes[0]
        return self.generatetoaddress(p, n, p.getnewaddress(), sync_fun=self.no_op)

    def mesh(self):
        up = [i for i in range(1, 5) if self.nodes[i].running]
        for a in up:
            for b in up:
                if a < b:
                    self.connect_nodes(a, b)

    def log_since(self, i, pos):
        with open(self.nodes[i].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(pos)
            return f.read()

    def m_signed(self, col, seen):
        mpub = self.stakers[M - 1][1]
        return [bh for bh, pk in list(col.shares) if pk == mpub and bh not in seen]

    def run_test(self):
        m, p = self.nodes[M], self.nodes[0]
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= 3, timeout=120)
        for i in (3, 4):
            self.stop_node(i)
        time.sleep(2)
        h = m.getblockcount() + 1
        col = m.add_p2p_connection(ShareCollector())

        self.log.info("M and node2 sign at H=%d with every proposal anchored on the parent's tip" % h)
        self.parent_mine(2)
        top = p.getblockcount()
        time.sleep(2 * COLLECTION_S + 2)  # every candidate now proposed on the parent's present tip
        seen = set(self.m_signed(col, set()))
        self.wait_until(lambda: self.m_signed(col, seen), timeout=60)
        assert m.getblockcount() + 1 == h
        # Replace the anchors in the middle of a collection, once its proposals are all in: a
        # proposal made after the replacement would carry the new branch's anchor and stay valid.
        mark = os.path.getsize(self.nodes[M].debug_log_path)
        self.wait_until(lambda: "proposing block" in self.log_since(M, mark), timeout=60)
        time.sleep(1.0)

        self.log.info("The parent replaces its top two blocks: every proposal at H loses its anchor")
        pos = [os.path.getsize(self.nodes[i].debug_log_path) for i in range(5)]
        p.invalidateblock(p.getblockhash(top - 1))
        self.parent_mine(4)
        parent_tip = p.getbestblockhash()
        t0 = time.time()
        # A share M made on the old branch just before it saw the reorganization may still be on its
        # way: one round later, whatever M signs next is a block proposed after it.
        time.sleep(ROUND_MS / 1000.0 + 0.5)
        seen = set(self.m_signed(col, set()))
        wait_s = 4 * COLLECTION_S + 4
        while time.time() < t0 + wait_s and not self.m_signed(col, seen):
            time.sleep(0.05)
        signed = self.m_signed(col, seen)
        text = self.log_since(M, pos[M])
        left_aside = text.count("leaving aside proposal")
        self.log.info("M left %d proposal(s) aside after the reorganization (anchor-stale: %d)" % (left_aside, text.count("anchor-stale")))
        assert p.getbestblockhash() == parent_tip, "the parent moved while the test waited"
        assert signed, ("M signed nothing at H=%d for %.0f s after Bitcoin replaced the anchors of every proposal there, "
                        "with the parent unmoved: the collection never ran out (Sequentia #411)" % (h, wait_s))
        assert left_aside and "anchor-stale" in text, "the case was not reached: M left no proposal aside for its anchor"
        assert "restarting collection" in text
        self.log.info("M left the stale proposals aside, restarted its collection and signed %s at H %.1f s after the reorganization"
                      % (signed[0][:16], time.time() - t0))

        self.log.info("Nodes 3 and 4 return: H is certified, the parent still unmoved")
        for i in (3, 4):
            self.start_node(i, extra_args=self.args_for(i))
        self.mesh()
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= h, timeout=120)
        assert p.getbestblockhash() == parent_tip, "the parent moved while the test waited"
        hashes = {self.nodes[i].getblockhash(h) for i in range(1, 5)}
        assert len(hashes) == 1, "the members disagree at H: %s" % hashes


if __name__ == '__main__':
    PosGossipAnchorReorgTest().main()
