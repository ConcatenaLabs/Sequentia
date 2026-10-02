#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""claimpoolrewards sweeps only pots that are mature under the chain's maturity.

Pot outputs are coinbase outputs, so a claim spending one is held to coinbase
maturity by consensus (CheckTxInputs, via CoinbaseMaturityAt). The real chains
hold that at 1,000 blocks -- the wall-clock figure of Bitcoin's 100 at a
60-second cadence -- not the inherited 100. A claim that sweeps a pot between
100 and 999 blocks deep is a transaction that cannot confirm.

This runs a custom chain with -con_coinbase_maturity=1000 and places pots at
four depths relative to the claim's spend height: 1,000 (mature), 999, 500 and
100 (all immature). The claim must take exactly the first, and confirm. A
one-block rollback right after the claim leaves it premature; the producer must
still build the next block.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript

MATURITY = 1000
NOTICE = 10
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


def pot_script_hex(signer_hex):
    return "06" + b"SEQPOT".hex() + "75" + "21" + signer_hex + "75" + "51"


class PosSplitMaturityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()
        self.extra_args = [[
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=5",
            "-posslotinterval=1",
            "-pospayoutnotice=%d" % NOTICE,
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_coinbase_maturity=%d" % MATURITY,
            "-staker=%s:%d" % (self.a_pub, 100 * COIN),
            "-validatepegin=0",
            "-txindex=1",
            "-acceptnonstdtxn=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.a_wif)

    def mine_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.mine(1)
        assert_equal(self.nodes[0].getblockcount(), height)

    def find_free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output found")

    def pot_outpoint(self, height):
        """The pot output a block's coinbase created, as (txid, vout)."""
        cb = self.nodes[0].getblock(self.nodes[0].getblockhash(height), 2)['tx'][0]
        found = [(cb['txid'], v['n']) for v in cb['vout']
                 if v['scriptPubKey']['hex'] == pot_script_hex(self.a_pub) and v.get('value', 0) > 0]
        assert_equal(len(found), 1)
        return found[0]

    def make_pot(self, w):
        """One block whose fees pay the pool's pot; returns its height."""
        w.settxfee(Decimal("0.02"))
        for _ in range(3):
            w.sendtoaddress(address=w.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        h = self.nodes[0].getblockcount()
        self.pot_outpoint(h)
        return h

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        self.log.info("Announce a split policy and fund the wallet from the genesis free coin")
        activation = n0.getblockcount() + NOTICE + 5
        rec = n0.getpayoutscript(self.a_pub, activation, "split", None, 0)
        w0_spk = bytes.fromhex(w0.getaddressinfo(w0.getnewaddress())["scriptPubKey"])
        txid, voutn, in_amount = self.find_free_coin(n0)
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        record_value = 1_000_000
        fee = 100_000
        endowment = 5000 * COIN
        fund.vout = [
            CTxOut(record_value, bytes.fromhex(rec["script"])),
            CTxOut(endowment, w0_spk),
            CTxOut(in_amount - record_value - endowment - fee, CScript([0x51])),
            CTxOut(fee),
        ]
        n0.sendrawtransaction(fund.serialize().hex())
        self.mine(1)
        w0.delegatestake(self.a_pub, 150)
        self.mine(1)
        self.mine_to(activation)

        self.log.info("Pots at four depths relative to the claim: 1000, 999, 500, 100")
        h1 = self.make_pot(w0)
        h2 = self.make_pot(w0)
        assert_equal(h2, h1 + 1)
        claim_height = h1 + MATURITY          # the block the claim confirms in
        self.mine_to(claim_height - 500 - 1)
        h3 = self.make_pot(w0)
        self.mine_to(claim_height - 100 - 1)
        h4 = self.make_pot(w0)
        assert_equal([claim_height - h for h in (h1, h2, h3, h4)], [1000, 999, 500, 100])
        w0.settxfee(0)

        self.log.info("One block early every pot is immature, so there is nothing to claim")
        self.mine_to(claim_height - 2)   # a claim now would confirm at claim_height - 1
        # The oldest pot is 999 deep there: mature under the inherited 100, not
        # under the chain's 1,000.
        assert_raises_rpc_error(-4, "all still inside coinbase maturity (%d blocks)" % MATURITY,
                                w0.claimpoolrewards, self.a_pub)

        self.log.info("At 1,000 deep the oldest pot is claimed, and only it")
        self.mine(1)
        assert_equal(n0.getblockcount() + 1, claim_height)
        claim = w0.claimpoolrewards(self.a_pub)
        assert_equal(claim["pot_outputs"], 1)
        tx = n0.getrawtransaction(claim["txid"], True)
        spent = [(i["txid"], i["vout"]) for i in tx["vin"]]
        assert_equal(spent, [self.pot_outpoint(h1)])

        # The claim is built at exactly the maturity for the next block, with
        # no margin, so a one-block rollback (what a Bitcoin reorg of the anchor
        # does) leaves it premature. It must leave the mempool rather than fail
        # every block the producer assembles. Its nLockTime is the tip it was
        # built on, so the same rollback also makes it non-final; either rule
        # evicts it (mempool_coinbase_maturity.py and
        # feature_pos_maturity_reorg.py cover a spend only maturity catches).
        self.log.info("A one-block rollback makes the fresh claim premature; the producer still builds")
        claim_hex = tx["hex"]
        n0.invalidateblock(n0.getbestblockhash())
        assert_equal(n0.getblockcount() + 2, claim_height)
        assert claim["txid"] not in n0.getrawmempool()
        # Other traffic, so the rebuilt block is not byte-identical to the
        # invalidated one, which a node refuses as a duplicate for that alone.
        other = w0.sendtoaddress(address=w0.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        assert_equal(n0.getblockcount() + 1, claim_height)
        assert other in n0.getblock(n0.getbestblockhash())["tx"]
        n0.sendrawtransaction(claim_hex)
        self.mine(1)
        tx = n0.getrawtransaction(claim["txid"], True)
        assert_equal(n0.getblock(tx["blockhash"])["height"], claim_height)

        self.log.info("The immature pots (999, 500 and 100 deep at the claim) are still unspent")
        for h in (h2, h3, h4):
            txid, n = self.pot_outpoint(h)
            assert n0.gettxout(txid, n) is not None, "pot at height %d was swept early" % h


if __name__ == '__main__':
    PosSplitMaturityTest().main()
