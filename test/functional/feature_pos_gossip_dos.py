#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Anti-DoS hardening of the autonomous committee gossip.

A peer that floods forged committee messages must be penalised and disconnected.
Honest nodes validate every `posproposal` / `posshare` before relaying, so a
message that fails validation (here: a `posshare` with malformed BLS fields)
could only have been originated or blindly forwarded by a misbehaving peer. The
node scores each such message and discourages/disconnects the sender, and never
relays it.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.p2p import P2PInterface
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class msg_posshare:
    """A raw `posshare` carrying an arbitrary (here deliberately malformed) body."""
    msgtype = b"posshare"

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_posshare(%d bytes)" % len(self.payload)


def forged_posshare(i):
    # PosShare = block_hash(32) || pubkey(CPubKey) || vrf_proof || bls_pubkey ||
    # bls_pop || bls_share. A valid 33-byte secp pubkey but empty BLS fields:
    # OnShare rejects it on the field-size check (bls_pubkey != 48 bytes).
    block_hash = i.to_bytes(32, 'little')          # distinct, so not deduplicated
    pubkey = b'\x21' + b'\x02' + b'\x00' * 32      # compactsize(33) + compressed key
    empty_vectors = b'\x00' * 4                     # vrf_proof, bls_pubkey, bls_pop, bls_share
    return msg_posshare(block_hash + pubkey + empty_vectors)


class PosGossipDosTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

        # A normal -posbls committee node, but its producer key is NOT one of the
        # registered stakers, so it produces nothing and floods no gossip of its
        # own — leaving the message handlers (and their anti-DoS scoring) as the
        # only committee traffic under test.
        stakers = [make_staker() for _ in range(3)]
        outsider, _ = make_staker()
        args = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=3",
            "-posslotinterval=1", "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
            "-posproducer", "-posproducerkey=%s" % outsider,
        ]
        args += ["-staker=%s:1" % pub for _, pub in stakers]
        self.extra_args = [args]

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PInterface())

        # The discouragement threshold is 100 and each forged message scores 10,
        # so well over ten distinct forgeries guarantees a disconnect.
        self.log.info("Flooding forged posshares should get the peer disconnected")
        for i in range(1, 16):
            peer.send_message(forged_posshare(i))

        peer.wait_for_disconnect(timeout=30)
        self.log.info("Misbehaving committee-gossip peer was disconnected")


if __name__ == '__main__':
    PosGossipDosTest().main()
