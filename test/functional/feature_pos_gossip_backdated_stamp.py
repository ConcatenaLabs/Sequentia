#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A proposal stamped before consensus allows cannot start the round schedule.

A collection's rounds are timed from round 0's stamp, and that counts a
proposal a member holds but never validates (one left aside for its anchor). A
stamp in the past would start the schedule in the past: the collection would
run out before any member tried to sign, and the re-proposals would meet the
same stamp again, at every collection of every height where that leader ranks
first. So a member neither backs nor relays a proposal stamped before the
earliest time consensus allows (its slot, the median time past, the minimum
spacing after its parent); a valid proposal is never stamped there.

nodes 0-3 are a gossip committee of four seats, one each, quorum 3. node3
stamps every proposal at least ten minutes before its parent, a second
earlier each time (fault injection via -posbyzantinebackdate). The three
honest members must refuse those proposals and keep certifying, height after height, without a collection running out.
"""

import re

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

COMMITTEE = 4
ATTACKER = 3
HONEST = [0, 1, 2]
TARGET = 16


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosGossipBackdatedStampTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = COMMITTEE
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(COMMITTEE)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=%d" % COMMITTEE,
            "-posslotinterval=1", "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0", "-debug=validation",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = []
        for i in range(COMMITTEE):
            args = common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
            if i == ATTACKER:
                args.append("-posbyzantinebackdate=600")
            self.extra_args.append(args)

    def setup_network(self):
        self.setup_nodes()
        for a in range(COMMITTEE):
            for b in range(a + 1, COMMITTEE):
                self.connect_nodes(a, b)

    def honest_log(self, i):
        with open(self.nodes[i].debug_log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def run_test(self):
        honest = [self.nodes[i] for i in HONEST]
        self.log.info("The honest members certify %d heights while node%d backdates every proposal" % (TARGET, ATTACKER))
        self.wait_until(lambda: all(n.getblockcount() >= TARGET for n in honest), timeout=240)
        assert_equal(len({n.getblockhash(TARGET) for n in honest}), 1)

        texts = {i: self.honest_log(i) for i in HONEST}
        attacker_log = self.honest_log(ATTACKER)
        assert "BYZANTINE stamping proposal" in attacker_log, "the case was not reached: node3 never backdated"
        for i, text in texts.items():
            refused = re.findall(r"not backing proposal \S+ at height (\d+): stamped (\d+) s before the earliest time consensus allows", text)
            restarts = re.findall(r"no certificate at height (\d+) after", text)
            self.log.info("node%d refused %d backdated proposal(s) at %d height(s); restarted %d collection(s)" %
                          (i, len(refused), len({h for h, _ in refused}), len(restarts)))
            assert not restarts, "node%d ran collections out at heights %s" % (i, sorted(set(restarts)))
            assert refused, "node%d never refused a backdated proposal" % i
            assert all(int(s) >= 600 for _, s in refused)


if __name__ == '__main__':
    PosGossipBackdatedStampTest().main()
