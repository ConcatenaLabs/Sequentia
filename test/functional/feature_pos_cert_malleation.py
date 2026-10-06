#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A bad committee certificate never poisons the hash of an honest block.

Under the public BLS committee the certificate (aggregate signature and signer
bitfield) lives in the proof solution, which the block hash does not commit to.
Anyone relaying a block can swap in a garbage aggregate without changing its
hash. Such a copy must be rejected as BLOCK_MUTATED: the relaying peer is
punished, but the hash is never marked failed, so the honest copy is still
accepted and the chain keeps moving.

Two paths are covered:

  * accept time: a tip-child with a garbage certificate is refused before its
    body is stored; the honest copy then connects.
  * connect time (headers-first / IBD): the garbage copies of two blocks are
    stored while their parent is still missing. When the parent arrives, the
    first of them fails at connect; its body is discarded rather than its hash
    failed, the node asks for it again, and the honest copies connect.
"""

import os
import shutil
import struct
import time
from io import BytesIO

from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.messages import CBlock, msg_block, msg_headers
from test_framework.p2p import P2PDataStore, P2PInterface
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

BLS_SIG_SIZE = 96


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def script_pushes(script):
    """Split a push-only script into its pushed data items."""
    pushes, i = [], 0
    while i < len(script):
        opcode = script[i]
        i += 1
        if opcode < 0x4c:
            size = opcode
        elif opcode == 0x4c:
            size = script[i]
            i += 1
        elif opcode == 0x4d:
            size = struct.unpack("<H", script[i:i + 2])[0]
            i += 2
        elif opcode == 0x4e:
            size = struct.unpack("<I", script[i:i + 4])[0]
            i += 4
        else:
            raise ValueError("not a push-only script: opcode %#x" % opcode)
        pushes.append(script[i:i + size])
        i += size
    return pushes


def block_from_hex(raw):
    block = CBlock()
    block.deserialize(BytesIO(bytes.fromhex(raw)))
    block.rehash()
    return block


def corrupt_aggregate(block):
    """Garble the 96-byte BLS aggregate of a bitfield certificate, leaving the
    leader signature, the bitfield and therefore the block hash untouched.
    Solution layout: <leader_sig> <96-byte aggregate> <bitfield>."""
    leader_sig, agg, bitfield = script_pushes(block.proof.solution)
    assert_equal(len(agg), BLS_SIG_SIZE)
    bad = bytearray(agg)
    bad[0] ^= 0xff
    bad[-1] ^= 0xff
    hash_before = block.hash
    block.proof.solution = bytes(CScript([leader_sig, bytes(bad), bitfield]))
    block.rehash()
    assert_equal(block.hash, hash_before)
    return block


class PosCertMalleationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2  # 0: producer holding every staker key; 1: victim
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(4)]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-acceptnonstdtxn=1", "-par=1",
        ]

    def setup_network(self):
        # The stakers' BLS registrations are derived by a running node.
        self.add_nodes(self.num_nodes, [list(self.common) for _ in range(self.num_nodes)])
        self.start_node(0, extra_args=self.common + ["-staker=%s:1" % pub for _, pub in self.stakers])
        specs = ["-staker=%s:1%s" % (pub, self.nodes[0].getblsregistration(wif)["spec"])
                 for wif, pub in self.stakers]
        self.stop_node(0)
        shutil.rmtree(os.path.join(self.nodes[0].datadir, self.chain))
        self.node_args = self.common + specs
        for i in range(self.num_nodes):
            self.start_node(i, extra_args=self.node_args)
        self.connect_nodes(0, 1)

    def produce(self, node):
        """A block certified by a full quorum, led by whichever staker's slot opens."""
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(90):
            for leader in wifs:
                others = [w for w in wifs if w != leader][:3]
                try:
                    return node.generateposblock(leader, others)["hash"]
                except JSONRPCException as e:
                    last = e.error
            time.sleep(0.5)
        raise AssertionError("could not produce: %s" % last)

    def tip_status(self, node, block_hash):
        for tip in node.getchaintips():
            if tip["hash"] == block_hash:
                return tip["status"]
        return None

    def stored(self, node, block_hash):
        try:
            return node.getblock(block_hash, 0)
        except JSONRPCException:
            return None

    def run_test(self):
        producer, victim = self.nodes
        for _ in range(2):
            self.produce(producer)
        self.sync_blocks()
        self.disconnect_nodes(0, 1)
        parent = victim.getbestblockhash()

        self.log.info("Accept time: a garbage certificate on a tip-child does not fail its hash")
        b = self.produce(producer)
        honest_b = block_from_hex(producer.getblock(b, 0))
        bad_b = corrupt_aggregate(block_from_hex(producer.getblock(b, 0)))
        relay = victim.add_p2p_connection(P2PDataStore())
        relay.block_store[bad_b.sha256] = bad_b
        relay.last_block_hash = bad_b.sha256
        with victim.assert_debug_log(expected_msgs=["bad-posbls-agg-invalid"]):
            relay.send_message(msg_headers([bad_b]))
            self.wait_until(lambda: bad_b.sha256 in relay.getdata_requests)
            # The relaying peer is still punished for the bad copy.
            relay.wait_for_disconnect()
        assert_equal(victim.getbestblockhash(), parent)
        assert self.tip_status(victim, b) != "invalid", self.tip_status(victim, b)
        assert_equal(self.stored(victim, b), None)

        honest = victim.add_p2p_connection(P2PInterface())
        honest.send_message(msg_block(honest_b))
        self.wait_until(lambda: victim.getbestblockhash() == b)

        self.log.info("Connect time: a stored garbage body is discarded and fetched again")
        c1, c2, c3 = (self.produce(producer) for _ in range(3))
        honest_c = [block_from_hex(producer.getblock(h, 0)) for h in (c1, c2, c3)]
        bad_c2 = corrupt_aggregate(block_from_hex(producer.getblock(c2, 0)))
        bad_c3 = corrupt_aggregate(block_from_hex(producer.getblock(c3, 0)))

        # The bad copies of C2 and C3 are stored while C1 is still missing.
        ibd = victim.add_p2p_connection(P2PDataStore())
        ibd.block_store[bad_c2.sha256] = bad_c2
        ibd.block_store[bad_c3.sha256] = bad_c3
        ibd.last_block_hash = bad_c3.sha256
        ibd.send_message(msg_headers(honest_c[:1] + [bad_c2, bad_c3]))
        self.wait_until(lambda: self.stored(victim, c2) == bad_c2.serialize().hex())
        self.wait_until(lambda: self.stored(victim, c3) == bad_c3.serialize().hex())
        assert_equal(victim.getbestblockhash(), b)

        # From here on the victim can fetch honest copies from a second peer.
        good = victim.add_p2p_connection(P2PDataStore())
        for block in honest_c:
            good.block_store[block.sha256] = block
        good.last_block_hash = honest_c[-1].sha256

        with victim.assert_debug_log(expected_msgs=["discarding stored body of %s" % c2]):
            good.send_message(msg_block(honest_c[0]))
            self.wait_until(lambda: victim.getbestblockhash() == c1)
            self.wait_until(lambda: self.stored(victim, c2) is None)
        assert self.tip_status(victim, c3) != "invalid", self.tip_status(victim, c3)

        # Announcing the chain is enough: the victim asks for the discarded body
        # itself, then fails and discards C3's bad body the same way.
        good.send_message(msg_headers(honest_c))
        self.wait_until(lambda: victim.getbestblockhash() == c3)
        assert_equal(victim.getbestblockhash(), producer.getbestblockhash())

        self.log.info("The repaired chain survives a restart")
        self.restart_node(1, extra_args=self.node_args)
        assert_equal(victim.getbestblockhash(), c3)


if __name__ == '__main__':
    PosCertMalleationTest().main()
