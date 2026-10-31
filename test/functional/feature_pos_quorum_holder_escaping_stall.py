#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A quorum holder, an escaping stall, and the reunion.

node0 is the parent chain ("Bitcoin"). node1 holds one staker of weight 9 (3 of
4 seats, the quorum). node2 holds three stakers of weight 1. Both are anchored
to node0 and validate anchors.

node1 is cut off from node2. node1 keeps certifying alone; node2 cannot reach a
quorum until the parent advances past the escaping-stall gap, then certifies
sub-quorum blocks. The two are reconnected: does the network converge, does
node2 lead blocks again, and is the holder ever held at a height?
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


PARENT_BLOCK_SECONDS = 600


class PosQuorumHolderEscapingStallTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--holderbinary", dest="holderbinary", default=None)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.big = make_staker()
        self.small = [make_staker() for _ in range(3)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
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
        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, chain)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.advance_parent(2)

        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=4", "-posexpraceheight=1",
            "-posslotinterval=1", "-par=1",
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        idle = self.common + ["-staker=%s:9" % self.big[1]] + ["-staker=%s:1" % p for _, p in self.small]
        bins = [self.options.holderbinary or self.options.bitcoind, self.options.bitcoind, self.options.bitcoind]
        self.add_nodes(3, [list(idle), list(idle), list(idle)], chain=[chain] * 3, binary=bins)
        self.start_node(1)
        self.start_node(2)
        self.start_node(3)

    def advance_parent(self, blocks):
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def producers(self, node, n):
        out = []
        for b in node.getposrecentblocks(n)["blocks"]:
            out.append("H" if b["producer"] == self.big[1] else "s")
        return "".join(reversed(out))

    def run_test(self):
        parent, holder, small, watcher = self.nodes
        specs = ["-staker=%s:9%s" % (self.big[1], holder.getblsregistration(self.big[0])["spec"])]
        specs += ["-staker=%s:1%s" % (p, holder.getblsregistration(w)["spec"]) for w, p in self.small]
        full = self.common + specs
        self.restart_node(1, extra_args=full + ["-posproducer", "-posproducerkey=%s" % self.big[0]])
        self.restart_node(2, extra_args=full + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in self.small])
        self.restart_node(3, extra_args=full)
        self.connect_nodes(1, 2)
        self.connect_nodes(1, 3)
        self.connect_nodes(2, 3)
        self.wait_until(lambda: small.getblockcount() >= 20, timeout=240)
        self.log.info("joined: last 20 producers %s" % self.producers(small, 20))

        holder.setnetworkactive(False)
        h_cut = small.getblockcount()
        self.log.info("holder cut off at height %d" % h_cut)
        time.sleep(5)
        self.log.info("5 s later, cut off: holder %d, small %d" % (holder.getblockcount(), small.getblockcount()))
        assert_equal(small.getblockcount(), h_cut)
        for r in range(3):
            self.advance_parent(4)
            t0 = time.time()
            try:
                self.wait_until(lambda: small.getblockcount() > h_cut + r, timeout=60)
                self.log.info("parent +4: small escaped to %d after %.1f s (posquorum %s, countersigs %s)" %
                              (small.getblockcount(), time.time() - t0,
                               small.getblockheader(small.getbestblockhash()).get("posquorum"),
                               small.getblockheader(small.getbestblockhash()).get("poscountersigs")))
            except AssertionError:
                self.log.info("parent +4: small did not escape in 60 s (at %d)" % small.getblockcount())
        self.log.info("before reunion: holder %d, small %d" % (holder.getblockcount(), small.getblockcount()))

        holder.setnetworkactive(True)
        self.connect_nodes(1, 2)
        self.connect_nodes(1, 3)
        t0 = time.time()
        converged = False
        while time.time() - t0 < 120:
            if holder.getbestblockhash() == small.getbestblockhash():
                converged = True
                break
            time.sleep(1)
        self.log.info("reunion: converged %s after %.0f s; holder %d small %d" %
                      (converged, time.time() - t0, holder.getblockcount(), small.getblockcount()))
        for i, n in ((1, holder), (2, small)):
            others = [(t["height"], t["hash"][:12], t["status"], t["branchlen"]) for t in n.getchaintips() if t["status"] != "active"]
            self.log.info("node%d other tips: %s" % (i, others))
        if converged:
            h = holder.getblockcount()
            self.wait_until(lambda: min(holder.getblockcount(), small.getblockcount()) >= h + 30, timeout=240)
            self.log.info("after reunion, 30 blocks: producers %s" % self.producers(small, 30))
            assert_equal(holder.getbestblockhash(), small.getbestblockhash())


if __name__ == '__main__':
    PosQuorumHolderEscapingStallTest().main()
