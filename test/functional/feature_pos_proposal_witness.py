#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A committee member that holds another witness of a proposal's transaction still signs the proposal.

A compact proposal names its transactions by txid, and a member rebuilds the block from its own mempool. A
transaction whose witness anyone can change (here a P2WSH output whose script drops one witness item)
has variants with one txid and different witnesses. A member holding a variant other than the leader's
rebuilds a block whose witness commitment does not match; it must fetch the leader's full proposal and
sign it, not judge the leader's block invalid and exclude the leader.

Three nodes each hold one of the three committee keys (quorum two). Between two blocks, the next leader
(the lowest VRF draw) gets one variant and both other members the other. The block at that height must be
the leader's, holding the leader's variant, and no node may log an exclusion. Before the fix both members
excluded the leader, so the leader's block had no quorum and a member's block, with the members' variant,
took the height.
"""

import hashlib

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutWitness
from test_framework.script import CScript, OP_0, OP_DROP, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

SPACING = 8   # -posblockspacing: the quiet seconds between blocks in which the variants are placed


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosProposalWitnessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-posblockspacing=%d" % SPACING, "-posblockspacingheight=1",
            "-con_max_block_sig_size=4000", "-signblockscript=51",
            "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1", "-con_blocksubsidy=0",
            "-anyonecanspendaremine=1", "-acceptnonstdtxn=1", "-validatepegin=0",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]] for i in range(3)]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def wait_height(self, h, timeout=120):
        self.wait_until(lambda: all(n.getblockcount() >= h for n in self.nodes), timeout=timeout)

    def variants(self, coin_txid, vout, value, script):
        """Two spends of the P2WSH(`script`) coin: one txid, two witnesses."""
        fee = 10_000
        out = []
        for item in (b"\x01", b"\x02"):
            tx = CTransaction()
            tx.nVersion = 2
            tx.vin = [CTxIn(COutPoint(int(coin_txid, 16), vout), nSequence=0xfffffffd)]
            tx.vout = [CTxOut(value - fee, CScript([OP_TRUE])), CTxOut(fee)]
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = [item, bytes(script)]
            tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
            tx.rehash()
            out.append(tx)
        assert_equal(out[0].hash, out[1].hash)
        assert out[0].getwtxid() != out[1].getwtxid()
        return out

    def leader_of_next(self):
        """The node whose key drew the lowest VRF output for the next block (the committee backs the lowest,
        compared as a uint256 in byte order)."""
        draws = []
        for i, n in enumerate(self.nodes):
            s = n.getposslot()
            draws.append((bytes.fromhex(s["keys"][0]["vrf"])[::-1], i, s["height"]))
        heights = {h for _, _, h in draws}
        assert_equal(len(heights), 1)
        return min(draws)[1], heights.pop()

    def run_test(self):
        n0 = self.nodes[0]
        self.wait_height(2)
        # A coin of P2WSH(OP_DROP OP_TRUE): its witness item is anyone's to choose.
        script = CScript([OP_DROP, OP_TRUE])
        spk = CScript([OP_0, hashlib.sha256(bytes(script)).digest()])
        g = n0.getblock(n0.getblockhash(0), 2)
        gtx, gn, gval = [(t["txid"], o["n"], int(o["value"] * 100_000_000)) for t in g["tx"] for o in t["vout"]
                         if o["scriptPubKey"]["hex"] == "51" and o.get("value", 0) > 0 and n0.gettxout(t["txid"], o["n"])][0]
        attempts = []
        for attempt in range(3):
            fund = CTransaction()
            fund.nVersion = 2
            fund.vin = [CTxIn(COutPoint(int(gtx, 16), gn))]
            amount, fee = 10 * 100_000_000, 100_000
            fund.vout = [CTxOut(amount, spk), CTxOut(gval - amount - fee, CScript([OP_TRUE])), CTxOut(fee)]
            fund.rehash()
            n0.sendrawtransaction(fund.serialize().hex())
            self.wait_until(lambda: all(n.gettxout(fund.hash, 0, False) is not None for n in self.nodes), timeout=120)
            gtx, gn, gval = fund.hash, 1, gval - amount - fee
            v1, v2 = self.variants(fund.hash, 0, amount, script)
            # Just after a block: SPACING seconds before the next one.
            h0 = n0.getblockcount()
            self.wait_height(h0 + 1)
            leader, height = self.leader_of_next()
            members = [i for i in range(3) if i != leader]
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())
            for i in members:
                self.nodes[i].sendrawtransaction(v2.serialize().hex())
            placed = (all(n.getblockcount() == height - 1 for n in self.nodes) and
                      self.nodes[leader].getmempoolentry(v1.hash)["wtxid"] == v1.getwtxid() and
                      all(self.nodes[i].getmempoolentry(v1.hash)["wtxid"] == v2.getwtxid() for i in members))
            logs = [len(open(self.debug_log(i), encoding="utf8").read().splitlines()) for i in range(3)]
            self.wait_height(height)
            blk = n0.getblock(n0.getblockhash(height), 2)
            held = [t for t in blk["tx"] if t["txid"] == v1.hash]
            excluded = [line for i in range(3) for line in open(self.debug_log(i), encoding="utf8").read().splitlines()[logs[i]:]
                        if "excluding its leader" in line]
            attempts.append((attempt, leader, height, placed, held[0]["hash"] if held else None, len(excluded)))
            self.log.info("attempt %d: leader node %d at height %d, variants placed %s; block holds %s; %d exclusion(s)",
                          attempt, leader, height, placed, held[0]["hash"] if held else "neither", len(excluded))
            if not placed or not held:
                # The block came before both variants were in place, or without the transaction: try again.
                self.wait_until(lambda: all(v1.hash not in n.getrawmempool() for n in self.nodes), timeout=120)
                continue
            assert_equal(held[0]["hash"], v1.getwtxid())  # the leader's variant: the leader's block took the height
            assert_equal(excluded, [])
            return
        raise AssertionError("the case was not reached: %s" % attempts)

    def debug_log(self, i):
        return "%s/%s/debug.log" % (self.nodes[i].datadir, self.chain)


if __name__ == '__main__':
    PosProposalWitnessTest().main()
