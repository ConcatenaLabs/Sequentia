#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A node says when block times and its own clock disagree (audit A12).

A producer whose clock runs fast stamps its blocks ahead of real time, which
consensus allows up to two hours. A node with a correct clock receiving such a
block warns (log and getnetworkinfo), naming both possible causes, and clears
the warning once its clock and the newest block agree again.
"""

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

COIN = 100_000_000


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosClockWarningTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_key()
        self.extra_args = [[
            "-con_pos=1", "-posvrf=1", "-posslotinterval=1",
            "-signblockscript=51", "-initialfreecoins=1000000000000",
            "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, COIN), "-validatepegin=0",
        ]] * 2

    def warning(self, node):
        return node.getnetworkinfo()["warnings"]

    def run_test(self):
        fast, ok = self.nodes
        fast.generateposblock(self.a_wif)
        self.sync_all()
        assert "stamped" not in self.warning(ok)

        self.log.info("A block from a producer whose clock runs ten minutes fast")
        now = int(fast.getblockheader(fast.getbestblockhash())["time"])
        fast.setmocktime(now + 600)
        fast.generateposblock(self.a_wif)
        self.sync_all()
        assert_equal(ok.getblockcount(), 2)
        self.log.info("The node with a correct clock warns")
        assert "The newest block is stamped about" in self.warning(ok)
        assert "producer has a clock running fast" in self.warning(ok)

        self.log.info("The warning clears once the clock and the newest block agree")
        ok.setmocktime(now + 700)
        ok.generateposblock(self.a_wif)
        assert "stamped" not in self.warning(ok)


if __name__ == '__main__':
    PosClockWarningTest().main()
