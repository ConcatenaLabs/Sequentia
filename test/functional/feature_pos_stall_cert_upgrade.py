#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A fuller certificate for a stall-escape block a node already holds is
adopted, so two nodes that received the block with different certificates
converge instead of finalizing different siblings (#282).

The certificate lies outside the block hash, so the same block can reach two
nodes with different certificates. Under a stall escape (the block's anchor at
least three parent-chain blocks past its parent's) a block is valid with fewer
signatures than the quorum, and whether it is certified is decided by the
count of the certificate the node holds. That count used to be fixed by the
first copy received.

Topology: node0 is the parent ("Bitcoin") chain; nodes 1-4 are anchored PoS
nodes X, Y, Z and W on a public BLS committee of cap 5 with four stakers
(quorum 3), never connected to each other. Z only builds blocks. After a
certified parent P the parent chain advances three blocks, so P's children may
escape a stall. One leader builds two siblings, so the block hash decides
between equally certified ones, and members sign BOTH: the equivocation the
attack needs.

  B  three signatures: certified
  S  the same block with two signatures (below the quorum) and with four

drawn until S < B by hash, so S wins wherever it is known to be certified.

  X  receives B and S with two signatures: B wins. Then the four-signature
     certificate of S arrives on gossip, within the observation window. X
     adopts it, S is certified and wins, and X finalizes S.
  Y  receives S with four signatures and B. It announces S's certificate on
     gossip by itself, and finalizes S: one chain with X.
  W  receives what X received but gets the certificate only after it has
     finalized B. It records the fuller count and stays on B: finality is
     never undone. This is the residual every quorum block has, a competing
     certificate released after the window: not specific to stall escapes.
"""
import os
import shutil
import time

from test_framework.authproxy import JSONRPCException
from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, p2p_port, rpc_port

PARENT_BLOCK_SECONDS = 600
PARENT_WARMUP_BLOCKS = 12
WINDOW_MS = 10000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def hash_order(h):
    """The order of uint256::operator<, which the comparator's hash key uses."""
    return bytes.fromhex(h)[::-1]


class msg_poscert:
    """A raw `poscert` carrying an already-serialized block header."""
    msgtype = b"poscert"

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def deserialize(self, f):
        self.payload = f.read()

    def __repr__(self):
        return "msg_poscert(%d bytes)" % len(self.payload)


class CertPeer(P2PInterface):
    """A mock peer that keeps the poscert messages it receives."""

    def __init__(self):
        super().__init__()
        self.certs = []

    def on_poscert(self, message):
        self.certs.append(message.payload)

    def on_getposprop(self, message):
        pass

    def on_inv(self, message):
        # Never fetch the announced blocks: the framework does not decode a
        # PoS block, and a failed decode drops the connection.
        pass


class PosStallCertUpgradeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        self.stakers = [make_staker() for _ in range(4)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def anchored_args(self, i):
        return [
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=5",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1", "-posfinalitydelayms=%d" % WINDOW_MS,
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % self.rpc_u, "-mainchainrpcpassword=%s" % self.rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.parent_time = self.nodes[0].getblockheader(self.parentgenesis)['time']
        self.rpc_u, self.rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

        self.add_nodes(4, [self.anchored_args(i) for i in range(1, 5)], chain=[chain] * 4)
        # The stakers' BLS registrations are derived by a running node.
        self.start_node(1, extra_args=self.anchored_args(1) + ["-staker=%s:1" % pub for _, pub in self.stakers])
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[1].getblsregistration(wif)["spec"])
                      for wif, pub in self.stakers]
        self.stop_node(1)
        shutil.rmtree(os.path.join(self.nodes[1].datadir, chain))
        for i in range(1, 5):
            self.start_node(i, extra_args=self.anchored_args(i) + self.specs)

    def advance_parent(self, blocks):
        """Parent blocks one Bitcoin interval apart, so median-time-past moves."""
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def produce(self, node, signers, leader):
        wifs = [w for w, _ in self.stakers]
        others = [w for w in wifs if w != leader][:signers]
        last = None
        for _ in range(60):
            try:
                return node.generateposblock(leader, others)
            except JSONRPCException as e:
                last = e.error
            time.sleep(0.5)
        raise AssertionError("could not produce: %s" % last)

    def bad_fork_lines(self, node):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return [line for line in f if 'bad-fork-prior-to-pos-final' in line]

    def run_test(self):
        MESSAGEMAP[b"poscert"] = msg_poscert
        _, x, y, z, w = self.nodes
        leader = self.stakers[0][0]
        all_keys = [wif for wif, _ in self.stakers]

        self.log.info("A certified parent P, shared by every node")
        self.advance_parent(PARENT_WARMUP_BLOCKS)
        for _ in range(2):
            self.produce(z, 3, leader)
        for h in range(1, z.getblockcount() + 1):
            raw = z.getblock(z.getblockhash(h), 0)
            for node in (x, y, w):
                assert node.submitblock(raw) in (None, "duplicate")
        parent = z.getbestblockhash()
        parent_height = z.getblockcount()
        for node in (x, y, w):
            assert_equal(node.getbestblockhash(), parent)
        assert_equal(z.getblockheader(parent)["poscertified"], True)
        parent_anchor = z.getblockheader(parent)["anchorheight"]

        self.log.info("The parent chain advances three blocks: P's children may escape a stall")
        self.advance_parent(3)

        # Two siblings by one leader, signed by the same members: equivocation.
        cand = {"B": [], "S": []}
        found = None
        for _ in range(20):
            for role, signers in (("B", 2), ("S", 1)):
                res = self.produce(z, signers, leader)
                cand[role].append((res, z.getblock(res["hash"], 0)))
                z.invalidateblock(res["hash"])
            for b, b_hex in cand["B"]:
                lower = [c for c in cand["S"] if hash_order(c[0]["hash"]) < hash_order(b["hash"])]
                if lower:
                    found = ((b, b_hex), lower[0])
                    break
            if found:
                break
            time.sleep(1.1)
        else:
            raise AssertionError("no siblings in the wanted hash order")
        (b, block_b), (s, block_s2) = found
        B, S = b["hash"], s["hash"]
        assert_equal((b["countersignatures"], s["countersignatures"]), (3, 2))
        assert_equal(z.getbestblockhash(), parent)
        full = z.recertifyposblock(block_s2, leader, all_keys)
        assert_equal(full["hash"], S)
        assert_equal(full["countersignatures"], 4)
        block_s4 = full["hex"]
        for h in (B, S):
            header = z.getblockheader(h)
            assert_equal(header["previousblockhash"], parent)
            assert header["anchorheight"] >= parent_anchor + 3, "not a stall escape"

        self.log.info("Y receives S with four signatures and announces its certificate")
        y_peer = y.add_p2p_connection(CertPeer())
        assert_equal(y.submitblock(block_s4), None)
        assert_equal(y.submitblock(block_b), "inconclusive")
        assert_equal(y.getbestblockhash(), S)
        header = y.getblockheader(S)
        assert_equal((header["poscountersigs"], header["poscertified"], header["poscertifiedknown"]), (4, True, True))
        self.wait_until(lambda: len(y_peer.certs) > 0, timeout=30)
        cert = y_peer.certs[0]
        assert_equal(cert.hex(), y.getblockheader(S, False))

        self.log.info("X and W receive B and S with two signatures: B wins on both")
        x_peer = x.add_p2p_connection(CertPeer())
        w_peer = w.add_p2p_connection(CertPeer())
        for node in (x, w):
            assert_equal(node.submitblock(block_b), None)
            assert_equal(node.submitblock(block_s2), "inconclusive")
            header = node.getblockheader(S)
            assert_equal((header["poscountersigs"], header["poscertified"], header["poscertifiedknown"]), (2, False, True))
            assert_equal(node.getbestblockhash(), B)

        self.log.info("Within its window X receives the fuller certificate: S is certified and wins")
        with x.assert_debug_log(["certificate of %s (height %d) upgraded from 2 to 4 member(s), now certified" % (S, parent_height + 1)]):
            x_peer.send_message(msg_poscert(cert))
            self.wait_until(lambda: x.getbestblockhash() == S, timeout=30)
        header = x.getblockheader(S)
        assert_equal((header["poscountersigs"], header["poscertified"], header["poscertifiedknown"]), (4, True, True))
        # Relayed on to X's peers, so it reaches nodes X is the only path to.
        self.wait_until(lambda: cert in x_peer.certs, timeout=30)
        # The fuller certificate is the one X now serves with the block.
        assert_equal(x.getblockheader(S, False), cert.hex())
        assert x.getblock(S, 0).startswith(cert.hex())

        self.log.info("X and Y finalize the same block, S")
        for node in (x, y):
            self.wait_until(lambda: node.getposfinality()["finalized_height"] == parent_height + 1, timeout=60)
            assert_equal(node.getposfinality()["finalized_hash"], S)

        self.log.info("W finalizes B before the certificate reaches it")
        self.wait_until(lambda: w.getposfinality()["finalized_height"] == parent_height + 1, timeout=60)
        assert_equal(w.getposfinality()["finalized_hash"], B)

        self.log.info("Late, the certificate upgrades S on W but does not undo its finality")
        with w.assert_debug_log(["certificate of %s (height %d) upgraded from 2 to 4 member(s), now certified" % (S, parent_height + 1),
                                 "bad-fork-prior-to-pos-final"]):
            w_peer.send_message(msg_poscert(cert))
            self.wait_until(lambda: w.getblockheader(S)["poscountersigs"] == 4, timeout=30)
        assert_equal(w.getblockheader(S)["poscertified"], True)
        assert_equal(w.getbestblockhash(), B)
        assert_equal(w.getposfinality()["finalized_hash"], B)

        self.log.info("After a restart X keeps the fuller certificate and its answer")
        self.restart_node(1, extra_args=self.anchored_args(1) + self.specs)
        assert_equal(x.getbestblockhash(), S)
        header = x.getblockheader(S)
        assert_equal((header["poscountersigs"], header["poscertified"], header["poscertifiedknown"]), (4, True, True))
        assert_equal(x.getposfinality()["finalized_hash"], S)

        self.log.info("Connected and extended, X and Y stay on one chain")
        self.connect_nodes(1, 2)
        self.produce(y, 3, leader)
        self.sync_blocks([x, y])
        for node in (x, y):
            assert_equal(node.getblockhash(parent_height + 1), S)
            assert_equal(self.bad_fork_lines(node), [])


if __name__ == '__main__':
    PosStallCertUpgradeTest().main()
