#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The mempool and the block template honour the chain's coinbase maturity.

Consensus holds a coinbase spend to the maturity in force at the spend height
(CoinbaseMaturityAt), which on the real chains is 1,000 blocks rather than the
inherited 100. A resident mempool entry can become premature without any input
being spent in two ways:

- a reorg lowers the tip, so a spend admitted at exactly the chain's maturity is
  one block short again (an anchor-driven one-block rollback does this);
- the maturity rises at a height, so a spend that was mature for the block
  before the boundary is premature for the block after it.

Either way the entry must leave the mempool, and the template must never carry
it: otherwise every block template fails validation and no producer holding the
entry can build a block. Both nodes of the reorg case run the custom chain's
default mempool consistency checks on one side and none on the other, so the
eviction is shown on its own and not only as "the checker did not abort".

The test also refuses -con_coinbase_maturity_height=0 with a maturity set, which
would otherwise leave the chain on 100 without saying so.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

MATURITY = 150
BOUNDARY = 130
FEE = Decimal('0.0001')
BASE = ["-con_blocksubsidy=5000000000", "-validatepegin=0", "-par=1", "-txindex=1"]


class MempoolCoinbaseMaturityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [
            # Reorg pair: the custom chain's default -checkmempool, and none.
            BASE + ["-con_coinbase_maturity=%d" % MATURITY],
            BASE + ["-con_coinbase_maturity=%d" % MATURITY, "-checkmempool=0"],
            # Boundary node: 100 below height 130, 150 from it. A different
            # chain, so it is not connected to the pair.
            BASE + ["-con_coinbase_maturity=%d" % MATURITY,
                    "-con_coinbase_maturity_height=%d" % BOUNDARY],
        ]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def addr(self, node):
        return node.getaddressinfo(node.getnewaddress())['unconfidential']

    def spend_of_coinbase(self, node, height, dest):
        """A signed spend of the wallet's output in the coinbase at `height`."""
        cb = node.getblock(node.getblockhash(height), 2)['tx'][0]
        mine = [v for v in cb['vout'] if v.get('value', 0) > 0 and
                node.getaddressinfo(v['scriptPubKey']['address'])['ismine']]
        assert_equal(len(mine), 1)
        vout = mine[0]
        raw = node.createrawtransaction([{'txid': cb['txid'], 'vout': vout['n']}],
                                        [{dest: vout['value'] - FEE}, {'fee': FEE}])
        signed = node.signrawtransactionwithwallet(raw)
        assert_equal(signed['complete'], True)
        return signed['hex'], node.decoderawtransaction(signed['hex'])['txid']

    def assert_block_refuses(self, node, hex_tx, depth):
        """Consensus refuses the spend in a block, for maturity and nothing else."""
        assert_raises_rpc_error(
            -25, "bad-txns-premature-spend-of-coinbase, tried to spend coinbase at depth %d" % depth,
            node.generateblock, self.addr(node), [hex_tx], invalid_call=False)

    def run_test(self):
        self.test_reorg()
        self.test_boundary()
        self.test_height_zero_refused()

    def test_reorg(self):
        n0, n1 = self.nodes[0], self.nodes[1]
        self.log.info("Reorg: a spend admitted at exactly the maturity leaves the mempool when the tip drops")
        pair = lambda: self.sync_blocks([n0, n1])
        a0 = self.addr(n0)
        other = self.addr(n1)
        self.generatetoaddress(n0, 1, a0, sync_fun=pair)
        hex_tx, txid = self.spend_of_coinbase(n0, 1, other)
        self.generatetoaddress(n0, MATURITY - 2, other, sync_fun=pair)
        assert_equal(n0.getblockcount(), MATURITY - 1)

        # Depth 149 at block 150: premature under 150, although mature under 100.
        res = n0.testmempoolaccept([hex_tx])[0]
        assert_equal(res['allowed'], False)
        assert_equal(res['reject-reason'], 'bad-txns-premature-spend-of-coinbase')

        self.generatetoaddress(n0, 1, other, sync_fun=pair)
        n0.sendrawtransaction(hex_tx)
        self.sync_mempools([n0, n1])
        assert txid in n1.getrawmempool()

        self.log.info("Disconnect the tip on both nodes, as an anchor-driven rollback would")
        tip = n0.getbestblockhash()
        for n in (n0, n1):
            n.invalidateblock(tip)
            assert_equal(n.getblockcount(), MATURITY - 1)
            assert txid not in n.getrawmempool(), "node%d kept a premature spend" % n.index
        self.assert_block_refuses(n0, hex_tx, MATURITY - 1)

        self.log.info("Both nodes still build blocks")
        for n in (n0, n1):
            assert_equal(len(n.generatetoaddress(1, self.addr(n), invalid_call=False)), 1)
            pair()
        assert_equal(n0.getblockcount(), MATURITY + 1)

        self.log.info("Once mature again the spend is admitted and confirms")
        n0.sendrawtransaction(hex_tx)
        self.generatetoaddress(n0, 1, other, sync_fun=pair)
        assert_equal(n1.getrawtransaction(txid, True)['confirmations'], 1)

    def test_boundary(self):
        n = self.nodes[2]
        self.log.info("Boundary: maturity rises from 100 to %d at height %d", MATURITY, BOUNDARY)
        a = self.addr(n)
        self.generatetoaddress(n, 1, a, sync_fun=self.no_op)
        hex_tx, txid = self.spend_of_coinbase(n, 1, a)
        self.generatetoaddress(n, BOUNDARY - 3, self.addr(n), sync_fun=self.no_op)
        assert_equal(n.getblockcount(), BOUNDARY - 2)

        # Depth 128 at block 129, where the maturity is still 100.
        n.sendrawtransaction(hex_tx)
        assert txid in n.getrawmempool()

        self.log.info("Block %d is produced without it, as a full block or a low fee rate would leave it",
                      BOUNDARY - 1)
        self.generateblock(n, self.addr(n), [], sync_fun=self.no_op)
        assert_equal(n.getblockcount(), BOUNDARY - 1)
        assert txid not in n.getrawmempool(), "a spend premature past the boundary stayed resident"
        self.assert_block_refuses(n, hex_tx, BOUNDARY - 1)

        self.log.info("Block %d builds", BOUNDARY)
        assert_equal(len(n.generatetoaddress(1, self.addr(n), invalid_call=False)), 1)
        assert_equal(n.getblockcount(), BOUNDARY)

        self.log.info("Under the new maturity the spend is refused until depth %d", MATURITY)
        self.generatetoaddress(n, MATURITY - 1 - BOUNDARY, self.addr(n), sync_fun=self.no_op)
        assert_equal(n.getblockcount(), MATURITY - 1)
        res = n.testmempoolaccept([hex_tx])[0]
        assert_equal(res['reject-reason'], 'bad-txns-premature-spend-of-coinbase')
        self.generatetoaddress(n, 1, self.addr(n), sync_fun=self.no_op)
        n.sendrawtransaction(hex_tx)
        self.generatetoaddress(n, 1, self.addr(n), sync_fun=self.no_op)
        assert_equal(n.getrawtransaction(txid, True)['confirmations'], 1)

    def test_height_zero_refused(self):
        self.log.info("-con_coinbase_maturity_height=0 with a maturity set is refused at start-up")
        self.stop_node(2)
        self.nodes[2].assert_start_raises_init_error(
            BASE + ["-con_coinbase_maturity=%d" % MATURITY, "-con_coinbase_maturity_height=0"],
            "Error: -con_coinbase_maturity_height must be at least 1 when -con_coinbase_maturity is set "
            "(1 applies it from the first block)")
        self.log.info("...and still accepted with no maturity set, where it means nothing")
        self.start_node(2, BASE + ["-con_coinbase_maturity_height=0"])


if __name__ == '__main__':
    MempoolCoinbaseMaturityTest().main()
