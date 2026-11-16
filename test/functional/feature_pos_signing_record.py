#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The signing record in a gossip committee (doc/sequentia/04-proof-of-stake.md,
"The signing record"), on a chain without anchors.

node0 holds one staker of weight 9: 3 of 4 seats, a quorum by itself. node1
holds three stakers of weight 1. node2 stakes nothing and watches.

holder: node0 is killed (SIGKILL, no flush) the moment it certifies a block X
  at height H, and restarts with no peers. Its block index may not know the
  blocks it signed last, but its signing record does: alone, it signs no rival
  at any height it signed (before the record it certified a rival of X and the
  chain split, Sequentia #304). Reconnected, it follows its peers' chain, and
  no node holds a certified block off the active chain.

fail: the record of node1 cannot be written (a directory stands where the file
  goes). node1 releases no share it has not recorded, and shuts down with
  "Failed to write the signing record". Every share of node1's keys that a
  peer received names a block node1 recorded first.
"""

import os
import re
import struct
import time
from io import BytesIO

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

ISOLATED_S = 5
HOLDER_TRIES = int(os.environ.get("SIGNREC_HOLDER_TRIES", "3"))


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class LogTail:
    def __init__(self, node):
        self.f = open(node.debug_log_path, encoding='utf-8', errors='replace')
        self.f.seek(0, os.SEEK_END)
        self.buf = ""

    def wait(self, pattern, timeout=120):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.f.read()
            if chunk:
                self.buf += chunk
                lines = self.buf.split("\n")
                self.buf = lines.pop()
                for ln in lines:
                    m = rx.search(ln)
                    if m:
                        return m
            else:
                time.sleep(0.003)
        raise AssertionError("no log line matching %r in %d s" % (pattern, timeout))

    def close(self):
        self.f.close()


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


class PosSigningRecordTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.big = make_staker()
        self.small = [make_staker() for _ in range(3)]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=4", "-posexpraceheight=1",
            "-posslotinterval=1", "-par=1", "-debug=validation",
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        idle = self.common + ["-staker=%s:9" % self.big[1]] + ["-staker=%s:1" % p for _, p in self.small]
        self.extra_args = [list(idle) for _ in range(self.num_nodes)]

    def setup_network(self):
        self.setup_nodes()

    def args_for(self, i):
        if i == 0:
            return self.full + ["-posproducer", "-posproducerkey=%s" % self.big[0]]
        if i == 1:
            return self.full + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in self.small]
        return list(self.full)

    def mesh(self):
        for i in range(self.num_nodes):
            for j in range(i + 1, self.num_nodes):
                if self.nodes[i].running and self.nodes[j].running:
                    self.connect_nodes(i, j)

    def sigkill(self, i):
        n = self.nodes[i]
        n.process.kill()
        n.process.wait()
        n.running = False
        n.process = None
        n.rpc_connected = False
        n.rpc = None

    def certified_rivals(self):
        rivals = []
        for i, n in enumerate(self.nodes):
            if not n.running:
                continue
            for t in n.getchaintips():
                if t["status"] == "active":
                    continue
                if n.getblockheader(t["hash"]).get("poscertified"):
                    rivals.append((i, t["height"], t["hash"][:16], t["status"]))
        return rivals

    def run_test(self):
        n0 = self.nodes[0]
        specs = ["-staker=%s:9%s" % (self.big[1], n0.getblsregistration(self.big[0])["spec"])]
        specs += ["-staker=%s:1%s" % (p, n0.getblsregistration(w)["spec"]) for w, p in self.small]
        self.full = self.common + specs
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.args_for(i))
        self.mesh()
        self.wait_until(lambda: self.nodes[2].getblockcount() >= 3, timeout=120)
        assert_equal(n0.getblockheader(n0.getblockhash(3))["posquorum"], 3)
        self.case_holder()
        self.case_fail()

    def case_holder(self):
        n0, n2 = self.nodes[0], self.nodes[2]
        for it in range(HOLDER_TRIES):
            tail = LogTail(n0)
            m = tail.wait(r"(?:PoS gossip: certified|PoS producer: created) block ([0-9a-f]{64}) at height (\d+)")
            self.sigkill(0)
            tail.close()
            x, h = m.group(1), int(m.group(2))
            # What the peers hold now: node0's seats are in every quorum, so every
            # certified block they hold carries node0's signature.
            time.sleep(0.5)
            peer_chain = {k: n2.getblockhash(k) for k in range(1, n2.getblockcount() + 1)}
            self.start_node(0, extra_args=self.args_for(0))
            restart_tip = n0.getblockcount()
            held = [b for b in n0.getsigningrecord()["held"] if b["hash"] == x]
            self.log.info("holder %d: killed after certifying %s at %d; restarted at tip %d; peers at %d" %
                          (it, x[:16], h, restart_tip, max(peer_chain) if peer_chain else 0))
            time.sleep(ISOLATED_S)
            top = n0.getblockcount()
            for k in range(restart_tip + 1, top + 1):
                mine = n0.getblockhash(k)
                want = x if k == h else peer_chain.get(k)
                assert want is None or mine == want, \
                    "holder %d: alone, node0 certified %s at height %d, a rival of %s it signed" % (it, mine[:16], k, want[:16])
            self.log.info("holder %d: alone %d s, node0 at %d: no rival of a block it signed" % (it, ISOLATED_S, top))
            if restart_tip < h:
                assert held and held[0]["restored"], "the record restored no share-lock: %s" % held
            self.mesh()
            self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=120)
            self.sync_blocks(timeout=60)
            if h <= n2.getblockcount() and peer_chain.get(h):
                assert_equal(n2.getblockhash(h), peer_chain[h])
            rivals = self.certified_rivals()
            assert not rivals, "certified blocks off the active chain: %s" % rivals
            self.log.info("holder %d: converged at %d, no certified rival anywhere" % (it, n2.getblockcount()))

    def case_fail(self):
        n1 = self.nodes[1]
        small_keys = {p for _, p in self.small}
        peer = n1.add_p2p_connection(ShareCollector())
        path = os.path.join(n1.datadir, n1.chain, "possigned.dat")
        self.wait_until(lambda: os.path.exists(path), timeout=60)
        h0 = n1.getblockcount()
        self.wait_until(lambda: n1.getblockcount() >= h0 + 2, timeout=60)
        # node0 holds a quorum alone and may certify before node1's round comes:
        # wait for a share of node1's keys, so that what reached the peer is
        # checked against the record below.
        self.wait_until(lambda: any(p in small_keys for _, p in peer.shares), timeout=60)
        os.rename(path, path + ".moved")
        os.mkdir(path)
        self.log.info("fail: a directory stands where node1's record goes, at height %d" % n1.getblockcount())
        n1.process.wait(timeout=120)
        rc = n1.process.returncode
        n1.running = False
        n1.process = None
        n1.rpc_connected = False
        n1.rpc = None
        with open(n1.debug_log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        assert "Failed to write the signing record" in text, "node1 did not shut down for its record"
        assert "cannot write the signing record" in text, "node1 never tried to sign after the failure"
        recorded = set(re.findall(r"Signing record: kind \d+, block ([0-9a-f]{64})", text))
        mine = [(b, p) for b, p in peer.shares if p in small_keys]
        assert mine, "the peer received no share of node1's keys at all"
        unrecorded = [b for b, _ in mine if b not in recorded]
        assert not unrecorded, "node1 released shares it had not recorded: %s" % unrecorded
        self.log.info("fail: node1 stopped (rc %s); %d share(s) of its keys reached the peer, every one recorded first" %
                      (rc, len(mine)))
        os.rmdir(path)
        os.rename(path + ".moved", path)
        # Leave the framework a node it can stop.
        self.start_node(1, extra_args=self.args_for(1))


if __name__ == '__main__':
    PosSigningRecordTest().main()
