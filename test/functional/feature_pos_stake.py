#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests on-chain stake registration via locked staking outputs.

Stake is real locked coins: an output paying the canonical bare script
    <csv> OP_CHECKSEQUENCEVERIFY OP_DROP <pubkey> OP_CHECKSIG
in the policy asset (explicit amount) adds its amount to the key's stake
weight while unspent. The registry is a pure function of the UTXO set:
mirrored on every tip connect/disconnect (reorg-safe) and rebuilt from the
UTXO set at startup. Unbonding is the CSV-gated spend, enforced by the
script — the paper's stake locktime. See doc/sequentia/04-proof-of-stake.md.

Scenario: staker A bootstraps the chain from the -staker config layer;
key B acquires stake by locking coins on-chain, produces blocks, then
unbonds (after the CSV matures) and loses eligibility — with a reorg
round-trip and a node restart along the way, all fully validated by a peer.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import CScript, LegacySignatureHash, SIGHASH_ALL

UNBONDING = 5
COIN = 100_000_000
FEE = 100_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return k, wif, pub


class PosOnChainStakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.a_key, self.a_wif, self.a_pub = make_staker()
        self.b_key, self.b_wif, self.b_pub = make_staker()

        common = [
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
            # A's config stake, scaled to coin atoms so on-chain stake amounts
            # are comparable.
            "-staker=%s:%d" % (self.a_pub, COIN),
            "-validatepegin=0",
            "-txindex=1",
        ]
        self.extra_args = [list(common), list(common)]

    def find_free_coin(self, node):
        """Locate an unspent genesis OP_TRUE output (the initial free coins)."""
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                spk = vout['scriptPubKey']['hex']
                if spk == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output found")

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # --- Bootstrap: only config staker A can produce; B cannot ---
        assert_equal(n0.getstakerinfo(), {self.a_pub: COIN})
        n0.generateposblock(self.a_wif)
        assert_raises_rpc_error(-5, "not a registered staker",
                                n0.generateposblock, self.b_wif)

        # --- B locks 2 coins on-chain in a staking output ---
        stake_script_hex = n0.getstakescript(self.b_pub, UNBONDING)['script']
        stake_script = bytes.fromhex(stake_script_hex)
        txid, voutn, in_amount = self.find_free_coin(n0)
        stake_amount = 2 * COIN

        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]  # OP_TRUE: no signature
        tx.vout = [
            CTxOut(stake_amount, stake_script),
            CTxOut(in_amount - stake_amount - FEE, CScript([0x51])),  # change to OP_TRUE
            CTxOut(FEE),  # explicit fee output (empty scriptPubKey)
        ]
        stake_txid = n0.sendrawtransaction(tx.serialize().hex())
        reg_block = n0.generateposblock(self.a_wif)['hash']

        # The registry now includes B with the locked amount; B can produce
        info = n0.getstakerinfo()
        assert_equal(info[self.b_pub], stake_amount)
        assert_equal(info[self.a_pub], COIN)
        res = n0.generateposblock(self.b_wif)
        assert 'vrf_output' in res

        # The peer tracked the same stake and accepted B's block
        self.sync_blocks()
        assert_equal(n1.getstakerinfo()[self.b_pub], stake_amount)
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())

        # --- Reorg safety: disconnecting the registration block removes B's
        # stake; reconsidering restores it ---
        tip_before = n0.getbestblockhash()
        n0.invalidateblock(reg_block)
        assert self.b_pub not in n0.getstakerinfo()
        n0.reconsiderblock(reg_block)
        assert_equal(n0.getbestblockhash(), tip_before)
        assert_equal(n0.getstakerinfo()[self.b_pub], stake_amount)

        # --- Restart: the registry is rebuilt from the UTXO set ---
        self.restart_node(0)
        self.connect_nodes(0, 1)
        assert_equal(n0.getstakerinfo()[self.b_pub], stake_amount)

        # --- Unbonding: spending the staking output needs the CSV maturity ---
        spend = CTransaction()
        spend.nVersion = 2
        spend.vin = [CTxIn(COutPoint(int(stake_txid, 16), 0), nSequence=UNBONDING)]
        spend.vout = [
            CTxOut(stake_amount - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        sighash, err = LegacySignatureHash(CScript(stake_script), spend, 0, SIGHASH_ALL)
        assert err is None
        sig = self.b_key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])
        spend.vin[0].scriptSig = CScript([sig])
        spend_hex = spend.serialize().hex()

        # Too early: the relative lock has not matured (2 confs so far)
        assert_raises_rpc_error(-26, "non-BIP68-final",
                                n0.sendrawtransaction, spend_hex)

        # Mature the lock, then unbond
        for _ in range(UNBONDING):
            n0.generateposblock(self.a_wif)
        n0.sendrawtransaction(spend_hex)
        n0.generateposblock(self.a_wif)

        # B's stake is gone everywhere; B can no longer produce
        assert self.b_pub not in n0.getstakerinfo()
        assert_raises_rpc_error(-5, "not a registered staker",
                                n0.generateposblock, self.b_wif)
        self.sync_blocks()
        assert self.b_pub not in n1.getstakerinfo()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())


if __name__ == '__main__':
    PosOnChainStakeTest().main()
