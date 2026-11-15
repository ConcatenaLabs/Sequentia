"""P2P helpers for the lost-block attack tests (from the KR41 review's probe)."""

import struct
from io import BytesIO

from feature_pos_cert_malleation import corrupt_aggregate
from test_framework.messages import (
    CBlock, CTransaction, deser_uint256, deser_vector, hash256, msg_block,
    msg_headers, ser_uint256, ser_vector, uint256_from_str,
)
from test_framework.p2p import MESSAGEMAP, P2PDataStore
from test_framework.util import assert_equal


class RawMessage:
    msgtype = b"poslost"

    def deserialize(self, f):
        self.data = f.read()


MESSAGEMAP[b"poslost"] = RawMessage


class LostP2PDataStore(P2PDataStore):
    def on_poslost(self, message):
        pass


class AnchoredBlock(CBlock):
    __slots__ = ("m_anchor_height", "m_anchor_hash")

    def deserialize(self, f):
        self.nVersion = struct.unpack("<i", f.read(4))[0]
        assert self.nVersion >= 0
        self.hashPrevBlock = deser_uint256(f)
        self.hashMerkleRoot = deser_uint256(f)
        self.nTime = struct.unpack("<I", f.read(4))[0]
        self.block_height = struct.unpack("<I", f.read(4))[0]
        self.m_anchor_height = struct.unpack("<I", f.read(4))[0]
        self.m_anchor_hash = deser_uint256(f)
        self.proof.deserialize(f)
        self.vtx = deser_vector(f, CTransaction)
        self.sha256 = None
        self.hash = None

    def header_bytes(self, for_hash=False):
        return (struct.pack("<i", self.nVersion) + ser_uint256(self.hashPrevBlock) +
                ser_uint256(self.hashMerkleRoot) + struct.pack("<I", self.nTime) +
                struct.pack("<I", self.block_height) + struct.pack("<I", self.m_anchor_height) +
                ser_uint256(self.m_anchor_hash) +
                (self.proof.serialize_for_hash() if for_hash else self.proof.serialize()))

    def serialize(self, with_witness=True):
        return self.header_bytes() + ser_vector(self.vtx, "serialize_with_witness" if with_witness else "serialize_without_witness")

    def calc_sha256(self):
        if self.sha256 is None:
            hashed = hash256(self.header_bytes(for_hash=True))
            self.sha256 = uint256_from_str(hashed)
            self.hash = hashed[::-1].hex()


def block_from_hex(raw):
    block = AnchoredBlock()
    block.deserialize(BytesIO(bytes.fromhex(raw)))
    block.rehash()
    return block


class msg_anchor_headers:
    msgtype = b"headers"

    def __init__(self, headers):
        self.headers = headers

    def serialize(self):
        return bytes([len(self.headers)]) + b"".join(h.header_bytes() + b"\x00" for h in self.headers)
