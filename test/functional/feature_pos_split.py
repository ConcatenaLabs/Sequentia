#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Split payouts: a pool pays every delegator its exact proportional share.

The design (doc/sequentia/split-payouts-design.md) in one paragraph: under a
`split` policy the coinbase pays the pool's POT, a bare anyone-can-spend output
whose ONLY valid spend is a CLAIM that distributes it. Each pot output pays the
delegators whose stake and delegation records existed before that output did, in
exact proportion; shares below POS_SPLIT_MIN_PAYOUT roll into a fresh pot; and a
claim may withhold (fee plus claimer's margin) at most 1/99 of what it delivers.
Commission reuses the lottery's draw: a bp/10000 chance a block pays the leader
instead of the pot.

What this asserts, end to end on a live-producing chain:
 - a split coinbase pays the pot script, byte for byte;
 - claimpoolrewards distributes EXACTLY the floor-division shares the design
   specifies, to every eligible participant including the operator's own stake;
 - a delegator who joined after a pot output was created gets NOTHING from it
   (the front-running defence needs no minimum-age constant), and shares in the
   pots created after it joined;
 - theft is impossible: a hand-built transaction taking the pot is rejected at
   the mempool (which is what protects producers from poisoned blocks);
 - the withhold cap rejects a claim that burns more than 1/99;
 - the registry's pot view survives a restart (pure function of the UTXO set);
 - the flag day: below -con_splitpayoutheight a split record is inert and
   announcing one is refused, exactly as on a node that lacks the mode.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, hash160

UNBONDING = 5
NOTICE = 10
COIN = 100_000_000
MIN_PAYOUT = 1000              # POS_SPLIT_MIN_PAYOUT, atoms
WITHHOLD = 99                  # POS_SPLIT_WITHHOLD_RATIO


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


def pot_script_hex(signer_hex):
    """The pot script, byte for byte: <"SEQPOT"> OP_DROP <signer> OP_DROP OP_TRUE."""
    return "06" + b"SEQPOT".hex() + "75" + "21" + signer_hex + "75" + "51"


def p2wpkh_hex(pubkey_hex):
    return "0014" + hash160(bytes.fromhex(pubkey_hex)).hex()


class PosSplitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()  # the pool: config staker AND producer
        self.extra_args = [[
            "-con_pos=1",
            "-posvrf=1",
            "-posunbonding=%d" % UNBONDING,
            "-posslotinterval=1",
            "-pospayoutnotice=%d" % NOTICE,
            "-signblockscript=51",
            "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, 100 * COIN),
            "-validatepegin=0",
            "-txindex=1",
            "-acceptnonstdtxn=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

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

    def coinbase_fee_outputs(self, node, height):
        """(script_hex, atoms) for every value-carrying coinbase output at `height`."""
        blk = node.getblock(node.getblockhash(height), 2)
        out = []
        for v in blk['tx'][0]['vout']:
            if v.get('value', 0) and v['scriptPubKey']['hex'] != '6a':
                if not v['scriptPubKey']['hex'].startswith('6a'):
                    out.append((v['scriptPubKey']['hex'], int(round(v['value'] * COIN))))
        return out

    def pots_of(self, node, signer):
        row = node.listpools(signer, 0, False, True)["pools"]
        for p in row:
            if p["signer"] == signer:
                return p.get("pot", {}), p.get("pot_outputs", 0)
        return {}, 0

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        self.log.info("Announce a split policy for the producing pool (raw record, zero commission)")
        activation = n0.getblockcount() + NOTICE + 5
        rec = n0.getpayoutscript(self.a_pub, activation, "split", None, 0)
        assert_equal(rec["mode"], "split")
        # One raw transaction funds the record AND endows the test wallet: the
        # genesis free coin is a bare OP_TRUE output no descriptor wallet tracks,
        # so the wallet's money has to arrive at a real address of its own.
        w0_addr = w0.getaddressinfo(w0.getnewaddress())
        w0_spk = bytes.fromhex(w0_addr["scriptPubKey"])
        txid, voutn, in_amount = self.find_free_coin(n0)
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        record_value = 1_000_000
        fee = 100_000
        endowment = 5000 * COIN
        fund.vout = [
            CTxOut(record_value, bytes.fromhex(rec["script"])),
            CTxOut(endowment, w0_spk),
            CTxOut(in_amount - record_value - endowment - fee, CScript([0x51])),
            CTxOut(fee),
        ]
        self.change_txid = n0.sendrawtransaction(fund.serialize().hex())
        self.change_vout = 2
        self.change_value = in_amount - record_value - endowment - fee
        self.mine(1)
        info = n0.getpayoutinfo(self.a_pub)
        assert_equal(info[self.a_pub][0]["mode"], "split")

        self.log.info("Two delegators bond and lend in one step; a config staker needs no record")
        res0 = w0.delegatestake(self.a_pub, 150)
        n0.createwallet("d2")
        w2 = n0.get_wallet_rpc("d2")
        w0.sendtoaddress(address=w2.getnewaddress(), amount=300, fee_asset_label="bitcoin")
        self.mine(1)
        res2 = w2.delegatestake(self.a_pub, 50)
        self.mine(1)
        ctrl0, ctrl2 = res0["controller"], res2["controller"]
        assert_equal(n0.getstakerinfo()[self.a_pub], 300 * COIN)  # 100 own + 150 + 50

        self.log.info("Wait out the notice, then a fee-paying block's coinbase pays the POT")
        while n0.getblockcount() < activation:
            self.mine(1)
        w0.settxfee(Decimal("0.02"))        # fat fees: the pot must outgrow the claim fee 99-fold
        w2.settxfee(Decimal("0.02"))
        for _ in range(2):
            w0.sendtoaddress(address=w2.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        pot_h1 = n0.getblockcount()
        outs = self.coinbase_fee_outputs(n0, pot_h1)
        assert_equal(len(outs), 1)
        assert_equal(outs[0][0], pot_script_hex(self.a_pub))
        pot1_value = outs[0][1]
        assert_greater_than(pot1_value, 0)

        self.log.info("The pot shows on listpools, and grows with a second fee block")
        pot, npots = self.pots_of(n0, self.a_pub)
        assert_equal(npots, 1)
        for _ in range(2):
            w2.sendtoaddress(address=w0.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        pot_h2 = n0.getblockcount()
        pot2_value = self.coinbase_fee_outputs(n0, pot_h2)[0][1]
        pot, npots = self.pots_of(n0, self.a_pub)
        assert_equal(npots, 2)
        asset_hex = list(pot.keys())[0]
        assert_equal(int(round(Decimal(str(pot[asset_hex])) * COIN)), pot1_value + pot2_value)

        self.log.info("A delegator who joins AFTER the pots exist is owed nothing from them")
        n0.createwallet("latecomer")
        w3 = n0.get_wallet_rpc("latecomer")
        w0.sendtoaddress(address=w3.getnewaddress(), amount=200, fee_asset_label="bitcoin")
        self.mine(1)
        pot_h3 = n0.getblockcount()  # that send's fee became pot output 3
        res3 = w3.delegatestake(self.a_pub, 100)
        self.mine(1)
        ctrl3 = res3["controller"]
        pot_h4 = n0.getblockcount()  # the delegation tx's own fee: pot output 4
        pot4_value = self.coinbase_fee_outputs(n0, pot_h4)[0][1]
        pot3_value = self.coinbase_fee_outputs(n0, pot_h3)[0][1]

        self.log.info("Claim: every eligible participant is paid its exact floor-division share")
        # Expected shares, computed the way the design specifies: per pot output,
        # over the participants whose stake and record existed before it.
        # Pots 1-4 all postdate ctrl0/ctrl2's delegations; only the latecomer's
        # eligibility differs (its record exists only before nothing here -- its
        # stake confirmed at pot_h4's block, so height ctrl3 < pot_h4 is FALSE:
        # created in the same block, and eligibility is strictly-before).
        weights_old = {self.a_pub: 100 * COIN, ctrl0: 150 * COIN, ctrl2: 50 * COIN}
        total_old = sum(weights_old.values())
        owed = {k: 0 for k in weights_old}
        for v in (pot1_value, pot2_value, pot3_value, pot4_value):
            distributable = v - v // 100   # each input reserves 1% to fund the claim
            for k, w in weights_old.items():
                owed[k] += distributable * w // total_old

        # The claim's fee comes out of the pot under the withhold cap; the fat
        # rate set above was for FILLING the pot and would eat the whole margin.
        w2.settxfee(0)
        # Pot outputs are coinbase value: they mature like any other coinbase
        # reward, and a claim sweeps only what has matured.
        assert_raises_rpc_error(-4, "coinbase maturity", w2.claimpoolrewards, self.a_pub)
        self.mine(100)
        claim = w2.claimpoolrewards(self.a_pub)
        assert_equal(claim["delegators_paid"], 3)
        tx = n0.getrawtransaction(claim["txid"], True)
        paid = {}
        repotted = 0
        for v in tx["vout"]:
            spk = v["scriptPubKey"]["hex"]
            atoms = int(round(v.get("value", 0) * COIN)) if v.get("value") else 0
            if spk == pot_script_hex(self.a_pub):
                repotted += atoms
            for name, ctrl in (("pool", self.a_pub), ("d0", ctrl0), ("d2", ctrl2), ("d3", ctrl3)):
                if spk == p2wpkh_hex(ctrl):
                    paid[ctrl] = paid.get(ctrl, 0) + atoms
        assert ctrl3 not in paid, "the latecomer was paid from pots that predate it"
        for ctrl in (self.a_pub, ctrl0, ctrl2):
            assert_equal(paid[ctrl], owed[ctrl])
        # The margin obeys the cap: (fee + margin) * 99 <= distributed.
        fee_atoms = int(round(Decimal(str(claim["fee"])) * COIN))
        margin_atoms = int(round(Decimal(str(claim["margin"])) * COIN))
        distributed = sum(paid.values())
        assert_greater_than(distributed + 1, (fee_atoms + margin_atoms) * WITHHOLD)
        self.mine(1)
        pot, npots = self.pots_of(n0, self.a_pub)

        self.log.info("The latecomer shares in pots created after it joined")
        for _ in range(3):
            w0.sendtoaddress(address=w2.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        self.mine(100)
        claim2 = w3.claimpoolrewards(self.a_pub)
        tx2 = n0.getrawtransaction(claim2["txid"], True)
        w3_paid = 0
        for v in tx2["vout"]:
            if v["scriptPubKey"]["hex"] == p2wpkh_hex(ctrl3) and v.get("value"):
                w3_paid = int(round(v["value"] * COIN))
        # Its share of pot 5 (the claim also sweeps the previous claim's crumbs,
        # whose creation height postdates ctrl3 too, so w3 shares in those).
        assert_greater_than(w3_paid, 0)
        self.mine(1)

        self.log.info("Theft is rejected at the mempool: the claim rules are the spend condition")
        # Accrue a fresh pot, then try to take it.
        w0.sendtoaddress(address=w2.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        h = n0.getblockcount()
        self.mine(100)   # past coinbase maturity, so the refusal tested is the claim rule itself
        blk = n0.getblock(n0.getblockhash(h), 2)
        cb = blk["tx"][0]
        pot_vout = None
        pot_atoms = 0
        for i, v in enumerate(cb["vout"]):
            if v["scriptPubKey"]["hex"] == pot_script_hex(self.a_pub):
                pot_vout, pot_atoms = i, int(round(v["value"] * COIN))
        assert pot_vout is not None
        steal = CTransaction()
        steal.nVersion = 2
        steal.vin = [CTxIn(COutPoint(int(cb["txid"], 16), pot_vout))]
        steal.vout = [
            CTxOut(pot_atoms - 500, bytes.fromhex(p2wpkh_hex(ctrl2))),  # not what anyone is owed
            CTxOut(500),
        ]
        assert_raises_rpc_error(-26, "bad-pot-claim", n0.sendrawtransaction, steal.serialize().hex())

        self.log.info("A claim that over-withholds is rejected too")
        burn = CTransaction()
        burn.nVersion = 2
        burn.vin = [CTxIn(COutPoint(int(cb["txid"], 16), pot_vout))]
        # Pay a token amount 'correctly-shaped' but burn the rest as fee: the
        # withhold cap must catch it whatever the output shapes are.
        burn.vout = [CTxOut(pot_atoms)]
        assert_raises_rpc_error(-26, "bad-pot-claim", n0.sendrawtransaction, burn.serialize().hex())

        self.log.info("The pot view is a pure function of the UTXO set: it survives a restart")
        before_pot, before_n = self.pots_of(n0, self.a_pub)
        self.restart_node(0, self.extra_args[0])
        n0 = self.nodes[0]
        after_pot, after_n = self.pots_of(n0, self.a_pub)
        assert_equal(before_pot, after_pot)
        assert_equal(before_n, after_n)

        self.log.info("The flag day: below it, split is refused and a split record is inert")
        self.restart_node(0, self.extra_args[0] + ["-con_splitpayoutheight=100000"])
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        tip = n0.getblockcount()
        assert_greater_than(100000, tip)
        # Announcing is refused, naming the height. (Announced for a key this
        # wallet holds: the key-control check comes first, and a raw config key
        # would trip that instead of the gate.)
        assert_raises_rpc_error(-8, "activates at height 100000",
                                w0.announcepayout, "split", ctrl0, None, None, 0)
        # And a hand-funded split record is not recognised: the registry shows
        # only the (pre-existing) recognised policy set, exactly as a node
        # without the mode would.
        rec2 = n0.getpayoutscript(self.a_pub, 99000, "split", None, 100)
        fund2 = CTransaction()
        fund2.nVersion = 2
        fund2.vin = [CTxIn(COutPoint(int(self.change_txid, 16), self.change_vout))]
        fund2.vout = [
            CTxOut(record_value, bytes.fromhex(rec2["script"])),
            CTxOut(self.change_value - record_value - fee, CScript([0x51])),
            CTxOut(fee),
        ]
        n0.sendrawtransaction(fund2.serialize().hex())
        self.mine(1)
        info = n0.getpayoutinfo(self.a_pub)
        for entry in info.get(self.a_pub, []):
            assert entry["activation"] != 99000, "an inert pre-flag split record was recognised"


if __name__ == '__main__':
    PosSplitTest().main()
