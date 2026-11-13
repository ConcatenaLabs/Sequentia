#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The anchor watcher revives only what Bitcoin's anchors allow.

When Bitcoin returns to a branch it showed before, the watcher reconsiders the
Sequentia blocks it had invalidated for that branch. Blocks above them whose own
anchor is a Bitcoin block that is still off Bitcoin's best chain must stay
invalid: no such block may be connected, not even for the moment before the
watcher would disconnect it again. They come back when their own anchor does.

Two shapes, both with Bitcoin blocks N (two) and P on top of N; Sequentia blocks
C anchored on N and D anchored on P:
  (a) Bitcoin replaces P, then N, then returns to N without P. The watcher had
      invalidated D and C separately.
  (b) The same, but the node is down while Bitcoin replaces P and N, so the
      watcher invalidates only C and D is failed as C's descendant.
In both, the node must return to C's top without any block of D ever being
connected (read from the node's own log: no UpdateTip names a block of D), and
then to D's top once Bitcoin returns to P.

Topology: node0 = parent ("Bitcoin"); node1 = the producer (a one-member
committee); node2 = the validator under test. The producer is stopped before the
parent moves, so the only way blocks can come back is the validator's watcher.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


SEED_STAKE = 1000000000
STAKE_CSV = 15


class AnchorReconsiderTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.founder_wif, self.founder_pub = make_key()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

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
        parentgenesis = self.nodes[0].getblockhash(0)
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        consensus = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=1", "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1", "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % parentgenesis,
        ]
        self.founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                         "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        self.validator_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2)]
        self.add_nodes(1, [self.founder_args], chain=[chain])
        self.start_node(1)
        self.add_nodes(1, [self.validator_args], chain=[chain])
        self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    # ------------------------------------------------------------------ helpers
    def mine_parent(self, n):
        return self.generatetoaddress(self.nodes[0], n, self.nodes[0].getnewaddress(), sync_fun=self.no_op)

    def anchored_on(self, node, anchors):
        """The active-chain blocks of node anchored on any of the parent blocks `anchors`, lowest first."""
        out = []
        for h in range(node.getblockcount(), 0, -1):
            b = node.getblockheader(node.getblockhash(h))
            if b["anchorhash"] in anchors:
                out.append(b["hash"])
            elif out:
                break
        return out[::-1]

    def produce_on(self, anchors, count):
        """Wait until the producer has `count` blocks anchored on `anchors` at its tip."""
        founder = self.nodes[1]
        self.wait_until(lambda: len(self.anchored_on(founder, anchors)) >= count, timeout=120)
        return self.anchored_on(founder, anchors)

    def updatetips_since(self, size, hashes):
        with open(self.nodes[2].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(size)
            return [line.strip() for line in f
                    if "UpdateTip: new best=" in line and any(h in line for h in hashes)]

    def log_lines_since(self, size, text):
        with open(self.nodes[2].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(size)
            return [line.strip() for line in f if text in line]

    def build_c_and_d(self):
        """With the producer running: Bitcoin blocks N1, N2 carry C, P on N2 carries D. Stops the producer."""
        parent, founder, validator = self.nodes
        n = self.mine_parent(2)
        c = self.produce_on(n, 2)
        p = self.mine_parent(1)[0]
        d = self.produce_on([p], 2)
        c = self.anchored_on(founder, n)
        self.wait_until(lambda: validator.getbestblockhash() == founder.getbestblockhash(), timeout=60)
        self.stop_node(1)
        self.log.info("C: %d block(s) on N (heights %d-%d); D: %d block(s) on P (heights %d-%d)",
                      len(c), validator.getblockheader(c[0])["height"], validator.getblockheader(c[-1])["height"],
                      len(d), validator.getblockheader(d[0])["height"], validator.getblockheader(d[-1])["height"])
        return n, p, c, d

    def replace_p_and_n(self, n, p):
        """Bitcoin replaces P by two blocks P'1, P'2 on N2, then N by a longer branch. Returns (P'1, alt1)."""
        parent = self.nodes[0]
        parent.invalidateblock(p)
        p_alt = self.mine_parent(2)
        parent.invalidateblock(n[0])
        alt = self.mine_parent(5)
        return p_alt[0], alt[0]

    def return_to_n(self, n, alt1):
        """Bitcoin returns to N, with P'1, P'2 above it and P still off its best chain."""
        parent = self.nodes[0]
        parent.invalidateblock(alt1)
        parent.reconsiderblock(n[0])
        self.wait_until(lambda: parent.getblockhash(parent.getblockheader(n[1])["height"]) == n[1], timeout=30)

    def check_return(self, c, d, p, size):
        """The validator is back on C's top; no block of D was connected on the way; D stays invalid."""
        validator = self.nodes[2]
        self.wait_until(lambda: validator.getbestblockhash() == c[-1], timeout=60)
        # Let the watcher run several more passes (-anchorpollinterval=1).
        self.wait_until(lambda: len(self.log_lines_since(size, "Anchor watcher: block ")) > 0 or
                        validator.getbestblockhash() != c[-1], timeout=30)
        connected = self.updatetips_since(size, d)
        for line in connected:
            self.log.info("  connected with an orphaned anchor: %s", line[:160])
        assert not connected, "%d block(s) of D, anchored on a Bitcoin block off the best chain, were connected" % len(connected)
        stays = self.log_lines_since(size, "stays invalid")
        assert any(d[0] in line for line in stays), "no 'stays invalid' line for D's first block"
        assert_equal(validator.getbestblockhash(), c[-1])
        tips = {t["hash"]: t["status"] for t in validator.getchaintips()}
        assert_equal(tips.get(d[-1]), "invalid")
        self.log.info("  back on C's top %s; no block of D connected; D stays invalid (%s)", c[-1][:16], stays[0].split("] ", 3)[-1][:120])

    def check_p_returns(self, d, p_alt1):
        """Bitcoin returns to P: the watcher revives D."""
        parent, _, validator = self.nodes
        parent.invalidateblock(p_alt1)
        self.wait_until(lambda: validator.getbestblockhash() == d[-1], timeout=60)
        self.log.info("  Bitcoin back on P: D is connected again, tip %s", d[-1][:16])

    # --------------------------------------------------------------------- test
    def run_test(self):
        parent, founder, validator = self.nodes
        self.mine_parent(6)
        self.wait_until(lambda: founder.getblockcount() >= 2, timeout=120)

        self.log.info("(a) D and C invalidated one after the other; Bitcoin returns to N without P")
        n, p, c, d = self.build_c_and_d()
        parent.invalidateblock(p)
        p_alt = self.mine_parent(2)
        self.wait_until(lambda: validator.getbestblockhash() == c[-1], timeout=60)
        parent.invalidateblock(n[0])
        alt = self.mine_parent(5)
        self.wait_until(lambda: validator.getblockcount() < validator.getblockheader(c[0])["height"], timeout=60)
        size = os.path.getsize(validator.debug_log_path)
        self.return_to_n(n, alt[0])
        self.check_return(c, d, p, size)
        self.check_p_returns(d, p_alt[0])

        self.log.info("(b) the node is down while Bitcoin replaces P and N: only C is invalidated")
        self.start_node(1, extra_args=self.founder_args)
        self.connect_nodes(1, 2)
        n, p, c, d = self.build_c_and_d()
        self.stop_node(2)
        p_alt1, alt1 = self.replace_p_and_n(n, p)
        size = os.path.getsize(validator.debug_log_path)
        self.start_node(2, extra_args=self.validator_args)
        validator = self.nodes[2]
        self.wait_until(lambda: validator.getblockcount() < validator.getblockheader(c[0])["height"], timeout=60)
        invalidated = self.log_lines_since(size, "reorganization detected: invalidating block")
        assert any(c[0] in line for line in invalidated), "C's first block was not invalidated"
        assert not any(h in line for line in invalidated for h in d), "a block of D was invalidated on its own"
        size = os.path.getsize(validator.debug_log_path)
        self.return_to_n(n, alt1)
        self.check_return(c, d, p, size)
        self.check_p_returns(d, p_alt1)


if __name__ == '__main__':
    AnchorReconsiderTest().main()
