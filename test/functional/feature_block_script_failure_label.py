#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""How the node names a script failure: mempool policy or block consensus.

A transaction refused by the mempool for its scripts is refused under the
mempool's flags, which are stricter than consensus: the reason reads
`mempool-script-verify-flag-failed`. A block carrying a transaction whose
script fails is refused under the block's flags, which are consensus: the
reason reads `mandatory-script-verify-flag-failed`, and names the script error
that consensus found, which can differ from the mempool's.

The node is started with `-par=1` so a block's script failure is reported with
its reason rather than the parallel checker's `block-validation-failed`.

  - a Simplicity spend whose program does not hash to the committed CMR
  - a P2WPKH spend whose signature does not verify
  - control: an honest Simplicity spend in the same kind of block is mined
"""

from decimal import Decimal

from test_framework.address import program_to_witness
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, CTxOutWitness, tx_from_hex,
)
from test_framework.script import LEAF_VERSION_TAPSIMPLICITY, taproot_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import BITCOIN_ASSET, assert_equal, assert_raises_rpc_error

NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")
# The Simplicity program `unit` (SimplicityHL `fn main() {}`) and its CMR.
UNIT_PROGRAM = bytes.fromhex("24")
UNIT_CMR = bytes.fromhex("c40a10263f7436b4160acbef1c36fba4be4d95df181a968afeab5eac247adff7")
# A leaf committing to a CMR that `unit` does not hash to.
OTHER_CMR = bytes.fromhex("ee" * 32)

CMR_MISMATCH = "Program's CMR does not match"
EVAL_FALSE = "Script evaluated without error but finished with a false/empty top stack element"
NULLFAIL = "Signature must be zero for failed CHECK(MULTI)SIG operation"


class BlockScriptFailureLabelTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-initialfreecoins=2100000000000000",
            "-anyonecanspendaremine=1",
            "-blindedaddresses=0",
            "-con_default_blinded_addresses=0",
            "-validatepegin=0",
            "-con_parent_chain_signblockscript=51",
            "-con_any_asset_fees=1",
            "-evbparams=simplicity:-1:::",
            "-txindex=1",
            "-par=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # --- helpers -----------------------------------------------------------

    def asset_out(self):
        return b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]

    def txout(self, amount, spk):
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk, nAsset=CTxOutAsset(self.asset_out()))

    def fee(self, amount):
        return CTxOut(CTxOutValue(amount), b"", CTxOutAsset(self.asset_out()))

    def wallet_spk(self):
        node = self.nodes[0]
        addr = node.getaddressinfo(node.getnewaddress("", "bech32"))["unconfidential"]
        return bytes.fromhex(node.getaddressinfo(addr)["scriptPubKey"])

    def pay_to(self, spk, amount):
        node = self.nodes[0]
        spk = bytes(spk)
        ver = 0 if spk[0] == 0 else spk[0] - 0x50
        txid = node.sendtoaddress(address=program_to_witness(ver, spk[2:]), amount=Decimal(amount) / COIN,
                                  fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        tx = tx_from_hex(node.getrawtransaction(txid))
        for n, out in enumerate(tx.vout):
            if bytes(out.scriptPubKey) == spk:
                return txid, n
        raise AssertionError("output not found")

    def spend(self, outpoint, amount):
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(outpoint[0], 16), outpoint[1])))
        tx.vout = [self.txout(amount - 5_000, self.wallet_spk()), self.fee(5_000)]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
        return tx

    def simplicity_coin(self, cmr):
        tap = taproot_construct(NUMS, [("leaf", cmr, LEAF_VERSION_TAPSIMPLICITY)])
        leaf = tap.leaves["leaf"]
        control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
        return self.pay_to(bytes(tap.scriptPubKey), 50_000), control

    def assert_refused(self, tx, mempool_error, block_error):
        node = self.nodes[0]
        hexd = tx.serialize().hex()
        res = node.testmempoolaccept([hexd])[0]
        self.log.info("  mempool: %s", res["reject-reason"])
        assert_equal(res["reject-reason"], "mempool-script-verify-flag-failed (%s)" % mempool_error)
        tip = node.getbestblockhash()
        reason = "mandatory-script-verify-flag-failed (%s)" % block_error
        with node.assert_debug_log(expected_msgs=["CheckInputScripts on %s failed with %s" % (tx.rehash(), reason)]):
            assert_raises_rpc_error(-25, "TestBlockValidity failed: %s" % reason,
                                    node.generateblock, node.getnewaddress(), [hexd], invalid_call=False)
        self.log.info("  block:   %s", reason)
        assert_equal(node.getbestblockhash(), tip)

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000, fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)

        self.log.info("A Simplicity program that does not hash to the committed CMR")
        coin, control = self.simplicity_coin(OTHER_CMR)
        tx = self.spend(coin, 50_000)
        tx.wit.vtxinwit[0].scriptWitness.stack = [b"", UNIT_PROGRAM, OTHER_CMR, control]
        self.assert_refused(tx, CMR_MISMATCH, CMR_MISMATCH)

        self.log.info("A P2WPKH signature that does not verify")
        coin = self.pay_to(self.wallet_spk(), 50_000)
        signed = node.signrawtransactionwithwallet(self.spend(coin, 50_000).serialize().hex())
        assert_equal(signed["complete"], True)
        tx = tx_from_hex(signed["hex"])
        sig = bytearray(tx.wit.vtxinwit[0].scriptWitness.stack[0])
        sig[10] ^= 0x01  # inside r: still strict DER, no longer a valid signature
        tx.wit.vtxinwit[0].scriptWitness.stack[0] = bytes(sig)
        # The mempool's NULLFAIL names the failed signature; consensus has no
        # NULLFAIL and finds a false result instead.
        self.assert_refused(tx, NULLFAIL, EVAL_FALSE)

        self.log.info("Control: an honest Simplicity spend is mined")
        coin, control = self.simplicity_coin(UNIT_CMR)
        tx = self.spend(coin, 50_000)
        tx.wit.vtxinwit[0].scriptWitness.stack = [b"", UNIT_PROGRAM, UNIT_CMR, control]
        block = node.generateblock(node.getnewaddress(), [tx.serialize().hex()], invalid_call=False)["hash"]
        assert_equal(node.getblock(block)["tx"][1], tx.rehash())


if __name__ == '__main__':
    BlockScriptFailureLabelTest().main()
