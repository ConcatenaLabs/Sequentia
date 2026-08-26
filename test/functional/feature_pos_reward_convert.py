#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Converting staking rewards: the whole path, on a live chain.

Everything else about reward auto-conversion is tested a piece at a time --
attribution in feature_pos_rewards.py, the policy and the covenant leaf in unit
tests. This is the one that puts the pieces together and spends real coins on a
real chain: a staker earns rewards, a maker rests a covenant offer, and the
node's own conversion pass sells the rewards into it, unattended.

What makes it worth its length is that the covenant here is REAL. The test
builds the same FILL leaf the node builds, funds a Taproot output with it, and
serves it through a stand-in relay; the node then rebuilds that leaf from the
offer's terms, checks it against the coin on chain, and spends it. A wrong
opcode, a wrong push encoding, a wrong output slot or a mis-rounded price all
fail here as a rejected transaction, which is exactly how they would fail in
production.

What it asserts:
 - the pass converts a matured reward and nothing else;
 - the maker is credited at the covenant's own ceil-rounded price;
 - the wallet ends up holding the asset it converted into;
 - the covenant's remainder returns to an identical covenant, so the offer
   survives a partial fill;
 - a reward already converted is never offered again;
 - with the setting off, nothing is sold however good the market is.
"""

import http.server
import json
import threading
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.key import ECKey, compute_xonly_pubkey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import (
    CScript, LEAF_VERSION_TAPSCRIPT, taproot_construct,
    OP_1, OP_ADD, OP_CHECKSIG, OP_CHECKLOCKTIMEVERIFY, OP_DROP, OP_DUP, OP_ELSE,
    OP_ENDIF, OP_EQUAL, OP_EQUALVERIFY, OP_IF, OP_LESSTHAN, OP_NIP, OP_ROT,
    OP_SWAP, OP_VERIFY,
)

UNBONDING = 5
COIN = 100_000_000

# Elements introspection opcodes the covenant leaf is built from. Named here
# rather than imported so this file states exactly which bytes it means.
OP_INSPECTINPUTVALUE = 0xc9
OP_INSPECTINPUTSCRIPTPUBKEY = 0xca
OP_PUSHCURRENTINPUTINDEX = 0xcd
OP_INSPECTOUTPUTASSET = 0xce
OP_INSPECTOUTPUTVALUE = 0xcf
OP_INSPECTOUTPUTSCRIPTPUBKEY = 0xd1
OP_INSPECTNUMOUTPUTS = 0xd5
OP_ADD64 = 0xd7
OP_SUB64 = 0xd8
OP_MUL64 = 0xd9
OP_DIV64 = 0xda
OP_GREATERTHANOREQUAL64 = 0xdf

# BIP341 nothing-up-my-sleeve internal key: no known discrete log, so the
# covenant has no key-path spend and the leaves are the whole of it.
NUMS = bytes.fromhex('50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0')

CREDIT_IDX = bytes([OP_PUSHCURRENTINPUTINDEX, OP_DUP, OP_ADD])            # 2k
REM_IDX = bytes([OP_PUSHCURRENTINPUTINDEX, OP_DUP, OP_ADD, 0x8b])         # 2k+1 (OP_1ADD)


def le8(n):
    return int(n).to_bytes(8, 'little')


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    return k, wif, k.get_pubkey().get_bytes().hex()


def fill_leaf(asset_a_internal, asset_b_internal, rate_num, rate_den, min_lot, maker_prog):
    """The permissionless FILL tapscript, byte for byte as the node builds it."""
    s = b''
    s += bytes([OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE])
    s += bytes([OP_1, OP_EQUALVERIFY])

    s += REM_IDX + bytes([OP_INSPECTNUMOUTPUTS, OP_LESSTHAN])
    s += bytes([OP_IF])
    s += REM_IDX + bytes([OP_INSPECTOUTPUTASSET])
    s += bytes([OP_1, OP_EQUALVERIFY])
    s += bytes([len(asset_a_internal)]) + asset_a_internal + bytes([OP_EQUAL])
    s += bytes([OP_IF])
    s += REM_IDX + bytes([OP_INSPECTOUTPUTSCRIPTPUBKEY])
    s += bytes([OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTSCRIPTPUBKEY])
    s += bytes([OP_ROT, OP_EQUALVERIFY, OP_EQUALVERIFY])
    s += REM_IDX + bytes([OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY])
    s += bytes([OP_DUP, 8]) + le8(min_lot) + bytes([OP_GREATERTHANOREQUAL64, OP_VERIFY])
    s += bytes([OP_ELSE])
    s += bytes([8]) + le8(0)
    s += bytes([OP_ENDIF])
    s += bytes([OP_ELSE])
    s += bytes([8]) + le8(0)
    s += bytes([OP_ENDIF])

    s += bytes([OP_SUB64, OP_VERIFY])
    s += bytes([OP_DUP, 8]) + le8(min_lot) + bytes([OP_GREATERTHANOREQUAL64, OP_VERIFY])

    s += bytes([8]) + le8(rate_num) + bytes([OP_MUL64, OP_VERIFY])
    s += bytes([8]) + le8(rate_den - 1) + bytes([OP_ADD64, OP_VERIFY])
    s += bytes([8]) + le8(rate_den) + bytes([OP_DIV64, OP_VERIFY, OP_NIP])

    s += CREDIT_IDX + bytes([OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY])
    s += bytes([len(asset_b_internal)]) + asset_b_internal + bytes([OP_EQUALVERIFY])
    s += CREDIT_IDX + bytes([OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_1, OP_EQUALVERIFY])
    s += bytes([len(maker_prog)]) + maker_prog + bytes([OP_EQUALVERIFY])
    s += CREDIT_IDX + bytes([OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY])
    s += bytes([OP_SWAP, OP_GREATERTHANOREQUAL64])
    return s


def refund_leaf(expiry, maker_x):
    return CScript([expiry, OP_CHECKLOCKTIMEVERIFY, OP_DROP, maker_x, OP_CHECKSIG])


class FakeRelay(http.server.BaseHTTPRequestHandler):
    """A stand-in for the SeqDEX relay: it serves one book and nothing else.

    The relay is untrusted by design, so a test one is not a shortcut: the node
    verifies every covenant against the chain before spending, and this file
    could lie about the terms without the node losing a coin.
    """
    offers: list = []

    def do_GET(self):
        if '/orderbook' in self.path:
            body = json.dumps({'offers': self.offers}).encode()
        elif self.path.endswith('/v1/markets'):
            body = json.dumps({'markets': []}).encode()
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class PosRewardConvertTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_key, self.a_wif, self.a_pub = make_key()
        self.extra_args = [[
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=%d" % UNBONDING,
            "-posslotinterval=1",
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, 100 * COIN),
            "-validatepegin=0",
            "-txindex=1",
            "-acceptnonstdtxn=1",
            # Sequentia is transparent by DEFAULT on its real chains, but a
            # CUSTOM chain keeps the Elements default of blinded addresses. A
            # covenant fill is an explicit construction throughout, so a test on
            # blinded coins would be testing a chain nobody runs.
            "-con_default_blinded_addresses=0",
            # ...and the wallet's own change with it. A covenant fill can only
            # be funded from explicit coins, and on Sequentia's real chains
            # everything is explicit by default; a custom chain has to be told
            # twice, once for the chain and once for the wallet.
            "-blindedaddresses=0",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # The relay has to exist before the node starts, because -seqoburl is a
        # startup argument.
        self.relay = http.server.ThreadingHTTPServer(('127.0.0.1', 0), FakeRelay)
        self.relay_port = self.relay.server_address[1]
        self.relay_thread = threading.Thread(target=self.relay.serve_forever, daemon=True)
        self.relay_thread.start()
        self.extra_args[0].append("-seqoburl=http://127.0.0.1:%d" % self.relay_port)
        super().setup_network()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.a_wif)

    def find_free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output found")

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        self.log.info("Own the producing key, so its blocks' fees are this wallet's rewards")
        w0.importprivkey(self.a_wif, "staker", False)

        policy = n0.dumpassetlabels()["bitcoin"]

        self.log.info("Endow the wallet, and issue the asset the maker will pay in")
        w0_addr = w0.getaddressinfo(w0.getnewaddress())
        txid, voutn, in_amount = self.find_free_coin(n0)
        endowment = 5000 * COIN
        fee = 100_000
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        fund.vout = [
            CTxOut(endowment, bytes.fromhex(w0_addr["scriptPubKey"])),
            CTxOut(in_amount - endowment - fee, CScript([0x51])),
            CTxOut(fee),
        ]
        self.change_txid = n0.sendrawtransaction(fund.serialize().hex())
        self.change_vout = 1
        self.change_value = in_amount - endowment - fee
        self.mine(1)

        # The asset the covenant SELLS (asset A) -- what the staker converts into.
        issue = w0.issueasset(1000, 0, False, None, "bitcoin")
        asset_a = issue["asset"]
        self.mine(1)

        self.log.info("Earn some rewards: fee-bearing blocks this wallet's key produced")
        n0.createwallet("other")
        wo = n0.get_wallet_rpc("other")
        w0.settxfee(Decimal("0.02"))
        for _ in range(3):
            w0.sendtoaddress(address=wo.getnewaddress(), amount=1, fee_asset_label="bitcoin")
            self.mine(1)
        self.mine(101)   # clear coinbase maturity

        rewards = w0.liststakingrewards()
        assert_greater_than(len(rewards["rewards"]), 0)
        earned = next(t for t in rewards["totals"] if t["asset"] == policy)
        assert_greater_than(earned["mature"], 0)
        earned_atoms = int(round(float(earned["mature"]) * COIN))
        self.log.info("matured rewards: %d atoms of the policy asset" % earned_atoms)

        self.log.info("A maker rests a REAL covenant offer: asset A for the reward asset")
        # Internal (non-reversed) byte order, which is how the leaf bakes ids and
        # how the relay serves them -- the trap this whole feature had to learn.
        a_internal = bytes.fromhex(asset_a)[::-1]
        b_internal = bytes.fromhex(policy)[::-1]

        maker_key, maker_wif, maker_pub = make_key()
        maker_x = compute_xonly_pubkey(maker_key.get_bytes())[0]
        maker_prog = maker_x           # v1 taproot payout program

        locked = 500 * 100_000_000      # asset A the covenant holds (8dp issuance)
        # Price: the maker wants `rate_num` of B per `rate_den` of A. Keep the
        # whole fill comfortably inside what the staker earned.
        rate_den = locked
        rate_num = max(1, earned_atoms // 4)
        min_lot = 1

        leaf = fill_leaf(a_internal, b_internal, rate_num, rate_den, min_lot, maker_prog)
        rleaf = bytes(refund_leaf(1, maker_x))
        info = taproot_construct(NUMS, [("fill", leaf, LEAF_VERSION_TAPSCRIPT),
                                        ("refund", rleaf, LEAF_VERSION_TAPSCRIPT)])
        cov_spk = bytes(info.scriptPubKey)

        cov_txid = w0.sendtoaddress(
            address=n0.deriveaddresses(n0.getdescriptorinfo("raw(%s)" % cov_spk.hex())["descriptor"])[0],
            amount=Decimal(locked) / COIN, assetlabel=asset_a, fee_asset_label="bitcoin")
        self.mine(1)
        raw = n0.getrawtransaction(cov_txid, True)
        cov_vout = next(v["n"] for v in raw["vout"]
                        if v["scriptPubKey"]["hex"] == cov_spk.hex())

        leaf_info = info.leaves["fill"]
        FakeRelay.offers = [{
            "offer_id": "test-offer-1",
            "maker_pubkey": maker_pub,
            "offer_asset": asset_a,
            "want_asset": policy,
            "offer_amount": str(locked),
            "want_amount": str(rate_num),
            "allow_partial": True,
            "covenant": {
                "covenant_txid": cov_txid,
                "covenant_vout": cov_vout,
                "asset_a": a_internal.hex(),
                "asset_b": b_internal.hex(),
                "rate_num": str(rate_num),
                "rate_den": str(rate_den),
                "min_lot": str(min_lot),
                "maker_prog": maker_prog.hex(),
                "maker_x": maker_x.hex(),
                "internal_key": NUMS.hex(),
                "merkle_path": [leaf_info.merklebranch[i:i + 32].hex()
                                for i in range(0, len(leaf_info.merklebranch), 32)],
                "expiry_locktime": 1,
            },
        }]

        self.log.info("With conversion OFF, nothing is sold however good the market is")
        before = w0.getbalance()
        try:
            w0.convertrewards(False)
            raise AssertionError("convertrewards ran while conversion was switched off")
        except Exception as e:
            assert "switched off" in str(e), str(e)
        assert_equal(w0.getbalance(), before)

        self.log.info("Switch it on, and let the node convert its rewards into asset A")
        w0.setrewardautoconvert(True, asset_a, 0.00000001, 10000)
        got_a_before = w0.getbalance().get(asset_a, 0)

        report = w0.convertrewards(False)
        assert_equal(report["enabled"], True)
        assert_greater_than(len(report["considered"]), 0)
        row = next(r for r in report["considered"] if r["asset"] == policy)
        assert_equal(row["converts"], True)
        assert "txid" in row
        self.mine(1)

        self.log.info("The covenant was really spent, and the maker was really paid")
        assert_equal(n0.gettxout(cov_txid, cov_vout), None)
        fill = n0.getrawtransaction(row["txid"], True)

        # Slot 0 is the maker's credit, and the leaf compares these bytes
        # exactly: asset B, to the committed program, at the ceil-rounded price.
        credit = fill["vout"][0]
        assert_equal(credit["asset"], policy)
        assert_equal(credit["scriptPubKey"]["hex"], "51" + "20" + maker_prog.hex())
        assert_greater_than(int(round(float(credit["value"]) * COIN)) + 1, rate_num)

        # Slot 1 is what the leaf reads as the remainder. On a full fill it must
        # NOT be asset A, or the leaf takes the self-replicating branch and
        # demands a covenant output that is not there.
        slot1 = fill["vout"][1]
        assert slot1["asset"] != asset_a, \
            "a full fill left asset A in the remainder slot; the leaf would have rejected it"

        got_a_after = w0.getbalance().get(asset_a, 0)
        assert_greater_than(got_a_after, got_a_before)

        self.log.info("Again, against an offer too big to fill: the remainder self-replicates")
        # Top the wallet up with an EXPLICIT coin first. The batch about to be
        # converted is nearly all of this wallet's policy asset, and a fill has
        # to pay a network fee out of something that is not the batch -- a real
        # constraint, not a test artefact: a staker converting their entire
        # balance of the fee asset leaves nothing to pay with.
        top_up = 2000 * COIN
        top = CTransaction()
        top.nVersion = 2
        top.vin = [CTxIn(COutPoint(int(self.change_txid, 16), self.change_vout))]
        top_fee = 100_000
        top.vout = [
            CTxOut(top_up, bytes.fromhex(w0.getaddressinfo(w0.getnewaddress())["scriptPubKey"])),
            CTxOut(self.change_value - top_up - top_fee, CScript([0x51])),
            CTxOut(top_fee),
        ]
        n0.sendrawtransaction(top.serialize().hex())
        self.mine(1)
        # The delicate case. A partial fill has to hand the covenant's leftovers
        # back into an IDENTICAL covenant, or the offer would be destroyed by
        # the first taker who could not afford all of it.
        locked2 = 400 * COIN
        rate_den2 = locked2
        rate_num2 = earned_atoms * 100        # the whole offer costs far more than we have
        leaf2 = fill_leaf(a_internal, b_internal, rate_num2, rate_den2, 1, maker_prog)
        info2 = taproot_construct(NUMS, [("fill", leaf2, LEAF_VERSION_TAPSCRIPT),
                                         ("refund", rleaf, LEAF_VERSION_TAPSCRIPT)])
        cov2_spk = bytes(info2.scriptPubKey)
        cov2_txid = w0.sendtoaddress(
            address=n0.deriveaddresses(n0.getdescriptorinfo("raw(%s)" % cov2_spk.hex())["descriptor"])[0],
            amount=Decimal(locked2) / COIN, assetlabel=asset_a, fee_asset_label="bitcoin")
        self.mine(1)
        raw2 = n0.getrawtransaction(cov2_txid, True)
        cov2_vout = next(v["n"] for v in raw2["vout"]
                         if v["scriptPubKey"]["hex"] == cov2_spk.hex())
        leaf2_info = info2.leaves["fill"]
        FakeRelay.offers = [{
            "offer_id": "test-offer-2",
            "maker_pubkey": maker_pub,
            "offer_asset": asset_a,
            "want_asset": policy,
            "offer_amount": str(locked2),
            "want_amount": str(rate_num2),
            "allow_partial": True,
            "covenant": {
                "covenant_txid": cov2_txid,
                "covenant_vout": cov2_vout,
                "asset_a": a_internal.hex(),
                "asset_b": b_internal.hex(),
                "rate_num": str(rate_num2),
                "rate_den": str(rate_den2),
                "min_lot": "1",
                "maker_prog": maker_prog.hex(),
                "maker_x": maker_x.hex(),
                "internal_key": NUMS.hex(),
                "merkle_path": [leaf2_info.merklebranch[i:i + 32].hex()
                                for i in range(0, len(leaf2_info.merklebranch), 32)],
                "expiry_locktime": 1,
            },
        }]

        # Earn a little more so there is something left to convert.
        w0.settxfee(Decimal("0.02"))
        for _ in range(2):
            w0.sendtoaddress(address=wo.getnewaddress(), amount=1, fee_asset_label="bitcoin")
            self.mine(1)
        self.mine(101)

        report2 = w0.convertrewards(False)
        row2 = next((r for r in report2["considered"] if r["asset"] == policy), None)
        assert row2 is not None and row2["converts"], str(report2)
        assert "txid" in row2, "the partial fill did not broadcast: %s" % str(report2)
        self.mine(1)

        fill2 = n0.getrawtransaction(row2["txid"], True)
        rem = fill2["vout"][1]
        assert_equal(rem["asset"], asset_a)
        assert_equal(rem["scriptPubKey"]["hex"], cov2_spk.hex())
        assert_greater_than(int(round(float(rem["value"]) * COIN)), 0)
        # The offer survived: its remainder is spendable by the next taker on
        # exactly the same terms.
        assert n0.gettxout(fill2["txid"], 1) is not None
        self.log.info("partial fill: %s of the covenant taken, %s handed back to an identical one"
                      % (fill2["vout"][2]["value"], rem["value"]))

        self.log.info("A converted reward is never offered again")
        second = w0.convertrewards(True)
        for r in second["considered"]:
            assert r["asset"] != policy or not r["converts"], \
                "the same rewards were offered for conversion twice"

        self.relay.shutdown()


if __name__ == '__main__':
    PosRewardConvertTest().main()
