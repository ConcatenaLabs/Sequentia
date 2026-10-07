#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""No supervision record in a coinbase (audit A8).

A coinbase skips the input checks that vet a supervision record's admission
signature, yet applying the block registered every record it carried. So a
block producer could freeze any holder of a supervised asset by putting a
record signed by nobody in its coinbase. From -poshardeningheight a coinbase
carrying a supervision declaration or record makes its block invalid.

Node 0 has the rule from block 1; node 1 has it far in the future and shows
what the old rule allowed: the same block, accepted, with the freeze in force.
"""

from decimal import Decimal
from io import BytesIO

from test_framework.blocktools import COINBASE_MATURITY, WITNESS_COMMITMENT_HEADER, add_witness_commitment
from test_framework.key import ECKey, compute_xonly_pubkey, sign_schnorr
from test_framework.messages import CBlock, CTxOut, CTxOutValue
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework import util
from test_framework.util import assert_equal


def make_key(seed):
    k = ECKey()
    k.set(seed.to_bytes(32, "big"), True)
    return k, compute_xonly_pubkey(k.get_bytes())[0].hex()


class SupervisedCoinbaseTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        common = [
            "-con_default_blinded_addresses=0", "-blindedaddresses=0",
            "-initialfreecoins=10000000000", "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1", "-anyonecanspendaremine=1",
            "-txindex=1", "-supervisedassetsheight=1",
        ]
        self.extra_args = [common + ["-poshardeningheight=1"], common + ["-poshardeningheight=1000000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node, old = self.nodes
        util.node_fastmerkle = node
        self.generate(node, COINBASE_MATURITY + 1)
        self.sync_all()

        _, op_pub = make_key(0x1111)
        _, rec_pub = make_key(0x2222)
        raw = node.createrawtransaction([], [{node.getnewaddress(): Decimal("0.9")}, {"fee": Decimal("0.001")}])
        funded = node.fundrawtransaction(raw)["hex"]
        issued = node.rawissueasset(funded, [{
            "asset_amount": 1000, "asset_address": node.getnewaddress(),
            "token_amount": 1, "token_address": node.getnewaddress(), "blind": False,
            "supervision": {"operationalkey": op_pub, "recoverykey": rec_pub},
        }])[0]
        signed = node.signrawtransactionwithwallet(issued["hex"])
        node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        self.sync_all()
        asset = issued["asset"]
        assert_equal(len(node.getsupervisedassets()), 1)
        self.disconnect_nodes(0, 1)

        self.log.info("A freeze signed by a stranger, carried in a coinbase")
        target = node.getnewaddress()
        utxo = next(u for u in node.listunspent() if u["amount"] > 1)
        sighash = node.getsupervisionrecordhash("freeze", asset, target, None, utxo["txid"], utxo["vout"])["sighash"]
        stranger, _ = make_key(0x4444)
        built = node.buildsupervisionrecord("freeze", asset, target, None,
                                            sign_schnorr(stranger.get_bytes(), bytes.fromhex(sighash)).hex())
        # A coinbase may not hold a spendable zero-value output, so the record
        # takes one satoshi of the block's fees.
        node.sendtoaddress(address=node.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        block = CBlock()
        block.deserialize(BytesIO(bytes.fromhex(node.getnewblockhex())))
        # The coinbase is part of Elements' witness commitment: drop the old
        # commitment, add the record, and commit again.
        cb = block.vtx[0]
        payee = next(o for o in cb.vout if o.nValue.getAmount() > 0)
        asset_out = payee.nAsset
        payee.nValue = CTxOutValue(payee.nValue.getAmount() - 1)
        cb.vout = [o for o in cb.vout if WITNESS_COMMITMENT_HEADER not in bytes(o.scriptPubKey)]
        cb.vout.append(CTxOut(1, CScript(bytes.fromhex(built["script"])), asset_out))
        add_witness_commitment(block)
        cb.vout[-1].nAsset = asset_out
        cb.rehash()
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()

        self.log.info("Refused from the hardening height")
        assert_equal(node.submitblock(block.serialize().hex()), "bad-cb-supervision")
        assert_equal(node.getsupervisedassets()[0]["frozen"], 0)

        self.log.info("Below it, the old rule took the forged freeze")
        assert_equal(old.submitblock(block.serialize().hex()), None)
        assert_equal(old.getsupervisedassets()[0]["frozen"], 1)


if __name__ == '__main__':
    SupervisedCoinbaseTest().main()
