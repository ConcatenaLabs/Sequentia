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
