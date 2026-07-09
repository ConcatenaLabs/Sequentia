#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stake delegation: lend block-signing rights without moving the coins.

A staker (the CONTROLLER) may lend its stake weight to a SIGNER -- a staking-pool
operator, or simply its own online key -- by funding a bare delegation record:

    <"SEQDEL"> OP_DROP <signer> OP_DROP <controller> OP_CHECKSIG

While that record is unspent, the controller's weight counts for the signer, and
the signer is the key that must produce and sign blocks. Two properties matter:

  * The signer can NEVER spend the stake. Only the controller can. Delegation is
    non-custodial: the coins never move, and the key that can move them can stay
    offline.
  * The record is a SEPARATE output, so re-pointing it (rotation) or reclaiming
    it never touches the staking output. That is what makes delegation work for a
    stake frozen for years by a vesting lock (liquid_locktime), whose script can
    not be spent at all -- a signer named inside it could never be replaced.

Under VRF sortition any staker with weight may produce a block, so "who holds the
weight" is directly observable as "who can produce a block". This test uses that
to show the right to produce moving from the controller to the pool and back.

Covers: delegate, produce, the signer cannot steal, rotate while vesting-locked,
survive a restart (rebuild from the UTXO set), survive a reorg, reclaim, and the
consensus rule forbidding two live records for one controller.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, LegacySignatureHash, SIGHASH_ALL

UNBONDING = 5
LOCK_HEIGHT = 4000        # far beyond the test: the stake stays frozen throughout
COIN = 100_000_000
FEE = 100_000
RECORD_VALUE = 1_000_000


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return k, wif, pub


class PosDelegationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

        self.a_key, self.a_wif, self.a_pub = make_key()   # bootstrap producer
        self.c_key, self.c_wif, self.c_pub = make_key()   # controller (vested)
        self.d_key, self.d_wif, self.d_pub = make_key()   # controller (unlocked)
        self.p1_key, self.p1_wif, self.p1_pub = make_key()  # pool 1
        self.p2_key, self.p2_wif, self.p2_pub = make_key()  # pool 2

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

    def can_produce(self, node, wif):
        """Whether this key may produce a block: i.e. whether it holds weight."""
        try:
            node.generateposblock(wif)
            return True
        except Exception:
            return False

    def spend_to(self, key, script, txid, vout, in_value, outs):
        """Spend a bare script-locked output, paying `outs` = [(value, script)]."""
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), vout), nSequence=0xfffffffe)]
        total = sum(v for v, _ in outs)
        tx.vout = [CTxOut(v, s) for v, s in outs]
        tx.vout.append(CTxOut(in_value - total - FEE, CScript([0x51])))
        tx.vout.append(CTxOut(FEE))
        sighash, err = LegacySignatureHash(CScript(script), tx, 0, SIGHASH_ALL)
        assert err is None
        tx.vin[0].scriptSig = CScript([key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        return tx

    def run_test(self):
        n0 = self.nodes[0]
        n0.generateposblock(self.a_wif)

        # C's stake is frozen by a vesting lock; D's is an ordinary stake.
        c_stake_script = bytes.fromhex(
            n0.getstakescript(self.c_pub, UNBONDING, None, None, None, LOCK_HEIGHT)["script"])
        d_stake_script = bytes.fromhex(n0.getstakescript(self.d_pub, UNBONDING)["script"])

        txid, voutn, in_amount = self.find_free_coin(n0)
        c_amount, d_amount = 5 * COIN, 3 * COIN
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        fund.vout = [
            CTxOut(c_amount, c_stake_script),
            CTxOut(d_amount, d_stake_script),
            CTxOut(in_amount - c_amount - d_amount - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        fund_txid = n0.sendrawtransaction(fund.serialize().hex())
        n0.generateposblock(self.a_wif)
        change_txid, change_n, change_val = fund_txid, 2, in_amount - c_amount - d_amount - FEE

        self.log.info("Before delegation: the controller holds the weight and may produce")
        assert_equal(n0.getstakerinfo()[self.c_pub], c_amount)
        assert self.p1_pub not in n0.getstakerinfo()
        assert_equal(n0.getdelegationinfo(), {})
        assert self.can_produce(n0, self.c_wif)
        assert not self.can_produce(n0, self.p1_wif)

        self.log.info("Delegating moves the weight (and the right to produce) to the pool")
        rec1 = bytes.fromhex(n0.getdelegationscript(self.c_pub, self.p1_pub)["script"])
        deleg_tx = CTransaction()
        deleg_tx.nVersion = 2
        deleg_tx.vin = [CTxIn(COutPoint(int(change_txid, 16), change_n), nSequence=0xfffffffe)]
        deleg_tx.vout = [
            CTxOut(RECORD_VALUE, rec1),
            CTxOut(change_val - RECORD_VALUE - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        rec_txid = n0.sendrawtransaction(deleg_tx.serialize().hex())
        n0.generateposblock(self.a_wif)
        change_txid, change_n, change_val = rec_txid, 1, change_val - RECORD_VALUE - FEE

        assert_equal(n0.getdelegationinfo(), {self.c_pub: self.p1_pub})
        assert_equal(n0.getstakerinfo()[self.p1_pub], c_amount)
        assert self.c_pub not in n0.getstakerinfo()
        # The coins did not move: the staking output is still unspent.
        assert n0.gettxout(fund_txid, 0) is not None
        assert self.can_produce(n0, self.p1_wif)
        assert not self.can_produce(n0, self.c_wif)

        self.log.info("The pool cannot spend a delegator's stake")
        # D delegates its (unlocked, CSV-matured) stake to the same pool, then the
        # pool tries to take the coins. Only D's key satisfies the staking script.
        rec_d = bytes.fromhex(n0.getdelegationscript(self.d_pub, self.p1_pub)["script"])
        deleg_d = CTransaction()
        deleg_d.nVersion = 2
        deleg_d.vin = [CTxIn(COutPoint(int(change_txid, 16), change_n), nSequence=0xfffffffe)]
        deleg_d.vout = [
            CTxOut(RECORD_VALUE, rec_d),
            CTxOut(change_val - RECORD_VALUE - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        d_rec_txid = n0.sendrawtransaction(deleg_d.serialize().hex())
        n0.generateposblock(self.p1_wif)
        change_txid, change_n, change_val = d_rec_txid, 1, change_val - RECORD_VALUE - FEE
        assert_equal(n0.getstakerinfo()[self.p1_pub], c_amount + d_amount)

        for _ in range(UNBONDING):
            n0.generateposblock(self.p1_wif)
        theft = CTransaction()
        theft.nVersion = 2
        theft.vin = [CTxIn(COutPoint(int(fund_txid, 16), 1), nSequence=UNBONDING)]
        theft.vout = [CTxOut(d_amount - FEE, CScript([0x51])), CTxOut(FEE)]
        sighash, err = LegacySignatureHash(CScript(d_stake_script), theft, 0, SIGHASH_ALL)
        assert err is None
        theft.vin[0].scriptSig = CScript([self.p1_key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        # The pool's signature does not satisfy the controller's OP_CHECKSIG.
        assert_raises_rpc_error(-26, "Signature must be zero for failed CHECK(MULTI)SIG operation",
                                n0.sendrawtransaction, theft.serialize().hex())
        # ...and the stake is still there, still counted for the pool.
        assert n0.gettxout(fund_txid, 1) is not None

        self.log.info("Rotating to a new pool works while the stake is frozen by its vesting lock")
        # Spend the record (only C can) and create a replacement in the same tx.
        rec2 = bytes.fromhex(n0.getdelegationscript(self.c_pub, self.p2_pub)["script"])
        rotate = self.spend_to(self.c_key, rec1, rec_txid, 0, RECORD_VALUE,
                               [(RECORD_VALUE - 2 * FEE, rec2)])
        rotate_txid = n0.sendrawtransaction(rotate.serialize().hex())
        rotate_block = n0.generateposblock(self.p1_wif)['hash']

        assert_equal(n0.getdelegationinfo()[self.c_pub], self.p2_pub)
        assert_equal(n0.getstakerinfo()[self.p2_pub], c_amount)
        assert_equal(n0.getstakerinfo()[self.p1_pub], d_amount)  # only D's now
        # The vesting-locked stake was never touched by the rotation.
        assert n0.gettxout(fund_txid, 0) is not None
        assert self.can_produce(n0, self.p2_wif)

        self.log.info("Delegation survives a restart (rebuilt from the UTXO set)")
        self.restart_node(0)
        assert_equal(n0.getdelegationinfo()[self.c_pub], self.p2_pub)
        assert_equal(n0.getstakerinfo()[self.p2_pub], c_amount)

        self.log.info("Delegation survives a reorg: disconnecting the rotation restores pool 1")
        n0.invalidateblock(rotate_block)
        assert_equal(n0.getdelegationinfo()[self.c_pub], self.p1_pub)
        assert_equal(n0.getstakerinfo()[self.p1_pub], c_amount + d_amount)
        n0.reconsiderblock(rotate_block)
        assert_equal(n0.getdelegationinfo()[self.c_pub], self.p2_pub)

        self.log.info("Two live records for one controller are rejected by consensus")
        dup_tx = CTransaction()
        dup_tx.nVersion = 2
        dup_tx.vin = [CTxIn(COutPoint(int(change_txid, 16), change_n), nSequence=0xfffffffe)]
        dup_tx.vout = [
            CTxOut(RECORD_VALUE, rec1),  # a SECOND record for C, first not spent
            CTxOut(change_val - RECORD_VALUE - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        n0.sendrawtransaction(dup_tx.serialize().hex())
        tip_before, height_before = n0.getbestblockhash(), n0.getblockcount()
        try:
            n0.generateposblock(self.p2_wif)   # builds a block containing dup_tx
        except Exception:
            pass
        # ConnectBlock rejects it (bad-delegation-exists): the tip does not move,
        # and the delegation in force is unchanged.
        assert_equal(n0.getbestblockhash(), tip_before)
        assert_equal(n0.getblockcount(), height_before)
        assert_equal(n0.getdelegationinfo()[self.c_pub], self.p2_pub)
        # -persistmempool=0, so the restart drops the offending transaction.
        self.restart_node(0)
        assert_equal(n0.getdelegationinfo()[self.c_pub], self.p2_pub)

        self.log.info("Reclaiming: spending the record returns signing rights to the controller")
        rotate_out = 0
        reclaim = self.spend_to(self.c_key, rec2, rotate_txid, rotate_out, RECORD_VALUE - 2 * FEE, [])
        n0.sendrawtransaction(reclaim.serialize().hex())
        n0.generateposblock(self.p1_wif)
        assert self.c_pub not in n0.getdelegationinfo()
        assert_equal(n0.getstakerinfo()[self.c_pub], c_amount)
        assert self.can_produce(n0, self.c_wif)

        self.log.info("Delegation: pools get the weight, never the coins — OK")


if __name__ == '__main__':
    PosDelegationTest().main()
