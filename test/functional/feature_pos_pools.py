#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Staking pools from a wallet: delegate, announce, watch, re-point, reclaim.

feature_pos_delegation.py covers the consensus primitive with hand-built
transactions. This covers the layer above it -- the RPCs a wallet, a GUI or a
pool board actually calls, and which until now did not exist: delegating meant
taking a script from getdelegationscript and assembling a payment to a bare
output by hand.

The shape of the feature:

  delegatestake     lend this wallet's stake weight to a signer. The coins never
                    move and the signer can never spend them; calling it again
                    with a different signer RE-POINTS, spending the old record
                    and creating the new one in ONE transaction (consensus
                    allows only one live record per controller, so two loose
                    transactions could be mined in an order that invalidates a
                    block).
  undelegatestake   take the rights back. Unilateral, no lock, no notice.
  announcepayout    the operator side: commit on-chain to how blocks pay out.
                    Cannot bind until the notice period has passed.
  listdelegations   the DELEGATOR'S WATCH. A payout policy cannot change without
                    notice -- but that only protects a delegator who sees the
                    notice, so any announced-but-not-yet-binding change is
                    reported here, with how long is left to act.
  listpools         the public board: who commands what weight, who lent it,
                    what each has committed to, and how reliably it produces.

The delegator watch is the point of the notice period, and this test asserts it
end to end: the pool announces a change, and the delegator's own wallet reports
it as pending with a warning, long before it binds.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

UNBONDING = 5
NOTICE = 10          # blocks a payout policy must be announced ahead of binding
COIN = 100_000_000
STAKE = 100          # SEQ


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosPoolsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()  # block producer (config-layer stake)
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
            "-staker=%s:%d" % (self.a_pub, COIN),
            "-validatepegin=0",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.a_wif)

    def pool_entry(self, node, signer):
        """The listpools row for a signer, or None.

        Asking for one signer explicitly answers whether or not it has declared
        itself a pool, which is what a wallet needs to describe the signer its
        stake is lent to."""
        for p in node.listpools(signer, 0)["pools"]:
            if p["signer"] == signer:
                return p
        return None

    def check_script_vector(self, node):
        """A pinned cross-implementation vector for the delegation record.

        The wallet kit (SWK) builds this same script in Rust, because a light
        wallet has to be able to SPEND a record to leave a pool and a bare script
        matches no descriptor. Two independent implementations of one consensus
        script is exactly where a silent divergence hides: a wrong push order
        would still look like a valid script, still relay, and simply credit the
        weight to the wrong key. The identical vector is asserted in
        lwk_wollet/src/sequentia_delegation.rs, so neither side can drift alone."""
        controller = "02989c0b76cb563971fdc9bef31ec06c3560f3249d6ee9e5d83c57625596e05f6f"
        signer = "03f991f944d1e1954a7fc8b9bf62e0d78f015f4c07762d505e20e6c45260a3661b"
        expected = ("06" + b"SEQDEL".hex() + "75"
                    + "21" + signer + "75"
                    + "21" + controller + "ac")
        assert_equal(node.getdelegationscript(controller, signer)["script"], expected)
        # The signer is pushed FIRST and the controller LAST, because the
        # controller is the key the final OP_CHECKSIG tests and therefore the
        # only key that can ever spend the record.
        assert_equal(len(expected) // 2, 78)

    def check_board_contract(self, node, expect_signer):
        """listpools is a published contract, not just an RPC: the staking pool
        board (github.com/ConcatenaLabs/sequentia-pool-board) is a
        static page that renders exactly these fields. Assert every one of them,
        so a rename here fails a test rather than silently emptying a column on
        a public page.

        `expect_signer` has not announced a payout policy at this point, so it is
        NOT a pool and must be reachable only by asking for it explicitly."""
        feed = node.listpools(None, 100, True, True)
        for key in ("height", "network_weight", "min_stake", "notice_blocks",
                    "block_seconds", "window", "pools", "declared_pools", "stakers"):
            assert key in feed, "listpools is missing %s" % key
        assert_equal(feed["notice_blocks"], NOTICE)
        # The board turns "binds in N blocks" into a date with this, so a zero
        # or missing cadence would render every deadline as "now".
        assert_greater_than(feed["block_seconds"], 0)

        row = None
        for p in feed["pools"]:
            if p["signer"] == expect_signer:
                row = p
        assert row is not None, "the pool this wallet delegates to is missing from listpools"
        for key in ("signer", "declared", "weight", "own_weight", "delegated_weight", "delegators",
                    "network_share", "eligible", "committee_ready", "payout",
                    "policy_pending", "blocks_produced", "blocks_expected", "delegator_list"):
            assert key in row, "listpools row is missing %s" % key
        assert_equal(row["delegated_weight"], STAKE * COIN)
        assert_equal(row["delegators"], 1)
        assert_equal(row["declared"], False)

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        self.log.info("The delegation-record script matches the vector SWK builds in Rust")
        self.check_script_vector(n0)

        self.log.info("A staker with no delegation signs for itself")
        addr = w0.getnewaddress()
        ctrl = w0.getaddressinfo(addr)["pubkey"]
        w0.registerstake(ctrl, STAKE)
        self.mine(1)
        assert_equal(n0.getstakerinfo()[ctrl], STAKE * COIN)

        mine_rows = w0.listdelegations()
        assert_equal(len(mine_rows), 1)
        row = mine_rows[0]
        assert_equal(row["controller"], ctrl)
        assert_equal(row["signer"], ctrl)          # itself: no record at all
        assert_equal(row["delegated"], False)
        assert_equal(row["weight"], STAKE * COIN)
        assert_equal(row["alerts"], [])            # nothing to warn about: nobody else is involved
        assert "record" not in row

        # It is already a "pool" on the board -- a pool is just a signer with
        # weight. It has committed to nothing, and the board says so.
        board = self.pool_entry(n0, ctrl)
        assert_equal(board["own_weight"], STAKE * COIN)
        assert_equal(board["delegated_weight"], 0)
        assert_equal(board["delegators"], 0)
        assert "no policy committed" in board["payout"]
        assert_equal(board["policy_pending"], [])

        self.log.info("Set up a pool operator in its own wallet")
        n0.createwallet("pool")
        pw = n0.get_wallet_rpc("pool")
        w0.sendtoaddress(address=pw.getnewaddress(), amount=50, fee_asset_label="bitcoin")
        self.mine(1)
        pool1 = pw.getaddressinfo(pw.getnewaddress())["pubkey"]
        pool2 = pw.getaddressinfo(pw.getnewaddress())["pubkey"]

        self.log.info("delegatestake lends the weight; the coins do not move")
        stake_utxo = w0.liststakeutxos()[0]
        res = w0.delegatestake(pool1)
        assert_equal(res["controller"], ctrl)
        assert_equal(res["signer"], pool1)
        assert_equal(res["delegated_weight"], STAKE * COIN)
        self.mine(1)

        assert_equal(n0.getdelegationinfo(), {ctrl: pool1})
        assert_equal(n0.getstakerinfo()[pool1], STAKE * COIN)
        assert ctrl not in n0.getstakerinfo()
        # The staking output itself was never touched.
        assert n0.gettxout(stake_utxo["txid"], stake_utxo["vout"]) is not None
        assert_equal(w0.liststakeutxos()[0]["amount"], Decimal(STAKE))

        row = w0.listdelegations()[0]
        assert_equal(row["delegated"], True)
        assert_equal(row["signer"], pool1)
        assert_equal(row["pool_share"], 1.0)
        assert_equal(row["record"]["confirmed"], True)
        assert_equal(row["record"]["changing"], False)
        # The honest default, stated plainly: an operator that has committed to
        # nothing keeps everything.
        assert any("committed to no payout policy" in a for a in row["alerts"])

        board = self.pool_entry(n0, pool1)
        assert_equal(board["delegated_weight"], STAKE * COIN)
        assert_equal(board["own_weight"], 0)
        assert_equal(board["delegators"], 1)
        detail = n0.listpools(pool1, 0, True)["pools"][0]
        assert_equal(detail["delegator_list"], [{"controller": ctrl, "weight": STAKE * COIN}])

        self.log.info("listpools carries every field the public pool board renders")
        self.check_board_contract(n0, pool1)

        self.log.info("Delegating to the same signer twice, or to yourself, is refused")
        assert_raises_rpc_error(-8, "already delegates to signer", w0.delegatestake, pool1)
        assert_raises_rpc_error(-8, "delegating to the controller itself", w0.delegatestake, ctrl)

        self.log.info("A holder with far less than the solo minimum can still delegate")
        # This is the whole point of pooling, and it must not require bonding
        # first: consensus applies the minimum-stake floor to what a SIGNER
        # commands once delegation is resolved, not to each delegator. So a
        # holding a fraction of the floor can be bonded and lent in ONE call,
        # and it counts in full toward the pool.
        small = w0.get_wallet_rpc(self.default_wallet_name) if False else w0
        n0.createwallet("smallholder")
        sw = n0.get_wallet_rpc("smallholder")
        w0.sendtoaddress(address=sw.getnewaddress(), amount=200, fee_asset_label="bitcoin")
        self.mine(1)

        # Nothing staked, and nowhere near the 40,000-style solo floor.
        assert_equal(sw.listdelegations(), [])
        before = n0.listpools(pool1, 0)["pools"][0]["weight"]

        res = sw.delegatestake(pool1, 150)
        assert_equal(res["staked"], Decimal("150"))
        assert_equal(res["signer"], pool1)
        assert_equal(res["delegated_weight"], 150 * COIN)
        # One transaction carries both the stake and the record: no separate
        # registerstake step, and nothing to get half-done.
        tx = n0.getrawtransaction(res["txid"], True)
        self.mine(1)

        # It counts for the pool, in full.
        assert_equal(n0.listpools(pool1, 0)["pools"][0]["weight"], before + 150 * COIN)
        assert_equal(n0.getdelegationinfo()[res["controller"]], pool1)
        row = sw.listdelegations()[0]
        assert_equal(row["delegated"], True)
        assert_equal(row["weight"], 150 * COIN)

        # And it can leave, like any other delegator.
        sw.undelegatestake()
        self.mine(1)
        assert res["controller"] not in n0.getdelegationinfo()

        # Staking ALONE still has a floor, because a stake that small could
        # never win a block by itself. The two paths differ on purpose.
        if n0.listpools(None, 0, False, True)["min_stake"] > 0:
            assert_raises_rpc_error(-8, "below the chain's minimum stake",
                                    sw.registerstake, sw.getaddressinfo(sw.getnewaddress())["pubkey"], 10)

        self.log.info("Delegating without an amount needs something already staked")
        n0.createwallet("emptyholder")
        ew = n0.get_wallet_rpc("emptyholder")
        assert_raises_rpc_error(-4, "Pass `amount` to bond some SEQ", ew.delegatestake, pool1)

        self.log.info("announcepayout: an operator commits, and cannot dodge the notice period")
        tip = n0.getblockcount()
        assert_raises_rpc_error(-8, "inside the notice period",
                                pw.announcepayout, "lottery", pool1, tip + 1, None, 500)
        ann = pw.announcepayout("lottery", pool1, None, None, 500)
        assert_equal(ann["signer"], pool1)
        assert_equal(ann["mode"], "lottery")
        assert_equal(ann["commission_bp"], 500)
        assert_equal(ann["notice_blocks"], NOTICE)
        assert_greater_than(ann["activation"], tip + NOTICE)
        activation = ann["activation"]
        self.mine(1)

        self.log.info("THE DELEGATOR WATCH: the delegator's own wallet reports the pending change")
        row = w0.listdelegations()[0]
        assert_equal(len(row["policy_pending"]), 1)
        pending = row["policy_pending"][0]
        assert_equal(pending["activation"], activation)
        assert_equal(pending["mode"], "lottery")
        assert_equal(pending["commission_bp"], 500)
        assert_greater_than(pending["blocks_away"], 0)
        assert any("announced a NEW payout policy" in a for a in row["alerts"])
        assert any("undelegatestake" in a for a in row["alerts"])
        # It is only PENDING: nothing binds the pool yet.
        assert "policy_in_force" not in row

        board = self.pool_entry(n0, pool1)
        assert_equal(len(board["policy_pending"]), 1)
        assert_equal(board["policy_pending"][0]["commission_bp"], 500)

        # Announcing IS the declaration, and a policy still serving its notice
        # counts: that is exactly what a new pool looks like, and it has to be
        # findable while delegators decide.
        assert_equal(board["declared"], True)
        listed = n0.listpools()
        assert_equal(listed["declared_pools"], 1)
        assert_equal([p["signer"] for p in listed["pools"]], [pool1])

        self.log.info("Once the notice has run, the policy binds and stops being pending")
        self.mine(activation - n0.getblockcount())
        assert_equal(n0.getblockcount(), activation)
        row = w0.listdelegations()[0]
        assert_equal(row["policy_pending"], [])
        assert_equal(row["policy_in_force"]["commission_bp"], 500)
        assert_equal(row["policy_in_force"]["mode"], "lottery")
        board = self.pool_entry(n0, pool1)
        assert "lottery" in board["payout"]
        assert "5.00%" in board["payout"]

        self.log.info("Re-pointing spends the old record and creates the new one in ONE transaction")
        old_record = w0.listdelegations()[0]["record"]
        res = w0.delegatestake(pool2)
        assert_equal(res["previous_signer"], pool1)
        assert_equal(res["signer"], pool2)
        rot = n0.getrawtransaction(res["txid"], True)
        # Exactly the old record in, exactly the new record out (plus the fee
        # output). A second record for one controller would invalidate the block
        # carrying it, so the swap has to be atomic.
        assert_equal(len(rot["vin"]), 1)
        assert_equal(rot["vin"][0]["txid"], old_record["txid"])
        assert_equal(rot["vin"][0]["vout"], old_record["vout"])
        self.mine(1)
        assert_equal(n0.getdelegationinfo(), {ctrl: pool2})
        assert_equal(n0.getstakerinfo()[pool2], STAKE * COIN)
        assert pool1 not in n0.getstakerinfo()
        # pool1 kept its announced policy, but commands nothing now.
        assert_equal(self.pool_entry(n0, pool1)["weight"], 0)

        self.log.info("undelegatestake takes the rights back, unilaterally")
        before = w0.getbalance()["bitcoin"]
        res = w0.undelegatestake()
        assert_equal(len(res["reclaimed"]), 1)
        assert_equal(res["reclaimed"][0]["controller"], ctrl)
        assert_equal(res["reclaimed"][0]["signer"], pool2)
        self.mine(1)

        assert_equal(n0.getdelegationinfo(), {})
        assert_equal(n0.getstakerinfo()[ctrl], STAKE * COIN)
        assert pool2 not in n0.getstakerinfo()
        row = w0.listdelegations()[0]
        assert_equal(row["delegated"], False)
        assert_equal(row["signer"], ctrl)
        assert_equal(row["alerts"], [])
        # The record's coins came home, and the stake was never touched by any
        # of this.
        assert_greater_than(w0.getbalance()["bitcoin"], before)
        assert_equal(w0.liststakeutxos()[0]["amount"], Decimal(STAKE))

        assert_raises_rpc_error(-4, "not delegating any stake", w0.undelegatestake)

        self.log.info("A pool can commit to a raw script, not only an address")
        # The genuinely custom arrangement: consensus compares the coinbase
        # output against the committed BYTES and asks nothing else of them, so
        # anything expressible as a script is a valid commitment -- a multisig, a
        # covenant, a contract that splits the reward. An address cannot say any
        # of that, and it is the only degree of freedom a payout record has that
        # the ordinary choices do not reach.
        raw = "51"  # OP_TRUE: shortest script that is not any address
        ann2 = pw.announcepayout("direct", pool2, None, None, None, None, raw)
        assert_equal(ann2["payout_script"], raw)
        assert "address" not in ann2, "OP_TRUE is no address, so none should be claimed"
        self.mine(1)
        # It reaches the board exactly as committed.
        entry = self.pool_entry(n0, pool2)
        assert_equal(entry["policy_pending"][0]["payout_script"], raw)
        assert_equal(entry["declared"], True)

        # An address and a raw script commit to different bytes, so both at once
        # is refused rather than silently preferring one.
        assert_raises_rpc_error(-8, "not both", pw.announcepayout,
                                "direct", pool2, None, pw.getnewaddress(), None, None, raw)
        # And a lottery has no script to commit to.
        assert_raises_rpc_error(-8, "payout_script applies to direct mode only",
                                pw.announcepayout, "lottery", pool2, None, None, 500, None, raw)
        # Bounds are enforced: consensus caps the committed script at 110 bytes.
        assert_raises_rpc_error(-8, "payout_script must be 1..110 bytes",
                                pw.announcepayout, "direct", pool2, None, None, None, None, "ab" * 111)

        self.log.info("The board survives a restart: it is a pure function of the UTXO set")
        w0.delegatestake(pool1)
        self.mine(1)
        before_restart = n0.listpools(pool1, 0)["pools"][0]
        self.restart_node(0, self.extra_args[0])
        n0 = self.nodes[0]
        after_restart = n0.listpools(pool1, 0)["pools"][0]
        assert_equal(before_restart["weight"], after_restart["weight"])
        assert_equal(before_restart["delegated_weight"], after_restart["delegated_weight"])
        assert_equal(before_restart["policy_in_force"], after_restart["policy_in_force"])


if __name__ == '__main__':
    PosPoolsTest().main()
