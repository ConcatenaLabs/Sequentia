#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A standing offer filled by a proof of a bitcoin payment, attacked.

doc/sequentia/bitcoin-payment-proof-standing-offer.md. The offer is a keyless
covenant: anyone who proves a bitcoin payment of at least the price to the
seller's receiving script, carrying an OP_RETURN that names a Sequentia
destination, releases asset in proportion to the payment to that destination,
and the covenant re-creates the rest under itself. It is built from the three
payment opcodes and Elements' introspection and 64-bit arithmetic, with no new
rule. Two floors are tried: relative (h > C) and absolute (h > F).

A real bitcoind is the parent chain; every negative is forced into a block with
generateblock and the reason the block was refused is asserted. The floor is the
only check that fails with OP_RETURN, so "OP_RETURN was encountered" names it.
"""

from decimal import Decimal

from test_framework.address import program_to_witness
from test_framework.messages import (COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset, CTxOutWitness,
                                     CTxOutValue, tx_from_hex)
from test_framework.script import (CScript, CScriptOp, OP_1, OP_1NEGATE, OP_CAT, OP_DROP, OP_DUP, OP_ENDIF,
                                   OP_EQUAL, OP_EQUALVERIFY, OP_GREATERTHAN, OP_GREATERTHANOREQUAL64, OP_NIP,
                                   OP_NOTIF, OP_RETURN, OP_ROT, OP_SHA256, OP_SWAP, OP_VERIFY, OP_0,
                                   taproot_construct)
from test_framework.util import assert_equal

from feature_btc_payment_proof import BtcPaymentProofTest, NUMS, OP_INSPECTBTCOUTPUT, OP_INSPECTBTCPAYMENT, \
    SEQ_NONFINAL_RBF, le8, scriptnum

OP_INSPECTINPUTASSET = CScriptOp(0xc8)
OP_INSPECTINPUTVALUE = CScriptOp(0xc9)
OP_INSPECTINPUTSCRIPTPUBKEY = CScriptOp(0xca)
OP_PUSHCURRENTINPUTINDEX = CScriptOp(0xcd)
OP_INSPECTOUTPUTASSET = CScriptOp(0xce)
OP_INSPECTOUTPUTVALUE = CScriptOp(0xcf)
OP_INSPECTOUTPUTSCRIPTPUBKEY = CScriptOp(0xd1)
OP_SUB64 = CScriptOp(0xd8)
OP_MUL64 = CScriptOp(0xd9)

K = 100           # asset atoms released per satoshi paid (the price)
FEEMAX = 100_000  # the most of a fill's release a claimant may give to the fee


def fill_leaf(program, floor, remainder=True):
    """The fill leaf. Witness: <opret index> <payment output index>.

    floor: "relative" (h > C) or an int F (h > F). remainder=False makes a
    one-shot slot: the whole coin is released, nothing is re-created.
    """
    s = [OP_PUSHCURRENTINPUTINDEX, OP_0, OP_EQUALVERIFY,           # input 0 alone: outputs 0 and 1 are its own
         OP_INSPECTBTCPAYMENT]                                      # o p h C
    if floor == "relative":
        s += [OP_GREATERTHAN]
    else:
        s += [OP_DROP, floor, OP_GREATERTHAN]
    s += [OP_NOTIF, OP_RETURN, OP_ENDIF,                            # the floor; the only OP_RETURN
          OP_INSPECTBTCOUTPUT, OP_0, OP_EQUALVERIFY, program, OP_EQUALVERIFY,   # o value: paid to the seller
          le8(K), OP_MUL64, OP_VERIFY]                              # o release
    if remainder:
        s += [OP_DUP, OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE, OP_1, OP_EQUALVERIFY,  # o rel rel in
              OP_SWAP, OP_SUB64, OP_VERIFY,                         # o rel (in - rel)
              OP_1, OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY, OP_EQUALVERIFY,          # output 1 = the rest
              OP_1, OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTSCRIPTPUBKEY,
              OP_ROT, OP_EQUALVERIFY, OP_EQUALVERIFY,               # ... under this covenant
              OP_1, OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY,
              OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTASSET, OP_1, OP_EQUALVERIFY, OP_EQUALVERIFY]
    else:
        # A slot releases the whole coin: the payment must buy all of it.
        s += [OP_DUP, OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE, OP_1, OP_EQUALVERIFY,
              OP_GREATERTHANOREQUAL64, OP_VERIFY, OP_DROP, OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE,
              OP_1, OP_EQUALVERIFY]                                 # o in (the release is the coin)
    s += [le8(FEEMAX), OP_SUB64, OP_VERIFY,                         # o (release - FEEMAX)
          OP_0, OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY,
          OP_SWAP, OP_GREATERTHANOREQUAL64, OP_VERIFY,              # o: output 0 keeps all but the fee
          OP_0, OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY,
          OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTASSET, OP_1, OP_EQUALVERIFY, OP_EQUALVERIFY,
          OP_0, OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_1, OP_EQUALVERIFY,  # o prog0 (a v1 destination)
          bytes.fromhex("6a2101"), OP_SWAP, OP_CAT, OP_SHA256,      # o sha(OP_RETURN 01 prog0)
          OP_SWAP, OP_INSPECTBTCOUTPUT, OP_1NEGATE, OP_EQUALVERIFY, OP_NIP,  # the payment's OP_RETURN names it
          OP_EQUAL]
    return CScript(s)


class StandingOfferTest(BtcPaymentProofTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        try:
            self.results = []
            self.setup_chain_state()
            self.setup_wallets()
            self.fill_twice_and_replay()
            self.same_block_buyers()
            self.adjacent_block_buyers()
            self.seller_front_runs_in_the_mempool()
            self.absolute_floor_one_payment_drains()
            self.slot_taken_by_the_sellers_own_payment()
            self.reorg_reorders_two_buyers()
        finally:
            self.write_results()
            self.stop_bitcoind()

    # -------------------------------------------------------------- set-up
    def setup_wallets(self):
        self.payer = {}
        b = self.btc_rpc()
        b.createwallet("buyer2")
        self.bbuyer2 = self.btc_rpc("buyer2")
        self.bminer.sendtoaddress(self.bseller.getnewaddress(), 20)
        self.bminer.sendtoaddress(self.bbuyer2.getnewaddress(), 20)
        self.btc_mine(1)

    def seller_script(self):
        from test_framework.key import compute_xonly_pubkey, generate_privkey
        pk = compute_xonly_pubkey(generate_privkey())[0]
        desc = self.bseller.getdescriptorinfo("wsh(and_v(v:pk(%s),older(1)))" % ("02" + pk.hex()))["descriptor"]
        addr = self.bseller.deriveaddresses(desc)[0]
        return addr, bytes.fromhex(self.bseller.getaddressinfo(addr)["witness_program"])

    def dest(self):
        node = self.nodes[0]
        a = node.getnewaddress("", "bech32m")
        info = node.getaddressinfo(a)
        assert_equal(info["witness_version"], 1)
        return bytes.fromhex(info["scriptPubKey"]), bytes.fromhex(info["witness_program"])

    def make_offer(self, floor="relative", atoms=100_000_000, remainder=True):
        """An offer of `atoms` of the asset; returns its first coin, aged 3 blocks."""
        node = self.nodes[0]
        addr, program = self.seller_script()
        if floor == "absolute":
            floor = self.anchor_of(node.getbestblockhash())
        leaf = fill_leaf(program, floor, remainder)
        tap = taproot_construct(NUMS, [("fill", leaf)])
        txid = node.sendtoaddress(address=program_to_witness(1, tap.scriptPubKey[2:]),
                                  amount=Decimal(atoms) / Decimal(100000000), assetlabel=self.asset,
                                  fee_asset_label=self.asset, replaceable=False)
        tx = tx_from_hex(node.getrawtransaction(txid))
        vout = [i for i, o in enumerate(tx.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        self.mine(3)
        bh = node.getrawtransaction(txid, True)["blockhash"]
        return dict(addr=addr, program=program, leaf=leaf, tap=tap, txid=txid, vout=vout, txout=tx.vout[vout],
                    C=self.anchor_of(bh), floor=floor, leaf_size=len(leaf))

    def pay_offer(self, offer, sat, wallet, dest_prog, mine=True):
        """An ordinary bitcoin payment: output 0 pays the seller, output 1 is an
        OP_RETURN naming the buyer's Sequentia destination, output 2 is change."""
        raw = wallet.createrawtransaction([], [{offer["addr"]: Decimal(sat) / Decimal(100000000)},
                                               {"data": (b"\x01" + dest_prog).hex()}])
        f = wallet.fundrawtransaction(raw, {"changePosition": 2})
        txid = wallet.sendrawtransaction(wallet.signrawtransactionwithwallet(f["hex"])["hex"])
        self.payer[txid] = wallet
        if mine:
            self.btc_mine(1)
        return txid

    def btc_block_of(self, txid):
        return self.payer[txid].gettransaction(txid)["blockhash"]

    def fill(self, coin, txid, sat, dest_spk, fee=5000, blockhash=None):
        """A keyless fill of `coin` by the payment `txid` (nobody signs)."""
        pr = self.proof_for(txid, coin["C"], blockhash or self.btc_block_of(txid))
        release = sat * K
        t = CTransaction()
        t.nVersion = 2
        t.vin.append(CTxIn(COutPoint(int(coin["txid"], 16), coin["vout"]), nSequence=SEQ_NONFINAL_RBF))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        value = coin["txout"].nValue.getAmount()
        if coin["remainder"]:
            t.vout.append(CTxOut(CTxOutValue(release - fee), dest_spk, asset))
            t.vout.append(CTxOut(CTxOutValue(max(value - release, 0)), coin["txout"].scriptPubKey, asset))
        else:
            t.vout.append(CTxOut(CTxOutValue(value - fee), dest_spk, asset))
        t.vout.append(CTxOut(CTxOutValue(fee), b"", asset))
        t.wit.vtxinwit.append(CTxInWitness())
        t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
        t.wit.vtxinwit[0].peginWitness.stack = [pr["record"]]
        tap = coin["tap"]
        control = bytes([tap.leaves["fill"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["fill"].merklebranch
        t.wit.vtxinwit[0].scriptWitness.stack = [scriptnum(1), scriptnum(0), bytes(coin["leaf"]), control]
        t.rehash()
        return t, pr

    def coin0(self, offer, remainder=True):
        return dict(txid=offer["txid"], vout=offer["vout"], txout=offer["txout"], C=offer["C"], tap=offer["tap"],
                    leaf=offer["leaf"], remainder=remainder)

    def next_coin(self, coin, fill_tx):
        """The coin a mined fill re-created, with its coin anchor C."""
        node = self.nodes[0]
        bh = node.getrawtransaction(fill_tx.hash, True)["blockhash"]
        return dict(coin, txid=fill_tx.hash, vout=1, txout=fill_tx.vout[1], C=self.anchor_of(bh))

    def h_of(self, txid):
        return self.btc_rpc().getblockheader(self.btc_block_of(txid))["height"]

    # ------------------------------------------------------------ scenarios
    def fill_twice_and_replay(self):
        node = self.nodes[0]
        self.step("SO1 a standing offer (relative floor), filled by a real payment, filled again")
        offer = self.make_offer()
        c0 = self.coin0(offer)
        spk_a, prog_a = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a)
        self.mine_anchored(self.h_of(pa))
        f1, pr1 = self.fill(c0, pa, 100_000, spk_a)
        node.sendrawtransaction(f1.serialize().hex())
        blk = self.mine(1)[0]
        assert f1.hash in node.getblock(blk)["tx"]
        vs = node.decoderawtransaction(f1.serialize().hex())["vsize"]
        c1 = self.next_coin(c0, f1)
        self.results.append((self.current, "fill 1 mined: %d atoms to the payment's destination, remainder %d "
                             "re-created at C=%d; fill vsize %d, leaf %d bytes, proof %d bytes"
                             % (f1.vout[0].nValue.getAmount(), f1.vout[1].nValue.getAmount(), c1["C"], vs,
                                offer["leaf_size"], len(pr1["record"]))))
        spk_b, prog_b = self.dest()
        pb = self.pay_offer(offer, 50_000, self.bbuyer2, prog_b)
        self.mine_anchored(self.h_of(pb))
        f2, _ = self.fill(c1, pb, 50_000, spk_b)
        node.sendrawtransaction(f2.serialize().hex())
        blk = self.mine(1)[0]
        assert f2.hash in node.getblock(blk)["tx"]
        c2 = self.next_coin(c1, f2)
        self.results.append((self.current + " (fill 2)", "mined: %d atoms to the second destination, remainder %d"
                             % (f2.vout[0].nValue.getAmount(), f2.vout[1].nValue.getAmount())))

        self.step("SO2 replay: payment 1 again against the re-created coin (relative floor)")
        assert self.h_of(pa) <= c2["C"]
        r, _ = self.fill(c2, pa, 100_000, spk_a)
        self.force([r], "OP_RETURN was encountered")

        self.step("SO3 a fill whose OP_RETURN names another destination than output 0 pays")
        spk_x, _ = self.dest()
        pc = self.pay_offer(offer, 10_000, self.bbuyer, prog_a)
        self.mine_anchored(self.h_of(pc))
        bad, _ = self.fill(c2, pc, 10_000, spk_x)
        self.force([bad], "Script evaluated without error but finished with a false/empty top stack element")
        self.step("SO4 a stranger completing a fill gives the buyer's asset to the fee")
        greedy, _ = self.fill(c2, pc, 10_000, spk_a, fee=FEEMAX + 1)
        self.force([greedy], "Script failed an OP_VERIFY operation")
        self.last_relative = (offer, c2)

    def same_block_buyers(self):
        node = self.nodes[0]
        self.step("SO5 two honest buyers in the same Bitcoin block (relative floor): the second is shut out")
        offer = self.make_offer()
        c0 = self.coin0(offer)
        spk_a, prog_a = self.dest()
        spk_b, prog_b = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a, mine=False)
        pb = self.pay_offer(offer, 100_000, self.bbuyer2, prog_b, mine=True)
        assert_equal(self.btc_block_of(pa), self.btc_block_of(pb))
        self.mine_anchored(self.h_of(pa))
        fa, _ = self.fill(c0, pa, 100_000, spk_a)
        self.force([fa])
        c1 = self.next_coin(c0, fa)
        fb, _ = self.fill(c1, pb, 100_000, spk_b)
        self.force([fb], "OP_RETURN was encountered")
        self.results.append((self.current + " (state)", "payment B at h=%d, re-created coin C=%d, inventory left %d "
                             "atoms: B paid 100000 sat and can never fill" % (self.h_of(pb), c1["C"],
                                                                             fa.vout[1].nValue.getAmount())))

    def adjacent_block_buyers(self):
        self.step("SO6 two honest buyers in adjacent Bitcoin blocks, the first filled after both: second shut out")
        offer = self.make_offer()
        c0 = self.coin0(offer)
        spk_a, prog_a = self.dest()
        spk_b, prog_b = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a)
        pb = self.pay_offer(offer, 100_000, self.bbuyer2, prog_b)
        assert_equal(self.h_of(pb), self.h_of(pa) + 1)
        self.mine_anchored(self.h_of(pb))
        fa, _ = self.fill(c0, pa, 100_000, spk_a)
        self.force([fa])
        c1 = self.next_coin(c0, fa)
        fb, _ = self.fill(c1, pb, 100_000, spk_b)
        self.force([fb], "OP_RETURN was encountered")
        self.results.append((self.current + " (state)", "A at h=%d, B at h=%d, fill A mined anchored at C=%d"
                             % (self.h_of(pa), self.h_of(pb), c1["C"])))

    def seller_front_runs_in_the_mempool(self):
        node = self.nodes[0]
        self.step("SO7 the seller pays itself beside the buyer and its fill replaces the buyer's in honest mempools")
        offer = self.make_offer()
        c0 = self.coin0(offer)
        spk_a, prog_a = self.dest()
        spk_s, prog_s = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a, mine=False)
        # The seller sees the buyer's payment in Bitcoin's mempool and pays its own
        # receiving script the same, naming its own destination.
        ps = self.pay_offer(offer, 100_000, self.bseller, prog_s, mine=True)
        assert_equal(self.btc_block_of(pa), self.btc_block_of(ps))
        self.mine_anchored(self.h_of(pa))
        fa, _ = self.fill(c0, pa, 100_000, spk_a)
        node.sendrawtransaction(fa.serialize().hex())
        fs, _ = self.fill(c0, ps, 100_000, spk_s, fee=20_000)
        node.sendrawtransaction(fs.serialize().hex())
        pool = node.getrawmempool()
        assert fs.hash in pool and fa.hash not in pool
        blk = self.mine(1)[0]   # an honest producer, from its mempool
        assert fs.hash in node.getblock(blk)["tx"]
        self.force([fa], "bad-txns-inputs-missingorspent")
        c1 = self.next_coin(c0, fs)
        fa2, _ = self.fill(c1, pa, 100_000, spk_a)
        self.force([fa2], "OP_RETURN was encountered")
        self.results.append((self.current + " (state)", "an honest producer mined the seller's fill (fee 20000 > 5000); "
                             "the buyer's 100000 sat reached the seller and opens nothing; the seller's own "
                             "payment returned to itself"))

    def absolute_floor_one_payment_drains(self):
        node = self.nodes[0]
        self.step("SO8 absolute floor: one payment of the seller's to itself fills the offer again and again")
        offer = self.make_offer(floor="absolute", atoms=35_000_000)
        coin = self.coin0(offer)
        spk_s, prog_s = self.dest()
        ps = self.pay_offer(offer, 100_000, self.bseller, prog_s)
        self.mine_anchored(self.h_of(ps))
        for i in range(3):
            f, _ = self.fill(coin, ps, 100_000, spk_s)
            self.log.info("    fill %d: coin value %d, outputs %s", i + 1, coin["txout"].nValue.getAmount(),
                          [o.nValue.getAmount() for o in f.vout])
            self.force([f])
            coin = self.next_coin(coin, f)
        left = coin["txout"].nValue.getAmount()
        assert_equal(left, 5_000_000)
        self.results.append((self.current + " (state)", "3 fills of 10000000 atoms each mined from one payment of "
                             "100000 sat; inventory left %d, less than one 100000-sat fill" % left))
        spk_a, prog_a = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a)
        self.mine_anchored(self.h_of(pa))
        fa, _ = self.fill(coin, pa, 100_000, spk_a)
        # The covenant asks output 1 to hold in - release = -5000000: no output can.
        # The closest balanced fill (everything left to the buyer, 1 atom re-created)
        # fails on that equality.
        fa.vout[0].nValue = CTxOutValue(left - 5000 - 1)
        fa.vout[1].nValue = CTxOutValue(1)
        fa.rehash()
        self.force([fa], "Script failed an OP_EQUALVERIFY operation")

    def slot_taken_by_the_sellers_own_payment(self):
        node = self.nodes[0]
        self.step("SO9 one-shot slots (one fresh address each, absolute floor): the seller's own payment takes a slot")
        offer = self.make_offer(floor="absolute", atoms=10_000_000, remainder=False)
        slot = self.coin0(offer, remainder=False)
        spk_a, prog_a = self.dest()
        spk_s, prog_s = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a, mine=False)
        ps = self.pay_offer(offer, 100_000, self.bseller, prog_s, mine=True)
        self.mine_anchored(self.h_of(pa))
        fa, _ = self.fill(slot, pa, 100_000, spk_a)
        node.sendrawtransaction(fa.serialize().hex())
        fs, _ = self.fill(slot, ps, 100_000, spk_s, fee=20_000)
        node.sendrawtransaction(fs.serialize().hex())
        blk = self.mine(1)[0]
        assert fs.hash in node.getblock(blk)["tx"]
        self.force([fa], "bad-txns-inputs-missingorspent")

    def reorg_reorders_two_buyers(self):
        node = self.nodes[0]
        b = self.btc_rpc()
        self.step("SO10 a Bitcoin reorganization moves two filled payments into one block: the second fill is lost")
        offer = self.make_offer()
        c0 = self.coin0(offer)
        spk_a, prog_a = self.dest()
        spk_b, prog_b = self.dest()
        pa = self.pay_offer(offer, 100_000, self.bbuyer, prog_a)
        ha = self.h_of(pa)
        self.mine_anchored(ha)
        fa, _ = self.fill(c0, pa, 100_000, spk_a)
        self.force([fa])
        c1 = self.next_coin(c0, fa)
        pb = self.pay_offer(offer, 100_000, self.bbuyer2, prog_b)
        hb = self.h_of(pb)
        assert hb > c1["C"]
        self.mine_anchored(hb)
        fb, _ = self.fill(c1, pb, 100_000, spk_b)
        blk_b = self.force([fb])
        self.results.append((self.current + " (before)", "A at h=%d filled (C=%d), B at h=%d filled: both mined"
                             % (ha, c1["C"], hb)))
        # Both payments stand; Bitcoin reorganizes from A's block and mines both again together.
        bh_a = self.btc_block_of(pa)
        b.invalidateblock(bh_a)
        self.btc_mine(3)
        assert_equal(self.btc_block_of(pa), self.btc_block_of(pb))
        h2 = self.h_of(pa)
        self.wait_for(lambda: node.getblockheader(blk_b)["confirmations"] < 0, "the fills' blocks are disconnected")
        self.mine_anchored(b.getblockcount())
        # The offer is older than the reorganization: c0 stands. Fill A comes back
        # (the node re-links it itself, or anyone completes it with a fresh proof).
        if node.gettxout(c0["txid"], c0["vout"], False) is not None:
            if fa.hash in node.getrawmempool():
                self.mine(1)
            else:
                fa_new, _ = self.fill(c0, pa, 100_000, spk_a)
                self.force([fa_new])
        assert node.gettxout(c0["txid"], c0["vout"], False) is None
        c1n = self.next_coin(c0, fa)
        assert_equal(c1n["txid"], c1["txid"])
        fb2, _ = self.fill(c1n, pb, 100_000, spk_b)
        self.force([fb2], "OP_RETURN was encountered")
        self.results.append((self.current + " (after)", "both payments now at h=%d; fill A re-mined, coin C=%d; "
                             "B's fill refused: honest buyer B lost 100000 sat to an honest reorganization"
                             % (h2, c1n["C"])))


if __name__ == '__main__':
    StandingOfferTest().main()
