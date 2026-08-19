p = "/home/aejkohl/sequentia-pool-board/test_pool_board_server.py"
s = open(p).read()

old = """        self.assertEqual(seen, [("listpools", [None, 123, False])],
                         "the feed must issue exactly one fixed query")"""
new = """        self.assertEqual(seen, [("listpools", [None, 123, False, True])],
                         "the feed must issue exactly one fixed query")
        # include_undeclared is the last argument and is deliberately true: the
        # PAGE lists only declared pools, but a wallet reading this same feed has
        # to describe the signer its stake is lent to, which may have declared
        # nothing. Flipping it would silently make that lookup fail.
        self.assertIs(seen[0][1][3], True)"""
assert old in s, "query anchor missing"
s = s.replace(old, new, 1)

# The sample gains the declaration fields and an undeclared staker.
old = """SAMPLE = {
    "height": 96000,
    "network_weight": 8000000000000,
    "min_stake": 4000000000000,
    "notice_blocks": 2880,
    "block_seconds": 60,
    "window": 500,
    "pools": [{
        "signer": "02" + "ab" * 32,"""
new = """SAMPLE = {
    "height": 96000,
    "network_weight": 8000000000000,
    "min_stake": 4000000000000,
    "notice_blocks": 2880,
    "block_seconds": 60,
    "window": 500,
    "declared_pools": 1,
    "stakers": 2,
    "pools": [{
        "signer": "02" + "ab" * 32,
        "declared": True,"""
assert old in s, "sample anchor missing"
s = s.replace(old, new, 1)

old = """        "policy_pending": [{"activation": 99000, "mode": "lottery",
                            "commission_bp": 1000, "blocks_away": 3000}],
    }],
}"""
new = """        "policy_pending": [{"activation": 99000, "mode": "lottery",
                            "commission_bp": 1000, "blocks_away": 3000}],
    }, {
        # A staker producing for itself. The feed carries it so a wallet can
        # look it up; the page must NOT list it as a pool.
        "signer": "03" + "cd" * 32,
        "declared": False,
        "weight": 3000000000000, "own_weight": 3000000000000, "delegated_weight": 0,
        "delegators": 0, "network_share": 0.375, "eligible": True,
        "committee_ready": True, "blocks_produced": 180, "blocks_expected": 187.5,
        "reliability": 0.96,
        "payout": "no policy committed: this pool keeps everything the blocks it produces earn",
        "policy_pending": [],
    }],
}"""
assert old in s, "sample tail anchor missing"
s = s.replace(old, new, 1)

# The page contract now includes the declaration fields.
old = """        for field in ("height", "network_weight", "min_stake", "notice_blocks",
                      "block_seconds", "window", "pools", "generated_at"):"""
new = """        for field in ("height", "network_weight", "min_stake", "notice_blocks",
                      "block_seconds", "window", "pools", "generated_at",
                      "declared_pools", "stakers"):"""
assert old in s, "page field anchor missing"
s = s.replace(old, new, 1)

old = """        for field in ("signer", "weight", "own_weight", "delegated_weight", "delegators",
                      "network_share", "eligible", "committee_ready", "blocks_produced",
                      "blocks_expected", "reliability", "payout", "policy_in_force",
                      "policy_pending"):"""
new = """        for field in ("signer", "declared", "weight", "own_weight", "delegated_weight",
                      "delegators", "network_share", "eligible", "committee_ready",
                      "blocks_produced", "blocks_expected", "reliability", "payout",
                      "policy_in_force", "policy_pending"):"""
assert old in s, "pool field anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("feed tests updated")

print("(render test handled separately)")
