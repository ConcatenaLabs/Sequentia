#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The signing RPCs on a transaction with a Simplicity input they cannot check.

A Simplicity program may read the spent output of every input of its
transaction, so the node can only run it when it knows all of them. A signing
RPC is often handed a transaction with an input whose parent it has never seen
(another party's coin, a parent not yet relayed). Beside an input that already
carries its Simplicity witness, that input leaves the program uncheckable.

The signing RPCs then say so for that input and answer; the node stays up.
Given the missing spent output (`prevtxs`), they check the program and report
it valid. The same transaction with every parent known signs and is mined.
"""

from decimal import Decimal

from test_framework.address import program_to_witness
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, CTxOutWitness, tx_from_hex,
)
from test_framework.script import LEAF_VERSION_TAPSIMPLICITY, taproot_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import BITCOIN_ASSET, assert_equal

# BIP341's nothing-up-my-sleeve point: the output has no key path.
NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")
# The Simplicity program `unit` (SimplicityHL `fn main() {}`): it accepts any
# transaction, takes an empty witness, and is one byte long.
UNIT_PROGRAM = bytes.fromhex("24")
UNIT_CMR = bytes.fromhex("c40a10263f7436b4160acbef1c36fba4be4d95df181a968afeab5eac247adff7")

UNCHECKABLE = "Simplicity input cannot be checked without the spent output of every input"


class SignSimplicityUnknownInputTest(BitcoinTestFramework):
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

    def wallet_spk(self):
        node = self.nodes[0]
        addr = node.getaddressinfo(node.getnewaddress("", "bech32"))["unconfidential"]
        return bytes.fromhex(node.getaddressinfo(addr)["scriptPubKey"])

    def pay_to(self, spk, amount):
        """Pay `amount` atoms to a witness output and return (txid, vout, txout)."""
        node = self.nodes[0]
        spk = bytes(spk)
        ver = 0 if spk[0] == 0 else spk[0] - 0x50
        txid = node.sendtoaddress(address=program_to_witness(ver, spk[2:]), amount=Decimal(amount) / COIN,
                                  fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        tx = tx_from_hex(node.getrawtransaction(txid))
        for n, out in enumerate(tx.vout):
            if bytes(out.scriptPubKey) == spk:
                return txid, n, out
        raise AssertionError("output not found")

    def build(self, inputs, outputs, sim_index):
        tx = CTransaction()
        tx.nVersion = 2
        for txid, n in inputs:
            tx.vin.append(CTxIn(COutPoint(int(txid, 16), n)))
        tx.vout = outputs
        tx.wit.vtxinwit = [CTxInWitness() for _ in tx.vin]
        tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
        # [witness, program, CMR, control block]
        tx.wit.vtxinwit[sim_index].scriptWitness.stack = [b"", UNIT_PROGRAM, UNIT_CMR, self.control]
        return tx.serialize().hex()

    def assert_alive(self):
        # The node answers a fresh RPC: it did not abort on the previous call.
        assert_equal(self.nodes[0].getblockcount(), self.nodes[0].getblockcount())
        assert self.nodes[0].process.poll() is None

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000, fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)

        tap = taproot_construct(NUMS, [("unit", UNIT_CMR, LEAF_VERSION_TAPSIMPLICITY)])
        leaf = tap.leaves["unit"]
        self.control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
        sim_spk = bytes(tap.scriptPubKey)

        self.log.info("A Simplicity input beside an input whose parent the node does not know")
        sim_txid, sim_n, _ = self.pay_to(sim_spk, 50_000)
        fin_txid, fin_n, _ = self.pay_to(self.wallet_spk(), 30_000)
        unknown_txid = "11" * 32
        unknown_out = self.txout(10_000, self.wallet_spk())
        outputs = [self.txout(85_000, self.wallet_spk()), CTxOut(CTxOutValue(5_000), b"", CTxOutAsset(self.asset_out()))]
        hexd = self.build([(unknown_txid, 0), (sim_txid, sim_n), (fin_txid, fin_n)], outputs, sim_index=1)

        for rpc in ("signrawtransactionwithkey", "signrawtransactionwithwallet"):
            self.log.info("  %s answers and names the uncheckable input", rpc)
            r = node.signrawtransactionwithkey(hexd, []) if rpc == "signrawtransactionwithkey" \
                else node.signrawtransactionwithwallet(hexd)
            self.assert_alive()
            assert_equal(r["complete"], False)
            errors = {"%s:%d" % (e["txid"], e["vout"]): e["error"] for e in r["errors"]}
            self.log.info("    errors: %s", errors)
            assert_equal(errors["%s:0" % unknown_txid], "Input not found or already spent")
            assert_equal(errors["%s:%d" % (sim_txid, sim_n)], UNCHECKABLE)
            # The Simplicity witness is left as it was.
            assert_equal(tx_from_hex(r["hex"]).wit.vtxinwit[1].scriptWitness.stack[1], UNIT_PROGRAM)
            if rpc == "signrawtransactionwithwallet":
                # The wallet still signs the input it owns.
                assert_equal(len(tx_from_hex(r["hex"]).wit.vtxinwit[2].scriptWitness.stack), 2)

        self.log.info("  testmempoolaccept and walletprocesspsbt answer as before")
        r = node.testmempoolaccept([hexd])[0]
        assert_equal(r["reject-reason"], "missing-inputs")
        self.assert_alive()
        r = node.walletprocesspsbt(node.converttopsbt(hexd, True))
        assert_equal(r["complete"], False)
        self.assert_alive()

        self.log.info("Given the missing spent output, signrawtransactionwithkey checks the program")
        prevtxs = [{"txid": unknown_txid, "vout": 0, "scriptPubKey": bytes(unknown_out.scriptPubKey).hex(),
                    "amount": Decimal(10_000) / COIN}]
        r = node.signrawtransactionwithkey(hexd, [], prevtxs)
        errors = {"%s:%d" % (e["txid"], e["vout"]): e["error"] for e in r["errors"]}
        self.log.info("  errors: %s", errors)
        assert "%s:%d" % (sim_txid, sim_n) not in errors
        assert_equal(set(errors), {"%s:0" % unknown_txid, "%s:%d" % (fin_txid, fin_n)})

        self.log.info("With every parent known, the Simplicity input checks, signs and is mined")
        hexd = self.build([(sim_txid, sim_n), (fin_txid, fin_n)],
                          [self.txout(75_000, self.wallet_spk()), CTxOut(CTxOutValue(5_000), b"", CTxOutAsset(self.asset_out()))],
                          sim_index=0)
        r = node.signrawtransactionwithwallet(hexd)
        assert_equal(r["complete"], True)
        assert_equal(node.testmempoolaccept([r["hex"]])[0]["allowed"], True)
        block = node.generateblock(node.getnewaddress(), [r["hex"]], invalid_call=False)["hash"]
        assert_equal(len(node.getblock(block)["tx"]), 2)


if __name__ == '__main__':
    SignSimplicityUnknownInputTest().main()
