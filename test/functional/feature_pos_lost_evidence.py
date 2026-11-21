#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Evidence never weakens what a holder demands (doc 03, "Transactions of a lost
block").

Live producers at the testnet's round lengths (LOST_ROUND_MS, LOST_WINDOW_MS) on
the network of feature_pos_lost_blocks.py. Cases (LOST_CASES):
  evstall  every honest node holds T in a lost block L. A coalition certifies a
           sibling of L that spends T's coin differently, after the fact, and
           then withholds three of the six seats, so no collection reaches the
           quorum. Bitcoin moves on until the escaping-stall valve opens at each
           height. T must be mined, or the chain must wait; the seller's respend
           R must not be mined while T is still bound or after T was left out.
"""

import time

from feature_pos_lost_autonomous import LostAutonomousTest
from feature_pos_lost_blocks import SELLER, W, WAIVER
from feature_pos_lost_record_attacks import LostRecordAttacksTest
from test_framework.messages import tx_from_hex

COALITION = [0, 4, 5]
HOLDERS = [1, 2, 3]
#: The quorum of the sibling at the lost height: the coalition and one more seat.
SIBLING_SIGNERS = [0, 4, 5, 1]


class LostEvidenceTest(LostAutonomousTest, LostRecordAttacksTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        LostAutonomousTest.run_test(self)

    def restarted(self, since, height):
        """Whether a holder's collection at `height` ended without a certificate."""
        text = "at height %d" % height
        return any(text in line for i in HOLDERS for line in self.log_has(i, "restarting collection", since[i]))

    def chain_has(self, node, txid, from_height):
        for h in range(from_height, node.getblockcount() + 1):
            if txid in node.getblock(node.getblockhash(h))["tx"]:
                return h
        return None

    def case_evstall(self):
        case = "evstall"
        self.log.info("(evstall) a sibling certified after the fact, then a blocking share, waiver depth %d" % WAIVER)
        st = self.gap(case)
        a, n1 = self.nodes[SELLER], self.nodes[1]
        hdr = n1.getblockheader(st["L"])
        fork = hdr["height"] - 1
        self.isolate()
        sibling = self.evidence_block([tx_from_hex(st["r_hex"])], hdr["anchorheight"], int(hdr["anchorhash"], 16), SIBLING_SIGNERS)
        sibling.rehash()
        self.show_evidence(sibling, to=2, btc=self.stale_chain(hdr["anchorhash"]))
        self.wait_until(lambda: all(sibling.hash in self.lost(i) for i in HOLDERS), timeout=60)
        self.record(case, "sibling", "%s at height %d carries R, certified by seats %s after the loss; every holder has it as evidence: %s" % (
            sibling.hash[:16], hdr["height"], SIBLING_SIGNERS, all(not self.lost(i)[sibling.hash]["own"] for i in HOLDERS)))

        since = [self.log_size(i) for i in range(len(self.nodes))]
        t0 = time.time()
        self.start_producers(HOLDERS)
        rows = []
        lapse = None
        for _ in range(8):
            height = n1.getblockcount() + 1
            # The first collection at this height fails on the withheld seats.
            self.wait_until(lambda: n1.getblockcount() >= height or self.restarted(since, height), timeout=420)
            if n1.getblockcount() < height:
                self.btc_mine(3)
            self.wait_until(lambda: n1.getblockcount() >= height, timeout=600)
            b = n1.getblock(n1.getblockhash(height))
            head = n1.getblockheader(b["hash"])
            rows.append(dict(height=height, at_s=round(time.time() - t0), anchor=head["anchorheight"], certified=head["poscertified"],
                             has_T=st["t_id"] in b["tx"], txs=len(b["tx"]) - 1))
            self.record(case, "block at height %d" % height, rows[-1])
            if st["t_id"] in b["tx"]:
                break
            if height >= hdr["height"]:
                at_lost = n1.getblockheader(n1.getblockhash(hdr["height"]))["anchorheight"]
                lapse = max(hdr["anchorheight"], at_lost) + W
                if self.btc().getblockcount() >= lapse and head["anchorheight"] >= lapse:
                    break

        t_at = self.chain_has(n1, st["t_id"], fork + 1)
        r_at = self.chain_has(n1, st["r_id"], fork + 1)
        if t_at is None:
            # T was left out up to the lapse. The producers stop, the seller comes back and proposes R.
            self.record(case, "lapse", "T left out of every block to Bitcoin height %s; the seller now mines R" % lapse)
            for i in HOLDERS:
                self.restart_node(i, extra_args=self.node_args(i))
            self.mesh(range(1, len(self.nodes)))
            self.rejoin()
            if st["r_id"] not in a.getrawmempool():
                a.sendrawtransaction(st["r_hex"])
            x = self.produce(SELLER, COALITION, leaders=[SELLER], txs_must=[st["r_id"]], allow_fail=True)
            self.record(case, "seller's block", {k: x[k] for k in ("hash", "height", "error") if k in x})
            if "hash" in x:
                self.wait_until(lambda: n1.getbestblockhash() == x["hash"] or
                                self.log_has(1, "Lost blocks: refusing block %s" % x["hash"], since[1]), timeout=60)
            t_at = self.chain_has(n1, st["t_id"], fork + 1)
            r_at = self.chain_has(n1, st["r_id"], fork + 1)
        self.record(case, "outcome", "T at height %s, the seller's R at height %s, %d block(s) after the fork in %d s" % (
            t_at, r_at, n1.getblockcount() - fork, time.time() - t0))
        if r_at is not None:
            raise AssertionError("ATTACK SUCCEEDED (evstall): the seller's respend is mined at height %d and T is not" % r_at)
        assert t_at is not None or n1.getblockcount() == fork, "T left out of a block without the seller's spend"
        self.record(case, "blocked", "T is mined, and R is not")


if __name__ == '__main__':
    LostEvidenceTest().main()
