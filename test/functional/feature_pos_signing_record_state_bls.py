#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""What a certified block binds is read from the node's state when the check
runs, for a certificate that reaches the node as a certificate message, before
or without its block (doc/sequentia/04-proof-of-stake.md, "The signing
record"). The other orders are in feature_pos_signing_record_state.py.

A public BLS committee of four seats, one key each, every block made by RPC:
node0 is the parent chain, node1 (M) holds every key and has no producer
thread, node2 is a peer that makes the blocks M hears of only as a header or
a certificate. A mock peer hands M the certificate message.

late: M holds only X's header; the operator invalidates X; then X's verified
  certificate arrives. The certificate does not undo the operator's repair: a
  rival at X's height is refused for one grace round, then made.
nowatcher: M runs without the anchor watcher. A verified certificate for X
  arrives, X's body never does, and Bitcoin then replaces X's anchor. A rival
  at X's height is made at once: on a node that does not watch Bitcoin a
  certified block binds only while it is on the active chain.
"""

import os
import shutil
import time

from feature_pos_signing_record_rpc import make_key
from feature_pos_signing_record_state import made_at_once, released_after_one_round
from test_framework.authproxy import JSONRPCException
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, p2p_port, rpc_port

CHAIN = "elementsregtest"


class msg_raw:
    """A message carried as raw bytes (the framework does not decode PoS ones)."""
    msgtype = b""

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def deserialize(self, f):
        self.payload = f.read()

    def __repr__(self):
        return "%s(%d bytes)" % (self.msgtype.decode(), len(self.payload))


class msg_poscert(msg_raw):
    msgtype = b"poscert"


class msg_getposcert(msg_raw):
    msgtype = b"getposcert"


class CertPeer(P2PInterface):
    """A peer that sends certificates, answers no query and fetches nothing."""

    def on_getposcert(self, message):
        pass

    def on_poscert(self, message):
        pass

    def on_inv(self, message):
        pass


class PosSigningRecordStateBlsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.stakers = [make_key() for _ in range(4)]

    def add_options(self, parser):
        parser.add_argument("--only", choices=("late", "nowatcher"), help="run one case only")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def args(self, i, watcher=True):
        return [
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=5",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1", "-posfinalitydelayms=60000", "-debug=validation",
            "-con_bitcoin_anchor=1", "-validateanchor=%d" % watcher, "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % self.rpc_u, "-mainchainrpcpassword=%s" % self.rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ] + self.specs

    def setup_network(self):
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-signblockscript=51"]], chain=[CHAIN])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.rpc_u, self.rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), CHAIN)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_mine(3)
        # The stakers' BLS registrations are derived by a running node.
        self.specs = ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.add_nodes(2, [self.args(i) for i in (1, 2)], chain=[CHAIN] * 2)
        self.start_node(1)
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[1].getblsregistration(wif)["spec"]) for wif, pub in self.stakers]
        self.stop_node(1)
        shutil.rmtree(os.path.join(self.nodes[1].datadir, CHAIN))
        for i in (1, 2):
            self.start_node(i, extra_args=self.args(i))
        self.connect_nodes(1, 2)

    def parent_mine(self, n):
        p = self.nodes[0]
        return self.generatetoaddress(p, n, p.getnewaddress(), sync_fun=self.no_op)

    def produce(self, node, bound=None):
        """A block with every key, by whichever key the slot lets lead. A
        refusal by the signing check (it names `bound`) is the caller's."""
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(60):
            for leader in wifs:
                try:
                    return node.generateposblock(leader, [w for w in wifs if w != leader])
                except JSONRPCException as e:
                    if bound and bound in e.error["message"]:
                        raise
                    last = e.error
            time.sleep(0.5)
        raise AssertionError("could not produce: %s" % last)

    def held(self, node, x):
        b = [b for b in node.getsigningrecord()["held"] if b["hash"] == x]
        return b[0] if b else None

    def send_certificate(self, m, header_hex):
        peer = m.add_p2p_connection(CertPeer())
        peer.send_message(msg_poscert(bytes.fromhex(header_hex)))

    def resync(self, x):
        """node2 drops X, so that it takes M's chain."""
        self.nodes[2].invalidateblock(x)
        self.connect_nodes(1, 2)
        self.sync_blocks(self.nodes[1:])

    def run_test(self):
        MESSAGEMAP[b"poscert"] = msg_poscert
        MESSAGEMAP[b"getposcert"] = msg_getposcert
        m = self.nodes[1]
        for _ in range(2):
            assert self.produce(m)["countersignatures"] >= 2
        self.sync_blocks(self.nodes[1:])
        only = self.options.only
        if only in (None, "late"):
            self.case_late()
        if only in (None, "nowatcher"):
            self.case_nowatcher()

    def case_late(self):
        m, peer = self.nodes[1], self.nodes[2]
        self.disconnect_nodes(1, 2)
        x = self.produce(peer)["hash"]
        h = peer.getblockheader(x)["height"]
        assert_equal(h, m.getblockcount() + 1)
        assert peer.getblockheader(x)["poscertified"]
        header = peer.getblockheader(x, False)
        m.submitheader(header)
        m.invalidateblock(x)
        assert not self.held(m, x), "late: a header alone is held"
        self.send_certificate(m, header)
        self.wait_until(lambda: self.held(m, x) is not None, timeout=30)
        self.log.info("late: M had X=%s's header at %d, invalidated it, then received its certificate; the check holds: %s" %
                      (x[:16], h, self.held(m, x)))
        time.sleep(1.1)  # a rival, not X again: a template of another second
        released_after_one_round(self.log, "late", "a rival at X's height (generateposblock)",
                                 lambda k: self.produce(m, bound=x), x)
        assert m.getblockcount() == h and m.getbestblockhash() != x
        m.disconnect_p2ps()
        self.resync(x)

    def case_nowatcher(self):
        m, peer, parent = self.nodes[1], self.nodes[2], self.nodes[0]
        self.stop_node(1)
        self.start_node(1, extra_args=self.args(1, watcher=False))
        # X on the peer, anchored on a Bitcoin block above the anchor of M's tip,
        # so that replacing X's anchor leaves M's tip standing.
        for _ in range(8):
            self.connect_nodes(1, 2)
            self.sync_blocks(self.nodes[1:])
            self.disconnect_nodes(1, 2)
            tip_anchor = m.getblockheader(m.getbestblockhash())["anchorheight"]
            self.parent_mine(1)
            time.sleep(2.5)  # the peer's watcher sees the new Bitcoin block
            x = self.produce(peer)["hash"]
            hdr = peer.getblockheader(x)
            if hdr["anchorheight"] > tip_anchor:
                break
        else:
            raise AssertionError("nowatcher: could not anchor X above M's tip")
        h = hdr["height"]
        assert_equal(h, m.getblockcount() + 1)
        self.send_certificate(m, peer.getblockheader(x, False))
        self.wait_until(lambda: self.held(m, x) is not None, timeout=30)
        self.log.info("nowatcher: M received X=%s's certificate at %d, anchored at %d, and never its body; the check holds: %s" %
                      (x[:16], h, hdr["anchorheight"], self.held(m, x)))
        # Bitcoin replaces X's anchor.
        parent.invalidateblock(parent.getblockhash(hdr["anchorheight"]))
        self.parent_mine(3)
        assert parent.getblockhash(hdr["anchorheight"]) != hdr["anchorhash"]
        time.sleep(1.1)
        r = made_at_once(self.log, "nowatcher", "a rival at X's height (generateposblock)", lambda: self.produce(m, bound=x))
        assert_equal(m.getblockheader(r["hash"])["height"], h)
        m.disconnect_p2ps()
        self.stop_node(1)
        self.start_node(1, extra_args=self.args(1))
        self.resync(x)


if __name__ == "__main__":
    PosSigningRecordStateBlsTest().main()
