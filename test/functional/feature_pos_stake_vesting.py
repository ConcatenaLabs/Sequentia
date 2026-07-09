#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Staking-only vesting periods: a stake that earns but cannot be sold.

Sequentia's mainnet pre-mine gives each allocation a cliff, a linear vest, and a
"staking-only period": tokens unlock for staking first and only become liquid
(sellable/transferable) months later. This test proves the primitive that makes
that expressible.

A staking output may carry an optional absolute timelock (BIP65):

    <csv> OP_CHECKSEQUENCEVERIFY OP_DROP [<blspub> OP_DROP <pop> OP_DROP]
        <liquid_locktime> OP_CHECKLOCKTIMEVERIFY OP_DROP <pubkey> OP_CHECKSIG

The pairing asserted here is the whole design:

  * the output CANNOT be spent -- so it cannot be sold or transferred -- until
    liquid_locktime, even once its CSV unbonding delay has matured; and
  * it accrues its FULL stake weight the entire time, because weight is credited
    for a staking UTXO merely existing (StakeFromTxOut). Producing a block never
    spends or references the stake outpoint.

So the tokens stake and earn fees while illiquid. Non-transferability falls out
of non-spendability; no covenant is involved. A relative (CSV) lock could not do
this: BIP68 locks are 16-bit, topping out at 65535*512s = 388 days.

An unlocked stake, spent at the same height, is the control: it isolates the
CLTV as the thing preventing the sale, rather than the CSV maturity.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, LegacySignatureHash, SIGHASH_ALL

UNBONDING = 5
LOCK_HEIGHT = 20          # absolute height at which the vested stake goes liquid
COIN = 100_000_000
FEE = 100_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return k, wif, pub


class PosStakeVestingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

        self.a_key, self.a_wif, self.a_pub = make_staker()   # block producer
        self.v_key, self.v_wif, self.v_pub = make_staker()   # vested staker
        self.u_key, self.u_wif, self.u_pub = make_staker()   # unlocked control

        self.extra_args = [[
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=%d" % UNBONDING,
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
        ]]

    def find_free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output found")

    def spend_stake(self, key, script, txid, vout, amount, nlocktime):
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = nlocktime
        tx.vin = [CTxIn(COutPoint(int(txid, 16), vout), nSequence=UNBONDING)]
        tx.vout = [CTxOut(amount - FEE, CScript([0x51])), CTxOut(FEE)]
        sighash, err = LegacySignatureHash(CScript(script), tx, 0, SIGHASH_ALL)
        assert err is None
        tx.vin[0].scriptSig = CScript([key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        return tx

    def run_test(self):
        n0 = self.nodes[0]
        n0.generateposblock(self.a_wif)

        # A vested staking output (locked until LOCK_HEIGHT) and an otherwise
        # identical unlocked one, funded from the same genesis coin.
        vested_script = bytes.fromhex(
            n0.getstakescript(self.v_pub, UNBONDING, None, None, None, LOCK_HEIGHT)["script"])
        unlocked_script = bytes.fromhex(
            n0.getstakescript(self.u_pub, UNBONDING)["script"])
        # The vesting lock is really in the script, and the plain form has none.
        assert_equal(n0.getstakescript(self.v_pub, UNBONDING, None, None, None,
                                       LOCK_HEIGHT)["liquid_locktime"], LOCK_HEIGHT)
        assert "liquid_locktime" not in n0.getstakescript(self.u_pub, UNBONDING)

        txid, voutn, in_amount = self.find_free_coin(n0)
        stake_amount = 2 * COIN
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        fund.vout = [
            CTxOut(stake_amount, vested_script),
            CTxOut(stake_amount, unlocked_script),
            CTxOut(in_amount - 2 * stake_amount - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        fund_txid = n0.sendrawtransaction(fund.serialize().hex())
        n0.generateposblock(self.a_wif)

        self.log.info("A vesting-locked stake accrues full weight while illiquid")
        info = n0.getstakerinfo()
        assert_equal(info[self.v_pub], stake_amount)
        assert_equal(info[self.u_pub], stake_amount)

        # Mature the CSV unbonding delay, staying well below LOCK_HEIGHT.
        while n0.getblockcount() < LOCK_HEIGHT - 6:
            n0.generateposblock(self.a_wif)
        height = n0.getblockcount()
        assert height < LOCK_HEIGHT
        assert height >= UNBONDING + 2   # CSV matured

        self.log.info("Control: the unlocked stake IS spendable now (CSV matured)")
        ctrl = self.spend_stake(self.u_key, unlocked_script, fund_txid, 1, stake_amount, 0)
        n0.sendrawtransaction(ctrl.serialize().hex())
        n0.generateposblock(self.a_wif)
        assert self.u_pub not in n0.getstakerinfo()

        self.log.info("The vested stake CANNOT be spent before its locktime")
        # (a) Not claiming the locktime at all: OP_CHECKLOCKTIMEVERIFY fails.
        early = self.spend_stake(self.v_key, vested_script, fund_txid, 0, stake_amount, 0)
        assert_raises_rpc_error(-26, "Locktime requirement not satisfied",
                                n0.sendrawtransaction, early.serialize().hex())
        # (b) Claiming it honestly: the transaction is simply not yet final.
        early2 = self.spend_stake(self.v_key, vested_script, fund_txid, 0, stake_amount, LOCK_HEIGHT)
        assert_raises_rpc_error(-26, "non-final",
                                n0.sendrawtransaction, early2.serialize().hex())

        # Still staking, still weighted, throughout the staking-only period.
        assert_equal(n0.getstakerinfo()[self.v_pub], stake_amount)

        self.log.info("At the locktime the stake becomes liquid and can be spent")
        while n0.getblockcount() < LOCK_HEIGHT:
            n0.generateposblock(self.a_wif)
        assert_equal(n0.getstakerinfo()[self.v_pub], stake_amount)  # weight until spent

        liquid = self.spend_stake(self.v_key, vested_script, fund_txid, 0, stake_amount, LOCK_HEIGHT)
        n0.sendrawtransaction(liquid.serialize().hex())
        n0.generateposblock(self.a_wif)
        assert self.v_pub not in n0.getstakerinfo()

        self.log.info("Staking-only vesting: stakes while locked, unsellable until liquid — OK")


if __name__ == '__main__':
    PosStakeVestingTest().main()
