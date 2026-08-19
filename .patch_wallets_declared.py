import re

# ---------------------------------------------------------------- web wallet --
p = "/home/aejkohl/sequentia-web-wallet/index.html"
s = open(p).read()

old = """  const total = Number(POOLS.network_weight)||0;
  for(const p of POOLS.pools){"""
new = """  // Only signers that DECLARED themselves a pool are offered. The feed carries
  // every staker so the card can describe whichever one this wallet is lent to,
  // but a staker producing for itself never asked for delegations and must not
  // be presented as though it had.
  const offered = POOLS.pools.filter(p => p.declared !== false);
  if(!offered.length){
    list.appendChild(el('div','muted','No pool has declared itself yet. '
      + ((POOLS.stakers||0) + ' signer(s) are producing blocks for themselves; a staker becomes a pool by '
      + 'committing a payout policy on-chain, and appears here when it does.')));
    return;
  }
  const total = Number(POOLS.network_weight)||0;
  for(const p of offered){"""
assert old in s, "ww picker anchor missing"
s = s.replace(old, new, 1)

old = """  if(!POOLS || !POOLS.pools.length){
    list.appendChild(el('div','muted', POOLS ? 'No pools are producing yet.' : 'Pool list unavailable right now.'));
    return;
  }"""
new = """  if(!POOLS || !POOLS.pools.length){
    list.appendChild(el('div','muted', POOLS ? 'No signers are producing yet.' : 'Pool list unavailable right now.'));
    return;
  }"""
assert old in s, "ww empty anchor missing"
s = s.replace(old, new, 1)

# Describing the signer we are lent to: say the accurate thing when it never declared.
old = """  if(!pool.policy_in_force){
    warn('This pool has committed to no payout policy, so by default it keeps everything its blocks earn. Nothing on-chain obliges it to pay you.');
  } else if(pool.policy_in_force.mode==='direct'){"""
new = """  if(pool.declared === false){
    warn('This signer has not declared itself a pool: it has committed to no payout policy and never asked for '
       + 'delegations. It keeps everything its blocks earn, and nothing on-chain obliges it to pay you.');
  } else if(!pool.policy_in_force){
    warn('This pool has committed to no payout policy, so by default it keeps everything its blocks earn. Nothing on-chain obliges it to pay you.');
  } else if(pool.policy_in_force.mode==='direct'){"""
assert old in s, "ww warn anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("web wallet: offers declared pools only")

# ----------------------------------------------------------------- extension --
p = "/home/aejkohl/sequentia-extension/popup/popup.js"
s = open(p).read()

old = """  const total = Number(board.network_weight) || 0;
  for (const p of board.pools) {"""
new = """  // Only signers that DECLARED themselves a pool are offered. The feed carries
  // every staker so the card can describe whichever one this wallet is lent to,
  // but a staker producing for itself never asked for delegations.
  const offered = board.pools.filter((p) => p.declared !== false);
  if (!offered.length) {
    list.appendChild(el('div', 'sub', 'No pool has declared itself yet. '
      + `${board.stakers || 0} signer(s) are producing blocks for themselves; a staker becomes a pool by `
      + 'committing a payout policy on-chain, and appears here when it does.'));
    return;
  }
  const total = Number(board.network_weight) || 0;
  for (const p of offered) {"""
assert old in s, "ext picker anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("extension popup: offers declared pools only")

p = "/home/aejkohl/sequentia-extension/src/staking-warnings.js"
s = open(p).read()
old = """  if (!pool.policy_in_force) {
    out.push('This pool has committed to no payout policy, so by default it keeps everything its blocks earn. Nothing on-chain obliges it to pay you.');
  } else if (pool.policy_in_force.mode === 'direct') {"""
new = """  if (pool.declared === false) {
    // Delegating to a plain staker is allowed and the chain will not stop it,
    // but the wallet should not let anyone believe they joined a pool.
    out.push('This signer has not declared itself a pool: it has committed to no payout policy and never asked '
      + 'for delegations. It keeps everything its blocks earn, and nothing on-chain obliges it to pay you.');
  } else if (!pool.policy_in_force) {
    out.push('This pool has committed to no payout policy, so by default it keeps everything its blocks earn. Nothing on-chain obliges it to pay you.');
  } else if (pool.policy_in_force.mode === 'direct') {"""
assert old in s, "ext warn anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("extension warnings: undeclared signer gets its own message")

# --------------------------------------------------------------------- ambra --
p = "/home/aejkohl/ambra/app/lib/src/screens/stake_screen.dart"
s = open(p).read()

old = """  _Pool(this.signer, this.weight, this.delegators, this.payout, this.reliability,
      this.eligible, this.pendingBlocks, this.pendingMode);
  final String signer;"""
new = """  _Pool(this.signer, this.weight, this.delegators, this.payout, this.reliability,
      this.eligible, this.pendingBlocks, this.pendingMode, this.declared);
  final String signer;
  /// Whether this signer declared itself a pool by committing a payout policy.
  /// The feed carries every staker so the screen can describe whichever one this
  /// wallet is lent to; only declared ones are offered to join.
  final bool declared;"""
assert old in s, "ambra pool ctor anchor missing"
s = s.replace(old, new, 1)

old = """      pending is Map ? (pending['blocks_away'] as num?)?.toInt() : null,
      pending is Map ? pending['mode'] as String? : null,
    );"""
new = """      pending is Map ? (pending['blocks_away'] as num?)?.toInt() : null,
      pending is Map ? pending['mode'] as String? : null,
      j['declared'] != false,
    );"""
assert old in s, "ambra parse anchor missing"
s = s.replace(old, new, 1)

old = """            _networkWeight = BigInt.tryParse('${j['network_weight']}') ?? BigInt.zero;
            _blockSeconds = (j['block_seconds'] as num?)?.toInt() ?? 60;"""
new = """            _networkWeight = BigInt.tryParse('${j['network_weight']}') ?? BigInt.zero;
            _blockSeconds = (j['block_seconds'] as num?)?.toInt() ?? 60;
            _stakers = (j['stakers'] as num?)?.toInt() ?? 0;"""
assert old in s, "ambra load anchor missing"
s = s.replace(old, new, 1)

old = """  BigInt _networkWeight = BigInt.zero;"""
new = """  BigInt _networkWeight = BigInt.zero;
  int _stakers = 0;"""
assert old in s, "ambra field anchor missing"
s = s.replace(old, new, 1)

old = """    if (p.payout.contains('no policy committed')) {"""
new = """    if (!p.declared) {
      out.add('This signer has not declared itself a pool: it has committed to no payout policy and never '
          'asked for delegations. It keeps everything its blocks earn, and nothing on-chain obliges it to '
          'pay you.');
    } else if (p.payout.contains('no policy committed')) {"""
assert old in s, "ambra warn anchor missing"
s = s.replace(old, new, 1)

# The picker lists declared pools only.
old = """      else
        for (final p in _pools)"""
new = """      else
        for (final p in _pools.where((p) => p.declared))"""
assert old in s, "ambra picker anchor missing"
s = s.replace(old, new, 1)

old = """      else if (_pools.isEmpty)
        const Padding(
            padding: EdgeInsets.all(12),
            child: Text('No pools to show right now.', style: AmbraText.muted))"""
new = """      else if (!_pools.any((p) => p.declared))
        Padding(
            padding: const EdgeInsets.all(12),
            child: Text(
                'No pool has declared itself yet. $_stakers signer(s) are producing blocks for themselves; '
                'a staker becomes a pool by committing a payout policy on-chain, and appears here when it does.',
                style: AmbraText.muted))"""
assert old in s, "ambra empty anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("ambra: offers declared pools only")
