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
stamps its proposals below the floor (fault injection):

 1. at least ten minutes before its parent, a second earlier each time
    (-posbyzantinebackdate);
 2. one second below the earliest time consensus allows
    (-posbyzantinestampfloor=-1);
 3. exactly at that time (-posbyzantinestampfloor=0), which is valid.

Each phase waits until node3 has ranked first (the lowest election slot, read
from every member's log) at several heights, so the case is exercised and not
merely not met. In phases 1 and 2 the three honest members must refuse those
proposals, report the distance below the floor, and keep certifying without a
collection running out. In phase 3 they must refuse nothing.
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

    def log_from(self, i, offset):
        return self.honest_log(i)[offset:]

    def log_sizes(self):
        return {i: len(self.honest_log(i)) for i in range(COMMITTEE)}

    def slots(self, texts):
        """Election slot of each member's proposal, per height, from the logs."""
        out = {}
        for i, text in texts.items():
            for h, slot in re.findall(r"proposing block [0-9a-f]{64} at height (\d+) \(election slot (\d+)\)", text):
                out.setdefault(int(h), {})[i] = int(slot)
            for h, slot in re.findall(r"BYZANTINE stamping proposal at height (\d+) with time \d+, .*\(election slot (\d+)\)", text):
                out.setdefault(int(h), {})[i] = int(slot)
        return out

    def attacker_first(self, texts):
        """Heights where node3's slot was lower than every other logged proposer's."""
        # A member whose slot is lower opens (and proposes) earlier, so every
        # member ranked ahead of node3 has logged its proposal by the time the
        # height certifies; one that never did ranked behind it.
        return sorted(h for h, by in self.slots(texts).items()
                      if ATTACKER in by and all(by[ATTACKER] < v for i, v in by.items() if i != ATTACKER))

    def phase(self, name, marks, want_first=3):
        """Run until node3 has ranked first at want_first heights; return the logs."""
        texts = {}

        def enough():
            texts.update({i: self.log_from(i, marks[i]) for i in range(COMMITTEE)})
            return len(self.attacker_first(texts)) >= want_first
        self.wait_until(enough, timeout=300)
        first = self.attacker_first(texts)
        # The honest members log their verdict on the last of those heights as
        # they settle it: read the logs again once every one of them is past it.
        self.wait_until(lambda: all(self.nodes[i].getblockcount() > max(first) for i in HONEST), timeout=120)
        texts.update({i: self.log_from(i, marks[i]) for i in range(COMMITTEE)})
        first = [h for h in self.attacker_first(texts) if h <= max(first)]
        self.log.info("%s: node%d ranked first at heights %s" % (name, ATTACKER, first))
        return texts, first

    def refusals(self, texts):
        return {i: re.findall(r"not backing proposal \S+ at height (\d+): stamped (\d+) s before the earliest time consensus allows", texts[i])
                for i in HONEST}

    def check_refused(self, name, texts, first, min_distance, max_distance=None):
        for i, refused in self.refusals(texts).items():
            restarts = re.findall(r"no certificate at height (\d+) after", texts[i])
            heights = {int(h) for h, _ in refused}
            self.log.info("%s: node%d refused %d proposal(s) at %d height(s); restarted %d collection(s)" %
                          (name, i, len(refused), len(heights), len(restarts)))
            assert not restarts, "%s: node%d ran collections out at heights %s" % (name, i, sorted(set(restarts)))
            # A member can certify a height from the others' shares before it
            # has seen node3's proposal, so each is held to most of the heights
            # and the three together to all of them.
            assert len([h for h in first if h in heights]) >= len(first) - 1, \
                "%s: node%d refused node%d's proposal at too few of heights %s: %s" % (name, i, ATTACKER, first, sorted(heights))
            assert all(int(s) >= min_distance for _, s in refused)
            if max_distance is not None:
                assert all(int(s) <= max_distance for _, s in refused), refused
        union = {int(h) for r in self.refusals(texts).values() for h, _ in r}
        assert not [h for h in first if h not in union], "%s: nobody refused node%d's proposal at %s" % (name, ATTACKER, first)

    def reconnect_attacker(self, extra):
        self.restart_node(ATTACKER, self.extra_args[ATTACKER] + extra)
        for b in HONEST:
            self.connect_nodes(ATTACKER, b)

    def run_test(self):
        honest = [self.nodes[i] for i in HONEST]
        self.log.info("The honest members certify %d heights while node%d backdates every proposal" % (TARGET, ATTACKER))
        self.wait_until(lambda: all(n.getblockcount() >= TARGET for n in honest), timeout=240)
        assert_equal(len({n.getblockhash(TARGET) for n in honest}), 1)
        assert "BYZANTINE stamping proposal" in self.honest_log(ATTACKER), "the case was not reached: node3 never backdated"
        texts, first = self.phase("backdated 600 s", {i: 0 for i in range(COMMITTEE)})
        self.check_refused("backdated 600 s", texts, first, 600)

        self.log.info("One second below the floor is refused")
        self.extra_args[ATTACKER] = [a for a in self.extra_args[ATTACKER] if not a.startswith("-posbyzantinebackdate")]
        self.reconnect_attacker(["-posbyzantinestampfloor=-1"])
        marks = self.log_sizes()
        texts, first = self.phase("floor - 1", marks)
        self.check_refused("floor - 1", texts, first, 1, 1)

        self.log.info("Exactly the floor is accepted")
        self.reconnect_attacker(["-posbyzantinestampfloor=0"])
        marks = self.log_sizes()
        texts, first = self.phase("floor", marks)
        for i, refused in self.refusals(texts).items():
            assert not refused, "node%d refused a proposal stamped at the floor: %s" % (i, refused)
            assert not re.findall(r"no certificate at height (\d+) after", texts[i])
        stamped = {int(h): int(t) for h, t in re.findall(
            r"BYZANTINE stamping proposal at height (\d+) with time (\d+), 0 s from", texts[ATTACKER])}
        # Whether such a proposal certifies depends on how far its stamp lags
        # the moment it is sent (a stamp at the floor can be a second older than
        # the proposal, past the collection window), so the claim is the one the
        # floor makes: it is not refused.
        assert len(stamped) >= 3, "node%d stamped only %d proposal(s) at the floor" % (ATTACKER, len(stamped))
        won = [h for h in first if h in stamped and h <= honest[0].getblockcount() and
               honest[0].getblock(honest[0].getblockhash(h))["time"] == stamped[h]]
        self.log.info("floor: %d proposals stamped at the floor, none refused; node%d's certified at %s of %s" %
                      (len(stamped), ATTACKER, won, first))


if __name__ == '__main__':
    PosGossipBackdatedStampTest().main()
