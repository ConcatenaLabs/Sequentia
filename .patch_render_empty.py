p = "/home/aejkohl/sequentia-pool-board/test_page_render.mjs"
s = open(p).read()

if "declared_pools" not in s:
    s = s.replace("""  window: 500,
  generated_at: Math.floor(Date.now() / 1000),
  pools: [
    {
      signer: '02' + 'ab'.repeat(32),""",
"""  window: 500,
  declared_pools: 1,
  stakers: 3,
  generated_at: Math.floor(Date.now() / 1000),
  pools: [
    {
      signer: '02' + 'ab'.repeat(32),
      declared: true,""", 1)

    s = s.replace("""    {
      signer: '03' + 'cd'.repeat(32),
      weight: 2000000000000,""",
"""    {
      // A staker producing for itself: carried by the feed so a wallet can look
      // it up, and deliberately NOT shown on the board as a pool.
      signer: '03' + 'cd'.repeat(32),
      declared: false,
      weight: 2000000000000,""", 1)

    s = s.replace("""    {
      signer: '02' + 'ef'.repeat(32),
      weight: 0,""",
"""    {
      signer: '02' + 'ef'.repeat(32),
      declared: true,
      weight: 0,""", 1)

    s = s.replace("""check((rows.match(/<tr class="row"/g) || []).length === feed.pools.length,
  `one row per pool (${feed.pools.length})`);""",
"""const declaredPools = feed.pools.filter((p) => p.declared !== false);
check((rows.match(/<tr class="row"/g) || []).length === declaredPools.length,
  `one row per DECLARED pool (${declaredPools.length} of ${feed.pools.length} signers)`);
for (const p of feed.pools.filter((x) => x.declared === false)) {
  check(!rows.includes(p.signer.slice(0, 10)),
    'a staker that never declared is not listed as a pool');
}
check(stats.includes('Other stakers'), 'the undeclared stakers are still counted');""", 1)

EMPTY_BLOCK = """
// The empty board is the state the live chain is in until someone declares, so
// it has to explain itself rather than look broken or look like an empty network.
{
  const emptyFeed = { ...FIXTURE, declared_pools: 0, stakers: 3,
                      pools: FIXTURE.pools.map((p) => ({ ...p, declared: false })) };
  const n2 = {};
  const mk2 = (id) => (n2[id] = { id, innerHTML: '', textContent: '', className: '', querySelectorAll: () => [] });
  ['stats', 'pending', 'rows', 'status', 'tbl'].forEach(mk2);
  const api2 = new Function('document', 'fetch', 'setInterval', 'console',
    script + '\\nreturn {render, load};')(
    { getElementById: (id) => n2[id] || mk2(id), querySelectorAll: () => [] },
    async () => ({ ok: true, json: async () => emptyFeed }), () => 0, console);
  await api2.load();
  const empty = n2.rows.innerHTML;
  check(empty.includes('No pool has declared itself yet'), 'an empty board says why it is empty');
  check(empty.includes('announcepayout'), 'and says how an operator appears on it');
  check(empty.includes('3 signer(s) are producing blocks'), 'and does not imply the network is empty');
  check(!empty.includes('undefined') && !empty.includes('NaN'), 'the empty state renders cleanly');
}

"""

REPORT = """console.log(failures ? `\\n${failures} FAILED` : '\\nPAGE RENDER OK');
process.exit(failures ? 1 : 0);
"""

if "No pool has declared itself yet" not in s:
    assert REPORT in s, "report anchor missing"
    s = s.replace(REPORT, EMPTY_BLOCK + REPORT, 1)

open(p, "w").write(s)
print("render test covers the declaration rule and the empty board")
