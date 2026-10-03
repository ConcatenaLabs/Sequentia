#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The fees of withdrawstake, claimunbonded and bumpwithdrawstakefee.

A fee is the node's fee rate (reference units per vbyte) over the
transaction's size, converted into the asset that pays it at the node's
exchange rate, so its value is the same whatever that asset is priced at.

 - By default the fee is paid in the Sequence token (SEQ) out of the coins the
   spend moves. Under two-step unbonding a stake may pay at most 1% of the
   staking outputs it spends that way (a consensus rule); a fee above that is
   refused in words that name fee_asset.
 - With fee_asset named, the fee is paid in that asset from the wallet's own
   coins, any asset the node accepts, and everything the spend moves arrives:
   the whole stake goes to the unbonding output. Consensus accepts that shape:
   the fee is not taken from the stake, so nothing is short.
 - An asset the node does not accept is refused, SEQ included.
 - Under two-step unbonding withdrawstake refuses an address; claimunbonded
   takes it.

Run on both sides of -posunbondheight (one-step below it, two-step from it),
at SEQ exchange rates of 0.25, 1 and 4 reference units per atom.

Topology: node0 = parent chain; node1 = the Sequentia node with the wallet.
"""
from decimal import Decimal, ROUND_UP

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_greater_than, assert_raises_rpc_error, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

UNBONDING = 5
DEPTH = 3
H = 40
COIN = 100_000_000
RATES = (25_000_000, 100_000_000, 400_000_000)   # SEQ exchange rates: 0.25, 1 and 4 reference units per atom


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def sats(x):
    return int((Decimal(str(x)) * COIN).to_integral_value())


class PosUnbondingFeesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.a_wif, self.a_pub = make_key()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        chain = "elementsregtest"
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-signblockscript=51"]], chain=[chain])
        self.start_node(0)
        pg = self.nodes[0].getblockhash(0)
        u, p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        self.add_nodes(1, [[
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-con_pos=1", "-posvrf=1", "-posunbonding=%d" % UNBONDING, "-posslotinterval=1",
            "-posunbondheight=%d" % H, "-posunbonddepth=%d" % DEPTH,
            "-signblockscript=51", "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1", "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, 100000 * COIN), "-validatepegin=0", "-txindex=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % u, "-mainchainrpcpassword=%s" % p,
            "-parentgenesisblockhash=%s" % pg, "-par=1",
        ]], chain=[chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.paddr = self.nodes[0].getnewaddress()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[1].generateposblock(self.a_wif)

    def rates(self, seq_rate, usdx_rate=100_000_000):
        self.nodes[1].setfeeexchangerates({self.seq: seq_rate, self.usdx: usdx_rate})

    def check_fee(self, txid, fee, fee_asset, seq_rate, bump=False):
        """The fee paid is the node's fee rate over the size, in the asset's units
        (for a replacement, at least that: it also pays for the one it replaces)."""
        s = self.nodes[1]
        e = s.getmempoolentry(txid)
        assert_equal(e["fees"]["asset"], fee_asset)
        assert_equal(sats(e["fees"]["base"]), sats(fee))
        rate = seq_rate if fee_asset == self.seq else 100_000_000
        value = sats(e["fees"]["value"])
        assert_equal(value, sats(fee) * rate // COIN)
        # value per vbyte: the node's fee rate, whatever the asset's price
        per_vb = Decimal(value) / e["vsize"]
        assert self.ref_rate <= per_vb, (per_vb, self.ref_rate)
        if not bump:
            assert per_vb < self.ref_rate + Decimal(rate) / COIN + 2, (per_vb, self.ref_rate)  # worst-case signature sizes
        return e

    def stake_value(self, txid, script_marker):
        tx = self.nodes[1].getrawtransaction(txid, True)
        return [o for o in tx["vout"] if script_marker in o["scriptPubKey"].get("asm", "")]

    def run_test(self):
        n0, s = self.nodes
        self.generatetoaddress(n0, 1, self.paddr, sync_fun=self.no_op)
        self.mine()
        if not s.listwallets():
            s.createwallet(wallet_name="", descriptors=False)
        s.rescanblockchain(0)
        self.seq = s.dumpassetlabels()["bitcoin"]
        issued = s.issueasset(assetamount=1000, tokenamount=0, blind=False, fee_asset=self.seq)
        self.usdx = issued["asset"]
        self.mine()
        assert_equal(s.getbalance()[self.usdx], 1000)
        pubs = [s.getaddressinfo(s.getnewaddress())["pubkey"] for _ in range(10)]
        for pk in pubs:
            s.registerstake(pk, 100)
        self.mine(UNBONDING + 2)
        assert_greater_than(H - 2, s.getblockcount())
        # The node's fee rate in reference units per vbyte, from a plain send at 1:1.
        self.rates(100_000_000)
        t = s.sendtoaddress(address=s.getnewaddress(), amount=1, fee_asset_label=self.seq)
        e = s.getmempoolentry(t)
        self.ref_rate = (Decimal(sats(e["fees"]["value"])) / e["vsize"]).quantize(Decimal(1), rounding=ROUND_UP) - 1
        self.mine()
        self.log.info("node fee rate about %s reference units per vbyte", self.ref_rate)

        self.log.info("One-step withdrawals (below the activation height): the fee follows the SEQ price")
        for i, rate in enumerate(RATES):
            self.rates(rate)
            r = s.withdrawstake(pubs[i])
            assert "unbonding" not in r
            self.check_fee(r["txid"], r["fee"], self.seq, rate)
            assert_equal(r["fee_asset"], self.seq)
            assert_equal(sats(r["amount"]) + sats(r["fee"]), 100 * COIN)
            self.log.info("  at %s rfa per SEQ atom: fee %s SEQ", Decimal(rate) / COIN, r["fee"])
            self.mine()
        self.rates(100_000_000)
        r = s.withdrawstake(pubs[3], None, None, self.usdx)
        assert_equal(r["fee_asset"], self.usdx)
        self.check_fee(r["txid"], r["fee"], self.usdx, 100_000_000)
        assert_equal(sats(r["amount"]), 100 * COIN)
        self.mine()
        assert_equal(s.gettransaction(r["txid"])["confirmations"], 1)

        while s.getblockcount() < H - 1:
            self.mine()
        assert_equal(s.listunbonding()["active"], True)

        self.log.info("Two-step: withdrawstake refuses an address and points to claimunbonded")
        assert_raises_rpc_error(-8, "pass it to claimunbonded", s.withdrawstake, pubs[4], None, s.getnewaddress())

        self.log.info("Two-step withdrawals: the fee follows the SEQ price, within the 1% cap")
        for i, rate in zip((4, 5, 6), RATES):
            self.rates(rate)
            r = s.withdrawstake(pubs[i])
            assert_equal(r["unbonding"], True)
            self.check_fee(r["txid"], r["fee"], self.seq, rate)
            assert_equal(sats(r["amount"]) + sats(r["fee"]), 100 * COIN)
            self.mine()
        first_anchor = s.getblockheader(s.getbestblockhash())["anchorheight"]
        # The next unbondings start two parent blocks later, so they unlock later.
        self.generatetoaddress(n0, 2, self.paddr, sync_fun=self.no_op)
        self.mine()

        self.log.info("A fee above the cap is refused in words that name fee_asset")
        self.rates(1000)   # SEQ almost worthless: the fee in SEQ atoms exceeds 1% of the stake
        assert_raises_rpc_error(-8, "Pass fee_asset to pay the fee from this wallet's other coins", s.withdrawstake, pubs[7])
        self.log.info("Paid in another asset from the wallet, the whole stake goes to the unbonding output")
        r = s.withdrawstake(pubs[7], None, None, self.usdx)
        assert_equal(r["fee_asset"], self.usdx)
        assert_equal(sats(r["amount"]), 100 * COIN)
        e = self.check_fee(r["txid"], r["fee"], self.usdx, 1000)
        tx = s.getrawtransaction(r["txid"], True)
        unbond = [o for o in tx["vout"] if o["scriptPubKey"]["hex"].startswith("0953455155")]
        assert_equal([sats(o["value"]) for o in unbond], [100 * COIN])
        assert_greater_than(len(tx["vin"]), 1)
        self.mine()   # consensus accepts it in a block
        assert_equal(s.gettransaction(r["txid"])["confirmations"], 1)
        assert pubs[7] not in s.getstakerinfo()

        self.log.info("An asset this node does not accept is refused, SEQ included")
        s.setfeeexchangerates({self.usdx: 100_000_000})
        assert_raises_rpc_error(-8, "does not accept the Sequence token (SEQ) for transaction fees", s.withdrawstake, pubs[8])
        s.setfeeexchangerates({self.seq: 100_000_000})
        assert_raises_rpc_error(-8, "does not accept %s for transaction fees" % self.usdx, s.withdrawstake, pubs[8], None, None, self.usdx)

        self.log.info("bumpwithdrawstakefee: SEQ out of the stake, from wallet coins, and switching asset")
        self.rates(400_000_000)
        w = s.withdrawstake(pubs[8])
        b = s.bumpwithdrawstakefee()
        assert_equal((b["old_fee_asset"], b["fee_asset"]), (self.seq, self.seq))
        assert_greater_than(sats(b["fee"]), sats(w["fee"]))
        self.check_fee(b["txid"], b["fee"], self.seq, 400_000_000, bump=True)
        old_value = sats(w["fee"]) * 400_000_000 // COIN
        new_value = sats(s.getmempoolentry(b["txid"])["fees"]["value"])
        assert_greater_than(new_value, old_value)
        b2 = s.bumpwithdrawstakefee(None, self.usdx)
        assert_equal((b2["old_fee_asset"], b2["fee_asset"]), (self.seq, self.usdx))
        assert_equal(sats(b2["amount"]), 100 * COIN)
        assert_greater_than(sats(s.getmempoolentry(b2["txid"])["fees"]["value"]), new_value)
        b3 = s.bumpwithdrawstakefee()
        assert_equal((b3["old_fee_asset"], b3["fee_asset"]), (self.usdx, self.usdx))
        assert_equal(sats(b3["amount"]), 100 * COIN)
        assert_greater_than(sats(b3["fee"]), sats(b2["fee"]))
        self.mine()
        assert_equal(s.gettransaction(b3["txid"])["confirmations"], 1)
        assert_equal(len(s.listunbonding()["outputs"]), 5)

        self.log.info("claimunbonded: the fee follows the SEQ price")
        self.generatetoaddress(n0, first_anchor + DEPTH - n0.getblockcount(), self.paddr, sync_fun=self.no_op)
        self.mine()
        claimable = [o for o in s.listunbonding()["outputs"] if o["claimable"]]
        assert_equal(len(claimable), 3)
        self.rates(1_000_000)   # 0.01 rfa per atom: 25.0.0 paid a fee the node refused to relay
        c = s.claimunbonded()
        assert_equal((c["fee_asset"], c["claimed_outputs"]), (self.seq, 3))
        self.check_fee(c["txid"], c["fee"], self.seq, 1_000_000)
        assert_equal(sats(c["amount"]) + sats(c["fee"]), sum(sats(o["amount"]) for o in claimable))
        self.mine()
        assert_equal(s.gettransaction(c["txid"])["confirmations"], 1)

        self.log.info("claimunbonded paid in another asset from the wallet: the whole claim arrives")
        self.generatetoaddress(n0, 2, self.paddr, sync_fun=self.no_op)
        self.mine()
        claimable = [o for o in s.listunbonding()["outputs"] if o["claimable"]]
        assert_equal(len(claimable), 2)
        self.rates(100_000_000)
        c2 = s.claimunbonded(None, self.usdx)
        assert_equal((c2["fee_asset"], c2["claimed_outputs"]), (self.usdx, 2))
        self.check_fee(c2["txid"], c2["fee"], self.usdx, 100_000_000)
        assert_equal(sats(c2["amount"]), sum(sats(o["amount"]) for o in claimable))
        self.mine()
        assert_equal(s.gettransaction(c2["txid"])["confirmations"], 1)
        assert_equal(s.listunbonding()["outputs"], [])


if __name__ == '__main__':
    PosUnbondingFeesTest().main()
