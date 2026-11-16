#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The hold for an unreadable last entry stops proposals as well as shares
(doc/sequentia/04-proof-of-stake.md, "The signing record").

The gossip committee of feature_pos_signing_record_anchor.py: node0 is the
parent chain, nodes 1-4 hold one seat each, node1 (M) is the member under test.
Nodes 3 and 4 stop, node2 restarts without a producer, so the height above the
tip cannot certify. M is killed (SIGKILL), the last entry of its record is
damaged (one byte flipped: whole in length, failing its checksum), and M starts
again connected to node2. Until the hold ends (one window and two rounds with a
peer and no certificate) M neither proposes nor shares at the held height; once
it ends, M proposes there (liveness).
"""

import os
import re

from feature_pos_signing_record_anchor import M, PosSigningRecordAnchorTest
from test_framework.util import p2p_port, rpc_port

PROPOSING = re.compile(r"PoS gossip: proposing block ([0-9a-f]{64}) at height (\d+)")
SHARING = re.compile(r"PoS gossip: signed ([0-9a-f]{64}) at height (\d+)")
HOLD_ENDS = "the hold for an unreadable entry ends at height"


class PosSigningRecordProposalTest(PosSigningRecordAnchorTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= 3, timeout=120)
        for i in (3, 4, 2):
            self.stop_node(i)
        # node2 comes back as a peer with no producer: it neither proposes nor shares.
        self.start_node(2, extra_args=self.full + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2)])
        self.connect_nodes(M, 2)
        self.sync_blocks([self.nodes[M], self.nodes[2]])
        tip = self.nodes[2].getblockcount()
        self.sigkill(M)
        path = os.path.join(self.nodes[M].datadir, self.nodes[M].chain, "possigned.dat")
        with open(path, "r+b") as f:
            f.seek(-1, os.SEEK_END)
            last = f.read(1)[0]
            f.seek(-1, os.SEEK_END)
            f.write(bytes([last ^ 1]))
        pos = self.log_size(M)
        self.start_node(M, extra_args=self.args_for(M))
        self.connect_nodes(M, 2)
        self.wait_until(lambda: HOLD_ENDS in self.log_since(M, pos) or PROPOSING.search(self.log_since(M, pos)), timeout=60)
        text = self.log_since(M, pos)
        assert "is whole but unreadable" in text, "the start did not report the unreadable entry"
        end = text.find(HOLD_ENDS)
        first = PROPOSING.search(text)
        if first and (end < 0 or first.start() < end):
            held = "unreadable" in self.nodes[M].getsigningrecord()
            raise AssertionError("M proposed %s at height %s while the hold for its unreadable entry was active (still active: %s)" %
                                 (first.group(1)[:16], first.group(2), held))
        shared = SHARING.search(text)
        assert not shared or shared.start() > end, "M shared at height %s during the hold" % (shared.group(2) if shared else "")
        refused = re.search(r"PoS gossip: not proposing at height (\d+): ([^\n]*)", text[:end])
        assert refused and "unreadable entry" in refused.group(2), "M was not refused a proposal by the hold"
        self.log.info("during the hold: %s" % refused.group(0).strip())
        self.log.info("the hold ended: %s" % text[end:text.find("\n", end)].strip())
        # Liveness: once the hold ends, M proposes at the held height or above.
        self.wait_until(lambda: PROPOSING.search(self.log_since(M, pos)[end:]), timeout=60)
        after = PROPOSING.search(self.log_since(M, pos)[end:])
        assert int(after.group(2)) >= tip + 1
        self.log.info("after the hold: M proposed %s at height %s" % (after.group(1)[:16], after.group(2)))


if __name__ == "__main__":
    PosSigningRecordProposalTest().main()
