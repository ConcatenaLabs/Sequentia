#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""From the hardening height the public committee is counted in seats by stake.

Four stakers of weights 3, 1, 1, 1 and a committee of 4 seats: the large one
holds 2 seats, two of the three small ones hold one each, and the quorum is 3
seats. So:

  * the large staker with any seated small one certifies (3 seats);
  * all three small ones without it cannot (2 seats), though under one place
    per staker the three of them were a quorum of the four;
  * the large staker alone cannot either (2 seats).

Below the height, with -poshardeningheight set past the test, the old rule
applies and the three small ones do certify.
"""

import os
import shutil
import time

from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

WEIGHTS = [3, 1, 1, 1]


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosCommitteeSeatsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2  # 0: seats from block 1; 1: the old rule throughout
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in WEIGHTS]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-con_max_block_sig_size=8000", "-posminstake=0",
            "-signblockscript=51", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-acceptnonstdtxn=1", "-par=1",
        ]

    def setup_network(self):
        self.add_nodes(self.num_nodes, [list(self.common) for _ in range(self.num_nodes)])
        self.start_node(0, extra_args=self.common + ["-staker=%s:1" % pub for _, pub in self.stakers])
        specs = ["-staker=%s:%d%s" % (pub, w, self.nodes[0].getblsregistration(wif)["spec"])
                 for (wif, pub), w in zip(self.stakers, WEIGHTS)]
        self.stop_node(0)
        shutil.rmtree(os.path.join(self.nodes[0].datadir, self.chain))
        self.start_node(0, extra_args=self.common + specs + ["-poshardeningheight=1"])
        self.start_node(1, extra_args=self.common + specs + ["-poshardeningheight=1000000"])

    def produce(self, node, group):
        """A block led and countersigned by `group` only (the leader signs as a
        member too): try each as leader until a slot opens; return the RPC
        result or the last error."""
        last = None
        for _ in range(20):
            for leader in group:
                try:
                    return node.generateposblock(leader, [w for w in group if w != leader])
                except JSONRPCException as e:
                    last = e.error["message"]
            time.sleep(0.5)
        return last

    def run_test(self):
        big = self.stakers[0][0]
        small = [w for w, _ in self.stakers[1:]]
        seated, n0, n1 = self.nodes[0], self.nodes[0], self.nodes[1]

        self.log.info("Seats: the large staker with the small ones certifies")
        res = self.produce(n0, [big] + small)
        assert isinstance(res, dict), res
        # 2 + 1 + 1: the third small staker holds no seat this slot.
        assert_equal(res["countersignatures"], 4)

        self.log.info("Seats: the three small stakers alone are not a quorum")
        height = n0.getblockcount()
        res = self.produce(n0, small)
        assert not isinstance(res, dict), res
        assert_equal(n0.getblockcount(), height)

        self.log.info("Seats: nor is the large staker alone")
        res = self.produce(n0, [big])
        assert not isinstance(res, dict), res
        assert_equal(n0.getblockcount(), height)

        self.log.info("Below the hardening height the three small stakers do certify")
        res = self.produce(n1, small)
        assert isinstance(res, dict), res
        assert_equal(res["countersignatures"], 3)


if __name__ == '__main__':
    PosCommitteeSeatsTest().main()
