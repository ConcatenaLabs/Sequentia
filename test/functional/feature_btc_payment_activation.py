#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The proof of a bitcoin payment is enforced from its height, and not before.

On a chain whose parameters set -con_btc_payment_height=H, a block at H - 1
takes what any node took before the rule: opcodes 0xe5 to 0xe7 are OP_SUCCESS
and the witness slot of a non-peg-in input is ignored. A block at H refuses the
same spends, each for the rule's own reason. Every case is forced into a block
with generateblock; the node runs -par=1, so a script failure names itself.

  op    a tapscript leaf with OP_INSPECTBTCPAYMENT and no proof: valid at H - 1,
        refused at H ("No valid proof of a bitcoin payment on this input");
  slot  an anchor floor no block meets, in the slot of an input whose leaf is
        OP_TRUE: ignored at H - 1, refused at H (bad-anchorlock-not-reached).

getdeploymentinfo reports btc_payment not active below H, active from the
block before it.
"""

from test_framework.address import program_to_witness
from test_framework.authproxy import JSONRPCException
from test_framework.messages import (
    COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset, CTxOutValue, CTxOutWitness, tx_from_hex,
)
from test_framework.script import CScript, CScriptOp, OP_DROP, OP_TRUE, taproot_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

OP_INSPECTBTCPAYMENT = CScriptOp(0xe5)
NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")
H = 130
FEE = 10000


class BtcPaymentActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-con_btc_payment_height=%d" % H, "-con_blocksubsidy=5000000000", "-par=1",
                            "-blindedaddresses=0", "-con_default_blinded_addresses=0", "-fallbackfee=0.0001"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def coin(self, leaf):
        """A coin of 1 under a P2TR output whose one leaf is `leaf`."""
        node = self.nodes[0]
        tap = taproot_construct(NUMS, [("leaf", leaf)])
        txid = node.sendtoaddress(address=program_to_witness(1, tap.scriptPubKey[2:]), amount=1, fee_asset_label="bitcoin")
        tx = tx_from_hex(node.getrawtransaction(txid))
        vout = [i for i, o in enumerate(tx.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        return dict(tap=tap, leaf=leaf, txid=txid, vout=vout)

    def spend(self, c, slot=()):
        node = self.nodes[0]
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(c["txid"], 16), c["vout"])))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        tx.vout.append(CTxOut(CTxOutValue(100_000_000 - FEE), spk, asset))
        tx.vout.append(CTxOut(CTxOutValue(FEE), b"", asset))
        tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
        tx.wit.vtxinwit[0].peginWitness.stack = list(slot)
        tap = c["tap"]
        leaf = tap.leaves["leaf"]
        control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
        tx.wit.vtxinwit[0].scriptWitness.stack = [bytes(c["leaf"]), control]
        tx.rehash()
        return tx.serialize().hex()

    def block(self, txs, expect=None):
        node = self.nodes[0]
        height = node.getblockcount() + 1
        if expect is None:
            node.generateblock(node.getnewaddress(), txs, invalid_call=False)
            assert_equal(node.getblockcount(), height)
            self.log.info("    block %d accepts it", height)
            return
        try:
            node.generateblock(node.getnewaddress(), txs, invalid_call=False)
        except JSONRPCException as e:
            assert expect in e.error["message"], "expected %r, got %r" % (expect, e.error["message"])
            self.log.info("    block %d refuses it: %s", height, e.error["message"])
            return
        raise AssertionError("block %d accepted, expected %r" % (height, expect))

    def dep(self):
        return self.nodes[0].getdeploymentinfo()["deployments"]["btc_payment"]

    def run_test(self):
        node = self.nodes[0]
        if not node.listwallets():
            node.createwallet("w")
        self.asset = node.dumpassetlabels()["bitcoin"]
        self.generatetoaddress(node, 101, node.getnewaddress(), sync_fun=self.no_op)
        op_leaf = CScript([OP_INSPECTBTCPAYMENT, OP_DROP, OP_DROP, OP_TRUE])
        coins = {"op": [self.coin(op_leaf) for _ in range(2)], "slot": [self.coin(CScript([OP_TRUE])) for _ in range(2)]}
        self.generatetoaddress(node, 1, node.getnewaddress(), sync_fun=self.no_op)
        floor = b"\x02" + (999_999).to_bytes(4, "little")

        self.generatetoaddress(node, H - 2 - node.getblockcount(), node.getnewaddress(), sync_fun=self.no_op)
        assert_equal(node.getblockcount(), H - 2)
        assert_equal(self.dep(), {"type": "buried", "active": False, "height": H})

        self.log.info("Block H - 1: the rule is not active; both spends are valid as before")
        self.block([self.spend(coins["op"][0]), self.spend(coins["slot"][0], [floor])])
        assert_equal(self.dep(), {"type": "buried", "active": True, "height": H})

        self.log.info("Block H: each is refused for the rule's own reason")
        self.block([self.spend(coins["op"][1])], "No valid proof of a bitcoin payment on this input")
        self.block([self.spend(coins["slot"][1], [floor])], "bad-anchorlock-not-reached")


if __name__ == '__main__':
    BtcPaymentActivationTest().main()
