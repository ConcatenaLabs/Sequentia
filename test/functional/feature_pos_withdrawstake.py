#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unstake: withdrawstake returns matured staking outputs to the wallet.

registerstake locks SEQ into the canonical staking output; the unbonding lock
(BIP68 CSV) counts from the block that FUNDS the stake. withdrawstake is the
inverse: it finds the wallet's own staking UTXOs, refuses with a dated message
while they are still unbonding, and once mature spends them back to a fresh
wallet address with the staker key. The GUI Staking page drives exactly these
two RPCs (plus liststakeutxos for the "withdrawable now / still unbonding"
card), so this test is the contract the page relies on.

Covered:
 - liststakeutxos: the funded stake appears, immature, with the exact height it
   becomes withdrawable at
 - withdrawstake before maturity: a clear error naming that height
 - withdrawstake at maturity: the whole stake returns to the wallet, minus the
   fee it names; the registry forgets the key when the withdrawal confirms; the
   returned coins are immediately spendable (a normal receive)
 - partial withdrawal: exactly the requested amount leaves the stake, the
   remainder is re-staked to the same key, and the remainder's unbonding clock
   restarts (a fresh withdrawstake is refused again with the new date)
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

UNBONDING = 5
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosWithdrawStakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()  # block producer (config-layer stake)
        self.extra_args = [[
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=%d" % UNBONDING,
            "-posslotinterval=1",
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, COIN),
            "-validatepegin=0",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.a_wif)

    def run_test(self):
        n0 = self.nodes[0]
        self.mine(1)

        self.log.info("Register a 100 SEQ stake from the wallet, exactly as the GUI does")
        addr = n0.getnewaddress()
        pub = n0.getaddressinfo(addr)["pubkey"]
        reg = n0.registerstake(pub, 100)
        self.mine(1)
        fund_height = n0.getblockcount()
        assert_equal(n0.getstakerinfo()[pub], 100 * COIN)

        utxos = n0.liststakeutxos()
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]["pubkey"], pub)
        assert_equal(utxos[0]["amount"], Decimal(100))
        assert_equal(utxos[0]["withdrawable"], False)
        assert_equal(utxos[0]["funded_height"], fund_height)
        assert_equal(utxos[0]["spendable_height"], fund_height + UNBONDING)
        assert "unbonding until block %d" % (fund_height + UNBONDING) in utxos[0]["status"]

        self.log.info("Too early: withdrawstake refuses, naming the unlock height")
        assert_raises_rpc_error(-4, "unbonding until block %d" % (fund_height + UNBONDING),
                                n0.withdrawstake)

        self.log.info("Mature the unbonding lock and withdraw everything")
        while n0.getblockcount() + 1 < fund_height + UNBONDING:
            self.mine(1)
        assert_equal(n0.liststakeutxos()[0]["withdrawable"], True)

        balance_before = n0.getbalance()["bitcoin"]
        res = n0.withdrawstake()
        assert_equal(res["unstaked"], Decimal(100))
        assert_equal(res["amount"] + res["fee"], Decimal(100))
        assert "restaked" not in res
        assert_equal(len(res["withdrawn_outputs"]), 1)
        assert_equal(res["withdrawn_outputs"][0]["txid"], reg["txid"])
        assert_equal(res["stake_before"] - res["stake_after"], 100 * COIN)
        self.mine(1)
        assert pub not in n0.getstakerinfo()
        assert_equal(n0.liststakeutxos(), [])
        # The coins came back as a normal receive: balance up by 100 minus the
        # fee the result named (the registration's own fee already hit the
        # balance earlier), and immediately spendable.
        assert_equal(n0.getbalance()["bitcoin"], balance_before + Decimal(100) - res["fee"])
        n0.sendtoaddress(n0.getnewaddress(), 50)
        self.mine(1)

        self.log.info("Partial withdrawal: 60 of 200 leaves, 140 is re-staked with a fresh clock")
        addr2 = n0.getnewaddress()
        pub2 = n0.getaddressinfo(addr2)["pubkey"]
        n0.registerstake(pub2, 200)
        self.mine(1)
        fund2 = n0.getblockcount()
        while n0.getblockcount() + 1 < fund2 + UNBONDING:
            self.mine(1)

        res = n0.withdrawstake(pub2, 60)
        assert_equal(res["unstaked"], Decimal(60))
        assert_equal(res["amount"] + res["fee"], Decimal(60))
        assert_equal(res["restaked"], Decimal(140))
        self.mine(1)
        restake_height = n0.getblockcount()
        assert_equal(n0.getstakerinfo()[pub2], 140 * COIN)

        utxos = n0.liststakeutxos()
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]["amount"], Decimal(140))
        assert_equal(utxos[0]["withdrawable"], False)  # the clock restarted
        assert_equal(utxos[0]["spendable_height"], restake_height + UNBONDING)
        assert_raises_rpc_error(-4, "unbonding until block %d" % (restake_height + UNBONDING),
                                n0.withdrawstake)

        self.log.info("A withdrawal above the mature total is refused")
        while n0.getblockcount() + 1 < restake_height + UNBONDING:
            self.mine(1)
        assert_raises_rpc_error(-4, "only 140.00", n0.withdrawstake, pub2, 150)

        self.log.info("...and the re-staked remainder withdraws normally once mature")
        res = n0.withdrawstake(pub2, 140)
        assert_equal(res["unstaked"], Decimal(140))
        self.mine(1)
        assert pub2 not in n0.getstakerinfo()
        assert_equal(n0.liststakeutxos(), [])

        self.log.info("withdrawstake: register, refuse-while-unbonding, withdraw, split — OK")


if __name__ == '__main__':
    PosWithdrawStakeTest().main()
