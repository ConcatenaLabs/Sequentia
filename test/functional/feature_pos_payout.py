#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Payout policies: how a block producer's fee reward is paid.

By default a block's fees go to the producer. A producer may instead COMMIT to a
payout policy, announced on-chain in a bare record spendable by itself:

    <"SEQPAY"> OP_DROP <activation> OP_DROP <mode> OP_DROP <param> OP_DROP
        <signer> OP_CHECKSIG

  direct  - the coinbase must pay a committed scriptPubKey. The operator cannot
            silently redirect the reward. The chain does not check the
            destination is fair; delegators audit it and can leave.
  lottery - the coinbase must pay ONE participant, drawn from everyone who
            delegated to this signer, weighted by stake, from a seed derived from
            Bitcoin's proof of work. Each delegator earns its proportional share
            over time with no accounting, at the cost of lumpy payouts.

A policy may not bind until `activation`, which consensus requires to be at least
`-pospayoutnotice` blocks beyond the block announcing it. Since a delegator can
leave a pool instantly and unilaterally (see feature_pos_delegation.py), that
notice is what makes auditing a pool meaningful: an operator cannot flip the
rewards to itself and collect before its delegators can react.

The producer builds the coinbase from the same function ConnectBlock enforces
(PosRequiredCoinbaseScript), so the two can never disagree.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, OP_0, hash160

UNBONDING = 5
NOTICE = 3
COIN = 100_000_000
FEE = 100_000
RECORD_VALUE = 1_000_000


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes()
    return k, wif, pub.hex(), pub


def p2wpkh(pub_bytes):
    return CScript([OP_0, hash160(pub_bytes)])


class PosPayoutTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

        self.a_key, self.a_wif, self.a_pub, self.a_raw = make_key()      # the pool / producer
        self.al_key, self.al_wif, self.al_pub, self.al_raw = make_key()  # delegator alice (3x)
        self.bo_key, self.bo_wif, self.bo_pub, self.bo_raw = make_key()  # delegator bob   (1x)
        self.ch_key, self.ch_wif, self.ch_pub, self.ch_raw = make_key()  # a payout address

        self.extra_args = [[
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=%d" % UNBONDING,
            "-pospayoutnotice=%d" % NOTICE,
            "-posslotinterval=1",
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=0",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1",
            "-staker=%s:%d" % (self.a_pub, COIN),
            "-validatepegin=0",
            "-txindex=1",
            "-persistmempool=0",
        ]]

    def find_free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output found")

    def spend_change(self, outs):
        """Spend the tracked OP_TRUE change into `outs`, paying a fee."""
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(self.change[0], 16), self.change[1]), nSequence=0xfffffffe)]
        total = sum(v for v, _ in outs)
        tx.vout = [CTxOut(v, s) for v, s in outs]
        rest = self.change[2] - total - FEE
        tx.vout.append(CTxOut(rest, CScript([0x51])))
        tx.vout.append(CTxOut(FEE))
        txid = self.nodes[0].sendrawtransaction(tx.serialize().hex())
        self.change = (txid, len(outs), rest)
        return txid

    def mine_fee_block(self):
        """Mine a block that carries a fee, so the coinbase has a paying output."""
        self.spend_change([])
        return self.nodes[0].generateposblock(self.a_wif)['hash']

    def coinbase_payee(self, blockhash):
        """The scriptPubKey the block's fee-bearing coinbase output pays."""
        blk = self.nodes[0].getblock(blockhash, 2)
        payees = [o['scriptPubKey']['hex'] for o in blk['tx'][0]['vout'] if o.get('value', 0) > 0]
        assert_equal(len(payees), 1)
        return payees[0]

    def announce(self, activation, mode, payout_script=None, commission_bp=None):
        rec = self.nodes[0].getpayoutscript(self.a_pub, activation, mode,
                                            payout_script.hex() if payout_script else None,
                                            commission_bp)
        return bytes.fromhex(rec["script"])

    def run_test(self):
        n0 = self.nodes[0]
        n0.generateposblock(self.a_wif)

        # Two delegators back the pool, 3:1 by stake.
        al_stake = bytes.fromhex(n0.getstakescript(self.al_pub, UNBONDING)["script"])
        bo_stake = bytes.fromhex(n0.getstakescript(self.bo_pub, UNBONDING)["script"])
        txid, voutn, in_amount = self.find_free_coin(n0)
        al_amt, bo_amt = 3 * COIN, 1 * COIN
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        fund.vout = [
            CTxOut(al_amt, al_stake),
            CTxOut(bo_amt, bo_stake),
            CTxOut(in_amount - al_amt - bo_amt - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        fund_txid = n0.sendrawtransaction(fund.serialize().hex())
        n0.generateposblock(self.a_wif)
        self.change = (fund_txid, 2, in_amount - al_amt - bo_amt - FEE)

        self.spend_change([(RECORD_VALUE, bytes.fromhex(n0.getdelegationscript(self.al_pub, self.a_pub)["script"])),
                           (RECORD_VALUE, bytes.fromhex(n0.getdelegationscript(self.bo_pub, self.a_pub)["script"]))])
        n0.generateposblock(self.a_wif)
        assert_equal(n0.getstakerinfo()[self.a_pub], COIN + al_amt + bo_amt)

        self.log.info("With no policy, the coinbase pays the producer")
        leader_spk = p2wpkh(self.a_raw).hex()
        assert_equal(self.coinbase_payee(self.mine_fee_block()), leader_spk)
        assert_equal(n0.getpayoutinfo(), {})

        self.log.info("A payout policy cannot bind before its notice period elapses")
        too_soon = self.announce(n0.getblockcount() + NOTICE - 1, "direct", p2wpkh(self.ch_raw))
        change_before = self.change   # the rejected tx is dropped; rewind to its input
        self.spend_change([(RECORD_VALUE, too_soon)])
        tip_before, height_before = n0.getbestblockhash(), n0.getblockcount()
        try:
            n0.generateposblock(self.a_wif)
        except Exception:
            pass
        assert_equal(n0.getbestblockhash(), tip_before)   # bad-payout-notice
        assert_equal(n0.getblockcount(), height_before)
        self.restart_node(0)                              # -persistmempool=0 drops it
        self.change = change_before

        self.log.info("DIRECT: after activation the coinbase must pay the committed script")
        activation = n0.getblockcount() + NOTICE + 2
        rec = self.announce(activation, "direct", p2wpkh(self.ch_raw))
        self.spend_change([(RECORD_VALUE, rec)])
        announce_block = n0.generateposblock(self.a_wif)['hash']
        info = n0.getpayoutinfo()[self.a_pub]
        assert_equal(len(info), 1)
        assert_equal(info[0]["mode"], "direct")
        assert_equal(info[0]["in_force"], False)          # still inside the notice
        assert_equal(info[0]["payout_script"], p2wpkh(self.ch_raw).hex())

        # Before activation the producer is still paid: the announcement alone
        # changes nothing, which is exactly what protects the delegators.
        while n0.getblockcount() < activation - 1:
            assert_equal(self.coinbase_payee(self.mine_fee_block()), leader_spk)
        assert_equal(n0.getpayoutinfo()[self.a_pub][0]["in_force"], False)

        # From the activation height onward it binds.
        payee = self.coinbase_payee(self.mine_fee_block())
        assert_equal(n0.getblockcount(), activation)
        assert_equal(payee, p2wpkh(self.ch_raw).hex())
        assert_equal(n0.getpayoutinfo()[self.a_pub][0]["in_force"], True)

        self.log.info("Policy survives a restart (rebuilt from the UTXO set)")
        self.restart_node(0)
        assert_equal(n0.getpayoutinfo()[self.a_pub][0]["in_force"], True)
        assert_equal(self.coinbase_payee(self.mine_fee_block()), p2wpkh(self.ch_raw).hex())

        self.log.info("Policy reverts across a reorg: disconnecting its announcement drops it")
        n0.invalidateblock(announce_block)
        assert_equal(n0.getpayoutinfo(), {})
        n0.reconsiderblock(announce_block)
        assert_equal(n0.getpayoutinfo()[self.a_pub][0]["in_force"], True)

        self.log.info("LOTTERY: the coinbase pays a delegator drawn by stake weight")
        lot_activation = n0.getblockcount() + NOTICE + 1
        lot = self.announce(lot_activation, "lottery", None, 0)
        self.spend_change([(RECORD_VALUE, lot)])
        n0.generateposblock(self.a_wif)
        while n0.getblockcount() < lot_activation:
            self.mine_fee_block()

        al_spk, bo_spk = p2wpkh(self.al_raw).hex(), p2wpkh(self.bo_raw).hex()
        # The participants are everyone whose weight counts for this signer. That
        # includes the operator's OWN stake (it has not delegated its away), so at
        # zero commission it still earns in proportion to its own stake -- it just
        # takes no cut of its delegators' share. Weights: alice 3, bob 1, operator 1.
        winners = {}
        for _ in range(60):
            w = self.coinbase_payee(self.mine_fee_block())
            assert w in (al_spk, bo_spk, leader_spk), "coinbase paid a non-participant"
            winners[w] = winners.get(w, 0) + 1
        assert al_spk in winners and bo_spk in winners, "a delegator never won: %s" % winners
        # Alice staked 3x bob, so she should win clearly more often.
        assert winners[al_spk] > winners[bo_spk], winners
        # ...and more often than the operator's own 1x stake.
        assert winners[al_spk] > winners.get(leader_spk, 0), winners
        self.log.info("  lottery winners over 60 blocks: alice=%d bob=%d operator=%d"
                      % (winners[al_spk], winners[bo_spk], winners.get(leader_spk, 0)))

        self.log.info("Payout policies: announce, notice, direct, lottery — OK")


if __name__ == '__main__':
    PosPayoutTest().main()
