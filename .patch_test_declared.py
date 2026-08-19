p = "test/functional/feature_pos_pools.py"
s = open(p).read()

# The board contract check now has to reflect that a row only appears once the
# signer has declared. At the point it runs, pool1 has NOT announced yet.
old = '''    def check_board_contract(self, node, expect_signer):
        """listpools is a published contract, not just an RPC: the staking pool
        board (github.com/GracedEternalKingCabbageMan/sequentia-pool-board) is a
        static page that renders exactly these fields. Assert every one of them,
        so a rename here fails a test rather than silently emptying a column on
        a public page."""
        feed = node.listpools(None, 100, True)
        for key in ("height", "network_weight", "min_stake", "notice_blocks",
                    "block_seconds", "window", "pools"):
            assert key in feed, "listpools is missing %s" % key'''
new = '''    def check_board_contract(self, node, expect_signer):
        """listpools is a published contract, not just an RPC: the staking pool
        board (github.com/GracedEternalKingCabbageMan/sequentia-pool-board) is a
        static page that renders exactly these fields. Assert every one of them,
        so a rename here fails a test rather than silently emptying a column on
        a public page.

        `expect_signer` has not announced a payout policy at this point, so it is
        NOT a pool and must be reachable only by asking for it explicitly."""
        feed = node.listpools(None, 100, True, True)
        for key in ("height", "network_weight", "min_stake", "notice_blocks",
                    "block_seconds", "window", "pools", "declared_pools", "stakers"):
            assert key in feed, "listpools is missing %s" % key'''
assert old in s, "contract anchor missing"
s = s.replace(old, new, 1)

old = '''        for key in ("signer", "weight", "own_weight", "delegated_weight", "delegators",
                    "network_share", "eligible", "committee_ready", "payout",
                    "policy_pending", "blocks_produced", "blocks_expected", "delegator_list"):
            assert key in row, "listpools row is missing %s" % key
        assert_equal(row["delegated_weight"], STAKE * COIN)
        assert_equal(row["delegators"], 1)'''
new = '''        for key in ("signer", "declared", "weight", "own_weight", "delegated_weight", "delegators",
                    "network_share", "eligible", "committee_ready", "payout",
                    "policy_pending", "blocks_produced", "blocks_expected", "delegator_list"):
            assert key in row, "listpools row is missing %s" % key
        assert_equal(row["delegated_weight"], STAKE * COIN)
        assert_equal(row["delegators"], 1)
        assert_equal(row["declared"], False)'''
assert old in s, "row anchor missing"
s = s.replace(old, new, 1)

# pool_entry() is used all over the test; it asks for one signer explicitly, which
# still answers for undeclared signers. Say why.
old = '''    def pool_entry(self, node, signer):
        """The listpools row for a signer, or None."""
        for p in node.listpools(signer, 0)["pools"]:'''
new = '''    def pool_entry(self, node, signer):
        """The listpools row for a signer, or None.

        Asking for one signer explicitly answers whether or not it has declared
        itself a pool, which is what a wallet needs to describe the signer its
        stake is lent to."""
        for p in node.listpools(signer, 0)["pools"]:'''
assert old in s, "pool_entry anchor missing"
s = s.replace(old, new, 1)

# And a section asserting the rule itself.
anchor = '''        self.log.info("announcepayout: an operator commits, and cannot dodge the notice period")'''
new_section = '''        self.log.info("A pool is a signer that DECLARED itself one, not any staker with weight")
        # The whole point of the board is to list deliberate pooling initiatives.
        # Every chain has stakers producing for themselves, and calling those
        # pools puts words in their mouth. The only deliberate, on-chain opt-in
        # is announcing a payout policy, so that is the line.
        board = n0.listpools()
        assert_equal(board["pools"], [], "nobody has declared a pool yet")
        assert_equal(board["declared_pools"], 0)
        assert_greater_than(board["stakers"], 0)  # ...but the chain has stakers
        # They are still reachable, just not as pools.
        everyone = n0.listpools(None, 0, False, True)
        assert_greater_than(len(everyone["pools"]), 0)
        assert all(p["declared"] is False for p in everyone["pools"])
        # And one can always be read by name, which is how a wallet describes the
        # signer its stake is currently lent to.
        assert_equal(self.pool_entry(n0, pool1)["declared"], False)

''' + anchor
assert anchor in s, "section anchor missing"
s = s.replace(anchor, new_section, 1)

# After announcing, pool1 becomes a declared pool and appears unfiltered.
old = '''        board = self.pool_entry(n0, pool1)
        assert_equal(len(board["policy_pending"]), 1)
        assert_equal(board["policy_pending"][0]["commission_bp"], 500)'''
new = '''        board = self.pool_entry(n0, pool1)
        assert_equal(len(board["policy_pending"]), 1)
        assert_equal(board["policy_pending"][0]["commission_bp"], 500)

        # Announcing IS the declaration, and a policy still serving its notice
        # counts: that is exactly what a new pool looks like, and it has to be
        # findable while delegators decide.
        assert_equal(board["declared"], True)
        listed = n0.listpools()
        assert_equal(listed["declared_pools"], 1)
        assert_equal([p["signer"] for p in listed["pools"]], [pool1])'''
assert old in s, "announce anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("test updated for the declaration rule")
