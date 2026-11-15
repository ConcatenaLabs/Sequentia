#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""What the signing record does with a damaged or foreign file at the start
(doc/sequentia/04-proof-of-stake.md, "The signing record").

The MuSig2 committee chain of feature_pos_signing_record_rpc.py; node1 (M)
makes blocks by RPC. Each case edits M's possigned.dat while M is stopped:

unreadable: the last entry is whole in length but fails its checksum (a synced
  entry that rotted, or an append whose size reached the disk before its
  contents). M starts, says so, keeps a copy of the file beside the record, and
  signs nothing at the next height it is asked to sign at until one window and
  two rounds have passed there with a peer and no certificate; then it signs,
  and a later start holds nothing.
newer: the last entry carries a later format's mark: the start is refused and
  says what to do.
chain: the record names another chain's genesis: the start is refused and says
  what to do.
"""

import glob
import hashlib
import os
import shutil
import struct
import time

from feature_pos_signing_record_rpc import PosSigningRecordRpcTest
from test_framework.authproxy import JSONRPCException
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

BOUND_S = (500 + 25 * 3 + 2 * (700 + 35 * 3)) / 1000.0


def frames(data):
    out, pos = [], 0
    while pos + 8 <= len(data):
        _, n = struct.unpack_from("<II", data, pos)
        out.append((pos, n))
        pos += 12 + n
    return out


class PosSigningRecordFileTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        super().set_test_params()

    def path(self):
        return os.path.join(self.nodes[1].datadir, "elementsregtest", "possigned.dat")

    def generate(self):
        m = self.nodes[1]
        return m.generateposblock(self.stakers[0][0], [w for w, _ in self.stakers[1:]])

    def run_test(self):
        m = self.nodes[1]
        self.generate()
        self.generate()
        self.case_unreadable()
        self.case_refused("newer", self.mark_newer, "written by a newer version of this software")
        self.case_refused("chain", self.other_chain, "belongs to another chain")

    def case_unreadable(self):
        m = self.nodes[1]
        made = self.generate()
        h = m.getblockheader(made["hash"])["height"]
        self.sigkill(1)
        with open(self.path(), "r+b") as f:
            f.seek(-1, os.SEEK_END)
            last = f.read(1)[0]
            f.seek(-1, os.SEEK_END)
            f.write(bytes([last ^ 1]))
        self.start_node(1, extra_args=self.args(1))
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        assert "is whole but unreadable" in text, "unreadable: the start did not say what it found"
        u = m.getsigningrecord()["unreadable"]
        assert os.path.exists(u["copy"]), u
        self.log.info("unreadable: started; holds from height %d for at most %d ms with a peer; copy kept as %s" %
                      (u["floor"], u["bound_ms"], os.path.basename(u["copy"])))
        assert u["floor"] <= h
        self.connect_nodes(1, 2)
        t0 = time.time()
        try:
            r = self.generate()
            raise AssertionError("unreadable: signed %s at once" % r["hash"])
        except JSONRPCException as e:
            assert "unreadable entry" in e.error["message"], e.error["message"]
            self.log.info("unreadable: refused: %s" % e.error["message"])
        held = m.getsigningrecord()["unreadable"]["height"]
        r = None
        while r is None and time.time() < t0 + 30:
            try:
                r = self.generate()
            except JSONRPCException as e:
                assert "unreadable entry" in e.error["message"], e.error["message"]
                time.sleep(0.2)
        assert r, "unreadable: the hold did not end"
        waited = time.time() - t0
        assert waited >= BOUND_S - 0.3, "unreadable: held only %.2f s" % waited
        assert_equal(m.getblockheader(r["hash"])["height"], held)
        assert "unreadable" not in m.getsigningrecord()
        self.log.info("unreadable: held height %d for %.1f s (bound %.2f s), then made %s there" % (held, waited, BOUND_S, r["hash"][:16]))
        self.restart_node(1, extra_args=self.args(1))
        assert "unreadable" not in m.getsigningrecord(), "unreadable: the hold came back at a later start"
        os.remove(u["copy"])

    def mark_newer(self, data):
        pos, _ = frames(data)[-1]
        data[pos + 3] = ord("2")

    def other_chain(self, data):
        pos, n = frames(data)[0]
        data[pos + 9] ^= 0xff  # the first byte of the genesis hash the chain entry names
        payload = bytes(data[pos + 8:pos + 8 + n])
        data[pos + 8 + n:pos + 12 + n] = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]

    def case_refused(self, name, edit, expected):
        m = self.nodes[1]
        self.generate()
        self.stop_node(1)
        keep = self.path() + ".keep"
        shutil.copyfile(self.path(), keep)
        with open(self.path(), "rb") as f:
            data = bytearray(f.read())
        edit(data)
        with open(self.path(), "wb") as f:
            f.write(data)
        m.assert_start_raises_init_error(self.args(1), expected, match=ErrorMatch.PARTIAL_REGEX)
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            line = [ln for ln in f.read().splitlines() if expected in ln][-1]
        self.log.info("%s: start refused: %s" % (name, line.split("Error: ", 1)[-1]))
        shutil.move(keep, self.path())
        self.start_node(1, extra_args=self.args(1))
        assert not glob.glob(self.path() + ".unreadable-*")


if __name__ == "__main__":
    PosSigningRecordFileTest().main()
