#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""liststakingrewards: which of a wallet's coins it was PAID for staking.

Attribution is layer 1 of reward auto-conversion
(doc/sequentia/reward-autoconvert-design.md). It has to be exact, because the
layer above it sells whatever this says is a reward, and it has to be decidable
from wallet data alone, because the light wallets reach the same verdict without
a chainstate to consult.

There are exactly two shapes, being the two ways the consensus rules pay a
staker: a coinbase output the wallet owns (solo, or a pool's direct/lottery
payout), and a share of a pool's pot paid to P2WPKH(controller) by a claim.

What this asserts:
 - a solo producer's fee-bearing coinbase is reported as `solo`, in the asset it
   was actually paid in, with maturity honoured and per-asset totals that add up;
 - a plain payment into the wallet is NOT a reward, however ordinary it looks;
 - a payment the wallet SENT to its own staking address is not a reward either
   (this is what excludes a delegator's own withdrawal or re-pointing);
 - a split pool's claim is reported as `split` by the delegator that was paid;
 - spent rewards are hidden unless asked for, and reappear with include_spent;
 - the asset filter and `count` behave, and `count` never truncates the totals.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript

UNBONDING = 5
NOTICE = 10
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosRewardsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()   # the producer, and the test wallet's own key
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

    def rewards(self, w, **kwargs):
        return w.liststakingrewards(**kwargs)

    def total_for(self, res, asset):
        for t in res["totals"]:
            if t["asset"] == asset:
                return t
        return None

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        self.log.info("A wallet with no staking history has no rewards")
        res = self.rewards(w0)
        assert_equal(res["rewards"], [])
        assert_equal(res["totals"], [])

        # The producing key is a CONFIG staker (-staker=), which holds weight
        # without any stake output in the wallet: exactly how the committee
        # nodes run, and the case that a wallet-outputs-only attribution misses.
        # Importing wpkh(<wif>) makes the wallet own the coinbase's payout
        # script, which is what makes those coinbases this wallet's rewards.
        self.log.info("Own the producer's key, and endow the wallet from the genesis free coin")
        desc = n0.getdescriptorinfo("wpkh(%s)" % self.a_pub)["descriptor"]
        w0.importprivkey(self.a_wif, "staker", False)

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
        fund_txid = n0.sendrawtransaction(fund.serialize().hex())
        self.mine(1)
        policy_asset = n0.dumpassetlabels()["bitcoin"]

        self.log.info("An ordinary payment into the wallet is not a reward")
        # The endowment is exactly that: a plain receive, at a plain address, in
        # a transaction the wallet did not send. The block that mined it DID pay
        # a reward -- it collected that transaction's fee, and there is no
        # subsidy, so the coinbase is the fee and nothing else -- which is the
        # point: the fee-bearing coinbase is a reward, the endowment beside it
        # is not.
        res = self.rewards(w0)
        assert all(x["txid"] != fund_txid for x in res["rewards"])
        assert_equal(set(x["source"] for x in res["rewards"]), {"solo"})

        self.log.info("A fee-bearing block this wallet's key produced IS a reward")
        n0.createwallet("other")
        wo = n0.get_wallet_rpc("other")
        w0.settxfee(Decimal("0.02"))
        before = {(x["txid"], x["vout"]) for x in res["rewards"]}
        w0.sendtoaddress(address=wo.getnewaddress(), amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        reward_height = n0.getblockcount()

        res = self.rewards(w0)
        fresh = [x for x in res["rewards"] if (x["txid"], x["vout"]) not in before]
        assert_equal(len(fresh), 1)
        r = fresh[0]
        assert_equal(r["source"], "solo")
        assert_equal(r["height"], reward_height)
        assert_equal(r["controller"], self.a_pub)
        assert_equal(r["spent"], False)
        assert_greater_than(r["amount"], 0)
        # No subsidy: the reward is exactly the fee the block collected, in the
        # asset that paid it.
        assert_equal(r["asset"], policy_asset)
        # Coinbase value: immature on arrival, and reported as such rather than
        # quietly omitted -- a staker wants to see income that is coming.
        assert_equal(r["mature"], False)
        assert_greater_than(r["blocks_to_maturity"], 0)

        t = self.total_for(res, policy_asset)
        assert_equal(t["outputs"], len(res["rewards"]))
        # Nothing has matured yet, so every atom earned is in `immature`.
        assert_equal(t["mature"], Decimal("0E-8"))
        assert_equal(t["immature"], sum(x["amount"] for x in res["rewards"]))

        self.log.info("Rewards accumulate, and mature into the spendable total")
        for _ in range(3):
            w0.sendtoaddress(address=wo.getnewaddress(), amount=1, fee_asset_label="bitcoin")
            self.mine(1)
        res = self.rewards(w0)
        assert_equal(len(res["rewards"]), len(before) + 4)
        assert_equal(set(x["source"] for x in res["rewards"]), {"solo"})
        # Newest first.
        heights = [x["height"] for x in res["rewards"]]
        assert_equal(heights, sorted(heights, reverse=True))
        earned = self.total_for(res, policy_asset)["immature"]

        self.mine(101)   # clear COINBASE_MATURITY for everything earned so far
        res = self.rewards(w0)
        t = self.total_for(res, policy_asset)
        assert_equal(t["mature"], earned)
        assert_equal(t["immature"], Decimal("0E-8"))
        assert_equal(all(x["mature"] for x in res["rewards"]), True)
        assert_equal(all(x["blocks_to_maturity"] == 0 for x in res["rewards"]), True)

        self.log.info("`count` caps the listing but never the totals")
        capped = self.rewards(w0, count=2)
        assert_equal(len(capped["rewards"]), 2)
        assert_equal(self.total_for(capped, r["asset"])["outputs"],
                     self.total_for(res, r["asset"])["outputs"])

        self.log.info("The asset filter selects, and an unknown asset is refused")
        assert_equal(len(self.rewards(w0, asset="bitcoin")["rewards"]), len(res["rewards"]))
        assert_raises_rpc_error(-11, "Unknown label and invalid asset hex",
                                w0.liststakingrewards, False, "NOSUCHASSET")

        self.log.info("A spent reward drops out of the listing, and comes back with include_spent")
        # Lock every other coin so the send has exactly one input to choose, and
        # drop the fee for this one transaction: a reward here is only as big as
        # the fee that earned it, so a fat fee cannot be paid out of one.
        spend_me = max(res["rewards"], key=lambda x: x["amount"])
        others = [{"txid": u["txid"], "vout": u["vout"]} for u in w0.listunspent()
                  if (u["txid"], u["vout"]) != (spend_me["txid"], spend_me["vout"])]
        w0.lockunspent(False, others)
        w0.settxfee(Decimal("0.00001"))
        # No fee_asset_label with subtractfeefromamount: the fee comes out of
        # the output, so its asset is already determined and is not a choice.
        w0.sendtoaddress(address=wo.getnewaddress(), amount=spend_me["amount"] / 2,
                         subtractfeefromamount=True)
        self.mine(1)
        w0.lockunspent(True)
        w0.settxfee(Decimal("0.02"))
        after = self.rewards(w0)
        spent_ids = {(x["txid"], x["vout"]) for x in after["rewards"]}
        assert (spend_me["txid"], spend_me["vout"]) not in spent_ids
        with_spent = self.rewards(w0, include_spent=True)
        found = [x for x in with_spent["rewards"]
                 if (x["txid"], x["vout"]) == (spend_me["txid"], spend_me["vout"])]
        assert_equal(len(found), 1)
        assert_equal(found[0]["spent"], True)

        self.log.info("A payment the wallet sent to its OWN staking address is not a reward")
        # This is the shape of a delegator's own withdrawal or re-pointing: it
        # pays back to the staking key, and it must never be mistaken for a
        # pool paying out.
        addr = n0.deriveaddresses(desc)[0]
        before = len(self.rewards(w0)["rewards"])
        w0.sendtoaddress(address=addr, amount=1, fee_asset_label="bitcoin")
        self.mine(1)
        # One new reward is expected: the coinbase of the block that mined it.
        # The self-payment itself must not add a second.
        assert_equal(len(self.rewards(w0)["rewards"]), before + 1)
        assert_equal(set(x["source"] for x in self.rewards(w0)["rewards"]), {"solo"})

        self.log.info("A split pool's claim is reported as `split` by the delegator it paid")
        activation = n0.getblockcount() + NOTICE + 2
        w0.announcepayout("split", self.a_pub, activation, None, 0)
        self.mine(1)
        n0.createwallet("delegator")
        wd = n0.get_wallet_rpc("delegator")
        w0.sendtoaddress(address=wd.getnewaddress(), amount=300, fee_asset_label="bitcoin")
        self.mine(1)
        res_d = wd.delegatestake(self.a_pub, 200)
        self.mine(1)
        # 100 the operator staked in config, 200 lent: the delegator commands
        # two thirds of what the pool signs with, and so of what it pays out.
        assert_equal(n0.getstakerinfo()[self.a_pub], 300 * COIN)
        assert_equal(self.rewards(wd)["rewards"], [])   # delegating earns nothing yet

        while n0.getblockcount() < activation:
            self.mine(1)
        # Fat fees so the pot outgrows the claim's own fee ninety-nine-fold.
        wd.settxfee(Decimal("0.02"))
        for _ in range(3):
            w0.sendtoaddress(address=wo.getnewaddress(), amount=1, fee_asset_label="bitcoin")
            self.mine(1)
        self.mine(101)   # the pot is coinbase value: it matures like one

        # The claim's own fee is capped at 1/99 of what it delivers, and it is
        # sized at THIS wallet's fee rate -- which is still set fat, to grow the
        # pot. Drop it back to something ordinary before claiming.
        w0.settxfee(Decimal("0.0001"))
        claim = w0.claimpoolrewards(self.a_pub)
        assert_greater_than(claim["delegators_paid"], 0)
        self.mine(1)

        res = self.rewards(wd)
        assert_greater_than(len(res["rewards"]), 0)
        assert_equal(set(x["source"] for x in res["rewards"]), {"split"})
        paid = res["rewards"][0]
        assert_equal(paid["txid"], claim["txid"])
        assert_equal(paid["controller"], res_d["controller"])
        # A pot claim is an ordinary transaction output: spendable at once.
        assert_equal(paid["mature"], True)
        assert_equal(paid["blocks_to_maturity"], 0)
        assert_equal(self.total_for(res, paid["asset"])["mature"], paid["amount"])

        self.log.info("The delegator's own delegation record is not a reward")
        # delegatestake pays a bare record script from the delegator's own
        # wallet; it is neither a coinbase nor a payment to the staking key.
        assert (claim["txid"], ) != (res_d["txid"], )
        assert all(x["txid"] != res_d["txid"] for x in res["rewards"])


if __name__ == '__main__':
    PosRewardsTest().main()
