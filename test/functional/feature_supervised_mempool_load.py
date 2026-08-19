#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""SEQUENTIA: a frozen spend planted in mempool.dat never reaches the mempool.

mempool.dat is trusted input written by whatever binary last shut down. A file
written before a pause confirmed, or by a build that predates supervision
eviction entirely, can hold a spend the registry now forbids. Loading it is
filtered by AcceptToMemoryPool, which is exactly as strong as the supervision
registry at that moment, and the registry rebuild in init races the import
thread; the loaded pool is therefore swept once after the load completes
(CChainState::LoadMempool). This test plants such a file by hand, the way an
older binary would have left it, and asserts the poisoned entry never
surfaces while an honest transaction loaded from the same file survives.

The live-network shape of this failure: a node that keeps a pause-invalidated
spend resident re-announces it on request, its wallet chains children onto its
change, and every peer rejects the whole family with rejection logging off by
default (2026-08-19, blocks 99880 through 100261 on the public testnet).
"""

import struct
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.key import ECKey, compute_xonly_pubkey, sign_schnorr
from test_framework.messages import ser_compact_size, tx_from_hex
from test_framework.util import assert_equal, assert_raises_rpc_error
from decimal import Decimal

MEMPOOL_DUMP_VERSION = 1


def make_key(seed):
    key = ECKey()
    key.set(seed.to_bytes(32, "big"), True)
    assert key.is_valid
    xonly, _ = compute_xonly_pubkey(key.get_bytes())
    return key, xonly.hex()


def schnorr(key, sighash_hex):
    return sign_schnorr(key.get_bytes(), bytes.fromhex(sighash_hex)).hex()


class SupervisedMempoolLoadTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-con_default_blinded_addresses=0",
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-anyonecanspendaremine=1",
            "-txindex=1",
            "-supervisedassetsheight=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def funding_outpoint(self, node, minimum=Decimal("1")):
        for utxo in node.listunspent():
            if utxo["asset"] == self.policy_asset and utxo["amount"] > minimum:
                return utxo
        raise AssertionError("no funding utxo")

    def send_raw(self, node, funded_hex):
        signed = node.signrawtransactionwithwallet(funded_hex)
        assert signed["complete"], signed
        return node.sendrawtransaction(signed["hex"])

    def record_tx(self, node, kind, asset, target, signer):
        utxo = self.funding_outpoint(node)
        sighash = node.getsupervisionrecordhash(
            kind, asset, target, None, utxo["txid"], utxo["vout"])["sighash"]
        built = node.buildsupervisionrecord(kind, asset, target, None, schnorr(signer, sighash))
        change = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{change: utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        return self.send_raw(node, raw)

    def issue_pausable(self, node):
        raw = node.createrawtransaction([], [{node.getnewaddress(): Decimal("0.9")},
                                             {"fee": Decimal("0.001")}])
        funded = node.fundrawtransaction(raw)["hex"]
        issued = node.rawissueasset(funded, [{
            "asset_amount": 1000,
            "asset_address": node.getnewaddress(),
            "token_amount": 1,
            "token_address": node.getnewaddress(),
            "blind": False,
            "supervision": {"operationalkey": self.op_pub,
                            "recoverykey": self.rec_pub,
                            "pause": True},
        }])[0]
        self.send_raw(node, issued["hex"])
        self.generate(node, 1)
        return issued["asset"]

    def build_signed(self, node, outputs):
        """Fund and sign a transaction without broadcasting it, then lock its
        inputs so the next build cannot double-pick them."""
        raw = node.createrawtransaction([], outputs)
        funded = node.fundrawtransaction(raw, {"fee_asset": self.policy_asset})["hex"]
        signed = node.signrawtransactionwithwallet(funded)
        assert signed["complete"], signed
        decoded = node.decoderawtransaction(signed["hex"])
        node.lockunspent(False, [{"txid": vin["txid"], "vout": vin["vout"]}
                                 for vin in decoded["vin"]])
        return signed["hex"], decoded["txid"]

    def write_mempool_dat(self, node, raw_hexes):
        """The file an older binary would have written: every entry as it stood
        in that binary's mempool, no filtering implied."""
        path = node.chain_path / "mempool.dat"
        blob = struct.pack("<Q", MEMPOOL_DUMP_VERSION)
        blob += struct.pack("<Q", len(raw_hexes))
        for raw in raw_hexes:
            blob += tx_from_hex(raw).serialize()
            blob += struct.pack("<q", int(time.time()))  # nTime: fresh, inside the expiry window
            blob += struct.pack("<q", 0)    # fee delta
        blob += ser_compact_size(0)         # mapDeltas
        blob += ser_compact_size(0)         # unbroadcast set
        path.write_bytes(blob)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, COINBASE_MATURITY + 1)
        self.policy_asset = node.dumpassetlabels()["bitcoin"]
        self.operational, self.op_pub = make_key(0x1111)
        self.recovery, self.rec_pub = make_key(0x2222)

        # Several separate policy coins: two get signed away (and locked) below,
        # and the pause record still needs one of its own afterwards.
        for _ in range(3):
            raw = node.createrawtransaction([], [{node.getnewaddress(): Decimal("5")}])
            funded = node.fundrawtransaction(raw, {"fee_asset": self.policy_asset})["hex"]
            self.send_raw(node, funded)
            self.generate(node, 1)

        self.log.info("Issue a pausable asset and give the wallet a holding")
        asset = self.issue_pausable(node)
        holding = node.getnewaddress()
        raw = node.createrawtransaction([], [{holding: Decimal("100"), "asset": asset}])
        funded = node.fundrawtransaction(raw, {"fee_asset": self.policy_asset})["hex"]
        self.send_raw(node, funded)
        self.generate(node, 1)

        self.log.info("Sign a spend of the asset and an honest control, broadcast neither")
        frozen_hex, frozen_txid = self.build_signed(
            node, [{node.getnewaddress(): Decimal("5"), "asset": asset}])
        control_hex, control_txid = self.build_signed(
            node, [{node.getnewaddress(): Decimal("1")}])

        self.log.info("Pause the asset; the held spend is now invalid")
        self.record_tx(node, "pause", asset, None, self.operational)
        self.generate(node, 1)
        assert_raises_rpc_error(-26, "asset-frozen",
                                node.sendrawtransaction, frozen_hex)
        assert_equal(node.getrawmempool(), [])

        self.log.info("Plant both into mempool.dat, the way an older binary would have left them")
        self.stop_node(0)
        self.write_mempool_dat(node, [control_hex, frozen_hex])
        with node.assert_debug_log(expected_msgs=["Imported mempool transactions from disk"], timeout=60):
            self.start_node(0)

        self.log.info("The honest entry loads, the frozen one never surfaces")
        self.wait_until(lambda: node.getmempoolinfo()["loaded"])
        pool = node.getrawmempool()
        assert control_txid in pool, "the honest mempool.dat entry must survive the load"
        assert frozen_txid not in pool, "a pause-invalidated spend survived mempool.dat loading"

        self.log.info("And it stays out: the next block does not include it")
        self.generate(node, 1)
        assert frozen_txid not in node.getrawmempool()
        block = node.getblock(node.getbestblockhash())
        assert frozen_txid not in block["tx"]
        assert control_txid in block["tx"], "the honest entry must be mined"


if __name__ == "__main__":
    SupervisedMempoolLoadTest().main()
