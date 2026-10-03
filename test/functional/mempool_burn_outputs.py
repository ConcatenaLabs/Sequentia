#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Relay policy: any number of value burns, still one data output.

A burn is an output whose script is a bare OP_RETURN, nothing after the opcode:
the amount it holds is destroyed. Relay policy limits a transaction to one
OP_RETURN output to bound the data carried on chain (multi-op-return). A burn
with a null nonce carries no data, so it does not count against that limit: an
issuer can destroy the remains of several outputs, in several assets, in one
transaction. The nonce is a field of the output its author can fill, so a burn
carrying one counts as a data output, like an OP_RETURN that pushes data.

Consensus accepts every shape here; the refused shapes are forced into blocks
to show the refusal is relay policy only.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import CTxOutNonce, tx_from_hex
from test_framework.p2p import P2PDataStore
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

    def build(self, special_outputs, nonce_payload=None):
        """A signed transaction spending one coin of each asset the special outputs
        burn (and one of the fee asset), with explicit change and an explicit fee.
        With nonce_payload, every bare burn carries it in its nonce."""
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
        if nonce_payload is not None:
            tx = tx_from_hex(raw)
            for o in tx.vout:
                if o.scriptPubKey == b'\x6a':
                    o.nNonce = CTxOutNonce(b'\x02' + nonce_payload)
            raw = tx.serialize().hex()
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
        self.generatetoaddress(self.node, COINBASE_MATURITY + 1, self.node.getnewaddress(), sync_fun=self.no_op)
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
        self.force_into_block(hex_tx)

        self.log.info("25 burns with a null nonce relay and confirm")
        hex_tx = self.build([{'burn': 0}] * 25)
        assert_equal(self.scripts(hex_tx).count('6a'), 25)
        assert_equal(self.node.testmempoolaccept([hex_tx])[0]['allowed'], True)
        self.confirm(self.node.sendrawtransaction(hex_tx))

        self.log.info("25 burns each carrying 32 bytes in the nonce are data outputs: refused")
        payload = b"data carried in a burn's nonce.."
        assert_equal(len(payload), 32)
        hex_tx = self.build([{'burn': 0}] * 25, nonce_payload=payload)
        assert_equal(bytes.fromhex(hex_tx).count(payload), 25)
        res = self.node.testmempoolaccept([hex_tx])[0]
        assert_equal(res['allowed'], False)
        assert_equal(res['reject-reason'], 'multi-op-return')
        # Over P2P, which is how such a transaction reaches the network
        # (sendrawtransaction refuses an unblinded output with a nonce before
        # relay policy is consulted).
        peer = self.node.add_p2p_connection(P2PDataStore())
        ptx = tx_from_hex(hex_tx)
        ptx.rehash()
        peer.send_txs_and_test([ptx], self.node, success=False, reject_reason='multi-op-return')
        self.node.disconnect_p2ps()
        self.force_into_block(hex_tx)

        self.log.info("One burn carrying a nonce is the transaction's one data output")
        hex_tx = self.build([{'burn': 0}], nonce_payload=payload)
        assert_equal(self.node.testmempoolaccept([hex_tx])[0]['allowed'], True)
        hex_tx = self.build([{'data': 'aa'}, {'burn': 0}], nonce_payload=payload)
        assert_equal(self.node.testmempoolaccept([hex_tx])[0]['reject-reason'], 'multi-op-return')

    def force_into_block(self, hex_tx):
        block = self.generateblock(self.node, output=self.node.getnewaddress(), transactions=[hex_tx],
                                   sync_fun=self.no_op)
        txid = self.node.decoderawtransaction(hex_tx)['txid']
        assert txid in self.node.getblock(block['hash'])['tx']
        assert_equal(self.node.getbestblockhash(), block['hash'])


if __name__ == '__main__':
    BurnOutputsTest().main()
