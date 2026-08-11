#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Runtime UTXO-layer committee BLS registration (impl spec Option A phase 2).

A staker joining at runtime (not from the -staker/genesis config) registers the
committee BLS key it will sign with by carrying it in its staking output:
    <csv> OP_CHECKSEQUENCEVERIFY OP_DROP <blspub(48)> OP_DROP <pop(96)> OP_DROP
        <pubkey> OP_CHECKSIG
The BLS key therefore rides in the UTXO, so the registry learns it as a pure
function of the UTXO set — reorg-safe exactly like the stake weight: mirrored on
every tip connect/disconnect and rebuilt from the UTXO set at startup, its
lifecycle tied to the staker having any staking output.

This exercises the consensus-state path with REAL blocks: register B's BLS key
on-chain, observe it via getstakerinfo verbose, then round-trip it through a
reorg (invalidate/reconsider), a node restart (rebuild from the UTXO set), and
finally unbonding (spending the last staking output drops the registration).
The key is derived with getblsregistration and the staking script assembled with
getstakescript. Validated throughout by a second, non-producing node.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
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


class PosBlsRegistrationTest(BitcoinTestFramework):
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
            "-staker=%s:%d" % (self.a_pub, COIN),
            "-validatepegin=0",
            "-txindex=1",
        ]
        self.extra_args = [list(common), list(common)]

    def find_free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output found")

    def bls_of(self, node, pub):
        info = node.getstakerinfo(True)
        return info.get(pub, {}).get("blspubkey", "") if pub in info else ""

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]
        n0.generateposblock(self.a_wif)

        # B's committee BLS registration, derived from its staking key, and the
        # staking script that carries it.
        reg = n0.getblsregistration(self.b_wif)
        blspub, pop = reg["blspubkey"], reg["pop"]
        assert_equal(len(blspub), 96)   # 48 bytes hex
        assert_equal(len(pop), 192)     # 96 bytes hex
        stake_script = bytes.fromhex(n0.getstakescript(self.b_pub, UNBONDING, None, blspub, pop)["script"])

        # B locks coins in the registered staking output.
        txid, voutn, in_amount = self.find_free_coin(n0)
        stake_amount = 2 * COIN
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        tx.vout = [
            CTxOut(stake_amount, stake_script),
            CTxOut(in_amount - stake_amount - FEE, CScript([0x51])),
            CTxOut(FEE),
        ]
        stake_txid = n0.sendrawtransaction(tx.serialize().hex())
        reg_block = n0.generateposblock(self.a_wif)['hash']

        self.log.info("Registration confirmed: B's BLS key is in the registry")
        info = n0.getstakerinfo(True)
        assert_equal(info[self.b_pub]["weight"], stake_amount)
        assert_equal(info[self.b_pub]["blspubkey"], blspub)
        # The non-verbose form is unchanged (a plain weight).
        assert_equal(n0.getstakerinfo()[self.b_pub], stake_amount)
        # A came from config with no BLS key; it has weight but no registration.
        assert_equal(self.bls_of(n0, self.a_pub), "")

        self.sync_blocks()
        assert_equal(self.bls_of(n1, self.b_pub), blspub)

        self.log.info("Reorg: disconnecting the registration drops B's BLS key; reconsidering restores it")
        tip = n0.getbestblockhash()
        n0.invalidateblock(reg_block)
        assert_equal(self.bls_of(n0, self.b_pub), "")   # no registration
        assert self.b_pub not in n0.getstakerinfo()      # and no weight
        n0.reconsiderblock(reg_block)
        assert_equal(n0.getbestblockhash(), tip)
        assert_equal(self.bls_of(n0, self.b_pub), blspub)

        self.log.info("Restart: the BLS registration is rebuilt from the UTXO set")
        self.restart_node(0)
        self.connect_nodes(0, 1)
        assert_equal(self.bls_of(n0, self.b_pub), blspub)

        self.log.info("Unbonding: spending the last staking output drops the registration")
        spend = CTransaction()
        spend.nVersion = 2
        spend.vin = [CTxIn(COutPoint(int(stake_txid, 16), 0), nSequence=UNBONDING)]
        spend.vout = [CTxOut(stake_amount - FEE, CScript([0x51])), CTxOut(FEE)]
        sighash, err = LegacySignatureHash(CScript(stake_script), spend, 0, SIGHASH_ALL)
        assert err is None
        spend.vin[0].scriptSig = CScript([self.b_key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        for _ in range(UNBONDING):
            n0.generateposblock(self.a_wif)
        n0.sendrawtransaction(spend.serialize().hex())
        n0.generateposblock(self.a_wif)
        assert self.b_pub not in n0.getstakerinfo()
        assert_equal(self.bls_of(n0, self.b_pub), "")
        self.sync_blocks()
        assert_equal(self.bls_of(n1, self.b_pub), "")

        self.log.info("Runtime UTXO-layer BLS registration: register, observe, reorg, restart, unbond — OK")


if __name__ == '__main__':
    PosBlsRegistrationTest().main()
