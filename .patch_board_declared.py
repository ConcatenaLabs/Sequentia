import re

# ---- feed: ask for everyone, flagged; the page decides what is a pool ----------
p = "/home/aejkohl/sequentia-pool-board/pool-board-server.py"
s = open(p).read()
old = """        data = rpc("listpools", [None, WINDOW, False])"""
new = """        # include_undeclared: the page shows only signers that DECLARED
        # themselves a pool, but a wallet reading this feed also has to be able
        # to describe the signer its stake is lent to, which may have declared
        # nothing. One fetch serves both; each entry carries `declared`.
        data = rpc("listpools", [None, WINDOW, False, True])"""
assert old in s, "feed anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("feed asks for all signers, flagged")

# ---- page ----------------------------------------------------------------------
p = "/home/aejkohl/sequentia-pool-board/index.html"
s = open(p).read()

# Subtitle: say what a pool is.
old = """  <p class="sub">Every signer producing blocks on the Sequentia network, the stake weight it commands, who lent
  it, what it has committed on-chain to paying, and how reliably it actually produces. A pool never holds your
  coins, so what you are choosing here is a block producer, not a custodian.</p>"""
new = """  <p class="sub">The staking pools on the Sequentia network: the stake weight each commands, who lent it, what
  it has committed on-chain to paying, and how reliably it actually produces. A pool never holds your coins, so
  what you are choosing here is a block producer, not a custodian.</p>
  <p class="sub"><strong>A pool is a signer that has declared itself one</strong>, by committing a payout policy
  on-chain. That is the only deliberate opt-in there is, and it is what separates an operator asking for your
  delegation from the many stakers who simply produce blocks for themselves. You may delegate to any signer, and
  the chain will not stop you, but a signer that has never made a commitment is not running a pool and is not
  listed here as though it were.</p>"""
assert old in s, "subtitle anchor missing"
s = s.replace(old, new, 1)

# Stat tiles: pools vs stakers.
old = """    ['Pools', pools.length, 'signers commanding weight or committed to a policy'],"""
new = """    ['Pools', d.declared_pools ?? pools.length, 'signers that declared themselves one'],
    ['Other stakers', Math.max(0, (d.stakers ?? 0) - (d.declared_pools ?? 0)), 'producing for themselves, not soliciting delegation'],"""
assert old in s, "stat anchor missing"
s = s.replace(old, new, 1)

# Only declared pools go in the table.
old = """  const sorted = pools.slice().sort((a,b) => {"""
new = """  // The board lists pools, not stakers. The feed carries every signer so a
  // wallet can look up whichever one its stake is lent to; the page shows only
  // those that declared.
  const declared = pools.filter(p => p.declared !== false);
  const sorted = declared.slice().sort((a,b) => {"""
assert old in s, "sort anchor missing"
s = s.replace(old, new, 1)

old = """  const changing = pools.filter(p => (p.policy_pending||[]).length);"""
new = """  const changing = declared.filter(p => (p.policy_pending||[]).length);"""
assert old in s, "pending anchor missing"
s = s.replace(old, new, 1)

# An empty board has to explain itself, not look broken.
old = """  $('rows').innerHTML = sorted.map(p => {"""
new = """  if (!sorted.length) {
    // Not an error, and not an empty network: nobody has opted in yet. Say both,
    // and say how one appears here, because the page is also read by operators.
    const others = Math.max(0, (d.stakers ?? 0) - (d.declared_pools ?? 0));
    $('rows').innerHTML = `<tr><td colspan="6" style="padding:26px 18px">
      <div style="font-size:16px;color:#fff;margin-bottom:8px">No pool has declared itself yet.</div>
      <div class="muted" style="max-width:70ch">
        ${others} signer(s) are producing blocks for themselves right now. They are not listed as pools because
        none of them has committed a payout policy, and listing a staker as a pool would put words in its mouth.
        <br><br>
        An operator appears here by making that commitment on-chain, from the node wallet:
        <code>sequentia-cli announcepayout "lottery"</code> (or <code>"direct"</code>). It takes effect after the
        notice period, and shows on this page from the moment it is announced.
      </div></td></tr>`;
  } else $('rows').innerHTML = sorted.map(p => {"""
assert old in s, "rows anchor missing"
s = s.replace(old, new, 1)

# Status line wording.
old = """  $('status').textContent = `${pools.length} pool(s), read at height ` +"""
new = """  $('status').textContent = `${sorted.length} pool(s) of ${d.stakers ?? pools.length} staker(s), read at height ` +"""
assert old in s, "status anchor missing"
s = s.replace(old, new, 1)

# Explainer: how to appear.
old = """  <h2>Joining a pool</h2>"""
new = """  <h2>Running one</h2>
  <div class="explain">
    <p>A staker becomes a pool by committing, on-chain, to how the blocks it produces will pay out. Until it does
    that it is simply a staker producing for itself, and this page will not list it. From the node wallet's
    Staking tab, or:</p>
    <pre>sequentia-cli announcepayout "lottery"          # pays one delegator per block, drawn by stake weight
sequentia-cli announcepayout "lottery" null null null 500   # ...keeping 5% commission
sequentia-cli announcepayout "direct"           # pays one committed address every block</pre>
    <p>The commitment appears here immediately and binds after the chain's notice period, which is the window in
    which anyone already delegated to you can read it and leave. Announcing is deliberately the only way onto
    this page: it is what turns "a staker" into "a pool asking for your delegation", and it is the thing a
    delegator is actually trusting.</p>
  </div>

  <h2>Joining a pool</h2>"""
assert old in s, "explainer anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("board page lists declared pools only, with an empty state")
