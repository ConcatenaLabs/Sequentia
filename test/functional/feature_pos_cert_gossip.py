#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test certificate gossip (poscert, honest-splits fix 3A Tier 1).

The asymmetric-finality split (3A): a block certifies, but the assembled block
reaches only part of the network; the members that share-signed it have no
record that it certified, so they later back a rival at that height and the
committee splits. The fix gossips the CERTIFICATE — the certified block's
header, whose proof solution carries the quorum aggregate — as its own small
object. A node that receives a valid certificate pins to it: it holds
production at that height (no rival, including via the escaping-stall valve),
fetches the body it lacks, connects the certified block, and relays the
certificate onward.

Here a producer node certifies a block on its own chain, and a second node
(never connected to it) receives ONLY the certificate through a mock peer.
The node must: log the production hold, accept the body when the mock supplies
it (completing it with the certificate it holds), connect the certified block
as its new tip, and relay the certificate to its other peers. Structurally
invalid certificates must be penalised.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.p2p import P2PInterface, MESSAGEMAP
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


class msg_poscert:
    """A raw `poscert` carrying an already-serialized block header."""
    msgtype = b"poscert"

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_poscert(%d bytes)" % len(self.payload)


class msg_poscert_recv:
    """Deserializer so a mock peer can RECEIVE relayed poscert messages."""
    msgtype = b"poscert"

    def __init__(self):
        self.payload = b""

    def deserialize(self, f):
        self.payload = f.read()

    def __repr__(self):
        return "msg_poscert_recv(%d bytes)" % len(self.payload)


class msg_getposprop_recv:
    """Deserializer so a mock peer can RECEIVE the node's body-fetch request."""
    msgtype = b"getposprop"

    def __init__(self):
        self.payload = b""

    def deserialize(self, f):
        self.payload = f.read()

    def __repr__(self):
        return "msg_getposprop_recv(%d bytes)" % len(self.payload)


class msg_posproposal:
    """A raw `posproposal` carrying an already-serialized full block."""
    msgtype = b"posproposal"

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_posproposal(%d bytes)" % len(self.payload)


class msg_getposcert:
    """A raw `getposcert` carrying a block hash (share-lock certificate query)."""
    msgtype = b"getposcert"

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_getposcert(%d bytes)" % len(self.payload)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class CertPeer(P2PInterface):
    """A mock peer that tolerates the committee-gossip message types."""

    def on_poscert(self, message):
        pass

    def on_getposprop(self, message):
        pass


class PosCertGossipTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(3)]
        outsider, _ = make_staker()
        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posbls=1",
            "-poscommitteesize=3",
            "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        # Node 0 certifies blocks (all committee keys local). Node 1 is an
        # active producer whose only key holds no stake: it processes committee
        # gossip but can never produce — the mock peer is its sole block source.
        producer = common + ["-posproducer"] + ["-posproducerkey=%s" % wif for wif, _ in self.stakers]
        follower = common + ["-posproducer", "-posproducerkey=%s" % outsider]
        self.extra_args = [producer, follower]

    def setup_network(self):
        # Deliberately NOT connected: the certificate must be the only channel.
        self.setup_nodes()

    def run_test(self):
        MESSAGEMAP[b"poscert"] = msg_poscert_recv
        MESSAGEMAP[b"getposprop"] = msg_getposprop_recv

        producer, follower = self.nodes[0], self.nodes[1]

        self.log.info("Producer certifies a block on its own chain")
        self.wait_until(lambda: producer.getblockcount() >= 1, timeout=60)
        cert_hash = producer.getblockhash(1)
        raw_header = bytes.fromhex(producer.getblockheader(cert_hash, False))
        raw_block = bytes.fromhex(producer.getblock(cert_hash, 0))
        assert_equal(follower.getblockcount(), 0)

        relay_peer = follower.add_p2p_connection(CertPeer())
        cert_peer = follower.add_p2p_connection(CertPeer())

        self.log.info("Certificate alone pins the follower and holds its production")
        with follower.assert_debug_log(["certificate received for block %s" % cert_hash,
                                        "holding production at height 1"]):
            cert_peer.send_message(msg_poscert(raw_header))
            self.wait_until(lambda: relay_peer.message_count["poscert"] > 0, timeout=30)
        # The certificate was relayed onward (partition-crossing), but the
        # block itself cannot connect yet: the follower lacks the body.
        assert_equal(follower.getblockcount(), 0)

        self.log.info("Body arrives; the follower completes it with the held certificate")
        # The follower asked the certificate's sender for the body it lacks.
        self.wait_until(lambda: cert_peer.message_count["getposprop"] > 0, timeout=30)
        cert_peer.send_message(msg_posproposal(raw_block))
        self.wait_until(lambda: follower.getbestblockhash() == cert_hash, timeout=30)
        assert_equal(follower.getblockheader(cert_hash)["poscertified"], True)

        self.log.info("A share-lock certificate query (getposcert) is answered")
        seen = relay_peer.message_count["poscert"]
        relay_peer.send_message(msg_getposcert(bytes.fromhex(cert_hash)[::-1]))
        self.wait_until(lambda: relay_peer.message_count["poscert"] > seen, timeout=30)

        self.log.info("Invalid certificates are penalised")
        bad_peer = follower.add_p2p_connection(CertPeer())
        for i in range(15):
            # Flip a merkle-root byte: the header still parses, its parent is
            # still known, the member set is still eligible — but the block
            # hash changed, so the leader signature no longer verifies and
            # CheckProof rejects the certificate as provable garbage. Each
            # flip gives a distinct hash, defeating the dedup. The node may
            # disconnect us mid-loop (penalty threshold reached): success.
            bad = bytearray(raw_header)
            bad[36 + i] ^= 0xFF
            try:
                bad_peer.send_message(msg_poscert(bytes(bad)))
            except OSError:
                break
        bad_peer.wait_for_disconnect(timeout=30)

        self.log.info("Certificate gossip: pin, hold, fetch, connect, relay — OK")


if __name__ == '__main__':
    PosCertGossipTest().main()
