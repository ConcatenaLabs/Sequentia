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
        """The listpools row for a signer, or None."""
        for p in node.listpools(signer, 0)["pools"]:
            if p["signer"] == signer:
                return p
        return None

    def check_board_contract(self, node, expect_signer):
        """listpools is a published contract, not just an RPC: the staking pool
        board (github.com/GracedEternalKingCabbageMan/sequentia-pool-board) is a
        static page that renders exactly these fields. Assert every one of them,
        so a rename here fails a test rather than silently emptying a column on
        a public page."""
        feed = node.listpools(None, 100, True)
        for key in ("height", "network_weight", "min_stake", "notice_blocks",
                    "block_seconds", "window", "pools"):
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
        for key in ("signer", "weight", "own_weight", "delegated_weight", "delegators",
                    "network_share", "eligible", "committee_ready", "payout",
                    "policy_pending", "blocks_produced", "blocks_expected", "delegator_list"):
            assert key in row, "listpools row is missing %s" % key
        assert_equal(row["delegated_weight"], STAKE * COIN)
        assert_equal(row["delegators"], 1)

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

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
