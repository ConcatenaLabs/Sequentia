p = "/home/aejkohl/sequentia-pool-board/index.html"
s = open(p).read()

# `declared` was defined below its first use, which is a temporal dead zone
# error that throws the whole render. Define it once, before anything reads it.
old_def = """  // The board lists pools, not stakers. The feed carries every signer so a
  // wallet can look up whichever one its stake is lent to; the page shows only
  // those that declared.
  const declared = pools.filter(p => p.declared !== false);
  const sorted = declared.slice().sort((a,b) => {"""
new_def = """  const sorted = declared.slice().sort((a,b) => {"""
assert old_def in s, "definition anchor missing"
s = s.replace(old_def, new_def, 1)

old_use = """  const changing = declared.filter(p => (p.policy_pending||[]).length);"""
new_use = """  // The board lists pools, not stakers. The feed carries every signer so a
  // wallet can look up whichever one its stake is lent to; the page shows only
  // those that declared. Defined before anything reads it.
  const declared = pools.filter(p => p.declared !== false);
  const changing = declared.filter(p => (p.policy_pending||[]).length);"""
assert old_use in s, "use anchor missing"
s = s.replace(old_use, new_use, 1)
open(p, "w").write(s)
print("declared defined before use")
