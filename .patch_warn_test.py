p = "/home/aejkohl/sequentia-extension/test/staking-warnings.test.mjs"
s = open(p).read()

old = """const board = (pool = {}) => ({
  block_seconds: 60,
  pools: [{
    signer: SIGNER, weight: 1, delegators: 1, network_share: 0.1,
    eligible: true, committee_ready: true, policy_pending: [], ...pool,
  }],
});"""
new = """const board = (pool = {}) => ({
  block_seconds: 60,
  pools: [{
    signer: SIGNER, weight: 1, delegators: 1, network_share: 0.1,
    eligible: true, committee_ready: true, policy_pending: [], declared: true, ...pool,
  }],
});"""
assert old in s, "board helper anchor missing"
s = s.replace(old, new, 1)

s += """
test('a signer that never declared itself a pool is named as such', () => {
  // Delegating to a plain staker is allowed and consensus will not stop it, but
  // the wallet must not let anyone believe they joined a pool. This is a
  // different sentence from "a pool that committed to nothing", because the
  // signer never asked for delegations at all.
  const w = delegationWarnings(record(), board({ declared: false, policy_in_force: undefined }));
  const first = w.find((s) => s.includes('has not declared itself a pool'));
  assert.ok(first, 'an undeclared signer must be named as one');
  assert.ok(first.includes('never asked'));
  assert.ok(!w.some((s) => s.includes('This pool has committed to no payout policy')),
    'and must not also be described as a pool');
});

test('a declared pool that committed nothing still gets the pool wording', () => {
  const w = delegationWarnings(record(), board({ declared: true, policy_in_force: undefined }));
  assert.ok(w.some((s) => s.includes('This pool has committed to no payout policy')));
  assert.ok(!w.some((s) => s.includes('has not declared itself a pool')));
});
"""
open(p, "w").write(s)
print("added the undeclared-signer cases")
