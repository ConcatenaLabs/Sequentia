#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Relay policy: any number of value burns, still one data output.

A burn is an output whose script is a bare OP_RETURN, nothing after the opcode:
it carries no data, and the amount it holds is destroyed. Relay policy limits a
transaction to one OP_RETURN output to bound the data carried on chain
(multi-op-return); burns carry none, so they do not count against that limit.
An issuer can destroy the remains of several outputs, in several assets, in one
transaction. The limit on data-carrying OP_RETURN outputs is unchanged.

Consensus accepts every shape here; the last case forces the still-refused one
into a block to show the refusal is relay policy only.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

FEE = Decimal('0.0005')


class BurnOutputsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-con_blocksubsidy=5000000000",
            "-validatepegin=0",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def utxo(self, asset):
        return max((u for u in self.node.listunspent() if u['asset'] == asset),
                   key=lambda u: u['amount'])

    def build(self, special_outputs):
        """A signed transaction spending one coin of each asset the special outputs
        burn (and one of the fee asset), with explicit change and an explicit fee."""
        spend = {self.btc: Decimal(0)}
        for o in special_outputs:
            if 'burn' in o:
                spend[o.get('asset', self.btc)] = spend.get(o.get('asset', self.btc), Decimal(0)) + Decimal(o['burn'])
        spend[self.btc] += FEE
        inputs, outputs = [], list(special_outputs)
        for asset, amount in spend.items():
            u = self.utxo(asset)
            inputs.append({'txid': u['txid'], 'vout': u['vout']})
            outputs.append({self.node.getnewaddress(): u['amount'] - amount, 'asset': asset})
        outputs.append({'fee': FEE})
        raw = self.node.createrawtransaction(inputs, outputs)
        signed = self.node.signrawtransactionwithwallet(raw)
        assert_equal(signed['complete'], True)
        return signed['hex']

    def confirm(self, txid):
        self.generate(self.node, 1, sync_fun=self.no_op)
        assert_equal(self.node.getrawtransaction(txid, True)['confirmations'], 1)

    def scripts(self, hex_tx):
        return [v['scriptPubKey']['hex'] for v in self.node.decoderawtransaction(hex_tx)['vout']]

    def run_test(self):
        self.node = self.nodes[0]
        self.btc = self.node.dumpassetlabels()['bitcoin']
        self.generatetoaddress(self.node, COINBASE_MATURITY + 10, self.node.getnewaddress(), sync_fun=self.no_op)
        issued = self.node.issueasset(assetamount=1000, tokenamount=0, blind=False, fee_asset='bitcoin')
        self.asset = issued['asset']
        self.generate(self.node, 1, sync_fun=self.no_op)

        self.log.info("Two burns, in two assets, relay and confirm")
        hex_tx = self.build([{'burn': 3, 'asset': self.asset}, {'burn': 1}])
        assert_equal(self.scripts(hex_tx).count('6a'), 2)
        res = self.node.testmempoolaccept([hex_tx])[0]
        assert_equal(res['allowed'], True)
        txid = self.node.sendrawtransaction(hex_tx)
        self.confirm(txid)
        burned = [(v['asset'], v['value']) for v in self.node.getrawtransaction(txid, True)['vout']
                  if v['scriptPubKey']['hex'] == '6a']
        assert_equal(sorted(burned), sorted([(self.asset, Decimal('3')), (self.btc, Decimal('1'))]))
        self.log.info("  two-burn transaction: %d vbytes", self.node.getrawtransaction(txid, True)['vsize'])

        self.log.info("One data output and two burns relay and confirm")
        hex_tx = self.build([{'data': 'aa'}, {'burn': 2, 'asset': self.asset}, {'burn': 1}])
        assert_equal(sorted(s for s in self.scripts(hex_tx) if s.startswith('6a')), ['6a', '6a', '6a01aa'])
        assert_equal(self.node.testmempoolaccept([hex_tx])[0]['allowed'], True)
        self.confirm(self.node.sendrawtransaction(hex_tx))

        self.log.info("Two data outputs are still refused, with the same error")
        hex_tx = self.build([{'data': 'aa'}, {'vdata': ['bb']}])
        res = self.node.testmempoolaccept([hex_tx])[0]
        assert_equal(res['allowed'], False)
        assert_equal(res['reject-reason'], 'multi-op-return')
        assert_raises_rpc_error(-26, "multi-op-return", self.node.sendrawtransaction, hex_tx)

        self.log.info("Two data outputs and a burn: still refused")
        hex_tx2 = self.build([{'data': 'aa'}, {'vdata': ['bb']}, {'burn': 1}])
        assert_equal(self.node.testmempoolaccept([hex_tx2])[0]['reject-reason'], 'multi-op-return')

        self.log.info("Consensus accepts the refused shape: it is relay policy only")
        block = self.generateblock(self.node, output=self.node.getnewaddress(), transactions=[hex_tx],
                                   sync_fun=self.no_op)
        txid = self.node.decoderawtransaction(hex_tx)['txid']
        assert txid in self.node.getblock(block['hash'])['tx']
        assert_equal(self.node.getbestblockhash(), block['hash'])


if __name__ == '__main__':
    BurnOutputsTest().main()
