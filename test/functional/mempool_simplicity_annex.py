#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""An annex on a Simplicity spend relays only when the program needs it, and only that large.

A Simplicity spend buys execution budget with its witness bytes, and an annex
is how it buys budget without carrying bytes the program has to read. Only a
program that signs Simplicity's sigAllHash commits to its annex; on any other
spend -- a SeqDEX order fill, which anyone may make and nobody signs -- a relay
can append an annex under the same txid, grow the transaction tenfold, and pin
it at the floor fee rate, because the mempool keeps the first witness it sees.

The relay rule (SCRIPT_VERIFY_SIMPLICITY_ANNEX_EXACT, policy only): an annex on
a Simplicity leaf is all zeros after its tag, and the program's cost bound
exceeds the budget of the same witness with the annex one byte shorter (or
absent, for the tag alone). A spend therefore has one standard witness size.

  - a real seqdex/order whole fill, which needs no budget: refused with a
    3-byte, a 1,000-byte and a 100,000-byte zero annex, and with a nonzero
    3-byte one; a 100,001-byte one stays bad-witness-nonstandard
  - the pinning race: the annexed copy is refused, the honest one relays to a
    second node, a late annexed copy cannot replace it, and the block carries
    the honest witness
  - consensus is unchanged: an annexed fill forced into a block is mined
  - a program that needs budget (a 256-step hash loop): relays with its
    minimal annex, which an honest wallet computes from the cost bound; is
    refused with one byte more (and mined anyway when forced into a block),
    with a nonzero annex, and with one byte less or none, which a block refuses
    too
"""

from decimal import Decimal
import hashlib

from test_framework.address import program_to_witness
from test_framework.key import TaggedHash
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, CTxOutWitness, tx_from_hex,
)
from test_framework.script import LEAF_VERSION_TAPSIMPLICITY, taproot_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import BITCOIN_ASSET, assert_equal, assert_raises_rpc_error

NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")

# The Simplicity program `unit` (SimplicityHL `fn main() {}`) and its CMR: an
# anyone-can-spend coin, here the taker's payment.
UNIT_PROGRAM = bytes.fromhex("24")
UNIT_CMR = bytes.fromhex("c40a10263f7436b4160acbef1c36fba4be4d95df181a968afeab5eac247adff7")

# seqdex/order (ConcatenaLabs/sequentia-contracts, templates/seqdex_order): one
# program serves every order, its terms in a data leaf beside it. A whole fill's
# pruned program is a constant (fills.json, path "fill_whole"), its witness the
# terms followed by the path bits 00. Its cost bound is 313 weight units.
ORDER_FILL_WHOLE_PROGRAM = bytes.fromhex(
    "e927ba050e8143a050e8143a050e8143a050e81420c38e38e38e38da4e38e38e385a0830e38e38e36671c71c2c86da71"
    "c70b11b88ce39061c6e29140b20b505c3483102841861c72050838e38e390284ece8210284186e441871c70b26ab9021"
    "02836c3f24cc38e38e16603620506e0e6e469871c71c2cdaae00840a0dc467e4c9871c71c70b400da8141b8bcfca530e"
    "38e38e38e38e16c01c35028371f9b98638e38e38e38e38e385b40721502837240dcb130e38e38e38e16a6ab4108141b9"
    "3e7e698c38e38e38e38e16b01ca340a0dcaed588201801c100e24038d80e4381c9403942072b40e5d01cc001cc481cc8"
    "81ccb13600008f8717821091433a219ce42cf3ffb7c699fcd7d39b9a70e4843427f76e462685308969df9b098b85ce60"
    "281985b59b74023008b001070201c012534440d7fa8b066064fc76756c5a00fd4bf77afe303a974c6923ab0d26d3674b"
    "2f0601c040c806b740a0fced1f9d4141f9ce3f394283f38e283f38a830542c42d6373848141b9c038e40a129f8703040"
    "9d026830c40a1b9c1bea08b890338ec9a988ab193bd899f2e152435b976a6659e66f4ad8e1adfcdd0061806013241c61"
    "c8141b9fc0ec0284c4fb84c4140dc0709038710283740c986e74ce385800e4701c8d4fc28210283740008d2e20840a0d"
    "c81313008160cda0401ca13f40799a63882102836f481b85406d0395a7e4b1c71c71c71c720507e83b30e38e39bb576e"
    "a2c7890f07ed99dd1c17e4e74174d89f6f362a96220ef1f880190468738300d21359b9e038e38e38e38e374270a34e01"
    "90200e64136600000000000000000813469c0720401ccb9f8d4ef868074241091420c0f35006006809d0a26e5d20506e"
    "87b0f2e80a131372dce40a0dd0e41e6080a1313ec3f4311c77c350200e6fc5ac2d006a038181c2c0e2720506e89630dd"
    "08a71c71c71c70b480e7400e73cfd1467186e8a638e38e38e38e38e16c01cf081cf01f911f0d20e890212284181e7c00"
    "c00d048b2079e8e79c100c4073e29141fa378100e0a3f2acdd0da71c71c71c2d0d36e408374621c61ba1b8e38e38e385"
    "a006d16d1ba2c04e8e33f4591c8141ba1f8dd0fe70ab4d1902102837402986e89438dd12c71c2c02cda67902102836d4"
    "81ce80a80e73c0e82f131038f41a40e0e071001c4a071412280390a2e4200e438b938039122e5780e458397c17354039"
    "1c2e6cc072401ce185cec80e49839e10b9e801c96073e6073f41740000e4d03a0280e82c03a0c80e8363210b1b072234"
    "17fea7135710255600af3678fc34e655440afe572380d5df0c658601d08e13a1940e518ba134072945d0b20394e0e918"
    "01d23403a4700748f00e92001d24403a4900749300"
)
ORDER_CMR = bytes.fromhex("f9b42b838b6295f4f376760fbb02eed9cb20f872f07ee4b9df7273d26822ee44")

# A program whose cost bound (8,045,103 milli weight units) is far above what
# its witness earns: SHA-256 over the bytes 0x00..0xff in a loop, pruned (the
# example of ElementsProject/elements#1539). It reads nothing from the
# transaction, so anyone can spend it.
HASH_LOOP_PROGRAM = bytes.fromhex(
    "e8144eac81081420c48a0f9128a0590da107248150b21b79b8118720e30e3e070a85a02d8370c41c920542c86e2341c9"
    "20542c86e2a36e30e3f0b30e3f0e38542cc2d6371b1b8e4c39071871f038542d016c1b906839240a8590dc8a41c92054"
    "2c86e489b71871f90461c7e429c2a16616b1b93a839240a8590dca441c920542c86e559b71871f93861c7e4f9c2a1661"
    "6b1b96e6e3430e3f204c38fc8438542cc2d6373066e618c39071871f038542d016c1b99041c70b06aa0420507cb36504"
    "20506e678e2b5620a203801a00dc0708038980e33039001390ac5f8bdd59a0d0ed8d3bb22cb0ef50f71e3a577040de5b"
    "fe99608095e7d53356765e430b9101722c0661c40cc0e4804e4a9792a2e4b85c9a01907681901c9f03958139625e588b"
    "966172d80641e0c064072ec273005e6005cc105cc280c83c380c80e6280e6600e694273545e6a85cd605cd780c83c500"
    "6407368139b92f3722e6ec2e6f80641e30032039c3039d109ceb179d6173b0173b60320f1c81901cf004e790bcf38b9e"
    "60b9ea01907902064073d840a136940aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880042"
    "050831061c9160366ce8867390b3cffedf1a67f35f4e6e69c39210d09fddb9189a14c225a77e6c262e1806006616b66d"
    "d008c0212283f4060201c180e180740780"
)
HASH_LOOP_CMR = bytes.fromhex("988c6d7a1c50012028523debc8ec575ce96920c46a45f663051aa3309f6fc539")
HASH_LOOP_COST_MILLIWU = 8_045_103

# The node's budget rule: four weight units a witness byte, plus 50, capped.
BUDGET_PER_BYTE = 4
BUDGET_OFFSET = 50
BUDGET_MAX = 4_000_050

ANNEX_TOO_LARGE = "mempool-script-verify-flag-failed (Simplicity annex is larger than the program's cost needs)"
ANNEX_NONZERO = "mempool-script-verify-flag-failed (Simplicity annex must be zeros after its tag)"
OVER_BUDGET = "Program's execution cost could exceed budget"


def compact_size_len(n):
    return 1 if n < 0xfd else 3 if n <= 0xffff else 5 if n <= 0xffffffff else 9


def stack_size(stack):
    return compact_size_len(len(stack)) + sum(compact_size_len(len(x)) + len(x) for x in stack)


def budget(stack):
    return min(stack_size(stack) * BUDGET_PER_BYTE + BUDGET_OFFSET, BUDGET_MAX)


def minimal_annex(stack, cost_milliwu):
    """What an honest wallet does: None when the witness pays for the program,
    else the shortest zero annex whose stack does. One byte shorter must not."""
    if cost_milliwu <= budget(stack) * 1000:
        return None
    length = 1
    while budget(stack + [bytes([0x50]) + bytes(length - 1)]) * 1000 < cost_milliwu:
        length += 1
    return bytes([0x50]) + bytes(length - 1)


def zero_annex(length):
    return bytes([0x50]) + bytes(length - 1)


class SimplicityAnnexPolicyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
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
        ]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # --- helpers -----------------------------------------------------------

    @staticmethod
    def asset_out(asset_hex):
        return b"\x01" + bytes.fromhex(asset_hex)[::-1]

    def txout(self, asset_hex, amount, spk):
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk, nAsset=CTxOutAsset(self.asset_out(asset_hex)))

    def fee(self, amount):
        return self.txout(BITCOIN_ASSET, amount, b"")

    def wallet_spk(self):
        node = self.nodes[0]
        addr = node.getaddressinfo(node.getnewaddress("", "bech32"))["unconfidential"]
        return bytes.fromhex(node.getaddressinfo(addr)["scriptPubKey"])

    def mine(self):
        """One block, read by every wallet before the next spend: a wallet that
        lags the chain picks coins a block already spent."""
        block = self.generate(self.nodes[0], 1)[0]
        for n in self.nodes:
            n.syncwithvalidationinterfacequeue()
        return block

    def pay_to(self, spk, asset_hex, amount):
        node = self.nodes[0]
        spk = bytes(spk)
        txid = node.sendtoaddress(address=program_to_witness(1, spk[2:]), amount=Decimal(amount) / COIN,
                                  assetlabel=asset_hex, fee_asset_label=BITCOIN_ASSET)
        self.mine()
        tx = tx_from_hex(node.getrawtransaction(txid))
        for n, out in enumerate(tx.vout):
            if bytes(out.scriptPubKey) == spk:
                return COutPoint(int(txid, 16), n)
        raise AssertionError("output not found")

    def unit_coin(self, amount):
        tap = taproot_construct(NUMS, [("unit", UNIT_CMR, LEAF_VERSION_TAPSIMPLICITY)])
        leaf = tap.leaves["unit"]
        control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
        return self.pay_to(tap.scriptPubKey, BITCOIN_ASSET, amount), [b"", UNIT_PROGRAM, UNIT_CMR, control]

    @staticmethod
    def order_tree(terms):
        """A seqdex/order output: P2TR(NUMS, {order leaf, TapData(terms)})."""
        return taproot_construct(NUMS, [[("order", ORDER_CMR, LEAF_VERSION_TAPSIMPLICITY),
                                         lambda _: TaggedHash("TapData", terms)]])

    def fill(self, terms, asset_x, lock, maker_spk, fee):
        """An order of `lock` X for as much bitcoin, filled whole by a taker
        paying from an anyone-can-spend coin. Inputs: the order, the payment.
        Outputs: the maker's bitcoin, the taker's X, the payment's change, fee."""
        tap = self.order_tree(terms)
        leaf = tap.leaves["order"]
        control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
        # The witness: the terms, then PATH = Left(false), a whole fill (bits 00).
        stack = [terms + b"\x00", ORDER_FILL_WHOLE_PROGRAM, ORDER_CMR, control]
        order = self.pay_to(tap.scriptPubKey, asset_x, lock)
        pay, pay_stack = self.unit_coin(lock + 2 * fee + 100_000)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(order), CTxIn(pay)]
        tx.vout = [
            self.txout(BITCOIN_ASSET, lock, maker_spk),
            self.txout(asset_x, lock, self.wallet_spk()),
            self.txout(BITCOIN_ASSET, fee + 100_000, self.wallet_spk()),
            self.fee(fee),
        ]
        tx.wit.vtxinwit = [CTxInWitness(), CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = stack
        tx.wit.vtxinwit[1].scriptWitness.stack = pay_stack
        tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
        return tx

    @staticmethod
    def with_annex(tx, annex):
        """The same transaction (same txid), input 0 carrying `annex`."""
        t = CTransaction(tx)
        t.wit.vtxinwit[0].scriptWitness.stack.append(annex)
        assert_equal(t.rehash(), tx.rehash())
        return t

    def wtxid(self, tx):
        return self.nodes[0].decoderawtransaction(tx.serialize().hex())["wtxid"]

    def describe(self, label, tx, fee):
        d = self.nodes[0].decoderawtransaction(tx.serialize().hex())
        self.log.info("  %s: %d vB, %.1f atoms/kvB", label, d["vsize"], fee * 1000 / d["vsize"])
        return d["vsize"]

    def refused(self, label, tx, reason):
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        self.log.info("  %s: %s", label, res.get("reject-reason", "accepted"))
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], reason)

    def mined_anyway(self, label, tx):
        """Consensus is unchanged: the block takes what the mempool refused."""
        node = self.nodes[0]
        block = node.generateblock(node.getnewaddress(), [tx.serialize().hex()], invalid_call=False)["hash"]
        self.sync_blocks()
        mined = tx_from_hex(node.getrawtransaction(tx.rehash(), True, block)["hex"])
        assert_equal(mined.wit.vtxinwit[0].scriptWitness.stack[-1], tx.wit.vtxinwit[0].scriptWitness.stack[-1])
        self.log.info("  %s: forced into block %s, mined", label, block)

    def block_refuses(self, label, tx, error):
        node = self.nodes[0]
        reason = "mandatory-script-verify-flag-failed (%s)" % error
        with node.assert_debug_log(expected_msgs=["CheckInputScripts on %s failed with %s" % (tx.rehash(), reason)]):
            assert_raises_rpc_error(-25, "TestBlockValidity failed: %s" % reason,
                                    node.generateblock, node.getnewaddress(), [tx.serialize().hex()], invalid_call=False)
        self.log.info("  %s: block refused, %s", label, reason)

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 100)
        self.mine()
        # The wallet lists the genesis free coin as spendable, but it is not in
        # the UTXO set (gettxout finds nothing), and a send that picks it fails.
        unusable = [{"txid": u["txid"], "vout": u["vout"]} for u in node.listunspent()
                    if node.gettxout(u["txid"], u["vout"]) is None]
        node.lockunspent(False, unusable)
        asset_x = node.issueasset(assetamount=1000, tokenamount=0, blind=False, fee_asset="bitcoin")["asset"]
        self.mine()

        self.order_fill_needs_no_annex(asset_x)
        self.pinning_race(asset_x)
        self.annex_that_buys_budget()

    def new_terms(self, asset_x, maker_spk):
        # ASSET_A, ASSET_B, RATE_NUM, RATE_DEN, MIN_LOT, MAKER, EXPIRY, REFUND_KEY:
        # asset ids in the internal byte order a covenant reads them in.
        return (bytes.fromhex(asset_x)[::-1] + bytes.fromhex(BITCOIN_ASSET)[::-1]
                + (1).to_bytes(8, "big") + (1).to_bytes(8, "big") + (1).to_bytes(8, "big")
                + hashlib.sha256(maker_spk).digest() + (0).to_bytes(4, "big") + NUMS)

    def order_fill_needs_no_annex(self, asset_x):
        self.log.info("A seqdex/order whole fill, which needs no budget, annexed by a third party")
        maker_spk = self.wallet_spk()
        terms = self.new_terms(asset_x, maker_spk)
        fee = 60_000
        honest = self.fill(terms, asset_x, 10_000_000, maker_spk, fee)
        stack = honest.wit.vtxinwit[0].scriptWitness.stack
        self.log.info("  the order input's witness is %d bytes and earns %d WU; the program costs 313",
                      stack_size(stack), budget(stack))
        assert_equal(minimal_annex(stack, 313_000), None)
        self.describe("honest", honest, fee)
        assert node_accepts(self.nodes[0], honest)

        for length in [3, 1_000, 100_000]:
            annexed = self.with_annex(honest, zero_annex(length))
            self.describe("%d-byte zero annex" % length, annexed, fee)
            self.refused("%d-byte zero annex" % length, annexed, ANNEX_TOO_LARGE)
        self.refused("3-byte annex 50f111", self.with_annex(honest, bytes.fromhex("50f111")), ANNEX_NONZERO)
        self.refused("100,001-byte zero annex", self.with_annex(honest, zero_annex(100_001)), "bad-witness-nonstandard")

        # Policy only: a block may still carry the annexed fill.
        self.mined_anyway("1,000-byte zero annex", self.with_annex(honest, zero_annex(1_000)))

    def pinning_race(self, asset_x):
        self.log.info("The pinning race: the annexed copy first, then the honest one")
        a, b = self.nodes
        maker_spk = self.wallet_spk()
        terms = self.new_terms(asset_x, maker_spk)
        fee = 60_000
        honest = self.fill(terms, asset_x, 10_000_000, maker_spk, fee)
        self.sync_blocks()
        annexed = self.with_annex(honest, zero_annex(100_000))
        for n in (a, b):
            assert_raises_rpc_error(-26, ANNEX_TOO_LARGE, n.sendrawtransaction, annexed.serialize().hex())
        self.log.info("  the annexed copy is refused by both nodes")
        txid = a.sendrawtransaction(honest.serialize().hex())
        self.sync_mempools()
        for n in (a, b):
            assert_equal(n.getmempoolentry(txid)["wtxid"], self.wtxid(honest))
        self.log.info("  the honest copy relays: both mempools hold wtxid %s", self.wtxid(honest))
        res = b.testmempoolaccept([annexed.serialize().hex()])[0]
        assert_equal(res["allowed"], False)
        self.log.info("  a late annexed copy: %s", res["reject-reason"])
        assert_equal(b.getmempoolentry(txid)["wtxid"], self.wtxid(honest))
        block = self.mine()
        mined = tx_from_hex(a.getrawtransaction(txid, True, block)["hex"])
        assert_equal(self.wtxid(mined), self.wtxid(honest))
        self.log.info("  mined with the honest witness")

    def annex_that_buys_budget(self):
        self.log.info("A program that needs budget: its minimal annex relays, nothing else does")
        tap = taproot_construct(NUMS, [("loop", HASH_LOOP_CMR, LEAF_VERSION_TAPSIMPLICITY)])
        leaf = tap.leaves["loop"]
        control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
        stack = [b"", HASH_LOOP_PROGRAM, HASH_LOOP_CMR, control]
        annex = minimal_annex(stack, HASH_LOOP_COST_MILLIWU)
        size = len(annex)
        self.log.info("  the witness is %d bytes and earns %d WU against a cost of %d milli-WU; the minimal annex is %d bytes "
                      "(%d WU with it, %d with one byte less)", stack_size(stack), budget(stack), HASH_LOOP_COST_MILLIWU,
                      size, budget(stack + [annex]), budget(stack + [zero_annex(size - 1)]))

        def spend():
            outpoint = self.pay_to(tap.scriptPubKey, BITCOIN_ASSET, 1_000_000)
            tx = CTransaction()
            tx.nVersion = 2
            tx.vin = [CTxIn(outpoint)]
            tx.vout = [self.txout(BITCOIN_ASSET, 1_000_000 - 50_000, self.wallet_spk()), self.fee(50_000)]
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = list(stack)
            tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
            return tx

        tx = spend()
        self.refused("no annex", tx, "mempool-script-verify-flag-failed (%s)" % OVER_BUDGET)
        self.block_refuses("no annex", tx, OVER_BUDGET)
        short = self.with_annex(tx, zero_annex(size - 1))
        self.refused("%d bytes, one short" % (size - 1), short, "mempool-script-verify-flag-failed (%s)" % OVER_BUDGET)
        self.block_refuses("%d bytes, one short" % (size - 1), short, OVER_BUDGET)
        for extra in [1, 1_000]:
            self.refused("%d bytes" % (size + extra), self.with_annex(tx, zero_annex(size + extra)), ANNEX_TOO_LARGE)
        nonzero = self.with_annex(tx, bytes([0x50]) + bytes(size - 2) + b"\x01")
        self.refused("%d bytes, last byte 01" % size, nonzero, ANNEX_NONZERO)

        exact = self.with_annex(tx, annex)
        self.describe("minimal annex", exact, 50_000)
        txid = self.nodes[0].sendrawtransaction(exact.serialize().hex())
        self.sync_mempools()
        assert_equal(self.nodes[1].getmempoolentry(txid)["wtxid"], self.wtxid(exact))
        self.log.info("  %d bytes: relayed to the second node", size)
        self.mine()

        # One byte more is valid by consensus and refused only by policy.
        self.mined_anyway("%d bytes" % (size + 1), self.with_annex(spend(), zero_annex(size + 1)))


def node_accepts(node, tx):
    return node.testmempoolaccept([tx.serialize().hex()])[0]["allowed"]


if __name__ == '__main__':
    SimplicityAnnexPolicyTest().main()
