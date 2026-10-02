#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A PoS producer still builds blocks after a rollback makes a spend premature.

A Bitcoin reorg rolls Sequentia back with it, so a one-block rollback is an
ordinary event here. A leader-fee coinbase spend admitted at exactly the
chain's coinbase maturity is premature again after one: it must leave the
mempool, or every block the producer assembles carries it and fails validation,
and a chain where every producer holds the entry stops.

Runs on the custom chain's default mempool consistency checks.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut

MATURITY = 150
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    return wif, k.get_pubkey().get_bytes().hex()


class PosMaturityReorgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.wif, self.pub = make_staker()
        self.extra_args = [[
            "-con_pos=1", "-posvrf=1", "-posunbonding=5", "-posslotinterval=1",
            "-signblockscript=51", "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1", "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_coinbase_maturity=%d" % MATURITY,
            "-staker=%s:%d" % (self.pub, 100 * COIN),
            "-validatepegin=0", "-txindex=1", "-par=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.wif)

    def free_coin(self):
        n = self.nodes[0]
        genesis = n.getblock(n.getblockhash(0), 2)
        for tx in genesis['tx']:
            for v in tx['vout']:
                if v['scriptPubKey']['hex'] == '51' and v.get('value', 0) > 0 and n.gettxout(tx['txid'], v['n']):
                    return tx['txid'], v['n'], int(v['value'] * COIN)
        raise AssertionError("no free coin")

    def run_test(self):
        n = self.nodes[0]
        self.mine(1)

        self.log.info("A block whose fee pays the leader a coinbase output")
        txid, voutn, amt = self.free_coin()
        fee = 1 * COIN
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        spk = bytes.fromhex(n.getaddressinfo(n.getnewaddress())['scriptPubKey'])
        tx.vout = [CTxOut(amt - fee, spk), CTxOut(fee)]
        n.sendrawtransaction(tx.serialize().hex(), 0)
        self.mine(1)
        h = n.getblockcount()
        cb = n.getblock(n.getblockhash(h), 2)['tx'][0]
        fee_out = [v for v in cb['vout'] if v.get('value', 0) > 0]
        assert_equal(len(fee_out), 1)
        fee_out = fee_out[0]

        dest = n.getaddressinfo(n.getnewaddress())['unconfidential']
        raw = n.createrawtransaction([{'txid': cb['txid'], 'vout': fee_out['n']}],
                                     [{dest: fee_out['value'] - Decimal('0.001')}, {'fee': Decimal('0.001')}])
        signed = n.signrawtransactionwithkey(raw, [self.wif], [{
            'txid': cb['txid'], 'vout': fee_out['n'],
            'scriptPubKey': fee_out['scriptPubKey']['hex'], 'amount': fee_out['value']}])
        assert_equal(signed['complete'], True)
        spend = signed['hex']
        spend_id = n.decoderawtransaction(spend)['txid']

        while n.getblockcount() < h + MATURITY - 2:
            self.mine(1)
        r = n.testmempoolaccept([spend])[0]
        assert_equal(r['reject-reason'], 'bad-txns-premature-spend-of-coinbase')
        self.mine(1)
        n.sendrawtransaction(spend)
        self.log.info("Tip %d: the spend is admitted at exactly the maturity (%d)", n.getblockcount(), MATURITY)

        self.log.info("Roll back one block: the spend is premature and leaves the mempool")
        n.invalidateblock(n.getbestblockhash())
        assert_equal(n.getblockcount(), h + MATURITY - 2)
        assert spend_id not in n.getrawmempool()
        # generateblock cannot build a PoS block (it has no VRF proof), so the
        # consensus refusal of the same spend in a block is forced in
        # mempool_coinbase_maturity.py instead.

        self.log.info("The producer builds the next block")
        # Other traffic, so the rebuilt block is not byte-identical to the one
        # just invalidated (which held nothing): a duplicate of an invalid
        # block is refused for that alone, and would prove nothing here.
        other = n.sendtoaddress(address=dest, amount=1, fee_asset_label='bitcoin')
        self.mine(1)
        assert_equal(n.getblockcount(), h + MATURITY - 1)
        assert other in n.getblock(n.getbestblockhash())['tx']

        self.log.info("Mature again, the spend is admitted and confirms in a produced block")
        n.sendrawtransaction(spend)
        self.mine(1)
        assert_equal(n.getrawtransaction(spend_id, True)['confirmations'], 1)


if __name__ == '__main__':
    PosMaturityReorgTest().main()
